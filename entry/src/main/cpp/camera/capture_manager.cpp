#include "capture_manager.h"
#include "expo_camera.h"
#include "file_saver.h"
#include "ohcamera/photo_output.h"
#include "hilog/log.h"

#include <thread>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "CaptureManager"

// 多线程调试日志标识符，使用 "CM_" 前缀便于过滤
// 过滤命令: hilog | grep -E "CM_|TQ_"
#define CM_LOG_DEBUG(fmt, ...) OH_LOG_DEBUG(LOG_APP, "[CM_DEBUG] " fmt, ##__VA_ARGS__)
#define CM_LOG_INFO(fmt, ...)  OH_LOG_INFO(LOG_APP, "[CM_INFO] " fmt, ##__VA_ARGS__)
#define CM_LOG_WARN(fmt, ...)  OH_LOG_WARN(LOG_APP, "[CM_WARN] " fmt, ##__VA_ARGS__)
#define CM_LOG_ERROR(fmt, ...) OH_LOG_ERROR(LOG_APP, "[CM_ERROR] " fmt, ##__VA_ARGS__)

namespace exposhot {

// 生成唯一的 sessionId
static std::string generateSessionId() {
    // 使用时间戳 + 随机数生成唯一 ID
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 9999);

    std::ostringstream oss;
    oss << "session_" << ms << "_" << std::setw(4) << std::setfill('0') << dis(gen);
    return oss.str();
}

CaptureManager& CaptureManager::getInstance() {
    static CaptureManager instance;
    return instance;
}

CaptureManager::CaptureManager()
    : taskQueue_(std::make_unique<TaskQueue>())
    , processor_(std::make_unique<ImageProcessor>()) {
}

CaptureManager::~CaptureManager() {
    release();
}

bool CaptureManager::init(CaptureMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);

    CM_LOG_INFO("[INIT_BEGIN] mode=%{public}d, currentState=%{public}d, taskQueueRunning=%{public}d",
                static_cast<int>(mode), static_cast<int>(state_.load()), taskQueue_->isRunning());

    // 如果已经初始化且模式相同，直接返回成功
    if (state_.load() == CaptureState::IDLE && taskQueue_->isRunning()) {
        // 检查 ExpoCamera 是否也已初始化
        if (ExpoCamera::getInstance().isInitialized() && currentMode_ == mode) {
            CM_LOG_INFO("[INIT_SKIP] Already initialized with same mode");
            return true;
        }
    }

    // 强制释放之前的资源（处理页面切换时新页面 init 在旧页面 release 之前的情况）
    if (taskQueue_->isRunning()) {
        CM_LOG_INFO("[INIT_FORCE_STOP] Stopping previous task queue");
        taskQueue_->stop();
    }
    processor_->reset();

    // 清理 ExpoCamera 回调
    ExpoCamera::getInstance().setPhotoCapturedCallback(nullptr);
    ExpoCamera::getInstance().setPhotoErrorCallback(nullptr);

    // 强制释放 ExpoCamera
    ExpoCamera::getInstance().release();

    // 重置状态
    state_.store(CaptureState::IDLE);

    // 保存当前模式
    currentMode_ = mode;

    // 初始化下层 ExpoCamera（依赖方向：CaptureManager → ExpoCamera）
    // 传入模式，让 ExpoCamera 选择正确的摄像头
    Camera_ErrorCode err = ExpoCamera::getInstance().init(mode);
    if (err != CAMERA_OK) {
        CM_LOG_ERROR("[INIT_FAILED] ExpoCamera init failed: %{public}d", err);
        return false;
    }

    // 初始化 FileSaver
    FileSaver::getInstance().init();

    // 注册照片回调到 ExpoCamera
    ExpoCamera::getInstance().setPhotoCapturedCallback(
        [this](void* buffer, size_t size, uint32_t width, uint32_t height) {
            // 根据当前模式分发到对应处理方法
            if (currentMode_ == CaptureMode::BURST) {
                this->onBurstPhotoCaptured(buffer, size, width, height);
            } else {
                this->onSinglePhotoCaptured(buffer, size, width, height);
            }
        });

    ExpoCamera::getInstance().setPhotoErrorCallback(
        [this](int32_t errorCode) {
            this->onPhotoError(errorCode);
        });

    // 启动任务队列
    taskQueue_->start([this](ImageTask&& task) {
        processTask(std::move(task));
    });

    CM_LOG_INFO("[INIT_DONE] CaptureManager initialized successfully");
    return true;
}

