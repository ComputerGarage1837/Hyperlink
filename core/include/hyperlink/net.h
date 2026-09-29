// Thin cross-platform (Winsock / POSIX) socket wrappers. IPv4 only for now.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace hl::net {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kInvalid = INVALID_SOCKET;
#else
using sock_t = int;
constexpr sock_t kInvalid = -1;
#endif

bool init();

struct Addr {
    sockaddr_in sa{};
    std::string ip() const;
    uint16_t port() const;
    std::string str() const { return ip() + ":" + std::to_string(port()); }
    bool sameAs(const Addr& o) const;
    static bool resolve(const std::string& host, uint16_t port, Addr& out);
    static Addr broadcast(uint16_t port);
};

class Socket {
public:
    Socket() = default;
    explicit Socket(sock_t s) : s_(s) {}
    ~Socket() { close(); }
    Socket(Socket&& o) noexcept : s_(o.s_) { o.s_ = kInvalid; }
    Socket& operator=(Socket&& o) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool valid() const { return s_ != kInvalid; }
    sock_t get() const { return s_; }
    void close();
    // Unblocks a thread sitting in recv/accept on this socket.
    void shutdown();

    // TCP
    static Socket listenTcp(uint16_t port);
    static Socket connectTcp(const Addr& addr, int timeoutMs);
    Socket accept(Addr* from = nullptr);
    bool sendAll(const void* data, size_t n);
    bool recvAll(void* data, size_t n);
    void setNoDelay();

    // UDP
    static Socket udp(uint16_t bindPort = 0);
    bool sendTo(const Addr& to, const void* data, size_t n);
    // Returns bytes received, 0 on timeout, -1 on error.
    int recvFrom(void* data, size_t cap, Addr* from, int timeoutMs);
    void setBuffers(int bytes);
    void enableBroadcast();

private:
    sock_t s_ = kInvalid;
};

// Length-prefixed message stream over a TCP socket. send() may be called from any thread.
class MessageConn {
public:
    explicit MessageConn(Socket s) : sock_(std::move(s)) {}
    bool send(const std::vector<uint8_t>& msg);
    // Blocks until a whole message arrives. body excludes the length prefix; body[0] is the type.
    bool read(std::vector<uint8_t>& body);
    void shutdown() { sock_.shutdown(); }
    Socket& socket() { return sock_; }

private:
    Socket sock_;
    std::mutex sendMutex_;
};

std::string hostName();
// IPv4 addresses of this machine's network interfaces (not loopback).
std::vector<std::string> localAddresses();

}  // namespace hl::net
