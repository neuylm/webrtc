/**
 * my-client — 使用 mywebrtc 新接口的示例客户端
 *
 * 用法:
 *   发起方: my-client alice bob ws://localhost:8000
 *   应答方: my-client bob alice ws://localhost:8000
 *
 * 规则: local_id < remote_id (字典序) 的一方做 offer，另一方做 answer（等待）。
 * 这样两边同时启动也能正确配对。
 */
#include "mywebrtc/session_manager.hpp"
#include "mywebrtc/auth.hpp"
#include "mywebrtc/signaling_channel.hpp"
#include "mywebrtc/peerconnection.hpp"
#include "mywebrtc/datachannel.hpp"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace mywebrtc;
using namespace std::chrono_literals;

int main(int argc, char* argv[]) {
    rtc::InitLogger(rtc::LogLevel::Warning);

    std::string signaling_url = "ws://localhost:8000";
    std::string local_id = "alice";
    std::string remote_id = "bob";
    std::string token = "secret-token-42";

    if (argc > 1) local_id = argv[1];
    if (argc > 2) remote_id = argv[2];
    if (argc > 3) signaling_url = argv[3];

    // 字典序小的一方做 caller
    bool is_caller = local_id < remote_id;

    std::cout << "Local ID: " << local_id
              << " | Remote ID: " << remote_id
              << " | Role: " << (is_caller ? "CALLER (offer)" : "CALLEE (answer)")
              << "\n";
    std::cout << "Signaling: " << signaling_url << "\n";

    /* 1. 创建鉴权策略 + 信令通道 + 会话管理器 */
    auto auth = std::make_shared<DefaultAuthPolicy>(token);
    auto signaling = std::make_shared<WebSocketSignalingChannel>();
    auto sm = std::make_shared<SessionManager>(auth, signaling);

    /* 2. 初始化 */
    if (!sm->init(signaling_url, local_id, token)) {
        std::cerr << "Failed to connect signaling server\n";
        return -1;
    }
    std::cout << "Signaling connected\n";

    /* 3. 监听对端 DataChannel（应答方路径） */
    std::atomic<bool> dc_ready{false};
    std::shared_ptr<Session> active_sess;
    std::vector<Subscription> subs;  // 保持回调订阅存活

    subs.push_back(sm->onIncomingDataChannel([&](const std::string& peer_id, std::shared_ptr<DataChannel> dc) {
        std::cout << "DataChannel from " << peer_id << " label=" << dc->label() << "\n";
        subs.push_back(dc->onMessage(
            [](rtc::binary b) {
                std::cout << "Recv binary size=" << b.size() << "\n";
            },
            [](std::string text) {
                std::cout << "Recv: " << text << "\n";
            }));
        dc->send("Hello from " + local_id);
        active_sess = sm->getSession(peer_id);
        dc_ready = true;
    }));

    /* 4. 监听会话状态变化 */
    subs.push_back(sm->onSessionStateChange([](const std::string& peer, SessionState s) {
        std::cout << "Session[" << peer << "] state: " << to_string(s) << "\n";
    }));

    /* 5. 发起或等待 */
    if (is_caller) {
        std::cout << "Offering to " << remote_id << "...\n";
        active_sess = sm->offer(remote_id);
        if (!active_sess) {
            std::cerr << "Offer failed (auth or state machine)\n";
            return -1;
        }
    } else {
        std::cout << "Waiting for incoming offer from " << remote_id << "...\n";
    }

    /* 6. 等 DataChannel 打开 */
    std::cout << "Waiting for DataChannel to open...\n";
    if (active_sess && active_sess->dc) {
        bool opened = false;
        auto sub = active_sess->dc->onOpen([&] {
            opened = true;
            std::cout << "DataChannel open!\n";
        });
        // 发起方也设置消息接收
        subs.push_back(active_sess->dc->onMessage(
            [](rtc::binary b) { std::cout << "Recv binary size=" << b.size() << "\n"; },
            [](std::string text) { std::cout << "Recv: " << text << "\n"; }));
        for (int i = 0; i < 100 && !opened; ++i) {
            std::this_thread::sleep_for(100ms);
        }
        if (!opened) {
            std::cerr << "DataChannel did not open within 10s, exiting.\n";
            sm->closeSession(remote_id);
            return -1;
        }
    } else {
        // 应答方：等 onIncomingDataChannel 回调
        for (int i = 0; i < 100 && !dc_ready; ++i) {
            std::this_thread::sleep_for(100ms);
        }
        if (!dc_ready) {
            std::cerr << "No DataChannel within 10s, exiting.\n";
            sm->closeSession(remote_id);
            return -1;
        }
    }

    /* 7. 主循环 */
    std::cout << "Enter message to send (q to quit):\n";
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "q") break;
        if (active_sess && active_sess->dc && active_sess->dc->isOpen()) {
            active_sess->dc->send(line);
        } else {
            std::cout << "DataChannel not open, message dropped.\n";
        }
    }

    sm->closeSession(remote_id);
    return 0;
}
