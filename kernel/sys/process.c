#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/spinlock.h>
#include <kernel/tree.h>
#include <kernel/list.h>

static fs_node_t * _entries[24] = {
	(fs_node_t *)1, (fs_node_t *)1, (fs_node_t *)1,
	NULL,
};

static uint64_t _offsets[24] = {0};
static int _modes[24] = {1, 2, 2, 0};

static fd_table_t _fake_fds = {
	_entries,
	_offsets,
	_modes,
	3,
	24,
	1,
};

static process_t _fake_process = {
	.user = 0,
	.real_user = 0,
	.wd_name = "/",
	.fds = &_fake_fds,
};

tree_t * process_tree;  /* Parent->Children tree */
list_t * process_list;  /* Flat storage */
list_t * process_queue; /* Ready queue */
list_t * sleep_queue;
volatile process_t * current_process = &_fake_process;
static spin_lock_t tree_lock = { 0 };
static spin_lock_t process_queue_lock = { 0 };
static spin_lock_t wait_lock_tmp = { 0 };
static spin_lock_t sleep_lock = { 0 };

uint8_t process_compare(void * proc_v, void * pid_v) {
	pid_t pid = (*(pid_t *)pid_v);
	process_t * proc = (process_t *)proc_v;

	return (uint8_t)(proc->id == pid);
}

void initialize_process_tree(void) {
	process_tree = tree_create();
	process_list = list_create();
	process_queue = list_create();
	sleep_queue = list_create();

	/* TODO: PID bitset? */
}

int is_valid_process(process_t * process) {
	foreach(lnode, process_list) {
		if (lnode->value == process) {
			return 1;
		}
	}

	return 0;
}

unsigned long process_append_fd(process_t * proc, fs_node_t * node) {
	/* Fill gaps */
	for (unsigned long i = 0; i < proc->fds->length; ++i) {
		if (!proc->fds->entries[i]) {
			proc->fds->entries[i] = node;
			/* modes, offsets must be set by caller */
			proc->fds->modes[i] = 0;
			proc->fds->offsets[i] = 0;
			return i;
		}
	}
	/* No gaps, expand */
	if (proc->fds->length == proc->fds->capacity) {
		proc->fds->capacity *= 2;
		proc->fds->entries = realloc(proc->fds->entries, sizeof(fs_node_t *) * proc->fds->capacity);
		proc->fds->modes   = realloc(proc->fds->modes,   sizeof(int) * proc->fds->capacity);
		proc->fds->offsets = realloc(proc->fds->offsets, sizeof(uint64_t) * proc->fds->capacity);
	}
	proc->fds->entries[proc->fds->length] = node;
	/* modes, offsets must be set by caller */
	proc->fds->modes[proc->fds->length] = 0;
	proc->fds->offsets[proc->fds->length] = 0;
	proc->fds->length++;
	return proc->fds->length-1;
}

pid_t get_next_pid(void) {
	static pid_t _next_pid = 1;
	return _next_pid++;
}

#define PROC_REUSE_FDS 0x0001

#define KERNEL_STACK_SIZE 0x8000

process_t * spawn_process(volatile process_t * parent, int flags) {
	process_t * proc = calloc(1,sizeof(process_t));

	proc->id          = get_next_pid();
	proc->group       = proc->id;
	proc->name        = strdup(parent->name);
	proc->description = NULL;
	proc->cmdline     = parent->cmdline; /* FIXME dup it? */

	proc->user        = parent->user;
	proc->real_user   = parent->real_user;
	proc->mask        = parent->mask;
	proc->job         = parent->job;
	proc->session     = parent->session;

	proc->thread.sp = 0;
	proc->thread.bp = 0;
	proc->thread.ip = 0;
	proc->thread.flags = 0;
	memcpy((void*)proc->thread.fp_regs, (void*)parent->thread.fp_regs, sizeof(parent->thread.fp_regs));

	proc->image.entry       = parent->image.entry;
	proc->image.heap        = parent->image.heap;
	proc->image.heap_actual = parent->image.heap_actual; /* XXX is this used? */
	proc->image.size        = parent->image.size; /* XXX same ^^ */
	proc->image.stack       = (uintptr_t)valloc(KERNEL_STACK_SIZE) + KERNEL_STACK_SIZE;
	proc->image.user_stack  = parent->image.user_stack;
	proc->image.shm_heap    = 0;

	if (flags & PROC_REUSE_FDS) {
		proc->fds = parent->fds;
		proc->fds->refs++; /* FIXME lock? */
	} else {
		proc->fds = malloc(sizeof(fd_table_t));
		proc->fds->refs = 1;
		proc->fds->length = parent->fds->length;
		proc->fds->capacity = parent->fds->capacity;
		proc->fds->entries = malloc(proc->fds->capacity * sizeof(fs_node_t *));
		proc->fds->modes   = malloc(proc->fds->capacity * sizeof(int));
		proc->fds->offsets = malloc(proc->fds->capacity * sizeof(uint64_t));
		for (uint32_t i = 0; i < parent->fds->length; ++i) {
			proc->fds->entries[i] = clone_fs(parent->fds->entries[i]);
			proc->fds->modes[i]   = parent->fds->modes[i];
			proc->fds->offsets[i] = parent->fds->offsets[i];
		}
	}

	proc->wd_node = clone_fs(parent->wd_node);
	proc->wd_name = strdup(parent->wd_name);

	proc->wait_queue   = list_create();
	proc->shm_mappings = list_create();
	proc->signal_queue = list_create();

	proc->sched_node.value = proc;
	proc->sleep_node.value = proc;

	gettimeofday(&proc->start, NULL);
	tree_node_t * entry = tree_node_create(proc);
	proc->tree_entry = entry;

	/* FIXME insert into process tree? */
	/* FIXME insert into process list? */
	return proc;
}

process_t * process_from_pid(pid_t pid) {
	if (pid < 0) return NULL;

	spin_lock(tree_lock);
	tree_node_t * entry = tree_find(process_tree,&pid,process_compare);
	spin_unlock(tree_lock);
	if (entry) {
		return (process_t *)entry->value;
	}
	return NULL;
}


long process_move_fd(process_t * proc, long src, long dest) {
	if ((size_t)src >= proc->fds->length || (dest != -1 && (size_t)dest >= proc->fds->length)) {
		return -1;
	}
	if (dest == -1) {
		dest = process_append_fd(proc, NULL);
	}
	if (proc->fds->entries[dest] != proc->fds->entries[src]) {
		close_fs(proc->fds->entries[dest]);
		proc->fds->entries[dest] = proc->fds->entries[src];
		proc->fds->modes[dest] = proc->fds->modes[src];
		proc->fds->offsets[dest] = proc->fds->offsets[src];
		open_fs(proc->fds->entries[dest], 0);
	}
	return dest;
}
