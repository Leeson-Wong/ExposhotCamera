# 架构设计

## 模块概览

```
┌──────────────────────────────────────────────────────────────────────┐
│                        ExpoCamera SDK                                 │
│                                                                      │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐              │
│  │ 相机控制     │    │ 连拍堆叠     │    │ 文件存储     │              │
│  │ ExpoCamera  │    │BurstCapture │    │ FileSaver   │              │
│  └─────────────┘    └─────────────┘    └─────────────┘              │
│         │                  │                  │                      │
│         └──────────────────┼──────────────────┘                      │
│                            ▼                                         │
│                   ┌─────────────┐                                    │
│                   │ 图像处理     │                                    │
│                   │ImageProcessor│                                   │
│                   └─────────────┘                                    │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 1. 相机服务架构

### 1.1 设计目标

- 相机只初始化一次，应用退出时才释放
- 支持在多个渲染场景间切换预览流
- 同时支持 ArkTS 和 NDK 渲染模块

### 1.2 核心架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                    NativeCameraService (C++ 单例)                    │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────────┐ │
│  │  相机硬件（生命周期独立于场景）                                   │ │
│  │                                                                  │ │
│  │  Camera_Manager                                                  │ │
│  │       │                                                          │ │
│  │  Camera_Input ──→ Camera_CaptureSession ──→ PreviewOutput        │ │
│  │                          │                                       │ │
│  │                          │ (持续运行，不停止)                      │ │
│  │                          ▼                                       │ │
│  │                   当前活跃的 Surface                              │ │
│  └────────────────────────────────────────────────────────────────┘ │
│                                                                      │
│  核心能力：switchSurface(newSurfaceId)                               │
│  - 销毁旧 PreviewOutput，创建新 PreviewOutput                        │
│  - Session 保持运行（短暂 pause/resume）                              │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
                              │
                              │ 相机流输出到当前 Surface
                              ▼
┌──────────────────────────────────────────────────────────────────────┐
│                         渲染层                                       │
│                                                                      │
│  ┌──────────────┐    切换     ┌──────────────┐                      │
│  │   场景 A      │ ─────────► │   场景 B      │                      │
│  │  AR 星图      │            │   拍照预览    │                      │
│  │ XComponent A │            │ XComponent B │                      │
│  │ SurfaceId A  │            │ SurfaceId B  │                      │
│  └──────────────┘            └──────────────┘                      │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### 1.3 模块职责

| 类 | 职责 | 不负责 |
|---|------|--------|
| **ExpoCamera** | 相机控制：初始化、预览、拍照触发、参数设置、接收回调转发数据 | 图像处理、文件IO |
| **BurstCapture** | 连拍协调：状态管理、收集帧、协调处理流程 | 图像处理、文件IO |
| **ImageProcessor** | 图像处理：解码、对齐、堆叠、降噪、色调映射、编码 | 文件IO、拍照触发 |
| **TaskQueue** | 异步调度：队列管理、线程消费 | 图像处理逻辑 |
| **FileSaver** | 纯IO：文件写入、路径管理、平台存储API | 图像处理、编码 |

---

## 2. Surface 切换机制

### 2.1 切换流程

```
[场景 A 运行中]
     │
     │  相机 → PreviewOutput → Surface A → 渲染器 A
     │
     ▼
switchSurface(Surface B)
     │
     ├─ 1. Session.stop()           (短暂停止)
     │
     ├─ 2. Session.beginConfig()
     │
     ├─ 3. Session.removePreviewOutput(旧)
     │      └─ 旧 PreviewOutput.release()
     │
     ├─ 4. createPreviewOutput(Surface B)
     │
     ├─ 5. Session.addPreviewOutput(新)
     │
     ├─ 6. Session.commitConfig()
     │
     └─ 7. Session.start()          (恢复运行)
     │
     ▼
[场景 B 运行中]
     │
     │  相机 → PreviewOutput → Surface B → 渲染器 B
```

---

## 3. RenderSlot 注册机制

### 3.1 设计背景

多渲染模块需要接收相机预览流，但 HarmonyOS 相机一次只能输出到一个 Surface。需要一个注册机制让渲染模块注册自己的 Surface，相机 so 管理切换并通知状态变化。

### 3.2 架构

```
┌─────────────────────────────────────────────────────┐
│  上层应用                                            │
│  订阅：camera_so.subscribeState(callback)           │
└─────────────────────────────────────────────────────┘
                        ▲
                        │ 相机整体状态
                        │
