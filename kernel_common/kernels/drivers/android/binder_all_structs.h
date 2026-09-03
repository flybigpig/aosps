/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * binder_all_structs.h — Binder 数据结构汇总（参考/阅读用）
 *
 * ============================= 重要警告 =============================
 * 本文件是从内核源码中"搬运汇总"的**参考文档**，不是驱动的一部分。
 *
 *   !!! 千万不要 #include 本文件到任何内核构建中 !!!
 *
 * 所有结构体在以下原始头文件中已有定义，同时 include 会导致
 * "redefinition of struct ..." 编译错误：
 *   - drivers/android/binder_internal.h
 *   - drivers/android/binder_alloc.h
 *   - drivers/android/dbitmap.h
 *   - include/uapi/linux/android/binder.h
 *
 * 本文件的用途：把分散在 4 个头文件里的 Binder 数据结构集中到一处，
 * 便于通读、对照和理解驱动的对象模型与锁/生命周期关系。
 * ==================================================================
 *
 * 来源（Android 通用内核 ）：
 *   kernel_common/kernels/drivers/android/binder_internal.h
 *   kernel_common/kernels/drivers/android/binder_alloc.h
 *   kernel_common/kernels/drivers/android/dbitmap.h
 *   kernel_common/kernels/include/uapi/linux/android/binder.h
 *
 * 内容分区：
 *   Part  1  UAPI 基础类型与对象类型
 *   Part  2  UAPI 用户态数据结构
 *   Part  3  ioctl 命令
 *   Part  4  事务数据与协议（BR_ / BC_）
 *   Part  5  dbitmap（动态位图，句柄分配）
 *   Part  6  binder 设备与上下文（binderfs）
 *   Part  7  统计与工作项
 *   Part  8  核心 IPC 对象：node / ref
 *   Part  9  优先级
 *   Part 10  内存分配器：alloc / buffer
 *   Part 11  进程：binder_proc
 *   Part 12  线程：binder_thread
 *   Part 13  事务：binder_transaction
 *   Part 14  对象联合体与其他
 *
 * 为便于查阅，结构体按"依赖顺序"排列，并保留原文件的 kernel-doc 注释。
 * 函数声明、静态内联辅助函数（除少量必要者）被省略，只保留数据结构本身。
 */

#ifndef _BINDER_ALL_STRUCTS_H_REFERENCE_ONLY
#define _BINDER_ALL_STRUCTS_H_REFERENCE_ONLY

/*
 * 误 include 防护。
 *
 * 本文件与 binder_internal.h / binder_alloc.h / dbitmap.h 同目录，
 * 一旦被 #include 进 binder.c 等内核源文件，下面所有结构体都会与
 * 原始头文件重复定义，产生大量晦涩的 "redefinition of struct ..." 错误。
 *
 * 因此在真正展开内容之前先检查原始头文件的 include guard：
 * 只要其中任何一个已被定义，就说明本文件正被卷入真实内核构建链，
 * 此时立即以明确的错误信息终止编译，而不是让使用者去猜。
 */
#if defined(_UAPI_LINUX_BINDER_H)
#error "binder_all_structs.h 是参考汇总文件，不可用于内核构建：" \
       "<uapi/linux/android/binder.h> 已被包含，结构体将重复定义。" \
       "请删除对本文件的 #include。"
#endif

#if defined(_LINUX_BINDER_INTERNAL_H)
#error "binder_all_structs.h 是参考汇总文件，不可用于内核构建：" \
       "binder_internal.h 已被包含，结构体将重复定义。" \
       "请删除对本文件的 #include。"
#endif

#if defined(_LINUX_BINDER_ALLOC_H)
#error "binder_all_structs.h 是参考汇总文件，不可用于内核构建：" \
       "binder_alloc.h 已被包含，结构体将重复定义。" \
       "请删除对本文件的 #include。"
#endif

#if defined(_LINUX_DBITMAP_H)
#error "binder_all_structs.h 是参考汇总文件，不可用于内核构建：" \
       "dbitmap.h 已被包含，结构体将重复定义。" \
       "请删除对本文件的 #include。"
#endif

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/uidgid.h>
#include <linux/wait.h>
#include <linux/bitmap.h>
#include <linux/ipc_namespace.h>	/* struct ipc_namespace（binderfs_info.ipc_ns） */
#include <uapi/linux/android/binderfs.h>

/* ==================================================================
 * Part 1 — UAPI 基础类型与对象类型
 * 来源：uapi/linux/android/binder.h
 * ================================================================== */

#define B_PACK_CHARS(c1, c2, c3, c4) \
	((((c1)<<24)) | (((c2)<<16)) | (((c3)<<8)) | (c4))
#define B_TYPE_LARGE 0x85

/* 传输对象的类型，编码在 flat_binder_object.hdr.type 中 */
enum {
	BINDER_TYPE_BINDER	= B_PACK_CHARS('s', 'b', '*', B_TYPE_LARGE),
	BINDER_TYPE_WEAK_BINDER	= B_PACK_CHARS('w', 'b', '*', B_TYPE_LARGE),
	BINDER_TYPE_HANDLE	= B_PACK_CHARS('s', 'h', '*', B_TYPE_LARGE),
	BINDER_TYPE_WEAK_HANDLE	= B_PACK_CHARS('w', 'h', '*', B_TYPE_LARGE),
	BINDER_TYPE_FD		= B_PACK_CHARS('f', 'd', '*', B_TYPE_LARGE),
	BINDER_TYPE_FDA		= B_PACK_CHARS('f', 'd', 'a', B_TYPE_LARGE),
	BINDER_TYPE_PTR		= B_PACK_CHARS('p', 't', '*', B_TYPE_LARGE),
};

/**
 * enum flat_binder_object_shifts: shift values for flat_binder_object_flags
 * @FLAT_BINDER_FLAG_SCHED_POLICY_SHIFT: shift for getting scheduler policy.
 */
enum flat_binder_object_shifts {
	FLAT_BINDER_FLAG_SCHED_POLICY_SHIFT = 9,
};

/**
 * enum flat_binder_object_flags - flags for use in flat_binder_object.flags
 */
