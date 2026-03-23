import nativeCamera, { BurstConfig, BurstProgress } from 'libexpocamera.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { image } from '@kit.ImageKit';
import { router } from '@kit.ArkUI';
import { SAFE_AREA_TOP, SAFE_AREA_BOTTOM } from '../util/WindowManager';

const DOMAIN = 0x3200;
const TAG = 'TestBurstCapture';

@Entry
@Component
struct TestBurstCapture {
  @StorageProp(SAFE_AREA_TOP) safeTop: number = 0;
  @StorageProp(SAFE_AREA_BOTTOM) safeBottom: number = 0;
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

  // 延迟拍摄控制
  @State delaySeconds: number = 2;  // 延迟秒数，0 表示无延迟
  @State countdown: number = 0;     // 当前倒计时
  @State isCountingDown: boolean = false;  // 是否正在倒计时

  // 缩略图列表（用于存储中间预览图）
  @State thumbnailList: PixelMap[] = [];
  // 最终堆叠结果
  @State finalImage?: PixelMap = undefined;

  private surfaceId: string = '';
  private xComponentController: XComponentController = new XComponentController();
  private countdownTimer: number = -1;  // 倒计时定时器 ID

  aboutToAppear(): void {
    // 连拍模式使用 startBurstCapture 的回调参数接收图像，不需要注册全局回调
    // nativeCamera.registerImageDataCallback 仅用于单次拍照 (takePhoto)

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
    // 清除倒计时定时器
    if (this.countdownTimer !== -1) {
      clearTimeout(this.countdownTimer);
      this.countdownTimer = -1;
    }

    // 取消连拍（防止页面销毁后回调继续执行）
    if (this.isCapturing) {
      nativeCamera.cancelBurstCapture();
      this.isCapturing = false;
    }

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
    }
  }

  registerObserver(surfaceId: string): string {
    const slotId = nativeCamera.registerObserver(surfaceId, this.onPreviewChanged);
    nativeCamera.startPreview(surfaceId);
    hilog.info(DOMAIN, TAG, 'registerObserver: slotId=%{public}s', slotId);
    return slotId;
  }

  // 处理连拍图像（中间预览 + 最终结果）
  async processPhotoBuffer(buffer: ArrayBuffer, isFinal: boolean): Promise<void> {
    if (!isFinal) {
      return
    }
    let imageSource: image.ImageSource | undefined;
    try {
      imageSource = image.createImageSource(buffer);
      const pixelMap = await imageSource.createPixelMap();

      if (isFinal) {
        // 先释放旧的最终结果
        if (this.finalImage) {
          this.finalImage.release();
          this.finalImage = undefined;
        }
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
    } finally {
      // 显式释放 ImageSource，避免 GC 延迟回收时 EXIF 野指针崩溃
      imageSource?.release();
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

  // 开始延迟拍摄
  startDelayedCapture(): void {
    if (!this.photoReady || this.isCapturing || this.isCountingDown) {
      hilog.warn(DOMAIN, TAG, 'Cannot start delayed capture');
      return;
    }

    // 如果延迟时间为 0，直接开始拍摄
    if (this.delaySeconds === 0) {
      this.startBurstCapture();
      return;
    }

    // 开始倒计时
    this.isCountingDown = true;
    this.countdown = this.delaySeconds;
    this.cameraStatus = `倒计时: ${this.countdown} 秒`;
    hilog.info(DOMAIN, TAG, 'Starting countdown: %{public}d seconds', this.delaySeconds);

    this.countdownTimer = setInterval(() => {
      this.countdown--;
      if (this.countdown > 0) {
        this.cameraStatus = `倒计时: ${this.countdown} 秒`;
        hilog.info(DOMAIN, TAG, 'Countdown: %{public}d', this.countdown);
      } else {
        // 倒计时结束，清除定时器并开始拍摄
        this.stopCountdown();
        this.cameraStatus = '开始拍摄!';
        this.startBurstCapture();
      }
    }, 1000);
  }

  // 停止倒计时
  stopCountdown(): void {
    if (this.countdownTimer !== -1) {
      clearInterval(this.countdownTimer);
      this.countdownTimer = -1;
    }
    this.isCountingDown = false;
    this.countdown = 0;
    if (!this.isCapturing) {
      this.cameraStatus = '已取消';
    }
  }

  // 取消拍摄（包括倒计时和正在进行的拍摄）
  cancelCapture(): void {
    if (this.isCountingDown) {
      this.stopCountdown();
      hilog.info(DOMAIN, TAG, 'Countdown cancelled');
    }
    if (this.isCapturing) {
      this.stopBurstCapture();
      hilog.info(DOMAIN, TAG, 'Burst capture cancelled');
    }
  }

  clearThumbnails(): void {
    // 释放所有预览图
    for (const thumbnail of this.thumbnailList) {
      thumbnail.release();
    }
    this.thumbnailList = [];

    // 释放最终结果
    if (this.finalImage) {
      this.finalImage.release();
      this.finalImage = undefined;
    }

    this.capturedCount = 0;
    this.processedCount = 0;
    this.progressMessage = '';
    this.cameraStatus = '已清空';
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

          Text('连拍测试')
            .fontSize(16)
            .fontColor(Color.White)
            .layoutWeight(1)
            .textAlign(TextAlign.Center)

          Text(`${this.capturedCount}/${this.burstCount} | ${this.processedCount} processed`)
            .fontSize(12)
            .fontColor(this.isCapturing ? Color.Yellow : Color.White)
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

        // 倒计时大字显示
        if (this.isCountingDown) {
          Column() {
            Text(`${this.countdown}`)
              .fontSize(120)
              .fontColor(Color.White)
              .fontWeight(FontWeight.Bold)
              .textShadow({ radius: 10, color: '#80000000', offsetX: 0, offsetY: 0 })
          }
          .width('100%')
          .layoutWeight(1)
          .justifyContent(FlexAlign.Center)
          .alignItems(HorizontalAlign.Center)
        } else {
          // 弹性空间
          Blank()
        }

        // 最终堆叠结果预览
        if (this.finalImage) {
          Column() {
            Image(this.finalImage)
              .width(80)
              .height(106)
              .objectFit(ImageFit.Cover)
              .border({ width: 2, color: Color.Green })
              .borderRadius(4)
          }
          .width('100%')
          .alignItems(HorizontalAlign.End)
          .padding({ right: 16, bottom: 16 })
        }

        // 底部控制区
        Column() {
          // 连拍数量设置
          Row() {
            Text('连拍数量:')
              .fontSize(14)
              .fontColor(Color.White)
              .width(70)

            Button('-')
              .fontSize(14)
              .width(32)
              .height(28)
              .backgroundColor('#666666')
              .enabled(this.burstCount > 1 && !this.isCapturing)
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
              .height(28)
              .backgroundColor('#666666')
              .enabled(this.burstCount < 20 && !this.isCapturing)
              .onClick(() => {
                if (this.burstCount < 20) {
                  this.burstCount++;
                }
              })
          }
          .width('100%')
          .padding({ left: 16, right: 16, top: 8, bottom: 4 })

          // 曝光时间设置
          Row() {
            Text('曝光时间(ms):')
              .fontSize(14)
              .fontColor(Color.White)
              .width(90)

            Button('-')
              .fontSize(14)
              .width(32)
              .height(28)
              .backgroundColor('#666666')
              .enabled(this.burstExposure > 1000 && !this.isCapturing)
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
              .height(28)
              .backgroundColor('#666666')
              .enabled(this.burstExposure < 30000 && !this.isCapturing)
              .onClick(() => {
                if (this.burstExposure < 30000) {
                  this.burstExposure += 1000;
                }
              })
          }
          .width('100%')
          .padding({ left: 16, right: 16, top: 4, bottom: 4 })

          // 延迟时间设置
          Row() {
            Text('延迟拍摄(秒):')
              .fontSize(14)
              .fontColor(Color.White)
              .width(90)

            Button('-')
              .fontSize(14)
              .width(32)
              .height(28)
              .backgroundColor('#666666')
              .enabled(this.delaySeconds > 0 && !this.isCapturing && !this.isCountingDown)
              .onClick(() => {
                if (this.delaySeconds > 0) {
                  this.delaySeconds--;
                }
              })

            Text(this.delaySeconds === 0 ? '关' : `${this.delaySeconds}`)
              .fontSize(16)
              .fontColor(Color.White)
              .width(40)
              .textAlign(TextAlign.Center)

            Button('+')
              .fontSize(14)
              .width(32)
              .height(28)
              .backgroundColor('#666666')
              .enabled(this.delaySeconds < 10 && !this.isCapturing && !this.isCountingDown)
              .onClick(() => {
                if (this.delaySeconds < 10) {
                  this.delaySeconds++;
                }
              })

            Text('(0=立即)')
              .fontSize(12)
              .fontColor(Color.Gray)
              .margin({ left: 8 })
          }
          .width('100%')
          .padding({ left: 16, right: 16, top: 4, bottom: 8 })

          // 控制按钮
          Row() {
            Button('清空')
              .fontSize(14)
              .width('25%')
              .height(44)
              .backgroundColor('#666666')
              .enabled(this.thumbnailList.length > 0 || this.finalImage !== undefined)
              .onClick(() => {
                this.clearThumbnails();
              })

            if (this.isCapturing || this.isCountingDown) {
              Button('取消')
                .fontSize(16)
                .width('65%')
                .height(44)
                .backgroundColor('#FF0000')
                .onClick(() => {
                  this.cancelCapture();
                })
            } else {
              Button(this.delaySeconds > 0 ? `延迟 ${this.delaySeconds}s 拍摄` : '开始连拍')
                .fontSize(16)
                .width('65%')
                .height(44)
                .enabled(this.photoReady)
                .backgroundColor(this.photoReady ? '#007DFF' : '#666666')
                .onClick(() => {
                  this.startDelayedCapture();
                })
            }
          }
          .width('100%')
          .padding({ left: 16, right: 16, top: 8, bottom: this.safeBottom + 16 })
          .justifyContent(FlexAlign.SpaceEvenly)
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
