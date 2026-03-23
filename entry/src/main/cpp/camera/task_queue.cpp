#include "task_queue.h"
#include "hilog/log.h"
#include <thread>
#include <sstream>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "TaskQueue"

// 多线程调试日志标识符，使用 "TQ_" 前缀便于过滤
// 过滤命令示例: hilog | grep "TQ_"
#define TQ_LOG_DEBUG(fmt, ...) OH_LOG_DEBUG(LOG_APP, "[TQ_DEBUG] " fmt, ##__VA_ARGS__)
#define TQ_LOG_INFO(fmt, ...)  OH_LOG_INFO(LOG_APP, "[TQ_INFO] " fmt, ##__VA_ARGS__)
#define TQ_LOG_WARN(fmt, ...)  OH_LOG_WARN(LOG_APP, "[TQ_WARN] " fmt, ##__VA_ARGS__)
#define TQ_LOG_ERROR(fmt, ...) OH_LOG_ERROR(LOG_APP, "[TQ_ERROR] " fmt, ##__VA_ARGS__)

// 获取当前线程 ID 的简短标识
static std::string getThreadId() {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
}

namespace exposhot {

TaskQueue::TaskQueue() {
    TQ_LOG_INFO("Constructor called, thread=%{public}s", getThreadId().c_str());
}

TaskQueue::~TaskQueue() {
    TQ_LOG_INFO("Destructor called, thread=%{public}s, running=%{public}d",
                getThreadId().c_str(), running_.load());
    stop();
}

void TaskQueue::enqueue(ImageTask&& task) {
    std::string producerThread = getThreadId();
    int32_t taskId = task.taskId;
    size_t taskSize = task.size;

    TQ_LOG_DEBUG("[ENQUEUE_START] taskId=%{public}d, size=%{public}zu, producerThread=%{public}s",
                 taskId, taskSize, producerThread.c_str());

    {
        TQ_LOG_DEBUG("[ENQUEUE_LOCK_WAIT] taskId=%{public}d, waiting for mutex", taskId);
        std::lock_guard<std::mutex> lock(mutex_);
        TQ_LOG_DEBUG("[ENQUEUE_LOCK_ACQUIRED] taskId=%{public}d, mutex acquired, queueSize=%{public}zu",
                     taskId, queue_.size());

        queue_.push(std::move(task));
        size_t newSize = queue_.size();

        TQ_LOG_INFO("[ENQUEUE_DONE] taskId=%{public}d, queueSize=%{public}zu, producerThread=%{public}s",
                    taskId, newSize, producerThread.c_str());
    }

    TQ_LOG_DEBUG("[ENQUEUE_NOTIFY] taskId=%{public}d, notifying consumer", taskId);
    cv_.notify_one();
    TQ_LOG_DEBUG("[ENQUEUE_END] taskId=%{public}d", taskId);
}

void TaskQueue::start(TaskHandler handler) {
    std::string callerThread = getThreadId();

    TQ_LOG_INFO("[START_BEGIN] callerThread=%{public}s, running=%{public}d",
                callerThread.c_str(), running_.load());

    if (running_.load()) {
        TQ_LOG_WARN("[START_FAILED] TaskQueue already running, callerThread=%{public}s",
                    callerThread.c_str());
        return;
    }

    handler_ = handler;
    stopped_.store(false);
    running_.store(true);

    TQ_LOG_DEBUG("[START_THREAD] creating consumer thread");
    consumerThread_ = std::thread(&TaskQueue::consumerLoop, this);
    std::string consumerThreadId;
    {
        std::ostringstream oss;
        oss << consumerThread_.get_id();
        consumerThreadId = oss.str();
    }

    TQ_LOG_INFO("[START_DONE] consumerThreadId=%{public}s, callerThread=%{public}s",
                consumerThreadId.c_str(), callerThread.c_str());
}

void TaskQueue::stop() {
    std::string callerThread = getThreadId();

    TQ_LOG_INFO("[STOP_BEGIN] callerThread=%{public}s, running=%{public}d, stopped=%{public}d",
                callerThread.c_str(), running_.load(), stopped_.load());

    // 先设置停止标志
    stopped_.store(true);
    running_.store(false);

    // 在持有锁的情况下通知，确保消费者线程不会错过信号
    {
        std::lock_guard<std::mutex> lock(mutex_);
        TQ_LOG_DEBUG("[STOP_NOTIFY] notifying all waiting threads");
    }
    cv_.notify_all();

    if (consumerThread_.joinable()) {
        TQ_LOG_DEBUG("[STOP_JOIN] waiting for consumer thread to finish");
        consumerThread_.join();
        TQ_LOG_DEBUG("[STOP_JOINED] consumer thread finished");
    }

    TQ_LOG_INFO("[STOP_DONE] callerThread=%{public}s", callerThread.c_str());
}

void TaskQueue::clear() {
    std::string callerThread = getThreadId();

    TQ_LOG_DEBUG("[CLEAR_BEGIN] callerThread=%{public}s", callerThread.c_str());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t originalSize = queue_.size();
        TQ_LOG_INFO("[CLEAR_LOCKED] originalQueueSize=%{public}zu, callerThread=%{public}s",
                    originalSize, callerThread.c_str());

        while (!queue_.empty()) {
            queue_.pop();  // ImageTask 析构会自动释放 buffer
        }

        TQ_LOG_INFO("[CLEAR_DONE] cleared %{public}zu tasks", originalSize);
    }
}

