#include <cstdint>
#include <napi/native_api.h>
#include <mutex>
#include <map>
#include "expo_camera.h"
#include "capture_manager.h"
#include "camera_command_queue.h"
#include "owned_buffer.h"
#include "file_saver.h"
#include "hilog/log.h"
#include "image_processor.h"
#include <rawfile/raw_file_manager.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "NapiExpoCamera"

// ========== Thread-safe callback data structures ==========

struct PhotoCallbackData {
    std::string sessionId;
    exposhot::OwnedBuffer buffer;
    uint32_t width;
    uint32_t height;
};

struct PhotoErrorCallbackData {
    std::string sessionId;
    int32_t errorCode;
};

struct ObserverNotifyData {
    std::string activeSlotId;
    std::string activeSurfaceId;
};

struct StateCallbackData {
    std::string state;
    std::string message;
};

struct BurstImageCallbackData {
    std::string sessionId;
    exposhot::OwnedBuffer buffer;
    bool isFinal;
};

struct BurstProgressCallbackData {
    exposhot::BurstProgress progress;
};

// ========== Global thread-safe function handles ==========

// Helper: release a threadsafe_function safely
static void ReleaseTsfn(napi_threadsafe_function& tsfn) {
    if (tsfn != nullptr) {
        napi_release_threadsafe_function(tsfn, napi_tsfn_release);
        tsfn = nullptr;
    }
}

// Photo data callback
static napi_threadsafe_function g_photoTsfn = nullptr;
static std::mutex g_photoMutex;

// Photo error callback
static napi_threadsafe_function g_photoErrorTsfn = nullptr;
static std::mutex g_photoErrorMutex;

// Observer callbacks
struct ObserverCallbackInfo {
    napi_threadsafe_function tsfn;
    std::string slotId;
};
static std::map<std::string, ObserverCallbackInfo> g_observerCallbacks;
static std::mutex g_observerMutex;

// State callback
static napi_threadsafe_function g_stateTsfn = nullptr;
static std::mutex g_stateMutex;

// Photo event callback
static napi_threadsafe_function g_photoEventTsfn = nullptr;
static std::mutex g_photoEventMutex;

// Process event callback
static napi_threadsafe_function g_processEventTsfn = nullptr;
static std::mutex g_processEventMutex;

// Burst callbacks
static napi_threadsafe_function g_burstProgressTsfn = nullptr;
static napi_threadsafe_function g_burstImageTsfn = nullptr;
static std::mutex g_burstMutex;

// 当前 sessionId 存储（用于异步回调）
static std::string g_currentSessionId;

// ========== CallJs functions (executed on JS main thread) ==========

// --- Photo data CallJs ---
static void PhotoCallJs(napi_env env, napi_value js_cb, void* context, void* data) {
    PhotoCallbackData* cbd = static_cast<PhotoCallbackData*>(data);
    if (!cbd) return;

    size_t bufSize = cbd->buffer.size();
    OH_LOG_INFO(LOG_APP, "PhotoCallJs sessionId:%{public}s, size:%{public}zu, %{public}ux%{public}u",
                cbd->sessionId.c_str(), bufSize, cbd->width, cbd->height);

    void* outputData = nullptr;
    napi_value arrayBuffer = nullptr;
    napi_create_arraybuffer(env, bufSize, &outputData, &arrayBuffer);
    std::memcpy(outputData, cbd->buffer.data(), bufSize);
    // OwnedBuffer 在 cbd 析构时自动释放

    napi_value imageDataObj;
    napi_create_object(env, &imageDataObj);

    napi_value sessionIdValue;
    napi_create_string_utf8(env, cbd->sessionId.c_str(), cbd->sessionId.length(), &sessionIdValue);
    napi_set_named_property(env, imageDataObj, "sessionId", sessionIdValue);
    napi_set_named_property(env, imageDataObj, "buffer", arrayBuffer);

    napi_value widthValue, heightValue;
    napi_create_uint32(env, cbd->width, &widthValue);
    napi_create_uint32(env, cbd->height, &heightValue);
    napi_set_named_property(env, imageDataObj, "width", widthValue);
    napi_set_named_property(env, imageDataObj, "height", heightValue);

    napi_value isFinalValue;
    napi_get_boolean(env, true, &isFinalValue);
    napi_set_named_property(env, imageDataObj, "isFinal", isFinalValue);

    napi_value retVal;
    napi_status status = napi_call_function(env, nullptr, js_cb, 1, &imageDataObj, &retVal);

    // 检查 JS 异常：如果回调中发生了未捕获的异常，先清除再继续
    if (status != napi_ok) {
        napi_value exception = nullptr;
        napi_get_and_clear_last_exception(env, &exception);
        OH_LOG_ERROR(LOG_APP, "PhotoCallJs: napi_call_function failed, status=%{public}d", status);
    }

    delete cbd;
}

// --- Photo error CallJs ---
static void PhotoErrorCallJs(napi_env env, napi_value js_cb, void* context, void* data) {
    PhotoErrorCallbackData* cbd = static_cast<PhotoErrorCallbackData*>(data);
    if (!cbd) return;

    napi_value errorObj;
    napi_create_object(env, &errorObj);

    napi_value sessionIdValue;
    napi_create_string_utf8(env, cbd->sessionId.c_str(), cbd->sessionId.length(), &sessionIdValue);
    napi_set_named_property(env, errorObj, "sessionId", sessionIdValue);

    napi_value errorCodeValue;
    napi_create_int32(env, cbd->errorCode, &errorCodeValue);
    napi_set_named_property(env, errorObj, "errorCode", errorCodeValue);

    napi_value retVal;
    napi_call_function(env, nullptr, js_cb, 1, &errorObj, &retVal);

    delete cbd;
}

// --- Observer notify CallJs ---
static void ObserverCallJs(napi_env env, napi_value js_cb, void* context, void* data) {
    ObserverNotifyData* ond = static_cast<ObserverNotifyData*>(data);
    if (!ond) return;

    napi_value argv[2];
    napi_create_string_utf8(env, ond->activeSlotId.c_str(), ond->activeSlotId.length(), &argv[0]);
    napi_create_string_utf8(env, ond->activeSurfaceId.c_str(), ond->activeSurfaceId.length(), &argv[1]);

    napi_value retVal;
    napi_call_function(env, nullptr, js_cb, 2, argv, &retVal);

    OH_LOG_INFO(LOG_APP, "ObserverCallJs: activeSlot=%{public}s", ond->activeSlotId.c_str());
    delete ond;
}

// --- State change CallJs ---
static void StateCallJs(napi_env env, napi_value js_cb, void* context, void* data) {
    StateCallbackData* scd = static_cast<StateCallbackData*>(data);
    if (!scd) return;

    napi_value argv[2];
    napi_create_string_utf8(env, scd->state.c_str(), scd->state.length(), &argv[0]);
    napi_create_string_utf8(env, scd->message.c_str(), scd->message.length(), &argv[1]);

    napi_value retVal;
    napi_call_function(env, nullptr, js_cb, 2, argv, &retVal);

    OH_LOG_INFO(LOG_APP, "StateCallJs: %{public}s, %{public}s", scd->state.c_str(), scd->message.c_str());
    delete scd;
}

