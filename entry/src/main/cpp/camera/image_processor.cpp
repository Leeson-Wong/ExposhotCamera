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

MeanRes ImageProcessor::MotionAnalysisAndStack(uint16_t *nativeBuffer1, uint16_t *nativeBuffer2, uint32_t width, uint32_t height) {
    // 边界检查：第三方库 motion_stack 使用 uint16_t，最大支持 65535
    if (width > UINT16_MAX || height > UINT16_MAX) {
        OH_LOG_ERROR(LOG_APP, "MotionAnalysisAndStack: dimension exceeds uint16_t limit (%{public}u x %{public}u)",
                     width, height);
        MeanRes emptyRes = {{0.0f, 0.0f}, {0.0f, 0.0f}};
        return emptyRes;
    }

    circular_buf_t cir_buf;
    memset(&cir_buf, 0, sizeof(circular_buf_t));

    // 用两个 uint16_t* 组装数据
    cir_buf.img_arr[0] = nativeBuffer1;
    cir_buf.img_arr[1] = nativeBuffer2;
    cir_buf.cir_size = 2;

    cir_buf.width = static_cast<uint16_t>(width);
    cir_buf.height = static_cast<uint16_t>(height);

    MeanRes res;

    motion_analysis_and_stack(cir_buf, res.mean_x, res.mean_y);
    return res;
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

    // TODO: 临时 - 跳过缓冲区分配（调试流程编排）
    OH_LOG_INFO(LOG_APP, "Stacking session initialized (passthrough mode): %{public}d frames, %{public}dx%{public}d",
                totalFrames, width, height);

    initialized_ = true;
    notifyState("initialized", "Stacking session started (passthrough mode)");
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

    // TODO: 临时 - 直接透传 buffer，跳过编解码和堆叠
    // 释放之前的 buffer
    if (passthroughBuffer_) {
        free(passthroughBuffer_);
        passthroughBuffer_ = nullptr;
        passthroughSize_ = 0;
    }
    // 复制新的 buffer
    passthroughBuffer_ = malloc(rawSize);
    if (!passthroughBuffer_) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate passthrough buffer");
        return false;
    }
    memcpy(passthroughBuffer_, rawBuffer, rawSize);
    passthroughSize_ = rawSize;
    OH_LOG_INFO(LOG_APP, "Passthrough: copied buffer, size=%{public}zu", passthroughSize_);

    /* 注释掉编解码和堆叠
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
    */

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

    // TODO: 临时 - 直接返回透传的 buffer
    if (!passthroughBuffer_ || passthroughSize_ == 0) {
        OH_LOG_ERROR(LOG_APP, "getCurrentResult: no passthrough buffer available");
        return false;
    }

    OH_LOG_INFO(LOG_APP, "Returning passthrough buffer, size=%{public}zu", passthroughSize_);

    // 复制一份返回（调用者负责 free）
    *outBuffer = malloc(passthroughSize_);
    if (!*outBuffer) {
        OH_LOG_ERROR(LOG_APP, "Failed to allocate output buffer");
        return false;
    }
    memcpy(*outBuffer, passthroughBuffer_, passthroughSize_);
    *outSize = passthroughSize_;

    return true;
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

    // TODO: 临时 - 释放透传 buffer
    if (passthroughBuffer_) {
        free(passthroughBuffer_);
        passthroughBuffer_ = nullptr;
        passthroughSize_ = 0;
    }

    initialized_ = false;
    totalFrames_ = 0;
    processedFrames_ = 0;
    width_ = 0;
    height_ = 0;

    OH_LOG_INFO(LOG_APP, "ImageProcessor reset");
}

bool ImageProcessor::stackFrame(uint16_t* bgraBuffer, bool isFirst) {
    // 快速失败：空指针检查
    if (!bgraBuffer) {
        OH_LOG_ERROR(LOG_APP, "stackFrame: null rgbaBuffer");
        return false;
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
