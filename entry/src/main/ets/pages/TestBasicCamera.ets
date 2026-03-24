import nativeCamera from 'libexpocamera.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { image } from '@kit.ImageKit';
import { router } from '@kit.ArkUI';
import { SAFE_AREA_TOP, SAFE_AREA_BOTTOM } from '../util/WindowManager';

const DOMAIN = 0x3200;
const TAG = 'TestBasicCamera';

// 对焦模式枚举
enum FocusMode {
  MANUAL = 0,
  CONTINUOUS_AUTO = 1,
  AUTO = 2,
  LOCKED = 3,
}

@Entry
@Component
struct TestBasicCamera {
  @StorageProp(SAFE_AREA_TOP) safeTop: number = 0;
  @StorageProp(SAFE_AREA_BOTTOM) safeBottom: number = 0;
  @State cameraStatus: string = '未初始化';
  @State photoReady: boolean = false;
  @State slotId: string = '';
  @State hasPreview: boolean = false;
  @State capturedImage: PixelMap | undefined = undefined;
  // 缩放控制
  @State zoomRatio: number = 1.0;
  @State zoomMin: number = 1.0;
  @State zoomMax: number = 1.0;
  @State zoomSupported: boolean = false;
  // 对焦控制
  @State focusMode: number = FocusMode.CONTINUOUS_AUTO;
  private surfaceId: string = ''
  // 防抖：防止并发处理图像
  private isProcessingImage: boolean = false;
  private xComponentController: XComponentController = new XComponentController();

  aboutToAppear(): void {
    // 注册图像数据回调
    nativeCamera.registerImageDataCallback((data) => {
      hilog.info(DOMAIN, TAG, 'Image data received: sessionId=%{public}s, %{public}dx%{public}d, isFinal=%{public}s',
        data.sessionId || '', data.width, data.height, data.isFinal?.toString() || 'false');
      this.cameraStatus = '拍照成功，正在处理...';
      this.processImageData(data);
    });

    // 订阅相机状态
    nativeCamera.subscribeState((state: string, message: string) => {
      hilog.info(DOMAIN, TAG, 'Camera state: %{public}s, message: %{public}s', state, message);
      this.cameraStatus = `${state}: ${message}`;
    });

    // 初始化相机
    const result = nativeCamera.initCamera(nativeCamera.CaptureMode.SINGLE);
    hilog.info(DOMAIN, TAG, 'initCamera result: %{public}d', result);
    if (result === 0) {
      this.cameraStatus = '相机初始化成功';
    } else {
      this.cameraStatus = `相机初始化失败: ${result}`;
    }
  }

  aboutToDisappear(): void {
    if (this.slotId) {
      nativeCamera.unregisterObserver(this.slotId);
    }
    nativeCamera.unsubscribeState();
    nativeCamera.stopPreview();
    nativeCamera.releaseCamera();
  }

  onPreviewChanged = (activeSlotId: string, activeSurfaceId: string): void => {
    hilog.info(DOMAIN, TAG, 'Preview changed: activeSlotId=%{public}s, activeSurfaceId=%{public}s', activeSlotId, activeSurfaceId);

    if (activeSurfaceId == this.surfaceId) {
      this.slotId = activeSlotId;
      this.hasPreview = true;
      this.cameraStatus = '预览中';
      this.photoReady = nativeCamera.isPhotoOutputReady();
      this.initCameraParams();
    }
  }

  registerObserver(surfaceId: string): string {
    const slotId = nativeCamera.registerObserver(surfaceId, this.onPreviewChanged);
    nativeCamera.startPreview(surfaceId);
    hilog.info(DOMAIN, TAG, 'registerObserver: slotId=%{public}s', slotId);
    return slotId;
  }

  initCameraParams(): void {
    this.zoomSupported = nativeCamera.isZoomSupported();
    if (this.zoomSupported) {
      const range = nativeCamera.getZoomRatioRange();
      this.zoomMin = range.min;
      this.zoomMax = range.max;
    }
    this.zoomRatio = nativeCamera.getZoomRatio();
    this.focusMode = nativeCamera.getFocusMode();
  }

