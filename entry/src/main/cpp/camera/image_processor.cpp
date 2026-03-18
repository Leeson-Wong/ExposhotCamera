#include "image_processor.h"
#include "hilog/log.h"

#include <cstring>
#include <cmath>
#include <cstdlib>
#include "opencv2/opencv.hpp"

// HarmonyOS Image Native API
#include <multimedia/image_framework/image/image_native.h>
#include <multimedia/image_framework/image/image_source_native.h>
#include <multimedia/image_framework/image/image_packer_native.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "ImageProcessor"

namespace exposhot {

ImageProcessor::ImageProcessor() {}

ImageProcessor::~ImageProcessor() {
    reset();
}

// ==================== 解码接口 ====================

bool ImageProcessor::decode(void* rawBuffer, size_t rawSize, ImageFormat format,
                            uint8_t** outRgbaBuffer, size_t* outSize,
                            int32_t* outWidth, int32_t* outHeight) {
    if (!rawBuffer || rawSize == 0 || !outRgbaBuffer || !outSize) {
        OH_LOG_ERROR(LOG_APP, "Invalid decode parameters");
        return false;
    }

    // 根据格式选择解码方式
    switch (format) {
        case ImageFormat::JPEG:
            return decodeJpeg(rawBuffer, rawSize, outRgbaBuffer, outSize, outWidth, outHeight);

        case ImageFormat::NV21:
        case ImageFormat::YUV420:
            // NV21/YUV420 需要外部提供尺寸信息
            OH_LOG_ERROR(LOG_APP, "NV21/YUV420 decode requires width/height, use decodeNv21 directly");
            return false;

        case ImageFormat::RGBA_8888:
            // 已经是 RGBA，直接复制
            *outRgbaBuffer = static_cast<uint8_t*>(malloc(rawSize));
            if (*outRgbaBuffer) {
                memcpy(*outRgbaBuffer, rawBuffer, rawSize);
                *outSize = rawSize;
                return true;
            }
            return false;

        default:
            // 尝试作为 JPEG 解码（最常见情况）
            OH_LOG_INFO(LOG_APP, "Unknown format, trying JPEG decode");
            return decodeJpeg(rawBuffer, rawSize, outRgbaBuffer, outSize, outWidth, outHeight);
    }
}

bool ImageProcessor::decodeJpeg(void* rawBuffer, size_t rawSize,
                                uint8_t** outRgbaBuffer, size_t* outSize,
                                int32_t* outWidth, int32_t* outHeight) {
    if (!rawBuffer || !outWidth || !outHeight) {
        return false;
    }

    // 1. 将原始 BGRA16 数据封装为 OpenCV Mat
    // 假设 BGRA16 对应 CV_16UC4 (4通道，每通道16位无符号整型)
    cv::Mat bgra16Mat(*outHeight, *outWidth, CV_16UC4, rawBuffer);

    // 2. 转换为 RGBA 8-bit
    // 第一步：将 16-bit 转换为 8-bit (通常需要缩放，16位最大值65535 -> 8位255)
    cv::Mat bgra8Mat;
    bgra16Mat.convertTo(bgra8Mat, CV_8UC4, 1.0 / 256.0);

    // 第二步：将 BGRA 转换为 RGBA
    cv::Mat rgba8Mat;
    cv::cvtColor(bgra8Mat, rgba8Mat, cv::COLOR_BGRA2RGBA);

    // 3. 准备输出数据
    *outSize = rgba8Mat.total() * rgba8Mat.elemSize();
    *outRgbaBuffer = (uint8_t*)malloc(*outSize);

    if (*outRgbaBuffer) {
        std::memcpy(*outRgbaBuffer, rgba8Mat.data, *outSize);
    }
    return true;
}

bool ImageProcessor::decodeNv21(void* rawBuffer, size_t rawSize,
                                int32_t width, int32_t height,
                                uint8_t** outRgbaBuffer, size_t* outSize) {
    // NV21 格式: YYYYYYY... VUVU...
    // Y 平面: width * height
    // UV 平面: width * height / 2
    // 总大小: width * height * 3 / 2

    size_t expectedSize = static_cast<size_t>(width) * height * 3 / 2;
    if (rawSize < expectedSize) {
        OH_LOG_ERROR(LOG_APP, "NV21 buffer too small: %{public}zu < %{public}zu", rawSize, expectedSize);
        return false;
    }

    size_t rgbaSize = static_cast<size_t>(width) * height * 4;
    uint8_t* rgbaBuffer = static_cast<uint8_t*>(malloc(rgbaSize));
    if (!rgbaBuffer) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate RGBA buffer");
        return false;
    }

