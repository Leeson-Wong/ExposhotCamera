import { display, window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const TAG = 'WindowManager';

// 安全区 key
export const SAFE_AREA_TOP = 'safeAreaTop';
export const SAFE_AREA_BOTTOM = 'safeAreaBottom';

class WindowManager {
  private mainWindow: window.Window | undefined = undefined;

  bindWindowClass(windowStage: window.WindowStage): void {
    try {
      this.mainWindow = windowStage.getMainWindowSync();
      this.init();
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'Failed to get main window: %{public}s', JSON.stringify(err));
    }
  }

  private init(): void {
    if (!this.mainWindow) {
      return;
    }

    // 沉浸式
    this.mainWindow.setWindowLayoutFullScreen(true)
      .then(() => {
        hilog.info(DOMAIN, TAG, 'Window layout set to full-screen mode');
      })
      .catch((err: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'Failed to set full-screen: %{public}s', JSON.stringify(err));
      });

    // 隐藏系统栏
    this.mainWindow.setWindowSystemBarEnable([])
      .then(() => {
        hilog.info(DOMAIN, TAG, 'System bars hidden');
      })
      .catch((err: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'Failed to hide system bars: %{public}s', JSON.stringify(err));
      });

    // 安全区
    try {
      const displayInfo = display.getDefaultDisplaySync();
      const densityPixels = displayInfo.densityPixels;

      const safeAreaTop = this.mainWindow.getWindowAvoidArea(window.AvoidAreaType.TYPE_SYSTEM);
      const safeAreaBottom = this.mainWindow.getWindowAvoidArea(window.AvoidAreaType.TYPE_NAVIGATION_INDICATOR);

      // 转换为 vp 并存入 AppStorage
      const topVp = safeAreaTop.topRect.height / densityPixels;
      const bottomVp = safeAreaBottom.bottomRect.height / densityPixels;

      AppStorage.setOrCreate(SAFE_AREA_TOP, topVp);
      AppStorage.setOrCreate(SAFE_AREA_BOTTOM, bottomVp);

      hilog.info(DOMAIN, TAG, 'Safe area: top=%{public}f vp, bottom=%{public}f vp', topVp, bottomVp);
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'Failed to get safe area: %{public}s', JSON.stringify(err));
    }
  }

  getDisplaySize(): Size {
    try {
      const displayInfo = display.getDefaultDisplaySync();
      return {
        width: displayInfo.width,
        height: displayInfo.height
      };
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'Failed to get display size: %{public}s', JSON.stringify(err));
      return { width: 0, height: 0 };
    }
  }
}

export const windowManager = new WindowManager();
