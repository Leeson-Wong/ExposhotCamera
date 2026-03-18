# 开发记录

## 开发状态总览

| 模块 | 状态 | 说明 |
|------|------|------|
| 相机基础框架 | ✅ 完成 | 初始化、预览、释放 |
| Surface 切换 | ✅ 完成 | 多场景切换预览流 |
| 拍摄管理 | ✅ 完成 | 单拍/连拍统一管理，互斥保证 |
| 相机参数控制 | ✅ 完成 | 缩放、对焦 |
| RenderSlot 注册 | ✅ 完成 | 观察者模式 |
| 图像处理 | ✅ 完成 | 解码、堆叠、编码 |
| 事件系统 | ✅ 完成 | sessionId 关联事件 |

---

## 最新重构：拍摄动作统一到 CaptureManager

### 重构日期
2026-03-16

### 重构目标

将所有拍摄动作（单次拍照、连拍）统一到 `CaptureManager` 模块管理，在 CaptureManager 内部实现单拍和连拍的互斥控制。

### 重构内容

#### 1. 文件变更

| 操作 | 文件 | 说明 |
|------|------|------|
| 新建 | `camera/capture_manager.h` | 拍摄管理器头文件 |
| 新建 | `camera/capture_manager.cpp` | 拍摄管理器实现 |
| 删除 | `camera/burst_capture.h` | 旧文件，已被 capture_manager.h 替代 |
| 删除 | `camera/burst_capture.cpp` | 旧文件，已被 capture_manager.cpp 替代 |
| 修改 | `camera/expo_camera.h` | 移除 takePhoto()，添加 getPhotoOutput() |
| 修改 | `camera/expo_camera.cpp` | 委托拍照逻辑给 CaptureManager |
| 修改 | `napi/napi_expo_camera.cpp` | 更新 NAPI 绑定使用 CaptureManager |
| 修改 | `CMakeLists.txt` | 更新源文件引用 |

#### 2. 状态机扩展

```cpp
// 之前 (BurstState)
enum class BurstState {
    IDLE = 0, CAPTURING = 1, PROCESSING = 2,
    COMPLETED = 3, ERROR = 4, CANCELLED = 5
};

// 现在 (CaptureState)
enum class CaptureState {
    IDLE = 0,              // 空闲
    SINGLE_CAPTURING = 1,  // 单次拍照中 (新增)
    BURST_CAPTURING = 2,   // 连拍拍摄中
    PROCESSING = 3,        // 连拍处理中
    COMPLETED = 4,         // 连拍完成
    ERROR = 5,             // 错误
    CANCELLED = 6          // 已取消
};
```

#### 3. 互斥实现

```cpp
// 检查是否有拍摄进行中（单拍或连拍）
bool isCaptureActive() const {
    CaptureState s = state_.load();
    return s == SINGLE_CAPTURING ||
           s == BURST_CAPTURING ||
           s == PROCESSING;
}

// 单次拍照入口
std::string captureSingle() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isCaptureActive()) {
        OH_LOG_WARN(LOG_APP, "Capture already in progress");
        return "";  // 拒绝
    }

    state_.store(CaptureState::SINGLE_CAPTURING);
    // ... 触发拍照
}

// 连拍入口
std::string startBurst(const BurstConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isCaptureActive()) {
        OH_LOG_WARN(LOG_APP, "Capture already in progress");
        return "";  // 拒绝
    }

    state_.store(CaptureState::BURST_CAPTURING);
    // ... 启动连拍
}
```

#### 4. 数据流向变化

**之前**：
```
takePhoto() → ExpoCamera::takePhoto()
    → onPhotoAvailable() → 检查 isBurstActive()
        → 分发到 BurstCapture 或 photoCallback_
```

**现在**：
```
takePhoto() → CaptureManager::captureSingle()
    → onPhotoAvailable() → CaptureManager::onSinglePhotoCaptured()
        → singlePhotoCallback_
```

#### 5. 兼容性

- **ArkTS 层 API 不变**：`takePhoto()` 和 `startBurstCapture()` 方法名和签名保持一致
- **行为变化**：连拍进行中调用 `takePhoto()` 现在会被拒绝（之前可能成功）
- **错误处理**：被拒绝时返回空字符串，ArkTS 层判断 `sessionId.isEmpty()`

---

## Bug 修复记录

### 2026-03-18：第二次拍照 IPC 崩溃修复

