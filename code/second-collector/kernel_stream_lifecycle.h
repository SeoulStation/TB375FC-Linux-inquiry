#ifndef TB_SUPERVISED_STREAM_LIFECYCLE_H
#define TB_SUPERVISED_STREAM_LIFECYCLE_H
#include "kernel_log_stream.h"
#include <stdatomic.h>
#define Q_STREAM_META_CAP (2U*1024U*1024U)
#define Q_STREAM_READY_MS 2000
#define Q_STREAM_JOIN_MS 15000
#define Q_STREAM_TERM_MS 1000
#define Q_STREAM_KILL_MS 1000
struct q_stream_state {
 _Atomic int attempted,fork_attempted,pid_committed,ready_received,ack_sent,core_done,failed,reaped,alive_unknown;
 _Atomic int source_open_attempted,source_opened,source_close_attempted,pdeath_set,parent_rechecked,go_received;
 _Atomic int raw_created,raw_sealed,meta_created,meta_sealed,passed;
 pid_t reader_pid,parent_pid;
 int source_open_error,source_close_return,source_close_error,raw_close_return,raw_close_error,reader_status;
 long long ready_sent_ns,parent_ready_ns,parent_ack_ns,watcher_start_ns,parent_cleanup_ns;
 long long reader_ended_ns,source_closed_ns,reaped_ns,raw_sealed_ns,meta_sealed_ns;
 char raw_path[512],meta_path[512],raw_sha[65],meta_sha[65],error[96];
 uint64_t raw_bytes,meta_bytes;
 struct tb_kstream_report core;
};
struct q_stream_owner {
 pid_t pid;
 int pidfd,rawfd,command_write,notice_read,parentfd;
 long long join_deadline_ms;
 dev_t raw_dev;ino_t raw_ino;
};
struct q_backend;
struct q_state;
long long q_stream_now_ns(void);
void q_stream_initialize(struct q_backend *);
int q_stream_start(struct q_backend *);
int q_stream_should_stop(void);
void q_stream_watcher_started(struct q_backend *,long long);
void q_stream_cleanup_verified(struct q_backend *);
int q_stream_cancel_before_viewer_adoption(struct q_backend *);
int q_stream_contain_unknown_watcher(struct q_backend *);
int q_stream_finish(struct q_backend *,int cleanup_verified,int diagnostic_ok);
int q_stream_finalize_artifacts(struct q_backend *);
int q_stream_report_json(const struct q_state *,char *,size_t);
#ifdef PC_FIXTURE
void q_fixture_stream_setup(struct q_backend *,int);
#endif
#endif
