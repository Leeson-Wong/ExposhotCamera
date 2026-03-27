# 开发记录

## 开发状态总览

| 模块 | 状态 | 说明 |
|------|------|------|
| 相机基础框架 | ✅ 完成 | 初始化、预览、释放 |
| Surface 切换 | ✅ 完成 | 多场景切换预览流 |
| 拍摄管理 | ✅ 完成 | 单拍/连拍统一管理，互斥保证 |
| 模式切换 | ✅ 完成 | 单拍/连拍模式切换，PhotoOutput 重配置 |
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

**之前（直接依赖）**：
```
takePhoto() → ExpoCamera::takePhoto()
    → onPhotoAvailable() → 直接调用 CaptureManager::onSinglePhotoCaptured()
```

**现在（回调解耦）**：
```
takePhoto() → CaptureManager::captureSingle()
    → OH_PhotoOutput_Capture()
    → ExpoCamera::onPhotoAvailable()
    → photoCapturedCallback_ (回调)
    → CaptureManager lambda (根据 currentMode_ 分发)
    → onSinglePhotoCaptured() / onBurstPhotoCaptured()
```

**依赖方向**：
```
CaptureManager → ExpoCamera (正确：上层依赖下层)
ExpoCamera ⇥ CaptureManager (通过回调，无编译依赖)
```

#### 5. 兼容性

- **ArkTS 层 API 不变**：`takePhoto()` 和 `startBurstCapture()` 方法名和签名保持一致
- **行为变化**：连拍进行中调用 `takePhoto()` 现在会被拒绝（之前可能成功）
- **错误处理**：被拒绝时返回空字符串，ArkTS 层判断 `sessionId.isEmpty()`

---

## Bug 修复记录

### 2026-03-23：架构重构 - 依赖方向修正

**问题描述**：
- ExpoCamera 和 CaptureManager 存在互相依赖（循环依赖）
- `expo_camera.cpp` include `capture_manager.h`
- `capture_manager.cpp` include `expo_camera.h`

**重构方案**：

1. **依赖方向修正**：CaptureManager（上层）→ ExpoCamera（下层）

2. **初始化流程统一**：
   ```
   initCamera() → CaptureManager::init()
       ├→ ExpoCamera::init()
       ├→ FileSaver::init()
       └→ 注册回调到 ExpoCamera
   ```

3. **回调机制解耦**：
   - ExpoCamera 不再直接调用 CaptureManager
   - 改为通过注册的回调通知上层
   - 新增 `PhotoCapturedCallback` 和 `PhotoErrorCallback` 类型

**文件变更**：

| 文件 | 变更 |
|------|------|
| `expo_camera.h` | 添加回调类型定义和注册接口 |
| `expo_camera.cpp` | 移除 `#include "capture_manager.h"`，改用回调 |
| `capture_manager.cpp` | init() 中初始化 ExpoCamera 和 FileSaver，注册回调 |
| `napi_expo_camera.cpp` | InitCamera/ReleaseCamera 调用 CaptureManager |

**架构图**：
```
NAPI Layer
    │
    ▼
CaptureManager (业务层)
    │
    ├─→ ExpoCamera (硬件层)
    │       │
    │       └─→ 回调通知
    │
    └─→ FileSaver
```

---

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

#### 4. ExpoCamera 集成（通过回调解耦）

```cpp
// expo_camera.h - 定义回调类型
using PhotoCapturedCallback = std::function<void(void* buffer, size_t size,
                                                  uint32_t width, uint32_t height)>;
using PhotoErrorCallback = std::function<void(int32_t errorCode)>;

// expo_camera.cpp - 通过回调通知上层，不直接依赖 CaptureManager
void ExpoCamera::onPhotoAvailable(...) {
    if (self.photoCapturedCallback_) {
        self.photoCapturedCallback_(bufferCopy, nativeBufferSize, imgWidth, imgHeight);
    }
}

// capture_manager.cpp - 注册回调，根据模式分发
bool CaptureManager::init() {
    // 初始化下层
    ExpoCamera::getInstance().init();
    FileSaver::getInstance().init();

    // 注册回调
    ExpoCamera::getInstance().setPhotoCapturedCallback(
        [this](void* buffer, size_t size, uint32_t width, uint32_t height) {
            if (currentMode_ == CaptureMode::BURST) {
                this->onBurstPhotoCaptured(buffer, size, width, height);
            } else {
                this->onSinglePhotoCaptured(buffer, size, width, height);
            }
        });
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
| 模式切换测试 | `pages/TestCaptureMode.ets` | 单拍/连拍模式切换，PhotoOutput 重配置 |
| Slot 切换测试 | `pages/TestFullFeatures.ets` | 双预览、Slot 切换、完整相机控制 |
| 对焦点测试 | `pages/TestFocusPoint.ets` | 手动对焦点设置、对焦轨迹、预设位置 |
| 对焦放大镜 | `pages/TestFocusMagnifier.ets` | 点击对焦时显示放大镜、精确对焦 |
| 堆叠模拟 | `pages/TestStackSimulate.ets` | 堆叠流程模拟、rawfile 读取 |

---

## 2026-03-24：API 改进与安全区适配

### API 改进

**`initCamera` 参数调整**：

```typescript
// 旧版（mode 可选）
initCamera(resourceManager?: object, mode?: CaptureMode): number;

