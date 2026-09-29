// JNI bridge between com.hyperlink.app.NativeClient and hl::Client.
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <map>
#include <memory>
#include <mutex>

#include "decoder.h"
#include "hyperlink/client.h"
#include "hyperlink/common.h"

namespace {

JavaVM* gVm = nullptr;

// Attaches native threads to the JVM once and detaches them when the thread ends.
struct ThreadEnv {
    JNIEnv* env = nullptr;
    bool attached = false;
    ~ThreadEnv() {
        if (attached) gVm->DetachCurrentThread();
    }
};

JNIEnv* env() {
    thread_local ThreadEnv t;
    if (!t.env) {
        if (gVm->GetEnv((void**)&t.env, JNI_VERSION_1_6) != JNI_OK) {
            gVm->AttachCurrentThread(&t.env, nullptr);
            t.attached = true;
        }
    }
    return t.env;
}

std::string str(JNIEnv* e, jstring s) {
    if (!s) return {};
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string out(c);
    e->ReleaseStringUTFChars(s, c);
    return out;
}

// Decoder bound to one Surface.
class SurfaceSink : public hl::FrameSink {
public:
    SurfaceSink(ANativeWindow* w, int fps) : window_(w), fps_(fps) {}
    ~SurfaceSink() override {
        decoder_.stop();
        if (window_) ANativeWindow_release(window_);
    }
    bool onFrame(const hl::CompleteFrame& f, const hl::StreamStarted& info) override {
        std::lock_guard<std::mutex> lock(m_);
        if (!window_) return true;  // no surface right now: drop quietly
        if (!decoder_.running() || codec_ != info.codec || w_ != info.width || h_ != info.height) {
            if (!f.keyframe) return false;
            codec_ = info.codec;
            w_ = info.width;
            h_ = info.height;
            if (!decoder_.start(window_, info.codec, info.width, info.height, info.fps ? info.fps : fps_,
                                f.data.data(), f.data.size()))
                return false;
        }
        if (!decoder_.submit(f.data.data(), f.data.size(), f.keyframe)) {
            decoder_.stop();  // restart on the next keyframe
            return false;
        }
        return true;
    }
    Decoder::Stats takeStats() {
        std::lock_guard<std::mutex> lock(m_);
        return decoder_.takeStats();
    }

private:
    std::mutex m_;
    ANativeWindow* window_;
    int fps_;
    Decoder decoder_;
    uint8_t codec_ = 255;
    int w_ = 0, h_ = 0;
};

class JniClient : public hl::ClientListener {
public:
    JniClient(JNIEnv* e, jobject obj) : client(this) {
        self = e->NewGlobalRef(obj);
        jclass c = e->GetObjectClass(obj);
        onMonitorsId = e->GetMethodID(c, "onMonitors", "([I[Ljava/lang/String;)V");
        onStreamStartedId = e->GetMethodID(c, "onStreamStarted", "(IIIIIILjava/lang/String;)V");
        onStreamErrorId = e->GetMethodID(c, "onStreamError", "(ILjava/lang/String;)V");
        onCursorShapeId = e->GetMethodID(c, "onCursorShape", "(IIII[I)V");
        onCursorPosId = e->GetMethodID(c, "onCursorPos", "(IIIZ)V");
        onDisconnectedId = e->GetMethodID(c, "onDisconnected", "(Ljava/lang/String;)V");
        onWelcomeId = e->GetMethodID(c, "onWelcome", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;)V");
    }
    ~JniClient() override {
        client.disconnect();
        sinks.clear();
        env()->DeleteGlobalRef(self);
    }

