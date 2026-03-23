#include <cstdint>
#include <napi/native_api.h>
#include <mutex>
#include <map>
#include "expo_camera.h"
#include "capture_manager.h"
#include "file_saver.h"
#include "hilog/log.h"
#include "image_processor.h"
#include <rawfile/raw_file_manager.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "NapiExpoCamera"

// 全局回调存储（单次拍照）
// 简化设计：假设只在初始化时注册一次，注册时加锁，调用时只做空指针检查
static napi_ref g_photoCallbackRef = nullptr;
static napi_env g_photoEnv = nullptr;
static std::mutex g_photoCallbackMutex;  // 仅用于注册时的保护
static size_t g_photoSize = 0;  // 保存照片大小，用于异步回调
static int32_t g_photoWidth = 0;  // 保存照片宽度
static int32_t g_photoHeight = 0;  // 保存照片高度

// 拍照错误回调存储
static napi_ref g_photoErrorCallbackRef = nullptr;
static napi_env g_photoErrorEnv = nullptr;
static std::mutex g_photoErrorMutex;
static bool g_photoErrorCallbackValid = false;

// 观察者回调存储
struct ObserverCallbackInfo {
    napi_ref callbackRef;
    napi_env env;
    std::string slotId;
};
static std::map<std::string, ObserverCallbackInfo> g_observerCallbacks;
static std::mutex g_observerMutex;  // 保护 g_observerCallbacks 的访问

// 状态订阅回调存储
static napi_ref g_stateCallbackRef = nullptr;
static napi_env g_stateEnv = nullptr;
static std::mutex g_stateMutex;

// 连拍相关全局存储
static napi_ref g_burstProgressCallbackRef = nullptr;
static napi_ref g_burstImageCallbackRef = nullptr;
static napi_env g_burstEnv = nullptr;
static std::mutex g_burstMutex;
static bool g_burstProgressCallbackValid = false;

// 全局 ResourceManager（用于访问 rawfile）
static NativeResourceManager* g_resourceManager = nullptr;
static bool g_burstImageCallbackValid = false;

// 拍照事件回调存储
static napi_ref g_photoEventCallbackRef = nullptr;
static napi_env g_photoEventEnv = nullptr;
static std::mutex g_photoEventMutex;

// 处理事件回调存储
static napi_ref g_processEventCallbackRef = nullptr;
static napi_env g_processEventEnv = nullptr;
static std::mutex g_processEventMutex;

// 当前 sessionId 存储（用于异步回调）
static std::string g_currentSessionId;

// 保存照片数据，回调数据结构（用于在异步工作间传递数据）
struct PhotoCallbackData {
    napi_env env;
    napi_ref callbackRef;
    std::string sessionId;
    void* buffer;
    size_t size;
    uint32_t width;
    uint32_t height;
};

