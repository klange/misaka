#include <stdint.h>
#include <errno.h>
#include <sys/sysfunc.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <syscall_nums.h>
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/version.h>

#include <kernel/arch/x86_64/regs.h>
#include <kernel/arch/x86_64/mmu.h>

/* TODO: kernel/arch/x86_64/syscall.c/h ? */
static unsigned long arch_syscall_number(struct regs * r) {
	return (unsigned long)r->rax;
}

static void arch_syscall_return(struct regs * r, long retval) {
	r->rax = retval;
}

static long arch_syscall_arg0(struct regs * r) { return r->rbx; }
static long arch_syscall_arg1(struct regs * r) { return r->rcx; }
static long arch_syscall_arg2(struct regs * r) { return r->rdx; }
static long arch_syscall_arg3(struct regs * r) { return r->rsi; }
static long arch_syscall_arg4(struct regs * r) { return r->rdi; }

static char   hostname[256];
static size_t hostname_len = 0;

#define FD_INRANGE(FD) \
	((FD) < (int)current_process->fds->length && (FD) >= 0)
#define FD_ENTRY(FD) \
	(current_process->fds->entries[(FD)])
#define FD_CHECK(FD) \
	(FD_INRANGE(FD) && FD_ENTRY(FD))
#define FD_OFFSET(FD) \
	(current_process->fds->offsets[(FD)])
#define FD_MODE(FD) \
	(current_process->fds->modes[(FD)])

#define PTR_INRANGE(PTR) \
	((uintptr_t)(PTR) > current_process->image.entry && ((uintptr_t)(PTR) < 0x8000000000000000))
#define PTR_VALIDATE(PTR) \
	ptr_validate((void *)(PTR), __func__)

static void ptr_validate(void * ptr, const char * syscall) {
	if (ptr && !PTR_INRANGE(ptr)) {
		printf("invalid pointer passed to %s (%p < %p)\n",
			syscall, ptr, current_process->image.entry);
		while (1) {}
	}
}

static const char * syscallNames[] = {
#define _(o) [o] = #o,
_(SYS_EXT)
_(SYS_GETEUID)
_(SYS_OPEN)
_(SYS_READ)
_(SYS_WRITE)
_(SYS_CLOSE)
_(SYS_GETTIMEOFDAY)
_(SYS_EXECVE)
_(SYS_FORK)
_(SYS_GETPID)
_(SYS_SBRK)
_(SYS_UNAME)
_(SYS_OPENPTY)
_(SYS_SEEK)
_(SYS_STAT)
_(SYS_MKPIPE)
_(SYS_DUP2)
_(SYS_GETUID)
_(SYS_SETUID)
_(SYS_REBOOT)
_(SYS_READDIR)
_(SYS_CHDIR)
_(SYS_GETCWD)
_(SYS_CLONE)
_(SYS_SETHOSTNAME)
_(SYS_GETHOSTNAME)
_(SYS_MKDIR)
_(SYS_SHM_OBTAIN)
_(SYS_SHM_RELEASE)
_(SYS_KILL)
_(SYS_SIGNAL)
_(SYS_GETTID)
_(SYS_YIELD)
_(SYS_SYSFUNC)
_(SYS_SLEEPABS)
_(SYS_SLEEP)
_(SYS_IOCTL)
_(SYS_ACCESS)
_(SYS_STATF)
_(SYS_CHMOD)
_(SYS_UMASK)
_(SYS_UNLINK)
_(SYS_WAITPID)
_(SYS_PIPE)
_(SYS_MOUNT)
_(SYS_SYMLINK)
_(SYS_READLINK)
_(SYS_LSTAT)
_(SYS_FSWAIT)
_(SYS_FSWAIT2)
_(SYS_CHOWN)
_(SYS_SETSID)
_(SYS_SETPGID)
_(SYS_GETPGID)
_(SYS_FSWAIT3)
};

static long unimplemented(void) {
	printf("unimplemented system call %s (%ld)\n",
		syscallNames[arch_syscall_number(current_process->syscall_registers)],
		arch_syscall_number(current_process->syscall_registers));
	return -EINVAL;
}

