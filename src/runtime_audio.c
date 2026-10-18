/* libSceAudioOut on SDL3 audio streams. sceAudioOutOutput returns once per
 * buffer period on a steady clock, like PS4 hardware: FMOD's output thread
 * reads 256-frame slots out of its mixer's 512-frame ring, and returning in
 * bursts (whenever the device pulls a quantum) lets it overtake the mixer and
 * play stale halves - a click every 10.7 ms. The lowest SDL queue level of
 * each window is steered to two buffers (silence prefill on underrun, ±3%
 * cadence corrections), which also absorbs device clock drift. Without an audio device (headless runs, GOW3_AUDIO=none)
 * ports only follow the clock. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <SDL3/SDL.h>

#define PORTS 25
#define ERR_NOT_OPENED ((int32_t)0x80260001)
#define ERR_INVALID_PORT ((int32_t)0x80260003)
#define ERR_INVALID_POINTER ((int32_t)0x80260004)
#define ERR_PORT_FULL ((int32_t)0x80260005)
#define ERR_INVALID_SIZE ((int32_t)0x80260006)
#define ERR_INVALID_FORMAT ((int32_t)0x80260007)
#define ERR_INVALID_FREQ ((int32_t)0x80260008)
#define ERR_INVALID_VOLUME ((int32_t)0x80260009)
#define ERR_INVALID_TYPE ((int32_t)0x8026000A)
#define ERR_ALREADY_INIT ((int32_t)0x8026000E)
#define ERR_NOT_INIT ((int32_t)0x8026000F)
#define VOLUME_0DB 32768

typedef struct {
    int used, type, channels, is_float, frames, sample_bytes, std_layout, downmix;
    int32_t volume[8];
    SDL_AudioStream *stream;
    uint64_t next_deadline_ns; /* next return of sceAudioOutOutput */
    int64_t adjust_ns; int window_min, window_count; /* queue level control */
    uint64_t last_output_us;
    pthread_mutex_t lock;
    FILE *dump;                     /* GOW3_AUDIO_DUMP: raw converted PCM per port */
    int stats;
    uint64_t stat_start_ns, stat_last_ns, stat_max_gap_ns; /* GOW3_AUDIO_STATS */
    unsigned stat_starved, stat_buffers; int stat_min_queued;
} Port;
typedef struct { uint16_t output; uint8_t channel, reserved; int16_t volume; uint16_t reroute; uint64_t flag, reserved64[2]; } PortState;
_Static_assert(sizeof(PortState)==32,"AudioOut port state layout");

static pthread_mutex_t table_lock=PTHREAD_MUTEX_INITIALIZER;
static Port ports[PORTS];
static int initialized, sdl_ready=-1;
static size_t buffers_out, ports_opened;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
static void sleep_until(uint64_t deadline) {
#ifdef _WIN32
    /* winpthreads' clock_nanosleep only takes CLOCK_REALTIME (EINVAL otherwise). */
    for (uint64_t now=now_ns(); now<deadline; now=now_ns()) compat_sleep_ns(deadline-now);
#else
    struct timespec t={(time_t)(deadline/1000000000u),(long)(deadline%1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL)) {}
#endif
}
static int sdl_audio(void) {
    if (sdl_ready<0) {
        const char *mode=getenv("GOW3_AUDIO");
        sdl_ready = (!mode || strcmp(mode,"none")) && SDL_InitSubSystem(SDL_INIT_AUDIO);
        printf("Runtime: audio backend %s\n", sdl_ready ? SDL_GetCurrentAudioDriver() : "timer (silent)");
    }
    return sdl_ready;
}
static int port_range(int type, int *first, int *last) {
    switch (type) {
    case 0: *first=0; *last=7; return 1;      /* main */
    case 1: *first=8; *last=8; return 1;      /* bgm */
    case 2: *first=9; *last=12; return 1;     /* voice */
    case 3: *first=13; *last=16; return 1;    /* personal */
    case 4: *first=17; *last=20; return 1;    /* pad speaker */
    case 126: *first=21; *last=22; return 1;  /* audio3d */
    case 127: *first=23; *last=24; return 1;  /* aux */
    default: return 0;
    }
}
/* SDL's 7.1-to-stereo matrix keeps only ~0.21 of the front channels (-13.5 dB), so the game
   was far too quiet on stereo outputs. Folds SDL-ordered FL FR C LFE BL BR SL SR frames to
   stereo in place: fronts at full level, center and surrounds at -3 dB, LFE dropped, clamped.
   Returns the stereo byte count. */
