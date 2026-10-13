#!/system/bin/sh
# ClipShield 安装脚本（KernelSU / Magisk 兼容安装环境）

SKIPUNZIP=0

ui_print "*******************************"
ui_print " ClipShield 剪贴板护盾"
ui_print "*******************************"

if [ "$ARCH" != "arm64" ]; then
  abort "! 仅支持 arm64-v8a 设备，当前架构: $ARCH"
fi

if [ "$API" -lt 33 ]; then
  abort "! 需要 Android 13 (API 33) 或更高版本，当前 API: $API"
fi

# 配置目录：模块更新不会覆盖 /data/adb/clipshield
# 注意：这里不依赖 $MODPATH 的文件拷贝（部分安装环境下 MODPATH 不可靠），
# 直接用 heredoc 写入默认配置
CFGDIR=/data/adb/clipshield
CFGF=$CFGDIR/config.json
mkdir -p "$CFGDIR" 2>/dev/null

if [ -f "$CFGF" ]; then
  ui_print "- 保留已有用户配置"
else
  cat > "$CFGF" <<'EOF'
{
 "version": 1,
 "default": "smart",
 "blockWrites": false,
 "verbose": true,
 "toast": true,
 "apps": {}
}
EOF
  chmod 600 "$CFGF" 2>/dev/null
fi

if [ -f "$CFGF" ]; then
  ui_print "- 配置就绪: $CFGF"
else
  ui_print "! 配置生成失败（不影响安装，WebUI 首次打开会自动重建）"
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644

# aapt 需要执行权限（WebUI 扫描应用名用）
if [ -f "$MODPATH/bin/aapt" ]; then
  set_perm "$MODPATH/bin/aapt" 0 0 0755
  chmod 755 "$MODPATH/bin/aapt" 2>/dev/null
fi

if [ ! -f "$MODPATH/zygisk/arm64-v8a.so" ]; then
  ui_print "! 警告: 未找到 zygisk/arm64-v8a.so，模块将不生效"
  ui_print "! 请通过 CI 或 tools/build.sh 构建后再打包"
fi

ui_print " "
ui_print "- 安装完成，重启后生效"
ui_print "- 在管理器中打开本模块 WebUI 配置拦截名单"
ui_print "- 修改配置后需强行停止对应应用（或重启）才会生效"
ui_print "- 需要 Zygisk 支持（Zygisk Next / ReZygisk）"
