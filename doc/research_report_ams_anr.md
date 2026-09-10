# Android 15 AMS ANR 处理机制深度分析报告

## 执行摘要

本报告基于本地 AOSP 源码（工作区 `frameworks_base_15/framework15`，Android 15）逐行取证，完整还原 ANR 从超时检测、上报、排队、抓栈、落盘到弹窗/杀进程的全链路。核心结论：Android 15 的 ANR 处理已经从旧版「AMS 主线程同步 dump + 弹窗」演进为一条高度异步化、可观测、可降级的流水线——`AnrTimer` 负责精确计时与延迟上报，`AnrHelper` 用独立 `AnrConsumer` 线程串行消费并在入队瞬间就提交「提前抓栈」（early dump）避免堆栈过期，真正的处理主体是 `ProcessErrorStateRecord.appNotResponding()`，它在 20 秒总预算内完成 firstPids / nativePids / extraPids 的分层抓取，并把结果写入 `/data/anr/anr_*`、DropBox（`data_app_anr` 等）、EventLog（`am_anr`）与 StatsLog（`ANR_OCCURRED`）。绝大多数「ANR 堆栈看不出问题」的排障困境，根源都在这条流水线的时间预算与降噪策略上，而不在应用层。

---

## 一、版本与代码坐标

本报告所有路径相对 `frameworks_base_15/framework15`。核心类坐标：

| 角色 | 类 | 路径 |
|---|---|---|
| 计时/延迟上报 | `AnrTimer` | `services/core/java/com/android/server/utils/AnrTimer.java` |
| 输入超时翻译层 | `AnrController`(wm) | `services/core/java/com/android/server/wm/AnrController.java` |
| 应用可插拔 ANR 策略 | `AnrController`(app) | `core/java/android/app/AnrController.java` |
| 超时描述对象 | `TimeoutRecord` | `core/java/com/android/internal/os/TimeoutRecord.java` |
| 排队与调度 | `AnrHelper` | `services/core/java/com/android/server/am/AnrHelper.java` |
| 处理主体 | `ProcessErrorStateRecord` | `services/core/java/com/android/server/am/ProcessErrorStateRecord.java` |
| 抓栈 | `StackTracesDumpHelper` | `services/core/java/com/android/server/am/StackTracesDumpHelper.java` |
| 耗时埋点 | `AnrLatencyTracker` | `core/java/com/android/internal/os/anr/AnrLatencyTracker.java` |
| 对话框 | `AppErrors` / `ErrorDialogController` | `services/core/java/com/android/server/am/` |

工作区未检出 ART（`art/runtime/signal_catcher.cc`）、native InputDispatcher 与 `IInputConstants.aidl`，涉及这三层的内容已明确标注为「按 AOSP 上游语义推断」。

---

## 二、ANR 的本质：不是「主线程卡了」，而是「系统侧计时器到期」

ANR（Application Not Responding）是 system_server 侧的一组**超时检测机制**的总称，而非应用内部的检测结果。共同模型是：system_server 在把某件事交给应用进程时启动一个计时器，应用在规定时间内没有通过 Binder 回调「我干完了」，计时器到期即触发 ANR。因此以下情况都会 ANR，即便主线程是空闲的：Binder 线程池被占满导致回调无法送达、进程被冻结（frozen）而 `AnrTimer` 未相应暂停、CPU 被其它进程/内核线程抢光导致回调迟到、system_server 自身持锁导致超时消息延迟处理。

`ProcessErrorStateRecord` 里的这一行最能说明问题——超时时刻是**记录下来的**，而不是处理时刻：

```360:365:services/core/java/com/android/server/am/ProcessErrorStateRecord.java
                ZonedDateTime timestamp = null;
                if (timeoutRecord != null && timeoutRecord.mEndUptimeMillis > 0) {
                    long millisSinceEndUptimeMs = anrTime - timeoutRecord.mEndUptimeMillis;
                    timestamp = Instant.now().minusMillis(millisSinceEndUptimeMs)
                                    .atZone(ZoneId.systemDefault());
                }
```

即：ANR 存在一个「到期时刻 `mEndUptimeMillis`」和「实际处理时刻 `anrTime`」，两者之差就是 `AnrHelper` 里的 `reportLatency`，也是日志里 `latency` 字段的来源。

---

## 三、触发源全景

### 3.1 输入分发超时（最常见）

native `InputDispatcher` 在派发事件后等待窗口/应用回执，超时（AOSP 默认 `UNMULTIPLIED_DEFAULT_DISPATCHING_TIMEOUT_MILLIS = 5000`，乘以 `Build.HW_TIMEOUT_MULTIPLIER`）后回调 Java 侧。Java 侧入口是 `WindowManagerService` 的 `AnrController`：

```68:74:services/core/java/com/android/server/wm/AnrController.java
    void notifyAppUnresponsive(InputApplicationHandle applicationHandle,
            TimeoutRecord timeoutRecord) {
        try {
            timeoutRecord.mLatencyTracker.notifyAppUnresponsiveStarted();
            timeoutRecord.mLatencyTracker.preDumpIfLockTooSlowStarted();
            preDumpIfLockTooSlow();
            timeoutRecord.mLatencyTracker.preDumpIfLockTooSlowEnded();
```

它有三个入口，分别对应「应用级无响应」「窗口级无响应（按 token）」「窗口级无响应（按 pid）」，见 `AnrController.java:68`、`AnrController.java:143`、`AnrController.java:204`。关键设计有三点。

第一，**归因修正**：如果 ANR 时正好有一个 pending 的焦点请求，则不怪当前 activity，而怪焦点窗口，见 `AnrController.java:93-124`：

```105:109:services/core/java/com/android/server/wm/AnrController.java
                    targetWindowState = focusTarget.getWindowState();
                    blamePendingFocusRequest = SystemClock.uptimeMillis()
                            - display.getInputMonitor().mInputFocusRequestTimeMillis
                            >= getInputDispatchingTimeoutMillisLocked(
                                    targetWindowState.getActivityRecord());
```

若窗口属于 activity 自身，走 `activity.inputDispatchingTimedOut(...)`；若是别的进程用该 token 加了窗（嵌入式场景），改为按 pid 归因，见 `ActivityRecord.java:7235-7262`：

