#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>

#define CARD "/dev/dri/card0"
#define EXPECTED_KERNEL "6.1.25-android14-11-g4528d59d4335-ab11203497"
#define MAX_OBJECTS 256U
#define WIDTH 2944U
#define HEIGHT 1840U
#define SECONDS 10
#define IMAGE_SECONDS 5
#define STATUS_IMAGE_PATH "/run/native-tools/status-image-20261029-late.bgrx"
#define STATUS_IMAGE_BYTES ((size_t)WIDTH * HEIGHT * 4U)
#define MAX_BUFFER_BYTES (32U * 1024U * 1024U)
#define XRGB8888 ((uint32_t)'X' | ((uint32_t)'R' << 8) | ((uint32_t)'2' << 16) | ((uint32_t)'4' << 24))
#define PRIMARY_PLANE 1U

static bool mutations_enabled;
static volatile sig_atomic_t stop_requested;
static unsigned long syscalls_issued;
static bool use_status_image;
static uint64_t ptr64(const void *p) { return (uint64_t)(uintptr_t)p; }
static void interrupted(int number) { (void)number; stop_requested = 1; }

/* The default mode can reach only the first, read-only ioctl group. */
static int call_ioctl(int fd, unsigned long request, void *arg) {
    switch (request) {
    case DRM_IOCTL_VERSION: case DRM_IOCTL_GET_CAP:
    case DRM_IOCTL_MODE_GETRESOURCES: case DRM_IOCTL_MODE_GETCONNECTOR:
    case DRM_IOCTL_MODE_GETENCODER: case DRM_IOCTL_MODE_GETCRTC:
    case DRM_IOCTL_MODE_GETPLANERESOURCES: case DRM_IOCTL_MODE_GETPLANE:
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES: case DRM_IOCTL_MODE_GETPROPERTY:
        break;
    case DRM_IOCTL_SET_CLIENT_CAP: case DRM_IOCTL_MODE_CREATE_DUMB:
    case DRM_IOCTL_MODE_MAP_DUMB: case DRM_IOCTL_MODE_DESTROY_DUMB:
    case DRM_IOCTL_MODE_ADDFB2: case DRM_IOCTL_MODE_RMFB:
    case DRM_IOCTL_MODE_CREATEPROPBLOB: case DRM_IOCTL_MODE_DESTROYPROPBLOB:
    case DRM_IOCTL_MODE_ATOMIC:
        if (!mutations_enabled) { errno = EPERM; return -1; }
        break;
    default: errno = EPERM; return -1;
    }
    int result;
    do { syscalls_issued++; result = ioctl(fd, request, arg); }
    while (result < 0 && errno == EINTR && !stop_requested);
    return result;
}

static void fail(const char *what) {
    fprintf(stderr, "display-test: %s: %s (errno=%d)\n", what, strerror(errno), errno);
}
static int rejected(const char *reason) {
    fprintf(stderr, "display-test: refusing: %s\n", reason); errno = EINVAL; return -1;
}
static bool mode_allowed(const struct drm_mode_modeinfo *m) {
    /* Only a mode returned by the panel with the exact recorded 30Hz timing. */
    return m->clock == 770472 && m->hdisplay == WIDTH && m->hsync_start == 3034 &&
        m->hsync_end == 3044 && m->htotal == 3132 && m->hskew == 0 &&
        m->vdisplay == HEIGHT && m->vsync_start == 8016 && m->vsync_end == 8018 &&
        m->vtotal == 8200 && m->vscan == 0 && m->vrefresh == 30 && m->flags == 0 &&
        memcmp(m->name, "2944x1840\0", 10) == 0;
}
static bool buffer_valid(uint32_t pitch, uint64_t size) {
    return pitch >= WIDTH * 4U && pitch % 4U == 0 && size >= (uint64_t)pitch * HEIGHT &&
        size <= MAX_BUFFER_BYTES;
}
static uint32_t pixel(unsigned x, unsigned y) {
    static const uint32_t colors[] = {0x00606060,0x00606000,0x00006060,0x00006000,
                                     0x00600060,0x00600000,0x00000060,0x00000000};
    unsigned bar = x * 8U / WIDTH;
    uint32_t value = colors[bar > 7 ? 7 : bar];
    if (y < 16 || y >= HEIGHT - 16 || x < 16 || x >= WIDTH - 16) value = 0x00303030;
    return value;
}

/* The source PNG is decoded on the PC without changing RGB samples.
 * Only this exact root-owned regular RAM payload can be read by image modes.
 * The PC controller must verify its expected SHA256 before invocation. */
