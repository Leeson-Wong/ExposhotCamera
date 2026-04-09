# ExposhotCamera 多线程模型重构方案

> 状态: 草案
> 日期: 2026-04-09
> 背景: 连续多次修复 ArrayBuffer GC 生命周期和非 JS 线程 N-API 调用问题无法稳定，根因是线程模型设计缺陷

---

## 1. 根因诊断

### 1.1 当前线程地图

当前有 **4 条线程**无归属地操作共享单例对象：

| 线程 | 身份 | 操作 | 问题 |
|------|------|------|------|
| JS 主线程 | NAPI 调用方 | `init`/`release`/`switchSurface`/`startPreview`、所有参数设置 | 直接操作 ExpoCamera，无锁保护 |
| 相机回调线程 | 系统管理，不可控 | `onPhotoAvailable`、`onError` | 读写 `photoCapturedCallback_`（裸 `std::function`）无锁 |
| detached 线程 | `std::thread` + `.detach()` | `startCaptureLoop` 循环调用 `OH_PhotoOutput_Capture` | detach 后不可追踪，`release` 时可能访问已释放对象 |
| TaskQueue 消费线程 | `std::thread` 由 TaskQueue 管理 | `processTask` 处理图像 | 读 `config_`、`currentSessionId_` 等无保护 |

### 1.2 具体缺陷清单

**缺陷 1: ExpoCamera mutex_ 全部被注释掉**

`expo_camera.cpp` 中 `init`、`release`、`switchSurface`、`startPreview`、`stopPreview`、`setZoomRatio`、`getZoomRatio`、`setFocusMode`、`getFocusMode`、`setFocusPoint`、`getFocusPoint` 等所有方法的 `std::lock_guard<std::mutex> lock(mutex_)` 均被注释。`init()` 和 `release()` 可与 `onPhotoAvailable`（相机系统线程）并发，导致 `photoOutput_`、`captureSession_` 等的 use-after-free。

**缺陷 2: `onPhotoAvailable` 无锁访问回调**

`expo_camera.cpp:506-614` 中，静态方法通过 `getInstance()` 获取单例，直接读取 `photoCapturedCallback_`（`std::function`）。若 JS 线程在 `release()` 中调用 `setPhotoCapturedCallback(nullptr)` 清除回调，对 `std::function` 对象本身构成数据竞争。

**缺陷 3: detached 线程不可控**

`capture_manager.cpp:405-408`:
```cpp
std::thread captureThread([this]() { startCaptureLoop(); });
captureThread.detach();
```
线程 detach 后无法 join。`release()` 被调用时，该线程可能仍在运行，lambda 捕获的 `this` 可能指向已部分析构的对象。此外，`captureNextFrame()` 在此线程上调用 `OH_PhotoOutput_Capture`（相机 API），可能违反相机子系统的线程亲和性要求。

**缺陷 4: Buffer 四次复制**

Buffer 在管道中经历的复制路径：
```
onPhotoAvailable:        malloc + memcpy (#1) → 传递给回调
onBurstPhotoCaptured:    malloc + memcpy (#2), free(#1) → 入队
onBurstImageCallback:    malloc + memcpy (#3) → 投递 tsfn
BurstImageCallJs:        memcpy into ArrayBuffer (#4), free(#3)
```
每张照片经历 4 次 malloc/memcpy 和 4 次 free。每个环节的错误处理或取消都可能导致内存泄漏或 double-free。

**缺陷 5: `processTask` 无锁读取共享状态**

`capture_manager.cpp:591-708` 中，`processTask` 在 TaskQueue 消费线程上读取 `config_.frameCount`、`currentSessionId_`、`config_.realtimePreview`、`imageCallback_` 等，均未持有 `mutex_`。当前因时序碰巧安全，但任何未来的修改都可能打破这一假设。

### 1.3 为什么补丁无法修复

这些问题的本质是**没有线程归属模型**。每个线程都可以随时读写任何共享状态，加锁只是推迟问题——在 `onPhotoAvailable`（系统回调线程）中加锁可能阻塞系统线程导致死锁，不加锁则有数据竞争。唯一的出路是重新设计线程之间的职责划分。

---

## 2. 新线程模型

### 2.1 架构图

