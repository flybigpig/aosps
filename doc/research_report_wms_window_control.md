# Android 15 WMS 窗口控制机制深度分析报告

## 执行摘要

本报告基于本地 AOSP 源码（`frameworks_base_15/framework15`，Android 15）逐行取证，解析 WindowManagerService 如何"控制"窗口。WMS 的控制力由四条互相咬合的机制构成：以 `WindowContainer` 树表达层级（子列表下标即 Z 序，SurfaceFlinger 的 layer 只是这个下标的映射结果）、以 `WindowManagerPolicy.getWindowLayerFromTypeLw` 表达类型优先级（36 级）、以 `WindowLayout.computeFrames` 表达几何（attrs + InsetsState + DisplayCutout 三者唯一决定一个窗口的 frame）、以 `SurfaceControl.Transaction` 表达最终提交（`prepareSurfaces` 递归 + `assignChildLayers` 发号）。

理解这四条后，绝大多数"窗口不显示 / 显示位置不对 / 层级错了 / Insets 没生效"的问题都能定位到具体一行：不显示通常是 `shouldRelayout` 或 `isGoneForLayout`，位置不对是 `WindowLayout.computeFrames` 的输入不对，层级错了是 `assignChildLayers` 或 `needsZBoost`，Insets 没生效则是 `InsetsStateController.updateAboveInsetsState` 与 `mRequestedVisibleTypes` 的交互。

---

核心结论（全部基于本地 Android 15 源码取证，路径相对 frameworks_base_15/framework15）：

WMS 的控制力由四条机制咬合而成。层级靠 WindowContainer.mChildren 列表下标（队尾最上层，SurfaceFlinger layer 只是它的映射，WindowContainer.java:2727-2748 的 assignChildLayers 分两趟发号，第二趟给 needsZBoost 提层）；类型优先级靠 WindowManagerPolicy.getWindowLayerFromTypeLw 的 36 级映射（WindowManagerPolicy.java:519-625）；几何靠 WindowLayout.computeFrames（注意它在 core/java/android/view/，不在 wm 包）；最终提交靠 prepareSurfaces 递归 + Transaction。

几个对排障最有价值的发现：

Z 序是动态分叉的。TYPE_SYSTEM_ALERT、TYPE_SYSTEM_OVERLAY、TYPE_SYSTEM_ERROR 三个类型按有无 INTERNAL_SYSTEM_WINDOW 权限落到不同层——比如 TYPE_SYSTEM_ERROR 从 9 跳到 27。同一 App 系统签名与非系统签名下层高表现不同，根因就在这里。

ITYPE_* 在 Android 15 已彻底移除。我在 InsetsState.java 全文件检索无匹配，现改用 WindowInsets.Type.* + InsetsSource.createId()（InsetsSource.java:71 的 ID_IME）。大量旧资料里的 ITYPE_STATUS_BARS 已不适用。

InputChannel 在 addWindow 阶段就创建（WMS:1757），不是首次 relayout——这点与很多旧文档相反。

策略校验已下移到 DisplayPolicy：adjustWindowParamsLw（WMS:1743）与 validateAddingWindowLw（WMS:1749），旧版 mPolicy.prepareAddWindowLw 已不存在。

relayout 是窗口真正出现的时刻，且它会 performSurfacePlacement(true) 穿透 deferLayout（WMS:2558）。窗口不显示先看 shouldRelayout 的三条件，最常见是 mActivityRecord.isClientVisible() 为 false。

窗口 frame 是无状态算出来的——只由 attrs + InsetsState + bounds 决定，没有记忆上次位置的逻辑。所以布局异常应优先怀疑 InsetsState/bounds 而非布局算法。

窗口移不掉多半卡在 tryStartExitingAnimation（WMS:2753-2800），它会依次尝试 WMCore 动画、自身动画、Shell 转场、旧 AppTransition，全不满足才销毁 Surface。

报告还含完整的 36 级类型→layer 映射表、DisplayAreaPolicyBuilder 默认容器树（DisplayAreaPolicyBuilder.java:120-142）、addWindow 五阶段逐校验项与所有 ADD_* 错误码抛出点、以及布局两趟遍历（先根窗口后子窗口）的原因说明。

与既有 ANR 报告的衔接点：InputMonitor.populateInputWindowHandle（InputMonitor.java:256）写入的 dispatchingTimeoutMillis 正是 InputDispatcher 判定 ANR 的超时来源，canReceiveKeysReason()（WindowState.java:2920）是排查焦点问题最快的手段。

## 一、WMS 的控制边界与并发模型

WMS 对窗口的控制全部收敛在一把锁 `mGlobalLock`（`WindowManagerGlobalLock`）之下。`addWindow`、`relayoutWindow`、`removeWindow`、`performSurfacePlacement` 的全部主体都在这把锁内执行，且都遵循同一套模板：先在锁外或锁内开头 `Binder.clearCallingIdentity()`，返回前 `restoreCallingIdentity`。以 `addWindow` 为例，清身份在 `WindowManagerService.java:1541`，恢复在 `:1945`。

