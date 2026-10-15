#include "EngineHost.h"
#include <android/log.h>
#include <cstdio>
#include <engine/effect/EffectRegistry.h>
#include <engine/inputmod/InputModRegistry.h>
#include <engine/machine/MachineRegistry.h>
#include <jni.h>
#include <string>

#define LOG_TAG "Acidulous.JNI"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

std::string toStdString(JNIEnv *env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return {};
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

acidulous::EngineHost &host() {
    return acidulous::EngineHost::instance();
}

} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeStart(JNIEnv *, jobject) {
    return host().start() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeStop(JNIEnv *, jobject) {
    host().stop();
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeIsRunning(JNIEnv *, jobject) {
    return host().isRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMountMachine(JNIEnv *env, jobject,
                                                      jint rackId, jstring typeName) {
    return host().mountMachine(rackId, toStdString(env, typeName)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeUnmountMachine(JNIEnv *, jobject, jint rackId) {
    host().unmountMachine(rackId);
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMountEffect(JNIEnv *env, jobject, jint rackId, jint slot, jstring typeName) {
    return host().mountEffect(rackId, slot, toStdString(env, typeName)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMountSend(JNIEnv *env, jobject, jint slot, jstring typeName) {
    return host().mountSend(slot, toStdString(env, typeName)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloatArray JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoudness(JNIEnv *env, jobject) {
    float v[4];
    host().loudness(v);
    jfloatArray out = env->NewFloatArray(4);
    env->SetFloatArrayRegion(out, 0, 4, v);
    return out;
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeResetLoudness(JNIEnv *, jobject) { host().resetLoudness(); }

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMountGroupInsert(JNIEnv *env, jobject, jint group, jint slot,
                                                                jstring typeName) {
    return host().mountGroupInsert(group, slot, toStdString(env, typeName)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeGroupPeak(JNIEnv *, jobject, jint group) {
    return host().groupPeak(group);
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMountMasterInsert(JNIEnv *env, jobject, jint slot, jstring typeName) {
    return host().mountMasterInsert(slot, toStdString(env, typeName)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMountInputEffect(JNIEnv *env, jobject, jint slot,
                                                                  jstring typeName) {
    return host().mountInputEffect(slot, toStdString(env, typeName)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeControlChange(JNIEnv *, jobject, jint rackId, jint cc, jint value,
                                                              jboolean record) {
    host().controlChange(rackId, static_cast<uint8_t>(cc), static_cast<uint8_t>(value), record == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeChannelPressure(JNIEnv *, jobject, jint rackId, jint value,
                                                                jboolean record) {
    host().channelPressure(rackId, static_cast<uint8_t>(value), record == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativePrewarm(JNIEnv *, jobject) { acidulous::EngineHost::prewarm(); }

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMountInputMod(JNIEnv *env, jobject, jint rackId, jint slot, jstring typeName) {
    return host().mountInputMod(rackId, slot, toStdString(env, typeName)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeInputModTypes(JNIEnv *env, jobject) {
    const int32_t n = acidulous::InputModRegistry::count();
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray(n, stringClass, nullptr);
    for (int32_t i = 0; i < n; ++i) {
        jstring s = env->NewStringUTF(acidulous::InputModRegistry::name(i));
        env->SetObjectArrayElement(out, i, s);
        env->DeleteLocalRef(s);
    }
    return out;
}

JNIEXPORT jobjectArray JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeEffectTypes(JNIEnv *env, jobject) {
    const int32_t n = acidulous::EffectRegistry::count();
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray(n, stringClass, nullptr);
    for (int32_t i = 0; i < n; ++i) {
        jstring s = env->NewStringUTF(acidulous::EffectRegistry::name(i));
        env->SetObjectArrayElement(out, i, s);
        env->DeleteLocalRef(s);
    }
    return out;
}

JNIEXPORT jobjectArray JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMachineTypes(JNIEnv *env, jobject) {
    const int32_t n = acidulous::MachineRegistry::count();
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray(n, stringClass, nullptr);
    for (int32_t i = 0; i < n; ++i) {
        jstring s = env->NewStringUTF(acidulous::MachineRegistry::name(i));
        env->SetObjectArrayElement(out, i, s);
        env->DeleteLocalRef(s);
    }
    return out;
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSoundFontPresets(JNIEnv *env, jobject, jstring path) {
    std::string error;
    const std::string list = acidulous::EngineHost::soundFontPresets(toStdString(env, path), error);
    return env->NewStringUTF(error.empty() ? list.c_str() : ("!" + error).c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoadSoundFont(JNIEnv *env, jobject, jint rackId, jstring path,
                                                              jint presetIndex) {
    std::string error;
    host().loadSoundFont(rackId, toStdString(env, path), presetIndex, error);
    return env->NewStringUTF(error.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoadZoneMap(JNIEnv *env, jobject, jint rackId, jstring spec,
                                                            jstring name) {
    std::string error;
    host().loadZoneMap(rackId, toStdString(env, spec), toStdString(env, name), error);
    return env->NewStringUTF(error.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSampleMapInfo(JNIEnv *env, jobject, jint rackId) {
    return env->NewStringUTF(host().sampleMapInfo(rackId).c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoadSample(JNIEnv *env, jobject, jint rackId, jint slot, jstring path,
                                                          jint maxSeconds) {
    std::string error;
    const bool ok = host().loadSample(rackId, slot, toStdString(env, path), error, maxSeconds);
    return env->NewStringUTF(ok ? "" : error.c_str()); // empty = success
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSampleInfo(JNIEnv *env, jobject, jint rackId, jint slot) {
    return env->NewStringUTF(host().sampleInfo(rackId, slot).c_str());
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSampleShape(JNIEnv *env, jobject, jint rack, jint pad,
                                                           jfloatArray out, jint fromFrame, jint toFrame) {
    const jsize max = env->GetArrayLength(out);
    if (max < 2) return 0;
    jfloat *data = env->GetFloatArrayElements(out, nullptr);
    const int32_t n = host().sampleShape(rack, pad, data, static_cast<int32_t>(max / 2), fromFrame, toFrame);
    env->ReleaseFloatArrayElements(out, data, 0);
    return n;
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeImportAudio(JNIEnv *env, jobject, jstring path, jint maxSeconds) {
    const char *p = env->GetStringUTFChars(path, nullptr);
    std::string error;
    const std::string out = host().importAudio(p != nullptr ? p : "", error, maxSeconds);
    env->ReleaseStringUTFChars(path, p);
    // Two lines, "ok", "cut" or "err" then the path or the reason. See
    // EngineHost::importAudio.
    return env->NewStringUTF(out.empty() ? ("err\n" + error).c_str() : out.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSlicePoints(JNIEnv *env, jobject, jstring path, jint mode,
                                                           jint count) {
    const char *p = env->GetStringUTFChars(path, nullptr);
    std::string error;
    const std::string out = host().slicePoints(p != nullptr ? p : "", mode, count, error);
    env->ReleaseStringUTFChars(path, p);
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoopShape(JNIEnv *env, jobject, jstring path) {
    const char *p = env->GetStringUTFChars(path, nullptr);
    std::string error;
    const std::string out = host().loopShape(p != nullptr ? p : "", error);
    env->ReleaseStringUTFChars(path, p);
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeNoteOn(JNIEnv *, jobject,
                                                jint rackId, jint note, jint velocity) {
    host().noteOn(rackId, static_cast<uint8_t>(note & 0x7f),
                  static_cast<uint8_t>(velocity & 0x7f));
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeNoteOff(JNIEnv *, jobject, jint rackId, jint note) {
    host().noteOff(rackId, static_cast<uint8_t>(note & 0x7f));
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativePanic(JNIEnv *, jobject) { host().panic(); }

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoadNexusPatch(JNIEnv *env, jobject, jint rack, jstring spec) {
    const char *chars = env->GetStringUTFChars(spec, nullptr);
    const std::string result = host().loadNexusPatch(rack, chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(spec, chars);
    return env->NewStringUTF(result.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeNexusPalette(JNIEnv *env, jobject) {
    return env->NewStringUTF(host().nexusPalette().c_str());
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeNexusScope(JNIEnv *env, jobject, jint rack, jfloatArray out) {
    const jsize max = env->GetArrayLength(out);
    if (max <= 0) return 0;
    jfloat *data = env->GetFloatArrayElements(out, nullptr);
    const int32_t n = host().nexusScope(rack, data, static_cast<int32_t>(max));
    env->ReleaseFloatArrayElements(out, data, 0);
    return n;
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeNexusActivity(JNIEnv *env, jobject, jint rack, jfloatArray out) {
    const jsize max = env->GetArrayLength(out);
    if (max <= 0) return 0;
    jfloat *data = env->GetFloatArrayElements(out, nullptr);
    const int32_t n = host().nexusActivity(rack, data, static_cast<int32_t>(max));
    env->ReleaseFloatArrayElements(out, data, 0);
    return n;
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeStartInput(JNIEnv *, jobject, jint deviceId) {
    return host().startInput(deviceId) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeStopInput(JNIEnv *, jobject) {
    return host().stopInput() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetInputClean(JNIEnv *, jobject, jboolean on) {
    host().setInputClean(on == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeInputSession(JNIEnv *, jobject) {
    return host().inputSession();
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeInputChannels(JNIEnv *, jobject) {
    return host().inputChannels();
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeInputRate(JNIEnv *, jobject) { return host().inputRate(); }

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeInputDevice(JNIEnv *, jobject) {
    return host().inputDevice();
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeInputRunning(JNIEnv *, jobject) {
    return host().inputRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeInputPeak(JNIEnv *, jobject) { return host().inputPeak(); }

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetInputGain(JNIEnv *, jobject, jfloat g) {
    host().setInputGain(g);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetSwingUnit(JNIEnv *, jobject, jint unit) {
    host().setSwingUnit(unit);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetTunerOn(JNIEnv *, jobject, jboolean on) {
    host().setTunerOn(on == JNI_TRUE);
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeTunerHz(JNIEnv *, jobject) { return host().tunerHz(); }

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetMonitorLevel(JNIEnv *, jobject, jfloat level) {
    host().setMonitorLevel(level);
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeStartCapture(JNIEnv *env, jobject, jstring path, jint source) {
    const char *chars = env->GetStringUTFChars(path, nullptr);
    const std::string result = host().startCapture(chars ? chars : "", source);
    if (chars) env->ReleaseStringUTFChars(path, chars);
    return env->NewStringUTF(result.c_str());
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeStopCapture(JNIEnv *, jobject) { host().stopCapture(); }

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCapturing(JNIEnv *, jobject) {
    return host().capturing() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCapturedSeconds(JNIEnv *, jobject) {
    return host().capturedSeconds();
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCapturedPeak(JNIEnv *, jobject) { return host().capturedPeak(); }

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCaptureOverflowed(JNIEnv *, jobject) {
    return host().captureOverflowed() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCaptureDeaf(JNIEnv *, jobject) {
    return host().captureDeaf() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeFileShape(JNIEnv *env, jobject, jstring path,
                                                          jfloatArray out, jint fromFrame,
                                                          jint toFrame) {
    const jsize max = env->GetArrayLength(out);
    if (max < 2) return 0;
    const char *p = env->GetStringUTFChars(path, nullptr);
    jfloat *data = env->GetFloatArrayElements(out, nullptr);
    const int32_t n = host().fileShape(p != nullptr ? p : "", data, static_cast<int32_t>(max / 2),
                                       fromFrame, toFrame);
    env->ReleaseFloatArrayElements(out, data, 0);
    if (p != nullptr) env->ReleaseStringUTFChars(path, p);
    return n;
}

JNIEXPORT jlong JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCapturedFrames(JNIEnv *, jobject) {
    return host().capturedFrames();
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCompCell(JNIEnv *env, jobject, jint rack,
                                                         jlong sceneId, jint frames, jfloat bpm,
                                                         jstring path, jfloatArray peakOut) {
    const char *p = path != nullptr ? env->GetStringUTFChars(path, nullptr) : "";
    float peak = 0.0f;
    const std::string out = host().compCell(rack, sceneId, frames, bpm, p, peak);
    if (path != nullptr) env->ReleaseStringUTFChars(path, p);
    if (peakOut != nullptr && env->GetArrayLength(peakOut) > 0) {
        env->SetFloatArrayRegion(peakOut, 0, 1, &peak);
    }
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeArmCapture(JNIEnv *, jobject, jint rack) {
    host().armCapture(rack);
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCaptureMarks(JNIEnv *env, jobject, jlongArray out) {
    const jsize max = env->GetArrayLength(out) / 5;
    if (max <= 0) return 0;
    jlong *data = env->GetLongArrayElements(out, nullptr);
    const int32_t n = host().captureMarks(reinterpret_cast<int64_t *>(data), max);
    env->ReleaseLongArrayElements(out, data, 0);
    return n;
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCutTake(JNIEnv *env, jobject, jstring path, jint kind, jfloat noteHz,
                                                        jfloat consonantNear) {
    const char *p = env->GetStringUTFChars(path, nullptr);
    const std::string out = host().cutTake(p != nullptr ? p : "", kind, noteHz, consonantNear);
    if (p != nullptr) env->ReleaseStringUTFChars(path, p);
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeFileSurvey(JNIEnv *env, jobject, jstring path,
                                                           jfloatArray out) {
    const jsize max = env->GetArrayLength(out);
    if (max < 2) return env->NewStringUTF("");
    const char *p = env->GetStringUTFChars(path, nullptr);
    jfloat *data = env->GetFloatArrayElements(out, nullptr);
    const std::string info =
        host().fileSurvey(p != nullptr ? p : "", data, static_cast<int32_t>(max / 2));
    env->ReleaseFloatArrayElements(out, data, 0);
    if (p != nullptr) env->ReleaseStringUTFChars(path, p);
    return env->NewStringUTF(info.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeAuditionFile(JNIEnv *env, jobject, jstring path) {
    const char *p = env->GetStringUTFChars(path, nullptr);
    const std::string out = host().auditionFile(p != nullptr ? p : "");
    if (p != nullptr) env->ReleaseStringUTFChars(path, p);
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeAuditioning(JNIEnv *, jobject) {
    return host().auditioning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeAuditionProgress(JNIEnv *, jobject) {
    return host().auditionProgress();
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeFileInfo(JNIEnv *env, jobject, jstring path) {
    const char *p = env->GetStringUTFChars(path, nullptr);
    const std::string out = host().fileInfo(p != nullptr ? p : "");
    if (p != nullptr) env->ReleaseStringUTFChars(path, p);
    return env->NewStringUTF(out.c_str());
}

/**
 * The edit as one flat array instead of a dozen float arguments, which would
 * be easy to swap by mistake. The order is fixed on both sides (see SampleOps
 * and NativeEngine.editSample). False if the array is short.
 */
static bool editOpsOf(JNIEnv *env, jfloatArray opsArray, acidulous::audio::SampleOps &ops) {
    const jsize n = env->GetArrayLength(opsArray);
    if (n < 14) return false;
    jfloat *o = env->GetFloatArrayElements(opsArray, nullptr);
    ops.from = static_cast<int32_t>(o[0]);
    ops.to = static_cast<int32_t>(o[1]);
    ops.fadeInMs = o[2];
    ops.fadeOutMs = o[3];
    ops.gainDb = o[4];
    ops.normaliseTo = o[5];
    ops.reverse = o[6] >= 0.5f;
    ops.lowCutHz = o[7];
    ops.cutoffHz = o[8];
    ops.resonance = o[9];
    ops.filterType = static_cast<int32_t>(o[10]);
    ops.squash = o[11];
    ops.squashAttackMs = o[12];
    ops.squashReleaseMs = o[13];
    env->ReleaseFloatArrayElements(opsArray, o, JNI_ABORT);
    return true;
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeEditSample(JNIEnv *env, jobject, jstring src,
                                                           jstring dst, jfloatArray opsArray) {
    acidulous::audio::SampleOps ops;
    if (!editOpsOf(env, opsArray, ops)) return env->NewStringUTF("the edit is incomplete");

    const char *a = env->GetStringUTFChars(src, nullptr);
    const char *b = env->GetStringUTFChars(dst, nullptr);
    const std::string out = host().editSample(a != nullptr ? a : "", b != nullptr ? b : "", ops);
    if (a != nullptr) env->ReleaseStringUTFChars(src, a);
    if (b != nullptr) env->ReleaseStringUTFChars(dst, b);
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeEditPreview(JNIEnv *env, jobject, jstring src,
                                                            jfloatArray opsArray, jfloatArray out,
                                                            jint fromFrame, jint toFrame) {
    acidulous::audio::SampleOps ops;
    const jsize max = env->GetArrayLength(out);
    if (max < 2 || !editOpsOf(env, opsArray, ops)) return 0;
    const char *p = env->GetStringUTFChars(src, nullptr);
    jfloat *data = env->GetFloatArrayElements(out, nullptr);
    const int32_t n = host().editPreview(p != nullptr ? p : "", ops, data, static_cast<int32_t>(max / 2),
                                         fromFrame, toFrame);
    env->ReleaseFloatArrayElements(out, data, 0);
    if (p != nullptr) env->ReleaseStringUTFChars(src, p);
    return n;
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeAuditionPreview(JNIEnv *env, jobject) {
    return env->NewStringUTF(host().auditionPreview().c_str());
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMidiEvent(JNIEnv *, jobject, jint rackId, jint status,
                                                   jint d1, jint d2, jint channel) {
    host().midiEvent(rackId, static_cast<uint8_t>(status), static_cast<uint8_t>(d1 & 0x7f),
                     static_cast<uint8_t>(d2 & 0x7f), static_cast<uint8_t>(channel));
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetMpeZone(JNIEnv *, jobject, jint kind,
                                                   jint members, jfloat bendSemis) {
    host().setMpeZone(kind, members, bendSemis);
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMpeHeldMask(JNIEnv *, jobject) {
    return host().mpeHeldMask();
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetParam(JNIEnv *env, jobject, jint rackId,
                                                  jstring unit, jstring name, jfloat value, jboolean record,
                                                  jint quantise) {
    return host().setParam(rackId, toStdString(env, unit), toStdString(env, name), value, record == JNI_TRUE,
                           quantise) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeGetSampleRate(JNIEnv *, jobject) {
    return host().sampleRate();
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetCacheRoot(JNIEnv *env, jobject, jstring path) {
    const char *p = path != nullptr ? env->GetStringUTFChars(path, nullptr) : "";
    host().setCacheRoot(p);
    if (path != nullptr) env->ReleaseStringUTFChars(path, p);
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoadReel(JNIEnv *env, jobject, jint rack, jstring spec) {
    const char *p = spec != nullptr ? env->GetStringUTFChars(spec, nullptr) : "";
    const std::string out = host().loadReel(rack, p);
    if (spec != nullptr) env->ReleaseStringUTFChars(spec, p);
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoadTake(JNIEnv *env, jobject, jint rack, jstring path) {
    const char *p = path != nullptr ? env->GetStringUTFChars(path, nullptr) : "";
    const std::string out = host().loadTake(rack, p);
    if (path != nullptr) env->ReleaseStringUTFChars(path, p);
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoadVoice(JNIEnv *env, jobject, jint rack, jint slot, jstring spec) {
    const char *p = env->GetStringUTFChars(spec, nullptr);
    const std::string out = host().loadVoice(rack, slot, p != nullptr ? p : "");
    if (p != nullptr) env->ReleaseStringUTFChars(spec, p);
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoadUtterance(JNIEnv *env, jobject, jint rack, jstring path) {
    const char *p = path != nullptr ? env->GetStringUTFChars(path, nullptr) : "";
    const std::string out = host().loadUtterance(rack, p);
    if (path != nullptr) env->ReleaseStringUTFChars(path, p);
    return env->NewStringUTF(out.c_str());
}



JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoadFormula(JNIEnv *env, jobject, jint rack, jstring formula,
                                                            jstring arp, jstring duty, jstring vol) {
    auto str = [&](jstring s) {
        if (s == nullptr) return std::string();
        const char *p = env->GetStringUTFChars(s, nullptr);
        std::string out(p);
        env->ReleaseStringUTFChars(s, p);
        return out;
    };
    return env->NewStringUTF(host().loadFormula(rack, str(formula), str(arp), str(duty), str(vol)).c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoadEffectFormula(JNIEnv *env, jobject, jint rack, jstring unit,
                                                                  jstring formula) {
    auto str = [&](jstring s) {
        if (s == nullptr) return std::string();
        const char *p = env->GetStringUTFChars(s, nullptr);
        std::string out(p);
        env->ReleaseStringUTFChars(s, p);
        return out;
    };
    return env->NewStringUTF(host().loadEffectFormula(rack, str(unit), str(formula)).c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeBuildCloud(JNIEnv *env, jobject, jint rack, jfloatArray spectrum) {
    std::vector<jfloat> v;
    if (spectrum != nullptr) {
        const jsize n = env->GetArrayLength(spectrum);
        v.resize(static_cast<size_t>(n));
        if (n > 0) env->GetFloatArrayRegion(spectrum, 0, n, v.data());
    }
    return env->NewStringUTF(host().buildCloud(rack, v.empty() ? nullptr : v.data(),
                                               static_cast<int32_t>(v.size())).c_str());
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeFreezeClip(JNIEnv *env, jobject, jint rack, jlong sceneId,
                                                           jstring path, jfloat tailSeconds) {
    const char *p = env->GetStringUTFChars(path, nullptr);
    int32_t frames = 0, tail = 0, ticks = 0;
    float bpm = 0.0f, peak = 0.0f;
    const std::string err = host().freezeClip(rack, sceneId, p, tailSeconds, frames, tail, ticks, bpm, peak);
    env->ReleaseStringUTFChars(path, p);
    // One string so the values can't get out of step:
    // "ok|frames|ticks|bpm|peak|tail", or the reason it failed.
    char out[160];
    if (err.empty()) {
        std::snprintf(out, sizeof(out), "ok|%d|%d|%.6f|%.6f|%d", frames, ticks, bpm, peak, tail);
    } else {
        std::snprintf(out, sizeof(out), "%s", err.c_str());
    }
    return env->NewStringUTF(out);
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLoadFrozen(JNIEnv *env, jobject, jint rack, jlongArray sceneIds,
                                                           jobjectArray paths, jfloatArray bpms, jintArray ticks,
                                                           jintArray tails) {
    const jsize n = env->GetArrayLength(sceneIds);
    std::vector<std::pair<int64_t, std::string>> clips;
    std::vector<float> bpmList;
    std::vector<int32_t> tickList;
    std::vector<int32_t> tailList;
    std::vector<jlong> ids(static_cast<size_t>(n));
    if (n > 0) env->GetLongArrayRegion(sceneIds, 0, n, ids.data());
    std::vector<jfloat> bs(static_cast<size_t>(n));
    if (n > 0) env->GetFloatArrayRegion(bpms, 0, n, bs.data());
    std::vector<jint> ts(static_cast<size_t>(n));
    if (n > 0) env->GetIntArrayRegion(ticks, 0, n, ts.data());
    std::vector<jint> tl(static_cast<size_t>(n));
    if (n > 0) env->GetIntArrayRegion(tails, 0, n, tl.data());
    for (jsize i = 0; i < n; ++i) {
        auto str = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
        const char *p = env->GetStringUTFChars(str, nullptr);
        clips.emplace_back(static_cast<int64_t>(ids[static_cast<size_t>(i)]), std::string(p));
        env->ReleaseStringUTFChars(str, p);
        env->DeleteLocalRef(str);
        bpmList.push_back(bs[static_cast<size_t>(i)]);
        tickList.push_back(ts[static_cast<size_t>(i)]);
        tailList.push_back(tl[static_cast<size_t>(i)]);
    }
    return env->NewStringUTF(host().loadFrozenSet(rack, clips, bpmList, tickList, tailList).c_str());
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetBufferBursts(JNIEnv *, jobject, jint bursts) {
    host().setBufferBursts(bursts);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetBufferFloor(JNIEnv *, jobject, jint frames) {
    host().setBufferFloor(frames);
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeBufferFloor(JNIEnv *, jobject) {
    return host().bufferFloor();
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeBufferFrames(JNIEnv *, jobject) {
    return host().bufferFrames();
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetVoiceLimit(JNIEnv *, jobject, jint notes) {
    host().setVoiceLimit(notes);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetQuality(JNIEnv *, jobject, jint level) {
    host().setQuality(level);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetRecordBits(JNIEnv *, jobject, jint bits) {
    host().setRecordBits(bits);
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeGetFramesPerBurst(JNIEnv *, jobject) {
    return host().framesPerBurst();
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeIsLowLatency(JNIEnv *, jobject) {
    return host().lowLatency() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeGetXRunCount(JNIEnv *, jobject) {
    return static_cast<jlong>(host().xRunCount());
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeGetLoadAvg(JNIEnv *, jobject) {
    return host().loadPercent();
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeWorstBlockUs(JNIEnv *, jobject) {
    return host().worstBlockUs();
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeWorstCallbackUs(JNIEnv *, jobject) {
    return host().worstCallbackUs();
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeWorstPhaseUs(JNIEnv *, jobject, jint phase) {
    return host().worstPhaseUs(phase);
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeWorstRackUs(JNIEnv *, jobject, jint rack) {
    return host().worstRackUs(rack);
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeRackPercentileUs(JNIEnv *, jobject, jint rack) {
    return host().rackPercentileUs(rack);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeResetRackCosts(JNIEnv *, jobject) {
    host().resetRackCosts();
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeWorstRackWasFrozen(JNIEnv *, jobject, jint rack) {
    return host().worstRackWasFrozen(rack) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeInterruptedPercent(JNIEnv *, jobject) {
    return host().interruptedPercent();
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeHintRunning(JNIEnv *, jobject) {
    return host().hintRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeHintAvailable(JNIEnv *, jobject) {
    return host().hintAvailable() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeHintState(JNIEnv *, jobject) {
    return host().hintState();
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeFastCores(JNIEnv *, jobject) {
    return host().fastCores();
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeRecentCallbackUs(JNIEnv *, jobject) {
    return host().recentCallbackUs();
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeRackCostUs(JNIEnv *, jobject, jint rack) {
    return host().rackCostUs(rack);
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeWorstCallbackCpuUs(JNIEnv *, jobject) {
    return host().worstCallbackCpuUs();
}

JNIEXPORT jlong JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLateCallbacks(JNIEnv *, jobject) {
    return host().lateCallbacks();
}

JNIEXPORT jlong JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeStalledCallbacks(JNIEnv *, jobject) {
    return host().stalledCallbacks();
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCallbackBudgetUs(JNIEnv *, jobject) {
    return host().callbackBudgetUs();
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeReadPeakLevel(JNIEnv *, jobject) {
    return host().peakLevel();
}

// --- Transport ---------------------------------------------------------------

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeTransportPlay(JNIEnv *, jobject, jint sceneIdx) {
    host().transportPlay(sceneIdx);
}

JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeRenderSong(JNIEnv *env, jobject, jstring path, jfloat tailSeconds,
                                                           jint format, jint bits, jint startScene, jfloat maxSeconds) {
    std::string error;
    const bool ok = host().renderSong(toStdString(env, path), tailSeconds, static_cast<acidulous::AudioFormat>(format),
                                      bits, error, startScene, maxSeconds);
    return env->NewStringUTF(ok ? "" : error.c_str());
}

/** Paths and the rack each belongs to, in step; a rack of -1 is the mix. */
JNIEXPORT jstring JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeRenderStems(JNIEnv *env, jobject, jobjectArray paths,
                                                            jintArray racks, jfloat tailSeconds, jint format,
                                                            jint bits, jint startScene, jfloat maxSeconds) {
    const jsize count = env->GetArrayLength(paths);
    std::vector<acidulous::EngineHost::RenderTarget> targets;
    targets.reserve(static_cast<size_t>(count));
    jint *rackIds = env->GetIntArrayElements(racks, nullptr);
    for (jsize i = 0; i < count; ++i) {
        auto path = reinterpret_cast<jstring>(env->GetObjectArrayElement(paths, i));
        targets.push_back({toStdString(env, path), rackIds[i]});
        env->DeleteLocalRef(path);
    }
    env->ReleaseIntArrayElements(racks, rackIds, JNI_ABORT);
    std::string error;
    const bool ok = host().renderStems(targets, tailSeconds, static_cast<acidulous::AudioFormat>(format), bits, error,
                                       startScene, maxSeconds);
    return env->NewStringUTF(ok ? "" : error.c_str());
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCancelRender(JNIEnv *, jobject) { host().cancelRender(); }

/** [integrated LUFS, true peak dBTP], or null with the error left in the log. */
JNIEXPORT jfloatArray JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMeasureLoudness(JNIEnv *env, jobject, jfloat tailSeconds, jint startScene,
                                                               jfloat maxSeconds) {
    float lufs = 0.0f, tp = 0.0f;
    std::string error;
    if (!host().measureLoudness(tailSeconds, startScene, maxSeconds, lufs, tp, error)) return nullptr;
    const float v[2] = {lufs, tp};
    jfloatArray out = env->NewFloatArray(2);
    env->SetFloatArrayRegion(out, 0, 2, v);
    return out;
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetRenderGain(JNIEnv *, jobject, jfloat db) { host().setRenderGain(db); }

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetCountInBars(JNIEnv *, jobject, jint bars) {
    host().setCountInBars(bars);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetRecordModes(JNIEnv *, jobject, jboolean onNote, jboolean once) {
    host().setRecordModes(onNote, once);
}

JNIEXPORT jlong JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCountInRemaining(JNIEnv *, jobject) {
    return static_cast<jlong>(host().countInRemaining());
}

JNIEXPORT jlong JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeElapsedMs(JNIEnv *, jobject) {
    return static_cast<jlong>(host().elapsedMs());
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetCores(JNIEnv *, jobject, jint cores) {
    host().setCores(cores);
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCoresInUse(JNIEnv *, jobject) {
    return host().coresInUse();
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCoresMax(JNIEnv *, jobject) {
    return host().coresMax();
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeReadWorkerWaitPeakUs(JNIEnv *, jobject) {
    return host().readWorkerWaitPeakUs();
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeIsRendering(JNIEnv *, jobject) { return host().isRendering() ? JNI_TRUE : JNI_FALSE; }

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeRenderedSeconds(JNIEnv *, jobject) { return host().renderedSeconds(); }

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeRenderedPeak(JNIEnv *, jobject) { return host().renderedPeak(); }

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetStopAtEnd(JNIEnv *, jobject, jboolean on) {
    host().setStopAtEnd(on == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeIsStopAtEndArmed(JNIEnv *, jobject) {
    return host().isStopAtEndArmed() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeQueueScene(JNIEnv *, jobject, jint idx) { host().queueScene(idx); }

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeQueuedScene(JNIEnv *, jobject) { return host().queuedScene(); }

// --- Clip mode ---------------------------------------------------------------

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetLauncher(JNIEnv *, jobject, jboolean on) {
    host().setLauncher(on == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetFill(JNIEnv *, jobject, jboolean on) {
    host().setFill(on == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetLaunchQuantise(JNIEnv *, jobject, jint ticks) {
    host().setLaunchQuantise(ticks);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLaunchClip(JNIEnv *, jobject, jint rack, jlong sceneId) {
    host().launchClip(rack, sceneId);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLaunchScene(JNIEnv *, jobject, jlong sceneId) {
    host().launchScene(sceneId);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeStopAllClips(JNIEnv *, jobject) { host().stopAllClips(); }

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeCancelLaunch(JNIEnv *, jobject, jint rack) {
    host().cancelLaunch(rack);
}

// --- MIDI out ----------------------------------------------------------------

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetClockOut(JNIEnv *, jobject, jboolean on) {
    host().setClockOut(on == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetExternalSync(JNIEnv *, jobject, jboolean on) {
    host().setExternalSync(on == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetTuning(JNIEnv *env, jobject, jint rack, jfloatArray ratios) {
    if (ratios == nullptr || env->GetArrayLength(ratios) < 128) {
        host().setTuning(rack, nullptr);
        return;
    }
    jfloat buf[128];
    env->GetFloatArrayRegion(ratios, 0, 128, buf);
    host().setTuning(rack, buf);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetLink(JNIEnv *, jobject, jboolean on) {
    host().setLinkEnabled(on == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLinkEnabled(JNIEnv *, jobject) {
    return host().linkEnabled() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetLinkStartStop(JNIEnv *, jobject, jboolean on) {
    host().setLinkStartStop(on == JNI_TRUE);
}

JNIEXPORT jlong JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLinkStatus(JNIEnv *, jobject) {
    return static_cast<jlong>(host().linkStatus());
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMidiClockIn(JNIEnv *, jobject, jlong frame, jint status, jint d1, jint d2) {
    host().midiClockIn(frame, static_cast<uint8_t>(status), static_cast<uint8_t>(d1), static_cast<uint8_t>(d2));
}

JNIEXPORT jlong JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSyncState(JNIEnv *, jobject) { return host().syncState(); }

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeDrainMidiOut(JNIEnv *env, jobject, jlongArray out) {
    const jsize cap = env->GetArrayLength(out) / 2;
    if (cap <= 0) return 0;
    jlong *data = env->GetLongArrayElements(out, nullptr);
    const int n = host().drainMidiOut(reinterpret_cast<int64_t *>(data), static_cast<int>(cap));
    env->ReleaseLongArrayElements(out, data, 0);
    return n;
}

/** Frame, nanoseconds, sample rate, or a zero frame when the stream doesn't know yet. */
JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeAudioAnchor(JNIEnv *env, jobject, jlongArray out) {
    if (env->GetArrayLength(out) < 3) return;
    int64_t frame = 0, nanos = 0;
    int32_t rate = acidulous::kSampleRate;
    const bool ok = host().audioAnchor(frame, nanos, rate);
    jlong v[3] = {ok ? frame : -1, nanos, rate};
    env->SetLongArrayRegion(out, 0, 3, v);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeLaunchStates(JNIEnv *env, jobject, jlongArray out) {
    const jsize n = env->GetArrayLength(out);
    int64_t packed[acidulous::kRackCount] = {};
    host().launchStates(packed, static_cast<int32_t>(n));
    env->SetLongArrayRegion(out, 0, n < acidulous::kRackCount ? n : acidulous::kRackCount,
                            reinterpret_cast<const jlong *>(packed));
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeTransportStop(JNIEnv *, jobject) { host().transportStop(); }

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeTransportRewind(JNIEnv *, jobject) { host().transportRewind(); }

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeIsPlaying(JNIEnv *, jobject) {
    return host().isPlaying() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetLoopScene(JNIEnv *, jobject, jboolean on) {
    host().setLoopScene(on == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetLoopSong(JNIEnv *, jobject, jboolean on) {
    host().setLoopSong(on == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetRecordArmed(JNIEnv *, jobject, jboolean on) {
    host().setRecordArmed(on == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeIsRecordArmed(JNIEnv *, jobject) {
    return host().isRecordArmed() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeDrainRecorded(JNIEnv *env, jobject, jlongArray out) {
    if (out == nullptr) {
        return 0;
    }
    const jsize len = env->GetArrayLength(out);
    const int maxEvents = static_cast<int>(len / 5);
    if (maxEvents <= 0) {
        return 0;
    }
    jlong *data = env->GetLongArrayElements(out, nullptr);
    if (data == nullptr) {
        return 0;
    }
    const int n = host().drainRecorded(reinterpret_cast<int64_t *>(data), maxEvents);
    env->ReleaseLongArrayElements(out, data, 0); // copy back
    return n;
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeGetRecordedDropped(JNIEnv *, jobject) {
    return static_cast<jint>(host().recordedDropped());
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSetTempo(JNIEnv *, jobject, jfloat bpm) { host().setTempo(bpm); }

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeGetTempo(JNIEnv *, jobject) { return host().tempo(); }

JNIEXPORT jlong JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeGetPositionPacked(JNIEnv *, jobject) {
    return static_cast<jlong>(host().positionPacked());
}

// --- Song snapshot builder -----------------------------------------------------

JNIEXPORT jlong JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSnapshotBegin(JNIEnv *, jobject) {
    return static_cast<jlong>(host().snapshotBegin());
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSnapshotAddScene(JNIEnv *, jobject, jlong handle, jlong sceneId,
                                                                 jint ticksPerBar, jint repeat, jfloat bpmOverride,
                                                                 jfloat rampToBpm, jint rampBars,
                                                                 jboolean smooth, jboolean fadeIn, jboolean fadeOut) {
    return host().snapshotAddScene(handle, sceneId, ticksPerBar, repeat, bpmOverride, rampToBpm, rampBars,
                                   smooth == JNI_TRUE,
                                   fadeIn == JNI_TRUE, fadeOut == JNI_TRUE)
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSnapshotSetClipCached(JNIEnv *, jobject, jlong handle, jint rack,
                                                                      jint scene, jlong rev) {
    return host().snapshotSetClipCached(handle, rack, scene, rev) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSnapshotSetClip(JNIEnv *env, jobject, jlong handle, jint rack,
                                                                jint scene, jlong rev, jint bars, jint playMode,
                                                                jboolean mute, jint seed, jintArray notes,
                                                                jfloatArray expr, jstring lyrics) {
    const jsize len = notes != nullptr ? env->GetArrayLength(notes) : 0;
    const int noteCount = static_cast<int>(len / 6);
    if (noteCount <= 0) {
        return host().snapshotSetClip(handle, rack, scene, rev, bars, playMode, mute == JNI_TRUE, seed, nullptr, 0,
                                      nullptr, 0)
                   ? JNI_TRUE
                   : JNI_FALSE;
    }
    jint *data = env->GetIntArrayElements(notes, nullptr);
    if (data == nullptr) {
        return JNI_FALSE;
    }
    const jsize elen = expr != nullptr ? env->GetArrayLength(expr) : 0;
    const int exprCount = static_cast<int>(elen / 3);
    jfloat *edata = exprCount > 0 ? env->GetFloatArrayElements(expr, nullptr) : nullptr;
    const char *words = lyrics != nullptr ? env->GetStringUTFChars(lyrics, nullptr) : nullptr;
    const bool ok = host().snapshotSetClip(handle, rack, scene, rev, bars, playMode, mute == JNI_TRUE, seed,
                                           reinterpret_cast<const int32_t *>(data), noteCount,
                                           reinterpret_cast<const float *>(edata), edata != nullptr ? exprCount : 0,
                                           words);
    if (words != nullptr) {
        env->ReleaseStringUTFChars(lyrics, words);
    }
    if (edata != nullptr) {
        env->ReleaseFloatArrayElements(expr, edata, JNI_ABORT);
    }
    env->ReleaseIntArrayElements(notes, data, JNI_ABORT); // read only; don't copy back
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSnapshotSetLane(JNIEnv *env, jobject, jlong handle, jint rack,
                                                                jint scene, jstring machineType, jstring unit,
                                                                jstring name, jboolean linear, jfloatArray points) {
    const jsize len = points != nullptr ? env->GetArrayLength(points) : 0;
    const int count = static_cast<int>(len / 2);
    jfloat *data = count > 0 ? env->GetFloatArrayElements(points, nullptr) : nullptr;
    const bool ok = host().snapshotSetLane(handle, rack, scene, toStdString(env, machineType), toStdString(env, unit),
                                           toStdString(env, name), linear == JNI_TRUE, data, count);
    if (data != nullptr) env->ReleaseFloatArrayElements(points, data, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMachineParamNames(JNIEnv *env, jobject, jstring type) {
    int32_t n = 0;
    const acidulous::ParamDef *defs = acidulous::MachineRegistry::paramDefs(toStdString(env, type).c_str(), n);
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray(n, stringClass, nullptr);
    for (int32_t i = 0; i < n; ++i) {
        jstring s = env->NewStringUTF(defs[i].name);
        env->SetObjectArrayElement(out, i, s);
        env->DeleteLocalRef(s);
    }
    return out;
}

// "name|min|max|def|curve|steps|unit" per parameter; curve 0 linear, 1 exponential, 2 stepped.
static jobjectArray paramInfoArray(JNIEnv *env, const acidulous::ParamDef *defs, int32_t n) {
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray(n, stringClass, nullptr);
    char buf[160];
    for (int32_t i = 0; i < n; ++i) {
        const auto &d = defs[i];
        snprintf(buf, sizeof(buf), "%s|%g|%g|%g|%d|%d|%s", d.name, d.min, d.max, d.def, static_cast<int>(d.curve), d.steps, d.unit);
        jstring s = env->NewStringUTF(buf);
        env->SetObjectArrayElement(out, i, s);
        env->DeleteLocalRef(s);
    }
    return out;
}

JNIEXPORT jobjectArray JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeMachineParamInfo(JNIEnv *env, jobject, jstring type) {
    int32_t n = 0;
    const acidulous::ParamDef *defs = acidulous::MachineRegistry::paramDefs(toStdString(env, type).c_str(), n);
    return paramInfoArray(env, defs, n);
}

JNIEXPORT jobjectArray JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeInputModParamInfo(JNIEnv *env, jobject, jstring type) {
    int32_t n = 0;
    const acidulous::ParamDef *defs = acidulous::InputModRegistry::paramDefs(toStdString(env, type).c_str(), n);
    return paramInfoArray(env, defs, n);
}

JNIEXPORT jobjectArray JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeEffectParamInfo(JNIEnv *env, jobject, jstring type) {
    int32_t n = 0;
    const acidulous::ParamDef *defs = acidulous::EffectRegistry::paramDefs(toStdString(env, type).c_str(), n);
    return paramInfoArray(env, defs, n);
}

JNIEXPORT jboolean JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSnapshotCommit(JNIEnv *, jobject, jlong handle) {
    return host().snapshotCommit(handle) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeSnapshotAbandon(JNIEnv *, jobject, jlong handle) {
    host().snapshotAbandon(handle);
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeGetNotesOn(JNIEnv *, jobject, jint rackId) {
    return static_cast<jint>(host().notesOn(rackId));
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeReadRackPeak(JNIEnv *, jobject, jint rackId) {
    return host().rackPeak(rackId);
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeGetMasterFade(JNIEnv *, jobject) {
    return host().masterFade();
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeParamNormalized(JNIEnv *env, jobject, jint rackId, jstring unit, jstring name) {
    return host().paramNormalized(rackId, toStdString(env, unit), toStdString(env, name));
}

JNIEXPORT jfloat JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeDebugParam(JNIEnv *env, jobject, jint rackId, jstring name) {
    return host().debugParam(rackId, toStdString(env, name));
}

JNIEXPORT jint JNICALL
Java_com_rm_acidulous_engine_EngineNative_nativeGetNotesOff(JNIEnv *, jobject, jint rackId) {
    return static_cast<jint>(host().notesOff(rackId));
}

} // extern "C"