```
                    JS 主线程
                    (NAPI 调用, tsfn 回调投递)
                         │
                         │ NAPI 调用 (只做参数解析)
                         ▼
               ┌───────────────────┐
               │   CaptureManager  │  ← 唯一外部入口，协调者
               │  (atomic state)   │
               └────────┬──────────┘
                        │
           ┌────────────┴────────────┐
           │                         │
           ▼                         ▼
  ┌──────────────────┐    ┌──────────────────┐
  │ CameraCommand    │    │ ProcessWorker    │
  │ Thread (新增)    │    │ (现有 TaskQueue) │
  │                  │    │                  │
  │ 独占:            │    │ 独占:            │
  │ - ExpoCamera     │    │ - ImageProcessor │
  │ - 所有           │    │ - 出队 + 处理    │
  │   OH_Camera_*    │    │                  │
  │   API 调用       │    │ 只读 (atomic):   │
  │                  │    │ - config_        │
  │ 接收:            │    │ - state_         │
  │ - 命令消息       │    └──────────────────┘
  │ - 照片 buffer    │
  │                  │
  │ 发送:            │
  │ - buffer 到      │
  │   ProcessWorker  │
  └──────────────────┘
```

### 2.2 设计原则

1. **线程归属**: 每块可变状态有且仅有一个归属线程，其他线程只通过消息传递访问
2. **相机硬件亲和性**: 所有 `OH_Camera_*` 和 `OH_PhotoOutput_*` API 调用收拢到 CameraCommand 线程
3. **Buffer 所有权转移**: 使用 `std::unique_ptr` 或 move-only 语义，生产者转交后不再持有引用
4. **不使用 detached 线程**: 所有线程都是 joinable 的，生命周期由所属对象管理

### 2.3 资源归属表

| 资源 | 归属线程 | 访问模式 |
|------|---------|---------|
| `ExpoCamera`（全部字段） | CameraCommand 线程 | JS 线程投递命令；CameraCommand 执行 |
| `CaptureManager::state_` | atomic CAS | 所有线程只读 atomic |
| `CaptureManager::config_`, `currentSessionId_` | 写一次读多次 | `startBurst` 中写入后不再修改 |
| `CaptureManager` 回调 | JS 线程 init 时设置 | 写一次读多次，无需锁 |
| `TaskQueue` | ProcessWorker 拥有消费端 | CameraCommand 线程入队 |
| `ImageProcessor` | ProcessWorker | 单线程访问，无竞争 |
| NAPI tsfn 句柄 | JS 线程创建/销毁 | 其他线程通过 tsfn 调用（安全） |

---

## 3. 分阶段实施计划

### Phase 1: 快速止血（低风险）

> 目标：消除最危险的崩溃源，不改变整体架构。

#### Step 1.1: 消除 detached 线程

**文件**: `capture_manager.h`, `capture_manager.cpp`

将 `startBurst` 中的 detached 线程改为可 join 的成员线程：

```cpp
// capture_manager.h 新增:
std::thread captureLoopThread_;
std::atomic<bool> captureLoopRunning_{false};

// startBurst 中:
captureLoopRunning_.store(true);
captureLoopThread_ = std::thread([this]() {
    startCaptureLoop();
    captureLoopRunning_.store(false);
});

// release 中新增:
captureLoopRunning_.store(false);
if (captureLoopThread_.joinable()) {
    captureLoopThread_.join();
}
```

**风险**: 低。行为不变，只是使线程可追踪、可等待。

#### Step 1.2: 回调安全防护

**文件**: `expo_camera.h`, `expo_camera.cpp`

用 `std::atomic<bool>` 标记回调是否有效，防止 `onPhotoAvailable` 在回调被清除后调用：

```cpp
// expo_camera.h 新增:
std::atomic<bool> photoCallbackValid_{false};
std::atomic<bool> photoErrorCallbackValid_{false};

// setPhotoCapturedCallback 中:
photoCallbackValid_.store(callback != nullptr, std::memory_order_release);
photoCapturedCallback_ = callback;

// onPhotoAvailable 中:
if (!self.photoCallbackValid_.load(std::memory_order_acquire)) {
    free(bufferCopy);
    return;  // 回调未注册或已清除
}
self.photoCapturedCallback_(bufferCopy, nativeBufferSize, imgWidth, imgHeight);
```

**风险**: 低。非侵入式，为最热的数据竞争路径增加安全检查。

#### Step 1.3: 恢复只读方法的锁

**文件**: `expo_camera.cpp`

取消注释以下方法的 `std::lock_guard`:
- `getZoomRatio`、`getZoomRatioRange`
- `getFocusMode`、`isFocusModeSupported`
- `getFocusPoint`、`setFocusPoint`
- `setZoomRatio`、`setFocusMode`
- `isZoomSupported`

**不恢复** `init`/`release`/`startPreview`/`switchSurface` 的锁（回调链较深，可能死锁），留给 Phase 2。

**风险**: 低。这些方法只操作相机参数，不涉及回调。

---

### Phase 2: CameraCommandQueue（中等风险）

> 目标：所有相机硬件 API 串行化，从根本上消除跨线程竞态。

