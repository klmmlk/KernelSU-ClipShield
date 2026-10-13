/* ClipShield WebUI
 * 配置文件: /data/adb/clipshield/config.json
 * 策略值: "smart" | "block" | "allow"（未配置的应用走 default）
 */
"use strict";

const CFG_PATH = "/data/adb/clipshield/config.json";
const MODE_ORDER = ["smart", "block", "allow"];
const MODE_LABEL = { smart: "🟡 智能", block: "🔴 拦截", allow: "🟢 放行" };

let config = { version: 1, default: "smart", blockWrites: false, verbose: true, apps: {} };
let presets = { categories: [] };
let installed = null; // Set<pkg> | null
let allApps = [];     // [{pkg, label}]

/* ---------- KernelSU JS 桥兼容层 ----------
 * 不同管理器版本的 exec API 形态：
 *  1. 回调式（KernelSU/Next 管理器主流）：ksu.exec(cmd, optionsJson, 回调名)
 *     结果通过全局回调传回，参数可能是 (errno, stdout, stderr) 或单个 JSON 字符串
 *  2. Promise 式：exec(cmd) 返回 Promise<{errno, stdout, stderr}>
 *  3. 同步式（老版本）：exec(cmd) 直接返回 stdout 字符串
 */
let execCounter = 0;
function ksuExec(cmd, timeoutMs) {
  const bridge =
    (typeof kernelsu !== "undefined" && kernelsu) ||
    (typeof window.kernelsu !== "undefined" && window.kernelsu) ||
    (typeof window.ksu !== "undefined" && window.ksu) ||
    null;
  if (!bridge || typeof bridge.exec !== "function") {
    return Promise.reject(new Error("KernelSU JS 桥不可用（请在 KernelSU 管理器中打开 WebUI）"));
  }

  const normalize = (out) => ({
    errno: out && out.errno !== undefined ? Number(out.errno) || 0 : 0,
    stdout: out && out.stdout !== undefined ? String(out.stdout) : "",
    stderr: out && out.stderr !== undefined ? String(out.stderr) : "",
  });

  return new Promise((resolve, reject) => {
    let settled = false;
    const done = (r) => { if (!settled) { settled = true; resolve(r); } };
    const fail = (e) => { if (!settled) { settled = true; reject(e); } };

    try {
      // 1) 回调式协议
      const callbackName = "__cs_exec_cb_" + Date.now() + "_" + execCounter++;
      globalThis[callbackName] = function (a, b, c) {
        try {
          if (typeof a === "string" && b === undefined && a.trim().startsWith("{")) {
            try {
              const o = JSON.parse(a);
              done(normalize(o));
              return;
            } catch (_) { /* 非法 JSON，按三参处理 */ }
          }
          if (a && typeof a === "object" && "stdout" in a) {
            done(normalize(a));
            return;
          }
          done({ errno: Number(a) || 0, stdout: b === undefined ? "" : String(b), stderr: c === undefined ? "" : String(c) });
        } finally {
          try { delete globalThis[callbackName]; } catch (_) { /* ignore */ }
        }
      };

      const ret = bridge.exec(cmd, "{}", callbackName);

      // 2) Promise 式返回
      if (ret && typeof ret.then === "function") {
        Promise.resolve(ret).then((out) => {
          if (out && typeof out === "object") done(normalize(out));
          else if (typeof out === "string") done({ errno: 0, stdout: out, stderr: "" });
          else done({ errno: 0, stdout: "", stderr: "" });
        }).catch(fail);
      } else if (typeof ret === "string") {
        // 3) 同步返回 stdout
        done({ errno: 0, stdout: ret, stderr: "" });
      } else if (ret && typeof ret === "object") {
        done(normalize(ret));
      }
      // 回调式：等待回调；超时兜底
      setTimeout(() => {
        if (!settled) fail(new Error("exec 超时，管理器 JS 桥无响应"));
      }, timeoutMs || 15000);
    } catch (e) {
      fail(e);
    }
  });
}

