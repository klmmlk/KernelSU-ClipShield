#include "hook_installer.h"

#include <sys/mman.h>
#include <unistd.h>

#include <string>

#include "dex_bytes.h"
#include "log.h"

#include <dobby.h>
#include <elf_util.h>
#include <lsplant.hpp>

namespace {

// ---- Dobby inline hook 适配（与 LSPlant 官方测试代码一致的姿势） ----

void *InlineHooker(void *target, void *hooker) {
    ALOGD("InlineHooker target=%p hooker=%p", target, hooker);
    auto addr = reinterpret_cast<uintptr_t>(target);
    auto page_begin = addr & ~(static_cast<uintptr_t>(getpagesize()) - 1);
    size_t page_span = (addr + 64 > page_begin + getpagesize())
                           ? static_cast<size_t>(getpagesize()) * 2
                           : static_cast<size_t>(getpagesize());
    mprotect(reinterpret_cast<void *>(page_begin), page_span,
             PROT_READ | PROT_WRITE | PROT_EXEC);
    void *backup = nullptr;
    int rc = DobbyHook(target, hooker, &backup);
    ALOGD("DobbyHook rc=%d backup=%p", rc, backup);
    if (rc == RS_SUCCESS) {
        return backup;
    }
    ALOGW("DobbyHook failed rc=%d target=%p", rc, target);
    return nullptr;
}

bool InlineUnhooker(void *func) {
    return DobbyDestroy(func) == RT_SUCCESS;
}

// ---- JNI 小工具 ----

jobject GetSystemClassLoader(JNIEnv *env) {
    jclass cl = env->FindClass("java/lang/ClassLoader");
    if (cl == nullptr) return nullptr;
    jmethodID mid = env->GetStaticMethodID(cl, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    if (mid == nullptr) return nullptr;
    return env->CallStaticObjectMethod(cl, mid);
}

jclass LoadClass(JNIEnv *env, jobject loader, const char *name) {
    jclass cl = env->FindClass("java/lang/ClassLoader");
    if (cl == nullptr) return nullptr;
    jmethodID mid = env->GetMethodID(cl, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (mid == nullptr) return nullptr;
    jstring nameStr = env->NewStringUTF(name);
    if (nameStr == nullptr) return nullptr;
    auto result = (jclass) env->CallObjectMethod(loader, mid, nameStr);
    env->DeleteLocalRef(nameStr);
    return result;
}

jobject GetMethodObject(JNIEnv *env, jclass clazz, jmethodID mid, bool isStatic) {
    return env->ToReflectedMethod(clazz, mid, isStatic ? JNI_TRUE : JNI_FALSE);
}

jmethodID FindInstanceMethod(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
    return env->GetMethodID(clazz, name, sig);
}

bool ClearException(JNIEnv *env, const char *where) {
    if (env->ExceptionCheck()) {
        ALOGE("JNI exception at %s:", where);
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
    return false;
}

// Hook 一条记录：目标类/方法 + 回调名 + backup 存放字段
struct HookEntry {
    const char *targetClass;      // FindClass 用的二进制名
    const char *methodName;
    const char *methodSig;
    bool isStatic;
    const char *callbackName;     // Hooker 类里的回调方法
    const char *backupField;      // Hooker 类里存放 backup 的字段
};

const HookEntry kHookEntries[] = {
    {"android/content/ClipboardManager", "getPrimaryClip", "()Landroid/content/ClipData;", false,
     "onGetPrimaryClip", "bkGetPrimaryClip"},
    {"android/content/ClipboardManager", "getPrimaryClipDescription", "()Landroid/content/ClipDescription;", false,
     "onGetPrimaryClipDescription", "bkGetPrimaryClipDescription"},
    {"android/content/ClipboardManager", "getText", "()Ljava/lang/CharSequence;", false,
     "onGetText", "bkGetText"},
    {"android/content/ClipboardManager", "hasPrimaryClip", "()Z", false,
     "onHasPrimaryClip", "bkHasPrimaryClip"},
    {"android/content/ClipboardManager", "setPrimaryClip", "(Landroid/content/ClipData;)V", false,
     "onSetPrimaryClip", "bkSetPrimaryClip"},
    {"android/content/ClipboardManager", "addPrimaryClipChangedListener",
     "(Landroid/content/ClipboardManager$OnPrimaryClipChangedListener;)V", false,
     "onAddListener", "bkAddListener"},
    {"android/widget/TextView", "onTextContextMenuItem", "(I)Z", false,
     "onTextContextMenuItem", "bkOnTextContextMenuItem"},
    // App 必须调用 super.onResume()，基类实现必被命中；用于“前台宽限期”判定
    {"android/app/Activity", "onResume", "()V", false,
     "onActivityResume", "bkActivityResume"},
};

jobject FindAndHook(JNIEnv *env, jobject hookerObj, jclass hookerClass, const HookEntry &e,
                    const AppPolicy &policy) {
    jclass targetClass = env->FindClass(e.targetClass);
    if (targetClass == nullptr) {
        ClearException(env, "FindClass target");
        ALOGE("target class not found: %s", e.targetClass);
        return nullptr;
    }
    jmethodID mid = e.isStatic ? env->GetStaticMethodID(targetClass, e.methodName, e.methodSig)
                               : env->GetMethodID(targetClass, e.methodName, e.methodSig);
    if (mid == nullptr) {
        ClearException(env, "GetMethodID target");
        ALOGE("target method not found: %s.%s %s", e.targetClass, e.methodName, e.methodSig);
        return nullptr;
    }
    jobject targetMethod = GetMethodObject(env, targetClass, mid, e.isStatic);
    if (targetMethod == nullptr) {
        ClearException(env, "ToReflectedMethod target");
        return nullptr;
    }

    jmethodID cbMid = env->GetMethodID(hookerClass, e.callbackName,
                                       "([Ljava/lang/Object;)Ljava/lang/Object;");
    if (cbMid == nullptr) {
        ClearException(env, "GetMethodID callback");
        return nullptr;
    }
    jobject callbackMethod = GetMethodObject(env, hookerClass, cbMid, false);
    if (callbackMethod == nullptr) {
        ClearException(env, "ToReflectedMethod callback");
        return nullptr;
    }

    jobject backup = lsplant::Hook(env, targetMethod, hookerObj, callbackMethod);
    ClearException(env, "lsplant::Hook");
    if (backup == nullptr) {
        ALOGE("lsplant::Hook failed: %s.%s", e.targetClass, e.methodName);
        return nullptr;
    }

    jfieldID fid = env->GetFieldID(hookerClass, e.backupField, "Ljava/lang/reflect/Method;");
    if (fid == nullptr) {
        ClearException(env, "GetFieldID backup");
        return nullptr;
    }
    env->SetObjectField(hookerObj, fid, backup);

    env->DeleteLocalRef(targetMethod);
    env->DeleteLocalRef(callbackMethod);
    env->DeleteLocalRef(targetClass);
    return backup;
}

void PushPolicyConfig(JNIEnv *env, jclass policyClass, const AppPolicy &policy) {
    jfieldID fMode = env->GetStaticFieldID(policyClass, "mode", "I");
    jfieldID fBlockWrites = env->GetStaticFieldID(policyClass, "blockWrites", "Z");
    jfieldID fVerbose = env->GetStaticFieldID(policyClass, "verbose", "Z");
    jfieldID fToast = env->GetStaticFieldID(policyClass, "toastEnabled", "Z");
    if (fMode) env->SetStaticIntField(policyClass, fMode, static_cast<jint>(policy.mode));
    if (fBlockWrites) env->SetStaticBooleanField(policyClass, fBlockWrites, policy.blockWrites);
    if (fVerbose) env->SetStaticBooleanField(policyClass, fVerbose, policy.verbose);
    if (fToast) env->SetStaticBooleanField(policyClass, fToast, policy.toast);
    ClearException(env, "PushPolicyConfig");
}

} // namespace

bool InstallClipboardHooks(JNIEnv *env, const AppPolicy &policy) {
    // 1. LSPlant 初始化（每进程一次）
    static bool lsplant_inited = false;
    if (!lsplant_inited) {
        // ElfImg 会解析 libart.so 的 .gnu_debugdata 压缩符号表
        //（2024-08 起 Google ART 主线更新把内部符号从 .dynsym 挪进了这里）
        SandHook::ElfImg art("libart.so");
        lsplant::InitInfo info{
            .inline_hooker = InlineHooker,
            .inline_unhooker = InlineUnhooker,
            .art_symbol_resolver =
                [&art](std::string_view symbol) -> void * {
                    return reinterpret_cast<void *>(art.getSymbAddress(symbol));
                },
            .art_symbol_prefix_resolver =
                [&art](std::string_view prefix) -> void * {
                    return reinterpret_cast<void *>(art.getSymbPrefixFirstOffset(prefix));
                },
        };
        if (!lsplant::Init(env, info)) {
            ALOGE("lsplant::Init failed");
            return false;
        }
        lsplant_inited = true;
        ALOGI("lsplant::Init ok");
    }

    // 2. 通过 InMemoryDexClassLoader 加载 hooker dex
    jobject sysLoader = GetSystemClassLoader(env);
    if (ClearException(env, "getSystemClassLoader") || sysLoader == nullptr) {
        ALOGE("failed to obtain system class loader");
        return false;
    }
    jclass imdl = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (imdl == nullptr) {
        ClearException(env, "FindClass InMemoryDexClassLoader");
        return false;
    }
    jobject byteBuf = env->NewDirectByteBuffer(
        const_cast<unsigned char *>(clipshield::kDexBytes),
        static_cast<jlong>(clipshield::kDexSize));
    if (byteBuf == nullptr) {
        ClearException(env, "NewDirectByteBuffer");
        return false;
    }
    jmethodID imdlCtor = env->GetMethodID(imdl, "<init>",
                                          "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (imdlCtor == nullptr) {
        ClearException(env, "InMemoryDexClassLoader.<init>");
        return false;
    }
    jobject dexLoader = env->NewObject(imdl, imdlCtor, byteBuf, sysLoader);
    if (ClearException(env, "new InMemoryDexClassLoader") || dexLoader == nullptr) {
        ALOGE("InMemoryDexClassLoader creation failed");
        return false;
    }

    jclass hookerClass = LoadClass(env, dexLoader, "com.clipshield.hook.Hooker");
    if (ClearException(env, "loadClass Hooker") || hookerClass == nullptr) {
        ALOGE("Hooker class not loadable");
        return false;
    }
    jclass policyClass = LoadClass(env, dexLoader, "com.clipshield.hook.Policy");
    if (ClearException(env, "loadClass Policy") || policyClass == nullptr) {
        ALOGE("Policy class not loadable");
        return false;
    }

    // 3. 下发策略配置
    PushPolicyConfig(env, policyClass, policy);

    // 4. 实例化 hooker 并安装全部 hooks
    jmethodID hookerCtor = env->GetMethodID(hookerClass, "<init>", "()V");
    if (hookerCtor == nullptr) {
        ClearException(env, "Hooker.<init>");
        return false;
    }
    jobject hookerObj = env->NewObject(hookerClass, hookerCtor);
    if (ClearException(env, "new Hooker") || hookerObj == nullptr) {
        ALOGE("Hooker instantiation failed");
        return false;
    }

    int okCount = 0;
    int total = 0;
    for (const auto &e : kHookEntries) {
        ++total;
        jobject backup = FindAndHook(env, hookerObj, hookerClass, e, policy);
        if (backup != nullptr) {
            ++okCount;
        } else {
            // 关键 hook（读路径）失败则整体判失败，让上层决定跳过
            ALOGW("hook failed: %s.%s", e.targetClass, e.methodName);
        }
    }

    // 进程生命周期内保活（App 进程常驻到被杀）
    static jobject s_hookerRef = env->NewGlobalRef(hookerObj);
    (void) s_hookerRef;
    static jobject s_loaderRef = env->NewGlobalRef(dexLoader);
    (void) s_loaderRef;

    ALOGI("hooks installed: %d/%d (mode=%d blockWrites=%d)", okCount, total,
          static_cast<int>(policy.mode), policy.blockWrites ? 1 : 0);
    return okCount == total;
}
