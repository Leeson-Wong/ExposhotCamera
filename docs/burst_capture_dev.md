# 连拍堆叠功能开发记录

## 开发日期
2026-03-13（初版） / 2026-03-14（架构重构）

## 功能概述

实现多张连拍 + 图像堆叠处理功能，支持：
- 一次拍摄 N 张（拍摄前已知数量）
- 长曝光支持（每张 ~10s）
- 堆叠处理（第1张作为基准，后续每张与累积结果堆叠）
- 异步处理（拍照不阻塞处理）
- 实时预览（每次堆叠后返回累积结果）

## 架构设计

### 模块职责划分

```
┌─────────────────┐     ┌─────────────────┐
│   ExpoCamera    │────▶│  BurstCapture   │
│  (相机控制)      │     │  (连拍协调)      │
└─────────────────┘     └────────┬────────┘
                                 │
                    ┌────────────┼────────────┐
                    ▼            ▼            ▼
            ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
            │ImageProcessor│ │  TaskQueue   │ │  FileSaver   │
            │ (图像处理)    │ │ (异步调度)   │ │ (纯IO)       │
            │ - 解码       │ │              │ │ - 写文件     │
            │ - 堆叠       │ │              │ │ - 路径管理   │
            │ - 编码       │ │              │ │              │
            └──────────────┘ └──────────────┘ └──────────────┘
```

### 类职责说明

| 类 | 职责 | 不负责 |
|---|------|--------|
| **ExpoCamera** | 相机控制：初始化、预览、拍照触发、参数设置、接收回调转发数据 | 图像处理、文件IO |
| **BurstCapture** | 连拍协调：状态管理、收集帧、协调处理流程 | 图像处理、文件IO |
| **ImageProcessor** | 图像处理：解码、对齐、堆叠、降噪、色调映射、编码 | 文件IO、拍照触发 |
| **TaskQueue** | 异步调度：队列管理、线程消费 | 图像处理逻辑 |
| **FileSaver** | 纯IO：文件写入、路径管理、平台存储API | 图像处理、编码 |

### 数据流向

```
拍照触发 → 获取原始数据 → 解码 → 处理(堆叠) → 编码 → 落盘 → 回调
   |            |            |        |      |      |
ExpoCamera   ExpoCamera   ImageProcessor    FileSaver
                               |            (纯IO)
                          BurstCapture
                          (协调处理)
```

## 核心文件

### 1. task_queue.h/cpp - 线程安全任务队列

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

**线程同步**: `std::mutex` + `std::condition_variable`

**文件位置**: `entry/src/main/cpp/camera/task_queue.h`, `entry/src/main/cpp/camera/task_queue.cpp`

### 2. image_processor.h/cpp - 图像处理器

```cpp
// 图像格式枚举
enum class ImageFormat {
    UNKNOWN = 0, JPEG = 1, NV21 = 2, YUV420 = 3, RGBA_8888 = 4, RAW = 5
};

class ImageProcessor {
    // 解码接口
    bool decode(void* rawBuffer, size_t rawSize, ImageFormat format,
                uint8_t** outRgbaBuffer, size_t* outSize,
                int32_t* outWidth, int32_t* outHeight);

    // 编码接口
    bool encodeJpeg(uint8_t* rgbaBuffer, size_t rgbaSize,
                    int32_t width, int32_t height, int32_t quality,
                    void** outJpegBuffer, size_t* outJpegSize);
    bool encodePng(...);

    // 堆叠接口
    bool initStacking(int32_t totalFrames, int32_t width, int32_t height);
    bool processFrame(int32_t frameIndex, void* rawBuffer, size_t rawSize,
                      int32_t width, int32_t height, ImageFormat format, bool isFirst);
    bool getCurrentResult(void** outBuffer, size_t* outSize);
    bool finalize(void** outBuffer, size_t* outSize);
    void reset();
};
```