// --- Photo event CallJs ---
static void PhotoEventCallJs(napi_env env, napi_value js_cb, void* context, void* data) {
    exposhot::PhotoEvent* event = static_cast<exposhot::PhotoEvent*>(data);
    if (!event) return;

    napi_value eventObj;
    napi_create_object(env, &eventObj);

    napi_value typeValue;
    napi_create_int32(env, static_cast<int32_t>(event->type), &typeValue);
    napi_set_named_property(env, eventObj, "type", typeValue);

    napi_value sessionIdValue;
    napi_create_string_utf8(env, event->sessionId.c_str(), event->sessionId.length(), &sessionIdValue);
    napi_set_named_property(env, eventObj, "sessionId", sessionIdValue);

    if (event->frameIndex >= 0) {
        napi_value frameIndexValue;
        napi_create_int32(env, event->frameIndex, &frameIndexValue);
        napi_set_named_property(env, eventObj, "frameIndex", frameIndexValue);
    }
    if (!event->message.empty()) {
        napi_value messageValue;
        napi_create_string_utf8(env, event->message.c_str(), event->message.length(), &messageValue);
        napi_set_named_property(env, eventObj, "message", messageValue);
    }

    napi_value retVal;
    napi_call_function(env, nullptr, js_cb, 1, &eventObj, &retVal);
    delete event;
}

// --- Process event CallJs ---
static void ProcessEventCallJs(napi_env env, napi_value js_cb, void* context, void* data) {
    exposhot::ProcessEvent* event = static_cast<exposhot::ProcessEvent*>(data);
    if (!event) return;

    napi_value eventObj;
    napi_create_object(env, &eventObj);

    napi_value typeValue;
    napi_create_int32(env, static_cast<int32_t>(event->type), &typeValue);
    napi_set_named_property(env, eventObj, "type", typeValue);

    napi_value sessionIdValue;
    napi_create_string_utf8(env, event->sessionId.c_str(), event->sessionId.length(), &sessionIdValue);
    napi_set_named_property(env, eventObj, "sessionId", sessionIdValue);

    napi_value progressValue;
    napi_create_int32(env, event->progress, &progressValue);
    napi_set_named_property(env, eventObj, "progress", progressValue);

    napi_value currentFrameValue;
    napi_create_int32(env, event->currentFrame, &currentFrameValue);
    napi_set_named_property(env, eventObj, "currentFrame", currentFrameValue);

    napi_value totalFramesValue;
    napi_create_int32(env, event->totalFrames, &totalFramesValue);
    napi_set_named_property(env, eventObj, "totalFrames", totalFramesValue);

    if (!event->message.empty()) {
        napi_value messageValue;
        napi_create_string_utf8(env, event->message.c_str(), event->message.length(), &messageValue);
        napi_set_named_property(env, eventObj, "message", messageValue);
    }

    napi_value retVal;
    napi_call_function(env, nullptr, js_cb, 1, &eventObj, &retVal);
    delete event;
}

// --- Burst image CallJs ---
static void BurstImageCallJs(napi_env env, napi_value js_cb, void* context, void* data) {
    BurstImageCallbackData* cbd = static_cast<BurstImageCallbackData*>(data);
    if (!cbd) return;

    size_t bufSize = cbd->buffer.size();
    OH_LOG_INFO(LOG_APP, "BurstImageCallJs sessionId: %{public}s, size: %{public}zu, isFinal: %{public}d",
                cbd->sessionId.c_str(), bufSize, cbd->isFinal);

    void* outputData = nullptr;
    napi_value arrayBuffer = nullptr;
    napi_create_arraybuffer(env, bufSize, &outputData, &arrayBuffer);
    std::memcpy(outputData, cbd->buffer.data(), bufSize);
    // OwnedBuffer 在 cbd 析构时自动释放

    napi_value isFinalValue;
    napi_get_boolean(env, cbd->isFinal, &isFinalValue);

    napi_value args[2] = {arrayBuffer, isFinalValue};
    napi_value retVal;
    napi_status status = napi_call_function(env, nullptr, js_cb, 2, args, &retVal);

    // 检查 JS 异常
    if (status != napi_ok) {
        napi_value exception = nullptr;
        napi_get_and_clear_last_exception(env, &exception);
        OH_LOG_ERROR(LOG_APP, "BurstImageCallJs: napi_call_function failed, status=%{public}d", status);
    }

    delete cbd;
}

// --- Burst progress CallJs ---
static void BurstProgressCallJs(napi_env env, napi_value js_cb, void* context, void* data) {
    BurstProgressCallbackData* cbd = static_cast<BurstProgressCallbackData*>(data);
    if (!cbd) return;

    napi_value progressObj;
    napi_create_object(env, &progressObj);

    napi_value stateValue;
    napi_create_int32(env, static_cast<int32_t>(cbd->progress.state), &stateValue);
    napi_set_named_property(env, progressObj, "state", stateValue);

    napi_value capturedFramesValue;
    napi_create_int32(env, cbd->progress.capturedFrames, &capturedFramesValue);
    napi_set_named_property(env, progressObj, "capturedFrames", capturedFramesValue);

    napi_value processedFramesValue;
    napi_create_int32(env, cbd->progress.processedFrames, &processedFramesValue);
    napi_set_named_property(env, progressObj, "processedFrames", processedFramesValue);

    napi_value totalFramesValue;
    napi_create_int32(env, cbd->progress.totalFrames, &totalFramesValue);
    napi_set_named_property(env, progressObj, "totalFrames", totalFramesValue);

    napi_value messageValue;
    napi_create_string_utf8(env, cbd->progress.message.c_str(), cbd->progress.message.length(), &messageValue);
    napi_set_named_property(env, progressObj, "message", messageValue);

    napi_value sessionIdValue;
    napi_create_string_utf8(env, cbd->progress.sessionId.c_str(), cbd->progress.sessionId.length(), &sessionIdValue);
    napi_set_named_property(env, progressObj, "sessionId", sessionIdValue);

    napi_value retVal;
    napi_call_function(env, nullptr, js_cb, 1, &progressObj, &retVal);
    delete cbd;
}

// ========== C++ callback functions (called from non-JS threads, use tsfn) ==========

static void onPhotoData(const std::string& sessionId, void* buffer, size_t size, uint32_t width, uint32_t height) {
    if (!buffer || size == 0) {
        OH_LOG_ERROR(LOG_APP, "Invalid buffer in onPhotoData");
        return;
    }

    std::lock_guard<std::mutex> lock(g_photoMutex);
    if (!g_photoTsfn) {
        OH_LOG_ERROR(LOG_APP, "Photo callback not registered");
        // 释放 buffer（所有权已转移给我们）
        free(buffer);
        return;
    }

    OH_LOG_INFO(LOG_APP, "onPhotoData sessionId:%{public}s, size:%{public}zu, %{public}ux%{public}u",
                sessionId.c_str(), size, width, height);

    // 接管 buffer 所有权，直接 move 到 tsfn 数据（消除一次 malloc+memcpy）
    PhotoCallbackData* cbd = new PhotoCallbackData{
        sessionId,
        exposhot::OwnedBuffer::takeOver(buffer, size),
        width,
        height
    };
    napi_call_threadsafe_function(g_photoTsfn, cbd, napi_tsfn_nonblocking);
}

