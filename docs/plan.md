# 重构计划

## 1. C++ 源码目录结构重组

### 背景

`entry/src/main/cpp/` 目录下平铺了 113 个源文件，缺乏模块化组织。

### 目标结构

```
cpp/
├── core/                  # 核心引擎
├── star/                  # 恒星系统
├── solarsystem/           # 太阳系天体
├── projector/             # 投影系统
├── painter/               # 绘制系统
├── geometry/              # 几何计算
├── healpix/               # HEALPix 天区划分
├── math/                  # 数学工具
├── utils/                 # 通用工具
├── culture/               # 星座文化
├── atmosphere/            # 大气折射/消光
├── milkyway/              # 银河系
├── object/                # 天体对象基类
├── napi/                  # NAPI 桥接层
├── recorder/              # 录制功能
├── render/                # 渲染模块 (已有)
├── manager/               # 生命周期管理 (已有)
├── types/                 # 类型定义 (已有)
└── thirdparty/            # 第三方库 (已有)
```

### 文件移动清单

#### 1. core/ - 核心引擎 (5 对)

| 文件 | 说明 |
|------|------|
| StelCore.cpp/h | 天文核心引擎 |
| StelModule.cpp/h | 模块基类 |
| StelObserver.cpp/h | 观察者 |
| StelLocation.cpp/h | 位置信息 |
| StelMovementMgr.cpp/h | 视角移动管理 |
| StelFader.cpp/h | 淡入淡出效果 |

#### 2. star/ - 恒星系统 (6 对)

| 文件 | 说明 |
|------|------|
| Star.cpp/h | 恒星实体 |
| StarMgr.cpp/h | 恒星管理器 |
| StarData.cpp/h | 恒星数据 |
| StarWrapper.cpp/h | 恒星包装器 |
| SpecialZoneArray.cpp/h | 特殊区域数组 |
| HipZoneArray.cpp/h | HIP 星表区域数组 |

#### 3. solarsystem/ - 太阳系天体 (5 对)

| 文件 | 说明 |
|------|------|
| Planet.cpp/h | 行星渲染 |
| SolarSystem.cpp/h | 太阳系管理 |
| Orbit.cpp/h | 轨道计算 |
| RotationElements.cpp/h | 自转元素 |
| Constellation.cpp/hpp | 星座系统 |

#### 4. projector/ - 投影系统 (2 对 + 1 单文件)

| 文件 | 说明 |
|------|------|
| StelProjector.cpp/h | 投影器基类 |
| StelProjectorClasses.cpp/h | 投影器实现 |
| StelProjectorType.h | 投影类型枚举 |

#### 5. painter/ - 绘制系统 (6 对)

| 文件 | 说明 |
|------|------|
| StelPainter.cpp/h | OpenGL 绘制器 |
| StelSkyDrawer.cpp/h | 天空绘制器 |
| StelVertexArray.cpp/h | 顶点数组 |
| StelToneReproducer.cpp/h | 色调复现 |
| StelSRGB.cpp/h | sRGB 转换 |
| Dithering.cpp/h | 抖动处理 |

#### 6. geometry/ - 几何计算 (4 对)

| 文件 | 说明 |
|------|------|
| StelSphereGeometry.cpp/h | 球面几何 |
| OctahedronPolygon.cpp/h | 八面体多边形 |
| StelGeodesicGrid.cpp/h | 测地网格 |
| GridLinesMgr.cpp/h | 网格线管理 |

#### 7. healpix/ - HEALPix 天区划分 (3 对 + 1 单文件)

| 文件 | 说明 |
|------|------|
| HEALPix.cpp/h | HEALPix 算法 |
| HEALPixCatalog.cpp/h | HEALPix 星表 |
| HEALPixHeader.h | HEALPix 头信息 |
| StelHealpix.cpp/h | Stellarium HEALPix 封装 |

#### 8. math/ - 数学工具 (2 对 + 1 单文件)

| 文件 | 说明 |
|------|------|
| VecMath.h | 向量数学 |
| qmatrix4x4.cpp/h | 4x4 矩阵 (Qt 移植) |
| GeomMath.cpp/h | 几何数学 |

#### 9. utils/ - 通用工具 (2 对 + 6 单文件)

