import { StateShootTaskStatus } from "./StateShootTaskStatus";
import { ShootTaskParam } from "./ShootTaskParam";

export class ShootTaskEntity {
  private state: StateShootTaskStatus = StateShootTaskStatus.INIT; // 任务状态
  private param: ShootTaskParam | undefined; // 任务参数
  private takenCount: number = 0; // 已拍摄张数
  // private resultImages: string[] = []; // 拍摄结果图片列表
  private stackedImageUri?: string; // 堆栈处理后的图片

  constructor(param?: ShootTaskParam) {
    if (param) {
      this.param = param;
    }
  }

  // 设置任务状态
  public setState(state: StateShootTaskStatus): void {
    this.state = state;
  }

  // 获取任务状态
  public getState(): StateShootTaskStatus {
    return this.state;
  }

  // 设置任务参数
  public setParam(param: ShootTaskParam): void {
    this.param = param;
  }

  // 获取任务参数
  public getParam(): ShootTaskParam | undefined {
    return this.param;
  }

  public setTakenCount(count: number): void {
    this.takenCount = count
  }

  public getTakenCount(): number {
    return this.takenCount
  }

  // 设置最终处理后的图片
  public setStackImage(imagePath: string): void {
    this.stackedImageUri = imagePath;
  }

  // 获取最终处理后的图片
  public getStackedImage(): string | undefined {
    return this.stackedImageUri;
  }
}