void CaptureManager::release() {
    std::lock_guard<std::mutex> lock(mutex_);

    CM_LOG_INFO("[RELEASE_BEGIN] currentState=%{public}d, taskQueueRunning=%{public}d",
                static_cast<int>(state_.load()), taskQueue_->isRunning());

    cancelBurst();
    taskQueue_->stop();
    processor_->reset();

    // 清理注册到 ExpoCamera 的回调
    ExpoCamera::getInstance().setPhotoCapturedCallback(nullptr);
    ExpoCamera::getInstance().setPhotoErrorCallback(nullptr);

    // 释放下层 ExpoCamera
    ExpoCamera::getInstance().release();

    // 重置状态，允许再次初始化
    state_.store(CaptureState::IDLE);

    CM_LOG_INFO("[RELEASE_DONE] CaptureManager released, state reset to IDLE");
}

void CaptureManager::setProgressCallback(BurstProgressCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    progressCallback_ = callback;
}

void CaptureManager::setImageCallback(BurstImageCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    imageCallback_ = callback;
}

void CaptureManager::setSinglePhotoCallback(SinglePhotoCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    singlePhotoCallback_ = callback;
}

void CaptureManager::setPhotoErrorCallback(PhotoErrorCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    photoErrorCallback_ = callback;
}

void CaptureManager::setPhotoEventCallback(PhotoEventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    photoEventCallback_ = callback;
}

void CaptureManager::setProcessEventCallback(ProcessEventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    processEventCallback_ = callback;
}

// 内部辅助函数：通知拍照事件
static void notifyPhotoEvent(const PhotoEventCallback& callback, const PhotoEvent& event) {
    if (callback) {
        callback(event);
    }
}

// 内部辅助函数：通知处理事件
static void notifyProcessEvent(const ProcessEventCallback& callback, const ProcessEvent& event) {
    if (callback) {
        callback(event);
    }
}

// ==================== 单次拍照 ====================

int32_t CaptureManager::captureSingle(std::string& outSessionId) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 检查是否有拍摄进行中
    if (isCaptureActive()) {
        OH_LOG_WARN(LOG_APP, "Capture already in progress, state: %{public}d",
                    static_cast<int>(state_.load()));
        return -EBUSY;  // 系统标准错误码：设备忙
    }

    // 生成 sessionId
    std::string sessionId = generateSessionId();
    currentSessionId_ = sessionId;
    currentMode_ = CaptureMode::SINGLE;

    // 进入单拍状态
    state_.store(CaptureState::SINGLE_CAPTURING);

    // 通知拍照开始事件
    notifyPhotoEvent(photoEventCallback_, {PhotoEventType::CAPTURE_START, sessionId, -1, "Single capture started"});

    OH_LOG_INFO(LOG_APP, "Single capture started: sessionId=%{public}s", sessionId.c_str());

    // 获取 PhotoOutput 并触发拍照
    Camera_PhotoOutput* photoOutput = getPhotoOutput();
    if (!photoOutput) {
        OH_LOG_ERROR(LOG_APP, "PhotoOutput is null");
        state_.store(CaptureState::IDLE);
        // 通知拍照失败事件
        notifyPhotoEvent(photoEventCallback_, {PhotoEventType::CAPTURE_FAILED, sessionId, -1, "PhotoOutput is null"});
        return -ENODEV;  // 系统标准错误码：设备不存在
    }

    Camera_ErrorCode err = OH_PhotoOutput_Capture(photoOutput);
    if (err != CAMERA_OK) {
        state_.store(CaptureState::IDLE);
        OH_LOG_ERROR(LOG_APP, "Failed to trigger photo capture: %{public}d", err);
        // 通知拍照失败事件
        notifyPhotoEvent(photoEventCallback_, {PhotoEventType::CAPTURE_FAILED, sessionId, -1, "Failed to trigger photo capture"});
        return static_cast<int32_t>(err);  // 返回 HarmonyOS 相机错误码
    }

    outSessionId = sessionId;
    return 0;  // 成功
}