**问题描述**：
- **症状**：首次拍照成功，第二次拍照必定崩溃
- **崩溃信息**：`Signal:SIGSEGV(SEGV_MAPERR)@0x006b00630061006a`（地址解码为 "jack"）
- **崩溃位置**：Binder IPC 线程（`OHOS::ProcessSkeleton::AttachInvokerProcInfo`）

**根本原因**：
`napi_expo_camera.cpp` 中的全局回调变量没有互斥锁保护，导致数据竞争：

```cpp
// ❌ 原始代码（无锁保护）
static napi_ref g_photoCallbackRef = nullptr;
static napi_env g_env = nullptr;  // ← 与线程绑定的环境
static size_t g_photoSize = 0;

static void onPhotoData(...) {
    // 直接访问全局变量，没有锁保护
    if (!g_photoCallbackRef || !g_env) { ... }
}
```

**修复方案**：

1. 添加互斥锁保护
2. 添加有效标志 (`g_callbackValid`)
3. 在锁内复制数据，异步回调使用副本

```cpp
// ✅ 修复后的代码
static bool g_callbackValid = false;
static std::mutex g_callbackMutex;

static void onPhotoData(...) {
    // 1. 在锁保护下获取数据
    napi_env callbackEnv = nullptr;
    napi_ref callbackRef = nullptr;
    bool callbackValid = false;

    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        callbackValid = g_callbackValid;
        callbackEnv = g_env;
        callbackRef = g_photoCallbackRef;
    }  // ← 锁释放

    // 2. 检查有效性
    if (!callbackValid || !callbackRef || !callbackEnv) {
        return;
    }

    // 3. 创建异步工作时使用拷贝的数据
    napi_create_async_work(callbackEnv, ...);
}
```

**修复范围**：
- `onPhotoData()` - 单次拍照回调
- `onPhotoErrorCallback()` - 拍照错误回调
- `onBurstImageCallback()` - 连拍图像回调
- `onBurstProgressCallback()` - 连拍进度回调
- `RegisterImageDataCallback()` - 回调注册
- `RegisterPhotoErrorCallback()` - 错误回调注册
- `StartBurstCapture()` - 连拍启动

**验证**：
- ✅ 连续多次拍照不再崩溃
- ✅ 连续多次连拍不再崩溃
- ✅ 混合单拍和连拍不再崩溃

---

### 2026-03-18：观察者回调通知修复

**问题描述**：
- **症状**：有多个观察者时，只有活跃 slot 的观察者收到通知
- **影响**：非活跃页面的 UI 状态不更新（如"无预览"提示不消失）

**根本原因**：
`onPreviewObserver` 只查找并调用活跃 slot 的回调：

```cpp
// ❌ 原始代码
static void onPreviewObserver(const std::string& activeSlotId, const std::string& activeSurfaceId) {
    auto it = g_observerCallbacks.find(activeSlotId);  // ← 只查找活跃 slot
    if (it == g_observerCallbacks.end()) {
        return;  // ← 非活跃观察者直接返回
    }
    // 只调用活跃观察者的回调...
}
```

**修复方案**：

```cpp
// ✅ 修复后：通知所有观察者
static void onPreviewObserver(const std::string& activeSlotId, const std::string& activeSurfaceId) {
    std::lock_guard<std::mutex> lock(g_observerMutex);

    for (auto& pair : g_observerCallbacks) {
        ObserverCallbackInfo &info = pair.second;
        if (!info.callbackRef) continue;

        // 为每个观察者调用回调
        napi_call_function(...);
    }
}
```

**验证**：
- ✅ 所有观察者都能收到预览状态变化通知
- ✅ 页面切换时 UI 状态正确更新
- ✅ `hasPreview` 状态在所有页面正确同步

---

### 2026-03-18：新增对焦点测试页面

**新增文件**：
- `pages/TestFocusPoint.ets` - 对焦点专门测试页面

**功能特性**：
1. 点击预览画面设置对焦点（所有对焦模式支持）
2. 黄色 '+' 标记显示当前对焦点位置
3. 对焦轨迹可视化（最近 10 个点击点）
4. 预设位置快速切换（中心、左上、右上、底部）
5. 对焦距离滑块调节（0-10m）
6. 拍照验证对焦效果

**技术要点**：
- XComponent 不直接响应点击事件，需要叠加透明 Column 处理
- 透明点击层需要始终渲染（不依赖 `hasPreview` 状态）
- 自动对焦模式切换以确保对焦点生效
- 对焦轨迹使用渐变透明度显示点击历史

