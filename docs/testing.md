# 测试规划文档

> ⚠️ **重要说明**：本文档为测试规划草案，所有方案均**未落地实现**，**未经过实际验证**。具体实现时可能需要根据实际情况调整。文档中的代码示例仅供参考。

## 测试策略概览

本项目采用分层测试策略，针对不同层次的代码使用不同的测试方法：

| 层次 | 测试方法 | 工具 | 运行环境 |
|------|----------|------|----------|
| NDK 纯算法层 | 本地单元测试 | Google Test | 开发机 |
| NDK HarmonyOS 依赖层 | Hypium Native 测试 | Hypium + GTest | 设备/模拟器 |
| NAPI 接口层 | 集成测试 | ArkTS + Hypium | 设备/模拟器 |
| UI 层 | UI 测试 | Hypium UI | 设备/模拟器 |

---

## 1. NDK 单元测试

### 1.1 测试目录结构

```
entry/
├── src/
│   ├── main/cpp/                    # 原有代码
│   │   ├── camera/
│   │   │   ├── image_processor.h
│   │   │   ├── image_processor.cpp
│   │   │   └── ...
│   │   └── tests/                   # 本地测试（纯算法）
│   │       ├── CMakeLists.txt
│   │       ├── image_processor_test.cpp
│   │       └── mock/
│   │           └── mock_hilog.h
│   └── ohosTest/
│       └── cpp/                     # Hypium Native 测试
│           ├── CMakeLists.txt
│           └── capture_manager_test.cpp
```

### 1.2 本地单元测试（方案 2 - 推荐）

将不依赖 HarmonyOS API 的纯算法代码分离，使用 Google Test 在本地测试。

#### 适用模块

| 模块 | 文件 | 可测试内容 | 备注 |
|------|------|-----------|------|
| ImageProcessor | `image_processor.cpp` | `dngToBGRA16()` DNG 解析 | 纯算法，可本地测试 |
| ImageProcessor | `image_processor.cpp` | 图像处理算法 | 需要分离纯 C++ 实现 |
| MotionStack | 第三方库 | 运动分析算法 | 需 mock GPU 调用 |

#### 本地测试 CMakeLists.txt

```cmake
# entry/src/main/cpp/tests/CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(exposhot_unit_tests)
set(CMAKE_CXX_STANDARD 17)

# 定义单元测试宏
add_definitions(-DUNIT_TEST)

# GTest
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# 项目源码
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../camera
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libraw/include
)

# Mock 文件
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/mock)

# 测试可执行文件
add_executable(run_tests
    image_processor_test.cpp
    ../camera/image_processor.cpp
)

target_link_libraries(run_tests
    ${GTEST_LIBRARIES}
    pthread
)

# CTest 集成
enable_testing()
add_test(NAME ImageProcessorTests COMMAND run_tests)
```

#### Mock HarmonyOS API

```cpp
// entry/src/main/cpp/tests/mock/mock_hilog.h
#pragma once

#define LOG_APP 0
#define OH_LOG_INFO(...) ((void)0)
#define OH_LOG_ERROR(...) ((void)0)
#define OH_LOG_WARN(...) ((void)0)
```

#### 测试用例示例

```cpp
// entry/src/main/cpp/tests/image_processor_test.cpp
#include <gtest/gtest.h>
#include <fstream>
#include <vector>
#include "image_processor.h"

class ImageProcessorTest : public testing::Test {
protected:
    void SetUp() override {
        processor = std::make_unique<exposhot::ImageProcessor>();
    }

    void TearDown() override {
        processor.reset();
    }

    std::unique_ptr<exposhot::ImageProcessor> processor;

    // 辅助函数：读取测试 DNG 文件
    std::vector<uint8_t> loadTestDng(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary);
        return std::vector<uint8_t>(
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()
        );
    }
};

// 测试 DNG 转 BGRA16
TEST_F(ImageProcessorTest, DngToBGRA16_ValidInput_ReturnsCorrectDimensions) {
    // Arrange
    auto dngData = loadTestDng("test_data/sample.dng");
    ASSERT_FALSE(dngData.empty()) << "测试文件加载失败";

    // Act
    exposhot::Bgra16Raw result = processor->dngToBGRA16(
        dngData.data(),
        dngData.size()
    );

    // Assert
    EXPECT_GT(result.width, 0);
    EXPECT_GT(result.height, 0);
    EXPECT_NE(result.data, nullptr);
}

TEST_F(ImageProcessorTest, DngToBGRA16_EmptyInput_HandlesGracefully) {
    // Act & Assert
    EXPECT_NO_THROW({
        exposhot::Bgra16Raw result = processor->dngToBGRA16(nullptr, 0);
    });
}

// 测试运动分析
TEST_F(ImageProcessorTest, MotionAnalysis_IdenticalImages_ZeroMotion) {
    // 需要准备两张相同图像的测试数据
    // ...
}
```