static size_t downmix_stereo(unsigned char *buf, size_t frames, int is_float) {
    const float k=0.70710678f;
    for (size_t f=0;f<frames;++f) {
        float s[8], lr[2];
        for (int c=0;c<8;++c) {
            if (is_float) memcpy(&s[c],buf+(f*8+(size_t)c)*4,4);
            else { int16_t v; memcpy(&v,buf+(f*8+(size_t)c)*2,2); s[c]=(float)v/32768.0f; }
        }
        lr[0]=s[0]+k*(s[2]+s[4]+s[6]); lr[1]=s[1]+k*(s[2]+s[5]+s[7]);
        for (int c=0;c<2;++c) {
            float v=lr[c]<-1.0f ? -1.0f : lr[c]>1.0f ? 1.0f : lr[c];
            if (is_float) memcpy(buf+(f*2+(size_t)c)*4,&v,4);
            else { int16_t o=(int16_t)(v*32767.0f); memcpy(buf+(f*2+(size_t)c)*2,&o,2); }
        }
    }
    return frames*2*(is_float ? 4u : 2u);
}
static Port *port_of(int32_t handle, int32_t *error) {
    int id=handle & 0xff;
    if ((handle & 0x3f000000)!=0x20000000 || id>=PORTS) { *error=ERR_INVALID_PORT; return NULL; }
    if (!ports[id].used) { *error=ERR_NOT_OPENED; return NULL; }
    return &ports[id];
}

