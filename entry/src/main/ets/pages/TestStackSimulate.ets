import nativeCamera from 'libexpocamera.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { router } from '@kit.ArkUI';

const DOMAIN = 0x3200;
const TAG = 'TestStackSimulate';

// 堆叠状态
enum StackState {
    IDLE = 0,
    STACKING = 1,
    COMPLETED = 2,
    ERROR = 3,
}

// 模拟帧数据
interface MockFrame {
    id: number;
    timestamp: number;
    processed: boolean;
}

@Entry
@Component
struct TestStackSimulate {
    @State cameraStatus: string = '未初始化';
    @State surfaceId: string = '';
    @State cameraInitialized: boolean = false;

    // 堆叠配置
    @State frameCount: number = 10;
    @State delayMs: number = 500;
    @State stackState: StackState = StackState.IDLE;
    @State currentFrameIndex: number = 0;
    @State progress: number = 0;
    @State stackResultMessage: string = '';

    // 模拟帧数据
    @State frames: MockFrame[] = [];

    // XComponent 控制器
    private xComponentController: XComponentController = new XComponentController();

    aboutToAppear(): void {
        // 获取 resourceManager 并传给 NDK
        const resMgr = getContext(this).resourceManager;

        // 初始化相机（传入 resourceManager 用于访问 rawfile）
        const result = nativeCamera.initCamera(resMgr);
        if (result === 0) {
            this.cameraInitialized = true;
            this.cameraStatus = '相机已初始化';
            hilog.info(DOMAIN, TAG, 'Camera initialized for mock stacking');
        } else {
            this.cameraStatus = `相机初始化失败: ${result}`;
        }
    }

    aboutToDisappear(): void {
        // 释放相机
        if (this.cameraInitialized) {
            nativeCamera.releaseCamera();
        }
    }

    // 开始模拟堆叠
    startStackSimulation(): void {
        if (this.stackState !== StackState.IDLE) {
            this.cameraStatus = '堆叠正在进行中';
            return;
        }

        if (!this.cameraInitialized) {
            this.cameraStatus = '相机未初始化';
            return;
        }

        if (!this.surfaceId) {
            this.cameraStatus = 'SurfaceId 未就绪';
            return;
        }

        this.stackState = StackState.STACKING;
        this.currentFrameIndex = 0;
        this.progress = 0;
        this.frames = [];
        this.stackResultMessage = '';

        this.cameraStatus = '开始模拟堆叠...';
        hilog.info(DOMAIN, TAG, 'Starting mock stack simulation, frameCount=%{public}d, surfaceId=%{public}s',
                  this.frameCount, this.surfaceId);

        // 开始处理第一帧
        this.processNextFrame();
    }

    // 处理下一帧
    processNextFrame(): void {
        if (this.currentFrameIndex >= this.frameCount) {
            // 所有帧处理完成
            this.stackState = StackState.COMPLETED;
            this.progress = 100;
            this.stackResultMessage = `堆叠完成！共处理 ${this.frameCount} 帧`;
            this.cameraStatus = this.stackResultMessage;
            hilog.info(DOMAIN, TAG, 'Mock stacking completed: %{public}d frames', this.frameCount);
            return;
        }

        // 调用 NDK 接口处理当前帧，传入 surfaceId
        const result = nativeCamera.mockStackProcess(this.surfaceId, this.currentFrameIndex);

        hilog.info(DOMAIN, TAG, 'MockStackProcess result: success=%{public}s, next=%{public}d, msg=%{public}s',
                  result.success.toString(), result.nextFrameIndex, result.message);

        if (result.success) {
            // 记录已处理的帧
            this.frames.push({
                id: this.currentFrameIndex,
                timestamp: Date.now(),
                processed: true
            });

            this.currentFrameIndex = result.nextFrameIndex;
            this.progress = Math.floor((this.currentFrameIndex / this.frameCount) * 100);
            this.cameraStatus = `堆叠中... ${this.currentFrameIndex}/${this.frameCount}`;
            this.stackResultMessage = result.message;

            // 延迟后处理下一帧
            if (this.currentFrameIndex < this.frameCount) {
                setTimeout(() => {
                    this.processNextFrame();
                }, this.delayMs);
            } else {
                // 最后一帧处理完成
                this.stackState = StackState.COMPLETED;
                this.progress = 100;
                this.stackResultMessage = `堆叠完成！共处理 ${this.frameCount} 帧`;
                this.cameraStatus = this.stackResultMessage;
                hilog.info(DOMAIN, TAG, 'Mock stacking completed successfully');
            }
        } else {
            this.stackState = StackState.ERROR;
            this.cameraStatus = `堆叠失败: ${result.message}`;
            this.stackResultMessage = result.message;
            hilog.error(DOMAIN, TAG, 'Mock stacking failed: %{public}s', result.message);
        }
    }

