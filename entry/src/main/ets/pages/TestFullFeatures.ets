import nativeCamera from 'libexpocamera.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { image } from '@kit.ImageKit';
import { router } from '@kit.ArkUI';
import { SAFE_AREA_TOP, SAFE_AREA_BOTTOM } from '../util/WindowManager';

const DOMAIN = 0x3200;
const TAG = 'TestSlotSwitch';

// 对焦模式枚举（与 C++ 侧保持一致）
enum FocusMode {
  MANUAL = 0,
  CONTINUOUS_AUTO = 1,
  AUTO = 2,
  LOCKED = 3,
};

interface FocusParam {
  mode: FocusMode
  label: string
}

// Slot 信息
interface SlotInfo {
  id: string;
  surfaceId: string;
  hasPreview: boolean;
}

@Entry
@Component
struct TestFullFeatures {
  @StorageProp(SAFE_AREA_TOP) safeTop: number = 0;
  @StorageProp(SAFE_AREA_BOTTOM) safeBottom: number = 0;
  @State cameraStatus: string = '未初始化';
  @State activeSlotId: string = '';
  @State photoReady: boolean = false;

  // Slot 信息
  @State slotA: SlotInfo = { id: '', surfaceId: '', hasPreview: false };
  @State slotB: SlotInfo = { id: '', surfaceId: '', hasPreview: false };

  // 缩放控制
  @State zoomRatio: number = 1.0;
  @State zoomMin: number = 1.0;
  @State zoomMax: number = 1.0;
  @State zoomSupported: boolean = false;

  // 对焦控制
  @State focusMode: number = FocusMode.CONTINUOUS_AUTO;
  @State focusPointX: number = 0.5;
  @State focusPointY: number = 0.5;

  @State capturedImage: PixelMap | undefined = undefined;

  // 防抖：防止并发处理图像
  private isProcessingImage: boolean = false;
  // 持有当前正在处理的 buffer，防止 await 期间 GC 回收 ArrayBuffer
  private processingBuffer: ArrayBuffer | null = null;
  private xComponentControllerA: XComponentController = new XComponentController();
  private xComponentControllerB: XComponentController = new XComponentController();

  aboutToAppear(): void {
    nativeCamera.registerImageDataCallback(this.processPhotoBufferWrapper);

    nativeCamera.subscribeState((state: string, message: string) => {
      hilog.info(DOMAIN, TAG, 'Camera state: %{public}s, message: %{public}s', state, message);
      this.cameraStatus = `${state}: ${message}`;
    });

    const result = nativeCamera.initCamera(nativeCamera.CaptureMode.SINGLE);
    hilog.info(DOMAIN, TAG, 'initCamera result: %{public}d', result);
    if (result === 0) {
      this.cameraStatus = '相机初始化成功';
    } else {
      this.cameraStatus = `相机初始化失败: ${result}`;
    }
  }

  aboutToDisappear(): void {
    if (this.slotA.id) {
      nativeCamera.unregisterObserver(this.slotA.id);
    }
    if (this.slotB.id) {
      nativeCamera.unregisterObserver(this.slotB.id);
    }

    nativeCamera.unsubscribeState();
    nativeCamera.stopPreview();
    nativeCamera.releaseCamera();
  }