  async processImageData(data: nativeCamera.ImageData): Promise<void> {
    if (!data.buffer) {
      hilog.warn(DOMAIN, TAG, 'No image buffer in data');
      return;
    }
    // 防抖：如果正在处理，跳过本次
    if (this.isProcessingImage) {
      hilog.warn(DOMAIN, TAG, 'Image processing in progress, skipping');
      return;
    }
    this.isProcessingImage = true;
    try {
      // 先释放旧的 PixelMap，避免内存泄漏
      if (this.capturedImage) {
        this.capturedImage.release();
        this.capturedImage = undefined;
      }

      const imageSource = image.createImageSource(data.buffer);
      const pixelMap = await imageSource.createPixelMap();
      this.capturedImage = pixelMap;
      // 显式释放 ImageSource，避免 GC 延迟回收时 EXIF 野指针崩溃
      imageSource.release();
      hilog.info(DOMAIN, TAG, 'Image processed: %{public}dx%{public}d', data.width, data.height);
      this.cameraStatus = '拍照成功，已显示';
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'Failed to process image: %{public}s', JSON.stringify(error));
    } finally {
      this.isProcessingImage = false;
    }
  }

  takePhoto(): void {
    if (!this.photoReady) {
      this.cameraStatus = '拍照未就绪';
      return;
    }
    const sessionIdRes = nativeCamera.takePhoto();
    if (sessionIdRes && sessionIdRes.sessionId.length > 0) {
      this.cameraStatus = '正在拍照...';
      hilog.info(DOMAIN, TAG, 'takePhoto sessionId: %{public}s', sessionIdRes.sessionId);
    } else {
      this.cameraStatus = '拍照失败: 无法获取 sessionId';
      hilog.error(DOMAIN, TAG, 'takePhoto failed: empty sessionId');
    }
  }

  setZoom(ratio: number): void {
    const result = nativeCamera.setZoomRatio(ratio);
    if (result === 0) {
      this.zoomRatio = ratio;
    }
  }

  setFocus(mode: number): void {
    const result = nativeCamera.setFocusMode(mode);
    if (result === 0) {
      this.focusMode = mode;
    }
  }

  build() {
    Stack() {
      // 底层 - 预览流铺满全屏
      XComponent({
        id: 'cameraPreview',
        type: 'surface',
        libraryname: 'expocamera',
        controller: this.xComponentController
      })
        .onLoad(() => {
          hilog.info(DOMAIN, TAG, 'XComponent onLoad');
          const surfaceId = this.xComponentController.getXComponentSurfaceId();
          this.surfaceId = surfaceId;
          this.registerObserver(surfaceId);
        })
        .width('100%')
        .height('100%')

      // 顶层 - UI叠加
      Column() {
        // 顶部状态栏
        Row() {
          Button('< 返回')
            .fontSize(14)
            .backgroundColor('#666666')
            .onClick(() => {
              router.back();
            })

          Text('基础相机测试')
            .fontSize(16)
            .fontColor(Color.White)
            .layoutWeight(1)
            .textAlign(TextAlign.Center)

          Text(this.hasPreview ? '预览中' : '无预览')
            .fontSize(12)
            .fontColor(this.hasPreview ? Color.Green : Color.Gray)
        }
        .width('100%')
        .padding({ top: this.safeTop + 8, left: 12, right: 12, bottom: 8 })
        .backgroundColor('#80000000')

        // 状态提示
        Text(this.cameraStatus)
          .fontSize(14)
          .fontColor(Color.White)
          .backgroundColor('#80000000')
          .padding(8)
          .borderRadius(4)
          .margin({ top: 16 })

        // 弹性空间
        Blank()

        // 缩略图预览
        if (this.capturedImage) {
          Column() {
            Image(this.capturedImage)
              .width(80)
              .height(100)
              .border({ width: 2, color: Color.White })
              .borderRadius(4)
          }
          .width('100%')
          .alignItems(HorizontalAlign.End)
          .padding({ right: 16, bottom: 16 })
        }

        // 底部控制区
        Column() {
          // 缩放控制
          if (this.zoomSupported) {
            Row() {
              Text('缩放:')
                .fontSize(14)
                .fontColor(Color.White)
                .width(50)

              Slider({
                value: this.zoomRatio,
                min: this.zoomMin,
                max: this.zoomMax,
                step: 0.1,
                style: SliderStyle.OutSet
              })
                .blockColor(Color.White)
                .trackColor('#666666')
                .selectedColor('#007DFF')
                .layoutWeight(1)
                .onChange((value: number) => {
                  this.setZoom(value);
                })

              Text(`${this.zoomRatio.toFixed(1)}x`)
                .fontSize(14)
                .fontColor(Color.White)
                .width(50)
                .textAlign(TextAlign.End)
            }
            .width('100%')
            .padding({ left: 16, right: 16, top: 8, bottom: 8 })
          }

          // 对焦模式控制
          Row() {
            Text('对焦:')
              .fontSize(14)
              .fontColor(Color.White)
              .width(50)

            Button('连续')
              .fontSize(12)
              .height(28)
              .backgroundColor(this.focusMode === FocusMode.CONTINUOUS_AUTO ? '#007DFF' : '#666666')
              .margin({ left: 4, right: 4 })
              .onClick(() => this.setFocus(FocusMode.CONTINUOUS_AUTO))

            Button('自动')
              .fontSize(12)
              .height(28)
              .backgroundColor(this.focusMode === FocusMode.AUTO ? '#007DFF' : '#666666')
              .margin({ left: 4, right: 4 })
              .onClick(() => this.setFocus(FocusMode.AUTO))

            Button('手动')
              .fontSize(12)
              .height(28)
              .backgroundColor(this.focusMode === FocusMode.MANUAL ? '#007DFF' : '#666666')
              .margin({ left: 4, right: 4 })
              .onClick(() => this.setFocus(FocusMode.MANUAL))
          }
          .width('100%')
          .padding({ left: 16, right: 16, top: 8, bottom: 8 })

          // 拍照按钮
          Row() {
            Button('拍照')
              .fontSize(18)
              .width('80%')
              .height(50)
              .enabled(this.photoReady)
              .backgroundColor(this.photoReady ? '#007DFF' : '#666666')
              .onClick(() => {
                this.takePhoto();
              })
          }
          .width('100%')
          .padding({ left: 16, right: 16, top: 8, bottom: this.safeBottom + 16 })
          .justifyContent(FlexAlign.Center)
        }
        .width('100%')
        .backgroundColor('#80000000')
      }
      .width('100%')
      .height('100%')
    }
    .width('100%')
    .height('100%')
  }
}
