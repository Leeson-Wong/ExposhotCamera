import nativeCamera from 'libexpocamera.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { image } from '@kit.ImageKit';
import { router } from '@kit.ArkUI';

const DOMAIN = 0x3200;
const TAG = 'TestFocusPoint';

// 对焦模式枚举
enum FocusMode {
  MANUAL = 0,
  CONTINUOUS_AUTO = 1,
  AUTO = 2,
  LOCKED = 3,
}

// 对焦点记录
interface FocusPointRecord {
  x: number;
  y: number;
  timestamp: number;
}

@Entry
@Component
struct TestFocusPoint {
  @State cameraStatus: string = '未初始化';
  @State photoReady: boolean = false;
  @State slotId: string = '';
  @State hasPreview: boolean = false;
  @State capturedImage: PixelMap | undefined = undefined;

  // 对焦控制
  @State focusMode: number = FocusMode.MANUAL;
  @State focusPointX: number = 0.5;
  @State focusPointY: number = 0.5;
  @State focusPointRecords: FocusPointRecord[] = [];

  // 对焦距离（真正的手动对焦）
  @State focusDistance: number = 0.0;
  @State focusDistanceMin: number = 0.0;
  @State focusDistanceMax: number = 10.0;

  // 提示显示控制
  @State showFocusHint: boolean = true;

  private xComponentController: XComponentController = new XComponentController();

