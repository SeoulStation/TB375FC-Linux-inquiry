/* One direct reader child. No viewer, SPI, tracefs or display-state operations. */
#define _GNU_SOURCE
#include "qualification.h"
#include "kernel_log_stream_adapter.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static struct q_stream_state *active_stream;
static int active_reader_pidfd=-1;
long long q_stream_now_ns(void){
 struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000L)return -1;
 if((unsigned long long)t.tv_sec>((unsigned long long)INT64_MAX-(unsigned long long)t.tv_nsec)/1000000000ULL)return -1;
 return (long long)t.tv_sec*1000000000LL+t.tv_nsec;
}
static int pid_dead(int fd){struct pollfd p={.fd=fd,.events=POLLIN};return fd>=0&&poll(&p,1,0)>0&&(p.revents&POLLIN);}
static void error_once(struct q_stream_state *s,const char *why){if(!s->error[0])snprintf(s->error,sizeof s->error,"%s",why);}
static void close_slot(int *fd){if(*fd>=0){(void)close(*fd);*fd=-1;}}
void q_stream_initialize(struct q_backend *b){
 struct q_stream_state *s=&b->shared->stream;
 b->stream.pid=-1;b->stream.pidfd=b->stream.rawfd=b->stream.command_write=b->stream.notice_read=b->stream.parentfd=-1;
 s->source_close_return=s->raw_close_return=-1;s->reader_status=-1;
 s->ready_sent_ns=s->parent_ready_ns=s->parent_ack_ns=s->watcher_start_ns=s->parent_cleanup_ns=-1;
 s->reader_ended_ns=s->source_closed_ns=s->reaped_ns=s->raw_sealed_ns=s->meta_sealed_ns=-1;
 active_stream=s;active_reader_pidfd=-1;
}
int q_stream_should_stop(void){
 return active_stream&&(atomic_load_explicit(&active_stream->failed,memory_order_acquire)||
                         (active_reader_pidfd>=0&&pid_dead(active_reader_pidfd)));
}
static void fixture_event(struct q_backend *b,const char *event){
#ifdef PC_FIXTURE
 char path[Q_PATH_CAP];int n=snprintf(path,sizeof path,"%s/stream-events.log",b->root);
 if(n>0&&(size_t)n<sizeof path){int fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);if(fd>=0){char line[160];pid_t logged=!strcmp(event,"FORK")?b->stream.pid:getpid();n=snprintf(line,sizeof line,"%s pid=%ld mono_ns=%lld\n",event,(long)logged,q_stream_now_ns());if(n>0&&(size_t)n<sizeof line){ssize_t recorded=write(fd,line,(size_t)n);if(recorded!=n)atomic_store(&b->shared->stream.failed,1);}(void)close(fd);}}
#else
 (void)b;(void)event;
