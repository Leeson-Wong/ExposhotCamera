#ifndef FILE_SAVER_H
#define FILE_SAVER_H

#include <string>
#include <cstdint>

namespace exposhot {

/**
 * 文件保存器
 *
 * 职责：
 * - 文件写入：将数据写入文件系统
 * - 路径管理：生成保存路径、创建目录
 * - 平台存储：调用 HarmonyOS 存储 API
 *
 * 不负责：
 * - 图像处理（由 ImageProcessor 负责）
 * - 图像编码（由 ImageProcessor 负责）
 */
class FileSaver {
public:
    FileSaver();
    ~FileSaver();

    // 初始化（设置保存目录等）
    bool init(const std::string& baseDir = "");

    // 保存数据到文件
    // data: 数据缓冲区
    // size: 数据大小
    // filename: 文件名（不含路径）
    // outPath: 输出完整路径（可选）
    // 返回: 是否成功
    bool save(const void* data, size_t size, const std::string& filename, std::string* outPath = nullptr);

    // 保存 JPEG 图像
    // 自动生成文件名：IMG_YYYYMMDD_HHMMSS.jpg
    bool saveJpeg(const void* data, size_t size, std::string* outPath = nullptr);

    // 保存 PNG 图像
    bool savePng(const void* data, size_t size, std::string* outPath = nullptr);

    // 获取保存目录
    std::string getSaveDir() const { return saveDir_; }

    // 检查文件是否存在
    bool fileExists(const std::string& filepath) const;

    // 删除文件
    bool deleteFile(const std::string& filepath);

    // 获取文件大小
    size_t getFileSize(const std::string& filepath) const;

private:
    // 生成时间戳文件名
    std::string generateTimestampFilename(const std::string& extension) const;

    // 确保目录存在
    bool ensureDirectoryExists(const std::string& dir) const;

private:
    std::string saveDir_;
    std::string cacheDir_;
    bool initialized_ = false;
};

} // namespace exposhot

#endif // FILE_SAVER_H