// 拍照错误回调（异步通知相机硬件错误）
static void onPhotoErrorCallback(const std::string& sessionId, int32_t errorCode) {
    std::lock_guard<std::mutex> lock(g_photoErrorMutex);
    if (!g_photoErrorTsfn) {
        OH_LOG_ERROR(LOG_APP, "Photo error callback not ready");
        return;
    }

    OH_LOG_ERROR(LOG_APP, "onPhotoErrorCallback sessionId: %{public}s, errorCode: %{public}d",
                 sessionId.c_str(), errorCode);

    PhotoErrorCallbackData* cbd = new PhotoErrorCallbackData{sessionId, errorCode};
    napi_call_threadsafe_function(g_photoErrorTsfn, cbd, napi_tsfn_nonblocking);
}

// 预览流变化回调（观察者模式 - 通知所有观察者）
static void onPreviewObserver(const std::string& activeSlotId, const std::string& activeSurfaceId) {
    OH_LOG_INFO(LOG_APP, "onPreviewObserver called: activeSlotId=%{public}s, activeSurfaceId=%{public}s",
                activeSlotId.c_str(), activeSurfaceId.c_str());

    std::lock_guard<std::mutex> lock(g_observerMutex);

    OH_LOG_INFO(LOG_APP, "Observer callbacks count: %{public}zu", g_observerCallbacks.size());

    for (auto& pair : g_observerCallbacks) {
        ObserverCallbackInfo& info = pair.second;
        if (!info.tsfn) continue;

        ObserverNotifyData* data = new ObserverNotifyData{activeSlotId, activeSurfaceId};
        napi_status status = napi_call_threadsafe_function(info.tsfn, data, napi_tsfn_nonblocking);
        if (status == napi_ok) {
            OH_LOG_INFO(LOG_APP, "Observer notify queued for slot: %{public}s", info.slotId.c_str());
        } else {
            OH_LOG_ERROR(LOG_APP, "Failed to queue observer notify for slot: %{public}s", info.slotId.c_str());
            delete data;
        }
    }
}

// 状态变化回调（从 C++ 调用 ArkTS）
static void onStateChanged(const std::string& state, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (!g_stateTsfn) return;

    StateCallbackData* data = new StateCallbackData{state, message};
    napi_call_threadsafe_function(g_stateTsfn, data, napi_tsfn_nonblocking);

    OH_LOG_INFO(LOG_APP, "onStateChanged queued: %{public}s, %{public}s",
                state.c_str(), message.c_str());
}

static napi_value InitCamera(napi_env env, napi_callback_info info) {
    // 参数：mode (必填)
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    // 解析 mode 参数（第一个参数，必填）
    int32_t modeValue = 0;  // 默认 SINGLE
    if (argc >= 1 && args[0] != nullptr) {
        napi_get_value_int32(env, args[0], &modeValue);
        OH_LOG_INFO(LOG_APP, "InitCamera with mode: %{public}d", modeValue);
    }

    // 通过 CaptureManager 初始化（内部会初始化 ExpoCamera 并注册回调）
    // 传入模式，让底层选择正确的摄像头
    CaptureMode mode = (modeValue == 1) ? CaptureMode::BURST : CaptureMode::SINGLE;
    bool success = exposhot::CaptureManager::getInstance().init(mode);

    int32_t result = success ? 0 : -1;

    napi_value napiResult;
    napi_create_int32(env, result, &napiResult);
    return napiResult;
}

static napi_value ReleaseCamera(napi_env env, napi_callback_info info) {
    // 通过 CaptureManager 释放（内部会释放 ExpoCamera）
    exposhot::CaptureManager::getInstance().release();

    napi_value napiResult;
    napi_create_int32(env, 0, &napiResult);
    return napiResult;
}

static napi_value SwitchSurface(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_value result;
        napi_create_int32(env, Camera_ErrorCode::CAMERA_INVALID_ARGUMENT, &result);
        return result;
    }

    size_t surfaceIdLen = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &surfaceIdLen);

    std::string surfaceId(surfaceIdLen, '\0');
    napi_get_value_string_utf8(env, args[0], &surfaceId[0], surfaceIdLen + 1, &surfaceIdLen);

    // 通过命令队列切换 Surface（串行化 OH_Camera_* 调用）
    int32_t err = exposhot::CaptureManager::getInstance().getCommandQueue()->postSync(
        exposhot::CameraCommand::makeSwitchSurface(surfaceId));

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
}

static napi_value StartPreview(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_value result;
        napi_create_int32(env, Camera_ErrorCode::CAMERA_INVALID_ARGUMENT, &result);
        return result;
    }

    size_t surfaceIdLen = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &surfaceIdLen);

    std::string surfaceId(surfaceIdLen, '\0');
    napi_get_value_string_utf8(env, args[0], &surfaceId[0], surfaceIdLen + 1, &surfaceIdLen);

    // 通过命令队列启动预览
    int32_t err = exposhot::CaptureManager::getInstance().getCommandQueue()->postSync(
        exposhot::CameraCommand::makeStartPreview(surfaceId));

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
}

static napi_value StopPreview(napi_env env, napi_callback_info info) {
    // 通过命令队列停止预览
    int32_t err = exposhot::CaptureManager::getInstance().getCommandQueue()->postSync(
        exposhot::CameraCommand::makeStopPreview());

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
}

static napi_value SetZoomRatio(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_value result;
        napi_create_int32(env, Camera_ErrorCode::CAMERA_INVALID_ARGUMENT, &result);
        return result;
    }

    double ratio;
    napi_get_value_double(env, args[0], &ratio);

    // 通过命令队列设置缩放
    int32_t err = exposhot::CaptureManager::getInstance().getCommandQueue()->postSync(
        exposhot::CameraCommand::makeSetZoomRatio(static_cast<float>(ratio)));

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
}

static napi_value GetZoomRatio(napi_env env, napi_callback_info info) {
    float ratio = 1.0f;

    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.getZoomRatio(&ratio);

    napi_value result;
    napi_create_double(env, static_cast<double>(ratio), &result);
    return result;
}

static napi_value SetFocusMode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_value result;
        napi_create_int32(env, Camera_ErrorCode::CAMERA_INVALID_ARGUMENT, &result);
        return result;
    }

    int32_t mode;
    napi_get_value_int32(env, args[0], &mode);

    // 通过命令队列设置对焦模式
    int32_t err = exposhot::CaptureManager::getInstance().getCommandQueue()->postSync(
        exposhot::CameraCommand::makeSetFocusMode(mode));

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
}

static napi_value GetFocusMode(napi_env env, napi_callback_info info) {
    Camera_FocusMode mode = Camera_FocusMode::FOCUS_MODE_MANUAL;

    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.getFocusMode(&mode);

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(mode), &result);
    return result;
}

static napi_value SetFocusDistance(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_value result;
        napi_create_int32(env, Camera_ErrorCode::CAMERA_INVALID_ARGUMENT, &result);
        return result;
    }

    double distance;
    napi_get_value_double(env, args[0], &distance);

    // 通过命令队列设置对焦距离
    int32_t err = exposhot::CaptureManager::getInstance().getCommandQueue()->postSync(
        exposhot::CameraCommand::makeSetFocusDistance(static_cast<float>(distance)));

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
}

static napi_value GetFocusDistance(napi_env env, napi_callback_info info) {
    float distance = 0.0f;

    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.getFocusDistance(&distance);

    napi_value result;
    napi_create_double(env, static_cast<double>(distance), &result);
    return result;
}