**XComponent 点击事件处理**：

```typescript
// ❌ 错误：直接在 XComponent 上绑定 onClick
XComponent({ ... })
  .onClick((event: ClickEvent) => { ... })  // 不会触发

// ✅ 正确：叠加透明 Column 处理点击
Stack() {
  XComponent({ ... })

  // 透明点击层 - 始终渲染
  Column()
    .width('100%')
    .height('100%')
    .backgroundColor('transparent')
    .onClick((event: ClickEvent) => {
      if (!this.hasPreview) {
        this.cameraStatus = '请等待预览启动';
        return;
      }
      // 处理点击...
    })
}
```

---

## 相机模块开发阶段

### 阶段 1：C++ 基础框架搭建 ✅

| 步骤 | 内容 | 状态 |
|------|------|------|
| 1.1 | 创建 `entry/src/main/cpp/` 目录结构 | ✅ |
| 1.2 | 创建 `CMakeLists.txt`，配置相机依赖 | ✅ |
| 1.3 | 创建 `expo_camera.h/cpp` 空壳类（单例模式） | ✅ |
| 1.4 | 创建 `napi_expo_camera.cpp`，导出空函数到 ArkTS | ✅ |
| 1.5 | 修改 `build-profile.json5` 启用 Native 编译 | ✅ |
| 1.6 | 修改 `module.json5` 添加相机权限 | ✅ |

### 阶段 2：相机初始化与释放 ✅

| 步骤 | 内容 | 状态 |
|------|------|------|
| 2.1 | 实现 `init()`：创建 CameraManager，获取相机列表 | ✅ |
| 2.2 | 创建 CameraInput，选择后置摄像头 | ✅ |
| 2.3 | 创建 CaptureSession，配置 Input | ✅ |
| 2.4 | 实现 `release()`：释放所有资源 | ✅ |
| 2.5 | 添加错误处理和日志 | ✅ |

### 阶段 3：预览功能 ✅

| 步骤 | 内容 | 状态 |
|------|------|------|
| 3.1 | ArkTS 创建 XComponent，获取 surfaceId | ✅ |
| 3.2 | 实现 `startPreview(surfaceId)` | ✅ |
| 3.3 | 实现 `stopPreview()` | ✅ |

### 阶段 4：Surface 切换 ✅

| 步骤 | 内容 | 状态 |
|------|------|------|
| 4.1 | 实现 `switchSurface(newSurfaceId)` | ✅ |
| 4.2 | 处理 Session beginConfig/commitConfig 流程 | ✅ |
| 4.3 | ArkTS 页面添加切换按钮测试 | ✅ |

### 阶段 5：拍照功能 ✅

| 步骤 | 内容 | 状态 |
|------|------|------|
| 5.1 | 创建 PhotoOutput，配置拍照回调 | ✅ |
| 5.2 | 实现 `takePhoto(callback)` | ✅ |
| 5.3 | 回调通过 NAPI 返回 ArrayBuffer 给 ArkTS | ✅ |

### 阶段 6：相机参数控制 ✅

| 步骤 | 内容 | 状态 |
|------|------|------|
| 6.1 | 实现 `setZoomRatio()` / `getZoomRatio()` | ✅ |
| 6.2 | 实现 `setFocusMode()` | ✅ |
| 6.3 | ArkTS 添加滑块控制 | ✅ |

### 阶段 7：稳定性优化 ✅

| 步骤 | 内容 | 状态 |
|------|------|------|
| 7.1 | 添加线程锁（std::mutex） | ✅ |
| 7.2 | 状态机检查（防止重复初始化等） | ✅ |
| 7.3 | 错误恢复机制 | ✅ |

### 阶段 8：RenderSlot 注册机制 ✅

| 步骤 | 内容 | 状态 |
|------|------|------|
| 8.1 | 创建 `render_slot.h` 数据结构 | ✅ |
| 8.2 | ExpoCamera 添加 RenderSlot 管理方法 | ✅ |
| 8.3 | 实现 `registerSlot()` / `unregisterSlot()` | ✅ |
| 8.4 | 实现 `switchToSlot()` 及回调通知 | ✅ |
| 8.5 | 实现状态订阅 `subscribeState()` | ✅ |
| 8.6 | NAPI 绑定新接口 | ✅ |

---

