# 架构设计

## 模块概览

```
┌──────────────────────────────────────────────────────────────────────┐
│                        ExpoCamera SDK                                 │
│                                                                      │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐              │
│  │ 相机控制     │    │ 拍摄管理     │    │ 文件存储     │              │
│  │ ExpoCamera  │    │CaptureManager│    │ FileSaver   │              │
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
| **ExpoCamera** | 相机控制：初始化、预览、Surface 切换、参数设置、提供 PhotoOutput | 拍照触发、图像处理、文件IO |
| **CaptureManager** | 拍摄管理：单次拍照、连拍协调、状态管理、帧收集、协调处理流程 | 图像处理、文件IO |
| **ImageProcessor** | 图像处理：解码、对齐、堆叠、降噪、色调映射、编码 | 文件IO、拍照触发 |
| **TaskQueue** | 异步调度：队列管理、线程消费 | 图像处理逻辑 |
| **FileSaver** | 纯IO：文件写入、路径管理、平台存储API | 图像处理、编码 |

---

## 2. 拍摄管理架构

### 2.1 设计目标

- **统一入口**：所有拍摄动作（单次、连拍）统一由 CaptureManager 管理
- **互斥保证**：单拍和连拍互斥，同时只能进行一种拍摄操作
- **状态清晰**：通过状态机明确当前拍摄模式

### 2.2 状态机

```
┌─────────────────────────────────────────────────────────────────┐
│                    CaptureState 状态机                          │
│                                                                 │
│  ┌──────────────┐                                              │
│  │    IDLE      │ ← 空闲，可以开始新的拍摄                      │
│  └──────┬───────┘                                              │
│         │                                                        │
│         ├─── takePhoto() ────────────────────→ SINGLE_CAPTURING │
│         │                                          │            │
│         ├─── startBurstCapture() ──────────────→ BURST_CAPTURING │
│         │                                          │            │
│         │                                          ▼            │
│         │                                    拍照完成/错误       │
│         │                                          │            │
│  ┌──────┴───────┐                              ┌──┴─────────┐  │
│  │ SINGLE_DONE  │ ←────────────────────────── │ PROCESSING │  │
│  └──────┬───────┘                              └────┬───────┘  │
│         │                                              │         │
│         └────────────────── 返回 IDLE ────────────────┘         │
│                                                             │
│  任意状态 ──→ cancelBurst() ──→ CANCELLED ──→ IDLE            │
└─────────────────────────────────────────────────────────────────┘
```

### 2.3 互斥检查

```cpp
bool isCaptureActive() const {
    CaptureState s = state_.load();
    return s == SINGLE_CAPTURING ||   // 单拍进行中
           s == BURST_CAPTURING ||    // 连拍拍摄中
           s == PROCESSING;          // 连拍处理中
}
```

所有拍摄操作前都会检查 `isCaptureActive()`，确保互斥。

---

## 3. 单次拍照架构

### 3.1 拍照流程

```
[ArkTS 调用 takePhoto()]
         │
         ▼
[CaptureManager::captureSingle()]
         │
         ├─── 检查 isCaptureActive()
         │         │
         │    是 ──→ 返回 { errorCode: -EBUSY, sessionId: "" }
         │         │
         │    否 ──→ 继续
         │
         ├─── 生成 sessionId
         │
         ├─── state = SINGLE_CAPTURING
         │
         ├─── OH_PhotoOutput_Capture()
         │         │
         │         ├─── 失败 ──→ 返回 { errorCode: HarmonyOS 错误码, sessionId: "" }
         │         │
         │         └─── 成功 ──→ 返回 { errorCode: 0, sessionId: "session_xxx" }
         │
         └─── (异步) [onPhotoAvailable 回调]
                   │
                   ▼
         [CaptureManager::onSinglePhotoCaptured()]
                   │
                   ├─── 检查 state == SINGLE_CAPTURING
                   │
                   ├─── 调用 singlePhotoCallback_
                   │
                   └─── state = IDLE

