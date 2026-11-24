/* Actual Linux backend; not linked into the PC fixture executable. */
#define _GNU_SOURCE
#include "qualification.h"
#include "sha256.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* Root links the reviewed descriptor implementation unchanged for module/viewer files (<1MiB). */
extern bool tb_reviewed_sha256_fd(int,uint64_t,char[65]);
const char *q_backend_name(void){return "ACTUAL_TRACEFS_GRID_OMISSION_SECOND_PHASED_KSTREAM_CAPTURE";}
void q_backend_checkpoint(struct q_backend *b,const char *stage,int i){(void)b;(void)stage;(void)i;}
static int join(char *out,size_t cap,const char *a,const char *b){size_t x=strlen(a),y=strlen(b);if(x+y+2>cap)return -1;memcpy(out,a,x);out[x]='/';memcpy(out+x+1,b,y+1);return 0;}
static int read_small(const char *path,char *out,size_t cap){size_t n;return q_read_file(path,out,cap,&n);}
static int root_directory(const struct stat *st){
 return S_ISDIR(st->st_mode)&&st->st_uid==0&&st->st_gid==0&&
  (st->st_mode&0700)==0700&&!(st->st_mode&0022);
}
/* Metadata only. Never open card0, issue a DRM query, or create a device node. */
static int drm_node_metadata_paths(const char *dev_path,const char *sysfs_dev_path){
 int dev=open(dev_path,O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dev<0)return -1;
 struct stat ds,rs,leaf,again;int dri=-1,ok=0;
 if(fstat(dev,&ds)||!root_directory(&ds))goto out;
 dri=openat(dev,"dri",O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dri<0)goto out;
 if(fstat(dri,&rs)||!root_directory(&rs))goto out;
 if(fstatat(dri,"card0",&leaf,AT_SYMLINK_NOFOLLOW)||!S_ISCHR(leaf.st_mode)||
    major(leaf.st_rdev)!=226||minor(leaf.st_rdev)!=0||leaf.st_uid!=0||leaf.st_gid!=0||
    (leaf.st_mode&0777)!=0600)goto out;
 if(fstatat(dev,"dri",&again,AT_SYMLINK_NOFOLLOW)||!root_directory(&again)||
    again.st_dev!=rs.st_dev||again.st_ino!=rs.st_ino)goto out;
 char raw[32];size_t n=0;
 if(q_read_file(sysfs_dev_path,raw,sizeof raw,&n)||strcmp(raw,"226:0\n"))goto out;
 ok=1;
out:
 if(dri>=0&&close(dri))ok=0;
 if(close(dev))ok=0;
 return ok?0:-1;
}
static int drm_node_metadata(struct q_backend *b){
 atomic_store_explicit(&b->shared->drm_node_metadata_verified,0,memory_order_release);
 int ok=drm_node_metadata_paths("/dev","/sys/class/drm/card0/dev")==0;
 atomic_store_explicit(&b->shared->drm_node_metadata_verified,ok,memory_order_release);
 return ok?0:-1;
}
static int prior_cold_marker(struct q_backend *b,const char *boot){
 atomic_store_explicit(&b->shared->prior_cold_marker_verified,0,memory_order_release);
 int ok=q_prior_cold_marker_valid("/run/tb-c-grid-omission-first-consumed",boot)==0;
 atomic_store_explicit(&b->shared->prior_cold_marker_verified,ok,memory_order_release);
 return ok?0:-1;
}
static int second_marker_absent(void){
 struct stat st;return lstat("/run/tb-c-grid-omission-second-consumed",&st)<0&&errno==ENOENT&&lstat("/run/tb-c-cold-first-consumed",&st)<0&&errno==ENOENT&&lstat("/run/tb-c-supervised-second-consumed",&st)<0&&errno==ENOENT?0:-1;
}
static int instances_empty(struct q_backend *b){
 char path[Q_PATH_CAP];if(join(path,sizeof path,b->mountpoint,"instances"))return -1;
 DIR *dir=opendir(path);if(!dir)return -1;
 int empty=1;struct dirent *entry;
 errno=0;
 while((entry=readdir(dir))!=NULL){if(strcmp(entry->d_name,".")&&strcmp(entry->d_name,"..")){empty=0;break;}}
 int read_error=errno;
 int close_error=closedir(dir);
 return empty&&!read_error&&!close_error?0:-1;
}
static int guard_hash(const char *raw,const char *key,char out[65]){
 size_t keylen=strlen(key);int found=0;
 for(const char *p=raw;*p;){
  const char *end=strchr(p,'\n');if(!end)end=p+strlen(p);
  if((size_t)(end-p)==keylen+64&&!memcmp(p,key,keylen)){
   if(found++)return -1;
   int nonzero=0;for(int i=0;i<64;i++){char c=p[keylen+(size_t)i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return -1;out[i]=c;if(c!='0')nonzero=1;}
   if(!nonzero)return -1;
   out[64]=0;
  }
  p=*end?end+1:end;
 }
 return found==1?0:-1;
}
static int proc_hash_matches(const char *path,const char *want){
 char *raw=malloc(Q_PROFILE_CAP);if(!raw)return -1;size_t n=0;uint8_t digest[32];char hex[65];
 int ok=q_read_file(path,raw,Q_PROFILE_CAP,&n)==0;
 if(ok){hash_bytes(raw,n,digest);hex32(digest,hex);ok=!strcmp(hex,want);}
 free(raw);return ok?0:-1;/* n==0 is the valid SHA256 of an empty proc file. */
}
static int readonly_guard(int fd,const char *boot,const char *nonce,char baseline_report_sha[65]){
 struct stat st;int flags=fcntl(fd,F_GETFL);if(fd<3||flags<0||(flags&O_ACCMODE)!=O_RDONLY||fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||(st.st_mode&0777)!=0400||st.st_size<100||st.st_size>2048)return -1;
 char raw[2049];ssize_t n=pread(fd,raw,sizeof raw-1,0);if(n!=st.st_size||n<0||memchr(raw,0,(size_t)n))return -1;raw[n]=0;
 const char *prefix="C_TRACE_GRID_OMISSION_SECOND_PHASED_KSTREAM_GUARD_V1\n";if(strncmp(raw,prefix,strlen(prefix)))return -1;
 char cold[65],replug[65],cgroup[65],mountinfo[65],baseline[65];
 if(guard_hash(raw,"ROOT_COLD_STATE_SHA256=",cold)||guard_hash(raw,"ROOT_CURRENT_REPLUG_SHA256=",replug)||
    guard_hash(raw,"PID1_CGROUP_SHA256=",cgroup)||guard_hash(raw,"PID1_MOUNTINFO_SHA256=",mountinfo)||
    guard_hash(raw,"ROOT_COLD_BASELINE_REPORT_SHA256=",baseline)||strcmp(baseline,Q_PRIOR_COLD_REPORT_SHA))return -1;
 char expect[2049];int k=snprintf(expect,sizeof expect,"BOOT_ID=%s\nOBSERVER_NONCE=%s\nKERNEL_RELEASE=%s\nMODULE_SHA256=%s\nBTF_SHA256=%s\nROOT_COLD_STATE_SHA256=%s\nROOT_CURRENT_REPLUG_SHA256=%s\nPID1_CGROUP_SHA256=%s\nPID1_MOUNTINFO_SHA256=%s\nPRIOR_COLD_OBSERVER_NONCE=%s\nROOT_COLD_BASELINE_REPORT_SHA256=%s\nROOT_MATERIALIZATION_INPUT_SHA256=bba24157bdffd92620b0171af34798f4f0fdc32863d489e4271a6cfd8613b79f\nROOT_READONLY_PREFLIGHT_VERIFIED=1\nMODULE_FILE_HASH_VERIFIED=1\nMODULE_LIVE_STATE_VERIFIED=1\nNO_MODULE_OR_HALL_UNLOAD_BY_WORKFLOW=1\nKERNEL_MODULE_REFERENCE_HELD_BY_HELPER=0\nRAM_RUN_VERIFIED=1\nPRIOR_FIRST_DISPLAY_CAPTURE_VERIFIED=1\nROOT_COLD_BASELINE_VERIFIED=1\nROOT_OPTICAL_STABLE_CONFIRMED=1\nSECOND_DISPLAY_UNUSED_VERIFIED=1\nFIXED_VIEWER_SHA256=%s\nFIXED_IMAGE_SHA256=%s\nROOT_VIEWER_IMAGE_PINS_VERIFIED=1\nDRM_NODE_METADATA_VERIFIED=1\nROOT_PHASED_VIEWER_SOURCE_BUILD_VERIFIED=1\nREQUIRE_VIEWER_PHASE_RECORDS=1\nREQUIRE_KERNEL_BEFORE_AFTER=1\nREQUIRE_KERNEL_STREAM=1\nSECOND_DISPLAY_DIAGNOSTIC_ADMISSION=1\nNO_GENERAL_WARM_ADMISSION=1\nCAPTURE_PURPOSE=GRID_OMISSION_SECOND_PHASED_KSTREAM_BASELINE_ONLY\nVIEWER_BUDGET_MS=12000\n",boot,nonce,Q_KERNEL,Q_MODULE_SHA,Q_BTF_SHA,cold,replug,cgroup,mountinfo,Q_PRIOR_COLD_NONCE,baseline,Q_VIEWER_SHA,Q_IMAGE_SHA);
 if(k<0||(size_t)k>=sizeof expect||strcmp(raw+strlen(prefix),expect))return -1;
 struct stat after;if(fstat(fd,&after)||after.st_ino!=st.st_ino||after.st_dev!=st.st_dev||after.st_size!=st.st_size||after.st_mtim.tv_sec!=st.st_mtim.tv_sec||after.st_mtim.tv_nsec!=st.st_mtim.tv_nsec)return -1;
 if(proc_hash_matches("/proc/1/cgroup",cgroup)||proc_hash_matches("/proc/1/mountinfo",mountinfo))return -1;
 if(baseline_report_sha)memcpy(baseline_report_sha,baseline,sizeof baseline);
 return 0;/* Root guard is the trusted BTF/read-only evidence producer, not this ASCII parser. */
}
static int module_file_pin(void){int fd=open("/lib/modules/nt36532.ko",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;struct stat s;char sha[65];int ok=!fstat(fd,&s)&&S_ISREG(s.st_mode)&&s.st_size==522784&&tb_reviewed_sha256_fd(fd,522784,sha)&&!strcmp(sha,Q_MODULE_SHA);close(fd);return ok?0:-1;}
static int image_metadata(void){
 int fd=open(Q_IMAGE_PATH,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;struct stat st;
 int ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&st.st_gid==0&&st.st_size==21667840&&!(st.st_mode&022);
 if(close(fd))ok=0;
 return ok?0:-1;
}
static int viewer_fd_pin(int fd){
 struct stat st;char sha[65];return fd>=0&&!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&
  !(st.st_mode&022)&&st.st_size==597816&&tb_reviewed_sha256_fd(fd,597816,sha)&&!strcmp(sha,Q_VIEWER_SHA)?0:-1;
}
static int ensure_view(struct q_backend *b){struct statfs st;if(!b->prepared)return -1;if(!statfs(b->mountpoint,&st)&&st.f_type==TRACEFS_MAGIC)return 0;/* Confirmed-dead fallback may recreate only its already-owned private view. */if(mount("tracefs",b->mountpoint,"tracefs",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL))return -1;b->mounted=1;return 0;}
int q_backend_prepare(struct q_backend *b,const char *fixture,const char *boot,int guard){
 if(!Q_TEMPLATE_EXECUTION_READY||!boot||strcmp(boot,Q_BOUND_BOOT_ID))return -1;/* No current or future device admission is bound. */
 if(fixture||geteuid()!=0||readonly_guard(guard,boot,b->nonce,b->root_cold_baseline_report_sha)||module_file_pin())return -1;
 if(drm_node_metadata(b)||prior_cold_marker(b,boot)||second_marker_absent())return -1;
 snprintf(b->boot_id,sizeof b->boot_id,"%s",boot);b->guard_fd=guard;
 b->viewer_exefd=open(Q_VIEWER_PATH,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
 if(viewer_fd_pin(b->viewer_exefd)||image_metadata())return -1;
 struct utsname u;if(uname(&u)||strcmp(u.release,Q_KERNEL))return -1;
 char value[128];if(read_small("/proc/sys/kernel/random/boot_id",value,sizeof value))return -1;value[strcspn(value,"\n")]=0;if(strcmp(value,boot))return -1;
 char mounts[Q_PROFILE_CAP];if(read_small("/proc/self/mountinfo",mounts,sizeof mounts)||strstr(mounts," - tracefs "))return -1;
 ssize_t ns=readlink("/proc/self/ns/mnt",b->parent_ns,sizeof b->parent_ns-1);if(ns<=0)return -1;b->parent_ns[ns]=0;
 struct statfs run;if(statfs("/run",&run)||(run.f_type!=TMPFS_MAGIC&&run.f_type!=RAMFS_MAGIC))return -1;
 int runfd=open("/run",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(runfd<0)return -1;
 char name[96];snprintf(name,sizeof name,"tb-c-grid-omission-second-%s",b->nonce);if(mkdirat(runfd,name,0700)){close(runfd);return -1;}close(runfd);
 if(join(b->claim,sizeof b->claim,"/run",name)||join(b->mountpoint,sizeof b->mountpoint,b->claim,"tracefs"))return -1;
 int claim=open(b->claim,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(claim<0)return -1;
 int marker=openat(claim,"CLAIM",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0400);if(marker<0){close(claim);return -1;}
 char text[256];int len=snprintf(text,sizeof text,"GRID_OMISSION_SECOND_PHASED_KSTREAM_BASELINE_ONLY %s %s %ld\n",boot,b->nonce,(long)getpid());if(write(marker,text,(size_t)len)!=len){close(marker);close(claim);return -1;}close(marker);
 if(mkdirat(claim,"tracefs",0700)){close(claim);return -1;}close(claim);
 if(unshare(CLONE_NEWNS)||mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL))return -1;
 char ownns[96];ns=readlink("/proc/self/ns/mnt",ownns,sizeof ownns-1);if(ns<=0)return -1;ownns[ns]=0;if(!strcmp(ownns,b->parent_ns))return -1;
 if(read_small("/proc/self/mountinfo",mounts,sizeof mounts)||strstr(mounts," shared:")||strstr(mounts," master:"))return -1;
 b->prepared=1;if(ensure_view(b))return -1;
 if(instances_empty(b))return -1;
 return 0;
}
int q_backend_begin(struct q_backend *b){
 /* Recheck the off display and module before creating this attempt's instance. */
 char value[128];
 if(read_small("/sys/class/drm/card0-DSI-1/enabled",value,sizeof value)||strcmp(value,"disabled\n"))return -1;
 if(read_small("/sys/module/nt36532/initstate",value,sizeof value)||strcmp(value,"live\n"))return -1;
 if(module_file_pin())return -1;
 if(instances_empty(b))return -1;
 return 0;
}
int q_backend_claim_second_once(struct q_backend *b){
 char value[128],baseline[65];struct statfs run;
 if(readonly_guard(b->guard_fd,b->boot_id,b->nonce,baseline)||strcmp(baseline,b->root_cold_baseline_report_sha)||
    module_file_pin()||viewer_fd_pin(b->viewer_exefd)||image_metadata()||
    read_small("/sys/class/drm/card0-DSI-1/enabled",value,sizeof value)||strcmp(value,"disabled\n")||
    read_small("/sys/module/nt36532/initstate",value,sizeof value)||strcmp(value,"live\n")||
    statfs("/run",&run)||(run.f_type!=TMPFS_MAGIC&&run.f_type!=RAMFS_MAGIC)||drm_node_metadata(b)||
    prior_cold_marker(b,b->boot_id)||second_marker_absent())return -1;
 int fd=open("/run/tb-c-grid-omission-second-consumed",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0400);if(fd<0)return -1;
 /* O_EXCL success consumes this diagnostic attempt even if the next write fails. */
 atomic_store_explicit(&b->shared->second_marker_claimed,1,memory_order_release);
 char text[160];int n=snprintf(text,sizeof text,"GRID_OMISSION_SECOND_DISPLAY_ATTEMPT %s %s\n",b->boot_id,b->nonce);
 int ok=n>0&&(size_t)n<sizeof text&&write(fd,text,(size_t)n)==n;
 if(close(fd))ok=0;
 return ok?0:-1;
}
int q_backend_profile(struct q_backend *b,int i,char *out,size_t cap,size_t *n){char path[Q_PATH_CAP];if(ensure_view(b)||join(path,sizeof path,b->mountpoint,q_profiles[i]))return -1;return q_read_file(path,out,cap,n);}
int q_backend_find(struct q_backend *b,int i,char *out,size_t cap){char *raw=malloc(Q_PROFILE_CAP);if(!raw)return -1;size_t n=0;int result=-1;if(!q_backend_profile(b,0,raw,Q_PROFILE_CAP,&n)){char key[128];snprintf(key,sizeof key,"p:%s/%s",b->group,q_events[i]);result=q_find_in_profile(raw,key,out,cap);}free(raw);return result;}
int q_backend_register(struct q_backend *b,int i,const char *definition){(void)i;char path[Q_PATH_CAP];if(ensure_view(b)||join(path,sizeof path,b->mountpoint,"kprobe_events"))return -1;return q_write_all_once(path,definition,strlen(definition));}
int q_backend_disabled(struct q_backend *b,int i){char tail[160],path[Q_PATH_CAP],raw[16];snprintf(tail,sizeof tail,"events/%s/%s/enable",b->group,q_events[i]);if(join(path,sizeof path,b->mountpoint,tail)||read_small(path,raw,sizeof raw)||strcmp(raw,"0\n"))return -1;return 0;}
int q_backend_format(struct q_backend *b,int i,char *out,size_t cap,size_t *n){char tail[160],path[Q_PATH_CAP];snprintf(tail,sizeof tail,"events/%s/%s/format",b->group,q_events[i]);if(join(path,sizeof path,b->mountpoint,tail))return -1;return q_read_file(path,out,cap,n);}
int q_backend_delete(struct q_backend *b,int i){char path[Q_PATH_CAP],line[160];if(ensure_view(b)||join(path,sizeof path,b->mountpoint,"kprobe_events"))return -1;int n=snprintf(line,sizeof line,"-:%s/%s\n",b->group,q_events[i]);return q_write_all_once(path,line,(size_t)n);}
int q_backend_finish(struct q_backend *b){
 int ok=1;
 if(b->prepared){
  char live[128];
  if(module_file_pin()||read_small("/sys/module/nt36532/initstate",live,sizeof live)||strcmp(live,"live\n"))ok=0;
  if(read_small("/sys/class/drm/card0-DSI-1/enabled",live,sizeof live)||strcmp(live,"disabled\n"))ok=0;
 }
 if(b->mounted&&instances_empty(b))ok=0;
 if(b->mounted&&umount2(b->mountpoint,0)&&errno!=EINVAL&&errno!=ENOENT)ok=0;
 if(b->mounted){struct statfs st;if(statfs(b->mountpoint,&st)||st.f_type==TRACEFS_MAGIC)ok=0;}
 if(ok)b->mounted=0;
 return ok?0:-1;/* No lazy/forced unmount, claim deletion or global reset. */
}