static bool status_file_allowed(const struct stat *st) {
    return S_ISREG(st->st_mode) && st->st_uid == 0 && st->st_gid == 0 &&
        st->st_size == (off_t)STATUS_IMAGE_BYTES &&
        !(st->st_mode & (S_IWGRP | S_IWOTH));
}
static bool status_row_allowed(const uint8_t *row) {
    for (unsigned x=0;x<WIDTH;x++) if (row[(size_t)x*4U+3U] != 0) return false;
    return true;
}
static int status_image_load_rows(void *map, uint32_t pitch) {
    int image_fd=open(STATUS_IMAGE_PATH,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(image_fd<0)return -1;
    struct stat before={0},after={0};
    int result=-1,saved_errno=0;
    if(fstat(image_fd,&before))goto done;
    if(!status_file_allowed(&before)){rejected("status image must be exact root-owned regular BGRX payload");goto done;}
    for(unsigned y=0;y<HEIGHT;y++) {
        uint8_t *line=(uint8_t *)map+(size_t)y*pitch;
        size_t have=0;
        while(have<(size_t)WIDTH*4U) {
            ssize_t n=read(image_fd,line+have,(size_t)WIDTH*4U-have);
            if(n<0&&errno==EINTR)continue;
            if(n<=0){if(n==0)errno=EIO;goto done;}
            have+=(size_t)n;
        }
        if(!status_row_allowed(line)){rejected("status image X padding must be zero");goto done;}
    }
    {uint8_t extra;ssize_t n;
     do{n=read(image_fd,&extra,1);}while(n<0&&errno==EINTR);
     if(n!=0){if(n>0)errno=EFBIG;goto done;}}
    if(fstat(image_fd,&after))goto done;
    if(!status_file_allowed(&after)||before.st_dev!=after.st_dev||before.st_ino!=after.st_ino||
       before.st_mtim.tv_sec!=after.st_mtim.tv_sec||before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec||
       before.st_ctim.tv_sec!=after.st_ctim.tv_sec||before.st_ctim.tv_nsec!=after.st_ctim.tv_nsec) {
        rejected("status image metadata changed during read");goto done;
    }
    result=0;
done:
    saved_errno=errno;
    if(close(image_fd)&&result==0)return -1;
    if(result!=0)errno=saved_errno;
    return result;
}

struct resources {
    struct drm_mode_card_res raw;
    uint32_t fbs[MAX_OBJECTS], crtcs[MAX_OBJECTS], connectors[MAX_OBJECTS], encoders[MAX_OBJECTS];
};
static int resources_read(int fd, struct resources *r) {
    memset(r, 0, sizeof(*r));
    r->raw.count_fbs = r->raw.count_crtcs = r->raw.count_connectors = r->raw.count_encoders = MAX_OBJECTS;
    r->raw.fb_id_ptr = ptr64(r->fbs); r->raw.crtc_id_ptr = ptr64(r->crtcs);
    r->raw.connector_id_ptr = ptr64(r->connectors); r->raw.encoder_id_ptr = ptr64(r->encoders);
    if (call_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r->raw)) return -1;
    if (r->raw.count_fbs > MAX_OBJECTS || r->raw.count_crtcs > MAX_OBJECTS ||
        r->raw.count_connectors > MAX_OBJECTS || r->raw.count_encoders > MAX_OBJECTS)
        return rejected("resource count exceeds fixed capacity");
    return 0;
}
struct connector {
    struct drm_mode_get_connector raw;
    struct drm_mode_modeinfo modes[MAX_OBJECTS];
    uint32_t encoders[MAX_OBJECTS], props[MAX_OBJECTS];
    uint64_t values[MAX_OBJECTS];
};
static int connector_read(int fd, uint32_t id, struct connector *c) {
    memset(c, 0, sizeof(*c)); c->raw.connector_id = id;
    /* A nonzero mode capacity avoids GETCONNECTOR's forced-probe count_modes=0 path. */
    c->raw.count_modes = c->raw.count_encoders = c->raw.count_props = MAX_OBJECTS;
    c->raw.modes_ptr = ptr64(c->modes); c->raw.encoders_ptr = ptr64(c->encoders);
    c->raw.props_ptr = ptr64(c->props); c->raw.prop_values_ptr = ptr64(c->values);
    if (call_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c->raw)) return -1;
    if (c->raw.count_modes > MAX_OBJECTS || c->raw.count_encoders > MAX_OBJECTS || c->raw.count_props > MAX_OBJECTS)
        return rejected("connector count exceeds fixed capacity");
    return 0;
}
struct property { uint32_t id; uint64_t old; };
static int property_read(int fd, uint32_t object, uint32_t type, const char *name, struct property *out) {
    uint32_t ids[MAX_OBJECTS] = {0}; uint64_t values[MAX_OBJECTS] = {0};
    struct drm_mode_obj_get_properties object_props = {0};
    object_props.obj_id = object; object_props.obj_type = type; object_props.count_props = MAX_OBJECTS;
    object_props.props_ptr = ptr64(ids); object_props.prop_values_ptr = ptr64(values);
    if (call_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &object_props)) return -1;
    if (object_props.count_props > MAX_OBJECTS) return rejected("too many object properties");
    unsigned matches = 0;
    for (uint32_t i = 0; i < object_props.count_props; i++) {
        struct drm_mode_get_property p = {0}; p.prop_id = ids[i];
        if (call_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p)) return -1;
        if (strncmp(p.name, name, sizeof(p.name)) == 0) {
            out->id = ids[i]; out->old = values[i]; matches++;
        }
    }
    return matches == 1 ? 0 : rejected("required unique atomic property absent");
}
enum { CONN_CRTC, CRTC_ACTIVE, CRTC_MODE, PLANE_FB, PLANE_CRTC, SRC_X, SRC_Y, SRC_W, SRC_H,
       DST_X, DST_Y, DST_W, DST_H, PROPERTY_COUNT };
