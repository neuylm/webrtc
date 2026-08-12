/**
 * eventbus.hpp — Layer E: 事件总线
 *
 * 提供发布/订阅模型，用于解耦业务模块（SessionManager/AuthPolicy/
 * SignalingChannel）与底层 PeerConnection。
 *
 * 线程安全。回调在事件发生的线程上执行。
 */
#ifndef MYRTC_EVENTBUS_H
#define MYRTC_EVENTBUS_H

#include <any>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "async.hpp"

namespace mywebrtc {

class EventBus {
public:
    using EventId = std::size_t;

    /* 订阅事件，返回可取消的 Subscription */
    template <typename EventType>
    Subscription subscribe(std::function<void(const EventType&)> cb) {
        auto id = nextId();
        auto key = typeId<EventType>();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            subs_[key].push_back({id, [cb = std::move(cb)](const std::any& a) {
                try {
                    cb(std::any_cast<const EventType&>(a));
                } catch (...) {}
            }});
        }
        return Subscription([this, id, key]() mutable {
            std::lock_guard<std::mutex> lk(mtx_);
            if (auto it = subs_.find(key); it != subs_.end()) {
                auto& vec = it->second;
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                    [id](const Entry& e) { return e.id == id; }),
                    vec.end());
            }
        });
    }

    /* 发布事件（同步派发） */
    template <typename EventType>
    void publish(const EventType& ev) {
        std::vector<std::function<void(const std::any&)>> cbs;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = subs_.find(typeId<EventType>());
            if (it != subs_.end()) {
                for (auto& e : it->second) cbs.push_back(e.cb);
            }
        }
        for (auto& cb : cbs) cb(std::any(ev));
    }

    static EventBus& instance() {
        static EventBus s;
        return s;
    }

private:
    struct Entry { EventId id; std::function<void(const std::any&)> cb; };

    template <typename T>
    static std::size_t typeId() {
        static const char id = 0;
        return reinterpret_cast<std::size_t>(&id);
    }

    EventId nextId() { return ++counter_; }

    std::mutex mtx_;
    std::unordered_map<std::size_t, std::vector<Entry>> subs_;
    std::atomic<EventId> counter_{0};
};

} /* namespace mywebrtc */

#endif /* MYRTC_EVENTBUS_H */
