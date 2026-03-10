#ifndef EXPO_CAMERA_H
#define EXPO_CAMERA_H

#include <string>
#include <mutex>
#include <functional>
#include <vector>
#include <algorithm>
#include "ohcamera/camera.h"
#include "render_slot.h"
#include "ohcamera/camera_input.h"
#include "ohcamera/capture_session.h"
#include "ohcamera/preview_output.h"
#include "ohcamera/photo_output.h"
#include "ohcamera/camera_manager.h"
#include <multimedia/image_framework/image/image_native.h>

// 预览流变化观察者回调
// activeSlotId: 当前获得预览流的 slot ID
// activeSurfaceId: 当前获得预览流的 surface ID
using PreviewObserverCallback = std::function<void(const std::string& activeSlotId, const std::string& activeSurfaceId)>;

// 观察者信息
struct PreviewObserver {
    std::string slotId;                    // slot 唯一标识
    std::string surfaceId;                 // Surface ID
    PreviewObserverCallback callback;      // 观察者回调
    void* userData = nullptr;              // 用户数据
};

class ExpoCamera {
public:
    static ExpoCamera& getInstance();

    // 生命周期
    Camera_ErrorCode init();
    Camera_ErrorCode release();

    // Surface 管理
    Camera_ErrorCode switchSurface(const std::string& surfaceId);
    Camera_ErrorCode startPreview(const std::string& surfaceId);
    Camera_ErrorCode stopPreview();

    // 拍照
    using PhotoCallback = std::function<void(void* buffer, size_t size)>;
    Camera_ErrorCode takePhoto();  // 只触发拍照动作，不接收参数
    void setPhotoCallback(const PhotoCallback& callback) { photoCallback_ = callback; }

    // 相机参数
    Camera_ErrorCode setZoomRatio(float ratio);
    Camera_ErrorCode getZoomRatio(float* ratio);
    Camera_ErrorCode getZoomRatioRange(float* min, float* max);
    Camera_ErrorCode isZoomSupported(bool* supported);
    Camera_ErrorCode setFocusMode(Camera_FocusMode mode);
    Camera_ErrorCode getFocusMode(Camera_FocusMode* mode);
    Camera_ErrorCode isFocusModeSupported(Camera_FocusMode mode, bool* supported);
    Camera_ErrorCode setFocusDistance(float distance);
    Camera_ErrorCode getFocusDistance(float* distance);
    Camera_ErrorCode getFocusDistanceRange(float* min, float* max);
    Camera_ErrorCode setFocusPoint(float x, float y);
    Camera_ErrorCode getFocusPoint(float* x, float* y);

    // 状态
    bool isInitialized() const { return initialized_; }
    bool isPreviewing() const { return previewing_; }
    bool isPhotoOutputReady() const { return photoOutput_ != nullptr; }

    // 观察者管理（新接口）
    // 注册观察者，返回 slotId
    std::string registerObserver(const std::string& surfaceId, PreviewObserverCallback callback, void* userData = nullptr);

    // 注销观察者
    Camera_ErrorCode unregisterObserver(const std::string& slotId);

    // 切换预览流到指定 slot
    Camera_ErrorCode switchToSlot(const std::string& slotId);

    // 获取当前活跃的 slotId
    std::string getActiveSlotId() const { return activeSlotId_; }

    // 状态订阅（上层应用）
    using StateCallback = std::function<void(const std::string& state, const std::string& message)>;
    void subscribeState(const StateCallback& callback);
    void unsubscribeState();

private:
    ExpoCamera();
    ~ExpoCamera();
    ExpoCamera(const ExpoCamera&) = delete;
    ExpoCamera& operator=(const ExpoCamera&) = delete;

    Camera_ErrorCode createCameraInput();
    Camera_ErrorCode createCaptureSession();
    Camera_ErrorCode createPreviewOutput(const std::string& surfaceId);
    Camera_ErrorCode createPhotoOutput();

    // 不加锁的内部版本（供内部调用）
    Camera_ErrorCode startPreviewInternal(const std::string& surfaceId);
    Camera_ErrorCode switchSurfaceInternal(const std::string& surfaceId);

    // 通知所有观察者
    void notifyAllObservers();

    // 拍照回调（静态方法，用于 C API 回调）
    static void onFrameStart(Camera_PhotoOutput* photoOutput);
    static void onFrameShutter(Camera_PhotoOutput* photoOutput, Camera_FrameShutterInfo* info);
    static void onFrameEnd(Camera_PhotoOutput* photoOutput, int32_t frameCount);
    static void onError(Camera_PhotoOutput* photoOutput, Camera_ErrorCode errorCode);

    // 照片可用回调（用于获取拍照数据）
    static void onPhotoAvailable(Camera_PhotoOutput* photoOutput, OH_PhotoNative* photo);

    // 相机资源
    Camera_Manager* cameraManager_ = nullptr;
    Camera_Input* cameraInput_ = nullptr;
    Camera_CaptureSession* captureSession_ = nullptr;
    Camera_PreviewOutput* previewOutput_ = nullptr;
    Camera_PhotoOutput* photoOutput_ = nullptr;

    // 相机列表
    Camera_Device* cameras_ = nullptr;
    uint32_t cameraCount_ = 0;
    uint32_t currentCameraIndex_ = 0;

    // 状态
    std::string activeSurfaceId_;
    std::string activeSlotId_;  // 当前活跃的 slot ID
    bool initialized_ = false;
    bool previewing_ = false;
    bool photoOutputAdded_ = false;
    std::mutex mutex_;

    // 拍照回调
    PhotoCallback photoCallback_;

    // 观察者列表
    std::vector<PreviewObserver> observers_;

    // 状态订阅
    StateCallback stateCallback_;
};

#endif // EXPO_CAMERA_H