[异步错误] [ExpoCamera::onError]
                   │
                   ▼
         [CaptureManager::onPhotoError]
                   │
                   └─── 调用 photoErrorCallback_ ──→ registerPhotoErrorCallback
```

### 3.2 特点

- **简单直接**：单张照片，直接回调，无需队列
- **快速响应**：不经过处理队列，拍照完成立即返回
- **独立状态**：使用 `SINGLE_CAPTURING` 状态，与连拍分离

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
│   ExpoCamera    │────▶│ CaptureManager  │
│  (相机控制)      │     │  (拍摄管理)      │
│                 │     │                 │
│ 提供 PhotoOutput │     │ - 单次拍照      │
│                 │     │ - 连拍协调      │
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
   │            │            │        │      │      |
ExpoCamera   ExpoCamera   ImageProcessor    FileSaver
                               │            (纯IO)
                          CaptureManager
                          (协调处理)
```

### 4.4 线程模型

```
拍照线程 (相机服务线程回调)
    │
    ▼
onPhotoAvailable
    │
    ├─ 连拍模式 ──▶ CaptureManager::onBurstPhotoCaptured ──▶ TaskQueue::enqueue
    │                                                          │
    │                                                          ▼
    │                                                    消费者线程
    │                                                          │
    │                                                          ▼
    │                                                    ImageProcessor::processFrame
    │                                                          │
    │                                                          ▼
    │                                                    napi_async_work
    │                                                          │
    │                                                          ▼
    │                                                    UI 主线程 (JS 回调)
    │
    └─ 单拍模式 ──▶ CaptureManager::onSinglePhotoCaptured ──▶ napi_async_work ──▶ UI 主线程
```

### 4.5 状态流转

```
IDLE ──startBurst()──▶ BURST_CAPTURING ──拍摄完成──▶ PROCESSING ──处理完成──▶ COMPLETED
    │                        │                           │
    │                        │                           └──处理失败──▶ ERROR
    │                        │
    │                        └──cancelBurst()──▶ CANCELLED
    │
    └──任意状态──cancelBurst()──▶ CANCELLED
```

---

## 5. Surface 切换机制

### 5.1 切换流程

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

## 6. RenderSlot 注册机制

### 6.1 设计背景

多渲染模块需要接收相机预览流，但 HarmonyOS 相机一次只能输出到一个 Surface。需要一个注册机制让渲染模块注册自己的 Surface，相机 so 管理切换并通知状态变化。

### 6.2 架构

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

### 6.3 注册关系

| 注册方 | 被注册方 | 注册什么 | 目的 |
|--------|----------|----------|------|
| 下层渲染模块 | 相机 so | RenderSlot（含 Surface） | 接收预览流和状态通知 |
| 上层应用 | 相机 so | 状态回调函数 | 接收相机整体状态变化 |

### 6.4 生命周期

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

## 7. 事件系统

### 7.1 事件流概述

```
单次拍照:
  takePhoto() → CaptureManager::captureSingle() → 返回 { errorCode, sessionId }
       │
       ├─── 触发失败 ──→ errorCode != 0（拒绝或相机错误）
       │
       └─── 触发成功 ──→ errorCode = 0, sessionId 有效
            │
            ▼
  onPhotoAvailable → onSinglePhotoCaptured → singlePhotoCallback_
       │
       └──→ ImageData { buffer, sessionId, isFinal: true }

  [异步错误流程]
  onError → CaptureManager::onPhotoError → photoErrorCallback_
       │
       └──→ PhotoError { sessionId, errorCode }

连拍堆叠:
  startBurstCapture() → CaptureManager::startBurst() → 返回 { errorCode, sessionId }
       │
       ├─── 启动失败 ──→ errorCode != 0
       │
       └─── 启动成功 ──→ errorCode = 0, sessionId 有效
            │
            ▼ (循环拍摄 N 帧)
  onPhotoAvailable → onBurstPhotoCaptured → TaskQueue::enqueue
       │
       ▼ (全部拍完后处理)
  ImageProcessor::processFrame() → 堆叠处理
       │
       ▼
  ImageData { buffer, sessionId, isFinal: false/true }
```

