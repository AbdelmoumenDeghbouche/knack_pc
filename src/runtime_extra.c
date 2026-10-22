/* System functions added for MediEvil (CUSA11227, Unreal Engine 4), with the same offline,
 * single-user contracts as runtime_services.c:
 *   - kernel event flags (full implementation), small kernel/libc queries;
 *   - POSIX names for rwlocks and scheduling;
 *   - sockets unavailable (network unplugged);
 *   - NGS2 renders silence (no voices play yet).
 * Every entry is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#define _CRT_RAND_S /* rand_s */
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#endif

#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define E_SRCH 3
#define E_BADF 9
#define E_NOMEM 12
#define E_BUSY 16
#define E_INVAL 22
#define E_NETDOWN 50
#define E_TIMEDOUT 60
#define E_CANCELED 85

/* ---- Event flags ---- */
#define EF_MAGIC 0x45464c47u
#define EF_ATTR_MULTI 0x20
#define EF_WAIT_AND 0x01
#define EF_WAIT_OR 0x02
#define EF_CLEAR_ALL 0x10
#define EF_CLEAR_PAT 0x20
typedef struct EventFlag {
    uint32_t magic, attr;
    uint64_t bits;
    unsigned waiters;
    uint64_t generation; /* bumped by cancel/delete so waiters can tell */
    int deleted;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    char name[32];
} EventFlag;
static size_t flags_created;

static EventFlag *flag(void *handle) {
    EventFlag *f=handle;
    return f && f->magic==EF_MAGIC && !f->deleted ? f : NULL;
}
static ABI int32_t ef_create(EventFlag **out, const char *name, uint32_t attr, uint64_t init, const void *param) {
    (void)param;
    if (!out) return ERR(E_INVAL);
    EventFlag *f=calloc(1,sizeof(*f));
    if (!f) return ERR(E_NOMEM);
    f->magic=EF_MAGIC; f->attr=attr; f->bits=init;
    pthread_mutex_init(&f->lock,NULL); pthread_cond_init(&f->cond,NULL);
    if (name) snprintf(f->name,sizeof(f->name),"%s",name);
    *out=f; ++flags_created;
    return 0;
}
static ABI int32_t ef_delete(EventFlag *handle) {
    EventFlag *f=flag(handle);
    if (!f) return ERR(E_SRCH);
    pthread_mutex_lock(&f->lock);
    f->deleted=1; ++f->generation;
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->lock);
    /* Freed lazily: a waiter may still hold the lock; the leak is bounded by deletions. */
    return 0;
}
static ABI int32_t ef_set(EventFlag *handle, uint64_t bits) {
    EventFlag *f=flag(handle);
    if (!f) return ERR(E_SRCH);
    pthread_mutex_lock(&f->lock);
    f->bits|=bits;
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->lock);
    return 0;
}
static ABI int32_t ef_clear(EventFlag *handle, uint64_t bits) {
    EventFlag *f=flag(handle);
    if (!f) return ERR(E_SRCH);
    pthread_mutex_lock(&f->lock);
    f->bits&=bits; /* PS4 contract: the argument is the mask of bits to keep */
    pthread_mutex_unlock(&f->lock);
    return 0;
}
static int ef_match(EventFlag *f, uint64_t bits, uint32_t mode) {
    return (mode & EF_WAIT_AND) ? (f->bits & bits)==bits : (f->bits & bits)!=0;
}
static void ef_consume(EventFlag *f, uint64_t bits, uint32_t mode, uint64_t *result) {
    if (result) *result=f->bits;
    if (mode & EF_CLEAR_ALL) f->bits=0;
    else if (mode & EF_CLEAR_PAT) f->bits&=~bits;
}
static int ef_valid_mode(uint32_t mode) {
    uint32_t wait=mode & 3, clear=mode & 0x30;
    return (wait==EF_WAIT_AND || wait==EF_WAIT_OR) && clear!=0x30 && !(mode & ~0x33u);
}
static ABI int32_t ef_poll(EventFlag *handle, uint64_t bits, uint32_t mode, uint64_t *result) {
    EventFlag *f=flag(handle);
    if (!f) return ERR(E_SRCH);
    if (!bits || !ef_valid_mode(mode)) return ERR(E_INVAL);
    pthread_mutex_lock(&f->lock);
    int32_t r=0;
    if (!(f->attr & EF_ATTR_MULTI) && f->waiters) r=ERR(1); /* EPERM: single-waiter flag in use */
    else if (ef_match(f,bits,mode)) ef_consume(f,bits,mode,result);
    else { if (result) *result=f->bits; r=ERR(E_BUSY); }
    pthread_mutex_unlock(&f->lock);
    return r;
}
static ABI int32_t ef_wait(EventFlag *handle, uint64_t bits, uint32_t mode, uint64_t *result, uint32_t *timeout_us) {
    EventFlag *f=flag(handle);
    if (!f) return ERR(E_SRCH);
    if (!bits || !ef_valid_mode(mode)) return ERR(E_INVAL);
    pthread_mutex_lock(&f->lock);
    if (!(f->attr & EF_ATTR_MULTI) && f->waiters) { pthread_mutex_unlock(&f->lock); return ERR(1); }
    struct timespec deadline;
    if (timeout_us) {
        clock_gettime(CLOCK_REALTIME,&deadline);
        uint64_t ns=(uint64_t)deadline.tv_nsec+(uint64_t)*timeout_us*1000u;
        deadline.tv_sec+=(time_t)(ns/1000000000u); deadline.tv_nsec=(long)(ns%1000000000u);
    }
    uint64_t generation=f->generation;
    int32_t r=0;
    ++f->waiters;
    while (!ef_match(f,bits,mode)) {
        if (f->generation!=generation) { r=f->deleted ? ERR(E_SRCH) : ERR(E_CANCELED); break; }
        if (timeout_us) {
            if (pthread_cond_timedwait(&f->cond,&f->lock,&deadline)==ETIMEDOUT && !ef_match(f,bits,mode)) {
                r=ERR(E_TIMEDOUT); *timeout_us=0; break;
            }
        } else pthread_cond_wait(&f->cond,&f->lock);
    }
    --f->waiters;
    if (!r) ef_consume(f,bits,mode,result);
    else if (result) *result=f->bits;
    pthread_mutex_unlock(&f->lock);
    return r;
}
static ABI int32_t ef_cancel(EventFlag *handle, uint64_t bits, int32_t *waiters) {
    EventFlag *f=flag(handle);
    if (!f) return ERR(E_SRCH);
    pthread_mutex_lock(&f->lock);
    if (waiters) *waiters=(int32_t)f->waiters;
    f->bits=bits; ++f->generation;
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->lock);
    return 0;
}

