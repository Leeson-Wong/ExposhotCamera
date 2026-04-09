#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <thread>
#include <cstdint>
#include "owned_buffer.h"

namespace exposhot {

// 任务数据结构
struct ImageTask {
    int32_t taskId;           // 任务 ID（第几张, 0-based)
    OwnedBuffer buffer;       // 图像数据(独占所有权，移动语义)
    uint32_t width;           // 图像宽度
    uint32_t height;          // 图像高度
    bool isFirst;             // 是否是第一张(基准帧)

    ImageTask() : taskId(0), width(0), height(0), isFirst(false) {}

    // 移动构造
    ImageTask(ImageTask&& other) noexcept
        : taskId(other.taskId), buffer(std::move(other.buffer)),
          width(other.width), height(other.height), isFirst(other.isFirst) {
        other.width = 0;
        other.height = 0;
    }

    // 移动赋值
    ImageTask& operator=(ImageTask&& other) noexcept {
        if (this != &other) {
            taskId = other.taskId;
            buffer = std::move(other.buffer);
            width = other.width;
            height = other.height;
            isFirst = other.isFirst;
            other.width = 0;
            other.height = 0;
        }
        return *this;
    }

    // 析构时 OwnedBuffer 自动释放
    ~ImageTask() = default;

    // 禁止拷贝
    ImageTask(const ImageTask&) = delete;
    ImageTask& operator=(const ImageTask&) = delete;
};

// 线程安全任务队列
class TaskQueue {
public:
    using TaskHandler = std::function<void(ImageTask&&)>;

    TaskQueue();
    ~TaskQueue();

    // 入队(拍照线程调用)
    void enqueue(ImageTask&& task);

    // 启动消费线程
    void start(TaskHandler handler);

    // 停止消费线程
    void stop();

    // 清空队列
    void clear();

    // 获取当前队列大小
    size_t size() const;

    // 检查是否正在运行
    bool isRunning() const { return running_.load(); }

private:
    void consumerLoop();

    std::queue<ImageTask> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread consumerThread_;
    TaskHandler handler_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{false};
};

} // namespace exposhot

#endif // TASK_QUEUE_H
