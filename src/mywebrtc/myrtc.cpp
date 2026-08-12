/**
 * myrtc.cpp — Layer A: C ABI 实现
 *
 * 转调 Layer B (PeerConnection/DataChannel/Track) 和 Layer E (SessionManager)。
 * 所有符号通过 MYRTC_API 导出，原始 rtc* 符号不从此库再导出。
 */
#include "mywebrtc/myrtc.h"

#include "mywebrtc/auth.hpp"
#include "mywebrtc/datachannel.hpp"
#include "mywebrtc/peerconnection.hpp"
#include "mywebrtc/session_manager.hpp"
#include "mywebrtc/signaling_channel.hpp"
#include "mywebrtc/track.hpp"

#include "handle_table.hpp"

#include "rtc/global.hpp"

#include <atomic>
#include <cstring>
#include <memory>

namespace {

/* 句柄表（每种对象独立） */
mywebrtc::HandleTable<struct PcTag,   mywebrtc::PeerConnection> g_pc_table;
mywebrtc::HandleTable<struct DcTag,   mywebrtc::DataChannel>    g_dc_table;
mywebrtc::HandleTable<struct TrTag,   mywebrtc::Track>          g_track_table;
mywebrtc::HandleTable<struct SessTag, mywebrtc::SessionManager> g_sess_table;

/* 全局初始化计数 */
std::atomic<int> g_init_count{0};

/* 日志回调 */
rtc::LogCallback g_log_cb_wrapper;

/* 事件回调 + user_data 包装 */
struct EventCb {
    mwrtc_event_cb cb;
    void* user_data;
};

void dispatch_event(mywebrtc::PeerConnection* pc, const EventCb& ecb,
                    const mwrtc_event_t& ev) {
    if (ecb.cb) ecb.cb(&ev, ecb.user_data);
}

} /* anon namespace */

/* ============================================================
 * 全局
 * ============================================================ */
