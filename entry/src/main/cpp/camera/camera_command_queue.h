#ifndef CAMERA_COMMAND_QUEUE_H
#define CAMERA_COMMAND_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <string>
#include <cstdint>
#include <future>

// 前向声明，避免头文件循环依赖
enum class CaptureMode : int;

namespace exposhot {

// 相机命令类型
struct CameraCommand {
    enum Type {
        INIT,               // 初始化相机
        RELEASE,            // 释放相机
        START_PREVIEW,      // 开始预览
        STOP_PREVIEW,       // 停止预览
        SWITCH_SURFACE,     // 切换 Surface
        CAPTURE_SINGLE,     // 单次拍照
        CAPTURE_BURST_FRAME, // 连拍单帧
        SWITCH_CAPTURE_MODE,// 切换拍摄模式
        SET_ZOOM_RATIO,     // 设置缩放比例
        SET_FOCUS_MODE,     // 设置对焦模式
        SET_FOCUS_POINT,    // 设置对焦点
        SET_FOCUS_DISTANCE, // 设置对焦距离
        PHOTO_AVAILABLE,    // 照片数据可用（从相机回调线程投递）
        SHUTDOWN            // 关闭命令队列
    };

    Type type;

    // 通用参数
    std::string stringParam;       // surfaceId, sessionId 等
    int32_t intParam = 0;          // mode, focusMode 等整型参数
    float floatParam1 = 0.0f;      // zoomRatio, x, distance 等
    float floatParam2 = 0.0f;      // y
    CaptureMode captureMode = static_cast<CaptureMode>(0);

    // PHOTO_AVAILABLE 专用：照片 buffer（所有权转移）
    void* photoBuffer = nullptr;
    size_t photoBufferSize = 0;
    uint32_t photoWidth = 0;
    uint32_t photoHeight = 0;

    // 完成回调（在 CameraCommand 线程上执行）
    using CompletionHandler = std::function<void(int32_t errorCode)>;
    CompletionHandler onComplete;

    // 构造便捷方法
    static CameraCommand makeInit(CaptureMode mode, CompletionHandler onDone = nullptr) {
        CameraCommand cmd;
        cmd.type = INIT;
        cmd.captureMode = mode;
        cmd.onComplete = onDone;
        return cmd;
    }

    static CameraCommand makeRelease(CompletionHandler onDone = nullptr) {
        CameraCommand cmd;
        cmd.type = RELEASE;
        cmd.onComplete = onDone;
        return cmd;
    }

    static CameraCommand makeStartPreview(const std::string& surfaceId,
                                          CompletionHandler onDone = nullptr) {
        CameraCommand cmd;
        cmd.type = START_PREVIEW;
        cmd.stringParam = surfaceId;
        cmd.onComplete = onDone;
        return cmd;
    }

    static CameraCommand makeStopPreview(CompletionHandler onDone = nullptr) {
        CameraCommand cmd;
        cmd.type = STOP_PREVIEW;
        cmd.onComplete = onDone;
        return cmd;
    }

    static CameraCommand makeSwitchSurface(const std::string& surfaceId,
                                           CompletionHandler onDone = nullptr) {
        CameraCommand cmd;
        cmd.type = SWITCH_SURFACE;
        cmd.stringParam = surfaceId;
        cmd.onComplete = onDone;
        return cmd;
    }

    static CameraCommand makeCaptureSingle(CompletionHandler onDone = nullptr) {
        CameraCommand cmd;
        cmd.type = CAPTURE_SINGLE;
        cmd.onComplete = onDone;
        return cmd;
    }

    static CameraCommand makeCaptureBurstFrame() {
        CameraCommand cmd;
        cmd.type = CAPTURE_BURST_FRAME;
        return cmd;
    }

    static CameraCommand makeSwitchCaptureMode(CaptureMode mode,
                                               CompletionHandler onDone = nullptr) {
        CameraCommand cmd;
        cmd.type = SWITCH_CAPTURE_MODE;
        cmd.captureMode = mode;
        cmd.onComplete = onDone;
        return cmd;
    }

    static CameraCommand makeSetZoomRatio(float ratio, CompletionHandler onDone = nullptr) {
        CameraCommand cmd;
        cmd.type = SET_ZOOM_RATIO;
        cmd.floatParam1 = ratio;
        cmd.onComplete = onDone;
        return cmd;
    }

    static CameraCommand makeSetFocusMode(int32_t mode, CompletionHandler onDone = nullptr) {
        CameraCommand cmd;
        cmd.type = SET_FOCUS_MODE;
        cmd.intParam = mode;
        cmd.onComplete = onDone;
        return cmd;
    }

    static CameraCommand makeSetFocusPoint(float x, float y,
                                           CompletionHandler onDone = nullptr) {
        CameraCommand cmd;
        cmd.type = SET_FOCUS_POINT;
        cmd.floatParam1 = x;
        cmd.floatParam2 = y;
        cmd.onComplete = onDone;
        return cmd;
    }

    static CameraCommand makeSetFocusDistance(float distance,
                                              CompletionHandler onDone = nullptr) {
        CameraCommand cmd;
        cmd.type = SET_FOCUS_DISTANCE;
        cmd.floatParam1 = distance;
        cmd.onComplete = onDone;
        return cmd;
    }

    static CameraCommand makePhotoAvailable(void* buffer, size_t size,
                                             uint32_t width, uint32_t height) {
        CameraCommand cmd;
        cmd.type = PHOTO_AVAILABLE;
        cmd.photoBuffer = buffer;
        cmd.photoBufferSize = size;
        cmd.photoWidth = width;
        cmd.photoHeight = height;
        return cmd;
    }

    static CameraCommand makeShutdown() {
        CameraCommand cmd;
        cmd.type = SHUTDOWN;
        return cmd;
    }
};

// MPSC（多生产者单消费者）命令队列
// 所有 OH_Camera_* API 调用在此线程串行执行
class CameraCommandQueue {
public:
    CameraCommandQueue();
    ~CameraCommandQueue();

    // 启动命令处理线程
    void start();

    // 停止命令处理线程（投递 SHUTDOWN 并等待线程结束）
    void stop();

    // 投递命令（线程安全，可从任何线程调用）
    void post(CameraCommand&& cmd);

    // 投递命令并同步等待结果（线程安全）
    // 注意：不能在命令处理线程内部调用，会死锁
    int32_t postSync(CameraCommand&& cmd);

    // 检查是否正在运行
    bool isRunning() const { return running_.load(); }

private:
    void commandLoop();
    void processCommand(CameraCommand& cmd);

    std::queue<CameraCommand> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread commandThread_;
    std::atomic<bool> running_{false};
};

} // namespace exposhot

#endif // CAMERA_COMMAND_QUEUE_H