void CaptureManager::onSinglePhotoCaptured(void* buffer, size_t size, uint32_t width, uint32_t height) {
    // 用于在锁外调用的回调数据
    struct CallbackData {
        bool shouldCallback = false;
        std::string sessionId;
        PhotoEventCallback photoEventCallback;
        SinglePhotoCallback singlePhotoCallback;
    } callbackData;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        CaptureState currentState = state_.load();

        CM_LOG_INFO("[SINGLE_CAPTURE_BEGIN] sessionId=%{public}s, size=%{public}zu, %{public}ux%{public}u, state=%{public}d",
                    currentSessionId_.c_str(), size, width, height, static_cast<int>(currentState));

        if (currentState != CaptureState::SINGLE_CAPTURING) {
            CM_LOG_WARN("[SINGLE_CAPTURE_FAILED] Not in SINGLE_CAPTURING state, current: %{public}d",
                        static_cast<int>(currentState));
            return;
        }

        // 准备回调数据（在锁外调用）
        callbackData.shouldCallback = true;
        callbackData.sessionId = currentSessionId_;
        callbackData.photoEventCallback = photoEventCallback_;
        callbackData.singlePhotoCallback = singlePhotoCallback_;

        // 恢复空闲状态
        state_.store(CaptureState::IDLE);
        CM_LOG_INFO("[SINGLE_CAPTURE_END] sessionId=%{public}s, state=IDLE", currentSessionId_.c_str());
    }  // 释放锁

    // 在锁外调用回调
    if (callbackData.shouldCallback) {
        // 通知拍照结束事件
        if (callbackData.photoEventCallback) {
            callbackData.photoEventCallback({PhotoEventType::CAPTURE_END, callbackData.sessionId,
                                            -1, "Single photo captured successfully"});
        }

        // 直接回调，不经过队列
        if (callbackData.singlePhotoCallback) {
            callbackData.singlePhotoCallback(callbackData.sessionId, buffer, size, width, height);
        }
    }
}

// 拍照错误通知（由 ExpoCamera 调用，异步通知相机硬件错误）
void CaptureManager::onPhotoError(int32_t errorCode) {
    // 用于在锁外调用的回调数据
    struct CallbackData {
        bool shouldCallback = false;
        std::string sessionId;
        PhotoEventCallback photoEventCallback;
        PhotoErrorCallback photoErrorCallback;
        int32_t errorCode = 0;
    } callbackData;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        CaptureState currentState = state_.load();

        CM_LOG_ERROR("[PHOTO_ERROR] sessionId=%{public}s, errorCode=%{public}d, currentState=%{public}d",
                    currentSessionId_.c_str(), errorCode, static_cast<int>(currentState));

        // 只有在拍摄状态下才处理错误
        if (currentState != CaptureState::SINGLE_CAPTURING &&
            currentState != CaptureState::BURST_CAPTURING) {
            CM_LOG_WARN("[PHOTO_ERROR_IGNORED] Not in capturing state, ignoring error");
            return;
        }

        // 准备回调数据（在锁外调用）
        callbackData.shouldCallback = true;
        callbackData.sessionId = currentSessionId_;
        callbackData.photoEventCallback = photoEventCallback_;
        callbackData.photoErrorCallback = photoErrorCallback_;
        callbackData.errorCode = errorCode;

        // 恢复空闲状态
        state_.store(CaptureState::IDLE);
        CM_LOG_INFO("[PHOTO_ERROR_END] State reset to IDLE");
    }  // 释放锁

    // 在锁外调用回调
    if (callbackData.shouldCallback) {
        // 通知拍照失败事件
        if (callbackData.photoEventCallback) {
            callbackData.photoEventCallback({PhotoEventType::CAPTURE_FAILED, callbackData.sessionId,
                                            -1, "Photo capture failed"});
        }

        // 通知错误回调
        if (callbackData.photoErrorCallback) {
            callbackData.photoErrorCallback(callbackData.sessionId, callbackData.errorCode);
        }
    }
}

// ==================== 连拍 ====================

