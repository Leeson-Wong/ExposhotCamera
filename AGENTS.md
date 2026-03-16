# 模块代理 (Agents)

本文档描述 ExpoCamera SDK 中各个模块代理的角色、职责和协作关系。

---

## 概览

```
┌─────────────────────────────────────────────────────────────────┐
│                      ExpoCamera SDK                              │
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │ ExpoCamera   │──│ BurstCapture │──│ImageProcessor│          │
│  │  相机控制     │  │  连拍管理     │  │  图像处理     │          │
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
| **拍照触发** | 单次拍照、连拍触发 |
| **参数控制** | 缩放、对焦模式、对焦点设置 |
| **事件分发** | 接收相机回调并分发给订阅者 |

### 不负责
- 图像处理（解码、堆叠、编码）
- 文件 IO 操作
- 具体的拍照业务逻辑

### 关键方法
```cpp
// 单例模式
static ExpoCamera& getInstance();

// 生命周期
bool init();
void release();

// 预览控制
bool startPreview(const std::string& surfaceId);
void stopPreview();
bool switchSurface(const std::string& surfaceId);

// 拍照
int takePhoto();

// 参数控制
bool setZoomRatio(float ratio);
bool setFocusMode(FocusMode mode);
bool setFocusPoint(float x, float y);

// RenderSlot 管理
bool registerSlot(const RenderSlot& slot);
bool unregisterSlot(const std::string& slotId);
bool switchToSlot(const std::string& slotId);

// 事件订阅
void subscribeState(StateCallback callback);
void unsubscribeState();
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
                │ 1. 触发拍照
                │ 2. 接收回调
                ▼
    ┌──────────────────────┐
    │   BurstCapture       │
    │   (连拍模式时)        │
    └──────────────────────┘
```

---

## 2. BurstCapture（连拍管理代理）

### 角色
连拍堆叠流程的协调者，管理多帧拍摄和处理的状态机。

### 职责
| 职责 | 说明 |
|------|------|
| **状态管理** | 维护连拍状态机（IDLE → CAPTURING → PROCESSING → COMPLETED） |
| **帧收集** | 接收拍照回调，收集原始帧数据 |
| **流程协调** | 协调拍照、处理、保存的异步流程 |
| **进度通知** | 向上层报告拍摄和处理进度 |

### 不负责
- 具体的图像处理算法
- 文件写入操作

### 状态机
```
                    startBurst()
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
static BurstCapture& getInstance();

// 连拍控制
bool startBurst(const BurstConfig& config);
void cancelBurst();
bool isBurstActive() const;

// 拍照回调（由 ExpoCamera 调用）
void onPhotoCaptured(void* buffer, size_t size, int32_t width, int32_t height);

// 状态查询
BurstState getState() const;
const BurstProgress& getProgress() const;
```

### 协作关系
```
┌─────────────┐                ┌─────────────┐
│ ExpoCamera  │───────────────▶│ BurstCapture│
│  (拍照回调)  │ onPhotoCaptured│  (状态协调)  │
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
│ BurstCapture │                │   Worker     │
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

### 场景 1：单次拍照

```
用户调用 takePhoto()
     │
     ▼
ExpoCamera: 触发拍照
     │
     ▼ (回调)
ExpoCamera: 接收图像数据
     │
     ▼
ExpoCamera: 通过 NAPI 返回给 ArkTS
```

### 场景 2：连拍堆叠

```
用户调用 startBurstCapture()
     │
     ▼
BurstCapture: 初始化状态机
     │
     ▼
ExpoCamera: 循环触发拍照 (N 次)
     │
     ├─────────────────────────────────┐
     ▼                                 ▼
ExpoCamera: onPhotoAvailable    BurstCapture: 收集帧
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

---

## 文件映射

| 代理 | 文件 |
|------|------|
| ExpoCamera | `camera/expo_camera.h`, `camera/expo_camera.cpp` |
| BurstCapture | `camera/burst_capture.h`, `camera/burst_capture.cpp` |
| ImageProcessor | `camera/image_processor.h`, `camera/image_processor.cpp` |
| TaskQueue | `camera/task_queue.h`, `camera/task_queue.cpp` |
| FileSaver | `camera/file_saver.h`, `camera/file_saver.cpp` |
| NAPI 桥接 | `napi_expo_camera.cpp` |