// 新版（mode 必填）
initCamera(mode: CaptureMode, resourceManager?: object): number;
```

**原因**：
- 可选参数必须在必选参数后面
- 拍摄模式是核心配置，应该在初始化时明确指定

**迁移示例**：
```typescript
// 旧版
nativeCamera.initCamera();  // 默认 SINGLE
nativeCamera.initCamera(resMgr, nativeCamera.CaptureMode.BURST);

// 新版
nativeCamera.initCamera(nativeCamera.CaptureMode.SINGLE);
nativeCamera.initCamera(nativeCamera.CaptureMode.BURST, resMgr);
```

### 安全区适配

所有测试页面统一添加上下安全区：

```typescript
import { SAFE_AREA_TOP, SAFE_AREA_BOTTOM } from '../util/WindowManager';

@Entry
@Component
struct TestPage {
  @StorageProp(SAFE_AREA_TOP) safeTop: number = 0;
  @StorageProp(SAFE_AREA_BOTTOM) safeBottom: number = 0;

  build() {
    Column() {
      // 顶部状态栏
      Row() { ... }
        .padding({ top: this.safeTop + 8, left: 12, right: 12, bottom: 8 })

      // 底部控制区
      Row() { ... }
        .padding({ left: 16, right: 16, top: 8, bottom: this.safeBottom + 16 })
    }
  }
}
```

### 新增测试页面

**TestFocusMagnifier.ets**：对焦放大镜测试

- 点击屏幕时显示放大镜
- 放大镜实时跟随预览内容更新
- 支持拖动更新对焦点
- 自动隐藏机制（2秒后消失）

### 页面命名调整

- `TestFullFeatures.ets` 入口标题改为 "Slot 切换测试"，更准确反映功能

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

### 拍摄模式

| 接口 | 说明 |
|------|------|
| `switchCaptureMode(mode)` | 切换拍摄模式（SINGLE/BURST） |
| `getCaptureMode()` | 获取当前拍摄模式 |
| `canSwitchMode()` | 检查是否可以切换模式 |

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

## 2026-03-23：拍摄模式切换功能

### 新增功能

实现 `switchCaptureMode` 功能，支持在单拍模式和连拍模式之间切换：

- **单拍模式 (SINGLE)**：选择最高分辨率，适合高质量单张拍照
- **连拍模式 (BURST)**：选择接近 1080p 的中等分辨率，平衡性能

### 新增文件

| 文件 | 说明 |
|------|------|
| `pages/TestCaptureMode.ets` | 模式切换测试页面 |

### 新增 API

| 接口 | 说明 |
|------|------|
| `switchCaptureMode(mode)` | 切换拍摄模式 |
| `getCaptureMode()` | 获取当前模式 |
| `canSwitchMode()` | 检查是否可切换 |

### 技术实现

模式切换流程：
1. 停止 Session
2. 移除旧 PhotoOutput
3. 释放旧 PhotoOutput
4. 根据模式选择合适的 photoProfile（单拍选最高分辨率，连拍选 1080p）
5. 创建新 PhotoOutput
6. 注册回调
7. 添加到 Session
8. 提交配置并启动

### 连拍默认参数

- 帧数：5 张
- 曝光时间：2 秒

---

## 待办事项

### 性能与稳定性优化（对比 CustomCamera 分析）

> 参考项目：`F:\opensource\CustomCamera`（纯 ArkTS 实现，性能优异）

#### 问题 1：预览 Profile 选择未优化
- **现状**：`expo_camera.cpp` 直接使用 `previewProfiles[0]`，未根据显示比例选择最优
- **CustomCamera 做法**：根据显示比例排序，选择最匹配的 Profile
- **影响**：可能选择了不合适的分辨率/帧率组合，影响预览流畅度
- **优先级**：高

#### 问题 2：缺少前后台生命周期管理
- **现状**：单例模式，没有前后台自动释放/恢复机制
- **CustomCamera 做法**：
  ```typescript
  onApplicationBackground: () => { cameraManagerRelease(); }
  onApplicationForeground: () => { cameraManagerStart(); }
  ```
- **影响**：后台长时间持有相机资源，可能导致系统回收或稳定性问题
- **优先级**：高

#### 问题 3：缺少休眠/唤醒机制
- **现状**：相机一直运行，没有省电策略
- **CustomCamera 做法**：30秒无操作自动释放相机，用户点击时恢复
- **影响**：功耗高，长时间运行可能积累问题
- **优先级**：中

#### 问题 4：拍照数据获取效率低
- **现状**：手动 `OH_NativeBuffer_Map` → `malloc` → `memcpy` → `OH_NativeBuffer_Unmap`
- **CustomCamera 做法**：使用 `photoAssetAvailable` 回调，系统托管内存
- **影响**：每次拍照有额外内存分配和复制开销
- **优先级**：中（如果是天文堆栈需求，Native 层处理是必要的）

#### 问题 5：连拍线程管理风险
- **现状**：`std::thread captureThread; captureThread.detach();`
- **风险**：detach 后线程不可控，异常时难以恢复
- **CustomCamera 做法**：没有自定义连拍，依赖系统单次拍照
- **优先级**：中（需要连拍功能时必须解决）

#### 问题 6：单例生命周期与页面不同步
- **现状**：`ExpoCamera::getInstance()` 是全局单例
- **问题**：页面销毁时相机可能未正确释放，或释放时机不对
- **CustomCamera 做法**：ViewModel 绑定页面生命周期
- **优先级**：高

#### ~~问题 7：Native 层是否有必要~~
- **结论**：Native 层必要（某些特性只能在 NDK 使用）
- **状态**：已确认，无需讨论

---

### 优先级高
- [x] 完善事件系统，添加 sessionId 关联同一次拍照的事件
- [x] 单次拍照返回 sessionId，便于追踪

### 构建配置
- [ ] **测试/正式环境页面分离（二进制级别）**
  - **目标**：测试环境打包不带正式页面，正式环境打包不带测试页面
  - **方案**：Hvigor 构建脚本在编译前物理移动不需要的页面目录
  - **参考文档**：[HarmonyOS 开发实践——基于hvigor插件定制构建](https://zhuanlan.zhihu.com/p/1896239022227056315)

  **目录结构设计**：
  ```
  entry/src/main/ets/
  ├── pages/           # 测试页面（debug 保留）
  ├── production/      # 正式页面（release 保留）
  └── camera/          # 通用 NDK 核心（始终保留）
  ```

  **关键配置**：
  - `resources/debug/profile/main_pages.json` - 测试环境页面路由
  - `resources/release/profile/main_pages.json` - 正式环境页面路由
  - `entry/hvigorfile.ts` - 使用 `hvigor.nodesEvaluated()` hook 获取 buildMode，移动目录

  **核心代码片段**：
  ```typescript
  import { hapTasks, OhosPluginId, OhosHapContext } from '@ohos/hvigor-ohos-plugin';
  import { hvigor, getNode } from '@ohos/hvigor';

  hvigor.nodesEvaluated(() => {
    const node = getNode(__filename);
    const hapContext = node.getContext(OhosPluginId.OHOS_HAP_PLUGIN) as OhosHapContext;
    const buildMode = hapContext.getBuildMode()?.toLowerCase() || 'debug';

    // 根据 buildMode 移动不需要的目录到 .staging/
    // 编译完成后恢复
  });
  ```

  **注意事项**：
  - `.staging/` 需加入 `.gitignore`
  - 编译异常中断时可能需手动恢复目录
  - IDE 热重载可能受影响

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

## 暗场处理集成方案

### 开发日期
2026-03-26（规划）

### 背景与目标

天文摄影长曝光时，传感器会产生热噪声和固定模式噪声。暗场校正通过减去同样条件下拍摄的暗场（完全遮光）来消除这些噪声。

**目标**：
1. 预处理阶段：接收暗场 DNG，解析并存储到沙箱，返回沙箱地址
2. 拍摄阶段：连拍时自动应用暗场校正

### LibRaw 处理流程中的调用时机

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        LibRaw 处理流程                                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  open_buffer()          打开 DNG 数据，解析元数据                            │
│       │                                                                     │
│       ▼                                                                     │
│  unpack()               解包 RAW 数据到 raw_image[]                          │
│       │                                                                     │
│       │   ┌─────────────────────────────────────────────────────────────┐   │
│       │   │  ★ 暗场减法就在这里 ★                                        │   │
│       │   │  processor.subtract(raw);  // 直接操作 raw_image[]          │   │
│       │   │  减法在 Bayer RAW 数据上进行，还未去马赛克                    │   │
│       │   └─────────────────────────────────────────────────────────────┘   │
│       │                                                                     │
│       ▼                                                                     │
│  dcraw_process()        去马赛克、白平衡、色彩转换等                         │
│       │                                                                     │
│       ▼                                                                     │
│  dcraw_make_mem_image() 输出处理后的 RGB 图像                               │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

**为什么必须在 `unpack()` 之后、`dcraw_process()` 之前？**

1. **unpack() 之后**：此时 `raw.imgdata.rawdata.raw_image` 包含原始 Bayer 数据
2. **dcraw_process() 之前**：去马赛克等操作会改变像素值，必须在原始数据上减暗场
3. **关键**：暗场数据也是 Bayer RAW，必须同格式相减

### 架构设计

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            暗场处理架构                                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                        ArkTS UI Layer                                │   │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐  │   │
│  │  │ 导入暗场 DNG     │  │ 查看暗场状态     │  │ 拍摄时自动应用   │  │   │
│  │  └────────┬─────────┘  └──────────────────┘  └──────────────────┘  │   │
│  │           │                                                         │   │
│  └───────────┼─────────────────────────────────────────────────────────┘   │
│              │ NAPI                                                        │
│              ▼                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │                         C++ Native Layer                              │ │
│  │                                                                       │ │
│  │  ┌─────────────────────────────────────────────────────────────────┐ │ │
│  │  │                    DarkFieldManager (新增)                       │ │ │
│  │  │                                                                  │ │ │
│  │  │  - loadDarkFrame(dngData, size) → path                          │ │ │
│  │  │  - loadDarkFieldBin(path)                                       │ │ │
│  │  │  - getDarkFieldPath() → path                                    │ │ │
│  │  │  - isDarkFieldLoaded() → bool                                   │ │ │
│  │  │  - clearDarkField()                                             │ │ │
│  │  │                                                                  │ │ │
│  │  │  内部使用:                                                       │ │ │
│  │  │  - DarkFieldPreprocessor (预处理)                               │ │ │
│  │  │  - DarkFieldProcessor (实时减法)                                │ │ │
│  │  └─────────────────────────────────────────────────────────────────┘ │ │
│  │                                │                                      │ │
│  │                                ▼                                      │ │
│  │  ┌─────────────────────────────────────────────────────────────────┐ │ │
│  │  │                    ImageProcessor (修改)                         │ │ │
│  │  │                                                                  │ │ │
│  │  │  dngToBGRA16() {                                                │ │ │
│  │  │      raw.open_buffer(...);                                      │ │ │
│  │  │      raw.unpack();                                              │ │ │
│  │  │      // ★ 新增：暗场减法                                        │ │ │
│  │  │      if (darkFieldManager_.isLoaded()) {                        │ │ │
│  │  │          darkFieldManager_.subtract(raw);                       │ │ │
│  │  │      }                                                          │ │ │
│  │  │      raw.dcraw_process();                                       │ │ │
│  │  │      ...                                                        │ │ │
│  │  │  }                                                              │ │ │
│  │  └─────────────────────────────────────────────────────────────────┘ │ │
│  │                                                                       │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │                           沙箱存储                                    │ │
│  │                                                                       │ │
│  │  /data/service/el2/user/0/com.exposhot.camera/files/                 │ │
│  │  ├── dark_field.bin          (预处理后的暗场数据)                     │ │
│  │  └── dark_field_meta.json    (元数据：尺寸、Bayer模式等)              │ │
│  │                                                                       │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 新增文件

| 文件 | 职责 |
|------|------|
| `camera/dark_field_manager.h` | 暗场管理器头文件 |
| `camera/dark_field_manager.cpp` | 暗场管理器实现 |
| `camera/dark/dark_field.hpp` | 已存在，预处理+处理核心 |

### 新增 API

#### NAPI 接口

```cpp
// 导入暗场 DNG（支持多帧）
// 返回：沙箱路径，失败返回空字符串
std::string LoadDarkFrame(void* dngData, size_t size);