这意味着 system_server 里"窗口变慢"和"AMS 变慢"是同一个线程上的竞争——`system_server` 的主线程或 `android.display`/`android.anim` 线程一旦被长任务占用，所有窗口操作（含应用 `requestLayout`）都会排队。这一点和 ANR 分析里 `waitingOnAMSLock` / `waitingOnGlobalLock` 的埋点是同一根链条。

窗口的 Binder 入口是 `Session`（每进程一个），应用侧 `ViewRootImpl` 通过 `IWindowSession` 调 `add`/`relayout`/`remove`；反向由 `IWindow` 回调 `resized`、`moved`、`dispatchGetNewSurface` 等。WMS 用 `mWindowMap`（key 为 `client.asBinder()`）维护所有窗口，判重就发生在 `addWindow:1569`。

---

## 二、容器树：Z 序的真相是列表下标

所有容器都继承自 `WindowContainer`（`services/core/java/com/android/server/wm/WindowContainer.java:139`）。最容易被误解的一点是：**SurfaceFlinger 的 layer 不是独立计算的，它是 `mChildren` 列表下标的映射**。`mChildren` 的语义在 `WindowContainer.java:180-182` 写得很直白——列表按 Z 序排列，**最上层在队尾**。

`onParentChanged`（`WindowContainer.java:655-675`）在容器换父时会创建或 reparent SurfaceControl，并在最后调用 `mParent.assignChildLayers()`。而 `assignChildLayers`（`WindowContainer.java:2727-2748`）的机制是：从 0 开始按子列表顺序发号，**分两趟**，第二趟才给 `needsZBoost()` 的子容器发号，把它们挤到最上面：

```2727:2748:services/core/java/com/android/server/wm/WindowContainer.java
    void assignChildLayers(Transaction t) {
        int layer = 0;

        // We use two passes as a way to promote children which
        // need Z-boosting to the end of the list.
        for (int j = 0; j < mChildren.size(); ++j) {
            final WindowContainer wc = mChildren.get(j);
            wc.assignChildLayers(t);
            if (!wc.needsZBoost()) {
                wc.assignLayer(t, layer++);
            }
        }
        for (int j = 0; j < mChildren.size(); ++j) {
            final WindowContainer wc = mChildren.get(j);
            if (wc.needsZBoost()) {
                wc.assignLayer(t, layer++);
            }
        }
        if (mOverlayHost != null) {
            mOverlayHost.setLayer(t, layer++);
        }
    }
```

`assignLayer` 本身有一个关键保护（`WindowContainer.java:2663-2673`）：转场动画播放期间 `mTransitionController.canAssignLayers(this)` 返回 false 时直接 return，避免动画期间的 Z 序抖动。

典型层级由 `DisplayAreaPolicyBuilder` 生成，其注释里就画出了 Android 15 的默认树（`DisplayAreaPolicyBuilder.java:120-142`）：

```
RootDisplayArea (DisplayContent)
  - WindowedMagnification
      - DisplayArea.Tokens (Wallpaper)
      - TaskDisplayArea
      - RootDisplayArea (FirstRoot)
          - DisplayArea.Tokens (Wallpaper) / TaskDisplayArea
          - DisplayArea.Tokens (above Tasks up to IME)
          - DisplayArea.Tokens (above IME)
      - RootDisplayArea (SecondRoot)  ...同上
      - DisplayArea.Tokens (above Tasks up to IME)
      - ImeContainers
      - DisplayArea.Tokens (above IME up to TYPE_ACCESSIBILITY_OVERLAY)
  - DisplayArea.Tokens (TYPE_ACCESSIBILITY_MAGNIFICATION_OVERLAY 及以上)
```

窗口并不直接挂在 DisplayArea 下，而是挂在 `WindowToken` 下，Token 再挂到某个 `DisplayArea.Tokens` 上。`WindowToken.addWindow`（`WindowToken.java:304-326`）用 `mWindowComparator` 插入，比较规则是 `mBaseLayer`：**新窗口的 baseLayer 大于等于已有窗口就排在它后面**（`WindowToken.java:298-302`），这就是同类型窗口后加者盖在上面的原因。子窗口（Panel/Dialog 等）不走 Token，直接挂到父 `WindowState` 上，用 `sWindowSubLayerComparator` 按 `mSubLayer` 排序（`WindowState.java:730-745`、`1173`）。

---

## 三、窗口类型与 Z 序：36 级映射表

类型的 Z 序优先级由 `WindowManagerPolicy.getWindowLayerFromTypeLw` 决定，实现在 `services/core/java/com/android/server/policy/WindowManagerPolicy.java:519-625`。返回值越小越靠底。完整映射（省略部分中间层）如下：