### 1.3 Hypium Native 测试（方案 1）

针对依赖 HarmonyOS API 的代码，使用 Hypium 测试框架在设备上运行。

#### 适用模块

| 模块 | 文件 | 测试内容 |
|------|------|----------|
| CaptureManager | `capture_manager.cpp` | 相机捕获流程 |
| ExpoCamera | `expo_camera.cpp` | 相机初始化/释放 |
| ImageProcessor GPU | `image_processor.cpp` | GPU 加速处理 |

#### 测试 CMakeLists.txt

```cmake
# entry/src/ohosTest/cpp/CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(exposhot_hypium_tests)
set(CMAKE_CXX_STANDARD 17)

include_directories(${OHOS_SDK_NATIVE}/include/gtest)
include_directories(${OHOS_SDK_NATIVE}/include/hypium)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main/cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main/cpp/camera
)

add_library(exposhot_test SHARED
    capture_manager_test.cpp
)

target_link_libraries(exposhot_test PUBLIC
    libhilog_ndk.z.so
    libohcamera.so
    ${OHOS_SDK_NATIVE}/lib/${OHOS_ARCH}/libgtest.so
    ${OHOS_SDK_NATIVE}/lib/${OHOS_ARCH}/libgtest_main.so
    ${OHOS_SDK_NATIVE}/lib/${OHOS_ARCH}/libhypium.so
)
```

#### 测试用例示例

```cpp
// entry/src/ohosTest/cpp/capture_manager_test.cpp
#include <gtest/gtest.h>
#include <hilog/log.h>
#include "capture_manager.h"

using namespace testing::ext;

class CaptureManagerTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}
    void SetUp() override {}
    void TearDown() override {}
};

HWTEST_F(CaptureManagerTest, Init_ValidState_Success, TestSize.Level1)
{
    exposhot::CaptureManager& mgr = exposhot::CaptureManager::getInstance();
    int result = mgr.init();
    EXPECT_EQ(result, 0);
}

HWTEST_F(CaptureManagerTest, Capture_WithoutInit_Fails, TestSize.Level1)
{
    exposhot::CaptureManager& mgr = exposhot::CaptureManager::getInstance();
    // 期望未初始化时捕获失败
    // ...
}
```

---

## 2. NAPI 接口层测试（集成测试）

### 2.1 测试策略

NAPI 接口是 NDK 和 ArkTS 的桥梁，应在 ArkTS 层进行集成测试。

### 2.2 测试用例

```typescript
// entry/src/ohosTest/ets/test/NapiInterface.test.ets
import { describe, it, expect } from '@ohos/hypium';
import nativeCamera from 'libexpocamera.so';

export default function napiInterfaceTest() {
  describe('NAPI Interface Tests', () => {
    it('initCamera_shouldReturnZero', () => {
      const result = nativeCamera.initCamera();
      expect(result).assertEqual(0);
    });

    it('initCamera_withResourceManager_shouldSucceed', () => {
      const resMgr = getContext(this).resourceManager;
      const result = nativeCamera.initCamera(resMgr);
      expect(result).assertEqual(0);
    });

    it('startPreview_withValidSurfaceId_shouldSucceed', () => {
      // 需要先获取有效的 surfaceId
      const result = nativeCamera.startPreview('valid_surface_id');
      expect(result).assertEqual(0);
    });

    it('mockStackProcess_withoutResourceManager_shouldFail', () => {
      nativeCamera.initCamera(); // 不传 resourceManager
      const result = nativeCamera.mockStackProcess('surface_id', 0);
      expect(result.success).assertFalse();
    });

    it('mockStackProcess_withResourceManager_shouldSucceed', () => {
      const resMgr = getContext(this).resourceManager;
      nativeCamera.initCamera(resMgr);
      const result = nativeCamera.mockStackProcess('surface_id', 0);
      expect(result.success).assertTrue();
    });
  });
}
```

---

## 3. UI 层测试

### 3.1 测试页面

| 测试页面 | 文件 | 测试重点 |
|---------|------|----------|
| TestBasicCamera | `TestBasicCamera.ets` | 基础预览、拍照、缩放、对焦 |
| TestBurstCapture | `TestBurstCapture.ets` | 连拍、堆叠、进度追踪、延迟拍摄 |
| TestCaptureMode | `TestCaptureMode.ets` | 模式切换、PhotoOutput 重配置、单拍/连拍切换 |
| TestFullFeatures | `TestFullFeatures.ets` | Slot 切换、双预览、完整相机控制 |
| TestFocusPoint | `TestFocusPoint.ets` | 对焦功能、对焦轨迹、预设位置 |
| TestFocusMagnifier | `TestFocusMagnifier.ets` | 对焦放大镜、触摸对焦、实时预览 |
| TestStackSimulate | `TestStackSimulate.ets` | 堆叠模拟、rawfile 读取、进度显示 |