## 连拍堆叠开发记录

### 开发日期
2026-03-13（初版） / 2026-03-14（架构重构） / 2026-03-16（统一到 CaptureManager）

### 核心文件

| 文件 | 职责 | 行数 |
|------|------|------|
| `camera/task_queue.h/cpp` | 线程安全任务队列 | ~150 |
| `camera/image_processor.h/cpp` | 图像编解码、堆叠处理 | ~400 |
| `camera/file_saver.h/cpp` | 文件保存、路径管理 | ~150 |
| `camera/capture_manager.h/cpp` | 拍摄管理（单拍+连拍） | ~400 |
| `camera/expo_camera.cpp` | 提供相机硬件能力 | ~1100 |
| `napi_expo_camera.cpp` | NAPI 桥接 | ~1050 |

### 关键实现

#### 1. TaskQueue - 线程安全队列

```cpp
struct ImageTask {
    int32_t taskId;       // 第几张 (0-based)
    void* buffer;         // 图像数据（所有权转移）
    size_t size;          // 数据大小
    int32_t width;        // 图像宽度
    int32_t height;       // 图像高度
    bool isFirst;         // 是否是基准帧
};

class TaskQueue {
    void enqueue(ImageTask&& task);   // 生产者
    void start(TaskHandler handler);  // 启动消费线程
    void stop();                      // 停止
    void clear();                     // 清空队列
};
```

#### 2. ImageProcessor - 图像处理

```cpp
class ImageProcessor {
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
};
```

堆叠算法：平均叠加
- 累积缓冲区使用 `float` 保持精度
- 第一帧直接复制，后续帧累加
- 输出时除以帧数取平均

#### 3. CaptureManager - 拍摄管理

```cpp
enum class CaptureState {
    IDLE = 0,              // 空闲
    SINGLE_CAPTURING = 1,  // 单次拍照中
    BURST_CAPTURING = 2,   // 连拍拍摄中
    PROCESSING = 3,        // 连拍处理中
    COMPLETED = 4,         // 连拍完成
    ERROR = 5,             // 错误
    CANCELLED = 6          // 已取消
};

class CaptureManager {  // 单例
    // 单次拍照
    std::string captureSingle();
    void onSinglePhotoCaptured(void* buffer, size_t size, int32_t width, int32_t height);

    // 连拍
    std::string startBurst(const BurstConfig& config);
    void cancelBurst();
    void onBurstPhotoCaptured(void* buffer, size_t size, int32_t width, int32_t height);

    // 状态查询
    bool isCaptureActive() const;  // 包含单拍和连拍
    bool isBurstActive() const;   // 仅连拍（兼容旧接口）
};
```

#### 4. ExpoCamera 集成

```cpp
// 在 onPhotoAvailable 中，统一路由到 CaptureManager
auto& captureManager = exposhot::CaptureManager::getInstance();

if (captureManager.isBurstActive()) {
    // 连拍模式
    captureManager.onBurstPhotoCaptured(
        bufferCopy, nativeBufferSize, size.width, size.height);
} else {
    // 单拍模式
    captureManager.onSinglePhotoCaptured(
        bufferCopy, nativeBufferSize, size.width, size.height);
}
```

### 验证清单

#### 基础功能
- [x] 单次拍照成功
- [x] 连拍成功
- [x] 取消功能正常
- [x] 拍照和处理不互相阻塞
- [x] 无内存泄漏

#### 互斥测试
- [x] 连拍进行中调用 takePhoto() → 被拒绝
- [x] 单拍进行中调用 startBurstCapture() → 被拒绝
- [x] 连续快速调用 takePhoto() → 第二次被拒绝
- [x] 连拍进行中调用 startBurstCapture() → 被拒绝

#### 状态转换
- [x] IDLE → SINGLE_CAPTURING → IDLE
- [x] IDLE → BURST_CAPTURING → PROCESSING → COMPLETED → IDLE
- [x] 任意状态 → CANCELLED → IDLE

---

## 测试页面

| 页面 | 文件 | 用途 |
|------|------|------|
| 首页 | `pages/Index.ets` | 功能入口 |
| 基础相机 | `pages/TestBasicCamera.ets` | 预览、拍照、缩放、对焦测试 |
| 连拍测试 | `pages/TestBurstCapture.ets` | 连拍堆叠、进度追踪、缩略图预览 |
| 完整功能 | `pages/TestFullFeatures.ets` | 双预览、Slot 切换、完整相机控制 |
| 对焦点测试 | `pages/TestFocusPoint.ets` | 手动对焦点设置、对焦轨迹、预设位置 |