static ABI int32_t audio_init(void) {
    pthread_mutex_lock(&table_lock);
    int32_t r=initialized ? ERR_ALREADY_INIT : 0;
    initialized=1;
    pthread_mutex_unlock(&table_lock);
    return r;
}
static ABI int32_t audio_open(int32_t user, int32_t type, int32_t index, uint32_t length, uint32_t freq, uint32_t param) {
    (void)user; (void)index;
    if (!initialized) return ERR_NOT_INIT;
    if (!length || length>2048 || (length & 0xff)) return ERR_INVALID_SIZE;
    if (freq!=48000) return ERR_INVALID_FREQ;
    uint32_t format=param & 0xff;
    if (format>7) return ERR_INVALID_FORMAT;
    int first, last;
    if (!port_range(type,&first,&last)) return ERR_INVALID_TYPE;
    static const int channels[8]={1,2,8,1,2,8,8,8};
    pthread_mutex_lock(&table_lock);
    int id=-1;
    for (int i=first;i<=last;++i) if (!ports[i].used) { id=i; break; }
    if (id<0) { pthread_mutex_unlock(&table_lock); return ERR_PORT_FULL; }
    Port *p=&ports[id];
    memset(p,0,sizeof(*p));
    pthread_mutex_init(&p->lock,NULL);
    p->used=1; p->type=type; p->channels=channels[format]; p->is_float=format>=3 && format!=6;
    p->sample_bytes=p->is_float ? 4 : 2; p->frames=(int)length; p->std_layout=format>=6;
    for (int c=0;c<8;++c) p->volume[c]=VOLUME_0DB;
    if (sdl_audio()) {
        SDL_AudioSpec device; int device_frames;
        p->downmix=p->channels==8 && SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&device,&device_frames)
                   && device.channels==2;
        SDL_AudioSpec spec={p->is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16, p->downmix ? 2 : p->channels, 48000};
        p->stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
        if (p->stream) SDL_ResumeAudioStreamDevice(p->stream);
        else fprintf(stderr,"Runtime: SDL audio stream failed (%s); port %d uses the timer sink\n",SDL_GetError(),id);
    }
    p->stats=getenv("GOW3_AUDIO_STATS")!=NULL;
    const char *dump=getenv("GOW3_AUDIO_DUMP");
    if (dump) {
        char path[4096];
        snprintf(path,sizeof(path),"%s.port%d.%dch.%s",dump,id,p->downmix ? 2 : p->channels,p->is_float ? "f32" : "s16");
        p->dump=fopen(path,"wb");
    }
    ++ports_opened;
    pthread_mutex_unlock(&table_lock);
    printf("Runtime: audio port %d opened (type %d, %d ch, %s, %u frames%s)\n",id,type,p->channels,p->is_float ? "float" : "s16",length,
           p->downmix ? ", downmixed to stereo" : "");
    return (type<<16) | id | 0x20000000;
}
static ABI int32_t audio_close(int32_t handle) {
    pthread_mutex_lock(&table_lock);
    int32_t error=0;
    Port *p=port_of(handle,&error);
    if (p) {
        pthread_mutex_lock(&p->lock);
        if (p->stream) SDL_DestroyAudioStream(p->stream);
        if (p->dump) fclose(p->dump);
        p->dump=NULL;
        p->stream=NULL; p->used=0;
        pthread_mutex_unlock(&p->lock);
    }
    pthread_mutex_unlock(&table_lock);
    return p ? 0 : error;
}
/* `pace`: wait for this port's next period. sceAudioOutOutputs waits once for all its ports. */
static int32_t output_port(int32_t handle, const void *data, int pace) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    pthread_mutex_lock(&p->lock);
    size_t samples=(size_t)p->frames*(size_t)p->channels, bytes=samples*(size_t)p->sample_bytes;
    uint64_t period=(uint64_t)p->frames*1000000000u/48000u;
    if (data) {
        /* Apply per-channel volume and map PS4 8ch (L R C LFE SL SR BL BR) to SDL 7.1 order. */
        unsigned char converted[2048*8*4];
        static const int remap[8]={0,1,2,3,6,7,4,5};
        for (size_t f=0;f<(size_t)p->frames;++f) for (int c=0;c<p->channels;++c) {
            int target=p->channels==8 && !p->std_layout ? remap[c] : c;
            size_t from=f*(size_t)p->channels+(size_t)c, to=f*(size_t)p->channels+(size_t)target;
            float gain=(float)p->volume[c]/VOLUME_0DB;
            if (p->is_float) { float v; memcpy(&v,(const char *)data+from*4,4); v*=gain; memcpy(converted+to*4,&v,4); }
            else { int16_t v; memcpy(&v,(const char *)data+from*2,2); v=(int16_t)((float)v*gain); memcpy(converted+to*2,&v,2); }
        }
        if (p->downmix) bytes=downmix_stereo(converted,(size_t)p->frames,p->is_float);
        if (p->dump) fwrite(converted,1,bytes,p->dump);
        if (p->stats) {
            uint64_t now=now_ns();
            if (!p->stat_start_ns) { p->stat_start_ns=now; p->stat_min_queued=1<<30; }
            else if (now-p->stat_last_ns>p->stat_max_gap_ns) p->stat_max_gap_ns=now-p->stat_last_ns;
            p->stat_last_ns=now; ++p->stat_buffers;
            if (p->stream) {
                int queued=SDL_GetAudioStreamQueued(p->stream);
                if (queued<p->stat_min_queued) p->stat_min_queued=queued;
                if (queued==0) ++p->stat_starved;
            }
            if (now-p->stat_start_ns>=5000000000u) {
                printf("Audio stats port %d: %u buffers/5s (expected %u), max gap %.2f ms, min queued %d bytes, starved %u\n",
                       (int)(p-ports),p->stat_buffers,(unsigned)(5u*48000u/(unsigned)p->frames),p->stat_max_gap_ns/1e6,
                       p->stat_min_queued,p->stat_starved);
                p->stat_start_ns=now; p->stat_max_gap_ns=0; p->stat_starved=0; p->stat_buffers=0; p->stat_min_queued=1<<30;
            }
        }
        if (pace) {
            uint64_t now=now_ns();
            if (!p->next_deadline_ns || now>p->next_deadline_ns+8*period) p->next_deadline_ns=now; /* start or stall: restart cadence */
            else sleep_until(p->next_deadline_ns);
            p->next_deadline_ns+=period;
        }
        if (p->stream) {
            /* The device drains the queue in quanta (21 ms on PipeWire), so the level
               is a sawtooth: its minimum over ~32 buffers is what gets controlled. */
            int low=2*(int)bytes, queued=SDL_GetAudioStreamQueued(p->stream);
            if (queued<(int)bytes) {
                static const unsigned char silence[2048*8*4];
                SDL_PutAudioStreamData(p->stream,silence,low-queued<(int)bytes ? low-queued : (int)bytes);
                queued=low;
            }
            if (!p->window_count || queued<p->window_min) p->window_min=queued;
            if (++p->window_count==32) {
                p->adjust_ns=p->window_min>low+2*(int)bytes ? (int64_t)(period/32) : p->window_min<low ? -(int64_t)(period/32) : 0;
                p->window_count=0;
            }
            p->next_deadline_ns+=(uint64_t)p->adjust_ns;
            SDL_PutAudioStreamData(p->stream,converted,(int)bytes);
        }
        ++buffers_out;
    }
    p->last_output_us=now_ns()/1000;
    pthread_mutex_unlock(&p->lock);
    return data ? (int32_t)samples : 0;
}
static ABI int32_t audio_output(int32_t handle, const void *data) { return output_port(handle,data,1); }
typedef struct { int32_t handle; const void *data; } OutputParam;
static ABI int32_t audio_outputs(const OutputParam *params, uint32_t count) {
    if (!params) return ERR_INVALID_POINTER;
    if (!count || count>PORTS) return ERR_PORT_FULL;
    int32_t result=0;
    /* One period per call for all ports (FMOD feeds main and BGM together): pacing every
       port would halve the rate and starve both. */
    for (uint32_t i=0;i<count;++i) { int32_t r=output_port(params[i].handle,params[i].data,i==0); if (r<0) return r; result=r; }
    return result;
}
static ABI int32_t audio_volume(int32_t handle, int32_t flags, const int32_t *volume) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!volume) return ERR_INVALID_POINTER;
    pthread_mutex_lock(&p->lock);
    for (int c=0;c<8;++c) if (flags & (1<<c)) {
        if (volume[c]<0 || volume[c]>VOLUME_0DB) { pthread_mutex_unlock(&p->lock); return ERR_INVALID_VOLUME; }
        p->volume[c]=volume[c];
    }
    pthread_mutex_unlock(&p->lock);
    return 0;
}
static ABI int32_t audio_state(int32_t handle, PortState *state) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!state) return ERR_INVALID_POINTER;
    memset(state,0,sizeof(*state));
    switch (p->type) {
    case 2: case 3: state->output=0x40; state->channel=1; break;          /* headphone */
    case 4: state->output=0x04; state->channel=1; state->volume=127; break; /* pad speaker */
    default: state->output=0x01; state->channel=(uint8_t)(p->channels>2 ? 2 : p->channels); break;
    }
    return 0;
}
static ABI int32_t audio_last_time(int32_t handle, uint64_t *time) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!time) return ERR_INVALID_POINTER;
    *time=p->last_output_us; return 0;
}