**堆叠算法**: 平均叠加
- 累积缓冲区使用 `float` 保持精度
- 第一帧直接复制，后续帧累加
- 输出时除以帧数取平均

**图像编解码**: 使用 HarmonyOS Image Native API
- `OH_ImageSourceNative_CreateFromData` 解码 JPEG
- `OH_ImagePackerNative_PackToData` 编码 JPEG

**文件位置**: `entry/src/main/cpp/camera/image_processor.h`, `entry/src/main/cpp/camera/image_processor.cpp`

### 3. file_saver.h/cpp - 文件保存器

```cpp
class FileSaver {
    // 初始化（设置保存目录等）
    bool init(const std::string& baseDir = "");

    // 保存数据到文件
    bool save(const void* data, size_t size, const std::string& filename, std::string* outPath = nullptr);

    // 保存 JPEG 图像（自动生成文件名：IMG_YYYYMMDD_HHMMSS.jpg）
    bool saveJpeg(const void* data, size_t size, std::string* outPath = nullptr);

    // 保存 PNG 图像
    bool savePng(const void* data, size_t size, std::string* outPath = nullptr);

    // 获取保存目录
    std::string getSaveDir() const;

    // 检查文件是否存在
    bool fileExists(const std::string& filepath) const;

    // 删除文件
    bool deleteFile(const std::string& filepath);
};
```

**文件位置**: `entry/src/main/cpp/camera/file_saver.h`, `entry/src/main/cpp/camera/file_saver.cpp`

### 4. burst_capture.h/cpp - 连拍管理器

```cpp
enum class BurstState {
    IDLE = 0, CAPTURING = 1, PROCESSING = 2,
    COMPLETED = 3, ERROR = 4, CANCELLED = 5
};

struct BurstConfig {
    int32_t frameCount = 5;
    int32_t exposureMs = 10000;
    bool realtimePreview = true;
};

struct BurstProgress {
    BurstState state;
    int32_t capturedFrames;
    int32_t processedFrames;
    int32_t totalFrames;
    std::string message;
};

class BurstCapture {  // 单例
    bool startBurst(const BurstConfig& config);
    void cancelBurst();
    void onPhotoCaptured(void* buffer, size_t size, int32_t width, int32_t height);
    bool isBurstActive() const;
};
```

**文件位置**: `entry/src/main/cpp/camera/burst_capture.h`, `entry/src/main/cpp/camera/burst_capture.cpp`

## 关联文件

### 1. expo_camera.cpp

**修改内容**: 在 `onPhotoAvailable` 中集成连拍回调

```cpp
// 检查是否是连拍模式
if (exposhot::BurstCapture::getInstance().isBurstActive()) {
    // 连拍模式: 通知 BurstCapture (由 BurstCapture 负责解码和释放)
    exposhot::BurstCapture::getInstance().onPhotoCaptured(
        bufferCopy, nativeBufferSize, size.width, size.height);
} else {
    // 普通模式: 调用 photoCallback_ (由回调调用者负责解码和释放)
    self.photoCallback_(bufferCopy, nativeBufferSize);
}
```

**文件位置**: `entry/src/main/cpp/camera/expo_camera.cpp`

### 2. napi_expo_camera.cpp

**新增 NAPI 方法**:
- `StartBurstCapture` - 开始连拍
- `CancelBurstCapture` - 取消连拍
- `GetBurstState` - 获取连拍状态
- `SetBurstImageSize` - 设置图像尺寸

**文件位置**: `entry/src/main/cpp/napi_expo_camera.cpp`

### 3. index.d.ts

**新增类型定义**:

```typescript
export const enum BurstState {
    IDLE = 0, CAPTURING = 1, PROCESSING = 2,
    COMPLETED = 3, ERROR = 4, CANCELLED = 5
}

export interface BurstConfig {
    frameCount: number;
    exposureMs: number;
    realtimePreview: boolean;
}

export interface BurstProgress {
    state: BurstState;
    capturedFrames: number;
    processedFrames: number;
    totalFrames: number;
    message: string;
}

export type BurstProgressCallback = (progress: BurstProgress) => void;
export type BurstImageCallback = (buffer: ArrayBuffer, isFinal: boolean) => void;
```

