/* Host-side check harness support, not console code: the minimum a KOS newlib (no syscalls, no malloc, no
 * locks; KOS supplies those) needs to run a gameplay program built by sh-elf-gcc in the GDB SH simulator
 * (sh-elf-run). System calls use the simulator's trapa #34 interface (r4 = call, r5..r7 = arguments, r0 =
 * result, r1 = errno). Memory: a bump allocator between the image end and the stack (free does nothing).
 * The start file (crt1.o) calls __setup_argv_and_call_main, which fetches argv from the simulator.
 * tools/sh4_gameplay_sim.py builds and runs it. */
#include <errno.h>
#include <reent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

enum { SYS_exit=1, SYS_read=3, SYS_write=4, SYS_open=5, SYS_close=6, SYS_lseek=19, SYS_argc=172, SYS_argnlen=173, SYS_argn=174 };

static long trap34(long call,long a,long b,long c,int *err) {
    register long r4 __asm__("r4")=call;
    register long r5 __asm__("r5")=a;
    register long r6 __asm__("r6")=b;
    register long r7 __asm__("r7")=c;
    register long r0 __asm__("r0");
    register long r1 __asm__("r1");
    __asm__ volatile("trapa #34" : "=r"(r0),"=r"(r1) : "r"(r4),"r"(r5),"r"(r6),"r"(r7) : "memory");
    if(err) *err=(int)r1;
    return r0;
}

void _exit(int code) { for(;;) trap34(SYS_exit,code,0,0,NULL); }

static long sys_errno(struct _reent *r,long result,int err) { if(result<0 && r) r->_errno=err ? err : EIO; return result; }
_ssize_t _read_r(struct _reent *r,int fd,void *buf,size_t n) { int e;const long v=trap34(SYS_read,fd,(long)buf,(long)n,&e);return sys_errno(r,v,e); }
_ssize_t _write_r(struct _reent *r,int fd,const void *buf,size_t n) { int e;const long v=trap34(SYS_write,fd,(long)buf,(long)n,&e);return sys_errno(r,v,e); }
int _open_r(struct _reent *r,const char *path,int flags,int mode) { int e;const long v=trap34(SYS_open,(long)path,flags,mode,&e);return (int)sys_errno(r,v,e); }
int _close_r(struct _reent *r,int fd) { int e;const long v=trap34(SYS_close,fd,0,0,&e);return (int)sys_errno(r,v,e); }
_off_t _lseek_r(struct _reent *r,int fd,_off_t off,int whence) { int e;const long v=trap34(SYS_lseek,fd,off,whence,&e);return sys_errno(r,v,e); }
int _fstat_r(struct _reent *r,int fd,struct stat *st) { (void)r;memset(st,0,sizeof(*st));st->st_mode=fd<3 ? S_IFCHR : S_IFREG;st->st_blksize=65536;return 0; }
int _isatty_r(struct _reent *r,int fd) { (void)r;return fd<3; }
int _kill_r(struct _reent *r,int pid,int sig) { (void)pid;(void)sig;r->_errno=EINVAL;return -1; }
int _getpid_r(struct _reent *r) { (void)r;return 1; }
int _unlink_r(struct _reent *r,const char *path) { (void)path;r->_errno=EIO;return -1; }
int _link_r(struct _reent *r,const char *a,const char *b) { (void)a;(void)b;r->_errno=EIO;return -1; }

/* Locks: one thread. KOS's sys/lock.h declares these with its own types; asm names avoid the clash. */
static void lock_nothing(void *l) { (void)l; }
void shim_lock_init(void *l) __asm__("___newlib_lock_init_recursive");
void shim_lock_close(void *l) __asm__("___newlib_lock_close_recursive");
void shim_lock_acquire(void *l) __asm__("___newlib_lock_acquire_recursive");
void shim_lock_release(void *l) __asm__("___newlib_lock_release_recursive");
void shim_lock_init(void *l) { lock_nothing(l); }
void shim_lock_close(void *l) { lock_nothing(l); }
void shim_lock_acquire(void *l) { lock_nothing(l); }
void shim_lock_release(void *l) { lock_nothing(l); }

