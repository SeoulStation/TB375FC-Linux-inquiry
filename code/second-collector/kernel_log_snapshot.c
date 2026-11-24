/* Read-only printk snapshot. The only kernel-log actions are READ_ALL(3) and
 * SIZE_BUFFER(10). No read-clear, clear, console control, /dev/kmsg or node IO. */
#define _GNU_SOURCE
#include "kernel_log_snapshot.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#ifndef TB_KLOG_TEST
struct tb_klog_test_ops {
 void *context;
 int (*size_buffer)(void *);
 int (*read_all)(void *,char *,size_t);
 int (*clock_now)(void *,struct timespec *);
 ssize_t (*write_at_zero)(void *,int,const void *,size_t);
};
#endif
static int system_size(void *p){(void)p;return klogctl(10,NULL,0);}
static int system_read(void *p,char *buf,size_t n){(void)p;return klogctl(3,buf,(int)n);}
static int system_clock(void *p,struct timespec *ts){(void)p;return clock_gettime(CLOCK_MONOTONIC,ts);}
static ssize_t system_write(void *p,int fd,const void *buf,size_t n){(void)p;return pwrite(fd,buf,n,0);}
static const struct tb_klog_test_ops system_ops={NULL,system_size,system_read,system_clock,system_write};

const char *tb_klog_status_name(int status){
 static const char *const names[]={"OK","BAD_ARGUMENT","BAD_FD","ROOT_RAM_REQUIRED","CLOCK_ERROR",
  "SIZE_ERROR","OVERSIZE","ALLOC_ERROR","READ_ERROR","READ_RANGE","SIZE_CHANGED",
  "WRITE_ERROR","PARTIAL_WRITE","OUTPUT_CHANGED"};
 return status>=0&&(size_t)status<sizeof names/sizeof names[0]?names[status]:"UNKNOWN_STATUS";
}
static int stamp(const struct tb_klog_test_ops *ops,int64_t *out,struct tb_klog_call *call){
 struct timespec ts;errno=0;
 call->attempted=1;call->result=ops->clock_now(ops->context,&ts);call->error_number=errno;
 if(call->result)return -1;
 if(ts.tv_sec<0||ts.tv_nsec<0||ts.tv_nsec>=1000000000L||
    (uint64_t)ts.tv_sec>((uint64_t)INT64_MAX-(uint64_t)ts.tv_nsec)/1000000000U){errno=EOVERFLOW;return -1;}
 *out=(int64_t)ts.tv_sec*1000000000LL+ts.tv_nsec;return 0;
}
static int fail(struct tb_klog_report *r,int status,int error){r->status=status;r->error_number=error;return -1;}
static int regular_empty_fd(int fd,struct stat *st,int *flags,int64_t *offset){
 *flags=fcntl(fd,F_GETFL);
 if(*flags<0)return -1;
 if(((*flags&O_ACCMODE)!=O_WRONLY&&(*flags&O_ACCMODE)!=O_RDWR)||(*flags&(O_APPEND|O_DIRECT|O_PATH))){errno=EINVAL;return -1;}
 if(fstat(fd,st))return -1;
 if(!S_ISREG(st->st_mode)||st->st_size!=0){errno=EINVAL;return -1;}
 off_t pos=lseek(fd,0,SEEK_CUR);if(pos<0)return -1;if(pos!=0){errno=EINVAL;return -1;}
 *offset=pos;return 0;
}
static int snapshot(int fd,int root_ram,struct tb_klog_report *r,const struct tb_klog_test_ops *ops){
 if(!r)return -1;
 memset(r,0,sizeof *r);r->schema=1;r->cap_bytes=TB_KLOG_MAX_BYTES;r->require_root_ram=root_ram;
 r->fd_flags_before=r->fd_flags_after=-1;r->offset_before=r->offset_after=-1;
 r->start_ns=r->read_start_ns=r->read_end_ns=r->end_ns=-1;
 if((root_ram!=0&&root_ram!=1)||!ops||!ops->size_buffer||!ops->read_all||!ops->clock_now||!ops->write_at_zero)
  return fail(r,TB_KLOG_BAD_ARGUMENT,EINVAL);
 if(stamp(ops,&r->start_ns,&r->clock_start))return fail(r,TB_KLOG_CLOCK_ERROR,errno);
 struct stat original,after;char *buffer=NULL;int status=TB_KLOG_OK,error=0;
 if(regular_empty_fd(fd,&original,&r->fd_flags_before,&r->offset_before)){status=TB_KLOG_BAD_FD;error=errno;goto done;}
 r->fd_verified=1;
 if(root_ram){
  struct statfs fs;
  if(original.st_uid||original.st_gid||original.st_nlink!=1||(original.st_mode&0022)||
     fstatfs(fd,&fs)||(fs.f_type!=TMPFS_MAGIC&&fs.f_type!=RAMFS_MAGIC)){
   status=TB_KLOG_ROOT_RAM_REQUIRED;error=errno;goto done;
  }
  r->root_ram_verified=1;
 }
 errno=0;r->size_before.attempted=1;r->size_before.result=ops->size_buffer(ops->context);r->size_before.error_number=errno;
 if(r->size_before.result<=0){status=TB_KLOG_SIZE_ERROR;error=r->size_before.error_number;goto done;}
 if(r->size_before.result>TB_KLOG_MAX_BYTES){status=TB_KLOG_OVERSIZE;goto done;}
 buffer=malloc((size_t)r->size_before.result);
 if(!buffer){status=TB_KLOG_ALLOC_ERROR;error=ENOMEM;goto done;}
 if(stamp(ops,&r->read_start_ns,&r->clock_read_start)||r->read_start_ns<r->start_ns){status=TB_KLOG_CLOCK_ERROR;error=errno;goto done;}
 errno=0;r->read_all.attempted=1;r->read_all.result=ops->read_all(ops->context,buffer,(size_t)r->size_before.result);r->read_all.error_number=errno;
 if(stamp(ops,&r->read_end_ns,&r->clock_read_end)||r->read_end_ns<r->read_start_ns){status=TB_KLOG_CLOCK_ERROR;error=errno;goto done;}
 if(r->read_all.result<0){status=TB_KLOG_READ_ERROR;error=r->read_all.error_number;goto done;}
 if(r->read_all.result>r->size_before.result){status=TB_KLOG_READ_RANGE;goto done;}
 r->returned_bytes=(uint64_t)r->read_all.result;
 errno=0;r->size_after.attempted=1;r->size_after.result=ops->size_buffer(ops->context);r->size_after.error_number=errno;
 if(r->size_after.result<=0){status=TB_KLOG_SIZE_ERROR;error=r->size_after.error_number;goto done;}
 if(r->size_after.result!=r->size_before.result){status=TB_KLOG_SIZE_CHANGED;goto done;}
 /* Recheck before the sole write. Concurrent callers/root mutations are not a
  * supported ownership model; even detected failures never erase their data. */
 if(fstat(fd,&after)||after.st_dev!=original.st_dev||after.st_ino!=original.st_ino||after.st_size!=0||
    fcntl(fd,F_GETFL)!=r->fd_flags_before||lseek(fd,0,SEEK_CUR)!=r->offset_before){status=TB_KLOG_OUTPUT_CHANGED;error=errno;goto done;}
 if(r->returned_bytes){
  errno=0;r->write_output.attempted=1;r->write_output.result=ops->write_at_zero(ops->context,fd,buffer,(size_t)r->returned_bytes);r->write_output.error_number=errno;
  if(r->write_output.result<0){status=TB_KLOG_WRITE_ERROR;error=r->write_output.error_number;goto done;}
  r->written_bytes=(uint64_t)r->write_output.result;
  if(r->written_bytes!=r->returned_bytes){status=TB_KLOG_PARTIAL_WRITE;goto done;}
 }
 r->returned_bytes_fully_written=1;
done:
 free(buffer);
 if(r->fd_verified){
  r->fd_flags_after=fcntl(fd,F_GETFL);r->offset_after=lseek(fd,0,SEEK_CUR);
  if(status==TB_KLOG_OK&&(fstat(fd,&after)||after.st_dev!=original.st_dev||after.st_ino!=original.st_ino||
     after.st_size!=(off_t)r->written_bytes||r->fd_flags_after!=r->fd_flags_before||r->offset_after!=r->offset_before)){
   status=TB_KLOG_OUTPUT_CHANGED;error=errno;
  }
 }
 if(stamp(ops,&r->end_ns,&r->clock_end)||r->end_ns<r->start_ns||(r->read_end_ns>=0&&r->end_ns<r->read_end_ns)){
  if(status==TB_KLOG_OK){status=TB_KLOG_CLOCK_ERROR;error=errno;}
 }
 r->status=status;r->error_number=error;return status==TB_KLOG_OK?0:-1;
}
int tb_klog_snapshot_fd(int fd,int root_ram,struct tb_klog_report *r){return snapshot(fd,root_ram,r,&system_ops);}
#ifdef TB_KLOG_TEST
int tb_klog_snapshot_fd_test(int fd,int root_ram,struct tb_klog_report *r,const struct tb_klog_test_ops *ops){return snapshot(fd,root_ram,r,ops);}
#endif