**文件位置**: `entry/src/main/cpp/types/libexpocamera/index.d.ts`

### 4. CMakeLists.txt

```cmake
add_library(expocamera SHARED
    napi_expo_camera.cpp
    camera/expo_camera.cpp
    camera/task_queue.cpp
    camera/image_processor.cpp
    camera/burst_capture.cpp
    camera/file_saver.cpp
)
```

**文件位置**: `entry/src/main/cpp/CMakeLists.txt`

### 5. 测试页面

**文件位置**:
- `entry/src/main/ets/pages/Index.ets` - 首页入口
- `entry/src/main/ets/pages/TestBasicCamera.ets` - 基础相机测试
- `entry/src/main/ets/pages/TestBurstCapture.ets` - 连拍功能测试
- `entry/src/main/ets/pages/TestFullFeatures.ets` - 完整功能测试

## 线程模型

```
拍照线程 (相机服务线程回调)
    │
    ▼
onPhotoAvailable
    │
    ├─ 连拍模式 ──▶ BurstCapture::onPhotoCaptured ──▶ TaskQueue::enqueue
    │                                                    │
    │                                                    ▼
    │                                              消费者线程
    │                                                    │
    │                                                    ▼
    │                                              ImageProcessor::processFrame
    │                                                    │
    │                                                    ▼
    │                                              napi_async_work
    │                                                    │
    │                                                    ▼
    │                                              UI 主线程 (JS 回调)
    │
    └─ 普通模式 ──▶ photoCallback_ ──▶ napi_async_work ──▶ UI 主线程
```

## 回调流程

```
拍照完成 → onPhotoAvailable
    → BurstCapture::onPhotoCaptured
        → TaskQueue::enqueue
            → 消费者线程 processTask
                → ImageProcessor::processFrame
                    → BurstImageCallback (通过 napi_async_work)
                        → 页面显示累积结果
```

## 状态流转

```
IDLE ──startBurst()──▶ CAPTURING ──拍摄完成──▶ PROCESSING ──处理完成──▶ COMPLETED
    │                      │                        │
    │                      │                        └──处理失败──▶ ERROR
    │                      │
    │                      └──cancelBurst()──▶ CANCELLED
    │
    └──任意状态──cancelBurst()──▶ CANCELLED
```

## 代码统计

| 类型 | 数量 |
|------|------|
| 核心文件 | 8 个 (task_queue, image_processor, file_saver, burst_capture, expo_camera, napi_expo_camera) |
| 测试页面 | 4 个 |
| 新增代码行数 | ~1500 行 |
| 新增方法 | 30+ 个 |

## 验证方案

1. **单元测试**: 验证 TaskQueue 线程安全
2. **集成测试**:
   - 启动连拍，检查状态变为 CAPTURING
   - 验证每帧处理后收到回调
   - 验证最终结果正确
   - 测试取消功能
3. **性能测试**: 验证拍照和处理不互相阻塞

## 注意事项

1. **HarmonyOS Image Native API**: 使用的 API 可能需要根据实际文档调整
2. **内存管理**: ImageTask 析构时自动释放 buffer，避免内存泄漏
3. **线程安全**: 使用 mutex 和 atomic 变量保护共享状态
4. **NAPI 异步回调**: 使用 `napi_async_work` 确保在主线程调用 JS 回调
5. **职责分离**: ImageProcessor 只负责图像处理，FileSaver 只负责文件IO

## 待优化项

1. [ ] 添加更复杂的堆叠算法（如对齐、去鬼影）
2. [ ] 支持 RAW 格式输出
3. [ ] 添加进度百分比显示
4. [ ] 优化大图处理性能
5. [ ] 添加错误重试机制
6. [ ] FileSaver 集成到连拍流程