```7251:7261:services/core/java/com/android/server/wm/ActivityRecord.java
            if (blameActivityProcess) {
                return mAtmService.mAmInternal.inputDispatchingTimedOut(anrApp.mOwner,
                        anrActivity.shortComponentName, anrActivity.info.applicationInfo,
                        shortComponentName, app, false, timeoutRecord);
            } else {
                // In this case another process added windows using this activity token.
                // So, we call the generic service input dispatch timed out method so
                // that the right process is blamed.
                long timeoutMillis = mAtmService.mAmInternal.inputDispatchingTimedOut(
                        windowPid, false /* aboveSystem */, timeoutRecord);
                return timeoutMillis <= 0;
            }
```

第二，**响应性恢复**：`AnrController` 会记住无响应的 app，等它的窗口重新获得焦点时通知 AMS 解除 `notResponding` 状态，见 `AnrController.java:263-273`（`onFocusChanged` → `inputDispatchingResumed`）。这解释了为什么有些 ANR 弹窗会自己消失。

第三，**调试构建专属的 pre-dump**：当 WM/AM 的 monitor 在 1 秒内拿不到锁时，说明 system_server 自己卡了，此时先抓一份 system_server + surfaceflinger 的栈并存为 `*_pre` 文件，见 `AnrController.java:282-352`（`PRE_DUMP_MONITOR_TIMEOUT_MS = 1s`，`PRE_DUMP_MIN_INTERVAL_MS = 20s`）。注意这只对 `Build.IS_DEBUGGABLE` 生效。

超时时长不是全局常量，而是可逐进程/逐 activity 覆盖的，最终由 AMS 读回：

```20045:20046:services/core/java/com/android/server/am/ActivityManagerService.java
        final long timeoutMillis = proc != null ? proc.getInputDispatchingTimeoutMillis() :
                DEFAULT_DISPATCHING_TIMEOUT_MILLIS;
```

AMS 的 `inputDispatchingTimedOut` 是本类 ANR 的汇入点，值得注意它硬编码了 `isContinuousAnr = true`：

```20085:20087:services/core/java/com/android/server/am/ActivityManagerService.java
                mAnrHelper.appNotResponding(proc, activityShortComponentName, aInfo,
                        parentShortComponentName, parentProcess, aboveSystem, timeoutRecord,
                        /*isContinuousAnr*/ true);
```

同时在把请求交给 `AnrHelper` 之前，会先处理两种特殊情形：正在被调试则直接返回 false（不上报），正在跑 instrumentation 则结束 instrumentation 并把 ANR 转交给测试框架（`ActivityManagerService.java:20073-20083`）。

### 3.2 广播超时

Android 14+ 引入了 `BroadcastQueueModernImpl`，广播 ANR 的计时由 `AnrTimer` 的子类 `BroadcastAnrTimer` 承担：

```1336:1341:services/core/java/com/android/server/am/BroadcastQueueModernImpl.java
    private class BroadcastAnrTimer extends AnrTimer<BroadcastProcessQueue> {
        BroadcastAnrTimer(@NonNull Handler handler) {
            super(Objects.requireNonNull(handler),
                    MSG_DELIVERY_TIMEOUT, "BROADCAST_TIMEOUT",
                    new AnrTimer.Args().extend(true).freeze(true));
        }
```

`extend(true)` 是广播特有的宽限机制：如果进程「可运行但在等 CPU」（cpuDelayTime），软超时到期时不立即 ANR，而是按被 CPU 饿死的时间延长一次，第二次才硬 ANR，见 `BroadcastQueueModernImpl.java:1310-1318`：

```1310:1317:services/core/java/com/android/server/am/BroadcastQueueModernImpl.java
            // Instead of immediately triggering an ANR, extend the timeout by
            // the amount of time the process was runnable-but-waiting; we're
            // only willing to do this once before triggering an hard ANR
            final long cpuDelayTime = queue.app.getCpuDelayTime() - queue.lastCpuDelayTime;
            final long hardTimeoutMillis = MathUtils.constrain(cpuDelayTime, 0, softTimeoutMillis);
            mAnrTimer.start(queue, hardTimeoutMillis);
        } else {
            deliveryTimeoutLocked(queue);
        }
```

真正上报时构造 `TimeoutRecord.forBroadcastReceiver(...)` 并调用 `mService.appNotResponding`，见 `BroadcastQueueModernImpl.java:1455-1463`。若目标进程正在被调试，则 `discard` 掉计时器而不上报。

超时基线在 AMS 里定义并注入 `BroadcastConstants`：`BROADCAST_FG_TIMEOUT = 10s`、`BROADCAST_BG_TIMEOUT = 60s`（`ActivityManagerService.java:590-591`，在 `20873/20877` 赋给 `foreConstants.TIMEOUT` / `backConstants.TIMEOUT`）。

### 3.3 Service 超时

`ActiveServices` 持有三个独立的 `AnrTimer`：

```776:785:services/core/java/com/android/server/am/ActiveServices.java
        this.mActiveServiceAnrTimer = new ProcessAnrTimer(service,
                ActivityManagerService.SERVICE_TIMEOUT_MSG,
                "SERVICE_TIMEOUT",
                new AnrTimer.Args().freeze(true));
        this.mShortFGSAnrTimer = new ServiceAnrTimer(service,
                ActivityManagerService.SERVICE_SHORT_FGS_ANR_TIMEOUT_MSG,
                "SHORT_FGS_TIMEOUT");
        this.mServiceFGAnrTimer = new ServiceAnrTimer(service,
                ActivityManagerService.SERVICE_FOREGROUND_TIMEOUT_MSG,
                "SERVICE_FOREGROUND_TIMEOUT");
```

其中 `mActiveServiceAnrTimer` 用了 `freeze(true)`：进程被 freezer 冻结时计时暂停，避免冻结期间「白跑」计时器。`SERVICE_TIMEOUT_MSG` 由 AMS 主线程 handler 分发到 `mServices.serviceTimeout(...)`（`ActivityManagerService.java:1858-1860`）。

超时判定的核心是「服务开始执行的时刻是否早于 now - 阈值」，且前台/后台用不同阈值：

```7507:7511:services/core/java/com/android/server/am/ActiveServices.java
                final long now = SystemClock.uptimeMillis();
                final long maxTime =  now
                        - (psr.shouldExecServicesFg()
                        ? mAm.mConstants.SERVICE_TIMEOUT
                        : mAm.mConstants.SERVICE_BACKGROUND_TIMEOUT);
```

