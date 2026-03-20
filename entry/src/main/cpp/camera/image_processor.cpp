#include "image_processor.h"
#include "hilog/log.h"

#include <cstring>
#include <cmath>
#include <cstdlib>
#include "opencv2/opencv.hpp"

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

// ==================== 解bayer接口 ====================
Bgra16Raw ImageProcessor::dngToBGRA16(const void *dng_data, size_t dng_size) {
    LibRaw processor;

    // ── 1. 从内存打开 ──────────────────────────────────────────────────────
    // open_buffer 不拷贝数据，dng_data 在整个调用期间必须有效
    int ret = processor.open_buffer(const_cast<void *>(dng_data), dng_size);
    if (ret != LIBRAW_SUCCESS)
        throw std::runtime_error(
            std::string("open_buffer 失败: ") + libraw_strerror(ret));

    // ── 2. 解包 RAW 数据 ───────────────────────────────────────────────────
    ret = processor.unpack();
    if (ret != LIBRAW_SUCCESS)
        throw std::runtime_error(
            std::string("unpack 失败: ") + libraw_strerror(ret));

    // ── 3. 设置后处理参数（固定 16-bit 线性输出）─────────────────────────
    libraw_output_params_t &params = processor.imgdata.params;
    params.output_bps      = 16;   // 16-bit 输出
    params.gamm[0]         = 0.45;  // 关闭 gamma，保留线性数据
    params.gamm[1]         = 0.45;
    params.no_auto_bright  = 1;    // 关闭自动亮度
    params.use_camera_wb   = 0;    // 使用相机白平衡
    params.output_color    = 1;    // sRGB 输出色彩空间

    // ── 4. 后处理（去马赛克 + 颜色转换）──────────────────────────────────
    ret = processor.dcraw_process();
    if (ret != LIBRAW_SUCCESS)
        throw std::runtime_error(
            std::string("dcraw_process 失败: ") + libraw_strerror(ret));

    // ── 5. 取出内存 RGB 图像 ───────────────────────────────────────────────
    int errcode = LIBRAW_SUCCESS;
    libraw_processed_image_t *img = processor.dcraw_make_mem_image(&errcode);
    if (!img || errcode != LIBRAW_SUCCESS)
    {
        if (img) LibRaw::dcraw_clear_mem(img);
        throw std::runtime_error(
            std::string("dcraw_make_mem_image 失败: ") + libraw_strerror(errcode));
    }
    
    // dcraw_make_mem_image 始终返回 LIBRAW_IMAGE_BITMAP
    if (img->type != LIBRAW_IMAGE_BITMAP)
    {
        LibRaw::dcraw_clear_mem(img);
        throw std::runtime_error("dcraw_make_mem_image: 返回类型不是 BITMAP");
    }

    // ── 6. 填充输出结构体，深拷贝像素数据（16-bit，小端序）──────────────
    Bgra16Raw result;
    result.width  = img->width;//img->width;
    result.height = img->height;//img->height;
    
    const uint16_t *src = reinterpret_cast<const uint16_t *>(img->data);
    //result.size = img->data_size;
//    void* bufferTemp = malloc(result.size);
//    std::memcpy(bufferTemp, src, result.size);
//    result.data = (uint16_t*) bufferTemp;
    
    Frame resFrame = convertLibRawToBGRA16(img);
    //std::memcpy(result.data, resFrame.data(), resFrame.size() * 2);
    result.data = resFrame.data();
    result.size = resFrame.size()*sizeof(resFrame.at(0));
    // ── 7. 释放 LibRaw 分配的缓冲区 ───────────────────────────────────────
    LibRaw::dcraw_clear_mem(img);
    return result;
}

const uint8_t CHANNELS_RGBA = 4;
Frame ImageProcessor::convertLibRawToBGRA16(libraw_processed_image_t* img) {
    /*if (img->width != width_ || img->height != height_)
        throw std::runtime_error(
            "DNG 尺寸与配置不符: " +
            std::to_string(img->width) + "x" + std::to_string(img->height));*/

    const auto* src = reinterpret_cast<const uint16_t*>(img->data);
    const int   ch = img->colors;  // 通常为 3（RGB）
    const int   n = img->width * img->height;

    Frame frame(static_cast<size_t>(n) * CHANNELS_RGBA);
    for (int i = 0; i < n; ++i) {
        const uint16_t r = src[i * ch + 0];
        const uint16_t g = src[i * ch + 1];
        const uint16_t b = (ch >= 3) ? src[i * ch + 2] : g;
        frame[i * CHANNELS_RGBA + 0] = b;        // B
        frame[i * CHANNELS_RGBA + 1] = g;        // G
        frame[i * CHANNELS_RGBA + 2] = r;        // R
        frame[i * CHANNELS_RGBA + 3] = 65535;    // A = 完全不透明
    }
    return frame;
}

MeanRes ImageProcessor::MotionAnalysisAndStack(uint16_t *nativeBuffer1, uint16_t *nativeBuffer2, uint16_t width, uint16_t height) {
    circular_buf_t cir_buf;
    memset(&cir_buf, 0, sizeof(circular_buf_t));

    // 用两个 uint16_t* 组装数据
    cir_buf.img_arr[0] = nativeBuffer1;
    cir_buf.img_arr[1] = nativeBuffer2;
    cir_buf.cir_size = 2;

    // TODO: 需要根据实际情况设置宽高
    cir_buf.width = width;
    cir_buf.height = height;

    MeanRes res;

    motion_analysis_and_stack(cir_buf, res.mean_x, res.mean_y);
    return res;
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
