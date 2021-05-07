#pragma once

#include <_cheader.h>
#include <stdint.h>
#include <stddef.h>

_Begin_C_Header

#define DECL_SYSCALL0(fn)                long syscall_##fn()
#define DECL_SYSCALL1(fn,p1)             long syscall_##fn(p1)
#define DECL_SYSCALL2(fn,p1,p2)          long syscall_##fn(p1,p2)
#define DECL_SYSCALL3(fn,p1,p2,p3)       long syscall_##fn(p1,p2,p3)
#define DECL_SYSCALL4(fn,p1,p2,p3,p4)    long syscall_##fn(p1,p2,p3,p4)
#define DECL_SYSCALL5(fn,p1,p2,p3,p4,p5) long syscall_##fn(p1,p2,p3,p4,p5)

#define DEFN_SYSCALL0(fn, num) \
	long syscall_##fn() { \
		long a = num; __asm__ __volatile__("int $0x7F" : "=a" (a) : "a" ((long)a)); \
		return a; \
	}

#define DEFN_SYSCALL1(fn, num, P1) \
	long syscall_##fn(P1 p1) { \
		long __res = num; __asm__ __volatile__("int $0x7F" \
				: "=a" (__res) \
				: "a" (__res), "b" ((long)(p1))); \
		return __res; \
	}

#define DEFN_SYSCALL2(fn, num, P1, P2) \
	long syscall_##fn(P1 p1, P2 p2) { \
		long __res = num; __asm__ __volatile__("int $0x7F" \
				: "=a" (__res) \
				: "a" (__res), "b" ((long)(p1)), "c"((long)(p2))); \
		return __res; \
	}

#define DEFN_SYSCALL3(fn, num, P1, P2, P3) \
	long syscall_##fn(P1 p1, P2 p2, P3 p3) { \
		long __res = num; __asm__ __volatile__("int $0x7F" \
				: "=a" (__res) \
				: "a" (__res), "b" ((long)(p1)), "c"((long)(p2)), "d"((long)(p3))); \
		return __res; \
	}

#define DEFN_SYSCALL4(fn, num, P1, P2, P3, P4) \
	long syscall_##fn(P1 p1, P2 p2, P3 p3, P4 p4) { \
		long __res = num; __asm__ __volatile__("int $0x7F" \
				: "=a" (__res) \
				: "a" (__res), "b" ((long)(p1)), "c"((long)(p2)), "d"((long)(p3)), "S"((long)(p4))); \
		return __res; \
	}

#define DEFN_SYSCALL5(fn, num, P1, P2, P3, P4, P5) \
	long syscall_##fn(P1 p1, P2 p2, P3 p3, P4 p4, P5 p5) { \
		long __res = num; __asm__ __volatile__("int $0x7F" \
				: "=a" (__res) \
				: "a" (__res), "b" ((long)(p1)), "c"((long)(p2)), "d"((long)(p3)), "S"((long)(p4)), "D"((long)(p5))); \
		return __res; \
	}


DECL_SYSCALL1(exit, int);
DECL_SYSCALL0(geteuid);
DECL_SYSCALL3(open, const char *, int, int);
DECL_SYSCALL3(read, int, char *, size_t);
DECL_SYSCALL3(write, int, char *, size_t);
DECL_SYSCALL1(close, int);
DECL_SYSCALL2(gettimeofday, void *, void *);
DECL_SYSCALL3(execve, char *, char **, char **);
DECL_SYSCALL0(fork);
DECL_SYSCALL0(getpid);
DECL_SYSCALL1(sbrk, int);
DECL_SYSCALL1(uname, void *);
DECL_SYSCALL5(openpty, int *, int *, char *, void *, void *);
DECL_SYSCALL3(seek, int, long, int);
DECL_SYSCALL2(stat, int, void *);
/* mkpipe removed, and there's a lot of space here */
DECL_SYSCALL2(dup2, int, int);
DECL_SYSCALL0(getuid);
DECL_SYSCALL1(setuid, unsigned int);
DECL_SYSCALL0(reboot);
DECL_SYSCALL3(readdir, int, int, void *);
DECL_SYSCALL1(chdir, char *);
DECL_SYSCALL2(getcwd, char *, size_t);
DECL_SYSCALL3(clone, uintptr_t, uintptr_t, void *);
DECL_SYSCALL1(sethostname, char *);
DECL_SYSCALL1(gethostname, char *);
DECL_SYSCALL2(mkdir, char *, unsigned int);
DECL_SYSCALL2(shm_obtain, char *, size_t *);
DECL_SYSCALL1(shm_release, char *);
DECL_SYSCALL2(kill, int, int);
DECL_SYSCALL2(signal, int, void *);
DECL_SYSCALL0(gettid);
DECL_SYSCALL0(yield);
DECL_SYSCALL2(sysfunc, int, char **);
DECL_SYSCALL2(sleepabs, unsigned long, unsigned long);
DECL_SYSCALL2(sleep, unsigned long, unsigned long);
DECL_SYSCALL3(ioctl, int, int, void *);
DECL_SYSCALL2(access, char *, int);
DECL_SYSCALL2(statf, char *, void *);
DECL_SYSCALL2(chmod, char *, int);
DECL_SYSCALL1(umask, int);
DECL_SYSCALL1(unlink, char *);
DECL_SYSCALL3(waitpid, int, int *, int);
DECL_SYSCALL1(pipe,  int *);
DECL_SYSCALL5(mount, char *, char *, char *, unsigned long, void *);
DECL_SYSCALL2(symlink, const char *, const char *);
DECL_SYSCALL3(readlink, char *, char *, int);
DECL_SYSCALL2(lstat, char *, void *);
DECL_SYSCALL2(fswait,int,int*);
DECL_SYSCALL3(fswait2,int,int*,int);
DECL_SYSCALL3(chown,char*,int,int);
DECL_SYSCALL0(setsid);
DECL_SYSCALL2(setpgid,int,int);
DECL_SYSCALL1(getpgid,int);
DECL_SYSCALL4(fswait3, int, int*, int, int*);

_End_C_Header

