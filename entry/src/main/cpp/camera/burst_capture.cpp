#include "burst_capture.h"
#include "expo_camera.h"
#include "hilog/log.h"

#include <thread>
#include <chrono>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "BurstCapture"

namespace exposhot {

BurstCapture& BurstCapture::getInstance() {
    static BurstCapture instance;
    return instance;
}

BurstCapture::BurstCapture()
    : taskQueue_(std::make_unique<TaskQueue>())
    , processor_(std::make_unique<ImageProcessor>()) {
}

BurstCapture::~BurstCapture() {
    release();
}

bool BurstCapture::init() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_.load() != BurstState::IDLE) {
        OH_LOG_WARN(LOG_APP, "BurstCapture not in IDLE state, current: %{public}d",
                    static_cast<int>(state_.load()));
        return false;
    }

    // 启动任务队列
    taskQueue_->start([this](ImageTask&& task) {
        processTask(std::move(task));
    });

    OH_LOG_INFO(LOG_APP, "BurstCapture initialized");
    return true;
}

void BurstCapture::release() {
    cancelBurst();
    taskQueue_->stop();
    processor_->reset();
    OH_LOG_INFO(LOG_APP, "BurstCapture released");
}

void BurstCapture::setProgressCallback(BurstProgressCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    progressCallback_ = callback;
}

void BurstCapture::setImageCallback(BurstImageCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    imageCallback_ = callback;
}

bool BurstCapture::startBurst(const BurstConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_.load() != BurstState::IDLE) {
        OH_LOG_WARN(LOG_APP, "Burst already in progress, state: %{public}d",
                    static_cast<int>(state_.load()));
        return false;
    }

    config_ = config;
    captureCount_.store(0);
    processCount_.store(0);

    // 重置图像处理器
    processor_->reset();

    // 设置处理器进度回调
    processor_->setProgressCallback([this](int32_t current, int32_t total,
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

    // 更新状态
    state_.store(BurstState::CAPTURING);
    progress_.state = BurstState::CAPTURING;
    progress_.totalFrames = config.frameCount;
    progress_.capturedFrames = 0;
    progress_.processedFrames = 0;
    progress_.message = "Starting burst capture";
    notifyProgress();

    OH_LOG_INFO(LOG_APP, "Burst started: %{public}d frames, exposure: %{public}dms",
                config.frameCount, config.exposureMs);

    // 启动拍照循环(在独立线程中)
    std::thread captureThread([this]() {
        startCaptureLoop();
    });
    captureThread.detach();

    return true;
}

void BurstCapture::cancelBurst() {
    BurstState expected = BurstState::CAPTURING;
    if (!state_.compare_exchange_strong(expected, BurstState::CANCELLED)) {
        expected = BurstState::PROCESSING;
        if (!state_.compare_exchange_strong(expected, BurstState::CANCELLED)) {
            return;
        }
    }

    progress_.state = BurstState::CANCELLED;
    progress_.message = "Burst cancelled";
    notifyProgress();

    // 清空队列
    taskQueue_->clear();

    OH_LOG_INFO(LOG_APP, "Burst cancelled");
}

bool BurstCapture::isBurstActive() const {
    BurstState s = state_.load();
    return s == BurstState::CAPTURING || s == BurstState::PROCESSING;
}

void BurstCapture::onPhotoCaptured(void* buffer, size_t size, int32_t width, int32_t height) {
    if (state_.load() == BurstState::CANCELLED) {
        OH_LOG_INFO(LOG_APP, "Burst cancelled, ignoring captured photo");
        return;
    }

    if (state_.load() != BurstState::CAPTURING) {
        OH_LOG_WARN(LOG_APP, "Not in CAPTURING state, ignoring captured photo");
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

void BurstCapture::startCaptureLoop() {
    OH_LOG_INFO(LOG_APP, "Capture loop started, will capture %{public}d frames",
                config_.frameCount);

    for (int32_t i = 0; i < config_.frameCount; i++) {
        if (state_.load() == BurstState::CANCELLED) {
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
    if (state_.load() == BurstState::CAPTURING) {
        state_.store(BurstState::PROCESSING);
        progress_.state = BurstState::PROCESSING;
        progress_.message = "All frames captured, processing stacked image";
        notifyProgress();
        OH_LOG_INFO(LOG_APP, "Capture completed, switched to PROCESSING state");
    }

    OH_LOG_INFO(LOG_APP, "Capture loop ended");
}

void BurstCapture::captureNextFrame() {
    Camera_ErrorCode err = ExpoCamera::getInstance().takePhoto();
    if (err != CAMERA_OK) {
        OH_LOG_ERROR(LOG_APP, "Failed to capture photo: %{public}d", err);
    } else {
        OH_LOG_INFO(LOG_APP, "Photo capture triggered");
    }
}

void BurstCapture::processTask(ImageTask&& task) {
    if (state_.load() == BurstState::CANCELLED) {
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
            state_.store(BurstState::ERROR);
            progress_.state = BurstState::ERROR;
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

            state_.store(BurstState::COMPLETED);
            progress_.state = BurstState::COMPLETED;
            progress_.message = "Burst capture completed successfully";
        } else {
            state_.store(BurstState::ERROR);
            progress_.state = BurstState::ERROR;
            progress_.message = "Failed to finalize processing result";
        }
        notifyProgress();
    }
}

void BurstCapture::notifyProgress() {
    if (progressCallback_) {
        progressCallback_(progress_);
    }
}

void BurstCapture::notifyImage(void* buffer, size_t size, bool isFinal) {
    if (imageCallback_) {
        imageCallback_(buffer, size, isFinal);
    }
}

} // namespace exposhot
