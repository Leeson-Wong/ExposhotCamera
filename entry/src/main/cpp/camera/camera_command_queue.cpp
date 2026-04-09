#include "camera_command_queue.h"
#include "expo_camera.h"
#include "capture_manager.h"
#include "ohcamera/photo_output.h"
#include "hilog/log.h"
#include <chrono>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "CameraCmdQueue"

// 多线程调试日志标识符，使用 "CCQ_" 前缀便于过滤
#define CCQ_LOG_DEBUG(fmt, ...) OH_LOG_DEBUG(LOG_APP, "[CCQ_DEBUG] " fmt, ##__VA_ARGS__)
#define CCQ_LOG_INFO(fmt, ...)  OH_LOG_INFO(LOG_APP, "[CCQ_INFO] " fmt, ##__VA_ARGS__)
#define CCQ_LOG_WARN(fmt, ...)  OH_LOG_WARN(LOG_APP, "[CCQ_WARN] " fmt, ##__VA_ARGS__)
#define CCQ_LOG_ERROR(fmt, ...) OH_LOG_ERROR(LOG_APP, "[CCQ_ERROR] " fmt, ##__VA_ARGS__)

namespace exposhot {

CameraCommandQueue::CameraCommandQueue() {}

CameraCommandQueue::~CameraCommandQueue() {
    stop();
}

void CameraCommandQueue::start() {
    if (running_.load()) {
        CCQ_LOG_WARN("Already running");
        return;
    }

    running_.store(true);
    commandThread_ = std::thread(&CameraCommandQueue::commandLoop, this);
    CCQ_LOG_INFO("Command thread started");
}

void CameraCommandQueue::stop() {
    if (!running_.load()) {
        return;
    }

    // 投递 SHUTDOWN 命令
    post(CameraCommand::makeShutdown());

    if (commandThread_.joinable()) {
        commandThread_.join();
    }

    CCQ_LOG_INFO("Command thread stopped");
}

void CameraCommandQueue::post(CameraCommand&& cmd) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(cmd));
    }
    cv_.notify_one();
}

int32_t CameraCommandQueue::postSync(CameraCommand&& cmd) {
    // 使用 promise/future 实现同步等待
    std::promise<int32_t> promise;
    std::future<int32_t> future = promise.get_future();

    cmd.onComplete = [&promise](int32_t errorCode) {
        promise.set_value(errorCode);
    };

    post(std::move(cmd));

    // 等待命令处理完成
    return future.get();
}

void CameraCommandQueue::commandLoop() {
    CCQ_LOG_INFO("Command loop started");

    while (running_.load()) {
        CameraCommand cmd;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !queue_.empty() || !running_.load();
            });

            if (!running_.load() && queue_.empty()) {
                break;
            }

            if (!queue_.empty()) {
                cmd = std::move(queue_.front());
                queue_.pop();
            } else {
                continue;
            }
        }

        // 处理 SHUTDOWN
        if (cmd.type == CameraCommand::SHUTDOWN) {
            CCQ_LOG_INFO("SHUTDOWN command received");
            running_.store(false);
            if (cmd.onComplete) {
                cmd.onComplete(0);
            }
            break;
        }

        processCommand(cmd);
    }

    CCQ_LOG_INFO("Command loop ended");
}