#### Step 2.1: 实现 CameraCommandQueue

**新增文件**: `camera/camera_command_queue.h`, `camera/camera_command_queue.cpp`

MPSC（多生产者单消费者）命令队列：

```cpp
struct CameraCommand {
    enum Type {
        INIT, RELEASE,
        START_PREVIEW, STOP_PREVIEW,
        SWITCH_SURFACE, SWITCH_CAPTURE_MODE,
        CAPTURE_SINGLE, CAPTURE_BURST_FRAME,
        SET_ZOOM_RATIO, SET_FOCUS_MODE, SET_FOCUS_POINT,
        PHOTO_AVAILABLE   // 来自相机回调线程
    };

    Type type;
    std::string stringParam;
    int32_t intParam = 0;
    float floatParam1 = 0.0f, floatParam2 = 0.0f;
    CaptureMode captureMode = CaptureMode::SINGLE;

    // PHOTO_AVAILABLE 专用
    void* photoBuffer = nullptr;
    size_t photoBufferSize = 0;
    uint32_t photoWidth = 0, photoHeight = 0;

    // 完成回调（同步模式：通过 promise/future 阻塞等待）
    std::promise<int32_t> resultPromise;
};

class CameraCommandQueue {
public:
    void start();
    void stop();
    // 同步投递：阻塞等待结果
    int32_t postSync(CameraCommand&& cmd);
    // 异步投递：不等结果
    void postAsync(CameraCommand&& cmd);
private:
    void commandLoop();
    std::queue<CameraCommand> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};
```

**关键设计**: 提供 `postSync`（阻塞等待完成，用于 init/release/capture 等需要返回值的操作）和 `postAsync`（不等待，用于 PHOTO_AVAILABLE 等事件通知）。

#### Step 2.2: 路由相机硬件调用

**文件**: `capture_manager.cpp`, `expo_camera.cpp`

将以下操作改为投递命令：

| 原直接调用 | 改为投递命令 |
|-----------|-------------|
| `ExpoCamera::init(mode)` | `postSync(INIT)` |
| `ExpoCamera::release()` | `postSync(RELEASE)` |
| `ExpoCamera::startPreview(surfaceId)` | `postSync(START_PREVIEW)` |
| `ExpoCamera::switchSurface(surfaceId)` | `postSync(SWITCH_SURFACE)` |
| `OH_PhotoOutput_Capture(...)` | `postAsync(CAPTURE_BURST_FRAME)` 或 `postSync(CAPTURE_SINGLE)` |
| `onPhotoAvailable` 中的回调调用 | `postAsync(PHOTO_AVAILABLE)` 携带 buffer |

CameraCommand 线程处理命令时，串行调用对应的 ExpoCamera 方法。**由于全部串行执行，ExpoCamera 不再需要任何锁。**

#### Step 2.3: 连拍整合

- `startCaptureLoop` 改为向 CameraCommandQueue 投递 `CAPTURE_BURST_FRAME` 命令序列
- 帧间延时在 CameraCommand 线程上通过 `std::this_thread::sleep_for` 实现
- **消除 `captureLoopThread_`**（Phase 1.1 引入的），连拍逻辑完全由命令队列驱动

**效果**：`OH_PhotoOutput_Capture` 始终在 CameraCommand 线程上调用，满足相机子系统的线程亲和性要求。

---

### Phase 3: Buffer 管道优化（低风险，Phase 2 之后）

> 目标：Buffer 所有权明确，复制次数从 4 次降到 2 次。

#### Step 3.1: 引入 OwnedBuffer 类型

**新增文件**: `camera/owned_buffer.h`

```cpp
class OwnedBuffer {
public:
    OwnedBuffer() = default;
    OwnedBuffer(void* data, size_t size) : data_(data), size_(size) {}
    ~OwnedBuffer() { if (data_) free(data_); }

    OwnedBuffer(OwnedBuffer&& o) noexcept;
    OwnedBuffer& operator=(OwnedBuffer&& o) noexcept;

    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;

    void* data() const { return data_; }
    size_t size() const { return size_; }
    explicit operator bool() const { return data_ != nullptr; }
private:
    void* data_ = nullptr;
    size_t size_ = 0;
};
```

#### Step 3.2: 替换 void* 为 OwnedBuffer

**文件**: `expo_camera.h`、`capture_manager.h`、`task_queue.h`、`napi_expo_camera.cpp`

- `PhotoCapturedCallback`: `(void* buffer, size_t size, ...)` → `(OwnedBuffer, ...)`
- `ImageTask::buffer`: `void*` → `OwnedBuffer`
- `BurstImageCallback`: 同上
- 编译器会捕获所有遗漏的修改点