| 文件 | 说明 |
|------|------|
| StelUtils.cpp/h | 通用工具函数 |
| StelIniParser.cpp/h | INI 解析器 |
| Endian.h | 字节序处理 |
| SampleLog.h | 日志采样 |
| SampleError.h | 错误采样 |
| StringArg.h | 字符串参数 |
| UniqueIDGenerator.h | 唯一 ID 生成器 |
| STLBase.h | STL 基类 |

#### 10. culture/ - 星座文化 (2 对)

| 文件 | 说明 |
|------|------|
| StelSkyCultureMgr.cpp/h | 星座文化管理 |
| StelSkyCultureSkyPartition.cpp/h | 星空分区 |

#### 11. atmosphere/ - 大气效果 (1 对)

| 文件 | 说明 |
|------|------|
| RefractionExtinction.cpp/h | 大气折射与消光 |

#### 12. milkyway/ - 银河系 (2 对)

| 文件 | 说明 |
|------|------|
| gotoMilkyWay.cpp/h | 导航到银河 |
| MilkyWayCropCalculator.cpp/h | 银河裁剪计算 |

#### 13. object/ - 天体对象基类 (3 对 + 3 单文件)

| 文件 | 说明 |
|------|------|
| StelObject.cpp/h | 天体对象基类 |
| StelObjectMgr.cpp/h | 天体对象管理 |
| StelObjectModule.cpp/h | 天体对象模块 |
| StelObjectType.h | 天体类型 |
| StelRegionObject.h | 区域对象 |
| CoordObject.cpp/h | 坐标对象 |

#### 14. napi/ - NAPI 桥接层 (1 单文件)

| 文件 | 说明 |
|------|------|
| napi_init.cpp | NAPI 入口 |

#### 15. recorder/ - 录制功能 (2 对)

| 文件 | 说明 |
|------|------|
| Recorder.cpp/h | 录制器 |
| SurfaceHelper.cpp/h | Surface 辅助 |

#### 16. thirdparty/ - 第三方库 (1 对 + 2 单文件)

| 文件 | 说明 |
|------|------|
| json.hpp | nlohmann/json |
| QSharedPointer.cpp/h | Qt 智能指针移植 |
| celestial_simple.c/h | 简化天体计算 |

---

### 执行步骤

1. 创建新目录结构
2. 移动文件到对应目录
3. 修改 CMakeLists.txt 更新源文件路径
4. 修改各文件中的 #include 路径
5. 编译验证

### 注意事项

- 需要同步更新所有 `#include` 语句
- CMakeLists.txt 中的源文件列表需要更新路径
- 建议分批次执行，每次移动一个模块后验证编译

---

## 统计

| 目录 | 文件数 |
|------|--------|
| core | 5 对 |
| star | 6 对 |
| solarsystem | 5 对 |
| projector | 2 对 + 1 单文件 |
| painter | 6 对 |
| geometry | 4 对 |
| healpix | 3 对 + 1 单文件 |
| math | 2 对 + 1 单文件 |
| utils | 2 对 + 6 单文件 |
| culture | 2 对 |
| atmosphere | 1 对 |
| milkyway | 2 对 |
| object | 3 对 + 3 单文件 |
| napi | 1 单文件 |
| recorder | 2 对 |
| thirdparty | 1 对 + 2 单文件 |

**总计：113 个文件移动到 16 个子目录**

---

## 2. NDK 相机服务 - Surface 切换方案

### 背景

当前相机控制在 ArkTS 层实现，存在以下问题：
- 相机生命周期与场景紧耦合
- 切换场景需要频繁申请/释放相机
- 无法在多个渲染场景间共享相机流

### 目标

- 相机只初始化一次，应用退出时才释放
- 支持在不同场景间切换 Surface（AR 星图、拍照预览等）
- 流不中断，切换过程对用户无感知

### 架构设计

