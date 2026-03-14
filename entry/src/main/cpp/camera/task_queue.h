#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <thread>
#include <cstdint>

namespace exposhot {

// 任务数据结构
struct ImageTask {
    int32_t taskId;           // 任务 ID（第几张, 0-based)
    void* buffer;             // 图像数据(所有权转移)
    size_t size;              // 数据大小
    int32_t width;            // 图像宽度
    int32_t height;           // 图像高度
    bool isFirst;             // 是否是第一张(基准帧)

    ImageTask() : taskId(0), buffer(nullptr), size(0), width(0), height(0), isFirst(false) {}

    // 移动构造
    ImageTask(ImageTask&& other) noexcept
        : taskId(other.taskId), buffer(other.buffer),
          size(other.size), width(other.width), height(other.height), isFirst(other.isFirst) {
        other.buffer = nullptr;
        other.size = 0;
        other.width = 0;
        other.height = 0;
    }

    // 移动赋值
    ImageTask& operator=(ImageTask&& other) noexcept {
        if (this != &other) {
            // 释放现有资源
            if (buffer) {
                free(buffer);
            }
            // 移动数据
            taskId = other.taskId;
            buffer = other.buffer;
            size = other.size;
            width = other.width;
            height = other.height;
            isFirst = other.isFirst;
            // 置空源对象
            other.buffer = nullptr;
            other.size = 0;
            other.width = 0;
            other.height = 0;
        }
        return *this;
    }

    // 析构时释放内存
    ~ImageTask() {
        if (buffer) {
            free(buffer);
            buffer = nullptr;
        }
    }

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