  onPreviewChanged = (activeSlotId: string, activeSurfaceId: string): void => {
    hilog.info(DOMAIN, TAG, 'Preview changed: activeSlotId=%{public}s, activeSurfaceId=%{public}s',
      activeSlotId, activeSurfaceId);

    this.activeSlotId = activeSlotId;

    this.slotA.hasPreview = (activeSlotId === this.slotA.id && activeSlotId !== '');
    this.slotB.hasPreview = (activeSlotId === this.slotB.id && activeSlotId !== '');

    if (activeSlotId !== '') {
      this.cameraStatus = `预览: ${activeSlotId}`;
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
    hilog.info(DOMAIN, TAG, 'Zoom supported: %{public}s', this.zoomSupported.toString());

    if (this.zoomSupported) {
      const range = nativeCamera.getZoomRatioRange();
      this.zoomMin = range.min;
      this.zoomMax = range.max;
      hilog.info(DOMAIN, TAG, 'Zoom range: %{public}f - %{public}f', this.zoomMin, this.zoomMax);
    }

    this.zoomRatio = nativeCamera.getZoomRatio();
    this.focusMode = nativeCamera.getFocusMode();

    const focusPoint = nativeCamera.getFocusPoint();
    this.focusPointX = focusPoint.x;
    this.focusPointY = focusPoint.y;
    hilog.info(DOMAIN, TAG, 'Focus point: (%{public}f, %{public}f)', this.focusPointX, this.focusPointY);
  }

  switchToSlot(slotId: string): void {
    const result = nativeCamera.switchToSlot(slotId);
    hilog.info(DOMAIN, TAG, 'switchToSlot %{public}s result: %{public}d', slotId, result);

    if (result === 0) {
      this.cameraStatus = `切换到 ${slotId}...`;
    } else {
      this.cameraStatus = `切换失败: ${result}`;
    }
  }

  processPhotoBufferWrapper = (data: nativeCamera.ImageData) => {
    if (!data.buffer) {
      // LOG 未拿到图像数据
      return
    }
    this.processPhotoBuffer(data.buffer);
  }

  async processPhotoBuffer(buffer: ArrayBuffer): Promise<void> {
    // 防抖：如果正在处理，跳过本次
    if (this.isProcessingImage) {
      hilog.warn(DOMAIN, TAG, 'Image processing in progress, skipping');
      return;
    }
    this.isProcessingImage = true;
    // 用实例变量持有 buffer，防止 await 期间 GC 回收 ArrayBuffer
    this.processingBuffer = buffer;
    try {
      // 先释放旧的 PixelMap
      if (this.capturedImage) {
        await this.capturedImage.release();
        this.capturedImage = undefined;
      }

      // slice(0) 创建独立副本，解耦 Native 层与 JS GC
      const ownedBuffer = this.processingBuffer!.slice(0);
      const imageSource = image.createImageSource(ownedBuffer);
      const pixelMap = await imageSource.createPixelMap();
      this.capturedImage = pixelMap;
      // await release 确保安全释放
      await imageSource.release();
    } catch (error) {
      console.error('Failed to process photo buffer:', error);
    } finally {
      this.processingBuffer = null;
      this.isProcessingImage = false;
    }
  }

  takePhoto(): void {
    if (!this.photoReady) {
      hilog.warn(DOMAIN, TAG, 'Photo output not ready');
      this.cameraStatus = '拍照未就绪';
      return;
    }

    const result = nativeCamera.takePhoto();

    hilog.info(DOMAIN, TAG, 'takePhoto result: %{public}d', result);
    if (!result) {
      this.cameraStatus = '正在拍照...';
    } else {
      this.cameraStatus = `拍照失败: ${'errorCode' + result.errorCode + '\tsessionId' + result.sessionId}`;
    }
  }

  setZoom(ratio: number): void {
    const result = nativeCamera.setZoomRatio(ratio);
    if (result === 0) {
      this.zoomRatio = ratio;
      hilog.info(DOMAIN, TAG, 'Zoom set to: %{public}f', ratio);
    } else {
      hilog.error(DOMAIN, TAG, 'Failed to set zoom: %{public}d', result);
    }
  }

  setFocus(mode: number): void {
    const result = nativeCamera.setFocusMode(mode);
    if (result === 0) {
      this.focusMode = mode;
      hilog.info(DOMAIN, TAG, 'Focus mode set to: %{public}d', mode);
    } else {
      hilog.error(DOMAIN, TAG, 'Failed to set focus mode: %{public}d', result);
    }
  }

  setFocusPoint(x: number, y: number): void {
    if (this.focusMode !== FocusMode.MANUAL) {
      hilog.warn(DOMAIN, TAG, 'Focus point can only be set in manual mode');
      return;
    }
    const result = nativeCamera.setFocusPoint(x, y);
    if (result === 0) {
      this.focusPointX = x;
      this.focusPointY = y;
      hilog.info(DOMAIN, TAG, 'Focus point set to: (%{public}f, %{public}f)', x, y);
    } else {
      hilog.error(DOMAIN, TAG, 'Failed to set focus point: %{public}d', result);
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

        Text(this.cameraStatus)
          .fontSize(12)
          .fontColor(Color.White)
          .layoutWeight(1)
          .textAlign(TextAlign.Center)
          .margin({ left: 8, right: 8 })

        Text(`活跃: ${this.activeSlotId}`)
          .fontSize(12)
          .fontColor(Color.Yellow)
      }
      .width('100%')
      .backgroundColor('#80000000')
      .padding({ top: this.safeTop + 8, left: 12, right: 12, bottom: 8 })

      // 双 Surface 预览区域
      Column() {
        Stack() {
          Column() {
            this.buildSlotA()
            this.buildSlotB()
          }.width('100%').height('100%')

          Column() {
            Row() {
              Image(this.capturedImage)
                .width(80)
                .height(100)
                .border({ width: 1, color: 'red' })
            }
            .width('100%')
            .justifyContent(FlexAlign.End)
          }
          .width('100%')
          .height('100%')
          .justifyContent(FlexAlign.End)
        }
        .width('100%')
        .height('100%')
      }
      .layoutWeight(1)
      .width('100%')

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

        Row() {
          ForEach([
            { mode: FocusMode.CONTINUOUS_AUTO, label: '连续' } as FocusParam,
            { mode: FocusMode.AUTO, label: '自动' } as FocusParam,
            { mode: FocusMode.MANUAL, label: '手动' } as FocusParam,
          ], (item: FocusParam) => {
            Button(item.label)
              .fontSize(12)
              .height(28)
              .backgroundColor(this.focusMode === item.mode ? '#007DFF' : '#666666')
              .margin({ left: 4, right: 4 })
              .onClick(() => {
                this.setFocus(item.mode);
              })
          })
        }
        .layoutWeight(1)
        .justifyContent(FlexAlign.Start)
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 8, bottom: 8 })
      .backgroundColor('#80000000')

