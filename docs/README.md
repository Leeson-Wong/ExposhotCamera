# Exposhot Camera SDK 文档

## 文档索引

| 文档 | 说明 |
|------|------|
| [usage.md](usage.md) | SDK 使用文档 - 集成步骤、API 参考、示例代码 |
| [architecture.md](architecture.md) | 架构设计 - 模块结构、Surface 切换、事件系统 |
| [AGENTS.md](AGENTS.md) | 模块代理 - 各模块的角色、职责和协作关系 |
| [development.md](development.md) | 开发记录 - 开发阶段、API 汇总、待办事项 |

## 快速开始

### 1. 集成 SDK

```typescript
import nativeCamera from 'libexpocamera.so';

// 初始化相机
nativeCamera.initCamera();

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
| 单次拍照 | 基础拍照，直接返回图片 |
| 连拍堆叠 | 多帧拍摄 + 图像合成（天文摄影） |
| 相机参数 | 缩放、对焦控制 |
| 观察者模式 | 多模块订阅相机状态 |

## 目录结构

```
docs/
├── README.md          # 本文档
├── usage.md           # 使用文档
├── architecture.md    # 架构设计
└── development.md     # 开发记录
```