enum flat_binder_object_flags {
	/**
	 * @FLAT_BINDER_FLAG_PRIORITY_MASK: bit-mask for min scheduler priority
	 *
	 * 设置进入该 node 的事务运行所需的最低调度优先级。
	 * SCHED_NORMAL/SCHED_BATCH 有效范围 [-20..19]；
	 * SCHED_FIFO/SCHED_RR 有效范围 [1..99]。
	 */
	FLAT_BINDER_FLAG_PRIORITY_MASK = 0xff,

	/** @FLAT_BINDER_FLAG_ACCEPTS_FDS: 该 node 是否接受 fd */
	FLAT_BINDER_FLAG_ACCEPTS_FDS = 0x100,

	/**
	 * @FLAT_BINDER_FLAG_SCHED_POLICY_MASK: 调度策略位掩码
	 * 00b: SCHED_NORMAL, 01b: SCHED_FIFO, 10b: SCHED_RR, 11b: SCHED_BATCH
	 */
	FLAT_BINDER_FLAG_SCHED_POLICY_MASK =
		3U << FLAT_BINDER_FLAG_SCHED_POLICY_SHIFT,

	/**
	 * @FLAT_BINDER_FLAG_INHERIT_RT: 该 node 是否继承 RT 策略
	 * 仅当置位时，对该 node 的同步调用会从调用方继承实时调度策略。
	 */
	FLAT_BINDER_FLAG_INHERIT_RT = 0x800,

	/**
	 * @FLAT_BINDER_FLAG_TXN_SECURITY_CTX: 请求安全上下文
	 * 置位时发送方需附带自己的 security context。
	 */
	FLAT_BINDER_FLAG_TXN_SECURITY_CTX = 0x1000,
};

/*
 * 在 64 位平台上，用户态代码可能以 32 位运行，
 * 驱动必须据此转换 buffer 和本地 binder 地址。
 */
#ifdef BINDER_IPC_32BIT
typedef __u32 binder_size_t;
typedef __u32 binder_uintptr_t;
#else
typedef __u64 binder_size_t;
typedef __u64 binder_uintptr_t;
#endif

#define BINDER_CURRENT_PROTOCOL_VERSION 8

/* ==================================================================
 * Part 2 — UAPI 用户态数据结构
 * 来源：uapi/linux/android/binder.h
 * ================================================================== */

/**
 * struct binder_object_header - 所有 binder 元数据对象共享的头部
 * @type:	对象类型
 */
struct binder_object_header {
	__u32        type;
};

/*
 * 这是 Binder 对象在进程间传递时的"扁平化"表示。
 * 作为事务一部分提供的 'offsets' 包含这些结构在数据中的偏移量。
 * Binder 驱动负责在对象跨进程移动时重写结构类型和数据。
 */
struct flat_binder_object {
	struct binder_object_header	hdr;
	__u32				flags;

	/* 8 bytes of data. */
	union {
		binder_uintptr_t	binder;	/* 本地对象 */
		__u32			handle;	/* 远程对象 */
	};

	/* 与本地对象关联的额外数据 */
	binder_uintptr_t	cookie;
};

/**
 * struct binder_fd_object - 描述需要被 fixup 的文件描述符
 * @hdr:		通用头部
 * @pad_flags:		填充，保持与旧用户态代码兼容
 * @pad_binder:		填充，保持与旧用户态代码兼容
 * @fd:			文件描述符
 * @cookie:		用户态使用的不透明数据
 */
struct binder_fd_object {
	struct binder_object_header	hdr;
	__u32				pad_flags;
	union {
		binder_uintptr_t	pad_binder;
		__u32			fd;
	};

	binder_uintptr_t		cookie;
};

/**
 * struct binder_buffer_object - 描述一个用户空间 buffer 的对象
 * @hdr:		通用头部
 * @flags:		一个或多个 BINDER_BUFFER_* 标志
 * @buffer:		buffer 地址
 * @length:		buffer 长度
 * @parent:		指向父 buffer 的偏移数组索引
 * @parent_offset:	在 @parent 中指向本 buffer 的偏移
 *
 * binder_buffer 对象表示驱动可以原样拷贝到目标地址空间的一块内存。
 * 一个 buffer 可能被另一个 buffer 内的指针引用，此时该指针也需要修正：
 * 设置 BINDER_BUFFER_FLAG_HAS_PARENT 标志、把 @parent 设为父
 * binder_buffer_object 在偏移数组中的索引、把 @parent_offset 设为
 * 父 buffer 中存放本 buffer 指针的偏移即可。
 */
struct binder_buffer_object {
	struct binder_object_header	hdr;
	__u32				flags;
	binder_uintptr_t		buffer;
	binder_size_t			length;
	binder_size_t			parent;
	binder_size_t			parent_offset;
};

enum {
	BINDER_BUFFER_FLAG_HAS_PARENT = 0x01,
};

/**
 * struct binder_fd_array_object - 描述 buffer 中的 fd 数组
 * @hdr:		通用头部
 * @pad:		填充以保证正确对齐
 * @num_fds:		buffer 中 fd 的数量
 * @parent:		持有 fd 数组的 buffer 在偏移数组中的索引
 * @parent_offset:	fd 数组在 buffer 中的起始偏移
 *
 * 典型用法是 Android 的 native_handle_t：结构体本身用
 * binder_buffer_object 表示，内嵌的 fd 列表用 binder_fd_array_object
 * 表示，并把前者作为 parent。
 */
struct binder_fd_array_object {
	struct binder_object_header	hdr;
	__u32				pad;
	binder_size_t			num_fds;
	binder_size_t			parent;
	binder_size_t			parent_offset;
};

/* BINDER_WRITE_READ 命令的参数 */
struct binder_write_read {
	binder_size_t		write_size;	/* 待写入字节数 */
	binder_size_t		write_consumed;	/* 驱动已消费字节数 */
	binder_uintptr_t	write_buffer;
	binder_size_t		read_size;	/* 待读取字节数 */
	binder_size_t		read_consumed;	/* 驱动已消费字节数 */
	binder_uintptr_t	read_buffer;
};

/* 与 BINDER_VERSION 配合使用，由驱动填充字段 */
struct binder_version {
	/* 驱动协议版本 —— 发生不兼容变更时递增 */
	__s32       protocol_version;
};

/*
 * 与 BINDER_GET_NODE_DEBUG_INFO 配合使用，驱动读 ptr 并写全部字段。
 * 首次调用把 ptr 置 NULL 以获取第一个 node 的信息，
 * 随后把上次返回值传入以遍历后续 node。无更多 node 时 ptr 为 0。
 */