/* ---- Audio3d (WipEout's Wwise sink). One port. Each period the game writes a bed (2, 6 or 8
 * channels) and mono object PCM with gain and position; PortAdvance mixes them into one 7.1 frame
 * (objects panned left/right by position), PortPush plays the queued frames on an internal
 * AudioOut port of type 126, which paces them like sceAudioOutOutput. ---- */
#define A3D_INVALID_PORT ((int32_t)0x80EA0002)
#define A3D_INVALID_PARAMETER ((int32_t)0x80EA0004)
#define A3D_OUT_OF_RESOURCES ((int32_t)0x80EA0006)
#define A3D_NOT_READY ((int32_t)0x80EA0007)
#define A3D_INVALID_OBJECT ((int32_t)0x80EA0008)
#define A3D_QUEUE 8 /* finished frames held between PortAdvance and PortPush */
enum { A3D_PCM=1, A3D_POSITION=2, A3D_GAIN=3, A3D_RESET_STATE=9 };
typedef struct { uint64_t size; uint32_t granularity, rate, max_objects, queue_depth, buffer_mode, pad, beds; } A3dParams;
typedef struct { uint32_t id, pad; const void *value; uint64_t size; } A3dAttribute;
typedef struct { uint32_t format, pad; const void *samples; uint32_t count; } A3dPcm;
typedef struct { int used, has_pcm; float gain, position[3]; float *pcm; } A3dObject;
static int a3d_ready, a3d_open;
static uint32_t a3d_granularity=256, a3d_depth=2, a3d_max_objects;
static A3dObject *a3d_objects;
static float *a3d_bed, *a3d_frames; /* the frame being written; A3D_QUEUE finished frames (8 channels) */
static uint32_t a3d_head, a3d_count;
static int32_t a3d_out=-1;
static uint64_t a3d_bed_writes, a3d_object_pcm, a3d_frames_played;
static pthread_mutex_t a3d_lock=PTHREAD_MUTEX_INITIALIZER;

