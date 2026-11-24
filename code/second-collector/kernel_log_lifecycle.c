/* Caller-owned, exclusive artifacts. Never skip display/trace cleanup on error. */
#define _GNU_SOURCE
#include "qualification.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
extern bool tb_reviewed_sha256_fd(int,uint64_t,char[65]);

static void error_once(struct q_kernel_record *r,const char *value){if(!r->error[0])snprintf(r->error,sizeof r->error,"%s",value);}
void q_kernel_after_skipped(struct q_backend *b){
 struct q_kernel_record *r=&b->shared->kernel[1];
 const char suffix[]="/kernel-after.txt";size_t n=strnlen(b->claim,sizeof b->claim);
 if(n+sizeof suffix<=sizeof r->path){memcpy(r->path,b->claim,n);memcpy(r->path+n,suffix,sizeof suffix);}
 error_once(r,"PREREQUISITE_CLEANUP_INCOMPLETE");
 atomic_store_explicit(&r->done,1,memory_order_release);
}
int q_kernel_snapshot(struct q_backend *b,int which){
 if(which<0||which>1)return -1;
 struct q_kernel_record *r=&b->shared->kernel[which];int expected=0;
 if(!atomic_compare_exchange_strong_explicit(&r->attempted,&expected,1,memory_order_acq_rel,memory_order_acquire))
  return atomic_load_explicit(&r->done,memory_order_acquire)&&atomic_load(&r->complete)?0:-1;
 const char *name=which?"kernel-after.txt":"kernel-before.txt";
 int n=snprintf(r->path,sizeof r->path,"%s/%s",b->claim,name);
 r->snapshot.schema=1;r->snapshot.cap_bytes=TB_KLOG_MAX_BYTES;r->snapshot.status=TB_KLOG_BAD_FD;
 r->snapshot.fd_flags_before=r->snapshot.fd_flags_after=-1;r->snapshot.offset_before=r->snapshot.offset_after=-1;
 r->snapshot.start_ns=r->snapshot.read_start_ns=r->snapshot.read_end_ns=r->snapshot.end_ns=-1;
 int dir=-1,fd=-1,reader=-1,api_ok=0,artifact_ok=0;struct stat ds,created,after;
 if(n<=0||(size_t)n>=sizeof r->path){error_once(r,"OUTPUT_PATH_FAILED");goto done;}
 q_backend_checkpoint(b,which?"kernel_after":"kernel_before",-1);
 dir=open(b->claim,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
 if(dir<0||fstat(dir,&ds)||!S_ISDIR(ds.st_mode)||ds.st_uid||ds.st_gid||(ds.st_mode&0777)!=0700){error_once(r,"OUTPUT_DIRECTORY_FAILED");goto done;}
#ifndef PC_FIXTURE
 struct statfs fs;
 if(fstatfs(dir,&fs)||(fs.f_type!=TMPFS_MAGIC&&fs.f_type!=RAMFS_MAGIC)){error_once(r,"OUTPUT_RAM_REQUIRED");goto done;}
#endif
 fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
 if(fd<0){error_once(r,"OUTPUT_CREATE_FAILED");r->snapshot.error_number=errno;goto done;}
 if(fchmod(fd,0600)||fstat(fd,&created)||!S_ISREG(created.st_mode)||created.st_uid||created.st_gid||created.st_nlink!=1||created.st_size!=0){error_once(r,"OUTPUT_INITIAL_METADATA_FAILED");goto done;}
#ifdef PC_FIXTURE
 api_ok=tb_klog_snapshot_fd_test(fd,0,&r->snapshot,q_fixture_kernel_ops(b,which))==0;
#else
 api_ok=tb_klog_snapshot_fd(fd,1,&r->snapshot)==0;
#endif
 if(!api_ok)error_once(r,tb_klog_status_name(r->snapshot.status));
 /* Even a known failed/partial snapshot is retained and sealed when possible.
  * Complete stays false; its SHA is evidence of that partial artifact only. */
 if(fstat(fd,&after)||!S_ISREG(after.st_mode)||after.st_uid||after.st_gid||after.st_nlink!=1||
    after.st_dev!=created.st_dev||after.st_ino!=created.st_ino||after.st_size<0||after.st_size>TB_KLOG_MAX_BYTES){error_once(r,"OUTPUT_FINAL_METADATA_FAILED");goto done;}
 r->bytes=(uint64_t)after.st_size;
 if(api_ok&&r->bytes!=r->snapshot.returned_bytes){api_ok=0;error_once(r,"OUTPUT_LENGTH_MISMATCH");}
 if(fchmod(fd,0400)){error_once(r,"OUTPUT_SEAL_FAILED");goto done;}
 char held[64];n=snprintf(held,sizeof held,"/proc/self/fd/%d",fd);
 if(n<=0||(size_t)n>=sizeof held){error_once(r,"OUTPUT_REOPEN_FAILED");goto done;}
 reader=open(held,O_RDONLY|O_CLOEXEC);
 if(reader<0||fstat(reader,&after)||after.st_dev!=created.st_dev||after.st_ino!=created.st_ino||
    (after.st_mode&07777)!=0400||!tb_reviewed_sha256_fd(reader,r->bytes,r->sha)){error_once(r,"OUTPUT_HASH_FAILED");goto done;}
 atomic_store_explicit(&r->sealed,1,memory_order_release);artifact_ok=1;
done:
 if(reader>=0&&close(reader)){artifact_ok=0;error_once(r,"OUTPUT_READER_CLOSE_FAILED");}
 if(fd>=0&&close(fd)){artifact_ok=0;error_once(r,"OUTPUT_CLOSE_FAILED");}
 if(dir>=0&&close(dir)){artifact_ok=0;error_once(r,"OUTPUT_DIRECTORY_CLOSE_FAILED");}
 atomic_store_explicit(&r->complete,api_ok&&artifact_ok,memory_order_release);
 atomic_store_explicit(&r->done,1,memory_order_release);
 return api_ok&&artifact_ok?0:-1;
}

struct json_out {char *data;size_t capacity,used;int bad;};
static void append(struct json_out *o,const char *format,...){
 if(o->bad)return;
 va_list args;va_start(args,format);int n=vsnprintf(o->data+o->used,o->capacity-o->used,format,args);va_end(args);
 if(n<0||(size_t)n>=o->capacity-o->used){o->bad=1;return;}o->used+=(size_t)n;
}
static void call_json(struct json_out *o,const char *name,const struct tb_klog_call *v){
 append(o,"\"%s\":{\"attempted\":%d,\"result\":%lld,\"error_number\":%d}",name,v->attempted,(long long)v->result,v->error_number);
}
static void snapshot_json(struct json_out *o,const struct tb_klog_report *s){
 append(o,"{\"schema\":%u,\"cap_bytes\":%u,\"status\":%d,\"require_root_ram\":%d,\"fd_verified\":%d,\"root_ram_verified\":%d,"
  "\"fd_flags_before\":%d,\"fd_flags_after\":%d,\"offset_before\":%lld,\"offset_after\":%lld,"
  "\"start_ns\":%lld,\"read_start_ns\":%lld,\"read_end_ns\":%lld,\"end_ns\":%lld,",
  s->schema,s->cap_bytes,s->status,s->require_root_ram,s->fd_verified,s->root_ram_verified,s->fd_flags_before,s->fd_flags_after,
  (long long)s->offset_before,(long long)s->offset_after,(long long)s->start_ns,(long long)s->read_start_ns,(long long)s->read_end_ns,(long long)s->end_ns);
 call_json(o,"clock_start",&s->clock_start);append(o,",");call_json(o,"clock_read_start",&s->clock_read_start);append(o,",");
 call_json(o,"clock_read_end",&s->clock_read_end);append(o,",");call_json(o,"clock_end",&s->clock_end);append(o,",");
 call_json(o,"size_before",&s->size_before);append(o,",");call_json(o,"read_all",&s->read_all);append(o,",");
 call_json(o,"size_after",&s->size_after);append(o,",");call_json(o,"write_output",&s->write_output);
 append(o,",\"returned_bytes\":%llu,\"written_bytes\":%llu,\"error_number\":%d,\"returned_bytes_fully_written\":%d,"
  "\"full_ring_history_guaranteed\":%d,\"interval_coverage_guaranteed\":%d,\"causal_order_guaranteed\":%d}",
  (unsigned long long)s->returned_bytes,(unsigned long long)s->written_bytes,s->error_number,s->returned_bytes_fully_written,
  s->full_ring_history_guaranteed,s->interval_coverage_guaranteed,s->causal_order_guaranteed);
}
int q_kernel_report_json(const struct q_state *s,char *out,size_t cap){
 if(!out||!cap)return -1;
 struct json_out o={out,cap,0,0};
 append(&o,"\"KernelSnapshotsPassed\":%s,",atomic_load(&s->kernel[0].complete)&&atomic_load(&s->kernel[1].complete)?"true":"false");
 for(int i=0;i<2;i++){
  const struct q_kernel_record *r=&s->kernel[i];int attempted=atomic_load(&r->attempted),done=atomic_load(&r->done);
  const char *error=attempted&&!done?"OWNER_DIED_OR_INCOMPLETE":r->error;
  append(&o,"\"%s\":{\"Attempted\":%s,\"Complete\":%s,\"Sealed\":%s,\"Unknown\":%s,\"Path\":\"%s\",\"Bytes\":%llu,\"SHA256\":\"%s\",\"SnapshotError\":\"%s\",\"Snapshot\":",
   i?"KernelAfter":"KernelBefore",attempted?"true":"false",atomic_load(&r->complete)?"true":"false",atomic_load(&r->sealed)?"true":"false",
   attempted&&!done?"true":"false",r->path,(unsigned long long)r->bytes,r->sha,error);
  snapshot_json(&o,&r->snapshot);append(&o,"},");
 }
 return o.bad?-1:(int)o.used;
}
