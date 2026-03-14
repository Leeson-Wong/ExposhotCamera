import nativeCamera from 'libexpocamera.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { image } from '@kit.ImageKit';
import { router } from '@kit.ArkUI';

const DOMAIN = 0x3200;
const TAG = 'TestBasicCamera';

// 对焦模式枚举
enum FocusMode {
  MANUAL = 0,
  CONTINUOUS_AUTO = 1,
  AUTO = 2,
  LOCKED = 3,
};

@Entry
@Component
struct TestBasicCamera {
  @State cameraStatus: string = '未初始化';
  @State photoReady: boolean = false;
  @State slotId: string = '';
  @State hasPreview: boolean = false;

  // 缩放控制
  @State zoomRatio: number = 1.0;
  @State zoomMin: number = 1.0;
  @State zoomMax: number = 1.0;
  @State zoomSupported: boolean = false;

  // 对焦控制
  @State focusMode: number = FocusMode.CONTINUOUS_AUTO;

  @State capturedImage: PixelMap | undefined = undefined;

  private xComponentController: XComponentController = new XComponentController();

  aboutToAppear(): void {
    nativeCamera.registerImageDataCallback(this.processPhotoBufferWrapper);

    nativeCamera.subscribeState((state: string, message: string) => {
      hilog.info(DOMAIN, TAG, 'Camera state: %{public}s, message: %{public}s', state, message);
      this.cameraStatus = `${state}: ${message}`;
    });

    const result = nativeCamera.initCamera();
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
    hilog.info(DOMAIN, TAG, 'Preview changed: activeSlotId=%{public}s', activeSlotId);
    this.hasPreview = (activeSlotId === this.slotId && activeSlotId !== '');

    if (activeSlotId !== '') {
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

  processPhotoBufferWrapper = (buffer: ArrayBuffer) => {
    this.processPhotoBuffer(buffer);
  }

  async processPhotoBuffer(buffer: ArrayBuffer): Promise<void> {
    try {
      const imageSource = image.createImageSource(buffer);
      const pixelMap = await imageSource.createPixelMap();
      this.capturedImage = pixelMap;
      this.cameraStatus = '拍照完成';
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'Failed to process photo buffer: %{public}s', JSON.stringify(error));
    }
  }

  takePhoto(): void {
    if (!this.photoReady) {
      this.cameraStatus = '拍照未就绪';
      return;
    }
    const result = nativeCamera.takePhoto();
    if (result === 0) {
      this.cameraStatus = '正在拍照...';
    } else {
      this.cameraStatus = `拍照失败: ${result}`;
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
      .padding(12)
      .backgroundColor('#80000000')

      // 预览区域
      Stack() {
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

        // 状态显示
        Column() {
          Text(this.cameraStatus)
            .fontSize(14)
            .fontColor(Color.White)
            .backgroundColor('#80000000')
            .padding(8)
            .borderRadius(4)
        }
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.Start)
        .alignItems(HorizontalAlign.Center)
        .padding({ top: 16 })

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
          .height('100%')
          .justifyContent(FlexAlign.End)
          .alignItems(HorizontalAlign.End)
          .padding({ right: 16, bottom: 16 })
        }
      }
      .layoutWeight(1)
      .width('100%')
      .backgroundColor('#000000')

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
        .backgroundColor('#80000000')
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
            this.takePhoto();
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
