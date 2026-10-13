#!/system/bin/sh
# 开机兜底：确保配置文件存在且权限正确（companion 进程以 root 读取）
CFGDIR=/data/adb/clipshield
CFG=$CFGDIR/config.json

mkdir -p "$CFGDIR"
if [ ! -f "$CFG" ]; then
  cat > "$CFG" <<'EOF'
{
 "version": 1,
 "default": "smart",
 "blockWrites": false,
 "verbose": true,
 "toast": true,
 "apps": {}
}
EOF
  chmod 600 "$CFG" 2>/dev/null
fi
