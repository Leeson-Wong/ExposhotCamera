# 回调流程与锁机制

本文档描述 NDK 层的回调机制和线程安全设计。

---

## 1. 全局变量与锁一览

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              napi_expo_camera.cpp                            │
├─────────────────────────────────────────────────────────────────────────────┤
│  全局变量                        │  保护锁                  │  用途         │
├─────────────────────────────────┼──────────────────────────┼───────────────┤
│  g_photoCallbackRef             │  g_photoCallbackMutex    │  单次拍照图像  │
│  g_photoEnv                     │  (仅注册时使用)          │  (简化设计)    │
├─────────────────────────────────┼──────────────────────────┼───────────────┤
│  g_photoErrorCallbackRef        │  g_photoErrorMutex       │  拍照错误      │
│  g_photoErrorEnv                │                          │               │
│  g_photoErrorCallbackValid      │                          │               │
├─────────────────────────────────┼──────────────────────────┼───────────────┤
│  g_burstProgressCallbackRef     │  g_burstMutex            │  连拍进度      │
│  g_burstImageCallbackRef        │                          │  连拍图像      │
│  g_burstEnv                     │                          │               │
│  g_burstProgressCallbackValid   │                          │               │
│  g_burstImageCallbackValid      │                          │               │
├─────────────────────────────────┼──────────────────────────┼───────────────┤
│  g_observerCallbacks (map)      │  g_observerMutex         │  预览观察者    │
├─────────────────────────────────┼──────────────────────────┼───────────────┤
│  g_stateCallbackRef             │  g_stateMutex            │  状态订阅      │
│  g_stateEnv                     │                          │               │
├─────────────────────────────────┼──────────────────────────┼───────────────┤
│  g_photoEventCallbackRef        │  g_photoEventMutex       │  拍照事件      │
│  g_photoEventEnv                │                          │               │
├─────────────────────────────────┼──────────────────────────┼───────────────┤
│  g_processEventCallbackRef      │  g_processEventMutex     │  处理事件      │
│  g_processEventEnv              │                          │               │
├─────────────────────────────────┼──────────────────────────┼───────────────┤
│  g_resourceManager              │  (无锁，启动时设置一次)   │  Rawfile 访问  │
└─────────────────────────────────┴──────────────────────────┴───────────────┘
```

---

## 2. 单次拍照流程（简化设计）

> **设计假设**：应用只在初始化时注册一次回调，之后不再修改。

### 2.1 流程图

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                              单次拍照流程（简化）                              │
└──────────────────────────────────────────────────────────────────────────────┘

  ArkTS (UI 线程)                    NDK (Native)                      相机回调线程
  ──────────────                     ────────────                      ──────────────
        │                                  │                                  │
        │  1. registerImageDataCallback()  │                                  │
        ├─────────────────────────────────►│                                  │
        │                                  │ ┌─────────────────────────────┐  │
        │                                  │ │ lock(g_photoCallbackMutex)  │  │
        │                                  │ │ 保存回调引用到              │  │
        │                                  │ │ g_photoCallbackRef          │  │
        │                                  │ │ g_photoEnv                  │  │
        │                                  │ └─────────────────────────────┘  │
        │                                  │                                  │
        │  2. takePhoto()                  │                                  │
        ├─────────────────────────────────►│                                  │
        │                                  │ ExpoCamera::takePhoto()          │
        │                                  │     │                            │
        │                                  │     ▼                            │
        │                                  │ OH_CaptureOutput_Capture()       │
        │                                  │     │                            │
        │                                  │     │  3. 相机捕获完成            │
        │                                  │     │◄───────────────────────────┤
        │                                  │     │                            │
        │                                  │     ▼  onPhotoFrameAvailable()   │
        │                                  │ CaptureManager::handlePhotoFrame │
        │                                  │     │                            │
        │                                  │     ▼                            │
        │                                  │ ┌─────────────────────────────┐  │
        │                                  │ │ 无锁，只做空指针检查：       │  │
        │                                  │ │ if (g_photoCallbackRef &&   │  │
        │                                  │ │     g_photoEnv) { ... }     │  │
        │                                  │ └─────────────────────────────┘  │
        │                                  │     │                            │
        │  4. ImageData 回调               │     │                            │
        │◄─────────────────────────────────┼─────┘                            │
        │                                  │                                  │
        ▼                                  ▼                                  ▼
```