// 添加更多暗场帧（叠加平均，减少噪声）
bool AddDarkFrame(void* dngData, size_t size);

// 完成暗场预处理（平均并保存）
bool FinalizeDarkField();

// 获取当前暗场路径
std::string GetDarkFieldPath();

// 检查暗场是否已加载
bool IsDarkFieldLoaded();

// 清除暗场
void ClearDarkField();
```

#### ArkTS 接口

```typescript
interface DarkFieldAPI {
  // 单帧暗场
  loadDarkFrame(dngBuffer: ArrayBuffer): Promise<string>;

  // 多帧暗场（推荐）
  startDarkFieldCapture(): void;
  addDarkFrame(dngBuffer: ArrayBuffer): Promise<boolean>;
  finalizeDarkField(): Promise<string>;

  // 状态查询
  getDarkFieldPath(): string | null;
  isDarkFieldLoaded(): boolean;
  clearDarkField(): void;
}
```

### 数据流程

#### 1. 暗场预处理流程

```
用户导入暗场 DNG
       │
       ▼
┌──────────────────────────────────────────────────────────┐
│  LoadDarkFrame(dngData, size)                            │
│                                                          │
│  1. LibRaw.open_buffer(dngData, size)                   │
│  2. LibRaw.unpack()                                      │
│  3. 提取 raw_image[] + 元数据                            │
│  4. DarkFieldPreprocessor.addDarkData(...)              │
│  5. (多帧时) DarkFieldPreprocessor.average()            │
│  6. 生成文件名: dark_field_{timestamp}.bin              │
│  7. DarkFieldPreprocessor.save(sandboxPath)             │
│  8. 记录当前暗场路径                                      │
│                                                          │
└──────────────────────────────────────────────────────────┘
       │
       ▼
  返回沙箱路径: /data/.../files/dark_field_xxx.bin