struct binder_node_debug_info {
	binder_uintptr_t ptr;
	binder_uintptr_t cookie;
	__u32            has_strong_ref;
	__u32            has_weak_ref;
};

struct binder_node_info_for_ref {
	__u32            handle;
	__u32            strong_count;
	__u32            weak_count;
	__u32            reserved1;
	__u32            reserved2;
	__u32            reserved3;
};

struct binder_freeze_info {
	__u32            pid;
	__u32            enable;
	__u32            timeout_ms;
};

struct binder_frozen_status_info {
	__u32            pid;

	/* 自上次冻结以来收到的同步事务
	 * bit 0: 被冻结后收到过同步事务
	 * bit 1: 冻结期间有新的 pending 同步事务
	 */
	__u32            sync_recv;

	/* 自上次冻结以来收到的异步事务 */
	__u32            async_recv;
};

struct binder_frozen_state_info {
	binder_uintptr_t cookie;
	__u32            is_frozen;
	__u32            reserved;
};

/**
 * struct binder_extended_error - 扩展错误信息
 * @id:		失败操作的标识符
 * @command:	返回协议定义的命令
 * @param:	存放负 errno 值的参数
 *
 * 与 BINDER_GET_EXTENDED_ERROR 配合使用。用户态可取出该数据
 * 以针对特定错误场景做处理。
 */
struct binder_extended_error {
	__u32	id;
	__u32	command;
	__s32	param;
};

/* ==================================================================
 * Part 3 — ioctl 命令
 * 来源：uapi/linux/android/binder.h
 * ================================================================== */

enum {
	BINDER_WRITE_READ		= _IOWR('b', 1, struct binder_write_read),
	BINDER_SET_IDLE_TIMEOUT		= _IOW('b', 3, __s64),
	BINDER_SET_MAX_THREADS		= _IOW('b', 5, __u32),
	BINDER_SET_IDLE_PRIORITY	= _IOW('b', 6, __s32),
	BINDER_SET_CONTEXT_MGR		= _IOW('b', 7, __s32),
	BINDER_THREAD_EXIT		= _IOW('b', 8, __s32),
	BINDER_VERSION			= _IOWR('b', 9, struct binder_version),
	BINDER_GET_NODE_DEBUG_INFO	= _IOWR('b', 11, struct binder_node_debug_info),
	BINDER_GET_NODE_INFO_FOR_REF	= _IOWR('b', 12, struct binder_node_info_for_ref),
	BINDER_SET_CONTEXT_MGR_EXT	= _IOW('b', 13, struct flat_binder_object),
	BINDER_FREEZE			= _IOW('b', 14, struct binder_freeze_info),
	BINDER_GET_FROZEN_INFO		= _IOWR('b', 15, struct binder_frozen_status_info),
	BINDER_ENABLE_ONEWAY_SPAM_DETECTION	= _IOW('b', 16, __u32),
	BINDER_GET_EXTENDED_ERROR	= _IOWR('b', 17, struct binder_extended_error),
};

/*
 * 调用驱动时应检查的两个特殊错误码：
 *   EINTR       —— 操作被中断，应重试 ioctl 直到返回其他错误码。
 *   ECONNREFUSED —— 驱动不再接受该进程的操作（进程正在销毁）。
 *                   收到后应退出进程；此后该进程所有线程的调用
 *                   都会返回同样的错误码。
 */

/* ==================================================================
 * Part 4 — 事务数据与驱动协议（BR_ / BC_）
 * 来源：uapi/linux/android/binder.h
 * ================================================================== */

enum transaction_flags {
	TF_ONE_WAY	= 0x01,	/* 单向调用：异步，不返回 */
	TF_ROOT_OBJECT	= 0x04,	/* 内容是组件的根对象 */
	TF_STATUS_CODE	= 0x08,	/* 内容是 32 位状态码 */
	TF_ACCEPT_FDS	= 0x10,	/* 允许回复中携带 fd */
	TF_CLEAR_BUF	= 0x20,	/* 事务完成时清空 buffer */
	TF_UPDATE_TXN	= 0x40,	/* 更新过期的 pending 异步事务 */
};

struct binder_transaction_data {
	/* 前两项仅用于 bcTRANSACTION 和 brTRANSACTION，
	 * 标识事务的目标和内容。
	 */
	union {
		/* 命令事务的目标描述符 */
		__u32	handle;
		/* 返回事务的目标描述符 */
		binder_uintptr_t ptr;
	} target;
	binder_uintptr_t	cookie;	/* 目标对象 cookie */
	__u32		code;		/* 事务命令 */

	/* 事务的通用信息 */
	__u32	        flags;
	__kernel_pid_t	sender_pid;
	__kernel_uid32_t	sender_euid;
	binder_size_t	data_size;	/* 数据字节数 */
	binder_size_t	offsets_size;	/* 偏移量字节数 */

	/* 若事务是 inline 的，数据紧随其后；
	 * 否则以一个指向数据 buffer 的指针结束。
	 */
	union {
		struct {
			/* 事务数据 */
			binder_uintptr_t	buffer;
			/* 从 buffer 到 flat_binder_object 结构的偏移 */
			binder_uintptr_t	offsets;
		} ptr;
		__u8	buf[8];
	} data;
};

struct binder_transaction_data_secctx {
	struct binder_transaction_data transaction_data;
	binder_uintptr_t secctx;
};

struct binder_transaction_data_sg {
	struct binder_transaction_data transaction_data;
	binder_size_t buffers_size;
};

struct binder_ptr_cookie {
	binder_uintptr_t ptr;
	binder_uintptr_t cookie;
};

struct binder_handle_cookie {
	__u32 handle;
	binder_uintptr_t cookie;
} __packed;

struct binder_pri_desc {
	__s32 priority;
	__u32 desc;
};

struct binder_pri_ptr_cookie {
	__s32 priority;
	binder_uintptr_t ptr;
	binder_uintptr_t cookie;
};

