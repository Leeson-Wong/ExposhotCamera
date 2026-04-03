import nativeCamera from 'libexpocamera.so';
import recorder, {
  loadStars,
  searchStars,
  startOrientation,
  stopOrientation,
  selectStar,
  stopFile,
  uninitFile,
  setTime,
  playCamera
} from 'libnativerender.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PermissionManager } from '../common/PermissionManager';
import { resourceManager } from '@kit.LocalizationKit';
import { common } from '@kit.AbilityKit';
import { CommonConstant } from '../common/CommonConstant';

const DOMAIN = 0x3200;
const TAG = 'IndexNext';

// 对焦模式枚举（与 C++ 侧保持一致）
enum FocusMode {
  MANUAL = 0,
  CONTINUOUS_AUTO = 1,
  AUTO = 2,
  LOCKED = 3,
}

// Slot 信息
interface SlotInfo {
  id: string;
  surfaceId: string;
  hasPreview: boolean;
}

@Entry
@Component
struct IndexNext {
  @State cameraStatus: string = 'Not initialized';
  @State activeSlotId: string = '';
  @State activeSurfaceId: string = '';

  // Slot A - 星图渲染（相机预览流输出到此 surface，然后叠加星图）
  @State slotA: SlotInfo = { id: '', surfaceId: '', hasPreview: false };

  // Slot B - 普通相机预览（测试切换用）
  @State slotB: SlotInfo = { id: '', surfaceId: '', hasPreview: false };

  // 星图渲染相关
  @State starRenderReady: boolean = false;
  @State starSearchRunning: boolean = false;
  private nativeRecorderObj: bigint | undefined;
  private timer: number = 0;
  private timeTimer: number = 0;
  private hour: number = 11;

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
  private context: common.UIAbilityContext | undefined;

  aboutToAppear(): void {
    this.context = this.getUIContext()?.getHostContext() as common.UIAbilityContext;
    PermissionManager.checkRequestPermissions('ohos.permission.CAMERA').then((res: boolean) => {
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
    });
  }

  // onPageShow(): void {
  //   hilog.info(DOMAIN, TAG, 'Slot A (Star) XComponent onLoad');
  //   const surfaceId = this.xComponentControllerA.getXComponentSurfaceId();
  //   hilog.info(DOMAIN, TAG, 'Slot A surfaceId: %{public}s', surfaceId);
  //
  //   // 注册到相机观察者，相机预览流会输出到这个 surface
  //   this.slotA.surfaceId = surfaceId;
  //   this.slotA.id = this.registerObserver(surfaceId);
  //
  //   // 初始化星图渲染器
  //   this.initStarRender(surfaceId);
  // }

