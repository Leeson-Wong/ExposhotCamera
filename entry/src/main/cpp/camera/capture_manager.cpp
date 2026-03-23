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

// 状态转字符串（用于日志）
static const char* stateToString(CaptureState state) {
    switch (state) {
        case CaptureState::IDLE:             return "IDLE";
        case CaptureState::SINGLE_CAPTURING: return "SINGLE_CAPTURING";
        case CaptureState::BURST_CAPTURING:  return "BURST_CAPTURING";
        case CaptureState::PROCESSING:       return "PROCESSING";
        case CaptureState::COMPLETED:        return "COMPLETED";
        case CaptureState::ERROR:            return "ERROR";
        case CaptureState::CANCELLED:        return "CANCELLED";
        default:                             return "UNKNOWN";
    }
}

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

bool CaptureManager::init() {
    std::lock_guard<std::mutex> lock(mutex_);

    CM_LOG_INFO("[INIT_BEGIN] currentState=%{public}d, taskQueueRunning=%{public}d",
                static_cast<int>(state_.load()), taskQueue_->isRunning());

    if (state_.load() != CaptureState::IDLE) {
        CM_LOG_WARN("[INIT_FAILED] CaptureManager not in IDLE state");
        return false;
    }

    // 确保之前的资源已释放
    if (taskQueue_->isRunning()) {
        CM_LOG_WARN("[INIT_FORCE_STOP] TaskQueue still running, force stop first");
        taskQueue_->stop();
    }

    // 初始化下层 ExpoCamera（依赖方向：CaptureManager → ExpoCamera）
    Camera_ErrorCode err = ExpoCamera::getInstance().init();
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
    CM_LOG_INFO("[RELEASE_BEGIN] currentState=%{public}d", static_cast<int>(state_.load()));

    cancelBurst();
    taskQueue_->stop();
    processor_->reset();

    // 清理注册到 ExpoCamera 的回调
    ExpoCamera::getInstance().setPhotoCapturedCallback(nullptr);
    ExpoCamera::getInstance().setPhotoErrorCallback(nullptr);

    // 释放下层 ExpoCamera
    ExpoCamera::getInstance().release();

    CM_LOG_INFO("[RELEASE_DONE] CaptureManager released");
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

    CaptureState currentState = state_.load();
    CM_LOG_INFO("[CAPTURE_SINGLE_BEGIN] currentState=%{public}d (%{public}s)",
                static_cast<int>(currentState), stateToString(currentState));

    // 检查是否有拍摄进行中
    if (isCaptureActive()) {
        CM_LOG_WARN("[CAPTURE_SINGLE_BUSY] Capture already in progress, state: %{public}d (%{public}s)",
                    static_cast<int>(currentState), stateToString(currentState));
        return -EBUSY;  // 系统标准错误码：设备忙
    }

    // 如果状态是 COMPLETED 或 ERROR 或 CANCELLED，重置为 IDLE
    if (currentState == CaptureState::COMPLETED ||
        currentState == CaptureState::ERROR ||
        currentState == CaptureState::CANCELLED) {
        CM_LOG_INFO("[CAPTURE_SINGLE_RESET] Resetting state from %{public}d (%{public}s) to IDLE",
                    static_cast<int>(currentState), stateToString(currentState));
        state_.store(CaptureState::IDLE);
    }

    // 生成 sessionId
    std::string sessionId = generateSessionId();
    currentSessionId_ = sessionId;
    currentMode_ = CaptureMode::SINGLE;

    // 进入单拍状态
    CaptureState oldState = state_.exchange(CaptureState::SINGLE_CAPTURING);
    CM_LOG_INFO("[STATE_CHANGE] %{public}s -> %{public}s",
                stateToString(oldState), stateToString(CaptureState::SINGLE_CAPTURING));

    // 通知拍照开始事件
    notifyPhotoEvent(photoEventCallback_, {PhotoEventType::CAPTURE_START, sessionId, -1, "Single capture started"});

    OH_LOG_INFO(LOG_APP, "Single capture started: sessionId=%{public}s", sessionId.c_str());

    // 获取 PhotoOutput 并触发拍照
    Camera_PhotoOutput* photoOutput = getPhotoOutput();
    if (!photoOutput) {
        OH_LOG_ERROR(LOG_APP, "PhotoOutput is null");
        CaptureState oldState = state_.exchange(CaptureState::IDLE);
        CM_LOG_INFO("[STATE_CHANGE] %{public}s -> %{public}s (PhotoOutput null)",
                    stateToString(oldState), stateToString(CaptureState::IDLE));
        // 通知拍照失败事件
        notifyPhotoEvent(photoEventCallback_, {PhotoEventType::CAPTURE_FAILED, sessionId, -1, "PhotoOutput is null"});
        return -ENODEV;  // 系统标准错误码：设备不存在
    }

    Camera_ErrorCode err = OH_PhotoOutput_Capture(photoOutput);
    if (err != CAMERA_OK) {
        CaptureState oldState = state_.exchange(CaptureState::IDLE);
        CM_LOG_INFO("[STATE_CHANGE] %{public}s -> %{public}s (capture trigger failed)",
                    stateToString(oldState), stateToString(CaptureState::IDLE));
        OH_LOG_ERROR(LOG_APP, "Failed to trigger photo capture: %{public}d", err);
        // 通知拍照失败事件
        notifyPhotoEvent(photoEventCallback_, {PhotoEventType::CAPTURE_FAILED, sessionId, -1, "Failed to trigger photo capture"});
        return static_cast<int32_t>(err);  // 返回 HarmonyOS 相机错误码
    }

    outSessionId = sessionId;
    return 0;  // 成功
}

