#include "config.h"

#include <fcntl.h>
#include <unistd.h>

#include <iterator>

namespace {

struct Scanner {
    const std::string &s;
    size_t i = 0;

    explicit Scanner(const std::string &str) : s(str) {}

    bool eof() const { return i >= s.size(); }
    char peek() const { return eof() ? '\0' : s[i]; }
    void skipWs() {
        while (!eof()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++i;
            } else {
                break;
            }
        }
    }
    bool consume(char c) {
        skipWs();
        if (!eof() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }

    // 解析字符串字面量；只处理常见转义，失败返回 false
    bool parseString(std::string &out) {
        skipWs();
        if (eof() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (!eof()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c == '\\') {
                if (eof()) return false;
                char e = s[i++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        // 跳过 4 位十六进制（本配置不含非 ASCII，容错处理）
                        if (std::distance(s.begin() + i, s.end()) < 4) return false;
                        i += 4;
                        out.push_back('?');
                        break;
                    }
                    default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false; // 未闭合
    }

    // 跳过任意 JSON 值（string/number/bool/null/object/array）
    bool skipValue() {
        skipWs();
        if (eof()) return false;
        char c = s[i];
        if (c == '"') {
            std::string tmp;
            return parseString(tmp);
        }
        if (c == '{' || c == '[') {
            char open = c, close = (c == '{') ? '}' : ']';
            int depth = 0;
            while (!eof()) {
                char d = s[i];
                if (d == '"') { // 跳过字符串避免括号误判
                    std::string tmp;
                    if (!parseString(tmp)) return false;
                    continue;
                }
                ++i;
                if (d == open) ++depth;
                else if (d == close) {
                    --depth;
                    if (depth == 0) return true;
                }
            }
            return false;
        }
        // 字面量：number/true/false/null
        while (!eof()) {
            char d = s[i];
            if (d == ',' || d == '}' || d == ']' ||
                d == ' ' || d == '\t' || d == '\r' || d == '\n') {
                break;
            }
            ++i;
        }
        return true;
    }

    bool parseBool(bool &out) {
        skipWs();
        if (s.compare(i, 4, "true") == 0) { i += 4; out = true; return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; out = false; return true; }
        return false;
    }
};

PolicyMode ModeFromString(const std::string &m) {
    if (m == "allow") return kAllow;
    if (m == "block") return kBlock;
    return kSmart; // "smart" 与未知值兜底
}

} // namespace

std::string ReadFileText(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return out;
}

AppPolicy QueryPolicy(const std::string &json, const std::string &pkg,
                      bool *explicitly_configured) {
    AppPolicy p; // 默认值即兜底策略
    if (explicitly_configured) *explicitly_configured = false;
    if (json.empty()) return p;

    Scanner sc(json);
    sc.skipWs();
    if (!sc.consume('{')) return p;

    std::string key;
    while (true) {
        sc.skipWs();
        if (sc.consume('}')) break;
        if (sc.eof()) break;
        if (!sc.parseString(key)) break;
        if (!sc.consume(':')) break;

        if (key == "default") {
            std::string v;
            if (sc.parseString(v)) p.mode = ModeFromString(v);
        } else if (key == "blockWrites") {
            sc.parseBool(p.blockWrites);
        } else if (key == "verbose") {
            sc.parseBool(p.verbose);
        } else if (key == "toast") {
            sc.parseBool(p.toast);
        } else if (key == "apps") {
            sc.skipWs();
            if (sc.consume('{')) {
                std::string appKey;
                while (true) {
                    sc.skipWs();
                    if (sc.consume('}')) break;
                    if (sc.eof()) break;
                    if (!sc.parseString(appKey)) break;
                    if (!sc.consume(':')) break;
                    if (appKey == pkg) {
                        if (explicitly_configured) *explicitly_configured = true;
                        std::string v;
                        if (sc.parseString(v)) p.mode = ModeFromString(v);
                    } else {
                        sc.skipValue();
                    }
                    if (sc.consume(',')) continue;
                    sc.consume('}');
                    break;
                }
            }
        } else {
            sc.skipValue();
        }
        if (sc.consume(',')) continue;
        sc.consume('}');
        break;
    }
    return p;
}
