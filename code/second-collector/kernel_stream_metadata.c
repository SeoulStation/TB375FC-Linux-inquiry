/* Bounded immutable artifacts after confirmed reader death; no live source IO. */
#define _GNU_SOURCE
#include "qualification.h"
#include "sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
extern bool tb_reviewed_sha256_fd(int,uint64_t,char[65]);
struct json {char *data;size_t size,used;int bad;};
static void add(struct json *o,const char *format,...){
 if(o->bad)return;
 va_list args;va_start(args,format);int n=vsnprintf(o->data+o->used,o->size-o->used,format,args);va_end(args);
 if(n<0||(size_t)n>=o->size-o->used){o->bad=1;return;}o->used+=(size_t)n;
}
static void string(struct json *o,const char *value,size_t cap){
 add(o,"\"");size_t length=strnlen(value,cap);if(length==cap){o->bad=1;return;}
 for(size_t i=0;i<length;i++){unsigned char c=(unsigned char)value[i];if(c=='"'||c=='\\')add(o,"\\%c",c);else if(c<32||c>126)add(o,"\\u%04x",c);else add(o,"%c",c);}add(o,"\"");
}
static void call(struct json *o,const char *name,const struct tb_kstream_call *c){add(o,"\"%s\":{\"attempted\":%d,\"result\":%lld,\"error_number\":%d},",name,c->attempted,(long long)c->result,c->error_number);}
static void core(struct json *o,const struct tb_kstream_report *c){
 add(o,"{");
#define I(n) add(o,"\"" #n "\":%d,",c->n)
#define U(n) add(o,"\"" #n "\":%u,",c->n)
#define L(n) add(o,"\"" #n "\":%lld,",(long long)c->n)
#define V(n) add(o,"\"" #n "\":%llu,",(unsigned long long)c->n)
 U(schema);U(window_ms);U(cap_bytes);U(record_cap);I(status);I(error_number);I(complete);I(source_verified);I(output_verified);
 I(caller_asserted_private_source_ofd);I(private_source_ofd_independently_proved);
 I(source_flags_before);I(source_flags_after);I(output_flags_before);I(output_flags_after);L(output_offset_before);L(output_offset_after);
 L(start_ns);L(arm_start_ns);L(arm_end_ns);L(ready_start_ns);L(ready_end_ns);L(deadline_ns);L(end_ns);
 call(o,"seek_end",&c->seek_end);call(o,"ready",&c->ready);call(o,"last_poll",&c->last_poll);call(o,"last_read",&c->last_read);call(o,"last_write",&c->last_write);call(o,"last_clock",&c->last_clock);
 V(records_read);V(records_written);V(returned_bytes);V(written_bytes);V(first_sequence);V(last_sequence);V(sequence_gap_total);
 U(operations);U(poll_calls);U(read_calls);U(write_calls);I(sequence_anchor_available);I(readiness_acknowledged);I(deadline_reached);
 I(byte_cap_hit);I(record_cap_hit);I(operation_cap_hit);I(epipe_observed);I(parse_error);I(truncation_or_oversize_possible);I(partial_output);I(poll_fault);
 I(read_end_after_deadline);I(consumed_record_not_fully_preserved);I(last_poll_revents);
 I(full_history_guaranteed);I(exact_window_history_guaranteed);I(kernel_clock_equals_monotonic_verified);I(causality_established);
#undef I
#undef U
#undef L
#undef V
 add(o,"\"records\":[");
 for(uint64_t i=0;i<c->records_read&&i<TB_KSTREAM_MAX_RECORDS;i++){
  const struct tb_kstream_record *r=&c->records[i];
  add(o,"%s{\"output_offset\":%llu,\"returned_bytes\":%llu,\"written_bytes\":%llu,\"sequence\":%llu,\"kernel_timestamp_us\":%llu,\"sequence_gap_before\":%llu,\"priority\":%u,\"flags_length\":%u,\"extra_header_fields\":%u,\"parse_valid\":%d,\"sequence_order_valid\":%d,\"flags_prefix_truncated\":%d,\"flags_prefix\":",
      i?",":"",(unsigned long long)r->output_offset,(unsigned long long)r->returned_bytes,(unsigned long long)r->written_bytes,
      (unsigned long long)r->sequence,(unsigned long long)r->kernel_timestamp_us,(unsigned long long)r->sequence_gap_before,
      r->priority,r->flags_length,r->extra_header_fields,r->parse_valid,r->sequence_order_valid,r->flags_prefix_truncated);
  string(o,r->flags_prefix,sizeof r->flags_prefix);add(o,",\"read_start_ns\":%lld,\"read_end_ns\":%lld}",(long long)r->read_start_ns,(long long)r->read_end_ns);
 }add(o,"]}");
}
static void lifecycle(struct json *o,const struct q_stream_state *s){
 int reaped=atomic_load(&s->reaped),ready=atomic_load(&s->ack_sent)&&s->parent_ack_ns>=0&&s->watcher_start_ns>=s->parent_ack_ns;
 int coverage=s->parent_cleanup_ns>=s->parent_ack_ns&&s->parent_cleanup_ns>=0&&s->parent_cleanup_ns<=s->core.deadline_ns&&s->reader_ended_ns>=s->parent_cleanup_ns;
 add(o,"{\"ReaderPID\":%ld,\"ParentPID\":%ld,\"ParentDeathSignalSet\":%s,\"ParentRecheckPassed\":%s,\"OwnershipGoReceived\":%s,",
     (long)s->reader_pid,(long)s->parent_pid,atomic_load(&s->pdeath_set)?"true":"false",atomic_load(&s->parent_rechecked)?"true":"false",atomic_load(&s->go_received)?"true":"false");
 add(o,"\"SourceOpenAttempted\":%s,\"SourceOpened\":%s,\"SourceOpenError\":%d,\"SourceCloseAttempted\":%s,\"SourceCloseReturn\":%d,\"SourceCloseError\":%d,\"SourceClosedNS\":%lld,\"RawWriterCloseReturn\":%d,\"RawWriterCloseError\":%d,",
     atomic_load(&s->source_open_attempted)?"true":"false",atomic_load(&s->source_opened)?"true":"false",s->source_open_error,atomic_load(&s->source_close_attempted)?"true":"false",s->source_close_return,s->source_close_error,s->source_closed_ns,s->raw_close_return,s->raw_close_error);
 add(o,"\"ReadySentNS\":%lld,\"ParentReadyReceivedNS\":%lld,\"ParentReadyAcknowledgedNS\":%lld,\"WatcherStartNS\":%lld,\"ParentCleanupVerifiedNS\":%lld,\"ReaderEndedNS\":%lld,\"ReaderReapedNS\":%lld,\"ReaderExitStatus\":%d,",
     s->ready_sent_ns,s->parent_ready_ns,s->parent_ack_ns,s->watcher_start_ns,s->parent_cleanup_ns,s->reader_ended_ns,s->reaped_ns,s->reader_status);
 add(o,"\"ReaderReaped\":%s,\"ReaderAliveUnknown\":%s,\"ReadyBeforeViewer\":%s,\"WaitAfterCleanup\":%s,\"CleanupWithinReaderWindow\":%s,\"RawSealed\":%s,\"RawSealedNS\":%lld,\"MetadataSealPendingAtSerialization\":true,\"FinalDrainAttempted\":false,\"ExactWindowHistoryGuaranteed\":false,\"KernelClockEqualityVerified\":false}",
     reaped?"true":"false",atomic_load(&s->alive_unknown)?"true":"false",ready?"true":"false",reaped&&s->parent_cleanup_ns>=0&&s->reaped_ns>=s->parent_cleanup_ns?"true":"false",coverage?"true":"false",atomic_load(&s->raw_sealed)?"true":"false",s->raw_sealed_ns);
}
static void error_once(struct q_stream_state *s,const char *value){if(!s->error[0])snprintf(s->error,sizeof s->error,"%s",value);}
static int meta_hash(int fd,uint64_t bytes,char result[65]){
 struct stat before,after;int flags=fcntl(fd,F_GETFL);off_t offset=lseek(fd,0,SEEK_CUR);
 if(bytes>Q_STREAM_META_CAP||flags<0||(flags&O_ACCMODE)!=O_RDONLY||offset!=0||fstat(fd,&before)||!S_ISREG(before.st_mode)||
    before.st_uid||before.st_gid||before.st_nlink!=1||(before.st_mode&07777)!=0400||before.st_size!=(off_t)bytes)return -1;
 unsigned char *buffer=malloc(bytes?(size_t)bytes:1);if(!buffer)return -1;
 if(bytes&&pread(fd,buffer,(size_t)bytes,0)!=(ssize_t)bytes){free(buffer);return -1;}
 if(fstat(fd,&after)||before.st_dev!=after.st_dev||before.st_ino!=after.st_ino||before.st_size!=after.st_size||before.st_mode!=after.st_mode||
    fcntl(fd,F_GETFL)!=flags||lseek(fd,0,SEEK_CUR)!=offset){free(buffer);return -1;}
 uint8_t digest[32];hash_bytes(buffer,(size_t)bytes,digest);free(buffer);hex32(digest,result);return 0;
}
static int seal_hash(int fd,dev_t device,ino_t inode,uint64_t cap,uint64_t *bytes,char sha[65],int metadata){
 struct stat st;int reader=-1,result=-1;
 if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid||st.st_gid||(st.st_mode&07777)!=0600||st.st_nlink!=1||st.st_dev!=device||st.st_ino!=inode||st.st_size<0||(uint64_t)st.st_size>cap)return -1;
 *bytes=(uint64_t)st.st_size;if(fchmod(fd,0400))return -1;
 char held[64];int n=snprintf(held,sizeof held,"/proc/self/fd/%d",fd);if(n<=0||(size_t)n>=sizeof held)return -1;
 reader=open(held,O_RDONLY|O_CLOEXEC);struct stat same;
 if(reader<0||fstat(reader,&same)||same.st_dev!=device||same.st_ino!=inode||(same.st_mode&07777)!=0400)goto done;
 if(metadata?meta_hash(reader,*bytes,sha)!=0:!tb_reviewed_sha256_fd(reader,*bytes,sha))goto done;
 result=0;