┌───────────────────────┴─────────────────────────────┐
│  相机 so (ExpoCamera)                                │
│                                                     │
│  持有：                                              │
│    - List<RenderSlot> slots_                        │
│    - RenderSlot* activeSlot_                        │
│    - StateCallback stateCallback_                   │
│                                                     │
│  方法：                                              │
│    - registerSlot(slot)                             │
│    - unregisterSlot(slotId)                         │
│    - switchToSlot(slotId)                           │
│    - subscribeState(callback)                       │
└───────────────────────────────────┬─────────────────┘
                                    │ 预览流归属变化
                                    ▼
┌───────────────────────────────────┴─────────────────┐
│  RenderSlot (渲染模块注册的数据结构)                  │
│  {                                                  │
│    id: "preview_main",                              │
│    surfaceId: "xxx",                                │
│    width: 1080,                                     │
│    height: 1920,                                    │
│    userData: void*,                                 │
│    onPreviewChanged: callback                       │
│  }                                                  │
└─────────────────────────────────────────────────────┘
```

### 3.3 注册关系

| 注册方 | 被注册方 | 注册什么 | 目的 |
|--------|----------|----------|------|
| 下层渲染模块 | 相机 so | RenderSlot（含 Surface） | 接收预览流和状态通知 |
| 上层应用 | 相机 so | 状态回调函数 | 接收相机整体状态变化 |

### 3.4 生命周期

```
1. 进入页面
     │
     ▼
2. XComponent.onLoad() → 获得 Surface
     │
     ▼
3. 渲染模块初始化，构造 RenderSlot
     │
     ▼
4. nativeCamera.registerSlot(slotId, surfaceId, width, height, callback)
     │
     ▼
5. nativeCamera.switchToSlot(slotId)  ← 切换预览流
     │
     ▼
6. callback(surfaceId, true)  ← 渲染模块收到预览流
     │
     ▼
7. 退出页面
     │
     ▼
8. nativeCamera.unregisterSlot(slotId)
```

---

## 4. 连拍堆叠架构

### 4.1 功能特点

- 一次拍摄 N 张（拍摄前已知数量）
- 长曝光支持（每张 ~10s）
- 堆叠处理（第1张作为基准，后续每张与累积结果堆叠）
- 异步处理（拍照不阻塞处理）
- 实时预览（每次堆叠后返回累积结果）

### 4.2 模块结构

```
┌─────────────────┐     ┌─────────────────┐
│   ExpoCamera    │────▶│  BurstCapture   │
│  (相机控制)      │     │  (连拍协调)      │
└─────────────────┘     └────────┬────────┘
                                 │
                    ┌────────────┼────────────┐
                    ▼            ▼            ▼
            ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
            │ImageProcessor│ │  TaskQueue   │ │  FileSaver   │
            │ (图像处理)    │ │ (异步调度)   │ │ (纯IO)       │
            │ - 解码       │ │              │ │ - 写文件     │
            │ - 堆叠       │ │              │ │ - 路径管理   │
            │ - 编码       │ │              │ │              │
            └──────────────┘ └──────────────┘ └──────────────┘
```

### 4.3 数据流向

```
拍照触发 → 获取原始数据 → 解码 → 处理(堆叠) → 编码 → 落盘 → 回调
   |            |            |        |      |      |
ExpoCamera   ExpoCamera   ImageProcessor    FileSaver
                               |            (纯IO)
                          BurstCapture
                          (协调处理)
```

### 4.4 线程模型

```
拍照线程 (相机服务线程回调)
    │
    ▼
onPhotoAvailable
    │
    ├─ 连拍模式 ──▶ BurstCapture::onPhotoCaptured ──▶ TaskQueue::enqueue
    │                                                    │
    │                                                    ▼
    │                                              消费者线程
    │                                                    │
    │                                                    ▼
    │                                              ImageProcessor::processFrame
    │                                                    │
    │                                                    ▼
    │                                              napi_async_work
    │                                                    │
    │                                                    ▼
    │                                              UI 主线程 (JS 回调)
    │
    └─ 普通模式 ──▶ photoCallback_ ──▶ napi_async_work ──▶ UI 主线程