/* ---- Small kernel and libc queries ---- */
/* Base PS4, or PS4 Pro with GOW3_NEO=1 (games then pick their Pro render path and resolution). */
static ABI int32_t is_neo_mode(void) {
    static int neo=-1;
    if (neo<0) { const char *v=getenv("GOW3_NEO"); neo=v && *v=='1'; if (neo) puts("Runtime: reporting a PS4 Pro (Neo mode)"); }
    return neo;
}
static ABI int32_t get_cpumode(void) { return 0; }        /* SCE_KERNEL_CPUMODE_6CPU */
static ABI int32_t thread_id(void) {
#ifdef _WIN32
    return (int32_t)GetCurrentThreadId();
#else
    return (int32_t)syscall(SYS_gettid);
#endif
}
static ABI int32_t thread_name(void *thread, char *name) {
    if (!name) return ERR(E_INVAL);
    const char *n=runtime_thread_name(thread);
    if (!n) return ERR(E_SRCH);
    snprintf(name,32,"%s",n);
    return 0;
}
static ABI int32_t ok_noop(void) { return 0; }
static ABI void *null_pointer(void) { return NULL; }
static ABI int32_t not_found(void) { return ERR(E_SRCH); }
/* f(id, int *out): *out = 0 (e.g. Remote Play connection status DISCONNECT). */
static ABI int32_t out2_zero(int32_t id, int32_t *out) { (void)id; if (out) *out=0; return 0; }
/* libc's _malloc_init_lv2 ignores the result and writes through both pointers (WipEout's
 * libc), so they must point at real storage. Layout as shadPS4's HeapInfoInfo. */
typedef struct { uint64_t size; uint32_t flag, get_segment_info; uint64_t *id_mask, *mstate_table; } HeapTraceInfo;
static uint64_t heap_id_mask, heap_mstate_table[64];
/* libSceDiscMap Func_7C980FFB0AA27E7A (Fios2 asks where a byte range lives): everything is
 * installed, no flags, as in shadPS4. */
static ABI int32_t disc_map_query(const char *path, int64_t offset, int64_t bytes, int32_t *flags, int32_t *a, int32_t *b) {
    (void)path; (void)offset; (void)bytes;
    if (flags) *flags=0;
    if (a) *a=0;
    if (b) *b=0;
    return 0;
}
static ABI int32_t heap_trace_info(HeapTraceInfo *info) {
    if (!info) return ERR(E_INVAL);
    info->get_segment_info = 0;
    info->id_mask = &heap_id_mask;
    info->mstate_table = heap_mstate_table;
    return 0;
}
static ABI int32_t kernel_chmod(const char *path, uint16_t mode) { (void)path; (void)mode; return 0; }
static ABI int32_t kernel_utimes(const char *path, const void *times) { (void)path; (void)times; return 0; }
static ABI int32_t kernel_getdents(int fd, char *buffer, uint64_t size) {
    int64_t r=runtime_file_getdents(fd,buffer,size,NULL);
    return r<0 ? ERR(runtime_guest_errno((int)-r)) : (int32_t)r;
}
static ABI int32_t prt_aperture(int index, void **address, uint64_t *size) {
    if (index<0 || index>2 || !address || !size) return ERR(E_INVAL);
    *address=NULL; *size=0;
    return 0;
}
static ABI int32_t kernel_nanosleep(const struct timespec *request, struct timespec *remain) {
    if (!request) return ERR(E_INVAL);
#ifdef _WIN32
    compat_sleep_ns((uint64_t)request->tv_sec*1000000000u+(uint64_t)request->tv_nsec);
#else
    struct timespec pending=*request;
    while (nanosleep(&pending,&pending)) if (errno!=EINTR) break;
#endif
    if (remain) remain->tv_sec=remain->tv_nsec=0;
    return 0;
}
/* sceKernelConvert{Localtime,Utc}...(time_t, ..., SceKernelTimezone*, int *dst) */
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static int32_t host_offset_seconds(time_t t, int *dst) {
    struct tm local, utc;
#ifdef _WIN32
    localtime_s(&local,&t); gmtime_s(&utc,&t);
#else
    localtime_r(&t,&local); gmtime_r(&t,&utc);
#endif
    if (dst) *dst=local.tm_isdst>0;
    utc.tm_isdst=local.tm_isdst;
    return (int32_t)difftime(mktime(&local),mktime(&utc));
}
static ABI int32_t local_to_utc(int64_t local, uint64_t unused, int64_t *utc, GuestTimezone *tz, int32_t *dst) {
    (void)unused;
    if (!utc) return ERR(E_INVAL);
    int d=0; int32_t off=host_offset_seconds((time_t)local,&d);
    *utc=local-off;
    if (tz) { tz->minuteswest=-off/60; tz->dsttime=d; }
    if (dst) *dst=d ? 3600 : 0;
    return 0;
}
static ABI int32_t utc_to_local(int64_t utc, int64_t *local, GuestTimezone *tz, int32_t *dst) {
    if (!local) return ERR(E_INVAL);
    int d=0; int32_t off=host_offset_seconds((time_t)utc,&d);
    *local=utc+off;
    if (tz) { tz->minuteswest=-off/60; tz->dsttime=d; }
    if (dst) *dst=d ? 3600 : 0;
    return 0;
}
static ABI uint64_t available_flexible(uint64_t *out) {
    if (!out) return (uint64_t)ERR(E_INVAL);
    *out=runtime_memory_flexible_available();
    return 0;
}
static ABI int32_t random_number(void *buffer, uint64_t size) {
    if (!buffer || size>64) return (int32_t)0x817C0016; /* SCE_RANDOM_ERROR_INVALID */
    unsigned char *p=buffer;
    for (uint64_t i=0;i<size;++i) {
#ifdef _WIN32
        unsigned v; rand_s(&v); p[i]=(unsigned char)v;
#else
        p[i]=(unsigned char)random();
#endif
    }
    return 0;
}
/* ---- PlayStation VR headset: the library works, no headset is detected (as shadPS4) ---- */
#define HMD_NOT_INITIALIZED ((int32_t)0x81110002)
#define HMD_HANDLE_INVALID ((int32_t)0x81110003)
#define HMD_ALREADY_OPENED ((int32_t)0x81110006)
#define HMD_PARAMETER_NULL ((int32_t)0x81110008)
#define HMD_HANDLE 0xf000000 /* positive, distinct from other VrTracker devices */
static int hmd_initialized;
static int32_t hmd_handle, hmd_user=-1;
static ABI int32_t hmd_initialize(const void *param) {
    if (!param) return HMD_PARAMETER_NULL;
    hmd_initialized=1;
    return 0;
}
static ABI int32_t hmd_terminate(void) {
    if (!hmd_initialized) return HMD_NOT_INITIALIZED;
    hmd_initialized=0;
    return 0;
}
static ABI int32_t hmd_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)type; (void)index; (void)param;
    if (!hmd_initialized) return HMD_NOT_INITIALIZED;
    if (hmd_handle) return HMD_ALREADY_OPENED;
    hmd_user=user;
    return hmd_handle=HMD_HANDLE;
}
static ABI int32_t hmd_close(int32_t handle) {
    if (!hmd_initialized) return HMD_NOT_INITIALIZED;
    if (handle!=hmd_handle) return HMD_HANDLE_INVALID;
    hmd_handle=0; hmd_user=-1;
    return 0;
}
/* OrbisHmdDeviceInformation (32 bytes): status NOT_DETECTED (2), user id. */
static ABI int32_t hmd_device_info(uint32_t *info) {
    if (!info) return HMD_PARAMETER_NULL;
    if (!hmd_initialized) return HMD_NOT_INITIALIZED;
    memset(info,0,32);
    info[0]=2; info[1]=(uint32_t)hmd_user;
    return 0;
}
/* SDK 4.50+ libraries report a missing headset as an invalid handle. */
static ABI int32_t hmd_by_handle(void) { return HMD_HANDLE_INVALID; }
static ABI int32_t hmd_garlic_size(void) { return 0x100000; }
static ABI int32_t hmd_onion_size(void) { return 0x810; }
static ABI int32_t hmd_buffer_align(void) { return 0x100; }
/* Setup dialog: finishes at once, as if the player pressed Circle (USER_CANCELED). */
static ABI int32_t dialog_finished(void) { return 3; }
static ABI int32_t hmd_dialog_result(int32_t *result) { if (result) *result=1; return 0; }