```

#### 2. 拍摄时暗场处理流程

```
连拍捕获 DNG
       │
       ▼
┌──────────────────────────────────────────────────────────┐
│  ImageProcessor::dngToBGRA16(dngData, size)              │
│                                                          │
│  1. LibRaw.open_buffer(dngData, size)                   │
│  2. LibRaw.unpack()                                      │
│  3. ★ if (darkFieldManager_.isLoaded())                 │
│        darkFieldManager_.subtract(raw);  // 暗场减法     │
│  4. LibRaw.dcraw_process()                              │
│  5. LibRaw.dcraw_make_mem_image()                       │
│  6. 转换为 BGRA16                                        │
│                                                          │
└──────────────────────────────────────────────────────────┘
       │
       ▼
  堆叠处理（后续帧）
```

### 代码集成点

#### 1. ImageProcessor 修改

```cpp
// image_processor.h
#include "dark/dark_field.hpp"

class ImageProcessor {
public:
    // 新增：设置暗场管理器引用
    void setDarkFieldManager(DarkFieldManager* manager) {
        darkFieldManager_ = manager;
    }

private:
    DarkFieldManager* darkFieldManager_ = nullptr;
};

// image_processor.cpp
Bgra16Raw ImageProcessor::dngToBGRA16(const void *dng_data, size_t dng_size) {
    LibRaw processor;

    int ret = processor.open_buffer(const_cast<void *>(dng_data), dng_size);
    if (ret != LIBRAW_SUCCESS)
        throw std::runtime_error(std::string("open_buffer 失败: ") + libraw_strerror(ret));

    ret = processor.unpack();
    if (ret != LIBRAW_SUCCESS)
        throw std::runtime_error(std::string("unpack 失败: ") + libraw_strerror(ret));

    // ★ 新增：暗场减法
    if (darkFieldManager_ && darkFieldManager_->isLoaded()) {
        darkFieldManager_->subtract(processor);
    }

    // 后续处理不变...
    libraw_output_params_t &params = processor.imgdata.params;
    params.output_bps = 16;
    // ...
}
```

#### 2. DarkFieldManager 新增

```cpp
// dark_field_manager.h
#pragma once