struct plan {
    uint32_t connector, encoder, crtc, plane;
    uint32_t plane_count, plane_ids[MAX_OBJECTS];
    struct drm_mode_modeinfo mode;
    struct property props[PROPERTY_COUNT];
};
static const char *property_names[PROPERTY_COUNT] = {"CRTC_ID","ACTIVE","MODE_ID","FB_ID","CRTC_ID","SRC_X","SRC_Y","SRC_W","SRC_H","CRTC_X","CRTC_Y","CRTC_W","CRTC_H"};
static uint32_t property_object(const struct plan *p, unsigned index) {
    return index == CONN_CRTC ? p->connector : index <= CRTC_MODE ? p->crtc : p->plane;
}
static uint32_t property_object_type(unsigned index) {
    return index == CONN_CRTC ? DRM_MODE_OBJECT_CONNECTOR : index <= CRTC_MODE ? DRM_MODE_OBJECT_CRTC : DRM_MODE_OBJECT_PLANE;
}
static bool initial_state_allowed(const struct plan *p) {
    return p->props[CONN_CRTC].old == 0 && p->props[CRTC_ACTIVE].old == 0 &&
        p->props[CRTC_MODE].old == 0 && p->props[PLANE_FB].old == 0 && p->props[PLANE_CRTC].old == 0;
}
static bool plane_ids_valid(const uint32_t *ids, uint32_t count) {
    if (count == 0 || count > MAX_OBJECTS) return false;
    for (uint32_t i=0;i<count;i++) {
        if (ids[i] == 0) return false;
        for (uint32_t j=0;j<i;j++) if (ids[i] == ids[j]) return false;
    }
    return true;
}
static bool same_plane_set(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb) {
    if (na != nb || !plane_ids_valid(a,na) || !plane_ids_valid(b,nb)) return false;
    for (uint32_t i=0;i<na;i++) {
        bool found=false;
        for (uint32_t j=0;j<nb;j++) if (a[i] == b[j]) found=true;
        if (!found) return false;
    }
    return true;
}
static bool plane_state_idle(const struct drm_mode_get_plane *plane,
                             const struct property *fb, const struct property *crtc) {
    /* Both legacy query fields and atomic properties must be zero for every plane. */
    return plane->fb_id == 0 && plane->crtc_id == 0 && fb->old == 0 && crtc->old == 0;
}
static int plane_ids_read(int fd, uint32_t ids[MAX_OBJECTS], uint32_t *count) {
    struct drm_mode_get_plane_res r={0}; r.count_planes=MAX_OBJECTS; r.plane_id_ptr=ptr64(ids);
    if (call_ioctl(fd,DRM_IOCTL_MODE_GETPLANERESOURCES,&r)) return -1;
    if (!plane_ids_valid(ids,r.count_planes)) return rejected("empty, duplicated, zero or oversized plane inventory");
    *count=r.count_planes;
    return 0;
}
static int idle_plane_read(int fd, uint32_t id, struct drm_mode_get_plane *plane,
                           uint32_t formats[MAX_OBJECTS]) {
    memset(plane,0,sizeof(*plane)); plane->plane_id=id;
    plane->count_format_types=MAX_OBJECTS; plane->format_type_ptr=ptr64(formats);
    if (call_ioctl(fd,DRM_IOCTL_MODE_GETPLANE,plane)) return -1;
    if (plane->count_format_types > MAX_OBJECTS) return rejected("too many plane formats");
    struct property fb={0}, crtc={0};
    if (property_read(fd,id,DRM_MODE_OBJECT_PLANE,"FB_ID",&fb) ||
        property_read(fd,id,DRM_MODE_OBJECT_PLANE,"CRTC_ID",&crtc)) return -1;
    printf("PLANE_STATE id=%u possible_crtcs=%u raw_fb=%u raw_crtc=%u FB_ID=%"PRIu64" CRTC_ID=%"PRIu64"\n",
        id,plane->possible_crtcs,plane->fb_id,plane->crtc_id,fb.old,crtc.old);
    if (!plane_state_idle(plane,&fb,&crtc)) return rejected("a plane retains FB_ID/CRTC_ID state; all planes must be detached");
    return 0;
}
static int all_planes_still_idle(int fd, const struct plan *plan) {
    uint32_t ids[MAX_OBJECTS]={0}, count=0;
    if (plane_ids_read(fd,ids,&count)) return -1;
    if (!same_plane_set(plan->plane_ids,plan->plane_count,ids,count))
        return rejected("plane inventory changed after the initial snapshot");
    for (uint32_t i=0;i<count;i++) {
        struct drm_mode_get_plane plane={0}; uint32_t formats[MAX_OBJECTS]={0};
        if (idle_plane_read(fd,ids[i],&plane,formats)) return -1;
    }
    return 0;
}
static int saved_idle_state_unchanged(int fd, const struct plan *plan) {
    struct resources r;
    if (resources_read(fd,&r)) return -1;
    if (r.raw.count_crtcs == 0 || r.raw.count_crtcs > 32) return rejected("invalid CRTC set during idle recheck");
    bool target_found=false;
    for (uint32_t i=0;i<r.raw.count_crtcs;i++) {
        struct drm_mode_crtc c={0}; c.crtc_id=r.crtcs[i];
        if (call_ioctl(fd,DRM_IOCTL_MODE_GETCRTC,&c)) return -1;
        if (c.fb_id || c.mode_valid) return rejected("a CRTC became active after the initial snapshot");
        if (c.crtc_id == plan->crtc) target_found=true;
    }
    if (!target_found) return rejected("target CRTC disappeared during idle recheck");
    if (all_planes_still_idle(fd,plan)) return -1;
    for (unsigned i=0;i<PROPERTY_COUNT;i++) {
        struct property current={0};
        if (property_read(fd,property_object(plan,i),property_object_type(i),property_names[i],&current)) return -1;
        if (current.id != plan->props[i].id || current.old != plan->props[i].old)
            return rejected("atomic property no longer matches saved inactive state");
    }
    return 0;
}
static int discover(int fd, struct plan *plan, bool atomic) {
    struct resources r;
    if (resources_read(fd, &r)) return -1;
    if (r.raw.count_crtcs == 0 || r.raw.count_crtcs > 32) return rejected("unsupported CRTC set");
    printf("RESOURCES crtcs=%u connectors=%u framebuffers=%u\n", r.raw.count_crtcs, r.raw.count_connectors, r.raw.count_fbs);
    unsigned matches = 0;
    struct connector selected = {0};
    for (uint32_t i = 0; i < r.raw.count_connectors; i++) {
        struct connector c;
        if (connector_read(fd, r.connectors[i], &c)) return -1;
        printf("CONNECTOR id=%u type=%u type_id=%u connection=%u modes=%u encoder=%u\n",
            c.raw.connector_id,c.raw.connector_type,c.raw.connector_type_id,c.raw.connection,c.raw.count_modes,c.raw.encoder_id);
        if (c.raw.connector_type == DRM_MODE_CONNECTOR_DSI && c.raw.connector_type_id == 1 && c.raw.connection == 1) {
            selected = c; matches++;
        }
    }
    if (matches != 1) return rejected("expected one connected DSI-1");
    plan->connector = selected.raw.connector_id;
    if (selected.raw.count_encoders != 1) return rejected("ambiguous DSI encoder");
    plan->encoder = selected.encoders[0];
    struct drm_mode_get_encoder encoder = {0}; encoder.encoder_id = plan->encoder;
    if (call_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder)) return -1;
    if (encoder.possible_crtcs != 1U) return rejected("DSI encoder no longer targets only CRTC index zero");
    plan->crtc = r.crtcs[0];
    matches = 0;
    for (uint32_t i = 0; i < selected.raw.count_modes; i++) {
        const struct drm_mode_modeinfo *m = &selected.modes[i];
        printf("MODE %ux%u@%u clock_khz=%u\n",m->hdisplay,m->vdisplay,m->vrefresh,m->clock);
        if (mode_allowed(m)) { plan->mode = *m; matches++; }
    }
    if (matches != 1) return rejected("unique exact recorded 2944x1840 30Hz mode absent");
    for (uint32_t i = 0; i < r.raw.count_crtcs; i++) {
        struct drm_mode_crtc c = {0}; c.crtc_id = r.crtcs[i];
        if (call_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c)) return -1;
        printf("CRTC id=%u framebuffer=%u mode_valid=%u\n",c.crtc_id,c.fb_id,c.mode_valid);
        if (atomic && (c.fb_id != 0 || c.mode_valid != 0)) return rejected("an existing CRTC is active; first test requires idle display");
    }
    if (!atomic) return 0;
    if (plane_ids_read(fd,plan->plane_ids,&plan->plane_count)) return -1;
    matches = 0;
    for (uint32_t i = 0; i < plan->plane_count; i++) {
        struct drm_mode_get_plane plane = {0}; uint32_t formats[MAX_OBJECTS] = {0};
        /* Check every primary/overlay/cursor plane before filtering for the test target. */
        if (idle_plane_read(fd,plan->plane_ids[i],&plane,formats)) return -1;
        if (!(plane.possible_crtcs & 1U)) continue;
        struct property type = {0};
        if (property_read(fd, plane.plane_id, DRM_MODE_OBJECT_PLANE, "type", &type)) return -1;
        bool format_ok = false;
        for (uint32_t j = 0; j < plane.count_format_types; j++) if (formats[j] == XRGB8888) format_ok = true;
        if (type.old == PRIMARY_PLANE && format_ok) { plan->plane = plane.plane_id; matches++; }
    }
    if (matches != 1) return rejected("unique compatible XRGB8888 primary plane absent");
    for (unsigned i = 0; i < PROPERTY_COUNT; i++) {
        uint32_t object = property_object(plan,i), type = property_object_type(i);
        if (property_read(fd, object, type, property_names[i], &plan->props[i])) return -1;
        printf("SNAPSHOT object=%u property=%s id=%u value=%"PRIu64"\n",object,property_names[i],plan->props[i].id,plan->props[i].old);
    }
    if (!initial_state_allowed(plan)) return rejected("atomic snapshot is not entirely detached/inactive");
    printf("PLAN connector=%u encoder=%u crtc=%u primary_plane=%u 2944x1840@30 duration=%d\n",
        plan->connector,plan->encoder,plan->crtc,plan->plane,SECONDS);
    return 0;
}
static void desired_values(const struct plan *p, uint32_t fb, uint32_t blob, uint64_t values[PROPERTY_COUNT]) {
    const uint64_t initial[PROPERTY_COUNT] = {p->crtc,1,blob,fb,p->crtc,0,0,
        (uint64_t)WIDTH << 16,(uint64_t)HEIGHT << 16,0,0,WIDTH,HEIGHT};
    memcpy(values, initial, sizeof(initial));
}
static int atomic_commit(int fd, const struct plan *p, const uint64_t values[PROPERTY_COUNT], uint32_t flags) {
    uint32_t objects[3] = {p->connector,p->crtc,p->plane};
    uint32_t counts[3] = {1,2,10}, ids[PROPERTY_COUNT];
    for (unsigned i = 0; i < PROPERTY_COUNT; i++) ids[i] = p->props[i].id;
    struct drm_mode_atomic atomic = {0}; atomic.flags = flags; atomic.count_objs = 3;
    atomic.objs_ptr = ptr64(objects); atomic.count_props_ptr = ptr64(counts);
    atomic.props_ptr = ptr64(ids); atomic.prop_values_ptr = ptr64(values);
    return call_ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
}
/* BEGIN PHASE_LOGGING_ONLY_V1: no display/control-flow decisions. */
static int phase_fd = -1;
static unsigned phase_attempted, phase_logged, phase_errors;
static int phase_last_errno;

