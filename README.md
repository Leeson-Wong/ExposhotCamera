# Exposhot Camera

HarmonyOS NDK 相机服务模块，提供高性能的相机预览、Surface 切换和拍照能力。

## 特性

- **单例相机服务** - 相机只初始化一次，应用退出时释放
- **Surface 动态切换** - 支持在多个渲染场景间无缝切换预览流
- **观察者模式** - 多个渲染模块可订阅预览流状态变化
- **完整相机控制** - 缩放、对焦、对焦点设置
- **混合渲染支持** - 同时支持 ArkTS 和 NDK 渲染模块

## 项目结构

```
ExposhotCamera/
├── entry/
│   └── src/main/
│       ├── cpp/                    # Native 代码
│       │   ├── camera/             # 相机模块
│       │   │   ├── expo_camera.h   # 相机控制类
│       │   │   ├── expo_camera.cpp
│       │   │   ├── render_slot.h   # 渲染槽数据结构
│       │   │   └── napi_expo_camera.cpp  # NAPI 绑定
│       │   └── types/libexpocamera/      # 类型声明
│       │       ├── Index.d.ts
│       │       └── oh-package.json5
│       ├── ets/                    # ArkTS 代码
│       └── resources/              # 资源文件
├── docs/                           # 文档
│   ├── plan.md                     # 重构计划
│   ├── deving.md                   # 开发进度
│   └── usage.md                    # SDK 使用文档
└── build-profile.json5             # 构建配置
```

## 快速开始

### 集成 SDK

参考 [docs/usage.md](docs/usage.md) 获取完整的集成指南。

### 基础用法

```typescript
import nativeCamera from 'libexpocamera.so';

// 初始化相机
nativeCamera.initCamera();

// 获取 XComponent Surface ID 并启动预览
const surfaceId = this.xComponentController.getXComponentSurfaceId();
nativeCamera.startPreview(surfaceId);

// 拍照
nativeCamera.takePhoto((arrayBuffer: ArrayBuffer) => {
  // 处理照片
});

// 释放资源
nativeCamera.releaseCamera();
```

## API 概览

### 基础控制

| 接口 | 说明 |
|------|------|
| `initCamera()` | 初始化相机 |
| `releaseCamera()` | 释放相机 |
| `startPreview(surfaceId)` | 启动预览 |
| `stopPreview()` | 停止预览 |
| `switchSurface(surfaceId)` | 切换预览 Surface |
| `takePhoto(callback)` | 拍照 |

### 相机参数

| 接口 | 说明 |
|------|------|
| `setZoomRatio(ratio)` | 设置缩放比例 |
| `getZoomRatio()` | 获取缩放比例 |
| `setFocusMode(mode)` | 设置对焦模式 |
| `setFocusPoint(x, y)` | 设置对焦点 |

### 观察者模式

| 接口 | 说明 |
|------|------|
| `registerObserver(surfaceId, callback)` | 注册观察者 |
| `unregisterObserver(slotId)` | 注销观察者 |
| `switchToSlot(slotId)` | 切换预览流 |

## 文档

- [SDK 使用文档](docs/usage.md) - 集成步骤和 API 参考
- [开发计划](docs/deving.md) - 开发进度跟踪
- [重构计划](docs/plan.md) - 架构设计和重构方案

## 架构图

```
┌─────────────────────────────────────────────────────────────┐
│                    三方应用                                   │
│                                                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │ ArkTS 渲染   │  │ NDK 渲染    │  │ 其他模块     │         │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘         │
│         │                │                │                 │
│         └────────────────┼────────────────┘                 │
│                          │                                  │
│                          ▼                                  │
│               ┌─────────────────────┐                       │
│               │  ArkTS 中转/分发层   │                       │
│               └──────────┬──────────┘                       │
│                          │                                  │
│                          ▼                                  │
│               ┌─────────────────────┐                       │
│               │  libexpocamera.so   │                       │
│               │                     │                       │
│               │  - 相机硬件控制      │                       │
│               │  - Surface 切换     │                       │
│               │  - 观察者管理        │                       │
│               └──────────┬──────────┘                       │
│                          │                                  │
│                          ▼                                  │
│               ┌─────────────────────┐                       │
│               │  HarmonyOS Camera   │                       │
│               │  NDK API            │                       │
│               └─────────────────────┘                       │
└─────────────────────────────────────────────────────────────┘
```

## 开发状态

| 阶段 | 内容 | 状态 |
|------|------|------|
| 1-7 | 相机基础功能 | ✅ 完成 |
| 8 | RenderSlot 注册机制 | ✅ 完成 |
| 9 | 集成测试 | 🚧 进行中 |

## 环境要求

- HarmonyOS SDK: 6.0.2(22)+
- DevEco Studio: 最新版本
- NDK: r18+

## 许可证

[MIT License](LICENSE)