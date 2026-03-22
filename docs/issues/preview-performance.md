# 预览流性能问题

## 问题描述

使用 NDK 原生相机接口实现的预览流存在明显卡顿，相比纯 ArkTS 实现的相机预览流畅度有明显差距。

## 现象

- 预览流画面有卡顿感
- 修改 Preview Profile 分辨率（1280x720 / 1920x1080）无明显改善
- Profile 选择逻辑本身不是卡顿原因

## 对比参考

纯 ArkTS 实现的相机（CustomCamera 示例）预览流畅，无明显卡顿。

## 可能原因

1. **NDK vs ArkTS API 差异**
   - NDK 的 Camera API 与 ArkTS 的 Camera Kit API 底层实现可能有差异
   - 需要确认 NDK API 是否有额外的性能开销

2. **Surface / Buffer 处理**
   - NDK 层的 Surface 创建和 Buffer 管理方式可能不同
   - 可能存在额外的内存拷贝或同步开销

3. **线程模型差异**
   - NDK 回调的线程上下文可能与 ArkTS 不同
   - 需要确认预览帧的回调频率和处理时机

4. **Preview Profile 配置**
   - 当前支持的 profiles 列表：
     ```
     [Profile 0] 480x480 (1:1)
     [Profile 1] 640x480 (4:3)
     [Profile 2] 720x720 (1:1)
     [Profile 3] 856x480 (~16:9)
     [Profile 4] 864x480 (~16:9)
     [Profile 5] 960x720 (4:3)
     [Profile 6] 1056x480 (~16:9)
     [Profile 7] 1064x480 (~16:9)
     [Profile 8] 1080x1080 (1:1)
     [Profile 9] 1088x1088 (1:1)
     [Profile 10] 1280x720 (16:9) ← 当前选中
     [Profile 11] 1440x1080 (4:3)
     [Profile 12] 1440x1440 (1:1)
     [Profile 13] 1920x1080 (16:9)
     [Profile 14] 1920x1440 (4:3)
     [Profile 15] 2384x1080 (~16:9)
     [Profile 16] 2392x1080 (~16:9)
     [Profile 17] 2408x1080 (~16:9)
     [Profile 18] 2560x1440 (16:9)
     ```

## 待确认事项

- [ ] 对比 NDK 和 ArkTS API 的预览帧率
- [ ] 检查 NDK 层是否有不必要的锁或同步
- [ ] 确认 Surface 创建和绑定的最佳实践
- [ ] 测试不同 Profile 的帧率表现
- [ ] 对比 CustomCamera 的 PreviewManager 实现

## 相关代码

- `entry/src/main/cpp/camera/expo_camera.cpp` - `createPreviewOutput()`
- `entry/src/main/cpp/camera/expo_camera.cpp` - `startPreviewInternal()`
- `entry/src/main/ets/pages/TestBasicCamera.ets` - 预览页面

## 历史记录

- 2024-03-23: 初始记录，尝试修改 Profile 选择逻辑无明显改善