#include "dark/dark_field.hpp"
#include "libraw.h"
#include <string>
#include <mutex>

namespace exposhot {

class DarkFieldManager {
public:
    static DarkFieldManager& getInstance();

    // 加载暗场 DNG（单帧或多帧）
    bool loadDarkFrame(const void* dngData, size_t size);
    bool addDarkFrame(const void* dngData, size_t size);
    bool finalizeDarkField();  // 多帧取平均并保存

    // 加载已预处理的暗场文件
    bool loadFromFile(const std::string& path);

    // 对 LibRaw 对象执行暗场减法
    bool subtract(LibRaw& raw);

    // 状态查询
    bool isLoaded() const;
    std::string getPath() const;
    uint32_t getWidth() const;
    uint32_t getHeight() const;

    // 清除
    void clear();

private:
    DarkFieldManager();
    ~DarkFieldManager() = default;

    std::mutex mutex_;
    DarkFieldPreprocessor preprocessor_;
    DarkFieldProcessor processor_;
    std::string currentPath_;
    bool isPreprocessing_ = false;  // 正在添加暗场帧
};

} // namespace exposhot
```

#### 3. NAPI 绑定

```cpp
// napi_expo_camera.cpp 新增

static napi_value LoadDarkFrame(napi_env env, napi_callback_info info) {
    // 1. 获取 ArrayBuffer 参数
    // 2. 调用 DarkFieldManager::loadDarkFrame()
    // 3. 返回沙箱路径字符串
}