| layer | 类型 | 说明 |
|---|---|---|
| 1 | `TYPE_WALLPAPER` | 壁纸，最底（但 WMS 可能移动它） |
| 2 | 应用窗口区间 | `FIRST_APPLICATION_WINDOW..LAST_APPLICATION_WINDOW` 返回 `APPLICATION_LAYER`(=2) |
| 3 | `TYPE_PRESENTATION` / `TYPE_PRIVATE_PRESENTATION` / `TYPE_DOCK_DIVIDER` / `TYPE_QS_DIALOG` / `TYPE_PHONE` | — |
| 4 | `TYPE_SEARCH_BAR` | — |
| 5 | `TYPE_INPUT_CONSUMER` | — |
| 6 | `TYPE_SYSTEM_DIALOG` | — |
| 7 | `TYPE_TOAST` | — |
| 8 | `TYPE_PRIORITY_PHONE` | SIM 错误/解锁 |
| 9 / 12 | `TYPE_SYSTEM_ALERT` | 无 `INTERNAL_SYSTEM_WINDOW` → 9；有 → 12 |
| 10 / 23 | `TYPE_SYSTEM_OVERLAY` | 无权限 → 10；有 → 23 |
| 11 | `TYPE_APPLICATION_OVERLAY` | 悬浮窗 |
| 13 / 14 | `TYPE_INPUT_METHOD` / `TYPE_INPUT_METHOD_DIALOG` | 输入法 |
| 15 / 16 | `TYPE_STATUS_BAR` / `TYPE_STATUS_BAR_ADDITIONAL` | — |
| 17 | `TYPE_NOTIFICATION_SHADE` | — |
| 18 | `TYPE_STATUS_BAR_SUB_PANEL` | — |
| 19 | `TYPE_KEYGUARD_DIALOG` | — |
| 20 / 21 | `TYPE_VOICE_INTERACTION_STARTING` / `TYPE_VOICE_INTERACTION` | — |
| 22 | `TYPE_VOLUME_OVERLAY` | — |
| 24 / 25 | `TYPE_NAVIGATION_BAR` / `TYPE_NAVIGATION_BAR_PANEL` | — |
| 26 | `TYPE_SCREENSHOT` | — |
| 27 / 9 | `TYPE_SYSTEM_ERROR` | 无权限 → 9；有 → 27 |
| 28 / 29 | `TYPE_MAGNIFICATION_OVERLAY` / `TYPE_DISPLAY_OVERLAY` | — |
| 30 | `TYPE_DRAG` | 拖拽层 |
| 31 / 32 | `TYPE_ACCESSIBILITY_OVERLAY` / `TYPE_ACCESSIBILITY_MAGNIFICATION_OVERLAY` | — |
| 33 | `TYPE_SECURE_SYSTEM_OVERLAY` | — |
| 34 / 35 | `TYPE_BOOT_PROGRESS` / `TYPE_POINTER` | 鼠标指针 |
| 36 | `getMaxWindowLayer()` | 保留给圆角覆盖层 |

两个容易踩坑的点。其一，`TYPE_SYSTEM_ALERT`、`TYPE_SYSTEM_OVERLAY`、`TYPE_SYSTEM_ERROR` 是**动态层**：同一类型，有 `INTERNAL_SYSTEM_WINDOW` 权限时层更高（`"system-level error dialogs"` 从 9 跳到 27）。这也是为什么同一款 App 在系统和非系统签名下层高表现不同。其二，`WindowState` 构造时把 layer 放大了：

```1124:1148:services/core/java/com/android/server/wm/WindowState.java
        if (mAttrs.type >= FIRST_SUB_WINDOW && mAttrs.type <= LAST_SUB_WINDOW) {
            // The multiplier here is to reserve space for multiple
            // windows in the same type layer.
            mBaseLayer = mPolicy.getWindowLayerLw(parentWindow)
                    * TYPE_LAYER_MULTIPLIER + TYPE_LAYER_OFFSET;
            mSubLayer = mPolicy.getSubWindowLayerFromTypeLw(a.type);
            mIsChildWindow = true;
            ...
        } else {
            mBaseLayer = mPolicy.getWindowLayerLw(this)
                    * TYPE_LAYER_MULTIPLIER + TYPE_LAYER_OFFSET;
            mSubLayer = 0;
```

`TYPE_LAYER_MULTIPLIER` 的注释说明其目的是"为同一类型层内的多个窗口预留空间"。子窗口的 `mSubLayer` 由 `getSubWindowLayerFromTypeLw`（`WindowManagerPolicy.java:650-663`）给出，`TYPE_APPLICATION_MEDIA` 和 `TYPE_APPLICATION_MEDIA_OVERLAY` 为负值（SurfaceView 的经典打洞层次），`TYPE_APPLICATION_PANEL`、`TYPE_APPLICATION_SUB_PANEL`、`TYPE_APPLICATION_ABOVE_SUB_PANEL` 为正值。

