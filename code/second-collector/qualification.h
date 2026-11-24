#ifndef TB_C_DISABLED_QUALIFICATION_H
#define TB_C_DISABLED_QUALIFICATION_H
#define _GNU_SOURCE
#include "kernel_log_snapshot.h"
#include "kernel_stream_lifecycle.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>

#define Q_EVENTS 4
#define Q_CAPTURE_MS 12000
#define Q_VIEWER_CAP 65536
#define Q_VIEWER_GRACE_MS 5000
#define Q_TRACE_CAP (1024*1024)
#define Q_STATS_CAP 65536
#define Q_PROFILES 6
#define Q_PROFILE_CAP 65536
#define Q_FORMAT_CAP 16384
#define Q_PATH_CAP 512
#define Q_MAIN_MS 18000
#define Q_WATCHER_MS 24000
#define Q_TEMPLATE_EXECUTION_READY 1
#define Q_BOUND_BOOT_ID "1a1dabf0-6854-4e36-a1b4-6cc09f5c14a6"
#define Q_MATERIALIZATION_INPUT_SHA "bba24157bdffd92620b0171af34798f4f0fdc32863d489e4271a6cfd8613b79f"
#define Q_VIEWER_SHA "208f8148867cfaa1c162c0a6064beff7e36ff6d0aad28c4888afd8a826db1e75"
#define Q_IMAGE_SHA "a69b3c694382c82dc219fbfa9ee4f14fc6cd3597a1ff26e58aaefc42daf85ecc"
#define Q_VIEWER_PATH "/run/native-tools/native-status-image-phased-208f8148867cfaa1"
#define Q_IMAGE_PATH "/run/native-tools/status-image-20261029-late.bgrx"
#define Q_MODULE_SHA "e6df2e95385bbb7a03ab9f7f90f02faf2ff20614860420b36a6b4e190c1e320c"
#define Q_BTF_SHA "6533e9f8e64f59d5794bd0ab83f057e738702b3e6954eec9cb578a06f44831dc"
#define Q_KERNEL "6.1.25-android14-11-g4528d59d4335-ab11203497"
#ifdef PC_FIXTURE
#define Q_PRIOR_COLD_NONCE "11111111111111111111111111111111"
#define Q_PRIOR_COLD_REPORT_SHA "1111111111111111111111111111111111111111111111111111111111111111"
#else
#define Q_PRIOR_COLD_NONCE "419ddf1b46a6408f9c20ad086ff31b2c"
#define Q_PRIOR_COLD_REPORT_SHA "9479c7701244cc008a1d38627d3c24e32d30feb1af1665f9a2651b84c404ff3b"
#endif
struct q_kernel_record {
 _Atomic int attempted, done, complete, sealed;
 uint64_t bytes;
 char path[Q_PATH_CAP], sha[65], error[64];
 struct tb_klog_report snapshot;
};

