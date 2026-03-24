import { window } from '@kit.ArkUI';
import { SAFE_AREA_TOP, SAFE_AREA_BOTTOM } from '../util/WindowManager';

@Entry
@Component
struct Index {
  @StorageLink(SAFE_AREA_TOP) safeTop: number = 0;
  @StorageLink(SAFE_AREA_BOTTOM) safeBottom: number = 0;
  private windowClass: window.Window | undefined = undefined;

  build() {
    Column() {
      // 标题
      Text('Exposhot Camera')
        .fontSize(32)
        .fontColor(Color.White)
        .fontWeight(FontWeight.Bold)
        .margin({ top: this.safeTop + 40 })

      Text('正式版首页')
        .fontSize(16)
        .fontColor('#888888')
        .margin({ top: 8, bottom: 60 })

      // 占位提示
      Column() {
        Text('📷')
          .fontSize(64)
          .margin({ bottom: 24 })

        Text('正式功能开发中...')
          .fontSize(18)
          .fontColor('#666666')
      }
      .layoutWeight(1)
      .justifyContent(FlexAlign.Center)

      // 底部版本
      Text('v1.0.0')
        .fontSize(12)
        .fontColor('#444444')
        .margin({ bottom: this.safeBottom + 20 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#1a1a1a')
    .justifyContent(FlexAlign.Start)
    .alignItems(HorizontalAlign.Center)
  }
}
