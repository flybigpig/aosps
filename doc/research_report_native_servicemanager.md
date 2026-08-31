# Android Native 层 ServiceManager 深度研究报告（基于 Android 15 源码）

## 执行摘要

native ServiceManager 是 Binder IPC 的"服务注册中心"，Android 15 的实现位于 `platform_frameworks_native/cmds/servicemanager/`，是一个用 C++ 写的独立 native 守护进程，通过 AIDL 接口 `android.os.IServiceManager` 对外提供服务，客户端侧则由 `libs/binder` 中的 `IServiceManager.cpp` 与 `BackendUnifiedServiceManager.cpp` 封装成 `defaultServiceManager()` 这个入口。它的关键工程特征有三点：进程内**不创建任何 Binder 线程**（`setThreadPoolMaxThreadCount(0)`）且**禁止发起同步 Binder 调用**（`FATAL_IF_NOT_ONEWAY`），因此是全局串行处理点；`getService` 与 `checkService` 的唯一区别是前者在找不到服务时会通过 `ctl.interface_start` 异步拉起 lazy 服务，后者不会；权限完全由 SELinux 决定，通过 `service_contexts`（vendor 侧为 `vndservice_contexts`）做标签查找后调用 `selinux_check_access` 校验 `add`/`find`/`list`。另外值得注意的是，官方已明确 `vndservicemanager` 自 Android 11 起废弃，vendor 进程应改用 Stable AIDL 直接走 `/dev/binder`。

## 背景与定位

工作区是一份 Android 15 的 AOSP 源码树，包含 `frameworks_base_15`（Java 框架层）、`platform_frameworks_native`（native Binder 与 servicemanager）、`platform_system_core` 与 `kernel_common`。本报告聚焦 native 层，即 `platform_frameworks_native/cmds/servicemanager/`（守护进程）与 `platform_frameworks_native/libs/binder/`（客户端库），并把 Java 层 `android.os.ServiceManager` 作为调用链的一端串起来。需要区分三个容易混淆的概念：Java 的 `SystemServiceManager`（负责 SystemServer 内系统服务的生命周期，与服务注册无关）、native 的 servicemanager（本报告对象）、以及 HIDL 体系的 hwservicemanager（独立代码体系，走 `/dev/hwbinder`，不在本工作区内）。

## 一、三个实例与三条 Binder 域

Android 8 引入 Binder 上下文（binder context）后，系统被拆成三个互不干扰的 IPC 域，每个域有自己的设备节点与自己的上下文管理器。源码层面，`servicemanager` 与 `vndservicemanager` 由**同一套代码**编译出两个二进制，区别只是传入的驱动路径和编译宏：`vndservicemanager.rc:1` 写的是 `service vndservicemanager /vendor/bin/vndservicemanager /dev/vndbinder`，而 `servicemanager.rc:1` 不传参数，`main.cpp:153` 的 `const char* driver = argc == 2 ? argv[1] : "/dev/binder";` 决定了默认值。同一个 `main.cpp` 与 `ServiceManager.cpp` 因此被编译出三种变体：标准版、带 `-DVENDORSERVICEMANAGER` 的 vendor 版（跳过 VINTF 声明检查、不设置 `servicemanager.ready` 属性、去掉 perfetto 埋点）、以及 `__ANDROID_RECOVERY__` 的 recovery 版（`servicemanager.recovery.rc`）。

| IPC 域 | 设备节点 | 上下文管理器 | 服务对象 | 代码来源 | 当前状态 |
| --- | --- | --- | --- | --- | --- |
| framework | `/dev/binder` | `servicemanager` | 框架/应用进程，AIDL | `frameworks/native/cmds/servicemanager` | 主力，持续演进 |
| vendor | `/dev/vndbinder` | `vndservicemanager` | vendor↔vendor，AIDL | 同上源码 + `VENDORSERVICEMANAGER` 宏 | Android 11 起官方标注废弃 |
| hardware | `/dev/hwbinder` | `hwservicemanager` | 框架↔HAL、HAL↔HAL，HIDL | `system/hwservicemanager`（libhidl） | 新 HAL 不再使用 |

