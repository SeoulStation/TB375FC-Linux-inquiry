/* Fixed viewer only. Parent-side code never signals an unverified numeric PID. */
#define _GNU_SOURCE
#include "qualification.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern bool tb_reviewed_sha256_fd(int,uint64_t,char[65]);

static int pidfd_term(int fd){return (int)syscall(SYS_pidfd_send_signal,fd,SIGTERM,NULL,0);}
static int child_owned_pidfd(pid_t pid){
 siginfo_t info={0};
 /* Alive or unreaped children retain their PID; ECHILD refuses arbitrary PIDs. */
 if(waitid(P_PID,(id_t)pid,&info,WEXITED|WNOHANG|WNOWAIT))return -1;
 int fd=(int)syscall(SYS_pidfd_open,pid,0);if(fd<0)return -1;
 memset(&info,0,sizeof info);
 if(waitid(P_PIDFD,(id_t)fd,&info,WEXITED|WNOHANG|WNOWAIT)){close(fd);return -1;}
 return fd;
}
static int drain(struct q_backend *b){
 struct q_state *s=b->shared;
 if(atomic_load(&s->viewer_output_eof))return 0;
 for(int tries=0;tries<8;tries++){
  int used=atomic_load_explicit(&s->viewer_output_len,memory_order_acquire);
  if(used<0||used>Q_VIEWER_CAP)return -1;
  int full=used==Q_VIEWER_CAP;char extra[4096];void *target=full?extra:s->viewer_output+used;
  size_t room=used<Q_VIEWER_CAP?(size_t)(Q_VIEWER_CAP-used):sizeof extra;
  atomic_store_explicit(&s->viewer_output_reading,1,memory_order_release);
  if(!atomic_load(&s->viewer_output_unknown))q_backend_checkpoint(b,"viewer_read_inflight",-1);
  ssize_t got=read(b->viewer_readfd,target,room);int saved=errno;
  if(got>0&&used<Q_VIEWER_CAP){if(memchr(s->viewer_output+used,0,(size_t)got))atomic_store(&s->viewer_output_unknown,1);used+=(int)got;s->viewer_output[used]=0;atomic_store_explicit(&s->viewer_output_len,used,memory_order_release);}
  if(got>0&&full)atomic_store(&s->viewer_output_unknown,1);
  atomic_store_explicit(&s->viewer_output_reading,0,memory_order_release);
  if(got==0){atomic_store_explicit(&s->viewer_output_eof,1,memory_order_release);return 0;}
  if(got<0&&(saved==EAGAIN||saved==EWOULDBLOCK||saved==EINTR))return 0;
  if(got<0){atomic_store(&s->viewer_output_unknown,1);return -1;}
 }
 return 0;
}
static int one_marker(const char *raw,const char *marker){
 int n=0;size_t len=strlen(marker);
 for(const char *p=raw;*p;){const char *end=strchr(p,'\n');if(!end)end=p+strlen(p);if((size_t)(end-p)==len&&!memcmp(p,marker,len))n++;p=*end?end+1:end;}
 return n==1;
}
static int verify_output(struct q_state *s){
 if(!atomic_load(&s->viewer_reaped)||!atomic_load(&s->viewer_output_eof)||atomic_load(&s->viewer_output_unknown)||
    !WIFEXITED(s->viewer_status)||WEXITSTATUS(s->viewer_status)!=0)return -1;
 int restored=one_marker(s->viewer_output,"ORIGINAL_INACTIVE_STATE_RESTORED_AND_VERIFIED all_13_atomic_properties=true all_planes_detached=true");
 int cleanup=one_marker(s->viewer_output,"RAM_DISPLAY_OBJECT_CLEANUP_FINISHED");
 int active=one_marker(s->viewer_output,"IMAGE_ACTIVE max_seconds_from_commit_start=5 no_backlight_sysfs_write=true");
 atomic_store(&s->viewer_restore_verified,restored);atomic_store(&s->viewer_cleanup_marker_verified,cleanup);
 return restored&&cleanup&&active?0:-1;
}
int q_viewer_parent_prepare(struct q_backend *b){
 int enabled=0,pipefd[2];
 if(prctl(PR_SET_CHILD_SUBREAPER,1)||prctl(PR_GET_CHILD_SUBREAPER,&enabled)||enabled!=1||pipe2(pipefd,O_CLOEXEC))return -1;
 int flags=fcntl(pipefd[0],F_GETFL);
 if(flags<0||fcntl(pipefd[0],F_SETFL,flags|O_NONBLOCK)){close(pipefd[0]);close(pipefd[1]);return -1;}
 b->viewer_readfd=pipefd[0];b->viewer_writefd=pipefd[1];return 0;
}
void q_viewer_parent_after_fork(struct q_backend *b){
 if(b->viewer_writefd>=0){close(b->viewer_writefd);b->viewer_writefd=-1;}
 if(b->viewer_exefd>=0){close(b->viewer_exefd);b->viewer_exefd=-1;}
}
static int wait_viewer(struct q_backend *b,pid_t pid,int fd,long long budget){
 struct q_state *s=b->shared;long long due=q_now_ms()+budget,term_time=0;int reaped=0;
 for(;;){
  (void)drain(b);
  int status=0;pid_t got=waitpid(pid,&status,WNOHANG);
  if(got==pid){s->viewer_status=status;atomic_store(&s->viewer_alive_unknown,0);atomic_store_explicit(&s->viewer_reaped,1,memory_order_release);reaped=1;break;}
  if(got<0&&errno!=EINTR){atomic_store(&s->viewer_alive_unknown,1);return -1;}
  if(!term_time&&(q_should_stop()||q_now_ms()>=due||atomic_load(&s->viewer_output_unknown))){
   atomic_store_explicit(&s->viewer_term_attempted,1,memory_order_release);
   if(pidfd_term(fd)&&errno!=ESRCH){atomic_store(&s->viewer_alive_unknown,1);return -1;}
   term_time=q_now_ms();
  }
  if(term_time&&q_now_ms()-term_time>=Q_VIEWER_GRACE_MS){atomic_store(&s->viewer_alive_unknown,1);return -1;}
  (void)poll(NULL,0,10);
 }
 long long eof_due=q_now_ms()+300;
 while(reaped&&!atomic_load(&s->viewer_output_eof)&&q_now_ms()<eof_due){(void)drain(b);(void)poll(NULL,0,1);}
 return verify_output(s);
}
int q_viewer_run(struct q_backend *b){
 struct q_state *s=b->shared;int go[2];
 if(q_should_stop()||q_backend_claim_second_once(b)||pipe2(go,O_CLOEXEC))return -1;
 pid_t expected_parent=getpid();
 atomic_store_explicit(&s->viewer_fork_attempted,1,memory_order_release);
 q_backend_checkpoint(b,"pre_viewer_fork",-1);
 if(q_should_stop()){close(go[0]);close(go[1]);return -1;}
 pid_t child=fork();
 if(child<0){close(go[0]);close(go[1]);return -1;}
 if(child==0){
  struct sigaction def={0};def.sa_handler=SIG_DFL;sigemptyset(&def.sa_mask);
  if(sigaction(SIGTERM,&def,NULL)||sigaction(SIGINT,&def,NULL)||sigaction(SIGALRM,&def,NULL)||
     prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=expected_parent)_exit(125);
  close(go[1]);char token=0;ssize_t got=read(go[0],&token,1);close(go[0]);
  if(got!=1||token!='G'||getppid()!=expected_parent)_exit(125);
  if(dup2(b->viewer_writefd,STDOUT_FILENO)<0||dup2(b->viewer_writefd,STDERR_FILENO)<0)_exit(126);
  close(b->viewer_readfd);close(b->viewer_writefd);
  if(b->guard_fd>=3&&b->guard_fd!=b->viewer_exefd)close(b->guard_fd);
  /* SIGPIPE remains ignored across exec so a lost reader cannot kill KMS restore. */
#ifdef PC_FIXTURE
  char *const argv[]={"viewer-fixture","--show-image",b->root,NULL};
#else
  char *const argv[]={Q_VIEWER_PATH,"--show-image",NULL};
#endif
  char *const envp[]={"PATH=/bin",NULL};
  fexecve(b->viewer_exefd,argv,envp);
  ssize_t note=write(STDERR_FILENO,"FIXED_VIEWER_EXEC_FAILED\n",25);if(note!=25)_exit(127);_exit(127);
 }
 q_backend_checkpoint(b,"viewer_fork_uncommitted",-1);
 close(go[0]);close(b->viewer_writefd);b->viewer_writefd=-1;
 s->viewer_pid=child;atomic_store_explicit(&s->viewer_pid_committed,1,memory_order_release);
 q_backend_checkpoint(b,"viewer_pid_committed",-1);
 int fd=child_owned_pidfd(child);
 if(fd<0){close(go[1]);atomic_store(&s->viewer_alive_unknown,1);return -1;}
 if(!q_should_stop()){
  atomic_store_explicit(&s->viewer_exec_allowed,1,memory_order_release);
  char token='G';if(write(go[1],&token,1)!=1)atomic_store(&s->viewer_output_unknown,1);
 }
 close(go[1]);
 q_backend_checkpoint(b,"viewer_started",-1);
 int ok=wait_viewer(b,child,fd,Q_CAPTURE_MS);close(fd);
 return !ok&&!atomic_load(&s->viewer_term_attempted)&&!q_should_stop()?0:-1;
}
static int adopted_child(pid_t *out){
 char path[96],raw[128];size_t n=0;
 snprintf(path,sizeof path,"/proc/self/task/%ld/children",(long)getpid());
 if(q_read_file(path,raw,sizeof raw,&n))return -1;
 long pid=0;char extra;
 if(!n){*out=0;return 0;}
 if(sscanf(raw,"%ld %c",&pid,&extra)!=1||pid<=0||pid>0x7fffffffL)return -1;
 siginfo_t info={0};if(waitid(P_PID,(id_t)pid,&info,WEXITED|WNOHANG|WNOWAIT))return -1;
 *out=(pid_t)pid;return 0;
}
int q_viewer_parent_finish(struct q_backend *b){
 struct q_state *s=b->shared;
 if(atomic_load_explicit(&s->viewer_output_reading,memory_order_acquire))atomic_store(&s->viewer_output_unknown,1);
 if(atomic_load(&s->viewer_reaped)){
  (void)drain(b);int result=verify_output(s);
  if(b->viewer_readfd>=0){close(b->viewer_readfd);b->viewer_readfd=-1;}
  return atomic_load(&s->viewer_fork_attempted)?result:0;
 }
 pid_t child=0;if(adopted_child(&child)){atomic_store(&s->viewer_alive_unknown,1);return -1;}
 if(!child){
  if(atomic_load(&s->viewer_pid_committed)){atomic_store(&s->viewer_alive_unknown,1);return -1;}
  (void)drain(b);if(b->viewer_readfd>=0){close(b->viewer_readfd);b->viewer_readfd=-1;}
  return atomic_load(&s->viewer_fork_attempted)?-1:0;
 }
 if(!atomic_load(&s->viewer_fork_attempted)||(atomic_load(&s->viewer_pid_committed)&&child!=s->viewer_pid)){
  atomic_store(&s->viewer_alive_unknown,1);return -1;
 }
 int fd=child_owned_pidfd(child);if(fd<0){atomic_store(&s->viewer_alive_unknown,1);return -1;}
 s->viewer_pid=child;
 atomic_store_explicit(&s->viewer_term_attempted,1,memory_order_release);
 if(pidfd_term(fd)&&errno!=ESRCH){close(fd);atomic_store(&s->viewer_alive_unknown,1);return -1;}
 int result=wait_viewer(b,child,fd,0);close(fd);
 if(b->viewer_readfd>=0){close(b->viewer_readfd);b->viewer_readfd=-1;}
 return result;
}
int q_viewer_seal(struct q_backend *b){
 struct q_state *s=b->shared;
 if(atomic_load(&s->viewer_artifact_ok))return 0;
 int count=atomic_load_explicit(&s->viewer_output_len,memory_order_acquire);
 if(count<0||count>Q_VIEWER_CAP)return -1;
 int n=snprintf(s->viewer_path,sizeof s->viewer_path,"%s/viewer.txt",b->claim);
 if(n<0||(size_t)n>=sizeof s->viewer_path)return -1;
 int fd=open(s->viewer_path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
 int ok=write(fd,s->viewer_output,(size_t)count)==count&&!fchmod(fd,0400);
 if(close(fd))ok=0;
 if(!ok)return -1;
 fd=open(s->viewer_path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
 ok=tb_reviewed_sha256_fd(fd,(uint64_t)count,s->viewer_sha);if(close(fd))ok=0;
 if(!ok)return -1;
 s->viewer_bytes=(uint64_t)count;
 atomic_store_explicit(&s->viewer_artifact_ok,1,memory_order_release);return 0;
}