```
┌──────────────────────────────────────────────────────────────────────┐
│                    NativeCameraService (C++ 单例)                    │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────────┐ │
│  │  相机硬件（生命周期独立于场景）                                   │ │
│  │                                                                  │ │
│  │  Camera_Manager                                                  │ │
│  │       │                                                          │ │
│  │  Camera_Input ──→ Camera_CaptureSession ──→ PreviewOutput        │ │
│  │                          │                                       │ │
│  │                          │ (持续运行，不停止)                      │ │
│  │                          ▼                                       │ │
│  │                   当前活跃的 Surface                              │ │
│  └────────────────────────────────────────────────────────────────┘ │
│                                                                      │
│  核心能力：switchSurface(newSurfaceId)                               │
│  - 销毁旧 PreviewOutput，创建新 PreviewOutput                        │
│  - Session 保持运行（短暂 pause/resume）                              │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
                              │
                              │ 相机流输出到当前 Surface
                              ▼
┌──────────────────────────────────────────────────────────────────────┐
│                         渲染层                                       │
│                                                                      │
│  ┌──────────────┐    切换     ┌──────────────┐                      │
│  │   场景 A      │ ─────────► │   场景 B      │                      │
│  │  AR 星图      │            │   拍照预览    │                      │
│  │ XComponent A │            │ XComponent B │                      │
│  │ SurfaceId A  │            │ SurfaceId B  │                      │
│  └──────────────┘            └──────────────┘                      │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### 依赖

```cmake
# CMakeLists.txt
target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    libnative_buffer.so
    libohcamera.so
    libohimage.so
    libohfileuri.so
)
```

```cpp
// 头文件
#include "ohcamera/camera.h"
#include "ohcamera/camera_input.h"
#include "ohcamera/capture_session.h"
#include "ohcamera/preview_output.h"
#include "ohcamera/photo_output.h"
#include "ohcamera/camera_manager.h"
#include <multimedia/image_framework/image/image_native.h>
```

### 核心接口

```cpp
class NativeCameraService {
public:
    static NativeCameraService& getInstance();

    // ==================== 生命周期 ====================

    /** 初始化相机（只调用一次） */
    Camera_ErrorCode init();

    /** 释放相机（应用退出时调用） */
    Camera_ErrorCode release();

    // ==================== Surface 管理 ====================

    /**
     * 切换预览输出到新的 Surface
     * - 销毁旧 PreviewOutput，创建新 PreviewOutput
     * - Session 短暂 stop/start
     */
    Camera_ErrorCode switchSurface(const std::string& surfaceId);

    /** 开始预览 */
    Camera_ErrorCode startPreview();

    /** 停止预览（暂停，不释放资源） */
    Camera_ErrorCode stopPreview();

    // ==================== 拍照 ====================

    using PhotoCallback = std::function<void(void* buffer, size_t size,
                                              uint32_t width, uint32_t height)>;
    Camera_ErrorCode takePhoto(const PhotoCallback& callback);

    // ==================== 相机参数 ====================

    Camera_ErrorCode setZoomRatio(float ratio);
    Camera_ErrorCode getZoomRatio(float* ratio);
    Camera_ErrorCode setFocusMode(Camera_FocusMode mode);
};
```

### Surface 切换流程

```
[场景 A 运行中]
     │
     │  相机 → PreviewOutput → Surface A → 渲染器 A
     │
     ▼
switchSurface(Surface B)
     │
     ├─ 1. Session.stop()           (短暂停止)
     │
     ├─ 2. Session.beginConfig()
     │
     ├─ 3. Session.removePreviewOutput(旧)
     │      └─ 旧 PreviewOutput.release()
     │
     ├─ 4. createPreviewOutput(Surface B)
     │
     ├─ 5. Session.addPreviewOutput(新)
     │
     ├─ 6. Session.commitConfig()
     │
     └─ 7. Session.start()          (恢复运行)
     │
     ▼
[场景 B 运行中]
     │
     │  相机 → PreviewOutput → Surface B → 渲染器 B
```

### NAPI 接口

```typescript
// libentry.so 导出接口
interface NativeCamera {
    initCamera(): number;
    switchSurface(surfaceId: string): number;
    startPreview(): number;
    stopPreview(): number;
    takePhoto(callback: (arrayBuffer: ArrayBuffer) => void): void;
    releaseCamera(): number;
}
```

### 使用示例（ArkTS）

```typescript
import nativeCamera from 'libentry.so';

// ========== 应用启动 ==========
nativeCamera.initCamera();

// ========== 场景 A: AR 星图 ==========
nativeCamera.switchSurface(starRenderSurfaceId);
nativeCamera.startPreview();
// ... AR 星图渲染运行中 ...

// ========== 切换到场景 B: 拍照 ==========
nativeCamera.switchSurface(photoPreviewSurfaceId);
// startPreview() 可选，switchSurface 内部会恢复

nativeCamera.takePhoto((arrayBuffer: ArrayBuffer) => {
    // 处理照片
});