官方文档对此的表述很直接：Android 8 时 `/dev/binder` 一度成为框架进程专有节点，vendor 进程只能转 HIDL 走 `/dev/hwbinder`；Android 10 引入 Stable AIDL 后"允许所有进程使用 `/dev/binder`"，同时给出警告——搭载 Android 11 及以上的设备若要继续使用 `vndservicemanager`，必须显式 `PRODUCT_PACKAGES += vndservicemanager`，并建议改用 AIDL 的 NDK 与 Rust 后端，"因为这可节省 Android 中的一个进程和多个额外的线程池"。这意味着今天新写的 vendor native 服务，默认应注册到 `/dev/binder` 的 servicemanager 上，而不是 vndservicemanager。

## 二、守护进程：启动、进程模型与主线程约束

`main()`（`main.cpp:146-195`）的启动序列本身很短，但每一行都是一个强约束。`ProcessState::initWithDriver(driver)` 打开驱动后，紧接着是三处关键设置：`ps->setThreadPoolMaxThreadCount(0)` 表示**不创建任何 Binder 工作线程**，所有 Binder 事务在 `Looper` 主线程里处理；`ps->setCallRestriction(ProcessState::CallRestriction::FATAL_IF_NOT_ONEWAY)` 表示 servicemanager 进程内**禁止发起任何同步（阻塞式）Binder 调用**，因为它单线程且是系统的同步点，一旦回调某个服务而被阻塞，整个系统的服务注册与查找会全部卡死——这也是 AIDL 里 `IServiceCallback::onRegistration` 与 `IClientCallback::onClients` 必须是 oneway 的根本原因。

随后 `manager->setRequestingSid(true)` 要求驱动在每次事务中附带调用方的 SELinux SID，配合 `ProcessState::becomeContextManager()`（`ProcessState.cpp:224-247`）先用 `BINDER_SET_CONTEXT_MGR_EXT` + `FLAT_BINDER_FLAG_TXN_SECURITY_CTX` 注册、失败再回退到裸 `BINDER_SET_CONTEXT_MGR`，从而让后面每一次 `add`/`find` 都能做 SELinux 决策。servicemanager 还把自己以名字 `"manager"` 注册进自己的表（`main.cpp:169`），所以客户端 `getService("manager")` 拿到的就是 servicemanager 自身。

主循环由 `Looper::prepare(false)` 加两个回调组成。`BinderCallback::setupTo()`（`main.cpp:60-89`）通过 `IPCThreadState::setupPolling()` 取出驱动 fd 挂到 Looper，`handleEvent` 直接调 `handlePolledCommands()`；`ClientCallbackCallback::setupTo()`（`main.cpp:91-144`）创建一个 `timerfd`，周期 5 秒到期后调 `mManager->handleClientCallbacks()` 并 `repoll`（对应 bug b/316829336）。最后非 vendor 变体设置系统属性 `servicemanager.ready=true`，客户端正是靠这个属性判断 servicemanager 是否就绪。

`servicemanager.rc` 的条目解释了它在 init 中的地位：`class core animation` 表示它属于最早启动的 core 组并参与开机动画阶段，`critical` 表示短时间内反复崩溃会把设备打进 recovery，`onrestart` 列表里除了 `setprop servicemanager.ready false`，还包括重启 `apexd`、`audioserver`、`gatekeeperd` 以及 `class_restart --only-enabled main / hal / early_hal`——也就是说 servicemanager 一旦重启，几乎整个用户态跟着重启。`task_profiles ProcessCapacityHigh` 与 `shutdown critical` 分别对应调度能力与关机时序。对比之下 `vndservicemanager.rc` 没有 `animation` class，且 task profile 是 `ServiceCapacityLow`。

## 三、服务接口与内部数据结构

接口定义在 `libs/binder/aidl/android/os/IServiceManager.aidl`。dump 优先级常量从 `DUMP_FLAG_PRIORITY_CRITICAL`（1<<0）到 `DUMP_FLAG_PRIORITY_DEFAULT`（1<<3），另有 `DUMP_FLAG_PRIORITY_ALL` 与 `FLAG_IS_LAZY_SERVICE`（1<<30，`IServiceManager.aidl:52`），后者不是一个 dump 标志，而是复用 dumpPriority 字段传递"这是 lazy 服务"的元数据。方法上，查询侧有三个版本：`getService`（`:70`，已标注 `@deprecated`，注释要求改用 `getService2`，因为它不返回元数据）、`getService2`（`:82`，返回 `Service` union）与 `checkService`（`:90`）；写入侧是 `addService(name, service, allowIsolated, dumpPriority)`（`:96`）；此外还有 `listServices`、`registerForNotifications`/`unregisterForNotifications`、`isDeclared`、`getDeclaredInstances`、`updatableViaApex`、`getUpdatableNames`、`getConnectionInfo`、`registerClientCallback`、`tryUnregisterService`、`getServiceDebugInfo`。