static napi_value GetFocusDistanceRange(napi_env env, napi_callback_info info) {
    float min = 0.0f, max = 0.0f;

    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.getFocusDistanceRange(&min, &max);

    napi_value result;
    napi_create_object(env, &result);

    napi_value minVal, maxVal;
    napi_create_double(env, static_cast<double>(min), &minVal);
    napi_create_double(env, static_cast<double>(max), &maxVal);

    napi_set_named_property(env, result, "min", minVal);
    napi_set_named_property(env, result, "max", maxVal);

    return result;
}

static napi_value SetFocusPoint(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 2) {
        napi_value result;
        napi_create_int32(env, Camera_ErrorCode::CAMERA_INVALID_ARGUMENT, &result);
        return result;
    }

    double x, y;
    napi_get_value_double(env, args[0], &x);
    napi_get_value_double(env, args[1], &y);

    // 通过命令队列设置对焦点
    int32_t err = exposhot::CaptureManager::getInstance().getCommandQueue()->postSync(
        exposhot::CameraCommand::makeSetFocusPoint(static_cast<float>(x), static_cast<float>(y)));

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
}

static napi_value GetFocusPoint(napi_env env, napi_callback_info info) {
    float x = 0.5f, y = 0.5f;

    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.getFocusPoint(&x, &y);

    napi_value result;
    napi_create_object(env, &result);

    napi_value xVal, yVal;
    napi_create_double(env, static_cast<double>(x), &xVal);
    napi_create_double(env, static_cast<double>(y), &yVal);

    napi_set_named_property(env, result, "x", xVal);
    napi_set_named_property(env, result, "y", yVal);

    return result;
}

static napi_value IsFocusModeSupported(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_value result;
        napi_get_boolean(env, false, &result);
        return result;
    }

    int32_t mode;
    napi_get_value_int32(env, args[0], &mode);

    bool supported = false;
    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.isFocusModeSupported(static_cast<Camera_FocusMode>(mode), &supported);

    napi_value result;
    napi_get_boolean(env, supported, &result);
    return result;
}

static napi_value GetZoomRatioRange(napi_env env, napi_callback_info info) {
    float min = 1.0f, max = 1.0f;

    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.getZoomRatioRange(&min, &max);

    napi_value result;
    napi_create_object(env, &result);

    napi_value minVal, maxVal;
    napi_create_double(env, static_cast<double>(min), &minVal);
    napi_create_double(env, static_cast<double>(max), &maxVal);

    napi_set_named_property(env, result, "min", minVal);
    napi_set_named_property(env, result, "max", maxVal);

    return result;
}

static napi_value IsZoomSupported(napi_env env, napi_callback_info info) {
    bool supported = false;

    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.isZoomSupported(&supported);

    napi_value result;
    napi_get_boolean(env, supported, &result);
    return result;
}

static napi_value TakePhoto(napi_env env, napi_callback_info info) {
    // takePhoto 触发单次拍照，返回错误码和 sessionId
    // 现在委托给 CaptureManager 统一管理
    // 回调需要先通过 registerImageDataCallback 注册

    exposhot::CaptureManager& manager = exposhot::CaptureManager::getInstance();
    std::string sessionId;
    int32_t err = manager.captureSingle(sessionId);

    // 返回对象: { errorCode: number, sessionId: string }
    napi_value resultObj;
    napi_create_object(env, &resultObj);

    napi_value errorCodeValue;
    napi_create_int32(env, err, &errorCodeValue);
    napi_set_named_property(env, resultObj, "errorCode", errorCodeValue);

    napi_value sessionIdValue;
    napi_create_string_utf8(env, sessionId.c_str(), sessionId.length(), &sessionIdValue);
    napi_set_named_property(env, resultObj, "sessionId", sessionIdValue);

    return resultObj;
}

// 注册图像数据回调函数（单次拍照使用）
static napi_value RegisterImageDataCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Wrong number of arguments");
        return nullptr;
    }

    napi_valuetype type;
    napi_typeof(env, args[0], &type);
    if (type != napi_function) {
        napi_throw_type_error(env, nullptr, "Argument must be a function");
        return nullptr;
    }

    // 创建 threadsafe_function
    {
        std::lock_guard<std::mutex> lock(g_photoMutex);
        ReleaseTsfn(g_photoTsfn);

        napi_value workName;
        napi_create_string_utf8(env, "PhotoCallback", NAPI_AUTO_LENGTH, &workName);
        napi_create_threadsafe_function(env, args[0], nullptr, workName, 0, 1,
                                        nullptr, nullptr, nullptr, PhotoCallJs, &g_photoTsfn);
    }

    // 设置 C++ 层回调（委托给 CaptureManager）
    exposhot::CaptureManager& manager = exposhot::CaptureManager::getInstance();
    manager.setSinglePhotoCallback(onPhotoData);

    OH_LOG_INFO(LOG_APP, "ImageDataCallback registered to CaptureManager");

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// 注册拍照错误回调函数
static napi_value RegisterPhotoErrorCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Wrong number of arguments");
        return nullptr;
    }

    napi_valuetype type;
    napi_typeof(env, args[0], &type);
    if (type != napi_function) {
        napi_throw_type_error(env, nullptr, "Argument must be a function");
        return nullptr;
    }

    // 创建 threadsafe_function
    {
        std::lock_guard<std::mutex> lock(g_photoErrorMutex);
        ReleaseTsfn(g_photoErrorTsfn);

        napi_value workName;
        napi_create_string_utf8(env, "PhotoErrorCallback", NAPI_AUTO_LENGTH, &workName);
        napi_create_threadsafe_function(env, args[0], nullptr, workName, 0, 1,
                                        nullptr, nullptr, nullptr, PhotoErrorCallJs, &g_photoErrorTsfn);
    }

    // 设置 C++ 层回调（委托给 CaptureManager）
    exposhot::CaptureManager& manager = exposhot::CaptureManager::getInstance();
    manager.setPhotoErrorCallback(onPhotoErrorCallback);

    OH_LOG_INFO(LOG_APP, "PhotoErrorCallback registered to CaptureManager");

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// ==================== 事件回调注册 ====================

// 拍照事件回调（内部使用）
static void onPhotoEventCallback(const exposhot::PhotoEvent& event) {
    std::lock_guard<std::mutex> lock(g_photoEventMutex);
    if (!g_photoEventTsfn) return;

    exposhot::PhotoEvent* eventCopy = new exposhot::PhotoEvent(event);
    napi_call_threadsafe_function(g_photoEventTsfn, eventCopy, napi_tsfn_nonblocking);
}