---

## 四、addWindow 全流程

`WindowManagerService.addWindow` 位于 `WindowManagerService.java:1523-1948`，约 426 行，是全系统最复杂的单点之一。按顺序：

**阶段 A（锁外，1528-1536）**：只有权限校验在 `mGlobalLock` 之外——`mPolicy.checkAddPermission(attrs.type, isRoundedCornerOverlay, attrs.packageName, appOp)`，非 `ADD_OKAY` 直接返回。实现在 `PhoneWindowManager.java:3105-3160`：先校验 type 落在三大区间之一，否则 `ADD_INVALID_TYPE`；非系统窗口直接放行；`TYPE_TOAST` 记 `OP_TOAST_WINDOW`；`TYPE_ACCESSIBILITY_OVERLAY`、`TYPE_INPUT_METHOD`、`TYPE_WALLPAPER`、`TYPE_PRESENTATION`、`TYPE_VOICE_INTERACTION`、`TYPE_QS_DIALOG`、`TYPE_NAVIGATION_BAR_PANEL` 交给 WMS 自行校验；其余系统窗口要求 `INTERNAL_SYSTEM_WINDOW`；alert 类型记 `OP_SYSTEM_ALERT_WINDOW`，SYSTEM_UID 直接放行。

**阶段 B（基础环境，1544-1606）**：`mDisplayReady` 未就绪抛 `IllegalStateException`；session 客户端已死返回 `ADD_APP_EXITING`；display 解析走 `getDisplayContentOrCreate(displayId, attrs.token)`（`:2009`，**优先用 token 所在 display**）；无访问权 `ADD_INVALID_DISPLAY`；`mWindowMap` 判重 `ADD_DUPLICATE_ADD`；子窗口父窗口缺失或父也是子窗口则 `ADD_BAD_SUBWINDOW_TOKEN`；`TYPE_PRIVATE_PRESENTATION` 非私有 display 返回 `ADD_PERMISSION_DENIED`。

**阶段 C（Token 解析，1608-1737）**：这是最容易出 `BadTokenException` 的地方。取 Token 时子窗口用父窗口的 token（`:1626`）。Token 为 null 时，`unprivilegedAppCanCreateTokenWith`（`:1950`）禁止应用窗口、IME、壁纸、VoiceInteraction、QS Dialog、无障碍覆盖层无 Token 创建，返回 `ADD_BAD_APP_TOKEN`；随后按 `WindowContext` 或 `client.asBinder()` 兜底建 Token。Token 非 null 时按 `rootType` 分派：应用窗口要求 `token.asActivityRecord()` 非 null（否则 `ADD_NOT_APP_TOKEN`），`TYPE_INPUT_METHOD`/`WALLPAPER`/`VOICE_INTERACTION` 等要求 Token 的 `windowType` 匹配（否则 `ADD_BAD_APP_TOKEN`）。注意 `:1727-1737`：系统窗口却传了 ActivityRecord 时会**丢弃 token**，改用 `client.asBinder()` 重建。

**阶段 D（创建与注册，1739-1822）**：

```1739:1758:services/core/java/com/android/server/wm/WindowManagerService.java
            final WindowState win = new WindowState(this, session, client, token, parentWindow,
                    appOp[0], attrs, viewVisibility, session.mUid, userId,
                    session.mCanAddInternalSystemWindow);
            final DisplayPolicy displayPolicy = displayContent.getDisplayPolicy();
            displayPolicy.adjustWindowParamsLw(win, win.mAttrs);
            attrs.flags = sanitizeFlagSlippery(attrs.flags, win.getName(), callingUid, callingPid);
            attrs.inputFeatures = sanitizeInputFeatures(attrs.inputFeatures, win.getName(),
                    callingUid, callingPid, win.isTrustedOverlay());
            win.setRequestedVisibleTypes(requestedVisibleTypes);

            res = displayPolicy.validateAddingWindowLw(attrs, callingPid, callingUid);
            if (res != ADD_OKAY) {
                return res;
            }

            final boolean openInputChannels = (outInputChannel != null
                    && (attrs.inputFeatures & INPUT_FEATURE_NO_INPUT_CHANNEL) == 0);
            if  (openInputChannels) {
                win.openInputChannel(outInputChannel);
            }
```

注意 Android 15 的策略校验已经下移到 `DisplayPolicy`（`adjustWindowParamsLw`、`validateAddingWindowLw`），旧文档里的 `mPolicy.prepareAddWindowLw` 已不存在。另一个常见误解是 **InputChannel 在 addWindow 阶段就创建**（`WindowState.openInputChannel`，`WindowState.java:2627-2637`），不是首次 relayout。

