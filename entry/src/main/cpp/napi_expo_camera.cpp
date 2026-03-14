#include <cstdint>
#include <napi/native_api.h>
#include <mutex>
#include <map>
#include "expo_camera.h"
#include "burst_capture.h"
#include "hilog/log.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "NapiExpoCamera"

// 全局回调存储
static napi_ref g_photoCallbackRef = nullptr;
static napi_env g_env = nullptr;
static std::mutex g_callbackMutex;
static size_t g_photoSize = 0;  // 保存照片大小，用于异步回调

// 观察者回调存储
struct ObserverCallbackInfo {
    napi_ref callbackRef;
    napi_env env;
    std::string slotId;
};
static std::map<std::string, ObserverCallbackInfo> g_observerCallbacks;
static std::mutex g_observerMutex;

// 状态订阅回调存储
static napi_ref g_stateCallbackRef = nullptr;
static napi_env g_stateEnv = nullptr;
static std::mutex g_stateMutex;

// 连拍相关全局存储
static napi_ref g_burstProgressCallbackRef = nullptr;
static napi_ref g_burstImageCallbackRef = nullptr;
static napi_env g_burstEnv = nullptr;
static std::mutex g_burstMutex;

// 保存照片数据，用于回调（使用异步工作队列）
static void onPhotoData(void* buffer, size_t size) {
    if (!g_photoCallbackRef || !g_env || !buffer || size == 0) {
        OH_LOG_ERROR(LOG_APP, "Photo callback not ready or invalid buffer");
        return;
    }

    OH_LOG_INFO(LOG_APP, "onPhotoData size:%{public}zu", size);
    g_photoSize = size;

    // 复制 buffer 数据
    void* copyBuffer = malloc(size);
    if (copyBuffer == nullptr) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate memory for photo buffer");
        return;
    }
    std::memcpy(copyBuffer, buffer, size);

    // 创建异步工作
    napi_value asyncResourceName = nullptr;
    napi_create_string_utf8(g_env, "onPhotoData", NAPI_AUTO_LENGTH, &asyncResourceName);

    napi_async_work work;
    napi_status status = napi_create_async_work(
        g_env, nullptr, asyncResourceName,
        [](napi_env envLocal, void* data) {
            // 异步执行阶段（空操作，数据已准备好）
        },
        [](napi_env envLocal, napi_status status, void* data) {
            // 主线程回调阶段
            napi_value callback = nullptr;
            void* outputData = nullptr;
            napi_value arrayBuffer = nullptr;
            size_t bufferSize = g_photoSize;

            napi_create_arraybuffer(envLocal, bufferSize, &outputData, &arrayBuffer);
            std::memcpy(outputData, data, bufferSize);

            OH_LOG_INFO(LOG_APP, "onPhotoData async callback, size: %{public}zu", g_photoSize);

            napi_get_reference_value(envLocal, g_photoCallbackRef, &callback);
            if (callback) {
                napi_value retVal;
                napi_call_function(envLocal, nullptr, callback, 1, &arrayBuffer, &retVal);
            } else {
                OH_LOG_ERROR(LOG_APP, "onPhotoData callback is null");
            }

            // 清理内存
            free(data);
        },
        copyBuffer, &work);

    if (status != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "Failed to create async work for photo callback");
        free(copyBuffer);
        return;
    }

    napi_queue_async_work_with_qos(g_env, work, napi_qos_user_initiated);
}

// 预览流变化回调（观察者模式 - 通知所有观察者）
// 参数: activeSlotId - 当前获得预览流的 slot ID
//       activeSurfaceId - 当前获得预览流的 surface ID
static void onPreviewObserver(const std::string& activeSlotId, const std::string& activeSurfaceId) {
    // 通知所有观察者
    for (auto& pair : g_observerCallbacks) {
        ObserverCallbackInfo& info = pair.second;

        if (!info.callbackRef || !info.env) {
            continue;
        }

        napi_env env = info.env;
        napi_value callback;
        napi_get_reference_value(env, info.callbackRef, &callback);

        if (!callback) {
            continue;
        }

        // 创建参数: activeSlotId, activeSurfaceId
        napi_value argv[2];
        napi_create_string_utf8(env, activeSlotId.c_str(), activeSlotId.length(), &argv[0]);
        napi_create_string_utf8(env, activeSurfaceId.c_str(), activeSurfaceId.length(), &argv[1]);

        // 调用回调
        napi_value global;
        napi_get_global(env, &global);
        napi_call_function(env, global, callback, 2, argv, nullptr);

        OH_LOG_INFO(LOG_APP, "Observer callback invoked for slot: %{public}s, activeSlot=%{public}s",
                    info.slotId.c_str(), activeSlotId.c_str());
    }
}