let toastTimer = null;
function toast(msg, isErr) {
  const el = document.getElementById("toast");
  el.textContent = msg;
  el.className = "show" + (isErr ? " err" : "");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (el.className = ""), 2600);
}

/* ---------- 配置读写 ---------- */
const DEFAULT_CONFIG = { version: 1, default: "smart", blockWrites: false, verbose: true, toast: true, apps: {} };

async function writeConfigFile(cfg) {
  const json = JSON.stringify(cfg, null, 1);
  const b64 = btoa(String.fromCharCode(...new TextEncoder().encode(json)));
  const res = await ksuExec(
    `mkdir -p /data/adb/clipshield && echo '${b64}' | base64 -d > ${CFG_PATH}.tmp` +
    ` && chmod 600 ${CFG_PATH}.tmp && mv ${CFG_PATH}.tmp ${CFG_PATH}`
  );
  return res.errno === 0;
}

async function loadConfig() {
  let text = "";
  const res = await ksuExec(`cat ${CFG_PATH} 2>/dev/null`);
  text = (res.stdout || "").trim();
  let cfg = null;
  if (text) {
    try { cfg = JSON.parse(text); } catch (e) { /* 损坏的配置按缺失处理 */ }
  }
  if (cfg && typeof cfg === "object") {
    if (!cfg.apps) cfg.apps = {};
    config = cfg;
    return { created: false };
  }
  // 配置缺失/损坏：直接用 root 权限自动重建，无需重启
  const ok = await writeConfigFile(DEFAULT_CONFIG);
  if (!ok) {
    throw new Error("无法读取配置，且自动创建失败（请在 KernelSU 管理器中授予本模块 WebUI 的 root 权限后重试）");
  }
  config = JSON.parse(JSON.stringify(DEFAULT_CONFIG));
  return { created: true };
}

async function saveConfig() {
  const cfg = {
    version: 1,
    default: document.getElementById("defaultMode").value,
    blockWrites: document.getElementById("blockWrites").checked,
    verbose: document.getElementById("verbose").checked,
    toast: document.getElementById("toast").checked,
    apps: config.apps,
  };
  const ok = await writeConfigFile(cfg);
  if (!ok) {
    toast("保存失败（写入 /data/adb/clipshield 失败）", true);
    return;
  }
  config = cfg;
  toast("已保存 ✅ 强行停止目标应用后生效");
}

/* ---------- 渲染 ---------- */
function modeOf(pkg) {
  return config.apps[pkg] || null; // null = 跟随默认
}

function chipEl(label, pkg, showName, hint) {
  const el = document.createElement("span");
  el.className = "chip";
  el.title = pkg + (hint ? "\n" + hint : "");
  const render = () => {
    const m = modeOf(pkg);
    const absent = installed && !installed.has(pkg);
    el.className = "chip" + (absent ? " absent" : "");
    el.classList.remove("m-smart", "m-block", "m-allow");
    // 按钮只显示策略状态；应用名在左侧（预设区保留名字便于区分）
    if (m) {
      el.classList.add("m-" + m);
      el.textContent = MODE_LABEL[m] + (showName ? ` ${label}` : "") + (absent ? " ·未装" : "");
    } else {
      el.textContent = (showName ? `⚪ 全局 ${label}` : "⚪ 全局") + (absent ? " ·未装" : "");
    }
  };
  el.addEventListener("click", () => {
    const cur = modeOf(pkg);
    if (!cur) config.apps[pkg] = "smart";
    else {
      const i = MODE_ORDER.indexOf(cur);
      if (i + 1 < MODE_ORDER.length) config.apps[pkg] = MODE_ORDER[i + 1];
      else delete config.apps[pkg];
    }
    render();
    renderAppList();
  });
  render();
  return el;
}