/* 驱动 -> 用户态 的返回协议 */
enum binder_driver_return_protocol {
	BR_ERROR = _IOR('r', 0, __s32),
	BR_OK = _IO('r', 1),
	BR_TRANSACTION_SEC_CTX = _IOR('r', 2,
				      struct binder_transaction_data_secctx),
	BR_TRANSACTION = _IOR('r', 2, struct binder_transaction_data),
	BR_REPLY = _IOR('r', 3, struct binder_transaction_data),
	BR_ACQUIRE_RESULT = _IOR('r', 4, __s32),
	BR_DEAD_REPLY = _IO('r', 5),
	BR_TRANSACTION_COMPLETE = _IO('r', 6),
	BR_INCREFS = _IOR('r', 7, struct binder_ptr_cookie),
	BR_ACQUIRE = _IOR('r', 8, struct binder_ptr_cookie),
	BR_RELEASE = _IOR('r', 9, struct binder_ptr_cookie),
	BR_DECREFS = _IOR('r', 10, struct binder_ptr_cookie),
	BR_ATTEMPT_ACQUIRE = _IOR('r', 11, struct binder_pri_ptr_cookie),
	BR_NOOP = _IO('r', 12),
	BR_SPAWN_LOOPER = _IO('r', 13),
	BR_FINISHED = _IO('r', 14),
	BR_DEAD_BINDER = _IOR('r', 15, binder_uintptr_t),
	BR_CLEAR_DEATH_NOTIFICATION_DONE = _IOR('r', 16, binder_uintptr_t),
	BR_FAILED_REPLY = _IO('r', 17),
	BR_FROZEN_REPLY = _IO('r', 18),
	BR_ONEWAY_SPAM_SUSPECT = _IO('r', 19),
	BR_TRANSACTION_PENDING_FROZEN = _IO('r', 20),
	BR_FROZEN_BINDER = _IOR('r', 21, struct binder_frozen_state_info),
	BR_CLEAR_FREEZE_NOTIFICATION_DONE = _IOR('r', 22, binder_uintptr_t),
};

/* 用户态 -> 驱动 的命令协议 */
enum binder_driver_command_protocol {
	BC_TRANSACTION = _IOW('c', 0, struct binder_transaction_data),
	BC_REPLY = _IOW('c', 1, struct binder_transaction_data),
	BC_ACQUIRE_RESULT = _IOW('c', 2, __s32),
	BC_FREE_BUFFER = _IOW('c', 3, binder_uintptr_t),
	BC_INCREFS = _IOW('c', 4, __u32),
	BC_ACQUIRE = _IOW('c', 5, __u32),
	BC_RELEASE = _IOW('c', 6, __u32),
	BC_DECREFS = _IOW('c', 7, __u32),
	BC_INCREFS_DONE = _IOW('c', 8, struct binder_ptr_cookie),
	BC_ACQUIRE_DONE = _IOW('c', 9, struct binder_ptr_cookie),
	BC_ATTEMPT_ACQUIRE = _IOW('c', 10, struct binder_pri_desc),
	BC_REGISTER_LOOPER = _IO('c', 11),
	BC_ENTER_LOOPER = _IO('c', 12),
	BC_EXIT_LOOPER = _IO('c', 13),
	BC_REQUEST_DEATH_NOTIFICATION = _IOW('c', 14,
						struct binder_handle_cookie),
	BC_CLEAR_DEATH_NOTIFICATION = _IOW('c', 15,
					    struct binder_handle_cookie),
	BC_DEAD_BINDER_DONE = _IOW('c', 16, binder_uintptr_t),
	BC_TRANSACTION_SG = _IOW('c', 17, struct binder_transaction_data_sg),
	BC_REPLY_SG = _IOW('c', 18, struct binder_transaction_data_sg),
	BC_REQUEST_FREEZE_NOTIFICATION =
			_IOW('c', 19, struct binder_handle_cookie),
	BC_CLEAR_FREEZE_NOTIFICATION = _IOW('c', 20,
					    struct binder_handle_cookie),
	BC_FREEZE_NOTIFICATION_DONE = _IOW('c', 21, binder_uintptr_t),
};

/* ==================================================================
 * Part 5 — dbitmap（动态位图，用于分配最小可用句柄 ID）
 * 来源：drivers/android/dbitmap.h
 *
 * 该库本身不提供并发保护，Binder 用 proc->outer_lock 来保护它。
 * ================================================================== */

#define NBITS_MIN	BITS_PER_TYPE(unsigned long)

struct dbitmap {
	unsigned int nbits;
	unsigned long *map;
};

/* ==================================================================
 * Part 6 — binder 设备与上下文（binderfs）
 * 来源：drivers/android/binder_internal.h
 * ================================================================== */

struct binder_node;	/* 前向声明 */

struct binder_context {
	struct binder_node *binder_context_mgr_node;
	struct mutex context_mgr_node_lock;
	kuid_t binder_context_mgr_uid;
	const char *name;
};

/**
 * struct binder_device - 一个 binder 设备节点的信息
 * @hlist:          binder 设备链表节点
 * @miscdev:        binder 字符设备节点信息
 * @context:        binder 上下文信息
 * @binderfs_inode: 该 binderfs 挂载所属超级块根 dentry 的 inode
 */
struct binder_device {
	struct hlist_node hlist;
	struct miscdevice miscdev;
	struct binder_context context;
	struct inode *binderfs_inode;
	refcount_t ref;
};

/**
 * binderfs_mount_opts - binderfs 挂载选项
 * @max:        可分配的 binderfs binder 设备最大数量
 * @stats_mode: 是否在 binderfs 中启用 binder 统计
 */
struct binderfs_mount_opts {
	int max;
	int stats_mode;
};

/**
 * binderfs_info - 一次 binderfs 挂载的信息
 * @ipc_ns:         该 binderfs 挂载所属的 ipc namespace
 * @control_dentry: 该挂载的 binder-control 设备 dentry
 * @root_uid:       新建设备时使用的 uid
 * @root_gid:       新建设备时使用的 gid
 * @mount_opts:     生效的挂载选项
 * @device_count:   当前已分配的 binder 设备数
 * @proc_log_dir:   存放 per-process 日志的目录 dentry
 */
struct binderfs_info {
	struct ipc_namespace *ipc_ns;
	struct dentry *control_dentry;
	kuid_t root_uid;
	kgid_t root_gid;
	struct binderfs_mount_opts mount_opts;
	int device_count;
	struct dentry *proc_log_dir;
};

struct binder_debugfs_entry {
	const char *name;
	umode_t mode;
	const struct file_operations *fops;
	void *data;
};

