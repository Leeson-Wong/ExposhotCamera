import nativeCamera, { BurstConfig, BurstProgress } from 'libexpocamera.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { image } from '@kit.ImageKit';
import { router } from '@kit.ArkUI';
import { SAFE_AREA_TOP, SAFE_AREA_BOTTOM } from '../util/WindowManager';

const DOMAIN = 0x3200;
const TAG = 'TestCaptureMode';

// 拍摄模式枚举（与 Native 层对应）
enum CaptureMode {
  SINGLE = 0,
  BURST = 1
}

@Entry
@Component
struct TestCaptureMode {
  @StorageProp(SAFE_AREA_TOP) safeTop: number = 0;
  @StorageProp(SAFE_AREA_BOTTOM) safeBottom: number = 0;
  @State cameraStatus: string = '未初始化';
  @State photoReady: boolean = false;
  @State slotId: string = '';
  @State hasPreview: boolean = false;

  // 模式切换相关
  @State currentMode: number = CaptureMode.SINGLE;
  @State canSwitch: boolean = false;
  @State isSwitching: boolean = false;

  // 连拍相关
  @State isCapturing: boolean = false;
  @State burstCount: number = 5;
  @State burstExposureMs: number = 2000;  // 曝光时间 2 秒
  @State capturedFrames: number = 0;
  @State processedFrames: number = 0;

  // 拍照结果
  @State capturedImage: PixelMap | undefined = undefined;

  private surfaceId: string = '';
  private xComponentController: XComponentController = new XComponentController();
  private isProcessingImage: boolean = false;

  aboutToAppear(): void {
    // 注册图像数据回调
    nativeCamera.registerImageDataCallback((data) => {
      hilog.info(DOMAIN, TAG, 'Image data received: %{public}dx%{public}d', data.width, data.height);
      this.cameraStatus = '拍照成功，正在处理...';
      this.processImageData(data);
    });

    // 订阅相机状态
    nativeCamera.subscribeState((state: string, message: string) => {
      hilog.info(DOMAIN, TAG, 'Camera state: %{public}s, message: %{public}s', state, message);
      this.cameraStatus = `${state}: ${message}`;
    });

    // 初始化相机
    const result = nativeCamera.initCamera();
    hilog.info(DOMAIN, TAG, 'initCamera result: %{public}d', result);
    if (result === 0) {
      this.cameraStatus = '相机初始化成功';
    } else {
      this.cameraStatus = `相机初始化失败: ${result}`;
    }
  }

  aboutToDisappear(): void {
    // 取消连拍
    if (this.isCapturing) {
      nativeCamera.cancelBurstCapture();
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

    if (activeSurfaceId == this.surfaceId) {
      this.slotId = activeSlotId;
      this.hasPreview = true;
      this.cameraStatus = '预览中';
      this.photoReady = nativeCamera.isPhotoOutputReady();
      this.updateModeStatus();
    }
  }

  registerObserver(surfaceId: string): string {
    const slotId = nativeCamera.registerObserver(surfaceId, this.onPreviewChanged);
    nativeCamera.startPreview(surfaceId);
    hilog.info(DOMAIN, TAG, 'registerObserver: slotId=%{public}s', slotId);
    return slotId;
  }

  updateModeStatus(): void {
    this.canSwitch = nativeCamera.canSwitchMode() && !this.isCapturing;
    this.currentMode = nativeCamera.getCaptureMode();
    hilog.info(DOMAIN, TAG, 'Mode status: canSwitch=%{public}s, currentMode=%{public}d',
      this.canSwitch.toString(), this.currentMode);
  }

  switchToMode(mode: number): void {
    if (this.isSwitching || !this.canSwitch || this.isCapturing) {
      hilog.warn(DOMAIN, TAG, 'Cannot switch: isSwitching=%{public}s, canSwitch=%{public}s, isCapturing=%{public}s',
        this.isSwitching.toString(), this.canSwitch.toString(), this.isCapturing.toString());
      return;
    }

    this.isSwitching = true;
    this.cameraStatus = `正在切换到 ${mode === CaptureMode.BURST ? '连拍' : '单拍'} 模式...`;

    const result = nativeCamera.switchCaptureMode(mode);
    hilog.info(DOMAIN, TAG, 'switchCaptureMode result: %{public}d', result);

    if (result === 0) {
      this.currentMode = mode;
      this.cameraStatus = `已切换到 ${mode === CaptureMode.BURST ? '连拍' : '单拍'} 模式`;
    } else {
      this.cameraStatus = `切换失败: 错误码 ${result}`;
    }

    this.isSwitching = false;
    this.updateModeStatus();
  }

  async processImageData(data: nativeCamera.ImageData): Promise<void> {
    if (!data.buffer) {
      hilog.warn(DOMAIN, TAG, 'No image buffer');
      return;
    }
    if (this.isProcessingImage) {
      return;
    }
    this.isProcessingImage = true;
    try {
      if (this.capturedImage) {
        this.capturedImage.release();
        this.capturedImage = undefined;
      }
      const imageSource = image.createImageSource(data.buffer);
      const pixelMap = await imageSource.createPixelMap();
      this.capturedImage = pixelMap;
      imageSource.release();
      if (data.isFinal) {
        this.cameraStatus = '拍摄完成';
        this.isCapturing = false;
        this.updateModeStatus();
      }
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'Failed to process image: %{public}s', JSON.stringify(error));
    } finally {
      this.isProcessingImage = false;
    }
  }

