# Exposhot Camera SDK 文档

## 文档索引

| 文档 | 说明 |
|------|------|
| [usage.md](usage.md) | SDK 使用文档 - 集成步骤、API 参考、示例代码 |
| [architecture.md](architecture.md) | 架构设计 - 模块结构、Surface 切换、事件系统 |
| [callback_flow.md](callback_flow.md) | 回调流程 - 单次拍照/连拍流程图、锁机制 |
| [development.md](development.md) | 开发记录 - 开发阶段、API 汇总、待办事项 |
| [testing.md](testing.md) | 测试规划 - NDK 单元测试、集成测试方案（📋 规划中，未落地） |

## 快速开始

### 1. 集成 SDK

```typescript
import nativeCamera from 'libexpocamera.so';

// 初始化相机（mode 参数必填）
nativeCamera.initCamera(nativeCamera.CaptureMode.SINGLE);

// 启动预览
nativeCamera.startPreview(surfaceId);
```

### 2. 拍照

```typescript
// 单次拍照（简单流程，无后处理）
nativeCamera.registerImageDataCallback((data) => {
    console.log('照片:', data.width, 'x', data.height);
});
nativeCamera.takePhoto();
```

### 3. 连拍堆叠

```typescript
// 连拍堆叠（天文摄影用）
nativeCamera.startBurstCapture(
    { frameCount: 10, exposureMs: 10000, realtimePreview: true },
    (progress) => console.log(`进度: ${progress.processedFrames}/${progress.totalFrames}`),
    (buffer, isFinal) => console.log(`图像: ${isFinal ? '最终' : '中间'}`)
);
```

## 功能特性

| 功能 | 说明 |
|------|------|
| 相机预览 | 实时预览，支持 Surface 切换 |
| 单次拍照 | 基础拍照，直接返回图片，支持连续多次拍照 |
| 连拍堆叠 | 多帧拍摄 + 图像合成（天文摄影） |
| 相机参数 | 缩放、对焦、对焦点设置 |
| 观察者模式 | 多模块订阅相机状态，支持多页面预览切换 |
| 模式切换 | 单拍/连拍模式切换，自动选择合适分辨率 |
| 对焦点测试 | 独立测试页面，支持点击设置对焦点、轨迹可视化 |
| 对焦放大镜 | 点击对焦时显示放大镜，精确对焦 |

## 最新更新 (v2.4.0)

### 架构改进
- ✅ `initCamera` 参数调整：`mode` 改为必填，参数顺序 `(mode, resourceManager?)`
- ✅ 测试页面统一添加安全区适配
- ✅ TestFullFeatures 重命名为 "Slot 切换测试"，更准确反映功能

### 新增功能
- ✅ 新增 TestFocusMagnifier 对焦放大镜测试页面

### Bug 修复
- ✅ 修复相机释放时机问题
- ✅ 修复 SwitchCaptureMode 架构依赖方向

## 测试页面

| 页面 | 文件 | 功能 |
|------|------|------|
| 首页 | `Index.ets` | 功能入口导航 |
| 基础相机 | `TestBasicCamera.ets` | 预览、拍照、缩放、对焦测试 |
| 连拍测试 | `TestBurstCapture.ets` | 连拍堆叠、进度追踪、缩略图预览 |
| 模式切换测试 | `TestCaptureMode.ets` | 单拍/连拍模式切换、PhotoOutput 重配置 |
| Slot 切换测试 | `TestFullFeatures.ets` | 双预览、Slot 切换、完整相机控制 |
| 对焦点测试 | `TestFocusPoint.ets` | 手动对焦点设置、对焦轨迹、预设位置 |
| 对焦放大镜 | `TestFocusMagnifier.ets` | 点击对焦时显示放大镜、精确对焦 |
| 堆叠模拟 | `TestStackSimulate.ets` | 堆叠流程模拟、rawfile 读取 |

## 目录结构

```
docs/
├── README.md          # 本文档
├── usage.md           # 使用文档
├── architecture.md    # 架构设计
├── callback_flow.md   # 回调流程与锁机制
├── development.md     # 开发记录
├── testing.md         # 测试规划（📋 规划中）
└── issues/            # 问题记录
    ├── imagesource-crash.md
    ├── preview-performance.md
    └── taskqueue-cv-wait-crash.md
```