// ========== 切换回场景 A ==========
nativeCamera.switchSurface(starRenderSurfaceId);

// ========== 应用退出 ==========
nativeCamera.stopPreview();
nativeCamera.releaseCamera();
```

### 文件结构

```
cpp/
├── camera/
│   ├── native_camera_service.h      # 相机服务头文件
│   ├── native_camera_service.cpp    # 相机服务实现
│   └── napi_camera.cpp              # NAPI 接口
```

### 关键实现点

#### 1. 创建 PreviewOutput

```cpp
Camera_ErrorCode NativeCameraService::createPreviewOutput(const std::string& surfaceId) {
    Camera_OutputCapability* outputCapability = nullptr;
    OH_CameraManager_GetSupportedCameraOutputCapability(
        cameraManager_, &cameras_[currentCameraIndex_], &outputCapability);

    const Camera_Profile* previewProfile = outputCapability->previewProfiles[0];

    return OH_CameraManager_CreatePreviewOutput(
        cameraManager_, previewProfile, surfaceId.c_str(), &previewOutput_);
}
```

#### 2. 切换 Surface

```cpp
Camera_ErrorCode NativeCameraService::switchSurface(const std::string& surfaceId) {
    bool wasPreviewing = previewing_;

    // 1. 停止 Session
    if (wasPreviewing) {
        OH_CaptureSession_Stop(captureSession_);
    }

    // 2. 重新配置
    OH_CaptureSession_BeginConfig(captureSession_);

    // 3. 移除旧输出
    if (previewOutput_) {
        OH_CaptureSession_RemovePreviewOutput(captureSession_, previewOutput_);
        OH_PreviewOutput_Release(previewOutput_);
    }

    // 4. 创建新输出
    createPreviewOutput(surfaceId);
    OH_CaptureSession_AddPreviewOutput(captureSession_, previewOutput_);

    // 5. 提交配置
    OH_CaptureSession_CommitConfig(captureSession_);

    // 6. 恢复预览
    if (wasPreviewing) {
        OH_CaptureSession_Start(captureSession_);
        OH_PreviewOutput_Start(previewOutput_);
    }

    activeSurfaceId_ = surfaceId;
    return CAMERA_OK;
}
```

### 注意事项

1. **线程安全**：所有公开方法需要加锁
2. **错误处理**：切换失败时尝试恢复到之前状态
3. **性能**：切换过程应尽量快，减少用户感知
4. **状态管理**：维护 initialized_、previewing_ 等状态标志

### 测试计划

| 测试项 | 预期结果 |
|--------|----------|
| 初始化相机 | 成功，可重复调用不报错 |
| 启动预览 | 预览画面正常显示 |
| 切换 Surface | 画面切换到新 Surface，无崩溃 |
| 快速连续切换 | 无内存泄漏，无崩溃 |
| 拍照 | 返回正确的照片数据 |
| 预览中拍照 | 预览不中断，照片正常 |
| 释放相机 | 资源正确释放 |

---

## 3. RenderSlot 注册机制

### 背景

多渲染模块需要接收相机预览流，但 HarmonyOS 相机一次只能输出到一个 Surface。需要一个注册机制让渲染模块注册自己的 Surface，相机 so 管理切换并通知状态变化。

### 架构

```
┌─────────────────────────────────────────────────────┐
│  上层应用                                            │
│  订阅：camera_so.subscribeState(callback)           │
└─────────────────────────────────────────────────────┘
                        ▲
                        │ 相机整体状态
                        │
┌───────────────────────┴─────────────────────────────┐
│  相机 so (ExpoCamera)                                │
│                                                     │
│  持有：                                              │
│    - List<RenderSlot> slots_                        │
│    - RenderSlot* activeSlot_                        │
│    - StateCallback stateCallback_                   │
│                                                     │
│  方法：                                              │
│    - registerSlot(slot)                             │
│    - unregisterSlot(slotId)                         │
│    - switchToSlot(slotId)                           │
│    - subscribeState(callback)                       │
└───────────────────────┬─────────────────────────────┘
                        │ 预览流归属变化
                        ▼