int32_t CaptureManager::startBurst(const BurstConfig& config, std::string& outSessionId) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_.load() != CaptureState::IDLE) {
        OH_LOG_WARN(LOG_APP, "Capture already in progress, state: %{public}d",
                    static_cast<int>(state_.load()));
        return -EBUSY;  // 系统标准错误码：设备忙
    }

    // 生成 sessionId
    std::string sessionId = generateSessionId();
    currentSessionId_ = sessionId;
    currentMode_ = CaptureMode::BURST;

    config_ = config;
    config_.sessionId = sessionId;
    captureCount_.store(0);
    processCount_.store(0);

    // 重置图像处理器
    processor_->reset();

    // 设置处理器进度回调
    processor_->setProgressCallback([this, sessionId](int32_t current, int32_t total,
                                                      void* buffer, size_t size) {
        if (config_.realtimePreview && imageCallback_ && buffer) {
            // 复制 buffer（回调后处理器可能释放）
            void* copyBuffer = malloc(size);
            if (copyBuffer) {
                memcpy(copyBuffer, buffer, size);
                notifyImage(copyBuffer, size, false);
            }
        }
    });

    // 设置处理器状态回调
    processor_->setStateCallback([this](const std::string& state, const std::string& message) {
        OH_LOG_INFO(LOG_APP, "Processor state: %{public}s, message: %{public}s",
                    state.c_str(), message.c_str());
    });

    // 更新状态和 sessionId
    state_.store(CaptureState::BURST_CAPTURING);
    progress_.sessionId = sessionId;
    progress_.state = CaptureState::BURST_CAPTURING;
    progress_.totalFrames = config.frameCount;
    progress_.capturedFrames = 0;
    progress_.processedFrames = 0;
    progress_.message = "Starting burst capture";
    notifyProgress();

    // 通知拍照开始事件
    notifyPhotoEvent(photoEventCallback_, {PhotoEventType::CAPTURE_START, sessionId, 0, "Burst capture started"});

    OH_LOG_INFO(LOG_APP, "Burst started: sessionId=%{public}s, %{public}d frames, exposure: %{public}dms",
                sessionId.c_str(), config.frameCount, config.exposureMs);

    // 启动拍照循环(在独立线程中)
    std::thread captureThread([this]() {
        startCaptureLoop();
    });
    captureThread.detach();

    outSessionId = sessionId;
    return 0;  // 成功
}

void CaptureManager::cancelBurst() {
    CaptureState expected = CaptureState::BURST_CAPTURING;
    if (!state_.compare_exchange_strong(expected, CaptureState::CANCELLED)) {
        expected = CaptureState::PROCESSING;
        if (!state_.compare_exchange_strong(expected, CaptureState::CANCELLED)) {
            CM_LOG_INFO("[CANCEL_BURST_IGNORED] Not in BURST_CAPTURING or PROCESSING state, currentState=%{public}d",
                        static_cast<int>(state_.load()));
            return;
        }
    }

    CM_LOG_INFO("[CANCEL_BURST] State changed to CANCELLED from %{public}d", static_cast<int>(expected));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.state = CaptureState::CANCELLED;
        progress_.message = "Burst cancelled";
    }
    notifyProgressSafe();

    // 清空队列
    taskQueue_->clear();

    CM_LOG_INFO("[CANCEL_BURST_DONE] Queue cleared");
}