命中后构造 `TimeoutRecord.forServiceExec(timeout.shortInstanceName, waitedMillis)` 并交给 `AnrHelper`，见 `ActiveServices.java:7524-7551`。未命中则重新排一个精确的下一次超时（而不是固定周期），见 `ActiveServices.java:7541-7545`。

`serviceForegroundTimeout` 覆盖的是另一类经典 ANR：调用了 `Context.startForegroundService()` 却没在时限内调 `Service.startForeground()`，见 `ActiveServices.java:7562-7565`。

### 3.4 ContentProvider 超时

有两条独立路径。其一是「provider 宿主不响应」——客户端可通过 `AMS.appNotRespondingViaProvider(IBinder)` 主动举报：

```1010:1012:services/core/java/com/android/server/am/ContentProviderHelper.java
            TimeoutRecord timeoutRecord = TimeoutRecord.forContentProvider(
                    "ContentProvider not responding");
            mService.mAnrHelper.appNotResponding(host, timeoutRecord);
```

其二是「provider 发布超时」，即进程启动后迟迟不 `publishContentProviders`，这**不会产生 ANR 日志**，而是直接杀进程：

```1235:1240:services/core/java/com/android/server/am/ContentProviderHelper.java
    void processContentProviderPublishTimedOutLocked(ProcessRecord app) {
        cleanupAppInLaunchingProvidersLocked(app, true);
        mService.mProcessList.removeProcessLocked(app, false, true,
                ApplicationExitInfo.REASON_INITIALIZATION_FAILURE,
                ApplicationExitInfo.SUBREASON_UNKNOWN,
                "timeout publishing content providers");
    }
```

超时值来自 `ContentResolver.CONTENT_PROVIDER_PUBLISH_TIMEOUT_MILLIS`（默认 10s），由 `AMS:4700-4703` 埋点、`CONTENT_PROVIDER_PUBLISH_TIMEOUT_MSG` 投递（`AMS:1907-1912`）。

### 3.5 进程/应用启动超时

Android 15 把「进程启动」拆成了软硬两级超时：`PROC_START_TIMEOUT = 10s`（等进程 attach）与 `BIND_APPLICATION_TIMEOUT = 15s`（等 Application 绑定完成），见 `ActivityManagerService.java:574`、`577`。软超时到期会先延长一段时间并打 trace，硬超时到期才真正走 ANR：

```5102:5108:services/core/java/com/android/server/am/ActivityManagerService.java
    private void handleBindApplicationTimeoutHard(ProcessRecord app) {
        final String anrMessage;
        synchronized (app) {
            anrMessage = "Process " + app + " failed to complete startup";
        }

        mAnrHelper.appNotResponding(app, TimeoutRecord.forAppStart(anrMessage));
    }
```

### 3.6 应用自报

应用可以主动调用 `AMS.appNotResponding(reason)` 触发一次 ANR 采集（用于自检/上报），见 `ActivityManagerService.java:7050-7073`，其 `TimeoutRecord.forApp` 的 reason 前缀是 `"App requested: "`。

### 3.7 触发源汇总表

| ANR 类型 | 触发条件 | 默认超时 | 常量/定义位置 | 上报点 |
|---|---|---|---|---|
| 输入分发 | 窗口/应用未及时消费输入事件 | 5s | `InputConstants.DEFAULT_DISPATCHING_TIMEOUT_MILLIS` | `AMS.inputDispatchingTimedOut` (20085) |
| 前台广播 | Receiver `onReceive` 未按时结束 | 10s | `AMS.BROADCAST_FG_TIMEOUT` (590) | `BroadcastQueueModernImpl` (1463) |
| 后台广播 | 同上（后台队列） | 60s | `AMS.BROADCAST_BG_TIMEOUT` (591) | 同上 |
| 前台 Service | Service 生命周期回调超时 | 20s | `ActivityManagerConstants.DEFAULT_SERVICE_TIMEOUT` (310) | `ActiveServices.serviceTimeout` (7550) |
| 后台 Service | 同上 | 200s | `DEFAULT_SERVICE_BACKGROUND_TIMEOUT` (315) | 同上 |
| 短时 FGS | `shortService` 超时 | 见 `ServiceRecord$ShortFgsInfo` | `mShortFGSAnrTimer` (780) | `onShortFgsAnrTimeout` |
| FGS 未调 startForeground | `startForegroundService` 后未提前台 | 30s | `DEFAULT_SERVICE_START_FOREGROUND_TIMEOUT_MS` (301) | `serviceForegroundTimeoutANR` (7608) |
| ContentProvider 不响应 | 客户端主动举报 | 客户端决定 | `forContentProvider` | `ContentProviderHelper` (1012) |
| Provider 发布超时 | 启动后未 publish | 10s | `CONTENT_PROVIDER_PUBLISH_TIMEOUT_MILLIS` | 直接杀进程，无 ANR |
| 进程 attach 超时 | fork 后未 attach | 10s | `AMS.PROC_START_TIMEOUT` (574) | 先清理后杀 |
| App 启动超时 | `bindApplication` 未完成 | 15s | `AMS.BIND_APPLICATION_TIMEOUT` (577) | `handleBindApplicationTimeoutHard` (5108) |
| 应用自报 | App 主动调用 | N/A | — | `AMS.appNotResponding` (7066) |

---

## 四、TimeoutRecord 与 AnrTimer：ANR 的「元数据」与「闹钟」

`TimeoutRecord` 是不可变的超时描述对象，核心字段是 `mKind`、`mReason`、`mEndUptimeMillis`、`mLatencyTracker`，以及一个可关闭的 `mExpiredTimer`（`TimeoutRecord.java:71-92`）。工厂方法按类型划分（`TimeoutRecord.java:106-203`）：`forBroadcastReceiver`、`forInputDispatchNoFocusedWindow`、`forInputDispatchWindowUnresponsive`、`forServiceExec`、`forServiceStartWithEndTime`、`forContentProvider`、`forApp`、`forShortFgsTimeout`、`forJobService`、`forAppStart`。

其中 `endingNow` 与 `endingApproximatelyNow` 的区别（`TimeoutRecord.java:96-104`）很关键：前者表示超时时刻是在**拿锁之前**取的（精确），后者表示是**拿锁之后**取的（近似，可能偏晚）。这直接影响后续日志里时间戳的可信度。