void CaptureManager::onSinglePhotoCaptured(void* buffer, size_t size, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);

    CaptureState currentState = state_.load();

    CM_LOG_INFO("[SINGLE_CAPTURE_BEGIN] sessionId=%{public}s, size=%{public}zu, %{public}ux%{public}u, state=%{public}d",
                currentSessionId_.c_str(), size, width, height, static_cast<int>(currentState));

    if (currentState != CaptureState::SINGLE_CAPTURING) {
        CM_LOG_WARN("[SINGLE_CAPTURE_FAILED] Not in SINGLE_CAPTURING state, current: %{public}d",
                    static_cast<int>(currentState));
        return;
    }

    // 通知拍照结束事件
    notifyPhotoEvent(photoEventCallback_, {PhotoEventType::CAPTURE_END, currentSessionId_, -1, "Single photo captured successfully"});

    // 直接回调，不经过队列
    if (singlePhotoCallback_) {
        singlePhotoCallback_(currentSessionId_, buffer, size, width, height);
    } else {
        CM_LOG_WARN("[SINGLE_CAPTURE_NO_CALLBACK] No single photo callback set");
    }

    // 恢复空闲状态
    CaptureState oldState = state_.exchange(CaptureState::IDLE);
    CM_LOG_INFO("[STATE_CHANGE] %{public}s -> %{public}s",
                stateToString(oldState), stateToString(CaptureState::IDLE));
    CM_LOG_INFO("[SINGLE_CAPTURE_END] sessionId=%{public}s, state=IDLE", currentSessionId_.c_str());
}

// 拍照错误通知（由 ExpoCamera 调用，异步通知相机硬件错误）
void CaptureManager::onPhotoError(int32_t errorCode) {
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

    // 通知拍照失败事件
    notifyPhotoEvent(photoEventCallback_, {PhotoEventType::CAPTURE_FAILED, currentSessionId_, -1, "Photo capture failed"});

    // 通知错误回调
    if (photoErrorCallback_) {
        photoErrorCallback_(currentSessionId_, errorCode);
    }

    // 恢复空闲状态
    CaptureState oldState = state_.exchange(CaptureState::IDLE);
    CM_LOG_INFO("[STATE_CHANGE] %{public}s -> %{public}s",
                stateToString(oldState), stateToString(CaptureState::IDLE));
    CM_LOG_INFO("[PHOTO_ERROR_END] State reset to IDLE");
}

// ==================== 连拍 ====================