    // 取消堆叠
    cancelStack(): void {
        this.stackState = StackState.IDLE;
        this.currentFrameIndex = 0;
        this.progress = 0;
        this.frames = [];
        this.stackResultMessage = '堆叠已取消';
        this.cameraStatus = '堆叠已取消';
        hilog.info(DOMAIN, TAG, 'Mock stacking cancelled');
    }

    // 重置
    reset(): void {
        this.cancelStack();
        this.stackState = StackState.IDLE;
        this.stackResultMessage = '';
        this.cameraStatus = '相机已初始化';
    }

    // 获取状态名称
    getStateName(state: StackState): string {
        switch (state) {
            case StackState.IDLE: return '空闲';
            case StackState.STACKING: return '堆叠中';
            case StackState.COMPLETED: return '已完成';
            case StackState.ERROR: return '错误';
            default: return '未知';
        }
    }

    // 获取状态颜色
    getStateColor(state: StackState): string {
        switch (state) {
            case StackState.IDLE: return '#888888';
            case StackState.STACKING: return '#FF9500';
            case StackState.COMPLETED: return '#34C759';
            case StackState.ERROR: return '#FF3B30';
            default: return '#888888';
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

                Text('模拟堆叠测试')
                    .fontSize(16)
                    .fontColor(Color.White)
                    .layoutWeight(1)
                    .textAlign(TextAlign.Center)

                // 相机状态
                Row() {
                    Circle()
                        .width(8)
                        .height(8)
                        .fill(this.cameraInitialized ? '#34C759' : '#FF3B30')

                    Text(this.cameraInitialized ? '相机已初始化' : '未初始化')
                        .fontSize(12)
                        .fontColor(Color.White)
                        .margin({ left: 4 })
                }
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

            // XComponent（用于获取 surfaceId）
            XComponent({
                id: 'stackPreview',
                type: 'surface',
                libraryname: 'expocamera',
                controller: this.xComponentController
            })
                .onLoad(() => {
                    hilog.info(DOMAIN, TAG, 'XComponent onLoad');
                    this.surfaceId = this.xComponentController.getXComponentSurfaceId();
                    this.cameraStatus = `SurfaceId: ${this.surfaceId}`;
                    hilog.info(DOMAIN, TAG, 'Got surfaceId: %{public}s', this.surfaceId);
                })
                .width('100%')
                .layoutWeight(1)
                .opacity(0.01)  // 几乎隐藏，但保持渲染以获取 surfaceId

            // Surface ID 显示
            if (this.surfaceId) {
                Text(`Surface: ${this.surfaceId}`)
                    .fontSize(12)
                    .fontColor('#00FF00')
                    .width('100%')
                    .textAlign(TextAlign.Center)
                    .backgroundColor('#800000')
                    .padding(4)
            }

            // 堆叠进度信息区域
            if (this.stackState !== StackState.IDLE) {
                Column() {
                    // 状态行
                    Row() {
                        // 状态图标
                        Text(this.stackState === StackState.COMPLETED ? '✅' :
                              this.stackState === StackState.ERROR ? '❌' : '⚙')
                            .fontSize(20)
                            .fontColor(Color.White)

                        Text(this.getStateName(this.stackState))
                            .fontSize(14)
                            .fontColor(Color.White)
                            .margin({ left: 8 })

                        Blank()

                        Text(`${this.progress}%`)
                            .fontSize(14)
                            .fontColor(Color.White)
                    }
                    .width('100%')
                    .padding(8)
                    .backgroundColor('#A0000000')

                    // 进度环和帧数
                    Row() {
                        Circle()
                            .width(80)
                            .height(80)
                            .fill(Color.Transparent)
                            .stroke('#007DFF')
                            .strokeWidth(4)

                        Text(`${this.currentFrameIndex}/${this.frameCount}`)
                            .fontSize(24)
                            .fontColor(Color.White)
                            .fontWeight(FontWeight.Bold)
                            .position({ x: '50%', y: '50%' })
                            .markAnchor({ x: '50%', y: '50%' })
                    }
                    .width(100)
                    .height(100)
                    .margin({ top: 8, bottom: 8 })
                    .justifyContent(FlexAlign.Center)

                    // 帧缩略图
                    if (this.frames.length > 0) {
                        Row({ space: 4 }) {
                            ForEach(this.frames, (frame: MockFrame) => {
                                Text(`#${frame.id}`)
                                    .fontSize(10)
                                    .fontColor(Color.White)
                                    .width(35)
                                    .height(35)
                                    .backgroundColor('#007DFF')
                                    .borderRadius(4)
                                    .textAlign(TextAlign.Center)
                                    .padding({ top: 8 })
                            })
                        }
                        .width('100%')
                        .padding({ left: 8, right: 8, bottom: 8 })
                        .justifyContent(FlexAlign.Start)
                    }
                }
                .width('100%')
                .backgroundColor('#60000000')
                .margin({ top: 8 })
            }

            // 配置和控制区域
            Column() {
                // 帧数配置
                Row() {
                    Text('帧数:')
                        .fontSize(14)
                        .fontColor(Color.White)
                        .width(60)

                    Slider({
                        value: this.frameCount,
                        min: 2,
                        max: 50,
                        step: 1,
                        style: SliderStyle.OutSet
                    })
                        .blockColor(Color.White)
                        .trackColor('#666666')
                        .selectedColor('#007DFF')
                        .layoutWeight(1)
                        .enabled(this.stackState === StackState.IDLE)
                        .onChange((value: number) => {
                            this.frameCount = value;
                        })

                    Text(`${this.frameCount}`)
                        .fontSize(14)
                        .fontColor(Color.White)
                        .width(30)
                        .textAlign(TextAlign.End)
                }
                .width('100%')
                .padding({ left: 16, right: 16, top: 8, bottom: 8 })
                .backgroundColor('#80000000')

                // 延迟配置
                Row() {
                    Text('延迟:')
                        .fontSize(14)
                        .fontColor(Color.White)
                        .width(60)

                    Slider({
                        value: this.delayMs,
                        min: 100,
                        max: 2000,
                        step: 100,
                        style: SliderStyle.OutSet
                    })
                        .blockColor(Color.White)
                        .trackColor('#666666')
                        .selectedColor('#007DFF')
                        .layoutWeight(1)
                        .enabled(this.stackState === StackState.IDLE)
                        .onChange((value: number) => {
                            this.delayMs = value;
                        })

                    Text(`${this.delayMs}ms`)
                        .fontSize(14)
                        .fontColor(Color.White)
                        .width(50)
                        .textAlign(TextAlign.End)
                }
                .width('100%')
                .padding({ left: 16, right: 16, top: 8, bottom: 8 })
                .backgroundColor('#80000000')

                // 控制按钮
                Row() {
                    Button('开始堆叠')
                        .fontSize(16)
                        .height(45)
                        .enabled(this.stackState === StackState.IDLE && this.surfaceId !== '')
                        .backgroundColor(this.stackState === StackState.IDLE && this.surfaceId !== '' ? '#007DFF' : '#666666')
                        .layoutWeight(1)
                        .onClick(() => {
                            this.startStackSimulation();
                        })

                    Button('取消')
                        .fontSize(16)
                        .height(45)
                        .enabled(this.stackState === StackState.STACKING)
                        .backgroundColor(this.stackState === StackState.STACKING ? '#FF9500' : '#666666')
                        .layoutWeight(1)
                        .margin({ left: 8 })
                        .onClick(() => {
                            this.cancelStack();
                        })

                    Button('重置')
                        .fontSize(16)
                        .height(45)
                        .backgroundColor('#666666')
                        .layoutWeight(1)
                        .margin({ left: 8 })
                        .onClick(() => {
                            this.reset();
                        })
                }
                .width('100%')
                .padding(16)
                .justifyContent(FlexAlign.Center)

                // 统计信息
                Row() {
                    Column() {
                        Text(`${this.currentFrameIndex}`)
                            .fontSize(24)
                            .fontColor(Color.White)
                            .fontWeight(FontWeight.Bold)
                        Text('已处理')
                            .fontSize(12)
                            .fontColor('#888888')
                    }
                    .width('30%')
                    .alignItems(HorizontalAlign.Center)

                    Column() {
                        Text(`${this.frameCount}`)
                            .fontSize(24)
                            .fontColor(Color.White)
                            .fontWeight(FontWeight.Bold)
                        Text('总帧数')
                            .fontSize(12)
                            .fontColor('#888888')
                    }
                    .width('30%')
                    .alignItems(HorizontalAlign.Center)

                    Column() {
                        Text(`${this.progress}%`)
                            .fontSize(24)
                            .fontColor(Color.White)
                            .fontWeight(FontWeight.Bold)
                        Text('进度')
                            .fontSize(12)
                            .fontColor('#888888')
                    }
                    .width('30%')
                    .alignItems(HorizontalAlign.Center)
                }
                .width('100%')
                .padding({ top: 8, bottom: 16 })
                .justifyContent(FlexAlign.SpaceEvenly)
            }
            .width('100%')
            .backgroundColor('#40000000')
        }
        .width('100%')
        .height('100%')
        .backgroundColor('#000000')
    }
}
