# 开发记录

## 开发状态总览

| 模块 | 状态 | 说明 |
|------|------|------|
| 相机基础框架 | ✅ 完成 | 初始化、预览、释放 |
| Surface 切换 | ✅ 完成 | 多场景切换预览流 |
| 单次拍照 | ✅ 完成 | 基础拍照功能 |
| 相机参数控制 | ✅ 完成 | 缩放、对焦 |
| RenderSlot 注册 | ✅ 完成 | 观察者模式 |
| 连拍堆叠 | ✅ 完成 | 多帧拍摄+堆叠处理 |
| 事件系统 | ✅ 完成 | sessionId 关联事件 |

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
2026-03-13（初版） / 2026-03-14（架构重构）

### 核心文件

| 文件 | 职责 | 行数 |
|------|------|------|
| `camera/task_queue.h/cpp` | 线程安全任务队列 | ~150 |
| `camera/image_processor.h/cpp` | 图像编解码、堆叠处理 | ~400 |
| `camera/file_saver.h/cpp` | 文件保存、路径管理 | ~150 |
| `camera/burst_capture.h/cpp` | 连拍协调、状态管理 | ~300 |
| `camera/expo_camera.cpp` | 集成连拍回调 | ~50 (新增) |
| `napi_expo_camera.cpp` | NAPI 新增方法 | ~100 (新增) |

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

#### 3. BurstCapture - 连拍管理

```cpp
enum class BurstState {
    IDLE = 0, CAPTURING = 1, PROCESSING = 2,
    COMPLETED = 3, ERROR = 4, CANCELLED = 5
};

class BurstCapture {  // 单例
    bool startBurst(const BurstConfig& config);
    void cancelBurst();
    void onPhotoCaptured(void* buffer, size_t size, int32_t width, int32_t height);
    bool isBurstActive() const;
};
```

#### 4. ExpoCamera 集成

```cpp
// 在 onPhotoAvailable 中
if (exposhot::BurstCapture::getInstance().isBurstActive()) {
    // 连拍模式: 通知 BurstCapture
    exposhot::BurstCapture::getInstance().onPhotoCaptured(
        bufferCopy, nativeBufferSize, size.width, size.height);
} else {
    // 普通模式: 调用 photoCallback_
    self.photoCallback_(bufferCopy, nativeBufferSize);
}
```

### 验证清单

- [x] 启动连拍，状态变为 CAPTURING
- [x] 每帧处理后收到回调
- [x] 最终结果正确
- [x] 取消功能正常
- [x] 拍照和处理不互相阻塞
- [x] 无内存泄漏

---

## 测试页面

| 页面 | 文件 | 用途 |
|------|------|------|
| 首页 | `pages/Index.ets` | 功能入口 |
| 基础相机 | `pages/TestBasicCamera.ets` | 预览、拍照测试 |
| 连拍测试 | `pages/TestBurstCapture.ets` | 连拍堆叠测试 |
| 完整功能 | `pages/TestFullFeatures.ets` | 全功能集成测试 |

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
| `setFocusMode(mode)` | 设置对焦模式 |
| `getFocusMode()` | 获取对焦模式 |
| `setFocusPoint(x, y)` | 设置对焦点 |
| `getFocusPoint()` | 获取对焦点 |

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