static long sys_sbrk(ssize_t size) {
	if (size & 0xFFF) return -EINVAL;
	uintptr_t out = current_process->image.heap;
	for (uintptr_t i = out; i < out + size; i += 0x1000) {
		union PML * page = mmu_get_page(i, MMU_GET_MAKE);
		mmu_frame_allocate(page, MMU_FLAG_WRITABLE);
	}
	current_process->image.heap += size;
	return (long)out;
}

extern void arch_set_tls_base(uintptr_t tlsbase);
static long sys_sysfunc(long fn, char ** args) {
	/* FIXME: Most of these should be top-level, many are hacks/broken in Misaka */
	switch (fn) {
		case TOARU_SYS_FUNC_SYNC:
			/* FIXME: There is no sync ability in the VFS at the moment. */
			printf("sync: not implemented\n");
			return -EINVAL;
		case TOARU_SYS_FUNC_LOGHERE:
			/* FIXME: Needs to redirect kprintf to the argument */
			printf("loghere: not implemented\n");
			return -EINVAL;
		case TOARU_SYS_FUNC_SETFDS:
			/* XXX Unused */
			printf("setfds: not implemented\n");
			return -EINVAL;
		case TOARU_SYS_FUNC_WRITESDB:
			/* XXX Unused */
			printf("writesdb: not implemented\n");
			return -EINVAL;
		case TOARU_SYS_FUNC_KDEBUG:
			/* FIXME: Starts kernel debugger as a child task of this process */
			printf("kdebug: not implemented\n");
			return -EINVAL;
		case TOARU_SYS_FUNC_INSMOD:
			/* FIXME: Load module */
			printf("insmod: not implemented\n");
			return -EINVAL;
		/* Begin unpriv */
		case TOARU_SYS_FUNC_SETHEAP:
			current_process->image.heap = (uintptr_t)args[0];
			return 0;
		case TOARU_SYS_FUNC_MMAP:
			/* FIXME: This whole thing should be removed, tbh */
			for (uintptr_t i = (uintptr_t)args[0]; i < (uintptr_t)args[0] + (size_t)args[1]; i += 0x1000) {
				union PML * page = mmu_get_page(i, MMU_GET_MAKE);
				mmu_frame_allocate(page, MMU_FLAG_WRITABLE);
			}
			return 0;
		case TOARU_SYS_FUNC_THREADNAME: {
			/* This should probably be moved to a new system call. */
			int count = 0;
			char **arg = args;
			PTR_VALIDATE(args);
			while (*arg) {
				PTR_VALIDATE(*args);
				count++;
				arg++;
			}
			current_process->cmdline = malloc(sizeof(char*)*(count+1));
			int i = 0;
			while (i < count) {
				current_process->cmdline[i] = strdup(args[i]);
				i++;
			}
			current_process->cmdline[i] = NULL;
			return 0;
		}
		case TOARU_SYS_FUNC_DEBUGPRINT:
			/* XXX I think _xlog uses this? */
			printf("debugprint: not implemented\n");
			return -EINVAL;
		case TOARU_SYS_FUNC_SETVGACURSOR:
			/* XXX This should be a device driver, along with the text-mode window... */
			printf("setvgacursor: not implemented\n");
			return -EINVAL;
		case TOARU_SYS_FUNC_SETGSBASE:
			PTR_VALIDATE(args);
			current_process->thread.tls_base = (uintptr_t)args[0];
			arch_set_tls_base(current_process->thread.tls_base);
			return 0;
		default:
			printf("Bad system function: %ld\n", fn);
			return -EINVAL;
	}
}

__attribute__((noreturn))
static long sys_exit(long exitcode) {
	/* FIXME: @ref task_exit */
	printf("(process exited with %ld)\n", exitcode);
	while (1) {};
}

static long sys_write(long fd, char * ptr, unsigned long len) {
	if (FD_CHECK(fd)) {
		PTR_VALIDATE(ptr);
		fs_node_t * node = FD_ENTRY(fd);
		if (!(FD_MODE(fd) & 2)) return -EACCES;
		long out = write_fs(node, FD_OFFSET(fd), len, (uint8_t*)ptr);
		FD_OFFSET(fd) += out;
		return out;
	}
	return -EBADF;
}