**阶段 E（挂载与副作用，1834-1948）**：`win.mToken.addWindow(win)`、`displayPolicy.addWindowLw(win, attrs)`、`mWinAnimator.mEnterAnimationPending = true`；`TYPE_INPUT_METHOD` 且可触摸时 `setInputMethodWindowLocked`；壁纸相关打 `FINISH_LAYOUT_REDO_WALLPAPER`；若 `win.canReceiveKeys()` 则 `updateFocusedWindowLocked`（`:1883-1889`）；随后**显式不在这里做布局**（`:1899-1906` 注释说明"窗口必须调用 relayout 才会显示"），只 `assignChildLayers()` 与 `updateInputWindowsLw`；最后 `fillInsetsState(outInsetsState, true)` 与 `getInsetsSourceControls(win, outActiveControls)` 回填给客户端。

值得单独记住的是 Toast 的处理（`:1770-1788`）：同一 UID 同时只允许一个 Toast 窗口（`canAddToastWindowForUid` 否则 `ADD_DUPLICATE_ADD`），并且会按 `hideTimeoutMilliseconds` 发 `WINDOW_HIDE_TIMEOUT`——**隐藏而非移除**，因为应用没准备好处理窗口被移除。

---

## 五、relayoutWindow：窗口真正"出现"的时刻

`relayoutWindow`（`:2272`）只是壳，主体是 `relayoutWindowInner`（`:2313`）。流程要点：

先按 `seq` 防乱序（`:2332-2336`），`win.cancelAndRedraw()` 时回 `RELAYOUT_RES_CANCEL_AND_REDRAW`。随后 `displayPolicy.adjustWindowParamsLw` 再次调整参数，并强制两条不可变规则：**窗口 type 添加后不可改**（`:2366-2368`）、**Insets 提供方与 ID 不可改**（`:2370-2406`）。

真正的分水岭在 `shouldRelayout`（`:2513-2515`）：只有 `viewVisibility == VISIBLE` 且（无 ActivityRecord / 是 starting window / `mActivityRecord.isClientVisible()`）才真正布局。不满足时：

```2520:2538:services/core/java/com/android/server/wm/WindowManagerService.java
            if (!shouldRelayout && winAnimator.hasSurface() && !win.mAnimatingExit) {
                ...
                result |= RELAYOUT_RES_SURFACE_CHANGED;
                ...
                if (wallpaperMayMove) {
                    displayContent.mWallpaperController.adjustWallpaperWindows();
                }
                tryStartExitingAnimation(win, winAnimator);
            }
```

`tryStartExitingAnimation`（`:2753-2800`）会判断是否能起退出动画（WMCore 动画 / 自身动画 / Shell 转场 / 旧 AppTransition），都不满足才 `win.destroySurface(false, stopped)`。这解释了"窗口移除后画面还在"的现象——它在等动画。

满足 `shouldRelayout` 时创建 Surface（`:2542-2554` → `createSurfaceControl` 在 `:2802-2827`，内部 `winAnimator.createSurfaceLocked()`），然后**强制跑一次布局**：

```2556:2563:services/core/java/com/android/server/wm/WindowManagerService.java
            // We may be deferring layout passes at the moment, but since the client is interested
            // in the new out values right now we need to force a layout.
            mWindowPlacerLocked.performSurfacePlacement(true /* force */);

            if (shouldRelayout) {
                ...
                result = win.relayoutVisibleWindow(result);
```

注意 `force = true`：relayout 会穿透 `deferLayout`。最后回传 frame / config / Insets / controls（`fillClientWindowFramesAndConfiguration`，`:2662`）与 `getInsetsSourceControls`（`:2742-2751`，其中 `PARCELABLE_WRITE_RETURN_VALUE` 用于跨进程后释放 leash 副本）。

---

## 六、removeWindow：为什么窗口"移不掉"

移除入口是 `win.removeIfPossible()`（`WindowManagerService.java:2060`）。它并不立即移除，而是先 `disposeInputChannel()`，然后按可见性/动画状态决定是否延后。真正执行移除的是 `removeImmediately()`（`WindowState.java:2334-2387`），其顺序很讲究——**先销毁 Surface 再调 super**（因为 `WindowStateAnimator` 是个"虚拟子节点"，必须先于父节点移除，否则同步引擎的追踪会错乱）：

```2342:2350:services/core/java/com/android/server/wm/WindowState.java
        mRemoved = true;
        // Destroy surface before super call. The general pattern is that the children need
        // to be removed before the parent (so that the sync-engine tracking works). Since
        // WindowStateAnimator is a "virtual" child, we have to do it manually here.
        mWinAnimator.destroySurfaceLocked(getSyncTransaction());
        if (!mDrawHandlers.isEmpty()) {
            mWmService.mH.removeMessages(WINDOW_STATE_BLAST_SYNC_TIMEOUT, this);
        }
        super.removeImmediately();
```

