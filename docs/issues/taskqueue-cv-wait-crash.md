# TaskQueue cv_.wait() 崩溃问题

## 问题描述

在 `task_queue.cpp` 的 `consumerLoop()` 函数中，`cv_.wait(lock, [this] {...})` 这一行偶现崩溃，不稳定复现。

## 现象

- 拍摄过程中偶现崩溃
- 崩溃位置固定在 `cv_.wait()` 调用处
- 问题不稳定复现，可能与拍摄次数、内存状态、时序有关

## 崩溃堆栈

```
#01 pc 00000000001cf078 /system/lib/ld-musl-aarch64.so.1(pthread_cond_timedwait+172)
#02 pc 00000000000c4424 /data/storage/el1/bundle/libs/arm64/libc++_shared.so(std::__n1::condition_variable::wait(std::__n1::unique_lock<std::__n1::mutex>&)+20)
#03 pc 00000000000c68d4 /data/storage/el1/bundle/libs/arm64/libexpocamera.so
#04 pc 00000000000c5dcc /data/storage/el1/bundle/libs/arm64/libexpocamera.so(exposhot::TaskQueue::consumerLoop()+436)
#05 pc 00000000000ca8cc /data/storage/el1/bundle/libs/arm64/libexpocamera.so
#06 pc 00000000000ca84c /data/storage/el1/bundle/libs/arm64/libexpocamera.so
```

## 崩溃位置

```cpp
// task_queue.cpp:160
cv_.wait(lock, [this] {
    bool shouldWake = !queue_.empty() || stopped_.load();
    TQ_LOG_DEBUG("[CV_PREDICATE] queueEmpty=%{public}d, stopped=%{public}d, shouldWake=%{public}d",
                 queue_.empty(), stopped_.load(), shouldWake);
    return shouldWake;
});
```

## 可能原因

1. **对象生命周期问题**
   - `TaskQueue` 对象在消费者线程还在运行时被销毁
   - `this` 指针在 lambda 谓词中失效

2. **内存损坏**
   - `mutex_` 或 `cv_` 的内存被其他代码踩坏

3. **HarmonyOS libc++ 兼容性问题**
   - musl libc 与 libc++ 的 condition_variable 实现可能存在兼容问题
   - 涉及 `pthread_cond_timedwait` 底层实现

4. **竞态条件**
   - `stop()` 与 `cv_.wait()` 之间的时序问题
   - `notify_all()` 与 `wait()` 的同步问题

## 已尝试的修复

1. **回调锁外调用** - 在 `onBurstPhotoCaptured`、`onSinglePhotoCaptured`、`onPhotoError` 中不再持有锁时调用回调，避免死锁

2. **异常处理** - `consumerLoop` 中添加 try-catch 异常处理
   ```cpp
   try {
       handler_(std::move(task));
   } catch (const std::exception& e) {
       TQ_LOG_ERROR("[PROCESS_EXCEPTION] taskId=%{public}d, exception: %{public}s",
                   taskId, e.what());
   } catch (...) {
       TQ_LOG_ERROR("[PROCESS_EXCEPTION] taskId=%{public}d, unknown exception", taskId);
   }
   ```

3. **stop() 同步改进** - 在 `notify_all()` 前获取锁确保同步
   ```cpp
   {
       std::lock_guard<std::mutex> lock(mutex_);
   }
   cv_.notify_all();
   ```

## 调试方法

```bash
# 查看相关日志
hilog | grep -E "TQ_|CM_"

# 只看错误日志
hilog | grep -E "\[TQ_ERROR\]|\[CM_ERROR\]"

# 保存日志到文件
hilog > /data/local/tmp/crash_log.txt
```

## 待确认事项

- [ ] 收集崩溃前的完整日志，确认崩溃前状态
- [ ] 确认崩溃是否与特定操作序列相关（如连续拍摄、快速取消等）
- [ ] 测试使用 `cv_.wait_for()` 替代 `cv_.wait()` 是否更稳定
- [ ] 检查是否有其他代码可能导致内存损坏
- [ ] 考虑使用 `std::shared_ptr` + `std::enable_shared_from_this` 管理生命周期

## 相关代码

- `entry/src/main/cpp/camera/task_queue.cpp` - `consumerLoop()`, `stop()`
- `entry/src/main/cpp/camera/task_queue.h` - TaskQueue 类定义
- `entry/src/main/cpp/camera/capture_manager.cpp` - 任务处理相关

## 历史记录

- 2024-03-23: 初始记录，已尝试多项修复但仍偶现