`AnrTimer` 是 Android 14 引入的通用计时器抽象（`AnrTimer.java:86`），取代了过去散落在各处的 `sendMessageDelayed`。它的价值在于三点：支持 `extend`（广播的 CPU 饿死宽限）、支持 `freeze`（进程冻结时暂停计时）、`accept()` 返回一个 `AutoCloseable` 句柄交给 `TimeoutRecord`，由处理方在合适时机 `closeExpiredTimer()` 释放（`ProcessErrorStateRecord.java:307`）。它是 feature-flag 化的（`Flags.anrTimerServiceEnabled`），关闭时回落到老的 Handler 消息机制，见 `BroadcastQueueModernImpl.java:1283-1301` 的双分支实现。

---

## 五、AnrHelper：排队、去重与「提前抓栈」

`AnrHelper` 是整个 Android 15 ANR 架构里最重要的工程改进，解决的是一个真实痛点：一次完整 ANR dump 要几秒，而 ANR 应用的堆栈在几百毫秒后就会「过期」（应用恢复了，栈就不卡了），同时系统卡顿时还可能出现 ANR 风暴。

它的做法分四步。其一，**三级去重**：

```133:158:services/core/java/com/android/server/am/AnrHelper.java
                if (incomingPid == 0) {
                    // Extreme corner case such as zygote is no response
                    // to return pid for the process.
                    Slog.i(TAG, "Skip zero pid ANR, process=" + anrProcess.processName);
                    return;
                }
                if (mProcessingPid == incomingPid) {
                    Slog.i(TAG,
                            "Skip duplicated ANR, pid=" + incomingPid + " "
                            + timeoutRecord.mReason);
                    return;
                }
                if (!mTempDumpedPids.add(incomingPid)) {
                    Slog.i(TAG,
                            "Skip ANR being predumped, pid=" + incomingPid + " "
                            + timeoutRecord.mReason);
                    return;
                }
                for (int i = mAnrRecords.size() - 1; i >= 0; i--) {
                    if (mAnrRecords.get(i).mPid == incomingPid) {
                        Slog.i(TAG,
                                "Skip queued ANR, pid=" + incomingPid + " "
                                + timeoutRecord.mReason);
                        return;
                    }
                }
```

即：正在处理的同一 pid 丢弃、正在提前 dump 的同一 pid 丢弃、已在队列中的同一 pid 丢弃。

其二，**入队瞬间提交提前抓栈**，不等 `AnrConsumer` 轮到它：

```164:171:services/core/java/com/android/server/am/AnrHelper.java
                Future<File> firstPidDumpPromise = mEarlyDumpExecutor.submit(() -> {
                    // the class AnrLatencyTracker is not generally thread safe but the values
                    // recorded/touched by the Temporary dump thread(s) are all volatile/atomic.
                    File tracesFile = StackTracesDumpHelper.dumpStackTracesTempFile(incomingPid,
                            timeoutRecord.mLatencyTracker);
                    mTempDumpedPids.remove(incomingPid);
                    return tracesFile;
                });
```

这个 `Future<File>` 会一路传到 `ProcessErrorStateRecord`，最终被 `StackTracesDumpHelper.copyFirstPidTempDump` 合并进正式 traces 文件。这是「ANR 堆栈终于能抓到真凶」的关键。

其三，**单线程串行消费**，`AnrConsumer` 线程只在有活时存在（`AnrHelper.java:205-258`），且线程工厂命名为 `AnrConsumer` / `AnrMainProcessDumpThread` / `AnrAuxiliaryTaskExecutor`（`AnrHelper.java:75-78`）——分析 traces 时看到这些线程名就知道在干 ANR。

其四，**降级策略**：如果 ANR 入队后延迟超过 10 秒，或开机还没到 10 分钟，则只 dump ANR 进程自己，避免雪上加霜：

```235:241:services/core/java/com/android/server/am/AnrHelper.java
                final long startTime = SystemClock.uptimeMillis();
                // If there are many ANR at the same time, the latency may be larger.
                // If the latency is too large, the stack trace might not be meaningful.
                final long reportLatency = startTime - r.mTimestamp;
                final boolean onlyDumpSelf = reportLatency > EXPIRED_REPORT_TIME_MS
                        || startTime < SELF_ONLY_AFTER_BOOT_MS;
                r.appNotResponding(onlyDumpSelf);
```

（`EXPIRED_REPORT_TIME_MS = 10s`，`CONSECUTIVE_ANR_TIME_MS = 2min`，`SELF_ONLY_AFTER_BOOT_MS = 10min`，见 `AnrHelper.java:58-68`。）另外，若两次 ANR 间隔小于 2 分钟，会自动开启 Binder Heavy Hitter 采样器（`AnrHelper.java:260-272`），用于排查 Binder 拥塞型 ANR。

---

## 六、处理主体：ProcessErrorStateRecord.appNotResponding

### 6.1 前置过滤

入口签名（`ProcessErrorStateRecord.java:293-297`）。第一步是释放 `AnrTimer` 句柄并检查调试态：

```306:322:services/core/java/com/android/server/am/ProcessErrorStateRecord.java
        // Release the expired timer preparatory to starting the dump or returning without dumping.
        timeoutRecord.closeExpiredTimer();

        if (mApp.isDebugging()) {
            Slog.i(TAG, "Skipping debugged app ANR: " + this + " " + annotation);
            return;
        }

        mApp.getWindowProcessController().appEarlyNotResponding(annotation, () -> {
```

`appEarlyNotResponding` 是给 `IActivityController`（Monkey/CTS/车机）的钩子，返回值 `< 0` 表示立刻杀进程，且对 system_server 自身（`MY_PID`）不生效，见 `WindowProcessController.java:1844-1865`。

随后拿 AMS 大锁，做计数与 `skipAnrLocked` 判定：

```271:291:services/core/java/com/android/server/am/ProcessErrorStateRecord.java
    boolean skipAnrLocked(String annotation) {
        // PowerManager.reboot() can block for a long time, so ignore ANRs while shutting down.
        if (mService.mAtmInternal.isShuttingDown()) {
            Slog.i(TAG, "During shutdown skipping ANR: " + this + " " + annotation);
            return true;
        } else if (isNotResponding()) {
            Slog.i(TAG, "Skipping duplicate ANR: " + this + " " + annotation);
            return true;
        } else if (isCrashing()) {
            Slog.i(TAG, "Crashing app skipping ANR: " + this + " " + annotation);
            return true;
        } else if (mApp.isKilledByAm()) {
            Slog.i(TAG, "App already killed by AM skipping ANR: " + this + " " + annotation);
            return true;
        } else if (mApp.isKilled()) {
            Slog.i(TAG, "Skipping died app ANR: " + this + " " + annotation);
            return true;
        }
        return false;
    }
```