    void onMonitors(const std::vector<hl::MonitorInfo>& mons) override {
        JNIEnv* e = env();
        // 7 ints per monitor: id, x, y, width, height, refreshHz, primary
        jintArray ints = e->NewIntArray((jsize)mons.size() * 7);
        std::vector<jint> v;
        for (auto& m : mons) {
            v.insert(v.end(), {(jint)m.id, m.x, m.y, (jint)m.width, (jint)m.height, m.refreshHz, m.primary});
        }
        e->SetIntArrayRegion(ints, 0, (jsize)v.size(), v.data());
        jobjectArray names = e->NewObjectArray((jsize)mons.size(), e->FindClass("java/lang/String"), nullptr);
        for (size_t i = 0; i < mons.size(); i++) {
            jstring s = e->NewStringUTF(mons[i].name.c_str());
            e->SetObjectArrayElement(names, (jsize)i, s);
            e->DeleteLocalRef(s);
        }
        e->CallVoidMethod(self, onMonitorsId, ints, names);
        e->DeleteLocalRef(ints);
        e->DeleteLocalRef(names);
        clear(e);
    }
    void onStreamStarted(const hl::StreamStarted& s) override {
        JNIEnv* e = env();
        jstring name = e->NewStringUTF(s.encoderName.c_str());
        e->CallVoidMethod(self, onStreamStartedId, s.streamId, (jint)s.monitorId, s.width, s.height, s.fps,
                          s.codec, name);
        e->DeleteLocalRef(name);
        clear(e);
    }
    void onStreamError(uint8_t id, const std::string& msg) override {
        JNIEnv* e = env();
        jstring m = e->NewStringUTF(msg.c_str());
        e->CallVoidMethod(self, onStreamErrorId, (jint)id, m);
        e->DeleteLocalRef(m);
        clear(e);
    }
    void onCursorShape(const hl::CursorShape& c) override {
        JNIEnv* e = env();
        size_t n = (size_t)c.width * c.height;
        std::vector<jint> argb(n);
        for (size_t i = 0; i < n; i++) {
            const uint8_t* p = &c.rgba[i * 4];
            argb[i] = (jint)((uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);
        }
        jintArray a = e->NewIntArray((jsize)n);
        e->SetIntArrayRegion(a, 0, (jsize)n, argb.data());
        e->CallVoidMethod(self, onCursorShapeId, c.width, c.height, c.hotX, c.hotY, a);
        e->DeleteLocalRef(a);
        clear(e);
    }
    void onCursorPos(uint32_t mon, uint16_t x, uint16_t y, bool visible) override {
        JNIEnv* e = env();
        e->CallVoidMethod(self, onCursorPosId, (jint)mon, (jint)x, (jint)y, (jboolean)visible);
        clear(e);
    }
    void onDisconnected(const std::string& reason) override {
        JNIEnv* e = env();
        jstring r = e->NewStringUTF(reason.c_str());
        e->CallVoidMethod(self, onDisconnectedId, r);
        e->DeleteLocalRef(r);
        clear(e);
    }
    void welcome(JNIEnv* e, const hl::Welcome& w) {
        jstring a = e->NewStringUTF(w.hostName.c_str());
        jstring b = e->NewStringUTF(w.hostId.c_str());
        jstring c = e->NewStringUTF(w.hostVersion.c_str());
        jstring rem = e->NewStringUTF(w.remoteAddresses.empty() ? "" : w.remoteAddresses[0].c_str());
        e->CallVoidMethod(self, onWelcomeId, a, b, c, (jint)w.codecMask, rem);
        e->DeleteLocalRef(rem);
        e->DeleteLocalRef(a);
        e->DeleteLocalRef(b);
        e->DeleteLocalRef(c);
        onMonitors(w.monitors);
    }

    static void clear(JNIEnv* e) {
        if (e->ExceptionCheck()) {
            e->ExceptionDescribe();
            e->ExceptionClear();
        }
    }

    hl::Client client;
    jobject self;
    std::mutex sinksMutex;
    std::map<int, std::shared_ptr<SurfaceSink>> sinks;
    jmethodID onMonitorsId, onStreamStartedId, onStreamErrorId, onCursorShapeId, onCursorPosId,
        onDisconnectedId, onWelcomeId;
};

JniClient* from(jlong h) { return reinterpret_cast<JniClient*>(h); }

std::shared_ptr<SurfaceSink> makeSink(JNIEnv* e, jobject surface, int fps) {
    ANativeWindow* w = surface ? ANativeWindow_fromSurface(e, surface) : nullptr;
    return std::make_shared<SurfaceSink>(w, fps);
}

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    gVm = vm;
    hl::setLogSink([](const std::string& s) { __android_log_write(ANDROID_LOG_INFO, "Hyperlink", s.c_str()); });
    return JNI_VERSION_1_6;
}

#define FN(name) Java_com_hyperlink_app_NativeClient_##name

JNIEXPORT jlong JNICALL FN(nativeCreate)(JNIEnv* e, jobject self) {
    return reinterpret_cast<jlong>(new JniClient(e, self));
}

JNIEXPORT void JNICALL FN(nativeDestroy)(JNIEnv*, jobject, jlong h) { delete from(h); }

JNIEXPORT jstring JNICALL FN(nativeConnect)(JNIEnv* e, jobject, jlong h, jstring host, jint port,
                                            jstring clientName, jstring clientId, jstring pin, jint w,
                                            jint hgt, jint hz, jint codecMask) {
    auto* c = from(h);
    hl::Hello hello;
    hello.clientName = str(e, clientName);
    hello.clientId = str(e, clientId);
    hello.pin = str(e, pin);
    hello.displayWidth = (uint16_t)w;
    hello.displayHeight = (uint16_t)hgt;
    hello.displayHz = (uint16_t)hz;
    hello.codecMask = (uint32_t)codecMask;
    hl::Welcome welcome;
    std::string err = c->client.connect(str(e, host), (uint16_t)port, hello, welcome);
    if (err.empty()) c->welcome(e, welcome);
    return e->NewStringUTF(err.c_str());
}

JNIEXPORT void JNICALL FN(nativeDisconnect)(JNIEnv*, jobject, jlong h) {
    auto* c = from(h);
    c->client.disconnect();
    std::lock_guard<std::mutex> lock(c->sinksMutex);
    c->sinks.clear();
}

JNIEXPORT void JNICALL FN(nativeStartStream)(JNIEnv* e, jobject, jlong h, jint streamId, jint monitorId,
                                             jobject surface, jint maxW, jint maxH, jint fps, jint kbps,
                                             jint codec, jint fec) {
    auto* c = from(h);
    auto sink = makeSink(e, surface, fps);
    {
        std::lock_guard<std::mutex> lock(c->sinksMutex);
        c->sinks[streamId] = sink;
    }
    hl::StartStream req;
    req.streamId = (uint8_t)streamId;
    req.monitorId = (uint32_t)monitorId;
    req.maxWidth = (uint16_t)maxW;
    req.maxHeight = (uint16_t)maxH;
    req.fps = (uint16_t)fps;
    req.bitrateKbps = (uint32_t)kbps;
    req.codec = (uint8_t)codec;
    req.fecPercent = (uint8_t)fec;
    c->client.startStream(req, sink);
}

JNIEXPORT void JNICALL FN(nativeSetSurface)(JNIEnv* e, jobject, jlong h, jint streamId, jobject surface,
                                            jint fps) {
    auto* c = from(h);
    auto sink = makeSink(e, surface, fps);
    {
        std::lock_guard<std::mutex> lock(c->sinksMutex);
        c->sinks[streamId] = sink;
    }
    c->client.setSink((uint8_t)streamId, sink);
}

JNIEXPORT void JNICALL FN(nativeStopStream)(JNIEnv*, jobject, jlong h, jint streamId) {
    auto* c = from(h);
    c->client.stopStream((uint8_t)streamId);
    std::lock_guard<std::mutex> lock(c->sinksMutex);
    c->sinks.erase(streamId);
}

JNIEXPORT void JNICALL FN(nativeMouseAbs)(JNIEnv*, jobject, jlong h, jint mon, jint x, jint y) {
    from(h)->client.mouseAbs((uint32_t)mon, (uint16_t)x, (uint16_t)y);
}
JNIEXPORT void JNICALL FN(nativeMouseRel)(JNIEnv*, jobject, jlong h, jint dx, jint dy) {
    from(h)->client.mouseRel(dx, dy);
}
JNIEXPORT void JNICALL FN(nativeMouseButton)(JNIEnv*, jobject, jlong h, jint b, jboolean down) {
    from(h)->client.mouseButton((uint8_t)b, down);
}
JNIEXPORT void JNICALL FN(nativeScroll)(JNIEnv*, jobject, jlong h, jint dy, jint dx) {
    from(h)->client.scroll(dy, dx);
}
JNIEXPORT void JNICALL FN(nativeKey)(JNIEnv*, jobject, jlong h, jint vk, jboolean down) {
    from(h)->client.key((uint16_t)vk, down);
}
JNIEXPORT void JNICALL FN(nativeText)(JNIEnv* e, jobject, jlong h, jstring t) {
    from(h)->client.text(str(e, t));
}

// [valid, width, height, codec, fps, mbps, loss%, framesLost, recovered, hostMs, assemblyMs,
//  decodeMs, decodedFps, rttMs, captureFps, encodeMs, droppedByDecoder]
JNIEXPORT jdoubleArray JNICALL FN(nativeStats)(JNIEnv* e, jobject, jlong h, jint streamId) {
    auto* c = from(h);
    hl::StreamStats s = c->client.takeStats((uint8_t)streamId);
    Decoder::Stats d;
    {
        std::lock_guard<std::mutex> lock(c->sinksMutex);
        auto it = c->sinks.find(streamId);
        if (it != c->sinks.end()) d = it->second->takeStats();
    }
    double v[] = {s.valid ? 1.0 : 0.0, (double)s.width, (double)s.height, (double)s.codec, s.fps, s.mbps,
                  s.lossPercent, (double)s.framesLost, (double)s.recovered, s.hostMs, s.assemblyMs,
                  d.framesDecoded ? d.decodeUsSum / 1000.0 / d.framesDecoded : 0.0, (double)d.framesDecoded,
                  c->client.rttMs(), (double)s.host.captureFps, s.host.encodeUsAvg / 1000.0, (double)d.dropped};
    jdoubleArray out = e->NewDoubleArray(17);
    e->SetDoubleArrayRegion(out, 0, 17, v);
    return out;
}

}  // extern "C"
