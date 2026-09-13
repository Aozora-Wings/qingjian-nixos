#pragma once
// 与 qingjian-server（apps/linux/server）的 Unix domain socket 通信客户端。
// 帧格式与 Rust 侧 codec 一致：4 字节小端长度前缀 + UTF-8 JSON 载荷。

#include <cstdint>
#include <string>

#include "json.h"

class IPCClient {
public:
    IPCClient() = default;
    ~IPCClient();

    // 发一条 ClientMessage JSON，同步等回复；连接断开自动重连一次。
    // 失败返回 null JSON（调用方按连接错误处理）。
    qj::Value request(const qj::Value &message);

private:
    bool connect();
    void disconnect();
    bool writeFrame(const std::string &payload);
    bool readFrame(std::string &payload, int timeoutMs);

    int fd_ = -1;
};

// socket 路径：与 server 端 socket_path() 保持一致（XDG_RUNTIME_DIR/qingjian.sock，
// 缺省 /tmp/qingjian-<uid>.sock）。
std::string defaultSocketPath();
