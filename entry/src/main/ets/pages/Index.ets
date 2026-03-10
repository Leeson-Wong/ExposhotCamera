import nativeCamera from 'libexpocamera.so';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x3200;
const TAG = 'Index';

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
struct Index {
  @State cameraStatus: string = 'Not initialized';
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

  private xComponentControllerA: XComponentController = new XComponentController();
  private xComponentControllerB: XComponentController = new XComponentController();

  aboutToAppear(): void {
    // 订阅相机状态
    nativeCamera.subscribeState((state: string, message: string) => {
      hilog.info(DOMAIN, TAG, 'Camera state: %{public}s, message: %{public}s', state, message);
      this.cameraStatus = `${state}: ${message}`;
    });

    // 初始化相机
    const result = nativeCamera.initCamera();
    hilog.info(DOMAIN, TAG, 'initCamera result: %{public}d', result);
    if (result === 0) {
      this.cameraStatus = 'Camera initialized';
    } else {
      this.cameraStatus = `Camera init failed: ${result}`;
    }
  }

  aboutToDisappear(): void {
    // 注销所有观察者
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

  // 观察者回调 - 所有观察者都会收到，自己判断是否获得预览流
  onPreviewChanged = (activeSlotId: string, activeSurfaceId: string): void => {
    hilog.info(DOMAIN, TAG, 'Preview changed: activeSlotId=%{public}s, activeSurfaceId=%{public}s',
      activeSlotId, activeSurfaceId);

    // 更新活跃 slot
    this.activeSlotId = activeSlotId;

    // 每个 slot 自己判断是否获得了预览流
    this.slotA.hasPreview = (activeSlotId === this.slotA.id && activeSlotId !== '');
    this.slotB.hasPreview = (activeSlotId === this.slotB.id && activeSlotId !== '');

    // 如果有活跃的 slot，更新相机参数
    if (activeSlotId !== '') {
      this.cameraStatus = `Preview on ${activeSlotId}`;
      this.photoReady = nativeCamera.isPhotoOutputReady();
      this.initCameraParams();
    }
  }

  // 注册观察者
  registerObserver(surfaceId: string): string {
    const slotId = nativeCamera.registerObserver(surfaceId, this.onPreviewChanged);
    hilog.info(DOMAIN, TAG, 'registerObserver: slotId=%{public}s', slotId);
    return slotId;
  }

  // 初始化相机参数
  initCameraParams(): void {
    // 检查缩放支持
    this.zoomSupported = nativeCamera.isZoomSupported();
    hilog.info(DOMAIN, TAG, 'Zoom supported: %{public}s', this.zoomSupported.toString());

    if (this.zoomSupported) {
      // 获取缩放范围
      const range = nativeCamera.getZoomRatioRange();
      this.zoomMin = range.min;
      this.zoomMax = range.max;
      hilog.info(DOMAIN, TAG, 'Zoom range: %{public}f - %{public}f', this.zoomMin, this.zoomMax);
    }

    // 获取当前缩放
    this.zoomRatio = nativeCamera.getZoomRatio();

    // 获取当前对焦模式
    this.focusMode = nativeCamera.getFocusMode();

    // 获取当前对焦点
    const focusPoint = nativeCamera.getFocusPoint();
    this.focusPointX = focusPoint.x;
    this.focusPointY = focusPoint.y;
    hilog.info(DOMAIN, TAG, 'Focus point: (%{public}f, %{public}f)', this.focusPointX, this.focusPointY);
  }

  // 切换到指定 Slot
  switchToSlot(slotId: string): void {
    const result = nativeCamera.switchToSlot(slotId);
    hilog.info(DOMAIN, TAG, 'switchToSlot %{public}s result: %{public}d', slotId, result);

    if (result === 0) {
      this.cameraStatus = `Switching to ${slotId}...`;
    } else {
      this.cameraStatus = `Switch failed: ${result}`;
    }
  }

  // 拍照
  takePhoto(): void {
    if (!this.photoReady) {
      hilog.warn(DOMAIN, TAG, 'Photo output not ready');
      this.cameraStatus = 'Photo not ready';
      return;
    }

    const result = nativeCamera.takePhoto((arrayBuffer: ArrayBuffer) => {
      hilog.info(DOMAIN, TAG, 'Photo callback, size: %{public}d', arrayBuffer.byteLength);
      this.cameraStatus = `Photo captured: ${arrayBuffer.byteLength} bytes`;
    });

    hilog.info(DOMAIN, TAG, 'takePhoto result: %{public}d', result);
    if (result === 0) {
      this.cameraStatus = 'Capturing...';
    } else {
      this.cameraStatus = `Capture failed: ${result}`;
    }
  }

  // 设置缩放
  setZoom(ratio: number): void {
    const result = nativeCamera.setZoomRatio(ratio);
    if (result === 0) {
      this.zoomRatio = ratio;
      hilog.info(DOMAIN, TAG, 'Zoom set to: %{public}f', ratio);
    } else {
      hilog.error(DOMAIN, TAG, 'Failed to set zoom: %{public}d', result);
    }
  }

  // 设置对焦模式
  setFocus(mode: number): void {
    const result = nativeCamera.setFocusMode(mode);
    if (result === 0) {
      this.focusMode = mode;
      hilog.info(DOMAIN, TAG, 'Focus mode set to: %{public}d', mode);
    } else {
      hilog.error(DOMAIN, TAG, 'Failed to set focus mode: %{public}d', result);
    }
  }

  // 设置对焦点（仅手动模式有效)
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
      // 状态栏
      Row() {
        Text(this.cameraStatus)
          .fontSize(14)
          .fontColor(Color.White)
          .layoutWeight(1)

        Text(`Active: ${this.activeSlotId}`)
          .fontSize(14)
          .fontColor(Color.Yellow)
          .margin({ left: 8 })
      }
      .width('100%')
      .backgroundColor('#80000000')
      .padding(12)

      // 双 Surface 预览区域 - 上下排列（田字格）
      Column() {
        // 上方 Slot A
        this.buildSlotA()

        // 下方 Slot B
        this.buildSlotB()
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

      // 手动对焦点提示（仅手动模式时显示）
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
      .padding(16)
      .justifyContent(FlexAlign.SpaceEvenly)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }

  // Slot A
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

          // 注册观察者，获取生成的 slotId
          this.slotA.surfaceId = surfaceId;
          this.slotA.id = this.registerObserver(surfaceId);
        })
        .width('100%')
        .height('100%')

      // 手动对焦模式下的触摸层
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

      // 显示对焦点指示器
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

      // Slot 状态指示
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

  // Slot B
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

          // 注册观察者，获取生成的 slotId
          this.slotB.surfaceId = surfaceId;
          this.slotB.id = this.registerObserver(surfaceId);
        })
        .width('100%')
        .height('100%')

      // 手动对焦模式下的触摸层
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

      // 显示对焦点指示器
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

      // Slot 状态指示
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