守护进程内部维护三张表（`ServiceManager.h:123-125`）：`mNameToService`（名字到服务）、`mNameToRegistrationCallback`（谁在等这个服务注册）、`mNameToClientCallback`（谁在关心这个服务有没有客户端）。`struct Service`（`ServiceManager.h:83-95`）保存 binder 引用、`allowIsolated`、`dumpPriority`、`hasClients`、`guaranteeClient`，以及注册者的 `CallingContext`（uid、sid、debugPid）。

## 四、查询与注册的两条主链路

三个查询 API 的实现一共只有二十多行，语义差异极其集中（`ServiceManager.cpp:395-420`）：

```cpp
395:Status ServiceManager::getService(const std::string& name, sp<IBinder>* outBinder) {
399:    *outBinder = tryGetBinder(name, true).service;
401:    return Status::ok();   // returns ok regardless of result for legacy reasons
417:    *outService = tryGetService(name, false);   // checkService
```

即 `getService`/`getService2` 传 `startIfNotFound=true`，`checkService` 传 `false`；三者无论找没找到都返回 `Status::ok()`，调用方必须自己判断返回对象是否为空。`tryGetService`（`:422-438`）先查 VINTF accessor（非 vendor 变体），若该服务声明了 accessor 则改为去取 accessor 的 binder；`tryGetBinder`（`:440-483`）依次做：隔离进程检查（`:452`，服务未声明 `allowIsolated` 时拒绝 isolated uid）、`canFind` 权限检查（`:460`）、找不到且允许启动则 `tryStartService`（`:464-466`）、命中后把 `guaranteeClient` 置 true 并立刻 `handleServiceClientCallback(2, name, false)`（`:474-476`，参数 2 表示"servicemanager 一份引用 + 当前事务一份引用"），最后把 `isLazyService` 元数据回填。

`tryStartService`（`:872-885`）是理解 lazy 服务的关键：它 `std::thread(...).detach()` 起一个线程执行 `base::SetProperty("ctl.interface_start", "aidl/" + name)`，即异步通知 init 拉起服务，不阻塞当前事务，也**不保证服务一定起来**（属性设置失败只打一条日志）。这与官方"动态可用的 HAL"文档描述一致：init 自 Android 9 起增加了 `ctl.interface_start` / `ctl.interface_stop` / `ctl.interface_restart` 三个控制消息，专门供服务管理器请求启动未运行的服务。

`addService`（`:503-597`）的校验顺序是排查注册失败的清单：先拒绝 App UID（`multiuser_get_app_id(ctx.uid) >= AID_APP`，`:509`），再走 SELinux `add` 检查（`:514`），然后拒绝空 binder、校验服务名（`isValidServiceName`，`:485-501`，要求长度 1–127 且只含 `[a-zA-Z0-9_./-]`），非 vendor 变体还要过 VINTF 声明检查 `meetsDeclarationRequirements`（`:527-532`），`dumpPriority` 未设置时打 warning（`:534`，这是 `service list` 里服务"消失"的常见原因），最后 `linkToDeath`（`:540`，仅对远端 binder）并覆盖写入表项。若该名字上已有人在等，则置 `guaranteeClient` 后逐个回调 `cb->onRegistration(name, binder)`（`:583-594`）。

服务进程死亡时，`binderDied`（`:844-870`）遍历三张表把该 binder 相关条目全部清除；源码注释坦承了一个设计债：`mNameToService` 条目同时承载了 client callback 的状态，如果未来允许别的进程代为注册 client callback，就得把 `hasClients` 迁到 `mNameToClientCallback` 里去。

## 五、SELinux 权限模型

权限全部落在 `Access.cpp`。`getCallingContext()`（`:111-126`）从 `IPCThreadState` 取 `getCallingSid()`、`getCallingPid()`、`getCallingUid()`，SID 缺失时回退到 `getPidcon(callingPid)`。`canFind` 与 `canAdd` 分别查 `find` 与 `add` 权限（`:128-134`），`canList` 则针对 `service_manager` 这个 type 查 `list`（`:136-138`）。真正做决策的是 `actionAllowed()`（`:140-160`）里的 `selinux_check_access(sctx.sid, tctx, "service_manager", perm, audit_data)`，其中目标上下文 `tctx` 由 `actionAllowedFromLookup()`（`:162-172`）用 `selabel_lookup(..., SELABEL_CTX_ANDROID_SERVICE)` 从 `service_contexts` 查得；**如果服务名在 `service_contexts` 里没有匹配项，直接 deny** 并打印 `SELinux: No match for <name> in service_contexts`——这是新增系统服务时最常见的拦路虎。vendor 侧对应文件是 `vndservice_contexts`，type 需要声明为 `vndservice_manager_type`，规则形如 `allow <domain> <service_type>:service_manager { add find };`，访问设备节点用 `vndbinder_use()` 宏。

