// JNI bridge between com.hyperlink.app.NativeHost and hl::HostCore (sharing this phone).
#include <jni.h>

#include <memory>
#include <string>

#include "hyperlink/common.h"
#include "hyperlink/hostcore.h"

JNIEnv* hlJniEnv();  // jni.cpp

namespace {

std::string jstr(JNIEnv* e, jstring s) {
    if (!s) return {};
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string out(c);
    e->ReleaseStringUTFChars(s, c);
    return out;
}

struct JniHost {
    jobject self = nullptr;
    jmethodID monitorInfo, startEncoder, stopEncoder, requestKeyframe, onInput, onStatus;
    std::unique_ptr<hl::HostCore> core;

    static void clear(JNIEnv* e) {
        if (e->ExceptionCheck()) {
            e->ExceptionDescribe();
            e->ExceptionClear();
        }
    }
};

}  // namespace

extern "C" {

#define FN(name) Java_com_hyperlink_app_NativeHost_##name

JNIEXPORT jlong JNICALL FN(nativeCreate)(JNIEnv* e, jobject self, jstring name, jstring pin, jstring hostId,
                                         jstring version, jint codecMask) {
    auto* h = new JniHost;
    h->self = e->NewGlobalRef(self);
    jclass c = e->GetObjectClass(self);
    h->monitorInfo = e->GetMethodID(c, "monitorInfo", "()[I");
    h->startEncoder = e->GetMethodID(c, "startEncoder", "(IIIII)V");
    h->stopEncoder = e->GetMethodID(c, "stopEncoder", "()V");
    h->requestKeyframe = e->GetMethodID(c, "requestKeyframe", "()V");
    h->onInput = e->GetMethodID(c, "onInput", "(IIIILjava/lang/String;)V");
    h->onStatus = e->GetMethodID(c, "onStatus", "()V");

    hl::HostCore::Config cfg;
    cfg.name = jstr(e, name);
    cfg.pin = jstr(e, pin);
    cfg.hostId = jstr(e, hostId);
    cfg.version = jstr(e, version);
    cfg.codecMask = (uint32_t)codecMask;
    std::string monitorName = cfg.name;

    hl::HostCore::Callbacks cb;
    cb.monitors = [h, monitorName] {
        JNIEnv* env = hlJniEnv();
        auto arr = (jintArray)env->CallObjectMethod(h->self, h->monitorInfo);
        JniHost::clear(env);
        std::vector<hl::MonitorInfo> out;
        if (!arr) return out;
        jint v[3] = {0, 0, 60};
        env->GetIntArrayRegion(arr, 0, 3, v);
        env->DeleteLocalRef(arr);
        hl::MonitorInfo m;
        m.id = 1;
        m.name = monitorName;
        m.width = (uint32_t)v[0];
        m.height = (uint32_t)v[1];
        m.refreshHz = (uint16_t)v[2];
        m.primary = 1;
        out.push_back(m);
        return out;
    };
    cb.startEncoder = [h](const hl::StartStream& r) {
        JNIEnv* env = hlJniEnv();
        env->CallVoidMethod(h->self, h->startEncoder, (jint)r.maxWidth, (jint)r.maxHeight, (jint)r.fps,
                            (jint)r.bitrateKbps, (jint)r.codec);
        JniHost::clear(env);
    };
    cb.stopEncoder = [h] {
        JNIEnv* env = hlJniEnv();
        env->CallVoidMethod(h->self, h->stopEncoder);
        JniHost::clear(env);
    };
    cb.requestKeyframe = [h] {
        JNIEnv* env = hlJniEnv();
        env->CallVoidMethod(h->self, h->requestKeyframe);
        JniHost::clear(env);
    };
    // Input arrives parsed into (type, a, b, c, text) so the Kotlin side stays simple.
    cb.input = [h](const std::vector<uint8_t>& body) {
        hl::Reader r(body.data() + 1, body.size() - 1);
        int type = body[0], a = 0, b = 0, c2 = 0;
        std::string text;
        switch (type) {
            case hl::MSG_MOUSE_ABS: r.u32(); a = r.u16(); b = r.u16(); break;
            case hl::MSG_MOUSE_REL: a = r.i16(); b = r.i16(); break;
            case hl::MSG_MOUSE_BUTTON: a = r.u8(); b = r.u8(); break;
            case hl::MSG_MOUSE_SCROLL: a = r.i16(); b = r.i16(); break;
            case hl::MSG_KEY: a = r.u16(); b = r.u8(); break;
            case hl::MSG_TEXT: text = r.str(); break;
        }
        if (!r.ok) return;
        JNIEnv* env = hlJniEnv();
        jstring t = text.empty() ? nullptr : env->NewStringUTF(text.c_str());
        env->CallVoidMethod(h->self, h->onInput, type, a, b, c2, t);
        if (t) env->DeleteLocalRef(t);
        JniHost::clear(env);
    };
    cb.statusChanged = [h] {
        JNIEnv* env = hlJniEnv();
        env->CallVoidMethod(h->self, h->onStatus);
        JniHost::clear(env);
    };
    h->core = std::make_unique<hl::HostCore>(cfg, cb);
    return reinterpret_cast<jlong>(h);
}

JNIEXPORT jboolean JNICALL FN(nativeStart)(JNIEnv*, jobject, jlong p) {
    return reinterpret_cast<JniHost*>(p)->core->start();
}

JNIEXPORT void JNICALL FN(nativeDestroy)(JNIEnv* e, jobject, jlong p) {
    auto* h = reinterpret_cast<JniHost*>(p);
    h->core->stop();
    h->core.reset();
    e->DeleteGlobalRef(h->self);
    delete h;
}

JNIEXPORT void JNICALL FN(nativeSetPin)(JNIEnv* e, jobject, jlong p, jstring pin) {
    reinterpret_cast<JniHost*>(p)->core->setPin(jstr(e, pin));
}

JNIEXPORT void JNICALL FN(nativeStreamReady)(JNIEnv* e, jobject, jlong p, jint w, jint h, jint fps, jint codec,
                                             jstring name) {
    reinterpret_cast<JniHost*>(p)->core->streamReady((uint16_t)w, (uint16_t)h, (uint16_t)fps, (uint8_t)codec,
                                                      jstr(e, name));
}

JNIEXPORT void JNICALL FN(nativeSendFrame)(JNIEnv* e, jobject, jlong p, jobject buffer, jint offset, jint size,
                                           jboolean key, jlong captureUs) {
    auto* data = static_cast<uint8_t*>(e->GetDirectBufferAddress(buffer));
    if (!data || size <= 0) return;
    reinterpret_cast<JniHost*>(p)->core->sendFrame(data + offset, (size_t)size, key, (uint64_t)captureUs);
}

JNIEXPORT void JNICALL FN(nativeMonitorsChanged)(JNIEnv*, jobject, jlong p) {
    reinterpret_cast<JniHost*>(p)->core->monitorsChanged();
}

JNIEXPORT jintArray JNICALL FN(nativeCounts)(JNIEnv* e, jobject, jlong p) {
    auto* h = reinterpret_cast<JniHost*>(p);
    jint v[2] = {h->core->clientCount(), h->core->streamCount()};
    jintArray a = e->NewIntArray(2);
    e->SetIntArrayRegion(a, 0, 2, v);
    return a;
}

JNIEXPORT jlong JNICALL FN(nativeNowUs)(JNIEnv*, jclass) { return (jlong)hl::nowUs(); }

}  // extern "C"
