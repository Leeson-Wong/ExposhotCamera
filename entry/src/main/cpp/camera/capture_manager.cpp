#include "capture_manager.h"
#include "expo_camera.h"
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

bool CaptureManager::init() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_.load() != CaptureState::IDLE) {
        OH_LOG_WARN(LOG_APP, "CaptureManager not in IDLE state, current: %{public}d",
                    static_cast<int>(state_.load()));
        return false;
    }

    // 启动任务队列
    taskQueue_->start([this](ImageTask&& task) {
        processTask(std::move(task));
    });

    OH_LOG_INFO(LOG_APP, "CaptureManager initialized");
    return true;
}

void CaptureManager::release() {
    cancelBurst();
    taskQueue_->stop();
    processor_->reset();
    OH_LOG_INFO(LOG_APP, "CaptureManager released");
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

    OH_LOG_INFO(LOG_APP, "Single capture started: sessionId=%{public}s", sessionId.c_str());

    // 获取 PhotoOutput 并触发拍照
    Camera_PhotoOutput* photoOutput = getPhotoOutput();
    if (!photoOutput) {
        OH_LOG_ERROR(LOG_APP, "PhotoOutput is null");
        state_.store(CaptureState::IDLE);
        return -ENODEV;  // 系统标准错误码：设备不存在
    }

    Camera_ErrorCode err = OH_PhotoOutput_Capture(photoOutput);
    if (err != CAMERA_OK) {
        state_.store(CaptureState::IDLE);
        OH_LOG_ERROR(LOG_APP, "Failed to trigger photo capture: %{public}d", err);
        return static_cast<int32_t>(err);  // 返回 HarmonyOS 相机错误码
    }

    outSessionId = sessionId;
    return 0;  // 成功
}

void CaptureManager::onSinglePhotoCaptured(void* buffer, size_t size, int32_t width, int32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_.load() != CaptureState::SINGLE_CAPTURING) {
        OH_LOG_WARN(LOG_APP, "Not in SINGLE_CAPTURING state, current: %{public}d",
                    static_cast<int>(state_.load()));
        return;
    }

    OH_LOG_INFO(LOG_APP, "Single photo captured: sessionId=%{public}s, size=%{public}zu, %{public}dx%{public}d",
                currentSessionId_.c_str(), size, width, height);

    // 直接回调，不经过队列
    if (singlePhotoCallback_) {
        singlePhotoCallback_(currentSessionId_, buffer, size, width, height);
    } else {
        OH_LOG_WARN(LOG_APP, "No single photo callback set");
    }

    // 恢复空闲状态
    state_.store(CaptureState::IDLE);
    OH_LOG_INFO(LOG_APP, "Single capture completed");
}

// 拍照错误通知（由 ExpoCamera 调用，异步通知相机硬件错误）
void CaptureManager::onPhotoError(int32_t errorCode) {
    std::lock_guard<std::mutex> lock(mutex_);

    CaptureState currentState = state_.load();

    // 只有在拍摄状态下才处理错误
    if (currentState != CaptureState::SINGLE_CAPTURING &&
        currentState != CaptureState::BURST_CAPTURING) {
        OH_LOG_WARN(LOG_APP, "Photo error received but not in capturing state: %{public}d",
                    static_cast<int>(currentState));
        return;
    }

    OH_LOG_ERROR(LOG_APP, "Photo error: sessionId=%{public}s, errorCode=%{public}d",
                currentSessionId_.c_str(), errorCode);

    // 通知错误回调
    if (photoErrorCallback_) {
        photoErrorCallback_(currentSessionId_, errorCode);
    }

    // 恢复空闲状态
    state_.store(CaptureState::IDLE);
    OH_LOG_INFO(LOG_APP, "Capture reset to IDLE due to error");
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
            return;
        }
    }

    progress_.state = CaptureState::CANCELLED;
    progress_.message = "Burst cancelled";
    notifyProgress();

    // 清空队列
    taskQueue_->clear();

    OH_LOG_INFO(LOG_APP, "Burst cancelled");
}

