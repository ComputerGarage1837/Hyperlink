// Unit tests for the shared core. Plain asserts, no framework; exits non-zero on failure.
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <random>

#include <thread>

#include "hyperlink/client.h"
#include "hyperlink/common.h"
#include "hyperlink/net.h"
#include "hyperlink/fec.h"
#include "hyperlink/json.h"
#include "hyperlink/protocol.h"
#include "hyperlink/video.h"

using namespace hl;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            failures++;                                                  \
        }                                                                \
    } while (0)

static std::mt19937 rng(1234);

static void testFecAllPatterns() {
    const size_t S = 64;
    for (int k = 1; k <= 20; k++) {
        for (int m = 1; m <= 6; m++) {
            std::vector<std::vector<uint8_t>> sh(k + m, std::vector<uint8_t>(S));
            for (int i = 0; i < k; i++)
                for (auto& b : sh[i]) b = (uint8_t)rng();
            auto orig = sh;
            std::vector<const uint8_t*> d(k);
            std::vector<uint8_t*> p(m);
            for (int i = 0; i < k; i++) d[i] = sh[i].data();
            for (int j = 0; j < m; j++) p[j] = sh[k + j].data();
            fec::encode(d.data(), k, p.data(), m, S);

            for (int trial = 0; trial < 30; trial++) {
                auto work = sh;
                std::vector<uint8_t> present(k + m, 1);
                int lose = (int)(rng() % (m + 1));
                for (int l = 0; l < lose; l++) {
                    int idx = (int)(rng() % (k + m));
                    present[idx] = 0;
                }
                for (int i = 0; i < k + m; i++)
                    if (!present[i]) std::fill(work[i].begin(), work[i].end(), 0xEE);
                std::vector<uint8_t*> ptr(k + m);
                for (int i = 0; i < k + m; i++) ptr[i] = work[i].data();
                CHECK(fec::decode(ptr.data(), present.data(), k, m, S));
                for (int i = 0; i < k; i++) CHECK(work[i] == orig[i]);
            }
        }
    }
    // Too many losses must fail cleanly.
    int k = 4, m = 2;
    std::vector<std::vector<uint8_t>> sh(k + m, std::vector<uint8_t>(S, 1));
    std::vector<uint8_t*> ptr(k + m);
    for (int i = 0; i < k + m; i++) ptr[i] = sh[i].data();
    std::vector<uint8_t> present = {0, 0, 0, 1, 1, 1};
    CHECK(!fec::decode(ptr.data(), present.data(), k, m, S));
}

struct Captured {
    std::vector<std::vector<uint8_t>> packets;
};

static std::vector<uint8_t> randomFrame(size_t n) {
    std::vector<uint8_t> f(n);
    for (auto& b : f) b = (uint8_t)rng();
    return f;
}

static void testRoundTripWithLoss() {
    Packetizer pk(20);
    pk.setEpoch(7);
    Reassembler ra;
    ra.setEpoch(7);
    std::vector<CompleteFrame> got;
    uint32_t lost = 0;
    ra.onFrame = [&](CompleteFrame&& f) { got.push_back(std::move(f)); };
    ra.onLoss = [&](uint32_t, uint32_t c) { lost += c; };

    std::vector<std::vector<uint8_t>> frames;
    size_t sizes[] = {1, 1199, 1200, 1201, 50000, 400000, 3000};
    uint32_t idx = 0;
    for (size_t sz : sizes) {
        auto f = randomFrame(sz);
        frames.push_back(f);
        EncodedFrame ef;
        ef.data = f.data();
        ef.size = f.size();
        ef.keyframe = idx == 0;
        ef.frameIndex = idx++;
        std::vector<std::vector<uint8_t>> pkts;
        pk.packetize(ef, [&](const uint8_t* p, size_t n) { pkts.emplace_back(p, p + n); });
        // Drop one packet out of every 10: always within the 20% parity budget of each block.
        std::shuffle(pkts.begin(), pkts.end(), rng);
        for (size_t i = 0; i < pkts.size(); i++) {
            if (pkts.size() >= 10 && i % 10 == 3) continue;
            ra.push(pkts[i].data(), pkts[i].size(), nowUs());
        }
    }
    CHECK(got.size() == frames.size());
    CHECK(lost == 0);
    for (size_t i = 0; i < got.size() && i < frames.size(); i++) {
        CHECK(got[i].data == frames[i]);
        CHECK(got[i].frameIndex == i);
    }
    CHECK(got.size() && got[0].keyframe);
    CHECK(ra.shardsRecovered > 0);
}

