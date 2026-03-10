# Exposhot Camera SDK 使用文档

## 概述

Exposhot Camera SDK 是一个 HarmonyOS NDK 相机服务模块，提供：

- 相机预览流的 Surface 切换能力
- 拍照功能
- 相机参数控制（缩放、对焦）
- 观察者模式支持多渲染模块订阅

**特点**：
- 相机只初始化一次，应用退出时才释放
- 支持在多个渲染场景间切换预览流
- 同时支持 ArkTS 和 NDK 渲染模块

---

## SDK 文件清单

```
exposhot-camera-sdk/
├── libs/
│   ├── arm64-v8a/
│   │   └── libexpocamera.so
│   └── x86_64/
│       └── libexpocamera.so
└── types/
    └── libexpocamera/
        ├── oh-package.json5
        └── Index.d.ts
```

---

## 集成步骤

### 1. 添加 SDK 文件

将 SDK 文件放入项目：

```
your-app/
├── entry/
│   ├── src/main/
│   │   ├── cpp/
│   │   │   └── libs/
│   │   │       ├── arm64-v8a/
│   │   │       │   └── libexpocamera.so
│   │   │       └── x86_64/
│   │   │           └── libexpocamera.so
│   │   └── ets/
│   │       └── types/
│   │           └── libexpocamera/
│   │               ├── oh-package.json5
│   │               └── Index.d.ts
```

### 2. 配置依赖

在 `entry/oh-package.json5` 中添加：

```json5
{
  "dependencies": {
    "libexpocamera.so": "file:./src/main/ets/types/libexpocamera"
  }
}
```

### 3. 添加相机权限

在 `entry/src/main/module.json5` 中添加：

```json5
{
  "module": {
    "requestPermissions": [
      {
        "name": "ohos.permission.CAMERA",
        "reason": "$string:camera_reason",
        "usedScene": {
          "abilities": ["EntryAbility"],
          "when": "inuse"
        }
      }
    ]
  }
}
```

---

## API 参考

### 基础控制

| 接口 | 说明 | 返回值 |
|------|------|--------|
| `initCamera()` | 初始化相机 | `0` 成功 |
| `releaseCamera()` | 释放相机 | `0` 成功 |
| `startPreview(surfaceId)` | 启动预览 | `0` 成功 |
| `stopPreview()` | 停止预览 | `0` 成功 |
| `switchSurface(surfaceId)` | 切换预览 Surface | `0` 成功 |
| `takePhoto(callback)` | 拍照 | `0` 成功 |
| `isPhotoOutputReady()` | 检查拍照就绪 | `boolean` |

### 相机参数

| 接口 | 说明 | 返回值 |
|------|------|--------|
| `setZoomRatio(ratio)` | 设置缩放比例 | `0` 成功 |
| `getZoomRatio()` | 获取当前缩放 | `number` |
| `getZoomRatioRange()` | 获取缩放范围 | `{min, max}` |
| `isZoomSupported()` | 是否支持缩放 | `boolean` |
| `setFocusMode(mode)` | 设置对焦模式 | `0` 成功 |
| `getFocusMode()` | 获取对焦模式 | `FocusMode` |
| `setFocusPoint(x, y)` | 设置对焦点 | `0` 成功 |
| `getFocusPoint()` | 获取对焦点 | `{x, y}` |

### 观察者模式

| 接口 | 说明 | 返回值 |
|------|------|--------|
| `registerObserver(surfaceId, callback)` | 注册观察者 | `slotId` |
| `unregisterObserver(slotId)` | 注销观察者 | `0` 成功 |
| `switchToSlot(slotId)` | 切换预览到指定 slot | `0` 成功 |

### 状态订阅

| 接口 | 说明 | 返回值 |
|------|------|--------|
| `subscribeState(callback)` | 订阅相机状态 | `0` 成功 |
| `unsubscribeState()` | 取消订阅 | `0` 成功 |

---

## 使用示例

### 基础用法

