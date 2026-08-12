/**
 * types.h — 公共类型定义 (Layer A)
 *
 * 纯 C99 兼容。对外暴露的枚举/结构/错误码统一在此。
 * 命名前缀 mwrtc_ 用于避免与原 rtc_ 符号冲突。
 */
#ifndef MYRTC_TYPES_H
#define MYRTC_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 错误码 ===== */
#define MWRTC_OK            0
#define MWRTC_ERR_INVALID  -1   /* 无效参数 / 空句柄 */
#define MWRTC_ERR_FAILURE  -2   /* 运行时错误 */
#define MWRTC_ERR_NOT_IMPL -3   /* 未实现 */
#define MWRTC_ERR_TOO_SMALL -4  /* 缓冲区不足 */
#define MWRTC_ERR_UNAUTH   -5   /* 鉴权失败 */
#define MWRTC_ERR_STATE    -6   /* 状态机非法转移 */
#define MWRTC_ERR_NOT_FOUND -7  /* 找不到会话 / 句柄 */

/* ===== 句柄类型（不透明指针，比原 rtc 的 int 句柄更安全） ===== */
typedef struct mwrtc_pc_s*   mwrtc_pc_t;     /* PeerConnection */
typedef struct mwrtc_dc_s*   mwrtc_dc_t;     /* DataChannel */
typedef struct mwrtc_track_s* mwrtc_track_t; /* Track */
typedef struct mwrtc_sess_s* mwrtc_sess_t;   /* Session */

/* ===== PeerConnection 状态 ===== */
typedef enum {
    MWRTC_STATE_NEW = 0,
    MWRTC_STATE_CONNECTING = 1,
    MWRTC_STATE_CONNECTED = 2,
    MWRTC_STATE_DISCONNECTED = 3,
    MWRTC_STATE_FAILED = 4,
    MWRTC_STATE_CLOSED = 5
} mwrtc_state_t;

/* ===== ICE 状态 ===== */
typedef enum {
    MWRTC_ICE_NEW = 0,
    MWRTC_ICE_CHECKING = 1,
    MWRTC_ICE_CONNECTED = 2,
    MWRTC_ICE_COMPLETED = 3,
    MWRTC_ICE_FAILED = 4,
    MWRTC_ICE_DISCONNECTED = 5,
    MWRTC_ICE_CLOSED = 6
} mwrtc_ice_state_t;

/* ===== 会话状态机 ===== */
typedef enum {
    MWRTC_SESS_IDLE = 0,
    MWRTC_SESS_OFFERING = 1,
    MWRTC_SESS_ANSWERING = 2,
    MWRTC_SESS_CONNECTING = 3,
    MWRTC_SESS_CONNECTED = 4,
    MWRTC_SESS_STREAMING = 5,
    MWRTC_SESS_PAUSING = 6,
    MWRTC_SESS_CLOSING = 7,
    MWRTC_SESS_CLOSED = 8,
    MWRTC_SESS_FAILED = 9
} mwrtc_sess_state_t;

/* ===== 事件类型（统一回调，替代原 rtc.h 的 13 个回调注册函数） ===== */
typedef enum {
    MWRTC_EV_PC_STATE = 0,        /* PeerConnection 状态变化 */
    MWRTC_EV_ICE_STATE,           /* ICE 状态变化 */
    MWRTC_EV_LOCAL_DESCRIPTION,  /* 本地 SDP 生成 */
    MWRTC_EV_LOCAL_CANDIDATE,    /* 本地 candidate */
    MWRTC_EV_DATA_CHANNEL,       /* 收到远端 DataChannel */
    MWRTC_EV_TRACK,              /* 收到远端 Track */
    MWRTC_EV_DC_OPEN,            /* DataChannel 打开 */
    MWRTC_EV_DC_CLOSED,          /* DataChannel 关闭 */
    MWRTC_EV_DC_MESSAGE,         /* DataChannel 消息 */
    MWRTC_EV_DC_ERROR,           /* DataChannel 错误 */
    MWRTC_EV_TRACK_FRAME,        /* Track 收到帧 */
    MWRTC_EV_SESS_STATE,         /* 会话状态机转移 */
    MWRTC_EV_AUTH_RESULT         /* 鉴权结果 */
} mwrtc_event_type_t;

