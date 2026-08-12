/**
 * async.hpp — Layer B: 异步工具
 *
 * 提供 Subscription（订阅句柄，可取消）和 Promise/Future 封装。
 * 用于把原 rtc::PeerConnection 的裸 callback 改造为 future 风格异步 API。
 */
#ifndef MYRTC_ASYNC_H
#define MYRTC_ASYNC_H

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>

namespace mywebrtc {

/* 订阅句柄：析构时自动取消订阅（使用 shared_ptr 实现可移动） */
class Subscription {
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> unsub)
        : unsub_(std::make_shared<std::function<void()>>(std::move(unsub))),
          flag_(std::make_shared<std::once_flag>()) {}
    ~Subscription() { unsubscribe(); }

    Subscription(Subscription&&) noexcept = default;
    Subscription& operator=(Subscription&&) noexcept = default;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    void unsubscribe() {
        if (!flag_ || !unsub_) return;
        std::call_once(*flag_, [&]{
            if (*unsub_) { (*unsub_)(); *unsub_ = nullptr; }
        });
    }
private:
    std::shared_ptr<std::function<void()>> unsub_;
    std::shared_ptr<std::once_flag>        flag_;
};

/* 简单的 Promise<T>：包装 std::promise，允许 set_exception 便捷化 */
template <typename T>
class Promise {
public:
    std::future<T> get_future() { return promise_.get_future(); }
    void set_value(T v) { promise_.set_value(std::move(v)); }
    void set_exception(std::exception_ptr e) { promise_.set_exception(e); }
    void set_error(const std::string& msg) {
        set_exception(std::make_exception_ptr(std::runtime_error(msg)));
    }
private:
    std::promise<T> promise_;
};

} /* namespace mywebrtc */

#endif /* MYRTC_ASYNC_H */