      // 手动对焦点提示
      if (this.focusMode === FocusMode.MANUAL) {
        Row() {
          Text('点击预览区域设置对焦点')
            .fontSize(12)
            .fontColor(Color.Yellow)
            .layoutWeight(1)

          Text(`对焦点: (${(this.focusPointX * 100).toFixed(0)}%, ${(this.focusPointY * 100).toFixed(0)}%)`)
            .fontSize(12)
            .fontColor(Color.White)
        }
        .width('100%')
        .padding({ left: 16, right: 16, top: 8, bottom: 8 })
        .backgroundColor('#80000000')
      }

      // 控制按钮
      Row() {
        Button('切换到 A')
          .fontSize(14)
          .width('30%')
          .backgroundColor(this.activeSlotId === this.slotA.id ? '#007DFF' : '#666666')
          .onClick(() => {
            if (this.slotA.id) {
              this.switchToSlot(this.slotA.id);
            }
          })

        Button('切换到 B')
          .fontSize(14)
          .width('30%')
          .backgroundColor(this.activeSlotId === this.slotB.id ? '#007DFF' : '#666666')
          .onClick(() => {
            if (this.slotB.id) {
              this.switchToSlot(this.slotB.id);
            }
          })

        Button('拍照')
          .fontSize(14)
          .width('30%')
          .enabled(this.photoReady)
          .backgroundColor(this.photoReady ? '#007DFF' : '#666666')
          .onClick(() => {
            this.takePhoto();
          })
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 8, bottom: this.safeBottom + 16 })
      .justifyContent(FlexAlign.SpaceEvenly)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }

  @Builder
  buildSlotA() {
    Stack({ alignContent: Alignment.Center }) {
      XComponent({
        id: 'cameraPreviewA',
        type: 'surface',
        libraryname: 'expocamera',
        controller: this.xComponentControllerA
      })
        .onLoad(() => {
          hilog.info(DOMAIN, TAG, 'XComponent A onLoad');
          const surfaceId = this.xComponentControllerA.getXComponentSurfaceId();
          hilog.info(DOMAIN, TAG, 'Slot A surfaceId: %{public}s', surfaceId);

          this.slotA.surfaceId = surfaceId;
          this.slotA.id = this.registerObserver(surfaceId);
        })
        .width('100%')
        .height('100%')

      if (this.focusMode === FocusMode.MANUAL && this.slotA.hasPreview) {
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('transparent')
          .onClick((event: ClickEvent) => {
            const targetArea = event.target.area;
            const x = event.x / (Number(targetArea.width.toString() || '1'));
            const y = event.y / (Number(targetArea.height.toString() || '1'));
            hilog.info(DOMAIN, TAG, 'Touch at A: (%{public}f, %{public}f)', x, y);
            this.setFocusPoint(x, y);
          })
      }

      if (this.focusMode === FocusMode.MANUAL && this.slotA.hasPreview) {
        Column() {
          Row() {
            Text('+')
              .fontSize(24)
              .fontColor(Color.Yellow)
              .opacity(0.8)
          }
        }
        .position({ x: `${this.focusPointX * 100}%`, y: `${this.focusPointY * 100}%` })
        .markAnchor({ x: '50%', y: '50%' })
      }

      Text(this.slotA.hasPreview ? '● A' : '○ A')
        .fontSize(14)
        .fontColor(this.slotA.hasPreview ? Color.Green : Color.Gray)
        .position({ x: 8, y: 8 })
    }
    .layoutWeight(1)
    .width('100%')
    .border(this.slotA.hasPreview ? { width: 3, color: Color.Green } : { width: 1, color: Color.Gray })
    .clip(true)
  }

  @Builder
  buildSlotB() {
    Stack({ alignContent: Alignment.Center }) {
      XComponent({
        id: 'cameraPreviewB',
        type: 'surface',
        libraryname: 'expocamera',
        controller: this.xComponentControllerB
      })
        .onLoad(() => {
          hilog.info(DOMAIN, TAG, 'XComponent B onLoad');
          const surfaceId = this.xComponentControllerB.getXComponentSurfaceId();
          hilog.info(DOMAIN, TAG, 'Slot B surfaceId: %{public}s', surfaceId);

          this.slotB.surfaceId = surfaceId;
          this.slotB.id = this.registerObserver(surfaceId);
        })
        .width('100%')
        .height('100%')

      if (this.focusMode === FocusMode.MANUAL && this.slotB.hasPreview) {
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('transparent')
          .onClick((event: ClickEvent) => {
            const targetArea = event.target.area;
            const x = event.x / (Number(targetArea.width.toString() || '1'));
            const y = event.y / (Number(targetArea.height.toString() || '1'));
            hilog.info(DOMAIN, TAG, 'Touch at B: (%{public}f, %{public}f)', x, y);
            this.setFocusPoint(x, y);
          })
      }

      if (this.focusMode === FocusMode.MANUAL && this.slotB.hasPreview) {
        Column() {
          Row() {
            Text('+')
              .fontSize(24)
              .fontColor(Color.Yellow)
              .opacity(0.8)
          }
        }
        .position({ x: `${this.focusPointX * 100}%`, y: `${this.focusPointY * 100}%` })
        .markAnchor({ x: '50%', y: '50%' })
      }

      Text(this.slotB.hasPreview ? '● B' : '○ B')
        .fontSize(14)
        .fontColor(this.slotB.hasPreview ? Color.Green : Color.Gray)
        .position({ x: 8, y: 8 })
    }
    .layoutWeight(1)
    .width('100%')
    .border(this.slotB.hasPreview ? { width: 3, color: Color.Green } : { width: 1, color: Color.Gray })
    .clip(true)
  }
}
