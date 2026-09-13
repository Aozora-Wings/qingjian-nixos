#include "ipc.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

namespace {

// 小端 4 字节。
void putU32LE(std::string &out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

bool getU32LE(const std::string &in, size_t at, uint32_t &out) {
    if (at + 4 > in.size()) return false;
    out = static_cast<uint32_t>(static_cast<unsigned char>(in[at])) |
          (static_cast<uint32_t>(static_cast<unsigned char>(in[at + 1])) << 8) |
          (static_cast<uint32_t>(static_cast<unsigned char>(in[at + 2])) << 16) |
          (static_cast<uint32_t>(static_cast<unsigned char>(in[at + 3])) << 24);
    return true;
}

}  // namespace

std::string defaultSocketPath() {
    if (const char *runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
        return std::string(runtime) + "/qingjian.sock";
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "/tmp/qingjian-%d.sock", static_cast<int>(getuid()));
    return buf;
}

IPCClient::~IPCClient() { disconnect(); }

void IPCClient::disconnect() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool IPCClient::connect() {
    disconnect();
    std::string path = defaultSocketPath();
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "qingjian: socket() 失败: %s\n", std::strerror(errno));
        return false;
    }
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        std::fprintf(stderr, "qingjian: socket 路径过长: %s\n", path.c_str());
        ::close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof addr) < 0) {
        std::fprintf(stderr, "qingjian: 连接 %s 失败: %s\n", path.c_str(), std::strerror(errno));
        ::close(fd);
        return false;
    }
    fd_ = fd;
    return true;
}

bool IPCClient::writeFrame(const std::string &payload) {
    if (fd_ < 0) return false;
    std::string frame;
    putU32LE(frame, static_cast<uint32_t>(payload.size()));
    frame += payload;
    size_t sent = 0;
    while (sent < frame.size()) {
        ssize_t n = ::send(fd_, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool IPCClient::readFrame(std::string &payload, int timeoutMs) {
    if (fd_ < 0) return false;
    std::string head;
    head.resize(4);
    size_t got = 0;
    while (got < 4) {
        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int ready = ::poll(&pfd, 1, timeoutMs);
        if (ready <= 0) return false;
        ssize_t n = ::recv(fd_, &head[got], 4 - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    uint32_t len = 0;
    if (!getU32LE(head, 0, len) || len > 16 * 1024 * 1024) return false;
    payload.resize(len);
    got = 0;
    while (got < len) {
        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int ready = ::poll(&pfd, 1, timeoutMs);
        if (ready <= 0) return false;
        ssize_t n = ::recv(fd_, &payload[got], len - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

qj::Value IPCClient::request(const qj::Value &message) {
    qj::Value nullValue;
    if (fd_ < 0 && !connect()) return nullValue;
    std::string payload = message.dump();
    if (!writeFrame(payload)) {
        disconnect();
        if (!connect() || !writeFrame(payload)) return nullValue;
    }
    std::string reply;
    if (!readFrame(reply, 2000)) {
        disconnect();
        return nullValue;
    }
    qj::Value parsed;
    std::string error;
    if (!qj::Value::parse(reply, parsed, error)) {
        std::fprintf(stderr, "qingjian: 回复 JSON 解析失败: %s\n", error.c_str());
        return nullValue;
    }
    return parsed;
}
