#include <napi/native_api.h>
#include <mutex>
#include <map>
#include "expo_camera.h"
#include "hilog/log.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "NapiExpoCamera"

// 全局回调存储
static napi_ref g_photoCallbackRef = nullptr;
static napi_env g_env = nullptr;
static std::mutex g_callbackMutex;

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

// 保存照片数据，用于回调
static void onPhotoData(void* buffer, size_t size) {
//    std::lock_guard<std::mutex> lock(g_callbackMutex);

    if (!g_photoCallbackRef || !g_env || !buffer || size == 0) {
        OH_LOG_ERROR(LOG_APP, "Photo callback not ready or invalid buffer");
        return;
    }

    napi_value callback;
    napi_get_reference_value(g_env, g_photoCallbackRef, &callback);

    if (!callback) {
        OH_LOG_ERROR(LOG_APP, "Callback is null");
        return;
    }

    // 创建 ArrayBuffer
    void* data = nullptr;
    napi_value arrayBuffer;
    napi_create_arraybuffer(g_env, size, &data, &arrayBuffer);

    // 复制数据
    memcpy(data, buffer, size);

    // 调用回调
    napi_value global;
    napi_get_global(g_env, &global);

    napi_value argv[1] = { arrayBuffer };
    napi_call_function(g_env, global, callback, 1, argv, nullptr);

    OH_LOG_INFO(LOG_APP, "Photo callback invoked, size=%{public}zu", size);
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
//        std::lock_guard<std::mutex> lock(g_callbackMutex);

        // 释放旧的回调引用
        if (g_photoCallbackRef) {
            napi_delete_reference(g_env, g_photoCallbackRef);
            g_photoCallbackRef = nullptr;
        }

        // 创建新的引用
        g_env = env;
        napi_create_reference(env, args[0], 1, &g_photoCallbackRef);
    }

    // 设置回调并触发拍照
    ExpoCamera& camera = ExpoCamera::getInstance();
    camera.setPhotoCallback(onPhotoData);
    Camera_ErrorCode err = camera.takePhoto(onPhotoData);

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(err), &result);
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

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"initCamera", nullptr, InitCamera, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"releaseCamera", nullptr, ReleaseCamera, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"switchSurface", nullptr, SwitchSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startPreview", nullptr, StartPreview, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopPreview", nullptr, StopPreview, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"takePhoto", nullptr, TakePhoto, nullptr, nullptr, nullptr, napi_default, nullptr},
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
