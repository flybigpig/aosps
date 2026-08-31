# 研究计划：Android Native 层 ServiceManager（Binder 服务管家）

## 用户意图
用户输入为 "native ServiceManager"，工作区为 Android 15（frameworks_base_15 + platform_frameworks_native + platform_system_core）AOSP 源码树。因此本研究的主题锁定为：**Android 原生的 native 层 ServiceManager（C++/libbinder 侧的 servicemanager 守护进程与 IServiceManager 客户端 API）**，而非 Java 层 SystemServiceManager，也不是字面上的 "Native 服务"。

## 查询类型判定
属于 **Depth-first（深度优先）查询**。核心问题单一（native ServiceManager 是什么、怎么工作、怎么用），但需要从多个方法/视角切入：守护进程实现、客户端 API 与调用链、多 binder 域与权限/调试、工程实践与版本演进。适合并行子代理从不同方法论切入，最后交叉综合。

## 研究目标
产出一份能直接指导源码阅读与开发实践的深度报告，覆盖：
1. servicemanager 守护进程的定位、启动方式（init rc）、进程模型、AIDL 接口与内部数据结构。
2. 服务注册（addService）与获取（getService/checkService）的完整链路，含 lazy start、death notification、client callback、Accessor 服务代理等机制。
3. libbinder 客户端侧：ProcessState/IServiceManager.cpp 的 Shim 与 Unified Backend、defaultServiceManager()、waitForService、IServiceManager.aidl 及其 ABI/稳定性。
4. 与 Java 层 ServiceManager（frameworks_base）的 JNI 贯通关系；与 hwservicemanager（HIDL）与 vndservicemanager 的分区与区别；binder 设备节点 /dev/binder、/dev/vndbinder、/dev/hwbinder。
5. 权限与 SELinux、调试手段（service list/dumpsys/lshal/atrace）、常见坑与最佳实践。
6. Android 15 相对早期版本的关键演进，以及 Android 16/未来方向（如 libbinder 重构、服务生命周期管理）。

## 子代理任务划分（4 个并行）

### S1 — servicemanager 守护进程源码解剖（重点本地源码）
目标：把 `platform_frameworks_native/cmds/servicemanager/` 完整讲透。
- 必读：`main.cpp`、`ServiceManager.cpp/.h`、`Access.cpp/.h`、`NameUtil.h`、`Android.bp`、`servicemanager.rc`、`vndservicemanager.rc`、`servicemanager.recovery.rc`、`ServiceManagerUnittest.cpp`、`test_sm.cpp`、`ServiceManagerFuzzer.cpp`、`corpus/`。
- 要回答：进程入口与 main 的循环（looper/驱动交互）、AIDL 接口 `android.os.IServiceManager` 各方法语义与版本差异（getService vs getService2 vs checkService）、mNameToService/mNameToRegistrationCallback/mNameToClientCallback 三张表、binderDied 与死亡通知、tryStartService 与 lazy services、guaranteeClient/hasClients 的客户端计数逻辑（getNodeStrongRefCount）、Access 权限校验与 CallingContext、dumpPriority、debug pid 与 allowIsolated、VENDORSERVICEMANAGER / RECOVERY 编译宏差异、perfetto tracing 埋点。
- 输出：带源码行号引用的事实清单 + 关键流程伪代码/时序描述。

### S2 — libbinder 客户端侧与调用链（重点本地源码 + 少量 web 交叉验证）
目标：客户端如何拿到并使用 ServiceManager。
- 必读：`libs/binder/IServiceManager.cpp`、`libs/binder/include/binder/IServiceManager.h`、`libs/binder/aidl/android/os/IServiceManager.aidl`、`libs/binder/BackendUnifiedServiceManager.cpp/.h`、`libs/binder/IServiceManagerFFI.cpp/.h`、`libs/binder/Static.cpp`、`libs/binder/ProcessState.cpp`（defaultServiceManager / context object handle 0）、`libs/binder/LazyServiceRegistrar.cpp`、`libs/binder/ServiceManagerHost.cpp`、`libs/fakeservicemanager/`（含 rust wrapper）、`libs/binder/rust/`、`libs/binder/ndk/`。
- 要回答：defaultServiceManager() 的单例与 handle 0 语义、AIDL/RPC/Unified 多 backend 后端（BackendUnifiedServiceManager 的作用与 libbinder_rpc_unstable）、IServiceManager asInterface、waitForService/waitForDeclaredService、IServiceManager::getService 模板与 Stability、lazy service 注册器、Rust/NDK 绑定（如何跨语言）、FakeServiceManager 在测试与 host 上的用途、VNDK 与 libbinder 稳定性约束。
- 输出：调用链（应用 → ProcessState → handle 0 → servicemanager）逐步说明 + 代码片段。

### S3 — 系统架构、权限、调试与版本演进（本地源码 + web）
目标：把 native ServiceManager 放进整机视角。
- 本地：`frameworks_base_15/framework15/core/jni/android_os_ServiceManager.cpp`、`frameworks_base_15/framework15/core/java/android/os/ServiceManager.java`、`ServiceManagerNative.java`；对比 hwservicemanager/vndservicemanager 与 binder 域；SELinux（`servicemanager` 域、`service_contexts`、`add_service` 权限、`vndservice_contexts`）；init 启动顺序与 `servicemanager.ready` 属性；lazy services 与 `init` 的联动。
- Web：Android 官方 source.android.com 文档（servicemanager、binder 域、Treble/VNDK、stability）、Android 15/16 变化、known issues/常见错误（如 "Waiting for service"、deadlock、Service not registered）。
- 要回答：三条 binder 域的划分与选谁、Java↔native 如何打通、SELinux 规则要点、调试与排障命令、版本演进差异表。
- 输出：对比表格 + 排障手册式清单。

### S4 — 中文技术资料检索（wechat-article-search + web）
目标：补充中文社区的高质量深度解析与实践经验，交叉验证本地源码结论。
- 使用 `use_skill` 加载 `wechat-article-search` 技能（务必带时间参数），检索关键词：
  - "Android native servicemanager 源码分析"（时间范围 2021-01-01 至 2026-08-31）
  - "servicemanager 启动流程 addService getService"（2021-01-01 至 2026-08-31）
  - "binder servicemanager 死亡通知 权限"（2021-01-01 至 2026-08-31）
  - "Android 15 servicemanager 变化 / lazy service"（2024-01-01 至 2026-08-31）
- 结合 web_search 检索 source.android.com、cs.android.com、Android 官方博客、知名博客（Gityuan、老罗等）。
- 输出：中文资料观点摘要 + 与源码结论的一致性/冲突点标注 + 带 URL 的来源清单。

## 综合方法
1. 以本地 Android 15 源码为第一手权威（源码 > 官方文档 > 博客）。
2. S1/S2/S3 提供机制事实，S4 提供中文表述与实践经验，作为交叉验证。
3. 冲突时以本地源码与官方文档为准，并在报告中标注分歧。
4. 报告结构：摘要 → 背景 → 守护进程解剖 → 客户端与调用链 → 多域与 Java/HIDL 关系 → 权限与调试 → 版本演进 → 结论 → 局限 → 参考。
5. 目标篇幅：2500-3500 字（深度优先型），含代码引用与表格，禁用 emoji。

## 终止条件
当四路结果回收且能完整回答上述 6 个目标时停止，不再追加子代理。
