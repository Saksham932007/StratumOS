/* StratumOS - the system call ABI.
 *
 * Shared verbatim between the kernel and user programs, which is the point:
 * the call numbers and error values are a contract, and a contract with two
 * copies is a contract with two versions. Nothing in here may depend on
 * kernel-internal headers, because user code compiles against it - which is
 * also why the shared structures below spell out `unsigned int` rather than
 * using the kernel's u32. user/syscall.h defines its own u32 *after*
 * including this file, so this file cannot use it.
 *
 * Calling convention: `int 0x80` with the call number in EAX and up to three
 * arguments in EBX, ECX and EDX. The result comes back in EAX. Negative
 * values are errors.
 *
 * Three arguments is a real constraint and it shaped this list. Calls that
 * would want more take a pointer to a structure instead (see
 * `struct sys_sockaddr` and SYS_SENDTO), rather than growing the convention
 * to a fourth register for the two calls that need it.
 */
#ifndef _KERNEL_SYSCALL_ABI_H
#define _KERNEL_SYSCALL_ABI_H

enum {
    SYS_EXIT = 0,
    SYS_WRITE = 1,
    SYS_GETPID = 2,
    SYS_YIELD = 3,
    SYS_SLEEP = 4,
    SYS_UPTIME = 5,
    SYS_GETKEY = 6,
    SYS_FORK = 7,
    SYS_WAIT = 8,
    SYS_GETPPID = 9,
    SYS_EXEC = 10,

    /* ---- descriptors ------------------------------------------------
     *
     * Everything below this line arrived with the descriptor table. The
     * eleven calls above predate it and keep their numbers, because a call
     * number is the one part of a kernel's interface that cannot be
     * renumbered without breaking every program already built against it.
     */
    SYS_OPEN = 11,
    SYS_CLOSE = 12,
    SYS_READ = 13,
    SYS_LSEEK = 14,
    SYS_STAT = 15,
    SYS_FSTAT = 16,
    SYS_GETDENT = 17,
    SYS_DUP = 18,
    SYS_DUP2 = 19,
    SYS_PIPE = 20,
    SYS_IOCTL = 21,

    /* ---- the filesystem, writing ------------------------------------ */
    SYS_UNLINK = 22,
    SYS_MKDIR = 23,
    SYS_RMDIR = 24,
    SYS_RENAME = 25,
    SYS_TRUNCATE = 26,
    SYS_SYNC = 27,
    SYS_CHDIR = 28,
    SYS_GETCWD = 29,

    /* ---- sockets ---------------------------------------------------- */
    SYS_SOCKET = 30,
    SYS_BIND = 31,
    SYS_LISTEN = 32,
    SYS_ACCEPT = 33,
    SYS_CONNECT = 34,
    SYS_SEND = 35,
    SYS_RECV = 36,
    SYS_SENDTO = 37,
    SYS_RECVFROM = 38,
    SYS_SHUTDOWN = 39,
    SYS_GETSOCKOPT = 40,
    SYS_SETSOCKOPT = 41,
    SYS_RESOLVE = 42,

    SYS_MAX
};

/* ---- errors -------------------------------------------------------------
 *
 * Small negative integers rather than a shared errno variable: errno is a
 * thread-local in a C library, and there is no C library here. A call that
 * fails says so in its own return value.
 */
#define SYS_EBADCALL     (-1)
#define SYS_EFAULT       (-2) /* a pointer from ring 3 was not usable       */
#define SYS_EINVAL       (-3)
#define SYS_ENOENT       (-4)
#define SYS_EBADF        (-5) /* not an open descriptor                     */
#define SYS_EMFILE       (-6) /* this process has no free descriptor        */
#define SYS_ENFILE       (-7) /* the kernel has no free open-file slot      */
#define SYS_EISDIR       (-8)
#define SYS_ENOTDIR      (-9)
#define SYS_ENOSPC       (-10)
#define SYS_EROFS        (-11) /* the filesystem, or this file, is read-only */
#define SYS_EEXIST       (-12)
#define SYS_ENOTEMPTY    (-13)
#define SYS_EIO          (-14)
#define SYS_EAGAIN       (-15) /* would block, and the caller asked not to   */
#define SYS_ENOTSUP      (-16)
#define SYS_ENOTCONN     (-17)
#define SYS_ECONNREFUSED (-18)
#define SYS_ETIMEDOUT    (-19)
#define SYS_EADDRINUSE   (-20)
#define SYS_EPIPE        (-21) /* the other end of the pipe or socket is gone */
#define SYS_ENAMETOOLONG (-22)
#define SYS_ECONNRESET   (-23)
#define SYS_EISCONN      (-24)
#define SYS_EHOSTUNREACH (-25)

