/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "ada.h"
#include "patch.h"
#include "safe_output.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int load(const char *path,unsigned char **data,size_t *size,char *err,size_t n)
{
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK);struct stat st;size_t done=0;unsigned char *p;
 if(fd<0){snprintf(err,n,"%s: %s",path,strerror(errno));return -1;}
 if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||(uintmax_t)st.st_size>SIZE_MAX){snprintf(err,n,"%s: not a nonempty regular input file",path);close(fd);return -1;}
 *size=(size_t)st.st_size;p=malloc(*size);if(!p){snprintf(err,n,"input allocation failed");close(fd);return -1;}
 while(done<*size){ssize_t got=read(fd,p+done,*size-done);if(got<0&&errno==EINTR)continue;if(got<=0){snprintf(err,n,"%s: incomplete read",path);free(p);close(fd);return -1;}done+=(size_t)got;}
 {unsigned char extra;ssize_t got;do {got=read(fd,&extra,1);}while(got<0&&errno==EINTR);if(got!=0){snprintf(err,n,"input changed size during read");free(p);close(fd);return -1;}}
 if(close(fd)){snprintf(err,n,"input close failed");free(p);return -1;}*data=p;return 0;
}
static void usage(FILE *f){fputs("Usage: geforce-sriov-patch inspect INPUT\n       geforce-sriov-patch apply INPUT NEWOUTPUT\n       geforce-sriov-patch revert PATCHED STOCK NEWOUTPUT\n       geforce-sriov-patch ada list\n       geforce-sriov-patch ada inspect INPUT\n       geforce-sriov-patch ada apply INPUT NEWOUTPUT\n       geforce-sriov-patch ada revert PATCHED NEWOUTPUT\nOffline edit of a copy of NVIDIA's host nv-kernel.o_binary; no module build, install or load.\nAccepted: 580.159.01 (supported), 595.91.04 (not supported). Other builds and open, GSP or guest objects are refused.\nada subcommands edit a copy of a file you supply and write only NEWOUTPUT; never the installed file.\nada scope: 580.159.01 host kernel object and vGPU plugin only; other builds are refused by SHA-256 digest.\nDoes not change GSP firmware.\n",f);}
int main(int argc,char **argv)
{
 unsigned char *p=NULL,*stock=NULL;size_t n=0,sn=0,i;struct gs_patch_info info;const struct gs_ada_revision *r;char err[512]={0};int rc=1,w,created=0,st;
 if(argc==2&&(!strcmp(argv[1],"--help")||!strcmp(argv[1],"-h"))){usage(stdout);rc=0;goto end;}
 if(argc==2&&!strcmp(argv[1],"--version")){puts("geforce-sriov-patch 0.2.2");rc=0;goto end;}
 if(argc>=2&&!strcmp(argv[1],"ada")){
  if(!((argc==3&&!strcmp(argv[2],"list"))||(argc==4&&!strcmp(argv[2],"inspect"))||(argc==5&&(!strcmp(argv[2],"apply")||!strcmp(argv[2],"revert"))))){usage(stderr);return 2;}
  if(!strcmp(argv[2],"list")){
   for(i=0;i<gs_ada_revision_count();i++){unsigned j,wd;r=gs_ada_revision_at(i);wd=r->width<sizeof(r->old_bytes)?r->width:(unsigned)sizeof(r->old_bytes);printf("%s: %s; file offset 0x%zx; width %u;",r->name,r->version,r->site,(unsigned)r->width);for(j=0;j<wd;j++)printf(" %02x",r->old_bytes[j]);fputs(" ->",stdout);for(j=0;j<wd;j++)printf(" %02x",r->new_bytes[j]);printf("; input %s; output %s\n",r->input_sha,r->output_sha);}
   rc=0;goto end;
  }
  if(load(argv[3],&p,&n,err,sizeof(err))||gs_ada_find(p,n,!strcmp(argv[2],"apply")?GS_ADA_FORWARD:!strcmp(argv[2],"revert")?GS_ADA_BACKWARD:GS_ADA_ANY,&r,&st,err,sizeof(err)))goto end;
  printf("row %zu: %s: %s; %s; file offset 0x%zx; %s\n",(size_t)(r-gs_ada_revision_at(0))+1,r->name,r->version,r->artifact,r->site,st==0?"file at input state":"file at output state");
  if(!strcmp(argv[2],"inspect")){rc=0;goto end;}
  if(!strcmp(argv[2],"apply")?gs_ada_apply(r,p,n,err,sizeof(err)):gs_ada_revert(r,p,n,err,sizeof(err)))goto end;
  w=gs_write_new_output(argv[4],p,n,err,sizeof(err));
  goto written;
 }
 if(!((argc==3&&!strcmp(argv[1],"inspect"))||(argc==4&&!strcmp(argv[1],"apply"))||(argc==5&&!strcmp(argv[1],"revert")))){usage(stderr);return 2;}
 if(load(argv[2],&p,&n,err,sizeof(err))||gs_patch_inspect(p,n,&info,err,sizeof(err)))goto end;
 printf("%s: %s; %s; function %s; file offset 0x%zx\n",info.version,info.patched?"already-patched":"stock",info.measured?"stall workaround":"not supported",info.function,info.offset);
 if(!strcmp(argv[1],"inspect")){rc=0;goto end;}
 if(!strcmp(argv[1],"apply")){
  if(info.patched){snprintf(err,sizeof(err),"already patched; refusing to repatch or create output");goto end;}
  p[info.offset]=0x90;p[info.offset+1]=0x90;
  w=gs_write_new_output(argv[3],p,n,err,sizeof(err));
 }else{
  if(load(argv[3],&stock,&sn,err,sizeof(err))||gs_patch_revert_check(p,n,stock,sn,err,sizeof(err)))goto end;
  w=gs_write_new_output(argv[4],stock,sn,err,sizeof(err));
 }
written:
 if(w<0)goto end;
 created=1;
 if(w==GS_OUTPUT_DIR_UNSYNCED)fprintf(stderr,"warning: %s\n",err);
 puts(strcmp(argv[1],"ada")?"Created new output only. Rebuild NVIDIA module separately; no installation or loading performed.":"Created new output only; no installation or loading performed.");rc=0;
end:
 if(!rc&&(fflush(stdout)||ferror(stdout))){
  snprintf(err,sizeof(err),"stdout write failed%s",created?" (output file was already created)":"");
  rc=1;
 }
 if(rc)fprintf(stderr,"geforce-sriov-patch: %s\n",err);
 free(p);free(stock);return rc;
}
