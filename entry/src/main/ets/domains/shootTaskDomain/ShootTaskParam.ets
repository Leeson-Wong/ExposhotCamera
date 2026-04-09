import { RA, DEC } from "../../common/Types";

export interface ShootTaskParam {
  x: number; // 目标星空水平角度 azimuth
  y: number; // 目标星空高度角 altitude
  iso: number; // ISO值
  aperture: number; // 光圈值
  exposureTime: number; // 曝光时间（秒）
  shootCount: number; // 拍摄张数
  shootMode: string; // 拍摄模式（normal/star trail等）
  // 推荐星座的信息，不单独声明结构体，信息较少直接平铺
  title: string; // 推荐星座名字
  RA_: RA; // 推荐星座经度
  DEC_: DEC; // 推荐星座纬度
}

export { RA, DEC };