// 注册拍照事件回调函数
static napi_value RegisterPhotoEventCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Wrong number of arguments");
        return nullptr;
    }

    napi_valuetype type;
    napi_typeof(env, args[0], &type);
    if (type != napi_function) {
        napi_throw_type_error(env, nullptr, "Argument must be a function");
        return nullptr;
    }

    // 创建 threadsafe_function
    {
        std::lock_guard<std::mutex> lock(g_photoEventMutex);
        ReleaseTsfn(g_photoEventTsfn);

        napi_value workName;
        napi_create_string_utf8(env, "PhotoEventCallback", NAPI_AUTO_LENGTH, &workName);
        napi_create_threadsafe_function(env, args[0], nullptr, workName, 0, 1,
                                        nullptr, nullptr, nullptr, PhotoEventCallJs, &g_photoEventTsfn);
    }

    // 设置 C++ 层回调（委托给 CaptureManager）
    exposhot::CaptureManager& manager = exposhot::CaptureManager::getInstance();
    manager.setPhotoEventCallback(onPhotoEventCallback);

    OH_LOG_INFO(LOG_APP, "PhotoEventCallback registered to CaptureManager");

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// 处理事件回调（内部使用）
static void onProcessEventCallback(const exposhot::ProcessEvent& event) {
    std::lock_guard<std::mutex> lock(g_processEventMutex);
    if (!g_processEventTsfn) return;

    exposhot::ProcessEvent* eventCopy = new exposhot::ProcessEvent(event);
    napi_call_threadsafe_function(g_processEventTsfn, eventCopy, napi_tsfn_nonblocking);
}

// 注册处理事件回调函数
static napi_value RegisterProcessEventCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Wrong number of arguments");
        return nullptr;
    }

    napi_valuetype type;
    napi_typeof(env, args[0], &type);
    if (type != napi_function) {
        napi_throw_type_error(env, nullptr, "Argument must be a function");
        return nullptr;
    }

    // 创建 threadsafe_function
    {
        std::lock_guard<std::mutex> lock(g_processEventMutex);
        ReleaseTsfn(g_processEventTsfn);

        napi_value workName;
        napi_create_string_utf8(env, "ProcessEventCallback", NAPI_AUTO_LENGTH, &workName);
        napi_create_threadsafe_function(env, args[0], nullptr, workName, 0, 1,
                                        nullptr, nullptr, nullptr, ProcessEventCallJs, &g_processEventTsfn);
    }

    // 设置 C++ 层回调（委托给 CaptureManager）
    exposhot::CaptureManager& manager = exposhot::CaptureManager::getInstance();
    manager.setProcessEventCallback(onProcessEventCallback);

    OH_LOG_INFO(LOG_APP, "ProcessEventCallback registered to CaptureManager");

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// ==================== 文件保存 ====================

// 保存图像到文件
static napi_value SaveImageToFile(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Wrong number of arguments");
        return nullptr;
    }

    // 获取 ArrayBuffer
    void* data = nullptr;
    size_t length = 0;
    napi_get_arraybuffer_info(env, args[0], &data, &length);

    if (!data || length == 0) {
        napi_throw_type_error(env, nullptr, "Invalid ArrayBuffer");
        return nullptr;
    }

    // 获取文件名（可选）
    std::string filename;
    if (argc >= 2) {
        size_t filenameLen = 0;
        napi_get_value_string_utf8(env, args[1], nullptr, 0, &filenameLen);
        filename.resize(filenameLen);
        napi_get_value_string_utf8(env, args[1], &filename[0], filenameLen + 1, &filenameLen);
    }

    // 调用 FileSaver 保存
    exposhot::FileSaver& saver = exposhot::FileSaver::getInstance();
    std::string outputPath;
    bool success;

    if (filename.empty()) {
        success = saver.saveJpeg(data, length, &outputPath);
    } else {
        success = saver.save(data, length, filename, &outputPath);
    }

    if (!success) {
        napi_throw_error(env, nullptr, "Failed to save image");
        return nullptr;
    }

    // 返回保存路径
    napi_value result;
    napi_create_string_utf8(env, outputPath.c_str(), outputPath.length(), &result);
    return result;
}

// 获取图片保存目录
static napi_value GetImageSaveDir(napi_env env, napi_callback_info info) {
    exposhot::FileSaver& saver = exposhot::FileSaver::getInstance();
    std::string saveDir = saver.getSaveDir();

    napi_value result;
    napi_create_string_utf8(env, saveDir.c_str(), saveDir.length(), &result);
    return result;
}

static napi_value IsPhotoOutputReady(napi_env env, napi_callback_info info) {
    ExpoCamera& camera = ExpoCamera::getInstance();

    napi_value result;
    napi_get_boolean(env, camera.isPhotoOutputReady(), &result);
    return result;
}

// 切换拍摄模式
static napi_value SwitchCaptureMode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Requires 1 argument: mode (0=SINGLE, 1=BURST)");
        return nullptr;
    }

    int32_t modeValue;
    napi_get_value_int32(env, args[0], &modeValue);

    CaptureMode mode = (modeValue == 1) ? CaptureMode::BURST : CaptureMode::SINGLE;

    // 通过 CaptureManager 统一切换模式，内部会调用 ExpoCamera 并同步状态
    int32_t err = exposhot::CaptureManager::getInstance().switchCaptureMode(mode);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
}

// 获取当前拍摄模式
static napi_value GetCaptureMode(napi_env env, napi_callback_info info) {
    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(exposhot::CaptureManager::getInstance().getCaptureMode()), &result);
    return result;
}

// 检查是否可以切换模式
static napi_value CanSwitchMode(napi_env env, napi_callback_info info) {
    napi_value result;
    napi_get_boolean(env, exposhot::CaptureManager::getInstance().canSwitchMode(), &result);
    return result;
}

// 注册观察者
static napi_value RegisterObserver(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 2) {
        napi_value result;
        napi_create_string_utf8(env, "", 0, &result);
        return result;
    }

    // 获取 surfaceId
    size_t surfaceIdLen = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &surfaceIdLen);
    std::string surfaceId(surfaceIdLen, '\0');
    napi_get_value_string_utf8(env, args[0], &surfaceId[0], surfaceIdLen + 1, &surfaceIdLen);

    // 注册观察者并获取 slotId
    ExpoCamera& camera = ExpoCamera::getInstance();
    napi_value callback = args[1];

    std::string slotId = camera.registerObserver(
        surfaceId,
        [env, callback](const std::string &slotId) -> PreviewObserverCallback {
            std::lock_guard<std::mutex> lock(g_observerMutex);
            ObserverCallbackInfo& cbInfo = g_observerCallbacks[slotId];
            cbInfo.slotId = slotId;

            napi_value workName;
            napi_create_string_utf8(env, "ObserverCallback", NAPI_AUTO_LENGTH, &workName);
            napi_create_threadsafe_function(env, callback, nullptr, workName, 0, 1,
                                            nullptr, nullptr, nullptr, ObserverCallJs, &cbInfo.tsfn);
            return onPreviewObserver;
        },
        nullptr);

    OH_LOG_INFO(LOG_APP, "Observer registered: %{public}s", slotId.c_str());

    // 返回 slotId
    napi_value result;
    napi_create_string_utf8(env, slotId.c_str(), slotId.length(), &result);
    return result;
}

// 注销观察者
static napi_value UnregisterObserver(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_value result;
        napi_create_int32(env, Camera_ErrorCode::CAMERA_INVALID_ARGUMENT, &result);
        return result;
    }

    // 获取 slotId
    size_t slotIdLen = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &slotIdLen);
    std::string slotId(slotIdLen, '\0');
    napi_get_value_string_utf8(env, args[0], &slotId[0], slotIdLen + 1, &slotIdLen);

    // 清理 tsfn
    {
        std::lock_guard<std::mutex> lock(g_observerMutex);
        auto it = g_observerCallbacks.find(slotId);
        if (it != g_observerCallbacks.end()) {
            if (it->second.tsfn) {
                napi_release_threadsafe_function(it->second.tsfn, napi_tsfn_release);
            }
            g_observerCallbacks.erase(it);
        }
    }

    ExpoCamera& camera = ExpoCamera::getInstance();
    Camera_ErrorCode err = camera.unregisterObserver(slotId);

    OH_LOG_INFO(LOG_APP, "Observer unregistered: %{public}s", slotId.c_str());

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(err), &result);
    return result;
}