static long stat_node(fs_node_t * fn, uintptr_t st) {
	struct stat * f = (struct stat *)st;

	PTR_VALIDATE(f);

	if (!fn) {
		/* XXX: Does this need to zero the stat struct when returning -ENOENT? */
		memset(f, 0x00, sizeof(struct stat));
		return -ENOENT;
	}

	f->st_dev   = (uint16_t)(((uint64_t)fn->device & 0xFFFF0) >> 8);
	f->st_ino   = fn->inode;

	uint32_t flags = 0;
	if (fn->flags & FS_FILE)        { flags |= _IFREG; }
	if (fn->flags & FS_DIRECTORY)   { flags |= _IFDIR; }
	if (fn->flags & FS_CHARDEVICE)  { flags |= _IFCHR; }
	if (fn->flags & FS_BLOCKDEVICE) { flags |= _IFBLK; }
	if (fn->flags & FS_PIPE)        { flags |= _IFIFO; }
	if (fn->flags & FS_SYMLINK)     { flags |= _IFLNK; }

	f->st_mode  = fn->mask | flags;
	f->st_nlink = fn->nlink;
	f->st_uid   = fn->uid;
	f->st_gid   = fn->gid;
	f->st_rdev  = 0;
	f->st_size  = fn->length;

	f->st_atime = fn->atime;
	f->st_mtime = fn->mtime;
	f->st_ctime = fn->ctime;
	f->st_blksize = 512; /* whatever */

	if (fn->get_size) {
		f->st_size = fn->get_size(fn);
	}

	return 0;
}

static long sys_stat(long fd, uintptr_t st) {
	PTR_VALIDATE(st);
	if (FD_CHECK(fd)) {
		return stat_node(FD_ENTRY(fd), st);
	}
	return -EBADF;
}

static long sys_statf(char * file, uintptr_t st) {
	int result;
	PTR_VALIDATE(file);
	PTR_VALIDATE(st);
	fs_node_t * fn = kopen(file, 0);
	result = stat_node(fn, st);
	if (fn) {
		close_fs(fn);
	}
	return result;
}

static long sys_symlink(char * target, char * name) {
	PTR_VALIDATE(target);
	PTR_VALIDATE(name);
	return symlink_fs(target, name);
}

static long sys_readlink(const char * file, char * ptr, long len) {
	PTR_VALIDATE(file);
	fs_node_t * node = kopen((char *) file, O_PATH | O_NOFOLLOW);
	if (!node) {
		return -ENOENT;
	}
	long rv = readlink_fs(node, ptr, len);
	close_fs(node);
	return rv;
}

static long sys_lstat(char * file, uintptr_t st) {
	PTR_VALIDATE(file);
	PTR_VALIDATE(st);
	fs_node_t * fn = kopen(file, O_PATH | O_NOFOLLOW);
	long result = stat_node(fn, st);
	if (fn) {
		close_fs(fn);
	}
	return result;
}

static long sys_open(const char * file, long flags, long mode) {
	PTR_VALIDATE(file);
	fs_node_t * node = kopen((char *)file, flags);

	int access_bits = 0;

	if (node && (flags & O_CREAT) && (flags & O_EXCL)) {
		close_fs(node);
		return -EEXIST;
	}

	if (!(flags & O_WRONLY) || (flags & O_RDWR)) {
		if (node && !has_permission(node, 04)) {
			close_fs(node);
			return -EACCES;
		} else {
			access_bits |= 01;
		}
	}

	if ((flags & O_RDWR) || (flags & O_WRONLY)) {
		if (node && !has_permission(node, 02)) {
			close_fs(node);
			return -EACCES;
		}
		if (node && (node->flags & FS_DIRECTORY)) {
			return -EISDIR;
		}
		if ((flags & O_RDWR) || (flags & O_WRONLY)) {
			/* truncate doesn't grant write permissions */
			access_bits |= 02;
		}
	}

	if (!node && (flags & O_CREAT)) {
		/* TODO check directory permissions */
		int result = create_file_fs((char *)file, mode);
		if (!result) {
			node = kopen((char *)file, flags);
		} else {
			return result;
		}
	}

	if (node && (flags & O_DIRECTORY)) {
		if (!(node->flags & FS_DIRECTORY)) {
			return -ENOTDIR;
		}
	}

	if (node && (flags & O_TRUNC)) {
		if (!(access_bits & 02)) {
			close_fs(node);
			return -EINVAL;
		}
		truncate_fs(node);
	}

	if (!node) {
		return -ENOENT;
	}
	if (node && (flags & O_CREAT) && (node->flags & FS_DIRECTORY)) {
		close_fs(node);
		return -EISDIR;
	}
	long fd = process_append_fd((process_t *)current_process, node);
	FD_MODE(fd) = access_bits;
	if (flags & O_APPEND) {
		FD_OFFSET(fd) = node->length;
	} else {
		FD_OFFSET(fd) = 0;
	}
	return fd;
}