  aboutToAppear(): void {
    nativeCamera.registerImageDataCallback((data) => {
      hilog.info(DOMAIN, TAG, 'Image data received: sessionId=%{public}s', data.sessionId || '');
      this.cameraStatus = '拍照成功';
      this.processImageData(data);
    });

    nativeCamera.subscribeState((state: string, message: string) => {
      hilog.info(DOMAIN, TAG, 'Camera state: %{public}s', state);
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
    hilog.info(DOMAIN, TAG, 'Preview changed: activeSlotId=%{public}s, this.slotId=%{public}s, activeSurfaceId=%{public}s',
              activeSlotId, this.slotId, activeSurfaceId);

    // 如果 this.slotId 还没设置（registerObserver 还没返回），但 activeSlotId 不为空
    // 说明这是在 registerObserver 内部触发的回调，此时应该设置为有预览
    // 因为当前正在注册的 observer 就是即将成为活跃的 observer
    if (this.slotId === '' && activeSlotId !== '') {
      hilog.info(DOMAIN, TAG, 'this.slotId not set yet, but activeSlotId is not empty, assuming will match');
      // 延迟检查，等待 this.slotId 被设置
      setTimeout(() => {
        if (this.slotId === activeSlotId) {
          this.hasPreview = true;
          this.cameraStatus = '预览中';
          this.photoReady = nativeCamera.isPhotoOutputReady();
          this.initCameraParams();
          hilog.info(DOMAIN, TAG, 'Delayed check: hasPreview set to true');
        }
      }, 50);
      return;
    }

    const shouldHavePreview = (activeSlotId === this.slotId && activeSlotId !== '');
    hilog.info(DOMAIN, TAG, 'hasPreview check: activeSlotId === this.slotId = %{public}s, activeSlotId !== \'\' = %{public}s, result = %{public}s',
              (activeSlotId === this.slotId).toString(), (activeSlotId !== '').toString(), shouldHavePreview.toString());
    this.hasPreview = shouldHavePreview;
    hilog.info(DOMAIN, TAG, 'hasPreview set to: %{public}s', this.hasPreview.toString());

    if (activeSlotId !== '') {
      this.cameraStatus = '预览中';
      this.photoReady = nativeCamera.isPhotoOutputReady();
      this.initCameraParams();
    }
  }

  registerObserver(surfaceId: string): string {
    hilog.info(DOMAIN, TAG, 'registerObserver called with surfaceId=%{public}s', surfaceId);
    const slotId = nativeCamera.registerObserver(surfaceId, this.onPreviewChanged);
    hilog.info(DOMAIN, TAG, 'registerObserver returned slotId=%{public}s', slotId);
    this.slotId = slotId;
    hilog.info(DOMAIN, TAG, 'this.slotId set to=%{public}s', this.slotId);
    nativeCamera.startPreview(surfaceId);
    hilog.info(DOMAIN, TAG, 'startPreview called with surfaceId=%{public}s', surfaceId);
    return slotId;
  }

  initCameraParams(): void {
    this.focusMode = nativeCamera.getFocusMode();
    const focusPoint = nativeCamera.getFocusPoint();
    this.focusPointX = focusPoint.x;
    this.focusPointY = focusPoint.y;

    const range = nativeCamera.getFocusDistanceRange();
    this.focusDistanceMin = range.min;
    this.focusDistanceMax = range.max;
    this.focusDistance = nativeCamera.getFocusDistance();

    hilog.info(DOMAIN, TAG, 'Focus point: (%{public}f, %{public}f), distance: %{public}f',
              this.focusPointX, this.focusPointY, this.focusDistance);
  }

  async processImageData(data: nativeCamera.ImageData): Promise<void> {
    if (!data.buffer) {
      hilog.warn(DOMAIN, TAG, 'No image buffer in data');
      return;
    }
    try {
      const imageSource = image.createImageSource(data.buffer);
      const pixelMap = await imageSource.createPixelMap();
      this.capturedImage = pixelMap;
      hilog.info(DOMAIN, TAG, 'Image processed: %{public}dx%{public}d', data.width, data.height);
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'Failed to process image: %{public}s', JSON.stringify(error));
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
      this.cameraStatus = '拍照失败';
      hilog.error(DOMAIN, TAG, 'takePhoto failed');
    }
  }

  setFocusMode(mode: number): void {
    const result = nativeCamera.setFocusMode(mode);
    if (result === 0) {
      this.focusMode = mode;
      this.cameraStatus = `对焦模式: ${mode === FocusMode.MANUAL ? '手动' : mode === FocusMode.AUTO ? '自动' : '连续自动'}`;
    } else {
      this.cameraStatus = `设置对焦模式失败: ${result}`;
    }
  }

  setFocusPoint(x: number, y: number): void {
    hilog.info(DOMAIN, TAG, 'setFocusPoint called: (%{public}f, %{public}f), current mode: %{public}d',
              x, y, this.focusMode);

    // 保存当前对焦模式（如果不是 MANUAL 或 AUTO）
    const previousMode = this.focusMode;

    // 切换到 AUTO 模式以支持对焦点设置
    if (this.focusMode !== FocusMode.MANUAL && this.focusMode !== FocusMode.AUTO) {
      this.setFocusMode(FocusMode.AUTO);
    }

    // 设置对焦点
    const result = nativeCamera.setFocusPoint(x, y);
    hilog.info(DOMAIN, TAG, 'setFocusPoint result: %{public}d', result);

    if (result === 0) {
      this.focusPointX = x;
      this.focusPointY = y;

      // 记录对焦点
      this.focusPointRecords.push({ x, y, timestamp: Date.now() });
      if (this.focusPointRecords.length > 10) {
        this.focusPointRecords.shift();
      }

      const modeName = this.getFocusModeName(this.focusMode);
      this.cameraStatus = `${modeName}对焦点: (${(x * 100).toFixed(0)}%, ${(y * 100).toFixed(0)}%)`;

      // 如果之前是连续自动模式，在短暂延迟后切回
      if (previousMode === FocusMode.CONTINUOUS_AUTO) {
        setTimeout(() => {
          this.setFocusMode(FocusMode.CONTINUOUS_AUTO);
          hilog.info(DOMAIN, TAG, 'Switched back to CONTINUOUS_AUTO after focus point set');
        }, 500);
      }
    } else {
      this.cameraStatus = `设置对焦点失败: ${result}`;
      hilog.error(DOMAIN, TAG, 'setFocusPoint failed: %{public}d', result);
    }
  }

  setFocusDistance(distance: number): void {
    const result = nativeCamera.setFocusDistance(distance);
    if (result === 0) {
      this.focusDistance = distance;
      this.cameraStatus = `对焦距离: ${distance.toFixed(2)}m`;
    } else {
      this.cameraStatus = `设置对焦距离失败: ${result}`;
    }
  }

  getFocusModeName(mode: number): string {
    switch (mode) {
      case FocusMode.MANUAL: return '手动';
      case FocusMode.AUTO: return '自动';
      case FocusMode.CONTINUOUS_AUTO: return '连续自动';
      case FocusMode.LOCKED: return '锁定';
      default: return '未知';
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

        Text('对焦点测试')
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

        // 透明点击层 - 始终渲染以处理点击事件
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('transparent')
          .onClick((event: ClickEvent) => {
            if (!this.hasPreview) {
              this.cameraStatus = '请等待预览启动';
              hilog.warn(DOMAIN, TAG, 'Preview not ready, ignoring click');
              return;
            }
            // 隐藏提示
            this.showFocusHint = false;
            const targetArea = event.target.area;
            const x = event.x / (Number(targetArea.width.toString() || '1'));
            const y = event.y / (Number(targetArea.height.toString() || '1'));
            hilog.info(DOMAIN, TAG, 'Touch at: (%{public}f, %{public}f)', x, y);
            this.setFocusPoint(x, y);
          })

        // 对焦点标记
        if (this.hasPreview) {
          Column() {
            Row() {
              Text('+')
                .fontSize(32)
                .fontColor(Color.Yellow)
                .fontWeight(FontWeight.Bold)
            }
            .width(40)
            .height(40)
            .justifyContent(FlexAlign.Center)
          }
          .position({ x: `${this.focusPointX * 100}%`, y: `${this.focusPointY * 100}%` })
          .markAnchor({ x: '50%', y: '50%' })
        }

        // 对焦点轨迹
        if (this.focusPointRecords.length > 1) {
          ForEach(this.focusPointRecords.slice(1), (record: FocusPointRecord, index: number) => {
            Circle()
              .width(8)
              .height(8)
              .fill(Color.Transparent)
              .stroke(Color.Yellow)
              .strokeWidth(2)
              .opacity(0.5 - (index * 0.05))
              .position({ x: `${record.x * 100}%`, y: `${record.y * 100}%` })
              .markAnchor({ x: '50%', y: '50%' })
          })
        }

        // 拍照结果缩略图
        if (this.capturedImage) {
          Column() {
            Image(this.capturedImage)
              .width(60)
              .height(80)
              .border({ width: 2, color: Color.Green })
              .borderRadius(4)
          }
          .width('100%')
          .height('100%')
          .justifyContent(FlexAlign.End)
          .alignItems(HorizontalAlign.End)
          .padding({ right: 16, bottom: 16 })
        }

        // 提示文字
        if (this.hasPreview && this.showFocusHint) {
          Text('点击画面设置对焦点')
            .fontSize(14)
            .fontColor(Color.Yellow)
            .backgroundColor('#80000000')
            .padding({ left: 12, right: 12, top: 6, bottom: 6 })
            .borderRadius(4)
        }
      }
      .layoutWeight(1)
      .width('100%')
      .backgroundColor('#000000')

      // 当前对焦点信息
      Row() {
        Text(`对焦点: (${(this.focusPointX * 100).toFixed(1)}%, ${(this.focusPointY * 100).toFixed(1)}%)`)
          .fontSize(12)
          .fontColor(Color.White)
          .layoutWeight(1)

        Text(`对焦距离: ${this.focusDistance.toFixed(2)}m`)
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

        Button('手动')
          .fontSize(12)
          .height(32)
          .backgroundColor(this.focusMode === FocusMode.MANUAL ? '#007DFF' : '#666666')
          .margin({ left: 4, right: 4 })
          .onClick(() => this.setFocusMode(FocusMode.MANUAL))

        Button('自动')
          .fontSize(12)
          .height(32)
          .backgroundColor(this.focusMode === FocusMode.AUTO ? '#007DFF' : '#666666')
          .margin({ left: 4, right: 4 })
          .onClick(() => this.setFocusMode(FocusMode.AUTO))

        Button('连续')
          .fontSize(12)
          .height(32)
          .backgroundColor(this.focusMode === FocusMode.CONTINUOUS_AUTO ? '#007DFF' : '#666666')
          .margin({ left: 4, right: 4 })
          .onClick(() => this.setFocusMode(FocusMode.CONTINUOUS_AUTO))
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 8, bottom: 8 })
      .backgroundColor('#80000000')

      // 对焦距离滑块（真正的手动对焦）
      if (this.focusMode === FocusMode.MANUAL) {
        Row() {
          Text('对焦距离:')
            .fontSize(12)
            .fontColor(Color.White)
            .width(60)

          Slider({
            value: this.focusDistance,
            min: this.focusDistanceMin,
            max: this.focusDistanceMax,
            step: 0.1,
            style: SliderStyle.OutSet
          })
            .blockColor(Color.White)
            .trackColor('#666666')
            .selectedColor('#007DFF')
            .layoutWeight(1)
            .onChange((value: number) => {
              this.setFocusDistance(value);
            })

          Text(`${this.focusDistance.toFixed(1)}m`)
            .fontSize(12)
            .fontColor(Color.White)
            .width(45)
            .textAlign(TextAlign.End)
        }
        .width('100%')
        .padding({ left: 16, right: 16, top: 8, bottom: 8 })
        .backgroundColor('#80000000')
      }

      // 预设对焦点按钮
      Row() {
        Button('中心')
          .fontSize(12)
          .height(32)
          .backgroundColor('#444444')
          .margin({ left: 4, right: 4 })
          .onClick(() => this.setFocusPoint(0.5, 0.5))

        Button('左上')
          .fontSize(12)
          .height(32)
          .backgroundColor('#444444')
          .margin({ left: 4, right: 4 })
          .onClick(() => this.setFocusPoint(0.25, 0.25))

        Button('右上')
          .fontSize(12)
          .height(32)
          .backgroundColor('#444444')
          .margin({ left: 4, right: 4 })
          .onClick(() => this.setFocusPoint(0.75, 0.25))

        Button('底部')
          .fontSize(12)
          .height(32)
          .backgroundColor('#444444')
          .margin({ left: 4, right: 4 })
          .onClick(() => this.setFocusPoint(0.5, 0.75))

        Button('清除轨迹')
          .fontSize(12)
          .height(32)
          .backgroundColor('#444444')
          .margin({ left: 4, right: 4 })
          .onClick(() => {
            this.focusPointRecords = [];
            this.cameraStatus = '对焦点轨迹已清除';
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