### 2.2 使用的锁

| 操作 | 锁 | 说明 |
|------|-----|------|
| `registerImageDataCallback` | `g_photoCallbackMutex` | **仅在注册时加锁** |
| 回调触发时 | **无锁** | 只做空指针检查 |
| `takePhoto` | 无全局锁 | 相机内部有自己的锁 |

### 2.3 关键代码

```cpp
// 注册回调 - 加锁保护
static napi_value RegisterImageDataCallback(napi_env env, napi_callback_info info) {
    // ...
    {
        std::lock_guard<std::mutex> lock(g_photoCallbackMutex);
        // 释放旧引用...
        g_photoEnv = env;
        napi_create_reference(env, args[0], 1, &g_photoCallbackRef);
    }
}

// 调用回调时 - 无锁，只做空指针检查
static void onPhotoData(...) {
    if (!g_photoCallbackRef || !g_photoEnv) {
        OH_LOG_ERROR(LOG_APP, "Photo callback not registered");
        return;
    }
    // 直接使用 g_photoCallbackRef 和 g_photoEnv
}
```

### 2.4 简化原因

1. **大多数场景只在初始化时注册一次**
2. **减少锁竞争开销**
3. **代码更简洁**

### 2.5 使用约束

⚠️ **必须遵守**：
- 在调用 `takePhoto` 之前完成 `registerImageDataCallback`
- 不要在拍照过程中重新注册回调
- 页面销毁时先停止拍照，再释放资源

---

## 3. 连拍流程

### 3.1 流程图

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                              连拍流程                                         │
└──────────────────────────────────────────────────────────────────────────────┘

  ArkTS (UI 线程)                    NDK (Native)                      相机/处理线程
  ──────────────                     ────────────                      ──────────────
        │                                  │                                  │
        │  1. startBurstCapture(config,    │                                  │
        │     progressCallback,            │                                  │
        │     imageCallback)               │                                  │
        ├─────────────────────────────────►│                                  │
        │                                  │ ┌─────────────────────────────┐  │
        │                                  │ │ lock(g_burstMutex)          │  │
        │                                  │ │ 保存 progressCallback 到    │  │
        │                                  │ │   g_burstProgressCallbackRef│  │
        │                                  │ │ 保存 imageCallback 到       │  │
        │                                  │ │   g_burstImageCallbackRef   │  │
        │                                  │ └─────────────────────────────┘  │
        │                                  │                                  │
        │                                  │ CaptureManager::startBurst()     │
        │                                  │     │                            │
        │                                  │     ▼  循环拍摄 N 帧             │
        │                                  │     │                            │
        │                                  │     │  2. 每帧捕获完成            │
        │                                  │     │◄───────────────────────────┤
        │                                  │     │                            │
        │                                  │     ▼  onBurstFrameAvailable()   │
        │                                  │ CaptureManager::handleBurstFrame │
        │                                  │     │                            │
        │                                  │     ▼                            │
        │                                  │ ┌─────────────────────────────┐  │
        │                                  │ │ lock(g_burstMutex)          │  │
        │                                  │ │ 调用进度回调                 │  │
        │                                  │ │ g_burstProgressCallbackRef  │  │
        │                                  │ └─────────────────────────────┘  │
        │                                  │     │                            │
        │  3. BurstProgress 回调           │     │                            │
        │◄─────────────────────────────────┼─────┤                            │
        │  { state, capturedFrames, ... }  │     │                            │
        │                                  │     │                            │
        │                                  │     ▼  图像处理完成              │
        │                                  │     │                            │
        │                                  │ ┌─────────────────────────────┐  │
        │                                  │ │ lock(g_burstMutex)          │  │
        │                                  │ │ 调用图像回调                 │  │
        │                                  │ │ g_burstImageCallbackRef     │  │
        │                                  │ └─────────────────────────────┘  │
        │                                  │     │                            │
        │  4. Image 回调 (buffer, isFinal) │     │                            │
        │◄─────────────────────────────────┼─────┤                            │
        │                                  │     │                            │
        │                                  │     ▼  所有帧处理完成            │
        │                                  │     │                            │
        │                                  │ ┌─────────────────────────────┐  │
        │                                  │ │ lock(g_burstMutex)          │  │
        │                                  │ │ state = COMPLETED           │  │
        │                                  │ │ 调用最终图像回调             │  │
        │                                  │ │ isFinal = true              │  │
        │                                  │ └─────────────────────────────┘  │
        │                                  │     │                            │
        │  5. 最终 Image 回调              │     │                            │
        │◄─────────────────────────────────┼─────┘                            │
        │                                  │                                  │
        ▼                                  ▼                                  ▼