void CaptureManager::onBurstPhotoCaptured(void* buffer, size_t size, uint32_t width, uint32_t height) {
    // 用于在锁外调用的回调数据
    struct CallbackData {
        bool shouldProcess = false;
        int32_t frameIndex = -1;
        bool isLastFrame = false;
        std::string sessionId;
        BurstProgress progress;
        PhotoEventCallback photoEventCallback;
        ProcessEventCallback processEventCallback;
        BurstProgressCallback progressCallback;
        int32_t frameCount = 0;
    } callbackData;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        CaptureState currentState = state_.load();

        if (currentState == CaptureState::CANCELLED) {
            CM_LOG_INFO("[BURST_CAPTURE_IGNORED] Burst cancelled, ignoring captured photo");
            return;
        }

        if (currentState != CaptureState::BURST_CAPTURING) {
            CM_LOG_WARN("[BURST_CAPTURE_IGNORED] Not in BURST_CAPTURING state, currentState=%{public}d",
                        static_cast<int>(currentState));
            return;
        }

        int32_t frameIndex = captureCount_.fetch_add(1);

        if (frameIndex >= config_.frameCount) {
            CM_LOG_WARN("[BURST_CAPTURE_IGNORED] Already captured enough frames, frameIndex=%{public}d >= frameCount=%{public}d",
                        frameIndex, config_.frameCount);
            return;
        }

        CM_LOG_INFO("[BURST_CAPTURE] frame=%{public}d/%{public}d, size=%{public}zu, %{public}ux%{public}u",
                    frameIndex + 1, config_.frameCount, size, width, height);

        // 复制 buffer
        void* copyBuffer = malloc(size);
        if (!copyBuffer) {
            CM_LOG_ERROR("[BURST_CAPTURE_ALLOC_FAILED] Failed to allocate buffer for frame %{public}d", frameIndex);
            return;
        }
        memcpy(copyBuffer, buffer, size);

        // 创建任务并入队
        ImageTask task;
        task.taskId = frameIndex;
        task.buffer = copyBuffer;
        task.size = size;
        task.width = width;
        task.height = height;
        task.isFirst = (frameIndex == 0);

        taskQueue_->enqueue(std::move(task));

        // 更新进度
        progress_.capturedFrames = frameIndex + 1;

        // 准备回调数据（在锁外调用）
        callbackData.shouldProcess = true;
        callbackData.frameIndex = frameIndex;
        callbackData.isLastFrame = (frameIndex + 1 == config_.frameCount);
        callbackData.sessionId = currentSessionId_;
        callbackData.progress = progress_;
        callbackData.photoEventCallback = photoEventCallback_;
        callbackData.processEventCallback = processEventCallback_;
        callbackData.progressCallback = progressCallback_;
        callbackData.frameCount = config_.frameCount;

        // 最后一帧捕获完成，切换到处理状态
        if (callbackData.isLastFrame) {
            CM_LOG_INFO("[BURST_CAPTURE_ALL_DONE] All %{public}d frames captured, switching to PROCESSING state",
                        config_.frameCount);

            state_.store(CaptureState::PROCESSING);
            progress_.state = CaptureState::PROCESSING;
            progress_.message = "All frames captured, processing stacked image";
            callbackData.progress = progress_;
        }
    }  // 释放锁

    // 在锁外调用回调
    if (callbackData.shouldProcess) {
        // 进度回调
        if (callbackData.progressCallback) {
            callbackData.progressCallback(callbackData.progress);
        }

        // 拍照事件回调
        if (callbackData.photoEventCallback) {
            callbackData.photoEventCallback({PhotoEventType::CAPTURE_END, callbackData.sessionId,
                                            callbackData.frameIndex, "Frame captured"});
        }

        // 最后一帧：处理开始事件
        if (callbackData.isLastFrame && callbackData.processEventCallback) {
            callbackData.processEventCallback({ProcessEventType::PROCESS_START, callbackData.sessionId,
                                              0, 0, callbackData.frameCount, "Processing started"});
        }
    }
}

void CaptureManager::startCaptureLoop() {
    CM_LOG_INFO("[CAPTURE_LOOP_START] frameCount=%{public}d", config_.frameCount);

    for (int32_t i = 0; i < config_.frameCount; i++) {
        if (state_.load() == CaptureState::CANCELLED) {
            CM_LOG_INFO("[CAPTURE_LOOP_CANCELLED] Burst cancelled at frame %{public}d", i);
            break;
        }

        CM_LOG_INFO("[CAPTURE_LOOP_TRIGGER] Triggering frame %{public}d/%{public}d", i + 1, config_.frameCount);
        captureNextFrame();

        // 等待曝光时间(除了最后一帧)
        if (i < config_.frameCount - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.exposureMs));
        }
    }

    // 注意：不在这里切换到 PROCESSING 状态！
    // 状态切换移到 onBurstPhotoCaptured 中，当最后一帧回调到达时才切换
    // 因为 OH_PhotoOutput_Capture 是异步的，回调可能在循环结束后才到达

    CM_LOG_INFO("[CAPTURE_LOOP_END] All capture requests sent, waiting for callbacks");
}

