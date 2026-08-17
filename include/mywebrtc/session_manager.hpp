/**
 * session_manager.hpp — Layer E: 会话管理器
 */
#ifndef MYRTC_SESSION_MANAGER_H
#define MYRTC_SESSION_MANAGER_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "async.hpp"
#include "callback_registry.hpp"
#include "auth.hpp"
#include "datachannel.hpp"
#include "peerconnection.hpp"
#include "session_state_machine.hpp"
#include "signaling_channel.hpp"

namespace mywebrtc {

struct Session {
    std::string local_id;
    std::string remote_id;
    std::shared_ptr<PeerConnection> pc;
    std::shared_ptr<SessionStateMachine> sm;
    std::shared_ptr<DataChannel> dc;
    std::vector<Subscription> subs;  // 保持回调订阅存活

    std::mutex offer_mtx;
    std::string offer_sdp;
    std::vector<SignalingMessage> held_candidates;
    int offer_attempts = 0;
    bool offering = false;
    bool answered = false;
};

class SessionManager : public std::enable_shared_from_this<SessionManager> {
public:
    SessionManager(std::shared_ptr<AuthPolicy> auth,
                   std::shared_ptr<SignalingChannel> signaling);
    ~SessionManager();

    bool init(const std::string& signaling_url, const std::string& local_id,
              const std::string& auth_token);

    std::shared_ptr<Session> offer(const std::string& remote_id);
    std::shared_ptr<Session> answer(const std::string& remote_id,
                                    const std::string& remote_sdp);
    void handleSignalingMessage(const SignalingMessage& msg);
    bool closeSession(const std::string& remote_id);
    std::shared_ptr<Session> getSession(const std::string& remote_id) const;

    Subscription onSessionStateChange(std::function<void(const std::string&,
                                                         SessionState)>);
    Subscription onIncomingDataChannel(std::function<void(const std::string&,
                                                            std::shared_ptr<DataChannel>)>);

private:
    std::shared_ptr<Session> createSession(const std::string& remote_id);
    void setupSignalingRouting();
    void retryLoop();
    void flushCandidates(const std::shared_ptr<Session>& s);

    std::shared_ptr<AuthPolicy> auth_;
    std::shared_ptr<SignalingChannel> signaling_;
    std::string local_id_;
    std::string auth_token_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    mutable std::mutex sessions_mtx_;

    TypedCallbackMap<void(const std::string&, SessionState)> state_cbs_;
    TypedCallbackMap<void(const std::string&, std::shared_ptr<DataChannel>)> dc_cbs_;
    std::vector<Subscription> sig_subs_;

    std::thread retry_thread_;
    std::mutex retry_mtx_;
    std::condition_variable retry_cv_;
    std::atomic<bool> retry_running_{false};
};

} /* namespace mywebrtc */

#endif /* MYRTC_SESSION_MANAGER_H */
