#include "file_saver.h"
#include "hilog/log.h"

#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <fstream>
#include <iomanip>
#include <sstream>

// HarmonyOS 文件系统 API
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "FileSaver"

namespace exposhot {

FileSaver::FileSaver() {}

FileSaver::~FileSaver() {}

FileSaver& FileSaver::getInstance() {
    static FileSaver instance;
    return instance;
}

bool FileSaver::init(const std::string& baseDir) {
    if (baseDir.empty()) {
        // 使用默认保存目录
        // HarmonyOS 应用沙箱路径
        saveDir_ = "/data/service/el2/user/0/com.exposhot.camera/files/photos";
    } else {
        saveDir_ = baseDir;
    }

    // 确保目录存在
    if (!ensureDirectoryExists(saveDir_)) {
        OH_LOG_ERROR(LOG_APP, "Failed to create save directory: %{public}s", saveDir_.c_str());
        return false;
    }

    cacheDir_ = "/data/service/el2/user/0/com.exposhot.camera/cache";

    initialized_ = true;
    OH_LOG_INFO(LOG_APP, "FileSaver initialized, saveDir: %{public}s", saveDir_.c_str());
    return true;
}

bool FileSaver::save(const void* data, size_t size, const std::string& filename, std::string* outPath) {
    if (!initialized_) {
        OH_LOG_ERROR(LOG_APP, "FileSaver not initialized");
        return false;
    }

    if (!data || size == 0) {
        OH_LOG_ERROR(LOG_APP, "Invalid data parameters");
        return false;
    }

    std::string filepath = saveDir_ + "/" + filename;

    // 打开文件
    std::ofstream file(filepath, std::ios::binary | std::ios::out);
    if (!file.is_open()) {
        OH_LOG_ERROR(LOG_APP, "Failed to open file for writing: %{public}s", filepath.c_str());
        return false;
    }

    // 写入数据
    file.write(static_cast<const char*>(data), size);
    file.close();

    if (file.fail()) {
        OH_LOG_ERROR(LOG_APP, "Failed to write data to file: %{public}s", filepath.c_str());
        return false;
    }

    if (outPath) {
        *outPath = filepath;
    }

    OH_LOG_INFO(LOG_APP, "File saved: %{public}s, size: %{public}zu bytes", filepath.c_str(), size);
    return true;
}

bool FileSaver::saveJpeg(const void* data, size_t size, std::string* outPath) {
    std::string filename = generateTimestampFilename("jpg");
    return save(data, size, filename, outPath);
}

bool FileSaver::savePng(const void* data, size_t size, std::string* outPath) {
    std::string filename = generateTimestampFilename("png");
    return save(data, size, filename, outPath);
}

bool FileSaver::fileExists(const std::string& filepath) const {
    struct stat buffer;
    return (stat(filepath.c_str(), &buffer) == 0);
}

bool FileSaver::deleteFile(const std::string& filepath) {
    if (unlink(filepath.c_str()) == 0) {
        OH_LOG_INFO(LOG_APP, "File deleted: %{public}s", filepath.c_str());
        return true;
    } else {
        OH_LOG_ERROR(LOG_APP, "Failed to delete file: %{public}s", filepath.c_str());
        return false;
    }
}

size_t FileSaver::getFileSize(const std::string& filepath) const {
    struct stat buffer;
    if (stat(filepath.c_str(), &buffer) == 0) {
        return static_cast<size_t>(buffer.st_size);
    }
    return 0;
}

std::string FileSaver::generateTimestampFilename(const std::string& extension) const {
    auto now = std::time(nullptr);
    auto tm = std::localtime(&now);

    std::ostringstream oss;
    oss << "IMG_"
        << std::put_time(tm, "%Y%m%d_%H%M%S")
        << "." << extension;

    return oss.str();
}

bool FileSaver::ensureDirectoryExists(const std::string& dir) const {
    // 检查目录是否存在
    struct stat buffer;
    if (stat(dir.c_str(), &buffer) == 0 && S_ISDIR(buffer.st_mode)) {
        return true;
    }

    // 递归创建目录
    size_t pos = 0;
    do {
        pos = dir.find('/', pos + 1);
        std::string subdir = dir.substr(0, pos);

        struct stat st;
        if (stat(subdir.c_str(), &st) != 0) {
            // 目录不存在，创建
            if (mkdir(subdir.c_str(), 0755) != 0 && errno != EEXIST) {
                OH_LOG_ERROR(LOG_APP, "Failed to create directory: %{public}s, errno: %{public}d",
                             subdir.c_str(), errno);
                return false;
            }
        }
    } while (pos != std::string::npos);

    return true;
}

} // namespace exposhot
