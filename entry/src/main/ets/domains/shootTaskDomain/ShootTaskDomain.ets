import { ShootTaskEntity } from "./ShootTaskEntity"
import { ShootTaskParam } from "./ShootTaskParam"
import { StateShootTaskStatus } from "./StateShootTaskStatus"

class ShootTaskDomain {
  private runningShootTask: ShootTaskEntity | undefined

  // 开始拍摄任务
  startShootTask(param: ShootTaskParam) {
    if (this.runningShootTask) {
      this.terminalShootTask()
    }
    // 创建新的拍摄任务
    this.runningShootTask = new ShootTaskEntity(param)
    this.runningShootTask.setState(StateShootTaskStatus.COMPOSITION)
  }

  // 获取当前运行的任务
  getCurrentTaskParam(): ShootTaskParam | undefined {
    return this.runningShootTask?.getParam()
  }

  // 执行拍摄
  executeShoot(): string[] {
    return []
  }

  // 执行AI后处理
  executeAIPostProcessing(): string | undefined {
    return ''
  }

  // 导出到相册
  exportToAlbum(): boolean {
    return true
  }

  // 停止拍摄任务
  finishShootTask(finishCallback?: () => void): boolean {
    if (!this.runningShootTask) {
      return false
    }

    // TODO: 实际停止拍摄过程
    console.log("[ShootTaskDomain] 停止拍摄任务")
    this.runningShootTask = undefined
    if (finishCallback) {
      finishCallback()
    }

    return true
  }

  // 强行终止拍摄任务
  terminalShootTask(): boolean {
    return this.finishShootTask()
  }

  setShootTaskShootCounts(count: number) {
    if (!this.runningShootTask) return
    let taskParam = this.runningShootTask.getParam()
    if (!taskParam) return
    taskParam.shootCount && ( taskParam.shootCount = count )
    this.runningShootTask.setParam(taskParam)
  }

  setShootTaskISO(iso: number) {
    if (!this.runningShootTask) return
    let taskParam = this.runningShootTask.getParam()
    if (!taskParam) return
    taskParam.iso && ( taskParam.iso = iso )
    this.runningShootTask.setParam(taskParam)
  }

  setShootTaskAperture(aperture: number) {
    if (!this.runningShootTask) return
    let taskParam = this.runningShootTask.getParam()
    if (!taskParam) return
    taskParam.aperture && ( taskParam.aperture = aperture )
    this.runningShootTask.setParam(taskParam)
  }

  setShootTaskExposureTime(exposureTime: number) {
    if (!this.runningShootTask) return
    let taskParam = this.runningShootTask.getParam()
    if (!taskParam) return
    taskParam.exposureTime && ( taskParam.shootCount = exposureTime )
    this.runningShootTask.setParam(taskParam)
  }

  // 拍摄任务开始回调
  taskStartCallback = () => {
    if (!this.runningShootTask) {
      return
    }
  }

  // 保活
  keepalive = () => {
    if (!this.runningShootTask) {
      return
    }
  }

  // 拍摄任务结束回调
  taskEndCallback = (finalImageUri: string) => {
    if (!this.runningShootTask) {
      return
    }
    return
  }
}

export const shootTaskDomain = new ShootTaskDomain()