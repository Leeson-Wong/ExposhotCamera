#include "expo_camera.h"
#include "hilog/log.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "ExpoCamera"

ExpoCamera& ExpoCamera::getInstance() {
    static ExpoCamera instance;
    return instance;
}

ExpoCamera::ExpoCamera() {}

ExpoCamera::~ExpoCamera() {
    release();
}

Camera_ErrorCode ExpoCamera::init(CaptureMode mode) {
//    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        OH_LOG_INFO(LOG_APP, "ExpoCamera already initialized");
        return CAMERA_OK;
    }

    // 保存当前模式
    currentMode_ = mode;
    OH_LOG_INFO(LOG_APP, "Initializing with mode: %{public}d", static_cast<int>(mode));

    // 创建 CameraManager
    Camera_ErrorCode err = OH_Camera_GetCameraManager(&cameraManager_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to create CameraManager: %{public}d", err);
        return err;
    }

    // 获取相机列表
    err = OH_CameraManager_GetSupportedCameras(cameraManager_, &cameras_, &cameraCount_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to get supported cameras: %{public}d", err);
        OH_Camera_DeleteCameraManager(cameraManager_);
        cameraManager_ = nullptr;
        return err;
    }

    OH_LOG_INFO(LOG_APP, "Found %{public}u cameras", cameraCount_);

    // 根据模式选择摄像头
    int32_t selectedIndex = selectCameraForMode(mode);
    if (selectedIndex < 0) {
        OH_LOG_ERROR(LOG_APP, "No suitable camera found for mode: %{public}d", static_cast<int>(mode));
        OH_CameraManager_DeleteSupportedCameras(cameraManager_, cameras_, cameraCount_);
        cameras_ = nullptr;
        OH_Camera_DeleteCameraManager(cameraManager_);
        cameraManager_ = nullptr;
        return CAMERA_INVALID_ARGUMENT;
    }
    currentCameraIndex_ = static_cast<uint32_t>(selectedIndex);
    OH_LOG_INFO(LOG_APP, "Selected camera index: %{public}u for mode: %{public}d",
                currentCameraIndex_, static_cast<int>(mode));

    // 创建 CameraInput
    err = createCameraInput();
    if (err != CAMERA_OK) {
        OH_CameraManager_DeleteSupportedCameras(cameraManager_, cameras_, cameraCount_);
        cameras_ = nullptr;
        OH_Camera_DeleteCameraManager(cameraManager_);
        cameraManager_ = nullptr;
        return err;
    }

    // 创建 CaptureSession
    err = createCaptureSession();
    if (err != CAMERA_OK) {
        OH_CameraInput_Release(cameraInput_);
        cameraInput_ = nullptr;
        OH_CameraManager_DeleteSupportedCameras(cameraManager_, cameras_, cameraCount_);
        cameras_ = nullptr;
        OH_Camera_DeleteCameraManager(cameraManager_);
        cameraManager_ = nullptr;
        return err;
    }

    initialized_ = true;
    OH_LOG_INFO(LOG_APP, "ExpoCamera initialized successfully");
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::release() {
//    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return CAMERA_OK;
    }

    previewing_ = false;
    photoOutputAdded_ = false;

    // 清理活跃状态
    activeSlotId_.clear();
    activeSurfaceId_.clear();

    // 清理观察者列表
    observers_.clear();

    // 释放 Session
    if (captureSession_) {
        OH_CaptureSession_Stop(captureSession_);
        OH_CaptureSession_Release(captureSession_);
        captureSession_ = nullptr;
    }

    // 释放 PreviewOutput
    if (previewOutput_) {
        OH_PreviewOutput_Release(previewOutput_);
        previewOutput_ = nullptr;
    }

    // 释放 PhotoOutput
    if (photoOutput_) {
        OH_PhotoOutput_Release(photoOutput_);
        photoOutput_ = nullptr;
    }

    // 释放 CameraInput
    if (cameraInput_) {
        OH_CameraInput_Release(cameraInput_);
        cameraInput_ = nullptr;
    }

    // 释放相机列表
    if (cameras_) {
        OH_CameraManager_DeleteSupportedCameras(cameraManager_, cameras_, cameraCount_);
        cameras_ = nullptr;
    }

    // 释放 CameraManager
    if (cameraManager_) {
        OH_Camera_DeleteCameraManager(cameraManager_);
        cameraManager_ = nullptr;
    }

    initialized_ = false;
    OH_LOG_INFO(LOG_APP, "ExpoCamera released");
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::switchSurface(const std::string& surfaceId) {
//    std::lock_guard<std::mutex> lock(mutex_);
    return switchSurfaceInternal(surfaceId);
}