/* sceHttpsGetCaList: an empty certificate list (OrbisSslCaList {certs, certsNum}). */
typedef struct { void *certs; int32_t count; } CaList;
static ABI int32_t https_ca_list(int32_t context, CaList *list) {
    (void)context;
    if (!list) return (int32_t)0x804311FE; /* SCE_HTTP_ERROR_INVALID_VALUE */
    list->certs=NULL; list->count=0;
    return 0;
}

static ABI int32_t no_entitlement(void) { return (int32_t)0x80D90007; } /* APP_CONTENT DRM_NO_ENTITLEMENT */

/* ---- PlayStation Camera: none attached; nothing can be opened ---- */
static ABI int32_t camera_attached(int32_t index) { return index ? (int32_t)0x802E0000 : 0; }
static ABI int32_t camera_not_connected(void) { return (int32_t)0x802E0010; }
static ABI int32_t camera_not_open(void) { return (int32_t)0x802E0004; }

/* sceKernelUuidCreate: 16 random bytes, version 4. */
static ABI int32_t uuid_create(unsigned char *uuid) {
    if (!uuid) return ERR(E_INVAL);
    random_number(uuid,16);
    uuid[6]=(unsigned char)((uuid[6]&0x0f)|0x40);
    uuid[8]=(unsigned char)((uuid[8]&0x3f)|0x80);
    return 0;
}

/* ---- POSIX rwlocks and scheduling (errno results, not SCE codes) ---- */
typedef int32_t (ABI *RwInit)(void **, void *const *, const char *);
typedef int32_t (ABI *RwOne)(void **);
static int32_t posix_code(int32_t r) { return r ? (int32_t)((uint32_t)r & 0xffff) : 0; }
static ABI int32_t posix_rw_init(void **rw, void *const *attr) {
    return posix_code(((RwInit)runtime_rwlock_resolve("6ULAa0fq4jA#libkernel"))(rw,attr,NULL));
}
#define POSIX_RW(name,nid) static ABI int32_t posix_rw_##name(void **rw) { \
    return posix_code(((RwOne)runtime_rwlock_resolve(nid))(rw)); }
POSIX_RW(destroy,"BB+kb08Tl9A#libkernel")
POSIX_RW(rdlock,"Ox9i0c7L5w0#libkernel")
POSIX_RW(wrlock,"mqdNorrB+gI#libkernel")
POSIX_RW(unlock,"+L98PIbGttk#libkernel")
typedef struct { int32_t priority; } GuestSchedParam;
static ABI int32_t posix_getschedparam(void *thread, int32_t *policy, GuestSchedParam *param) {
    (void)thread;
    if (policy) *policy=1; /* SCHED_FIFO, as the PS4 default */
    if (param) param->priority=700;
    return 0;
}
static ABI int32_t posix_setschedparam(void *thread, int32_t policy, const GuestSchedParam *param) {
    (void)thread; (void)policy; (void)param; return 0; /* host scheduler decides */
}
static ABI int32_t sched_max(int32_t policy) { (void)policy; return 767; }
static ABI int32_t sched_min(int32_t policy) { (void)policy; return 256; }
static ABI int32_t mutexattr_setprotocol(void *attr, int32_t protocol) { (void)attr; (void)protocol; return 0; }

/* ---- Sockets: network cable unplugged ---- */
static ABI int32_t socket_none(void) { *runtime_errno()=E_NETDOWN; return -1; }
static ABI int32_t socket_badf(void) { *runtime_errno()=E_BADF; return -1; }

/* ---- Coredump, PlayGo, AppContent ---- */
static ABI int32_t playgo_eta(int32_t handle, const uint16_t *chunks, uint32_t count, int64_t *eta) {
    (void)handle; (void)chunks; (void)count;
    if (!eta) return (int32_t)0x80B2000A;
    *eta=0; return 0;
}
static ABI int32_t playgo_todo(int32_t handle, void *list, uint32_t count, uint32_t *entries) {
    (void)handle; (void)list; (void)count;
    if (!entries) return (int32_t)0x80B2000A;
    *entries=0; return 0; /* whole package installed */
}
typedef struct { char data[16]; } MountPoint;
static ABI int32_t temp_mount2(uint32_t option, MountPoint *mount) {
    (void)option;
    if (!mount) return (int32_t)0x80D90002; /* SCE_APP_CONTENT_ERROR_PARAMETER */
    char host[1024];
    snprintf(host,sizeof(host),"%s/temp0",runtime_file_user_dir());
    mkdir(host,0755);
    if (runtime_file_mount("/temp0",host)) return (int32_t)0x80D90007; /* BUSY */
    memset(mount,0,sizeof(*mount));
    strcpy(mount->data,"/temp0");
    printf("Runtime: temporary data mounted at /temp0 -> %s\n",host);
    return 0;
}
static ABI int32_t temp_space(const MountPoint *mount, uint64_t *kb) {
    if (!mount || !kb) return (int32_t)0x80D90002;
    *kb=1048576; return 0;
}