## 六、客户端计数与 lazy 服务生命周期

servicemanager 能回答"这个服务还有没有人用"，靠的是 Binder 驱动提供的引用计数。`Service::getNodeStrongRefCount()`（`:967-972`）取 `ProcessState::getStrongRefCountForNode(bpBinder)`，驱动不支持时返回 -1，此时按"有客户端"处理（`:992`）。`handleServiceClientCallback()`（`:980-1023`）的策略明显是防抖设计：一旦发现有客户端就**立刻**上报 `onClients(true)`（`:1008-1010`），而发现没客户端时，只有在 5 秒周期调用（`isCalledOnInterval=true`）时才上报 `onClients(false)`（`:1013-1018`），源码注释明确写着"我们有意延迟客户端消失的消息以减少抖动"。`guaranteeClient` 是一次性的强制标记（`:996-1003`），用于保证刚被 getService 拿到的服务不会因为定时器误判而被立刻停掉。`registerClientCallback`（`:887-946`）只允许服务进程给自己注册（比较 `debugPid` 与调用 pid，`:909-913`）、且传入的 binder 必须与表内一致（`:916-921`），`LazyServiceRegistrar`（`libs/binder/LazyServiceRegistrar.cpp`）正是靠它在无客户端时主动退出进程，实现按需启停。

## 七、客户端侧：从 defaultServiceManager 到 handle 0

客户端入口短得出人意料（`libs/binder/IServiceManager.cpp:307-314`）：

```cpp
307:sp<IServiceManager> defaultServiceManager()
308:{
309:    std::call_once(gSmOnce, []() {
310:        gDefaultServiceManager = sp<CppBackendShim>::make(getBackendUnifiedServiceManager());
311:    });
312:    return gDefaultServiceManager;
313:}
```

真正的等待逻辑在 `getBackendUnifiedServiceManager()`（`BackendUnifiedServiceManager.cpp:374-401`）：bionic 且非 VNDK 的进程先以 1 秒为间隔无限等待属性 `servicemanager.ready == "true"`（VNDK 进程读不到系统属性，被编译期排除），然后无限重试 `interface_cast<AidlServiceManager>(ProcessState::self()->getContextObject(nullptr))`，失败就 `sleep(1)`。也就是说，**servicemanager 未启动时 `defaultServiceManager()` 会永久阻塞调用线程**，而不是返回空。`getContextObject` 内部取的就是 handle 0 的代理对象，并调 `internal::Stability::markCompilationUnit()` 打上编译单元稳定性标记（`ProcessState.cpp:173-186`）——handle 0 之所以"就是"servicemanager，是驱动层把 context manager 固定映射为 handle 0 的约定。

`CppBackendShim` 是把 AIDL 世界适配回传统 `android::IServiceManager` C++ 接口的兼容层，注释（`IServiceManager.cpp:557-560`）承认它的实现"本可以委托给 waitForService，但那会改变某些场景的线程结构并可能破坏 prebuilts"，于是 `getService`（`:561-604`）自己做轮询：先 `checkService` 一次，未命中则在 5 秒超时内循环重试，重试间隔在 `sys.boot_completed=1` 之后是 1000ms、开机早期与 vendor 进程固定 100ms；超时后打印 `Service %s didn't start. Returning NULL`。`waitForService`（`:635-724`）则更优雅：注册一个 `BnServiceCallback` 等待者，用条件变量每次等 1 秒，每轮醒来再发一次 `realGetService`——源码注释（`:708-716`）解释了原因：lazy 服务死亡后可能出现"init 收到启动信号但认为服务已在运行、随后才收到死亡信号"的竞态，必须重新请求一次才能真正拉起它。NDK 与 Rust 端的 `AServiceManager_*` / `binder` crate 最终都通过 `IServiceManagerFFI.cpp` 落到同一套实现，`libs/fakeservicemanager/` 与 `ServiceManagerHost.cpp` 则提供 host（非 Android）与单元测试环境下的替身。