随后处理 IME 目标重算（`isImeLayeringTarget` 时 `computeImeTarget`）、Presentation 通知、`displayPolicy.removeWindowLw`、`disposeInputChannel()`、`mSession.onWindowRemoved`、`postWindowRemoveCleanupLocked`。

Token 级别移除走 `removeWindowToken`（`:3097-3120`），默认 `removeWindows=false, animateExit=true`；只有 `removeWindows=true` 时才 `token.removeAllWindowsIfPossible()`。

---

## 七、布局计算：一个窗口的 frame 是怎么算出来的

布局的总闸是 `WindowSurfacePlacer`。`requestTraversal()`（`:217-231`）把任务 post 到 `mAnimationHandler`，`performSurfacePlacement(force)`（`:118-131`）在 `mDeferDepth > 0` 且非 force 时只累加 `mDeferredRequests`，否则最多循环 6 次直到 `mTraversalScheduled` 被清空。`deferLayout`/`continueLayout`（`:86-102`）用于批量操作期间抑制布局。

单次遍历的主体是 `RootWindowContainer.performSurfacePlacementNoTrace`（`:759`）→ `applySurfaceChangesTransaction()`（`:964-993`，逐个 DisplayContent 派发并汇入 `mDisplayTransactions`）→ `DisplayContent.performLayout`（`:5120`）。

`performLayoutNoTrace`（`DisplayContent.java:5129-5161`）的关键设计是**两趟**：

```5148:5160:services/core/java/com/android/server/wm/DisplayContent.java
        // First perform layout of any root windows (not attached to another window).
        forAllWindows(mPerformLayout, true /* traverseTopToBottom */);

        // Now perform layout of attached windows, which usually depend on the position of the
        // window they are attached to. XXX does not deal with windows that are attached to windows
        // that are themselves attached.
        forAllWindows(mPerformLayoutAttached, true /* traverseTopToBottom */);

        // Window frames may have changed. Tell the input dispatcher about it.
        mInputMonitor.setUpdateInputWindowsNeededLw();
        if (updateInputWindows) {
            mInputMonitor.updateInputWindowsLw(false /*force*/);
        }
```

先算根窗口再算子窗口（子窗口依赖父窗口位置），且注释明确承认不支持"子窗口的子窗口"。每趟都自顶向下遍历。

单窗口的几何计算在 `DisplayPolicy.layoutWindowLw`（`DisplayPolicy.java:1393-1416`）：

```1402:1415:services/core/java/com/android/server/wm/DisplayPolicy.java
        final WindowManager.LayoutParams attrs = win.mAttrs.forRotation(displayFrames.mRotation);
        sTmpClientFrames.attachedFrame = attached != null ? attached.getFrame() : null;

        // If this window has different LayoutParams for rotations, we cannot trust its requested
        // size. Because it might have not sent its requested size for the new rotation.
        final boolean trustedSize = attrs == win.mAttrs;
        final int requestedWidth = trustedSize ? win.mRequestedWidth : UNSPECIFIED_LENGTH;
        final int requestedHeight = trustedSize ? win.mRequestedHeight : UNSPECIFIED_LENGTH;

        mWindowLayout.computeFrames(attrs, win.getInsetsState(), displayFrames.mDisplayCutoutSafe,
                win.getBounds(), win.getWindowingMode(), requestedWidth, requestedHeight,
                win.getRequestedVisibleTypes(), win.mGlobalScale, sTmpClientFrames);

        win.setFrames(sTmpClientFrames, win.mRequestedWidth, win.mRequestedHeight);
```

真正的数学在 `WindowLayout.computeFrames`（注意：**它在 `core/java/android/view/WindowLayout.java`，不在 wm 包**，容易找错）。它的输入只有四个：attrs、InsetsState、displayCutoutSafe、windowBounds。第一步就是按 `fitInsetsTypes`/`fitInsetsSides` 从 InsetsState 取 insets 并收缩出 `displayFrame`：

```79:100:core/java/android/view/WindowLayout.java
        // Compute bounds restricted by insets
        final Insets insets = state.calculateInsets(windowBounds, attrs.getFitInsetsTypes(),
                attrs.isFitInsetsIgnoringVisibility());
        final @WindowInsets.Side.InsetsSide int sides = attrs.getFitInsetsSides();
        final int left = (sides & WindowInsets.Side.LEFT) != 0 ? insets.left : 0;
        final int top = (sides & WindowInsets.Side.TOP) != 0 ? insets.top : 0;
        final int right = (sides & WindowInsets.Side.RIGHT) != 0 ? insets.right : 0;
        final int bottom = (sides & WindowInsets.Side.BOTTOM) != 0 ? insets.bottom : 0;
        outDisplayFrame.set(windowBounds.left + left, windowBounds.top + top,
                windowBounds.right - right, windowBounds.bottom - bottom);

        if (attachedWindowFrame == null) {
            outParentFrame.set(outDisplayFrame);
            if ((pfl & PRIVATE_FLAG_INSET_PARENT_FRAME_BY_IME) != 0) {
                final InsetsSource source = state.peekSource(ID_IME);
                if (source != null) {
                    outParentFrame.inset(source.calculateInsets(
                            outParentFrame, false /* ignoreVisibility */));
                }
            }
        } else {
            outParentFrame.set(!layoutInScreen ? attachedWindowFrame : outDisplayFrame);
        }
```

