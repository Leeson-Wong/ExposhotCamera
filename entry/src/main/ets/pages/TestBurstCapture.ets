import nativeCamera, { BurstConfig, BurstProgress } from 'libexpocamera.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { image } from '@kit.ImageKit';
import { router } from '@kit.ArkUI';

const DOMAIN = 0x3200;
const TAG = 'TestBurstCapture';

@Entry
@Component
struct TestBurstCapture {
  @State cameraStatus: string = '未初始化';
  @State photoReady: boolean = false;
  @State slotId: string = '';
  @State hasPreview: boolean = false;

  // 连拍控制
  @State burstCount: number = 5;
  @State burstExposure: number = 10000; // ms
  @State isCapturing: boolean = false;
  @State capturedCount: number = 0;
  @State processedCount: number = 0;
  @State progressMessage: string = '';

  // 缩略图列表（用于存储中间预览图）
  @State thumbnailList: PixelMap[] = [];
  // 最终堆叠结果
  @State finalImage?: PixelMap = undefined;

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
    }
  }

  registerObserver(surfaceId: string): string {
    const slotId = nativeCamera.registerObserver(surfaceId, this.onPreviewChanged);
    nativeCamera.startPreview(surfaceId);
    hilog.info(DOMAIN, TAG, 'registerObserver: slotId=%{public}s', slotId);
    return slotId;
  }

  processPhotoBufferWrapper = (data: nativeCamera.ImageData) => {
    // 单次拍照模式（非连拍）
    if (!data.buffer) {
      return;
    }
    this.processPhotoBuffer(data.buffer, data.isFinal);
  }

  async processPhotoBuffer(buffer: ArrayBuffer, isFinal: boolean): Promise<void> {
    try {
      const imageSource = image.createImageSource(buffer);
      const pixelMap = await imageSource.createPixelMap();

      if (isFinal) {
        // 最终堆叠结果
        this.finalImage = pixelMap;
        this.isCapturing = false;
        this.cameraStatus = `连拍完成: 拍摄 ${this.capturedCount} 张，处理 ${this.processedCount} 张`;
        hilog.info(DOMAIN, TAG, 'Final stacked image received');
      } else {
        // 中间预览图
        this.thumbnailList.push(pixelMap);
        hilog.info(DOMAIN, TAG, 'Preview image received, total: %{public}d', this.thumbnailList.length);
      }
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'Failed to process photo buffer: %{public}s', JSON.stringify(error));
    }
  }

  // NDK 连拍堆叠
  startBurstCapture(): void {
    if (!this.photoReady || this.isCapturing) {
      hilog.warn(DOMAIN, TAG, 'Cannot start burst: photoReady=%{public}s, isCapturing=%{public}s',
        this.photoReady.toString(), this.isCapturing.toString());
      return;
    }

    this.isCapturing = true;
    this.capturedCount = 0;
    this.processedCount = 0;
    this.thumbnailList = [];
    this.finalImage = undefined;
    this.progressMessage = '准备连拍...';
    this.cameraStatus = '正在连拍...';

    // 构建连拍配置
    const config: BurstConfig = {
      frameCount: this.burstCount,
      exposureMs: this.burstExposure,
      realtimePreview: true
    };

    hilog.info(DOMAIN, TAG, 'Starting burst capture: frameCount=%{public}d, exposureMs=%{public}d',
      this.burstCount, this.burstExposure);

    // 调用 NDK 连拍 API
    const result = nativeCamera.startBurstCapture(
      config,
      // 进度回调
      (progress: BurstProgress) => {
        this.capturedCount = progress.capturedFrames;
        this.processedCount = progress.processedFrames;
        this.progressMessage = progress.message;

        const stateStr = ['IDLE', 'SINGLE_CAPTURING', 'BURST_CAPTURING', 'PROCESSING', 'COMPLETED', 'ERROR', 'CANCELLED'][progress.state];
        hilog.info(DOMAIN, TAG,
          'Burst progress: state=%{public}s, captured=%{public}d/%{public}d, processed=%{public}d/%{public}d, msg=%{public}s',
          stateStr, progress.capturedFrames, progress.totalFrames, progress.processedFrames, progress.totalFrames, progress.message);

        this.cameraStatus = `状态: ${stateStr} | 已拍: ${progress.capturedFrames}/${progress.totalFrames} | 已处理: ${progress.processedFrames}`;

        // 连拍完成或出错
        if (progress.state === nativeCamera.BurstState.COMPLETED ||
          progress.state === nativeCamera.BurstState.ERROR ||
          progress.state === nativeCamera.BurstState.CANCELLED) {
          this.isCapturing = false;
          if (progress.state === nativeCamera.BurstState.COMPLETED) {
            this.cameraStatus = `连拍完成: ${progress.capturedFrames} 张`;
          } else if (progress.state === nativeCamera.BurstState.ERROR) {
            this.cameraStatus = `连拍失败: ${progress.message}`;
          } else {
            this.cameraStatus = `连拍已取消`;
          }
        }
      },
      // 图像回调 (中间预览 + 最终结果)
      (buffer: ArrayBuffer, isFinal: boolean) => {
        hilog.info(DOMAIN, TAG, 'Burst image callback: isFinal=%{public}s, size=%{public}d',
          isFinal.toString(), buffer.byteLength);
        this.processPhotoBuffer(buffer, isFinal);
      }
    );

    if (result.errorCode !== 0) {
      hilog.error(DOMAIN, TAG, 'Failed to start burst capture: errorCode=%{public}d', result.errorCode);
      this.isCapturing = false;
      this.cameraStatus = `启动失败: 错误码 ${result.errorCode}`;
      return;
    }

    hilog.info(DOMAIN, TAG, 'Burst capture started: sessionId=%{public}s', result.sessionId);
  }

  stopBurstCapture(): void {
    if (this.isCapturing) {
      nativeCamera.cancelBurstCapture();
      hilog.info(DOMAIN, TAG, 'Burst capture cancelled');
    }
    this.isCapturing = false;
  }

  clearThumbnails(): void {
    this.thumbnailList = [];
    this.finalImage = undefined;
    this.capturedCount = 0;
    this.processedCount = 0;
    this.progressMessage = '';
    this.cameraStatus = '已清空';
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

        Text('连拍测试')
          .fontSize(16)
          .fontColor(Color.White)
          .layoutWeight(1)
          .textAlign(TextAlign.Center)

        Text(`${this.capturedCount}/${this.burstCount} | ${this.processedCount} processed`)
          .fontSize(14)
          .fontColor(this.isCapturing ? Color.Yellow : Color.White)
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

        // 连拍指示器
        if (this.isCapturing) {
          Column() {
            LoadingProgress()
              .width(48)
              .height(48)
              .color(Color.White)
          }
          .width('100%')
          .height('100%')
          .justifyContent(FlexAlign.Center)
          .alignItems(HorizontalAlign.Center)
        }
      }
      .layoutWeight(1)
      .width('100%')
      .backgroundColor('#000000')

      // 连拍设置
      Row() {
        Text('连拍数量:')
          .fontSize(14)
          .fontColor(Color.White)

        Button('-')
          .fontSize(14)
          .width(32)
          .height(32)
          .backgroundColor('#666666')
          .enabled(this.burstCount > 1)
          .onClick(() => {
            if (this.burstCount > 1) {
              this.burstCount--;
            }
          })

        Text(`${this.burstCount}`)
          .fontSize(16)
          .fontColor(Color.White)
          .width(40)
          .textAlign(TextAlign.Center)

        Button('+')
          .fontSize(14)
          .width(32)
          .height(32)
          .backgroundColor('#666666')
          .enabled(this.burstCount < 20)
          .onClick(() => {
            if (this.burstCount < 20) {
              this.burstCount++;
            }
          })
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 8, bottom: 8 })
      .backgroundColor('#80000000')
      .justifyContent(FlexAlign.Center)

      Row() {
        Text('曝光时间(ms):')
          .fontSize(14)
          .fontColor(Color.White)

        Button('-')
          .fontSize(14)
          .width(32)
          .height(32)
          .backgroundColor('#666666')
          .enabled(this.burstExposure > 1000)
          .onClick(() => {
            if (this.burstExposure > 1000) {
              this.burstExposure -= 1000;
            }
          })

        Text(`${this.burstExposure}`)
          .fontSize(16)
          .fontColor(Color.White)
          .width(50)
          .textAlign(TextAlign.Center)

        Button('+')
          .fontSize(14)
          .width(32)
          .height(32)
          .backgroundColor('#666666')
          .enabled(this.burstExposure < 30000)
          .onClick(() => {
            if (this.burstExposure < 30000) {
              this.burstExposure += 1000;
            }
          })
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 8, bottom: 8 })
      .backgroundColor('#80000000')
      .justifyContent(FlexAlign.Center)

      // 最终堆叠结果
      if (this.finalImage) {
        Row() {
          Text('最终结果')
            .fontSize(14)
            .fontColor(Color.White)
            .margin({ left: 8, right: 8 })

          Image(this.finalImage)
            .width(80)
            .height(106)
            .objectFit(ImageFit.Cover)
            .border({ width: 2, color: Color.Green })
            .margin({ left: 4, right: 4 })
        }
        .width('100%')
        .padding({ top: 8, bottom: 8 })
        .backgroundColor('#60000000')
        .alignItems(VerticalAlign.Center)
      }

      // 中间预览图列表
      if (this.thumbnailList.length > 0) {
        Scroll() {
          Row() {
            Text(`中间预览 (${this.thumbnailList.length})`)
              .fontSize(12)
              .fontColor(Color.White)
              .margin({ left: 8, right: 8 })

            ForEach(this.thumbnailList, (thumbnail: PixelMap, index: number) => {
              Image(thumbnail)
                .width(60)
                .height(80)
                .border({ width: 1, color: Color.Gray })
                .margin({ left: 4, right: 4 })
            })
          }
          .padding({ left: 8, right: 8 })
        }
        .width('100%')
        .height(100)
        .backgroundColor('#40000000')
        .scrollable(ScrollDirection.Horizontal)
      }

      // 控制按钮
      Row() {
        Button('清空')
          .fontSize(14)
          .width('25%')
          .backgroundColor('#666666')
          .enabled(this.thumbnailList.length > 0 || this.finalImage !== undefined)
          .onClick(() => {
            this.clearThumbnails();
          })

        if (this.isCapturing) {
          Button('停止')
            .fontSize(14)
            .width('65%')
            .backgroundColor('#FF0000')
            .onClick(() => {
              this.stopBurstCapture();
            })
        } else {
          Button('开始连拍')
            .fontSize(14)
            .width('65%')
            .enabled(this.photoReady)
            .backgroundColor(this.photoReady ? '#007DFF' : '#666666')
            .onClick(() => {
              this.startBurstCapture();
            })
        }
      }
      .width('100%')
      .padding(16)
      .justifyContent(FlexAlign.SpaceEvenly)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }
}
