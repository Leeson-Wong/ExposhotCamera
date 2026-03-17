#include "image_processor.h"
#include "hilog/log.h"

#include <cstring>
#include <cmath>
#include <cstdlib>

// HarmonyOS Image Native API
#include <multimedia/image_framework/image/image_common.h>
#include <multimedia/image_framework/image/image_native.h>
#include <multimedia/image_framework/image/image_source_native.h>
#include <multimedia/image_framework/image/image_packer_native.h>
#include <multimedia/image_framework/image/pixelmap_native.h>

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
                            uint32_t* outWidth, uint32_t* outHeight) {
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
                                uint32_t* outWidth, uint32_t* outHeight) {
    // 快速失败：空指针检查
    if (!rawBuffer || rawSize == 0) {
        OH_LOG_ERROR(LOG_APP, "decodeJpeg: null or empty input buffer");
        return false;
    }
    if (!outRgbaBuffer || !outSize || !outWidth || !outHeight) {
        OH_LOG_ERROR(LOG_APP, "decodeJpeg: null output parameters");
        return false;
    }

    // 初始化输出
    *outRgbaBuffer = nullptr;
    *outSize = 0;
    *outWidth = 0;
    *outHeight = 0;

    // 创建 ImageSource
    OH_ImageSourceNative* imageSource = nullptr;
    int32_t ret = OH_ImageSourceNative_CreateFromData(
        static_cast<uint8_t*>(rawBuffer), rawSize, &imageSource);
    if (ret != IMAGE_SUCCESS || !imageSource) {
        OH_LOG_ERROR(LOG_APP, "Failed to create ImageSource: %{public}d", ret);
        return false;
    }

//    // 获取图像信息
//    OH_ImageSource_Info* imageInfo = nullptr;
//    OH_ImageSourceInfo_Create(&imageInfo);
//    ret = OH_ImageSourceNative_GetImageInfo(imageSource, 0, imageInfo);
//    if (ret != IMAGE_SUCCESS) {
//        OH_LOG_ERROR(LOG_APP, "Failed to get image info: %{public}d", ret);
//        OH_ImageSourceNative_Release(imageSource);
//        return false;
//    }
//    
//    Image_MimeType* mimetype;
//    OH_ImageSourceInfo_GetWidth(imageInfo, outWidth);
//    OH_ImageSourceInfo_GetHeight(imageInfo, outHeight);
//    OH_ImageSourceInfo_GetMimeType(imageInfo, mimetype);
//    OH_LOG_INFO(LOG_APP, "Image info: %{public}dx%{public}d, format=%{public}s",
//                *outWidth, *outHeight, mimetype->data);

    // 创建 PixelMap（解码为 RGBA）
    OH_Pixelmap_InitializationOptions* opts = nullptr;
    OH_PixelmapInitializationOptions_Create(&opts);
    OH_PixelmapInitializationOptions_SetWidth(opts, *outWidth);
    OH_PixelmapInitializationOptions_SetHeight(opts, *outHeight);
    OH_PixelmapInitializationOptions_SetPixelFormat(opts, PIXELMAP_ALPHA_TYPE_OPAQUE);
    
    OH_PixelmapNative* pixelMap = nullptr;
    ret = OH_PixelmapNative_CreatePixelmap(imageSource, outSize, &opts, &pixelMap);
    OH_ImageSourceNative_Release(imageSource);  // PixelMap 创建后可以释放 ImageSource

    if (ret != IMAGE_SUCCESS || !pixelMap) {
        OH_LOG_ERROR(LOG_APP, "Failed to create PixelMap: %{public}d", ret);
        return false;
    }

    // 获取像素数据
    void* pixelData = nullptr;
    uint32_t dataCapacity = 0;
    ret = OH_PixelMap_Native_GetPixels(pixelMap, &pixelData, &dataCapacity);
    if (ret != IMAGE_SUCCESS || !pixelData) {
        OH_LOG_ERROR(LOG_APP, "Failed to get pixel data: %{public}d", ret);
        OH_PixelMap_Native_Release(pixelMap);
        return false;
    }

    // 分配输出缓冲区并复制数据
    *outSize = static_cast<size_t>((*outWidth) * (*outHeight) * 4);  // RGBA = 4 bytes per pixel
    *outRgbaBuffer = static_cast<uint8_t*>(malloc(*outSize));
    if (!*outRgbaBuffer) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate output buffer: size=%{public}zu", *outSize);
        OH_PixelMap_Native_Release(pixelMap);
        return false;
    }

    memcpy(*outRgbaBuffer, pixelData, *outSize);
    OH_PixelMap_Native_Release(pixelMap);

    OH_LOG_INFO(LOG_APP, "Decode successful: %{public}dx%{public}d, size=%{public}zu",
                *outWidth, *outHeight, *outSize);
    return true;
}