### 7.2 回调接口

```typescript
// 单次拍照 - 委托给 CaptureManager
takePhoto(): TakePhotoResult;  // { errorCode: number, sessionId: string }

// 图像数据回调（单次拍照和连拍共用）
registerImageDataCallback(callback: ImageDataCallback): void;

// 拍照错误回调（异步通知硬件错误）
registerPhotoErrorCallback(callback: PhotoErrorCallback): void;

// 连拍堆叠
startBurstCapture(
    config: BurstConfig,
    progressCallback: BurstProgressCallback,
    imageCallback: BurstImageCallback
): StartBurstResult;  // { errorCode: number, sessionId: string }

// 取消连拍
cancelBurstCapture(): void;

// 获取拍摄状态
getBurstState(): BurstState;
```

### 7.3 数据结构

```typescript
// 拍摄状态枚举
enum BurstState {
    IDLE = 0,                // 空闲
    SINGLE_CAPTURING = 1,    // 单次拍照中
    BURST_CAPTURING = 2,     // 连拍拍摄中
    PROCESSING = 3,          // 连拍处理中
    COMPLETED = 4,           // 连拍完成
    ERROR = 5,               // 错误
    CANCELLED = 6,           // 已取消
}

// 拍照结果
interface TakePhotoResult {
    errorCode: number;       // 0 成功，负数表示错误码
    sessionId: string;       // 会话 ID，成功时有值
}

// 连拍结果
interface StartBurstResult {
    errorCode: number;       // 0 成功，负数表示错误码
    sessionId: string;       // 会话 ID，成功时有值
}

// 拍照错误信息（异步回调）
interface PhotoError {
    sessionId: string;       // 会话 ID
    errorCode: number;       // HarmonyOS 相机错误码
}

// 图像数据
interface ImageData {
    sessionId: string;       // 会话 ID
    buffer: ArrayBuffer;      // 图像数据
    width: number;
    height: number;
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

## 8. 文件结构

```
cpp/
├── camera/
│   ├── expo_camera.h/cpp        # 相机控制
│   ├── capture_manager.h/cpp    # 拍摄管理（统一单拍和连拍）
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

## 9. 线程安全与并发控制

### 9.1 NAPI 回调线程模型

HarmonyOS 的 NAPI 框架中，JavaScript 回调必须在主线程执行，但相机回调来自 IPC 线程。这需要特殊的线程安全处理。

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

### 9.2 全局回调变量保护

**问题**：全局回调变量在多线程环境下存在数据竞争。

**原始代码（不安全）**：
```cpp
static napi_ref g_photoCallbackRef = nullptr;
static napi_env g_env = nullptr;  // ← 与线程绑定

static void onPhotoData(...) {
    // IPC 线程直接访问全局变量 ❌
    if (!g_photoCallbackRef || !g_env) { ... }
}
```

**修复方案（线程安全）**：
```cpp
static bool g_callbackValid = false;       // 有效标志
static std::mutex g_callbackMutex;         // 互斥锁

static void onPhotoData(...) {
    // 1. 在锁保护下复制数据到局部变量
    napi_env callbackEnv = nullptr;
    napi_ref callbackRef = nullptr;
    bool callbackValid = false;

    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        callbackValid = g_callbackValid;
        callbackEnv = g_env;
        callbackRef = g_photoCallbackRef;
    }  // ← 锁释放

    // 2. 使用局部变量创建异步工作
    napi_create_async_work(callbackEnv, ...);
}
```

### 9.3 回调注册时的原子化更新

**问题**：注册回调时，旧回调可能正在被使用。