// 切换到指定 Slot
static napi_value SwitchToSlot(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_value result;
        napi_create_int32(env, Camera_ErrorCode::CAMERA_INVALID_ARGUMENT, &result);
        return result;
    }

    // 获取 slotId
    size_t slotIdLen = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &slotIdLen);
    std::string slotId(slotIdLen, '\0');
    napi_get_value_string_utf8(env, args[0], &slotId[0], slotIdLen + 1, &slotIdLen);

    ExpoCamera& camera = ExpoCamera::getInstance();
    Camera_ErrorCode err = camera.switchToSlot(slotId);

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(err), &result);
    return result;
}

// 订阅相机状态
static napi_value SubscribeState(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_value result;
        napi_create_int32(env, Camera_ErrorCode::CAMERA_INVALID_ARGUMENT, &result);
        return result;
    }

    // 创建 threadsafe_function
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        ReleaseTsfn(g_stateTsfn);

        napi_value workName;
        napi_create_string_utf8(env, "StateCallback", NAPI_AUTO_LENGTH, &workName);
        napi_create_threadsafe_function(env, args[0], nullptr, workName, 0, 1,
                                        nullptr, nullptr, nullptr, StateCallJs, &g_stateTsfn);
    }

    // 设置状态回调
    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.subscribeState(onStateChanged);

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(CAMERA_OK), &result);
    return result;
}

// 取消订阅相机状态
static napi_value UnsubscribeState(napi_env env, napi_callback_info info) {
    // 释放 threadsafe_function
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        ReleaseTsfn(g_stateTsfn);
    }

    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.unsubscribeState();

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(CAMERA_OK), &result);
    return result;
}

// ========== 连拍相关 NAPI 方法 ==========

// 连拍图像数据回调 (内部使用)
static void onBurstImageCallback(const std::string& sessionId, void* buffer, size_t size, bool isFinal) {
    if (!buffer || size == 0) {
        OH_LOG_ERROR(LOG_APP, "Invalid buffer in onBurstImageCallback");
        return;
    }

    std::lock_guard<std::mutex> lock(g_burstMutex);
    if (!g_burstImageTsfn) {
        OH_LOG_ERROR(LOG_APP, "Burst image callback not ready");
        // 释放 buffer（所有权已转移给我们）
        free(buffer);
        return;
    }

    OH_LOG_INFO(LOG_APP, "onBurstImageCallback sessionId: %{public}s, size: %{public}zu, isFinal: %{public}d",
                sessionId.c_str(), size, isFinal);

    // 接管 buffer 所有权，直接 move 到 tsfn 数据（消除一次 malloc+memcpy）
    BurstImageCallbackData* cbd = new BurstImageCallbackData{
        sessionId,
        exposhot::OwnedBuffer::takeOver(buffer, size),
        isFinal
    };
    napi_call_threadsafe_function(g_burstImageTsfn, cbd, napi_tsfn_nonblocking);
}

typedef struct {
    uint16_t* data; // RGB 数据 格式：R G B R G B  ......
    uint32_t width;
    uint32_t height;
    size_t size;
    int error;
} Rgb16Result;

// 连拍进度回调 (内部使用)
static void onBurstProgressCallback(const exposhot::BurstProgress& progress) {
    OH_LOG_INFO(LOG_APP, "[BURST_PROGRESS] sessionId=%{public}s, state=%{public}d, captured=%{public}d/%{public}d, processed=%{public}d/%{public}d, msg=%{public}s",
                progress.sessionId.c_str(),
                static_cast<int>(progress.state),
                progress.capturedFrames, progress.totalFrames,
                progress.processedFrames, progress.totalFrames,
                progress.message.c_str());

    std::lock_guard<std::mutex> lock(g_burstMutex);
    if (!g_burstProgressTsfn) return;

    BurstProgressCallbackData* cbd = new BurstProgressCallbackData{progress};
    napi_call_threadsafe_function(g_burstProgressTsfn, cbd, napi_tsfn_nonblocking);
}

// 开始连拍
static napi_value StartBurstCapture(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    // 解析配置
    int32_t frameCount = 5;
    int32_t exposureMs = 10000;
    bool realtimePreview = true;

    // 从配置对象中读取
    if (argc >= 1 && args[0] != nullptr) {
        napi_value frameCountValue;
        napi_get_named_property(env, args[0], "frameCount", &frameCountValue);
        napi_get_value_int32(env, frameCountValue, &frameCount);

        napi_value exposureMsValue;
        napi_get_named_property(env, args[0], "exposureMs", &exposureMsValue);
        napi_get_value_int32(env, exposureMsValue, &exposureMs);

        napi_value realtimePreviewValue;
        napi_get_named_property(env, args[0], "realtimePreview", &realtimePreviewValue);
        napi_get_value_bool(env, realtimePreviewValue, &realtimePreview);
    }

    // 保存进度回调 - 创建 threadsafe_function
    if (argc >= 2 && args[1] != nullptr) {
        std::lock_guard<std::mutex> lock(g_burstMutex);
        ReleaseTsfn(g_burstProgressTsfn);
        napi_value workName;
        napi_create_string_utf8(env, "BurstProgress", NAPI_AUTO_LENGTH, &workName);
        napi_create_threadsafe_function(env, args[1], nullptr, workName, 0, 1,
                                        nullptr, nullptr, nullptr, BurstProgressCallJs, &g_burstProgressTsfn);
    }

    // 保存图像回调 - 创建 threadsafe_function
    if (argc >= 3 && args[2] != nullptr) {
        std::lock_guard<std::mutex> lock(g_burstMutex);
        ReleaseTsfn(g_burstImageTsfn);
        napi_value workName;
        napi_create_string_utf8(env, "BurstImage", NAPI_AUTO_LENGTH, &workName);
        napi_create_threadsafe_function(env, args[2], nullptr, workName, 0, 1,
                                        nullptr, nullptr, nullptr, BurstImageCallJs, &g_burstImageTsfn);
    }

    // 设置回调
    exposhot::CaptureManager::getInstance().setProgressCallback(onBurstProgressCallback);
    exposhot::CaptureManager::getInstance().setImageCallback(onBurstImageCallback);

    // 初始化并启动连拍
    if (!exposhot::CaptureManager::getInstance().init(CaptureMode::BURST)) {
        OH_LOG_ERROR(LOG_APP, "Failed to init CaptureManager");
    }

    exposhot::BurstConfig config;
    config.frameCount = frameCount;
    config.exposureMs = exposureMs;
    config.realtimePreview = realtimePreview;

    std::string sessionId;
    int32_t err = exposhot::CaptureManager::getInstance().startBurst(config, sessionId);

    // 返回对象: { errorCode: number, sessionId: string }
    napi_value resultObj;
    napi_create_object(env, &resultObj);

    napi_value errorCodeValue;
    napi_create_int32(env, err, &errorCodeValue);
    napi_set_named_property(env, resultObj, "errorCode", errorCodeValue);

    napi_value sessionIdValue;
    napi_create_string_utf8(env, sessionId.c_str(), sessionId.length(), &sessionIdValue);
    napi_set_named_property(env, resultObj, "sessionId", sessionIdValue);

    return resultObj;
}