function renderPresets() {
  const root = document.getElementById("presets");
  root.innerHTML = "";
  for (const cat of presets.categories || []) {
    const wrap = document.createElement("div");
    wrap.className = "cat";
    const h = document.createElement("h3");
    h.textContent = cat.name;
    const chips = document.createElement("div");
    chips.className = "chips";
    const entries = Object.entries(cat.apps || {}).sort((a, b) => a[1].localeCompare(b[1], "zh"));
    for (const [pkg, label] of entries) chips.appendChild(chipEl(label, pkg, true));
    wrap.appendChild(h);
    wrap.appendChild(chips);
    root.appendChild(wrap);
  }
}

function renderAppList() {
  const root = document.getElementById("applist");
  const q = document.getElementById("search").value.trim().toLowerCase();
  root.innerHTML = "";
  const pool = allApps.length
    ? allApps
    : Object.entries(config.apps).map(([pkg, label]) => ({ pkg, label }));
  const filtered = q
    ? pool.filter((a) => a.pkg.toLowerCase().includes(q) || (a.label || "").toLowerCase().includes(q))
    : pool;
  document.getElementById("appListHint").textContent = filtered.length
    ? `共 ${filtered.length} 个（已安装的排前面；输入法自动放行，无需配置）`
    : allApps.length
      ? "无匹配应用"
      : "点击「重新扫描已装应用」加载完整列表；也可直接在预设区点击配置。";
  for (const a of filtered.slice(0, 400)) {
    const item = document.createElement("div");
    item.className = "appitem";
    const left = document.createElement("div");
    left.className = "appinfo";
    // 有预设名显示“应用名 + 包名”；没有则直接以包名为名（列表中的应用必然已安装）
    left.innerHTML = a.label
      ? `<div class="name">${escapeHtml(a.label)}</div><div class="pkg">${escapeHtml(a.pkg)}</div>`
      : `<div class="name">${escapeHtml(a.pkg)}</div><div class="pkg">第三方应用</div>`;
    const chip = chipEl(a.label || a.pkg, a.pkg, false);
    item.appendChild(left);
    item.appendChild(chip);
    root.appendChild(item);
  }
}

