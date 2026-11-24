/* Byte-identical V2 core + small POSIX mapping. No open/dup/F_SETFL/close,
 * source writes, source path selection, guard issuance or actor integration. */
#define _GNU_SOURCE
#ifndef TB_KSTREAM_TEST
#define TB_KSTREAM_TEST 1
#endif
#include "kernel_log_stream_adapter.h"
#include "qualification.h"
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

static int source_metadata(void *ctx,int fd,struct stat *st,int *flags){
 (void)ctx;*flags=fcntl(fd,F_GETFL);if(*flags<0)return -1;return fstat(fd,st);
}
static off_t source_seek_end(void *ctx,int fd){(void)ctx;return lseek(fd,0,SEEK_END);}
static int source_poll(void *ctx,int fd,int timeout,short *events){
 (void)ctx;struct pollfd p={.fd=fd,.events=POLLIN|POLLRDNORM};
 int result=poll(&p,1,timeout);*events=p.revents;return result;
}
static ssize_t source_read(void *ctx,int fd,void *buffer,size_t cap){(void)ctx;return read(fd,buffer,cap);}
static int monotonic_clock(void *ctx,struct timespec *ts){(void)ctx;return clock_gettime(CLOCK_MONOTONIC,ts);}
static ssize_t output_write(void *ctx,int fd,const void *buffer,size_t count,off_t offset){
 (void)ctx;return pwrite(fd,buffer,count,offset);
}
static const struct tb_kstream_ops real_ops __attribute__((unused))={
 NULL,source_metadata,source_seek_end,source_poll,source_read,monotonic_clock,output_write
};
int tb_kstream_adapter_observe_fds(int source,int output,int private_ofd,tb_kstream_ready_hook ready,void *ctx,struct tb_kstream_report *report){
 /* Only the fixed collector reaches this entry after its FD3/boot/first/
  * second-unused/RAM guard and owned reader G handshake. No standalone
  * admission API is added. Historical derivations remain locked. */
#if Q_TEMPLATE_EXECUTION_READY
 return tb_kstream_observe_fds_test(source,output,private_ofd,ready,ctx,report,&real_ops);
#else
 return tb_kstream_observe_fds(source,output,private_ofd,ready,ctx,report);
#endif
}
#ifdef TB_ADAPTER_TEST
int tb_kstream_adapter_observe_fds_test(int source,int output,int private_ofd,tb_kstream_ready_hook ready,void *ctx,struct tb_kstream_report *report){
 return tb_kstream_observe_fds_test(source,output,private_ofd,ready,ctx,report,&real_ops);
}
#endif
