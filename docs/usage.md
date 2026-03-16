# Exposhot Camera SDK 使用文档

## 概述

Exposhot Camera SDK 是一个 HarmonyOS NDK 相机服务模块，提供：

- 相机预览流的 Surface 切换能力
- **统一拍摄管理**（单次拍照、连拍堆叠）
- 相机参数控制（缩放、对焦）
- 观察者模式支持多渲染模块订阅

**特点**：
- 相机只初始化一次，应用退出时才释放
- 支持在多个渲染场景间切换预览流
- 同时支持 ArkTS 和 NDK 渲染模块
- **拍摄动作互斥**：单拍和连拍不能同时进行

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

## 拍照模式

SDK 提供两种拍照模式，适用于不同场景：

| 模式 | API | 适用场景 | 流程 |
|------|-----|----------|------|
| **单次拍照** | `takePhoto()` | 普通拍照 | 拍完直接返回图片，无后处理 |
| **连拍堆叠** | `startBurstCapture()` | 天文摄影 | 多帧拍摄 → 堆叠处理 → 返回合成图 |

**重要说明**：单拍和连拍**互斥**，同时只能进行一种拍摄操作。

### 互斥行为

| 当前状态 | 调用 takePhoto() | 调用 startBurstCapture() |
|----------|------------------|--------------------------|
| IDLE | ✅ 成功 | ✅ 成功 |
| SINGLE_CAPTURING | ❌ 被拒绝 | ❌ 被拒绝 |
| BURST_CAPTURING | ❌ 被拒绝 | ❌ 被拒绝 |
| PROCESSING | ❌ 被拒绝 | ❌ 被拒绝 |

被拒绝时返回 `errorCode != 0`，建议判断：
```typescript
const result = nativeCamera.takePhoto();
if (result.errorCode !== 0) {
    console.log(`拍照失败: 错误码 ${result.errorCode}`);
}
```

### 单次拍照

简单流程，拍完直接返回图片数据：

```
takePhoto() → 返回 sessionId → onPhotoAvailable → registerImageDataCallback 回调
```

| 接口 | 说明 | 返回值 |
|------|------|--------|
| `takePhoto()` | 触发单次拍照 | `{ errorCode, sessionId }` |
| `registerImageDataCallback(callback)` | 注册图像数据回调 | - |
| `registerPhotoErrorCallback(callback)` | 注册拍照错误回调 | - |

**示例**：
```typescript
// 注册图像数据回调
nativeCamera.registerImageDataCallback((data) => {
    console.log(`拍照: ${data.width}x${data.height}`);
    // 保存图片
    nativeCamera.saveImageToFile(data.buffer!);
});

// 注册错误回调
nativeCamera.registerPhotoErrorCallback((error) => {
    console.error(`拍照失败: ${error.sessionId}, 错误码: ${error.errorCode}`);
});

// 触发拍照
const result = nativeCamera.takePhoto();
if (result.errorCode !== 0) {
    console.log(`拍照触发失败: 错误码 ${result.errorCode}`);
} else {
    console.log(`拍照已触发: ${result.sessionId}`);
}
```

### 连拍堆叠

完整流程，包含拍摄和处理阶段：

```
startBurstCapture()
    │
    ▼ (循环拍摄 N 帧)
每帧 → 图像入队 → 异步处理
    │
    ▼ (全部拍完后)
堆叠处理 → 进度更新 → 最终结果
```

| 接口 | 说明 | 返回值 |
|------|------|--------|
| `startBurstCapture(config, progressCallback, imageCallback)` | 开始连拍 | `sessionId`，空字符串表示失败 |
| `cancelBurstCapture()` | 取消连拍 | - |
| `getBurstState()` | 获取连拍状态 | `BurstState` |
| `setBurstImageSize(width, height)` | 设置图像尺寸 | - |

**示例**：
```typescript
const sessionId = nativeCamera.startBurstCapture(
    { frameCount: 10, exposureMs: 10000, realtimePreview: true },
    (progress) => {
        console.log(`进度: ${progress.processedFrames}/${progress.totalFrames}`);
    },
    (buffer, isFinal) => {
        if (isFinal) {
            nativeCamera.saveImageToFile(buffer, 'final.jpg');
        }
    }
);

if (sessionId.length === 0) {
    console.log('连拍启动失败');
}
```

### 文件保存

| 接口 | 说明 |
|------|------|
| `saveImageToFile(buffer, filename?)` | 保存图像到文件 |
| `getImageSaveDir()` | 获取保存目录 |

---

## 使用示例

### 单次拍照

简单流程，拍完直接获取图片：

