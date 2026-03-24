#ifndef IMAGE_PROCESSOR_H
#define IMAGE_PROCESSOR_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <functional>
#include <string>
#include <opencv2/opencv.hpp>
#include "motion_stack.h"
#include "file_saver.h"

#include "libraw.h"
#include "libraw_alloc.h"
#include "libraw_const.h"
#include "libraw_datastream.h"
#include "libraw_internal.h"
#include "libraw_types.h"
#include "libraw_version.h"

namespace exposhot {

// 图像格式枚举
enum class ImageFormat {
    UNKNOWN = 0,
    JPEG = 1,
    NV21 = 2,
    YUV420 = 3,
    RGBA_8888 = 4,
    RAW = 5,
    RGB48 = 6,
    BGRA64 = 7
};

struct MeanRes {
    float mean_x[2];
    float mean_y[2];
};

typedef struct {
    uint16_t* data;
    uint32_t width;
    uint32_t height;
    size_t size;
    int error;
} Rgb16Raw, Bgra16Raw;

using Frame = std::vector<uint16_t>;

// 处理进度回调
// currentFrame: 当前处理的帧索引 (1-based)
// totalFrames: 总帧数
// resultBuffer: 当前累积结果 (调用者负责 free)
// resultSize: 结果大小
using ProcessProgressCallback = std::function<void(int32_t currentFrame, int32_t totalFrames,
                                                   void* resultBuffer, size_t resultSize)>;

// 处理状态回调
using ProcessStateCallback = std::function<void(const std::string& state, const std::string& message)>;

/**
 * 图像处理器
 *
 * 职责：
 * - 图像解码：将原始格式(JPEG/YUV等)解码为 RGBA
 * - 图像堆叠：多帧对齐、累积、平均
 * - 图像编码：将 RGBA 编码为 JPEG/PNG
 *
 * 不负责：
 * - 文件 IO（由 FileSaver 负责）
 * - 拍照触发（由 ExpoCamera 负责）
 * - 流程协调（由 BurstCapture 负责）
 */
class ImageProcessor {
public:
    ImageProcessor();
    ~ImageProcessor();

    // 设置进度回调
    void setProgressCallback(ProcessProgressCallback callback) {
        progressCallback_ = callback;
    }

    // 设置状态回调
    void setStateCallback(ProcessStateCallback callback) {
        stateCallback_ = callback;
    }

    // ==================== 解码接口 ====================
    Bgra16Raw dngToBGRA16(const void *dng_data, size_t dng_size);
    Frame convertLibRawToBGRA16(libraw_processed_image_t* img);
    MeanRes MotionAnalysisAndStack(uint16_t *nativeBuffer1, uint16_t *nativeBuffer2, uint32_t width, uint32_t height);

    // ==================== 堆叠接口 ====================

    // 初始化堆叠会话
    // totalFrames: 总帧数
    // width, height: 输出图像尺寸(来自第一帧)
    // 返回: 是否成功
    bool initStacking(int32_t totalFrames, uint32_t width, uint32_t height);

    // 处理单帧图像（解码 + 堆叠）
    // frameIndex: 帧索引 (0-based)
    // rawBuffer: 原始图像数据
    // rawSize: 数据大小
    // width, height: 图像尺寸
    // format: 图像格式
    // isFirst: 是否是第一帧
    // 返回: 是否成功
    bool processFrame(int32_t frameIndex, void* rawBuffer, size_t rawSize,
                      uint32_t width, uint32_t height, ImageFormat format, bool isFirst);

    // 获取当前累积结果（编码为 JPEG）
    // 调用者负责 free 返回的 buffer
    // 返回: 是否成功
    bool getCurrentResult(void** outBuffer, size_t* outSize);

    // 完成堆叠，获取最终结果
    // 返回: 是否成功
    bool finalize(void** outBuffer, size_t* outSize);

    // 重置状态
    void reset();

    // 获取当前状态
    bool isInitialized() const { return initialized_; }
    int32_t getTotalFrames() const { return totalFrames_; }
    int32_t getProcessedFrames() const { return processedFrames_; }

private:
    // 堆叠一帧到累积缓冲区
    bool stackFrame(uint16_t* bgraBuffer, bool isFirst);

    // 通知状态
    void notifyState(const std::string& state, const std::string& message);

    // 通知进度
    void notifyProgress(int32_t current);

private:
    std::mutex mutex_;

    // 会话状态
    bool initialized_ = false;
    int32_t totalFrames_ = 0;
    int32_t processedFrames_ = 0;

    // 图像尺寸
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // TODO: 临时 - 用于直接透传原始 buffer（调试流程编排）
    void* passthroughBuffer_ = nullptr;
    size_t passthroughSize_ = 0;

    // 回调
    ProcessProgressCallback progressCallback_;
    ProcessStateCallback stateCallback_;
};

} // namespace exposhot

#endif // IMAGE_PROCESSOR_H
