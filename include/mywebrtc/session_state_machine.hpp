/**
 * session_state_machine.hpp — Layer E: 会话状态机
 */
#ifndef MYRTC_SESSION_STATE_MACHINE_H
#define MYRTC_SESSION_STATE_MACHINE_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "async.hpp"
#include "callback_registry.hpp"

namespace mywebrtc {

enum class SessionState {
    Idle,
    Offering,
    Answering,
    Connecting,
    Connected,
    Streaming,
    Pausing,
    Closing,
    Closed,
    Failed
};

std::string to_string(SessionState s);

class SessionStateMachine {
public:
    SessionStateMachine();
    ~SessionStateMachine();

    SessionState state() const;
    bool transition(SessionState next);
    bool fail();

    Subscription onTransition(std::function<void(SessionState from, SessionState to)>);

private:
    static bool isValidTransition(SessionState from, SessionState to);

    std::atomic<SessionState> state_{SessionState::Idle};
    TypedCallbackMap<void(SessionState, SessionState)> cbs_;
};

} /* namespace mywebrtc */

#endif /* MYRTC_SESSION_STATE_MACHINE_H */
