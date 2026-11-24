/* One supervised second-display instance, stopped snapshot only; never root trace/trace_pipe. */
#define _GNU_SOURCE
#include "qualification.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern bool tb_reviewed_sha256_fd(int,uint64_t,char[65]);

static int join(char *out,size_t cap,const char *a,const char *b){
 int n=snprintf(out,cap,"%s/%s",a,b);return n>0&&(size_t)n<cap?0:-1;
}
static int instance_path(struct q_backend *b,char out[Q_PATH_CAP]){
 char tail[96];int n=snprintf(tail,sizeof tail,"instances/%s",b->group);
 return n>0&&(size_t)n<sizeof tail?join(out,Q_PATH_CAP,b->mountpoint,tail):-1;
}
static int own_instance(struct q_backend *b,char out[Q_PATH_CAP]){
 struct q_state *s=b->shared;struct stat st;
 if(!atomic_load_explicit(&s->instance_created,memory_order_acquire)||instance_path(b,out)||lstat(out,&st)||
    !S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode)||st.st_dev!=s->instance_dev||st.st_ino!=s->instance_ino)return -1;
 return 0;
}
static int control_path(struct q_backend *b,const char *tail,char out[Q_PATH_CAP]){
 char instance[Q_PATH_CAP];return own_instance(b,instance)||join(out,Q_PATH_CAP,instance,tail)?-1:0;
}
static int control_write(struct q_backend *b,const char *tail,const char *value){
 char path[Q_PATH_CAP];if(control_path(b,tail,path))return -1;
 int flags=O_WRONLY|O_CLOEXEC|O_NOFOLLOW;
#ifdef PC_FIXTURE
 flags|=O_TRUNC;
#endif
 int fd=open(path,flags);if(fd<0)return -1;
 size_t n=strlen(value);ssize_t wrote=write(fd,value,n);int saved=wrote==(ssize_t)n?0:(wrote<0?errno:EIO);
 if(close(fd)&&!saved)saved=errno;
 errno=saved;return saved?-1:0;
}
static int control_read(struct q_backend *b,const char *tail,char *raw,size_t cap,size_t *n){
 char path[Q_PATH_CAP];return control_path(b,tail,path)?-1:q_read_file(path,raw,cap,n);
}
static int equals(struct q_backend *b,const char *tail,const char *want){
 char raw[512];size_t n=0;return control_read(b,tail,raw,sizeof raw,&n)||strcmp(raw,want)?-1:0;
}
static int change_verified(struct q_backend *b,const char *tail,const char *value){
 return control_write(b,tail,value)||equals(b,tail,value)?-1:0;
}
static int event_tail(struct q_backend *b,int i,char out[160]){
 int n=snprintf(out,160,"events/%s/%s/enable",b->group,q_events[i]);return n>0&&n<160?0:-1;
}
static int numeric(const char *raw,unsigned cap,unsigned *value){
 while(isspace((unsigned char)*raw))raw++;
 if(!isdigit((unsigned char)*raw))return -1;
 unsigned result=0;
 while(isdigit((unsigned char)*raw)){
  unsigned d=(unsigned)(*raw++-'0');if(result>cap/10||(result==cap/10&&d>cap%10))return -1;
  result=result*10+d;
 }
 while(isspace((unsigned char)*raw))raw++;
 if(*raw)return -1;
 *value=result;return 0;
}
static int numeric_control(struct q_backend *b,const char *tail,unsigned cap,unsigned *value){
 char raw[64];size_t n=0;return control_read(b,tail,raw,sizeof raw,&n)||numeric(raw,cap,value)?-1:0;
}
static int cpu_number(const char *name){
 if(strncmp(name,"cpu",3)||!isdigit((unsigned char)name[3]))return -1;
 unsigned n;if(numeric(name+3,4095,&n))return -1;return (int)n;
}
static int stats_parse(const char *raw,unsigned *entries){
 unsigned found=0;*entries=0;
 for(const char *p=raw;*p;){
  const char *end=strchr(p,'\n');if(!end)end=p+strlen(p);
  size_t n=(size_t)(end-p);if(n>=512)return -1;
  char line[512];memcpy(line,p,n);line[n]=0;
  char *colon=strchr(line,':');if(!colon)return -1;
  *colon++=0;char *key=line;while(isspace((unsigned char)*key))key++;
  char *finish=key+strlen(key);while(finish>key&&isspace((unsigned char)finish[-1]))*--finish=0;
  unsigned bit=0,cap=20000,value=0;
  if(!strcmp(key,"entries"))bit=1;
  else if(!strcmp(key,"overrun"))bit=2;
  else if(!strcmp(key,"commit overrun"))bit=4;
  else if(!strcmp(key,"dropped events"))bit=8;
  if(bit){
   if(found&bit||numeric(colon,cap,&value)||(bit!=1&&value))return -1;
   found|=bit;if(bit==1)*entries=value;
  }
  p=*end?end+1:end;
 }
 return found==15?0:-1;
}
static int read_stats(struct q_backend *b,char *out,size_t cap,size_t *length,unsigned *cpu_count,unsigned *total_entries){
 char path[Q_PATH_CAP];if(control_path(b,"per_cpu",path))return -1;
 DIR *dir=opendir(path);if(!dir)return -1;
 struct dirent *entry;size_t pos=0;unsigned count=0,sum=0;int ok=1;errno=0;
 while((entry=readdir(dir))!=NULL){
  if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
  if(cpu_number(entry->d_name)<0||++count>16){ok=0;break;}
  char tail[96],raw[4096];size_t n=0;unsigned events=0;
  int k=snprintf(tail,sizeof tail,"per_cpu/%s/stats",entry->d_name);
  if(k<0||(size_t)k>=sizeof tail||control_read(b,tail,raw,sizeof raw,&n)||!n||raw[n-1]!='\n'||stats_parse(raw,&events)||events>20000-sum){ok=0;break;}
  sum+=events;k=snprintf(out+pos,cap-pos,"CPU=%s\n",entry->d_name);
  if(k<0||(size_t)k>=cap-pos||(size_t)k+n+1>=cap-pos){ok=0;break;}
  pos+=(size_t)k;memcpy(out+pos,raw,n);pos+=n;out[pos++]='\n';
  errno=0;
 }
 if(errno)ok=0;
 if(closedir(dir))ok=0;
 if(!ok||!count)return -1;
 out[pos]=0;*length=pos;*cpu_count=count;*total_entries=sum;return 0;
}
static int utf8_valid(const unsigned char *p,size_t n){
 for(size_t i=0;i<n;){
  unsigned c=p[i++],need=0,code=0,min=0;
  if(c<128){if(c<32&&c!='\n'&&c!='\t'&&c!='\r')return 0;continue;}
  if(c>=0xc2&&c<=0xdf){need=1;code=c&31;min=0x80;}
  else if(c>=0xe0&&c<=0xef){need=2;code=c&15;min=0x800;}
  else if(c>=0xf0&&c<=0xf4){need=3;code=c&7;min=0x10000;}
  else return 0;
  if(need>n-i)return 0;
  while(need--){unsigned next=p[i++];if((next&0xc0)!=0x80)return 0;code=(code<<6)|(next&63);}
  if(code<min||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return 0;
 }
 return 1;
}
static int seal_file(struct q_backend *b,const char *name,const char *raw,size_t n,char path[Q_PATH_CAP],char sha[65]){
 if(join(path,Q_PATH_CAP,b->claim,name))return -1;
 int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
 size_t pos=0;unsigned interrupted=0;int ok=1;
 while(pos<n){
  ssize_t got=write(fd,raw+pos,n-pos);
  if(got<0&&errno==EINTR&&++interrupted<16)continue;
  if(got<=0){ok=0;break;}pos+=(size_t)got;
 }
 if(ok&&fchmod(fd,0400))ok=0;
 if(close(fd))ok=0;
 if(!ok)return -1;
 fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
 struct stat st;
 ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&(st.st_mode&0777)==0400&&
    st.st_size>=0&&(uint64_t)st.st_size==n&&tb_reviewed_sha256_fd(fd,n,sha);
 if(close(fd))ok=0;
 return ok?0:-1;
}
int q_profile_names_unambiguous(const char *raw){
 for(const char *p=raw;*p;){
  const char *end=strchr(p,'\n');if(!end)end=p+strlen(p);
  const char *finish=p;while(finish<end&&!isspace((unsigned char)*finish))finish++;
  const char *name=NULL;
  for(const char *q=p;q<finish;q++)if(*q=='/'||*q==':')name=q+1;
  if(name)for(int i=0;i<4;i++)if((size_t)(finish-name)==strlen(q_events[i])&&!memcmp(name,q_events[i],(size_t)(finish-name)))return -1;
  p=*end?end+1:end;
 }
 return 0;
}
static int count64(const char *raw,uint64_t *value){
 if(!isdigit((unsigned char)*raw))return -1;
 uint64_t out=0;
 while(isdigit((unsigned char)*raw)){
  unsigned d=(unsigned)(*raw++-'0');if(out>UINT64_MAX/10||(out==UINT64_MAX/10&&d>UINT64_MAX%10))return -1;
  out=out*10+d;
 }
 if(*raw)return -1;
 *value=out;return 0;
}
static int own_profile(struct q_backend *b){
 char path[Q_PATH_CAP];if(join(path,sizeof path,b->mountpoint,"kprobe_profile"))return -1;
 char *raw=malloc(Q_PROFILE_CAP);if(!raw)return -1;size_t n=0,pos=0;unsigned mask=0;int ok=0,missed_zero=1;char own[2048];
 if(q_read_file(path,raw,Q_PROFILE_CAP,&n))goto out;
 for(const char *p=raw;*p;){
  const char *end=strchr(p,'\n');if(!end)end=p+strlen(p);
  size_t len=(size_t)(end-p);if(len>=256)goto out;
  char line[256],event[80],hits[32],missed[32],extra;
  memcpy(line,p,len);line[len]=0;
  if(sscanf(line,"%79s %31s %31s %c",event,hits,missed,&extra)!=3)goto out;
  int found=-1;for(int i=0;i<4;i++)if(!strcmp(event,q_events[i]))found=i;
  if(found>=0){
   uint64_t hit_count=0,miss_count=0;
   if(mask&(1u<<found)||count64(hits,&hit_count)||count64(missed,&miss_count)||pos+len+1>=sizeof own)goto out;
   b->shared->own_hits[found]=hit_count;b->shared->own_missed[found]=miss_count;if(miss_count)missed_zero=0;mask|=1u<<found;
   memcpy(own+pos,p,len);pos+=len;own[pos++]='\n';
  }
  p=*end?end+1:end;
 }
 if(mask!=15)goto out;
 if(seal_file(b,"profile.txt",own,pos,b->shared->profile_path,b->shared->profile_sha))goto out;
 b->shared->profile_bytes=pos;
 atomic_store_explicit(&b->shared->own_profile_verified,1,memory_order_release);
 atomic_store_explicit(&b->shared->own_profile_missed_zero,missed_zero,memory_order_release);ok=missed_zero;
out:
 free(raw);return ok?0:-1;
}

#ifdef PC_FIXTURE
static int fixture_put(const char *base,const char *tail,const char *raw){
 char path[Q_PATH_CAP];if(join(path,sizeof path,base,tail))return -1;
 int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return -1;
 size_t n=strlen(raw);int ok=write(fd,raw,n)==(ssize_t)n;if(close(fd))ok=0;return ok?0:-1;
}
static int fixture_dir(const char *base,const char *tail){char path[Q_PATH_CAP];return join(path,sizeof path,base,tail)||mkdir(path,0700)?-1:0;}
static int fixture_setup(struct q_backend *b,const char *instance){
 const char *dirs[]={"options","events","per_cpu","per_cpu/cpu0"};
 for(unsigned i=0;i<sizeof dirs/sizeof dirs[0];i++)if(fixture_dir(instance,dirs[i]))return -1;
 char group[96];snprintf(group,sizeof group,"events/%s",b->group);if(fixture_dir(instance,group))return -1;
 for(int i=0;i<4;i++){
  char event[160],enable[180];snprintf(event,sizeof event,"events/%s/%s",b->group,q_events[i]);
  if(fixture_dir(instance,event))return -1;
  snprintf(enable,sizeof enable,"%s/enable",event);if(fixture_put(instance,enable,"0\n"))return -1;
 }
 const char *files[]={"tracing_on","current_tracer","trace_clock","buffer_size_kb","buffer_total_size_kb","options/context-info","set_event","trace","per_cpu/cpu0/stats"};
 const char *stats=getenv("PC_CAPTURE_BAD_LOSS")?"entries: 0\noverrun: 1\ncommit overrun: 0\ndropped events: 0\n":"entries: 0\noverrun: 0\ncommit overrun: 0\ndropped events: 0\n";
 const char *values[]={"1\n","nop\n","local [mono] boot\n","256\n","256\n","1\n","","# tracer: nop\n# empty synthetic PC fixture\n",stats};
 for(unsigned i=0;i<sizeof files/sizeof files[0];i++)if(fixture_put(instance,files[i],values[i]))return -1;
 char profile[Q_PATH_CAP];if(join(profile,sizeof profile,b->mountpoint,"kprobe_profile"))return -1;
 int fd=open(profile,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
 const char *rows="  read_pre  0  0\n  read_post  0  0\n  write_pre  0  0\n  write_post  0  0\n";
 size_t size=strlen(rows);int ok=write(fd,rows,size)==(ssize_t)size;if(close(fd))ok=0;if(!ok)return -1;
 return 0;
}
static int fixture_set_event(struct q_backend *b,int on){
 char raw[1024];size_t pos=0;raw[0]=0;
 if(on){for(int i=0;i<4;i++){int n=snprintf(raw+pos,sizeof raw-pos,"%s:%s\n",b->group,q_events[i]);if(n<0||(size_t)n>=sizeof raw-pos)return -1;pos+=(size_t)n;}}
 return control_write(b,"set_event",raw);
}
static int fixture_remove(struct q_backend *b,const char *instance){
 const char *files[]={"tracing_on","current_tracer","trace_clock","buffer_size_kb","buffer_total_size_kb","options/context-info","set_event","trace","per_cpu/cpu0/stats"};
 for(unsigned i=0;i<sizeof files/sizeof files[0];i++){char path[Q_PATH_CAP];if(join(path,sizeof path,instance,files[i])||unlink(path))return -1;}
 for(int i=0;i<4;i++){
  char tail[160],path[Q_PATH_CAP];if(event_tail(b,i,tail)||join(path,sizeof path,instance,tail)||unlink(path))return -1;
  snprintf(tail,sizeof tail,"events/%s/%s",b->group,q_events[i]);if(join(path,sizeof path,instance,tail)||rmdir(path))return -1;
 }
 char tail[96],path[Q_PATH_CAP];snprintf(tail,sizeof tail,"events/%s",b->group);
 if(join(path,sizeof path,instance,tail)||rmdir(path))return -1;
 const char *dirs[]={"events","per_cpu/cpu0","per_cpu","options"};
 for(unsigned i=0;i<sizeof dirs/sizeof dirs[0];i++)if(join(path,sizeof path,instance,dirs[i])||rmdir(path))return -1;
 return 0;
}
#endif

static int only_four_enabled(struct q_backend *b){
 char raw[2048];size_t n=0;if(control_read(b,"set_event",raw,sizeof raw,&n))return -1;
 unsigned mask=0;
 for(char *p=raw;*p;){
  char *end=strchr(p,'\n');if(!end)end=p+strlen(p);char held=*end;*end=0;
  int found=-1;
  for(int i=0;i<4;i++){char expected[128];snprintf(expected,sizeof expected,"%s:%s",b->group,q_events[i]);if(!strcmp(p,expected)){found=i;break;}}
  if(found<0||(mask&(1u<<found)))return -1;
  mask|=1u<<found;p=held?end+1:end;
 }
 return mask==15?0:-1;
}
static int stop_disable(struct q_backend *b){
 int ok=change_verified(b,"tracing_on","0\n")==0;
 if(!b->shared->capture_stop_ms)b->shared->capture_stop_ms=q_now_ms();
 for(int i=0;i<4;i++){char tail[160];if(event_tail(b,i,tail)||change_verified(b,tail,"0\n"))ok=0;}
#ifdef PC_FIXTURE
 if(fixture_set_event(b,0))ok=0;
#endif
 if(equals(b,"set_event",""))ok=0;
 return ok?0:-1;
}
int q_capture_cleanup(struct q_backend *b){
 struct q_state *s=b->shared;
 if(!atomic_load_explicit(&s->instance_attempted,memory_order_acquire)){
  atomic_store_explicit(&s->capture_cleanup_ok,1,memory_order_release);return 0;
 }
 char instance[Q_PATH_CAP];if(instance_path(b,instance))return -1;
 if(!atomic_load_explicit(&s->instance_created,memory_order_acquire)){
  struct stat st;if(lstat(instance,&st)&&errno==ENOENT){atomic_store(&s->capture_cleanup_ok,1);return 0;}
  return -1;/* Ambiguous mkdir: never delete another owner's instance. */
 }
 if(atomic_load_explicit(&s->instance_removed,memory_order_acquire)){
  struct stat st;return lstat(instance,&st)&&errno==ENOENT?0:-1;
 }
 if(own_instance(b,instance)||stop_disable(b))return -1;
 q_backend_checkpoint(b,"before_instance_remove",-1);
#ifdef PC_FIXTURE
 if(fixture_remove(b,instance))return -1;
#endif
 if(rmdir(instance))return -1;
 struct stat st;if(!lstat(instance,&st)||errno!=ENOENT)return -1;
 atomic_store_explicit(&s->instance_removed,1,memory_order_release);
 atomic_store_explicit(&s->capture_cleanup_ok,1,memory_order_release);return 0;
}
int q_capture_run(struct q_backend *b){
 struct q_state *s=b->shared;char instance[Q_PATH_CAP];struct stat st;
 if(q_should_stop()||q_backend_begin(b))return -1;
 if(instance_path(b,instance))return -1;
 if(!lstat(instance,&st)||errno!=ENOENT)return -1;
 atomic_store_explicit(&s->instance_attempted,1,memory_order_release);
 q_backend_checkpoint(b,"pre_instance_mkdir",-1);
 if(q_should_stop()||mkdir(instance,0700))return -1;
 q_backend_checkpoint(b,"post_instance_mkdir_precommit",-1);
 if(lstat(instance,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return -1;
 s->instance_dev=st.st_dev;s->instance_ino=st.st_ino;
 atomic_store_explicit(&s->instance_created,1,memory_order_release);
#ifdef PC_FIXTURE
 if(fixture_setup(b,instance))return -1;
#endif
 if(change_verified(b,"tracing_on","0\n")||equals(b,"set_event","")||
    change_verified(b,"current_tracer","nop\n")||control_write(b,"trace_clock","mono\n")||
    change_verified(b,"buffer_size_kb","256\n")||change_verified(b,"options/context-info","1\n"))return -1;
#ifdef PC_FIXTURE
 if(control_write(b,"trace_clock","local [mono] boot\n"))return -1;
#endif
 char clock[512];size_t clock_n=0;
 if(control_read(b,"trace_clock",clock,sizeof clock,&clock_n)||!strstr(clock,"[mono]"))return -1;
 atomic_store_explicit(&s->context_verified,1,memory_order_release);
 unsigned total_buffer=0;
 if(numeric_control(b,"buffer_total_size_kb",4096,&total_buffer)||!total_buffer)return -1;
 s->total_buffer_kib=total_buffer;
 char *stats=malloc(Q_STATS_CAP);if(!stats)return -1;
 size_t stats_n=0;unsigned cpus=0,entries=0;int ok=0;
 if(read_stats(b,stats,Q_STATS_CAP,&stats_n,&cpus,&entries)||entries||cpus*256>4096)goto out;
 for(int i=0;i<4;i++){
  if(q_should_stop())goto out;
  char tail[160];if(event_tail(b,i,tail)||equals(b,tail,"0\n"))goto out;
  atomic_fetch_or_explicit(&s->enable_attempted_mask,1<<i,memory_order_release);
  q_backend_checkpoint(b,"pre_enable",i);
#ifdef PC_FIXTURE
  const char *fail=getenv("PC_CAPTURE_ENABLE_FAIL_AT");if(fail&&atoi(fail)==i)goto out;
#endif
  if(q_should_stop()||change_verified(b,tail,"1\n"))goto out;
  q_backend_checkpoint(b,"post_enable",i);
 }
#ifdef PC_FIXTURE
 if(fixture_set_event(b,1))goto out;
#endif
 if(only_four_enabled(b)||q_should_stop())goto out;
 atomic_store_explicit(&s->trace_on_attempted,1,memory_order_release);
 s->capture_start_ms=q_now_ms();
 if(change_verified(b,"tracing_on","1\n"))goto out;
 q_backend_checkpoint(b,"capture_running",-1);
 int viewer_ok=q_viewer_run(b)==0;
 if(q_capture_snapshot(b))goto out;
 ok=viewer_ok;
out:
 free(stats);return ok?0:-1;
}
int q_capture_snapshot(struct q_backend *b){
 struct q_state *s=b->shared;
 if(atomic_load_explicit(&s->capture_ok,memory_order_acquire))return 0;
 if(!atomic_load(&s->instance_created)||atomic_load(&s->instance_removed)||!atomic_load(&s->trace_on_attempted))return -1;
 if(stop_disable(b))return -1;
 int viewer_sealed=q_viewer_seal(b)==0;
 char *stats=malloc(Q_STATS_CAP),*trace=malloc(Q_TRACE_CAP+2);if(!stats||!trace){free(stats);free(trace);return -1;}
 size_t stats_n=0,trace_n=0;unsigned cpus=0,entries=0;int ok=0;
 if(read_stats(b,stats,Q_STATS_CAP,&stats_n,&cpus,&entries)||cpus*256>4096)goto out;
 s->capture_cpus=cpus;s->capture_entries=entries;
 atomic_store_explicit(&s->loss_valid,1,memory_order_release);
 atomic_store_explicit(&s->trace_snapshot_attempted,1,memory_order_release);
 if(control_read(b,"trace",trace,Q_TRACE_CAP+2,&trace_n)||!trace_n||trace_n>Q_TRACE_CAP||!utf8_valid((unsigned char *)trace,trace_n))goto out;
 if(seal_file(b,"trace.txt",trace,trace_n,s->trace_path,s->trace_sha)||seal_file(b,"stats.txt",stats,stats_n,s->stats_path,s->stats_sha))goto out;
 s->trace_bytes=trace_n;s->stats_bytes=stats_n;
 if(own_profile(b)||!viewer_sealed)goto out;
 atomic_store_explicit(&s->capture_ok,1,memory_order_release);ok=1;
out:
 free(stats);free(trace);return ok?0:-1;
}