之后处理刘海（`DisplayFrames.mDisplayCutoutSafe`，`:106-120`，`SHORT_EDGES` 模式只在短边方向收缩）、再按 width/height 与 gravity 解出最终 `frame`。**结论：窗口 frame 完全由 attrs + 当前 InsetsState + 窗口 bounds 决定，没有"记住上次位置"的逻辑**——所以布局异常时优先怀疑 InsetsState 或 bounds，而不是布局算法本身。

---

## 八、Insets：WMS 与 SystemUI/IME 的契约

Android 15 里 `InsetsState` **已经不再定义 `ITYPE_*` 常量**（我在 `core/java/android/view/InsetsState.java` 全文件检索无匹配），取而代之的是 `WindowInsets.Type.*` 作为类型、以及 `InsetsSource.createId(owner, index, type)` 生成的 ID（`InsetsSource.java:71` 的 `ID_IME`、`:74` 的 `ID_IME_CAPTION_BAR`）。这是 Android 14 起的重大改动，大量旧资料仍写 `ITYPE_STATUS_BARS`，在 15 上已不适用。

系统侧的管控者是 `InsetsStateController`。`getRawInsetsState()`（`InsetsStateController.java:95`）返回原始状态；`onPostLayout()`（`:169-179`）在每轮布局后跑一遍所有 `InsetsSourceProvider`，若整体状态变化则 `notifyInsetsChanged()`；`updateAboveInsetsState()`（`:186-198`）自顶向下遍历整棵树，为每个窗口计算"它上方的 insets"并写入 `WindowState.mAboveInsetsState`——这就是"同屏两个窗口拿到不同 insets"的来源。

窗口对 insets 的诉求通过 `mRequestedVisibleTypes` 表达（`addWindow:1747` 的 `win.setRequestedVisibleTypes(requestedVisibleTypes)`），并作为 `computeFrames` 的入参参与计算。控制能力（leash）通过 `getInsetsSourceControls` 下发给客户端（`:2742`），客户端用它直接操作 SystemUI/IME 的 Surface。

---

## 九、Surface 提交：Transaction 是怎么攒出来的

最终写 SurfaceFlinger 的只有 `SurfaceControl.Transaction`。WMS 的提交链是：`WindowSurfacePlacer.performSurfacePlacement` → `RootWindowContainer.applySurfaceChangesTransaction` → `DisplayContent.applySurfaceChangesTransaction` → `WindowContainer.prepareSurfaces()` 递归。

`WindowState.prepareSurfaces`（`WindowState.java:5268-5284`）依次做：dim、位置、帧率优先级、缩放，最后 `mWinAnimator.prepareSurfaceLocked(getSyncTransaction())`。位置更新在 `updateSurfacePosition`（`:5288-5308`），它把 `mWindowFrames.mFrame` 的左上角转成 Surface 坐标，并有两个提前返回的保护：布局被 defer 时（`mWindowPlacerLocked.isLayoutDeferred()`）与 `isGoneForLayout()` 时都跳过——注释说明这是为了避免用无效 frame 计算位置。

所有 layer 调整都攒在 `getSyncTransaction()` 里，直到动画帧触发才统一 `apply`。`DisplayContent.assignWindowLayers`（`:4034-4047`）的注释把这个设计说得很清楚：累积到 pending transaction 但延迟到 `prepareSurfaces` 才应用，这样才能把 Z 序变化和 Surface 的显隐**同步**。

---

## 十、焦点与输入窗口

`canReceiveKeys`（`WindowState.java:2935`）是焦点判定的核心，条件包括 `isVisibleRequestedOrAdding()`、`mViewVisibility == VISIBLE`、`!mRemoveOnExit`、没有 `FLAG_NOT_FOCUSABLE`、`mActivityRecord.windowsAreFocusable()`、以及 display 是否 onTop/trusted。调试它时可以直接用 `canReceiveKeysReason(boolean)`（`:2920-2933`），它把每个子条件都打印出来——这是排查"为什么焦点没到我"最快的手段。

