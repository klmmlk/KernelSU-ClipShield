# ClipShield 剪贴板护盾

按应用拦截剪贴板窥探的 KernelSU Zygisk 模块。阻止被拉黑的国产 App 在后台/启动时自动读取你的剪贴板（验证码、口令、复制的文本），同时不影响你正常粘贴。

## 它做什么

- **按应用拦截**：在黑名单 App 的进程内 hook `android.content.ClipboardManager`（读取返回空剪贴板），拦截对用户无感知
- **被动监听也拦**：`addPrimaryClipChangedListener` 一律抑制——监听剪贴板变化同样是窥探
- **智能模式**：只有以下情况放行读取
  - 用户在输入框长按点了「粘贴」（捕获 `TextView` 粘贴菜单事件，3 秒放行窗口）
  - App 回读自己刚复制的内容（30 秒内）
  - 调用栈来自框架粘贴路径
- **可选拦写入**：默认放行 `setPrimaryClip`（保护你正常复制），可在 WebUI 按需全局开启
- **WebUI 配置**：预设分类（购物/短视频/外卖/社区…）一键勾选，或按包名逐个配置

策略分三档：**智能**（默认，推荐）/ **拦截**（读写全拦）/ **放行**。

## 要求

| 项目 | 要求 |
|---|---|
| Root 方案 | KernelSU（或兼容管理器）+ **Zygisk Next / ReZygisk** |
| Android | 13 (API 33) 及以上 |
| 架构 | arm64-v8a |

> 已知限制：应用内嵌 WebView 里的「粘贴」在智能模式下可能被误拦，此时把该应用改为「放行」即可。请勿拉黑输入法（搜狗/百度/讯飞等），否则剪贴板建议会失效。

## 安装

1. 在 KernelSU 管理器中安装模块 zip
2. 重启
3. 管理器 → 模块 → ClipShield → 打开 WebUI，勾选要拦截的应用
4. **强行停止**目标应用（或重启）使其重新被 Zygisk 注入

验证：复制一段文字 → 打开被拦截的应用 → 观察它不再弹出「已读取剪贴板」/提示；`adb logcat -s ClipShield` 可看到拦截决策。

## 构建

需要：JDK 11+、Python 3、Android SDK（build-tools / platforms / cmake / NDK r25+，默认找 `D:/AndroidSDK`，可用 `ANDROID_SDK_ROOT` / `ANDROID_NDK_HOME` 覆盖）。

```bash
tools/build.sh          # 产出 dist/clipshield-<versionCode>-<version>.zip
```

流程：`javac` 编译 hooker → `d8` 转 dex → 嵌入 .so → NDK CMake 编译（链接 [LSPlant v3.2](https://github.com/LSPosed/LSPlant) + [Dobby](https://github.com/jmpews/Dobby)）→ 打包模块 zip。依赖拉取见 `tools/fetch_deps.sh`。

推送 tag `v*` 时 GitHub Actions 会自动构建并发布 immutable release（配合 [KernelSU Modules](https://developers.kernelsu.org/docs/zh-CN/) 索引收录流程）。

## 技术原理

```
App 进程启动
 └─ Zygisk (Zygisk Next) 注入 libclipshield.so
     ├─ preAppSpecialize: 解析包名 → root companion 读
     │   /data/adb/clipshield/config.json → 返回该包策略
     ├─ 策略=allow → 不做任何事（零开销）
     └─ postAppSpecialize: LSPlant 初始化
         ├─ InMemoryDexClassLoader 加载内嵌 dex（Hooker/Policy）
         └─ hook ClipboardManager 6 个方法 + TextView 粘贴菜单
```

- 全部 hook 只发生在**目标 App 自己的进程**，不碰 system_server，最坏情况只是单个 App 崩溃
- 拦截时返回 `ClipData.newPlainText("", "")`，App 拿到的是合法的空剪贴板
- 配置存放于 `/data/adb/clipshield/config.json`，模块更新不覆盖用户配置

## 许可

GPL-3.0。依赖：[LSPlant](https://github.com/LSPosed/LSPlant)（LGPL-3.0）、elf_util（GPL-3.0, LSPosed）、[Dobby](https://github.com/jmpews/Dobby)（Apache-2.0）、[Zygisk API](https://github.com/topjohnwu/zygisk-module-sample)（0BSD 风格）。