// 取消连拍
static napi_value CancelBurstCapture(napi_env env, napi_callback_info info) {
    // 释放 threadsafe_function，防止新的调用被投递
    {
        std::lock_guard<std::mutex> lock(g_burstMutex);
        ReleaseTsfn(g_burstProgressTsfn);
        ReleaseTsfn(g_burstImageTsfn);
    }

    exposhot::CaptureManager::getInstance().cancelBurst();

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// 获取连拍状态
static napi_value GetBurstState(napi_env env, napi_callback_info info) {
    int32_t state = static_cast<int32_t>(exposhot::CaptureManager::getInstance().getState());

    napi_value result;
    napi_create_int32(env, state, &result);
    return result;
}

// 设置连拍图像尺寸
static napi_value SetBurstImageSize(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    int32_t width = 4032;
    int32_t height = 3024;

    if (argc >= 2) {
        napi_get_value_int32(env, args[0], &width);
        napi_get_value_int32(env, args[1], &height);
    }

    exposhot::CaptureManager::getInstance().setImageSize(
        static_cast<uint32_t>(width), static_cast<uint32_t>(height));

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// ==================== MockStackProcess 异步实现 ====================

// 异步工作数据结构
typedef struct {
    napi_async_work asyncWork;
    napi_deferred deferred;

    // 输入参数
    int32_t frameIndex;

    // 中间数据（工作线程填充）
    void* dngBuffer;
    size_t dngSize;
    int fileIndex;
    std::string fileName;

    // 输出结果（工作线程填充）
    bool success;
    std::string errorMsg;
    exposhot::Bgra16Raw bgra16Raw;
    std::vector<float> dx;
    std::vector<float> dy;
} MockStackAsyncData;

// 定义测试用的 DNG 文件列表
static const char* RAW_FILE_LIST[] = {
    "IMG_20260314_010259.dng",
    "IMG_20260314_010348.dng",
    "IMG_20260314_010521.dng",
    "IMG_20260314_010555.dng"
};
static const int RAW_FILE_COUNT = 4;

// 工作线程执行函数
static void MockStackExecute(napi_env env, void* data) {
    MockStackAsyncData* asyncData = (MockStackAsyncData*)data;

    OH_LOG_INFO(LOG_APP, "MockStackExecute: frameIndex=%{public}d (worker thread)", asyncData->frameIndex);

    // 检查 ResourceManager
//    if (g_resourceManager == nullptr) {
//        asyncData->success = false;
//        asyncData->errorMsg = "ResourceManager not initialized";
//        return;
//    }

    // 选择文件
    asyncData->fileIndex = asyncData->frameIndex % RAW_FILE_COUNT;
    asyncData->fileName = RAW_FILE_LIST[asyncData->fileIndex];

    OH_LOG_INFO(LOG_APP, "Reading rawfile: %{public}s (fileIndex=%{public}d)",
                asyncData->fileName.c_str(), asyncData->fileIndex);

    // 打开 RawFile
//    RawFile* rawFile = OH_ResourceManager_OpenRawFile(g_resourceManager, asyncData->fileName.c_str());
    RawFile* rawFile = nullptr;
    if (rawFile == nullptr) {
        asyncData->success = false;
        asyncData->errorMsg = "Failed to open rawfile: " + asyncData->fileName;
        return;
    }

    // 获取文件大小
    long fileSize = OH_ResourceManager_GetRawFileSize(rawFile);
    if (fileSize <= 0) {
        OH_ResourceManager_CloseRawFile(rawFile);
        asyncData->success = false;
        asyncData->errorMsg = "Invalid rawfile size";
        return;
    }

    // 读取文件内容
    asyncData->dngBuffer = malloc((size_t)fileSize);
    asyncData->dngSize = (size_t)fileSize;
    int bytesRead = OH_ResourceManager_ReadRawFile(rawFile, asyncData->dngBuffer, fileSize);
    OH_ResourceManager_CloseRawFile(rawFile);

    if (bytesRead != fileSize) {
        asyncData->success = false;
        asyncData->errorMsg = "Failed to read rawfile completely";
        return;
    }

    OH_LOG_INFO(LOG_APP, "Read %{public}d bytes, starting dngToBGRA16...", bytesRead);

    // ★ 耗时操作：DNG 解码
    try {
        std::unique_ptr<exposhot::ImageProcessor> processor = std::make_unique<exposhot::ImageProcessor>();
        asyncData->bgra16Raw = processor->dngToBGRA16(asyncData->dngBuffer, asyncData->dngSize);

        OH_LOG_INFO(LOG_APP, "dngToBGRA16 done: %{public}ux%{public}u",
                    asyncData->bgra16Raw.width, asyncData->bgra16Raw.height);

        // 运动分析和堆叠
        asyncData->dx = {0.0f, 0.0f};
        asyncData->dy = {0.0f, 0.0f};

        if (asyncData->fileIndex != 0) {
            uint16_t* stackedRes = nullptr; // TODO: 从 GPU 获取已堆叠结果
            exposhot::MeanRes res = processor->MotionAnalysisAndStack(
                stackedRes, asyncData->bgra16Raw.data,
                asyncData->bgra16Raw.width, asyncData->bgra16Raw.height);
            asyncData->dx[0] = res.mean_x[0];
            asyncData->dx[1] = res.mean_x[1];
            asyncData->dy[0] = res.mean_y[0];
            asyncData->dy[1] = res.mean_y[1];
        }

        asyncData->success = true;
        OH_LOG_INFO(LOG_APP, "MockStackExecute completed successfully");

    } catch (const std::exception& e) {
        asyncData->success = false;
        asyncData->errorMsg = std::string("Processing error: ") + e.what();
        OH_LOG_ERROR(LOG_APP, "MockStackExecute error: %{public}s", e.what());
    }
}

// 主线程完成函数
static void MockStackComplete(napi_env env, napi_status status, void* data) {
    MockStackAsyncData* asyncData = (MockStackAsyncData*)data;

    OH_LOG_INFO(LOG_APP, "MockStackComplete: success=%{public}d", asyncData->success);

    napi_value resultObj;
    napi_create_object(env, &resultObj);

    if (status != napi_ok || !asyncData->success) {
        // 失败情况
        napi_value successValue;
        napi_get_boolean(env, false, &successValue);
        napi_set_named_property(env, resultObj, "success", successValue);

        napi_value errorMsgValue;
        napi_create_string_utf8(env, asyncData->errorMsg.c_str(), NAPI_AUTO_LENGTH, &errorMsgValue);
        napi_set_named_property(env, resultObj, "error", errorMsgValue);

        napi_reject_deferred(env, asyncData->deferred, resultObj);
    } else {
        // 成功情况
        napi_value successValue;
        napi_get_boolean(env, true, &successValue);
        napi_set_named_property(env, resultObj, "success", successValue);

        // nextFrameIndex
        napi_value frameIndexValue;
        napi_create_int32(env, asyncData->frameIndex + 1, &frameIndexValue);
        napi_set_named_property(env, resultObj, "nextFrameIndex", frameIndexValue);

        // message
        napi_value messageValue;
        std::string msg = "Frame " + std::to_string(asyncData->frameIndex) +
                          " (" + asyncData->fileName + ") processed";
        napi_create_string_utf8(env, msg.c_str(), NAPI_AUTO_LENGTH, &messageValue);
        napi_set_named_property(env, resultObj, "message", messageValue);

        // fileName
        napi_value fileNameValue;
        napi_create_string_utf8(env, asyncData->fileName.c_str(), NAPI_AUTO_LENGTH, &fileNameValue);
        napi_set_named_property(env, resultObj, "fileName", fileNameValue);

        // fileSize
        napi_value fileSizeValue;
        napi_create_int32(env, static_cast<int32_t>(asyncData->dngSize), &fileSizeValue);
        napi_set_named_property(env, resultObj, "fileSize", fileSizeValue);

        // width, height
        napi_value widthValue, heightValue;
        napi_create_int32(env, asyncData->bgra16Raw.width, &widthValue);
        napi_create_int32(env, asyncData->bgra16Raw.height, &heightValue);
        napi_set_named_property(env, resultObj, "width", widthValue);
        napi_set_named_property(env, resultObj, "height", heightValue);

        // dx, dy 偏移量
        napi_value dxArr, dyArr;
        napi_create_array_with_length(env, 2, &dxArr);
        napi_create_array_with_length(env, 2, &dyArr);
        for (int i = 0; i < 2; i++) {
            napi_value dxVal, dyVal;
            napi_create_double(env, asyncData->dx[i], &dxVal);
            napi_create_double(env, asyncData->dy[i], &dyVal);
            napi_set_element(env, dxArr, i, dxVal);
            napi_set_element(env, dyArr, i, dyVal);
        }
        napi_set_named_property(env, resultObj, "dx", dxArr);
        napi_set_named_property(env, resultObj, "dy", dyArr);

        // buffer (原始 DNG 数据)
        if (asyncData->dngBuffer && asyncData->dngSize > 0) {
            void* outputData = nullptr;
            napi_value arrayBuffer;
            napi_status bufStatus = napi_create_arraybuffer(env, asyncData->dngSize, &outputData, &arrayBuffer);
            if (bufStatus == napi_ok && outputData != nullptr) {
                std::memcpy(outputData, asyncData->dngBuffer, asyncData->dngSize);
                napi_set_named_property(env, resultObj, "buffer", arrayBuffer);
            }
        }

        napi_resolve_deferred(env, asyncData->deferred, resultObj);
    }

    // 清理
    if (asyncData->dngBuffer) {
        free(asyncData->dngBuffer);
    }
    // 注意：bgra16Raw.data 由 Frame 管理，无需手动释放
    napi_delete_async_work(env, asyncData->asyncWork);
    delete asyncData;
}

// 模拟堆叠过程（异步版本，返回 Promise）
static napi_value MockStackProcess(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    // 解析 frameIndex
    int32_t frameIndex = 0;
    if (argc >= 2 && args[1] != nullptr) {
        napi_get_value_int32(env, args[1], &frameIndex);
    }

    OH_LOG_INFO(LOG_APP, "MockStackProcess called: frameIndex=%{public}d (main thread)", frameIndex);

    // 创建 Promise
    napi_value promise;
    napi_deferred deferred;
    napi_create_promise(env, &deferred, &promise);

    // 快速检查 ResourceManager
//    if (g_resourceManager == nullptr) {
//        napi_value resultObj;
//        napi_create_object(env, &resultObj);
//        napi_value successValue;
//        napi_get_boolean(env, false, &successValue);
//        napi_set_named_property(env, resultObj, "success", successValue);
//        napi_value errorMsgValue;
//        napi_create_string_utf8(env, "ResourceManager not initialized", NAPI_AUTO_LENGTH, &errorMsgValue);
//        napi_set_named_property(env, resultObj, "error", errorMsgValue);
//        napi_reject_deferred(env, deferred, resultObj);
//        return promise;
//    }

    // 创建异步工作数据
    MockStackAsyncData* asyncData = new MockStackAsyncData();
    asyncData->deferred = deferred;
    asyncData->frameIndex = frameIndex;
    asyncData->dngBuffer = nullptr;
    asyncData->dngSize = 0;
    asyncData->success = false;

    // 创建异步工作
    napi_value workName;
    napi_create_string_utf8(env, "MockStackProcess", NAPI_AUTO_LENGTH, &workName);
    napi_create_async_work(env, nullptr, workName, MockStackExecute, MockStackComplete,
                           asyncData, &asyncData->asyncWork);

    // 加入队列
    napi_queue_async_work(env, asyncData->asyncWork);

    OH_LOG_INFO(LOG_APP, "MockStackProcess: async work queued, returning Promise");
    return promise;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"initCamera", nullptr, InitCamera, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"releaseCamera", nullptr, ReleaseCamera, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"switchSurface", nullptr, SwitchSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startPreview", nullptr, StartPreview, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopPreview", nullptr, StopPreview, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"takePhoto", nullptr, TakePhoto, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerImageDataCallback", nullptr, RegisterImageDataCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerPhotoErrorCallback", nullptr, RegisterPhotoErrorCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerPhotoEventCallback", nullptr, RegisterPhotoEventCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerProcessEventCallback", nullptr, RegisterProcessEventCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isPhotoOutputReady", nullptr, IsPhotoOutputReady, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 拍摄模式切换
        {"switchCaptureMode", nullptr, SwitchCaptureMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getCaptureMode", nullptr, GetCaptureMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"canSwitchMode", nullptr, CanSwitchMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setZoomRatio", nullptr, SetZoomRatio, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getZoomRatio", nullptr, GetZoomRatio, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getZoomRatioRange", nullptr, GetZoomRatioRange, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isZoomSupported", nullptr, IsZoomSupported, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setFocusMode", nullptr, SetFocusMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getFocusMode", nullptr, GetFocusMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isFocusModeSupported", nullptr, IsFocusModeSupported, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setFocusDistance", nullptr, SetFocusDistance, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getFocusDistance", nullptr, GetFocusDistance, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getFocusDistanceRange", nullptr, GetFocusDistanceRange, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setFocusPoint", nullptr, SetFocusPoint, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getFocusPoint", nullptr, GetFocusPoint, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 观察者管理
        {"registerObserver", nullptr, RegisterObserver, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"unregisterObserver", nullptr, UnregisterObserver, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"switchToSlot", nullptr, SwitchToSlot, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 状态订阅
        {"subscribeState", nullptr, SubscribeState, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"unsubscribeState", nullptr, UnsubscribeState, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 连拍相关
        {"startBurstCapture", nullptr, StartBurstCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cancelBurstCapture", nullptr, CancelBurstCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getBurstState", nullptr, GetBurstState, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setBurstImageSize", nullptr, SetBurstImageSize, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 文件保存
        {"saveImageToFile", nullptr, SaveImageToFile, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getImageSaveDir", nullptr, GetImageSaveDir, nullptr, nullptr, nullptr, napi_default, nullptr},
        // 测试接口
        {"mockStackProcess", nullptr, MockStackProcess, nullptr, nullptr, nullptr, napi_default, nullptr},
    };

    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module expocameraModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "expocamera",
    .nm_priv = ((void*)0),
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void) {
    napi_module_register(&expocameraModule);
}
