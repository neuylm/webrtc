/**
 * session_manager.cpp — Layer E 实现
 */
#include "session_manager.hpp"

#include <stdexcept>

namespace mywebrtc {

SessionManager::SessionManager(std::shared_ptr<AuthPolicy> auth,
                               std::shared_ptr<SignalingChannel> signaling)
    : auth_(std::move(auth)), signaling_(std::move(signaling)) {}

SessionManager::~SessionManager() {
    std::lock_guard<std::mutex> lk(sessions_mtx_);
    for (auto& [id, s] : sessions_) {
        if (s && s->pc) s->pc->close();
        if (s && s->sm) s->sm->transition(SessionState::Closed);
    }
    sessions_.clear();
}

bool SessionManager::init(const std::string& signaling_url, const std::string& local_id,
                          const std::string& auth_token) {
    local_id_   = local_id;
    auth_token_ = auth_token;

    // 信令服务器以 URL path 作为客户端 ID，如 ws://host:port/alice
    std::string url = signaling_url;
    if (url.back() != '/') url += '/';
    url += local_id;

    if (!signaling_->connect(url)) return false;

    setupSignalingRouting();
    return true;
}

void SessionManager::setupSignalingRouting() {
    sig_subs_.push_back(signaling_->onMessage(
        [this](const SignalingMessage& msg) {
            handleSignalingMessage(msg);
        }));
}

std::shared_ptr<Session> SessionManager::createSession(const std::string& remote_id) {
    rtc::Configuration cfg;
    cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");

    auto s = std::make_shared<Session>();
    s->local_id = local_id_;
    s->remote_id = remote_id;
    s->pc = PeerConnection::create(cfg);
    s->sm = std::make_shared<SessionStateMachine>();

    s->subs.push_back(s->pc->onStateChange([self = weak_from_this(), s](PeerConnection::State state) {
        auto sp = self.lock();
        if (!sp) return;
        switch (state) {
        case PeerConnection::State::Connecting:
            s->sm->transition(SessionState::Connecting); break;
        case PeerConnection::State::Connected:
            s->sm->transition(SessionState::Connected);
            s->sm->transition(SessionState::Streaming); break;
        case PeerConnection::State::Disconnected:
        case PeerConnection::State::Failed:
            s->sm->fail(); break;
        case PeerConnection::State::Closed:
            s->sm->transition(SessionState::Closed); break;
        default: break;
        }
        auto cbs = sp->state_cbs_.snapshot();
        for (auto& cb : cbs) cb(s->remote_id, s->sm->state());
    }));

    s->subs.push_back(s->pc->onLocalDescription([self = weak_from_this(), s](const std::string& sdp,
                                                            const std::string& type) {
        auto sp = self.lock();
        if (!sp) return;
        SignalingMessage msg;
        msg.peer_id = sp->local_id_;
        msg.type = type;
        msg.description = sdp;
        sp->signaling_->send(s->remote_id, msg);
    }));
    s->subs.push_back(s->pc->onLocalCandidate([self = weak_from_this(), s](const std::string& cand,
                                                          const std::string& mid) {
        auto sp = self.lock();
        if (!sp) return;
        SignalingMessage msg;
        msg.peer_id = sp->local_id_;
        msg.type = "candidate";
        msg.candidate = cand;
        msg.mid = mid;
        sp->signaling_->send(s->remote_id, msg);
    }));

    s->subs.push_back(s->pc->onDataChannel([self = weak_from_this(), s](std::shared_ptr<DataChannel> dc) {
        auto sp = self.lock();
        if (!sp) return;
        s->dc = dc;
        auto cbs = sp->dc_cbs_.snapshot();
        for (auto& cb : cbs) cb(s->remote_id, dc);
    }));

    std::lock_guard<std::mutex> lk(sessions_mtx_);
    sessions_[remote_id] = s;
    return s;
}

std::shared_ptr<Session> SessionManager::offer(const std::string& remote_id) {
    if (!auth_->authenticate(auth_token_, local_id_)) {
        return nullptr;
    }

    auto s = createSession(remote_id);
    if (!s) return nullptr;

    if (!s->sm->transition(SessionState::Offering)) return nullptr;

    // createDataChannel 触发自动协商（setLocalDescription Offer）
    s->dc = s->pc->createDataChannel("default");

    return s;
}

std::shared_ptr<Session> SessionManager::answer(const std::string& remote_id,
                                                const std::string& remote_sdp) {
    if (!auth_->authenticate(auth_token_, local_id_)) return nullptr;

    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        auto it = sessions_.find(remote_id);
        if (it != sessions_.end()) s = it->second;
    }
    if (!s) {
        s = createSession(remote_id);
        if (!s) return nullptr;
    }

    if (s->sm->state() == SessionState::Idle) {
        s->sm->transition(SessionState::Answering);
    }
    try {
        s->pc->setRemoteDescription(remote_sdp, "offer").get();
        // setRemoteDescription(offer) 自动触发 setLocalDescription(Answer)
    } catch (const std::exception&) {
        return nullptr;
    }

    return s;
}

void SessionManager::handleSignalingMessage(const SignalingMessage& msg) {
    if (msg.type == "offer") {
        answer(msg.peer_id, msg.description.value_or(""));
    } else if (msg.type == "answer") {
        std::shared_ptr<Session> s;
        {
            std::lock_guard<std::mutex> lk(sessions_mtx_);
            auto it = sessions_.find(msg.peer_id);
            if (it != sessions_.end()) s = it->second;
        }
        if (s) {
            s->sm->transition(SessionState::Connecting);
            s->pc->setRemoteDescription(msg.description.value_or(""), "answer");
        }
    } else if (msg.type == "candidate") {
        std::shared_ptr<Session> s;
        {
            std::lock_guard<std::mutex> lk(sessions_mtx_);
            auto it = sessions_.find(msg.peer_id);
            if (it != sessions_.end()) s = it->second;
        }
        if (s) {
            s->pc->addRemoteCandidate(msg.candidate.value_or(""),
                                      msg.mid.value_or(""));
        }
    }
}

bool SessionManager::closeSession(const std::string& remote_id) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lk(sessions_mtx_);
        auto it = sessions_.find(remote_id);
        if (it == sessions_.end()) return false;
        s = it->second;
        sessions_.erase(it);
    }
    if (s) {
        s->sm->transition(SessionState::Closing);
        s->pc->close();
        s->sm->transition(SessionState::Closed);
    }
    return true;
}

std::shared_ptr<Session> SessionManager::getSession(const std::string& remote_id) const {
    std::lock_guard<std::mutex> lk(sessions_mtx_);
    auto it = sessions_.find(remote_id);
    return it != sessions_.end() ? it->second : nullptr;
}

Subscription SessionManager::onSessionStateChange(
        std::function<void(const std::string&, SessionState)> cb) {
    auto id = state_cbs_.add(std::move(cb));
    auto* reg = &state_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

Subscription SessionManager::onIncomingDataChannel(
        std::function<void(const std::string&, std::shared_ptr<DataChannel>)> cb) {
    auto id = dc_cbs_.add(std::move(cb));
    auto* reg = &dc_cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

} /* namespace mywebrtc */
