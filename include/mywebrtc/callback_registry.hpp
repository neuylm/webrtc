/**
 * callback_registry.hpp — 通用回调注册表
 *
 * 解决 std::function 不可比较导致 std::remove 无法使用的问题。
 * 每个回调分配唯一 ID，取消时按 ID 删除。
 */
#ifndef MYRTC_CALLBACK_REGISTRY_H
#define MYRTC_CALLBACK_REGISTRY_H

#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace mywebrtc {

class CallbackRegistry {
public:
    using Id = uint64_t;

    template <typename... Args>
    Id add(std::function<void(Args...)> cb) {
        std::lock_guard<std::mutex> lk(mtx_);
        Id id = ++counter_;
        // type erasure: store as std::function<void()> with bound args
        // 实际存储由派发方在锁内遍历时处理
        return id;
    }

    static Id nextId() {
        static std::atomic<Id> c{0};
        return ++c;
    }

private:
    std::mutex mtx_;
    Id counter_{0};
};

/*
 * TypedCallbackMap: 类型安全的回调容器
 * 存储回调函数 + 唯一 ID，支持按 ID 取消订阅。
 */
template <typename Signature>
class TypedCallbackMap;

template <typename... Args>
class TypedCallbackMap<void(Args...)> {
public:
    using Cb = std::function<void(Args...)>;
    using Id  = uint64_t;

    Id add(Cb cb) {
        std::lock_guard<std::mutex> lk(mtx_);
        Id id = ++counter_;
        cbs_[id] = std::move(cb);
        return id;
    }

    bool remove(Id id) {
        std::lock_guard<std::mutex> lk(mtx_);
        return cbs_.erase(id) > 0;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        cbs_.clear();
    }

    /* 在锁内拷贝一份回调列表，在锁外派发（避免回调中再次操作导致死锁） */
    std::vector<Cb> snapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<Cb> out;
        out.reserve(cbs_.size());
        for (const auto& [id, cb] : cbs_) out.push_back(cb);
        return out;
    }

private:
    mutable std::mutex mtx_;
    std::atomic<Id> counter_{0};
    std::unordered_map<Id, Cb> cbs_;
};

} /* namespace mywebrtc */

#endif /* MYRTC_CALLBACK_REGISTRY_H */