#### Step 3.3: 消除冗余复制

**文件**: `napi_expo_camera.cpp`

优化后的 Buffer 路径：
```
onPhotoAvailable:    malloc + memcpy (1次) → OwnedBuffer
CameraCommand 线程:  std::move → CaptureManager（零复制）
TaskQueue:           std::move → ImageTask（零复制）
processTask:         处理后，投递到 JS:
onBurstImageCallback: 直接 move 到 tsfn 数据（零复制）
BurstImageCallJs:    memcpy into ArrayBuffer (1次), 释放 original
```
**总计: 2 次复制**（从 4 次减少），且所有权链条清晰，每个环节要么 move 要么释放，不存在悬空引用。

---

## 4. 关键文件清单

| 文件 | 改动级别 | 涉及步骤 |
|------|---------|---------|
| `cpp/camera/capture_manager.h` | 中 | 1.1, 2.2, 3.2 |
| `cpp/camera/capture_manager.cpp` | **大** | 1.1, 2.2, 2.3, 3.2 |
| `cpp/camera/expo_camera.h` | 中 | 1.2, 3.2 |
| `cpp/camera/expo_camera.cpp` | **大** | 1.2, 1.3, 2.2, 2.3 |
| `cpp/napi_expo_camera.cpp` | 中 | 3.2, 3.3 |
| `cpp/camera/task_queue.h` | 小 | 3.2 |
| `cpp/camera/camera_command_queue.h` | **新增** | 2.1 |
| `cpp/camera/camera_command_queue.cpp` | **新增** | 2.1 |
| `cpp/camera/owned_buffer.h` | **新增** | 3.1 |

### 不改动的

- **`napi_threadsafe_function` 机制** — 已正确实现，CallJs 函数无问题
- **`TaskQueue`** — 设计良好，保留
- **`ImageProcessor`** — 内部锁使用正确，Phase 2 后仅 ProcessWorker 调用
- **`FileSaver`** — init 后无状态，线程安全
- **NAPI 模块注册** — 标准 HarmonyOS 模式
- **JS 端代码** — NAPI 接口不变

---

## 5. 验证方案

### Phase 1 验证

- [ ] 单拍 → 保存 → 释放 → 重新初始化（快速循环 10 次）
- [ ] 连拍 → 取消 → 释放（快速循环 5 次）
- [ ] 连拍进行中直接 `release`（测试 join 是否正常）
- [ ] 开启方舟多线程检测，确认无 `Fatal: ecma_vm cannot run in multi-thread`

### Phase 2 验证

- [ ] 完整生命周期：init → preview → capture → burst → release
- [ ] 多次切换 Surface 后拍照
- [ ] 连拍中取消，然后立即开始新的连拍
- [ ] 压力测试：连续 20 次单拍 + 连拍交替
- [ ] 命令队列性能：测量 `postSync` 延迟，确保不超过 5ms

### Phase 3 验证

- [ ] ASAN 运行，确认零内存泄漏和零 use-after-free
- [ ] 对比优化前后连拍内存峰值（预期下降约 50%）
- [ ] 输出图像文件 diff，确认处理结果不变

---

## 6. 风险评估

| 步骤 | 风险 | 出错影响 | 可逆性 |
|------|------|---------|--------|
| 1.1 可 join 线程 | **低** | 连拍无法启动 | 改一行即回退 |
| 1.2 atomic 回调守卫 | **低** | 回调不被触发 | 删除检查即回退 |
| 1.3 恢复只读锁 | **低** | 潜在死锁（锁中调回调） | 逐方法回退 |
| 2.1 命令队列类 | **中** | 新代码有 bug | 独立文件，可删除 |
| 2.2 路由硬件调用 | **中** | 相机操作失败或死锁 | 恢复直接调用 |
| 2.3 连拍整合 | **中** | 连拍时序变化 | 恢复独立线程 |
| 3.1 OwnedBuffer | **低** | 纯新增，不影响现有代码 | 删除文件 |
| 3.2 替换 void* | **中** | 编译错误（编译器捕获） | 恢复签名 |
| 3.3 消除复制 | **低** | use-after-free（ASAN 捕获） | 加回复制 |

---

## 7. 后续优化（本方案范围外）

- **SPSC 队列**：Phase 2 完成后，生产者固定为 CameraCommand 线程，消费者为 ProcessWorker 线程，可将 TaskQueue 替换为无锁 SPSC 队列（参见 `docs/design/spsc-queue-refactor.md`）
- **暗场处理**：线程模型稳定后可安全引入
- **GPU 加速**：ProcessWorker 中可引入 GPU 计算管线
