/**
 * auth.hpp — Layer E: 鉴权策略接口
 *
 * 抽象接口，便于替换为 JWT/OAuth/自定义实现。
 */
#ifndef MYRTC_AUTH_H
#define MYRTC_AUTH_H

#include <memory>
#include <string>

namespace mywebrtc {

class AuthPolicy {
public:
    virtual ~AuthPolicy() = default;

    /* 校验 token 是否对该 peerId 有效 */
    virtual bool authenticate(const std::string& token, const std::string& peerId) = 0;

    /* 是否允许此 peer 发布指定 track */
    virtual bool canPublish(const std::string& peerId, const std::string& trackId) = 0;

    /* 是否允许此 peer 订阅指定 track */
    virtual bool canSubscribe(const std::string& peerId, const std::string& trackId) = 0;
};

/* 默认实现：简单 token 字符串匹配。
 * 可替换为 JWT 解析 / 数据库查询 / OAuth 远程校验等。 */
class DefaultAuthPolicy : public AuthPolicy {
public:
    /* 构造时传入期望的 token；token 非空且匹配即通过 */
    explicit DefaultAuthPolicy(std::string expected_token);

    bool authenticate(const std::string& token, const std::string& peerId) override;
    bool canPublish(const std::string& peerId, const std::string& trackId) override;
    bool canSubscribe(const std::string& peerId, const std::string& trackId) override;

private:
    std::string expected_token_;
};

} /* namespace mywebrtc */

#endif /* MYRTC_AUTH_H */