void __assert_func(const char *file,int line,const char *func,const char *expr) {
    fprintf(stderr,"assertion \"%s\" failed: file \"%s\", line %d, function %s\n",expr,file,line,func ? func : "?");
    fflush(NULL);_exit(134);
}

/* KOS's assert() calls __assert (kos/include/assert.h). */
void shim_kos_assert(const char *file,int line,const char *expr,const char *msg,const char *func) __asm__("___assert");
void shim_kos_assert(const char *file,int line,const char *expr,const char *msg,const char *func) {
    fprintf(stderr,"assertion \"%s\" failed: file \"%s\", line %d, function %s%s%s\n",expr,file,line,func ? func : "?",
        msg ? ": " : "",msg ? msg : "");
    fflush(NULL);_exit(134);
}

/* Bump allocator: blocks carry their size in front (for realloc). */
extern char end[];
static uintptr_t heap_next,heap_peak;
enum { HEAP_LIMIT=0x07000000 }; /* the stack is at 0x07fffff0 (--defsym), memory 128 MiB */
static void *bump(size_t n,size_t align) {
    if(!heap_next) heap_next=(uintptr_t)end;
    uintptr_t at=(heap_next+8+align-1)&~(uintptr_t)(align-1);
    if(at+n>HEAP_LIMIT || n>HEAP_LIMIT) return NULL;
    ((size_t *)at)[-1]=n;heap_next=at+n;if(heap_next>heap_peak) heap_peak=heap_next;
    return (void *)at;
}
void *_malloc_r(struct _reent *r,size_t n) { void *p=bump(n ? n : 1,16);if(!p && r) r->_errno=ENOMEM;return p; }
void _free_r(struct _reent *r,void *p) { (void)r;(void)p; }
void *_calloc_r(struct _reent *r,size_t a,size_t b) {
    if(b && a>SIZE_MAX/b) return NULL;
    void *p=_malloc_r(r,a*b);if(p) memset(p,0,a*b);return p;
}
void *_realloc_r(struct _reent *r,void *p,size_t n) {
    if(!p) return _malloc_r(r,n);
    const size_t old=((size_t *)p)[-1];
    if(n<=old) return p;
    void *q=_malloc_r(r,n);if(q) memcpy(q,p,old);return q;
}
void *_memalign_r(struct _reent *r,size_t align,size_t n) { (void)r;return bump(n,align<16 ? 16 : align); }
void *malloc(size_t n) { return _malloc_r(_REENT,n); }
void free(void *p) { (void)p; }
void *calloc(size_t a,size_t b) { return _calloc_r(_REENT,a,b); }
void *realloc(void *p,size_t n) { return _realloc_r(_REENT,p,n); }
void *memalign(size_t a,size_t n) { return _memalign_r(_REENT,a,n); }
int posix_memalign(void **out,size_t a,size_t n) { void *p=memalign(a,n);if(!p) return ENOMEM;*out=p;return 0; }
void *aligned_alloc(size_t a,size_t n) { return memalign(a,n); }
void *_sbrk_r(struct _reent *r,ptrdiff_t n) { (void)n;r->_errno=ENOMEM;return (void *)-1; }

int main(int argc,char **argv);
int __setup_argv_and_call_main(void);
int __setup_argv_and_call_main(void) {
    static char *argv[32];static char text[4096];
    const long argc=trap34(SYS_argc,0,0,0,NULL);
    size_t used=0;int n=0;
    for(long i=0;i<argc && i<31;++i) {
        const long len=trap34(SYS_argnlen,i,0,0,NULL);
        if(len<0 || used+(size_t)len+1>sizeof(text)) break;
        trap34(SYS_argn,i,(long)(text+used),0,NULL);
        argv[n++]=text+used;used+=(size_t)len+1;
    }
    argv[n]=NULL;
    const int code=main(n,argv);
    fflush(NULL);
    fprintf(stderr,"sh4sim: heap peak %lu bytes\n",(unsigned long)(heap_peak-(uintptr_t)end));
    return code;
}