```typescript
import nativeCamera from 'libexpocamera.so';

@Entry
@Component
struct CameraPage {
  private xComponentController: XComponentController = new XComponentController();
  private slotId: string = '';

  build() {
    Column() {
      XComponent({
        id: 'cameraPreview',
        type: 'surface',
        libraryname: 'expocamera',
        controller: this.xComponentController
      })
      .onLoad(() => {
        this.initCamera();
      })
      .width('100%')
      .height('100%')

      Button('拍照')
        .onClick(() => this.takePhoto())
    }
  }

  async initCamera() {
    // 1. 初始化相机
    nativeCamera.initCamera();

    // 2. 获取 Surface ID
    const surfaceId = this.xComponentController.getXComponentSurfaceId();

    // 3. 启动预览
    nativeCamera.startPreview(surfaceId);

    // 4. 注册观察者（可选）
    this.slotId = nativeCamera.registerObserver(surfaceId, (activeSlotId, activeSurfaceId) => {
      console.log(`预览流切换到: ${activeSlotId}`);
    });
  }

  takePhoto() {
    if (!nativeCamera.isPhotoOutputReady()) {
      console.warn('拍照未就绪');
      return;
    }

    nativeCamera.takePhoto((arrayBuffer: ArrayBuffer) => {
      console.log(`拍照成功，大小: ${arrayBuffer.byteLength}`);
      // 处理照片数据...
    });
  }

  aboutToDisappear() {
    nativeCamera.unregisterObserver(this.slotId);
    nativeCamera.stopPreview();
    nativeCamera.releaseCamera();
  }
}
```

### 多页面切换

```typescript
// 页面 A
@Entry
@Component
struct PageA {
  private slotId: string = '';

  onPageShow() {
    // 切换预览流到当前页面
    nativeCamera.switchToSlot(this.slotId);
  }
}

// 页面 B
@Entry
@Component
struct PageB {
  private slotId: string = '';

  onPageShow() {
    // 切换预览流到当前页面
    nativeCamera.switchToSlot(this.slotId);
  }
}
```

### 缩放控制

```typescript
@Entry
@Component
struct ZoomControl {
  @State zoomRatio: number = 1.0;
  private zoomRange: ZoomRange = { min: 1.0, max: 10.0 };

  aboutToAppear() {
    if (nativeCamera.isZoomSupported()) {
      this.zoomRange = nativeCamera.getZoomRatioRange();
      this.zoomRatio = nativeCamera.getZoomRatio();
    }
  }

  build() {
    Column() {
      Slider({
        value: this.zoomRatio,
        min: this.zoomRange.min,
        max: this.zoomRange.max,
        step: 0.1
      })
      .onChange((value: number) => {
        nativeCamera.setZoomRatio(value);
        this.zoomRatio = value;
      })
    }
  }
}
```

---

## 支持 NDK 渲染模块

当应用同时包含 NDK 渲染模块时，通过 ArkTS 中转层统一分发事件：

```typescript
import nativeCamera from 'libexpocamera.so';
import rendererSo from 'librenderer.so';  // NDK 渲染模块

// 统一在 ArkTS 层注册回调
nativeCamera.registerObserver(surfaceId, (slotId, activeSurfaceId) => {
  // 1. ArkTS 渲染处理
  this.updateUI(slotId, activeSurfaceId);

  // 2. NDK 渲染模块通知（只传字符串，性能无损）
  rendererSo.onPreviewChanged(slotId, activeSurfaceId);
});

nativeCamera.subscribeState((state, message) => {
  // 分发状态变化
  this.handleState(state, message);
  rendererSo.onCameraStateChanged(state, message);
});
```

**性能说明**：回调只传递 `slotId` 和 `surfaceId` 字符串，ArkTS 中转开销可忽略（纳秒级）。

---

## 生命周期建议

```
┌─────────────────────────────────────────────────────────┐
│  应用启动                                                │
│      │                                                  │
│      ▼                                                  │
│  nativeCamera.initCamera()  ← 只调用一次                │
│      │                                                  │
│      ▼                                                  │
│  ┌─────────────────────────────────────────────────┐   │
│  │  页面生命周期                                     │   │
│  │                                                  │   │
│  │  XComponent.onLoad()                            │   │
│  │      │                                          │   │
│  │      ▼                                          │   │
│  │  registerObserver() + startPreview()            │   │
│  │      │                                          │   │
│  │      ▼                                          │   │
│  │  [页面显示中...]                                  │   │
│  │      │                                          │   │
│  │      ▼                                          │   │
│  │  aboutToDisappear()                             │   │
│  │      │                                          │   │
│  │      ▼                                          │   │
│  │  unregisterObserver() + stopPreview()           │   │
│  │                                                  │   │
│  └─────────────────────────────────────────────────┘   │
│      │                                                  │
│      ▼                                                  │
│  nativeCamera.releaseCamera()  ← 应用退出时调用          │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

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

1. **相机权限**：确保已申请 `ohos.permission.CAMERA` 权限
2. **单例模式**：相机服务是单例，整个应用共享一个实例
3. **线程安全**：所有接口都是线程安全的
4. **Surface 有效期**：确保 XComponent 销毁前先调用 `unregisterObserver()`
5. **内存管理**：拍照回调的 `ArrayBuffer` 由调用方管理

---

## 版本历史

| 版本 | 日期 | 更新内容 |
|------|------|----------|
| 1.0.0 | - | 初始版本 |