  // 单拍
  takeSinglePhoto(): void {
    if (!this.photoReady || this.isCapturing) {
      this.cameraStatus = '拍照未就绪';
      return;
    }
    const result = nativeCamera.takePhoto();
    if (result && result.sessionId) {
      this.cameraStatus = '正在拍照...';
    } else {
      this.cameraStatus = '拍照启动失败';
    }
  }

  // 连拍
  startBurstCapture(): void {
    if (!this.photoReady || this.isCapturing) {
      this.cameraStatus = '拍照未就绪';
      return;
    }

    this.isCapturing = true;
    this.capturedFrames = 0;
    this.processedFrames = 0;
    this.cameraStatus = '正在连拍...';
    this.updateModeStatus();

    const config: BurstConfig = {
      frameCount: this.burstCount,
      exposureMs: this.burstExposureMs,
      realtimePreview: true
    };

    hilog.info(DOMAIN, TAG, 'Starting burst capture: frameCount=%{public}d', this.burstCount);

    const result = nativeCamera.startBurstCapture(
      config,
      // 进度回调
      (progress: BurstProgress) => {
        this.capturedFrames = progress.capturedFrames;
        this.processedFrames = progress.processedFrames;
        this.cameraStatus = `连拍中: ${progress.capturedFrames}/${progress.totalFrames}`;

        hilog.info(DOMAIN, TAG, 'Burst progress: state=%{public}d, captured=%{public}d, processed=%{public}d',
          progress.state, progress.capturedFrames, progress.processedFrames);

        if (progress.state === nativeCamera.BurstState.COMPLETED ||
            progress.state === nativeCamera.BurstState.ERROR ||
            progress.state === nativeCamera.BurstState.CANCELLED) {
          this.isCapturing = false;
          this.updateModeStatus();
          if (progress.state === nativeCamera.BurstState.COMPLETED) {
            this.cameraStatus = `连拍完成: ${progress.capturedFrames} 张`;
          } else if (progress.state === nativeCamera.BurstState.ERROR) {
            this.cameraStatus = `连拍失败: ${progress.message}`;
          } else {
            this.cameraStatus = '连拍已取消';
          }
        }
      },
      // 图像回调
      (buffer: ArrayBuffer, isFinal: boolean) => {
        hilog.info(DOMAIN, TAG, 'Burst image: isFinal=%{public}s, size=%{public}d',
          isFinal.toString(), buffer.byteLength);
        if (isFinal) {
          this.processImageData({ buffer, width: 0, height: 0, isFinal: true } as nativeCamera.ImageData);
        }
      }
    );

    if (result.errorCode !== 0) {
      hilog.error(DOMAIN, TAG, 'Failed to start burst: %{public}d', result.errorCode);
      this.isCapturing = false;
      this.cameraStatus = `连拍启动失败: ${result.errorCode}`;
      this.updateModeStatus();
    }
  }

  // 根据模式拍照
  capture(): void {
    if (this.currentMode === CaptureMode.BURST) {
      this.startBurstCapture();
    } else {
      this.takeSinglePhoto();
    }
  }

  // 取消连拍
  cancelCapture(): void {
    if (this.isCapturing) {
      nativeCamera.cancelBurstCapture();
      this.isCapturing = false;
      this.cameraStatus = '已取消';
      this.updateModeStatus();
    }
  }