int32_t CaptureManager::startBurst(const BurstConfig& config, std::string& outSessionId) {
    std::lock_guard<std::mutex> lock(mutex_);

    CaptureState currentState = state_.load();
    CM_LOG_INFO("[START_BURST_BEGIN] currentState=%{public}d (%{public}s), frameCount=%{public}d",
                static_cast<int>(currentState), stateToString(currentState), config.frameCount);

    if (currentState != CaptureState::IDLE) {
        // 如果状态是 COMPLETED/ERROR/CANCELLED，可以重置并继续
        if (currentState == CaptureState::COMPLETED ||
            currentState == CaptureState::ERROR ||
            currentState == CaptureState::CANCELLED) {
            CM_LOG_INFO("[START_BURST_RESET] Resetting state from %{public}d (%{public}s) to IDLE",
                        static_cast<int>(currentState), stateToString(currentState));
            state_.store(CaptureState::IDLE);
        } else {
            CM_LOG_WARN("[START_BURST_BUSY] Capture already in progress, state: %{public}d (%{public}s)",
                        static_cast<int>(currentState), stateToString(currentState));
            return -EBUSY;  // 系统标准错误码：设备忙
        }
    }

    // 生成 sessionId
    std::string sessionId = generateSessionId();
    currentSessionId_ = sessionId;
    currentMode_ = CaptureMode::BURST;

    config_ = config;
    config_.sessionId = sessionId;
    captureCount_.store(0);
    processCount_.store(0);

    // 注意：不要在这里重置图像处理器！
    // 如果上一轮还在处理中，reset() 会释放 passthroughBuffer_ 导致崩溃
    // reset 应该在 finalize 完成后或取消后进行
    // processor_->reset();  // 移除，改为在需要时重置

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
    CaptureState oldState = state_.exchange(CaptureState::BURST_CAPTURING);
    CM_LOG_INFO("[STATE_CHANGE] %{public}s -> %{public}s",
                stateToString(oldState), stateToString(CaptureState::BURST_CAPTURING));

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
            CM_LOG_INFO("[CANCEL_BURST_IGNORED] Not in BURST_CAPTURING or PROCESSING state, currentState=%{public}d (%{public}s)",
                        static_cast<int>(state_.load()), stateToString(state_.load()));
            return;
        }
    }

    CM_LOG_INFO("[STATE_CHANGE] %{public}s -> %{public}s (cancel burst)",
                stateToString(expected), stateToString(CaptureState::CANCELLED));
    CM_LOG_INFO("[CANCEL_BURST] State changed to CANCELLED");

    progress_.state = CaptureState::CANCELLED;
    progress_.message = "Burst cancelled";
    notifyProgress();

    // 清空队列（等待当前任务完成后再清空）
    // 注意：clear() 会等待锁，但不会中断正在处理的任务
    taskQueue_->clear();

    // 重置处理器，释放可能存在的资源
    processor_->reset();

    CM_LOG_INFO("[CANCEL_BURST_DONE] Queue cleared, processor reset");
}