// 保存照片数据，用于回调（使用异步工作队列）
// 简化设计：假设注册在初始化时完成，调用时只做空指针检查
static void onPhotoData(const std::string& sessionId, void* buffer, size_t size, uint32_t width, uint32_t height) {
    if (!buffer || size == 0) {
        OH_LOG_ERROR(LOG_APP, "Invalid buffer in onPhotoData");
        return;
    }

    // 简化：只做空指针检查，不加锁（假设注册已在初始化时完成）
    if (!g_photoCallbackRef || !g_photoEnv) {
        OH_LOG_ERROR(LOG_APP, "Photo callback not registered");
        return;
    }

    OH_LOG_INFO(LOG_APP, "onPhotoData sessionId:%{public}s, size:%{public}zu, %{public}ux%{public}u",
                sessionId.c_str(), size, width, height);

    // 复制 buffer 数据
    void* copyBuffer = malloc(size);
    if (copyBuffer == nullptr) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate memory for photo buffer");
        return;
    }
    std::memcpy(copyBuffer, buffer, size);

    // 创建回调数据结构
    PhotoCallbackData* callbackData = new PhotoCallbackData{
        g_photoEnv,
        g_photoCallbackRef,
        sessionId,
        copyBuffer,
        size,
        width,
        height
    };

    // 创建异步工作
    napi_value asyncResourceName = nullptr;
    napi_create_string_utf8(g_photoEnv, "onPhotoData", NAPI_AUTO_LENGTH, &asyncResourceName);

    napi_async_work work;
    napi_status status = napi_create_async_work(
        g_photoEnv, nullptr, asyncResourceName,
        [](napi_env envLocal, void* data) {
            // 异步执行阶段（空操作，数据已准备好）
        },
        [](napi_env envLocal, napi_status status, void* data) {
            PhotoCallbackData* callbackData = static_cast<PhotoCallbackData*>(data);

            // 主线程回调阶段
            napi_value callback = nullptr;
            napi_get_reference_value(envLocal, callbackData->callbackRef, &callback);

            if (callback) {
                void* outputData = nullptr;
                napi_value arrayBuffer = nullptr;
                napi_create_arraybuffer(envLocal, callbackData->size, &outputData, &arrayBuffer);
                std::memcpy(outputData, callbackData->buffer, callbackData->size);

                OH_LOG_INFO(LOG_APP, "onPhotoData async callback, sessionId: %{public}s, size: %{public}zu",
                            callbackData->sessionId.c_str(), callbackData->size);

                // 创建 ImageData 对象
                napi_value imageDataObj;
                napi_create_object(envLocal, &imageDataObj);

                // 设置 sessionId
                napi_value sessionIdValue;
                napi_create_string_utf8(envLocal, callbackData->sessionId.c_str(),
                                        callbackData->sessionId.length(), &sessionIdValue);
                napi_set_named_property(envLocal, imageDataObj, "sessionId", sessionIdValue);

                // 设置 buffer
                napi_set_named_property(envLocal, imageDataObj, "buffer", arrayBuffer);

                // 设置 width 和 height
                napi_value widthValue;
                napi_create_uint32(envLocal, callbackData->width, &widthValue);
                napi_set_named_property(envLocal, imageDataObj, "width", widthValue);

                napi_value heightValue;
                napi_create_uint32(envLocal, callbackData->height, &heightValue);
                napi_set_named_property(envLocal, imageDataObj, "height", heightValue);

                // 设置 isFinal (单次拍照始终为 true)
                napi_value isFinalValue;
                napi_get_boolean(envLocal, true, &isFinalValue);
                napi_set_named_property(envLocal, imageDataObj, "isFinal", isFinalValue);

                napi_value retVal;
                napi_call_function(envLocal, nullptr, callback, 1, &imageDataObj, &retVal);
            } else {
                OH_LOG_ERROR(LOG_APP, "onPhotoData callback is null");
            }

            // 清理内存
            free(callbackData->buffer);
            delete callbackData;
        },
        callbackData, &work);

    if (status != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "Failed to create async work for photo callback");
        free(copyBuffer);
        delete callbackData;
        return;
    }

    napi_queue_async_work_with_qos(g_photoEnv, work, napi_qos_user_initiated);
}

// 拍照错误回调（异步通知相机硬件错误）
static void onPhotoErrorCallback(const std::string& sessionId, int32_t errorCode) {
    // 在锁保护下获取回调引用
    napi_env callbackEnv = nullptr;
    napi_ref callbackRef = nullptr;
    bool callbackValid = false;

    {
        std::lock_guard<std::mutex> lock(g_photoErrorMutex);
        callbackValid = g_photoErrorCallbackValid;
        callbackEnv = g_photoErrorEnv;
        callbackRef = g_photoErrorCallbackRef;
    }

    if (!callbackValid || !callbackRef || !callbackEnv) {
        OH_LOG_ERROR(LOG_APP, "Photo error callback not ready");
        return;
    }

    OH_LOG_ERROR(LOG_APP, "onPhotoErrorCallback sessionId: %{public}s, errorCode: %{public}d",
                 sessionId.c_str(), errorCode);

    // 创建异步工作
    napi_value asyncResourceName = nullptr;
    napi_create_string_utf8(callbackEnv, "onPhotoError", NAPI_AUTO_LENGTH, &asyncResourceName);

    // 将 sessionId 和 errorCode 作为 data 的一部分传递
    struct PhotoErrorData {
        std::string sessionId;
        int32_t errorCode;
        napi_env env;
        napi_ref callbackRef;
    };
    PhotoErrorData* errorData = new PhotoErrorData{sessionId, errorCode, callbackEnv, callbackRef};

    napi_async_work work;
    napi_status status = napi_create_async_work(
        callbackEnv, nullptr, asyncResourceName,
        [](napi_env envLocal, void* data) {
            // 异步执行阶段(空操作)
        },
        [](napi_env envLocal, napi_status status, void* data) {
            PhotoErrorData* errorData = static_cast<PhotoErrorData*>(data);

            // 主线程回调阶段
            napi_value callback = nullptr;
            napi_get_reference_value(envLocal, errorData->callbackRef, &callback);
            if (callback) {
                // 创建错误对象
                napi_value errorObj;
                napi_create_object(envLocal, &errorObj);

                // 设置 sessionId
                napi_value sessionIdValue;
                napi_create_string_utf8(envLocal, errorData->sessionId.c_str(),
                                        errorData->sessionId.length(), &sessionIdValue);
                napi_set_named_property(envLocal, errorObj, "sessionId", sessionIdValue);

                // 设置 errorCode
                napi_value errorCodeValue;
                napi_create_int32(envLocal, errorData->errorCode, &errorCodeValue);
                napi_set_named_property(envLocal, errorObj, "errorCode", errorCodeValue);

                // 调用回调
                napi_value retVal;
                napi_call_function(envLocal, nullptr, callback, 1, &errorObj, &retVal);
            } else {
                OH_LOG_ERROR(LOG_APP, "onPhotoErrorCallback callback is null");
            }

            // 清理内存
            delete errorData;
        },
        errorData, &work);

    if (status != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "Failed to create async work for photo error callback");
        delete errorData;
        return;
    }

    napi_queue_async_work_with_qos(callbackEnv, work, napi_qos_user_initiated);
}