### 3.2 自动化 UI 测试

```typescript
// entry/src/ohosTest/ets/test/StackSimulateUITest.test.ets
import { Driver, Component } from '@ohos.uitest';

export default function stackSimulateUITest() {
  describe('Stack Simulate UI Tests', () => {
    let driver: Driver;

    beforeAll(async () => {
      driver = Driver.create();
    });

    it('startStackButton_shouldBeDisabled_withoutSurfaceId', async () => {
      // 启动页面
      await driver.pressBack();
      // 验证按钮状态
      const startBtn = await driver.findComponent(ON.text('开始堆叠'));
      expect(await startBtn.isEnabled()).assertFalse();
    });

    it('stackProgress_shouldUpdate_duringStacking', async () => {
      // 测试进度更新
    });
  });
}
```

---

## 4. 测试数据管理

### 4.1 测试 DNG 文件

```
entry/src/main/cpp/tests/test_data/
├── sample_001.dng       # 正常 DNG 文件
├── sample_002.dng       # 不同曝光参数
├── corrupt.dng          # 损坏文件（测试错误处理）
└── metadata.json        # 测试文件元数据
```

### 4.2 测试数据生成

可使用脚本生成测试用的 DNG 文件：

```python
# scripts/generate_test_dng.py
import rawpy
from PIL import Image
import numpy as np

def create_test_dng(width, height, pattern='gradient'):
    """生成测试用 DNG 文件"""
    # 实现略
    pass
```

---

## 5. 持续集成

### 5.1 本地测试命令

```bash
# 运行本地单元测试
cd entry/src/main/cpp/tests
mkdir build && cd build
cmake .. && make
./run_tests

# 运行 Hypium 测试（需要连接设备）
hdc shell aa test -p com.exposhot.camera -m TestAbility
```

### 5.2 测试覆盖率

使用 `gcov` 和 `lcov` 生成覆盖率报告：

```bash
# 编译时添加覆盖率标志
cmake -DCMAKE_CXX_FLAGS="--coverage" ..

# 运行测试后生成报告
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_report
```

---

## 6. 测试优先级

### 高优先级（必须测试）

- [ ] `ImageProcessor::dngToBGRA16()` - 核心图像处理
- [ ] `CaptureManager` 捕获流程 - 拍照核心
- [ ] NAPI `initCamera` / `releaseCamera` - 生命周期管理
- [ ] NAPI `mockStackProcess` - rawfile 读取

### 中优先级

- [ ] `ImageProcessor::MotionAnalysisAndStack()` - 堆叠算法
- [ ] NAPI 对焦相关接口
- [ ] NAPI 缩放相关接口

### 低优先级

- [ ] UI 自动化测试
- [ ] 性能测试
- [ ] 压力测试

---

## 7. 待实现

> ⚠️ 以下所有测试任务均未开始，本文档仅作为规划参考

- [ ] 创建 `entry/src/main/cpp/tests/` 目录结构
- [ ] 创建 `entry/src/ohosTest/cpp/` 目录结构
- [ ] 实现第一个本地单元测试（ImageProcessor）
- [ ] 实现第一个 Hypium Native 测试
- [ ] 集成到 CI/CD 流程

---

## 8. 文档状态

| 项目 | 状态 |
|------|------|
| 文档状态 | 📋 规划中 |
| 实现状态 | ❌ 未开始 |
| 验证状态 | ❌ 未验证 |
| 最后更新 | 2026-03-24 |

**备注**：本文档中的所有 CMake 配置、测试代码、目录结构均为规划方案，尚未在实际项目中验证。在实施前需要：
1. 确认 HarmonyOS SDK 版本对应的 GTest/Hypium 支持情况
2. 验证 CMake 配置是否与项目构建系统兼容
3. 实际测试目录结构和代码示例的正确性

---

## 9. 测试页面汇总

> 以下测试页面均已实现，可用于手动测试各功能模块

| 页面 | 初始化模式 | 主要功能 |
|------|------------|----------|
| TestBasicCamera | `CaptureMode.SINGLE` | 预览、拍照、缩放、对焦 |
| TestBurstCapture | `CaptureMode.BURST` | 连拍堆叠、进度追踪、延迟拍摄 |
| TestCaptureMode | 动态切换 | 模式切换、单拍/连拍测试 |
| TestFullFeatures | `CaptureMode.SINGLE` | Slot 切换、双预览、完整控制 |
| TestFocusPoint | `CaptureMode.SINGLE` | 对焦功能、对焦轨迹 |
| TestFocusMagnifier | `CaptureMode.SINGLE` | 对焦放大镜、触摸对焦 |
| TestStackSimulate | `CaptureMode.SINGLE` | 堆叠模拟、rawfile 读取 |