  getModeText(): string {
    return this.currentMode === CaptureMode.BURST ? '连拍模式' : '单拍模式';
  }

  getButtonText(): string {
    if (this.isCapturing) {
      return '取消';
    }
    return this.currentMode === CaptureMode.BURST ? `连拍 ${this.burstCount} 张` : '拍照';
  }

  build() {
    Stack() {
      // 底层 - 预览流
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

      // 顶层 - UI
      Column() {
        // 顶部状态栏
        Row() {
          Button('< 返回')
            .fontSize(14)
            .backgroundColor('#666666')
            .onClick(() => {
              router.back();
            })

          Text('模式切换测试')
            .fontSize(16)
            .fontColor(Color.White)
            .layoutWeight(1)
            .textAlign(TextAlign.Center)

          Text(this.getModeText())
            .fontSize(12)
            .fontColor(this.currentMode === CaptureMode.BURST ? Color.Yellow : Color.Green)
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

        // 连拍进度
        if (this.isCapturing && this.currentMode === CaptureMode.BURST) {
          Text(`${this.capturedFrames}/${this.burstCount} | 处理: ${this.processedFrames}`)
            .fontSize(12)
            .fontColor(Color.Yellow)
            .margin({ top: 4 })
        }

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
          // 模式切换状态
          Row() {
            Text('当前模式:')
              .fontSize(14)
              .fontColor(Color.White)

            Text(this.getModeText())
              .fontSize(14)
              .fontColor(this.currentMode === CaptureMode.BURST ? Color.Yellow : Color.Green)
              .margin({ left: 8 })

            Text(` | 可切换: ${this.canSwitch ? '是' : '否'}`)
              .fontSize(12)
              .fontColor(Color.Gray)
              .margin({ left: 8 })
          }
          .width('100%')
          .padding({ left: 16, right: 16, top: 8, bottom: 4 })

          // 连拍模式提示
          if (this.currentMode === CaptureMode.BURST) {
            Column() {
              Row() {
                Text('💡 连拍模式: 中等分辨率')
                  .fontSize(12)
                  .fontColor('#FFD700')
              }
              Row() {
                Text(`连拍 ${this.burstCount} 张 | 曝光 ${this.burstExposureMs / 1000}s`)
                  .fontSize(11)
                  .fontColor('#CCCCCC')
              }
            }
            .width('100%')
            .padding({ left: 16, right: 16, top: 2, bottom: 4 })
            .alignItems(HorizontalAlign.Start)
          } else {
            Row() {
              Text('📷 单拍模式: 最高分辨率')
                .fontSize(12)
                .fontColor('#90EE90')
            }
            .width('100%')
            .padding({ left: 16, right: 16, top: 2, bottom: 4 })
          }

          // 模式切换按钮
          Row() {
            Button('单拍模式')
              .fontSize(14)
              .width('45%')
              .height(40)
              .backgroundColor(this.currentMode === CaptureMode.SINGLE ? '#007DFF' : '#666666')
              .enabled(this.canSwitch && this.currentMode !== CaptureMode.SINGLE && !this.isSwitching)
              .opacity(this.canSwitch && this.currentMode !== CaptureMode.SINGLE ? 1.0 : 0.5)
              .onClick(() => this.switchToMode(CaptureMode.SINGLE))

            Button('连拍模式')
              .fontSize(14)
              .width('45%')
              .height(40)
              .backgroundColor(this.currentMode === CaptureMode.BURST ? '#FF9500' : '#666666')
              .enabled(this.canSwitch && this.currentMode !== CaptureMode.BURST && !this.isSwitching)
              .opacity(this.canSwitch && this.currentMode !== CaptureMode.BURST ? 1.0 : 0.5)
              .onClick(() => this.switchToMode(CaptureMode.BURST))
          }
          .width('100%')
          .padding({ left: 16, right: 16, top: 4, bottom: 8 })
          .justifyContent(FlexAlign.SpaceEvenly)

          // 拍照/取消按钮
          Row() {
            Button(this.getButtonText())
              .fontSize(18)
              .width('80%')
              .height(50)
              .enabled(this.photoReady && !this.isSwitching)
              .backgroundColor(this.isCapturing ? '#FF0000' : (this.photoReady ? '#007DFF' : '#666666'))
              .onClick(() => {
                if (this.isCapturing) {
                  this.cancelCapture();
                } else {
                  this.capture();
                }
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