## 八、Java 层如何落到这里

`frameworks_base_15/framework15/core/java/android/os/ServiceManager.java` 并不是另一套实现，而是 native 链路的封装与缓存层。`getIServiceManager()`（`:149-158`）用 `ServiceManagerNative.asInterface(Binder.allowBlocking(BinderInternal.getContextObject()))` 拿到 handle 0 的代理，其中 `Binder.allowBlocking` 的注释点明系统服务获取必须允许同步调用。该类维护 `sCache`（由 SystemServer 通过 `initServiceCache()` 预灌，`:56`、`:404-408`）、慢调用统计与阈值（`GET_SERVICE_SLOW_THRESHOLD_US_CORE`/`NON_CORE`、`SLOW_LOG_INTERVAL_MS=5000`）。`addService` 默认使用 `DUMP_FLAG_PRIORITY_DEFAULT`，而 `listServices` 传的是 `DUMP_FLAG_PRIORITY_ALL`（所以 Java 侧枚举不受 dump 过滤影响）。`waitForService` 与 `waitForDeclaredService` 提供懒启动等待，`ServiceNotFoundException` 供 `getServiceOrThrow` 使用。JNI 层只有 `android_os_ServiceManager.cpp:32-50` 的 `waitForServiceNative`，直接转调 `defaultServiceManager()->waitForService()`，其余方法一律走标准 AIDL 代理。

## 九、排查与实践要点

实践中绝大多数"服务拿不到"的问题都能归到几条固定模式。进程卡在启动阶段没有任何 crash，往往是 `defaultServiceManager()` 阻塞在等 `servicemanager.ready`，用 `getprop servicemanager.ready` 与 `ps -A | grep servicemanager` 一眼可判。`Waiting for service 'xxx'` 反复打印后服务返回 NULL，说明 5 秒超时被触发，此时要看该服务是否真的注册（VINTF 声明、`rc` 文件、`ctl.interface_start` 是否生效），以及调用进程是否启动了 Binder 线程池——`waitForService` 在检测到 `getThreadPoolMaxTotalThreadCount() == 0` 时会专门打印警告（`:669-675`、`:703-706`），因为没有线程接收回调就会退化成轮询。注册侧失败则以 SELinux 为主：先确认 `service_contexts`（vendor 为 `vndservice_contexts`）里有对应标签，再确认调用域有 `add` 权限，日志里会明确出现 `No match for <name> in service_contexts`；VINTF 未声明会得到 `VINTF declaration error`，App 进程注册会得到 `App UIDs cannot add services`。此外，`service list` 过滤 dumpPriority，若注册时未设置优先级（会打一条 `Dump flag priority is not set` 警告），服务可能在某些 dump 场景中不可见；`dumpsys` 的顺序与优先级也由此决定。

## 十、版本演进

| 阶段 | 关键变化 | 影响 |
| --- | --- | --- |
| Android 7 及更早 | C 实现（`service_manager.c` + `binder.c`，`svcmgr_handler`/`do_add_service`），自行循环，不用 libbinder 多线程模型 | 社区流传最广的 Gityuan《Binder系列3—启动ServiceManager》（2015，Android 6.0）描述的就是这一版 |
| Android 8–9 | Treble：三 Binder 域；servicemanager 改为 C++ 并迁至 `frameworks/native/cmds/servicemanager`；新增 hwservicemanager 与 vndservicemanager；init 增加 `ctl.interface_*` | 形成今天的多实例格局 |
| Android 10 | servicemanager AIDL 化，接口改为 `android.os.IServiceManager`；Stable AIDL 允许所有进程使用 `/dev/binder` | 旧的手写 Binder 协议调用被 AIDL 取代，C++ 侧保留 shim 兼容 |
| Android 11 起 | 官方标注 `vndservicemanager` 废弃，需显式 `PRODUCT_PACKAGES` 启用；lazy 服务机制进入 binder 域 | 新 vendor 服务应直接注册到 `/dev/binder` |
| Android 12–13 | client callback、`LazyServiceRegistrar`、Accessor 机制、`dumpPriority`/`FLAG_IS_LAZY_SERVICE` | 服务可以按需启停，HAL 不再常驻 |
| Android 14–15 | `getService2`/`checkService` 返回 `os::Service` 元数据；`isDeclared`/`getDeclaredInstances`/`updatableViaApex`/`getConnectionInfo`/`getServiceDebugInfo`；`BackendUnifiedServiceManager` 统一 AIDL/RPC/Host 后端；perfetto `servicemanager` trace category | 支持 RPC accessor、APEX 可更新服务与跨后端访问 |