/* The longest program name or path a call will look at. A path arrives from
 * ring 3, so the bound is what stops a crafted string from being walked
 * forever; SYS_NAME_MAX stays as it was because exec() still accepts a bare
 * program name as well as a path. */
#define SYS_NAME_MAX     32
#define SYS_PATH_MAX     128

/* ---- open() ------------------------------------------------------------- */

#define O_RDONLY         0x0000
#define O_WRONLY         0x0001
#define O_RDWR           0x0002
#define O_ACCMODE        0x0003
#define O_CREAT          0x0100
#define O_TRUNC          0x0200
#define O_APPEND         0x0400
#define O_EXCL           0x0800
#define O_DIRECTORY      0x1000
#define O_NONBLOCK       0x2000

/* ---- lseek() ------------------------------------------------------------ */

#define SEEK_SET         0
#define SEEK_CUR         1
#define SEEK_END         2

/* ---- what a descriptor refers to ---------------------------------------
 *
 * Returned in `struct sys_stat.type`. A program that has to care which kind
 * of thing it is holding - a shell deciding whether to recurse into a name -
 * asks for this rather than inferring it from whether a call failed.
 */
#define S_TYPE_FILE      1
#define S_TYPE_DIR       2
#define S_TYPE_CHAR      3 /* a character device: the console, /dev/null      */
#define S_TYPE_PIPE      4
#define S_TYPE_SOCKET    5

#define S_MODE_READ      0x1
#define S_MODE_WRITE     0x2

/* The three descriptors a program is started with. Not a convention this
 * kernel merely honours - the loader really does open them before the entry
 * point runs, so a program that writes to 1 without opening anything works
 * for the same reason it does anywhere else. */
#define STDIN_FD         0
#define STDOUT_FD        1
#define STDERR_FD        2

/* Per-process descriptor table size. Fixed rather than grown, like TASK_MAX:
 * a bound that a ring-3 loop cannot push past is worth more here than the
 * ability to hold a thousand files open. */
#define SYS_FDS_MAX      32

struct sys_stat {
    unsigned int size;   /* bytes; 0 for a directory or a device       */
    unsigned int type;   /* one of S_TYPE_*                            */
    unsigned int mode;   /* S_MODE_READ | S_MODE_WRITE                 */
    unsigned int blocks; /* allocation units the backing store charges */
};

#define SYS_DIRENT_NAME_MAX 13 /* "FILENAME.EXT" and a terminator */

struct sys_dirent {
    char name[SYS_DIRENT_NAME_MAX];
    unsigned char type; /* S_TYPE_FILE or S_TYPE_DIR */
    unsigned int size;
};

/* ---- ioctl() -----------------------------------------------------------
 *
 * Deliberately tiny. The console needs two things a read/write interface
 * cannot express, and nothing else here needs any.
 */
#define TIOCNREAD   1 /* how many bytes could be read without blocking   */
#define TIOCSECHO   2 /* turn input echo on (arg != 0) or off            */

/* ---- sockets -----------------------------------------------------------
 *
 * One address family and two socket types, which is what the stack under it
 * has. The constants match the Berkeley ones in value as well as in name,
 * not for binary compatibility - there is none - but because a programmer
 * reading `AF_INET` should not have to check whether this kernel made up a
 * different number for it.
 */
#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

/* Fixed-layout, 16 bytes, mirroring sockaddr_in. `port` and `addr` are in
 * network byte order, as they are everywhere else, because a conversion that
 * happens in only some of the places is worse than one that happens in all
 * of them. */
struct sys_sockaddr {
    unsigned short family; /* AF_INET                       */
    unsigned short port;   /* network byte order            */
    unsigned int addr;     /* IPv4, network byte order      */
    unsigned char pad[8];
};

/* SYS_SENDTO and SYS_RECVFROM want five arguments and there are three
 * registers, so they take one of these instead. */
struct sys_msg {
    unsigned int buf; /* a user pointer, as an integer       */
    unsigned int len;
    unsigned int addr; /* a user pointer to a sys_sockaddr    */
    unsigned int flags;
};

/* setsockopt/getsockopt levels and names. */
#define SOL_SOCKET   1
#define SO_RCVTIMEO  1
#define SO_SNDTIMEO  2
#define SO_REUSEADDR 3
#define SO_ERROR     4

#define SHUT_RD      0
#define SHUT_WR      1
#define SHUT_RDWR    2

/* The vector the gate is installed on. */
#define INT_SYSCALL  0x80

#endif /* _KERNEL_SYSCALL_ABI_H */