#define binder_for_each_debugfs_entry(entry)	\
	for ((entry) = binder_debugfs_entries;	\
	     (entry)->name;			\
	     (entry)++)

/* ==================================================================
 * Part 7 — 统计与工作项
 * 来源：drivers/android/binder_internal.h
 * ================================================================== */

enum binder_stat_types {
	BINDER_STAT_PROC,
	BINDER_STAT_THREAD,
	BINDER_STAT_NODE,
	BINDER_STAT_REF,
	BINDER_STAT_DEATH,
	BINDER_STAT_TRANSACTION,
	BINDER_STAT_TRANSACTION_COMPLETE,
	BINDER_STAT_FREEZE,
	BINDER_STAT_COUNT
};

struct binder_stats {
	atomic_t br[_IOC_NR(BR_CLEAR_FREEZE_NOTIFICATION_DONE) + 1];
	atomic_t bc[_IOC_NR(BC_FREEZE_NOTIFICATION_DONE) + 1];
	atomic_t obj_created[BINDER_STAT_COUNT];
	atomic_t obj_deleted[BINDER_STAT_COUNT];
};

/**
 * struct binder_work - 挂在 worklist 上的工作项
 * @entry: 链表节点
 * @type:  待执行的工作类型
 *
 * proc、thread、node（async）各有独立的工作链表。
 */
struct binder_work {
	struct list_head entry;

	enum binder_work_type {
		BINDER_WORK_TRANSACTION = 1,
		BINDER_WORK_TRANSACTION_COMPLETE,
		BINDER_WORK_TRANSACTION_PENDING,
		BINDER_WORK_TRANSACTION_ONEWAY_SPAM_SUSPECT,
		BINDER_WORK_RETURN_ERROR,
		BINDER_WORK_NODE,
		BINDER_WORK_DEAD_BINDER,
		BINDER_WORK_DEAD_BINDER_AND_CLEAR,
		BINDER_WORK_CLEAR_DEATH_NOTIFICATION,
		BINDER_WORK_FROZEN_BINDER,
		BINDER_WORK_CLEAR_FREEZE_NOTIFICATION,
	} type;
};

struct binder_error {
	struct binder_work work;
	uint32_t cmd;
};

/* ==================================================================
 * Part 8 — 核心 IPC 对象：node（服务端实体）与 ref（客户端引用）
 * 来源：drivers/android/binder_internal.h
 * ================================================================== */

struct binder_proc;	/* 前向声明 */

/**
 * struct binder_node - binder 实体节点记账
 * @debug_id:             调试用唯一 ID（初始化后不变）
 * @lock:                 保护 node 字段的锁
 * @work:                 node 工作用的 worklist 元素（受 @proc->inner_lock 保护）
 * @rb_node:              proc->nodes 红黑树节点（受 @proc->inner_lock 保护）
 * @dead_node:            binder_dead_nodes 链表元素（受 binder_dead_nodes_lock 保护）
 * @proc:                 拥有该 node 的 binder_proc（初始化后不变）
 * @refs:                 该 node 上的引用链表（受 @lock 保护）
 * @internal_strong_refs: 发起事务时持有的强引用（受 @proc->inner_lock 和 @lock 保护）
 * @local_weak_refs:      来自本地进程的弱引用（受 @proc->inner_lock 和 @lock 保护）
 * @local_strong_refs:    来自本地进程的强引用（受 @proc->inner_lock 和 @lock 保护）
 * @tmp_refs:             临时内核引用（受 @proc->inner_lock 保护）
 * @ptr:                  node 的用户空间指针（不变，无需锁）
 * @cookie:               node 的用户空间 cookie（不变，无需锁）
 * @has_strong_ref:       已通知用户态强引用
 * @pending_strong_ref:   用户态已确认强引用通知
 * @has_weak_ref:         已通知用户态弱引用
 * @pending_weak_ref:     用户态已确认弱引用通知
 * @has_async_transaction: 该 node 上有异步事务正在进行（受 @lock 保护）
 * @sched_policy:         node 的最低调度策略（初始化后不变）
 * @accept_fds:           node 是否支持 fd 操作（初始化后不变）
 * @min_priority:         最低调度优先级（初始化后不变）
 * @inherit_rt:           是否从调用方继承 RT 调度策略
 * @txn_security_ctx:     是否要求发送方的安全上下文
 * @async_todo:           异步工作项链表（受 @proc->inner_lock 保护）
 *
 * Binder 实体节点的记账结构。
 */
struct binder_node {
	int debug_id;
	spinlock_t lock;
	struct binder_work work;
	union {
		struct rb_node rb_node;
		struct hlist_node dead_node;
	};
	struct binder_proc *proc;
	struct hlist_head refs;
	int internal_strong_refs;
	int local_weak_refs;
	int local_strong_refs;
	int tmp_refs;
	binder_uintptr_t ptr;
	binder_uintptr_t cookie;
	struct {
		/* 位域元素受 proc inner_lock 保护 */
		u8 has_strong_ref:1;
		u8 pending_strong_ref:1;
		u8 has_weak_ref:1;
		u8 pending_weak_ref:1;
	};
	struct {
		/* 初始化后不变 */
		u8 sched_policy:2;
		u8 inherit_rt:1;
		u8 accept_fds:1;
		u8 txn_security_ctx:1;
		u8 min_priority;
	};
	bool has_async_transaction;
	struct list_head async_todo;
};

struct binder_ref_death {
	/**
	 * @work: 死亡通知用的 worklist 元素
	 *        （受该 ref 所属 proc 的 inner_lock 保护）
	 */
	struct binder_work work;
	binder_uintptr_t cookie;
};

struct binder_ref_freeze {
	struct binder_work work;
	binder_uintptr_t cookie;
	bool is_frozen:1;
	bool sent:1;
	bool resend:1;
};

/**
 * struct binder_ref_data - binder_ref 的计数与 id
 * @debug_id:       ref 的唯一 ID
 * @desc:           ref 的用户空间唯一句柄
 * @strong:         强引用计数（未持锁时仅供调试）
 * @weak:           弱引用计数（未持锁时仅供调试）
 *
 * 由于实际 ref 只能在持锁时访问，该结构用于向 ref inc/dec 函数的
 * 调用者返回 ref 信息。
 */
