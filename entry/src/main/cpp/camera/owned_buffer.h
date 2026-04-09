#ifndef OWNED_BUFFER_H
#define OWNED_BUFFER_H

#include <cstdlib>
#include <cstring>

namespace exposhot {

// 独占所有权的 buffer 封装，移动语义传递所有权，禁止拷贝
// 析构时自动 free，消除手动内存管理错误
class OwnedBuffer {
public:
    OwnedBuffer() = default;
    OwnedBuffer(void* data, size_t size) : data_(data), size_(size) {}
    ~OwnedBuffer() { free(); }

    // 移动构造
    OwnedBuffer(OwnedBuffer&& o) noexcept : data_(o.data_), size_(o.size_) {
        o.data_ = nullptr;
        o.size_ = 0;
    }

    // 移动赋值
    OwnedBuffer& operator=(OwnedBuffer&& o) noexcept {
        if (this != &o) {
            free();
            data_ = o.data_;
            size_ = o.size_;
            o.data_ = nullptr;
            o.size_ = 0;
        }
        return *this;
    }

    // 禁止拷贝
    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;

    // 从 raw pointer 接管所有权（调用者放弃所有权）
    static OwnedBuffer takeOver(void* data, size_t size) {
        return OwnedBuffer(data, size);
    }

    // 从数据复制一份（原数据仍由调用者管理）
    static OwnedBuffer copyFrom(const void* data, size_t size) {
        if (!data || size == 0) return OwnedBuffer();
        void* copy = malloc(size);
        if (copy) {
            memcpy(copy, data, size);
        }
        return OwnedBuffer(copy, size);
    }

    void* data() const { return data_; }
    size_t size() const { return size_; }
    explicit operator bool() const { return data_ != nullptr; }
    bool empty() const { return data_ == nullptr; }

    // 释放所有权（返回 raw pointer，调用者接管管理责任）
    void* release() {
        void* d = data_;
        data_ = nullptr;
        size_ = 0;
        return d;
    }

private:
    void free() {
        if (data_) {
            ::free(data_);
            data_ = nullptr;
            size_ = 0;
        }
    }

    void* data_ = nullptr;
    size_t size_ = 0;
};

} // namespace exposhot

#endif // OWNED_BUFFER_H
