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

// ==================== 事件系统 ====================

/**
 * 拍照事件类型
 */
export const enum PhotoEventType {
    CAPTURE_START = 0,      // 拍照命令已发送
    CAPTURE_END = 1,        // 拍照成功，原始数据已获取
    CAPTURE_FAILED = 2,     // 拍照失败
}

/**
 * 拍照事件
 */
export interface PhotoEvent {
    type: PhotoEventType;
    sessionId: string;      // 会话 ID，关联同一次拍照的所有事件
    frameIndex?: number;    // 连拍帧索引 (0-based)
    message?: string;
}

/**
 * 处理事件类型
 */
export const enum ProcessEventType {
    PROCESS_START = 0,      // 开始处理
    PROCESS_PROGRESS = 1,   // 处理进度更新
    PROCESS_END = 2,        // 处理完成
    PROCESS_FAILED = 3,     // 处理失败
}

/**
 * 处理事件
 */
export interface ProcessEvent {
    type: ProcessEventType;
    sessionId: string;          // 会话 ID，关联同一次拍照的所有事件
    progress?: number;          // 进度百分比 (0-100)
    currentFrame?: number;      // 当前帧
    totalFrames?: number;       // 总帧数
    message?: string;
}

/**
 * 图像数据（数据就绪回调）
 */
export interface ImageData {
    sessionId: string;          // 会话 ID，关联同一次拍照的所有事件
    buffer?: ArrayBuffer;       // 图像数据（可选，与 filePath 二选一）
    filePath?: string;          // 文件路径（NDK 落盘后）
    width: number;
    height: number;
    frameIndex?: number;        // 连拍帧索引
    isFinal: boolean;           // 是否最终结果
}

/**
 * 拍照事件回调
 */
export type PhotoEventCallback = (event: PhotoEvent) => void;

/**
 * 处理事件回调
 */
export type ProcessEventCallback = (event: ProcessEvent) => void;

/**
 * 图像数据回调
 */
export type ImageDataCallback = (data: ImageData) => void;

/**
 * 注册拍照事件回调
 * @param callback 拍照事件回调
 */
export const registerPhotoEventCallback: (callback: PhotoEventCallback) => void;

/**
 * 注册处理事件回调
 * @param callback 处理事件回调
 */
export const registerProcessEventCallback: (callback: ProcessEventCallback) => void;

/**
 * 注册图像数据回调
 * @param callback 图像数据回调
 */
export const registerImageDataCallback: (callback: ImageDataCallback) => void;

// ==================== 相机控制 ====================

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
 * 拍照事件通过 registerPhotoEventCallback 接收
 * 图像数据通过 registerImageDataCallback 接收
 * @returns sessionId 用于关联同一次拍照的所有事件
 */
export const takePhoto: () => string;

/**
 * 检查拍照输出是否就绪
 * @returns true 就绪，false 未就绪
 */
export const isPhotoOutputReady: () => boolean;

// ==================== 缩放控制 ====================

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

// ==================== 对焦控制 ====================

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

// ==================== 连拍堆叠 ====================

/**
 * 连拍状态枚举
 */
export const enum BurstState {
    IDLE = 0,
    CAPTURING = 1,
    PROCESSING = 2,
    COMPLETED = 3,
    ERROR = 4,
    CANCELLED = 5,
}

/**
 * 连拍配置
 */
export interface BurstConfig {
    frameCount: number;
    exposureMs: number;
    realtimePreview: boolean;
}

/**
 * 连拍进度
 */
export interface BurstProgress {
    sessionId: string;          // 会话 ID，关联同一次连拍的所有事件
    state: BurstState;
    capturedFrames: number;
    processedFrames: number;
    totalFrames: number;
    message: string;
}

/**
 * 连拍进度回调
 * @param progress 进度信息
 */
export type BurstProgressCallback = (progress: BurstProgress) => void;

/**
 * 连拍图像回调
 * @param buffer 图像数据
 * @param isFinal 是否是最终结果
 */
export type BurstImageCallback = (buffer: ArrayBuffer, isFinal: boolean) => void;

/**
 * 开始连拍
 * @param config 连拍配置
 * @param progressCallback 进度回调
 * @param imageCallback 图像回调
 * @returns sessionId 用于关联同一次连拍的所有事件，空字符串表示启动失败
 */
export const startBurstCapture: (
    config: BurstConfig,
    progressCallback: BurstProgressCallback,
    imageCallback: BurstImageCallback
) => string;

/**
 * 取消连拍
 */
export const cancelBurstCapture: () => void;

/**
 * 获取连拍状态
 * @returns 当前连拍状态
 */
export const getBurstState: () => BurstState;

/**
 * 设置连拍图像尺寸
 * @param width 图像宽度
 * @param height 图像高度
 */
export const setBurstImageSize: (width: number, height: number) => void;

// ==================== 文件保存 ====================

/**
 * 保存图像到文件
 * @param buffer 图像数据
 * @param filename 文件名（可选，自动生成时间戳文件名）
 * @returns 保存的文件路径
 */
export const saveImageToFile: (buffer: ArrayBuffer, filename?: string) => string;

/**
 * 获取图片保存目录
 * @returns 保存目录路径
 */
export const getImageSaveDir: () => string;