/* The reviewed owner passes an output pipe and ignores SIGPIPE across exec.
   A fresh open-file-description avoids changing its inherited status flags.
   Unsupported output is a missing/incomplete phase stream, never admission. */
static int phase_writer_open(void) {
    if (phase_fd >= 0) return 0;
    int inherited_flags = fcntl(STDOUT_FILENO,F_GETFL);
    struct stat inherited, opened;
    struct sigaction pipe_action;
    if (inherited_flags < 0 || fstat(STDOUT_FILENO,&inherited) ||
        sigaction(SIGPIPE,NULL,&pipe_action)) return -1;
    if (!S_ISFIFO(inherited.st_mode) || pipe_action.sa_handler != SIG_IGN) {
        errno=ENOTSUP; return -1;
    }
    int fresh=open("/proc/self/fd/1",O_WRONLY|O_NONBLOCK|O_NOCTTY|O_CLOEXEC);
    if (fresh < 0) return -1;
    int flags=fcntl(fresh,F_GETFL);
    if (fstat(fresh,&opened) || !S_ISFIFO(opened.st_mode) ||
        inherited.st_dev!=opened.st_dev || inherited.st_ino!=opened.st_ino ||
        inherited.st_rdev!=opened.st_rdev || flags<0 || !(flags&O_NONBLOCK) ||
        fcntl(STDOUT_FILENO,F_GETFL)!=inherited_flags) {
        int saved=errno; close(fresh); errno=saved?saved:EINVAL; return -1;
    }
    phase_fd=fresh;
    return 0;
}
static void phase_error(int code) {
    phase_errors++; phase_last_errno=code?code:EIO;
}
static int phase_write(const char *line,size_t length) {
    if (phase_writer_open()) return -1;
    /* A single <=256-byte nonblocking pipe write. No EINTR/EAGAIN retry. */
    ssize_t written=write(phase_fd,line,length);
    if (written==(ssize_t)length) return 0;
    if (written>=0) errno=EIO;
    return -1;
}
static void phase_record(const char *label) {
    int saved=errno;
    unsigned sequence=++phase_attempted;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC,&now) || now.tv_sec<0 || now.tv_nsec<0 ||
        now.tv_nsec>=1000000000L ||
        (uint64_t)now.tv_sec>(UINT64_MAX-(uint64_t)now.tv_nsec)/1000000000ULL) {
        phase_error(errno?errno:EOVERFLOW); errno=saved; return;
    }
    uint64_t ns=(uint64_t)now.tv_sec*1000000000ULL+(uint64_t)now.tv_nsec;
    char line[256];
    int n=snprintf(line,sizeof line,"TB_VIEWER_PHASE_V1 seq=%u phase=%s mono_ns=%"PRIu64" pid=%ld mode=%s\n",
                   sequence,label,ns,(long)getpid(),use_status_image?"image":"pattern");
    if (n<=0 || (size_t)n>=sizeof line) phase_error(EOVERFLOW);
    else if (phase_write(line,(size_t)n)) phase_error(errno);
    else phase_logged++;
    errno=saved;
}
static void phase_summary(int original_result) {
    int saved=errno;
    char line[256];
    int n=snprintf(line,sizeof line,"TB_VIEWER_PHASE_SUMMARY_V1 attempted=%u logged=%u errors=%u last_errno=%d original_rc=%d pid=%ld mode=%s\n",
                   phase_attempted,phase_logged,phase_errors,phase_last_errno,original_result,
                   (long)getpid(),use_status_image?"image":"pattern");
    if (n<=0 || (size_t)n>=sizeof line) phase_error(EOVERFLOW);
    else if (phase_write(line,(size_t)n)) phase_error(errno);
    /* This one CLOEXEC writer is released by normal process exit. No changes to
       stdout/stdio, signal dispositions, alarm, return value or restore path. */
    errno=saved;
}
/* END PHASE_LOGGING_ONLY_V1 */

