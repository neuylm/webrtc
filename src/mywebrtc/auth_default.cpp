/**
 * auth_default.cpp — Layer E: 默认鉴权实现
 */
#include "auth.hpp"

namespace mywebrtc {

DefaultAuthPolicy::DefaultAuthPolicy(std::string expected_token)
    : expected_token_(std::move(expected_token)) {}

bool DefaultAuthPolicy::authenticate(const std::string& token, const std::string& peerId) {
    (void)peerId;
    /* 简单 token 匹配。
     * 真实场景应替换为 JWT 解析 / OAuth 远程校验 / 数据库查询。 */
    return !expected_token_.empty() && token == expected_token_;
}

bool DefaultAuthPolicy::canPublish(const std::string& peerId, const std::string& trackId) {
    (void)peerId; (void)trackId;
    /* 默认放行；实际项目可基于角色矩阵做细粒度控制 */
    return true;
}

bool DefaultAuthPolicy::canSubscribe(const std::string& peerId, const std::string& trackId) {
    (void)peerId; (void)trackId;
    return true;
}

} /* namespace mywebrtc */
