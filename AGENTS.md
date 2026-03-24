# 模块代理 (Agents)

本文档描述 ExpoCamera SDK 中各个模块代理的角色、职责和协作关系。

---

## 概览

```
┌─────────────────────────────────────────────────────────────────┐
│                      ExpoCamera SDK                              │
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │ ExpoCamera   │──│CaptureManager│──│ImageProcessor│          │
│  │  相机控制     │  │  拍摄管理     │  │  图像处理     │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
│         │                  │                  │                  │
│         └──────────────────┼──────────────────┘                  │
│                            │                                     │
│                            ▼                                     │
│                   ┌──────────────┐                               │
│                   │  TaskQueue   │                               │
│                   │  任务调度     │                               │
│                   └──────────────┘                               │
│                            │                                     │
│                            ▼                                     │
│                   ┌──────────────┐                               │
│                   │  FileSaver   │                               │
│                   │  文件存储     │                               │
│                   └──────────────┘                               │
└─────────────────────────────────────────────────────────────────┘
```

---

## 1. ExpoCamera（相机控制代理）

### 角色
相机硬件的统一管理者，负责相机生命周期和基础控制。

### 职责
| 职责 | 说明 |
|------|------|
| **生命周期管理** | 相机初始化、释放、状态维护 |
| **预览控制** | 启动/停止预览、Surface 切换 |
| **参数控制** | 缩放、对焦模式、对焦点设置 |
| **观察者管理** | 多观察者注册、预览流切换通知 |
| **事件分发** | 接收相机回调并分发到订阅者 |

### 不负责
- 拍照触发（委托给 CaptureManager）
- 图像处理（解码、堆叠、编码）
- 文件 IO 操作
- 具体的拍照业务逻辑

### 关键方法
```cpp
// 单例模式
static ExpoCamera& getInstance();

// 生命周期
bool init(CaptureMode mode);  // mode: SINGLE 或 BURST
void release();

// 预览控制
bool startPreview(const std::string& surfaceId);
void stopPreview();
bool switchSurface(const std::string& surfaceId);

// 参数控制
bool setZoomRatio(float ratio);
bool setFocusMode(FocusMode mode);
bool setFocusPoint(float x, float y);

// 模式切换
Camera_ErrorCode switchCaptureMode(CaptureMode mode);
CaptureMode getCaptureMode() const;
bool canSwitchMode() const;

// 观察者管理
std::string registerObserver(const std::string& surfaceId, BindPreviewObserverCallback bindCallback);
bool unregisterObserver(const std::string& slotId);
bool switchToSlot(const std::string& slotId);

// 事件订阅
void subscribeState(StateCallback callback);
void unsubscribeState();

// 提供 PhotoOutput（供 CaptureManager 使用）
Camera_PhotoOutput* getPhotoOutput() const;

// 回调注册（供 CaptureManager 注册）
void setPhotoCapturedCallback(PhotoCapturedCallback callback);
void setPhotoErrorCallback(PhotoErrorCallback callback);
```

### 协作关系
```
         ┌─────────────┐
         │ HarmonyOS   │
         │ Camera API  │
         └──────┬──────┘
                │ 管理相机硬件
                ▼
         ┌─────────────┐
         │ ExpoCamera  │
         └──────┬──────┘
                │ 1. 提供硬件能力
                │ 2. 接收回调并转发
                ▼
    ┌──────────────────────┐
    │   CaptureManager     │
    │   (拍照管理)          │
    └──────────────────────┘
```

---

## 2. CaptureManager（拍摄管理代理）

### 角色
拍摄流程的统一协调者，管理单次拍照和连拍的状态机，确保拍摄互斥。

### 职责
| 职责 | 说明 |
|------|------|
| **状态管理** | 维护拍摄状态机（IDLE → SINGLE_CAPTURING/BURST_CAPTURING → PROCESSING） |
| **互斥控制** | 确保单拍和连拍不能同时进行 |
| **帧收集** | 接收拍照回调，收集原始帧数据（连拍模式） |
| **流程协调** | 协调拍照、处理、保存的异步流程 |
| **进度通知** | 向上层报告拍摄和处理进度 |

### 不负责
- 具体的图像处理算法
- 文件写入操作
- 相机硬件控制（由 ExpoCamera 负责）

### 状态机
```
                    takePhoto() / startBurst()
                         │
                         ▼
┌──────────┐        ┌──────────┐        ┌──────────┐
│   IDLE   │───────▶│CAPTURING │───────▶│PROCESSING│
└──────────┘        └──────────┘        └──────────┘
     ▲                    │                    │
     │                    ▼                    │
     │              ┌──────────┐               │
     │              │CANCELLED │◀──────────────┘
     │              └──────────┘
     │                    ▲
     └────────────────────┘
         cancelBurst() (任意状态)
```

