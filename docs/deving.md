# NDK 相机服务开发计划

## 阶段 1：C++ 基础框架搭建 ✅
**验收标准**：项目能编译通过，NAPI 接口可调用

| 步骤 | 内容 | 状态 |
|------|------|------|
| 1.1 | 创建 `entry/src/main/cpp/` 目录结构 | ✅ |
| 1.2 | 创建 `CMakeLists.txt`，配置相机依赖 | ✅ |
| 1.3 | 创建 `expo_camera.h/cpp` 空壳类（单例模式） | ✅ |
| 1.4 | 创建 `napi_expo_camera.cpp`，导出空函数到 ArkTS | ✅ |
| 1.5 | 修改 `build-profile.json5` 启用 Native 编译 | ✅ |
| 1.6 | 修改 `module.json5` 添加相机权限 | ✅ |

**验收**：ArkTS 调用 `nativeCamera.initCamera()` 返回成功码

---

## 阶段 2：相机初始化与释放 ✅
**验收标准**：相机能正常初始化和释放

| 步骤 | 内容 | 状态 |
|------|------|------|
| 2.1 | 实现 `init()`：创建 CameraManager，获取相机列表 | ✅ |
| 2.2 | 创建 CameraInput，选择后置摄像头 | ✅ |
| 2.3 | 创建 CaptureSession，配置 Input | ✅ |
| 2.4 | 实现 `release()`：释放所有资源 | ✅ |
| 2.5 | 添加错误处理和日志 | ✅ |

**验收**：调用 `initCamera()` 后日志显示相机初始化成功，`releaseCamera()` 正确释放

---

## 阶段 3：预览功能（单 Surface） ✅
**验收标准**：相机画面能在 XComponent 上显示

| 步骤 | 内容 | 状态 |
|------|------|------|
| 3.1 | ArkTS 创建 XComponent，获取 surfaceId | ✅ |
| 3.2 | 实现 `startPreview(surfaceId)`：创建 PreviewOutput 并启动 | ✅ |
| 3.3 | 实现 `stopPreview()`：停止预览 | ✅ |

**验收**：应用启动后能看到相机实时画面

---

## 阶段 4：Surface 切换 ✅
**验收标准**：能在两个 XComponent 间切换画面

| 步骤 | 内容 | 状态 |
|------|------|------|
| 4.1 | 实现 `switchSurface(newSurfaceId)` | ✅ |
| 4.2 | 处理 Session beginConfig/commitConfig 流程 | ✅ |
| 4.3 | ArkTS 页面添加切换按钮测试 | ✅ |

**验收**：点击切换按钮，相机画面从 XComponent A 切换到 XComponent B，无崩溃

---

## 阶段 5：拍照功能 ✅
**验收标准**：能拍摄照片并返回数据

| 步骤 | 内容 | 状态 |
|------|------|------|
| 5.1 | 创建 PhotoOutput，配置拍照回调 | ✅ |
| 5.2 | 实现 `takePhoto(callback)` | ✅ |
| 5.3 | 回调通过 NAPI 返回 ArrayBuffer 给 ArkTS | ✅ |

**验收**：点击拍照，ArkTS 收到照片数据并显示

---

## 阶段 6：相机参数控制 ✅
**验收标准**：能调整缩放和对焦

| 步骤 | 内容 | 状态 |
|------|------|------|
| 6.1 | 实现 `setZoomRatio()` / `getZoomRatio()` | ✅ |
| 6.2 | 实现 `setFocusMode()` | ✅ |
| 6.3 | ArkTS 添加滑块控制 | ✅ |

**验收**：滑块调整时画面缩放正确响应

---

## 阶段 7：稳定性与优化 ✅
**验收标准**：快速操作无崩溃、无内存泄漏

| 步骤 | 内容 | 状态 |
|------|------|------|
| 7.1 | 添加线程锁（std::mutex） | ✅ |
| 7.2 | 状态机检查（防止重复初始化等） | ✅ |
| 7.3 | 错误恢复机制 | ✅ |

**验收**：快速连续切换 Surface、拍照，无崩溃

---

## 阶段 8：RenderSlot 注册机制 ✅
**验收标准**：渲染模块可注册/注销 Surface，接收预览流状态回调

| 步骤 | 内容 | 状态 |
|------|------|------|
| 8.1 | 创建 `render_slot.h` 数据结构 | ✅ |
| 8.2 | ExpoCamera 添加 RenderSlot 管理方法 | ✅ |
| 8.3 | 实现 `registerSlot()` / `unregisterSlot()` | ✅ |
| 8.4 | 实现 `switchToSlot()` 及回调通知 | ✅ |
| 8.5 | 实现状态订阅 `subscribeState()` | ✅ |
| 8.6 | NAPI 绑定新接口 | ✅ |

**验收**：
- 调用 `registerSlot()` 后渲染槽被记录
- 调用 `switchToSlot()` 后回调触发，渲染模块收到预览流状态
- 调用 `unregisterSlot()` 后渲染槽被移除，无崩溃

---

## 阶段 9：集成测试
**验收标准**：完整流程验证

| 步骤 | 内容 |
|------|------|
| 9.1 | 多页面切换测试 |
| 9.2 | 内存泄漏检测 |
| 9.3 | 异常场景测试（相机被占用、权限拒绝等） |

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
| `takePhoto(callback)` | 拍照 |
| `isPhotoOutputReady()` | 检查拍照输出就绪 |

### 相机参数

| 接口 | 说明 |
|------|------|
| `setZoomRatio(ratio)` | 设置缩放比例 |
| `getZoomRatio()` | 获取缩放比例 |
| `getZoomRatioRange()` | 获取缩放范围 |
| `isZoomSupported()` | 是否支持缩放 |
| `setFocusMode(mode)` | 设置对焦模式 |
| `getFocusMode()` | 获取对焦模式 |
| `isFocusModeSupported(mode)` | 是否支持对焦模式 |
| `setFocusPoint(x, y)` | 设置对焦点 |
| `getFocusPoint()` | 获取对焦点 |

### RenderSlot 管理

| 接口 | 说明 |
|------|------|
| `registerSlot(slotId, surfaceId, width, height, callback)` | 注册渲染槽 |
| `unregisterSlot(slotId)` | 注销渲染槽 |
| `switchToSlot(slotId)` | 切换预览流到指定槽 |
| `subscribeState(callback)` | 订阅相机状态 |
| `unsubscribeState()` | 取消订阅 |