static napi_value AddDarkFrame(napi_env env, napi_callback_info info) {
    // 多帧暗场时使用
}

static napi_value FinalizeDarkField(napi_env env, napi_callback_info info) {
    // 完成预处理
}

static napi_value GetDarkFieldPath(napi_env env, napi_callback_info info) {
    // 返回当前暗场路径
}

static napi_value IsDarkFieldLoaded(napi_env env, napi_callback_info info) {
    // 返回 bool
}

static napi_value ClearDarkField(napi_env env, napi_callback_info info) {
    // 清除暗场
}

// 注册到 exports
napi_property_descriptor desc[] = {
    // ... 现有接口
    {"loadDarkFrame", nullptr, LoadDarkFrame, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"addDarkFrame", nullptr, AddDarkFrame, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"finalizeDarkField", nullptr, FinalizeDarkField, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"getDarkFieldPath", nullptr, GetDarkFieldPath, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"isDarkFieldLoaded", nullptr, IsDarkFieldLoaded, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"clearDarkField", nullptr, ClearDarkField, nullptr, nullptr, nullptr, napi_default, nullptr},
};
```

### 沙箱路径管理

```cpp
// file_saver.h 新增接口
class FileSaver {
public:
    // 获取暗场存储目录
    std::string getDarkFieldDir() const {
        return saveDir_ + "/dark_fields";
    }

    // 生成暗场文件名
    std::string generateDarkFieldPath() const {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        return getDarkFieldDir() + "/dark_field_" + std::to_string(timestamp) + ".bin";
    }
};
```

### 文件格式 (dark_field.bin)

已定义在 `dark_field.hpp` 中：

```
Header: 32 bytes
  [0-3]   Magic: "DARK"
  [4-7]   Version: uint32_t (1)
  [8-11]  Width: uint32_t
  [12-15] Height: uint32_t
  [16-19] Bayer Pattern (0=RGGB, 1=BGGR, 2=GRBG, 3=GBRG)
  [20-23] Black Level: uint32_t
  [24-31] Reserved/CRC32
Data:
  [32+]   Raw pixels (width * height * 2 bytes, uint16_t)
```

### 开发步骤

| 步骤 | 内容 | 状态 |
|------|------|------|
| 1 | 创建 `dark_field_manager.h/cpp` | ⬜ |
| 2 | 实现 `loadDarkFrame()` - 单帧 DNG 解析并保存 | ⬜ |
| 3 | 实现 `addDarkFrame()` + `finalizeDarkField()` - 多帧叠加平均 | ⬜ |
| 4 | 实现 `subtract()` - 对 LibRaw 执行暗场减法 | ⬜ |
| 5 | 修改 `ImageProcessor::dngToBGRA16()` 集成暗场减法 | ⬜ |
| 6 | 添加 NAPI 绑定 | ⬜ |
| 7 | 添加 ArkTS 接口和类型定义 | ⬜ |
| 8 | 创建测试页面 `TestDarkField.ets` | ⬜ |
| 9 | 端到端测试 | ⬜ |

### 验证清单

- [ ] 单帧暗场导入成功
- [ ] 多帧暗场叠加平均成功
- [ ] 暗场文件正确保存到沙箱
- [ ] 返回的沙箱路径可访问
- [ ] 拍摄时暗场减法正确执行
- [ ] 尺寸不匹配时正确处理（警告或跳过）
- [ ] 清除暗场后拍摄正常（不减暗场）
- [ ] 暗场效果验证（对比处理前后图像）

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