### 关键方法
```cpp
// 单例模式
static CaptureManager& getInstance();

// 生命周期
bool init(CaptureMode mode);  // mode: SINGLE 或 BURST
void release();

// 回调设置
void setProgressCallback(BurstProgressCallback callback);
void setImageCallback(BurstImageCallback callback);
void setSinglePhotoCallback(SinglePhotoCallback callback);
void setPhotoErrorCallback(PhotoErrorCallback callback);

// 单次拍照
int32_t captureSingle(std::string& outSessionId);
void onSinglePhotoCaptured(void* buffer, size_t size, int32_t width, int32_t height);

// 连拍
int32_t startBurst(const BurstConfig& config, std::string& outSessionId);
void cancelBurst();
void onBurstPhotoCaptured(void* buffer, size_t size, int32_t width, int32_t height);

// 模式切换
int32_t switchCaptureMode(CaptureMode mode);
CaptureMode getCaptureMode() const;
bool canSwitchMode() const;

// 状态查询
bool isCaptureActive() const;  // 包含单拍和连拍
bool isBurstActive() const;   // 仅连拍（兼容旧接口）
CaptureState getState() const;
```

### 协作关系
```
┌─────────────┐                ┌─────────────┐
│ ExpoCamera  │───────────────▶│CaptureManager│
│ (提供 PhotoOutput)          │  (状态协调)  │
└─────────────┘                └──────┬──────┘
                                      │
                                      │ 1. 提交任务
                                      │ 2. 接收结果
                                      ▼
                               ┌─────────────┐
                               │  TaskQueue  │
                               │ (异步调度)   │
                               └─────────────┘
```

---

## 3. ImageProcessor（图像处理代理）

### 角色
图像数据的处理引擎，执行解码、堆叠、编码等图像算法。

### 职责
| 职责 | 说明 |
|------|------|
| **图像解码** | 将相机原始数据解码为 RGBA 格式 |
| **堆叠处理** | 多帧图像对齐、平均堆叠 |
| **图像编码** | 将处理结果编码为 JPEG 格式 |
| **内存管理** | 管理处理过程中的缓冲区 |

### 不负责
- 文件写入
- 拍照触发
- 状态通知

### 处理流程
```
原始数据 (JPEG)
     │
     ▼
┌─────────┐
│ decode() │──▶ RGBA Buffer (帧1)
└─────────┘
     │
     ▼
┌─────────┐
│initStack()│ 初始化累积缓冲区
└─────────┘
     │
     ▼
┌────────────────────────────────┐
│ processFrame() (循环每帧)       │
│  - 累加到累积缓冲区              │
│  - getCurrentResult() 获取中间结果 │
└────────────────────────────────┘
     │
     ▼
┌─────────┐
│finalize()│──▶ 最终 RGBA Buffer
└─────────┘
     │
     ▼
┌─────────┐
│encode() │──▶ JPEG Buffer
└─────────┘
```

### 关键方法
```cpp
// 解码
bool decode(void* rawBuffer, size_t rawSize, ImageFormat format,
            uint8_t** outRgbaBuffer, size_t* outSize,
            int32_t* outWidth, int32_t* outHeight);

// 编码
bool encodeJpeg(uint8_t* rgbaBuffer, size_t rgbaSize,
                int32_t width, int32_t height, int32_t quality,
                void** outJpegBuffer, size_t* outJpegSize);

// 堆叠
bool initStacking(int32_t totalFrames, int32_t width, int32_t height);
bool processFrame(int32_t frameIndex, void* rawBuffer, size_t rawSize,
                  int32_t width, int32_t height, ImageFormat format, bool isFirst);
bool getCurrentResult(void** outBuffer, size_t* outSize);
bool finalize(void** outBuffer, size_t* outSize);
void reset();
```

### 协作关系
```
┌──────────────┐
│  TaskQueue   │
│  (调用者)     │
└──────┬───────┘
       │
       │ 消费 ImageTask
       ▼
┌──────────────────┐
│ ImageProcessor   │
│                  │
│  decode()        │──▶ 内部缓冲区
│  processFrame()  │
│  encode()        │
└──────────────────┘
```

---

## 4. TaskQueue（任务调度代理）

### 角色
异步任务的生产者-消费者模型，线程安全的任务调度器。

### 职责
| 职责 | 说明 |
|------|------|
| **任务入队** | 接收待处理的图像任务 |
| **异步消费** | 独立线程处理任务队列 |
| **线程安全** | 使用互斥锁和条件变量保证安全 |
| **生命周期** | 启动、停止、清空队列 |

### 不负责
- 具体的任务处理逻辑（由 handler 决定）
- 业务状态管理