/* ---- NGS2: silent renderer. Handles are distinct non-null tokens; voices never play. ---- */
static uintptr_t ngs2_next=0x1000;
static pthread_mutex_t ngs2_lock=PTHREAD_MUTEX_INITIALIZER;
static size_t ngs2_renders;
static uintptr_t ngs2_token(void) {
    pthread_mutex_lock(&ngs2_lock);
    uintptr_t t=(ngs2_next+=0x10);
    pthread_mutex_unlock(&ngs2_lock);
    return t;
}
static ABI int32_t ngs2_system_create(const void *option, const void *allocator, uintptr_t *out) {
    (void)option; (void)allocator;
    if (!out) return (int32_t)0x804A0003;
    *out=ngs2_token();
    puts("Runtime: NGS2 system created (silent renderer)");
    return 0;
}
static ABI int32_t ngs2_rack_create(uintptr_t system, uint32_t rack, const void *option, const void *allocator, uintptr_t *out) {
    (void)system; (void)rack; (void)option; (void)allocator;
    if (!out) return (int32_t)0x804A0003;
    *out=ngs2_token(); return 0;
}
/* Knack: racks in a buffer the game allocates (OrbisNgs2ContextBufferInfo: host buffer, size, ...).
 * The silent renderer keeps no state there; one page is asked for. */
typedef struct { void *buffer; uint64_t size; uintptr_t reserved[5], user; } Ngs2BufferInfo;
static ABI int32_t ngs2_rack_buffer_size(uint32_t rack, const void *option, Ngs2BufferInfo *info) {
    (void)rack; (void)option;
    if (!info) return (int32_t)0x804A0003;
    info->size=0x1000; return 0;
}
static ABI int32_t ngs2_rack_create_buffer(uintptr_t system, uint32_t rack, const void *option, const Ngs2BufferInfo *info, uintptr_t *out) {
    (void)info;
    return ngs2_rack_create(system,rack,option,NULL,out);
}
static ABI int32_t ngs2_voice_handle(uintptr_t rack, uint32_t index, uintptr_t *out) {
    if (!out) return (int32_t)0x804A0003;
    *out=rack+0x100000+(uintptr_t)index*0x10; return 0;
}
static ABI int32_t ngs2_voice_state(uintptr_t voice, void *state, uint64_t size) {
    (void)voice;
    if (state && size) memset(state,0,size); /* stopped, not in use */
    return 0;
}
static ABI int32_t ngs2_voice_flags(uintptr_t voice, uint32_t *flags) {
    (void)voice;
    if (flags) *flags=0;
    return 0;
}
typedef struct { void *buffer; uint64_t size; uint32_t waveform, channels; } Ngs2RenderBuffer;
static ABI int32_t ngs2_render(uintptr_t system, Ngs2RenderBuffer *buffers, uint32_t count) {
    (void)system;
    for (uint32_t i=0;buffers && i<count;++i) if (buffers[i].buffer) memset(buffers[i].buffer,0,buffers[i].size);
    ++ngs2_renders;
    return 0;
}
static ABI int32_t ngs2_unsupported_waveform(void) { return (int32_t)0x804A0200; } /* invalid waveform */

/* ---- PngDec (Knack): FFmpeg decodes (gow3gpu_png_decode); the header is read here. The handle
 * is the game's work memory holding the create parameters. 8-bit RGBA/BGRA output only. ---- */
#define PNG_INVALID_ADDR ((int32_t)0x80690001)
#define PNG_INVALID_SIZE ((int32_t)0x80690002)
#define PNG_INVALID_PARAM ((int32_t)0x80690003)
#define PNG_INVALID_HANDLE ((int32_t)0x80690004)
#define PNG_INVALID_DATA ((int32_t)0x80690010)
#define PNG_DECODE_ERROR ((int32_t)0x80690012)
#define PNG_MAGIC 0x504e4744u
typedef struct { uint32_t this_size, attribute, max_width; } PngCreateParam;
typedef struct { uint32_t magic, attribute, max_width; } PngState;
typedef struct { const unsigned char *png; uint32_t size, reserved; } PngParseParam;
typedef struct { uint32_t width, height; uint16_t color_space, bit_depth; uint32_t flags; } PngInfo;
typedef struct { const unsigned char *png; unsigned char *image; uint32_t png_size, image_size;
                 uint16_t pixel_format, alpha; uint32_t pitch; } PngDecodeParam;
