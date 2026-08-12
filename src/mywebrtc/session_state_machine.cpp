/**
 * session_state_machine.cpp — Layer E 实现
 */
#include "session_state_machine.hpp"

namespace mywebrtc {

std::string to_string(SessionState s) {
    switch (s) {
    case SessionState::Idle:       return "Idle";
    case SessionState::Offering:   return "Offering";
    case SessionState::Answering:  return "Answering";
    case SessionState::Connecting: return "Connecting";
    case SessionState::Connected:  return "Connected";
    case SessionState::Streaming:  return "Streaming";
    case SessionState::Pausing:    return "Pausing";
    case SessionState::Closing:    return "Closing";
    case SessionState::Closed:     return "Closed";
    case SessionState::Failed:     return "Failed";
    }
    return "Unknown";
}

SessionStateMachine::SessionStateMachine()
    : state_{SessionState::Idle} {}

SessionStateMachine::~SessionStateMachine() = default;

SessionState SessionStateMachine::state() const {
    return state_.load(std::memory_order_acquire);
}

bool SessionStateMachine::isValidTransition(SessionState from, SessionState to) {
    if (from == SessionState::Closed || from == SessionState::Failed)
        return false;

    switch (from) {
    case SessionState::Idle:
        return to == SessionState::Offering || to == SessionState::Answering ||
               to == SessionState::Closed;
    case SessionState::Offering:
    case SessionState::Answering:
        return to == SessionState::Connecting || to == SessionState::Failed;
    case SessionState::Connecting:
        return to == SessionState::Connected || to == SessionState::Failed;
    case SessionState::Connected:
        return to == SessionState::Streaming || to == SessionState::Pausing ||
               to == SessionState::Closing || to == SessionState::Failed;
    case SessionState::Streaming:
    case SessionState::Pausing:
        return to == SessionState::Streaming || to == SessionState::Pausing ||
               to == SessionState::Closing || to == SessionState::Failed;
    case SessionState::Closing:
        return to == SessionState::Closed || to == SessionState::Failed;
    default:
        return false;
    }
}

bool SessionStateMachine::transition(SessionState next) {
    SessionState cur = state_.load(std::memory_order_acquire);
    if (!isValidTransition(cur, next)) return false;
    state_.store(next, std::memory_order_release);

    auto cbs = cbs_.snapshot();
    for (auto& cb : cbs) cb(cur, next);
    return true;
}

bool SessionStateMachine::fail() {
    return transition(SessionState::Failed);
}

Subscription SessionStateMachine::onTransition(
        std::function<void(SessionState, SessionState)> cb) {
    auto id = cbs_.add(std::move(cb));
    auto* reg = &cbs_;
    return Subscription([reg, id]{ reg->remove(id); });
}

} /* namespace mywebrtc */