### 任务结构
```cpp
struct ImageTask {
    int32_t taskId;       // 任务 ID（帧索引）
    void* buffer;         // 图像数据（所有权转移）
    size_t size;          // 数据大小
    int32_t width;        // 图像宽度
    int32_t height;       // 图像高度
    bool isFirst;         // 是否是基准帧
    ImageFormat format;   // 图像格式

    // 所有权管理
    ImageTask();
    ~ImageTask();         // 自动释放 buffer
    ImageTask(ImageTask&& other) noexcept;  // 移动构造
    ImageTask& operator=(ImageTask&& other) noexcept;
};
```

### 关键方法
```cpp
// 任务控制
void enqueue(ImageTask&& task);
void start(TaskHandler handler);
void stop();
void clear();

// 状态查询
size_t getQueueSize() const;
bool isRunning() const;
```

### 协作关系
```
┌──────────────┐                ┌──────────────┐
│CaptureManager│                │   Worker     │
│ (生产者)      │ enqueue()      │  Thread      │
└──────┬───────┘───────────────▶└──────┬───────┘
       │                                 │
       │                                 │ 消费任务
       │                                 ▼
       │                         ┌──────────────┐
       │                         │  TaskHandler │
       │                         │  (处理函数)   │
       │                         └──────────────┘
       │                                 │
       │                                 ▼
       │                         ┌──────────────┐
       └─────────────────────────│ImageProcessor│
                                 └──────────────┘
```

---

## 5. FileSaver（文件存储代理）

### 角色
纯粹的文件 IO 模块，负责将处理后的图像保存到存储设备。

### 职责
| 职责 | 说明 |
|------|------|
| **文件写入** | 将图像数据写入文件系统 |
| **路径管理** | 生成文件名、管理保存目录 |
| **平台适配** | 调用 HarmonyOS 存储 API |

### 不负责
- 图像处理
- 编码解码
- 业务逻辑

### 关键方法
```cpp
// 保存控制
bool saveImageToFile(void* buffer, size_t size, const std::string& filename);

// 路径管理
std::string getImageSaveDir() const;
void setImageSaveDir(const std::string& dir);

// 文件名生成
std::string generateFileName(const std::string& prefix = "IMG");
std::string generateFileName(const std::string& prefix, int32_t index);
```

### 协作关系
```
┌──────────────────┐
│ ImageProcessor   │
│ (处理完成)        │
└────────┬─────────┘
         │
         │ 编码后的 JPEG 数据
         ▼
┌──────────────────┐
│   FileSaver      │
│                  │
│  saveImageToFile()│──▶ 文件系统
└──────────────────┘
```

---

## 协作场景

### 场景 0：初始化与模式选择

```
应用启动
     │
     ▼
用户选择拍摄模式（SINGLE 或 BURST）
     │
     ▼
NAPI: initCamera(mode, resourceManager?)
     │
     ▼
CaptureManager::init(mode)
     │
     ├─→ ExpoCamera::init(mode)  // 根据 mode 选择分辨率
     │         │
     │         └─→ SINGLE: 选择最高分辨率
     │             BURST:  选择 1080p 附近分辨率
     │
     ├─→ FileSaver::init()
     │
     └─→ 注册回调到 ExpoCamera
```

### 场景 1：单次拍照

```
用户调用 takePhoto()
     │
     ▼
CaptureManager: 检查互斥，生成 sessionId
     │
     ▼
ExpoCamera: OH_PhotoOutput_Capture()
     │
     ▼ (回调 - IPC 线程)
ExpoCamera: onPhotoAvailable
     │
     ▼
photoCapturedCallback_ (注册的回调)
     │
     ▼
CaptureManager: currentMode_ == SINGLE → onSinglePhotoCaptured
     │
     ▼ (异步)
NAPI 回调: 返回 ImageData 给 ArkTS
```

### 场景 2：连拍堆叠

```
用户调用 startBurstCapture()
     │
     ▼
CaptureManager: 初始化状态机，启动 TaskQueue
     │
     ▼
ExpoCamera: 循环触发拍照 (N 次)
     │
     ├─────────────────────────────────┐
     ▼                                 ▼
ExpoCamera: onPhotoAvailable    CaptureManager: 收集帧
     │                                 │
     └─────────────────────────────────┘
                                       │
                                       ▼
                                TaskQueue: 入队任务
                                       │
                                       ▼
                                ImageProcessor: 处理帧
                                       │
                                       ├─▶ decode()
                                       ├─▶ processFrame()
                                       └─▶ encode()
                                       │
                                       ▼
                                FileSaver: 保存文件
                                       │
                                       ▼
                                NAPI 回调: 返回结果
```

### 场景 3：模式切换