static void testUnrecoverableFrameReportsLoss() {
    Packetizer pk(10);
    Reassembler ra;
    std::vector<uint32_t> delivered;
    uint32_t lostFirst = 99, lostCount = 0;
    ra.onFrame = [&](CompleteFrame&& f) { delivered.push_back(f.frameIndex); };
    ra.onLoss = [&](uint32_t first, uint32_t c) { lostFirst = first; lostCount += c; };
    for (uint32_t i = 0; i < 3; i++) {
        auto f = randomFrame(20000);
        EncodedFrame ef{f.data(), f.size(), false, i, 0, 0};
        int n = 0;
        pk.packetize(ef, [&](const uint8_t* p, size_t len) {
            if (i == 1 && n++ < 5) return;  // lose 5 shards of frame 1: more than its parity
            ra.push(p, len, 0);
        });
    }
    CHECK((delivered == std::vector<uint32_t>{0, 2}));
    CHECK(lostFirst == 1 && lostCount == 1);
}

static void testStaleEpochIgnored() {
    Packetizer pk(0);
    pk.setEpoch(1);
    Reassembler ra;
    ra.setEpoch(2);
    int n = 0;
    ra.onFrame = [&](CompleteFrame&&) { n++; };
    auto f = randomFrame(100);
    EncodedFrame ef{f.data(), f.size(), true, 0, 0, 0};
    pk.packetize(ef, [&](const uint8_t* p, size_t len) { ra.push(p, len, 0); });
    CHECK(n == 0);
}

static void testProtocolRoundTrip() {
    Welcome w;
    w.hostName = "desk";
    w.sessionId = 42;
    w.codecMask = 3;
    MonitorInfo m;
    m.id = 2; m.name = "DELL"; m.x = -2560; m.y = 0; m.width = 2560; m.height = 1440;
    m.refreshHz = 144; m.primary = 1;
    w.monitors = {m, m};
    auto bytes = w.encode();
    CHECK(get32(bytes.data()) == bytes.size() - 4);
    CHECK(bytes[4] == MSG_WELCOME);
    Reader r(bytes.data() + 5, bytes.size() - 5);
    Welcome w2;
    CHECK(w2.decode(r));
    CHECK(w2.hostName == "desk" && w2.sessionId == 42 && w2.monitors.size() == 2);
    CHECK(w2.monitors[1].x == -2560 && w2.monitors[1].refreshHz == 144);

    // Truncated input fails instead of reading past the end.
    Reader bad(bytes.data() + 5, 10);
    Welcome w3;
    CHECK(!w3.decode(bad));

    StartStream s;
    s.monitorId = 5; s.maxWidth = 2400; s.maxHeight = 1080; s.fps = 120; s.bitrateKbps = 60000;
    auto sb = s.encode();
    Reader sr(sb.data() + 5, sb.size() - 5);
    StartStream s2;
    CHECK(s2.decode(sr) && s2.monitorId == 5 && s2.maxWidth == 2400 && s2.bitrateKbps == 60000);
}

static void testDiscovery() {
    DiscoveryReply d;
    d.hostId = "abc"; d.hostName = "Gaming PC"; d.version = "0.1.0"; d.pinRequired = 1;
    d.clients = 2; d.streams = 3;
    d.remoteAddresses = {"100.101.102.103"};
    auto b = d.encode();
    DiscoveryReply e;
    CHECK(e.decode(b.data(), b.size()));
    CHECK(e.hostName == "Gaming PC" && e.clients == 2 && e.streams == 3 && e.pinRequired == 1);
    CHECK(!e.decode(b.data(), 5));
    CHECK(e.remoteAddresses.size() == 1 && e.remoteAddresses[0] == "100.101.102.103");
    // A reply from an older host (no address list) still decodes.
    DiscoveryReply old = d;
    old.remoteAddresses.clear();
    auto ob = old.encode();
    DiscoveryReply o2;
    CHECK(o2.decode(ob.data(), ob.size() - 2) && o2.remoteAddresses.empty());

    // Stream ids travel in the video header.
    Packetizer pk(0);
    pk.setStreamId(9);
    auto f = randomFrame(10);
    EncodedFrame ef{f.data(), f.size(), true, 0, 0, 0};
    int sid = -2;
    pk.packetize(ef, [&](const uint8_t* p, size_t n) { sid = peekStreamId(p, n); });
    CHECK(sid == 9);
}

static void testJson() {
    json::Value v;
    CHECK(json::parse(R"({"tag_name":"v1.2.3","body":"line\n\u00e9","draft":false,
        "assets":[{"name":"a.apk","size":12,"browser_download_url":"https://x/a.apk"}]})", v));
    CHECK(v["tag_name"].asString() == "v1.2.3");
    CHECK(v["body"].asString() == "line\n\xc3\xa9");
    CHECK(v["assets"][0]["size"].asNumber() == 12);
    CHECK(v["assets"][1]["name"].asString("none") == "none");
    CHECK(!v["draft"].asBool(true));
    CHECK(!json::parse("{\"a\":", v));
    CHECK(json::versionCode("v1.2.3") == 1002003);
    CHECK(json::versionCode("0.10.0") > json::versionCode("0.9.9"));
    CHECK(json::versionCode("2.0") == 2000000);
}

