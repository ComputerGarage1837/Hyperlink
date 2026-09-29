#include "hyperlink/net.h"

#include <cstring>

#include "hyperlink/common.h"
#include "hyperlink/protocol.h"

#ifdef _WIN32
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
using socklen_t = int;
#define HL_CLOSE closesocket
#define HL_SHUT_BOTH SD_BOTH
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>
#define HL_CLOSE ::close
#define HL_SHUT_BOTH SHUT_RDWR
#endif

namespace hl::net {

bool init() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
        done = true;
    }
#endif
    return true;
}

std::string Addr::ip() const {
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &sa.sin_addr, buf, sizeof buf);
    return buf;
}
uint16_t Addr::port() const { return ntohs(sa.sin_port); }
bool Addr::sameAs(const Addr& o) const {
    return sa.sin_addr.s_addr == o.sa.sin_addr.s_addr && sa.sin_port == o.sa.sin_port;
}

bool Addr::resolve(const std::string& host, uint16_t port, Addr& out) {
    out = Addr();
    out.sa.sin_family = AF_INET;
    out.sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &out.sa.sin_addr) == 1) return true;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.sa.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

Addr Addr::broadcast(uint16_t port) {
    Addr a;
    a.sa.sin_family = AF_INET;
    a.sa.sin_port = htons(port);
    a.sa.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    return a;
}

Socket& Socket::operator=(Socket&& o) noexcept {
    if (this != &o) {
        close();
        s_ = o.s_;
        o.s_ = kInvalid;
    }
    return *this;
}

void Socket::close() {
    if (s_ != kInvalid) {
        HL_CLOSE(s_);
        s_ = kInvalid;
    }
}

void Socket::shutdown() {
    if (s_ != kInvalid) ::shutdown(s_, HL_SHUT_BOTH);
}

Socket Socket::listenTcp(uint16_t port) {
    Socket s(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!s.valid()) return s;
    int one = 1;
    setsockopt(s.s_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s.s_, (sockaddr*)&a, sizeof a) != 0 || listen(s.s_, 8) != 0) s.close();
    return s;
}

static bool waitWritable(sock_t s, int timeoutMs) {
#ifdef _WIN32
    fd_set w, e;
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(s, &w); FD_SET(s, &e);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    return select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s, &w);
#else
    pollfd p{s, POLLOUT, 0};
    return poll(&p, 1, timeoutMs) > 0 && (p.revents & POLLOUT);
#endif
}

static void setBlocking(sock_t s, bool blocking) {
#ifdef _WIN32
    u_long nb = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
}

Socket Socket::connectTcp(const Addr& addr, int timeoutMs) {
    Socket s(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!s.valid()) return s;
    setBlocking(s.s_, false);
    int r = ::connect(s.s_, (const sockaddr*)&addr.sa, sizeof addr.sa);
    if (r != 0 && !waitWritable(s.s_, timeoutMs)) {
        s.close();
        return s;
    }
    int err = 0;
    socklen_t len = sizeof err;
    getsockopt(s.s_, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
    if (err != 0) {
        s.close();
        return s;
    }
    setBlocking(s.s_, true);
    s.setNoDelay();
    return s;
}

Socket Socket::accept(Addr* from) {
    sockaddr_in a{};
    socklen_t len = sizeof a;
    Socket c(::accept(s_, (sockaddr*)&a, &len));
    if (c.valid()) {
        c.setNoDelay();
        if (from) from->sa = a;
    }
    return c;
}

void Socket::setNoDelay() {
    int one = 1;
    setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
}

bool Socket::sendAll(const void* data, size_t n) {
    const char* p = (const char*)data;
    while (n) {
#ifdef MSG_NOSIGNAL
        int r = ::send(s_, p, (int)n, MSG_NOSIGNAL);
#else
        int r = ::send(s_, p, (int)n, 0);
#endif
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

bool Socket::recvAll(void* data, size_t n) {
    char* p = (char*)data;
    while (n) {
        int r = ::recv(s_, p, (int)n, 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

Socket Socket::udp(uint16_t bindPort) {
    Socket s(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!s.valid()) return s;
    if (bindPort) {
        int one = 1;
        setsockopt(s.s_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(bindPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s.s_, (sockaddr*)&a, sizeof a) != 0) s.close();
#ifdef _WIN32
    // Without this, an ICMP "port unreachable" makes the next recvfrom fail with WSAECONNRESET.
    if (s.valid()) {
        BOOL off = FALSE;
        DWORD ret = 0;
        WSAIoctl(s.s_, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
    }
#endif
    return s;
}

bool Socket::sendTo(const Addr& to, const void* data, size_t n) {
    return ::sendto(s_, (const char*)data, (int)n, 0, (const sockaddr*)&to.sa, sizeof to.sa) ==
           (int)n;
}

int Socket::recvFrom(void* data, size_t cap, Addr* from, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD p{s_, POLLRDNORM, 0};
    int pr = WSAPoll(&p, 1, timeoutMs);
#else
    pollfd p{s_, POLLIN, 0};
    int pr = poll(&p, 1, timeoutMs);
#endif
    if (pr == 0) return 0;
    if (pr < 0) return -1;
    sockaddr_in a{};
    socklen_t len = sizeof a;
    int r = ::recvfrom(s_, (char*)data, (int)cap, 0, (sockaddr*)&a, &len);
    if (r < 0) return -1;
    if (from) from->sa = a;
    return r;
}

void Socket::setBuffers(int bytes) {
    setsockopt(s_, SOL_SOCKET, SO_SNDBUF, (const char*)&bytes, sizeof bytes);
    setsockopt(s_, SOL_SOCKET, SO_RCVBUF, (const char*)&bytes, sizeof bytes);
}

void Socket::enableBroadcast() {
    int one = 1;
    setsockopt(s_, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
}

bool MessageConn::send(const std::vector<uint8_t>& msg) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    return sock_.sendAll(msg.data(), msg.size());
}

bool MessageConn::read(std::vector<uint8_t>& body) {
    uint8_t len[4];
    if (!sock_.recvAll(len, 4)) return false;
    uint32_t n = get32(len);
    if (n == 0 || n > kMaxMessage) return false;
    body.resize(n);
    return sock_.recvAll(body.data(), n);
}

std::string hostName() {
    char buf[256] = {};
    if (gethostname(buf, sizeof buf - 1) != 0) return "Hyperlink host";
    return buf;
}

std::vector<std::string> localAddresses() {
    std::vector<std::string> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size) != NO_ERROR) return out;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            char ip[INET_ADDRSTRLEN] = {};
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
            out.push_back(ip);
        }
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return out;
    for (auto* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, ip, sizeof ip);
        out.push_back(ip);
    }
    freeifaddrs(ifs);
#endif
    return out;
}

}  // namespace hl::net