    uint8_t* yPlane = static_cast<uint8_t*>(rawBuffer);
    uint8_t* uvPlane = yPlane + width * height;

    for (int32_t j = 0; j < height; j++) {
        for (int32_t i = 0; i < width; i++) {
            int yIndex = j * width + i;
            int uvIndex = (j / 2) * width + (i & ~1);

            int y = yPlane[yIndex];
            int u = uvPlane[uvIndex];
            int v = uvPlane[uvIndex + 1];

            // YUV to RGBA conversion
            int r = y + static_cast<int>(1.402 * (v - 128));
            int g = y - static_cast<int>(0.344 * (u - 128)) - static_cast<int>(0.714 * (v - 128));
            int b = y + static_cast<int>(1.772 * (u - 128));

            int rgbaIndex = yIndex * 4;
            rgbaBuffer[rgbaIndex] = static_cast<uint8_t>(std::min(255, std::max(0, r)));
            rgbaBuffer[rgbaIndex + 1] = static_cast<uint8_t>(std::min(255, std::max(0, g)));
            rgbaBuffer[rgbaIndex + 2] = static_cast<uint8_t>(std::min(255, std::max(0, b)));
            rgbaBuffer[rgbaIndex + 3] = 255; // Alpha
        }
    }

    *outRgbaBuffer = rgbaBuffer;
    *outSize = rgbaSize;

    OH_LOG_INFO(LOG_APP, "NV21 decode complete, RGBA size: %{public}zu", rgbaSize);
    return true;
}

// ==================== 编码接口 ====================

bool ImageProcessor::encodeJpeg(uint8_t* rgbaBuffer, size_t rgbaSize,
                                int32_t width, int32_t height, int32_t quality,
                                void** outJpegBuffer, size_t* outJpegSize) {
    return true;
}

bool ImageProcessor::encodePng(uint8_t* rgbaBuffer, size_t rgbaSize,
                               int32_t width, int32_t height,
                               void** outPngBuffer, size_t* outPngSize) {
    // TODO: 实现 PNG 编码
    // 与 encodeJpeg 类似，只需将 format 改为 "image/png"
    OH_LOG_ERROR(LOG_APP, "encodePng not implemented yet");
    return false;
}

// ==================== 堆叠接口 ====================

bool ImageProcessor::initStacking(int32_t totalFrames, int32_t width, int32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        OH_LOG_WARN(LOG_APP, "Session already initialized, reset first");
        return false;
    }

    totalFrames_ = totalFrames;
    processedFrames_ = 0;
    width_ = width;
    height_ = height;

    // 分配累积缓冲区（4 个 float 通道: R, G, B, A）
    size_t bufferSize = static_cast<size_t>(width) * height * 4 * sizeof(float);
    accumulateBuffer_ = static_cast<float*>(malloc(bufferSize));
    if (!accumulateBuffer_) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate accumulate buffer: size=%{public}zu", bufferSize);
        return false;
    }
    memset(accumulateBuffer_, 0, bufferSize);

    // 分配临时 RGBA 缓冲区
    size_t rgbaSize = static_cast<size_t>(width) * height * 4;
    rgbaBuffer_ = static_cast<uint8_t*>(malloc(rgbaSize));
    if (!rgbaBuffer_) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate RGBA buffer");
        free(accumulateBuffer_);
        accumulateBuffer_ = nullptr;
        return false;
    }

    initialized_ = true;
    OH_LOG_INFO(LOG_APP, "Stacking session initialized: %{public}d frames, %{public}dx%{public}d",
                totalFrames, width, height);

    notifyState("initialized", "Stacking session started");
    return true;
}

