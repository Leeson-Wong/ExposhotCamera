import { router } from '@kit.ArkUI';

@Entry
@Component
struct Index {
  build() {
    Column() {
      // 标题
      Text('ExposhotCamera')
        .fontSize(28)
        .fontColor(Color.White)
        .fontWeight(FontWeight.Bold)
        .margin({ top: 60, bottom: 8 })

      Text('相机功能测试入口')
        .fontSize(14)
        .fontColor('#888888')
        .margin({ bottom: 40 })

      // 测试入口列表
      Column() {
        // 基础相机测试
        this.buildTestEntry(
          '基础相机测试',
          '单预览、缩放、对焦、拍照',
          'pages/TestBasicCamera'
        )

        // 连拍测试
        this.buildTestEntry(
          '连拍功能测试',
          '多张连拍、间隔设置、缩略图预览',
          'pages/TestBurstCapture'
        )

        // 完整功能测试（双 Slot）
        this.buildTestEntry(
          '完整功能测试',
          '双预览、Slot 切换、完整相机控制',
          'pages/TestFullFeatures'
        )
      }
      .width('90%')
      .layoutWeight(1)

      // 版本信息
      Text('v1.0.0')
        .fontSize(12)
        .fontColor('#555555')
        .margin({ bottom: 20 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#1a1a1a')
    .justifyContent(FlexAlign.Start)
    .alignItems(HorizontalAlign.Center)
  }

  @Builder
  buildTestEntry(title: string, description: string, targetPage: string) {
    Row() {
      Column() {
        Text(title)
          .fontSize(18)
          .fontColor(Color.White)
          .fontWeight(FontWeight.Medium)
          .margin({ bottom: 4 })

        Text(description)
          .fontSize(12)
          .fontColor('#888888')
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      Text('>')
        .fontSize(20)
        .fontColor('#555555')
    }
    .width('100%')
    .padding(16)
    .backgroundColor('#2a2a2a')
    .borderRadius(12)
    .margin({ bottom: 12 })
    .onClick(() => {
      router.pushUrl({ url: targetPage });
    })
  }
}
