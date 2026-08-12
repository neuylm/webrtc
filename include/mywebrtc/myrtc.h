/**
 * myrtc.h — Layer A: C ABI 对外唯一头文件
 *
 * 设计要点：
 * 1) 所有符号 mwrtc_ 前缀，与原 rtc_ 完全隔离
 * 2) 不透明句柄 (mwrtc_pc_t 等) 替代原 int 句柄，防止跨句柄误用
 * 3) 单一 mwrtc_pc_on_event 回调替代原 rtc.h 的 13 个回调注册函数
 * 4) 保留原 rtc.h 不动；此库 link 原 datachannel 库，不重新导出 rtc 符号
 *
 * 兼容性：C99
 */
#ifndef MYRTC_C_API_H
#define MYRTC_C_API_H

#include "myrtc_export.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 全局
 * ============================================================ */

/* 初始化库（线程池等）。可重复调用，幂等。返回 MWRTC_OK 或错误码。 */
MYRTC_API int MYRTC_CALL mwrtc_init(void);

/* 关闭库，释放全局资源。 */
MYRTC_API void MYRTC_CALL mwrtc_cleanup(void);

/* 设置日志级别（0=None ~ 6=Verbose）和可选回调 */
typedef void (MYRTC_CALL *mwrtc_log_cb)(int level, const char* message);
MYRTC_API void MYRTC_CALL mwrtc_set_log_level(int level, mwrtc_log_cb cb);

/* ============================================================
 * Layer B: PeerConnection
 * ============================================================ */

/* 创建 PeerConnection，返回句柄。失败返回 NULL。 */
MYRTC_API mwrtc_pc_t MYRTC_CALL mwrtc_pc_create(const mwrtc_config_t* cfg);

/* 显式销毁句柄（析构时也会自动释放，但建议显式调用） */
MYRTC_API void MYRTC_CALL mwrtc_pc_destroy(mwrtc_pc_t pc);

/* 关闭 PeerConnection */
MYRTC_API int MYRTC_CALL mwrtc_pc_close(mwrtc_pc_t pc);

/* 注册统一事件回调（替代原 13 个独立回调） */
MYRTC_API int MYRTC_CALL mwrtc_pc_on_event(mwrtc_pc_t pc, mwrtc_event_cb cb, void* user_data);

/* SDP 协商 */
MYRTC_API int MYRTC_CALL mwrtc_pc_set_local_description(mwrtc_pc_t pc, const char* type);
MYRTC_API int MYRTC_CALL mwrtc_pc_set_remote_description(mwrtc_pc_t pc, const char* sdp, const char* type);
MYRTC_API int MYRTC_CALL mwrtc_pc_add_remote_candidate(mwrtc_pc_t pc, const char* cand, const char* mid);

/* 查询类（buffer 形式，size in/out；返回长度，负数表示错误） */
MYRTC_API int MYRTC_CALL mwrtc_pc_get_local_description(mwrtc_pc_t pc, char* buf, int size);
MYRTC_API int MYRTC_CALL mwrtc_pc_get_remote_description(mwrtc_pc_t pc, char* buf, int size);

MYRTC_API mwrtc_state_t      MYRTC_CALL mwrtc_pc_state(mwrtc_pc_t pc);
MYRTC_API mwrtc_ice_state_t  MYRTC_CALL mwrtc_pc_ice_state(mwrtc_pc_t pc);
MYRTC_API bool               MYRTC_CALL mwrtc_pc_is_negotiation_needed(mwrtc_pc_t pc);

/* ============================================================
 * Layer B: DataChannel
 * ============================================================ */

MYRTC_API mwrtc_dc_t MYRTC_CALL mwrtc_pc_create_data_channel(mwrtc_pc_t pc, const char* label,
                                                              const mwrtc_dc_init_t* init);
MYRTC_API int  MYRTC_CALL mwrtc_dc_destroy(mwrtc_dc_t dc);
MYRTC_API int  MYRTC_CALL mwrtc_dc_send(mwrtc_dc_t dc, const char* data, int size);
MYRTC_API int  MYRTC_CALL mwrtc_dc_close(mwrtc_dc_t dc);
MYRTC_API bool MYRTC_CALL mwrtc_dc_is_open(mwrtc_dc_t dc);
MYRTC_API int  MYRTC_CALL mwrtc_dc_max_message_size(mwrtc_dc_t dc);
MYRTC_API int  MYRTC_CALL mwrtc_dc_label(mwrtc_dc_t dc, char* buf, int size);

/* ============================================================
 * Layer B: Track (媒体)
 * ============================================================ */

MYRTC_API mwrtc_track_t MYRTC_CALL mwrtc_pc_add_track(mwrtc_pc_t pc, const mwrtc_track_init_t* init);
MYRTC_API int  MYRTC_CALL mwrtc_track_destroy(mwrtc_track_t tr);
MYRTC_API int  MYRTC_CALL mwrtc_track_send_frame(mwrtc_track_t tr, const char* data, int size,
                                                  const mwrtc_frame_info_t* info);
MYRTC_API int  MYRTC_CALL mwrtc_track_request_keyframe(mwrtc_track_t tr);
MYRTC_API int  MYRTC_CALL mwrtc_track_request_bitrate(mwrtc_track_t tr, unsigned int bitrate);
MYRTC_API int  MYRTC_CALL mwrtc_track_mid(mwrtc_track_t tr, char* buf, int size);
MYRTC_API mwrtc_direction_t MYRTC_CALL mwrtc_track_direction(mwrtc_track_t tr);

/* ============================================================
 * Layer E: 业务接口
 * ============================================================ */

/* 会话管理器：聚合 PeerConnection + 信令 + 鉴权 + 状态机 */
MYRTC_API mwrtc_sess_t MYRTC_CALL mwrtc_sess_create(const mwrtc_config_t* cfg);
MYRTC_API void MYRTC_CALL mwrtc_sess_destroy(mwrtc_sess_t sess);

/* 发起呼叫 */
MYRTC_API int MYRTC_CALL mwrtc_sess_offer(mwrtc_sess_t sess, const char* remote_peer_id);
/* 接受呼叫（收到远端 offer 后调用） */
MYRTC_API int MYRTC_CALL mwrtc_sess_answer(mwrtc_sess_t sess, const char* remote_peer_id,
                                            const char* remote_sdp);
/* 主动关闭会话 */
MYRTC_API int MYRTC_CALL mwrtc_sess_close(mwrtc_sess_t sess);

/* 查询当前会话状态 */
MYRTC_API mwrtc_sess_state_t MYRTC_CALL mwrtc_sess_state(mwrtc_sess_t sess);

/* 获取底层 PeerConnection 句柄（供高级用户直接操作） */
MYRTC_API mwrtc_pc_t MYRTC_CALL mwrtc_sess_pc(mwrtc_sess_t sess);

/* 获取会话绑定的 DataChannel（若已建立） */
MYRTC_API mwrtc_dc_t MYRTC_CALL mwrtc_sess_data_channel(mwrtc_sess_t sess);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MYRTC_C_API_H */