static long sys_close(long fd) {
	if (FD_CHECK(fd)) {
		close_fs(FD_ENTRY(fd));
		FD_ENTRY(fd) = NULL;
		return 0;
	}
	return -EBADF;
}

static long sys_seek(long fd, long offset, long whence) {
	if (FD_CHECK(fd)) {
		if ((FD_ENTRY(fd)->flags & FS_PIPE) || (FD_ENTRY(fd)->flags & FS_CHARDEVICE)) return -ESPIPE;
		switch (whence) {
			case 0:
				FD_OFFSET(fd) = offset;
				break;
			case 1:
				FD_OFFSET(fd) += offset;
				break;
			case 2:
				FD_OFFSET(fd) = FD_ENTRY(fd)->length + offset;
				break;
			default:
				return -EINVAL;
		}
		return FD_OFFSET(fd);
	}
	return -EBADF;
}

static long sys_read(long fd, char * ptr, unsigned long len) {
	if (FD_CHECK(fd)) {
		PTR_VALIDATE(ptr);

		fs_node_t * node = FD_ENTRY(fd);
		if (!(FD_MODE(fd) & 01)) {
			return -EACCES;
		}
		long out = read_fs(node, FD_OFFSET(fd), len, (uint8_t *)ptr);
		FD_OFFSET(fd) += out;
		return out;
	}
	return -EBADF;
}

static long sys_ioctl(long fd, long request, void * argp) {
	if (FD_CHECK(fd)) {
		PTR_VALIDATE(argp);
		return ioctl_fs(FD_ENTRY(fd), request, argp);
	}
	return -EBADF;
}

static long sys_readdir(long fd, long index, struct dirent * entry) {
	if (FD_CHECK(fd)) {
		PTR_VALIDATE(entry);
		struct dirent * kentry = readdir_fs(FD_ENTRY(fd), (uint64_t)index);
		if (kentry) {
			memcpy(entry, kentry, sizeof *entry);
			free(kentry);
			return 1;
		} else {
			return 0;
		}
	}
	return -EBADF;
}

static long sys_mkdir(char * path, uint64_t mode) {
	return mkdir_fs(path, mode);
}

static long sys_access(const char * file, long flags) {
	PTR_VALIDATE(file);
	fs_node_t * node = kopen((char *)file, 0);
	if (!node) return -ENOENT;
	close_fs(node);
	return 0;
}

static long sys_chmod(char * file, long mode) {
	PTR_VALIDATE(file);
	fs_node_t * fn = kopen(file, 0);
	if (fn) {
		/* Can group members change bits? I think it's only owners. */
		if (current_process->user != 0 && current_process->user != fn->uid) {
			close_fs(fn);
			return -EACCES;
		}
		long result = chmod_fs(fn, mode);
		close_fs(fn);
		return result;
	} else {
		return -ENOENT;
	}
}

static long sys_chown(char * file, uid_t uid, uid_t gid) {
	PTR_VALIDATE(file);
	fs_node_t * fn = kopen(file, 0);
	if (fn) {
		/* TODO: Owners can change groups... */
		if (current_process->user != 0) {
			close_fs(fn);
			return -EACCES;
		}
		long result = chown_fs(fn, uid, gid);
		close_fs(fn);
		return result;
	} else {
		return -ENOENT;
	}
}

static long sys_gettimeofday(struct timeval * tv, void * tz) {
	PTR_VALIDATE(tv);
	PTR_VALIDATE(tz);
	return gettimeofday(tv, tz);
}

static long sys_getuid(void) {
	return (long)current_process->real_user;
}

static long sys_geteuid(void) {
	return (long)current_process->user;
}