```

### 4.5 状态流转

```
IDLE ──startBurst()──▶ CAPTURING ──拍摄完成──▶ PROCESSING ──处理完成──▶ COMPLETED
    │                      │                        │
    │                      │                        └──处理失败──▶ ERROR
    │                      │
    │                      └──cancelBurst()──▶ CANCELLED
    │
    └──任意状态──cancelBurst()──▶ CANCELLED
```

---

## 5. 事件系统

### 5.1 事件流概述

```
单次拍照（简单流程）:
  takePhoto() → 返回 sessionId
       │
       ▼
  capture_start → capture_end + 图片数据 (所有事件携带相同 sessionId)

连拍堆叠（完整流程）:
  startBurstCapture() → 返回 sessionId
       │
       ▼ (循环拍摄 N 帧)
  capture_start → capture_end (每帧，       │
       ▼ (全部拍完后处理)
  process_start → process_progress → process_end
       │
       ▼
  image_ready (堆叠合成后的最终图片)
  (所有事件携带相同 sessionId，便于追踪同一次拍照)
```

### 5.2 事件类型

| 阶段 | 事件 | 说明 |
|------|------|------|
| **拍照** | `capture_start` | 拍照命令已发送 |
| | `capture_end` | 拍照成功，原始数据已获取 |
| | `capture_failed` | 拍照失败 |
| **处理** | `process_start` | 开始处理 |
| | `process_progress` | 处理进度更新 |
| | `process_end` | 处理完成 |
| | `process_failed` | 处理失败 |
| **数据** | `image_ready` | 图像数据就绪 |

### 5.3 回调接口

```typescript
// 单次拍照
takePhoto(): number;

// 拍照事件回调
registerPhotoEventCallback(callback: PhotoEventCallback): void;

// 处理事件回调（连拍堆叠用）
registerProcessEventCallback(callback: ProcessEventCallback): void;

// 图像数据回调
registerImageDataCallback(callback: ImageDataCallback): void;

// 连拍堆叠（独立系统）
startBurstCapture(
    config: BurstConfig,
    progressCallback: BurstProgressCallback,
    imageCallback: BurstImageCallback
): boolean;
```

### 5.4 数据结构

```typescript
// 拍照事件
interface PhotoEvent {
    type: PhotoEventType;  // CAPTURE_START | CAPTURE_END | CAPTURE_FAILED
    sessionId: string;       // 会话 ID， 关联同一次拍照
    frameIndex?: number;   // 连拍帧索引
    message?: string;
}

// 处理事件
interface ProcessEvent {
    type: ProcessEventType;    // PROCESS_START | PROCESS_PROGRESS | PROCESS_END | PROCESS_FAILED
    sessionId: string;           // 会话 ID
    progress?: number;         // 进度百分比 (0-100)
    currentFrame?: number;     // 当前帧
    totalFrames?: number;      // 总帧数
    message?: string;
}

// 图像数据
interface ImageData {
    sessionId: string;       // 会话 ID
    buffer?: ArrayBuffer;      // 图像数据
    filePath?: string;         // 文件路径
    width: number;
    height: number;
    frameIndex?: number;       // 连拍帧索引
    isFinal: boolean;          // 是否最终结果
}

// 连拍配置
interface BurstConfig {
    frameCount: number;        // 拍摄帧数
    exposureMs: number;        // 每帧曝光时间(ms)
    realtimePreview: boolean;  // 是否实时预览中间结果
}

// 连拍进度
interface BurstProgress {
    sessionId: string;       // 会话 ID
    state: BurstState;
    capturedFrames: number;
    processedFrames: number;
    totalFrames: number;
    message: string;
}
```

---

## 6. 文件结构

```
cpp/
├── camera/
│   ├── expo_camera.h/cpp        # 相机控制
│   ├── burst_capture.h/cpp      # 连拍管理
│   ├── task_queue.h/cpp         # 异步任务队列
│   ├── image_processor.h/cpp    # 图像处理
│   └── file_saver.h/cpp         # 文件保存
├── napi/
│   └── napi_expo_camera.cpp     # NAPI 桥接
└── types/
    └── libexpocamera/
        └── Index.d.ts           # 类型定义
```

---

## 7. 依赖

```cmake
target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    libnative_buffer.so
    libohcamera.so
    libohimage.so
    libohfileuri.so
)
```