  aboutToDisappear(): void {
    // 停止星图渲染
    this.stopStarSearch();

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

  onStarPreviewChanged = (activeSlotId: string, activeSurfaceId: string): void => {
    this.activeSurfaceId = activeSurfaceId
    hilog.info(DOMAIN, TAG, 'onStarPreviewChanged changed: activeSlotId=%{public}s', activeSlotId);

    // 更新活跃 slot
    this.activeSlotId = activeSlotId;

    // 每个 slot 自己判断是否获得了预览流
    this.slotA.hasPreview = (activeSlotId === this.slotA.id && activeSlotId !== '');
    this.slotB.hasPreview = (activeSlotId === this.slotB.id && activeSlotId !== '');

    // 如果有活跃的 slot，更新相机参数
    if (activeSlotId !== '') {
      this.cameraStatus = `Preview on ${activeSlotId}`;
      this.initCameraParams();
    }
  };

  // 观察者回调 - 所有观察者都会收到，自己判断是否获得预览流
  onPreviewChanged = (activeSlotId: string, activeSurfaceId: string): void => {
    this.activeSurfaceId = activeSurfaceId
    hilog.info(DOMAIN, TAG, 'Preview changed: activeSlotId=%{public}s', activeSlotId);

    // 更新活跃 slot
    this.activeSlotId = activeSlotId;

    // 每个 slot 自己判断是否获得了预览流
    this.slotA.hasPreview = (activeSlotId === this.slotA.id && activeSlotId !== '');
    this.slotB.hasPreview = (activeSlotId === this.slotB.id && activeSlotId !== '');

    // 如果有活跃的 slot，更新相机参数
    if (activeSlotId !== '') {
      this.cameraStatus = `Preview on ${activeSlotId}`;
      this.initCameraParams();
    }
  };

  // 注册观察者
  registerObserver(surfaceId: string, callback?: (activeSlotId: string, activeSurfaceId: string) => void): string {
    const slotId = nativeCamera.registerObserver(surfaceId, callback || this.onPreviewChanged);
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

  // ==================== 星图渲染方法 ====================

  // 初始化星图渲染（type=0 表示相机预览模式，与 YUVView 一致）
  async initStarRender(surfaceId: string): Promise<void> {
    await recorder.initObject(1920, 1080, 30, 0, surfaceId).then((data: recorder.Response) => {
      if (data.code === 0) {
        this.nativeRecorderObj = data.recorder;
        hilog.info(DOMAIN, TAG, 'Star render initialized (type=0)');
        this.starRenderReady = true;
      } else {
        hilog.error(DOMAIN, TAG, 'Failed to init star render: %{public}d', data.code);
      }
    });
  }

  // 启动星图渲染（先启动相机预览，再加载星图）
  startStarRender(): void {
    if (!this.nativeRecorderObj || !this.context) {
      hilog.error(DOMAIN, TAG, 'Native recorder not initialized');
      return;
    }
    // 启动相机预览渲染
    recorder.playCamera(this.nativeRecorderObj, this.slotA.surfaceId);
    hilog.info(DOMAIN, TAG, 'Camera preview started');

    // 加载星图数据
    loadStars(this.context.resourceManager, this.slotA.surfaceId);
    hilog.info(DOMAIN, TAG, 'Star data loaded');
  }

  // 开始搜索星星
  startStarSearch(): void {
    if (!this.nativeRecorderObj || !this.context) {
      hilog.error(DOMAIN, TAG, 'Native recorder not initialized');
      return;
    }
    const result = searchStars(
      this.context.resourceManager,
      30, 120, 60,
      this.slotA.surfaceId,
      3.3,
      this.nativeRecorderObj
    );

    if (result === 0) {
      this.starSearchRunning = true;
      hilog.info(DOMAIN, TAG, 'Star search started');
      // 定时更新
      this.timer = setInterval(() => {
        searchStars(
          this.context!.resourceManager,
          30, 120, 60,
          this.slotA.surfaceId,
          3.3,
          this.nativeRecorderObj
        );
      }, 50);
    } else {
      hilog.error(DOMAIN, TAG, 'Search failed: %{public}d', result);
    }
  }

  // 停止星图搜索
  stopStarSearch(): void {
    // 停止定时器
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = 0;
    }
    if (this.timeTimer) {
      clearInterval(this.timeTimer);
      this.timeTimer = 0;
    }

    // 停止星图渲染
    if (this.nativeRecorderObj) {
      stopOrientation();
      stopFile(this.nativeRecorderObj).then(() => {
        uninitFile(this.nativeRecorderObj);
        this.nativeRecorderObj = undefined;
        this.starRenderReady = false;
        this.starSearchRunning = false;
        hilog.info(DOMAIN, TAG, 'Star render stopped');
      });
    }
  }

  // 设置时间
  setTimeForStars(): void {
    if (!this.nativeRecorderObj) {
      hilog.error(DOMAIN, TAG, 'Native recorder not initialized');
      return;
    }

    // 如果定时器未启动，则启动
    if (!this.timeTimer) {
      this.timeTimer = setInterval(() => {
        setTime(this.nativeRecorderObj, 2026, 3, 5, (this.hour++) % 20, 7, 6);
      }, 1000);
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

        Text(this.starRenderReady ? '★ 星图就绪' : '')
          .fontSize(14)
          .fontColor(Color.Orange)
          .margin({ left: 8 })
      }
      .width('100%')
      .backgroundColor('#80000000')
      .padding(12)

      // 主渲染区域 - 两个 Slot
      Column() {
        // Slot A - 星图渲染（相机预览流输出到此 surface
        this.buildStarSlot()

        // Slot B - 普通相机预览（测试切换用）
        // if (this.slotB.id && !this.slotA.hasPreview) {
        //
        // }
        this.buildCameraSlot(this.slotB, this.xComponentControllerB)
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

      // 控制按钮 - Slot 切换
      Row() {
        Button('星图渲染')
          .fontSize(14)
          .width('45%')
          .backgroundColor(this.slotA.hasPreview ? '#007DFF' : '#666666')
          .onClick(() => {
            if (this.slotA.id) {
              this.switchToSlot(this.slotA.id);
            }
          })

        Button('相机预览')
          .fontSize(14)
          .width('45%')
          .backgroundColor(this.slotB.hasPreview ? '#007DFF' : '#666666')
          .onClick(() => {
            if (this.slotB.id) {
              this.switchToSlot(this.slotB.id);
            }
          })
      }
      .width('100%')
      .padding(12)
      .justifyContent(FlexAlign.SpaceEvenly)

      // 控制按钮 - 星图功能
      Row() {
        Button('显示星图')
          .fontSize(14)
          .width('22%')
          .backgroundColor(this.starRenderReady ? '#007DFF' : '#666666')
          .enabled(this.slotA.hasPreview)
          .onClick(() => {
            this.startStarRender();
            // playCamera(this.nativeRecorderObj, this.activeSlotId)
          })

        Button('搜索星星')
          .fontSize(14)
          .width('22%')
          .backgroundColor(this.starSearchRunning ? '#00FF00' : '#666666')
          .enabled(this.starRenderReady && !this.starSearchRunning)
          .onClick(() => {
            this.startStarSearch();
          })

        Button('停止')
          .fontSize(14)
          .width('22%')
          .backgroundColor('#FF4444')
          .onClick(() => {
            this.stopStarSearch();
          })

        Button('时间')
          .fontSize(14)
          .width('22%')
          .backgroundColor(this.timeTimer ? '#00FF00' : '#666666')
          .enabled(this.starSearchRunning)
          .onClick(() => {
            this.setTimeForStars();
          })
      }
      .width('100%')
      .padding(12)
      .justifyContent(FlexAlign.SpaceEvenly)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }

  // ==================== Slot A - 星图渲染 ====================
  @Builder
  buildStarSlot() {
    Stack({ alignContent: Alignment.TopStart }) {
      // XComponent 绑定到 nativerender，用于星图渲染
      XComponent({
        id: 'starRenderA',
        type: XComponentType.TEXTURE,
        libraryname: 'nativerender',
        controller: this.xComponentControllerA
      })
        .onLoad(() => {
          hilog.info(DOMAIN, TAG, 'Slot A (Star) XComponent onLoad');
          const surfaceId = this.xComponentControllerA.getXComponentSurfaceId();
          hilog.info(DOMAIN, TAG, 'Slot A surfaceId: %{public}s', surfaceId);

          // 注册到相机观察者，相机预览流会输出到这个 surface
          this.slotA.surfaceId = surfaceId;
          this.slotA.id = this.registerObserver(surfaceId, this.onStarPreviewChanged);

          // 初始化星图渲染器
          this.initStarRender(surfaceId);
        })
        .width('100%')
        .height('100%')

      // 状态指示
      Row() {
        Text(this.slotA.hasPreview ? '●' : '○')
          .fontSize(16)
          .fontColor(this.slotA.hasPreview ? Color.Green : Color.Gray)
        Text(' 星图渲染')
          .fontSize(12)
          .fontColor(Color.Orange)
      }
      .padding(8)
    }
    .layoutWeight(1)
    .width('100%')
    .border(this.slotA.hasPreview ? { width: 3, color: Color.Orange } : { width: 1, color: Color.Gray })
    .clip(true)
  }

  // ==================== Slot B - 普通相机预览 ====================
  @Builder
  buildCameraSlot(slotInfo: SlotInfo, controller: XComponentController) {
    Stack({ alignContent: Alignment.TopStart }) {
      XComponent({
        id: 'cameraPreviewB',
        type: 'surface',
        libraryname: 'expocamera',
        controller: controller
      })
        .onLoad(() => {
          hilog.info(DOMAIN, TAG, 'Slot B (Camera) XComponent onLoad');
          const surfaceId = controller.getXComponentSurfaceId();
          hilog.info(DOMAIN, TAG, 'Slot B surfaceId: %{public}s', surfaceId);

          // 注册观察者
          slotInfo.surfaceId = surfaceId;
          slotInfo.id = this.registerObserver(surfaceId);
        })
        .width('100%')
        .height('100%')

      // 状态指示
      Row() {
        Text(slotInfo.hasPreview ? '●' : '○')
          .fontSize(16)
          .fontColor(slotInfo.hasPreview ? Color.Green : Color.Gray)
        Text(' 相机预览')
          .fontSize(12)
          .fontColor(Color.White)
      }
      .padding(8)
    }
    .layoutWeight(1)
    .width('100%')
    .border(slotInfo.hasPreview ? { width: 3, color: Color.Green } : { width: 1, color: Color.Gray })
    .clip(true)
  }
}
