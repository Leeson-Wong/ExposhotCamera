#include "task_queue.h"
#include "hilog/log.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "TaskQueue"

namespace exposhot {

TaskQueue::TaskQueue() {}

TaskQueue::~TaskQueue() {
    stop();
}

void TaskQueue::enqueue(ImageTask&& task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(task));
    }
    cv_.notify_one();
    OH_LOG_INFO(LOG_APP, "Task enqueued: id=%{public}d, size=%{public}zu, queue size: %{public}zu",
                  task.taskId, task.size, queue_.size());
}

void TaskQueue::start(TaskHandler handler) {
    if (running_.load()) {
        OH_LOG_WARN(LOG_APP, "TaskQueue already running");
        return;
    }

    handler_ = handler;
    stopped_.store(false);
    running_.store(true);
    consumerThread_ = std::thread(&TaskQueue::consumerLoop, this);

    OH_LOG_INFO(LOG_APP, "TaskQueue started");
}

void TaskQueue::stop() {
    stopped_.store(true);
    running_.store(false);
    cv_.notify_all();

    if (consumerThread_.joinable()) {
        consumerThread_.join();
    }

    OH_LOG_INFO(LOG_APP, "TaskQueue stopped");
}

void TaskQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
        queue_.pop();  // ImageTask 析构会自动释放 buffer
    }
    OH_LOG_INFO(LOG_APP, "TaskQueue cleared");
}

size_t TaskQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void TaskQueue::consumerLoop() {
    OH_LOG_INFO(LOG_APP, "Consumer loop started");

    while (!stopped_.load()) {
        ImageTask task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !queue_.empty() || stopped_.load();
            });

            if (stopped_.load() && queue_.empty()) {
                break;
            }

            if (!queue_.empty()) {
                task = std::move(queue_.front());
                queue_.pop();
            }
        }

        if (task.buffer && handler_) {
            OH_LOG_INFO(LOG_APP, "Processing task: id=%{public}d", task.taskId);
            handler_(std::move(task));
        }
    }

    OH_LOG_INFO(LOG_APP, "Consumer loop ended");
}

} // namespace exposhot
