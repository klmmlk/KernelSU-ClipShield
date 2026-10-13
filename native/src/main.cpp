#include <unistd.h>

#include <ctime>
#include <mutex>
#include <set>
#include <string>

#include "config.h"
#include "hook_installer.h"
#include "log.h"
#include "zygisk.hpp"

namespace {

constexpr const char *kConfigPath = "/data/adb/clipshield/config.json";

// ---------- 输入法自动放行 ----------
// 输入法进程需要读取剪贴板（粘贴建议等核心功能），且没有 Activity、
// 吃不到"前台宽限期"判定，默认智能模式会把它一直拦死。
// companion（root）通过 `cmd ime list -s` 识别启用的输入法，
// 未被用户单独配置的输入法一律放行；带 5 分钟缓存，避免每次 App 启动都 fork。

std::mutex g_ime_mutex;
std::set<std::string> g_ime_pkgs;
bool g_ime_loaded = false;
long g_ime_loaded_at = 0;

void RefreshImeListLocked() {
    long now = time(nullptr);
    if (g_ime_loaded && now - g_ime_loaded_at < 300) return;
    g_ime_loaded = false;
    g_ime_pkgs.clear();

    // Android 16 上 `cmd ime` 已不可用，改读 Settings：
    //   default_input_method = 默认输入法（单个 pkg/cls）
    //   enabled_input_methods = 启用的输入法（pkg/cls 以 ':' 分隔，尾部可能跟 ';id'）
    const char *const keys[] = {"default_input_method", "enabled_input_methods"};
    for (const char *key : keys) {
        std::string cmd = std::string("/system/bin/settings get secure ") + key +
                          " 2>/dev/null";
        FILE *fp = popen(cmd.c_str(), "r");
        if (fp == nullptr) continue;
        char buf[2048];
        while (fgets(buf, sizeof(buf), fp) != nullptr) {
            std::string value(buf);
            while (!value.empty() &&
                   (value.back() == '\n' || value.back() == '\r')) {
                value.pop_back();
            }
            size_t pos = 0;
            while (pos < value.size()) {
                size_t sep = value.find_first_of(":;", pos);
                std::string token = value.substr(
                    pos, (sep == std::string::npos ? value.size() : sep) - pos);
                pos = (sep == std::string::npos) ? value.size() : sep + 1;
                if (token.empty() || token == "null") continue;
                auto slash = token.find('/');
                std::string pkg =
                    (slash == std::string::npos) ? token : token.substr(0, slash);
                if (!pkg.empty()) g_ime_pkgs.insert(std::move(pkg));
            }
        }
        pclose(fp);
    }
    g_ime_loaded = true;
    g_ime_loaded_at = now;
}

bool IsImePackage(const std::string &pkg) {
    std::lock_guard<std::mutex> lock(g_ime_mutex);
    RefreshImeListLocked();
    return g_ime_pkgs.count(pkg) != 0;
}

bool readAll(int fd, void *buf, size_t len) {
    auto *p = static_cast<char *>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool writeAll(int fd, const void *buf, size_t len) {
    auto *p = static_cast<const char *>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string pathBasename(const std::string &path) {
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string jstringToString(JNIEnv *env, jstring s) {
    if (s == nullptr) return {};
    const char *chars = env->GetStringUTFChars(s, nullptr);
    if (chars == nullptr) return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

// companion 根进程：按包名查策略，回包 [mode][flags]
// flags: bit0 = blockWrites, bit1 = verbose
void companionHandler(int client) {
    uint32_t len = 0;
    if (!readAll(client, &len, sizeof(len))) return;
    if (len == 0 || len > 512) return;
    std::string pkg(len, '\0');
    if (!readAll(client, pkg.data(), len)) return;

    std::string json = ReadFileText(kConfigPath);
    bool explicit_cfg = false;
    AppPolicy p = QueryPolicy(json, pkg, &explicit_cfg);

    // 用户没有单独配置的输入法自动放行（粘贴建议等核心功能依赖剪贴板）
    if (!explicit_cfg && IsImePackage(pkg)) {
        ALOGI("IME auto-allow: %s", pkg.c_str());
        p.mode = kAllow;
    }

    // flags: bit0 = blockWrites, bit1 = verbose, bit2 = toast
    uint8_t flags = (p.blockWrites ? 0x01 : 0x00) | (p.verbose ? 0x02 : 0x00) |
                    (p.toast ? 0x04 : 0x00);
    uint8_t resp[2] = {static_cast<uint8_t>(p.mode), flags};
    writeAll(client, resp, sizeof(resp));
}

} // namespace

class ClipShieldModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        // WebView 子 zygote 沙箱进程不处理
        if (args->is_child_zygote != nullptr && *args->is_child_zygote) {
            return;
        }

        std::string dataDir = jstringToString(env_, args->app_data_dir);
        if (dataDir.empty()) return;
        std::string pkg = pathBasename(dataDir);
        if (pkg.empty() || pkg == ".") return;

        int fd = api_->connectCompanion();
        if (fd < 0) {
            ALOGW("connectCompanion failed for %s", pkg.c_str());
            return;
        }

        uint32_t len = static_cast<uint32_t>(pkg.size());
        AppPolicy policy;
        bool got = false;
        if (writeAll(fd, &len, sizeof(len)) && writeAll(fd, pkg.data(), len)) {
            uint8_t resp[2] = {0, 0};
            if (readAll(fd, resp, sizeof(resp))) {
                policy.mode = static_cast<PolicyMode>(resp[0]);
                policy.blockWrites = (resp[1] & 0x01) != 0;
                policy.verbose = (resp[1] & 0x02) != 0;
                policy.toast = (resp[1] & 0x04) != 0;
                got = true;
            }
        }
        close(fd);

        if (!got) {
            ALOGW("companion query failed for %s, skipping", pkg.c_str());
            return;
        }
        if (policy.mode == kAllow) return; // 不 hook，零开销

        ALOGI("will hook %s (mode=%d blockWrites=%d verbose=%d)", pkg.c_str(),
              static_cast<int>(policy.mode), policy.blockWrites ? 1 : 0, policy.verbose ? 1 : 0);
        pending_ = policy;
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (pending_.mode == kAllow) return;
        AppPolicy policy = pending_;
        pending_.mode = kAllow; // 一次性
        if (!InstallClipboardHooks(env_, policy)) {
            ALOGE("hook installation failed; module inactive for this process");
        }
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    AppPolicy pending_{};
};

REGISTER_ZYGISK_MODULE(ClipShieldModule)
REGISTER_ZYGISK_COMPANION(companionHandler)
