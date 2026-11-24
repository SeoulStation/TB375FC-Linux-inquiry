/* Supervised second-display baseline only: fixed probes/viewer; no general warm admission. */
#define _GNU_SOURCE
#include "qualification.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "shared journal needs lock-free int atomics");
const char *q_events[4] = {"read_pre", "read_post", "write_pre", "write_post"};
/* trace_kprobe_show publishes decimal offsets. Emit that exact canonical form. */
const char *q_sites[4] = {"CTP_SPI_READ+340", "CTP_SPI_READ+344", "CTP_SPI_WRITE+336", "CTP_SPI_WRITE+340"};
const char *q_fetch[4] = {
 "stack=%sp:x64 len=+32(%sp):u32 tx_first2=+0(+16(%sp)):x8[2]",
 "stack=%sp:x64 len=+32(%sp):u32 ret=%x0:s32 frame_length=+216(%sp):u32 actual_length=+220(%sp):u32 status=+224(%sp):s32 rx_first10=+0(+24(%sp)):x8[10]",
 "stack=%sp:x64 len=+32(%sp):u32 tx_first9=+0(+16(%sp)):x8[9]",
 "stack=%sp:x64 len=+32(%sp):u32 ret=%x0:s32 frame_length=+216(%sp):u32 actual_length=+220(%sp):u32 status=+224(%sp):s32"};
