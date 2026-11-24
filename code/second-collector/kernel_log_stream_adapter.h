#ifndef TB_KERNEL_LOG_STREAM_ADAPTER_H
#define TB_KERNEL_LOG_STREAM_ADAPTER_H
#include "kernel_log_stream.h"
/* No opener, actor or admission mechanism is implemented. The public entry
 * remains locked, regardless of caller arguments. Future collector integration
 * requires a separate reviewed derivative and external admission. */
int tb_kstream_adapter_observe_fds(int,int,int,tb_kstream_ready_hook,void *,struct tb_kstream_report *);
#ifdef TB_ADAPTER_TEST
/* Test entry exercises real syscall mappings, but the fixture link-wraps every
 * source operation. Never call this against a device in this PC prototype. */
int tb_kstream_adapter_observe_fds_test(int,int,int,tb_kstream_ready_hook,void *,struct tb_kstream_report *);
#endif
#endif