void CaptureManager::captureNextFrame() {
    // 获取 PhotoOutput 并触发拍照
    Camera_PhotoOutput* photoOutput = getPhotoOutput();
    if (!photoOutput) {
        OH_LOG_ERROR(LOG_APP, "PhotoOutput is null");
        return;
    }

    Camera_ErrorCode err = OH_PhotoOutput_Capture(photoOutput);
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to capture photo: %{public}d", err);
    } else {
        OH_LOG_INFO(LOG_APP, "Photo capture triggered");
    }
}

void CaptureManager::processTask(ImageTask&& task) {
    CaptureState currentState = state_.load();

    if (currentState == CaptureState::CANCELLED) {
        CM_LOG_INFO("[PROCESS_TASK_SKIP] taskId=%{public}d, burst cancelled", task.taskId);
        return;
    }

    CM_LOG_INFO("[PROCESS_TASK_BEGIN] taskId=%{public}d, size=%{public}zu, %{public}dx%{public}d, state=%{public}d",
                task.taskId, task.size, task.width, task.height, static_cast<int>(currentState));

    // 第一帧: 初始化处理会话
    if (task.isFirst) {
        // 使用第一帧的实际尺寸
        imageWidth_ = task.width;
        imageHeight_ = task.height;

        CM_LOG_INFO("[PROCESS_TASK_INIT] Initializing processor, frameCount=%{public}d, size=%{public}dx%{public}d",
                    config_.frameCount, imageWidth_, imageHeight_);

        if (!processor_->initStacking(config_.frameCount, imageWidth_, imageHeight_)) {
            CM_LOG_ERROR("[PROCESS_TASK_INIT_FAILED] Failed to init processing session");
            state_.store(CaptureState::ERROR);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                progress_.state = CaptureState::ERROR;
                progress_.message = "Failed to initialize processing session";
            }
            notifyProgressSafe();

            // 通知处理失败事件
            notifyProcessEvent(processEventCallback_, {ProcessEventType::PROCESS_FAILED, currentSessionId_,
                              0, 0, config_.frameCount, "Failed to initialize processing session"});

            return;
        }
        CM_LOG_INFO("[PROCESS_TASK_INIT_OK] Processor initialized successfully");
    }

    // 处理帧（原始图像数据，由 processor 内部解码）
    // 假设相机输出为 JPEG 格式
    if (!processor_->processFrame(task.taskId, task.buffer, task.size,
                                   task.width, task.height, ImageFormat::JPEG, task.isFirst)) {
        CM_LOG_ERROR("[PROCESS_TASK_FRAME_FAILED] Failed to process frame %{public}d", task.taskId);
    }

    int32_t processed = processCount_.fetch_add(1) + 1;

    // 更新进度（加锁保护）并通知
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.processedFrames = processed;
    }
    notifyProgressSafe();

    // 计算进度百分比
    int32_t progressPercent = (processed * 100) / config_.frameCount;

    CM_LOG_INFO("[PROCESS_TASK_PROGRESS] taskId=%{public}d, processed=%{public}d/%{public}d (%{public}d%%)",
                task.taskId, processed, config_.frameCount, progressPercent);

    // 通知处理进度事件
    notifyProcessEvent(processEventCallback_, {ProcessEventType::PROCESS_PROGRESS, currentSessionId_,
                      progressPercent, processed, config_.frameCount, "Processing frame"});

    // 如果开启实时预览,获取当前累积结果
    if (config_.realtimePreview && imageCallback_) {
        void* resultBuffer = nullptr;
        size_t resultSize = 0;
        if (processor_->getCurrentResult(&resultBuffer, &resultSize)) {
            notifyImage(resultBuffer, resultSize, false);
        } else {
            CM_LOG_WARN("[PROCESS_TASK_PREVIEW_FAILED] Failed to get current result for frame %{public}d", task.taskId);
        }
    }

    // 检查是否全部处理完成
    if (processed >= config_.frameCount) {
        CM_LOG_INFO("[PROCESS_TASK_FINALIZE] All frames processed, finalizing... processed=%{public}d, frameCount=%{public}d",
                    processed, config_.frameCount);

        // 获取最终结果
        void* finalBuffer = nullptr;
        size_t finalSize = 0;

        if (processor_->finalize(&finalBuffer, &finalSize)) {
            CM_LOG_INFO("[PROCESS_TASK_FINALIZE_OK] finalSize=%{public}zu", finalSize);
            // 通知最终结果
            notifyImage(finalBuffer, finalSize, true);

            // 恢复空闲状态，允许再次拍摄
            state_.store(CaptureState::IDLE);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                progress_.state = CaptureState::IDLE;
                progress_.message = "Burst capture completed successfully";
            }

            // 通知处理完成事件
            notifyProcessEvent(processEventCallback_, {ProcessEventType::PROCESS_END, currentSessionId_,
                              100, config_.frameCount, config_.frameCount, "Processing completed successfully"});
        } else {
            CM_LOG_ERROR("[PROCESS_TASK_FINALIZE_FAILED] Failed to finalize processing result");
            // 恢复空闲状态，允许再次拍摄
            state_.store(CaptureState::IDLE);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                progress_.state = CaptureState::IDLE;
                progress_.message = "Failed to finalize processing result";
            }

            // 通知处理失败事件
            notifyProcessEvent(processEventCallback_, {ProcessEventType::PROCESS_FAILED, currentSessionId_,
                              progressPercent, processed, config_.frameCount, "Failed to finalize processing result"});
        }
        notifyProgressSafe();
    }
}