```typescript
import nativeCamera from 'libexpocamera.so';

@Entry
@Component
struct PhotoPage {
  private xComponentController: XComponentController = new XComponentController();

  aboutToAppear() {
    // 注册图像数据回调
    nativeCamera.registerImageDataCallback((data) => {
      console.log(`拍照成功: ${data.width}x${data.height}`);
      if (data.isFinal) {
        // 保存或显示图片
        const path = nativeCamera.saveImageToFile(data.buffer!);
        console.log(`已保存: ${path}`);
      }
    });

    // 注册错误回调
    nativeCamera.registerPhotoErrorCallback((error) => {
      console.error(`拍照失败: sessionId=${error.sessionId}, errorCode=${error.errorCode}`);
    });
  }

  build() {
    Column() {
      XComponent({
        id: 'cameraPreview',
        type: 'surface',
        libraryname: 'expocamera',
        controller: this.xComponentController
      })
      .onLoad(() => {
        nativeCamera.initCamera();
        const surfaceId = this.xComponentController.getXComponentSurfaceId();
        nativeCamera.startPreview(surfaceId);
      })
      .width('100%')
      .height('100%')

      Button('拍照')
        .onClick(() => {
          if (nativeCamera.isPhotoOutputReady()) {
            const result = nativeCamera.takePhoto();
            if (result.errorCode !== 0) {
              console.error(`拍照失败: 错误码 ${result.errorCode}`);
            }
          }
        })
    }
  }
}
```

### 连拍堆叠（天文摄影）

多帧拍摄 + 堆叠合成：

```typescript
import nativeCamera, { BurstState } from 'libexpocamera.so';

@Entry
@Component
struct BurstCapturePage {
  @State progress: string = '';
  @State isCapturing: boolean = false;

  build() {
    Column() {
      // ... XComponent 预览 ...

      Text(this.progress)

      Button(this.isCapturing ? '取消' : '开始连拍')
        .onClick(() => {
          if (this.isCapturing) {
            nativeCamera.cancelBurstCapture();
          } else {
            this.startBurst();
          }
        })
    }
  }

  startBurst() {
    this.isCapturing = true;
    this.progress = '准备拍摄...';

    const success = nativeCamera.startBurstCapture(
      {
        frameCount: 10,       // 拍 10 帧
        exposureMs: 10000,    // 每帧 10 秒
        realtimePreview: true // 实时预览中间结果
      },
      // 进度回调
      (progress) => {
        this.progress = `状态: ${progress.state}, ` +
          `已拍: ${progress.capturedFrames}/${progress.totalFrames}, ` +
          `已处理: ${progress.processedFrames}`;
      },
      // 图像回调
      (buffer, isFinal) => {
        if (isFinal) {
          console.log('最终图像已生成');
          const path = nativeCamera.saveImageToFile(buffer, 'burst_final.jpg');
          console.log(`已保存: ${path}`);
          this.isCapturing = false;
        } else {
          console.log('中间预览图像');
        }
      }
    );

    if (!success) {
      this.progress = '启动失败';
      this.isCapturing = false;
    }
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

### HarmonyOS 相机错误码

| 错误码 | 说明 |
|--------|------|
| 0 | 成功 |
| -1 | 参数无效 (CAMERA_INVALID_ARGUMENT) |
| -2 | 操作不允许 (CAMERA_OPERATION_NOT_ALLOWED) |
| -3 | SDK 桩错误 (CAMERA_SDK_STUB_ERROR) |
| -4 | 内存分配失败 (CAMERA_ALLOC_ERROR) |
| -5 | 内存不足 (CAMERA_NO_MEMORY) |
| -6 | 没有权限 (CAMERA_NOT_PERMITTED) |
| -7 | 不支持 (CAMERA_NOT_SUPPORTED) |
| -8 | 状态错误 (CAMERA_STATE_ERROR) |
| -9 | 未知错误 (CAMERA_UNKNOWN_ERROR) |

### 系统标准错误码

| 错误码 | 说明 |
|--------|------|
| -16 (-EBUSY) | 设备忙（有其他拍摄进行中） |
| -19 (-ENODEV) | 设备不存在（PhotoOutput 未初始化） |

---

## 注意事项

1. **相机权限**：确保已申请 `ohos.permission.CAMERA` 权限
2. **单例模式**：相机服务是单例，整个应用共享一个实例
3. **线程安全**：所有接口都是线程安全的
4. **Surface 有效期**：确保 XComponent 销毁前先调用 `unregisterObserver()`
5. **内存管理**：拍照回调的 `ArrayBuffer` 由调用方管理
6. **拍摄互斥**：单拍和连拍不能同时进行，需要等待当前拍摄完成

---

## 版本历史

| 版本 | 日期 | 更新内容 |
|------|------|----------|
| 2.1.0 | 2026-03-16 | 拍照错误处理：返回值改为 `{ errorCode, sessionId }`，添加 `registerPhotoErrorCallback` |
| 2.0.0 | 2026-03-16 | 重构：统一拍摄动作到 CaptureManager，实现单拍/连拍互斥 |
| 1.0.0 | - | 初始版本 |