注意 Android 15 的关机判定用 `mAtmInternal.isShuttingDown()`，而非旧版的 `mService.mShuttingDown` 字段。计数通过 `Counter.logIncrement("stability_anr.value_total_anrs")` 与 `"stability_anr.value_skipped_anrs"` 完成（`ProcessErrorStateRecord.java:347-350`），对应 `stability_anr` 的 Express/Counter 指标。

### 6.2 状态写入与 ErrorId

在 `mProcLock` 下设 `setNotResponding(true)`（同时会同步给 `WindowProcessController`，见 `ProcessErrorStateRecord.java:194-197`），并计算 ANR 发生的真实墙钟时间。随后写 EventLog：

```373:375:services/core/java/com/android/server/am/ProcessErrorStateRecord.java
            // Log the ANR to the event log.
            EventLog.writeEvent(EventLogTags.AM_ANR, mApp.userId, pid, mApp.processName,
                    mApp.info.flags, annotation);
```

`am_anr` 的字段定义在 `services/core/java/com/android/server/am/EventLogTags.logtags:17`：`30008 am_anr (User|1|5),(pid|1|5),(Package Name|3),(Flags|1|5),(reason|3)`。这是自动化 ANR 采集最应该接的数据源——结构化、轻量、不依赖 traces 文件。

`ErrorId` 由 `TraceErrorLogger` 生成并写入当前 trace，用于把 ANR 日志和 Perfetto trace 关联起来；紧接着写 `FrameworkStatsLog.ANR_OCCURRED_PROCESSING_STARTED`，注释明确说明这个 atom 的唯一目的是**触发 Perfetto 抓取**：

```387:392:services/core/java/com/android/server/am/ProcessErrorStateRecord.java
            // This atom is only logged with the purpose of triggering Perfetto and the logging
            // needs to happen as close as possible to the time when the ANR is detected.
            // Also, it needs to be logged after adding the error id to the trace, to make sure
            // the error id is present in the trace when the Perfetto trace is captured.
            FrameworkStatsLog.write(FrameworkStatsLog.ANR_OCCURRED_PROCESSING_STARTED,
                    mApp.processName);
```

### 6.3 PID 集合的构造规则

这是理解 traces 文件结构的钥匙：

```394:429:services/core/java/com/android/server/am/ProcessErrorStateRecord.java
            // Dump thread traces as quickly as we can, starting with "interesting" processes.
            firstPids.add(pid);

            // Don't dump other PIDs if it's a background ANR or is requested to only dump self.
            isSilentAnr = isSilentAnr();
            if (!isSilentAnr && !onlyDumpSelf) {
                int parentPid = pid;
                if (parentProcess != null && parentProcess.getPid() > 0) {
                    parentPid = parentProcess.getPid();
                }
                if (parentPid != pid) firstPids.add(parentPid);

                if (MY_PID != pid && MY_PID != parentPid) firstPids.add(MY_PID);

                final int ppid = parentPid;
                mService.mProcessList.forEachLruProcessesLOSP(false, r -> {
                    if (r != null && r.getThread() != null) {
                        int myPid = r.getPid();
                        if (myPid > 0 && myPid != pid && myPid != ppid && myPid != MY_PID) {
                            if (r.isPersistent()) {
                                firstPids.add(myPid);
                            } else if (r.mServices.isTreatedLikeActivity()) {
                                firstPids.add(myPid);
                            } else {
                                lastPids.put(myPid, true);
                            }
                        }
                    }
                });
            }
```

即 `firstPids` = [ANR 进程, 父进程, system_server, 所有 persistent 进程, 疑似 IME 的进程]；其余 Java 进程进 `lastPids`（只是候选池，最终只会挑 CPU 占用最高的 2 个真正 dump，见 6.5）。`nativePids` 在辅助线程异步收集，且非系统应用只收集自己（若自己是 native 关注进程）：

```501:514:services/core/java/com/android/server/am/ProcessErrorStateRecord.java
                        String[] nativeProcs = null;
                        boolean isSystemApp = mApp.info.isSystemApp() || mApp.info.isSystemExt();
                        // Do not collect system daemons dumps as this is not likely to be useful
                        // for non-system apps.
                        if (!isSystemApp || isSilentAnr || onlyDumpSelf) {
                            for (int i = 0; i < NATIVE_STACKS_OF_INTEREST.length; i++) {
                                if (NATIVE_STACKS_OF_INTEREST[i].equals(mApp.processName)) {
                                    nativeProcs = new String[] { mApp.processName };
                                    break;
                                }
                            }
                        } else {
                            nativeProcs = NATIVE_STACKS_OF_INTEREST;
                        }
```

`NATIVE_STACKS_OF_INTEREST` 来自 `com.android.server.Watchdog`，包含 servicemanager、surfaceflinger、mediaserver 等。

### 6.4 日志块逐行来源

主日志那一段 `ANR in xxx` 就是下面这段拼的（`ProcessErrorStateRecord.java:444-462`）：

```446:462:services/core/java/com/android/server/am/ProcessErrorStateRecord.java
        info.append("ANR in ").append(mApp.processName);
        if (activityShortComponentName != null) {
            info.append(" (").append(activityShortComponentName).append(")");
        }
        info.append("\n");
        info.append("PID: ").append(pid).append("\n");
        if (annotation != null) {
            info.append("Reason: ").append(annotation).append("\n");
        }
        if (parentShortComponentName != null
                && parentShortComponentName.equals(activityShortComponentName)) {
            info.append("Parent: ").append(parentShortComponentName).append("\n");
        }
        if (errorId != null) {
            info.append("ErrorId: ").append(errorId.toString()).append("\n");
        }
        info.append("Frozen: ").append(mApp.mOptRecord.isFrozen()).append("\n");
```

之后追加 PSI（Pressure Stall Information）状态、CPU load（`processCpuTracker.printCurrentLoad()`）与 CPU 占用排行（`printCurrentState(anrTime)`），最后 `Slog.e(TAG, info.toString())`。`Frozen: true` 是 Android 11+ freezer 引入的字段，为 true 时说明 ANR 时进程处于冻结态——通常意味着进程状态判定或解冻路径有问题，而非应用真的在跑。

### 6.5 抓栈调用