void CaptureManager::onBurstPhotoCaptured(void* buffer, size_t size, uint32_t width, uint32_t height) {
    CaptureState currentState = state_.load();

    CM_LOG_ERROR("[BURST_PHOTO_RAW_INPUT] buffer=%{public}p, size=%{public}zu, width=%{public}u, height=%{public}u",
                buffer, size, static_cast<unsigned int>(width), static_cast<unsigned int>(height));

    // 验证图像尺寸是否合理
    if (width == 0 || height == 0 || width > 10000 || height > 10000) {
        CM_LOG_ERROR("[BURST_PHOTO_INVALID_SIZE] Invalid image dimensions, freeing buffer");
        free(buffer);  // 释放传入的 buffer
        return;
    }

    if (currentState == CaptureState::CANCELLED) {
        CM_LOG_INFO("[BURST_CAPTURE_IGNORED] Burst cancelled, ignoring captured photo");
        free(buffer);  // 释放传入的 buffer
        return;
    }

    if (currentState != CaptureState::BURST_CAPTURING) {
        CM_LOG_WARN("[BURST_CAPTURE_IGNORED] Not in BURST_CAPTURING state, currentState=%{public}d",
                    static_cast<int>(currentState));
        free(buffer);  // 释放传入的 buffer
        return;
    }

    int32_t frameIndex = captureCount_.fetch_add(1);

    if (frameIndex >= config_.frameCount) {
        // 已拍摄足够帧数
        CM_LOG_WARN("[BURST_CAPTURE_IGNORED] Already captured enough frames, frameIndex=%{public}d >= frameCount=%{public}d",
                    frameIndex, config_.frameCount);
        free(buffer);  // 释放传入的 buffer
        return;
    }

    CM_LOG_INFO("[BURST_CAPTURE] frame=%{public}d/%{public}d, bufferSize=%{public}zu",
                frameIndex + 1, config_.frameCount, size);
    CM_LOG_INFO("[BURST_CAPTURE_DIMS] width=%{public}u, height=%{public}u",
                static_cast<unsigned int>(width), static_cast<unsigned int>(height));

    // 复制 buffer
    void* copyBuffer = malloc(size);
    if (!copyBuffer) {
        CM_LOG_ERROR("[BURST_CAPTURE_ALLOC_FAILED] Failed to allocate buffer for frame %{public}d", frameIndex);
        return;
    }
    memcpy(copyBuffer, buffer, size);

    // 释放原始 buffer（数据已复制）
    free(buffer);

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
    notifyProgress();

    // 通知拍照进度事件
    notifyPhotoEvent(photoEventCallback_, {PhotoEventType::CAPTURE_END, currentSessionId_, frameIndex,
                                           "Frame captured"});

    // 最后一帧捕获完成，切换到处理状态
    if (frameIndex + 1 == config_.frameCount) {
        CM_LOG_INFO("[BURST_CAPTURE_ALL_DONE] All %{public}d frames captured, switching to PROCESSING state",
                    config_.frameCount);

        CaptureState oldState = state_.exchange(CaptureState::PROCESSING);
        CM_LOG_INFO("[STATE_CHANGE] %{public}s -> %{public}s",
                    stateToString(oldState), stateToString(CaptureState::PROCESSING));

        progress_.state = CaptureState::PROCESSING;
        progress_.message = "All frames captured, processing stacked image";
        notifyProgress();

        // 通知处理开始事件
        notifyProcessEvent(processEventCallback_, {ProcessEventType::PROCESS_START, currentSessionId_,
                          0, 0, config_.frameCount, "Processing started"});
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

    CM_LOG_INFO("[PROCESS_TASK_ENTER] taskId=%{public}d, currentState=%{public}s",
                task.taskId, stateToString(currentState));

    if (currentState == CaptureState::CANCELLED) {
        CM_LOG_INFO("[PROCESS_TASK_SKIP] taskId=%{public}d, burst cancelled", task.taskId);
        return;
    }

    CM_LOG_INFO("[PROCESS_TASK_BEGIN] taskId=%{public}d, bufferSize=%{public}zu",
                task.taskId, task.size);
    CM_LOG_INFO("[PROCESS_TASK_DIMS] width=%{public}u, height=%{public}u",
                static_cast<unsigned int>(task.width), static_cast<unsigned int>(task.height));

    // 第一帧: 初始化处理会话
    if (task.isFirst) {
        // 使用第一帧的实际尺寸
        imageWidth_ = task.width;
        imageHeight_ = task.height;

        CM_LOG_INFO("[PROCESS_TASK_INIT] Initializing processor, frameCount=%{public}d, size=%{public}dx%{public}d",
                    config_.frameCount, imageWidth_, imageHeight_);

        // 在初始化新会话前重置处理器（确保旧资源已释放）
        processor_->reset();

        if (!processor_->initStacking(config_.frameCount, imageWidth_, imageHeight_)) {
            CM_LOG_ERROR("[PROCESS_TASK_INIT_FAILED] Failed to init processing session");
            CaptureState oldState = state_.exchange(CaptureState::ERROR);
            CM_LOG_INFO("[STATE_CHANGE] %{public}s -> %{public}s",
                        stateToString(oldState), stateToString(CaptureState::ERROR));

            progress_.state = CaptureState::ERROR;
            progress_.message = "Failed to initialize processing session";
            notifyProgress();

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
    progress_.processedFrames = processed;
    notifyProgress();

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
        }
    }

    // 检查是否全部处理完成
    if (processed >= config_.frameCount) {
        CM_LOG_INFO("[PROCESS_TASK_FINALIZE] All frames processed, finalizing... processed=%{public}d, frameCount=%{public}d",
                    processed, config_.frameCount);

        // 获取最终结果
        void* finalBuffer = nullptr;
        size_t finalSize = 0;

        CM_LOG_INFO("[PROCESS_TASK_FINALIZE_CALL] Calling processor_->finalize()");
        if (processor_->finalize(&finalBuffer, &finalSize)) {
            CM_LOG_INFO("[PROCESS_TASK_FINALIZE_OK] finalBuffer=%{public}p, finalSize=%{public}zu",
                        finalBuffer, finalSize);

            // 通知最终结果
            CM_LOG_INFO("[PROCESS_TASK_NOTIFY_IMAGE] Calling notifyImage()");
            notifyImage(finalBuffer, finalSize, true);
            CM_LOG_INFO("[PROCESS_TASK_NOTIFY_IMAGE_DONE] notifyImage() returned");

            CaptureState oldState = state_.exchange(CaptureState::COMPLETED);
            CM_LOG_INFO("[STATE_CHANGE] %{public}s -> %{public}s (session finished, ready for next)",
                        stateToString(oldState), stateToString(CaptureState::COMPLETED));

            progress_.state = CaptureState::COMPLETED;
            progress_.message = "Burst capture completed successfully";

            // 通知处理完成事件
            CM_LOG_INFO("[PROCESS_TASK_NOTIFY_PROCESS_END] Calling notifyProcessEvent()");
            notifyProcessEvent(processEventCallback_, {ProcessEventType::PROCESS_END, currentSessionId_,
                              100, config_.frameCount, config_.frameCount, "Processing completed successfully"});
            CM_LOG_INFO("[PROCESS_TASK_ALL_DONE] All done, function returning");
        } else {
            CM_LOG_ERROR("[PROCESS_TASK_FINALIZE_FAILED] Failed to finalize processing result");
            CaptureState oldState = state_.exchange(CaptureState::ERROR);
            CM_LOG_INFO("[STATE_CHANGE] %{public}s -> %{public}s",
                        stateToString(oldState), stateToString(CaptureState::ERROR));

            progress_.state = CaptureState::ERROR;
            progress_.message = "Failed to finalize processing result";

            // 通知处理失败事件
            notifyProcessEvent(processEventCallback_, {ProcessEventType::PROCESS_FAILED, currentSessionId_,
                              progressPercent, processed, config_.frameCount, "Failed to finalize processing result"});
        }
        notifyProgress();
    }
}

void CaptureManager::notifyProgress() {
    if (progressCallback_) {
        progressCallback_(progress_);
    }
}

void CaptureManager::notifyImage(void* buffer, size_t size, bool isFinal) {
    CM_LOG_INFO("[NOTIFY_IMAGE_BEGIN] buffer=%{public}p, size=%{public}zu, isFinal=%{public}d, callback=%{public}s",
                buffer, size, isFinal, imageCallback_ ? "set" : "null");

    if (!buffer || size == 0) {
        CM_LOG_ERROR("[NOTIFY_IMAGE_ERROR] Invalid buffer or size");
        return;
    }

    if (imageCallback_) {
        // 回调到 UI 层，注意：buffer 的所有权转移给回调接收者
        // 调用者负责确保 buffer 在回调返回后不再被使用
        CM_LOG_INFO("[NOTIFY_IMAGE_CALL] Calling imageCallback_...");
        imageCallback_(progress_.sessionId, buffer, size, isFinal);
        CM_LOG_INFO("[NOTIFY_IMAGE_END] callback returned, isFinal=%{public}d", isFinal);
    } else {
        // 没有回调，需要释放 buffer 避免内存泄漏
        CM_LOG_WARN("[NOTIFY_IMAGE_NO_CALLBACK] No callback, freeing buffer");
        free(buffer);
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

} // namespace exposhot
