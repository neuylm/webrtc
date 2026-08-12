/**
 * handle_table.hpp — Layer A 内部：线程安全句柄表
 *
 * 不透明指针 → shared_ptr<T> 映射。
 * 生命周期：句柄销毁时自动从表中移除。
 */
#ifndef MYRTC_HANDLE_TABLE_H
#define MYRTC_HANDLE_TABLE_H

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace mywebrtc {

template <typename HandleTag, typename T>
class HandleTable {
public:
    using key_type = std::uintptr_t;

    /* 注册对象，返回不透明句柄 */
    void* register_obj(std::shared_ptr<T> p) {
        std::lock_guard<std::mutex> lk(mtx_);
        /* 使用对象指针作为唯一 key（稳定且唯一） */
        auto raw = reinterpret_cast<std::uintptr_t>(p.get());
        table_[raw] = std::move(p);
        return reinterpret_cast<void*>(raw);
    }

    /* 查找对象 */
    std::shared_ptr<T> lookup(void* h) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = table_.find(reinterpret_cast<std::uintptr_t>(h));
        return it != table_.end() ? it->second : nullptr;
    }

    /* 注销并释放 */
    bool unregister(void* h) {
        std::lock_guard<std::mutex> lk(mtx_);
        return table_.erase(reinterpret_cast<std::uintptr_t>(h)) > 0;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        table_.clear();
    }

private:
    mutable std::mutex mtx_;
    std::unordered_map<key_type, std::shared_ptr<T>> table_;
};

} /* namespace mywebrtc */

#endif /* MYRTC_HANDLE_TABLE_H */
