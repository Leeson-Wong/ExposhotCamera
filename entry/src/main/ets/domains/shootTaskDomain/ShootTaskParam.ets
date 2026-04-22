

export interface ShootTaskParam {
  iso: number; // ISO值
  aperture: number; // 光圈值
  exposureTime: number; // 曝光时间（秒）
  shootCount: number; // 拍摄张数
  shootMode: string; // 拍摄模式（normal/star trail等）
}