void CameraCommandQueue::processCommand(CameraCommand& cmd) {
    const char* typeName = "UNKNOWN";
    switch (cmd.type) {
        case CameraCommand::INIT: typeName = "INIT"; break;
        case CameraCommand::RELEASE: typeName = "RELEASE"; break;
        case CameraCommand::START_PREVIEW: typeName = "START_PREVIEW"; break;
        case CameraCommand::STOP_PREVIEW: typeName = "STOP_PREVIEW"; break;
        case CameraCommand::SWITCH_SURFACE: typeName = "SWITCH_SURFACE"; break;
        case CameraCommand::CAPTURE_SINGLE: typeName = "CAPTURE_SINGLE"; break;
        case CameraCommand::CAPTURE_BURST_FRAME: typeName = "CAPTURE_BURST_FRAME"; break;
        case CameraCommand::SWITCH_CAPTURE_MODE: typeName = "SWITCH_CAPTURE_MODE"; break;
        case CameraCommand::SET_ZOOM_RATIO: typeName = "SET_ZOOM_RATIO"; break;
        case CameraCommand::SET_FOCUS_MODE: typeName = "SET_FOCUS_MODE"; break;
        case CameraCommand::SET_FOCUS_POINT: typeName = "SET_FOCUS_POINT"; break;
        case CameraCommand::SET_FOCUS_DISTANCE: typeName = "SET_FOCUS_DISTANCE"; break;
        case CameraCommand::PHOTO_AVAILABLE: typeName = "PHOTO_AVAILABLE"; break;
        default: break;
    }

    CCQ_LOG_INFO("Processing command: %{public}s", typeName);

    int32_t result = 0;

    switch (cmd.type) {
        case CameraCommand::INIT: {
            Camera_ErrorCode err = ExpoCamera::getInstance().init(cmd.captureMode);
            result = static_cast<int32_t>(err);
            CCQ_LOG_INFO("INIT result: %{public}d", result);
            break;
        }

        case CameraCommand::RELEASE: {
            Camera_ErrorCode err = ExpoCamera::getInstance().release();
            result = static_cast<int32_t>(err);
            CCQ_LOG_INFO("RELEASE result: %{public}d", result);
            break;
        }

        case CameraCommand::START_PREVIEW: {
            Camera_ErrorCode err = ExpoCamera::getInstance().startPreview(cmd.stringParam);
            result = static_cast<int32_t>(err);
            CCQ_LOG_INFO("START_PREVIEW result: %{public}d, surfaceId=%{public}s",
                         result, cmd.stringParam.c_str());
            break;
        }

        case CameraCommand::STOP_PREVIEW: {
            Camera_ErrorCode err = ExpoCamera::getInstance().stopPreview();
            result = static_cast<int32_t>(err);
            CCQ_LOG_INFO("STOP_PREVIEW result: %{public}d", result);
            break;
        }

        case CameraCommand::SWITCH_SURFACE: {
            Camera_ErrorCode err = ExpoCamera::getInstance().switchSurface(cmd.stringParam);
            result = static_cast<int32_t>(err);
            CCQ_LOG_INFO("SWITCH_SURFACE result: %{public}d, surfaceId=%{public}s",
                         result, cmd.stringParam.c_str());
            break;
        }

        case CameraCommand::CAPTURE_SINGLE: {
            // 直接调用 ExpoCamera 的 PhotoOutput 进行拍照
            Camera_PhotoOutput* photoOutput = ExpoCamera::getInstance().getPhotoOutput();
            if (!photoOutput) {
                result = -ENODEV;
                CCQ_LOG_ERROR("CAPTURE_SINGLE: PhotoOutput is null");
            } else {
                Camera_ErrorCode err = OH_PhotoOutput_Capture(photoOutput);
                result = static_cast<int32_t>(err);
                CCQ_LOG_INFO("CAPTURE_SINGLE result: %{public}d", result);
            }
            break;
        }

        case CameraCommand::CAPTURE_BURST_FRAME: {
            // 连拍单帧触发
            Camera_PhotoOutput* photoOutput = ExpoCamera::getInstance().getPhotoOutput();
            if (!photoOutput) {
                result = -ENODEV;
                CCQ_LOG_ERROR("CAPTURE_BURST_FRAME: PhotoOutput is null");
            } else {
                Camera_ErrorCode err = OH_PhotoOutput_Capture(photoOutput);
                result = static_cast<int32_t>(err);
                CCQ_LOG_INFO("CAPTURE_BURST_FRAME result: %{public}d", result);
            }
            break;
        }

        case CameraCommand::SWITCH_CAPTURE_MODE: {
            Camera_ErrorCode err = ExpoCamera::getInstance().switchCaptureMode(cmd.captureMode);
            result = static_cast<int32_t>(err);
            CCQ_LOG_INFO("SWITCH_CAPTURE_MODE result: %{public}d", result);
            break;
        }

        case CameraCommand::SET_ZOOM_RATIO: {
            Camera_ErrorCode err = ExpoCamera::getInstance().setZoomRatio(cmd.floatParam1);
            result = static_cast<int32_t>(err);
            break;
        }

        case CameraCommand::SET_FOCUS_MODE: {
            Camera_ErrorCode err = ExpoCamera::getInstance().setFocusMode(
                static_cast<Camera_FocusMode>(cmd.intParam));
            result = static_cast<int32_t>(err);
            break;
        }

        case CameraCommand::SET_FOCUS_POINT: {
            Camera_ErrorCode err = ExpoCamera::getInstance().setFocusPoint(
                cmd.floatParam1, cmd.floatParam2);
            result = static_cast<int32_t>(err);
            break;
        }

        case CameraCommand::SET_FOCUS_DISTANCE: {
            Camera_ErrorCode err = ExpoCamera::getInstance().setFocusDistance(cmd.floatParam1);
            result = static_cast<int32_t>(err);
            break;
        }

        case CameraCommand::PHOTO_AVAILABLE: {
            // 照片数据到达 CameraCommand 线程，转发给 CaptureManager 处理
            // 注意：buffer 所有权已经转移到本命令，由 CaptureManager 的回调处理释放
            CCQ_LOG_INFO("PHOTO_AVAILABLE: size=%{public}zu, %{public}ux%{public}u",
                         cmd.photoBufferSize, cmd.photoWidth, cmd.photoHeight);

            // 直接调用 CaptureManager 的分发回调（已在同一线程）
            ExpoCamera& cam = ExpoCamera::getInstance();
            // 使用 CaptureManager 的事件分发，通过已注册的 photoCapturedCallback_
            // 该回调在 CaptureManager::init 中设置，会根据模式分发到 onSinglePhotoCaptured 或 onBurstPhotoCaptured
            if (cam.photoCapturedCallback_) {
                cam.photoCapturedCallback_(cmd.photoBuffer, cmd.photoBufferSize,
                                           cmd.photoWidth, cmd.photoHeight);
            } else {
                // 没有回调注册，释放 buffer
                if (cmd.photoBuffer) {
                    free(cmd.photoBuffer);
                    cmd.photoBuffer = nullptr;
                }
            }
            result = 0;
            break;
        }

        default:
            CCQ_LOG_ERROR("Unknown command type: %{public}d", static_cast<int>(cmd.type));
            result = -1;
            break;
    }

    // 调用完成回调
    if (cmd.onComplete) {
        cmd.onComplete(result);
    }

    CCQ_LOG_INFO("Command %{public}s done, result=%{public}d", typeName, result);
}

} // namespace exposhot