static int run_test(int fd, struct plan *p, bool show) {
    struct drm_mode_create_dumb dumb = {0}; dumb.width = WIDTH; dumb.height = HEIGHT; dumb.bpp = 32;
    uint32_t fb_id = 0, blob_id = 0;
    void *map = MAP_FAILED;
    bool applied = false, attempted = false;
    int result = 1;
    if (call_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)) { fail("CREATE_DUMB"); goto done; }
    if (!buffer_valid(dumb.pitch,dumb.size)) { rejected("unexpected dumb buffer pitch/size"); goto done; }
    struct drm_mode_map_dumb mapping = {0}; mapping.handle = dumb.handle;
    if (call_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mapping)) { fail("MAP_DUMB"); goto done; }
    map = mmap(NULL,(size_t)dumb.size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off_t)mapping.offset);
    if (map == MAP_FAILED) { fail("mmap dumb buffer"); goto done; }
    memset(map,0,(size_t)dumb.size);
    if(use_status_image) {
        if(status_image_load_rows(map,dumb.pitch)){fail("read fixed status image");goto done;}
        puts("STATUS_IMAGE_PAYLOAD_READ exact_dimensions=true X_padding_zero=true");
    } else {
    for (unsigned y = 0; y < HEIGHT; y++) {
        uint32_t *line = (uint32_t *)((uint8_t *)map + (size_t)y * dumb.pitch);
        for (unsigned x = 0; x < WIDTH; x++) line[x] = pixel(x,y);
    }
    }
    struct drm_mode_fb_cmd2 fb = {0}; fb.width = WIDTH; fb.height = HEIGHT; fb.pixel_format = XRGB8888;
    fb.handles[0] = dumb.handle; fb.pitches[0] = dumb.pitch;
    if (call_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb)) { fail("ADDFB2 XRGB8888 linear"); goto done; }
    fb_id = fb.fb_id;
    struct drm_mode_create_blob blob = {0}; blob.data = ptr64(&p->mode); blob.length = sizeof(p->mode);
    if (call_ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob)) { fail("CREATEPROPBLOB"); goto done; }
    blob_id = blob.blob_id;
    uint64_t desired[PROPERTY_COUNT], old[PROPERTY_COUNT]; desired_values(p,fb_id,blob_id,desired);
    for (unsigned i=0;i<PROPERTY_COUNT;i++) old[i]=p->props[i].old;
    puts("IDLE_RECHECK_BEFORE_TEST_ONLY");
    if (saved_idle_state_unchanged(fd,p)) { fail("all-plane idle recheck before TEST_ONLY"); goto done; }
    if (atomic_commit(fd,p,desired,DRM_MODE_ATOMIC_TEST_ONLY|DRM_MODE_ATOMIC_ALLOW_MODESET)) { fail("atomic TEST_ONLY"); goto done; }
    puts("ATOMIC_TEST_ONLY_PASSED display_state_not_applied=true"); fflush(stdout);
    if (!show) { result=0; goto done; }
    /* Confirm that the saved detached state itself is accepted before altering the panel. */
    if (atomic_commit(fd,p,old,DRM_MODE_ATOMIC_TEST_ONLY|DRM_MODE_ATOMIC_ALLOW_MODESET)) { fail("saved-state TEST_ONLY"); goto done; }
    puts("IDLE_RECHECK_BEFORE_APPLY");
    if (saved_idle_state_unchanged(fd,p)) { fail("all-plane idle recheck before apply"); goto done; }
    struct sigaction action = {0}; action.sa_handler=interrupted; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT,&action,NULL) || sigaction(SIGTERM,&action,NULL) || sigaction(SIGALRM,&action,NULL)) { fail("signal setup"); goto done; }
    stop_requested=0; alarm(use_status_image?IMAGE_SECONDS:SECONDS);
    attempted=true;
    phase_record("APPLY_BEGIN");
    if (atomic_commit(fd,p,desired,DRM_MODE_ATOMIC_ALLOW_MODESET)) { phase_record("APPLY_FAILED"); fail("atomic apply"); goto restore; }
    phase_record("APPLY_OK");
    applied=true;
    phase_record("ACTIVE");
    if(use_status_image)puts("IMAGE_ACTIVE max_seconds_from_commit_start=5 no_backlight_sysfs_write=true");
    else puts("PATTERN_ACTIVE max_seconds_from_commit_start=10 no_backlight_sysfs_write=true");
    fflush(stdout);
    while (!stop_requested) { struct timespec delay={0,100000000}; nanosleep(&delay,NULL); }