size_t TaskQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t s = queue_.size();
    TQ_LOG_DEBUG("[SIZE] queueSize=%{public}zu", s);
    return s;
}

void TaskQueue::consumerLoop() {
    std::string consumerThread = getThreadId();
    TQ_LOG_INFO("[CONSUMER_START] consumerThread=%{public}s", consumerThread.c_str());

    int64_t loopCount = 0;
    int64_t processedCount = 0;

    while (!stopped_.load()) {
        loopCount++;
        TQ_LOG_DEBUG("[LOOP_ITERATION] loopCount=%{public}lld, consumerThread=%{public}s",
                     (long long)loopCount, consumerThread.c_str());

        ImageTask task;
        {
            TQ_LOG_DEBUG("[WAIT_LOCK] loopCount=%{public}lld, waiting for mutex", loopCount);
            std::unique_lock<std::mutex> lock(mutex_);
            TQ_LOG_DEBUG("[LOCK_ACQUIRED] loopCount=%{public}lld, queueSize=%{public}zu, stopped=%{public}d",
                         loopCount, queue_.size(), stopped_.load());

            // 等待条件: 队列不为空 或 收到停止信号
            TQ_LOG_DEBUG("[CV_WAIT_BEGIN] loopCount=%{public}lld, waiting for condition", loopCount);
            cv_.wait(lock, [this] {
                bool shouldWake = !queue_.empty() || stopped_.load();
                TQ_LOG_DEBUG("[CV_PREDICATE] queueEmpty=%{public}d, stopped=%{public}d, shouldWake=%{public}d",
                             queue_.empty(), stopped_.load(), shouldWake);
                return shouldWake;
            });
            TQ_LOG_DEBUG("[CV_WAIT_END] loopCount=%{public}lld, woken up, queueSize=%{public}zu, stopped=%{public}d",
                         loopCount, queue_.size(), stopped_.load());

            // 检查是否应该退出
            if (stopped_.load() && queue_.empty()) {
                TQ_LOG_INFO("[CONSUMER_EXIT] stopped=true, queue empty, consumerThread=%{public}s, loopCount=%{public}lld",
                            consumerThread.c_str(), (long long)loopCount);
                break;
            }

            // 取出任务
            if (!queue_.empty()) {
                task = std::move(queue_.front());
                queue_.pop();
                TQ_LOG_DEBUG("[DEQUEUE] taskId=%{public}d, remainingQueueSize=%{public}zu",
                             task.taskId, queue_.size());
            } else {
                TQ_LOG_WARN("[SPURIOUS_WAKEUP] queue is empty after wakeup, loopCount=%{public}lld", loopCount);
            }
        }  // lock released here
        TQ_LOG_DEBUG("[LOCK_RELEASED] loopCount=%{public}lld, taskId=%{public}d", loopCount, task.taskId);

        // 处理任务 (在锁外执行，避免阻塞生产者)
        if (task.buffer && handler_) {
            processedCount++;
            int32_t taskId = task.taskId;
            TQ_LOG_INFO("[PROCESS_BEGIN] taskId=%{public}d, processedCount=%{public}lld, consumerThread=%{public}s",
                        taskId, (long long)processedCount, consumerThread.c_str());

            try {
                handler_(std::move(task));
            } catch (const std::exception& e) {
                TQ_LOG_ERROR("[PROCESS_EXCEPTION] taskId=%{public}d, exception: %{public}s",
                            taskId, e.what());
            } catch (...) {
                TQ_LOG_ERROR("[PROCESS_EXCEPTION] taskId=%{public}d, unknown exception", taskId);
            }

            TQ_LOG_INFO("[PROCESS_END] taskId=%{public}d, consumerThread=%{public}s",
                        taskId, consumerThread.c_str());
        } else if (task.buffer == nullptr) {
            TQ_LOG_WARN("[SKIP_NULL_BUFFER] taskId=%{public}d, buffer is null", task.taskId);
        } else {
            TQ_LOG_WARN("[SKIP_NO_HANDLER] taskId=%{public}d, handler is null", task.taskId);
        }
    }

    TQ_LOG_INFO("[CONSUMER_END] consumerThread=%{public}s, totalLoops=%{public}lld, totalProcessed=%{public}lld",
                consumerThread.c_str(), (long long)loopCount, (long long)processedCount);
}

} // namespace exposhot
