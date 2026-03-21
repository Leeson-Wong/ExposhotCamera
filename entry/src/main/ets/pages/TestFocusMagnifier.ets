import nativeCamera, { FocusMode } from 'libexpocamera.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { image } from '@kit.ImageKit';
import { router } from '@kit.ArkUI';
import { componentSnapshot } from '@kit.ArkUI';

const DOMAIN = 0x3200;
const TAG = 'TestFocusMagnifier';

// 放大镜配置
const MAGNIFIER_SIZE = 180;           // 放大镜直径
const MAGNIFIER_ZOOM = 2.0;           // 放大倍数
const MAGNIFIER_BORDER_WIDTH = 3;     // 边框宽度
const MAGNIFIER_DURATION = 2000;      // 显示时长(ms)

@Entry
@Component
struct TestFocusMagnifier {
  @State cameraStatus: string = '未初始化';
  @State photoReady: boolean = false;
  @State slotId: string = '';
  @State hasPreview: boolean = false;

  // 对焦控制
  @State focusMode: number = FocusMode.CONTINUOUS_AUTO;
  @State focusPointX: number = 0.5;
  @State focusPointY: number = 0.5;

  // 放大镜状态
  @State showMagnifier: boolean = false;
  @State magnifierX: number = 0;
  @State magnifierY: number = 0;
  @State magnifierPixelMap: PixelMap | undefined = undefined;
  @State focusAnimating: boolean = false;  // 对焦动画状态

  private xComponentController: XComponentController = new XComponentController();
  private previewStackId: string = 'previewStack';
  private magnifierTimer: number = -1;
  private previewWidth: number = 0;
  private previewHeight: number = 0;

  aboutToAppear(): void {
    nativeCamera.registerImageDataCallback((data) => {
      hilog.info(DOMAIN, TAG, 'Image data received');
      this.cameraStatus = '拍照成功';
    });

    nativeCamera.subscribeState((state: string, message: string) => {
      hilog.info(DOMAIN, TAG, 'Camera state: %{public}s', state);
      this.cameraStatus = `${state}: ${message}`;
    });

    const result = nativeCamera.initCamera();
    if (result === 0) {
      this.cameraStatus = '相机初始化成功';
    } else {
      this.cameraStatus = `相机初始化失败: ${result}`;
    }
  }

  aboutToDisappear(): void {
    if (this.magnifierTimer !== -1) {
      clearTimeout(this.magnifierTimer);
    }
    if (this.slotId) {
      nativeCamera.unregisterObserver(this.slotId);
    }
    nativeCamera.unsubscribeState();
    nativeCamera.stopPreview();
    nativeCamera.releaseCamera();
  }

  onPreviewChanged = (activeSlotId: string, activeSurfaceId: string): void => {
    hilog.info(DOMAIN, TAG, 'Preview changed: activeSlotId=%{public}s', activeSlotId);

    if (this.slotId === '' && activeSlotId !== '') {
      setTimeout(() => {
        if (this.slotId === activeSlotId) {
          this.hasPreview = true;
          this.cameraStatus = '预览中 - 点击屏幕对焦';
          this.photoReady = nativeCamera.isPhotoOutputReady();
          this.focusMode = nativeCamera.getFocusMode();
        }
      }, 50);
      return;
    }

    this.hasPreview = (activeSlotId === this.slotId && activeSlotId !== '');
    if (activeSlotId !== '') {
      this.cameraStatus = '预览中 - 点击屏幕对焦';
      this.photoReady = nativeCamera.isPhotoOutputReady();
      this.focusMode = nativeCamera.getFocusMode();
    }
  }

  registerObserver(surfaceId: string): string {
    const slotId = nativeCamera.registerObserver(surfaceId, this.onPreviewChanged);
    nativeCamera.startPreview(surfaceId);
    hilog.info(DOMAIN, TAG, 'registerObserver: slotId=%{public}s', slotId);
    return slotId;
  }

