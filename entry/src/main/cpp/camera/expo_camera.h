#ifndef EXPO_CAMERA_H
#define EXPO_CAMERA_H

#include <rawfile/raw_file_manager.h>
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

// ==================== 拍摄模式定义 ====================

enum class CaptureMode {
    SINGLE = 0,  // 单拍模式
    BURST = 1    // 连拍模式
};

// ==================== 回调类型定义 ====================

// 预览流变化观察者回调
// activeSlotId: 当前获得预览流的 slot ID
// activeSurfaceId: 当前获得预览流的 surface ID
using PreviewObserverCallback = std::function<void(const std::string& activeSlotId, const std::string& activeSurfaceId)>;
using BindPreviewObserverCallback = std::function<PreviewObserverCallback(const std::string& slotId)>;

// 照片捕获回调（供上层如 CaptureManager 注册）
// buffer: 图像数据（调用者负责 free）
// size: 数据大小
// width, height: 图像尺寸
using PhotoCapturedCallback = std::function<void(void* buffer, size_t size, uint32_t width, uint32_t height)>;

// 照片错误回调（供上层如 CaptureManager 注册）
// errorCode: HarmonyOS 相机错误码
using PhotoErrorCallback = std::function<void(int32_t errorCode)>;

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
    // mode: 初始化时指定拍摄模式，不同模式可能选择不同的摄像头
    Camera_ErrorCode init(CaptureMode mode = CaptureMode::SINGLE);
    Camera_ErrorCode release();

    // Surface 管理
    Camera_ErrorCode switchSurface(const std::string& surfaceId);
    Camera_ErrorCode startPreview(const std::string& surfaceId);
    Camera_ErrorCode stopPreview();

    // 拍照 - 委托给 CaptureManager
    // 注意：此方法已移至 CaptureManager，此处保留仅为兼容旧代码
    // 实际拍照逻辑由 CaptureManager::captureSingle() 处理

    // 获取 PhotoOutput（供 CaptureManager 使用）
    Camera_PhotoOutput* getPhotoOutput() const { return photoOutput_; }

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

    // ==================== 拍摄模式管理 ====================

    // 切换拍摄模式（会重新配置 PhotoOutput）
    // 必须在预览已启动且没有拍摄进行中时调用
    Camera_ErrorCode switchCaptureMode(CaptureMode mode);

    // 获取当前拍摄模式
    CaptureMode getCaptureMode() const { return currentMode_; }

    // 检查是否可以切换模式
    bool canSwitchMode() const;

    // 观察者管理（新接口）
    // 注册观察者，返回 slotId
    std::string registerObserver(const std::string& surfaceId, BindPreviewObserverCallback bindCallback, void* userData = nullptr);

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

    // 照片回调注册（供 CaptureManager 等上层使用）
    void setPhotoCapturedCallback(PhotoCapturedCallback callback);
    void setPhotoErrorCallback(PhotoErrorCallback callback);

private:
    ExpoCamera();
    ~ExpoCamera();
    ExpoCamera(const ExpoCamera&) = delete;
    ExpoCamera& operator=(const ExpoCamera&) = delete;

    Camera_ErrorCode createCameraInput();
    Camera_ErrorCode createCaptureSession();
    Camera_ErrorCode createPreviewOutput(const std::string& surfaceId);
    Camera_ErrorCode createPhotoOutput();

    // 模式切换相关
    int32_t selectPhotoProfileForMode(CaptureMode mode);
    int32_t selectCameraForMode(CaptureMode mode);

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

    // 拍摄模式
    CaptureMode currentMode_ = CaptureMode::SINGLE;

    // 观察者列表
    std::vector<PreviewObserver> observers_;

    // 状态订阅
    StateCallback stateCallback_;

    // 照片回调（由 CaptureManager 等上层注册）
    PhotoCapturedCallback photoCapturedCallback_;
    PhotoErrorCallback photoErrorCallback_;

    // 回调有效性标记（原子操作，避免竞态）
    std::atomic<bool> photoCallbackValid_{false};
    std::atomic<bool> photoErrorCallbackValid_{false};
};

#endif // EXPO_CAMERA_H
