#ifndef BURST_CAPTURE_H
#define BURST_CAPTURE_H

#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include "task_queue.h"
#include "image_processor.h"

namespace exposhot {

// 连拍状态
enum class BurstState {
    IDLE = 0,           // 空闲
    CAPTURING = 1,      // 正在拍摄
    PROCESSING = 2,     // 正在处理(拍摄已完成)
    COMPLETED = 3,      // 完成
    ERROR = 4,          // 错误
    CANCELLED = 5,      // 已取消
};

// 连拍配置
struct BurstConfig {
    int32_t frameCount = 5;         // 拍摄帧数
    int32_t exposureMs = 10000;     // 每帧曝光时间(毫秒)
    bool realtimePreview = true;    // 是否实时返回累积结果
    std::string sessionId;          // 会话 ID
};

// 连拍进度信息
struct BurstProgress {
    std::string sessionId;          // 会话 ID
    BurstState state = BurstState::IDLE;
    int32_t capturedFrames = 0;     // 已拍摄帧数
    int32_t processedFrames = 0;    // 已处理帧数
    int32_t totalFrames = 0;        // 总帧数
    std::string message;
};

// 进度回调(回调到 UI 线程)
using BurstProgressCallback = std::function<void(const BurstProgress& progress)>;

// 图像数据回调(回调到 UI 线程)
// sessionId: 会话 ID
// buffer: JPEG 图像数据(调用者负责 free)
// size: 数据大小
// isFinal: 是否是最终结果
using BurstImageCallback = std::function<void(const std::string& sessionId, void* buffer, size_t size, bool isFinal)>;

// 连拍管理器(单例)
class BurstCapture {
public:
    static BurstCapture& getInstance();

    // 初始化/释放
    bool init();
    void release();

    // 设置回调
    void setProgressCallback(BurstProgressCallback callback);
    void setImageCallback(BurstImageCallback callback);

    // 开始连拍
    // config: 连拍配置
    // 返回: sessionId，空字符串表示启动失败
    std::string startBurst(const BurstConfig& config);

    // 取消连拍
    void cancelBurst();

    // 获取当前状态
    BurstState getState() const { return state_.load(); }
    const BurstProgress& getProgress() const { return progress_; }

    // 拍照完成回调(由 ExpoCamera 调用)
    // buffer: 原始图像数据（格式取决于相机输出）
    // size: 数据大小
    // width, height: 图像尺寸
    void onPhotoCaptured(void* buffer, size_t size, int32_t width, int32_t height);

    // 检查是否正在进行连拍
    bool isBurstActive() const;

    // 设置图像尺寸(从第一帧获取)
    void setImageSize(int32_t width, int32_t height) {
        imageWidth_ = width;
        imageHeight_ = height;
    }

private:
    BurstCapture();
    ~BurstCapture();
    BurstCapture(const BurstCapture&) = delete;
    BurstCapture& operator=(const BurstCapture&) = delete;

    // 内部方法
    void notifyProgress();
    void notifyImage(void* buffer, size_t size, bool isFinal);
    void processTask(ImageTask&& task);
    void startCaptureLoop();
    void captureNextFrame();

    // 状态
    std::atomic<BurstState> state_{BurstState::IDLE};
    BurstProgress progress_;
    BurstConfig config_;

    // 帧计数
    std::atomic<int32_t> captureCount_{0};
    std::atomic<int32_t> processCount_{0};

    // 图像尺寸
    int32_t imageWidth_ = 4032;
    int32_t imageHeight_ = 3024;

    // 任务队列
    std::unique_ptr<TaskQueue> taskQueue_;

    // 图像处理器
    std::unique_ptr<ImageProcessor> processor_;

    // 回调
    BurstProgressCallback progressCallback_;
    BurstImageCallback imageCallback_;

    // 线程锁
    mutable std::mutex mutex_;
};

} // namespace exposhot

#endif // BURST_CAPTURE_H