static ABI int32_t a3d_init(int64_t reserved) { if (reserved) return A3D_INVALID_PARAMETER; a3d_ready=1; return 0; }
static ABI int32_t a3d_term(void) { if (!a3d_ready) return A3D_NOT_READY; a3d_ready=a3d_open=0; return 0; }
/* Only the first 0x20 bytes: callers pass the SDK's 32-byte parameters (no num_beds field). */
static ABI int32_t a3d_defaults(A3dParams *p) {
    A3dParams d={.size=0x20,.granularity=256,.rate=0,.max_objects=512,.queue_depth=2,.buffer_mode=2};
    if (p) memcpy(p,&d,0x20);
    return 0;
}
static void a3d_free(void) {
    for (uint32_t i=0;a3d_objects && i<a3d_max_objects;++i) free(a3d_objects[i].pcm);
    free(a3d_objects); free(a3d_bed); free(a3d_frames);
    a3d_objects=NULL; a3d_bed=a3d_frames=NULL;
}
static ABI int32_t a3d_port_open(int32_t user, const A3dParams *p, uint32_t *port) {
    (void)user;
    if (!a3d_ready) return A3D_NOT_READY;
    if (!p || !port) return A3D_INVALID_PARAMETER;
    uint32_t granularity=p->granularity ? p->granularity : 256;
    if (granularity>2048 || (granularity & 0xff)) return A3D_INVALID_PARAMETER;
    pthread_mutex_lock(&a3d_lock);
    if (a3d_open) { pthread_mutex_unlock(&a3d_lock); return A3D_OUT_OF_RESOURCES; } /* one port */
    a3d_free();
    a3d_granularity=granularity;
    a3d_depth=p->queue_depth ? p->queue_depth : 1;
    a3d_max_objects=p->max_objects ? p->max_objects : 512;
    a3d_objects=calloc(a3d_max_objects,sizeof(*a3d_objects));
    a3d_bed=calloc((size_t)granularity*8,sizeof(float));
    a3d_frames=calloc((size_t)A3D_QUEUE*granularity*8,sizeof(float));
    for (uint32_t i=0;i<a3d_max_objects;++i) a3d_objects[i].pcm=calloc(granularity,sizeof(float));
    a3d_head=a3d_count=0; a3d_open=1; *port=0;
    pthread_mutex_unlock(&a3d_lock);
    printf("Runtime: Audio3d port opened (%u frames, queue %u, %u objects, mode %u)\n",granularity,a3d_depth,a3d_max_objects,p->buffer_mode);
    return 0;
}
static int a3d_port_ok(uint32_t port) { return a3d_open && port==0; }
static ABI int32_t a3d_port_close(uint32_t port) {
    if (!a3d_port_ok(port)) return A3D_INVALID_PORT;
    pthread_mutex_lock(&a3d_lock);
    a3d_open=0;
    a3d_free();
    pthread_mutex_unlock(&a3d_lock);
    if (a3d_out>=0) audio_close(a3d_out);
    a3d_out=-1;
    return 0;
}
static ABI int32_t a3d_port_any(uint32_t port) { return a3d_port_ok(port) ? 0 : A3D_INVALID_PORT; }
static ABI int32_t a3d_flush(uint32_t port) {
    if (!a3d_port_ok(port)) return A3D_INVALID_PORT;
    pthread_mutex_lock(&a3d_lock); a3d_count=0; pthread_mutex_unlock(&a3d_lock);
    return 0;
}
static ABI int32_t a3d_object_reserve(uint32_t port, uint32_t *object) {
    if (!a3d_port_ok(port)) return A3D_INVALID_PORT;
    if (!object) return A3D_INVALID_PARAMETER;
    pthread_mutex_lock(&a3d_lock);
    uint32_t i=0;
    while (i<a3d_max_objects && a3d_objects[i].used) ++i;
    if (i<a3d_max_objects) {
        float *pcm=a3d_objects[i].pcm;
        a3d_objects[i]=(A3dObject){.used=1,.pcm=pcm}; /* gain 0 until set, as the library */
    }
    pthread_mutex_unlock(&a3d_lock);
    if (i==a3d_max_objects) return A3D_OUT_OF_RESOURCES;
    *object=i; return 0;
}
static A3dObject *a3d_object(uint32_t port, uint32_t object) {
    return a3d_port_ok(port) && object<a3d_max_objects && a3d_objects[object].used ? &a3d_objects[object] : NULL;
}
static ABI int32_t a3d_object_unreserve(uint32_t port, uint32_t object) {
    if (!a3d_port_ok(port)) return A3D_INVALID_PORT;
    pthread_mutex_lock(&a3d_lock);
    A3dObject *o=a3d_object(port,object);
    if (o) o->used=0;
    pthread_mutex_unlock(&a3d_lock);
    return o ? 0 : A3D_INVALID_OBJECT;
}
/* Adds `frames` frames of `channels`-channel s16 (format 0) or float (1) samples to dst (stride 8 or 1). */
static void a3d_add(float *dst, int stride, const void *src, uint32_t format, uint32_t channels, uint32_t frames) {
    for (uint32_t f=0;f<frames;++f) for (uint32_t c=0;c<channels;++c) {
        size_t i=(size_t)f*channels+c;
        dst[(size_t)f*(size_t)stride+c]+=format ? ((const float *)src)[i] : ((const int16_t *)src)[i]/32768.0f;
    }
}
static ABI int32_t a3d_object_attributes(uint32_t port, uint32_t object, uint64_t count, const A3dAttribute *attributes) {
    if (!a3d_port_ok(port)) return A3D_INVALID_PORT;
    if (!count || !attributes) return A3D_INVALID_PARAMETER;
    pthread_mutex_lock(&a3d_lock);
    A3dObject *o=a3d_object(port,object);
    for (uint64_t i=0;o && i<count;++i) if (attributes[i].id==A3D_RESET_STATE) {
        o->has_pcm=0; o->gain=0; memset(o->position,0,sizeof(o->position));
    }
    for (uint64_t i=0;o && i<count;++i) {
        const A3dAttribute *a=&attributes[i];
        if (!a->value) continue;
        if (a->id==A3D_GAIN && a->size>=4) memcpy(&o->gain,a->value,4);
        else if (a->id==A3D_POSITION && a->size>=12) memcpy(o->position,a->value,12);
        else if (a->id==A3D_PCM && a->size>=sizeof(A3dPcm)) {
            const A3dPcm *pcm=a->value;
            if (!pcm->samples || pcm->format>1) continue;
            memset(o->pcm,0,a3d_granularity*sizeof(float));
            a3d_add(o->pcm,1,pcm->samples,pcm->format,1,pcm->count<a3d_granularity ? pcm->count : a3d_granularity);
            o->has_pcm=1; ++a3d_object_pcm;
        }
    }
    pthread_mutex_unlock(&a3d_lock);
    return 0; /* unknown objects are ignored, like the library */
}
static ABI int32_t a3d_bed_write(uint32_t port, uint32_t channels, uint32_t format, const void *buffer, uint32_t frames) {
    if (!a3d_port_ok(port)) return A3D_INVALID_PORT;
    if ((channels!=2 && channels!=6 && channels!=8) || format>1 || !buffer || !frames) return A3D_INVALID_PARAMETER;
    pthread_mutex_lock(&a3d_lock);
    /* 2: L R; 6: L R C LFE SL SR; 8: L R C LFE SL SR BL BR (the AudioOut 8-channel order). */
    a3d_add(a3d_bed,8,buffer,format,channels,frames<a3d_granularity ? frames : a3d_granularity);
    ++a3d_bed_writes;
    pthread_mutex_unlock(&a3d_lock);
    return 0;
}
/* Mixes the written bed and object PCM into the next queued frame. */
static ABI int32_t a3d_advance(uint32_t port) {
    if (!a3d_port_ok(port)) return A3D_INVALID_PORT;
    pthread_mutex_lock(&a3d_lock);
    if (a3d_count==A3D_QUEUE) { pthread_mutex_unlock(&a3d_lock); return A3D_NOT_READY; }
    size_t n=(size_t)a3d_granularity*8;
    float *frame=a3d_frames+(size_t)((a3d_head+a3d_count)%A3D_QUEUE)*n;
    memcpy(frame,a3d_bed,n*sizeof(float));
    memset(a3d_bed,0,n*sizeof(float));
    for (uint32_t i=0;i<a3d_max_objects;++i) {
        A3dObject *o=&a3d_objects[i];
        if (!o->used || !o->has_pcm) continue;
        /* Equal-power pan on the direction's left/right part (x right), front speakers. */
        float x=o->position[0], d=sqrtf(x*x+o->position[1]*o->position[1]+o->position[2]*o->position[2]);
        float pan=d>1e-6f ? x/d : 0.0f, angle=(pan+1.0f)*0.78539816f;
        float left=cosf(angle)*o->gain, right=sinf(angle)*o->gain;
        for (uint32_t f=0;f<a3d_granularity;++f) { frame[f*8]+=o->pcm[f]*left; frame[f*8+1]+=o->pcm[f]*right; }
        o->has_pcm=0;
    }
    ++a3d_count;
    pthread_mutex_unlock(&a3d_lock);
    return 0;
}
/* Plays the queued frames; each output waits for its period, so SYNC and ASYNC both pace. */
static ABI int32_t a3d_push(uint32_t port, uint32_t blocking) {
    (void)blocking;
    if (!a3d_port_ok(port)) return A3D_INVALID_PORT;
    if (a3d_out<0) {
        a3d_out=audio_open(0xff,126,0,a3d_granularity,48000,5); /* float, 8 channels */
        if (a3d_out<0) return a3d_out;
    }
    static float frame[2048*8]; /* one pushing thread */
    for (;;) {
        pthread_mutex_lock(&a3d_lock);
        if (!a3d_count) { pthread_mutex_unlock(&a3d_lock); return 0; }
        memcpy(frame,a3d_frames+(size_t)a3d_head*a3d_granularity*8,(size_t)a3d_granularity*8*sizeof(float));
        a3d_head=(a3d_head+1)%A3D_QUEUE; --a3d_count;
        pthread_mutex_unlock(&a3d_lock);
        for (size_t i=0;i<(size_t)a3d_granularity*8;++i) frame[i]=frame[i]<-1.0f ? -1.0f : frame[i]>1.0f ? 1.0f : frame[i];
        int32_t r=output_port(a3d_out,frame,1);
        if (r<0) return r;
        ++a3d_frames_played;
    }
}
static ABI int32_t a3d_queue_level(uint32_t port, uint32_t *level, uint32_t *available) {
    if (!a3d_port_ok(port)) return A3D_INVALID_PORT;
    if (!level && !available) return A3D_INVALID_PARAMETER;
    pthread_mutex_lock(&a3d_lock);
    uint32_t queued=a3d_count<a3d_depth ? a3d_count : a3d_depth;
    pthread_mutex_unlock(&a3d_lock);
    if (level) *level=queued;
    if (available) *available=a3d_depth-queued;
    return 0;
}
static ABI int32_t a3d_attributes_supported(uint32_t port, uint32_t *ids, uint32_t *count) {
    static const uint32_t supported[]={A3D_PCM,A3D_POSITION,A3D_GAIN,A3D_RESET_STATE};
    if (!a3d_port_ok(port)) return A3D_INVALID_PORT;
    if (!count) return A3D_INVALID_PARAMETER;
    uint32_t n=ids && *count<4 ? *count : 4;
    if (ids) memcpy(ids,supported,n*sizeof(*ids));
    *count=n; return 0;
}
static ABI int32_t a3d_audio_out_open(uint32_t port, int32_t user, int32_t type, int32_t index, uint32_t length, uint32_t freq, uint32_t param) {
    if (!a3d_port_ok(port)) return A3D_INVALID_PORT;
    if (length!=a3d_granularity) return A3D_INVALID_PARAMETER;
    return audio_open(user,type,index,length,freq,param);
}