```

### 3.2 使用的锁

| 操作 | 锁 | 说明 |
|------|-----|------|
| `startBurstCapture` | `g_burstMutex` | 保存两个回调引用 |
| 进度回调触发 | `g_burstMutex` | 调用进度回调 |
| 图像回调触发 | `g_burstMutex` | 调用图像回调 |
| `cancelBurstCapture` | `g_burstMutex` | 取消连拍 |
| `getBurstState` | `g_burstMutex` | 获取当前状态 |

### 3.3 关键代码

```cpp
// 开始连拍 (napi_expo_camera.cpp:1510-1575)
static napi_value StartBurstCapture(napi_env env, napi_callback_info info) {
    // ...
    // 保存进度回调
    if (argc >= 2 && args[1] != nullptr) {
        std::lock_guard<std::mutex> lock(g_burstMutex);
        g_burstProgressCallbackValid = false;
        // 释放旧引用...
        napi_create_reference(env, args[1], 1, &g_burstProgressCallbackRef);
        g_burstProgressCallbackValid = true;
    }

    // 保存图像回调
    if (argc >= 3 && args[2] != nullptr) {
        std::lock_guard<std::mutex> lock(g_burstMutex);
        g_burstImageCallbackValid = false;
        // 释放旧引用...
        napi_create_reference(env, args[2], 1, &g_burstImageCallbackRef);
        g_burstImageCallbackValid = true;
    }
    // ...
}
```

---

## 4. 两种模式对比

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                         单次拍照 vs 连拍 对比                                 │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  单次拍照 (takePhoto) - 简化设计                                             │
│  ═════════════════════                                                       │
│                                                                              │
│  ArkTS                          NDK                                          │
│  ──────                         ────                                         │
│    │                              │                                          │
│    │ registerImageDataCallback() ├────► g_photoCallbackRef                  │
│    │      (全局注册一次)          │      🔒 g_photoCallbackMutex             │
│    │                              │      (仅注册时加锁，调用时无锁)           │
│    │                              │                                          │
│    │ takePhoto()                  │                                          │
│    ├─────────────────────────────►│                                          │
│    │                              │                                          │
│    │◄───── ImageData 回调 ────────┤  (从 g_photoCallbackRef 调用，无锁)      │
│    │                              │                                          │
│                                                                              │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  连拍 (startBurstCapture) - 完整锁保护                                       │
│  ═════════════════════════                                                   │
│                                                                              │
│  ArkTS                          NDK                                          │
│  ──────                         ────                                         │
│    │                              │                                          │
│    │ startBurstCapture(           │                                          │
│    │   config,                    ├────► g_burstProgressCallbackRef         │
│    │   progressCallback,          │      g_burstImageCallbackRef            │
│    │   imageCallback              │      🔒 g_burstMutex (全程加锁)          │
│    │ )                            │                                          │
│    │                              │                                          │
│    │◄───── BurstProgress 回调 ────┤  (多次，每帧一次，加锁)                   │
│    │      { state, frames, ... }  │                                          │
│    │                              │                                          │
│    │◄───── Image 回调 ────────────┤  (多次：中间预览 + 最终结果，加锁)        │
│    │      (buffer, isFinal)       │      isFinal=false: 中间预览             │
│    │                              │      isFinal=true:  最终结果             │
│    │                              │                                          │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 5. CaptureManager 内部锁

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                           CaptureManager (capture_manager.h)                 │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  成员变量                      │  使用的锁      │  说明                       │
│  ─────────────────────────────┼───────────────┼────────────────────────────  │
│  所有公共方法                  │  mutex_       │  保护内部状态                │
│  - startBurst()               │               │                             │
│  - cancelBurst()              │               │                             │
│  - handlePhotoFrame()         │               │                             │
│  - handleBurstFrame()         │               │                             │
│  - getBurstState()            │               │                             │
│                              │               │                             │
│  taskQueue_                   │  TaskQueue::  │  任务队列内部锁              │
│                              │  mutex_       │                             │
│                              │               │                             │
│  processor_                   │  ImageProcessor│  图像处理器锁               │
│                              │  ::mutex_     │                             │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 6. 锁的层次结构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              锁层次结构                                      │
│  (从外到内获取，避免死锁)                                                    │
└─────────────────────────────────────────────────────────────────────────────┘

  Level 1 (最外层)
  ┌─────────────────────────────────────────────────────────────────────────┐
  │  g_burstMutex             - 连拍相关回调 (全程加锁)                      │
  │  g_photoCallbackMutex     - 单次拍照回调 (仅注册时加锁) ⭐简化           │
  │  g_observerMutex          - 观察者管理                                   │
  └─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
  Level 2
  ┌─────────────────────────────────────────────────────────────────────────┐
  │  CaptureManager::mutex_   - 捕获管理器内部状态                           │
  └─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
  Level 3 (最内层)
  ┌─────────────────────────────────────────────────────────────────────────┐
  │  TaskQueue::mutex_        - 任务队列                                     │
  │  ImageProcessor::mutex_   - 图像处理                                     │
  └─────────────────────────────────────────────────────────────────────────┘

  ⚠️ 注意：不同 Level 1 的锁之间不应该同时持有，避免交叉锁定
```

