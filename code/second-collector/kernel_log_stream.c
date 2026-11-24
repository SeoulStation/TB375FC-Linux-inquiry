/* PC-only source-reader prototype. The public live entry is deliberately
 * locked. The actual core runs only with explicit fake operations in tests. */
#define _GNU_SOURCE
#include "kernel_log_stream.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <poll.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

const char *tb_kstream_status_name(int s){
 static const char *const n[]={"OK","BAD_ARGUMENT","BAD_SOURCE","BAD_OUTPUT","CLOCK_ERROR","SEEK_ERROR",
  "READY_ERROR","POLL_ERROR","POLL_FLAGS","READ_ERROR","EPIPE","READ_RANGE","PARSE_ERROR","SEQUENCE_ERROR",
  "BYTE_CAP","RECORD_CAP","OPERATION_CAP","WRITE_ERROR","PARTIAL_WRITE","FD_CHANGED","NOT_EXECUTION_READY","WINDOW_OVERRUN"};
 return s>=0&&(size_t)s<sizeof n/sizeof n[0]?n[s]:"UNKNOWN_STATUS";
}
static void init(struct tb_kstream_report *r,int private_ofd){
 memset(r,0,sizeof *r);r->schema=1;r->window_ms=TB_KSTREAM_WINDOW_MS;r->cap_bytes=TB_KSTREAM_MAX_BYTES;r->record_cap=TB_KSTREAM_MAX_RECORDS;
 r->caller_asserted_private_source_ofd=private_ofd;
 r->source_flags_before=r->source_flags_after=r->output_flags_before=r->output_flags_after=-1;
 r->output_offset_before=r->output_offset_after=-1;
 r->start_ns=r->arm_start_ns=r->arm_end_ns=r->ready_start_ns=r->ready_end_ns=r->deadline_ns=r->end_ns=-1;
}
int tb_kstream_observe_fds(int source,int output,int private_ofd,tb_kstream_ready_hook ready,void *ctx,struct tb_kstream_report *r){
 (void)source;(void)output;(void)ready;(void)ctx;
 if(!r){errno=EINVAL;return -1;}
 init(r,private_ofd);r->status=TB_KSTREAM_NOT_EXECUTION_READY;r->error_number=EPERM;errno=EPERM;return -1;
}

