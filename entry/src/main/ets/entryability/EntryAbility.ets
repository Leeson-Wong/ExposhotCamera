import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import PermissionManager from '../util/PermissionManager';
import CameraConstant from '../util/Constants';
import { windowManager } from '../util/WindowManager';

// ============ 测试模式开关 ============
// true: 进入测试页面列表
// false: 显示正式版首页
const ENABLE_TEST_PAGES = true;
// =====================================

const DOMAIN = 0x0000;
const CAMERA_PERMISSION: Permissions = 'ohos.permission.CAMERA';

export default class EntryAbility extends UIAbility {
  async requestCameraPermission(): Promise<void> {

    await PermissionManager.request(CameraConstant.PERMISSIONS, this.context)
      .catch(() => {
      });
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'Failed to set colorMode. Cause: %{public}s', JSON.stringify(err));
    }
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // Main window is created, set main page for this ability
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');

    // Request camera permission
    this.requestCameraPermission();

    // Set immersive mode and safe area
    windowManager.bindWindowClass(windowStage);

    // 根据编译时常量决定加载哪个首页
    const startPage = ENABLE_TEST_PAGES ? 'pages/IndexTest' : 'pages/Index';
    hilog.info(DOMAIN, 'testTag', 'Loading page: %{public}s, ENABLE_TEST_PAGES=%{public}s', startPage, String(ENABLE_TEST_PAGES));

    windowStage.loadContent(startPage, (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'testTag', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, 'testTag', 'Succeeded in loading the content.');
    });
  }

  onWindowStageDestroy(): void {
    // Main window is destroyed, release UI related resources
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    // Ability has brought to foreground
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onForeground');
  }

  onBackground(): void {
    // Ability has back to background
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
  }
}