static uint32_t big32(const unsigned char *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
/* IHDR, and a tRNS chunk before IDAT. Color space: 2 gray, 3 RGB, 4 palette, 18 gray+alpha, 19 RGBA. */
static int32_t png_header(const unsigned char *png, uint32_t size, PngInfo *info) {
    static const unsigned char signature[8]={0x89,'P','N','G','\r','\n',0x1a,'\n'};
    if (!png || size<33 || memcmp(png,signature,8) || memcmp(png+12,"IHDR",4)) return PNG_INVALID_DATA;
    static const uint16_t spaces[7]={2,0,3,4,18,0,19};
    unsigned type=png[25];
    if (type>6 || !spaces[type]) return PNG_INVALID_DATA;
    info->width=big32(png+16); info->height=big32(png+20);
    info->bit_depth=png[24]; info->color_space=spaces[type];
    info->flags=png[28]==1; /* Adam7 */
    for (uint32_t at=8; at+12<=size;) {
        uint32_t length=big32(png+at);
        if (!memcmp(png+at+4,"IDAT",4) || length>size-at-12) break;
        if (!memcmp(png+at+4,"tRNS",4)) { info->flags|=2; break; }
        at+=12+length;
    }
    return 0;
}
static ABI int32_t png_memory_size(const PngCreateParam *param) {
    if (!param || param->attribute>1) return PNG_INVALID_PARAM;
    if (param->max_width-1>1000000) return PNG_INVALID_SIZE;
    return (int32_t)sizeof(PngState);
}
static ABI int32_t png_create(const PngCreateParam *param, void *memory, uint32_t size, void **handle) {
    int32_t need=png_memory_size(param);
    if (need<0) return need;
    if (!memory || !handle) return PNG_INVALID_ADDR;
    if (size<(uint32_t)need) return PNG_INVALID_SIZE;
    if (param->attribute) { fputs("STOP: PngDec 16-bit output is not supported\n",stderr); exit(21); }
    PngState *state=memory;
    state->magic=PNG_MAGIC; state->attribute=param->attribute; state->max_width=param->max_width;
    *handle=state;
    return 0;
}
static ABI int32_t png_delete(PngState *handle) {
    if (!handle || handle->magic!=PNG_MAGIC) return PNG_INVALID_HANDLE;
    handle->magic=0; return 0;
}
static ABI int32_t png_parse(const PngParseParam *param, PngInfo *info) {
    if (!param || !info) return PNG_INVALID_PARAM;
    return png_header(param->png,param->size,info);
}
static ABI int32_t png_decode(PngState *handle, const PngDecodeParam *param, PngInfo *info) {
    if (!handle || handle->magic!=PNG_MAGIC) return PNG_INVALID_HANDLE;
    if (!param) return PNG_INVALID_PARAM;
    if (!param->png || !param->image) return PNG_INVALID_ADDR;
    if (param->pixel_format>1) return PNG_INVALID_PARAM;
    PngInfo header;
    int32_t r=png_header(param->png,param->png_size,&header);
    if (r) return r;
    if (header.width>handle->max_width) return PNG_INVALID_SIZE;
    uint32_t pitch=param->pitch ? param->pitch : header.width*4, width, height;
    if (!gow3gpu_png_decode(param->png,param->png_size,param->image,param->image_size,pitch,&width,&height))
        return PNG_DECODE_ERROR;
    /* No alpha channel and no tRNS: alpha is the caller's value. RGBA, or BGRA (format 1). */
    int fill=(header.color_space==2 || header.color_space==3 || header.color_space==4) && !(header.flags&2);
    for (uint32_t y=0;(fill || param->pixel_format) && y<height;++y) {
        unsigned char *p=param->image+(uint64_t)pitch*y;
        for (uint32_t x=0;x<width;++x,p+=4) {
            if (param->pixel_format) { unsigned char t=p[0]; p[0]=p[2]; p[2]=t; }
            if (fill) p[3]=(unsigned char)param->alpha;
        }
    }
    if (info) *info=header;
    return width>32767 || height>32767 ? 0 : (int32_t)(width<<16|height);
}

/* ---- Console features with no PC counterpart: accepted and ignored ---- */
static ABI int32_t privacy_setting(int32_t type, int32_t *value) {
    (void)type;
    if (!value) return (int32_t)0x80A10003; /* SCE_SYSTEM_SERVICE_ERROR_PARAMETER */
    *value=0; return 0;
}

/* ---- Error dialog: logs the code the game wanted to show and finishes at once ---- */
static int32_t error_dialog_status=1; /* 1 INITIALIZED, 2 RUNNING, 3 FINISHED */
static ABI int32_t error_dialog_open(const int32_t *param) {
    if (!param) return (int32_t)0x80ED0003; /* SCE_ERROR_DIALOG_ERROR_PARAM_INVALID */
    printf("Runtime: error dialog requested for code 0x%08x (closed immediately)\n",(unsigned)param[1]);
    fflush(stdout);
    error_dialog_status=3;
    return 0;
}
static ABI int32_t error_dialog_status_get(void) { return error_dialog_status; }

/* ---- VoiceQoS (voice chat transport): endpoints exist, no packets ever arrive ---- */
static int32_t voice_next=1;
static ABI int32_t voiceqos_create(int32_t *out) {
    if (!out) return (int32_t)0x80A30002; /* SCE_VOICE_QOS_ERROR_INVALID_ARGUMENT */
    *out=__atomic_add_fetch(&voice_next,1,__ATOMIC_RELAXED);
    return 0;
}
static ABI int32_t voiceqos_get_attr(int32_t id, int32_t attr, void *value, int32_t size) {
    (void)id; (void)attr;
    if (value && size>0) memset(value,0,(size_t)size);
    return 0;
}
static ABI int32_t voiceqos_read(int32_t connection, void *data, uint32_t *size) {
    (void)connection; (void)data;
    if (size) *size=0;
    return 0;
}

/* ---- Dynamic modules: everything the game needs is linked up front ---- */
static ABI int32_t load_start_module(const char *path, uint64_t argc, const void *argv, uint32_t flags,
                                     const void *option, int32_t *result) {
    (void)argc; (void)argv; (void)flags; (void)option;
    printf("Runtime: sceKernelLoadStartModule(%s) -> not available\n",path ? path : "(null)");
    if (result) *result=0;
    return ERR(2); /* ENOENT */
}
static ABI int32_t posix_fcntl(int32_t fd, int32_t cmd, int64_t arg) {
    (void)fd; (void)arg;
    return cmd==1 || cmd==3 ? 0 : 0; /* F_GETFD/F_GETFL: no flags; set commands accepted */
}
static ABI int32_t posix_cancel(void *thread) { (void)thread; return E_SRCH; }

/* ---- HTTP extras: the network is unplugged ---- */
static ABI int32_t http_last_errno(int32_t request, int32_t *error) {
    (void)request;
    if (!error) return (int32_t)0x80431077;
    *error=65; /* EHOSTUNREACH */
    return 0;
}
static ABI int32_t http_network(void) { return (int32_t)0x80431063; } /* SCE_HTTP_ERROR_NETWORK */
static ABI int32_t http_bad_url(void) { return (int32_t)0x80431061; } /* SCE_HTTP_ERROR_INVALID_URL */

/* ---- Dialogs the offline console never shows: opening fails, status stays idle ---- */
static ABI int32_t dialog_unavailable(void) { return (int32_t)0x80B80001; } /* COMMON_DIALOG not available */
static ABI int32_t dialog_none(void) { return 0; }                           /* status NONE */

/* ---- Misc devices ---- */
static ABI int32_t net_mac(uint8_t *mac, int32_t flags) {
    (void)flags;
    if (!mac) return (int32_t)0x80410116;
    static const uint8_t fixed[6]={0x02,0x00,0x4d,0x45,0x44,0x49}; /* locally administered */
    memcpy(mac,fixed,6);
    return 0;
}
static ABI int32_t move_absent(void) { return (int32_t)0x80EE0002; }  /* SCE_MOVE_ERROR_NOT_INIT */
static ABI int32_t streaming_off(void) { return (int32_t)0x804E0002; } /* not streaming */
static ABI int32_t trophy_no_icon(void) { return (int32_t)0x80551616; } /* SCE_NP_TROPHY_ERROR_ICON_FILE_NOT_FOUND */
static ABI __attribute__((noreturn)) void load_exec(const char *path, const void *argv) {
    (void)argv;
    printf("Runtime: game asked to launch %s; exiting\n",path ? path : "(null)");
    fflush(NULL);
    _exit(0);
}

static const RuntimeExport exports[]={
    {"sceKernelLoadStartModule",load_start_module}, {"sceKernelDlsym",not_found},
    {"fcntl",posix_fcntl}, {"pthread_cancel",posix_cancel},
    {"sceHttpGetLastErrno",http_last_errno}, {"sceHttpAbortRequest",ok_noop},
    {"sceHttpGetAllResponseHeaders",http_network}, {"sceHttpGetMemoryPoolStats",http_network},
    {"sceHttpUriParse",http_bad_url}, {"sceHttpUriBuild",http_bad_url}, {"sceHttpUriEscape",http_bad_url},
    {"sceWebBrowserDialogOpen",dialog_unavailable}, {"sceWebBrowserDialogOpenForPredeterminedContent",dialog_unavailable},
    {"sceWebBrowserDialogUpdateStatus",dialog_none}, {"sceWebBrowserDialogGetResult",dialog_unavailable},
    {"sceWebBrowserDialogClose",ok_noop}, {"sceWebBrowserDialogResetCookie",ok_noop},
    {"sceGameCustomDataDialogOpenA",dialog_unavailable}, {"sceGameCustomDataDialogUpdateStatus",dialog_none},
    {"sceGameCustomDataDialogGetResultA",dialog_unavailable},
    {"sceInvitationDialogOpenA",dialog_unavailable}, {"sceInvitationDialogUpdateStatus",dialog_none},
    {"sceInvitationDialogGetResultA",dialog_unavailable},
    {"sceSigninDialogOpen",dialog_unavailable}, {"sceSigninDialogUpdateStatus",dialog_none},
    {"sceSigninDialogGetResult",dialog_unavailable},
    {"sceNpSnsFacebookDialogOpen",dialog_unavailable}, {"sceNpSnsFacebookDialogUpdateStatus",dialog_none},
    {"sceNpSnsFacebookDialogGetResult",dialog_unavailable},
    {"sceNpProfileDialogOpenA",dialog_unavailable}, {"sceNpCommerceDialogGetResult",dialog_unavailable},
    {"sceImeDialogGetPanelSizeExtended",dialog_unavailable},
    {"sceNetGetMacAddress",net_mac}, {"sceNetGetSockInfo",socket_badf},
    {"sceMoveGetDeviceInfo",move_absent}, {"sceMoveSetVibration",move_absent},
    {"sceGameLiveStreamingGetCurrentStatus2",streaming_off}, {"sceGameLiveStreamingGetProgramInfo",streaming_off},
    {"sceNpTrophyShowTrophyList",ok_noop}, {"sceNpTrophyCaptureScreenshot",ok_noop},
    {"sceNpTrophyGetGameIcon",trophy_no_icon}, {"sceNpTrophyGetGroupIcon",trophy_no_icon},
    {"sceNpTrophyGetTrophyIcon",trophy_no_icon}, {"sceNpTrophyGetGroupInfo",trophy_no_icon},
    {"sceSystemServiceLoadExec",load_exec},
    {"sceVoiceQoSInit",ok_noop}, {"sceVoiceQoSEnd",ok_noop},
    {"sceVoiceQoSCreateLocalEndpoint",voiceqos_create}, {"sceVoiceQoSDeleteLocalEndpoint",ok_noop},
    {"sceVoiceQoSCreateRemoteEndpoint",voiceqos_create}, {"sceVoiceQoSDeleteRemoteEndpoint",ok_noop},
    {"sceVoiceQoSConnect",voiceqos_create}, {"sceVoiceQoSDisconnect",ok_noop},
    {"sceVoiceQoSGetLocalEndpointAttribute",voiceqos_get_attr}, {"sceVoiceQoSSetLocalEndpointAttribute",ok_noop},
    {"sceVoiceQoSReadPacket",voiceqos_read}, {"sceVoiceQoSWritePacket",ok_noop},
    {"sceScreenShotEnable",ok_noop}, {"sceScreenShotDisable",ok_noop},
    {"sceSharePlayInitialize",ok_noop}, {"sceSharePlaySetProhibition",ok_noop}, {"sceSharePlayTerminate",ok_noop},
    {"sceGameLiveStreamingInitialize",ok_noop}, {"sceGameLiveStreamingTerminate",ok_noop},
    {"sceGameLiveStreamingEnableLiveStreaming",ok_noop},
    {"sceNetCtlTerm",ok_noop}, {"sceNpCommerceShowPsStoreIcon",ok_noop}, {"sceNpCommerceHidePsStoreIcon",ok_noop},
    {"sceSystemServiceGetPlatformPrivacySetting",privacy_setting},
    {"sceContentExportInit",ok_noop}, {"sceContentExportTerm",ok_noop},
    {"sceErrorDialogInitialize",ok_noop}, {"sceErrorDialogTerminate",ok_noop},
    {"sceErrorDialogOpen",error_dialog_open}, {"sceErrorDialogUpdateStatus",error_dialog_status_get},
    {"sceWebBrowserDialogInitialize",ok_noop}, {"sceWebBrowserDialogTerminate",ok_noop},
    {"sceGameCustomDataDialogInitialize",ok_noop}, {"sceGameCustomDataDialogTerminate",ok_noop},
    {"sceInvitationDialogInitialize",ok_noop}, {"sceInvitationDialogTerminate",ok_noop},
    /* Video recording (share button clips): accepted, nothing is recorded (as in shadPS4). */
    {"sceVideoRecordingSetInfo",ok_noop}, {"sceVideoRecordingQueryMemSize2",ok_noop},
    {"sceVideoRecordingOpen2",ok_noop}, {"sceVideoRecordingStart",ok_noop}, {"sceVideoRecordingStop",ok_noop},
    {"sceVideoRecordingClose",ok_noop}, {"sceVideoRecordingGetStatus",ok_noop},
    {"sceKernelCreateEventFlag",ef_create}, {"sceKernelDeleteEventFlag",ef_delete},
    {"sceKernelSetEventFlag",ef_set}, {"sceKernelClearEventFlag",ef_clear},
    {"sceKernelPollEventFlag",ef_poll}, {"sceKernelWaitEventFlag",ef_wait},
    {"sceKernelCancelEventFlag",ef_cancel},
    {"sceKernelIsNeoMode",is_neo_mode}, {"sceKernelGetCpumode",get_cpumode},
    {"scePthreadGetthreadid",thread_id}, {"scePthreadGetname",thread_name},
    {"sceKernelChmod",kernel_chmod}, {"sceKernelUtimes",kernel_utimes}, {"sceKernelGetdents",kernel_getdents},
    {"sceKernelGetPrtAperture",prt_aperture}, {"_nanosleep",kernel_nanosleep},
    {"sceKernelConvertLocaltimeToUtc",local_to_utc}, {"sceKernelConvertUtcToLocaltime",utc_to_local},
    {"sceKernelAvailableFlexibleMemorySize",available_flexible},
    {"_sceKernelRtldThreadAtexitIncrement",ok_noop}, {"_sceKernelRtldThreadAtexitDecrement",ok_noop},
    {"_sceKernelSetThreadAtexitCount",ok_noop}, {"_sceKernelSetThreadAtexitReport",ok_noop},
    {"_sceKernelSetThreadDtors",ok_noop},
    {"sceKernelIsAddressSanitizerEnabled",ok_noop},
    {"sceKernelGetSanitizerMallocReplaceExternal",null_pointer},
    {"sceKernelGetSanitizerNewReplaceExternal",null_pointer},
    {"sceKernelGetModuleInfoForUnwind",not_found}, {"sceKernelInternalMemoryGetModuleSegmentInfo",not_found},
    {"sceSysmoduleGetModuleInfoForUnwind",not_found}, {"sceLibcHeapGetTraceInfo",heap_trace_info}, {"Func_7C980FFB0AA27E7A",disc_map_query},
    /* WipEout: nobody connects through Remote Play. */
    {"sceRemoteplayInitialize",ok_noop}, {"sceRemoteplayTerminate",ok_noop},
    {"sceRemoteplayGetConnectionStatus",out2_zero},
    /* WipEout: no share menu or social screen; mastering and Ajm memory need no state (shadPS4). */
    {"sceShareUtilityInitializeEx2",ok_noop}, {"sceShareUtilityTerminate",ok_noop},
    {"sceShareUtilityOpenShareMenuDefault",ok_noop},
    {"sceSocialScreenInitialize",ok_noop}, {"sceSocialScreenTerminate",ok_noop},
    {"sceAudioOutMasteringInit",ok_noop}, {"sceAudioOutMasteringSetParam",ok_noop},
    {"sceAudioOutMasteringTerm",ok_noop}, {"sceAjmMemoryUnregister",ok_noop},
    {"sceKernelUuidCreate",uuid_create},
    {"sceHmdInitialize315",hmd_initialize}, {"sceHmdTerminate",hmd_terminate},
    {"sceHmdOpen",hmd_open}, {"sceHmdClose",hmd_close}, {"sceHmdGetDeviceInformation",hmd_device_info},
    {"sceHmdGetDeviceInformationByHandle",hmd_by_handle}, {"sceHmdGetFieldOfView",hmd_by_handle},
    {"sceHmdReprojectionInitialize",ok_noop}, {"sceHmdReprojectionFinalize",ok_noop},
    {"sceHmdReprojectionStart",ok_noop}, {"sceHmdReprojectionStop",ok_noop},
    {"sceHmdReprojectionSetDisplayBuffers",ok_noop}, {"sceHmdReprojectionUnsetDisplayBuffers",ok_noop},
    {"sceHmdReprojectionSetOutputMinColor",ok_noop},
    {"sceHmdReprojectionQueryGarlicBuffSize",hmd_garlic_size}, {"sceHmdReprojectionQueryOnionBuffSize",hmd_onion_size},
    {"sceHmdReprojectionQueryGarlicBuffAlign",hmd_buffer_align}, {"sceHmdReprojectionQueryOnionBuffAlign",hmd_buffer_align},
    {"sceHmdSetupDialogInitialize",ok_noop}, {"sceHmdSetupDialogTerminate",ok_noop},
    {"sceHmdSetupDialogOpen",ok_noop}, {"sceHmdSetupDialogClose",ok_noop},
    {"sceHmdSetupDialogUpdateStatus",dialog_finished}, {"sceHmdSetupDialogGetResult",hmd_dialog_result},
    {"sceCameraIsAttached",camera_attached}, {"sceCameraOpen",camera_not_connected},
    {"sceCameraClose",camera_not_open}, {"sceCameraStart",camera_not_open}, {"sceCameraStop",camera_not_open},
    {"sceCameraSetConfig",camera_not_open}, {"sceCameraSetVideoSync",camera_not_open},
    {"sceCameraGetFrameData",camera_not_open},
    /* PlayGo: every chunk is installed, so its dialog finishes at once. */
    {"scePlayGoDialogInitialize",ok_noop}, {"scePlayGoDialogTerminate",ok_noop},
    {"scePlayGoDialogOpen",ok_noop}, {"scePlayGoDialogClose",ok_noop},
    {"scePlayGoDialogUpdateStatus",dialog_finished}, {"scePlayGoDialogGetStatus",dialog_finished},
    {"sceErrorDialogGetStatus",error_dialog_status_get}, {"sceErrorDialogClose",ok_noop},
    {"sceSigninDialogInitialize",ok_noop}, {"sceSigninDialogTerminate",ok_noop},
    {"sceNpSnsFacebookDialogInitialize",ok_noop}, {"sceNpSnsFacebookDialogTerminate",ok_noop},
    /* No add-ons: downloads are accepted but never arrive, and no entitlement key exists. */
    {"sceAppContentAddcontEnqueueDownload",ok_noop}, {"sceAppContentGetEntitlementKey",no_entitlement},
    /* HTTP settings are accepted; every request still fails with SCE_HTTP_ERROR_NETWORK. */
    {"sceHttpsGetCaList",https_ca_list}, {"sceHttpsSetSslCallback",ok_noop},
    {"sceHttpSetCookieEnabled",ok_noop}, {"sceHttpSetChunkedTransferEnabled",ok_noop},
    {"sceHttpSetRecvTimeOut",ok_noop}, {"sceHttpSetResolveTimeOut",ok_noop}, {"sceHttpSetSendTimeOut",ok_noop},
    {"sceRandomGetRandomNumber",random_number},
    {"pthread_rwlock_init",posix_rw_init}, {"pthread_rwlock_destroy",posix_rw_destroy},
    {"pthread_rwlock_rdlock",posix_rw_rdlock}, {"pthread_rwlock_wrlock",posix_rw_wrlock},
    {"pthread_rwlock_unlock",posix_rw_unlock},
    {"pthread_getschedparam",posix_getschedparam}, {"pthread_setschedparam",posix_setschedparam},
    {"sched_get_priority_max",sched_max}, {"sched_get_priority_min",sched_min},
    {"pthread_mutexattr_setprotocol",mutexattr_setprotocol},
    {"socket",socket_none},
    {"accept",socket_badf}, {"bind",socket_badf}, {"connect",socket_badf}, {"getpeername",socket_badf},
    {"getsockname",socket_badf}, {"getsockopt",socket_badf}, {"setsockopt",socket_badf}, {"listen",socket_badf},
    {"recv",socket_badf}, {"recvfrom",socket_badf}, {"send",socket_badf}, {"sendto",socket_badf},
    {"shutdown",socket_badf}, {"select",socket_badf},
    {"sceCoredumpRegisterCoredumpHandler",ok_noop}, {"sceCoredumpWriteUserData",ok_noop},
    {"scePlayGoGetEta",playgo_eta}, {"scePlayGoGetToDoList",playgo_todo}, {"scePlayGoPrefetch",ok_noop},
    {"sceAppContentTemporaryDataMount2",temp_mount2},
    {"sceNgs2SystemCreateWithAllocator",ngs2_system_create}, {"sceNgs2SystemDestroy",ok_noop},
    {"sceNgs2RackCreateWithAllocator",ngs2_rack_create}, {"sceNgs2RackDestroy",ok_noop},
    {"sceNgs2RackGetVoiceHandle",ngs2_voice_handle}, {"sceNgs2VoiceControl",ok_noop},
    {"sceNgs2VoiceGetState",ngs2_voice_state}, {"sceNgs2VoiceGetStateFlags",ngs2_voice_flags},
    {"sceNgs2SystemRender",ngs2_render}, {"sceNgs2PanInit",ok_noop}, {"sceNgs2PanGetVolumeMatrix",ok_noop},
    {"sceNgs2ParseWaveformData",ngs2_unsupported_waveform}, {"sceNgs2CalcWaveformBlock",ngs2_unsupported_waveform},
    {"sceNgs2RackQueryBufferSize",ngs2_rack_buffer_size}, {"sceNgs2RackCreate",ngs2_rack_create_buffer},
    /* Knack: CPU 0 for every thread, as shadPS4; temporary data always has 1 GB free. */
    {"sceKernelGetCurrentCpu",ok_noop}, {"sceAppContentTemporaryDataGetAvailableSpaceKb",temp_space},
    {"scePngDecQueryMemorySize",png_memory_size}, {"scePngDecCreate",png_create}, {"scePngDecDelete",png_delete},
    {"scePngDecParseHeader",png_parse}, {"scePngDecDecode",png_decode},
};
/* MediEvil native output (GOW3_UE4_SETRES=WxH, run.py): the base PS4 renderer makes its buffers at
 * the display size at start and only a live r.SetRes change resizes them (the debug menu's
 * Cinematic button). After about 10 s of play this sets the game's own r.SetRes console variable
 * as UE4's FSystemResolution::RequestResolutionChange does (0x2765ef1): flags = GetFlags()
 * & 0xff000000 (vtable +0x20), Set(value, flags) (vtable +0x68). Called from scePadReadState on
 * the game's input thread. CUSA11227 01.00 only: TAutoConsoleVariable at 0x5c28320, its
 * IConsoleVariable* at +8. */
/* Sets r.SetRes on the calling (game) thread; 0 when the game's variable is not where expected. */
static int ue4_setres(const char *text) {
    uint16_t value[24];
    size_t n=strlen(text);
    if (!n || n>=sizeof(value)/sizeof(value[0])) return 0;
    for (size_t i=0;i<=n;++i) value[i]=(uint16_t)(unsigned char)text[i];
    unsigned char *image=runtime_image_base();
    if (!image) return 0;
    const uintptr_t lo=(uintptr_t)image, hi=lo+0x6000000;
    uintptr_t cvar=*(const uintptr_t *)(image+0x5c28328);
    if (!cvar) { printf("Native output: r.SetRes not found\n"); return 0; }
    uintptr_t vtable=*(const uintptr_t *)cvar;
    if (vtable<lo || vtable>=hi) { printf("Native output: unexpected r.SetRes object\n"); return 0; }
    typedef uint32_t (ABI *GetFlags)(uintptr_t);
    typedef void (ABI *Set)(uintptr_t, const uint16_t *, uint32_t);
    uintptr_t get_flags=*(const uintptr_t *)(vtable+0x20), set=*(const uintptr_t *)(vtable+0x68);
    if (get_flags<lo || get_flags>=hi || set<lo || set>=hi) { printf("Native output: unexpected r.SetRes methods\n"); return 0; }
    uint32_t flags=((GetFlags)get_flags)(cvar)&0xff000000u;
    ((Set)set)(cvar,value,flags);
    printf("Native output: r.SetRes %s (live resolution change)\n",text);
    return 1;
}

/* A switch asked for by the in-game menu (window thread), done on the next game-thread poll. */
static char ue4_requested[24];
static int ue4_request_pending;
void runtime_ue4_request_setres(const char *text) {
    if (!text || strlen(text)>=sizeof(ue4_requested)) return;
    strcpy(ue4_requested,text);
    __atomic_store_n(&ue4_request_pending,1,__ATOMIC_RELEASE);
}

/* MediEvil native output (GOW3_UE4_SETRES=WxH, run.py): the base PS4 renderer makes its buffers at
 * the display size at start and only a live r.SetRes change resizes them (the debug menu's
 * Cinematic button). After about 10 s of play this sets the game's own r.SetRes console variable
 * as UE4's FSystemResolution::RequestResolutionChange does (0x2765ef1): flags = GetFlags()
 * & 0xff000000 (vtable +0x20), Set(value, flags) (vtable +0x68). Called from scePadReadState on
 * the game's input thread. CUSA11227 01.00 only: TAutoConsoleVariable at 0x5c28320, its
 * IConsoleVariable* at +8. */
void runtime_ue4_setres_poll(void) {
    static int automatic_done;
    static unsigned reads;
    if (__atomic_exchange_n(&ue4_request_pending,0,__ATOMIC_ACQ_REL)) {
        char text[sizeof(ue4_requested)];
        strcpy(text,ue4_requested);
        if (reads>=600) ue4_setres(text);
        automatic_done=1; /* a choice in the menu wins over the start setting */
    }
    if (automatic_done) return;
    if (++reads<600) return; /* the main menu, with the game viewport up */
    automatic_done=1;
    const char *want=getenv("GOW3_UE4_SETRES");
    if (want && *want) ue4_setres(want);
}

uintptr_t runtime_extra_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_extra_report(void) {
    printf("Runtime: event flags created=%zu, NGS2 silent renders=%zu\n",flags_created,ngs2_renders);
}