struct binder_ref_data {
	int debug_id;
	uint32_t desc;
	int strong;
	int weak;
};

/**
 * struct binder_ref - 跟踪对 node 的引用
 * @data:        包含 id、句柄和当前引用计数的 binder_ref_data
 * @rb_node_desc: 按 @data.desc 在 proc 红黑树中查找的节点
 * @rb_node_node: 按 @node 在 proc 红黑树中查找的节点
 * @node_entry:  目标 node 的 node->refs 链表元素（受 @node->lock 保护）
 * @proc:        持有该 ref 的 binder_proc
 * @node:        目标 binder_node。在 binder_cleanup_ref 清理删除时，
 *               @node 非 NULL 表示该 node 必须被释放
 * @death:       若请求了死亡通知则指向 ref_death（受 @node->lock 保护）
 * @freeze:      若请求了冻结通知则指向 ref_freeze（受 @node->lock 保护）
 *
 * 跟踪从 procA 到目标 node（在 procB 上）的引用。
 * 未持 @proc->outer_lock 时访问该结构是不安全的。
 *
 * 需要的查找：
 *   node + proc => ref（事务）
 *   desc + proc => ref（事务、inc/dec ref）
 *   node => refs + procs（进程退出）
 */
struct binder_ref {
	struct binder_ref_data data;
	struct rb_node rb_node_desc;
	struct rb_node rb_node_node;
	struct hlist_node node_entry;
	struct binder_proc *proc;
	struct binder_node *node;
	struct binder_ref_death *death;
	struct binder_ref_freeze *freeze;
};

/* ==================================================================
 * Part 9 — 优先级
 * 来源：drivers/android/binder_internal.h
 * ================================================================== */

/**
 * struct binder_priority - 调度策略与优先级
 * @sched_policy:  调度策略
 * @prio:          SCHED_NORMAL 为 [100..139]，FIFO/RT 为 [0..99]
 *
 * binder 驱动支持继承以下调度策略：
 *   SCHED_NORMAL, SCHED_BATCH, SCHED_FIFO, SCHED_RR
 */
struct binder_priority {
	unsigned int sched_policy;
	int prio;
};

enum binder_prio_state {
	BINDER_PRIO_SET,	/* 已设置期望优先级 */
	BINDER_PRIO_PENDING,	/* 已发起一次保存优先级的恢复 */
	BINDER_PRIO_ABORT,	/* 中止待处理的优先级恢复 */
};

/* ==================================================================
 * Part 10 — 内存分配器：alloc / buffer
 * 来源：drivers/android/binder_alloc.h
 * ================================================================== */

struct binder_transaction;	/* 前向声明 */
struct binder_alloc;		/* 前向声明（binder_shrinker_mdata 先于其定义引用） */

/**
 * struct binder_buffer - 用于 binder 事务的 buffer
 * @entry:               alloc->buffers 链表节点
 * @rb_node:             allocated_buffers/free_buffers 红黑树节点
 * @free:                %true 表示该 buffer 空闲
 * @clear_on_free:       %true 表示使用后必须清零
 * @allow_user_free:     %true 表示允许用户释放该 buffer
 * @async_transaction:   %true 表示该 buffer 正用于异步事务
 * @oneway_spam_suspect: %true 表示异步分配总大小刚超过 spam 检测阈值
 * @debug_id:            调试用唯一 ID
 * @transaction:         关联的 struct binder_transaction
 * @target_node:         关联的 struct binder_node
 * @data_size:           @transaction 数据大小
 * @offsets_size:        偏移数组大小
 * @extra_buffers_size:  其他对象（如 sg list）占用空间大小
 * @user_data:           指向 buffer 空间基址的用户指针
 * @pid:                 归属的 pid（调用方）
 *
 * binder 事务 buffer 的记账结构。
 */
struct binder_buffer {
	struct list_head entry; /* 按地址排列的 free 和 allocated 项 */
	struct rb_node rb_node; /* 按大小排列的 free 项，或按地址排列的 allocated 项 */
	unsigned free:1;
	unsigned clear_on_free:1;
	unsigned allow_user_free:1;
	unsigned async_transaction:1;
	unsigned oneway_spam_suspect:1;
	unsigned debug_id:27;
	struct binder_transaction *transaction;
	struct binder_node *target_node;
	size_t data_size;
	size_t offsets_size;
	size_t extra_buffers_size;
	unsigned long user_data;
	int pid;
};

/**
 * struct binder_shrinker_mdata - 用于回收页面的 binder 元数据
 * @lru:        binder_freelist 中的 LRU 项
 * @alloc:      拥有待回收页面的 binder_alloc
 * @page_index: 待回收页面在 @alloc->pages[] 中的偏移
 */
struct binder_shrinker_mdata {
	struct list_head lru;
	struct binder_alloc *alloc;
	unsigned long page_index;
};

/**
 * struct binder_alloc - 每个 binder proc 的分配器状态
 * @mutex:             保护 binder_alloc 字段
 * @mm:                task->mm 的副本（open 后不变）
 * @vm_start:          通过 mmap 映射的 per-proc 地址空间基址
 * @buffers:           该 proc 所有 buffer 的链表
 * @free_buffers:      可供分配的 buffer 红黑树，按大小排序
 * @allocated_buffers: 已分配 buffer 红黑树，按地址排序
 * @free_async_space:  异步 buffer 可用的 VA 空间。
 *                     mmap 时初始化为完整 VA 空间的一半
 * @pages:             struct page * 数组
 * @buffer_size:       通过 mmap 指定的地址空间大小
 * @pid:               关联 binder_proc 的 pid（初始化后不变）
 * @pages_high:        @pages 中的高水位偏移
 * @mapped:            vm area 是否已映射，每个 binder 实例
 *                     生命周期内只允许映射一次
 * @oneway_spam_detected: %true 表示 oneway spam 检测已触发，
 *                     一旦异步 buffer 恢复健康状态则清除该标志
 *
 * per-proc 地址空间管理的记账结构，通常在 binder_init() 和
 * binder_mmap() 调用期间初始化。该地址空间既用于用户可见的 buffer，
 * 也用于跟踪这些用户 buffer 的 struct binder_buffer 对象。
 */