// 预览流变化回调（观察者模式 - 通知所有观察者）
// 参数: activeSlotId - 当前获得预览流的 slot ID
//       activeSurfaceId - 当前获得预览流的 surface ID
static void onPreviewObserver(const std::string& activeSlotId, const std::string& activeSurfaceId) {
    OH_LOG_INFO(LOG_APP, "onPreviewObserver called: activeSlotId=%{public}s, activeSurfaceId=%{public}s",
                activeSlotId.c_str(), activeSurfaceId.c_str());

    // 通知所有观察者（而不仅仅是活跃的观察者）
    std::lock_guard<std::mutex> lock(g_observerMutex);

    OH_LOG_INFO(LOG_APP, "Observer callbacks count: %{public}zu", g_observerCallbacks.size());

    for (auto& pair : g_observerCallbacks) {
        ObserverCallbackInfo &info = pair.second;
        OH_LOG_INFO(LOG_APP, "Processing observer: slotId=%{public}s, hasCallback=%{public}d",
                    info.slotId.c_str(), info.callbackRef != nullptr);

        if (!info.callbackRef) {
            continue;
        }

        // 调用 JS callback
        napi_value callback;
        napi_status status = napi_get_reference_value(info.env, info.callbackRef, &callback);
        if (status != napi_ok || !callback) {
            OH_LOG_WARN(LOG_APP, "Failed to get callback reference for slot: %{public}s, status=%{public}d",
                        info.slotId.c_str(), status);
            continue;
        }

        napi_value argv[2];
        napi_create_string_utf8(info.env, activeSlotId.c_str(), activeSlotId.length(), &argv[0]);
        napi_create_string_utf8(info.env, activeSurfaceId.c_str(), activeSurfaceId.length(), &argv[1]);

        napi_value global;
        napi_get_global(info.env, &global);
        napi_status callStatus = napi_call_function(info.env, global, callback, 2, argv, nullptr);
        if (callStatus == napi_ok) {
            OH_LOG_INFO(LOG_APP, "Observer callback invoked successfully for slot: %{public}s, activeSlot=%{public}s",
                        info.slotId.c_str(), activeSlotId.c_str());
        } else {
            OH_LOG_ERROR(LOG_APP, "Failed to invoke observer callback for slot: %{public}s, status=%{public}d",
                         info.slotId.c_str(), callStatus);
        }
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
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    // 初始化 ResourceManager（如果传入了参数）
    if (argc >= 1 && args[0] != nullptr) {
        // 释放之前可能存在的 ResourceManager
        if (g_resourceManager != nullptr) {
            OH_ResourceManager_ReleaseNativeResourceManager(g_resourceManager);
            g_resourceManager = nullptr;
        }
        g_resourceManager = OH_ResourceManager_InitNativeResourceManager(env, args[0]);
        if (g_resourceManager != nullptr) {
            OH_LOG_INFO(LOG_APP, "ResourceManager initialized successfully");
        } else {
            OH_LOG_ERROR(LOG_APP, "Failed to initialize ResourceManager");
        }
    } else {
        OH_LOG_WARN(LOG_APP, "InitCamera called without ResourceManager argument");
    }

    // 通过 CaptureManager 初始化（内部会初始化 ExpoCamera 并注册回调）
    bool success = exposhot::CaptureManager::getInstance().init();

    int32_t result = success ? 0 : -1;

    napi_value napiResult;
    napi_create_int32(env, result, &napiResult);
    return napiResult;
}

static napi_value ReleaseCamera(napi_env env, napi_callback_info info) {
    // 释放全局 ResourceManager
    if (g_resourceManager != nullptr) {
        OH_ResourceManager_ReleaseNativeResourceManager(g_resourceManager);
        g_resourceManager = nullptr;
        OH_LOG_INFO(LOG_APP, "ResourceManager released");
    }

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

    // 保存回调引用 - 加锁保护注册过程
    {
        std::lock_guard<std::mutex> lock(g_photoCallbackMutex);

        // 释放旧的回调引用
        if (g_photoCallbackRef && g_photoEnv) {
            napi_delete_reference(g_photoEnv, g_photoCallbackRef);
        }

        g_photoEnv = env;
        napi_create_reference(env, args[0], 1, &g_photoCallbackRef);
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

    // 保存回调引用
    {
        std::lock_guard<std::mutex> lock(g_photoErrorMutex);

        // 先标记为无效，防止在更新过程中有回调发生
        g_photoErrorCallbackValid = false;

        // 释放旧的回调引用
        if (g_photoErrorCallbackRef && g_photoErrorEnv) {
            napi_delete_reference(g_photoErrorEnv, g_photoErrorCallbackRef);
        }

        g_photoErrorEnv = env;
        napi_create_reference(env, args[0], 1, &g_photoErrorCallbackRef);

        // 标记为有效
        g_photoErrorCallbackValid = true;
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
    if (!g_photoEventCallbackRef || !g_photoEventEnv) {
        return;
    }

    // 创建异步工作
    napi_value asyncResourceName = nullptr;
    napi_create_string_utf8(g_photoEventEnv, "onPhotoEvent", NAPI_AUTO_LENGTH, &asyncResourceName);

    // 复制事件数据
    exposhot::PhotoEvent* eventCopy = new exposhot::PhotoEvent(event);

    napi_async_work work;
    napi_status status = napi_create_async_work(
        g_photoEventEnv, nullptr, asyncResourceName,
        [](napi_env envLocal, void* data) {
            // 异步执行阶段(空操作)
        },
        [](napi_env envLocal, napi_status status, void* data) {
            exposhot::PhotoEvent* event = static_cast<exposhot::PhotoEvent*>(data);

            // 主线程回调阶段
            napi_value callback = nullptr;
            napi_get_reference_value(envLocal, g_photoEventCallbackRef, &callback);
            if (callback) {
                // 创建事件对象
                napi_value eventObj;
                napi_create_object(envLocal, &eventObj);

                // 设置 type
                napi_value typeValue;
                napi_create_int32(envLocal, static_cast<int32_t>(event->type), &typeValue);
                napi_set_named_property(envLocal, eventObj, "type", typeValue);

                // 设置 sessionId
                napi_value sessionIdValue;
                napi_create_string_utf8(envLocal, event->sessionId.c_str(), event->sessionId.length(), &sessionIdValue);
                napi_set_named_property(envLocal, eventObj, "sessionId", sessionIdValue);

                // 设置 frameIndex (可选)
                if (event->frameIndex >= 0) {
                    napi_value frameIndexValue;
                    napi_create_int32(envLocal, event->frameIndex, &frameIndexValue);
                    napi_set_named_property(envLocal, eventObj, "frameIndex", frameIndexValue);
                }

                // 设置 message (可选)
                if (!event->message.empty()) {
                    napi_value messageValue;
                    napi_create_string_utf8(envLocal, event->message.c_str(), event->message.length(), &messageValue);
                    napi_set_named_property(envLocal, eventObj, "message", messageValue);
                }

                // 调用回调
                napi_value retVal;
                napi_call_function(envLocal, nullptr, callback, 1, &eventObj, &retVal);
            }

            // 清理
            delete event;
        },
        eventCopy, &work);

    if (status != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "Failed to create async work for photo event callback");
        delete eventCopy;
        return;
    }

    napi_queue_async_work_with_qos(g_photoEventEnv, work, napi_qos_user_initiated);
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

    // 保存回调引用
    {
        std::lock_guard<std::mutex> lock(g_photoEventMutex);

        // 释放旧的回调引用
        if (g_photoEventCallbackRef) {
            napi_delete_reference(g_photoEventEnv, g_photoEventCallbackRef);
            g_photoEventCallbackRef = nullptr;
        }

        g_photoEventEnv = env;
        napi_create_reference(env, args[0], 1, &g_photoEventCallbackRef);
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
    if (!g_processEventCallbackRef || !g_processEventEnv) {
        return;
    }

    // 创建异步工作
    napi_value asyncResourceName = nullptr;
    napi_create_string_utf8(g_processEventEnv, "onProcessEvent", NAPI_AUTO_LENGTH, &asyncResourceName);

    // 复制事件数据
    exposhot::ProcessEvent* eventCopy = new exposhot::ProcessEvent(event);

    napi_async_work work;
    napi_status status = napi_create_async_work(
        g_processEventEnv, nullptr, asyncResourceName,
        [](napi_env envLocal, void* data) {
            // 异步执行阶段(空操作)
        },
        [](napi_env envLocal, napi_status status, void* data) {
            exposhot::ProcessEvent* event = static_cast<exposhot::ProcessEvent*>(data);

            // 主线程回调阶段
            napi_value callback = nullptr;
            napi_get_reference_value(envLocal, g_processEventCallbackRef, &callback);
            if (callback) {
                // 创建事件对象
                napi_value eventObj;
                napi_create_object(envLocal, &eventObj);

                // 设置 type
                napi_value typeValue;
                napi_create_int32(envLocal, static_cast<int32_t>(event->type), &typeValue);
                napi_set_named_property(envLocal, eventObj, "type", typeValue);

                // 设置 sessionId
                napi_value sessionIdValue;
                napi_create_string_utf8(envLocal, event->sessionId.c_str(), event->sessionId.length(), &sessionIdValue);
                napi_set_named_property(envLocal, eventObj, "sessionId", sessionIdValue);

                // 设置 progress
                napi_value progressValue;
                napi_create_int32(envLocal, event->progress, &progressValue);
                napi_set_named_property(envLocal, eventObj, "progress", progressValue);

                // 设置 currentFrame
                napi_value currentFrameValue;
                napi_create_int32(envLocal, event->currentFrame, &currentFrameValue);
                napi_set_named_property(envLocal, eventObj, "currentFrame", currentFrameValue);

                // 设置 totalFrames
                napi_value totalFramesValue;
                napi_create_int32(envLocal, event->totalFrames, &totalFramesValue);
                napi_set_named_property(envLocal, eventObj, "totalFrames", totalFramesValue);

                // 设置 message (可选)
                if (!event->message.empty()) {
                    napi_value messageValue;
                    napi_create_string_utf8(envLocal, event->message.c_str(), event->message.length(), &messageValue);
                    napi_set_named_property(envLocal, eventObj, "message", messageValue);
                }

                // 调用回调
                napi_value retVal;
                napi_call_function(envLocal, nullptr, callback, 1, &eventObj, &retVal);
            }

            // 清理
            delete event;
        },
        eventCopy, &work);

    if (status != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "Failed to create async work for process event callback");
        delete eventCopy;
        return;
    }

    napi_queue_async_work_with_qos(g_processEventEnv, work, napi_qos_user_initiated);
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

    // 保存回调引用
    {
        std::lock_guard<std::mutex> lock(g_processEventMutex);

        // 释放旧的回调引用
        if (g_processEventCallbackRef) {
            napi_delete_reference(g_processEventEnv, g_processEventCallbackRef);
            g_processEventCallbackRef = nullptr;
        }

        g_processEventEnv = env;
        napi_create_reference(env, args[0], 1, &g_processEventCallbackRef);
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

    std::string slotId = camera.registerObserver(
        surfaceId,
        [env, &args](const std::string &slotId) -> PreviewObserverCallback {
            ObserverCallbackInfo& cbInfo = g_observerCallbacks[slotId];
            cbInfo.env = env;
            cbInfo.slotId = slotId;
            napi_create_reference(env, args[1], 1, &cbInfo.callbackRef);
            // 返回一个 callback，捕获 slotId
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
static void onBurstImageCallback(const std::string& sessionId, void* buffer, size_t size, bool isFinal) {
    if (!buffer || size == 0) {
        OH_LOG_ERROR(LOG_APP, "Invalid buffer in onBurstImageCallback");
        return;
    }

    // 在锁保护下获取回调引用
    napi_env callbackEnv = nullptr;
    napi_ref callbackRef = nullptr;
    bool callbackValid = false;

    {
        std::lock_guard<std::mutex> lock(g_burstMutex);
        callbackValid = g_burstImageCallbackValid;
        callbackEnv = g_burstEnv;
        callbackRef = g_burstImageCallbackRef;
    }

    if (!callbackValid || !callbackRef || !callbackEnv) {
        OH_LOG_ERROR(LOG_APP, "Burst image callback not ready");
        return;
    }

    OH_LOG_INFO(LOG_APP, "onBurstImageCallback sessionId: %{public}s, size: %{public}zu, isFinal: %{public}d",
                sessionId.c_str(), size, isFinal);

    // 复制 buffer 数据
    void* copyBuffer = malloc(size);
    if (copyBuffer == nullptr) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate memory for burst image buffer");
        return;
    }
    std::memcpy(copyBuffer, buffer, size);

    // 创建异步工作
    napi_value asyncResourceName = nullptr;
    napi_create_string_utf8(callbackEnv, "onBurstImage", NAPI_AUTO_LENGTH, &asyncResourceName);

    // 将 sessionId 和 isFinal 作为 data 的一部分传递
    struct BurstImageData {
        std::string sessionId;
        void* buffer;
        size_t size;
        bool isFinal;
        napi_env env;
        napi_ref callbackRef;
    };
    BurstImageData* imageData = new BurstImageData{sessionId, copyBuffer, size, isFinal, callbackEnv, callbackRef};

    napi_async_work work;
    napi_status status = napi_create_async_work(
        callbackEnv, nullptr, asyncResourceName,
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

            // 创建 ImageData 对象
            napi_value imageDataObj;
            napi_create_object(envLocal, &imageDataObj);

            // 设置 sessionId
            napi_value sessionIdValue;
            napi_create_string_utf8(envLocal, imageData->sessionId.c_str(),
                                     imageData->sessionId.length(), &sessionIdValue);
            napi_set_named_property(envLocal, imageDataObj, "sessionId", sessionIdValue);

            // 设置 buffer
            napi_set_named_property(envLocal, imageDataObj, "buffer", arrayBuffer);

            // 设置 isFinal
            napi_value isFinalValue;
            napi_get_boolean(envLocal, imageData->isFinal, &isFinalValue);
            napi_set_named_property(envLocal, imageDataObj, "isFinal", isFinalValue);

            napi_get_reference_value(envLocal, imageData->callbackRef, &callback);
            if (callback) {
                // 传递 2 个参数: buffer, isFinal
                napi_value args[2];
                args[0] = arrayBuffer;  // buffer
                args[1] = isFinalValue; // isFinal
                napi_value retVal;
                napi_call_function(envLocal, nullptr, callback, 2, args, &retVal);
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

    napi_queue_async_work_with_qos(callbackEnv, work, napi_qos_user_initiated);
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
    // 在锁保护下获取回调引用
    napi_env callbackEnv = nullptr;
    napi_ref callbackRef = nullptr;
    bool callbackValid = false;

    {
        std::lock_guard<std::mutex> lock(g_burstMutex);
        callbackValid = g_burstProgressCallbackValid;
        callbackEnv = g_burstEnv;
        callbackRef = g_burstProgressCallbackRef;
    }

    if (!callbackValid || !callbackRef || !callbackEnv) {
        return;
    }

    // 创建异步工作
    napi_value asyncResourceName = nullptr;
    napi_create_string_utf8(callbackEnv, "onBurstProgress", NAPI_AUTO_LENGTH, &asyncResourceName);

    // 复制进度数据
    struct BurstProgressData {
        exposhot::BurstProgress progress;
        napi_env env;
        napi_ref callbackRef;
    };
    BurstProgressData* progressCopy = new BurstProgressData{progress, callbackEnv, callbackRef};

    napi_async_work work;
    napi_status status = napi_create_async_work(
        callbackEnv, nullptr, asyncResourceName,
        [](napi_env envLocal, void* data) {
            // 异步执行阶段(空操作)
        },
        [](napi_env envLocal, napi_status status, void* data) {
            BurstProgressData* progressData = static_cast<BurstProgressData*>(data);

            // 主线程回调阶段
            napi_value callback = nullptr;
            napi_get_reference_value(envLocal, progressData->callbackRef, &callback);
            if (callback) {
                // 创建进度对象
                napi_value progressObj;
                napi_create_object(envLocal, &progressObj);

                // 设置属性
                napi_value stateValue;
                napi_create_int32(envLocal, static_cast<int32_t>(progressData->progress.state), &stateValue);
                napi_set_named_property(envLocal, progressObj, "state", stateValue);

                napi_value capturedFramesValue;
                napi_create_int32(envLocal, progressData->progress.capturedFrames, &capturedFramesValue);
                napi_set_named_property(envLocal, progressObj, "capturedFrames", capturedFramesValue);

                napi_value processedFramesValue;
                napi_create_int32(envLocal, progressData->progress.processedFrames, &processedFramesValue);
                napi_set_named_property(envLocal, progressObj, "processedFrames", processedFramesValue);

                napi_value totalFramesValue;
                napi_create_int32(envLocal, progressData->progress.totalFrames, &totalFramesValue);
                napi_set_named_property(envLocal, progressObj, "totalFrames", totalFramesValue);

                napi_value messageValue;
                napi_create_string_utf8(envLocal, progressData->progress.message.c_str(), progressData->progress.message.length(), &messageValue);
                napi_set_named_property(envLocal, progressObj, "message", messageValue);

                // 设置 sessionId
                napi_value sessionIdValue;
                napi_create_string_utf8(envLocal, progressData->progress.sessionId.c_str(), progressData->progress.sessionId.length(), &sessionIdValue);
                napi_set_named_property(envLocal, progressObj, "sessionId", sessionIdValue);

                // 调用回调
                napi_value retVal;
                napi_call_function(envLocal, nullptr, callback, 1, &progressObj, &retVal);
            }

            // 清理
            delete progressData;
        },
        progressCopy, &work);

    if (status != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "Failed to create async work for burst progress callback");
        delete progressCopy;
        return;
    }

    napi_queue_async_work_with_qos(callbackEnv, work, napi_qos_user_initiated);
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
        g_burstProgressCallbackValid = false;
        if (g_burstProgressCallbackRef && g_burstEnv) {
            napi_delete_reference(g_burstEnv, g_burstProgressCallbackRef);
        }
        g_burstEnv = env;
        napi_create_reference(env, args[1], 1, &g_burstProgressCallbackRef);
        g_burstProgressCallbackValid = true;
    }

    // 保存图像回调引用
    if (argc >= 3 && args[2] != nullptr) {
        std::lock_guard<std::mutex> lock(g_burstMutex);
        g_burstImageCallbackValid = false;
        if (g_burstImageCallbackRef && g_burstEnv) {
            napi_delete_reference(g_burstEnv, g_burstImageCallbackRef);
        }
        g_burstEnv = env;
        napi_create_reference(env, args[2], 1, &g_burstImageCallbackRef);
        g_burstImageCallbackValid = true;
    }

    // 设置回调
    exposhot::CaptureManager::getInstance().setProgressCallback(onBurstProgressCallback);
    exposhot::CaptureManager::getInstance().setImageCallback(onBurstImageCallback);

    // 初始化并启动连拍
    if (!exposhot::CaptureManager::getInstance().init()) {
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
    // 先清除回调有效性，防止新的异步工作被创建
    {
        std::lock_guard<std::mutex> lock(g_burstMutex);
        g_burstProgressCallbackValid = false;
        g_burstImageCallbackValid = false;
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

// 模拟堆叠过程（读取 RAWFILE 文件到内存，用于测试堆叠功能）
static napi_value MockStackProcess(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    // 解析参数
    std::string surfaceId;
    int32_t frameIndex = 0;

    if (argc >= 1) {
        size_t surfaceIdLen = 0;
        napi_get_value_string_utf8(env, args[0], nullptr, 0, &surfaceIdLen);
        surfaceId.resize(surfaceIdLen);
        napi_get_value_string_utf8(env, args[0], &surfaceId[0], surfaceIdLen + 1, &surfaceIdLen);
    }

    if (argc >= 2) {
        napi_get_value_int32(env, args[1], &frameIndex);
    }

    OH_LOG_INFO(LOG_APP, "MockStackProcess called: surfaceId=%{public}s, frameIndex=%{public}d",
                surfaceId.c_str(), frameIndex);

    // 读取本地 RAWFILE 文件到内存
    // 定义测试用的 DNG 文件列表
    static const char* RAW_FILE_LIST[] = {
        "IMG_20260314_010259.dng",
        "IMG_20260314_010348.dng",
        "IMG_20260314_010521.dng",
        "IMG_20260314_010555.dng"
    };
    static const int RAW_FILE_COUNT = 4;

    // 使用全局 ResourceManager
    if (g_resourceManager == nullptr) {
        OH_LOG_ERROR(LOG_APP, "ResourceManager not initialized, call initCamera with resourceManager first");

        napi_value resultObj;
        napi_create_object(env, &resultObj);
        napi_value successValue;
        napi_get_boolean(env, false, &successValue);
        napi_set_named_property(env, resultObj, "success", successValue);
        return resultObj;
    }

    // 根据 frameIndex 选择要读取的文件
    int fileIndex = frameIndex % RAW_FILE_COUNT;
    const char* fileName = RAW_FILE_LIST[fileIndex];

    OH_LOG_INFO(LOG_APP, "Reading rawfile: %{public}s (frameIndex=%{public}d, fileIndex=%{public}d)",
                fileName, frameIndex, fileIndex);

    // 打开 RawFile（使用全局 ResourceManager）
    RawFile* rawFile = OH_ResourceManager_OpenRawFile(g_resourceManager, fileName);
    if (rawFile == nullptr) {
        OH_LOG_ERROR(LOG_APP, "Failed to open rawfile: %{public}s", fileName);

        napi_value resultObj;
        napi_create_object(env, &resultObj);
        napi_value successValue;
        napi_get_boolean(env, false, &successValue);
        napi_set_named_property(env, resultObj, "success", successValue);
        return resultObj;
    }

    // 获取文件大小
    long fileSize = OH_ResourceManager_GetRawFileSize(rawFile);
    OH_LOG_INFO(LOG_APP, "Rawfile size: %{public}ld bytes", fileSize);

    if (fileSize <= 0) {
        OH_LOG_ERROR(LOG_APP, "Invalid rawfile size: %{public}ld", fileSize);
        OH_ResourceManager_CloseRawFile(rawFile);

        napi_value resultObj;
        napi_create_object(env, &resultObj);
        napi_value successValue;
        napi_get_boolean(env, false, &successValue);
        napi_set_named_property(env, resultObj, "success", successValue);
        return resultObj;
    }

    // 分配内存并读取文件内容
    void* bufferCopy = malloc((size_t) fileSize);
    int bytesRead = OH_ResourceManager_ReadRawFile(rawFile, bufferCopy, fileSize);

    OH_LOG_INFO(LOG_APP, "Read %{public}d bytes from rawfile", bytesRead);
    std::unique_ptr<exposhot::ImageProcessor> processor = std::make_unique<exposhot::ImageProcessor>();
    
    exposhot::Bgra16Raw bgra16Raw = processor->dngToBGRA16(bufferCopy, fileSize);
    
    std::vector<float> dx {0.0f, 0.0f};
    std::vector<float> dy {0.0f, 0.0f};
    
    // NOTE 从GPU获取已堆叠的结果
    uint16_t* stackedRes;
    if (fileIndex != 0) {
        exposhot::MeanRes res = processor->MotionAnalysisAndStack(stackedRes, bgra16Raw.data, bgra16Raw.width, bgra16Raw.height);
        dx[0] = res.mean_x[0];
        dx[1] = res.mean_x[1];
        dy[0] = res.mean_y[0];
        dy[1] = res.mean_y[1];
    }
    // NOTE 实际处理区域 开始  当前图像信息都在bgra16Raw中偏移量为dx和dy

    // NOTE 实际处理区域 结束

    // 关闭文件（ResourceManager 是全局的，不需要释放）
    OH_ResourceManager_CloseRawFile(rawFile);

    // 创建返回结果对象
    napi_value resultObj;
    napi_create_object(env, &resultObj);

    // 设置 success
    napi_value successValue;
    napi_get_boolean(env, true, &successValue);
    napi_set_named_property(env, resultObj, "success", successValue);

    // 设置 nextFrameIndex
    napi_value frameIndexValue;
    napi_create_int32(env, frameIndex + 1, &frameIndexValue);
    napi_set_named_property(env, resultObj, "nextFrameIndex", frameIndexValue);

    // 设置 message
    napi_value messageValue;
    std::string msg = "Frame " + std::to_string(frameIndex) + " (" + fileName + ") processed, " +
                      std::to_string(bytesRead) + " bytes read";
    napi_create_string_utf8(env, msg.c_str(), NAPI_AUTO_LENGTH, &messageValue);
    napi_set_named_property(env, resultObj, "message", messageValue);

    // 设置 fileName
    napi_value fileNameValue;
    napi_create_string_utf8(env, fileName, NAPI_AUTO_LENGTH, &fileNameValue);
    napi_set_named_property(env, resultObj, "fileName", fileNameValue);

    // 设置 fileSize
    napi_value fileSizeValue;
    napi_create_int32(env, static_cast<int32_t>(fileSize), &fileSizeValue);
    napi_set_named_property(env, resultObj, "fileSize", fileSizeValue);

    // 创建并设置 buffer (ArrayBuffer)
    if (bytesRead > 0) {
        void* outputData = nullptr;
        napi_value arrayBuffer = nullptr;
        napi_status status = napi_create_arraybuffer(env, bytesRead, &outputData, &arrayBuffer);

        if (status == napi_ok && outputData != nullptr) {
            std::memcpy(outputData, bufferCopy, bytesRead);
            napi_set_named_property(env, resultObj, "buffer", arrayBuffer);

            OH_LOG_INFO(LOG_APP, "Created ArrayBuffer with %{public}d bytes", bytesRead);
        } else {
            OH_LOG_ERROR(LOG_APP, "Failed to create ArrayBuffer");
        }
    }

    return resultObj;
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