```535:539:services/core/java/com/android/server/am/ProcessErrorStateRecord.java
        File tracesFile = StackTracesDumpHelper.dumpStackTraces(firstPids,
                isSilentAnr ? null : processCpuTracker, isSilentAnr ? null : lastPids,
                nativePidsFuture, tracesFileException, firstPidEndOffset, annotation,
                criticalEventLog, memoryHeaders, auxiliaryTaskExecutor, firstPidFilePromise,
                latencyTracker);
```

注意背景 ANR（`isSilentAnr`）传 null，省掉约 500ms 的 CPU 统计与 lastPids 排序。`firstPidEndOffset` 记录第一个进程栈在文件中的结束偏移，用于只把「属于该应用的那一段」通过 `AppExitInfoTracker.scheduleLogAnrTrace` 暴露给应用自己（隐私考虑，见 `ProcessErrorStateRecord.java:564-575`）：

```564:575:services/core/java/com/android/server/am/ProcessErrorStateRecord.java
        if (tracesFile == null) {
            // There is no trace file, so dump (only) the alleged culprit's threads to the log
            Process.sendSignal(pid, Process.SIGNAL_QUIT);
        } else if (firstPidEndOffset.get() > 0) {
            final long startOffset = 0L;
            final long endOffset = firstPidEndOffset.get();
            mService.mProcessList.mAppExitInfoTracker.scheduleLogAnrTrace(
                    pid, mApp.uid, mApp.getPackageList(), tracesFile, startOffset, endOffset);
        }
```

若连文件都创建失败，退化到最原始的 `SIGQUIT` 让应用自己把栈打进 logcat。

### 6.6 落盘与指标

DropBox 的 tag 由 `processClass(process) + "_" + eventType` 拼成，`processClass` 分三类（`ActivityManagerService.java:9839-9847`）：

```9839:9846:services/core/java/com/android/server/am/ActivityManagerService.java
    private static String processClass(ProcessRecord process) {
        if (process == null || process.getPid() == MY_PID) {
            return "system_server";
        } else if (process.info.isSystemApp() || process.info.isSystemExt()) {
            return "system_app";
        } else {
            return "data_app";
        }
    }
```

所以 ANR 的 dropbox tag 就是 `data_app_anr` / `system_app_anr` / `system_server_anr`。写入前会先过 `DropboxRateLimiter`（`ActivityManagerService.java:9894-9897`）——这也是为什么短时间内大量 ANR 时 dropbox 里只有一部分。调用点在 `ProcessErrorStateRecord.java:646-649`。

随后写完整的 `ANR_OCCURRED` atom，包含前台/后台状态、进程类别、instant app 标记、以及 Incremental 安装的一堆指标（是否还在加载、加载进度、读延迟等），见 `ProcessErrorStateRecord.java:608-643`。这一大坨是排查「ANR 其实是代码还在从 Incremental FS 加载」的关键证据。

### 6.7 后续处置：三条分岔

```651:675:services/core/java/com/android/server/am/ProcessErrorStateRecord.java
        if (mApp.getWindowProcessController().appNotResponding(info.toString(),
                () -> {
                    synchronized (mService) {
                        mApp.killLocked("anr", ApplicationExitInfo.REASON_ANR, true);
                    }
                },
                () -> {
                    synchronized (mService) {
                        mService.mServices.scheduleServiceTimeoutLocked(mApp);
                    }
                })) {
            return;
        }
        ...
            if (isSilentAnr() && !mApp.isDebugging()) {
                mApp.killLocked("bg anr", ApplicationExitInfo.REASON_ANR, true);
                return;
            }
```

第一条是 `IActivityController` 决策（同 `appEarlyNotResponding` 的返回值语义：0 弹窗、1 继续等待、-1 立刻杀，见 `WindowProcessController.java:1867-1885`）。第二条是**后台 ANR 直接杀进程、不弹窗**——`isSilentAnr()` 的定义是「未开启 `Settings.Secure.ANR_SHOW_BACKGROUND` 且该进程不值得关注」（`ProcessErrorStateRecord.java:783-785`，判定细节见 `728-754`：system_server 永远算关注，此外要求有可见 Activity、是 SystemUI、或有 overlay/top UI）。第三条才是常规路径：`makeAppNotRespondingLSP` 生成报告 + 发 `SHOW_NOT_RESPONDING_UI_MSG`。

```686:694:services/core/java/com/android/server/am/ProcessErrorStateRecord.java
            if (mService.mUiHandler != null) {
                // Bring up the infamous App Not Responding dialog
                Message msg = Message.obtain();
                msg.what = ActivityManagerService.SHOW_NOT_RESPONDING_UI_MSG;
                msg.obj = new AppNotRespondingDialog.Data(mApp, aInfo, aboveSystem,
                        isContinuousAnr);

                mService.mUiHandler.sendMessageDelayed(msg, anrDialogDelayMs);
            }
```

`anrDialogDelayMs` 来自应用注册的 `AnrController`（见下节），默认为 0。`SHOW_NOT_RESPONDING_UI_MSG` 由 AMS 的 UiHandler 分发到 `AppErrors.handleShowAnrUi`（`ActivityManagerService.java:1776-1779`）。

---

## 七、AnrController：应用可插拔的 ANR 策略

`android.app.AnrController`（`core/java/android/app/AnrController.java:23-51`）允许系统应用（如 OEM 的桌面/性能管家）介入 ANR 对话框：

```35:50:core/java/android/app/AnrController.java
    long getAnrDelayMillis(String packageName, int uid);

    void onAnrDelayStarted(String packageName, int uid);

    boolean onAnrDelayCompleted(String packageName, int uid);
```

流程是：在 dump 之前先取 controller 与 delay（这样即使 dump 耗时几秒也不会错过 delay 窗口，代码注释明确说明了这点，见 `ProcessErrorStateRecord.java:464-478`），`onAnrDelayStarted` 给 controller 机会展示「正在恢复中」的进度 UI，对话框延迟 `anrDialogDelayMs` 后发出，`handleShowAnrUi` 再调 `onAnrDelayCompleted` 由 controller 决定最终是否真的弹窗：