输入侧由 `InputMonitor` 维护。`populateInputWindowHandle`（`InputMonitor.java:250-257`）把 token、dispatching timeout、touch occlusion mode、paused 等写进 `InputWindowHandleWrapper`；`updateInputWindowsLw(force)`（`:333-338`）在 `!force && !mUpdateInputWindowsNeeded` 时直接返回，否则 `scheduleUpdateInputWindows`。遍历时（`:618-627`）凡 `mInputChannelToken == null || mRemoved || !canReceiveTouchInput()` 的窗口会被降级为 `populateOverlayInputInfo`（`:715-723`：timeout 设 0、focusable 设 false、token 设 null），即**不接收输入但参与遮挡计算**。

这里与输入 ANR 直接相连：`inputWindowHandle.setDispatchingTimeoutMillis(w.getInputDispatchingTimeoutMillis())`（`:256`）就是 InputDispatcher 判定 ANR 的超时来源，与既有 ANR 报告中的 `WindowState.getInputDispatchingTimeoutMillis()` 是同一处。

---

## 十一、排障手册

**层级/遮挡问题**：先 `adb shell dumpsys window windows` 看容器树与每个窗口的 `mBaseLayer`/`mLayer`/`mSubLayer`；再确认是否被 `needsZBoost`（`WindowContainer.java:2755`）提到最后；转场期间层不变是 `canAssignLayers` 的保护，属正常。

**窗口不显示**：按 `shouldRelayout`（`WMS:2513`）三条件排查，最常见是 `mActivityRecord.isClientVisible()` 为 false；其次看 `isGoneForLayout()`（`updateSurfacePosition` 会因此跳过定位）；Surface 创建失败会有 `WM_ERROR` 的 `"Failed to create surface control for %s"`（`WMS:2822`）。

**位置不对**：直接看 `WindowLayout.computeFrames` 的四个输入。`dumpsys window` 里对比 `mWindowFrames` 的 `mFrame` 与 `mParentFrame`/`mDisplayFrame`，通常能立刻看出是 Insets 还是 bounds 的问题。

**Insets 不生效**：确认 `mRequestedVisibleTypes`、`InsetsStateController.updateAboveInsetsState` 的结果，以及客户端是否真的拿到了 `InsetsSourceControl`（`WMS:1931`/`:2746`）。注意 Android 15 已无 `ITYPE_*`。

**性能**：`performSurfacePlacement` 有 6 次循环上限（`WindowSurfacePlacer.java:123-129`），`mLayoutRepeatCount` 达到 `LAYOUT_REPEAT_THRESHOLD` 会打 "Layouts looping"（`:202-207`），这是布局震荡的强信号。所有窗口操作都在 `mGlobalLock` 下，system_server 卡顿会直接放大为窗口卡顿与输入 ANR。

**转场与动画**：`tryStartExitingAnimation`（`WMS:2753-2800`）会依次尝试 WMCore 动画、自身动画、Shell 转场（`mTransitionController.isShellTransitionsEnabled()`）、旧 AppTransition；都不满足才销毁 Surface。窗口"移除后还停留"多半卡在这里。

---

## 十二、局限性

本报告全部结论取自本地 `frameworks_base_15/framework15` 源码。未逐行展开的部分包括：`DisplayAreaPolicyBuilder` 的完整 feature 构造与 OEM 自定义 `mSelectRootForWindowFunc`、`SurfaceAnimator`/`WindowAnimator` 的动画实现细节、`BLASTSyncEngine` 的同步机制、以及 `SurfaceFlinger` 侧（`frameworks/native`）对 layer 的最终合成。此外 `WindowState.removeIfPossible` 的中段（动画期间延后移除的具体判定）因输出限制未完整取证，排障时应结合 `dumpsys window` 实际观察。

## References

1. [AOSP frameworks/base — WindowManagerService.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/wm/WindowManagerService.java)
2. [AOSP frameworks/base — WindowState.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/wm/WindowState.java)
3. [AOSP frameworks/base — WindowContainer.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/wm/WindowContainer.java)
4. [AOSP frameworks/base — DisplayContent.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/wm/DisplayContent.java)
5. [AOSP frameworks/base — DisplayPolicy.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/wm/DisplayPolicy.java)
6. [AOSP frameworks/base — WindowLayout.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/core/java/android/view/WindowLayout.java)
7. [AOSP frameworks/base — WindowManagerPolicy.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/policy/WindowManagerPolicy.java)
8. [AOSP frameworks/base — InsetsStateController.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/wm/InsetsStateController.java)
9. [AOSP frameworks/base — InputMonitor.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/wm/InputMonitor.java)
10. [AOSP frameworks/base — DisplayAreaPolicyBuilder.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/wm/DisplayAreaPolicyBuilder.java)
11. [Android Developers — WindowManager.LayoutParams](https://developer.android.com/reference/android/view/WindowManager.LayoutParams)
12. [Android Developers — WindowInsets 与 Insets 处理](https://developer.android.com/develop/ui/views/layout/insets)