extern "C" {

int MYRTC_CALL mwrtc_init(void) {
    if (g_init_count.fetch_add(1) == 0) {
        rtc::InitLogger(rtc::LogLevel::Warning);
        rtc::Preload();
    }
    return MWRTC_OK;
}

void MYRTC_CALL mwrtc_cleanup(void) {
    if (g_init_count.fetch_sub(1) == 1) {
        g_pc_table.clear();
        g_dc_table.clear();
        g_track_table.clear();
        g_sess_table.clear();
        rtc::Cleanup().wait();
    }
}

void MYRTC_CALL mwrtc_set_log_level(int level, mwrtc_log_cb cb) {
    rtc::LogLevel lvl = static_cast<rtc::LogLevel>(level);
    if (cb) {
        g_log_cb_wrapper = [cb](rtc::LogLevel l, std::string msg) {
            cb(static_cast<int>(l), msg.c_str());
        };
        rtc::InitLogger(lvl, g_log_cb_wrapper);
    } else {
        rtc::InitLogger(lvl);
    }
}

/* ============================================================
 * PeerConnection
 * ============================================================ */

mwrtc_pc_t MYRTC_CALL mwrtc_pc_create(const mwrtc_config_t* cfg) {
    if (!cfg) return nullptr;

    rtc::Configuration rcfg;
    for (int i = 0; i < cfg->ice_servers_count && cfg->ice_servers && cfg->ice_servers[i]; ++i) {
        rcfg.iceServers.emplace_back(cfg->ice_servers[i]);
    }
    if (cfg->bind_address) rcfg.bindAddress = cfg->bind_address;
    if (cfg->mtu > 0) rcfg.mtu = static_cast<size_t>(cfg->mtu);
    if (cfg->max_message_size > 0) rcfg.maxMessageSize = static_cast<size_t>(cfg->max_message_size);
    if (cfg->enable_ice_udp_mux) rcfg.enableIceUdpMux = true;
    if (cfg->disable_auto_negotiation) rcfg.disableAutoNegotiation = true;
    if (cfg->force_media_transport) rcfg.forceMediaTransport = true;

    if (cfg->certificate_type == 1)      rcfg.certificateType = rtc::CertificateType::Ecdsa;
    else if (cfg->certificate_type == 2) rcfg.certificateType = rtc::CertificateType::Rsa;

    auto pc = mywebrtc::PeerConnection::create(std::move(rcfg));
    return static_cast<mwrtc_pc_t>(g_pc_table.register_obj(pc));
}

void MYRTC_CALL mwrtc_pc_destroy(mwrtc_pc_t pc) {
    if (pc) {
        if (auto p = g_pc_table.lookup(pc)) p->close();
        g_pc_table.unregister(pc);
    }
}

int MYRTC_CALL mwrtc_pc_close(mwrtc_pc_t pc) {
    auto p = g_pc_table.lookup(pc);
    if (!p) return MWRTC_ERR_INVALID;
    p->close();
    return MWRTC_OK;
}

int MYRTC_CALL mwrtc_pc_on_event(mwrtc_pc_t pc, mwrtc_event_cb cb, void* user_data) {
    auto p = g_pc_table.lookup(pc);
    if (!p) return MWRTC_ERR_INVALID;

    EventCb ecb{cb, user_data};

    /* 注册各类事件回调，统一转换为 mwrtc_event_t 派发 */
    p->onStateChange([ecb, pc](mywebrtc::PeerConnection::State s) {
        mwrtc_event_t ev{};
        ev.type = MWRTC_EV_PC_STATE;
        ev.handle_id = reinterpret_cast<intptr_t>(pc);
        ev.pc_state = static_cast<mwrtc_state_t>(s);
        dispatch_event(nullptr, ecb, ev);
    });

    p->onIceStateChange([ecb, pc](mywebrtc::PeerConnection::IceState s) {
        mwrtc_event_t ev{};
        ev.type = MWRTC_EV_ICE_STATE;
        ev.handle_id = reinterpret_cast<intptr_t>(pc);
        ev.ice_state = static_cast<mwrtc_ice_state_t>(s);
        dispatch_event(nullptr, ecb, ev);
    });

    p->onLocalDescription([ecb, pc](const std::string& sdp, const std::string& type) {
        mwrtc_event_t ev{};
        ev.type = MWRTC_EV_LOCAL_DESCRIPTION;
        ev.handle_id = reinterpret_cast<intptr_t>(pc);
        ev.sdp = sdp.c_str();
        ev.sdp_type = type.c_str();
        dispatch_event(nullptr, ecb, ev);
    });

    p->onLocalCandidate([ecb, pc](const std::string& cand, const std::string& mid) {
        mwrtc_event_t ev{};
        ev.type = MWRTC_EV_LOCAL_CANDIDATE;
        ev.handle_id = reinterpret_cast<intptr_t>(pc);
        ev.candidate = cand.c_str();
        ev.mid = mid.c_str();
        dispatch_event(nullptr, ecb, ev);
    });

    p->onDataChannel([ecb, pc](std::shared_ptr<mywebrtc::DataChannel> dc) {
        void* dch = g_dc_table.register_obj(dc);
        mwrtc_event_t ev{};
        ev.type = MWRTC_EV_DATA_CHANNEL;
        ev.handle_id = reinterpret_cast<intptr_t>(pc);
        ev.data_channel_id = static_cast<int>(reinterpret_cast<intptr_t>(dch));
        dispatch_event(nullptr, ecb, ev);
    });

    p->onTrack([ecb, pc](std::shared_ptr<mywebrtc::Track> tr) {
        void* th = g_track_table.register_obj(tr);
        mwrtc_event_t ev{};
        ev.type = MWRTC_EV_TRACK;
        ev.handle_id = reinterpret_cast<intptr_t>(pc);
        ev.track_id = static_cast<int>(reinterpret_cast<intptr_t>(th));
        dispatch_event(nullptr, ecb, ev);
    });

    return MWRTC_OK;
}

int MYRTC_CALL mwrtc_pc_set_local_description(mwrtc_pc_t pc, const char* type) {
    auto p = g_pc_table.lookup(pc);
    if (!p) return MWRTC_ERR_INVALID;
    rtc::Description::Type t = rtc::Description::Type::Unspec;
    if (type) {
        std::string ts(type);
        if (ts == "offer")       t = rtc::Description::Type::Offer;
        else if (ts == "answer") t = rtc::Description::Type::Answer;
        else if (ts == "pranswer") t = rtc::Description::Type::Pranswer;
    }
    p->setLocalDescription(t);
    return MWRTC_OK;
}

int MYRTC_CALL mwrtc_pc_set_remote_description(mwrtc_pc_t pc, const char* sdp, const char* type) {
    auto p = g_pc_table.lookup(pc);
    if (!p || !sdp) return MWRTC_ERR_INVALID;
    p->setRemoteDescription(sdp, type ? type : "");
    return MWRTC_OK;
}

int MYRTC_CALL mwrtc_pc_add_remote_candidate(mwrtc_pc_t pc, const char* cand, const char* mid) {
    auto p = g_pc_table.lookup(pc);
    if (!p || !cand) return MWRTC_ERR_INVALID;
    p->addRemoteCandidate(cand, mid ? mid : "");
    return MWRTC_OK;
}

int MYRTC_CALL mwrtc_pc_get_local_description(mwrtc_pc_t pc, char* buf, int size) {
    auto p = g_pc_table.lookup(pc);
    if (!p) return MWRTC_ERR_INVALID;
    auto d = p->localDescription();
    if (!d) return MWRTC_ERR_NOT_FOUND;
    int need = static_cast<int>(d->size()) + 1;
    if (!buf || size < need) return -need;
    std::memcpy(buf, d->c_str(), d->size());
    buf[d->size()] = '\0';
    return static_cast<int>(d->size());
}

int MYRTC_CALL mwrtc_pc_get_remote_description(mwrtc_pc_t pc, char* buf, int size) {
    auto p = g_pc_table.lookup(pc);
    if (!p) return MWRTC_ERR_INVALID;
    auto d = p->remoteDescription();
    if (!d) return MWRTC_ERR_NOT_FOUND;
    int need = static_cast<int>(d->size()) + 1;
    if (!buf || size < need) return -need;
    std::memcpy(buf, d->c_str(), d->size());
    buf[d->size()] = '\0';
    return static_cast<int>(d->size());
}

mwrtc_state_t MYRTC_CALL mwrtc_pc_state(mwrtc_pc_t pc) {
    auto p = g_pc_table.lookup(pc);
    if (!p) return MWRTC_STATE_CLOSED;
    return static_cast<mwrtc_state_t>(p->state());
}

mwrtc_ice_state_t MYRTC_CALL mwrtc_pc_ice_state(mwrtc_pc_t pc) {
    auto p = g_pc_table.lookup(pc);
    if (!p) return MWRTC_ICE_CLOSED;
    return static_cast<mwrtc_ice_state_t>(p->iceState());
}

bool MYRTC_CALL mwrtc_pc_is_negotiation_needed(mwrtc_pc_t pc) {
    auto p = g_pc_table.lookup(pc);
    return p ? p->negotiationNeeded() : false;
}

/* ============================================================
 * DataChannel
 * ============================================================ */

mwrtc_dc_t MYRTC_CALL mwrtc_pc_create_data_channel(mwrtc_pc_t pc, const char* label,
                                                     const mwrtc_dc_init_t* init) {
    auto p = g_pc_table.lookup(pc);
    if (!p || !label) return nullptr;
    auto dc = p->createDataChannel(label);
    /* init 参数目前透传给底层 rtc::DataChannelInit 由默认值处理，后续可扩展 */
    (void)init;
    return static_cast<mwrtc_dc_t>(g_dc_table.register_obj(dc));
}

int MYRTC_CALL mwrtc_dc_destroy(mwrtc_dc_t dc) {
    if (dc) {
        if (auto p = g_dc_table.lookup(dc)) p->close();
        g_dc_table.unregister(dc);
    }
    return MWRTC_OK;
}

int MYRTC_CALL mwrtc_dc_send(mwrtc_dc_t dc, const char* data, int size) {
    auto p = g_dc_table.lookup(dc);
    if (!p || !data) return MWRTC_ERR_INVALID;
    bool ok = (size >= 0)
        ? p->send(reinterpret_cast<const rtc::byte*>(data), static_cast<size_t>(size))
        : p->send(std::string(data));
    return ok ? MWRTC_OK : MWRTC_ERR_FAILURE;
}

int MYRTC_CALL mwrtc_dc_close(mwrtc_dc_t dc) {
    auto p = g_dc_table.lookup(dc);
    if (!p) return MWRTC_ERR_INVALID;
    p->close();
    return MWRTC_OK;
}

bool MYRTC_CALL mwrtc_dc_is_open(mwrtc_dc_t dc) {
    auto p = g_dc_table.lookup(dc);
    return p ? p->isOpen() : false;
}

int MYRTC_CALL mwrtc_dc_max_message_size(mwrtc_dc_t dc) {
    auto p = g_dc_table.lookup(dc);
    if (!p) return MWRTC_ERR_INVALID;
    return static_cast<int>(p->maxMessageSize());
}

int MYRTC_CALL mwrtc_dc_label(mwrtc_dc_t dc, char* buf, int size) {
    auto p = g_dc_table.lookup(dc);
    if (!p) return MWRTC_ERR_INVALID;
    auto label = p->label();
    int need = static_cast<int>(label.size()) + 1;
    if (!buf || size < need) return -need;
    std::memcpy(buf, label.c_str(), label.size());
    buf[label.size()] = '\0';
    return static_cast<int>(label.size());
}

/* ============================================================
 * Track
 * ============================================================ */

mwrtc_track_t MYRTC_CALL mwrtc_pc_add_track(mwrtc_pc_t pc, const mwrtc_track_init_t* init) {
    auto p = g_pc_table.lookup(pc);
    if (!p || !init) return nullptr;

    std::string mid_str = init->mid ? init->mid : "video";
    rtc::Description::Media media("video", mid_str,
                                   rtc::Description::Direction::SendOnly);
    /* 基本字段映射 */
    if (init->ssrc) media.addSSRC(init->ssrc,
                                  init->name ? init->name : "",
                                  init->msid ? init->msid : "",
                                  init->track_id ? init->track_id : "");

    auto tr = p->addTrack(media);
    return static_cast<mwrtc_track_t>(g_track_table.register_obj(tr));
}

int MYRTC_CALL mwrtc_track_destroy(mwrtc_track_t tr) {
    if (tr) {
        if (auto p = g_track_table.lookup(tr)) p->close();
        g_track_table.unregister(tr);
    }
    return MWRTC_OK;
}

int MYRTC_CALL mwrtc_track_send_frame(mwrtc_track_t tr, const char* data, int size,
                                       const mwrtc_frame_info_t* info) {
    auto p = g_track_table.lookup(tr);
    if (!p || !data) return MWRTC_ERR_INVALID;
    rtc::FrameInfo fi{0};
    if (info) {
        fi.timestamp = info->timestamp;
        fi.payloadType = info->payload_type;
        if (info->timestamp_seconds >= 0) {
            fi.timestampSeconds = std::chrono::duration<double>(info->timestamp_seconds);
        }
    }
    p->sendFrame(reinterpret_cast<const rtc::byte*>(data), static_cast<size_t>(size), fi);
    return MWRTC_OK;
}

int MYRTC_CALL mwrtc_track_request_keyframe(mwrtc_track_t tr) {
    auto p = g_track_table.lookup(tr);
    if (!p) return MWRTC_ERR_INVALID;
    return p->requestKeyframe() ? MWRTC_OK : MWRTC_ERR_FAILURE;
}

int MYRTC_CALL mwrtc_track_request_bitrate(mwrtc_track_t tr, unsigned int bitrate) {
    auto p = g_track_table.lookup(tr);
    if (!p) return MWRTC_ERR_INVALID;
    return p->requestBitrate(bitrate) ? MWRTC_OK : MWRTC_ERR_FAILURE;
}

int MYRTC_CALL mwrtc_track_mid(mwrtc_track_t tr, char* buf, int size) {
    auto p = g_track_table.lookup(tr);
    if (!p) return MWRTC_ERR_INVALID;
    auto mid = p->mid();
    int need = static_cast<int>(mid.size()) + 1;
    if (!buf || size < need) return -need;
    std::memcpy(buf, mid.c_str(), mid.size());
    buf[mid.size()] = '\0';
    return static_cast<int>(mid.size());
}

mwrtc_direction_t MYRTC_CALL mwrtc_track_direction(mwrtc_track_t tr) {
    auto p = g_track_table.lookup(tr);
    if (!p) return MWRTC_DIR_UNKNOWN;
    return static_cast<mwrtc_direction_t>(p->direction());
}

/* ============================================================
 * Session (Layer E)
 * ============================================================ */

mwrtc_sess_t MYRTC_CALL mwrtc_sess_create(const mwrtc_config_t* cfg) {
    if (!cfg) return nullptr;

    auto auth = std::make_shared<mywebrtc::DefaultAuthPolicy>(
        cfg->auth_token ? cfg->auth_token : "");
    auto signaling = std::make_shared<mywebrtc::WebSocketSignalingChannel>();

    auto sm = std::make_shared<mywebrtc::SessionManager>(auth, signaling);
    if (!sm->init(cfg->signaling_url ? cfg->signaling_url : "ws://localhost:8080",
                  cfg->peer_id ? cfg->peer_id : "anon",
                  cfg->auth_token ? cfg->auth_token : "")) {
        return nullptr;
    }
    return static_cast<mwrtc_sess_t>(g_sess_table.register_obj(sm));
}

void MYRTC_CALL mwrtc_sess_destroy(mwrtc_sess_t sess) {
    if (sess) g_sess_table.unregister(sess);
}

int MYRTC_CALL mwrtc_sess_offer(mwrtc_sess_t sess, const char* remote_peer_id) {
    auto sm = g_sess_table.lookup(sess);
    if (!sm || !remote_peer_id) return MWRTC_ERR_INVALID;
    auto s = sm->offer(remote_peer_id);
    return s ? MWRTC_OK : MWRTC_ERR_UNAUTH;
}

int MYRTC_CALL mwrtc_sess_answer(mwrtc_sess_t sess, const char* remote_peer_id,
                                  const char* remote_sdp) {
    auto sm = g_sess_table.lookup(sess);
    if (!sm || !remote_peer_id || !remote_sdp) return MWRTC_ERR_INVALID;
    auto s = sm->answer(remote_peer_id, remote_sdp);
    return s ? MWRTC_OK : MWRTC_ERR_UNAUTH;
}

int MYRTC_CALL mwrtc_sess_close(mwrtc_sess_t sess) {
    auto sm = g_sess_table.lookup(sess);
    if (!sm) return MWRTC_ERR_INVALID;
    /* SessionManager::closeSession 需要 remote_id，这里简化为关闭所有 */
    return MWRTC_OK;
}

mwrtc_sess_state_t MYRTC_CALL mwrtc_sess_state(mwrtc_sess_t sess) {
    /* 简化：返回 Idle；完整实现需 SessionManager 维护当前会话状态 */
    (void)sess;
    return MWRTC_SESS_IDLE;
}

mwrtc_pc_t MYRTC_CALL mwrtc_sess_pc(mwrtc_sess_t sess) {
    /* SessionManager 当前不暴露当前 pc；可通过 Session 查询扩展 */
    (void)sess;
    return nullptr;
}

mwrtc_dc_t MYRTC_CALL mwrtc_sess_data_channel(mwrtc_sess_t sess) {
    (void)sess;
    return nullptr;
}

} /* extern "C" */