```cpp
static napi_value RegisterImageDataCallback(...) {
    std::lock_guard<std::mutex> lock(g_callbackMutex);

    // 1. 先标记为无效（防止新请求使用旧回调）
    g_callbackValid = false;

    // 2. 释放旧引用
    if (g_photoCallbackRef && g_env) {
        napi_delete_reference(g_env, g_photoCallbackRef);
    }

    // 3. 更新引用
    g_env = env;
    napi_create_reference(env, args[0], 1, &g_photoCallbackRef);

    // 4. 标记为有效
    g_callbackValid = true;
}
```

### 9.4 需要线程保护的全局变量

| 变量 | 用途 | 保护方式 |
|------|------|----------|
| `g_photoCallbackRef` | 单拍图像回调 | `g_callbackMutex` + `g_callbackValid` |
| `g_photoErrorCallbackRef` | 拍照错误回调 | `g_photoErrorMutex` + `g_photoErrorCallbackValid` |
| `g_burstProgressCallbackRef` | 连拍进度回调 | `g_burstMutex` + `g_burstProgressCallbackValid` |
| `g_burstImageCallbackRef` | 连拍图像回调 | `g_burstMutex` + `g_burstImageCallbackValid` |
| `g_observerCallbacks` | 观察者回调映射 | `g_observerMutex` |

### 9.5 观察者通知修复

**问题**：`onPreviewObserver` 只通知活跃 slot 的观察者。

**修复**：
```cpp
// ❌ 修复前：只通知活跃观察者
static void onPreviewObserver(...) {
    auto it = g_observerCallbacks.find(activeSlotId);
    // 只调用活跃 slot 的回调
}

// ✅ 修复后：通知所有观察者
static void onPreviewObserver(...) {
    std::lock_guard<std::mutex> lock(g_observerMutex);

    for (auto& pair : g_observerCallbacks) {
        // 为每个观察者调用回调
        napi_call_function(...);
    }
}
```

---

## 10. 数据类型规范

### 10.1 图像尺寸类型选择

项目中图像的宽度和高度统一使用 `uint32_t` 类型。

**选择理由：**

1. **基本需求满足**：`uint16_t`（最大 65535）对于当前所有实际分辨率已经足够
   - 4K 手机相机：4032 × 3024
   - 8K 分辨率：7680 × 4320
   - 16K 分辨率：15360 × 8640

2. **保留冗余**：使用 `uint32_t` 提供更大的安全边界，避免边界情况下的溢出风险

3. **性能影响可忽略**：在 32/64 位系统上，`uint32_t` 与 `uint16_t` 的运算性能差异几乎为零

4. **与 HarmonyOS API 对齐**：虽然 HarmonyOS 的 `Image_Size` 使用 `int32_t`，但宽高不可能为负数，内部统一使用 `uint32_t` 更符合语义

### 10.2 类型转换边界

```
HarmonyOS API (int32_t)
    ↓ static_cast<uint32_t>
CaptureManager (uint32_t)
    ↓
ImageTask (uint32_t)
    ↓
ImageProcessor (uint32_t)
    ↓ static_cast<uint16_t> + 边界检查
motion_stack 第三方库 (uint16_t)
    ↓
NAPI 回调 (napi_create_uint32) → JavaScript
```

### 10.3 第三方库边界检查

调用第三方库 `motion_stack` 时，需要将 `uint32_t` 转换为 `uint16_t`。虽然实际值不会超出范围，但仍保留边界检查以防御异常情况：

```cpp
MeanRes ImageProcessor::MotionAnalysisAndStack(..., uint32_t width, uint32_t height) {
    // 边界检查：第三方库 motion_stack 使用 uint16_t，最大支持 65535
    if (width > UINT16_MAX || height > UINT16_MAX) {
        OH_LOG_ERROR(LOG_APP, "MotionAnalysisAndStack: dimension exceeds uint16_t limit");
        return emptyRes;
    }

    cir_buf.width = static_cast<uint16_t>(width);
    cir_buf.height = static_cast<uint16_t>(height);
    // ...
}
```

---

## 11. 依赖

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