struct binder_alloc {
	struct mutex mutex;
	struct mm_struct *mm;
	unsigned long vm_start;
	struct list_head buffers;
	struct rb_root free_buffers;
	struct rb_root allocated_buffers;
	size_t free_async_space;
	struct page **pages;
	size_t buffer_size;
	int pid;
	size_t pages_high;
	bool mapped;
	bool oneway_spam_detected;
};

/* ==================================================================
 * Part 11 — 进程：binder_proc
 * 来源：drivers/android/binder_internal.h
 *
 * 驱动中最核心的"总账本"：每个 open("/dev/binder") 的进程对应一个。
 * ================================================================== */

/**
 * struct binder_proc - binder 进程记账
 * @proc_node:          binder_procs 链表元素
 * @threads:            该 proc 的 binder_thread 红黑树（受 @inner_lock 保护）
 * @nodes:              与该 proc 关联的 binder node 红黑树，
 *                      按 node->ptr 排序（受 @inner_lock 保护）
 * @refs_by_desc:       按 ref->desc 排序的 ref 红黑树（受 @outer_lock 保护）
 * @refs_by_node:       按 ref->node 排序的 ref 红黑树（受 @outer_lock 保护）
 * @waiting_threads:    当前等待 proc 工作的线程（受 @inner_lock 保护）
 * @pid                 进程 group_leader 的 PID（初始化后不变）
 * @tsk                 进程 group_leader 的 task_struct（初始化后不变）
 * @cred                binder_open() 中 `struct file` 关联的 struct cred
 *                      （初始化后不变）
 * @deferred_work_node: binder_deferred_list 链表元素
 *                      （受 binder_deferred_lock 保护）
 * @deferred_work:      待执行的延迟工作位图（受 binder_deferred_lock 保护）
 * @outstanding_txns:   在唤醒 freeze_wait 中的进程前需传输的事务数
 *                      （受 @inner_lock 保护）
 * @is_dead:            进程已死，等待事务清理完毕后释放（受 @inner_lock 保护）
 * @is_frozen:          进程被冻结，无法服务 binder 事务（受 @inner_lock 保护）
 * @sync_recv:          自上次冻结以来收到的同步事务
 *                      bit 0: 被冻结后收到同步事务
 *                      bit 1: 冻结期间有新的 pending 同步事务
 *                      （受 @inner_lock 保护）
 * @async_recv:         自上次冻结以来收到的异步事务（受 @inner_lock 保护）
 * @freeze_wait:        等待所有未完成事务处理完毕的进程等待队列
 *                      （受 @inner_lock 保护）
 * @dmap                管理可用引用描述符的 dbitmap（受 @outer_lock 保护）
 * @todo:               该进程的工作链表（受 @inner_lock 保护）
 * @stats:              per-process binder 统计（原子量，无需锁）
 * @delivered_death:    已投递的死亡通知链表（受 @inner_lock 保护）
 * @delivered_freeze:   已投递的冻结通知链表（受 @inner_lock 保护）
 * @max_threads:        binder 线程数上限（受 @inner_lock 保护）
 * @requested_threads:  已请求但尚未启动的 binder 线程数。
 *                      当前实现中只能为 0 或 1（受 @inner_lock 保护）
 * @requested_threads_started: 已启动的 binder 线程数（受 @inner_lock 保护）
 * @tmp_ref:            表示 proc 正在使用的临时引用（受 @inner_lock 保护）
 * @default_priority:   默认调度优先级（初始化后不变）
 * @debugfs_entry:      debugfs 节点
 * @alloc:              binder 分配器记账
 * @context:            该 proc 的 binder_context（初始化后不变）
 * @inner_lock:         可嵌套在 outer_lock 和/或 node lock 之下
 * @outer_lock:         不可嵌套在 inner 或 node lock 之下
 *                      锁顺序：1) outer, 2) node, 3) inner
 * @binderfs_entry:     进程专属的 binderfs 日志文件
 * @oneway_spam_detection_enabled: 进程是否启用了 oneway spam 检测
 *
 * Binder 进程的记账结构。
 */
struct binder_proc {
	struct hlist_node proc_node;
	struct rb_root threads;
	struct rb_root nodes;
	struct rb_root refs_by_desc;
	struct rb_root refs_by_node;
	struct list_head waiting_threads;
	int pid;
	struct task_struct *tsk;
	const struct cred *cred;
	struct hlist_node deferred_work_node;
	int deferred_work;
	int outstanding_txns;
	bool is_dead;
	bool is_frozen;
	bool sync_recv;
	bool async_recv;
	wait_queue_head_t freeze_wait;
	struct dbitmap dmap;
	struct list_head todo;
	struct binder_stats stats;
	struct list_head delivered_death;
	struct list_head delivered_freeze;
	u32 max_threads;
	int requested_threads;
	int requested_threads_started;
	int tmp_ref;
	struct binder_priority default_priority;
	struct dentry *debugfs_entry;
	struct binder_alloc alloc;
	struct binder_context *context;
	spinlock_t inner_lock;
	spinlock_t outer_lock;
	struct dentry *binderfs_entry;
	bool oneway_spam_detection_enabled;
};

/* ==================================================================
 * Part 12 — 线程：binder_thread
 * 来源：drivers/android/binder_internal.h
 * ================================================================== */

/**
 * struct binder_thread - binder 线程记账
 * @proc:                该线程所属的 binder 进程（初始化后不变）
 * @rb_node:             proc->threads 红黑树节点（受 @proc->inner_lock 保护）
 * @waiting_thread_node: @proc->waiting_threads 链表元素
 *                       （受 @proc->inner_lock 保护）
 * @pid:                 该线程的 PID（初始化后不变）
 * @looper:              循环状态位图（仅由本线程访问）
 * @looper_need_return:  需要退出驱动的循环线程（无需锁）
 * @transaction_stack:   该线程正在进行的事务栈（受 @proc->inner_lock 保护）
 * @todo:                该线程待办工作链表（受 @proc->inner_lock 保护）
 * @process_todo:        @todo 中的工作是否应被处理（受 @proc->inner_lock 保护）
 * @return_error:        本线程报告的事务错误（仅由本线程访问）
 * @reply_error:         目标线程报告的事务错误（受 @proc->inner_lock 保护）
 * @ee:                  本线程的扩展错误信息（受 @proc->inner_lock 保护）
 * @wait:                线程工作等待队列
 * @stats:               per-thread 统计（原子量，无需锁）
 * @tmp_ref:             表示线程正在使用的临时引用
 *                       （用 atomic，因为 @proc->inner_lock 不总是能获取）
 * @is_dead:             线程已死，等待事务清理完毕后释放
 *                       （受 @proc->inner_lock 保护）
 * @task:                该线程的 struct task_struct
 * @prio_lock:           保护线程优先级字段
 * @prio_next:           下次要恢复的已保存优先级（受 @prio_lock 保护）
 * @prio_state:          优先级恢复过程的状态，见 enum binder_prio_state
 *                       （受 @prio_lock 保护）
 *
 * Binder 线程的记账结构。
 */