bool ImageProcessor::decodeNv21(void* rawBuffer, size_t rawSize,
                                uint32_t width, uint32_t height,
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
                                uint32_t width, uint32_t height, int32_t quality,
                                void** outJpegBuffer, size_t* outJpegSize) {
    // 快速失败：空指针检查
    if (!rgbaBuffer || rgbaSize == 0) {
        OH_LOG_ERROR(LOG_APP, "encodeJpeg: null or empty input buffer");
        return false;
    }
    if (!outJpegBuffer || !outJpegSize) {
        OH_LOG_ERROR(LOG_APP, "encodeJpeg: null output parameters");
        return false;
    }

    // JPEG 编码暂未实现，使用 PNG 代替
    OH_LOG_ERROR(LOG_APP, "encodeJpeg not implemented, use PNG instead");
    return false;
}

bool ImageProcessor::encodePng(uint8_t* rgbaBuffer, size_t rgbaSize,
                               uint32_t width, uint32_t height,
                               void** outPngBuffer, size_t* outPngSize) {
    // 快速失败：空指针检查
    if (!rgbaBuffer || rgbaSize == 0) {
        OH_LOG_ERROR(LOG_APP, "encodePng: null or empty input buffer");
        return false;
    }
    if (!outPngBuffer || !outPngSize) {
        OH_LOG_ERROR(LOG_APP, "encodePng: null output parameters");
        return false;
    }

    // 初始化输出
    *outPngBuffer = nullptr;
    *outPngSize = 0;

    // 创建 PixelMap
    OH_PixelMap_InitializationOpts opts;
    opts.pixelFormat = IMAGE_PIXEL_FORMAT_RGBA_8888;
    opts.alphaType = IMAGE_ALPHA_TYPE_OPAQUE;
    opts.size.width = width;
    opts.size.height = height;

    OH_PixelMapNative* pixelMap = nullptr;
    int32_t ret = OH_PixelMap_Native_Create(&opts, &pixelMap);
    if (ret != IMAGE_SUCCESS || !pixelMap) {
        OH_LOG_ERROR(LOG_APP, "Failed to create PixelMap for encoding: %{public}d", ret);
        return false;
    }

    // 写入像素数据
    void* pixelData = nullptr;
    uint32_t capacity = 0;
    ret = OH_PixelMap_Native_GetPixels(pixelMap, &pixelData, &capacity);
    if (ret != IMAGE_SUCCESS || !pixelData || capacity < rgbaSize) {
        OH_LOG_ERROR(LOG_APP, "Failed to get PixelMap buffer: %{public}d, capacity=%{public}u",
                     ret, capacity);
        OH_PixelMap_Native_Release(pixelMap);
        return false;
    }

    memcpy(pixelData, rgbaBuffer, rgbaSize);

    // 创建 ImagePacker
    OH_ImagePackerNative* packer = nullptr;
    ret = OH_ImagePackerNative_Create(&packer);
    if (ret != IMAGE_SUCCESS || !packer) {
        OH_LOG_ERROR(LOG_APP, "Failed to create ImagePacker: %{public}d", ret);
        OH_PixelMap_Native_Release(pixelMap);
        return false;
    }

    // 设置 PNG 格式
    ret = OH_ImagePackerNative_SetFormat(packer, "image/png");
    if (ret != IMAGE_SUCCESS) {
        OH_LOG_ERROR(LOG_APP, "Failed to set PNG format: %{public}d", ret);
        OH_PixelMap_Native_Release(pixelMap);
        OH_ImagePackerNative_Release(packer);
        return false;
    }

    // 打包 PixelMap
    ret = OH_ImagePackerNative_PackPixelMap(packer, pixelMap);
    if (ret != IMAGE_SUCCESS) {
        OH_LOG_ERROR(LOG_APP, "Failed to pack PixelMap: %{public}d", ret);
        OH_PixelMap_Native_Release(pixelMap);
        OH_ImagePackerNative_Release(packer);
        return false;
    }

    // 获取编码后的数据
    void* pngData = nullptr;
    size_t pngSize = 0;
    ret = OH_ImagePackerNative_GetData(packer, &pngData, &pngSize);
    if (ret != IMAGE_SUCCESS || !pngData || pngSize == 0) {
        OH_LOG_ERROR(LOG_APP, "Failed to get PNG data: %{public}d", ret);
        OH_PixelMap_Native_Release(pixelMap);
        OH_ImagePackerNative_Release(packer);
        return false;
    }

    // 分配缓冲区并复制数据
    *outPngBuffer = malloc(pngSize);
    if (!*outPngBuffer) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate PNG buffer: size=%{public}zu", pngSize);
        OH_PixelMap_Native_Release(pixelMap);
        OH_ImagePackerNative_Release(packer);
        return false;
    }

    memcpy(*outPngBuffer, pngData, pngSize);
    *outPngSize = pngSize;

    OH_PixelMap_Native_Release(pixelMap);
    OH_ImagePackerNative_Release(packer);

    OH_LOG_INFO(LOG_APP, "PNG encode successful: %{public}dx%{public}d, size=%{public}zu",
                width, height, pngSize);
    return true;
}

