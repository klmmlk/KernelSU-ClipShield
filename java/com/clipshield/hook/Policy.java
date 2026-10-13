package com.clipshield.hook;

import android.content.ClipData;
import android.content.ClipDescription;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.widget.Toast;

/**
 * 剪贴板访问策略判定（运行在被拉黑 App 的进程内）。
 * 字段由 native 层通过 JNI 静态字段写入，一次进程生命周期内不再变化。
 */
public final class Policy {
    public static final int MODE_ALLOW = 0;
    public static final int MODE_BLOCK = 1;
    public static final int MODE_SMART = 2;

    /** 由 native 写入：MODE_ALLOW / MODE_BLOCK / MODE_SMART */
    public static volatile int mode = MODE_SMART;
    /** 由 native 写入：是否连写入也拦截 */
    public static volatile boolean blockWrites = false;
    /** 由 native 写入：是否输出决策日志 */
    public static volatile boolean verbose = true;
    /** 由 native 写入：拦截时是否弹提示 */
    public static volatile boolean toastEnabled = true;

    private static final long PASTE_WINDOW_MS = 3000L;
    private static final long SELF_WRITE_WINDOW_MS = 30000L;
    /** 回到前台后的宽限期：这段时间内主线程读取视为启动窥探，仍拦截 */
    private static final long RESUME_GRACE_MS = 3000L;
    private static final long TOAST_INTERVAL_MS = 5000L;

    private static volatile long pasteSignalAt = 0L;
    private static volatile long lastSelfWriteAt = 0L;
    /** -1 表示尚未有任何 Activity resume */
    private static volatile long lastResumeAt = -1L;
    private static volatile long lastToastAt = 0L;

    /** 由 Activity.onResume hook 调用 */
    public static void onActivityResume() {
        lastResumeAt = SystemClock.uptimeMillis();
    }

    /** 用户在 TextView 上点击了“粘贴”菜单 */
    public static void onPasteSignal() {
        pasteSignalAt = SystemClock.uptimeMillis();
        log("paste signal received");
    }

    /** 本 App 自己写入了剪贴板（复制回读应放行） */
    public static void onSelfWrite() {
        lastSelfWriteAt = SystemClock.uptimeMillis();
    }

    /** 是否放行一次剪贴板读取 */
    public static boolean allowRead() {
        int m = mode;
        boolean allow;
        if (m == MODE_ALLOW) {
            allow = true;
        } else if (m == MODE_BLOCK) {
            allow = false;
        } else {
            allow = withinPasteWindow() || withinSelfWriteWindow() || stackHasPaste()
                    || mainThreadAfterGrace();
        }
        log("allowRead=" + allow);
        return allow;
    }

    private static boolean withinPasteWindow() {
        long t = pasteSignalAt;
        return t != 0L && SystemClock.uptimeMillis() - t < PASTE_WINDOW_MS;
    }

    private static boolean withinSelfWriteWindow() {
        long t = lastSelfWriteAt;
        return t != 0L && SystemClock.uptimeMillis() - t < SELF_WRITE_WINDOW_MS;
    }

    /**
     * 主线程 + 已过前台宽限期 → 视为用户主动操作（粘贴一定发生在主线程的输入框交互中）；
     * 刚回前台 3 秒内的主线程读取仍视为启动窥探。
     */
    private static boolean mainThreadAfterGrace() {
        long resume = lastResumeAt;
        if (resume < 0L) return false; // 尚未有任何 Activity 显示
        if (SystemClock.uptimeMillis() - resume < RESUME_GRACE_MS) return false;
        return Looper.getMainLooper() == Looper.myLooper();
    }

    /**
     * 调用栈里出现框架“粘贴”菜单处理（TextView.onTextContextMenuItem），
     * 说明这次读取来自用户主动粘贴，而非 App 在 onResume 等时机的自动窥探。
     */
    private static boolean stackHasPaste() {
        StackTraceElement[] stack = new Throwable().getStackTrace();
        for (StackTraceElement e : stack) {
            String cn = e.getClassName();
            if (cn != null && cn.startsWith("android.widget.")
                    && "onTextContextMenuItem".equals(e.getMethodName())) {
                return true;
            }
        }
        return false;
    }

    /** 拦截时返回的空剪贴板内容 */
    public static ClipData emptyClip() {
        return ClipData.newPlainText("", "");
    }

    /** 拦截时返回的空剪贴板描述 */
    public static ClipDescription emptyDescription() {
        return new ClipDescription("", new String[]{ClipDescription.MIMETYPE_TEXT_PLAIN});
    }

    // ---------- 拦截提示（Toast） ----------

    private static volatile Context sAppContext = null;
    private static volatile boolean sContextTried = false;

    /** 从被 hook 的 ClipboardManager 实例上取宿主 App 的 Context（每个进程只需一次） */
    public static void captureContext(Object clipboardManager) {
        if (sContextTried || clipboardManager == null) return;
        sContextTried = true;
        try {
            java.lang.reflect.Field f =
                    android.content.ClipboardManager.class.getDeclaredField("mContext");
            f.setAccessible(true);
            sAppContext = (Context) f.get(clipboardManager);
        } catch (Throwable t) {
            log("captureContext failed: " + t);
        }
    }

    /** 拦截发生时弹提示（限频，主线程展示） */
    public static void notifyBlocked() {
        if (!toastEnabled) return;
        long now = SystemClock.uptimeMillis();
        if (now - lastToastAt < TOAST_INTERVAL_MS) return;
        lastToastAt = now;
        final Context ctx = sAppContext;
        if (ctx == null) return;
        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                Toast.makeText(ctx, "🛡️ 已拦截本应用读取剪贴板", Toast.LENGTH_SHORT).show();
            } catch (Throwable ignored) {
            }
        });
    }

    public static void log(String msg) {
        if (verbose) {
            Log.i("ClipShield", msg);
        }
    }
}