---

## API 汇总

### 基础控制

| 接口 | 说明 |
|------|------|
| `initCamera()` | 初始化相机 |
| `releaseCamera()` | 释放相机 |
| `startPreview(surfaceId)` | 启动预览 |
| `stopPreview()` | 停止预览 |
| `switchSurface(surfaceId)` | 切换预览 Surface |
| `takePhoto()` | 单次拍照 |
| `isPhotoOutputReady()` | 检查拍照就绪 |

### 相机参数

| 接口 | 说明 |
|------|------|
| `setZoomRatio(ratio)` | 设置缩放比例 |
| `getZoomRatio()` | 获取缩放比例 |
| `getZoomRatioRange()` | 获取缩放范围 |
| `isZoomSupported()` | 是否支持缩放 |
| `setFocusMode(mode)` | 设置对焦模式（MANUAL/AUTO/CONTINUOUS_AUTO） |
| `getFocusMode()` | 获取对焦模式 |
| `setFocusPoint(x, y)` | 设置对焦点（x, y 范围 0-1） |
| `getFocusPoint()` | 获取对焦点 |
| `setFocusDistance(distance)` | 设置对焦距离（部分设备支持） |
| `getFocusDistance()` | 获取对焦距离 |
| `getFocusDistanceRange()` | 获取对焦距离范围 |

### 观察者模式

| 接口 | 说明 |
|------|------|
| `registerObserver(surfaceId, callback)` | 注册观察者 |
| `unregisterObserver(slotId)` | 注销观察者 |
| `switchToSlot(slotId)` | 切换预览到指定 slot |
| `subscribeState(callback)` | 订阅相机状态 |
| `unsubscribeState()` | 取消订阅 |

### 连拍堆叠

| 接口 | 说明 |
|------|------|
| `startBurstCapture(config, progressCallback, imageCallback)` | 开始连拍 |
| `cancelBurstCapture()` | 取消连拍 |
| `getBurstState()` | 获取连拍状态 |
| `setBurstImageSize(width, height)` | 设置图像尺寸 |

### 事件回调

| 接口 | 说明 |
|------|------|
| `registerPhotoEventCallback(callback)` | 注册拍照事件回调 |
| `registerProcessEventCallback(callback)` | 注册处理事件回调 |
| `registerImageDataCallback(callback)` | 注册图像数据回调 |
| `registerPhotoErrorCallback(callback)` | 注册拍照错误回调 |

### 文件保存

| 接口 | 说明 |
|------|------|
| `saveImageToFile(buffer, filename?)` | 保存图像到文件 |
| `getImageSaveDir()` | 获取保存目录 |

---

## 待办事项

### 优先级高
- [x] 完善事件系统，添加 sessionId 关联同一次拍照的事件
- [x] 单次拍照返回 sessionId，便于追踪

### 优化项
- [ ] 添加更复杂的堆叠算法（对齐、去鬼影）
- [ ] 支持 RAW 格式输出
- [ ] 添加进度百分比显示
- [ ] 优化大图处理性能
- [ ] 添加错误重试机制

### 测试
- [ ] 多页面切换测试
- [ ] 内存泄漏检测
- [ ] 异常场景测试（相机被占用、权限拒绝等）

---

## 错误码

| 错误码 | 说明 |
|--------|------|
| 0 | 成功 |
| -1 | 通用错误 |
| -2 | 相机未初始化 |
| -3 | 相机已被占用 |
| -4 | 参数无效 |
| -5 | 操作不支持 |

---

## 注意事项

1. **HarmonyOS Image Native API**: 使用的 API 可能需要根据实际文档调整
2. **内存管理**: ImageTask 析构时自动释放 buffer，避免内存泄漏
3. **线程安全**: 使用 mutex 和 atomic 变量保护共享状态
4. **NAPI 异步回调**: 使用 `napi_async_work` 确保在主线程调用 JS 回调
5. **职责分离**: ImageProcessor 只负责图像处理，FileSaver 只负责文件IO
6. **相机权限**: 确保已申请 `ohos.permission.CAMERA` 权限
7. **单例模式**: 相机服务是单例，整个应用共享一个实例
8. **Surface 有效期**: 确保 XComponent 销毁前先调用 `unregisterObserver()`