// ==================== 堆叠接口 ====================

bool ImageProcessor::initStacking(int32_t totalFrames, uint32_t width, uint32_t height) {
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
                                  uint32_t width, uint32_t height, ImageFormat format, bool isFirst) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 快速失败：空指针检查
    if (!rawBuffer || rawSize == 0) {
        OH_LOG_ERROR(LOG_APP, "processFrame: null or empty input buffer");
        return false;
    }

    if (!initialized_) {
        OH_LOG_ERROR(LOG_APP, "Session not initialized");
        return false;
    }

    OH_LOG_INFO(LOG_APP, "Processing frame %{public}d/%{public}d, rawSize=%{public}zu, %{public}dx%{public}d",
                frameIndex + 1, totalFrames_, rawSize, width, height);

    // 1. 解码原始图像为 RGBA
    uint8_t* rgbaBuffer = nullptr;
    size_t rgbaSize = 0;
    uint32_t decodedWidth = 0, decodedHeight = 0;

    if (!decode(rawBuffer, rawSize, format, &rgbaBuffer, &rgbaSize, &decodedWidth, &decodedHeight)) {
        OH_LOG_ERROR(LOG_APP, "Failed to decode image for frame %{public}d", frameIndex);
        return false;
    }

    // 快速失败：验证解码结果
    if (!rgbaBuffer) {
        OH_LOG_ERROR(LOG_APP, "decode succeeded but returned null buffer");
        return false;
    }
    if (rgbaSize == 0) {
        OH_LOG_ERROR(LOG_APP, "decode succeeded but returned zero size");
        free(rgbaBuffer);
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

    // 快速失败：空指针检查
    if (!outBuffer || !outSize) {
        OH_LOG_ERROR(LOG_APP, "getCurrentResult: null output parameters");
        return false;
    }

    if (!initialized_ || processedFrames_ == 0) {
        OH_LOG_ERROR(LOG_APP, "Not initialized or no frames processed");
        return false;
    }

    if (!rgbaBuffer_ || !accumulateBuffer_) {
        OH_LOG_ERROR(LOG_APP, "getCurrentResult: null internal buffers");
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

    // 编码为 PNG（使用已实现的 PNG 编码）
    return encodePng(rgbaBuffer_, static_cast<size_t>(width_) * height_ * 4,
                     width_, height_, outBuffer, outSize);
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
    // 快速失败：空指针检查
    if (!rgbaBuffer) {
        OH_LOG_ERROR(LOG_APP, "stackFrame: null rgbaBuffer");
        return false;
    }
    if (!accumulateBuffer_) {
        OH_LOG_ERROR(LOG_APP, "stackFrame: null accumulateBuffer_");
        return false;
    }
    if (width_ <= 0 || height_ <= 0) {
        OH_LOG_ERROR(LOG_APP, "stackFrame: invalid dimensions %{public}dx%{public}d", width_, height_);
        return false;
    }

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