void CaptureManager::notifyProgress() {
    // 调用者负责确保 progress_ 的一致性
    // 大多数调用者已在锁内修改 progress_，直接使用即可
    // processTask 中需要先在锁内更新，再调用此方法
    if (progressCallback_) {
        progressCallback_(progress_);
    }
}

void CaptureManager::notifyProgressSafe() {
    // 线程安全版本：加锁复制后回调
    BurstProgress progressCopy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progressCopy = progress_;
    }
    if (progressCallback_) {
        progressCallback_(progressCopy);
    }
}

void CaptureManager::notifyImage(void* buffer, size_t size, bool isFinal) {
    CM_LOG_INFO("[NOTIFY_IMAGE] buffer=%{public}p, size=%{public}zu, isFinal=%{public}d, callback=%{public}s",
                buffer, size, isFinal, imageCallback_ ? "set" : "null");
    if (imageCallback_) {
        imageCallback_(progress_.sessionId, buffer, size, isFinal);
    }
}

// ==================== 状态查询 ====================

bool CaptureManager::isCaptureActive() const {
    CaptureState s = state_.load();
    return s == CaptureState::SINGLE_CAPTURING ||
           s == CaptureState::BURST_CAPTURING ||
           s == CaptureState::PROCESSING;
}

bool CaptureManager::isBurstActive() const {
    CaptureState s = state_.load();
    return s == CaptureState::BURST_CAPTURING || s == CaptureState::PROCESSING;
}

Camera_PhotoOutput* CaptureManager::getPhotoOutput() const {
    // 通过 ExpoCamera 获取 PhotoOutput
    return ExpoCamera::getInstance().getPhotoOutput();
}

int32_t CaptureManager::switchCaptureMode(CaptureMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 检查是否可以切换
    if (!ExpoCamera::getInstance().canSwitchMode()) {
        CM_LOG_WARN("[SWITCH_MODE_FAILED] Cannot switch mode: not ready");
        return -1;  // 不满足切换条件
    }

    // 检查是否已经是目标模式
    if (currentMode_ == mode) {
        CM_LOG_INFO("[SWITCH_MODE_SKIP] Already in mode %{public}d", static_cast<int>(mode));
        return 0;
    }

    CM_LOG_INFO("[SWITCH_MODE_BEGIN] Switching mode: %{public}d -> %{public}d",
                static_cast<int>(currentMode_), static_cast<int>(mode));

    // 调用 ExpoCamera 切换模式
    Camera_ErrorCode err = ExpoCamera::getInstance().switchCaptureMode(mode);
    if (err != CAMERA_OK) {
        CM_LOG_ERROR("[SWITCH_MODE_FAILED] ExpoCamera switchCaptureMode failed: %{public}d", err);
        return static_cast<int32_t>(err);
    }

    // 同步更新当前模式
    currentMode_ = mode;

    CM_LOG_INFO("[SWITCH_MODE_DONE] Mode switched to %{public}d", static_cast<int>(mode));
    return 0;
}

bool CaptureManager::canSwitchMode() const {
    return ExpoCamera::getInstance().canSwitchMode() && !isCaptureActive();
}

} // namespace exposhot