done:if(reader>=0&&close(reader))result=-1;return result;
}
int q_stream_finalize_artifacts(struct q_backend *b){
 struct q_stream_state *s=&b->shared->stream;
 if(b->stream.pid>0&&!atomic_load(&s->reaped)){error_once(s,"LIVE_READER_SEAL_REFUSED");return -1;}
 int raw_ok=0,meta_ok=0;
 if(b->stream.rawfd>=0){
  if(!seal_hash(b->stream.rawfd,b->stream.raw_dev,b->stream.raw_ino,TB_KSTREAM_MAX_BYTES,&s->raw_bytes,s->raw_sha,0)){
   s->raw_sealed_ns=q_stream_now_ns();atomic_store_explicit(&s->raw_sealed,1,memory_order_release);raw_ok=1;
  }else error_once(s,"RAW_SEAL_OR_HASH_FAILED");
  if(close(b->stream.rawfd)){raw_ok=0;error_once(s,"RAW_PARENT_CLOSE_FAILED");}b->stream.rawfd=-1;
 }
 char *buffer=malloc(Q_STREAM_META_CAP);if(!buffer){error_once(s,"META_ALLOCATION_FAILED");return -1;}
 struct json o={buffer,Q_STREAM_META_CAP,0,0};
 add(&o,"{\"Schema\":1,\"Kind\":\"TB375FC_KERNEL_STREAM_RECORD_METADATA_V1\",\"BootId\":\"%s\",\"ObserverNonce\":\"%s\",\"ReaderPID\":%ld,\"Raw\":{\"Path\":\"%s\",\"Bytes\":%llu,\"SHA256\":\"%s\",\"Mode\":256},\"Core\":",
     b->boot_id,b->nonce,(long)s->reader_pid,s->raw_path,(unsigned long long)s->raw_bytes,s->raw_sha);
 if(atomic_load_explicit(&s->core_done,memory_order_acquire)&&s->core.records_read<=TB_KSTREAM_MAX_RECORDS)core(&o,&s->core);else add(&o,"null");
 add(&o,",\"Lifecycle\":");lifecycle(&o,s);add(&o,"}\n");
 if(o.bad){error_once(s,"META_SERIALIZATION_CAP");free(buffer);return -1;}
 int dir=open(b->claim,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),fd=-1;struct stat st={0},ds;struct statfs fs;
 if(dir>=0&&!fstat(dir,&ds)&&S_ISDIR(ds.st_mode)&&!ds.st_uid&&!ds.st_gid&&(ds.st_mode&07777)==0700&&
    !fstatfs(dir,&fs)&&(fs.f_type==TMPFS_MAGIC||fs.f_type==RAMFS_MAGIC))
  fd=openat(dir,"kernel-stream-meta.json",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
 if(dir>=0&&close(dir)){error_once(s,"META_DIRECTORY_CLOSE_FAILED");}
 if(fd<0){error_once(s,"META_CREATE_FAILED");free(buffer);return -1;}
 atomic_store_explicit(&s->meta_created,1,memory_order_release);
 int write_ok=!fchmod(fd,0600)&&!fstat(fd,&st)&&S_ISREG(st.st_mode)&&!st.st_uid&&!st.st_gid&&st.st_nlink==1&&st.st_size==0;
 if(write_ok){ssize_t n=write(fd,buffer,o.used);write_ok=n==(ssize_t)o.used;}
 free(buffer);
 if(!seal_hash(fd,st.st_dev,st.st_ino,Q_STREAM_META_CAP,&s->meta_bytes,s->meta_sha,1)){
  s->meta_sealed_ns=q_stream_now_ns();atomic_store_explicit(&s->meta_sealed,1,memory_order_release);meta_ok=write_ok;
 }else error_once(s,"META_SEAL_OR_HASH_FAILED");
 if(close(fd)){meta_ok=0;error_once(s,"META_CLOSE_FAILED");}
 if(!write_ok)error_once(s,"META_WRITE_INCOMPLETE");
 return raw_ok&&meta_ok?0:-1;
}
int q_stream_report_json(const struct q_state *state,char *out,size_t cap){
 const struct q_stream_state *s=&state->stream;struct json o={out,cap,0,0};
 int complete=atomic_load(&s->passed),unknown=atomic_load(&s->fork_attempted)&&!atomic_load(&s->core_done);
 int lifetime=(s->reader_pid<=0||atomic_load(&s->reaped))&&!atomic_load(&s->alive_unknown);
 add(&o,"\"KernelStreamPassed\":%s,\"KernelStream\":{\"Attempted\":%s,\"Complete\":%s,\"Unknown\":%s,\"Error\":",
     complete?"true":"false",atomic_load(&s->attempted)?"true":"false",complete?"true":"false",unknown?"true":"false");string(&o,s->error,sizeof s->error);
 add(&o,",\"ReaderPID\":%ld,\"ReaderReaped\":%s,\"ReaderAliveUnknown\":%s,\"ReaderLifetimeClean\":%s,",
     (long)s->reader_pid,atomic_load(&s->reaped)?"true":"false",atomic_load(&s->alive_unknown)?"true":"false",lifetime?"true":"false");
 add(&o,"\"Raw\":{\"Path\":\"%s\",\"Bytes\":%llu,\"SHA256\":\"%s\",\"Mode\":256,\"Sealed\":%s},\"Metadata\":{\"Path\":\"%s\",\"Bytes\":%llu,\"SHA256\":\"%s\",\"Mode\":256,\"Sealed\":%s},\"MetadataSealedNS\":%lld},",
     s->raw_path,(unsigned long long)s->raw_bytes,s->raw_sha,atomic_load(&s->raw_sealed)?"true":"false",s->meta_path,(unsigned long long)s->meta_bytes,s->meta_sha,atomic_load(&s->meta_sealed)?"true":"false",s->meta_sealed_ns);
 return o.bad?-1:(int)o.used;
}
