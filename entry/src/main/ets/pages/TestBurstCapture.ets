import nativeCamera from 'libexpocamera.so';
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
  @State burstInterval: number = 100; // ms
  @State isCapturing: boolean = false;
  @State capturedCount: number = 0;

  // 缩略图列表
  @State thumbnailList: PixelMap[] = [];

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
    if (!data.buffer) {
      // LOG 未拿到图像数据
      return
    }
    this.processPhotoBuffer(data.buffer);
  }

  async processPhotoBuffer(buffer: ArrayBuffer): Promise<void> {
    try {
      const imageSource = image.createImageSource(buffer);
      const pixelMap = await imageSource.createPixelMap();

      // 添加到缩略图列表
      this.thumbnailList.push(pixelMap);
      this.capturedCount = this.thumbnailList.length;

      hilog.info(DOMAIN, TAG, 'Captured image %{public}d/%{public}d', this.capturedCount, this.burstCount);

      // 检查是否完成连拍
      if (this.capturedCount >= this.burstCount) {
        this.isCapturing = false;
        this.cameraStatus = `连拍完成: ${this.capturedCount} 张`;
      }
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'Failed to process photo buffer: %{public}s', JSON.stringify(error));
    }
  }

  // 模拟连拍
  async startBurstCapture(): Promise<void> {
    if (!this.photoReady || this.isCapturing) {
      return;
    }

    this.isCapturing = true;
    this.capturedCount = 0;
    this.thumbnailList = [];
    this.cameraStatus = '正在连拍...';

    for (let i = 0; i < this.burstCount; i++) {
      if (!this.isCapturing) {
        break;
      }
      nativeCamera.takePhoto();
      await this.delay(this.burstInterval);
    }
  }

  delay(ms: number): Promise<void> {
    return new Promise(resolve => setTimeout(resolve, ms));
  }

  stopBurstCapture(): void {
    this.isCapturing = false;
    this.cameraStatus = `已停止: 拍摄 ${this.capturedCount} 张`;
  }

  clearThumbnails(): void {
    this.thumbnailList = [];
    this.capturedCount = 0;
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

        Text(`${this.capturedCount}/${this.burstCount}`)
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
        Text('间隔(ms):')
          .fontSize(14)
          .fontColor(Color.White)

        Button('-')
          .fontSize(14)
          .width(32)
          .height(32)
          .backgroundColor('#666666')
          .enabled(this.burstInterval > 50)
          .onClick(() => {
            if (this.burstInterval > 50) {
              this.burstInterval -= 50;
            }
          })

        Text(`${this.burstInterval}`)
          .fontSize(16)
          .fontColor(Color.White)
          .width(40)
          .textAlign(TextAlign.Center)

        Button('+')
          .fontSize(14)
          .width(32)
          .height(32)
          .backgroundColor('#666666')
          .enabled(this.burstInterval < 1000)
          .onClick(() => {
            if (this.burstInterval < 1000) {
              this.burstInterval += 50;
            }
          })
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 8, bottom: 8 })
      .backgroundColor('#80000000')
      .justifyContent(FlexAlign.Center)

      // 缩略图列表
      if (this.thumbnailList.length > 0) {
        Scroll() {
          Row() {
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
          .enabled(this.thumbnailList.length > 0)
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