struct binder_thread {
	struct binder_proc *proc;
	struct rb_node rb_node;
	struct list_head waiting_thread_node;
	int pid;
	int looper;              /* 仅由本线程修改 */
	bool looper_need_return; /* 可由其他线程写入 */
	struct binder_transaction *transaction_stack;
	struct list_head todo;
	bool process_todo;
	struct binder_error return_error;
	struct binder_error reply_error;
	struct binder_extended_error ee;
	wait_queue_head_t wait;
	struct binder_stats stats;
	atomic_t tmp_ref;
	bool is_dead;
	struct task_struct *task;
	spinlock_t prio_lock;
	struct binder_priority prio_next;
	enum binder_prio_state prio_state;
};

/* ==================================================================
 * Part 13 — 事务：binder_transaction
 * 来源：drivers/android/binder_internal.h
 * ================================================================== */

/**
 * struct binder_txn_fd_fixup - 事务 fd 修正链表元素
 * @fixup_entry:  链表项
 * @file:        要与新 fd 关联的 struct file
 * @offset:      该 fixup 在 buffer 数据中的偏移
 * @target_fd:   目标安装 @file 时使用的 fd
 *
 * 由于文件描述符必须在目标进程的上下文中分配，
 * 每个要处理的 fd 都通过该结构传递。
 */
struct binder_txn_fd_fixup {
	struct list_head fixup_entry;
	struct file *file;
	size_t offset;
	int target_fd;
};

struct binder_transaction {
	int debug_id;
	struct binder_work work;
	struct binder_thread *from;
	pid_t from_pid;
	pid_t from_tid;
	struct binder_transaction *from_parent;
	struct binder_proc *to_proc;
	struct binder_thread *to_thread;
	struct binder_transaction *to_parent;
	unsigned need_reply:1;
	/* unsigned is_dead:1; */       /* 当前未使用 */

	struct binder_buffer *buffer;
	unsigned int    code;
	unsigned int    flags;
	struct binder_priority priority;
	struct binder_priority saved_priority;
	bool set_priority_called;
	bool is_nested;
	kuid_t  sender_euid;
	ktime_t start_time;
	struct list_head fd_fixups;
	binder_uintptr_t security_ctx;
	/**
	 * @lock:  保护 @from、@to_proc 和 @to_thread
	 *
	 * 在线程拆除期间，@from、@to_proc 和 @to_thread 可能被置为 NULL
	 */
	spinlock_t lock;
};

/* ==================================================================
 * Part 14 — 对象联合体
 * 来源：drivers/android/binder_internal.h
 * ================================================================== */

/**
 * struct binder_object - 扁平 binder 对象类型的联合体
 * @hdr:   通用对象头部
 * @fbo:   binder 对象（node 和 ref）
 * @fdo:   文件描述符对象
 * @bbo:   binder buffer 指针
 * @fdao:  文件描述符数组
 *
 * 用于类型无关的对象拷贝。
 */
struct binder_object {
	union {
		struct binder_object_header hdr;
		struct flat_binder_object fbo;
		struct binder_fd_object fdo;
		struct binder_buffer_object bbo;
		struct binder_fd_array_object fdao;
	};
};

#endif /* _BINDER_ALL_STRUCTS_H_REFERENCE_ONLY */

/*
 * ==================================================================
 * 附：对象模型与锁速查
 * ==================================================================
 *
 * 对象层次：
 *   binder_device  (一个 /dev/binderX 设备节点)
 *     └── binder_context  (binder / hwbinder / vndbinder 各自的上下文)
 *   binder_proc    (一个 open 了 binder 的进程，驱动的"总账本")
 *     ├── threads            (rbtree of binder_thread)
 *     ├── nodes              (rbtree of binder_node，本进程导出的服务实体)
 *     ├── refs_by_desc       (rbtree of binder_ref，按句柄查)
 *     ├── refs_by_node       (rbtree of binder_ref，按目标 node 查)
 *     ├── todo               (进程级待办工作链表)
 *     ├── waiting_threads    (阻塞等待工作的线程)
 *     ├── alloc              (binder_alloc，内核缓冲区管理)
 *     │     ├── buffers           (所有 buffer 链表)
 *     │     ├── free_buffers      (按大小排序的空闲 buffer)
 *     │     └── allocated_buffers (按地址排序的已分配 buffer)
 *     └── dmap               (dbitmap，分配最小可用句柄 desc)
 *
 *   binder_thread  (线程池中的一个线程)
 *     ├── todo               (线程私有待办)
 *     └── transaction_stack  (嵌套事务栈)
 *
 *   binder_transaction  (一次 IPC)
 *     ├── from / to_proc / to_thread
 *     ├── buffer             (binder_buffer，承载数据)
 *     └── fd_fixups          (待在目标进程安装的 fd)
 *
 * 锁顺序（必须严格遵守，否则死锁）：
 *   1) proc->outer_lock   —— 保护 refs_by_desc/refs_by_node 和 dmap
 *   2) node->lock         —— 保护单个 node 的引用计数等
 *   3) proc->inner_lock   —— 保护 todo、threads、nodes 等
 *
 * 另有两个全局锁：
 *   binder_deferred_lock —— 保护 deferred_work 延迟释放
 *   binder_dead_nodes_lock —— 保护已死 node 列表
 *
 * 生命周期：
 *   binder_open()      -> 创建 binder_proc，挂入全局 binder_procs
 *   binder_mmap()      -> 初始化 proc->alloc 地址空间
 *   binder_ioctl(WRITE_READ) -> 事务收发、线程注册、引用增减
 *   close(fd) / 进程退出 -> deferred 释放，等 outstanding_txns 归零
 *                           后由 binder_free_proc() 真正销毁
 * ==================================================================
 */
