# SPSC 无锁队列改造方案

## 背景当前 `TaskQueue` 和 `CaptureManager` 使用 `std::mutex` 保护所有共享状态。  
拍摄流程保证"同一时间只有一个拍摄任务"，锁的开销和保护可以省掉。

---

## 1. 设计决策

### 1.1 CaptureManager — 完全无锁

| 竞争场景 | 当前 | 改造后 |
|---------|------|--------|
| 入口互斥 | `mutex_` | `state_` 原子 CAS |
| progress 字段 | `mutex_` 保护整个 struct | 拆成独立 `std::atomic<int32_t>` |
| callback 引用 | `mutex_` 保护 | init 时设置一次，之后只读 |

```
入口互斥示例：

  CaptureState expected = IDLE;
  if (!state_.compare_exchange_strong(expected, SINGLE_CAPTURING))
      return -EBUSY;  // 有任务在跑，直接返回
```

### 1.2 TaskQueue — SPSC 环形缓冲区 + POSIX 信号量

- **单生产者**：相机回调线程（enqueue）
- **单消费者**：处理线程（dequeue）
- **C++17 约束**：不使用 C++20 `atomic_wait`，使用 POSIX `sem_t`

```cpp
template<typename T, size_t Capacity = 32>
class SPSCQueue {
    T slots_[Capacity];
    std::atomic<uint32_t> head_{0};  // 消费者写
    std::atomic<uint32_t> tail_{0};  // 生产者写
    sem_t sem_;
    std::atomic<bool> stopped_{false};

    bool enqueue(T&& item);   // 满了返回 false（不阻塞生产者）
    bool dequeue(T& out);     // 空了阻塞等 sem_wait
    void start(handler);
    void stop();
    void clear();
};
```

### 1.3 队列满策略

**拒绝入队，返回 false**

- 生产者（相机回调）丢弃该帧
- 不阻塞相机回调线程
- CAPACITY=32 对连拍场景足够（消费速度应大于捕获速度）

---

## 2. 数据流

```
主线程 (入口)                 相机回调线程 (生产者)          消费者线程
    │                              │                           │
    CAS(IDLE→CAPTURING)            │                           │
    ├─ 失败 → return -EBUSY        │                           │
    ├─ 成功 → 触发拍照             │                           │
    │                              │                           │
    │                     onPhotoAvailable()                   │
    │                     复制 buffer → enqueue(task)          │
    │                     tail_.store(t+1, release)            │
    │                     sem_post(&sem_) ──────────────────▶  │
    │                              │                    sem_wait 返回
    │                              │                    读 slots_[h % CAP]
    │                              │                    head_.store(h+1, release)
    │                              │                    handler_(task)
    │                              │                           │
    │                              │                    全部处理完
    │                              │                    state_→IDLE
```

---

## 3. 实施步骤

### 第一步：PC 端验证

目录结构：

```
tests/
├── CMakeLists.txt                # Google Test 集成
├── task_queue_spsc.h             # SPSC 环形缓冲区（无 HarmonyOS 依赖）
├── task_queue_spsc.cpp
└── task_queue_spsc_test.cpp      # 测试用例
```

测试用例清单：

| # | 测试名 | 验证内容 |
|---|--------|---------|
| 1 | BasicEnqueueDequeue | 入队 N 个任务，全部消费 |
| 2 | FIFOOrder | 保证先进先出顺序 |
| 3 | BurstEnqueue | 短时间入队 30 帧，不丢数据 |
| 4 | RejectWhenFull | 队列满时返回 false |
| 5 | ConsumerWaitsOnEmpty | 空队列时消费者阻塞，不 CPU 空转 |
| 6 | StopMidConsume | 消费过程中 stop，不死锁不崩溃 |
| 7 | StopWhenEmpty | 队列空时 stop，消费者线程正常退出 |
| 8 | MultipleStartStop | 多轮启停，状态干净无泄漏 |
| 9 | OwnershipMove | ImageTask 移动语义正确，无 double-free |
| 10 | StressTest | 10000 次快速入队出队，持续运行不崩溃 |

验证指标：

- 正确性：所有测试通过，零数据丢失
- 性能：enqueue/dequeue 单次 < 100ns
- 无死锁：stop() 在任何状态下 < 10ms 终止消费者线程

### 第二步：移植到 HarmonyOS

1. 用 `task_queue_spsc.h/cpp` 替换现有 `task_queue.h/cpp`
2. 改造 `capture_manager.h/cpp` 去掉 `mutex_`
3. 适配 `napi_expo_camera.cpp` 回调注册

---

## 4. 改造文件清单

| 文件 | 改动 |
|------|------|
| `camera/task_queue.h` | 替换为 SPSC 环形缓冲区接口 |
| `camera/task_queue.cpp` | 替换实现：atomic head/tail + sem_t |
| `camera/capture_manager.h` | 去掉 `mutex_`，progress 拆 atomic，callback 改 init-only |
| `camera/capture_manager.cpp` | 入口 CAS 互斥，去掉所有 lock_guard |
| `napi_expo_camera.cpp` | 适配 enqueue 返回值（满队列时处理） |

---

## 5. 约束

- C++17 标准（不用 C++20 `atomic_wait`、`std::jthread` 等）
- HarmonyOS NDK 支持 POSIX `sem_t`
- 单生产者单消费者模型，不支持多生产者

---

## 版本

| 版本 | 日期 | 说明 |
|------|------|------|
| 1.0.0 | 2026-04-03 | 初始方案文档 |