bool ImageProcessor::processFrame(int32_t frameIndex, void* rawBuffer, size_t rawSize,
                                  int32_t width, int32_t height, ImageFormat format, bool isFirst) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        OH_LOG_ERROR(LOG_APP, "Session not initialized");
        return false;
    }

    OH_LOG_INFO(LOG_APP, "Processing frame %{public}d/%{public}d, rawSize=%{public}zu, %{public}dx%{public}d",
                frameIndex + 1, totalFrames_, rawSize, width, height);

    // 1. 解码原始图像为 RGBA
    uint8_t* rgbaBuffer = nullptr;
    size_t rgbaSize = 0;
    int32_t decodedWidth = 0, decodedHeight = 0;

    if (!decode(rawBuffer, rawSize, format, &rgbaBuffer, &rgbaSize, &decodedWidth, &decodedHeight)) {
        OH_LOG_ERROR(LOG_APP, "Failed to decode image for frame %{public}d", frameIndex);
        return false;
    }

    // 2. 堆叠到累积缓冲区
    if (!stackFrame(rgbaBuffer, isFirst)) {
        free(rgbaBuffer);
        return false;
    }

    free(rgbaBuffer);

    processedFrames_++;
    notifyProgress(frameIndex + 1);

    OH_LOG_INFO(LOG_APP, "Frame %{public}d processed, total processed: %{public}d",
                frameIndex + 1, processedFrames_);

    return true;
}

bool ImageProcessor::getCurrentResult(void** outBuffer, size_t* outSize) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || processedFrames_ == 0) {
        OH_LOG_ERROR(LOG_APP, "Not initialized or no frames processed");
        return false;
    }

    OH_LOG_INFO(LOG_APP, "Getting current result, processed frames: %{public}d", processedFrames_);

    // 将累积缓冲区转换为 uint8_t（取平均）
    int32_t pixelCount = width_ * height_;
    float weight = 1.0f / static_cast<float>(processedFrames_);

    for (int32_t i = 0; i < pixelCount * 4; i++) {
        float value = accumulateBuffer_[i] * weight;
        rgbaBuffer_[i] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, value)));
    }

    // 编码为 JPEG
    return encodeJpeg(rgbaBuffer_, static_cast<size_t>(width_) * height_ * 4,
                      width_, height_, 90, outBuffer, outSize);
}

bool ImageProcessor::finalize(void** outBuffer, size_t* outSize) {
    bool result = getCurrentResult(outBuffer, outSize);

    if (result) {
        notifyState("completed", "Stacking completed");
        OH_LOG_INFO(LOG_APP, "Stacking finalized, result size: %{public}zu", *outSize);
    }

    return result;
}

void ImageProcessor::reset() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (accumulateBuffer_) {
        free(accumulateBuffer_);
        accumulateBuffer_ = nullptr;
    }
    if (rgbaBuffer_) {
        free(rgbaBuffer_);
        rgbaBuffer_ = nullptr;
    }

    initialized_ = false;
    totalFrames_ = 0;
    processedFrames_ = 0;
    width_ = 0;
    height_ = 0;

    OH_LOG_INFO(LOG_APP, "ImageProcessor reset");
}

bool ImageProcessor::stackFrame(uint8_t* rgbaBuffer, bool isFirst) {
    int32_t pixelCount = width_ * height_ * 4;  // RGBA

    if (isFirst) {
        // 第一帧：直接复制到累积缓冲区
        for (int32_t i = 0; i < pixelCount; i++) {
            accumulateBuffer_[i] = static_cast<float>(rgbaBuffer[i]);
        }
        OH_LOG_INFO(LOG_APP, "First frame copied to accumulate buffer");
    } else {
        // 后续帧：累加（平均叠加）
        for (int32_t i = 0; i < pixelCount; i++) {
            accumulateBuffer_[i] += static_cast<float>(rgbaBuffer[i]);
        }
        OH_LOG_INFO(LOG_APP, "Frame accumulated");
    }

    return true;
}

void ImageProcessor::notifyState(const std::string& state, const std::string& message) {
    if (stateCallback_) {
        stateCallback_(state, message);
    }
}

void ImageProcessor::notifyProgress(int32_t current) {
    if (progressCallback_) {
        // 注意：这里传递 nullptr 因为在锁内不能调用 getCurrentResult
        progressCallback_(current, totalFrames_, nullptr, 0);
    }
}

} // namespace exposhot