// 状态变化回调（从 C++ 调用 ArkTS）
static void onStateChanged(const std::string& state, const std::string& message) {
//    std::lock_guard<std::mutex> lock(g_stateMutex);

    if (!g_stateCallbackRef || !g_stateEnv) {
        return;
    }

    napi_env env = g_stateEnv;
    napi_value callback;
    napi_get_reference_value(env, g_stateCallbackRef, &callback);

    if (!callback) {
        return;
    }

    // 创建参数: state, message
    napi_value argv[2];
    napi_create_string_utf8(env, state.c_str(), state.length(), &argv[0]);
    napi_create_string_utf8(env, message.c_str(), message.length(), &argv[1]);

    // 调用回调
    napi_value global;
    napi_get_global(env, &global);
    napi_call_function(env, global, callback, 2, argv, nullptr);

    OH_LOG_INFO(LOG_APP, "State callback invoked: %{public}s, %{public}s",
                state.c_str(), message.c_str());
}

static napi_value InitCamera(napi_env env, napi_callback_info info) {
    ExpoCamera& camera = ExpoCamera::getInstance();
    Camera_ErrorCode err = camera.init();

    int32_t result = static_cast<int32_t>(err);

    napi_value napiResult;
    napi_create_int32(env, result, &napiResult);
    return napiResult;
}

static napi_value ReleaseCamera(napi_env env, napi_callback_info info) {
    ExpoCamera& camera = ExpoCamera::getInstance();
    Camera_ErrorCode err = camera.release();

    int32_t result = static_cast<int32_t>(err);

    napi_value napiResult;
    napi_create_int32(env, result, &napiResult);
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

    ExpoCamera& camera = ExpoCamera::getInstance();
    Camera_ErrorCode err = camera.switchSurface(surfaceId);

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(err), &result);
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

    ExpoCamera& camera = ExpoCamera::getInstance();
    Camera_ErrorCode err = camera.startPreview(surfaceId);

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(err), &result);
    return result;
}

static napi_value StopPreview(napi_env env, napi_callback_info info) {
    ExpoCamera& camera = ExpoCamera::getInstance();
    Camera_ErrorCode err = camera.stopPreview();

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(err), &result);
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

    ExpoCamera& camera = ExpoCamera::getInstance();
    Camera_ErrorCode err = camera.setZoomRatio(static_cast<float>(ratio));

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(err), &result);
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

    ExpoCamera& camera = ExpoCamera::getInstance();
    Camera_ErrorCode err = camera.setFocusMode(static_cast<Camera_FocusMode>(mode));

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(err), &result);
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

    ExpoCamera& camera = ExpoCamera::getInstance();
    Camera_ErrorCode err = camera.setFocusDistance(static_cast<float>(distance));

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(err), &result);
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

    ExpoCamera& camera = ExpoCamera::getInstance();
    Camera_ErrorCode err = camera.setFocusPoint(static_cast<float>(x), static_cast<float>(y));

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(err), &result);
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
    // takePhoto 只作为动作触发，不接收参数
    // 回调需要先通过 registerImageDataCallback 注册

    ExpoCamera& camera = ExpoCamera::getInstance();
    Camera_ErrorCode err = camera.takePhoto();

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(err), &result);
    return result;
}

// 注册图像数据回调函数
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

    // 保存回调引用
    {
        // 释放旧的回调引用
        if (g_photoCallbackRef) {
            napi_delete_reference(g_env, g_photoCallbackRef);
            g_photoCallbackRef = nullptr;
        }

        g_env = env;
        napi_create_reference(env, args[0], 1, &g_photoCallbackRef);
    }

    // 设置 C++ 层回调
    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.setPhotoCallback(onPhotoData);

    OH_LOG_INFO(LOG_APP, "ImageDataCallback registered");

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