restore:
    phase_record("RESTORE_BEGIN");
    alarm(0);
    if (attempted) {
        if (atomic_commit(fd,p,old,DRM_MODE_ATOMIC_ALLOW_MODESET)) { phase_record("RESTORE_FAILED"); fail("restore original detached atomic state"); result=3; }
        else {
            phase_record("RESTORE_COMMIT_OK");
            struct drm_mode_crtc verify={0}; verify.crtc_id=p->crtc;
            if (call_ioctl(fd,DRM_IOCTL_MODE_GETCRTC,&verify) || verify.fb_id != 0 || verify.mode_valid != 0) {
                rejected("restored CRTC did not verify inactive"); result=3;
            } else {
                puts("IDLE_RECHECK_AFTER_RESTORE");
                if(saved_idle_state_unchanged(fd,p)==0) {
                    puts("ORIGINAL_INACTIVE_STATE_RESTORED_AND_VERIFIED all_13_atomic_properties=true all_planes_detached=true");result=applied?0:1;
                    phase_record("RESTORE_VERIFIED");
                } else {rejected("restored properties, plane inventory or all-plane idle state differ from snapshot");result=3;}
            }
        }
    }
    phase_record("RESTORE_END");
done:
    phase_record("CLEANUP_BEGIN");
    /* Initial state is constrained to inactive. RMFB is a last-resort plane disable,
       but does not certify exact restoration if the earlier restore failed. */
    if (fb_id && call_ioctl(fd,DRM_IOCTL_MODE_RMFB,&fb_id)) { fail("RMFB cleanup"); result=3; }
    if (blob_id) { struct drm_mode_destroy_blob blob={0};blob.blob_id=blob_id;
        if (call_ioctl(fd,DRM_IOCTL_MODE_DESTROYPROPBLOB,&blob)) {fail("destroy mode blob");result=3;} }
    if (map != MAP_FAILED && munmap(map,(size_t)dumb.size)) {fail("munmap");result=3;}
    if (dumb.handle) {struct drm_mode_destroy_dumb destroy={0};destroy.handle=dumb.handle;
        if(call_ioctl(fd,DRM_IOCTL_MODE_DESTROY_DUMB,&destroy)){fail("destroy dumb buffer");result=3;} }
    puts("RAM_DISPLAY_OBJECT_CLEANUP_FINISHED");
    phase_record("CLEANUP_END");
    phase_summary(result);
    return result;
}
static int self_test(void) {
    struct drm_mode_modeinfo m={0};
    m.clock=770472;m.hdisplay=WIDTH;m.hsync_start=3034;m.hsync_end=3044;m.htotal=3132;
    m.vdisplay=HEIGHT;m.vsync_start=8016;m.vsync_end=8018;m.vtotal=8200;m.vrefresh=30;
    memcpy(m.name,"2944x1840",10);
    if(!mode_allowed(&m))return 1;
    m.vrefresh=120;if(mode_allowed(&m))return 1;m.vrefresh=30;
    m.clock++;if(mode_allowed(&m))return 1;
    if(buffer_valid(WIDTH*4U-4U,(uint64_t)WIDTH*HEIGHT*4U))return 1;
    if(buffer_valid(WIDTH*4U,(uint64_t)WIDTH*HEIGHT*4U-1U))return 1;
    if(buffer_valid(WIDTH*4U,MAX_BUFFER_BYTES+1ULL))return 1;
    if(!buffer_valid(WIDTH*4U,(uint64_t)WIDTH*HEIGHT*4U))return 1;
    struct plan p={0}; if(!initial_state_allowed(&p))return 1;
    p.props[CRTC_ACTIVE].old=1;if(initial_state_allowed(&p))return 1;p.props[CRTC_ACTIVE].old=0;
    struct drm_mode_get_plane plane={0};struct property plane_fb={0},plane_crtc={0};
    if(!plane_state_idle(&plane,&plane_fb,&plane_crtc))return 1;
    plane.possible_crtcs=2;plane.fb_id=7; /* A nonselected overlay/cursor path must also be refused. */
    if(plane_state_idle(&plane,&plane_fb,&plane_crtc))return 1;
    plane.fb_id=0;
    plane.crtc_id=156;
    if(plane_state_idle(&plane,&plane_fb,&plane_crtc))return 1;
    plane.crtc_id=0;
    plane_fb.old=9;
    if(plane_state_idle(&plane,&plane_fb,&plane_crtc))return 1;
    plane_fb.old=0;
    plane_crtc.old=115;
    if(plane_state_idle(&plane,&plane_fb,&plane_crtc))return 1;
    plane_crtc.old=0;
    const uint32_t ids_a[]={41,42,43},ids_reordered[]={43,41,42},ids_changed[]={41,42,44},ids_duplicate[]={41,41,43},ids_zero[]={0,42,43};
    if(!same_plane_set(ids_a,3,ids_reordered,3))return 1;
    if(same_plane_set(ids_a,3,ids_changed,3)||same_plane_set(ids_a,3,ids_duplicate,3)||
       same_plane_set(ids_a,3,ids_zero,3)||same_plane_set(ids_a,3,ids_reordered,2))return 1;
    if(plane_ids_valid(ids_a,0)||plane_ids_valid(ids_a,MAX_OBJECTS+1U))return 1;
    uint64_t values[PROPERTY_COUNT];p.crtc=115;desired_values(&p,9,10,values);
    if(values[CONN_CRTC]!=115 || values[PLANE_FB]!=9 || values[SRC_W]!=((uint64_t)WIDTH<<16) || values[DST_H]!=HEIGHT)return 1;
    mutations_enabled=false;unsigned long before=syscalls_issued;
    if(call_ioctl(-1,DRM_IOCTL_MODE_ATOMIC,NULL)!=-1 || errno!=EPERM || syscalls_issued!=before)return 1;
    if(call_ioctl(-1,DRM_IOCTL_MODE_SETCRTC,NULL)!=-1 || errno!=EPERM || syscalls_issued!=before)return 1;
    mutations_enabled=true;
    if(call_ioctl(-1,DRM_IOCTL_MODE_SETCRTC,NULL)!=-1 || errno!=EPERM || syscalls_issued!=before)return 1;
    mutations_enabled=false;
    for(unsigned y=0;y<HEIGHT;y+=101)for(unsigned x=0;x<WIDTH;x+=101){uint32_t v=pixel(x,y);if((v>>24)||((v>>16)&255)>96||((v>>8)&255)>96||(v&255)>96)return 1;}
    struct stat image_st={0};image_st.st_mode=S_IFREG|0600;image_st.st_uid=0;image_st.st_gid=0;
    image_st.st_size=(off_t)STATUS_IMAGE_BYTES;
    if(!status_file_allowed(&image_st))return 1;
    image_st.st_size--;if(status_file_allowed(&image_st))return 1;image_st.st_size++;
    image_st.st_uid=1;if(status_file_allowed(&image_st))return 1;image_st.st_uid=0;
    image_st.st_gid=1;if(status_file_allowed(&image_st))return 1;image_st.st_gid=0;
    image_st.st_mode=S_IFCHR|0600;if(status_file_allowed(&image_st))return 1;
    image_st.st_mode=S_IFREG|0666;if(status_file_allowed(&image_st))return 1;
    uint8_t image_row[WIDTH*4U]={0};
    if(!status_row_allowed(image_row))return 1;
    image_row[0]=255;image_row[1]=255;image_row[2]=255;
    if(!status_row_allowed(image_row))return 1;
    image_row[3]=1;if(status_row_allowed(image_row))return 1;image_row[3]=0;
    image_row[sizeof(image_row)-1U]=1;if(status_row_allowed(image_row))return 1;
    if(STATUS_IMAGE_BYTES!=21667840U||IMAGE_SECONDS!=5)return 1;
    puts("SELF_TEST_PASSED exact_30Hz_whitelist wrong_clock_high_refresh_rejected buffer_bounds inactive_state all_planes_raw_and_atomic_idle_gate nonselected_plane_rejected plane_inventory_changed_duplicate_zero_and_capacity_rejected atomic_pack read_only_ioctl_gate no_legacy_fallback dim_pixel_bounds device_opens=0 ioctls=0");
    return 0;
}
int main(int argc,char **argv) {
    bool prepare=false,show=false;
    if(argc==2 && strcmp(argv[1],"--self-test")==0)return self_test();
    if(argc==2 && strcmp(argv[1],"--help")==0){
        puts("native-status-image: default/--query = O_RDONLY DRM queries only.\n--image-test-only = fixed status payload plus atomic TEST_ONLY, no applied modeset.\n--show-image = same guards and fixed payload for nominal 5 seconds from commit start; restore initial inactive state.\n--atomic-test-only = allocate RAM display objects and atomic TEST_ONLY, no applied modeset.\n--show-pattern = same validation, then exact DSI 2944x1840@30 mode for at most nominal 10 seconds from commit start; restore initial inactive state.\nNo storage/sysfs/backlight writes, no node creation, no legacy SETCRTC fallback, no arbitrary path/mode/duration. Kernel ioctl hangs cannot be time-bounded by userspace; restoration is best-effort and errors are fatal.");return 0;}
    if(argc==2 && strcmp(argv[1],"--atomic-test-only")==0)prepare=true;
    else if(argc==2 && strcmp(argv[1],"--show-pattern")==0)prepare=show=true;
    else if(argc==2 && strcmp(argv[1],"--image-test-only")==0){prepare=true;use_status_image=true;}
    else if(argc==2 && strcmp(argv[1],"--show-image")==0){prepare=show=true;use_status_image=true;}
    else if(argc!=1 && !(argc==2 && strcmp(argv[1],"--query")==0)){fprintf(stderr,"usage: native-status-image [--query|--image-test-only|--show-image|--atomic-test-only|--show-pattern|--self-test|--help]\n");return 2;}
    mutations_enabled=prepare;
    struct utsname uname_data;
    if(uname(&uname_data)){fail("uname");return 1;}
    if(prepare && (strcmp(uname_data.release,EXPECTED_KERNEL)!=0 || strcmp(uname_data.machine,"aarch64")!=0)) {
        rejected("unexpected kernel/architecture");return 1;
    }
    int fd=open(CARD,(prepare?O_RDWR:O_RDONLY)|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){fail("open exact DRM card");return 1;}
    struct stat st;
    if(fstat(fd,&st)||!S_ISCHR(st.st_mode)||major(st.st_rdev)!=226||minor(st.st_rdev)!=0){rejected("DRM node is not character 226:0");close(fd);return 1;}
    char name[64]={0};struct drm_version version={0};version.name_len=sizeof(name)-1;version.name=name;
    if(call_ioctl(fd,DRM_IOCTL_VERSION,&version)||version.name_len>=sizeof(name)){fail("DRM VERSION");close(fd);return 1;}
    name[version.name_len]='\0';printf("DRIVER %s query_only=%s\n",name,prepare?"false":"true");
    if(prepare && strcmp(name,"mediatek")!=0 && strcmp(name,"mediatek-drm")!=0){rejected("unexpected DRM driver");close(fd);return 1;}
    struct drm_get_cap cap={0};cap.capability=DRM_CAP_DUMB_BUFFER;
    if(call_ioctl(fd,DRM_IOCTL_GET_CAP,&cap)){fail("GET_CAP dumb");close(fd);return 1;}
    printf("DUMB_BUFFER_SUPPORTED %"PRIu64"\n",(uint64_t)cap.value);
    if(prepare && cap.value!=1){rejected("dumb buffers unsupported");close(fd);return 1;}
    if(prepare){struct drm_set_client_cap client={0};client.capability=DRM_CLIENT_CAP_UNIVERSAL_PLANES;client.value=1;
        if(call_ioctl(fd,DRM_IOCTL_SET_CLIENT_CAP,&client)){fail("universal planes capability");close(fd);return 1;}
        client.capability=DRM_CLIENT_CAP_ATOMIC;
        if(call_ioctl(fd,DRM_IOCTL_SET_CLIENT_CAP,&client)){fail("atomic capability; no legacy fallback");close(fd);return 1;}}
    struct plan plan={0};
    if(discover(fd,&plan,prepare)){fail("display plan");close(fd);return 1;}
    int result=prepare?run_test(fd,&plan,show):0;
    if(!prepare)puts("READ_ONLY_QUERY_FINISHED no_allocation=true no_modeset=true");
    if(close(fd)){fail("close DRM");return 3;}return result;
}