┌───────────────────────┴─────────────────────────────┐
│  RenderSlot (渲染模块注册的数据结构)                  │
│  {                                                  │
│    id: "preview_main",                              │
│    surfaceId: "xxx",                                │
│    width: 1080,                                     │
│    height: 1920,                                    │
│    userData: void*,                                 │
│    onPreviewChanged: callback                       │
│  }                                                  │
└─────────────────────────────────────────────────────┘
```

### 注册关系

| 注册方 | 被注册方 | 注册什么 | 目的 |
|--------|----------|----------|------|
| 下层渲染模块 | 相机 so | RenderSlot（含 Surface） | 接收预览流和状态通知 |
| 上层应用 | 相机 so | 状态回调函数 | 接收相机整体状态变化 |

### 生命周期（由应用协调）

```
1. 进入页面
     │
     ▼
2. XComponent.onLoad() → 获得 Surface
     │
     ▼
3. 渲染模块初始化，构造 RenderSlot
     │
     ▼
4. nativeCamera.registerSlot(slotId, surfaceId, width, height, callback)
     │
     ▼
5. nativeCamera.switchToSlot(slotId)  ← 切换预览流
     │
     ▼
6. callback(surfaceId, true)  ← 渲染模块收到预览流
     │
     ▼
7. 退出页面
     │
     ▼
8. nativeCamera.unregisterSlot(slotId)
```

### 数据结构

```cpp
// render_slot.h
struct RenderSlot {
    std::string id;           // 唯一标识
    std::string surfaceId;    // Surface ID
    int width = 0;            // 渲染区域宽
    int height = 0;           // 渲染区域高
    void* userData = nullptr; // 渲染模块上下文

    // 预览流状态回调
    // hasPreview: true = 预览流已到达, false = 预览流已离开
    std::function<void(void* userData, const std::string& surfaceId, bool hasPreview)> onPreviewChanged;
};
```

### NAPI 接口

```typescript
// 注册渲染槽
nativeCamera.registerSlot(
    slotId: string,      // 唯一标识
    surfaceId: string,   // XComponent Surface ID
    width: number,       // 渲染区域宽
    height: number,      // 渲染区域高
    callback: (surfaceId: string, hasPreview: boolean) => void  // 预览流变化回调
): number

// 注销渲染槽
nativeCamera.unregisterSlot(slotId: string): number

// 切换预览流到指定槽
nativeCamera.switchToSlot(slotId: string): number

// 订阅相机状态
nativeCamera.subscribeState(callback: (state: string, message: string) => void): number

// 取消订阅
nativeCamera.unsubscribeState(): number
```

### 使用示例

```typescript
import nativeCamera from 'libexpocamera.so';

// 1. 上层应用订阅相机状态
nativeCamera.subscribeState((state, message) => {
    console.log(`Camera state: ${state}, ${message}`);
});

// 2. 页面 A 进入，注册渲染槽
XComponent({
    id: 'previewA',
    type: 'surface',
    libraryname: 'expocamera',
    controller: this.xComponentControllerA
})
.onLoad(() => {
    const surfaceId = this.xComponentControllerA.getXComponentSurfaceId();

    nativeCamera.registerSlot(
        'previewA',
        surfaceId,
        1080,
        1920,
        (surfaceId, hasPreview) => {
            console.log(`Slot A preview: ${hasPreview}`);
        }
    );

    // 切换预览流到此槽
    nativeCamera.switchToSlot('previewA');
})

// 3. 页面 A 退出，注销
aboutToDisappear() {
    nativeCamera.unregisterSlot('previewA');
}
```

### 状态通知

| 状态 | 说明 |
|------|------|
| `slot_registered` | 渲染槽已注册 |
| `slot_unregistered` | 渲染槽已注销 |
| `slot_switched` | 预览流已切换到新槽 |

### 文件结构

```
cpp/camera/
├── expo_camera.h         # 相机控制类头文件
├── expo_camera.cpp       # 相机控制类实现
├── render_slot.h         # RenderSlot 数据结构
└── napi_expo_camera.cpp  # NAPI 绑定层
```

---

## 开发计划

| 阶段 | 内容 | 状态 |
|------|------|------|
| 1 | NDK 相机服务基础框架 | ✅ 完成 |
| 2 | Surface 切换功能 | ✅ 完成 |
| 3 | 拍照功能 | ✅ 完成 |
| 4 | RenderSlot 注册机制 | ✅ 完成 |
| 5 | 集成测试 | 待开发 |
| 6 | C++ 源码目录重构 | 待开发 |