function escapeHtml(s) {
  return s.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

function syncGlobalControls() {
  document.getElementById("defaultMode").value = config.default || "smart";
  document.getElementById("blockWrites").checked = !!config.blockWrites;
  document.getElementById("verbose").checked = config.verbose !== false;
  document.getElementById("toast").checked = config.toast !== false;
}

/* ---------- 数据获取 ---------- */
const LS_LABELS = "cs_labels_v1";   // 包名 -> 应用名（aapt 解析结果缓存）
const LS_LIST = "cs_applist_v1";    // 上次扫描的完整应用列表
let labelCache = {};
try { labelCache = JSON.parse(localStorage.getItem(LS_LABELS) || "{}"); } catch (_) { labelCache = {}; }

function saveList() {
  try { localStorage.setItem(LS_LIST, JSON.stringify(allApps)); } catch (_) {}
}

const AAPT_BIN = "/data/adb/modules/clipshield/bin/aapt";
const AAPT_LIBS = "/data/adb/modules/clipshield/bin/libs";

/* 应用名获取（后台文件协议）：
 * 1. 把 aapt 脚本写到 /data/local/tmp/cs_labels.sh（base64 传输防转义）
 * 2. nohup 后台启动（exec 立即返回，长任务完全不占 JS 桥）
 * 3. 轮询结果文件显示进度，写完标记后一次性读取合并
 * 任何单个 aapt 挂起都有 timeout 3s 兜底，界面永不冻结。 */
const LS_DIR = "/data/local/tmp";
const LS_SCRIPT = LS_DIR + "/cs_labels.sh";
const LS_OUT = LS_DIR + "/cs_labels.out";
const LS_DONE = LS_DIR + "/cs_labels.done";

function b64(str) {
  return btoa(String.fromCharCode(...new TextEncoder().encode(str)));
}

async function fetchLabels(entries, onProgress) {
  const pairs = entries.map((e) => `'${e.pkg}|${e.apk}'`).join(" ");
  const script =
    `#!/system/bin/sh\n` +
    `A=${AAPT_BIN}\n` +
    `export LD_LIBRARY_PATH=${AAPT_LIBS}\n` +
    `rm -f ${LS_OUT} ${LS_DONE}\n` +
    `printf '%s\\n' ${pairs} | while IFS='|' read -r pkg apk; do\n` +
    `  lbl=$(timeout 3 $A d badging "$apk" 2>/dev/null | sed -n "s/^application-label:'\\(.*\\)'\$/\\1/p" | head -1)\n` +
    `  printf '%s\\t%s\\n' "$pkg" "$lbl"\n` +
    `done > ${LS_OUT}\n` +
    `touch ${LS_DONE}\n`;
  const w = await ksuExec(`echo '${b64(script)}' | base64 -d > ${LS_SCRIPT} && chmod 755 ${LS_SCRIPT} && rm -f ${LS_OUT} ${LS_DONE}`, 10000);
  if (w.errno !== 0) throw new Error("脚本写入失败 errno=" + w.errno);

  const s = await ksuExec(`nohup sh ${LS_SCRIPT} >/dev/null 2>&1 &`, 10000);

  const t0 = Date.now();
  let lastCount = -1;
  while (Date.now() - t0 < 300000) {
    await new Promise((r) => setTimeout(r, 1500));
    const poll = await ksuExec(
      `wc -l < ${LS_OUT} 2>/dev/null; [ -f ${LS_DONE} ] && echo __CS_DONE__`,
      10000
    );
    const lines = (poll.stdout || "").split("\n");
    const done = lines.includes("__CS_DONE__");
    const count = parseInt(lines[0], 10) || 0;
    if (count !== lastCount) {
      lastCount = count;
      onProgress(count, entries.length, done);
    }
    if (done) {
      const out = await ksuExec(`cat ${LS_OUT}`, 15000);
      const found = {};
      for (const line of (out.stdout || "").split("\n")) {
        const tab = line.indexOf("\t");
        if (tab <= 0) continue;
        const pkg = line.slice(0, tab).trim();
        const label = line.slice(tab + 1).trim();
        if (pkg && label) found[pkg] = label;
      }
      await ksuExec(`rm -f ${LS_SCRIPT} ${LS_OUT} ${LS_DONE}`, 8000).catch(() => {});
      return found;
    }
  }
  throw new Error("读取应用名超时（300 秒），请重试");
}

let scanning = false;
async function scanInstalled() {
  if (scanning) return;
  scanning = true;
  const btn = document.getElementById("btnRescan");
  btn.disabled = true;
  btn.textContent = "扫描中…";
  try {
    return await scanInstalledInner();
  } finally {
    scanning = false;
    btn.disabled = false;
    btn.textContent = "重新扫描已装应用";
  }
}

async function scanInstalledInner() {
  // 1) 包名 + APK 路径（-f）
  const res = await ksuExec("pm list packages -3 -f", 30000);
  if (res.errno !== 0) {
    throw new Error("pm list packages 失败 (errno " + res.errno + (res.stderr ? ": " + res.stderr : "") + ")");
  }
  const entries = [];
  for (const line of res.stdout.split("\n")) {
    if (!line.startsWith("package:")) continue;
    const body = line.slice(8);
    const eq = body.lastIndexOf("=");
    if (eq <= 0) continue;
    entries.push({ pkg: body.slice(eq + 1).trim(), apk: body.slice(0, eq).trim() });
  }
  if (!entries.length) throw new Error("未获取到任何第三方应用");
  installed = new Set(entries.map((e) => e.pkg));

  // 2) 应用名：预设名 > aapt 缓存 > 包名；随后用 aapt 增量刷新
  const known = new Set();
  for (const cat of presets.categories || []) {
    for (const [pkg, label] of Object.entries(cat.apps || {})) {
      known.add(pkg);
      if (!allApps.some((a) => a.pkg === pkg)) allApps.push({ pkg, label });
    }
  }
  for (const e of entries) {
    if (known.has(e.pkg)) { e.apk = e.apk; continue; }
    const idx = allApps.findIndex((a) => a.pkg === e.pkg);
    const label = labelCache[e.pkg] || "";
    if (idx >= 0) allApps[idx].apk = e.apk;
    else allApps.push({ pkg: e.pkg, label, apk: e.apk });
  }
  for (const a of allApps) {
    if (!a.label && labelCache[a.pkg]) a.label = labelCache[a.pkg];
  }
  sortAllApps();
  renderPresets();
  renderAppList();

  // 3) aapt 解析（有缓存的也刷新，保证改名后同步）
  const need = allApps.filter((a) => a.apk);
  if (need.length) {
    const hint = document.getElementById("appListHint");
    const labels = await fetchLabels(need, (count, total, done) => {
      hint.textContent = `正在读取应用名… ${count}/${total}${done ? "（完成）" : ""}`;
    });
    let changed = 0;
    for (const a of allApps) {
      if (labels[a.pkg] && a.label !== labels[a.pkg]) {
        a.label = a.label && known.has(a.pkg) ? a.label : labels[a.pkg];
        if (!known.has(a.pkg)) changed++;
      }
    }
    Object.assign(labelCache, labels);
    try { localStorage.setItem(LS_LABELS, JSON.stringify(labelCache)); } catch (_) {}
    sortAllApps();
    renderPresets();
    renderAppList();
    saveList();
    return { count: installed.size, labeled: changed };
  }
  sortAllApps();
  renderPresets();
  renderAppList();
  saveList();
  return { count: installed.size, labeled: 0 };
}

function sortAllApps() {
  // 已安装的排前面，其余按包名排在后面
  allApps.sort((a, b) => {
    const ia = installed && installed.has(a.pkg) ? 0 : 1;
    const ib = installed && installed.has(b.pkg) ? 0 : 1;
    if (ia !== ib) return ia - ib;
    return (a.label || a.pkg).localeCompare(b.label || b.pkg, "zh");
  });
}

/* ---------- 启动 ---------- */
async function main() {
  const status = document.getElementById("status");
  console.log("ClipShield WebUI build v6");
  try {
    presets = await (await fetch("presets.json")).json();
  } catch (e) {
    console.error(e);
  }
  try {
    const r = await loadConfig();
    if (r.created) {
      toast("检测到配置缺失，已自动创建默认配置 ✅");
    }
    // 先读上次扫描的缓存列表（秒出完整列表；手动扫描可刷新）
    try {
      const cached = JSON.parse(localStorage.getItem(LS_LIST) || "[]");
      if (Array.isArray(cached) && cached.length) {
        allApps = cached;
        installed = new Set(cached.map((a) => a.pkg));
      }
    } catch (_) { /* 缓存损坏则等待手动扫描 */ }
    status.textContent = `配置已加载 · 默认策略: ${MODE_LABEL[config.default] || config.default} · ${Object.keys(config.apps).length} 个应用单独配置`;
    syncGlobalControls();
    renderPresets();
    renderAppList();
  } catch (e) {
    status.textContent = "⚠️ " + e.message;
    status.className = "status err";
    syncGlobalControls();
    renderPresets();
    renderAppList();
  }
}

document.getElementById("btnSave").addEventListener("click", () => saveConfig().catch((e) => toast(e.message, true)));
document.getElementById("btnRescan").addEventListener("click", () => scanInstalled().then(() => toast("扫描完成")).catch((e) => toast(e.message, true)));
document.getElementById("search").addEventListener("input", renderAppList);

main();