---

## 7. 线程安全最佳实践

### 7.1 单次拍照回调（简化设计）

```cpp
// ✅ 正确：注册时加锁，之后不再修改
{
    std::lock_guard<std::mutex> lock(g_photoCallbackMutex);
    // 释放旧引用...
    g_photoEnv = env;
    napi_create_reference(env, callback, 1, &g_photoCallbackRef);
}

// ✅ 调用时：无锁，只做空指针检查
static void onPhotoData(...) {
    if (!g_photoCallbackRef || !g_photoEnv) {
        return;  // 未注册或已释放
    }
    // 直接使用回调
}

// ⚠️ 前提条件：必须在初始化时完成注册，之后不再修改
```

### 7.2 连拍回调（完整保护）

```cpp
// ✅ 连拍需要全程加锁，因为可能动态取消
{
    std::lock_guard<std::mutex> lock(g_burstMutex);
    if (g_burstImageCallbackRef && g_burstImageCallbackValid) {
        napi_call_function(...);
    }
}
```

---

## 8. 常见问题

### Q1: 为什么单次拍照和连拍使用不同的锁策略？

| 特性 | 单次拍照 | 连拍 |
|------|---------|------|
| 回调注册 | 全局注册一次 | 每次调用时传入 |
| 锁策略 | 仅注册时加锁 ⭐简化 | 全程加锁 |
| 原因 | 注册后不变，无需运行时保护 | 可能动态取消，需要保护 |
| 图像数量 | 1 张 | N 张 |
| 使用场景 | 简单拍照 | 天文堆叠 |

### Q2: 可以混用两种模式吗？

不建议。同一时间应该只用一种模式：
- 使用 `takePhoto` 时，确保没有正在进行的连拍
- 使用 `startBurstCapture` 时，不要调用 `takePhoto`

### Q3: 回调在哪个线程执行？

回调可能在以下线程执行：
- **相机回调线程**：帧可用时触发
- **处理线程**：图像处理完成后触发
- **不要在回调中执行耗时操作**，否则会阻塞后续帧处理