// 内部版本，不加锁（调用者已持有锁）
Camera_ErrorCode ExpoCamera::switchSurfaceInternal(const std::string& surfaceId) {
    if (!initialized_) {
        OH_LOG_ERROR(LOG_APP, "ExpoCamera not initialized");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    if (!previewing_) {
        OH_LOG_WARN(LOG_APP, "Preview not running, use startPreview instead");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    if (surfaceId == activeSurfaceId_) {
        OH_LOG_INFO(LOG_APP, "Same surface, skip switch");
        return CAMERA_OK;
    }

    OH_LOG_INFO(LOG_APP, "Switching surface from %{public}s to %{public}s",
                activeSurfaceId_.c_str(), surfaceId.c_str());

    Camera_ErrorCode err;

    // 1. 停止 Session
    err = OH_CaptureSession_Stop(captureSession_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to stop session: %{public}d", err);
        // 继续尝试切换
    }

    // 2. 重新设置 SessionMode（必须在 BeginConfig 之前！）
    err = OH_CaptureSession_SetSessionMode(captureSession_, Camera_SceneMode::NORMAL_PHOTO);
    if (err != CAMERA_OK) {
        OH_LOG_WARN(LOG_APP, "Failed to re-set session mode: %{public}d", err);
    } else {
        OH_LOG_INFO(LOG_APP, "Session mode re-set to NORMAL_PHOTO before BeginConfig");
    }

    // 3. 开始重新配置
    err = OH_CaptureSession_BeginConfig(captureSession_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to begin config: %{public}d", err);
        return err;
    }

    // 4. 移除旧的 PreviewOutput
    if (previewOutput_) {
        err = OH_CaptureSession_RemovePreviewOutput(captureSession_, previewOutput_);
        if (err != CAMERA_OK) {
            OH_LOG_WARN(LOG_APP, "Failed to remove preview output: %{public}d", err);
        }
        err = OH_PreviewOutput_Release(previewOutput_);
        if (err != CAMERA_OK) {
            OH_LOG_WARN(LOG_APP, "Failed to release preview output: %{public}d", err);
        }
        previewOutput_ = nullptr;
    }

    // 4. 移除 PhotoOutput（需要在 BeginConfig 后移除，稍后重新添加）
    if (photoOutputAdded_ && photoOutput_) {
        err = OH_CaptureSession_RemovePhotoOutput(captureSession_, photoOutput_);
        if (err != CAMERA_OK) {
            OH_LOG_WARN(LOG_APP, "Failed to remove photo output: %{public}d", err);
        }
        photoOutputAdded_ = false;
    }

    // 4. 创建新的 PreviewOutput
    err = createPreviewOutput(surfaceId);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to create new preview output: %{public}d", err);
        // 尝试恢复：重新创建旧的 preview output
        OH_CaptureSession_CommitConfig(captureSession_);
        createPreviewOutput(activeSurfaceId_);
        OH_CaptureSession_AddPreviewOutput(captureSession_, previewOutput_);
        OH_CaptureSession_Start(captureSession_);
        return err;
    }

    // 5. 添加新的 PreviewOutput
    err = OH_CaptureSession_AddPreviewOutput(captureSession_, previewOutput_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to add preview output: %{public}d", err);
        OH_PreviewOutput_Release(previewOutput_);
        previewOutput_ = nullptr;
        return err;
    }

    // 6. 重新添加 PhotoOutput
    if (photoOutput_) {
        err = OH_CaptureSession_AddPhotoOutput(captureSession_, photoOutput_);
        if (err != CAMERA_OK) {
            OH_LOG_WARN(LOG_APP, "Failed to re-add photo output: %{public}d", err);
            // 拍照功能可选，继续
        } else {
            photoOutputAdded_ = true;
            OH_LOG_INFO(LOG_APP, "PhotoOutput re-added to session");
        }
    }

    // 7. 提交配置
    err = OH_CaptureSession_CommitConfig(captureSession_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to commit config: %{public}d", err);
        return err;
    }

    // 7. 启动 Session
    err = OH_CaptureSession_Start(captureSession_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to start session: %{public}d", err);
        return err;
    }

    // 重新设置默认对焦模式和曝光模式（session 重启后需要重新设置）
    bool focusModeSupported = false;
    err = OH_CaptureSession_IsFocusModeSupported(captureSession_,
        Camera_FocusMode::FOCUS_MODE_CONTINUOUS_AUTO, &focusModeSupported);
    if (err == CAMERA_OK && focusModeSupported) {
        OH_CaptureSession_SetFocusMode(captureSession_, Camera_FocusMode::FOCUS_MODE_CONTINUOUS_AUTO);
    }

    bool exposureModeSupported = false;
    err = OH_CaptureSession_IsExposureModeSupported(captureSession_,
        Camera_ExposureMode::EXPOSURE_MODE_CONTINUOUS_AUTO, &exposureModeSupported);
    if (err == CAMERA_OK && exposureModeSupported) {
        OH_CaptureSession_SetExposureMode(captureSession_, Camera_ExposureMode::EXPOSURE_MODE_CONTINUOUS_AUTO);
    }

    activeSurfaceId_ = surfaceId;
    OH_LOG_INFO(LOG_APP, "Surface switched successfully to %{public}s", surfaceId.c_str());
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::startPreview(const std::string& surfaceId) {
//    std::lock_guard<std::mutex> lock(mutex_);
    return startPreviewInternal(surfaceId);
}

// 内部版本，不加锁（调用者已持有锁）
Camera_ErrorCode ExpoCamera::startPreviewInternal(const std::string& surfaceId) {
    if (!initialized_) {
        OH_LOG_ERROR(LOG_APP, "ExpoCamera not initialized");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    if (previewing_) {
        OH_LOG_INFO(LOG_APP, "Preview already running");
        return CAMERA_OK;
    }

    Camera_ErrorCode err;

    // 设置会话模式为普通拍照模式（关键！不设置会导致拍照返回 CAMERA_OPERATION_NOT_ALLOWED）
    err = OH_CaptureSession_SetSessionMode(captureSession_, Camera_SceneMode::NORMAL_PHOTO);
    if (err != CAMERA_OK) {
        OH_LOG_WARN(LOG_APP, "Failed to set session mode: %{public}d", err);
        // 继续尝试，某些设备可能不需要
    } else {
        OH_LOG_INFO(LOG_APP, "Session mode set to NORMAL_PHOTO");
    }

    // 开始配置 Session
    err = OH_CaptureSession_BeginConfig(captureSession_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to begin session config: %{public}d", err);
        return err;
    }

    // 添加 CameraInput
    err = OH_CaptureSession_AddInput(captureSession_, cameraInput_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to add input: %{public}d", err);
        return err;
    }

    // 创建 PreviewOutput
    err = createPreviewOutput(surfaceId);
    if (err != CAMERA_OK) {
        return err;
    }

    // 添加 PreviewOutput 到 Session
    err = OH_CaptureSession_AddPreviewOutput(captureSession_, previewOutput_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to add preview output: %{public}d", err);
        OH_PreviewOutput_Release(previewOutput_);
        previewOutput_ = nullptr;
        return err;
    }

    // 创建并添加 PhotoOutput（用于拍照）
    // 如果 PhotoOutput 已存在，直接复用
    if (!photoOutput_) {
        err = createPhotoOutput();
        if (err != CAMERA_OK) {
            OH_LOG_WARN(LOG_APP, "Failed to create photo output, photo feature unavailable");
        }
    }

    if (photoOutput_) {
        err = OH_CaptureSession_AddPhotoOutput(captureSession_, photoOutput_);
        if (err != CAMERA_OK) {
            OH_LOG_WARN(LOG_APP, "Failed to add photo output: %{public}d", err);
            // 拍照功能可选，不影响预览
        } else {
            photoOutputAdded_ = true;
            OH_LOG_INFO(LOG_APP, "PhotoOutput added to session");
        }
    }

    // 提交配置
    err = OH_CaptureSession_CommitConfig(captureSession_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to commit session config: %{public}d", err);
        return err;
    }

    // 启动 Session（Session 启动后预览流自动开始）
    err = OH_CaptureSession_Start(captureSession_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to start session: %{public}d", err);
        return err;
    }

    // 注意：OH_CaptureSession_Start() 已启动预览流
    // OH_PreviewOutput_Start() 仅用于 Session 运行中单独恢复预览的场景
    // 初始化时调用会导致 CAMERA_SERVICE_FATAL_ERROR

    // 设置默认对焦模式为连续自动对焦
    bool focusModeSupported = false;
    err = OH_CaptureSession_IsFocusModeSupported(captureSession_,
        Camera_FocusMode::FOCUS_MODE_CONTINUOUS_AUTO, &focusModeSupported);
    if (err == CAMERA_OK && focusModeSupported) {
        err = OH_CaptureSession_SetFocusMode(captureSession_, Camera_FocusMode::FOCUS_MODE_CONTINUOUS_AUTO);
        if (err == CAMERA_OK) {
            OH_LOG_INFO(LOG_APP, "Default focus mode set to CONTINUOUS_AUTO");
        } else {
            OH_LOG_WARN(LOG_APP, "Failed to set default focus mode: %{public}d", err);
        }
    } else {
        OH_LOG_WARN(LOG_APP, "CONTINUOUS_AUTO focus mode not supported");
    }

    // 设置默认曝光模式为连续自动曝光
    bool exposureModeSupported = false;
    err = OH_CaptureSession_IsExposureModeSupported(captureSession_,
        Camera_ExposureMode::EXPOSURE_MODE_CONTINUOUS_AUTO, &exposureModeSupported);
    if (err == CAMERA_OK && exposureModeSupported) {
        err = OH_CaptureSession_SetExposureMode(captureSession_, Camera_ExposureMode::EXPOSURE_MODE_CONTINUOUS_AUTO);
        if (err == CAMERA_OK) {
            OH_LOG_INFO(LOG_APP, "Default exposure mode set to CONTINUOUS_AUTO");
        } else {
            OH_LOG_WARN(LOG_APP, "Failed to set default exposure mode: %{public}d", err);
        }
    } else {
        OH_LOG_WARN(LOG_APP, "CONTINUOUS_AUTO exposure mode not supported");
    }

    activeSurfaceId_ = surfaceId;
    previewing_ = true;
    OH_LOG_INFO(LOG_APP, "Preview started successfully");

    // 如果没有活跃的 slot，设置当前 surfaceId 对应的 slot 为活跃
    if (activeSlotId_.empty()) {
        // 查找使用此 surfaceId 的观察者
        for (const auto& observer : observers_) {
            if (observer.surfaceId == surfaceId) {
                activeSlotId_ = observer.slotId;
                OH_LOG_INFO(LOG_APP, "Set active slot: %{public}s", activeSlotId_.c_str());
                break;
            }
        }
    }

    // 通知所有观察者预览已启动
    notifyAllObservers();

    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::stopPreview() {
//    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || !previewing_) {
        return CAMERA_OK;
    }

    // 停止 PreviewOutput
    if (previewOutput_) {
        OH_PreviewOutput_Stop(previewOutput_);
    }

    // 停止 Session
    if (captureSession_) {
        OH_CaptureSession_Stop(captureSession_);
    }

    previewing_ = false;
    OH_LOG_INFO(LOG_APP, "Preview stopped");
    return CAMERA_OK;
}

// 拍照回调（静态方法）
void ExpoCamera::onFrameStart(Camera_PhotoOutput* photoOutput) {
    OH_LOG_INFO(LOG_APP, "Photo frame start");
}

void ExpoCamera::onFrameShutter(Camera_PhotoOutput* photoOutput, Camera_FrameShutterInfo* info) {
    OH_LOG_INFO(LOG_APP, "Photo frame shutter");
}

void ExpoCamera::onFrameEnd(Camera_PhotoOutput* photoOutput, int32_t frameCount) {
    OH_LOG_INFO(LOG_APP, "Photo frame end, count=%{public}d", frameCount);
    // 注意：实际照片数据在 onPhotoAvailable 中获取
}

void ExpoCamera::onError(Camera_PhotoOutput* photoOutput, Camera_ErrorCode errorCode) {
    OH_LOG_ERROR(LOG_APP, "Photo output error: %{public}d", errorCode);

    // 通过回调通知上层（如 CaptureManager）
    ExpoCamera& self = ExpoCamera::getInstance();
    if (self.photoErrorCallback_) {
        self.photoErrorCallback_(static_cast<int32_t>(errorCode));
    }
}

// 照片可用回调（获取实际照片数据）
void ExpoCamera::onPhotoAvailable(Camera_PhotoOutput* photoOutput, OH_PhotoNative* photo) {
    OH_LOG_INFO(LOG_APP, "onPhotoAvailable start!");

    if (photo == nullptr) {
        OH_LOG_ERROR(LOG_APP, "onPhotoAvailable: photo is null");
        return;
    }

    ExpoCamera& self = ExpoCamera::getInstance();

    // 获取主图像
    OH_ImageNative* imageNative = nullptr;
    Camera_ErrorCode errCode = OH_PhotoNative_GetMainImage(photo, &imageNative);
    if (errCode != CAMERA_OK || imageNative == nullptr) {
        OH_LOG_ERROR(LOG_APP, "OH_PhotoNative_GetMainImage failed: %{public}d", errCode);
        return;
    }

    // 获取图像尺寸
    Image_Size size;
    Image_ErrorCode imageErr = OH_ImageNative_GetImageSize(imageNative, &size);
    if (imageErr != IMAGE_SUCCESS) {
        OH_LOG_ERROR(LOG_APP, "OH_ImageNative_GetImageSize failed: %{public}d", imageErr);
        OH_ImageNative_Release(imageNative);
        return;
    }
    OH_LOG_INFO(LOG_APP, "Photo size: %{public}d x %{public}d", size.width, size.height);

    // 获取组件类型数量
    size_t componentTypeSize = 0;
    imageErr = OH_ImageNative_GetComponentTypes(imageNative, nullptr, &componentTypeSize);
    if (imageErr != IMAGE_SUCCESS || componentTypeSize == 0) {
        OH_LOG_ERROR(LOG_APP, "OH_ImageNative_GetComponentTypes failed: %{public}d", imageErr);
        OH_ImageNative_Release(imageNative);
        return;
    }

    // 获取组件类型列表
    uint32_t* components = new (std::nothrow) uint32_t[componentTypeSize];
    if (!components) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate memory for components");
        OH_ImageNative_Release(imageNative);
        return;
    }
    imageErr = OH_ImageNative_GetComponentTypes(imageNative, &components, &componentTypeSize);
    if (imageErr != IMAGE_SUCCESS) {
        OH_LOG_ERROR(LOG_APP, "OH_ImageNative_GetComponentTypes failed: %{public}d", imageErr);
        OH_ImageNative_Release(imageNative);
        delete[] components;
        return;
    }

    // 获取缓冲区
    OH_NativeBuffer* nativeBuffer = nullptr;
    imageErr = OH_ImageNative_GetByteBuffer(imageNative, components[0], &nativeBuffer);
    if (imageErr != IMAGE_SUCCESS) {
        OH_LOG_ERROR(LOG_APP, "OH_ImageNative_GetByteBuffer failed: %{public}d", imageErr);
        OH_ImageNative_Release(imageNative);
        delete[] components;
        return;
    }

    // 获取缓冲区大小
    size_t nativeBufferSize = 0;
    imageErr = OH_ImageNative_GetBufferSize(imageNative, components[0], &nativeBufferSize);
    if (imageErr != IMAGE_SUCCESS) {
        OH_LOG_ERROR(LOG_APP, "OH_ImageNative_GetBufferSize failed: %{public}d", imageErr);
        OH_ImageNative_Release(imageNative);
        delete[] components;
        return;
    }
    OH_LOG_INFO(LOG_APP, "Buffer size: %{public}zu", nativeBufferSize);

    // 映射内存
    void* virAddr = nullptr;
    int32_t ret = OH_NativeBuffer_Map(nativeBuffer, &virAddr);
    if (ret != 0) {
        OH_LOG_ERROR(LOG_APP, "OH_NativeBuffer_Map failed: %{public}d", ret);
        OH_ImageNative_Release(imageNative);
        delete[] components;
        return;
    }

    // 复制数据并通过回调传递给上层
    void* bufferCopy = malloc(nativeBufferSize);
    if (bufferCopy) {
        std::memcpy(bufferCopy, virAddr, nativeBufferSize);

        // 转换尺寸类型（HarmonyOS API 返回 int32_t，内部使用 uint32_t）
        uint32_t imgWidth = static_cast<uint32_t>(size.width);
        uint32_t imgHeight = static_cast<uint32_t>(size.height);

        // 通过回调通知上层（如 CaptureManager）
        if (self.photoCapturedCallback_) {
            self.photoCapturedCallback_(bufferCopy, nativeBufferSize, imgWidth, imgHeight);
        } else {
            OH_LOG_WARN(LOG_APP, "No photo captured callback registered, buffer will be freed");
            free(bufferCopy);
        }
    } else {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate buffer for photo copy");
    }

    // 释放资源
    delete[] components;
    OH_NativeBuffer_Unmap(nativeBuffer);
    OH_ImageNative_Release(imageNative);

    OH_LOG_INFO(LOG_APP, "onPhotoAvailable end");
}

Camera_ErrorCode ExpoCamera::setZoomRatio(float ratio) {
//    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        OH_LOG_ERROR(LOG_APP, "ExpoCamera not initialized");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    if (!captureSession_) {
        OH_LOG_ERROR(LOG_APP, "CaptureSession not ready");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    // 获取缩放范围来判断是否支持缩放
    float minZoom = 1.0f, maxZoom = 1.0f;
    Camera_ErrorCode err = OH_CaptureSession_GetZoomRatioRange(captureSession_, &minZoom, &maxZoom);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to get zoom range: %{public}d", err);
        return err;
    }

    // 限制在有效范围内
    if (ratio < minZoom) ratio = minZoom;
    if (ratio > maxZoom) ratio = maxZoom;

    err = OH_CaptureSession_SetZoomRatio(captureSession_, ratio);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to set zoom ratio: %{public}d", err);
        return err;
    }

    OH_LOG_INFO(LOG_APP, "Zoom ratio set to: %{public}f", ratio);
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::getZoomRatio(float* ratio) {
    if (!ratio) {
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

//    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || !captureSession_) {
        *ratio = 1.0f;
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    Camera_ErrorCode err = OH_CaptureSession_GetZoomRatio(captureSession_, ratio);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to get zoom ratio: %{public}d", err);
        *ratio = 1.0f;
        return err;
    }

    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::getZoomRatioRange(float* min, float* max) {
    if (!min || !max) {
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

//    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || !captureSession_) {
        *min = 1.0f;
        *max = 1.0f;
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    Camera_ErrorCode err = OH_CaptureSession_GetZoomRatioRange(captureSession_, min, max);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to get zoom range: %{public}d", err);
        *min = 1.0f;
        *max = 1.0f;
        return err;
    }

    OH_LOG_INFO(LOG_APP, "Zoom range: %{public}f - %{public}f", *min, *max);
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::isZoomSupported(bool* supported) {
    if (!supported) {
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

//    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || !captureSession_) {
        *supported = false;
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    // 通过检查缩放范围来判断是否支持缩放
    float minZoom = 1.0f, maxZoom = 1.0f;
    Camera_ErrorCode err = OH_CaptureSession_GetZoomRatioRange(captureSession_, &minZoom, &maxZoom);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to get zoom range: %{public}d", err);
        *supported = false;
        return err;
    }

    // 如果 max > min，则支持缩放
    *supported = (maxZoom > minZoom);
    OH_LOG_INFO(LOG_APP, "Zoom supported: %{public}d (range: %{public}f - %{public}f)", *supported, minZoom, maxZoom);
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::setFocusMode(Camera_FocusMode mode) {
//    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        OH_LOG_ERROR(LOG_APP, "ExpoCamera not initialized");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    if (!captureSession_) {
        OH_LOG_ERROR(LOG_APP, "CaptureSession not ready");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    // 检查是否支持该对焦模式
    bool isSupported = false;
    Camera_ErrorCode err = OH_CaptureSession_IsFocusModeSupported(captureSession_, mode, &isSupported);
    if (err != CAMERA_OK || !isSupported) {
        OH_LOG_ERROR(LOG_APP, "Focus mode not supported: mode=%{public}d, err=%{public}d", mode, err);
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    err = OH_CaptureSession_SetFocusMode(captureSession_, mode);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to set focus mode: %{public}d", err);
        return err;
    }

    OH_LOG_INFO(LOG_APP, "Focus mode set to: %{public}d", mode);
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::getFocusMode(Camera_FocusMode* mode) {
    if (!mode) {
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

//    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || !captureSession_) {
        *mode = Camera_FocusMode::FOCUS_MODE_MANUAL;
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    Camera_ErrorCode err = OH_CaptureSession_GetFocusMode(captureSession_, mode);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to get focus mode: %{public}d", err);
        return err;
    }

    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::isFocusModeSupported(Camera_FocusMode mode, bool* supported) {
    if (!supported) {
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

//    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || !captureSession_) {
        *supported = false;
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    Camera_ErrorCode err = OH_CaptureSession_IsFocusModeSupported(captureSession_, mode, supported);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to check focus mode support: %{public}d", err);
        *supported = false;
        return err;
    }

    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::setFocusDistance(float distance) {
    // TODO: 等待验证 API 可用性
    // 目前先返回成功，不做实际操作
    OH_LOG_INFO(LOG_APP, "setFocusDistance: %{public}f (not implemented)", distance);
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::getFocusDistance(float* distance) {
    if (!distance) {
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }
    // TODO: 等待验证 API 可用性
    *distance = 0.0f;
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::getFocusDistanceRange(float* min, float* max) {
    if (!min || !max) {
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }
    // TODO: 等待验证 API 可用性
    *min = 0.0f;
    *max = 10.0f;
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::setFocusPoint(float x, float y) {
//    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        OH_LOG_ERROR(LOG_APP, "ExpoCamera not initialized");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    if (!captureSession_) {
        OH_LOG_ERROR(LOG_APP, "CaptureSession not ready");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    Camera_Point point = { x, y };
    Camera_ErrorCode err = OH_CaptureSession_SetFocusPoint(captureSession_, point);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to set focus point: %{public}d", err);
        return err;
    }

    OH_LOG_INFO(LOG_APP, "Focus point set to: (%{public}f, %{public}f)", x, y);
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::getFocusPoint(float* x, float* y) {
    if (!x || !y) {
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

//    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || !captureSession_) {
        *x = 0.5f;
        *y = 0.5f;
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    Camera_Point point;
    Camera_ErrorCode err = OH_CaptureSession_GetFocusPoint(captureSession_, &point);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to get focus point: %{public}d", err);
        *x = 0.5f;
        *y = 0.5f;
        return err;
    }

    *x = point.x;
    *y = point.y;
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::createCameraInput() {
    if (!cameraManager_ || cameraCount_ == 0) {
        OH_LOG_ERROR(LOG_APP, "CameraManager not ready or no cameras");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    Camera_ErrorCode err = OH_CameraManager_CreateCameraInput(
        cameraManager_, &cameras_[currentCameraIndex_], &cameraInput_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to create CameraInput: %{public}d", err);
        return err;
    }

    err = OH_CameraInput_Open(cameraInput_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to open CameraInput: %{public}d", err);
        OH_CameraInput_Release(cameraInput_);
        cameraInput_ = nullptr;
        return err;
    }

    OH_LOG_INFO(LOG_APP, "CameraInput created and opened");
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::createCaptureSession() {
    if (!cameraManager_) {
        OH_LOG_ERROR(LOG_APP, "CameraManager not ready");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    Camera_ErrorCode err = OH_CameraManager_CreateCaptureSession(cameraManager_, &captureSession_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to create CaptureSession: %{public}d", err);
        return err;
    }

    OH_LOG_INFO(LOG_APP, "CaptureSession created");
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::createPreviewOutput(const std::string& surfaceId) {
    if (!cameraManager_ || !captureSession_) {
        OH_LOG_ERROR(LOG_APP, "CameraManager or CaptureSession not ready");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    // 获取支持的输出能力（优先使用带场景模式的版本，与 PhotoOutput 保持一致）
    Camera_OutputCapability* outputCapability = nullptr;
    Camera_ErrorCode err = OH_CameraManager_GetSupportedCameraOutputCapabilityWithSceneMode(
        cameraManager_, &cameras_[currentCameraIndex_], Camera_SceneMode::NORMAL_PHOTO, &outputCapability);
    if (err != CAMERA_OK) {
        OH_LOG_WARN(LOG_APP, "Failed to get output capability with scene mode: %{public}d, fallback to default", err);
        // 回退到不带场景模式的版本
        err = OH_CameraManager_GetSupportedCameraOutputCapability(
            cameraManager_, &cameras_[currentCameraIndex_], &outputCapability);
        if (err != CAMERA_OK) {
            OH_LOG_ERROR(LOG_APP, "Failed to get output capability: %{public}d", err);
            return err;
        }
    }

    // 获取预览 Profile
    if (outputCapability->previewProfilesSize == 0) {
        OH_LOG_ERROR(LOG_APP, "No preview profiles available");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    // 打印所有可用的 preview profiles
    OH_LOG_INFO(LOG_APP, "========== PREVIEW PROFILES START ==========");
    OH_LOG_INFO(LOG_APP, "Total profiles: %{public}u", outputCapability->previewProfilesSize);
    for (uint32_t i = 0; i < outputCapability->previewProfilesSize; i++) {
        Camera_Profile* profile = outputCapability->previewProfiles[i];
        float ratio = static_cast<float>(profile->size.width) / static_cast<float>(profile->size.height);
        OH_LOG_INFO(LOG_APP, "  [Profile %{public}u] %{public}ux%{public}u | ratio=%.3f | format=%{public}d",
                    i, profile->size.width, profile->size.height, ratio, profile->format);
    }

    // 选择与 16:9 比例最接近的 profile，优先选择 1920x1080
    const float targetRatio = 16.0f / 9.0f;  // 1.777...
    const uint32_t preferredWidth = 1920;
    const uint32_t preferredHeight = 1080;
    Camera_Profile* previewProfile = outputCapability->previewProfiles[0];
    float bestDiff = 999.0f;
    bool foundPreferred = false;

    // 先尝试找精确匹配 1920x1080 的
    for (uint32_t i = 0; i < outputCapability->previewProfilesSize; i++) {
        Camera_Profile* profile = outputCapability->previewProfiles[i];
        if (profile->size.width == preferredWidth && profile->size.height == preferredHeight) {
            previewProfile = profile;
            foundPreferred = true;
            OH_LOG_INFO(LOG_APP, "Found preferred profile: 1920x1080");
            break;
        }
    }

    // 如果没找到 1920x1080，选择与 16:9 比例最接近的
    if (!foundPreferred) {
        for (uint32_t i = 0; i < outputCapability->previewProfilesSize; i++) {
            Camera_Profile* profile = outputCapability->previewProfiles[i];
            float ratio = static_cast<float>(profile->size.width) / static_cast<float>(profile->size.height);
            float diff = std::abs(ratio - targetRatio);
            if (diff < bestDiff) {
                bestDiff = diff;
                previewProfile = profile;
            }
        }
    }

    float selectedRatio = static_cast<float>(previewProfile->size.width) / static_cast<float>(previewProfile->size.height);
    OH_LOG_INFO(LOG_APP, "========== SELECTED: %{public}ux%{public}u | ratio=%.3f | diff=%.3f ==========",
                previewProfile->size.width, previewProfile->size.height, selectedRatio, bestDiff);
    OH_LOG_INFO(LOG_APP, "========== PREVIEW PROFILES END ==========");

    // 创建 PreviewOutput
    err = OH_CameraManager_CreatePreviewOutput(
        cameraManager_, previewProfile, surfaceId.c_str(), &previewOutput_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to create preview output: %{public}d", err);
        return err;
    }

    OH_LOG_INFO(LOG_APP, "PreviewOutput created with surfaceId: %{public}s", surfaceId.c_str());
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::createPhotoOutput() {
    if (!cameraManager_) {
        OH_LOG_ERROR(LOG_APP, "CameraManager not ready");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    // 获取支持的输出能力（优先使用带场景模式的版本）
    Camera_OutputCapability* outputCapability = nullptr;
    Camera_ErrorCode err = OH_CameraManager_GetSupportedCameraOutputCapabilityWithSceneMode(
        cameraManager_, &cameras_[currentCameraIndex_], Camera_SceneMode::NORMAL_PHOTO, &outputCapability);
    if (err != CAMERA_OK) {
        OH_LOG_WARN(LOG_APP, "Failed to get output capability with scene mode: %{public}d, fallback to default", err);
        // 回退到不带场景模式的版本
        err = OH_CameraManager_GetSupportedCameraOutputCapability(
            cameraManager_, &cameras_[currentCameraIndex_], &outputCapability);
        if (err != CAMERA_OK) {
            OH_LOG_ERROR(LOG_APP, "Failed to get output capability: %{public}d", err);
            return err;
        }
    }

    // 获取拍照 Profile
    if (outputCapability->photoProfilesSize == 0) {
        OH_LOG_ERROR(LOG_APP, "No photo profiles available");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    Camera_Profile* photoProfile = outputCapability->photoProfiles[0];
    OH_LOG_INFO(LOG_APP, "Photo profile: %{public}ux%{public}u, format=%{public}d",
                photoProfile->size.width, photoProfile->size.height, photoProfile->format);

    // 创建 PhotoOutput
    err = OH_CameraManager_CreatePhotoOutputWithoutSurface(cameraManager_, photoProfile, &photoOutput_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to create photo output: %{public}d", err);
        return err;
    }

    // 注册照片可用回调（使用 OH_PhotoOutput_RegisterPhotoAvailableCallback）
    err = OH_PhotoOutput_RegisterPhotoAvailableCallback(photoOutput_, ExpoCamera::onPhotoAvailable);
    if (err != CAMERA_OK) {
        OH_LOG_WARN(LOG_APP, "Failed to register photo available callback: %{public}d", err);
        // 回调注册失败不影响拍照，只是无法获取数据
    } else {
        OH_LOG_INFO(LOG_APP, "Photo available callback registered");
    }

    OH_LOG_INFO(LOG_APP, "PhotoOutput created");
    return CAMERA_OK;
}

// ==================== 观察者管理 ====================

void ExpoCamera::notifyAllObservers() {
    for (auto& observer : observers_) {
        if (observer.callback) {
            observer.callback(activeSlotId_, activeSurfaceId_);
        }
    }
}

std::string ExpoCamera::registerObserver(const std::string& surfaceId, BindPreviewObserverCallback bindCallback, void* userData) {
    // 生成唯一的 slot ID
    static uint32_t slotCounter = 0;
    std::string slotId = "slot_" + std::to_string(++slotCounter);

    PreviewObserverCallback previewCallback = bindCallback(slotId);
    PreviewObserver observer;
    observer.slotId = slotId;
    observer.surfaceId = surfaceId;
    observer.callback = previewCallback;  // callback 已通过参数传入并绑定
    observer.userData = userData;

    observers_.push_back(observer);

    OH_LOG_INFO(LOG_APP, "Observer registered: %{public}s, surfaceId=%{public}s",
                slotId.c_str(), surfaceId.c_str());

    // 如果当前没有活跃的 slot，切换到这个新 slot
    if (activeSlotId_.empty()) {
        switchToSlot(slotId);
    } else {
        // 通知新观察者当前的活跃状态（callback 已绑定，可以直接调用）
        if (observer.callback) {
            observer.callback(activeSlotId_, activeSurfaceId_);
        }
    }

    return slotId;
}

Camera_ErrorCode ExpoCamera::unregisterObserver(const std::string& slotId) {
    auto it = std::find_if(observers_.begin(), observers_.end(),
        [&](const PreviewObserver& o) { return o.slotId == slotId; });

    if (it == observers_.end()) {
        OH_LOG_WARN(LOG_APP, "Observer not found: %{public}s", slotId.c_str());
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    observers_.erase(it);

    // 如果注销的是当前活跃的，清空活跃状态并通知所有观察者
    if (activeSlotId_ == slotId) {
        activeSlotId_.clear();
        activeSurfaceId_.clear();
        // 通知所有观察者当前没有活跃的 slot
        notifyAllObservers();
    }

    OH_LOG_INFO(LOG_APP, "Observer unregistered: %{public}s", slotId.c_str());
    return CAMERA_OK;
}

Camera_ErrorCode ExpoCamera::switchToSlot(const std::string& slotId) {
    // 查找观察者
    auto it = std::find_if(observers_.begin(), observers_.end(),
        [&](const PreviewObserver& o) { return o.slotId == slotId; });

    if (it == observers_.end()) {
        OH_LOG_ERROR(LOG_APP, "Observer not found: %{public}s", slotId.c_str());
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    // 如果是同一个 slot，跳过
    if (activeSlotId_ == slotId) {
        OH_LOG_INFO(LOG_APP, "Already on slot: %{public}s", slotId.c_str());
        return CAMERA_OK;
    }

    PreviewObserver& newObserver = *it;
    OH_LOG_INFO(LOG_APP, "Switching to slot: %{public}s (surfaceId=%{public}s)",
                slotId.c_str(), newObserver.surfaceId.c_str());

    // 切换 Surface
    Camera_ErrorCode err;

    if (!previewing_) {
        // 预览未启动，直接启动
        err = startPreviewInternal(newObserver.surfaceId);
    } else {
        // 预览已启动，切换 Surface
        err = switchSurfaceInternal(newObserver.surfaceId);
    }

    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to switch to slot: %{public}d", err);
        return err;
    }

    // 更新活跃状态
    activeSlotId_ = slotId;
    activeSurfaceId_ = newObserver.surfaceId;

    // 通知所有观察者
    notifyAllObservers();

    OH_LOG_INFO(LOG_APP, "Switched to slot: %{public}s", slotId.c_str());
    return CAMERA_OK;
}

// ==================== 状态订阅 ====================

void ExpoCamera::subscribeState(const StateCallback& callback) {
    stateCallback_ = callback;
    OH_LOG_INFO(LOG_APP, "State callback subscribed");
}

void ExpoCamera::unsubscribeState() {
//    std::lock_guard<std::mutex> lock(mutex_);
    stateCallback_ = nullptr;
    OH_LOG_INFO(LOG_APP, "State callback unsubscribed");
}

// ==================== 照片回调注册 ====================

void ExpoCamera::setPhotoCapturedCallback(PhotoCapturedCallback callback) {
    photoCapturedCallback_ = callback;
    OH_LOG_INFO(LOG_APP, "Photo captured callback %{public}s", callback ? "registered" : "cleared");
}

void ExpoCamera::setPhotoErrorCallback(PhotoErrorCallback callback) {
    photoErrorCallback_ = callback;
    OH_LOG_INFO(LOG_APP, "Photo error callback %{public}s", callback ? "registered" : "cleared");
}

// ==================== 拍摄模式管理 ====================

bool ExpoCamera::canSwitchMode() const {
    return initialized_ && previewing_ && captureSession_ != nullptr;
}

Camera_ErrorCode ExpoCamera::switchCaptureMode(CaptureMode mode) {
    if (!canSwitchMode()) {
        OH_LOG_ERROR(LOG_APP, "Cannot switch mode: not initialized or not previewing");
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    if (currentMode_ == mode) {
        OH_LOG_INFO(LOG_APP, "Already in mode %{public}d, skip", static_cast<int>(mode));
        return CAMERA_OK;
    }

    OH_LOG_INFO(LOG_APP, "Switching capture mode: %{public}d -> %{public}d",
                static_cast<int>(currentMode_), static_cast<int>(mode));

    Camera_ErrorCode err;

    // 1. 停止 Session
    err = OH_CaptureSession_Stop(captureSession_);
    if (err != CAMERA_OK) {
        OH_LOG_WARN(LOG_APP, "Stop session failed: %{public}d", err);
    }

    // 2. 开始配置
    err = OH_CaptureSession_BeginConfig(captureSession_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "BeginConfig failed: %{public}d", err);
        // 尝试恢复
        OH_CaptureSession_Start(captureSession_);
        return err;
    }

    // 3. 从 Session 移除旧的 PhotoOutput
    if (photoOutput_ && photoOutputAdded_) {
        err = OH_CaptureSession_RemovePhotoOutput(captureSession_, photoOutput_);
        if (err != CAMERA_OK) {
            OH_LOG_WARN(LOG_APP, "RemovePhotoOutput failed: %{public}d", err);
        }
        photoOutputAdded_ = false;
    }

    // 4. 释放旧的 PhotoOutput
    if (photoOutput_) {
        err = OH_PhotoOutput_Release(photoOutput_);
        if (err != CAMERA_OK) {
            OH_LOG_WARN(LOG_APP, "PhotoOutput release failed: %{public}d", err);
        }
        photoOutput_ = nullptr;
    }

    // 5. 选择新模式对应的 photoProfile
    int32_t profileIndex = selectPhotoProfileForMode(mode);

    // 6. 获取输出能力
    Camera_OutputCapability* outputCapability = nullptr;
    err = OH_CameraManager_GetSupportedCameraOutputCapabilityWithSceneMode(
        cameraManager_, &cameras_[currentCameraIndex_],
        Camera_SceneMode::NORMAL_PHOTO, &outputCapability);
    if (err != CAMERA_OK) {
        OH_LOG_WARN(LOG_APP, "GetCapabilityWithSceneMode failed: %{public}d, fallback", err);
        err = OH_CameraManager_GetSupportedCameraOutputCapability(
            cameraManager_, &cameras_[currentCameraIndex_], &outputCapability);
        if (err != CAMERA_OK) {
            OH_LOG_ERROR(LOG_APP, "GetCapability failed: %{public}d", err);
            OH_CaptureSession_CommitConfig(captureSession_);
            OH_CaptureSession_Start(captureSession_);
            return err;
        }
    }

    if (outputCapability->photoProfilesSize == 0) {
        OH_LOG_ERROR(LOG_APP, "No photo profiles");
        OH_CaptureSession_CommitConfig(captureSession_);
        OH_CaptureSession_Start(captureSession_);
        return Camera_ErrorCode::CAMERA_INVALID_ARGUMENT;
    }

    // 确保索引有效
    if (profileIndex < 0 || profileIndex >= static_cast<int32_t>(outputCapability->photoProfilesSize)) {
        profileIndex = 0;
    }

    Camera_Profile* photoProfile = outputCapability->photoProfiles[profileIndex];
    OH_LOG_INFO(LOG_APP, "Selected profile: %{public}ux%{public}u",
                photoProfile->size.width, photoProfile->size.height);

    // 7. 创建新的 PhotoOutput
    err = OH_CameraManager_CreatePhotoOutputWithoutSurface(cameraManager_, photoProfile, &photoOutput_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "CreatePhotoOutput failed: %{public}d", err);
        OH_CaptureSession_CommitConfig(captureSession_);
        OH_CaptureSession_Start(captureSession_);
        return err;
    }

    // 8. 注册回调
    err = OH_PhotoOutput_RegisterPhotoAvailableCallback(photoOutput_, ExpoCamera::onPhotoAvailable);
    if (err != CAMERA_OK) {
        OH_LOG_WARN(LOG_APP, "RegisterCallback failed: %{public}d", err);
    }

    // 9. 添加到 Session
    err = OH_CaptureSession_AddPhotoOutput(captureSession_, photoOutput_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "AddPhotoOutput failed: %{public}d", err);
        OH_PhotoOutput_Release(photoOutput_);
        photoOutput_ = nullptr;
        OH_CaptureSession_CommitConfig(captureSession_);
        OH_CaptureSession_Start(captureSession_);
        return err;
    }
    photoOutputAdded_ = true;

    // 10. 提交配置
    err = OH_CaptureSession_CommitConfig(captureSession_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "CommitConfig failed: %{public}d", err);
        return err;
    }

    // 11. 启动 Session
    err = OH_CaptureSession_Start(captureSession_);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Start session failed: %{public}d", err);
        return err;
    }

    currentMode_ = mode;
    OH_LOG_INFO(LOG_APP, "Capture mode switched to %{public}d", static_cast<int>(mode));

    return CAMERA_OK;
}

int32_t ExpoCamera::selectPhotoProfileForMode(CaptureMode mode) {
    Camera_OutputCapability* outputCapability = nullptr;
    Camera_ErrorCode err = OH_CameraManager_GetSupportedCameraOutputCapability(
        cameraManager_, &cameras_[currentCameraIndex_], &outputCapability);
    if (err != CAMERA_OK || outputCapability == nullptr) {
        OH_LOG_WARN(LOG_APP, "GetCapability failed, use default");
        return 0;
    }

    if (outputCapability->photoProfilesSize == 0) {
        return 0;
    }

    // 打印可用 profiles
    OH_LOG_INFO(LOG_APP, "Photo profiles (%{public}u):", outputCapability->photoProfilesSize);
    for (uint32_t i = 0; i < outputCapability->photoProfilesSize; i++) {
        Camera_Profile* p = outputCapability->photoProfiles[i];
        OH_LOG_INFO(LOG_APP, "  [%{public}u] %{public}ux%{public}u", i, p->size.width, p->size.height);
    }

    int32_t selectedIndex = 0;

    switch (mode) {
        case CaptureMode::SINGLE:
            // 单拍: 选最高分辨率
            {
                uint32_t maxPixels = 0;
                for (uint32_t i = 0; i < outputCapability->photoProfilesSize; i++) {
                    Camera_Profile* p = outputCapability->photoProfiles[i];
                    uint32_t pixels = p->size.width * p->size.height;
                    if (pixels > maxPixels) {
                        maxPixels = pixels;
                        selectedIndex = static_cast<int32_t>(i);
                    }
                }
            }
            break;

        case CaptureMode::BURST:
            // 连拍: 选接近 1080p 的分辨率（平衡性能和质量）
            {
                uint32_t targetPixels = 1920 * 1080;
                uint32_t minDiff = UINT32_MAX;
                for (uint32_t i = 0; i < outputCapability->photoProfilesSize; i++) {
                    Camera_Profile* p = outputCapability->photoProfiles[i];
                    uint32_t pixels = p->size.width * p->size.height;
                    // 只考虑 4K 以下的
                    if (pixels <= 3840 * 2160) {
                        uint32_t diff = (pixels > targetPixels) ? (pixels - targetPixels) : (targetPixels - pixels);
                        if (diff < minDiff) {
                            minDiff = diff;
                            selectedIndex = static_cast<int32_t>(i);
                        }
                    }
                }
                // 如果没找到，用第一个
                if (minDiff == UINT32_MAX && outputCapability->photoProfilesSize > 0) {
                    selectedIndex = 0;
                }
            }
            break;
    }

    OH_LOG_INFO(LOG_APP, "Selected profile index: %{public}d", selectedIndex);
    return selectedIndex;
}

int32_t ExpoCamera::selectCameraForMode(CaptureMode mode) {
    if (cameras_ == nullptr || cameraCount_ == 0) {
        OH_LOG_ERROR(LOG_APP, "No cameras available");
        return -1;
    }

    // TODO: 物理摄像头选择逻辑待实现
    // 当前使用逻辑摄像头选择（根据 Camera_Position）
    // 未来需要根据模式选择不同的物理摄像头:
    // - SINGLE: 主摄像头（高分辨率）
    // - BURST: 专用连拍摄像头（如果存在）

    OH_LOG_INFO(LOG_APP, "Selecting camera for mode: %{public}d", static_cast<int>(mode));

    // 打印所有相机信息
    for (uint32_t i = 0; i < cameraCount_; i++) {
        OH_LOG_INFO(LOG_APP, "  Camera[%{public}u]: position=%{public}d, type=%{public}d",
                    i, cameras_[i].cameraPosition, cameras_[i].cameraType);
    }

    // 目前暂时选择后置摄像头
    // 未来根据 mode 选择不同的物理摄像头
    for (uint32_t i = 0; i < cameraCount_; i++) {
        if (cameras_[i].cameraPosition == CAMERA_POSITION_BACK) {
            OH_LOG_INFO(LOG_APP, "Selected camera index: %{public}u (BACK)", i);
            return static_cast<int32_t>(i);
        }
    }

    // 如果没有后置摄像头，返回第一个
    if (cameraCount_ > 0) {
        OH_LOG_WARN(LOG_APP, "No BACK camera found, using first camera");
        return 0;
    }

    return -1;
}

//void ExpoCamera::notifyState(const std::string& state, const std::string& message) {
//    if (stateCallback_) {
//        stateCallback_(state, message);
//    }
//}