const char *q_profiles[6] = {"kprobe_events", "dynamic_events", "current_tracer", "tracing_on", "set_event", "events/enable"};
static volatile sig_atomic_t stopped;
static long long watcher_deadline;
static int watched_parent = -1;
static void stop_signal(int n) { (void)n; stopped = 1; }
long long q_now_ms(void) { struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1; return (long long)t.tv_sec*1000+t.tv_nsec/1000000; }
static int dead(int fd) { struct pollfd p={.fd=fd,.events=POLLIN}; int r=poll(&p,1,0); return r>0 && (p.revents&(POLLIN|POLLHUP)); }
int q_should_stop(void) { return stopped || q_stream_should_stop() || (watched_parent>=0 && dead(watched_parent)) || (watcher_deadline && q_now_ms()>=watcher_deadline); }
static int valid_hex(const char *s, size_t n) { if (!s || strlen(s)!=n) return 0; int nonzero=0; for(size_t i=0;i<n;i++){if(!isdigit((unsigned char)s[i]) && !(s[i]>='a'&&s[i]<='f'))return 0;nonzero|=s[i]!='0';}return nonzero; }
static int valid_boot(const char *s) { if(!s||strlen(s)!=36||s[14]!='4'||!strchr("89ab",s[19]))return 0;for(int i=0;i<36;i++){if(i==8||i==13||i==18||i==23){if(s[i]!='-')return 0;}else if(!isdigit((unsigned char)s[i])&&!(s[i]>='a'&&s[i]<='f'))return 0;}return 1; }
int q_definition(const struct q_backend *b,int i,char *out,size_t cap){int n=snprintf(out,cap,"p:%s/%s nt36532:%s %s\n",b->group,q_events[i],q_sites[i],q_fetch[i]);return n>0&&(size_t)n<cap?0:-1;}
int q_read_file(const char *path,char *out,size_t cap,size_t *length){
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
 size_t n=0;int ok=1;
 while(n<cap-1){
  ssize_t got=read(fd,out+n,cap-1-n);
  if(got<0&&errno==EINTR)continue;
  if(got<0){ok=0;break;}if(!got)break;n+=(size_t)got;
 }
 char extra;ssize_t got;
 do{got=read(fd,&extra,1);}while(got<0&&errno==EINTR);
 if(got!=0||memchr(out,0,n))ok=0;
 close(fd);out[n]=0;*length=n;return ok?0:-1;
}
/* The successful first attempt is immutable input, never a marker we rewrite. */
int q_prior_cold_marker_valid(const char *path,const char *boot){
 if(!valid_boot(boot))return -1;
 char expected[160],raw[160];
 int n=snprintf(expected,sizeof expected,"GRID_OMISSION_COLD_FIRST_DISPLAY_ATTEMPT %s %s\n",boot,Q_PRIOR_COLD_NONCE);
 if(n<=0||(size_t)n>=sizeof expected)return -1;
 int held=open(path,O_PATH|O_NOFOLLOW|O_CLOEXEC);if(held<0)return -1;
 struct stat before,after,opened;int ok=0,fd=-1;
 if(fstat(held,&before)||!S_ISREG(before.st_mode)||before.st_uid||before.st_gid||
    (before.st_mode&07777)!=0400||before.st_nlink!=1||before.st_size!=n)goto out;
 /* Reopen only the held regular inode. A FIFO/device at the input pathname is
  * never opened for I/O, and replacing that pathname cannot redirect this FD. */
 char ownfd[64];int m=snprintf(ownfd,sizeof ownfd,"/proc/self/fd/%d",held);
 if(m<=0||(size_t)m>=sizeof ownfd)goto out;
 fd=open(ownfd,O_RDONLY|O_CLOEXEC);
 if(fd<0||fstat(fd,&opened)||opened.st_dev!=before.st_dev||opened.st_ino!=before.st_ino)goto out;
 ssize_t got=pread(fd,raw,(size_t)n,0);
 if(got!=n||memcmp(raw,expected,(size_t)n)||fstat(fd,&after))goto out;
 ok=after.st_dev==before.st_dev&&after.st_ino==before.st_ino&&after.st_size==before.st_size&&
    after.st_mode==before.st_mode&&after.st_uid==before.st_uid&&after.st_gid==before.st_gid&&
    after.st_nlink==before.st_nlink&&after.st_mtim.tv_sec==before.st_mtim.tv_sec&&
    after.st_mtim.tv_nsec==before.st_mtim.tv_nsec&&after.st_ctim.tv_sec==before.st_ctim.tv_sec&&
    after.st_ctim.tv_nsec==before.st_ctim.tv_nsec;
out:
 if(fd>=0&&close(fd))ok=0;
 if(close(held))ok=0;
 return ok?0:-1;
}
int q_write_all_once(const char *path,const char *text,size_t n){
 int fd=open(path,O_WRONLY|O_APPEND|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
 /* Never retry an ambiguous registration write; stale errno is not a short-write result. */
 ssize_t written=write(fd,text,n);
 int saved=written<0?errno:written==(ssize_t)n?0:EIO;
 close(fd);errno=saved;return saved? -1:0;
}
int q_find_in_profile(const char *text,const char *key,char *out,size_t cap){
 size_t key_len=strlen(key);int found=0;
 for(const char *p=text;*p;){
  const char *end=strchr(p,'\n');if(!end)end=p+strlen(p);
  const char *space=memchr(p,' ',(size_t)(end-p));
  size_t first=space?(size_t)(space-p):(size_t)(end-p);
  if(first==key_len&&!memcmp(p,key,key_len)){
   if(found)return -1; /* Removing an ambiguous multi-probe event could remove another owner. */
   size_t n=(size_t)(end-p);if(n>=cap)return -1;
   memcpy(out,p,n);out[n]=0;found=1;
  }
  p=*end?end+1:end;
 }
 return found;
}
int q_same_definition(const char *a,const char *b){
 /* Linux6.1 trace_kprobe_show prints target offset with %u but preserves arg.comm. */
 char aa[2048],bb[2048];if(strlen(a)>=sizeof aa||strlen(b)>=sizeof bb)return 0;strcpy(aa,a);strcpy(bb,b);
 char *as=NULL,*bs=NULL,*ak=strtok_r(aa," \t\r\n",&as),*bk=strtok_r(bb," \t\r\n",&bs);if(!ak||!bk||strcmp(ak,bk))return 0;
 char *at=strtok_r(NULL," \t\r\n",&as),*bt=strtok_r(NULL," \t\r\n",&bs);if(!at||!bt)return 0;
 char *ap=strrchr(at,'+'),*bp=strrchr(bt,'+');if(!ap||!bp)return 0;*ap++=0;*bp++=0;if(strcmp(at,bt))return 0;
 if(*ap=='-'||*bp=='-'||!*ap||!*bp)return 0;
 char *ae,*be;
 errno=0;
 unsigned long av=strtoul(ap,&ae,(!strncmp(ap,"0x",2)?16:10));
 if(errno||*ae||av>65536)return 0;
 errno=0;
 unsigned long bv=strtoul(bp,&be,(!strncmp(bp,"0x",2)?16:10));
 if(errno||*be||bv>65536||av!=bv)return 0;
 for(;;){ak=strtok_r(NULL," \t\r\n",&as);bk=strtok_r(NULL," \t\r\n",&bs);if(!ak||!bk)return !ak&&!bk;if(strcmp(ak,bk))return 0;}
}
static int blank(const char *p){while(*p){if(!isspace((unsigned char)*p))return 0;p++;}return 1;}
static int bounded_decimal(const char *p,unsigned cap,unsigned *out){
 while(isspace((unsigned char)*p))p++;
 if(!isdigit((unsigned char)*p))return 0;
 unsigned value=0;
 while(isdigit((unsigned char)*p)){
  unsigned digit=(unsigned)(*p++-'0');
  if(value>cap/10||(value==cap/10&&digit>cap%10))return 0;
  value=value*10+digit;
 }
 if(!blank(p))return 0;
 *out=value;return 1;
}
static int field_type(const char *type,int bytes,int sign){
 if(sign)return bytes==4&&(!strcmp(type,"int")||!strcmp(type,"s32")||!strcmp(type,"signed int"));
 if(bytes==1)return !strcmp(type,"u8")||!strcmp(type,"unsigned char");
 if(bytes==2)return !strcmp(type,"u16")||!strcmp(type,"unsigned short")||!strcmp(type,"unsigned short int");
 if(bytes==4)return !strcmp(type,"u32")||!strcmp(type,"unsigned int");
 return bytes==8&&(!strcmp(type,"u64")||!strcmp(type,"unsigned long")||!strcmp(type,"unsigned long int")||!strcmp(type,"unsigned long long")||!strcmp(type,"unsigned long long int"));
}
int q_format_valid(int event,const char *raw,size_t n){
 const char *common[]={"common_type","common_flags","common_preempt_count","common_pid","__probe_ip"};
 const char *names[4][8]={{"stack","len","tx_first2",NULL},{"stack","len","ret","frame_length","actual_length","status","rx_first10",NULL},{"stack","len","tx_first9",NULL},{"stack","len","ret","frame_length","actual_length","status",NULL}};
 int sizes[4][8]={{8,4,2},{8,4,4,4,4,4,10},{8,4,9},{8,4,4,4,4,4}};
 int signs[4][8]={{0,0,0},{0,0,1,0,0,1,0},{0,0,0},{0,0,1,0,0,1}};
 int common_size[]={2,1,1,4,8}, common_sign[]={0,0,0,1,0};
 if(!n||n>=Q_FORMAT_CAP||memchr(raw,0,n))return 0;
 int got_name=0,id=0,section=0,printed=0,count=0;unsigned previous_end=0;
 for(const char *p=raw;*p;){
  const char *end=strchr(p,'\n');if(!end)end=p+strlen(p);size_t len=(size_t)(end-p);
  if(len>1023)return 0;
  char line[1024];memcpy(line,p,len);line[len]=0;
  char *s=line;while(isspace((unsigned char)*s))s++;
  if(!strncmp(s,"name:",5)){
   char name[64],extra;if(section||got_name||sscanf(s,"name: %63s %c",name,&extra)!=1||strcmp(name,q_events[event]))return 0;got_name=1;
  }else if(!strncmp(s,"ID:",3)){
   char token[32],extra;unsigned parsed_id;
   if(section||id||sscanf(s,"ID: %31s %c",token,&extra)!=1||!bounded_decimal(token,65535,&parsed_id)||!parsed_id)return 0;
   id=(int)parsed_id;
  }else if(!strcmp(s,"format:")){if(section||!got_name||!id)return 0;section=1;
  }else if(!strncmp(s,"print fmt:",10)){if(section!=1||printed)return 0;printed=1;section=2;
  }else if(!strncmp(s,"field:",6)){
   if(section!=1||count>=12)return 0;
   char decl[256],offset_token[32],size_token[32],sign_token[32];
   unsigned offset,width,sign;int consumed=0;
   if(sscanf(s,"field:%255[^;]; offset:%31[^;]; size:%31[^;]; signed:%31[^;];%n",decl,offset_token,size_token,sign_token,&consumed)!=4||!consumed||!blank(s+consumed))return 0;
   if(!bounded_decimal(offset_token,64,&offset)||!bounded_decimal(size_token,64,&width)||!bounded_decimal(sign_token,1,&sign))return 0;
   size_t d=strlen(decl);while(d&&isspace((unsigned char)decl[d-1]))decl[--d]=0;
   char *last=strrchr(decl,' ');if(!last)return 0;*last++=0;
   char name[80];unsigned array=0;char extra;int unsized_array=0,had_array=strchr(last,'[')!=NULL;
   if(had_array){
    char *open=strchr(last,'[');const char *close=strchr(open+1,']');
    size_t name_len=(size_t)(open-last);
    if(!name_len||name_len>=sizeof name||!close||close[1]||strchr(open+1,'['))return 0;
    memcpy(name,last,name_len);name[name_len]=0;
    size_t digits=(size_t)(close-open-1);
    if(!digits)unsized_array=1;
    else{char count_token[32];if(digits>=sizeof count_token)return 0;memcpy(count_token,open+1,digits);count_token[digits]=0;if(!bounded_decimal(count_token,63,&array))return 0;}
   }else{if(sscanf(last,"%79s %c",name,&extra)!=1)return 0;}
   int index=count-5;const char *expected=count<5?common[count]:names[event][index];
   if(!expected)return 0;
   int expected_width=count<5?common_size[count]:sizes[event][index];
   int expected_sign=count<5?common_sign[count]:signs[event][index];
   int expected_array=!strcmp(expected,"tx_first2")?2:!strcmp(expected,"rx_first10")?10:!strcmp(expected,"tx_first9")?9:0;
   /* The captured Android kernel format uses u8 name[] for fixed byte arrays.
    * Infer count only for the three known byte fields after exact width, type,
    * signedness and scalar/array-kind checks; never accept a dynamic locator. */
   if(strcmp(name,expected)||width!=(unsigned)expected_width||sign!=(unsigned)expected_sign||had_array!=(expected_array!=0)||!field_type(decl,expected_array?1:expected_width,expected_sign))return 0;
   if(unsized_array)array=width;
   if(array!=(unsigned)expected_array)return 0;
   unsigned align=expected_array?1:width;unsigned expected_offset=(previous_end+align-1)/align*align;
   if(offset!=expected_offset||offset+width>64)return 0;
   previous_end=offset+width;count++;
  }else if(!blank(s))return 0;
  p=*end?end+1:end;
 }
 int extra_count=0;while(names[event][extra_count])extra_count++;
 int ends[]={30,54,37,44};return got_name&&id&&printed&&count==5+extra_count&&previous_end==(unsigned)ends[event]?id:0;
}
static void fault(struct q_state *s){atomic_store_explicit(&s->fault,1,memory_order_release);}
static int snapshot(struct q_backend *b,struct q_state *s){for(int i=0;i<6;i++)if(q_backend_profile(b,i,s->baseline[i],Q_PROFILE_CAP,&s->baseline_len[i]))return -1;/* A unique group means no existing event of ANY name in it. */char key[80];snprintf(key,sizeof key,"%s/",b->group);if(strstr(s->baseline[0],key)||strstr(s->baseline[1],key))return -1;return q_profile_names_unambiguous(s->baseline[0]);}
static int absent_all(struct q_backend *b){char line[2048];for(int i=0;i<4;i++)if(q_backend_find(b,i,line,sizeof line)!=0)return 0;return 1;}
static int cleanup(struct q_backend *b,struct q_state *s){
 int ok=q_capture_cleanup(b)==0;char current[2048],expected[2048];
 if(!ok){atomic_store(&s->remaining_or_ambiguous_mask,15);atomic_store(&s->other_equal,0);atomic_store(&s->cleanup_ok,0);atomic_store(&s->cleanup_done,1);return -1;}
 for(int i=0;i<4;i++){
  int present=q_backend_find(b,i,current,sizeof current);
  if(present<0){ok=0;continue;}if(!present)continue;
  if(!atomic_load_explicit(&s->attempted[i],memory_order_acquire)||atomic_load_explicit(&s->foreign[i],memory_order_acquire)){ok=0;continue;}
  if(q_definition(b,i,expected,sizeof expected)||!q_same_definition(current,expected)){ok=0;continue;}
  q_backend_checkpoint(b,"cleanup",i);
  if(q_backend_delete(b,i))ok=0;
  if(q_backend_find(b,i,current,sizeof current)!=0)ok=0;
 }
 int equal=1;char *after=malloc(Q_PROFILE_CAP);if(!after){equal=0;ok=0;}
 if(after){for(int i=0;i<6;i++){size_t n=0;if(q_backend_profile(b,i,after,Q_PROFILE_CAP,&n)||n!=s->baseline_len[i]||memcmp(after,s->baseline[i],n))equal=0;}free(after);}
 int remaining=0;
 for(int i=0;i<4;i++)if(q_backend_find(b,i,current,sizeof current)!=0)remaining|=1<<i;
 if(remaining)ok=0;
 atomic_store_explicit(&s->remaining_or_ambiguous_mask,remaining,memory_order_release);
 atomic_store_explicit(&s->other_equal,equal,memory_order_release);
 if(q_backend_finish(b))ok=0;
 atomic_store_explicit(&s->cleanup_ok,ok&&equal,memory_order_release);
 atomic_store_explicit(&s->cleanup_done,1,memory_order_release);
 return ok&&equal?0:-1;
}
static void watcher(struct q_backend *b,struct q_state *s,int parentfd,int from_main,int to_main){
 watched_parent=parentfd;watcher_deadline=q_now_ms()+Q_WATCHER_MS;
 if(setsid()<0){fault(s);(void)cleanup(b,s);_exit(2);}
 char ready='R';if(write(to_main,&ready,1)!=1){fault(s);(void)cleanup(b,s);_exit(2);}
 char command=0;while(!q_should_stop()){struct pollfd p={.fd=from_main,.events=POLLIN};int r=poll(&p,1,20);if(r>0){if(read(from_main,&command,1)==1)break;if(p.revents&POLLHUP)break;}}
 if(command!='S')fault(s);
 if(command=='S'&&!q_should_stop()&&q_kernel_snapshot(b,0)){fault(s);command=0;}
 if(command=='S'&&!q_should_stop()&&q_backend_begin(b)){fault(s);command=0;}
 for(int i=0;i<4 && command=='S' && !q_should_stop();i++){
  char actual[2048],expected[2048];if(q_backend_find(b,i,actual,sizeof actual)!=0){atomic_store(&s->foreign[i],1);fault(s);break;}
  if(q_definition(b,i,expected,sizeof expected)){fault(s);break;}
  atomic_store_explicit(&s->attempted[i],1,memory_order_release);
  q_backend_checkpoint(b,"pre_write",i);if(q_should_stop()){fault(s);break;}
  int written=q_backend_register(b,i,expected);int saved=errno;
  q_backend_checkpoint(b,"post_write",i);
  if(written){if(saved==EEXIST)atomic_store(&s->foreign[i],1);fault(s);break;}
  if(q_backend_find(b,i,actual,sizeof actual)!=1||!q_same_definition(actual,expected)){fault(s);break;}
  atomic_store_explicit(&s->committed[i],1,memory_order_release);
  q_backend_checkpoint(b,"post_commit",i);if(q_should_stop()){fault(s);break;}
  if(q_backend_disabled(b,i)||q_backend_format(b,i,s->formats[i],Q_FORMAT_CAP,&s->format_len[i])){fault(s);break;}
  int format_id=q_format_valid(i,s->formats[i],s->format_len[i]);
  if(q_backend_disabled(b,i)){fault(s);break;}
  if(!format_id){fault(s);break;}for(int old=0;old<i;old++)if(s->format_ids[old]==format_id)format_id=0;
  if(!format_id){fault(s);break;}s->format_ids[i]=format_id;
 }
 int all=1;for(int i=0;i<4;i++)if(!atomic_load(&s->committed[i])||!s->format_len[i])all=0;
 if(q_should_stop())fault(s);
 atomic_store_explicit(&s->formats_ok,all&&!atomic_load(&s->fault),memory_order_release);
 if(atomic_load(&s->formats_ok)&&q_capture_run(b))fault(s);
 (void)cleanup(b,s);
 char done='D';ssize_t notice=write(to_main,&done,1);if(notice!=1)fault(s);/* EPIPE/EAGAIN cannot kill or block cleanup. */
 close(from_main);close(to_main);close(parentfd);_exit(atomic_load(&s->fault)||!atomic_load(&s->cleanup_ok)?2:0);
}
static int output(const char *text,size_t n,long long limit){size_t off=0;while(off<n&&q_now_ms()<limit){ssize_t r=write(STDOUT_FILENO,text+off,n-off);if(r>0){off+=(size_t)r;continue;}if(r<0&&(errno==EAGAIN||errno==EINTR)){struct pollfd p={.fd=STDOUT_FILENO,.events=POLLOUT};(void)poll(&p,1,20);continue;}return -1;}return off==n?0:-1;}
/* C_TRACE_PRIVATE_OUTPUT_OFD_V2: never change inherited open-file-description flags. */
static int private_stdout(void){
 int before=fcntl(STDOUT_FILENO,F_GETFL);
 struct stat inherited,reopened;
 if(before<0||fstat(STDOUT_FILENO,&inherited))return -1;
 /* Qualification writes only to a console or an explicitly supplied pipe. */
 if(!S_ISCHR(inherited.st_mode)&&!S_ISFIFO(inherited.st_mode))return -1;
 int fresh=open("/proc/self/fd/1",O_WRONLY|O_NONBLOCK|O_NOCTTY|O_CLOEXEC);
 if(fresh<0)return -1;
 int valid=!fstat(fresh,&reopened)&&inherited.st_dev==reopened.st_dev&&
   inherited.st_ino==reopened.st_ino&&inherited.st_rdev==reopened.st_rdev&&
   (inherited.st_mode&S_IFMT)==(reopened.st_mode&S_IFMT)&&
   fcntl(STDOUT_FILENO,F_GETFL)==before;
 if(!valid){close(fresh);return -1;}
 if(dup2(fresh,STDOUT_FILENO)<0){close(fresh);return -1;}
 close(fresh);
 int flags=fcntl(STDOUT_FILENO,F_GETFL);
 return flags>=0&&(flags&O_NONBLOCK)?0:-1;
}
static int stream_refusal(struct q_backend *b,int cleanup_known,int no_registration){
 char report[4096];int n=snprintf(report,sizeof report,"{\"Kind\":\"C_TRACE_GRID_OMISSION_SECOND_PHASED_KSTREAM_CAPTURE\",\"NOT_EXECUTION_READY\":false,\"MaterializedSource\":true,\"BootId\":\"1a1dabf0-6854-4e36-a1b4-6cc09f5c14a6\",\"MaterializationInputSHA256\":\"bba24157bdffd92620b0171af34798f4f0fdc32863d489e4271a6cfd8613b79f\",\"Qualified\":false,\"CaptureEvidencePassed\":false,\"KernelSnapshotsPassed\":false,\"CleanupVerified\":%s,\"CleanupUnknown\":%s,\"NoRegistrationAttempted\":%s,",cleanup_known?"true":"false",cleanup_known?"false":"true",no_registration?"true":"false");
 if(n<0||(size_t)n>=sizeof report)return 2;
 int k=q_stream_report_json(b->shared,report+n,sizeof report-(size_t)n);if(k<0)return 2;n+=k;
 k=snprintf(report+n,sizeof report-(size_t)n,"\"DisplayAttempted\":%s,\"NoGeneralWarmAdmission\":true}\n",atomic_load(&b->shared->viewer_exec_allowed)?"true":"false");if(k<0||(size_t)k>=sizeof report-(size_t)n)return 2;n+=k;
 (void)output(report,(size_t)n,q_now_ms()+1000);return 2;
}
int main(int argc,char **argv){
 if(argc==1||(argc==2&&!strcmp(argv[1],"--plan"))){puts("{\"Mode\":\"PLAN\",\"NOT_EXECUTION_READY\":false,\"MaterializedSource\":true,\"BootId\":\"1a1dabf0-6854-4e36-a1b4-6cc09f5c14a6\",\"MaterializationInputSHA256\":\"bba24157bdffd92620b0171af34798f4f0fdc32863d489e4271a6cfd8613b79f\",\"DeviceActions\":0,\"Artifacts\":8,\"ExecutableAuthorization\":false,\"RuntimeABIValidated\":false,\"ProductionReaderEntryLocked\":false}");return 0;}
#if !Q_TEMPLATE_EXECUTION_READY
 puts("{\"Mode\":\"NOT_EXECUTION_READY\",\"DeviceActions\":0,\"KernelLogAPIExecuted\":false,\"DisplayAttempted\":false,\"BoundToNewBootAndFirstBaseline\":false,\"PhasedViewerPinsBound\":false}");return 78;
#endif
 const char *boot=NULL,*nonce=NULL,*fixture=NULL;int guard=-1;
 if(strcmp(argv[1],"--capture-grid-omission-second-phased-kstream"))return 64;
 for(int i=2;i<argc;i+=2){if(i+1>=argc)return 64;if(!strcmp(argv[i],"--boot-id"))boot=argv[i+1];else if(!strcmp(argv[i],"--observer-nonce"))nonce=argv[i+1];else if(!strcmp(argv[i],"--guard-fd")){char *e;long n=strtol(argv[i+1],&e,10);if(*e||n<3||n>1024)return 64;guard=(int)n;}
#ifdef PC_FIXTURE
 else if(!strcmp(argv[i],"--fixture-root"))fixture=argv[i+1];
#endif
 else return 64;}
 if(!valid_boot(boot)||!valid_hex(nonce,32)||strcmp(boot,Q_BOUND_BOOT_ID)||!strcmp(boot,"028a51d4-bbf6-4f82-88ac-6c3e4640323c")||!strcmp(boot,"1804e12d-731e-4202-bc16-68e83538f489")||!strcmp(boot,"61efdb7e-903d-43ce-ba95-89fc261981b7")||!strcmp(boot,"d4861cd2-de55-469c-ad62-759fc90ca1ad"))return 64;
 struct sigaction sa={0};sa.sa_handler=stop_signal;sigemptyset(&sa.sa_mask);sigaction(SIGTERM,&sa,NULL);sigaction(SIGINT,&sa,NULL);signal(SIGPIPE,SIG_IGN);
 if(private_stdout())return 2;
 struct q_state *s=mmap(NULL,sizeof *s,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);if(s==MAP_FAILED)return 2;memset(s,0,sizeof *s);
 if(!atomic_is_lock_free(&s->fault))return 2;
 struct q_backend b={.viewer_exefd=-1,.viewer_readfd=-1,.viewer_writefd=-1};b.shared=s;q_stream_initialize(&b);snprintf(b.nonce,sizeof b.nonce,"%s",nonce);snprintf(b.group,sizeof b.group,"tb375fc_d_second_%s",nonce);
 if(q_backend_prepare(&b,fixture,boot,guard)){int clean=q_backend_finish(&b)==0;printf("{\"Qualified\":false,\"PreflightRefused\":true,\"NoRegistrationAttempted\":true,\"PreparedViewCleanupVerified\":%s}\n",clean?"true":"false");return 2;}
 if(snapshot(&b,s)||!absent_all(&b)){(void)q_backend_finish(&b);puts("{\"Qualified\":false,\"NamesOrBaselineRefused\":true,\"NoRegistrationAttempted\":true}");return 2;}
 /* Fork reader before viewer output pipes exist, so it cannot keep their
  * writer endpoint alive and delay the viewer EOF/restore protocol. */
 if(q_stream_start(&b)){fault(s);int clean=cleanup(&b,s)==0;(void)q_stream_finish(&b,clean,0);return stream_refusal(&b,clean,1);}
 if(q_viewer_parent_prepare(&b)){fault(s);int clean=cleanup(&b,s)==0;(void)q_stream_finish(&b,clean,0);return stream_refusal(&b,clean,1);}
 int parentfd=(int)syscall(SYS_pidfd_open,getpid(),0);int a[2],c[2];
 if(parentfd<0||pipe2(a,O_NONBLOCK|O_CLOEXEC)||pipe2(c,O_NONBLOCK|O_CLOEXEC)){fault(s);int clean=cleanup(&b,s)==0;(void)q_stream_finish(&b,clean,0);return stream_refusal(&b,clean,1);}
#ifdef PC_FIXTURE
 if(getenv("PC_FILL_NOTICE_PIPE")){char junk[1024];memset(junk,'X',sizeof junk);ssize_t n;do{n=write(c[1],junk,sizeof junk);}while(n>0);}
#endif
 long long main_deadline=q_now_ms()+Q_MAIN_MS;pid_t child=fork();
 if(child<0){fault(s);int clean=cleanup(&b,s)==0;(void)q_stream_finish(&b,clean,0);return stream_refusal(&b,clean,1);}
 if(child==0){close(a[1]);close(c[0]);watcher(&b,s,parentfd,a[0],c[1]);}
 q_viewer_parent_after_fork(&b);
 close(parentfd);close(a[0]);close(c[1]);int childfd=(int)syscall(SYS_pidfd_open,child,0);
#ifdef PC_FIXTURE
 if(getenv("PC_CLOSE_NOTICE_READER")){close(c[0]);c[0]=-1;}
#endif
 if(childfd<0){kill(child,SIGTERM);/* Do not write or take over until waitpid confirms actual death. */}
 char pidline[160];int pn=snprintf(pidline,sizeof pidline,"MAIN_PID=%ld WATCHER_PID=%ld BACKEND=%s\n",(long)getpid(),(long)child,q_backend_name());if(output(pidline,(size_t)pn,q_now_ms()+200)){fault(s);stopped=1;}
 int started=0,term_sent=0,status=0,reaped=0;
 while(q_now_ms()<main_deadline+6000){
  if(q_stream_should_stop())stopped=1;
  if(childfd>=0&&dead(childfd)){if(waitpid(child,&status,0)==child)reaped=1;break;}
  if(childfd<0){pid_t r=waitpid(child,&status,WNOHANG);if(r==child){reaped=1;break;}}
  char msg;ssize_t n=read(c[0],&msg,1);if(n==1&&msg=='R'&&!started&&!stopped&&childfd>=0&&q_now_ms()<main_deadline){char start='S';long long stamp=q_stream_now_ns();if(stamp>=0&&write(a[1],&start,1)==1){started=1;q_stream_watcher_started(&b,stamp);}else fault(s);}
  if((stopped||q_now_ms()>=main_deadline||childfd<0)&&!term_sent){fault(s);kill(child,SIGTERM);term_sent=1;}
  struct pollfd p[2]={{.fd=childfd,.events=POLLIN},{.fd=c[0],.events=POLLIN}};(void)poll(p,2,20);
 }
 close(a[1]);close(c[0]);if(childfd>=0)close(childfd);
 if(!reaped){/* No racing viewer fallback or large artifact serialization. */(void)q_stream_contain_unknown_watcher(&b);return stream_refusal(&b,0,0);}
 int viewer_ok=0;
 if(!atomic_load(&s->viewer_reaped)&&q_stream_cancel_before_viewer_adoption(&b))atomic_store(&s->viewer_alive_unknown,1);
 else viewer_ok=q_viewer_parent_finish(&b)==0;
 if(q_viewer_seal(&b))viewer_ok=0;
 if(!atomic_load_explicit(&s->cleanup_done,memory_order_acquire)){(void)q_capture_snapshot(&b);atomic_store(&s->fallback_after_death,1);fault(s);(void)cleanup(&b,s);}
 int capture_evidence_ok=WIFEXITED(status)&&WEXITSTATUS(status)==0&&!atomic_load(&s->fault)&&atomic_load(&s->cleanup_ok)&&atomic_load(&s->formats_ok)&&atomic_load(&s->capture_ok)&&viewer_ok&&atomic_load(&s->viewer_restore_verified)&&atomic_load(&s->viewer_cleanup_marker_verified);
 int display_safe=!atomic_load(&s->viewer_exec_allowed)||(atomic_load(&s->viewer_restore_verified)&&atomic_load(&s->viewer_cleanup_marker_verified)&&atomic_load(&s->viewer_reaped)&&!atomic_load(&s->viewer_alive_unknown)&&!atomic_load(&s->viewer_output_unknown));
 int viewer_lifetime_clean=!atomic_load(&s->viewer_fork_attempted)||(atomic_load(&s->viewer_reaped)&&!atomic_load(&s->viewer_alive_unknown));
 int cleanup_all=atomic_load(&s->cleanup_ok)&&display_safe&&viewer_lifetime_clean;
 if(cleanup_all)q_stream_cleanup_verified(&b);
 int kernel_after_ok=0;
 if(cleanup_all&&atomic_load_explicit(&s->cleanup_done,memory_order_acquire))kernel_after_ok=q_kernel_snapshot(&b,1)==0;
 else q_kernel_after_skipped(&b);
 int kernel_ok=atomic_load(&s->kernel[0].complete)&&kernel_after_ok;
#ifdef PC_FIXTURE
 if(getenv("PC_STREAM_CANCEL_AFTER_CORE_DONE")){
  while(q_now_ms()<b.stream.join_deadline_ms&&!dead(b.stream.pidfd)){struct pollfd p={.fd=b.stream.pidfd,.events=POLLIN};(void)poll(&p,1,20);}
  capture_evidence_ok=0;
 }
#endif
 int stream_ok=q_stream_finish(&b,cleanup_all,capture_evidence_ok&&kernel_ok)==0;
 cleanup_all=cleanup_all&&(!atomic_load(&s->stream.fork_attempted)||atomic_load(&s->stream.reaped))&&!atomic_load(&s->stream.alive_unknown);
 int ok=capture_evidence_ok&&kernel_ok&&stream_ok;
 char *report=malloc(Q_EVENTS*Q_FORMAT_CAP*2+16384);if(!report)return 2;size_t cap=Q_EVENTS*Q_FORMAT_CAP*2+16384;
 int k=snprintf(report,cap,
  "{\"Kind\":\"C_TRACE_GRID_OMISSION_SECOND_PHASED_KSTREAM_CAPTURE\",\"Backend\":\"%s\",\"NOT_EXECUTION_READY\":false,\"MaterializedSource\":true,\"BootId\":\"1a1dabf0-6854-4e36-a1b4-6cc09f5c14a6\",\"MaterializationInputSHA256\":\"bba24157bdffd92620b0171af34798f4f0fdc32863d489e4271a6cfd8613b79f\",\"ObserverNonce\":\"%s\","
  "\"Qualified\":%s,\"CleanupVerified\":%s,\"OtherBaselinePreserved\":%s,"
  "\"WatcherReapedByMain\":true,\"FallbackAfterConfirmedDeath\":%s,"
  "\"RemainingOrAmbiguousEventMask\":%d,"
  "\"DisplayAttempted\":%s,\"DRMNodeMetadataVerified\":%s,\"ExtraSPIAttempted\":false,\"DirectExtraSPI\":false,\"NoGeneralWarmAdmission\":true,"
  "\"DiagnosticExecutionIsNotGeneralWarmAdmission\":true,\"SecondDisplayDiagnosticAdmission\":true,\"OutputFDPolicy\":\"PRIVATE_OPEN_OFD_V2\",",
  q_backend_name(),b.nonce,ok?"true":"false",cleanup_all?"true":"false",
  atomic_load(&s->other_equal)?"true":"false",atomic_load(&s->fallback_after_death)?"true":"false",
  atomic_load(&s->remaining_or_ambiguous_mask),atomic_load(&s->viewer_exec_allowed)?"true":"false",atomic_load(&s->drm_node_metadata_verified)?"true":"false");
 size_t pos=(size_t)k;
 k=snprintf(report+pos,cap-pos,"\"CaptureEvidencePassed\":%s,",capture_evidence_ok?"true":"false");pos+=(size_t)k;
 k=q_kernel_report_json(s,report+pos,cap-pos);if(k<0){free(report);return 2;}pos+=(size_t)k;
 k=q_stream_report_json(s,report+pos,cap-pos);if(k<0){free(report);return 2;}pos+=(size_t)k;
 k=snprintf(report+pos,cap-pos,
  "\"SecondDisplayCapturePassed\":%s,\"CleanupUnknown\":%s,\"Purpose\":\"GRID_OMISSION_SECOND_PHASED_KSTREAM_BASELINE_ONLY\","
  "\"InstanceCreated\":%s,\"InstanceRemoved\":%s,\"CaptureCleanupVerified\":%s,"
  "\"ProbeEnableAttempted\":%s,\"CaptureAttempted\":%s,\"TracePipeRead\":false,\"RootTraceRead\":false,"
  "\"CaptureWindowRequestedMS\":12000,\"CaptureStartMonotonicMS\":%lld,\"CaptureStopMonotonicMS\":%lld,"
  "\"CaptureElapsedMS\":%lld,\"Entries\":%u,\"LossCountersZero\":%s,\"CPUCount\":%u,\"TotalBufferKiB\":%u,"
  "\"ContextInfoVerified\":%s,\"Clock\":\"mono\",\"TraceSnapshotRead\":%s,"
  "\"TracePath\":\"%s\",\"TraceBytes\":%llu,\"TraceSHA256\":\"%s\","
  "\"StatsPath\":\"%s\",\"StatsBytes\":%llu,\"StatsSHA256\":\"%s\",",
  ok?"true":"false",cleanup_all?"false":"true",atomic_load(&s->instance_created)?"true":"false",atomic_load(&s->instance_removed)?"true":"false",
  atomic_load(&s->capture_cleanup_ok)?"true":"false",atomic_load(&s->enable_attempted_mask)?"true":"false",
  atomic_load(&s->trace_on_attempted)?"true":"false",s->capture_start_ms,s->capture_stop_ms,
  s->capture_stop_ms>=s->capture_start_ms?s->capture_stop_ms-s->capture_start_ms:0,s->capture_entries,
  atomic_load(&s->loss_valid)?"true":"false",s->capture_cpus,s->total_buffer_kib,
  atomic_load(&s->context_verified)?"true":"false",atomic_load(&s->trace_snapshot_attempted)?"true":"false",
  s->trace_path,(unsigned long long)s->trace_bytes,s->trace_sha,
  s->stats_path,(unsigned long long)s->stats_bytes,s->stats_sha);
 pos+=(size_t)k;
 k=snprintf(report+pos,cap-pos,
  "\"PriorColdMarkerVerified\":%s,\"PriorColdObserverNonce\":\"%s\",\"RootColdBaselineReportSHA256\":\"%s\","
  "\"SecondMarkerConsumed\":%s,\"TraceResourceCleanupVerified\":%s,\"ViewerForkAttempted\":%s,\"ViewerReaped\":%s,"
  "\"ViewerPID\":%ld,\"ViewerStatus\":%d,\"ViewerTermAttempted\":%s,\"ViewerAliveUnknown\":%s,"
  "\"ViewerOutputComplete\":%s,\"ViewerRestorationVerified\":%s,\"ViewerCleanupMarkerVerified\":%s,"
  "\"ViewerPath\":\"%s\",\"ViewerBytes\":%llu,\"ViewerSHA256\":\"%s\","
  "\"ProfilePath\":\"%s\",\"ProfileBytes\":%llu,\"ProfileSHA256\":\"%s\",\"ProfileVerified\":%s,\"MissedCountersZero\":%s,"
  "\"OwnHitCounts\":{\"read_pre\":%llu,\"read_post\":%llu,\"write_pre\":%llu,\"write_post\":%llu},"
  "\"OwnMissedCounts\":{\"read_pre\":%llu,\"read_post\":%llu,\"write_pre\":%llu,\"write_post\":%llu},\"FormatsHex\":{",
  atomic_load(&s->prior_cold_marker_verified)?"true":"false",Q_PRIOR_COLD_NONCE,b.root_cold_baseline_report_sha,
  atomic_load(&s->second_marker_claimed)?"true":"false",atomic_load(&s->cleanup_ok)?"true":"false",
  atomic_load(&s->viewer_fork_attempted)?"true":"false",atomic_load(&s->viewer_reaped)?"true":"false",
  (long)s->viewer_pid,s->viewer_status,atomic_load(&s->viewer_term_attempted)?"true":"false",atomic_load(&s->viewer_alive_unknown)?"true":"false",
  atomic_load(&s->viewer_output_eof)&&!atomic_load(&s->viewer_output_unknown)?"true":"false",
  atomic_load(&s->viewer_restore_verified)?"true":"false",atomic_load(&s->viewer_cleanup_marker_verified)?"true":"false",
  s->viewer_path,(unsigned long long)s->viewer_bytes,s->viewer_sha,s->profile_path,(unsigned long long)s->profile_bytes,s->profile_sha,
  atomic_load(&s->own_profile_verified)?"true":"false",atomic_load(&s->own_profile_missed_zero)?"true":"false",
  (unsigned long long)s->own_hits[0],(unsigned long long)s->own_hits[1],(unsigned long long)s->own_hits[2],(unsigned long long)s->own_hits[3],
  (unsigned long long)s->own_missed[0],(unsigned long long)s->own_missed[1],(unsigned long long)s->own_missed[2],(unsigned long long)s->own_missed[3]);
 pos+=(size_t)k;
 static const char hex[]="0123456789abcdef";for(int i=0;i<4;i++){k=snprintf(report+pos,cap-pos,"%s\"%s\":\"",i?",":"",q_events[i]);pos+=(size_t)k;for(size_t n=0;n<s->format_len[i]&&n<Q_FORMAT_CAP;n++){unsigned char v=(unsigned char)s->formats[i][n];report[pos++]=hex[v>>4];report[pos++]=hex[v&15];}report[pos++]='"';}memcpy(report+pos,"}}\n",3);pos+=3;
 int written=output(report,pos,q_now_ms()+1000);free(report);munmap(s,sizeof *s);return ok&&!written?0:2;
}