static const RuntimeExport exports[]={
    {"sceAudio3dInitialize",a3d_init}, {"sceAudio3dTerminate",a3d_term},
    {"sceAudio3dGetDefaultOpenParameters",a3d_defaults}, {"sceAudio3dPortOpen",a3d_port_open},
    {"sceAudio3dPortClose",a3d_port_close}, {"sceAudio3dPortAdvance",a3d_advance},
    {"sceAudio3dPortFlush",a3d_flush}, {"sceAudio3dPortSetAttribute",a3d_port_any},
    {"sceAudio3dPortGetAttributesSupported",a3d_attributes_supported},
    {"sceAudio3dPortPush",a3d_push}, {"sceAudio3dPortGetQueueLevel",a3d_queue_level},
    {"sceAudio3dObjectReserve",a3d_object_reserve}, {"sceAudio3dObjectUnreserve",a3d_object_unreserve},
    {"sceAudio3dObjectSetAttributes",a3d_object_attributes}, {"sceAudio3dBedWrite",a3d_bed_write},
    {"sceAudio3dAudioOutOpen",a3d_audio_out_open}, {"sceAudio3dAudioOutOutputs",audio_outputs},
    {"sceAudioOutInit",audio_init}, {"sceAudioOutOpen",audio_open}, {"sceAudioOutClose",audio_close},
    {"sceAudioOutOutput",audio_output}, {"sceAudioOutOutputs",audio_outputs},
    {"sceAudioOutSetVolume",audio_volume}, {"sceAudioOutGetPortState",audio_state},
    {"sceAudioOutGetLastOutputTime",audio_last_time},
};
uintptr_t runtime_audio_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_audio_report(void) {
    printf("Runtime: audio ports opened=%zu, buffers output=%zu\n",ports_opened,buffers_out);
    if (a3d_bed_writes || a3d_object_pcm)
        printf("Runtime: Audio3d bed writes=%llu, object buffers=%llu, frames played=%llu\n",(unsigned long long)a3d_bed_writes,
               (unsigned long long)a3d_object_pcm,(unsigned long long)a3d_frames_played);
}