  // 处理触摸事件 - 显示放大镜并设置对焦点
  async handleTouch(x: number, y: number): Promise<void> {
    if (!this.hasPreview) {
      this.cameraStatus = '请等待预览启动';
      return;
    }

    // 计算归一化坐标 (0-1)
    const normalizedX = x / this.previewWidth;
    const normalizedY = y / this.previewHeight;

    hilog.info(DOMAIN, TAG, 'Touch at: (%{public}f, %{public}f) -> normalized: (%{public}f, %{public}f)',
              x, y, normalizedX, normalizedY);

    // 更新对焦点
    this.focusPointX = normalizedX;
    this.focusPointY = normalizedY;

    // 设置相机对焦点
    if (this.focusMode !== FocusMode.MANUAL && this.focusMode !== FocusMode.AUTO) {
      nativeCamera.setFocusMode(FocusMode.AUTO);
    }
    nativeCamera.setFocusPoint(normalizedX, normalizedY);

    // 显示放大镜
    await this.showMagnifierAt(x, y);

    // 开始对焦动画
    this.focusAnimating = true;

    // 清除之前的定时器
    if (this.magnifierTimer !== -1) {
      clearTimeout(this.magnifierTimer);
    }

    // 设置自动隐藏
    this.magnifierTimer = setTimeout(() => {
      this.hideMagnifier();
    }, MAGNIFIER_DURATION);

    this.cameraStatus = `对焦点: (${(normalizedX * 100).toFixed(0)}%, ${(normalizedY * 100).toFixed(0)}%)`;
  }

