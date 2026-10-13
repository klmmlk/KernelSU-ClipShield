#pragma once

#include <jni.h>

#include "config.h"

// 在当前（App）进程内安装剪贴板 hooks。
// 前置条件：postAppSpecialize 阶段调用（沙箱内、App 代码尚未运行）。
// 任一步骤失败都会记日志并返回 false，不影响宿主 App 继续运行。
bool InstallClipboardHooks(JNIEnv *env, const AppPolicy &policy);