/* ===== 事件载荷（tagged union） ===== */
typedef struct {
    mwrtc_event_type_t type;
    int handle_id;                /* pc / dc / track 之一，按 type 解释 */
    const char* sdp;              /* LOCAL_DESCRIPTION */
    const char* sdp_type;         /* "offer"/"answer"/NULL */
    const char* candidate;        /* LOCAL_CANDIDATE */
    const char* mid;              /* LOCAL_CANDIDATE */
    mwrtc_state_t pc_state;       /* PC_STATE */
    mwrtc_ice_state_t ice_state;  /* ICE_STATE */
    mwrtc_sess_state_t sess_state;/* SESS_STATE */
    int  data_channel_id;         /* DATA_CHANNEL / DC_* 事件的 dc 句柄 */
    int  track_id;                /* TRACK / TRACK_FRAME 的 track 句柄 */
    const char* data;             /* DC_MESSAGE / TRACK_FRAME */
    int  data_size;               /* DC_MESSAGE / TRACK_FRAME */
    const char* error_msg;        /* DC_ERROR / AUTH_RESULT */
    bool  auth_ok;                /* AUTH_RESULT */
} mwrtc_event_t;

/* 统一事件回调签名 */
typedef void (MYRTC_CALL *mwrtc_event_cb)(const mwrtc_event_t* ev, void* user_data);

/* ===== 配置（Builder 风格的 C 结构体） ===== */
typedef struct {
    const char* const* ice_servers;   /* "stun:xxx" / "turn:xxx" */
    int  ice_servers_count;
    const char* bind_address;          /* NULL = any */
    int  certificate_type;             /* 0=ECDSA, 1=RSA, 2=Default */
    int  mtu;                           /* <=0 自动 */
    int  max_message_size;              /* <=0 默认 */
    bool enable_ice_udp_mux;
    bool disable_auto_negotiation;
    bool force_media_transport;
    const char* signaling_url;          /* Layer E: 信令服务器 URL */
    const char* auth_token;             /* Layer E: 鉴权 token */
    const char* peer_id;                /* Layer E: 本端 ID */
} mwrtc_config_t;

/* ===== DataChannel 初始化 ===== */
typedef struct {
    bool unordered;
    bool unreliable;
    unsigned int max_packet_life_time;  /* unreliable 时有效 */
    unsigned int max_retransmits;      /* unreliable 时有效 */
    const char* protocol;               /* 可空 */
    bool negotiated;
    bool manual_stream;
    uint16_t stream;
} mwrtc_dc_init_t;

/* ===== Track 初始化 ===== */
typedef enum {
    MWRTC_CODEC_H264 = 0,
    MWRTC_CODEC_VP8 = 1,
    MWRTC_CODEC_VP9 = 2,
    MWRTC_CODEC_H265 = 3,
    MWRTC_CODEC_AV1 = 4,
    MWRTC_CODEC_OPUS = 128,
    MWRTC_CODEC_PCMU = 129,
    MWRTC_CODEC_PCMA = 130,
    MWRTC_CODEC_AAC = 131,
    MWRTC_CODEC_G722 = 132
} mwrtc_codec_t;

typedef enum {
    MWRTC_DIR_UNKNOWN = 0,
    MWRTC_DIR_SENDONLY = 1,
    MWRTC_DIR_RECVONLY = 2,
    MWRTC_DIR_SENDRECV = 3,
    MWRTC_DIR_INACTIVE = 4
} mwrtc_direction_t;

typedef struct {
    mwrtc_direction_t direction;
    mwrtc_codec_t codec;
    int payload_type;
    uint32_t ssrc;
    const char* mid;
    const char* name;       /* 可空 */
    const char* msid;      /* 可空 */
    const char* track_id;  /* 可空 */
    const char* profile;   /* 可空 */
} mwrtc_track_init_t;

/* ===== 帧信息 ===== */
typedef struct {
    uint32_t timestamp;
    uint8_t  payload_type;
    double   timestamp_seconds; /* <0 表示不可用 */
} mwrtc_frame_info_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MYRTC_TYPES_H */
