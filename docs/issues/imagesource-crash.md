# ImageSource 崩溃问题

## 问题描述

在处理拍照返回的图像数据时，`createPixelMap` 和 `imageSource.release()` 过程可能出现崩溃。

## 现象

- `image.createImageSource(data.buffer)` 后调用 `createPixelMap()` 可能崩溃
- `imageSource.release()` 调用可能崩溃
- 崩溃与 EXIF 数据处理相关的野指针有关

## 可能原因

1. **EXIF 野指针**
   - ImageSource 内部解析 EXIF 数据时存在野指针
   - GC 延迟回收导致 EXIF 数据在 release 时已被释放

2. **Buffer 生命周期问题**
   - Native 层传递的 ArrayBuffer 在 ArkTS 层使用时可能已被释放
   - 跨语言边界的内存管理不当

3. **并发访问问题**
   - 多次拍照时，前一次的 ImageSource 未正确释放就开始新的处理
   - 图像处理回调在非 UI 线程执行

4. **资源释放时序**
   - 在 ImageSource 完成解码前调用 release
   - PixelMap 未创建完成就释放 ImageSource

## 当前缓解措施

```typescript
async processImageData(data: nativeCamera.ImageData): Promise<void> {
  if (!data.buffer) {
    return;
  }
  // 防抖：防止并发处理
  if (this.isProcessingImage) {
    return;
  }
  this.isProcessingImage = true;
  try {
    // 先释放旧的 PixelMap
    if (this.capturedImage) {
      this.capturedImage.release();
      this.capturedImage = undefined;
    }

    const imageSource = image.createImageSource(data.buffer);
    const pixelMap = await imageSource.createPixelMap();
    this.capturedImage = pixelMap;
    // 显式释放 ImageSource
    imageSource.release();
  } catch (error) {
    hilog.error(DOMAIN, TAG, 'Failed to process image: %{public}s', JSON.stringify(error));
  } finally {
    this.isProcessingImage = false;
  }
}
```

## 待确认事项

- [ ] 收集崩溃日志，定位具体崩溃位置
- [ ] 确认崩溃是否与特定图片格式/大小相关
- [ ] 测试使用 creatingImageSource 的其他选项参数
- [ ] 尝试延迟 release ImageSource
- [ ] 考虑在 Native 层完成图像解码，直接返回 PixelMap 数据

## 相关代码

- `entry/src/main/ets/pages/TestBasicCamera.ets` - `processImageData()`
- `entry/src/main/cpp/camera/expo_camera.cpp` - 图像回调处理

## 历史记录

- 2024-03-23: 初始记录，基于用户反馈