#endif
}
static int byte_write(int fd,char value,long long until,int parentfd){
 while(q_now_ms()<until){if(parentfd>=0&&pid_dead(parentfd))return -1;
  ssize_t n=write(fd,&value,1);if(n==1)return 0;if(n<0&&(errno==EAGAIN||errno==EINTR)){struct pollfd p={.fd=fd,.events=POLLOUT};(void)poll(&p,1,10);continue;}return -1;
 }errno=ETIMEDOUT;return -1;
}
static int byte_read(int fd,char value,long long until,int parentfd){
 while(q_now_ms()<until){if(parentfd>=0&&pid_dead(parentfd))return -1;
  char actual;ssize_t n=read(fd,&actual,1);if(n==1){if(actual==value)return 0;errno=EPROTO;return -1;}
  if(n==0)return -1;
  if(n<0&&errno!=EAGAIN&&errno!=EINTR)return -1;
  struct pollfd p={.fd=fd,.events=POLLIN};(void)poll(&p,1,10);
 }errno=ETIMEDOUT;return -1;
}
struct ready_context {struct q_backend *backend;int from_main,to_main,parentfd;long long until;};
static int ready_hook(void *opaque){
 struct ready_context *c=opaque;struct q_stream_state *s=&c->backend->shared->stream;
 q_backend_checkpoint(c->backend,"stream_before_ready",-1);
#ifdef PC_FIXTURE
 if(getenv("PC_STREAM_NO_READY")){while(q_now_ms()<c->until&&!pid_dead(c->parentfd)){struct pollfd p={.fd=c->parentfd,.events=POLLIN};(void)poll(&p,1,20);}errno=ETIMEDOUT;return -1;}
#endif
 long long stamp=q_stream_now_ns();if(stamp<0||byte_write(c->to_main,'R',c->until,c->parentfd))return -1;
 s->ready_sent_ns=stamp;fixture_event(c->backend,"READY");
 if(byte_read(c->from_main,'A',c->until,c->parentfd))return -1;
 close_slot(&c->from_main);close_slot(&c->to_main);
 q_backend_checkpoint(c->backend,"stream_after_ready",-1);return 0;
}
static void reader_child(struct q_backend *b,int from_main,int to_main,int parentfd,pid_t expected_parent){
 struct q_stream_state *s=&b->shared->stream;struct sigaction sa={0};sa.sa_handler=SIG_DFL;sigemptyset(&sa.sa_mask);
 if(sigaction(SIGTERM,&sa,NULL)||sigaction(SIGINT,&sa,NULL)||prctl(PR_SET_PDEATHSIG,SIGTERM)){atomic_store(&s->failed,1);_exit(70);}
 atomic_store_explicit(&s->pdeath_set,1,memory_order_release);
 if(getppid()!=expected_parent){atomic_store(&s->failed,1);_exit(70);}atomic_store_explicit(&s->parent_rechecked,1,memory_order_release);
 struct ready_context c={b,from_main,to_main,parentfd,q_now_ms()+Q_STREAM_READY_MS};
 if(byte_read(from_main,'G',c.until,parentfd)||!atomic_load_explicit(&s->pid_committed,memory_order_acquire)){atomic_store(&s->failed,1);_exit(70);}
 atomic_store_explicit(&s->go_received,1,memory_order_release);
 if(b->viewer_exefd>=0)close(b->viewer_exefd);
 if(b->viewer_readfd>=0)close(b->viewer_readfd);
 if(b->viewer_writefd>=0)close(b->viewer_writefd);
 q_backend_checkpoint(b,"stream_before_open",-1);
#ifdef PC_FIXTURE
 q_fixture_stream_setup(b,b->stream.rawfd);
#endif
 atomic_store_explicit(&s->source_open_attempted,1,memory_order_release);
 int source=open("/dev/kmsg",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
 s->source_open_error=source<0?errno:0;
 if(source>=0)atomic_store_explicit(&s->source_opened,1,memory_order_release);
 int ok=0;
 if(source>=0){
#ifdef PC_FIXTURE
  ok=tb_kstream_adapter_observe_fds_test(source,b->stream.rawfd,1,ready_hook,&c,&s->core)==0;
#else
  /* The template and public adapter stay locked; no external admission exists. */
  ok=tb_kstream_adapter_observe_fds(source,b->stream.rawfd,1,ready_hook,&c,&s->core)==0;
#endif
  s->reader_ended_ns=q_stream_now_ns();atomic_store_explicit(&s->core_done,1,memory_order_release);
 }
 if(!ok||!s->core.complete||!atomic_load_explicit(&b->shared->cleanup_done,memory_order_acquire))atomic_store_explicit(&s->failed,1,memory_order_release);
 if(source>=0){atomic_store_explicit(&s->source_close_attempted,1,memory_order_release);errno=0;s->source_close_return=close(source);s->source_close_error=s->source_close_return?errno:0;s->source_closed_ns=q_stream_now_ns();if(s->source_close_return)ok=0;}
 close_slot(&c.from_main);close_slot(&c.to_main);close(parentfd);
 errno=0;s->raw_close_return=close(b->stream.rawfd);s->raw_close_error=s->raw_close_return?errno:0;if(s->raw_close_return)ok=0;
 if(!ok)atomic_store_explicit(&s->failed,1,memory_order_release);
 fixture_event(b,"ENDED");_exit(ok?0:2);
}
int q_stream_start(struct q_backend *b){
 struct q_stream_state *s=&b->shared->stream;atomic_store_explicit(&s->attempted,1,memory_order_release);s->parent_pid=getpid();
 int n=snprintf(s->raw_path,sizeof s->raw_path,"%s/kernel-stream.raw",b->claim);
 int m=snprintf(s->meta_path,sizeof s->meta_path,"%s/kernel-stream-meta.json",b->claim);
 if(n<=0||(size_t)n>=sizeof s->raw_path||m<=0||(size_t)m>=sizeof s->meta_path){error_once(s,"STREAM_PATH_FAILED");return -1;}
 int dir=open(b->claim,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat ds,created;struct statfs fs;
 if(dir<0||fstat(dir,&ds)||!S_ISDIR(ds.st_mode)||ds.st_uid||ds.st_gid||(ds.st_mode&07777)!=0700||
    fstatfs(dir,&fs)||(fs.f_type!=TMPFS_MAGIC&&fs.f_type!=RAMFS_MAGIC)){if(dir>=0)close(dir);error_once(s,"STREAM_DIRECTORY_NOT_ROOT_RAM");return -1;}
 b->stream.rawfd=openat(dir,"kernel-stream.raw",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);close(dir);
 if(b->stream.rawfd<0){error_once(s,"STREAM_RAW_CREATE_FAILED");return -1;}
 atomic_store_explicit(&s->raw_created,1,memory_order_release);
 if(fchmod(b->stream.rawfd,0600)||fstat(b->stream.rawfd,&created)||!S_ISREG(created.st_mode)||created.st_uid||created.st_gid||created.st_nlink!=1||created.st_size){error_once(s,"STREAM_RAW_METADATA_FAILED");return -1;}
 b->stream.raw_dev=created.st_dev;b->stream.raw_ino=created.st_ino;
 int command[2]={-1,-1},notice[2]={-1,-1};
 b->stream.parentfd=(int)syscall(SYS_pidfd_open,getpid(),0);
 if(b->stream.parentfd<0||pipe2(command,O_NONBLOCK|O_CLOEXEC)||pipe2(notice,O_NONBLOCK|O_CLOEXEC)){
  close_slot(&command[0]);close_slot(&command[1]);close_slot(&notice[0]);close_slot(&notice[1]);error_once(s,"STREAM_PIPES_FAILED");return -1;
 }
 atomic_store_explicit(&s->fork_attempted,1,memory_order_release);pid_t pid=fork();
 if(pid<0){close_slot(&command[0]);close_slot(&command[1]);close_slot(&notice[0]);close_slot(&notice[1]);error_once(s,"STREAM_FORK_FAILED");return -1;}
 if(!pid){close(command[1]);close(notice[0]);reader_child(b,command[0],notice[1],b->stream.parentfd,s->parent_pid);}
 close(command[0]);close(notice[1]);close_slot(&b->stream.parentfd);
 b->stream.pid=pid;s->reader_pid=pid;b->stream.pidfd=(int)syscall(SYS_pidfd_open,pid,0);active_reader_pidfd=b->stream.pidfd;
 b->stream.command_write=command[1];b->stream.notice_read=notice[0];b->stream.join_deadline_ms=q_now_ms()+Q_STREAM_JOIN_MS;
 fixture_event(b,"FORK");
 if(b->stream.pidfd<0){error_once(s,"STREAM_PIDFD_FAILED");return -1;}
 atomic_store_explicit(&s->pid_committed,1,memory_order_release);
 long long until=q_now_ms()+Q_STREAM_READY_MS;
 if(byte_write(command[1],'G',until,b->stream.pidfd)||byte_read(notice[0],'R',until,b->stream.pidfd)){
  error_once(s,"STREAM_READY_FAILED");return -1;
 }
 s->parent_ready_ns=q_stream_now_ns();if(s->parent_ready_ns<0){error_once(s,"STREAM_READY_CLOCK_FAILED");return -1;}atomic_store_explicit(&s->ready_received,1,memory_order_release);
 long long stamp=q_stream_now_ns();
 if(stamp<0||byte_write(command[1],'A',until,b->stream.pidfd)){error_once(s,"STREAM_ACK_FAILED");return -1;}
 s->parent_ack_ns=stamp;atomic_store_explicit(&s->ack_sent,1,memory_order_release);
 close_slot(&b->stream.command_write);close_slot(&b->stream.notice_read);return 0;
}
void q_stream_watcher_started(struct q_backend *b,long long stamp){b->shared->stream.watcher_start_ns=stamp;}
void q_stream_cleanup_verified(struct q_backend *b){if(b->shared->stream.parent_cleanup_ns<0)b->shared->stream.parent_cleanup_ns=q_stream_now_ns();}
static int reap_once(struct q_backend *b){
 struct q_stream_state *s=&b->shared->stream;int status=0;
 pid_t result=waitpid(b->stream.pid,&status,WNOHANG);
 if(result==b->stream.pid){s->reader_status=status;s->reaped_ns=q_stream_now_ns();atomic_store_explicit(&s->reaped,1,memory_order_release);return 1;}
 return 0;
}
static void signal_owned_reader(struct q_backend *b,int signal_number){
 if(atomic_load(&b->shared->stream.reaped)||b->stream.pid<=0)return;
#ifdef SYS_pidfd_send_signal
 if(b->stream.pidfd>=0){(void)syscall(SYS_pidfd_send_signal,b->stream.pidfd,signal_number,NULL,0);return;}
#endif
 /* An exclusively owned, unreaped direct child cannot have a reused PID.
  * External root/wait interference is outside this ownership contract. */
 (void)kill(b->stream.pid,signal_number);
}
static int cancel_owned_reader(struct q_backend *b,const char *reason){
 struct q_stream_state *s=&b->shared->stream;
 if(b->stream.pid<=0||atomic_load(&s->reaped))return 0;
 if(reap_once(b)){close_slot(&b->stream.pidfd);active_reader_pidfd=-1;return 0;}
 /* The unchanged viewer fallback accepts one adopted child. Remove only our
  * positively owned reader first; never let it be mistaken for the viewer. */
 error_once(s,reason);atomic_store_explicit(&s->failed,1,memory_order_release);
 signal_owned_reader(b,SIGTERM);long long until=q_now_ms()+Q_STREAM_TERM_MS;
 while(q_now_ms()<until&&!atomic_load(&s->reaped)&&!reap_once(b)){struct pollfd p={.fd=b->stream.pidfd,.events=POLLIN};(void)poll(&p,1,20);}
 if(!atomic_load(&s->reaped)){signal_owned_reader(b,SIGKILL);until=q_now_ms()+Q_STREAM_KILL_MS;while(q_now_ms()<until&&!atomic_load(&s->reaped)&&!reap_once(b)){struct pollfd p={.fd=b->stream.pidfd,.events=POLLIN};(void)poll(&p,1,20);}}
 if(!atomic_load(&s->reaped)){atomic_store(&s->alive_unknown,1);error_once(s,"READER_DEATH_UNKNOWN");return -1;}
 close_slot(&b->stream.pidfd);active_reader_pidfd=-1;return 0;
}
int q_stream_cancel_before_viewer_adoption(struct q_backend *b){return cancel_owned_reader(b,"READER_CANCELLED_FOR_VIEWER_ADOPTION");}
int q_stream_contain_unknown_watcher(struct q_backend *b){
 error_once(&b->shared->stream,"WATCHER_DEATH_UNKNOWN");
 return cancel_owned_reader(b,"WATCHER_DEATH_UNKNOWN");
}
int q_stream_finish(struct q_backend *b,int cleanup_verified,int diagnostic_ok){
 struct q_stream_state *s=&b->shared->stream;
 if(!atomic_load(&s->attempted))return 0;
 if(cleanup_verified)q_stream_cleanup_verified(b);
 close_slot(&b->stream.command_write);close_slot(&b->stream.notice_read);close_slot(&b->stream.parentfd);
 if(b->stream.pid>0){
  if(!diagnostic_ok){error_once(s,"DIAGNOSTIC_FAILED_READER_CANCELLED");signal_owned_reader(b,SIGTERM);}
  long long until=diagnostic_ok?b->stream.join_deadline_ms:q_now_ms()+Q_STREAM_TERM_MS;
  while(q_now_ms()<until&&!atomic_load(&s->reaped)&&!reap_once(b)){struct pollfd p={.fd=b->stream.pidfd,.events=POLLIN};(void)poll(&p,1,20);}
  if(!atomic_load(&s->reaped)){error_once(s,"READER_JOIN_TIMEOUT");signal_owned_reader(b,SIGTERM);until=q_now_ms()+Q_STREAM_TERM_MS;while(q_now_ms()<until&&!reap_once(b)){struct pollfd p={.fd=b->stream.pidfd,.events=POLLIN};(void)poll(&p,1,20);}}
  if(!atomic_load(&s->reaped)){signal_owned_reader(b,SIGKILL);until=q_now_ms()+Q_STREAM_KILL_MS;while(q_now_ms()<until&&!reap_once(b)){struct pollfd p={.fd=b->stream.pidfd,.events=POLLIN};(void)poll(&p,1,20);}}
  if(!atomic_load(&s->reaped)){atomic_store(&s->alive_unknown,1);error_once(s,"READER_DEATH_UNKNOWN");return -1;}
 }
 close_slot(&b->stream.pidfd);active_reader_pidfd=-1;
 if(!atomic_load(&s->core_done))error_once(s,"READER_REPORT_UNAVAILABLE");
 else if(!s->core.complete)error_once(s,tb_kstream_status_name(s->core.status));
 int sealed=q_stream_finalize_artifacts(b)==0;
 int coverage=cleanup_verified&&s->parent_cleanup_ns>=s->parent_ack_ns&&s->parent_cleanup_ns<=s->core.deadline_ns&&
              s->reader_ended_ns>=s->parent_cleanup_ns;
 int clocks=s->ready_sent_ns>=s->core.ready_start_ns&&s->ready_sent_ns<=s->core.ready_end_ns&&
            s->ready_sent_ns>=s->core.arm_end_ns&&s->parent_ready_ns>=s->ready_sent_ns&&s->parent_ack_ns>=s->parent_ready_ns&&
            s->watcher_start_ns>=s->parent_ack_ns&&s->reader_ended_ns>=s->core.end_ns&&s->source_closed_ns>=s->reader_ended_ns&&
            s->reaped_ns>=s->source_closed_ns&&s->raw_sealed_ns>=s->reaped_ns&&s->meta_sealed_ns>=s->raw_sealed_ns;
 int good=diagnostic_ok&&!atomic_load(&s->failed)&&!s->error[0]&&clocks&&atomic_load(&s->core_done)&&s->core.complete&&atomic_load(&s->reaped)&&WIFEXITED(s->reader_status)&&WEXITSTATUS(s->reader_status)==0&&
          s->source_close_return==0&&s->source_close_error==0&&s->raw_close_return==0&&s->raw_close_error==0&&coverage&&sealed&&
          atomic_load(&s->ack_sent)&&atomic_load(&s->pdeath_set)&&atomic_load(&s->parent_rechecked)&&atomic_load(&s->go_received)&&
          atomic_load(&s->source_opened)&&atomic_load(&s->source_close_attempted)&&s->parent_ack_ns<=s->watcher_start_ns;
 atomic_store_explicit(&s->passed,good,memory_order_release);
 if(!good&&!s->error[0])error_once(s,"STREAM_SCOPE_OR_ARTIFACT_INCOMPLETE");
 return good?0:-1;
}
