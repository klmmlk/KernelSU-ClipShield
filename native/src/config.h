#pragma once

#include <string>

// 与 java/com/clipshield/hook/Policy.java 的常量保持一致
enum PolicyMode : int {
    kAllow = 0,
    kBlock = 1,
    kSmart = 2,
};

struct AppPolicy {
    PolicyMode mode = kSmart;
    bool blockWrites = false;
    bool verbose = true;
    bool toast = true;
};

// companion 侧：解析配置 JSON，返回指定包名的策略。
// JSON 结构（由 WebUI 写入，见 module/default_config.json）：
//   { "default": "smart", "blockWrites": false, "verbose": true,
//     "apps": { "com.example.app": "block", ... } }
// 任何解析失败都回退到默认策略，绝不抛出或崩溃。
// explicitly_configured（可空）：该包名是否在 apps 里被用户单独配置过。
AppPolicy QueryPolicy(const std::string &json, const std::string &pkg,
                      bool *explicitly_configured = nullptr);

// 独立工具：容错读取整个文件；失败返回空字符串
std::string ReadFileText(const std::string &path);
