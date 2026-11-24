#ifndef TB_KERNEL_LOG_SNAPSHOT_H
#define TB_KERNEL_LOG_SNAPSHOT_H
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define TB_KLOG_MAX_BYTES 1048576U
enum tb_klog_status {
 TB_KLOG_OK=0, TB_KLOG_BAD_ARGUMENT, TB_KLOG_BAD_FD, TB_KLOG_ROOT_RAM_REQUIRED,
 TB_KLOG_CLOCK_ERROR, TB_KLOG_SIZE_ERROR, TB_KLOG_OVERSIZE, TB_KLOG_ALLOC_ERROR,
 TB_KLOG_READ_ERROR, TB_KLOG_READ_RANGE, TB_KLOG_SIZE_CHANGED,
 TB_KLOG_WRITE_ERROR, TB_KLOG_PARTIAL_WRITE, TB_KLOG_OUTPUT_CHANGED
};
struct tb_klog_call { int attempted; int64_t result; int error_number; };
struct tb_klog_report {
 uint32_t schema, cap_bytes;
 int status, require_root_ram, fd_verified, root_ram_verified;
 int fd_flags_before, fd_flags_after;
 int64_t offset_before, offset_after;
 int64_t start_ns, read_start_ns, read_end_ns, end_ns;
 struct tb_klog_call clock_start, clock_read_start, clock_read_end, clock_end;
 struct tb_klog_call size_before, read_all, size_after, write_output;
 uint64_t returned_bytes, written_bytes;
 int error_number, returned_bytes_fully_written;
 /* These deliberately remain false even when status == TB_KLOG_OK. */
 int full_ring_history_guaranteed, interval_coverage_guaranteed, causal_order_guaranteed;
};
/* Caller owns fd, file naming, exclusive creation and sealing. This function
 * never closes/truncates/seals it and never changes its flags or seek offset.
 * require_root_ram=1 additionally requires root:root, one link, safe permissions
 * and an fd backed by tmpfs/ramfs. Integration must choose that flag explicitly.
 * A failed/partial write is preserved, never silently retried or erased. */
int tb_klog_snapshot_fd(int fd,int require_root_ram,struct tb_klog_report *report);
const char *tb_klog_status_name(int status);

#ifdef TB_KLOG_TEST
/* PC injection seam is absent from ordinary production builds. */
struct tb_klog_test_ops {
 void *context;
 int (*size_buffer)(void *);
 int (*read_all)(void *,char *,size_t);
 int (*clock_now)(void *,struct timespec *);
 ssize_t (*write_at_zero)(void *,int,const void *,size_t);
};
int tb_klog_snapshot_fd_test(int,int,struct tb_klog_report *,const struct tb_klog_test_ops *);
#endif
#endif