```1110:1128:services/core/java/com/android/server/am/AppErrors.java
                AnrController anrController = errState.getDialogController().getAnrController();
                if (anrController == null) {
                    errState.getDialogController().showAnrDialogs(data);
                } else {
                    String packageName = proc.info.packageName;
                    int uid = proc.info.uid;
                    boolean showDialog = anrController.onAnrDelayCompleted(packageName, uid);

                    if (showDialog) {
                        Slog.d(TAG, "ANR delay completed. Showing ANR dialog for package: "
                                + packageName);
                        errState.getDialogController().showAnrDialogs(data);
                    } else {
                        Slog.d(TAG, "ANR delay completed. Cancelling ANR dialog for package: "
                                + packageName);
                        errState.setNotResponding(false);
                        errState.setNotRespondingReport(null);
                        errState.getDialogController().clearAnrDialogs();
                    }
                }
```

若设备当前不允许弹错误对话框（息屏、锁屏、无输入设备等）且未开启 `ANR_SHOW_BACKGROUND`，则记录 `CANT_SHOW` 并 `doKill = true` 直接杀进程（`AppErrors.java:1109`、`1130-1134`）。对话框本体由 `ErrorDialogController.showAnrDialogs` 创建，注意它会按显示设备数量创建多个 `AppNotRespondingDialog`（多屏场景），见 `ErrorDialogController.java:188-197`。对话框可通过 `AMS:19335-19341` 的 `rescheduleAnrDialog` 重新排期（延迟一个输入超时时长）。

---

## 八、抓栈机制：StackTracesDumpHelper

### 8.1 文件与总预算

traces 目录固定为 `/data/anr`，正式文件名 `anr_yyyy-MM-dd-HH-mm-ss-SSS`，临时文件前缀 `temp_anr_`，见 `StackTracesDumpHelper.java:70-73`。**全部抓栈必须在 20 秒内完成**，这是硬预算：

```202:203:services/core/java/com/android/server/am/StackTracesDumpHelper.java
        // We must complete all stack dumps within 20 seconds.
        long remainingTime = 20 * 1000 * Build.HW_TIMEOUT_MULTIPLIER;
```

其它关键常量：`NATIVE_DUMP_TIMEOUT_MS = 2000`（单个 native 栈）、`TEMP_DUMP_TIME_LIMIT = 10000`（提前 dump 的单进程上限）、`JAVA_DUMP_MINIMUM_SIZE = 100`（判定 Java dump 是否为空），见 `StackTracesDumpHelper.java:74-79`。旧文件清理策略是「最多保留 `tombstoned.max_anr_count`（默认 64）个，且超过 1 天删除」，见 `StackTracesDumpHelper.java:507-527`。

### 8.2 抓取顺序与合并

顺序是：合并提前 dump 的临时文件 → firstPids → nativePids → extraPids。合并提前 dump 是第一步，且合并成功后会立刻把 latency 数组追加进文件：

```215:236:services/core/java/com/android/server/am/StackTracesDumpHelper.java
        if (firstPidFilePromise != null && firstPids != null && firstPids.size() > 0) {
            final int primaryPid = firstPids.get(0);
            final long start = SystemClock.elapsedRealtime();
            firstPidTempDumpCopied = copyFirstPidTempDump(tracesFile, firstPidFilePromise,
                    remainingTime, latencyTracker);
            final long timeTaken = SystemClock.elapsedRealtime() - start;
            remainingTime -= timeTaken;
            ...
```

注意 `firstPidEnd` 只在「不是 system_server」时才记录（`StackTracesDumpHelper.java:228-230`）——system_server 的栈被认为不应暴露给应用。

`extraPids` 的选取逻辑值得单独说：它先 `processCpuTracker.init()`，睡 200ms，再 `update()`，从 CPU 占用最高的进程里挑**最多 2 个且必须属于 lastPids** 的：

```478:492:services/core/java/com/android/server/am/StackTracesDumpHelper.java
            final int workingStatsNumber = processCpuTracker.countWorkingStats();
            for (int i = 0; i < workingStatsNumber && extraPids.size() < 2; i++) {
                ProcessCpuTracker.Stats stats = processCpuTracker.getWorkingStats(i);
                if (lastPids.indexOfKey(stats.pid) >= 0) {
                    extraPids.add(stats.pid);
                } else {
                    Slog.i(TAG,
                            "Skipping next CPU consuming process, not a java proc: "
                            + stats.pid);
                }
            }
```

所以「traces 文件里只有 2 个其它进程的栈」是设计使然，不是丢数据。

### 8.3 Java 栈是怎么抓出来的

核心是 `Debug.dumpJavaBacktraceToFileTimeout(pid, fileName, timeoutSec)`（`StackTracesDumpHelper.java:553`）。其 native 实现（本工作区未检出 `framework15/core/jni/android_os_Debug.cpp` 与 `art/runtime/signal_catcher.cc`，按 AOSP 上游语义）为：通过 `tombstoned` 建立 intercept fd，向目标进程的 SignalCatcher 线程发信号，ART 的 `ThreadList::DumpForSigQuit` 收集所有 Java 线程栈并写回该 fd。若 Java dump 失败或产出小于 100 字节，自动回退到 native backtrace：

```550:575:services/core/java/com/android/server/am/StackTracesDumpHelper.java
    private static long dumpJavaTracesTombstoned(int pid, String fileName, long timeoutMs) {
        final long timeStart = SystemClock.elapsedRealtime();
        int headerSize = writeUptimeStartHeaderForPid(pid, fileName);
        boolean javaSuccess = Debug.dumpJavaBacktraceToFileTimeout(pid, fileName,
                (int) (timeoutMs / 1000));
        ...
        if (!javaSuccess) {
            Slog.w(TAG, "Dumping Java threads failed, initiating native stack dump.");
            if (!Debug.dumpNativeBacktraceToFileTimeout(pid, fileName,
                    (NATIVE_DUMP_TIMEOUT_MS / 1000))) {
                Slog.w(TAG, "Native stack dump failed!");
            }
        }
```

这也解释了 traces 里常见的现象：某些进程只有 native 栈没有 Java 栈（进程正在 fork/zygote 阶段、或 ART 已不响应）。

### 8.4 AnrLatencyTracker：ANR 自身的耗时体检表

`AnrLatencyTracker`（`core/java/com/android/internal/os/anr/AnrLatencyTracker.java`）把整条链路切成约 50 个可测点，包括 `appNotRespondingStarted/Ended`、`earlyDumpRequestSubmittedWithSize`、`anrRecordPlacingOnQueueWithSize`、`waitingOnAMSLockStarted/Ended`、`waitingOnGlobalLockStarted/Ended`、`waitingOnProcLockStarted/Ended`、`copyingFirstPidStarted/Ended`、`dumpingFirstPidsStarted/Ended`、`dumpingNativePidsStarted/Ended`、`dumpingExtraPidsStarted/Ended`、`dumpStackTracesTempFileTimedOut` 等。结果以逗号分隔数组形式追加到 traces 文件头部：