```
用户调用 switchCaptureMode(SINGLE → BURST)
     │
     ▼
CaptureManager::switchCaptureMode()
     │
     ├─→ 检查 canSwitchMode()（预览已启动且无拍摄进行中）
     │         │
     │         └─→ false → 返回错误码
     │
     ▼
ExpoCamera::switchCaptureMode(BURST)
     │
     ├─→ Session.stop()
     ├─→ Session.removeOutput(old PhotoOutput)
     ├─→ old PhotoOutput.release()
     ├─→ 选择 1080p 附近的 photoProfile
     ├─→ 创建新 PhotoOutput
     ├─→ 注册拍照回调
     ├─→ Session.addOutput(new PhotoOutput)
     ├─→ Session.commitConfig()
     └─→ Session.start()
     │
     ▼
CaptureManager: 更新 currentMode_ = BURST
```

### 场景 4：预览流切换

```
页面 A 注册观察者
     │
     ▼
ExpoCamera: registerObserver(surfaceId, callback)
     │
     ▼
返回 slotId_A
     │
     ▼
用户切换到页面 B
     │
     ▼
ExpoCamera: switchToSlot(slotId_B)
     │
     ▼
ExpoCamera: switchSurface(surfaceId_B)
     │
     ▼
ExpoCamera: notifyAllObservers(activeSlotId=slotId_B)
     │
     ├────────────────┬────────────────┐
     ▼                ▼                ▼
观察者 A 回调      观察者 B 回调      ...
(hasPreview=false)  (hasPreview=true)
```

---

## 线程安全设计

### 全局回调变量保护

由于相机回调来自 IPC 线程，而 NAPI 回调必须在主线程执行，需要特殊的线程安全处理。

| 变量 | 用途 | 保护方式 |
|------|------|----------|
| `g_photoCallbackRef` | 单拍图像回调 | `g_callbackMutex` + `g_callbackValid` |
| `g_photoErrorCallbackRef` | 拍照错误回调 | `g_photoErrorMutex` + `g_photoErrorCallbackValid` |
| `g_burstProgressCallbackRef` | 连拍进度回调 | `g_burstMutex` + `g_burstProgressCallbackValid` |
| `g_burstImageCallbackRef` | 连拍图像回调 | `g_burstMutex` + `g_burstImageCallbackValid` |
| `g_observerCallbacks` | 观察者回调映射 | `g_observerMutex` |

### 线程模型

```
IPC 线程 (相机回调)                      主线程 (ArkTS)
     │                                          │
     ▼                                          ▼
onPhotoAvailable()                         napi_async_work
     │                                          │
     ├─ 问题：不能直接调用 JS 回调              ├─ 解决：异步工作队列
     │                                          │
     └─→ 创建 napi_async_work ─────────────────▶ execute (主线程)
                                                     │
                                                     ▼
                                               JS callback()
```

---

## 设计原则

### 1. 单一职责原则
每个代理只负责一个明确的功能域，职责清晰。

### 2. 依赖倒置
上层依赖接口，不依赖具体实现。

### 3. 所有权明确
使用移动语义传递所有权，避免内存泄漏。

### 4. 线程安全
共享状态使用互斥锁保护，避免数据竞争。

### 5. 错误隔离
每个代理独立处理错误，不传播到其他模块。

### 6. 拍摄互斥
单拍和连拍共享状态机，确保同时只能进行一种拍摄操作。

---

## 文件映射

| 代理 | 文件 |
|------|------|
| ExpoCamera | `camera/expo_camera.h`, `camera/expo_camera.cpp` |
| CaptureManager | `camera/capture_manager.h`, `camera/capture_manager.cpp` |
| ImageProcessor | `camera/image_processor.h`, `camera/image_processor.cpp` |
| TaskQueue | `camera/task_queue.h`, `camera/task_queue.cpp` |
| FileSaver | `camera/file_saver.h`, `camera/file_saver.cpp` |
| NAPI 桥接 | `napi_expo_camera.cpp` |

---

## 版本历史

| 版本 | 日期 | 更新内容 |
|------|------|----------|
| 2.4.0 | 2026-03-24 | API 改进：`init()` 接收 `CaptureMode` 必填参数；新增模式切换功能 `switchCaptureMode()` |
| 2.3.0 | 2026-03-23 | 新增：`ExpoCamera::switchCaptureMode()` 支持运行时切换单拍/连拍模式 |
| 2.2.0 | 2026-03-18 | 更新：BurstCapture 替换为 CaptureManager（统一单拍和连拍）；添加线程安全设计说明 |
| 2.0.0 | 2026-03-16 | 重构：统一拍摄动作到 CaptureManager，实现单拍/连拍互斥 |
| 1.0.0 | - | 初始版本 |
