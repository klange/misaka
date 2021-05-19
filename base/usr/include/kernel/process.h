#pragma once

#include <stdint.h>
#include <kernel/types.h>
#include <kernel/vfs.h>
#include <kernel/tree.h>
#include <kernel/list.h>
#include <kernel/arch/x86_64/pml.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/signal_defs.h>

#define PROC_REUSE_FDS 0x0001
#define KERNEL_STACK_SIZE 0x9000
#define USER_ROOT_UID 0

typedef struct {
	intptr_t refcount;
	union PML * directory;
} page_directory_t;

typedef struct {
	uintptr_t sp;        /* 0 */
	uintptr_t bp;        /* 8 */
	uintptr_t ip;        /* 16 */
	uintptr_t tls_base;  /* 24 */
	uintptr_t saved[5]; /* XXX Arch dependent */
	/**
	 * 32: rbx
	 * 40: r12
	 * 48: r13
	 * 56: r14
	 * 64: r15
	 */
} kthread_context_t;

typedef struct thread {
	kthread_context_t context;
	uint8_t fp_regs[512];
	page_directory_t * page_directory;
} thread_t;

typedef struct image {
	uintptr_t entry;
	uintptr_t heap;
	uintptr_t stack;
	uintptr_t shm_heap;
	volatile int lock[2];
} image_t;

typedef struct file_descriptors {
	fs_node_t ** entries;
	uint64_t * offsets;
	int * modes;
	size_t length;
	size_t capacity;
	size_t refs;
} fd_table_t;

#define PROC_FLAG_IS_TASKLET 0x01
#define PROC_FLAG_FINISHED   0x02
#define PROC_FLAG_STARTED    0x04
#define PROC_FLAG_RUNNING    0x08
#define PROC_FLAG_SLEEP_INT  0x10
#define PROC_FLAG_SUSPENDED  0x20

typedef struct process {
	pid_t id;    /* PID */
	pid_t group; /* thread group */
	pid_t job;   /* tty job */
	pid_t session; /* tty session */
	int status; /* status code */
	unsigned int flags; /* finished, started, running, isTasklet */

	uid_t user;
	uid_t real_user;
	unsigned int mask;

	char * name;
	char * description;
	char ** cmdline;

	char * wd_name;
	fs_node_t * wd_node;
	fd_table_t *  fds;               /* File descriptor table */

	tree_node_t * tree_entry;
	struct regs * syscall_registers;
	struct regs * interrupt_registers;
	list_t * wait_queue;
	list_t * shm_mappings;
	list_t * node_waits;
	list_t * signal_queue;
	char * signal_kstack;

	node_t sched_node;
	node_t sleep_node;
	node_t * timed_sleep_node;
	node_t * timeout_node;

	struct timeval start;
	int awoken_index;

	thread_t thread;
	thread_t signal_state;
	image_t image;

	uintptr_t signals[NUMSIGNALS+1];
} process_t;

typedef struct {
	uint64_t end_tick;
	uint64_t end_subtick;
	process_t * process;
	int is_fswait;
} sleeper_t;

extern volatile process_t * current_process;
extern process_t * kernel_idle_task;
extern unsigned long process_append_fd(process_t * proc, fs_node_t * node);
extern long process_move_fd(process_t * proc, long src, long dest);
extern void initialize_process_tree(void);
extern process_t * process_from_pid(pid_t pid);

extern void process_delete(process_t * proc);
extern void make_process_ready(volatile process_t * proc);
extern process_t * next_ready_process(void);
extern int wakeup_queue(list_t * queue);
extern int wakeup_queue_interrupted(list_t * queue);
extern int sleep_on(list_t * queue);
extern int process_alert_node(process_t * process, void * value);
extern void sleep_until(process_t * process, unsigned long seconds, unsigned long subseconds);
extern void switch_task(uint8_t reschedule);
extern int process_wait_nodes(process_t * process,fs_node_t * nodes[], int timeout);
extern process_t * process_get_parent(process_t * process);
extern int process_is_ready(process_t * proc);
extern void wakeup_sleepers(unsigned long seconds, unsigned long subseconds);
extern void task_exit(int retval);
extern __attribute__((noreturn)) void switch_next(void);
extern int process_awaken_from_fswait(process_t * process, int index);
extern void process_release_directory(page_directory_t * dir);
extern process_t * spawn_worker_thread(void (*entrypoint)(void * argp), const char * name, void * argp);
extern pid_t fork(void);
extern pid_t clone(uintptr_t new_stack, uintptr_t thread_func, uintptr_t arg);
extern int waitpid(int pid, int * status, int options);
extern int exec(const char * path, int argc, char *const argv[], char *const env[], int interp_depth);

extern tree_t * process_tree;  /* Parent->Children tree */
extern list_t * process_list;  /* Flat storage */
extern list_t * process_queue; /* Ready queue */
extern list_t * sleep_queue;

extern void arch_enter_tasklet(void);
extern __attribute__((noreturn)) void arch_resume_user(void);
extern __attribute__((noreturn)) void arch_restore_context(volatile thread_t * buf);
extern __attribute__((returns_twice)) int arch_save_context(volatile thread_t * buf);
extern void arch_restore_floating(process_t * proc);
extern void arch_save_floating(process_t * proc);
extern void arch_set_kernel_stack(uintptr_t);
extern void arch_enter_user(uintptr_t entrypoint, int argc, char * argv[], char * envp[], uintptr_t stack);
__attribute__((noreturn))
extern void arch_enter_signal_handler(uintptr_t,int);