static long sys_setuid(uid_t new_uid) {
	if (current_process->user == USER_ROOT_UID) {
		current_process->user = new_uid;
		current_process->real_user = new_uid;
		return 0;
	}
	return -EPERM;
}

static long sys_getpid(void) {
	/* The user actually wants the pid of the originating thread (which can be us). */
	return current_process->group ? (long)current_process->group : (long)current_process->id;
}

static long sys_gettid(void) {
	return (long)current_process->id;
}

static long sys_setsid(void) {
	if (current_process->job == current_process->group) {
		return -EPERM;
	}
	current_process->session = current_process->group;
	current_process->job = current_process->group;
	return current_process->session;
}

static long sys_setpgid(pid_t pid, pid_t pgid) {
	/* FIXME: @ref process_from_pid */
	if (pgid < 0) {
		return -EINVAL;
	}
	process_t * proc = NULL;
	if (pid == 0) {
		proc = (process_t*)current_process;
	} else {
		/* FIXME */
		//proc = process_from_pid(pid);
	}

	if (!proc) {
		return -ESRCH;
	}
	if (proc->session != current_process->session || proc->session == proc->group) {
		return -EPERM;
	}

	if (pgid == 0) {
		proc->job = proc->group;
	} else {
		process_t * pgroup = NULL; /* FIXME process_from_pid(pgid); */

		if (!pgroup || pgroup->session != proc->session) {
			return -EPERM;
		}

		proc->job = pgid;
	}
	return 0;
}

static long sys_getpgid(pid_t pid) {
	/* FIXME: @ref process_from_pid */
	process_t * proc;
	if (pid == 0) {
		proc = (process_t*)current_process;
	} else {
		proc = NULL; /* FIXME process_from_pid(pid); */
	}

	if (!proc) {
		return -ESRCH;
	}

	return proc->job;
}

static long sys_uname(struct utsname * name) {
	PTR_VALIDATE(name);
	char version_number[256];
	snprintf(version_number, 255, __kernel_version_format,
			__kernel_version_major,
			__kernel_version_minor,
			__kernel_version_lower,
			__kernel_version_suffix);
	char version_string[256];
	snprintf(version_string, 255, "%s %s %s",
			__kernel_version_codename,
			__kernel_build_date,
			__kernel_build_time);
	strcpy(name->sysname,  __kernel_name);
	strcpy(name->nodename, hostname);
	strcpy(name->release,  version_number);
	strcpy(name->version,  version_string);
	strcpy(name->machine,  __kernel_arch);
	strcpy(name->domainname, ""); /* TODO */
	return 0;
}

static long sys_chdir(char * newdir) {
	PTR_VALIDATE(newdir);
	char * path = canonicalize_path(current_process->wd_name, newdir);
	fs_node_t * chd = kopen(path, 0);
	if (chd) {
		if ((chd->flags & FS_DIRECTORY) == 0) {
			close_fs(chd);
			return -ENOTDIR;
		}
		if (!has_permission(chd, 01)) {
			close_fs(chd);
			return -EACCES;
		}
		close_fs(chd);
		free(current_process->wd_name);
		current_process->wd_name = malloc(strlen(path) + 1);
		memcpy(current_process->wd_name, path, strlen(path) + 1);
		return 0;
	} else {
		return -ENOENT;
	}
}

static long sys_getcwd(char * buf, size_t size) {
	if (buf) {
		PTR_VALIDATE(buf);
		size_t len = strlen(current_process->wd_name) + 1;
		return (long)memcpy(buf, current_process->wd_name, size < len ? size : len);
	}
	return 0;
}

static long sys_sethostname(char * new_hostname) {
	if (current_process->user == USER_ROOT_UID) {
		PTR_VALIDATE(new_hostname);
		size_t len = strlen(new_hostname) + 1;
		if (len > 256) {
			return -ENAMETOOLONG;
		}
		hostname_len = len;
		memcpy(hostname, new_hostname, hostname_len);
		return 0;
	} else {
		return -EPERM;
	}
}

static long sys_gethostname(char * buffer) {
	PTR_VALIDATE(buffer);
	memcpy(buffer, hostname, hostname_len);
	return hostname_len;
}