// A fake host on loopback drives a real Client through sign-in, a stream, loss and recovery.
static void testClientEndToEnd() {
    const uint16_t tcpPort = 47900, udpPort = 47901;
    net::init();
    net::Socket listen = net::Socket::listenTcp(tcpPort);
    net::Socket hostUdp = net::Socket::udp(udpPort);
    CHECK(listen.valid() && hostUdp.valid());
    if (!listen.valid() || !hostUdp.valid()) return;

    std::atomic<int> idrRequests{0};
    std::thread host([&] {
        for (int round = 0; round < 2; round++) {
            net::MessageConn c(listen.accept());
            std::vector<uint8_t> body;
            if (!c.read(body)) return;
            Reader r(body.data() + 1, body.size() - 1);
            Hello h;
            h.decode(r);
            if (h.pin != "1234") {
                Writer w(MSG_AUTH_FAILED);
                w.str("Wrong PIN");
                c.send(w.done());
                continue;
            }
            Welcome w;
            w.hostName = "fake";
            w.sessionId = 77;
            w.videoPort = udpPort;
            MonitorInfo m;
            m.id = 3; m.width = 1920; m.height = 1080;
            w.monitors = {m};
            c.send(w.encode());

            uint8_t pbuf[64];
            net::Addr clientAddr;
            bool havePunch = false;
            for (int i = 0; i < 50 && !havePunch; i++) {
                int n = hostUdp.recvFrom(pbuf, sizeof pbuf, &clientAddr, 100);
                havePunch = n >= 8 && pbuf[2] == PKT_PUNCH && get32(pbuf + 4) == 77;
            }
            if (!c.read(body) || body[0] != MSG_START_STREAM) return;
            Reader sr(body.data() + 1, body.size() - 1);
            StartStream ss;
            ss.decode(sr);
            StreamStarted st;
            st.streamId = ss.streamId;
            st.monitorId = ss.monitorId;
            st.width = 1920; st.height = 1080; st.fps = 120; st.epoch = 5;
            c.send(st.encode());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            Packetizer pk(20);
            pk.setEpoch(5);
            pk.setStreamId(ss.streamId);
            auto sendFrame = [&](uint32_t idx, bool key, bool drop) {
                auto f = randomFrame(5000);
                f[0] = (uint8_t)idx;
                EncodedFrame ef{f.data(), f.size(), key, idx, 0, 0};
                pk.packetize(ef, [&](const uint8_t* p, size_t n) {
                    if (!drop) hostUdp.sendTo(clientAddr, p, n);
                });
            };
            sendFrame(0, true, false);
            sendFrame(1, false, false);
            sendFrame(2, false, true);   // lost entirely
            sendFrame(3, false, false);  // must be held back: it depends on frame 2
            // Expect a keyframe request.
            c.socket();
            for (int i = 0; i < 20; i++) {
                if (!c.read(body)) return;
                if (body[0] == MSG_REQUEST_IDR) { idrRequests++; break; }
            }
            sendFrame(4, true, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return;
        }
    });

    struct Sink : FrameSink {
        std::mutex m;
        std::vector<int> got;
        bool onFrame(const CompleteFrame& f, const StreamStarted&) override {
            std::lock_guard<std::mutex> l(m);
            got.push_back(f.data[0]);
            return true;
        }
    };
    ClientListener listener;
    {
        Client bad(&listener);
        Hello h;
        h.pin = "0000";
        Welcome w;
        std::string err = bad.connect("127.0.0.1", tcpPort, h, w);
        CHECK(err == "Wrong PIN");
    }
    Client client(&listener);
    Hello h;
    h.pin = "1234";
    Welcome w;
    std::string err = client.connect("127.0.0.1", tcpPort, h, w);
    CHECK(err.empty());
    CHECK(w.hostName == "fake" && w.monitors.size() == 1);
    auto sink = std::make_shared<Sink>();
    StartStream ss;
    ss.streamId = 1;
    ss.monitorId = 3;
    client.startStream(ss, sink);
    host.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard<std::mutex> l(sink->m);
        CHECK((sink->got == std::vector<int>{0, 1, 4}));
        if (sink->got != std::vector<int>{0, 1, 4}) {
            std::printf("got:");
            for (int x : sink->got) std::printf(" %d", x);
            std::printf("\n");
        }
    }
    CHECK(idrRequests == 1);
    auto st = client.takeStats(1);
    CHECK(st.valid && st.width == 1920);
    client.disconnect();
}

int main() {
    testClientEndToEnd();
    testJson();
    testDiscovery();
    testFecAllPatterns();
    testRoundTripWithLoss();
    testUnrecoverableFrameReportsLoss();
    testStaleEpochIgnored();
    testProtocolRoundTrip();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all core tests passed\n");
    return 0;
}
