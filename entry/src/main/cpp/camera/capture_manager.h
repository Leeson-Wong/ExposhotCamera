#ifndef CAPTURE_MANAGER_H
#define CAPTURE_MANAGER_H

#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include "task_queue.h"
#include "image_processor.h"
#include "expo_camera.h"  // 使用全局 CaptureMode
#include "ohcamera/photo_output.h"

namespace exposhot {

// 拍摄状态
enum class CaptureState {
    IDLE = 0,              // 空闲
    SINGLE_CAPTURING = 1,  // 单次拍照中
    BURST_CAPTURING = 2,   // 连拍拍摄中
    PROCESSING = 3,        // 连拍处理中
    COMPLETED = 4,         // 连拍完成
    ERROR = 5,             // 错误
    CANCELLED = 6          // 已取消
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
    CaptureState state = CaptureState::IDLE;
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

// 单次拍照回调
// sessionId: 会话 ID
// buffer: 原始图像数据(调用者负责 free)
// size: 数据大小
// width, height: 图像尺寸
using SinglePhotoCallback = std::function<void(const std::string& sessionId, void* buffer, size_t size, uint32_t width, uint32_t height)>;

// 拍照失败回调（异步通知相机硬件错误）
// 仅用于拍照过程中的硬件错误，触发失败直接通过返回值返回错误码
// sessionId: 会话 ID
// errorCode: HarmonyOS 相机错误码
using PhotoErrorCallback = std::function<void(const std::string& sessionId, int32_t errorCode)>;

// ==================== 事件系统 ====================

// 拍照事件类型
enum class PhotoEventType {
    CAPTURE_START = 0,      // 拍照命令已发送
    CAPTURE_END = 1,        // 拍照成功，原始数据已获取
    CAPTURE_FAILED = 2,     // 拍照失败
};

// 处理事件类型
enum class ProcessEventType {
    PROCESS_START = 0,      // 开始处理
    PROCESS_PROGRESS = 1,   // 处理进度更新
    PROCESS_END = 2,        // 处理完成
    PROCESS_FAILED = 3,     // 处理失败
};

// 拍照事件数据
struct PhotoEvent {
    PhotoEventType type;
    std::string sessionId;
    int32_t frameIndex = -1;     // 连拍帧索引 (0-based)，单拍为 -1
    std::string message;
};

// 处理事件数据
struct ProcessEvent {
    ProcessEventType type;
    std::string sessionId;
    int32_t progress = 0;        // 进度百分比 (0-100)
    int32_t currentFrame = 0;    // 当前帧
    int32_t totalFrames = 0;     // 总帧数
    std::string message;
};

// 拍照事件回调
using PhotoEventCallback = std::function<void(const PhotoEvent& event)>;

// 处理事件回调
using ProcessEventCallback = std::function<void(const ProcessEvent& event)>;

// 拍摄管理器(单例)
class CaptureManager {
public:
    static CaptureManager& getInstance();

    // 初始化/释放
    // mode: 初始化时指定拍摄模式，传递给 ExpoCamera::init
    bool init(CaptureMode mode);
    void release();

    // 设置回调
    void setProgressCallback(BurstProgressCallback callback);
    void setImageCallback(BurstImageCallback callback);
    void setSinglePhotoCallback(SinglePhotoCallback callback);
    void setPhotoErrorCallback(PhotoErrorCallback callback);
    void setPhotoEventCallback(PhotoEventCallback callback);
    void setProcessEventCallback(ProcessEventCallback callback);

    // ==================== 单次拍照 ====================

    // 单次拍照
    // 返回: 0 成功，负数表示错误码
    // outSessionId: 输出参数，成功时返回 sessionId
    int32_t captureSingle(std::string& outSessionId);

    // 单次拍照数据回调（由 ExpoCamera 调用）
    void onSinglePhotoCaptured(void* buffer, size_t size, uint32_t width, uint32_t height);

    // 拍照错误通知（由 ExpoCamera 调用，异步通知相机硬件错误）
    void onPhotoError(int32_t errorCode);

    // ==================== 连拍 ====================

    // 开始连拍
    // config: 连拍配置
    // 返回: 0 成功，负数表示错误码
    // outSessionId: 输出参数，成功时返回 sessionId
    int32_t startBurst(const BurstConfig& config, std::string& outSessionId);

    // 取消连拍
    void cancelBurst();

    // 连拍拍照完成回调(由 ExpoCamera 调用)
    // buffer: 原始图像数据（格式取决于相机输出）
    // size: 数据大小
    // width, height: 图像尺寸
    void onBurstPhotoCaptured(void* buffer, size_t size, uint32_t width, uint32_t height);

    // ==================== 状态查询 ====================

    // 获取当前状态
    CaptureState getState() const { return state_.load(); }
    const BurstProgress& getProgress() const { return progress_; }

    // 检查是否正在进行拍摄（单拍或连拍）
    bool isCaptureActive() const;

    // 兼容旧接口：检查是否正在进行连拍
    bool isBurstActive() const;

    // 设置图像尺寸(从第一帧获取)
    void setImageSize(uint32_t width, uint32_t height) {
        imageWidth_ = width;
        imageHeight_ = height;
    }

    // 获取 PhotoOutput（用于触发拍照）
    Camera_PhotoOutput* getPhotoOutput() const;

    // 切换拍摄模式（统一入口，同步更新 currentMode_ 和 ExpoCamera）
    int32_t switchCaptureMode(CaptureMode mode);

    // 获取当前拍摄模式
    CaptureMode getCaptureMode() const { return currentMode_; }

    // 检查是否可以切换模式
    bool canSwitchMode() const;

private:
    CaptureManager();
    ~CaptureManager();
    CaptureManager(const CaptureManager&) = delete;
    CaptureManager& operator=(const CaptureManager&) = delete;

    // 内部方法
    void notifyProgress();      // 不加锁版本，调用者需确保已持有锁或 progress_ 已安全
    void notifyProgressSafe();  // 线程安全版本，内部加锁
    void notifyImage(void* buffer, size_t size, bool isFinal);
    void processTask(ImageTask&& task);
    void startCaptureLoop();
    void captureNextFrame();

    // 状态
    std::atomic<CaptureState> state_{CaptureState::IDLE};
    BurstProgress progress_;
    BurstConfig config_;

    // 当前拍摄会话
    std::string currentSessionId_;
    CaptureMode currentMode_{CaptureMode::SINGLE};

    // 帧计数
    std::atomic<int32_t> captureCount_{0};
    std::atomic<int32_t> processCount_{0};

    // 图像尺寸
    uint32_t imageWidth_ = 4032;
    uint32_t imageHeight_ = 3024;

    // 任务队列
    std::unique_ptr<TaskQueue> taskQueue_;

    // 图像处理器
    std::unique_ptr<ImageProcessor> processor_;

    // 回调
    BurstProgressCallback progressCallback_;
    BurstImageCallback imageCallback_;
    SinglePhotoCallback singlePhotoCallback_;
    PhotoErrorCallback photoErrorCallback_;
    PhotoEventCallback photoEventCallback_;
    ProcessEventCallback processEventCallback_;

    // 线程锁
    mutable std::mutex mutex_;
};

} // namespace exposhot

#endif // CAPTURE_MANAGER_H