static long sys_mount(char * arg, char * mountpoint, char * type, unsigned long flags, void * data) {
	/* TODO: Make use of flags and data from mount command. */
	(void)flags;
	(void)data;

	if (current_process->user != USER_ROOT_UID) {
		return -EPERM;
	}

	if (PTR_INRANGE(arg) && PTR_INRANGE(mountpoint) && PTR_INRANGE(type)) {
		return vfs_mount_type(type, arg, mountpoint);
	}

	return -EFAULT;
}

static long sys_umask(long mode) {
	current_process->mask = mode & 0777;
	return 0;
}

static long sys_unlink(char * file) {
	PTR_VALIDATE(file);
	return unlink_fs(file);
}

static long (*syscalls[])() = {
	/* System Call Table */
	[SYS_EXT]          = sys_exit,
	[SYS_GETEUID]      = sys_geteuid,
	[SYS_OPEN]         = sys_open,
	[SYS_READ]         = sys_read,
	[SYS_WRITE]        = sys_write,
	[SYS_CLOSE]        = sys_close,
	[SYS_GETTIMEOFDAY] = sys_gettimeofday,
	[SYS_GETPID]       = sys_getpid,
	[SYS_SBRK]         = sys_sbrk,
	[SYS_UNAME]        = sys_uname,
	[SYS_SEEK]         = sys_seek,
	[SYS_STAT]         = sys_stat,
	[SYS_GETUID]       = sys_getuid,
	[SYS_SETUID]       = sys_setuid,
	[SYS_READDIR]      = sys_readdir,
	[SYS_CHDIR]        = sys_chdir,
	[SYS_GETCWD]       = sys_getcwd,
	[SYS_SETHOSTNAME]  = sys_gethostname,
	[SYS_GETHOSTNAME]  = sys_sethostname,
	[SYS_MKDIR]        = sys_mkdir,
	[SYS_GETTID]       = sys_gettid,
	[SYS_SYSFUNC]      = sys_sysfunc,
	[SYS_IOCTL]        = sys_ioctl,
	[SYS_ACCESS]       = sys_access,
	[SYS_STATF]        = sys_statf,
	[SYS_CHMOD]        = sys_chmod,
	[SYS_UMASK]        = sys_umask,
	[SYS_UNLINK]       = sys_unlink,
	[SYS_MOUNT]        = sys_mount,
	[SYS_SYMLINK]      = sys_symlink,
	[SYS_READLINK]     = sys_readlink,
	[SYS_LSTAT]        = sys_lstat,
	[SYS_CHOWN]        = sys_chown,
	[SYS_SETSID]       = sys_setsid,
	[SYS_SETPGID]      = sys_setpgid,
	[SYS_GETPGID]      = sys_getpgid,

	[SYS_EXECVE]       = unimplemented,
	[SYS_FORK]         = unimplemented,
	[SYS_OPENPTY]      = unimplemented,
	[SYS_MKPIPE]       = unimplemented,
	[SYS_DUP2]         = unimplemented,
	[SYS_REBOOT]       = unimplemented,
	[SYS_CLONE]        = unimplemented,
	[SYS_SLEEPABS]     = unimplemented,
	[SYS_SLEEP]        = unimplemented,
	[SYS_YIELD]        = unimplemented,
	[SYS_SHM_OBTAIN]   = unimplemented,
	[SYS_SHM_RELEASE]  = unimplemented,
	[SYS_KILL]         = unimplemented,
	[SYS_SIGNAL]       = unimplemented,
	[SYS_WAITPID]      = unimplemented,
	[SYS_PIPE]         = unimplemented,
	[SYS_FSWAIT]       = unimplemented,
	[SYS_FSWAIT2]      = unimplemented,
	[SYS_FSWAIT3]      = unimplemented,
};

static size_t num_syscalls = sizeof(syscalls) / sizeof(*syscalls);
typedef long (*scall_func)();

void syscall_handler(struct regs * r) {

	if (arch_syscall_number(r) >= num_syscalls) {
		arch_syscall_return(r, -EINVAL);
		return;
	}

	scall_func func = syscalls[arch_syscall_number(r)];
	current_process->syscall_registers = r;
	arch_syscall_return(r, func(
		arch_syscall_arg0(r), arch_syscall_arg1(r), arch_syscall_arg2(r),
		arch_syscall_arg3(r), arch_syscall_arg4(r)));
}