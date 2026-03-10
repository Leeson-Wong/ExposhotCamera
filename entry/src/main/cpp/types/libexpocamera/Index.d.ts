/**
 * NDK Camera Service - Surface Switching & Photo Capture & Camera Control
 */

/**
 * 缩放范围
 */
export interface ZoomRange {
    min: number;
    max: number;
}

/**
 * 对焦模式枚举
 */
export const enum FocusMode {
    MANUAL = 0,
    CONTINUOUS_AUTO = 1,
    AUTO = 2,
    LOCKED = 3,
}

/**
 * 初始化相机
 * @returns 0 成功，其他值表示错误码
 */
export const initCamera: () => number;

/**
 * 释放相机资源
 * @returns 0 成功，其他值表示错误码
 */
export const releaseCamera: () => number;

/**
 * 切换预览输出到新的 Surface
 * @param surfaceId 目标 Surface ID
 * @returns 0 成功，其他值表示错误码
 */
export const switchSurface: (surfaceId: string) => number;

/**
 * 启动预览
 * @param surfaceId Surface ID
 * @returns 0 成功，其他值表示错误码
 */
export const startPreview: (surfaceId: string) => number;

/**
 * 停止预览+
 *
 * @returns 0 成功，其他值表示错误码
 */
export const stopPreview: () => number;

/**
 * 拍照
 * @param callback 照片回调函数，参数为照片数据的 ArrayBuffer
 * @returns 0 成功，其他值表示错误码
 */
export const takePhoto: (callback: (arrayBuffer: ArrayBuffer) => void) => number;

/**
 * 检查拍照输出是否就绪
 * @returns true 就绪，false 未就绪
 */
export const isPhotoOutputReady: () => boolean;

/**
 * 设置缩放比例
 * @param ratio 缩放比例
 * @returns 0 成功，其他值表示错误码
 */
export const setZoomRatio: (ratio: number) => number;

/**
 * 获取当前缩放比例
 * @returns 当前缩放比例
 */
export const getZoomRatio: () => number;

/**
 * 获取缩放范围
 * @returns 缩放范围 { min, max }
 */
export const getZoomRatioRange: () => ZoomRange;

/**
 * 检查是否支持缩放
 * @returns true 支持，false 不支持
 */
export const isZoomSupported: () => boolean;

/**
 * 设置对焦模式
 * @param mode 对焦模式 (FocusMode)
 * @returns 0 成功，其他值表示错误码
 */
export const setFocusMode: (mode: FocusMode) => number;

/**
 * 获取当前对焦模式
 * @returns 当前对焦模式
 */
export const getFocusMode: () => FocusMode;

/**
 * 检查是否支持指定对焦模式
 * @param mode 对焦模式
 * @returns true 支持，false 不支持
 */
export const isFocusModeSupported: (mode: FocusMode) => boolean;

/**
 * 对焦距离范围
 */
export interface FocusDistanceRange {
    min: number;
    max: number;
}

/**
 * 设置对焦距离（仅手动对焦模式下有效）
 * @param distance 对焦距离
 * @returns 0 成功，其他值表示错误码
 */
export const setFocusDistance: (distance: number) => number;

/**
 * 获取当前对焦距离
 * @returns 当前对焦距离
 */
export const getFocusDistance: () => number;

/**
 * 获取对焦距离范围
 * @returns 对焦距离范围 { min, max }
 */
export const getFocusDistanceRange: () => FocusDistanceRange;

/**
 * 对焦点坐标（归一化 0-1）
 */
export interface FocusPoint {
    x: number;
    y: number;
}

/**
 * 设置对焦点（归一化坐标 0-1）
 * @param x X 坐标 (0-1)
 * @param y Y 坐标 (0-1)
 * @returns 0 成功，其他值表示错误码
 */
export const setFocusPoint: (x: number, y: number) => number;

/**
 * 获取当前对焦点
 * @returns 对焦点坐标 { x, y }
 */
export const getFocusPoint: () => FocusPoint;

// ==================== 观察者管理 ====================

/**
 * 预览流变化观察者回调
 * 所有观察者都会收到此回调
 * @param activeSlotId 当前获得预览流的 slot ID
 * @param activeSurfaceId 当前获得预览流的 surface ID
 */
export type PreviewObserverCallback = (activeSlotId: string, activeSurfaceId: string) => void;

/**
 * 注册观察者
 * @param surfaceId XComponent Surface ID
 * @param callback 预览流变化回调，所有观察者都会收到通知
 * @returns 生成的 slotId
 */
export const registerObserver: (
    surfaceId: string,
    callback: PreviewObserverCallback
) => string;

/**
 * 注销观察者
 * @param slotId 注册时返回的唯一标识
 * @returns 0 成功，其他值表示错误码
 */
export const unregisterObserver: (slotId: string) => number;

/**
 * 切换预览流到指定 slot
 * @param slotId slot 唯一标识
 * @returns 0 成功，其他值表示错误码
 */
export const switchToSlot: (slotId: string) => number;

// ==================== 状态订阅 ====================

/**
 * 相机状态变化回调
 * @param state 状态名称
 * @param message 状态消息
 */
export type StateChangedCallback = (state: string, message: string) => void;

/**
 * 订阅相机状态
 * @param callback 状态变化回调
 * @returns 0 成功，其他值表示错误码
 */
export const subscribeState: (callback: StateChangedCallback) => number;

/**
 * 取消订阅相机状态
 * @returns 0 成功，其他值表示错误码
 */
export const unsubscribeState: () => number;
