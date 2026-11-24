#ifndef TB_KERNEL_LOG_STREAM_H
#define TB_KERNEL_LOG_STREAM_H
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* PC prototype, not collector or execution admission. Callers supply/own FDs.
 * Source MUST be independently opened: metadata alone cannot prove private OFD.
 * Arm moves only that private source cursor once. No open/close/dup/F_SETFL,
 * console control, clear, write-to-source, actor launch or readiness permission.
 * All potentially blocking calls (including a caller hook) remain unbounded by
 * the kernel. The fixed deadline is a requested observation limit only. */
#define TB_KSTREAM_MAX_BYTES 1048576U
#define TB_KSTREAM_MAX_RECORDS 4096U
#define TB_KSTREAM_WINDOW_MS 12000U
#define TB_KSTREAM_MAX_OPERATIONS 32768U
#define TB_KSTREAM_MAX_POLL_MS 50
/* A conservative PC buffer cap, NOT an ABI-defined maximum. EINVAL means
 * possible dropped/oversize record and is terminal; never resize/retry. */
#define TB_KSTREAM_RECORD_BUFFER 8192U

enum tb_kstream_status {
 TB_KSTREAM_OK=0,TB_KSTREAM_BAD_ARGUMENT,TB_KSTREAM_BAD_SOURCE,
 TB_KSTREAM_BAD_OUTPUT,TB_KSTREAM_CLOCK_ERROR,TB_KSTREAM_SEEK_ERROR,
 TB_KSTREAM_READY_ERROR,TB_KSTREAM_POLL_ERROR,TB_KSTREAM_POLL_FLAGS,
 TB_KSTREAM_READ_ERROR,TB_KSTREAM_EPIPE,TB_KSTREAM_READ_RANGE,
 TB_KSTREAM_PARSE_ERROR,TB_KSTREAM_SEQUENCE_ERROR,TB_KSTREAM_BYTE_CAP,
 TB_KSTREAM_RECORD_CAP,TB_KSTREAM_OPERATION_CAP,TB_KSTREAM_WRITE_ERROR,
 TB_KSTREAM_PARTIAL_WRITE,TB_KSTREAM_FD_CHANGED,TB_KSTREAM_NOT_EXECUTION_READY,TB_KSTREAM_WINDOW_OVERRUN
};
struct tb_kstream_call { int attempted; int64_t result; int error_number; };
struct tb_kstream_record {
 uint64_t output_offset,returned_bytes,written_bytes;
 uint64_t sequence,kernel_timestamp_us,sequence_gap_before;
 uint32_t priority,flags_length,extra_header_fields;
 int parse_valid,sequence_order_valid,flags_prefix_truncated;
 char flags_prefix[32];
 int64_t read_start_ns,read_end_ns;
};
struct tb_kstream_report {
 uint32_t schema,window_ms,cap_bytes,record_cap;
 int status,error_number,complete,source_verified,output_verified;
 int caller_asserted_private_source_ofd,private_source_ofd_independently_proved;
 int source_flags_before,source_flags_after,output_flags_before,output_flags_after;
 int64_t output_offset_before,output_offset_after;
 int64_t start_ns,arm_start_ns,arm_end_ns,ready_start_ns,ready_end_ns,deadline_ns,end_ns;
 struct tb_kstream_call seek_end,ready,last_poll,last_read,last_write,last_clock;
 uint64_t records_read,records_written,returned_bytes,written_bytes;
 uint64_t first_sequence,last_sequence,sequence_gap_total;
 uint32_t operations,poll_calls,read_calls,write_calls;
 int sequence_anchor_available,readiness_acknowledged,deadline_reached;
 int byte_cap_hit,record_cap_hit,operation_cap_hit,epipe_observed;
 int parse_error,truncation_or_oversize_possible,partial_output,poll_fault;
 int read_end_after_deadline;
 int consumed_record_not_fully_preserved;
 short last_poll_revents;
 /* These never become true merely because the component returns OK. */
 int full_history_guaranteed,exact_window_history_guaranteed;
 int kernel_clock_equals_monotonic_verified,causality_established;
 struct tb_kstream_record records[TB_KSTREAM_MAX_RECORDS];
};
typedef int (*tb_kstream_ready_hook)(void *context);
int tb_kstream_observe_fds(int source_fd,int output_fd,int caller_private_source_ofd,
                          tb_kstream_ready_hook ready,void *ready_context,
                          struct tb_kstream_report *report);
const char *tb_kstream_status_name(int status);

#ifdef TB_KSTREAM_TEST
/* Fake source metadata/read/poll/seek/clock only. Output validation uses actual
 * ordinary files on PC tmpfs. No kernel character node or live API is tested. */
struct tb_kstream_ops {
 void *context;
 int (*source_metadata)(void *,int,struct stat *,int *);
 off_t (*source_seek_end)(void *,int);
 int (*source_poll)(void *,int,int,short *);
 ssize_t (*source_read)(void *,int,void *,size_t);
 int (*clock_now)(void *,struct timespec *);
 ssize_t (*output_write)(void *,int,const void *,size_t,off_t);
};
int tb_kstream_observe_fds_test(int,int,int,tb_kstream_ready_hook,void *,
                              struct tb_kstream_report *,const struct tb_kstream_ops *);
#endif
#endif
