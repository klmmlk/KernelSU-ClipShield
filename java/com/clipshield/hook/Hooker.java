package com.clipshield.hook;

import java.lang.reflect.Method;

/**
 * LSPlant hook 回调类。
 * 约定：每个回调签名为 public Object xxx(Object[] args)；
 * 对实例方法 args[0] 是调用对象，其余是原参数。
 * bk* 字段由 native 层在 hook 成功后写入（即原方法 backup），不得改名。
 */
public final class Hooker {
    public Method bkGetPrimaryClip;
    public Method bkGetPrimaryClipDescription;
    public Method bkGetText;
    public Method bkHasPrimaryClip;
    public Method bkSetPrimaryClip;
    public Method bkAddListener;
    public Method bkOnTextContextMenuItem;
    public Method bkActivityResume;

    public Object onGetPrimaryClip(Object[] args) throws Throwable {
        if (Policy.allowRead()) {
            return bkGetPrimaryClip.invoke(args[0]);
        }
        Policy.log("getPrimaryClip -> blocked");
        Policy.captureContext(args[0]);
        Policy.notifyBlocked();
        return Policy.emptyClip();
    }

    public Object onGetPrimaryClipDescription(Object[] args) throws Throwable {
        if (Policy.allowRead()) {
            return bkGetPrimaryClipDescription.invoke(args[0]);
        }
        return Policy.emptyDescription();
    }

    public Object onGetText(Object[] args) throws Throwable {
        if (Policy.allowRead()) {
            return bkGetText.invoke(args[0]);
        }
        return "";
    }

    public Object onHasPrimaryClip(Object[] args) throws Throwable {
        if (Policy.allowRead()) {
            return bkHasPrimaryClip.invoke(args[0]);
        }
        return Boolean.FALSE;
    }

    public Object onSetPrimaryClip(Object[] args) throws Throwable {
        if (!Policy.blockWrites) {
            Policy.onSelfWrite();
            return bkSetPrimaryClip.invoke(args[0], args[1]);
        }
        Policy.log("setPrimaryClip -> blocked");
        return null;
    }

    /** 被动监听剪贴板变化同样是窥探，一律抑制 */
    public Object onAddListener(Object[] args) throws Throwable {
        Policy.log("addPrimaryClipChangedListener -> suppressed");
        return null;
    }

    /** 放行原调用，但记录“用户点了粘贴”信号供智能模式使用 */
    public Object onTextContextMenuItem(Object[] args) throws Throwable {
        Object idObj = args[1];
        if (idObj instanceof Integer) {
            int id = (Integer) idObj;
            if (id == android.R.id.paste || id == android.R.id.pasteAsPlainText) {
                Policy.onPasteSignal();
            }
        }
        return bkOnTextContextMenuItem.invoke(args[0], args[1]);
    }

    /** 记录回到前台的时间点：前台宽限期判定用（App 必须调用 super.onResume，命中稳定） */
    public Object onActivityResume(Object[] args) throws Throwable {
        Policy.onActivityResume();
        return bkActivityResume.invoke(args[0]);
    }
}