#ifdef TB_KSTREAM_TEST
static int set_error(struct tb_kstream_report *r,int status,int error){r->status=status;r->error_number=error;return -1;}
static int stamp(const struct tb_kstream_ops *ops,struct tb_kstream_report *r,int64_t *out,int64_t *last){
 struct timespec ts;errno=0;r->last_clock.attempted=1;r->last_clock.result=ops->clock_now(ops->context,&ts);r->last_clock.error_number=errno;
 if(r->last_clock.result)return -1;
 if(ts.tv_sec<0||ts.tv_nsec<0||ts.tv_nsec>=1000000000L||
    (uint64_t)ts.tv_sec>((uint64_t)INT64_MAX-(uint64_t)ts.tv_nsec)/1000000000U){errno=EOVERFLOW;return -1;}
 int64_t now=(int64_t)ts.tv_sec*1000000000LL+ts.tv_nsec;
 if(*last>=0&&now<*last){errno=ERANGE;return -1;}
 *last=now;*out=now;return 0;
}
static int source_valid(int source,const struct tb_kstream_ops *ops,struct stat *st,int *flags){
 errno=0;if(ops->source_metadata(ops->context,source,st,flags))return -1;
 if(!S_ISCHR(st->st_mode)||major(st->st_rdev)!=1||minor(st->st_rdev)!=11||st->st_uid||st->st_gid||
    (st->st_mode&07777)!=0600||(*flags&O_ACCMODE)!=O_RDONLY||!(*flags&O_NONBLOCK)||
    (*flags&(O_APPEND|O_DIRECT|O_PATH|O_ASYNC))){errno=EINVAL;return -1;}
 return 0;
}
static int output_valid(int output,struct stat *st,int *flags,int64_t *offset){
 *flags=fcntl(output,F_GETFL);if(*flags<0)return -1;
 if(((*flags&O_ACCMODE)!=O_WRONLY&&(*flags&O_ACCMODE)!=O_RDWR)||(*flags&(O_APPEND|O_DIRECT|O_PATH))){errno=EINVAL;return -1;}
 struct statfs fs;
 if(fstat(output,st)||fstatfs(output,&fs))return -1;
 if(!S_ISREG(st->st_mode)||st->st_uid||st->st_gid||(st->st_mode&07777)!=0600||st->st_nlink!=1||st->st_size!=0||
    (fs.f_type!=TMPFS_MAGIC&&fs.f_type!=RAMFS_MAGIC)){errno=EINVAL;return -1;}
 off_t pos=lseek(output,0,SEEK_CUR);if(pos<0)return -1;
 if(pos!=0){errno=EINVAL;return -1;}*offset=pos;return 0;
}
static int number(const unsigned char **p,const unsigned char *end,uint64_t max,uint64_t *out,unsigned char delimiter){
 uint64_t value=0;const unsigned char *start=*p;
 while(*p<end&&**p>='0'&&**p<='9'){
  unsigned digit=**p-'0';if(value>(max-digit)/10)return -1;value=value*10+digit;(*p)++;
 }
 if(*p==start||*p>=end||**p!=delimiter)return -1;
 (*p)++;*out=value;return 0;
}
static int record_header(const unsigned char *raw,size_t len,struct tb_kstream_record *record){
 if(!len||raw[len-1]!='\n'||memchr(raw,0,len))return -1;
 const unsigned char *p=raw,*end=raw+len;uint64_t priority;
 if(number(&p,end,UINT_MAX,&priority,',')||number(&p,end,UINT64_MAX,&record->sequence,',')||
    number(&p,end,UINT64_MAX,&record->kernel_timestamp_us,','))return -1;
 const unsigned char *delimiter=memchr(p,';',(size_t)(end-p));
 if(!delimiter||delimiter==p)return -1;
 const unsigned char *flag_end=delimiter;
 for(const unsigned char *q=p;q<delimiter;q++){
  if(*q<0x20||*q>0x7e)return -1;
  if(*q==','){if(flag_end==delimiter)flag_end=q;record->extra_header_fields++;}
 }
 if(flag_end==p)return -1;
 record->flags_length=(uint32_t)(flag_end-p);
 size_t copy=record->flags_length;if(copy>=sizeof record->flags_prefix){copy=sizeof record->flags_prefix-1;record->flags_prefix_truncated=1;}
 memcpy(record->flags_prefix,p,copy);record->priority=(uint32_t)priority;record->parse_valid=1;return 0;
}
static int same_source(const struct stat *a,const struct stat *b){
 return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_rdev==b->st_rdev&&a->st_uid==b->st_uid&&a->st_gid==b->st_gid&&a->st_mode==b->st_mode;
}
static int output_unchanged(int fd,const struct stat *original,int flags,int64_t offset,uint64_t bytes){
 struct stat st;
 return !fstat(fd,&st)&&st.st_dev==original->st_dev&&st.st_ino==original->st_ino&&st.st_uid==original->st_uid&&
        st.st_gid==original->st_gid&&st.st_mode==original->st_mode&&st.st_nlink==1&&st.st_size==(off_t)bytes&&
        fcntl(fd,F_GETFL)==flags&&lseek(fd,0,SEEK_CUR)==offset;
}
int tb_kstream_observe_fds_test(int source,int output,int private_ofd,tb_kstream_ready_hook ready,void *ready_ctx,
                              struct tb_kstream_report *r,const struct tb_kstream_ops *ops){
 if(!r){errno=EINVAL;return -1;}
 init(r,private_ofd);
 if(private_ofd!=1||source<0||source==output||!ready||!ops||!ops->source_metadata||!ops->source_seek_end||
    !ops->source_poll||!ops->source_read||!ops->clock_now||!ops->output_write)return set_error(r,TB_KSTREAM_BAD_ARGUMENT,EINVAL);
 int64_t last=-1,now=-1;struct stat src_before,src_after,out_before;unsigned char buffer[TB_KSTREAM_RECORD_BUFFER];
 if(stamp(ops,r,&r->start_ns,&last))return set_error(r,TB_KSTREAM_CLOCK_ERROR,errno);
 if(source_valid(source,ops,&src_before,&r->source_flags_before)){set_error(r,TB_KSTREAM_BAD_SOURCE,errno);goto done;}
 r->source_verified=1;
 if(output_valid(output,&out_before,&r->output_flags_before,&r->output_offset_before)){set_error(r,TB_KSTREAM_BAD_OUTPUT,errno);goto done;}
 r->output_verified=1;
 if(stamp(ops,r,&r->arm_start_ns,&last)){set_error(r,TB_KSTREAM_CLOCK_ERROR,errno);goto done;}
 errno=0;r->seek_end.attempted=1;r->seek_end.result=ops->source_seek_end(ops->context,source);r->seek_end.error_number=errno;
 if(stamp(ops,r,&r->arm_end_ns,&last)){set_error(r,TB_KSTREAM_CLOCK_ERROR,errno);goto done;}
 if(r->seek_end.result!=0){set_error(r,TB_KSTREAM_SEEK_ERROR,r->seek_end.error_number);goto done;}
 if(r->arm_end_ns>INT64_MAX-(int64_t)TB_KSTREAM_WINDOW_MS*1000000LL){set_error(r,TB_KSTREAM_CLOCK_ERROR,EOVERFLOW);goto done;}
 r->deadline_ns=r->arm_end_ns+(int64_t)TB_KSTREAM_WINDOW_MS*1000000LL;
 if(stamp(ops,r,&r->ready_start_ns,&last)){set_error(r,TB_KSTREAM_CLOCK_ERROR,errno);goto done;}
 errno=0;r->ready.attempted=1;r->ready.result=ready(ready_ctx);r->ready.error_number=errno;
 if(stamp(ops,r,&r->ready_end_ns,&last)){set_error(r,TB_KSTREAM_CLOCK_ERROR,errno);goto done;}
 if(r->ready.result){set_error(r,TB_KSTREAM_READY_ERROR,r->ready.error_number);goto done;}
 r->readiness_acknowledged=1;
 if(r->ready_end_ns>=r->deadline_ns){r->deadline_reached=1;set_error(r,TB_KSTREAM_READY_ERROR,ETIMEDOUT);goto done;}
 for(;;){
  if(stamp(ops,r,&now,&last)){set_error(r,TB_KSTREAM_CLOCK_ERROR,errno);break;}
  if(now>=r->deadline_ns){r->deadline_reached=1;break;}
  if(r->operations++>=TB_KSTREAM_MAX_OPERATIONS){r->operation_cap_hit=1;set_error(r,TB_KSTREAM_OPERATION_CAP,EOVERFLOW);break;}
  /* Reserve one full conservative read buffer: never consume a record that
   * cannot fit solely because of this component's output cap. */
  if(TB_KSTREAM_MAX_BYTES-r->written_bytes<TB_KSTREAM_RECORD_BUFFER){r->byte_cap_hit=1;set_error(r,TB_KSTREAM_BYTE_CAP,EOVERFLOW);break;}
  if(r->records_read>=TB_KSTREAM_MAX_RECORDS){r->record_cap_hit=1;set_error(r,TB_KSTREAM_RECORD_CAP,EOVERFLOW);break;}
  int64_t remain=r->deadline_ns-now;
  int timeout=(int)((remain+999999)/1000000);if(timeout>TB_KSTREAM_MAX_POLL_MS)timeout=TB_KSTREAM_MAX_POLL_MS;
  short events=0;errno=0;r->poll_calls++;r->last_poll.attempted=1;
  r->last_poll.result=ops->source_poll(ops->context,source,timeout,&events);r->last_poll.error_number=errno;r->last_poll_revents=events;
  if(r->last_poll.result<0){r->poll_fault=1;set_error(r,TB_KSTREAM_POLL_ERROR,r->last_poll.error_number);break;}
  if(r->last_poll.result>1||(!r->last_poll.result&&events)||
     (events&~(POLLIN|POLLRDNORM|POLLERR|POLLPRI))){r->poll_fault=1;set_error(r,TB_KSTREAM_POLL_FLAGS,EINVAL);break;}
  if(!r->last_poll.result)continue;
  if(!events){r->poll_fault=1;set_error(r,TB_KSTREAM_POLL_FLAGS,EINVAL);break;}
  if(events&(POLLERR|POLLPRI))r->poll_fault=1;
  if(stamp(ops,r,&now,&last)){set_error(r,TB_KSTREAM_CLOCK_ERROR,errno);break;}
  if(now>=r->deadline_ns){r->deadline_reached=1;break;}
  struct tb_kstream_record *record=&r->records[r->records_read];record->output_offset=r->written_bytes;
  if(stamp(ops,r,&record->read_start_ns,&last)){set_error(r,TB_KSTREAM_CLOCK_ERROR,errno);break;}
  errno=0;r->read_calls++;r->last_read.attempted=1;r->last_read.result=ops->source_read(ops->context,source,buffer,sizeof buffer);r->last_read.error_number=errno;
  int read_clock_error=stamp(ops,r,&record->read_end_ns,&last)?(errno?errno:EIO):0;
  if(record->read_end_ns>=r->deadline_ns)r->read_end_after_deadline=1;
  if(r->last_read.result<0){
   if(read_clock_error){set_error(r,TB_KSTREAM_CLOCK_ERROR,read_clock_error);break;}
   if(r->last_read.error_number==EPIPE){r->epipe_observed=1;set_error(r,TB_KSTREAM_EPIPE,EPIPE);break;}
   if(events&(POLLERR|POLLPRI)){r->poll_fault=1;set_error(r,TB_KSTREAM_POLL_FLAGS,r->last_read.error_number);break;}
   if(r->last_read.error_number==EAGAIN||r->last_read.error_number==EWOULDBLOCK){
    if(r->read_end_after_deadline){r->deadline_reached=1;set_error(r,TB_KSTREAM_WINDOW_OVERRUN,ETIMEDOUT);break;}
    continue;
   }
   if(r->last_read.error_number==EINVAL)r->truncation_or_oversize_possible=1;
   set_error(r,TB_KSTREAM_READ_ERROR,r->last_read.error_number);break;
  }
  if(r->last_read.result<=0||r->last_read.result>(int64_t)sizeof buffer){r->truncation_or_oversize_possible=1;r->consumed_record_not_fully_preserved=r->last_read.result>0;set_error(r,TB_KSTREAM_READ_RANGE,EOVERFLOW);break;}
  record->returned_bytes=(uint64_t)r->last_read.result;r->records_read++;r->returned_bytes+=record->returned_bytes;
  if(record->returned_bytes>TB_KSTREAM_MAX_BYTES-r->written_bytes){r->byte_cap_hit=1;r->consumed_record_not_fully_preserved=1;set_error(r,TB_KSTREAM_BYTE_CAP,EOVERFLOW);break;}
  int parse_ok=record_header(buffer,(size_t)record->returned_bytes,record)==0;
  if(!parse_ok)r->parse_error=1;
  if(parse_ok){
   record->sequence_order_valid=1;
   if(!r->sequence_anchor_available){r->first_sequence=record->sequence;r->sequence_anchor_available=1;}
   else if(record->sequence<=r->last_sequence){record->sequence_order_valid=0;}
   else if(record->sequence-r->last_sequence>1){record->sequence_gap_before=record->sequence-r->last_sequence-1;record->sequence_order_valid=0;r->sequence_gap_total+=record->sequence_gap_before;}
   r->last_sequence=record->sequence;
  }
  if(!output_unchanged(output,&out_before,r->output_flags_before,r->output_offset_before,r->written_bytes)){
   r->consumed_record_not_fully_preserved=1;set_error(r,TB_KSTREAM_FD_CHANGED,errno?errno:EINVAL);break;
  }
  errno=0;r->write_calls++;r->last_write.attempted=1;
  r->last_write.result=ops->output_write(ops->context,output,buffer,(size_t)record->returned_bytes,(off_t)r->written_bytes);r->last_write.error_number=errno;
  if(r->last_write.result>0){record->written_bytes=(uint64_t)r->last_write.result;r->written_bytes+=record->written_bytes;}
  if(r->last_write.result<0){r->partial_output=1;r->consumed_record_not_fully_preserved=1;set_error(r,TB_KSTREAM_WRITE_ERROR,r->last_write.error_number);break;}
  if(record->written_bytes!=record->returned_bytes){r->partial_output=1;r->consumed_record_not_fully_preserved=1;set_error(r,TB_KSTREAM_PARTIAL_WRITE,EIO);break;}
  r->records_written++;
  if(read_clock_error){set_error(r,TB_KSTREAM_CLOCK_ERROR,read_clock_error);break;}
  if(!parse_ok){r->parse_error=1;set_error(r,TB_KSTREAM_PARSE_ERROR,EINVAL);break;}
  if(!record->sequence_order_valid){set_error(r,TB_KSTREAM_SEQUENCE_ERROR,EOVERFLOW);break;}
  if(events&(POLLERR|POLLPRI)){r->poll_fault=1;set_error(r,TB_KSTREAM_POLL_FLAGS,EIO);break;}
  if(r->read_end_after_deadline){r->deadline_reached=1;set_error(r,TB_KSTREAM_WINDOW_OVERRUN,ETIMEDOUT);break;}
 }
 if(r->status==TB_KSTREAM_OK&&r->read_end_after_deadline)set_error(r,TB_KSTREAM_WINDOW_OVERRUN,ETIMEDOUT);
 if(r->status==TB_KSTREAM_OK&&!r->sequence_anchor_available)set_error(r,TB_KSTREAM_SEQUENCE_ERROR,ENODATA);
done:
 if(r->source_verified){
  int flags=-1;
  if(source_valid(source,ops,&src_after,&flags)||!same_source(&src_before,&src_after)||flags!=r->source_flags_before){
   if(r->status==TB_KSTREAM_OK)set_error(r,TB_KSTREAM_FD_CHANGED,errno?errno:EINVAL);
  }
  r->source_flags_after=flags;
 }
 if(r->output_verified){
  r->output_flags_after=fcntl(output,F_GETFL);r->output_offset_after=lseek(output,0,SEEK_CUR);
  if(!output_unchanged(output,&out_before,r->output_flags_before,r->output_offset_before,r->written_bytes)){
   if(r->status==TB_KSTREAM_OK)set_error(r,TB_KSTREAM_FD_CHANGED,errno?errno:EINVAL);
  }
 }
 if(stamp(ops,r,&r->end_ns,&last)&&r->status==TB_KSTREAM_OK)set_error(r,TB_KSTREAM_CLOCK_ERROR,errno);
 r->complete=r->status==TB_KSTREAM_OK&&r->deadline_reached&&r->readiness_acknowledged&&r->sequence_anchor_available&&
             r->records_read==r->records_written&&r->returned_bytes==r->written_bytes;
 return r->status==TB_KSTREAM_OK?0:-1;
}
#endif