  // 显示放大镜
  async showMagnifierAt(touchX: number, touchY: number): Promise<void> {
    try {
      // 截取预览区域
      const snapshot = await componentSnapshot.get(this.previewStackId);
      if (!snapshot) {
        hilog.warn(DOMAIN, TAG, 'Failed to get snapshot');
        return;
      }

      // 计算裁剪区域 (考虑放大倍数)
      const cropSize = MAGNIFIER_SIZE / MAGNIFIER_ZOOM;
      const cropX = Math.max(0, Math.min(touchX - cropSize / 2, this.previewWidth - cropSize));
      const cropY = Math.max(0, Math.min(touchY - cropSize / 2, this.previewHeight - cropSize));

      hilog.info(DOMAIN, TAG, 'Crop area: x=%{public}f, y=%{public}f, size=%{public}f',
                cropX, cropY, cropSize);

      // 创建裁剪后的 PixelMap
      const imageInfo = await snapshot.getImageInfo();
      hilog.info(DOMAIN, TAG, 'Snapshot size: %{public}dx%{public}d', imageInfo.size.width, imageInfo.size.height);

      // 计算缩放比例 (snapshot 可能与实际预览尺寸不同)
      const scaleX = imageInfo.size.width / this.previewWidth;
      const scaleY = imageInfo.size.height / this.previewHeight;

      // 裁剪区域 (在 snapshot 坐标系中)
      const actualCropX = cropX * scaleX;
      const actualCropY = cropY * scaleY;
      const actualCropSize = cropSize * scaleX;

      // 先复制 snapshot，然后裁剪
      // 获取 snapshot 的像素数据
      const pixelMapNumber = imageInfo.size.width * imageInfo.size.height * 4; // RGBA
      const readBuffer = new ArrayBuffer(pixelMapNumber);
      await snapshot.readPixelsToBuffer(readBuffer);

      // 创建新的 PixelMap 用于裁剪区域
      const cropBufferSize = Math.ceil(actualCropSize) * Math.ceil(actualCropSize) * 4;
      const cropBuffer = new ArrayBuffer(cropBufferSize);

      // 手动裁剪：从 readBuffer 复制指定区域到 cropBuffer
      const srcWidth = imageInfo.size.width;
      const cropW = Math.ceil(actualCropSize);
      const cropH = Math.ceil(actualCropSize);
      const srcX = Math.floor(actualCropX);
      const srcY = Math.floor(actualCropY);

      const srcView = new Uint8Array(readBuffer);
      const dstView = new Uint8Array(cropBuffer);

      for (let row = 0; row < cropH; row++) {
        for (let col = 0; col < cropW; col++) {
          const srcIdx = ((srcY + row) * srcWidth + (srcX + col)) * 4;
          const dstIdx = (row * cropW + col) * 4;
          if (srcIdx + 3 < srcView.length && dstIdx + 3 < dstView.length) {
            dstView[dstIdx] = srcView[srcIdx];
            dstView[dstIdx + 1] = srcView[srcIdx + 1];
            dstView[dstIdx + 2] = srcView[srcIdx + 2];
            dstView[dstIdx + 3] = srcView[srcIdx + 3];
          }
        }
      }

      // 创建裁剪后的 PixelMap
      const croppedPixelMap = await image.createPixelMap(cropBuffer, {
        size: { width: cropW, height: cropH },
        pixelFormat: image.PixelMapFormat.RGBA_8888
      });

      // 缩放到放大镜尺寸
      await croppedPixelMap.scale(MAGNIFIER_ZOOM, MAGNIFIER_ZOOM);

      this.magnifierPixelMap = croppedPixelMap;
      this.magnifierX = touchX;
      this.magnifierY = touchY;
      this.showMagnifier = true;

      hilog.info(DOMAIN, TAG, 'Magnifier shown at (%{public}f, %{public}f)', touchX, touchY);
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'Failed to show magnifier: %{public}s', JSON.stringify(error));
    }
  }

  // 隐藏放大镜
  hideMagnifier(): void {
    this.showMagnifier = false;
    this.focusAnimating = false;
    this.magnifierPixelMap = undefined;
    hilog.info(DOMAIN, TAG, 'Magnifier hidden');
  }

  build() {
    Column() {
      // 顶部状态栏
      Row() {
        Button('< 返回')
          .fontSize(14)
          .backgroundColor('#666666')
          .onClick(() => {
            router.back();
          })

        Text('对焦放大镜测试')
          .fontSize(16)
          .fontColor(Color.White)
          .layoutWeight(1)
          .textAlign(TextAlign.Center)

        Text(this.hasPreview ? '预览中' : '无预览')
          .fontSize(12)
          .fontColor(this.hasPreview ? Color.Green : Color.Gray)
      }
      .width('100%')
      .padding(12)
      .backgroundColor('#80000000')

      // 状态信息
      Text(this.cameraStatus)
        .fontSize(12)
        .fontColor(Color.Yellow)
        .width('100%')
        .textAlign(TextAlign.Center)
        .padding({ top: 8, bottom: 8 })
        .backgroundColor('#60000000')

      // 预览区域
      Stack() {
        // 相机预览
        XComponent({
          id: 'cameraPreview',
          type: 'surface',
          libraryname: 'expocamera',
          controller: this.xComponentController
        })
          .onLoad(() => {
            hilog.info(DOMAIN, TAG, 'XComponent onLoad');
            const surfaceId = this.xComponentController.getXComponentSurfaceId();
            this.slotId = this.registerObserver(surfaceId);
          })
          .width('100%')
          .height('100%')

        // 透明触摸层
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('transparent')
          .onTouch((event: TouchEvent) => {
            const touch = event.touches[0];
            if (!touch) return;

            // 获取预览区域尺寸
            const target = event.target;
            this.previewWidth = Number(target.area.width);
            this.previewHeight = Number(target.area.height);

            if (event.type === TouchType.Down) {
              // 按下时显示放大镜并设置对焦点
              this.handleTouch(touch.x, touch.y);
            } else if (event.type === TouchType.Move) {
              // 移动时更新放大镜位置（不重新截图，只更新位置）
              if (this.showMagnifier) {
                this.magnifierX = touch.x;
                this.magnifierY = touch.y;
                this.focusPointX = touch.x / this.previewWidth;
                this.focusPointY = touch.y / this.previewHeight;
              }
            } else if (event.type === TouchType.Up) {
              // 手指抬起时重新设置对焦点
              if (this.showMagnifier) {
                nativeCamera.setFocusPoint(this.focusPointX, this.focusPointY);
              }
            }
          })

        // 对焦点标记
        if (this.hasPreview) {
          Column() {
            // 对焦框
            Column() {
              Row() {
                Text('╔').fontSize(20).fontColor(Color.White)
              }.width('100%').justifyContent(FlexAlign.Start)
              Row() {
                Text('╚').fontSize(20).fontColor(Color.White)
              }.width('100%').justifyContent(FlexAlign.Start).layoutWeight(1)
            }
            .width(50)
            .height(50)
            .justifyContent(FlexAlign.SpaceBetween)

            // 中心十字
            Text('+')
              .fontSize(24)
              .fontColor(Color.Yellow)
              .fontWeight(FontWeight.Bold)
          }
          .position({ x: `${this.focusPointX * 100}%`, y: `${this.focusPointY * 100}%` })
          .markAnchor({ x: '50%', y: '50%' })
          .opacity(this.focusAnimating ? 1 : 0.7)
          .animation(this.focusAnimating ? {
            duration: 300,
            curve: Curve.EaseInOut,
            iterations: 3,
            playMode: PlayMode.Alternate
          } : undefined)
        }

        // 放大镜
        if (this.showMagnifier && this.magnifierPixelMap) {
          Column() {
            // 放大镜图片
            Stack() {
              Image(this.magnifierPixelMap)
                .width(MAGNIFIER_SIZE)
                .height(MAGNIFIER_SIZE)
                .objectFit(ImageFit.Cover)

              // 十字准星
              Column() {
                Row()
                  .width(20)
                  .height(2)
                  .backgroundColor('rgba(255, 255, 0, 0.8)')
                Row()
                  .width(2)
                  .height(20)
                  .backgroundColor('rgba(255, 255, 0, 0.8)')
              }
              .justifyContent(FlexAlign.Center)
              .alignItems(HorizontalAlign.Center)
            }
            .width(MAGNIFIER_SIZE)
            .height(MAGNIFIER_SIZE)
            .clip(new Circle({ width: MAGNIFIER_SIZE, height: MAGNIFIER_SIZE }))
            .border({
              width: MAGNIFIER_BORDER_WIDTH,
              color: Color.White
            })
            .shadow({
              radius: 10,
              color: 'rgba(0, 0, 0, 0.5)',
              offsetX: 0,
              offsetY: 4
            })

            // 小三角指示器
            Path()
              .width(20)
              .height(10)
              .commands('M10 0 L0 10 L20 10 Z')
              .fill(Color.White)
              .margin({ top: -MAGNIFIER_BORDER_WIDTH })
          }
          .position({
            x: Math.max(MAGNIFIER_SIZE / 2 + 20,
                       Math.min(this.magnifierX, this.previewWidth - MAGNIFIER_SIZE / 2 - 20)) - MAGNIFIER_SIZE / 2,
            y: Math.max(MAGNIFIER_SIZE / 2 + 10,
                       Math.min(this.magnifierY - MAGNIFIER_SIZE / 2 - 40, this.previewHeight - MAGNIFIER_SIZE - 50))
          })
          .transition(TransitionEffect.OPACITY.animation({ duration: 200 }))
        }

        // 提示文字
        if (this.hasPreview && !this.showMagnifier) {
          Text('点击画面设置对焦点')
            .fontSize(14)
            .fontColor(Color.Yellow)
            .backgroundColor('#80000000')
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(4)
            .margin({ top: 20 })
        }
      }
      .id(this.previewStackId)
      .layoutWeight(1)
      .width('100%')
      .backgroundColor('#000000')
      .onAreaChange((_oldValue, newValue) => {
        // 更新预览区域尺寸
        this.previewWidth = Number(newValue.width);
        this.previewHeight = Number(newValue.height);
        hilog.info(DOMAIN, TAG, 'Preview area: %{public}dx%{public}d', this.previewWidth, this.previewHeight);
      })

      // 放大镜配置信息
      Row() {
        Text(`放大镜: ${MAGNIFIER_ZOOM}x / ${MAGNIFIER_SIZE}px`)
          .fontSize(12)
          .fontColor(Color.White)
          .layoutWeight(1)

        Text(`显示时长: ${MAGNIFIER_DURATION}ms`)
          .fontSize(12)
          .fontColor(Color.White)
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 8, bottom: 8 })
      .backgroundColor('#60000000')

      // 对焦模式切换
      Row() {
        Text('对焦模式:')
          .fontSize(14)
          .fontColor(Color.White)
          .width(70)

        Button('连续')
          .fontSize(12)
          .height(32)
          .backgroundColor(this.focusMode === FocusMode.CONTINUOUS_AUTO ? '#007DFF' : '#666666')
          .margin({ left: 4, right: 4 })
          .onClick(() => {
            nativeCamera.setFocusMode(FocusMode.CONTINUOUS_AUTO);
            this.focusMode = FocusMode.CONTINUOUS_AUTO;
          })

        Button('自动')
          .fontSize(12)
          .height(32)
          .backgroundColor(this.focusMode === FocusMode.AUTO ? '#007DFF' : '#666666')
          .margin({ left: 4, right: 4 })
          .onClick(() => {
            nativeCamera.setFocusMode(FocusMode.AUTO);
            this.focusMode = FocusMode.AUTO;
          })
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 8, bottom: 8 })
      .backgroundColor('#80000000')

      // 拍照按钮
      Row() {
        Button('拍照')
          .fontSize(18)
          .width('80%')
          .height(50)
          .enabled(this.photoReady)
          .backgroundColor(this.photoReady ? '#007DFF' : '#666666')
          .onClick(() => {
            const result = nativeCamera.takePhoto();
            if (result && result.sessionId) {
              this.cameraStatus = '正在拍照...';
            }
          })
      }
      .width('100%')
      .padding(16)
      .justifyContent(FlexAlign.Center)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }
}
