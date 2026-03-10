#ifndef RENDER_SLOT_H
#define RENDER_SLOT_H

#include <string>

// RenderSlot - 渲染槽信息
// 只存储基本信息，不包含回调
// 回调由观察者模式统一管理
struct RenderSlot {
    std::string id;           // 唯一标识
    std::string surfaceId;    // Surface ID
    int width = 0;            // 渲染区域宽
    int height = 0;           // 渲染区域高
};

#endif // RENDER_SLOT_H