static napi_value IsPhotoOutputReady(napi_env env, napi_callback_info info) {
    ExpoCamera& camera = ExpoCamera::getInstance();

    napi_value result;
    napi_get_boolean(env, camera.isPhotoOutputReady(), &result);
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
    std::string slotId = camera.registerObserver(surfaceId, onPreviewObserver, nullptr);

    // 保存回调引用
    {
        ObserverCallbackInfo& cbInfo = g_observerCallbacks[slotId];
        cbInfo.env = env;
        cbInfo.slotId = slotId;
        napi_create_reference(env, args[1], 1, &cbInfo.callbackRef);
    }

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

    // 清理回调引用
    auto it = g_observerCallbacks.find(slotId);
    if (it != g_observerCallbacks.end()) {
        if (it->second.callbackRef) {
            napi_delete_reference(it->second.env, it->second.callbackRef);
        }
        g_observerCallbacks.erase(it);
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

    // 保存回调引用
    {
//        std::lock_guard<std::mutex> lock(g_stateMutex);

        // 释放旧的回调引用
        if (g_stateCallbackRef) {
            napi_delete_reference(g_stateEnv, g_stateCallbackRef);
            g_stateCallbackRef = nullptr;
        }

        // 创建新的引用
        g_stateEnv = env;
        napi_create_reference(env, args[0], 1, &g_stateCallbackRef);
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
    // 清理回调引用
    {
//        std::lock_guard<std::mutex> lock(g_stateMutex);

        if (g_stateCallbackRef) {
            napi_delete_reference(g_stateEnv, g_stateCallbackRef);
            g_stateCallbackRef = nullptr;
        }
    }

    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.unsubscribeState();

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(CAMERA_OK), &result);
    return result;
}

// ========== 连拍相关 NAPI 方法 ==========

// 连拍图像数据回调 (内部使用)
static void onBurstImageCallback(void* buffer, size_t size, bool isFinal) {
    if (!g_burstImageCallbackRef || !g_burstEnv || !buffer) {
        OH_LOG_ERROR(LOG_APP, "Burst image callback not ready");
        return;
    }

    OH_LOG_INFO(LOG_APP, "onBurstImageCallback size: %{public}zu, isFinal: %{public}d", size, isFinal);

    // 复制 buffer 数据
    void* copyBuffer = malloc(size);
    if (copyBuffer == nullptr) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate memory for burst image buffer");
        return;
    }
    std::memcpy(copyBuffer, buffer, size);

    // 创建异步工作
    napi_value asyncResourceName = nullptr;
    napi_create_string_utf8(g_burstEnv, "onBurstImage", NAPI_AUTO_LENGTH, &asyncResourceName);

    // 将 isFinal 作为 data 的一部分传递
    struct BurstImageData {
        void* buffer;
        size_t size;
        bool isFinal;
    };
    BurstImageData* imageData = new BurstImageData{copyBuffer, size, isFinal};

    napi_async_work work;
    napi_status status = napi_create_async_work(
        g_burstEnv, nullptr, asyncResourceName,
        [](napi_env envLocal, void* data) {
            // 异步执行阶段(空操作)
        },
        [](napi_env envLocal, napi_status status, void* data) {
            BurstImageData* imageData = static_cast<BurstImageData*>(data);

            // 主线程回调阶段
            napi_value callback = nullptr;
            void* outputData = nullptr;
            napi_value arrayBuffer = nullptr;

            napi_create_arraybuffer(envLocal, imageData->size, &outputData, &arrayBuffer);
            std::memcpy(outputData, imageData->buffer, imageData->size);

            // 创建 isFinal 参数
            napi_value isFinalValue;
            napi_get_boolean(envLocal, imageData->isFinal, &isFinalValue);

            napi_value argv[2] = {arrayBuffer, isFinalValue};

            napi_get_reference_value(envLocal, g_burstImageCallbackRef, &callback);
            if (callback) {
                napi_value retVal;
                napi_call_function(envLocal, nullptr, callback, 2, argv, &retVal);
            } else {
                OH_LOG_ERROR(LOG_APP, "onBurstImageCallback callback is null");
            }

            // 清理内存
            free(imageData->buffer);
            delete imageData;
        },
        imageData, &work);

    if (status != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "Failed to create async work for burst image callback");
        free(copyBuffer);
        delete imageData;
        return;
    }

    napi_queue_async_work_with_qos(g_burstEnv, work, napi_qos_user_initiated);
}
typedef struct {
    uint16_t* data; // RGB 数据 格式：R G B R G B  ......
    int width;
    int height;
    size_t size;
    int error;
} Rgb16Result;
// 连拍进度回调 (内部使用)
static void onBurstProgressCallback(const exposhot::BurstProgress& progress) {
    if (!g_burstProgressCallbackRef || !g_burstEnv) {
        return;
    }

    // 创建异步工作
    napi_value asyncResourceName = nullptr;
    napi_create_string_utf8(g_burstEnv, "onBurstProgress", NAPI_AUTO_LENGTH, &asyncResourceName);

    // 复制进度数据
    exposhot::BurstProgress* progressCopy = new exposhot::BurstProgress(progress);

    napi_async_work work;
    napi_status status = napi_create_async_work(
        g_burstEnv, nullptr, asyncResourceName,
        [](napi_env envLocal, void* data) {
            // 异步执行阶段(空操作)
        },
        [](napi_env envLocal, napi_status status, void* data) {
            exposhot::BurstProgress* progress = static_cast<exposhot::BurstProgress*>(data);

            // 主线程回调阶段
            napi_value callback = nullptr;
            napi_get_reference_value(envLocal, g_burstProgressCallbackRef, &callback);
            if (callback) {
                // 创建进度对象
                napi_value progressObj;
                napi_create_object(envLocal, &progressObj);

                // 设置属性
                napi_value stateValue;
                napi_create_int32(envLocal, static_cast<int32_t>(progress->state), &stateValue);
                napi_set_named_property(envLocal, progressObj, "state", stateValue);

                napi_value capturedFramesValue;
                napi_create_int32(envLocal, progress->capturedFrames, &capturedFramesValue);
                napi_set_named_property(envLocal, progressObj, "capturedFrames", capturedFramesValue);

                napi_value processedFramesValue;
                napi_create_int32(envLocal, progress->processedFrames, &processedFramesValue);
                napi_set_named_property(envLocal, progressObj, "processedFrames", processedFramesValue);

                napi_value totalFramesValue;
                napi_create_int32(envLocal, progress->totalFrames, &totalFramesValue);
                napi_set_named_property(envLocal, progressObj, "totalFrames", totalFramesValue);

                napi_value messageValue;
                napi_create_string_utf8(envLocal, progress->message.c_str(), progress->message.length(), &messageValue);
                napi_set_named_property(envLocal, progressObj, "message", messageValue);

                // 调用回调
                napi_value retVal;
                napi_call_function(envLocal, nullptr, callback, 1, &progressObj, &retVal);
            }

            // 清理
            delete progress;
        },
        progressCopy, &work);

    if (status != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "Failed to create async work for burst progress callback");
        delete progressCopy;
        return;
    }

    napi_queue_async_work_with_qos(g_burstEnv, work, napi_qos_user_initiated);
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

    // 保存进度回调引用
    if (argc >= 2 && args[1] != nullptr) {
        std::lock_guard<std::mutex> lock(g_burstMutex);
        if (g_burstProgressCallbackRef) {
            napi_delete_reference(g_burstEnv, g_burstProgressCallbackRef);
            g_burstProgressCallbackRef = nullptr;
        }
        g_burstEnv = env;
        napi_create_reference(env, args[1], 1, &g_burstProgressCallbackRef);
    }

    // 保存图像回调引用
    if (argc >= 3 && args[2] != nullptr) {
        std::lock_guard<std::mutex> lock(g_burstMutex);
        if (g_burstImageCallbackRef) {
            napi_delete_reference(g_burstEnv, g_burstImageCallbackRef);
            g_burstImageCallbackRef = nullptr;
        }
        g_burstEnv = env;
        napi_create_reference(env, args[2], 1, &g_burstImageCallbackRef);
    }

    // 设置回调
    exposhot::BurstCapture::getInstance().setProgressCallback(onBurstProgressCallback);
    exposhot::BurstCapture::getInstance().setImageCallback(onBurstImageCallback);

    // 初始化并启动连拍
    if (!exposhot::BurstCapture::getInstance().init()) {
        OH_LOG_ERROR(LOG_APP, "Failed to init BurstCapture");
    }

    exposhot::BurstConfig config;
    config.frameCount = frameCount;
    config.exposureMs = exposureMs;
    config.realtimePreview = realtimePreview;

    bool result = exposhot::BurstCapture::getInstance().startBurst(config);

    napi_value napiResult;
    napi_get_boolean(env, result, &napiResult);
    return napiResult;
}

// 取消连拍
static napi_value CancelBurstCapture(napi_env env, napi_callback_info info) {
    exposhot::BurstCapture::getInstance().cancelBurst();

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// 获取连拍状态
static napi_value GetBurstState(napi_env env, napi_callback_info info) {
    int32_t state = static_cast<int32_t>(exposhot::BurstCapture::getInstance().getState());

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

    exposhot::BurstCapture::getInstance().setImageSize(width, height);

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
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
        {"isPhotoOutputReady", nullptr, IsPhotoOutputReady, nullptr, nullptr, nullptr, napi_default, nullptr},
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