需要说明的一点是，上表中版本归属依据的是工作区 Android 15 源码中的 API 形态、注释与官方站点文档，个别特性（如 client callback 的引入时间点）在社区文章里存在不同说法，应以你实际分支的源码为准。

## 结论

Android 的 native ServiceManager 是一个刻意做得"小而不可阻塞"的服务注册中心：单线程、不允许同步出向调用、无锁依赖（事务在主线程串行处理），对外只暴露一套 AIDL 接口，把权限交给 SELinux、把服务启停交给 init 与 lazy 机制、把客户端存活判断交给 Binder 驱动的引用计数。理解它只需抓住三件事：handle 0 的约定让 `defaultServiceManager()` 成为一切 Binder 通信的起点；`getService` 与 `checkService` 的差别仅在于是否触发 `ctl.interface_start`；`addService`/`findService` 的成败几乎完全由 `service_contexts` 标签与 `add`/`find` 规则决定。对今天的开发者而言，最实际的结论是：新写的 native 服务应直接用 Stable AIDL 注册到 `/dev/binder` 上的 servicemanager，不要再用 `vndservicemanager`；需要按需启停的服务用 `LazyServiceRegistrar` 配合 `registerClientCallback`，而不是自己实现心跳。

## 局限性

本报告的一手依据是工作区内的 Android 15 源码，但工作区并非完整 AOSP：`system/sepolicy` 与 `system/hwservicemanager` 不在树中，SELinux 策略与 HIDL 侧的结论来自官方站点文档与镜像，未能与本地源码逐条对照。`cs.android.com` 与 `source.android.com` 在本次环境下直连超时，线上最新分支与本地分支的差异未做逐行比对。微信公众号检索虽按时间参数执行，但搜狗对冷门中文关键词的时间筛选基本失效、原文抓取又被验证码与链接解析拦截，因此中文资料只拿到标题层级（例如「Android系统攻城狮」的《Android14 Binder专题第一弹：ServiceManager服务开篇》2024-03、「千里马学框架」的《Android12 ServiceManager启动流程》2024-03），未能用于正文交叉验证——好在社区中关于 servicemanager 的中文文章大量仍停留在 Android 6–10 的旧实现（如 Gityuan 2015 年那篇描述的 `binder.c` 单循环模型），与 Android 11+ 的实际代码已有本质差异，不建议作为当前版本的判断依据。

## 参考来源

1. [使用 binder IPC | Android Open Source Project](https://source.android.google.cn/docs/core/architecture/hidl/binder-ipc?hl=zh-cn)
2. [动态可用的 HAL | Android Open Source Project](https://source.android.google.cn/docs/core/architecture/hal/dynamic-lifecycle?hl=zh-cn)
3. [Android 中的安全增强型 Linux](https://source.android.google.cn/docs/security/features/selinux?hl=zh-cn)
4. [适用于 HAL 的 AIDL](https://source.android.google.cn/docs/core/architecture/aidl/aidl-hals?hl=zh-cn)
5. [一文总结 Android 系统服务大管家 ServiceManager（知乎，2024-04）](https://zhuanlan.zhihu.com/p/691374002)
6. [Android ServiceManager 和它的兄弟们（掘金，2024-06）](https://juejin.cn/post/7378553024187007013)
7. [Android ServiceManager 进阶（CSDN）](https://blog.csdn.net/lizhenjun114/article/details/129524558)
8. [Binder 系列 3—启动 ServiceManager（Gityuan，2015，基于 Android 6.0，仅作历史对照）](http://gityuan.com/2015/11/07/binder-start-sm/)
9. 本地源码：`platform_frameworks_native/cmds/servicemanager/`（`main.cpp`、`ServiceManager.cpp`、`Access.cpp`、`servicemanager.rc`、`vndservicemanager.rc`）
10. 本地源码：`platform_frameworks_native/libs/binder/`（`IServiceManager.cpp`、`BackendUnifiedServiceManager.cpp`、`ProcessState.cpp`、`aidl/android/os/IServiceManager.aidl`）
11. 本地源码：`frameworks_base_15/framework15/core/java/android/os/ServiceManager.java`、`ServiceManagerNative.java`、`core/jni/android_os_ServiceManager.cpp`
