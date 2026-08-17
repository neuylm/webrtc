/**
 * session_manager.cpp — Layer E 实现
 */
#include "session_manager.hpp"

#include <chrono>
#include <stdexcept>

namespace mywebrtc {

namespace {

constexpr int kOfferRetryMs = 1000;
constexpr int kMaxOfferAttempts = 30;

}

SessionManager::SessionManager(std::shared_ptr<AuthPolicy> auth,
                               std::shared_ptr<SignalingChannel> signaling)
    : auth_(std::move(auth)), signaling_(std::move(signaling)) {
    retry_running_.store(true);
    retry_thread_ = std::thread([this] { retryLoop(); });
}

SessionManager::~SessionManager() {
    retry_running_.store(false);
    retry_cv_.notify_all();
    if (retry_thread_.joinable())
        retry_thread_.join();

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
    std::string query;
    size_t mark = url.find('?');
    if (mark != std::string::npos) {
        query = url.substr(mark);
        url.erase(mark);
    }
    if (url.empty() || url.back() != '/') url += '/';
    url += local_id;
    url += query;

    if (!signaling_->connect(url)) return false;

    setupSignalingRouting();
    return true;
}

void SessionManager::retryLoop() {
    while (retry_running_.load()) {
        {
            std::unique_lock<std::mutex> lk(retry_mtx_);
            retry_cv_.wait_for(lk, std::chrono::milliseconds(kOfferRetryMs),
                               [this] { return !retry_running_.load(); });
        }
        if (!retry_running_.load())
            break;

        std::vector<std::shared_ptr<Session>> live;
        {
            std::lock_guard<std::mutex> lk(sessions_mtx_);
            for (auto& [id, s] : sessions_) {
                if (s)
                    live.push_back(s);
            }
        }

        for (auto& s : live) {
            SignalingMessage msg;
            bool resend = false;
            bool giveUp = false;
            {
                std::lock_guard<std::mutex> lk(s->offer_mtx);
                if (!s->offering || s->answered || s->offer_sdp.empty())
                    continue;
                if (s->offer_attempts >= kMaxOfferAttempts) {
                    giveUp = true;
                } else {
                    s->offer_attempts++;
                    msg.peer_id = local_id_;
                    msg.type = "offer";
                    msg.description = s->offer_sdp;
                    resend = true;
                }
            }
            if (giveUp)
                s->sm->fail();
            else if (resend)
                signaling_->send(s->remote_id, msg);
        }
    }
}

void SessionManager::flushCandidates(const std::shared_ptr<Session>& s) {
    std::vector<SignalingMessage> held;
    {
        std::lock_guard<std::mutex> lk(s->offer_mtx);
        if (s->answered)
            return;
        s->answered = true;
        held.swap(s->held_candidates);
    }
    for (auto& msg : held)
        signaling_->send(s->remote_id, msg);
}

void SessionManager::setupSignalingRouting() {
    sig_subs_.push_back(signaling_->onMessage(
        [this](const SignalingMessage& msg) {
            handleSignalingMessage(msg);
        }));
}

void SessionManager::setIceServers(std::vector<std::string> urls) {
    std::lock_guard<std::mutex> lk(ice_mtx_);
    ice_servers_ = std::move(urls);
}

std::shared_ptr<Session> SessionManager::createSession(const std::string& remote_id) {
    rtc::Configuration cfg;
    {
        std::lock_guard<std::mutex> lk(ice_mtx_);
        for (const auto& url : ice_servers_)
            cfg.iceServers.emplace_back(url);
    }

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
        if (type == "offer") {
            std::lock_guard<std::mutex> lk(s->offer_mtx);
            s->offer_sdp = sdp;
            s->offer_attempts = 1;
        }
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
        {
            std::lock_guard<std::mutex> lk(s->offer_mtx);
            if (s->offering && !s->answered) {
                s->held_candidates.push_back(msg);
                return;
            }
        }
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

    {
        std::lock_guard<std::mutex> lk(s->offer_mtx);
        s->offering = true;
    }

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
            flushCandidates(s);
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