```231:235:services/core/java/com/android/server/am/StackTracesDumpHelper.java
            // Append the Durations/latency comma separated array after the first PID.
            if (firstPidTempDumpCopied && latencyTracker != null) {
                appendtoANRFile(tracesFile,
                        latencyTracker.dumpAsCommaSeparatedArrayWithHeader());
            }
```

**这份数据是判断「ANR 处理本身是否被系统卡顿污染」的第一手证据**：如果 `waitingOnAMSLock` 或 `copyingFirstPid` 耗时巨大，说明 system_server 自己已经很忙，traces 里的栈很可能已经过期。

---

## 九、Android 15 相对旧版的关键演进

从代码线索可以清晰看到几个代际变化。其一，`AnrHelper` 与 `AnrConsumer` 线程把 ANR 处理从 AMS 主线程剥离（2020 年引入），彻底解决了「处理 ANR 本身加剧卡顿」的问题。其二，早期 dump（`firstPidFilePromise`）保证主进程的栈在几百毫秒内被抓到，而不是等几秒后。其三，`AnrTimer` 统一了所有超时机制的计时，并引入 `extend`（CPU 饿死宽限）与 `freeze`（冻结暂停）语义。其四，可观测性大幅增强：ErrorId + Perfetto 联动、PSI 状态、内存头（RssHwmKb / RssKb / RssAnonKb / RssShmemKb / VmSwapKb，见 `ProcessErrorStateRecord.java:756-776`）、CriticalEventLog、`stability_anr` counter、以及完整的 latency 埋点。其五，`isContinuousAnr` 标记（输入类 ANR 恒为 true）与 `AnrController` 延迟弹窗，让系统可以对「持续无响应」和「瞬间卡顿」区别对待。

---

## 十、排障手册

**取日志。** ANR 主日志用 `adb logcat -b main -s ActivityManager:E` 过滤 `ANR in`；结构化事件用 `adb logcat -b events | grep am_anr`（字段：User / pid / Package Name / Flags / reason）；traces 文件在 `/data/anr/anr_*`，用 `adb pull /data/anr/`；dropbox 条目用 `adb shell dumpsys dropbox --print data_app_anr`。

**看 latency。** 打开 traces 文件头部的 latency 数组，若 `waitingOnAMSLock`、`waitingOnGlobalLock` 或 `copyingFirstPid` 异常大，说明栈已过期，应转向 CPU/内存/PSI 段与 perfetto trace 分析，而不是死磕主线程栈。

**区分前台/后台。** 后台 ANR（`isSilentAnr`）会被直接杀掉且不弹窗、不收集 native 栈、不统计 CPU。开发期可用 `adb shell settings put secure anr_show_background 1` 打开后台 ANR 弹窗以观察。

**关注 Frozen 与 PSI。** `Frozen: true` 说明进程处于冻结态，真因多半在解冻/进程状态判定；PSI 段（`some avg10=...` 之类）反映整机 CPU/IO/内存压力，高压力下的 ANR 通常是资源问题而非代码问题。

**运行时可调参数。** 与 Service/FGS 相关的阈值走 `DeviceConfig`，namespace 为 `activity_manager`：`adb shell device_config put activity_manager service_start_foreground_timeout_ms <ms>`（默认 30000，见 `ActivityManagerConstants.java:301`、`446-447`）、`service_start_foreground_anr_delay_ms`（默认 10000）、`service_bind_almost_perceptible_timeout_ms`（默认 15000）。`SERVICE_TIMEOUT`（20s）与 `SERVICE_BACKGROUND_TIMEOUT`（200s）是 Java 字段（`ActivityManagerConstants.java:586-589`），默认值定义在 310/315。广播阈值来自 `BROADCAST_FG_TIMEOUT`（10s）/ `BROADCAST_BG_TIMEOUT`（60s），可通过 `BroadcastConstants` 的 DeviceConfig 调整。输入超时基线是 `UNMULTIPLIED_DEFAULT_DISPATCHING_TIMEOUT_MILLIS`（5000），逐进程可覆盖。所有带 `Build.HW_TIMEOUT_MULTIPLIER` 的常量在慢速硬件上会自动放大。

**禁止在 ANR 路径上加长任务。** `appNotResponding` 全程可能持有 AMS 大锁，且整体受 20 秒 dump 预算约束；任何在此路径上的新增同步 I/O 都会直接放大 ANR 影响面。

---

## 十一、局限性

本工作区未检出 ART 运行时、native InputDispatcher、`IInputConstants.aidl` 与 `android_os_Debug.cpp`，因此「SIGQUIT → SignalCatcher → `ThreadList::DumpForSigQuit`」这一段与输入超时 5000ms 的具体常量值来自 AOSP 上游语义而非本地源码取证。`AnrTimer` 的内部实现（native timer、`Flags.anrTimerServiceEnabled` 开关状态）也未逐行展开。`platform_frameworks_native` 与 `platform_system_core`（含 debuggerd）中的抓栈后端同样需要另行核对。

## References

1. [AOSP frameworks/base — ProcessErrorStateRecord.java (Android 15)](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/am/ProcessErrorStateRecord.java)
2. [AOSP frameworks/base — AnrHelper.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/am/AnrHelper.java)
3. [AOSP frameworks/base — StackTracesDumpHelper.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/am/StackTracesDumpHelper.java)
4. [AOSP frameworks/base — AnrTimer.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/utils/AnrTimer.java)
5. [AOSP frameworks/base — AnrController.java (wm)](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/wm/AnrController.java)
6. [AOSP frameworks/base — ActivityManagerService.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java)
7. [AOSP frameworks/base — ActiveServices.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/am/ActiveServices.java)
8. [AOSP frameworks/base — BroadcastQueueModernImpl.java](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/services/core/java/com/android/server/am/BroadcastQueueModernImpl.java)
9. [Android Developers — ANR 诊断与诊断指南](https://developer.android.com/topic/performance/vitals/anr)
10. [Android Developers — ActivityManagerConstants / DeviceConfig](https://developer.android.com/reference/android/provider/DeviceConfig)