struct q_state {
    _Atomic int attempted[Q_EVENTS], committed[Q_EVENTS], foreign[Q_EVENTS];
    _Atomic int fault, cleanup_done, cleanup_ok, other_equal, formats_ok;
    _Atomic int fallback_after_death;
    _Atomic int remaining_or_ambiguous_mask;
    _Atomic int instance_attempted, instance_created, instance_removed;
    _Atomic int enable_attempted_mask, trace_on_attempted;
    _Atomic int capture_ok, capture_cleanup_ok, loss_valid, context_verified, trace_snapshot_attempted;
    dev_t instance_dev; ino_t instance_ino;
    long long capture_start_ms, capture_stop_ms;
    unsigned capture_entries, capture_cpus, total_buffer_kib;
    uint64_t trace_bytes, stats_bytes;
    char trace_path[Q_PATH_CAP], stats_path[Q_PATH_CAP], trace_sha[65], stats_sha[65];
    _Atomic int second_marker_claimed, viewer_fork_attempted, viewer_pid_committed, viewer_exec_allowed;
    _Atomic int viewer_reaped, viewer_output_eof, viewer_output_reading, viewer_output_unknown;
    _Atomic int viewer_alive_unknown, viewer_restore_verified, viewer_cleanup_marker_verified;
    _Atomic int viewer_artifact_ok, viewer_term_attempted, viewer_output_len;
    pid_t viewer_pid;
    int viewer_status;
    char viewer_output[Q_VIEWER_CAP+1];
    char viewer_path[Q_PATH_CAP], viewer_sha[65], profile_path[Q_PATH_CAP], profile_sha[65];
    uint64_t viewer_bytes, profile_bytes;
    uint64_t own_hits[4], own_missed[4];
    _Atomic int own_profile_verified, own_profile_missed_zero;
    _Atomic int drm_node_metadata_verified;
    _Atomic int prior_cold_marker_verified;
    struct q_kernel_record kernel[2];
    struct q_stream_state stream;
    size_t baseline_len[Q_PROFILES], format_len[Q_EVENTS];
    int format_ids[Q_EVENTS];
    char baseline[Q_PROFILES][Q_PROFILE_CAP];
    char formats[Q_EVENTS][Q_FORMAT_CAP];
};
struct q_backend {
    char nonce[33], group[64], root[Q_PATH_CAP], claim[Q_PATH_CAP];
    char mountpoint[Q_PATH_CAP];
    char parent_ns[96];
    int prepared, mounted, parent_pidfd;
    int guard_fd, viewer_exefd, viewer_readfd, viewer_writefd;
    char boot_id[37];
    char root_cold_baseline_report_sha[65];
    struct q_state *shared;
    struct q_stream_owner stream;
};
extern const char *q_events[Q_EVENTS];
extern const char *q_sites[Q_EVENTS];
extern const char *q_fetch[Q_EVENTS];
extern const char *q_profiles[Q_PROFILES];
long long q_now_ms(void);
int q_should_stop(void);
int q_definition(const struct q_backend *, int, char *, size_t);
int q_read_file(const char *, char *, size_t, size_t *);
int q_prior_cold_marker_valid(const char *, const char *);
int q_kernel_snapshot(struct q_backend *,int);
void q_kernel_after_skipped(struct q_backend *);
int q_kernel_report_json(const struct q_state *,char *,size_t);
#ifdef PC_FIXTURE
const struct tb_klog_test_ops *q_fixture_kernel_ops(struct q_backend *,int);
#endif
int q_write_all_once(const char *, const char *, size_t);
int q_find_in_profile(const char *, const char *, char *, size_t);
int q_same_definition(const char *, const char *);
int q_format_valid(int, const char *, size_t);

/* Backend paths/definitions are fixed. There is no arbitrary command input. */
int q_backend_prepare(struct q_backend *, const char *fixture, const char *boot, int guard_fd);
int q_backend_begin(struct q_backend *);
int q_backend_profile(struct q_backend *, int, char *, size_t, size_t *);
int q_backend_find(struct q_backend *, int, char *, size_t);
int q_backend_register(struct q_backend *, int, const char *);
int q_backend_disabled(struct q_backend *, int);
int q_backend_format(struct q_backend *, int, char *, size_t, size_t *);
int q_backend_delete(struct q_backend *, int);
int q_backend_finish(struct q_backend *);
int q_capture_run(struct q_backend *);
int q_capture_cleanup(struct q_backend *);
int q_capture_snapshot(struct q_backend *);
int q_viewer_parent_prepare(struct q_backend *);
void q_viewer_parent_after_fork(struct q_backend *);
int q_viewer_run(struct q_backend *);
int q_viewer_parent_finish(struct q_backend *);
int q_viewer_seal(struct q_backend *);
int q_backend_claim_second_once(struct q_backend *);
int q_profile_names_unambiguous(const char *);
void q_backend_checkpoint(struct q_backend *, const char *, int);
const char *q_backend_name(void);
#endif