void CaptureManager::onBurstPhotoCaptured(void* buffer, size_t size, int32_t width, int32_t height) {
    if (state_.load() == CaptureState::CANCELLED) {
        OH_LOG_INFO(LOG_APP, "Burst cancelled, ignoring captured photo");
        return;
    }

    if (state_.load() != CaptureState::BURST_CAPTURING) {
        OH_LOG_WARN(LOG_APP, "Not in BURST_CAPTURING state, ignoring captured photo");
        return;
    }

    int32_t frameIndex = captureCount_.fetch_add(1);

    if (frameIndex >= config_.frameCount) {
        // 已拍摄足够帧数
        OH_LOG_INFO(LOG_APP, "Already captured enough frames, ignoring");
        return;
    }

    OH_LOG_INFO(LOG_APP, "Photo captured: frame %{public}d/%{public}d, size=%{public}zu, %{public}dx%{public}d",
                frameIndex + 1, config_.frameCount, size, width, height);

    // 复制 buffer
    void* copyBuffer = malloc(size);
    if (!copyBuffer) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate buffer for frame %{public}d", frameIndex);
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
    notifyProgress();
}

void CaptureManager::startCaptureLoop() {
    OH_LOG_INFO(LOG_APP, "Capture loop started, will capture %{public}d frames",
                config_.frameCount);

    for (int32_t i = 0; i < config_.frameCount; i++) {
        if (state_.load() == CaptureState::CANCELLED) {
            OH_LOG_INFO(LOG_APP, "Burst cancelled, stopping capture loop");
            break;
        }

        OH_LOG_INFO(LOG_APP, "Capturing frame %{public}d/%{public}d", i + 1, config_.frameCount);
        captureNextFrame();

        // 等待曝光时间(除了最后一帧)
        if (i < config_.frameCount - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.exposureMs));
        }
    }

    // 拍摄完成,切换到处理状态
    if (state_.load() == CaptureState::BURST_CAPTURING) {
        state_.store(CaptureState::PROCESSING);
        progress_.state = CaptureState::PROCESSING;
        progress_.message = "All frames captured, processing stacked image";
        notifyProgress();
        OH_LOG_INFO(LOG_APP, "Capture completed, switched to PROCESSING state");
    }

    OH_LOG_INFO(LOG_APP, "Capture loop ended");
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
    if (state_.load() == CaptureState::CANCELLED) {
        OH_LOG_INFO(LOG_APP, "Burst cancelled, skipping task %{public}d", task.taskId);
        return;
    }

    OH_LOG_INFO(LOG_APP, "Processing task %{public}d, buffer size: %{public}zu, %{public}dx%{public}d",
                task.taskId, task.size, task.width, task.height);

    // 第一帧: 初始化处理会话
    if (task.isFirst) {
        // 使用第一帧的实际尺寸
        imageWidth_ = task.width;
        imageHeight_ = task.height;

        if (!processor_->initStacking(config_.frameCount, imageWidth_, imageHeight_)) {
            OH_LOG_ERROR(LOG_APP, "Failed to init processing session");
            state_.store(CaptureState::ERROR);
            progress_.state = CaptureState::ERROR;
            progress_.message = "Failed to initialize processing session";
            notifyProgress();
            return;
        }
    }

    // 处理帧（原始图像数据，由 processor 内部解码）
    // 假设相机输出为 JPEG 格式
    if (!processor_->processFrame(task.taskId, task.buffer, task.size,
                                   task.width, task.height, ImageFormat::JPEG, task.isFirst)) {
        OH_LOG_ERROR(LOG_APP, "Failed to process frame %{public}d", task.taskId);
    }

    int32_t processed = processCount_.fetch_add(1) + 1;
    progress_.processedFrames = processed;
    notifyProgress();

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
        OH_LOG_INFO(LOG_APP, "All frames processed, finalizing...");

        // 获取最终结果
        void* finalBuffer = nullptr;
        size_t finalSize = 0;

        if (processor_->finalize(&finalBuffer, &finalSize)) {
            // 通知最终结果
            notifyImage(finalBuffer, finalSize, true);

            state_.store(CaptureState::COMPLETED);
            progress_.state = CaptureState::COMPLETED;
            progress_.message = "Burst capture completed successfully";
        } else {
            state_.store(CaptureState::ERROR);
            progress_.state = CaptureState::ERROR;
            progress_.message = "Failed to finalize processing result";
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

} // namespace exposhot
