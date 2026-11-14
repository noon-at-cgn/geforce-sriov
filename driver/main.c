/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "ada.h"
#include "display.h"
#include "modcheck.h"
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
static void display_report(const struct gs_display_info *d)
{
 printf("admission=%s\nstate=%s\nrevision=display-idx39\nrecipe=%s\ninput_sha256=%s\noutput_sha256=%s\nfunction=_nv045867rm\nfunction_offset=0x%zx\nfile_offset=0x%zx\nrelocation_section=%s\nrelocation_index=%zu\nrelocation_offset=0x%llx\nrelocation_symbol=_nv036611rm\nrelocation_original_type=R_X86_64_PC32\nrelocation_new_type=R_X86_64_NONE\nrelocation_addend=-4\n",
 d->digest?"digest":"anchor",d->confirmed?"confirmed":"unconfirmed:unknown",d->recipe?d->recipe->name:"idx39-context",d->input_sha,d->output_sha,d->function_offset,d->file_offset,d->rela_name,d->rela_index,(unsigned long long)d->rela_offset);
}
static void ada_report(const struct gs_ada_found *f,const char *input,const char *output)
{
 printf("admission=%s\nstate=%s\nrevision=%s\nrecipe=%s\ninput_sha256=%s\noutput_sha256=%s\nfile_offset=0x%zx\n",
 f->admission==GS_ADA_ANCHOR?"anchor":"digest",
 f->admission==GS_ADA_DIGEST&&f->row->measured?"confirmed":"unconfirmed:unknown",
 f->row->name,f->row->version,input,output,f->site);
 if(f->divisor)
  printf("divisor=%u\n",f->divisor);
 if(f->function)
  printf("function=%s\nfunction_offset=0x%zx\n",f->function,f->function_offset);
}
static void usage(FILE *f){fputs("Usage: geforce-sriov-patch inspect INPUT\n       geforce-sriov-patch apply INPUT NEWOUTPUT\n       geforce-sriov-patch revert PATCHED STOCK NEWOUTPUT\n       geforce-sriov-patch ada list|inspect INPUT|apply INPUT NEWOUTPUT|revert PATCHED NEWOUTPUT\n       geforce-sriov-patch display list|inspect INPUT|apply INPUT NEWOUTPUT|revert PATCHED NEWOUTPUT\n       geforce-sriov-patch abi-check --core CORE.ko --module MOD.ko [--module MOD2.ko ...] [--kernel-symvers Module.symvers] [--vermagic STRING]\n",f);}
int main(int argc,char **argv)
{
 unsigned char *p=NULL,*stock=NULL;size_t n=0,sn=0,i;struct gs_patch_info info;struct gs_display_info display;struct gs_ada_found found;const struct gs_ada_revision *r;char err[512]={0},input_hash[65],output_hash[65];int rc=1,w,created=0;
 if(argc==2&&(!strcmp(argv[1],"--help")||!strcmp(argv[1],"-h"))){usage(stdout);rc=0;goto end;}
 if(argc==2&&!strcmp(argv[1],"--version")){puts("geforce-sriov-patch 0.4.1");rc=0;goto end;}
 if(argc>=2&&!strcmp(argv[1],"abi-check"))return gs_modcheck(argc-2,argv+2);
 if(argc>=2&&!strcmp(argv[1],"display")){
  const struct gs_display_recipe *rows;size_t count;
  if(!((argc==3&&!strcmp(argv[2],"list"))||(argc==4&&!strcmp(argv[2],"inspect"))||(argc==5&&(!strcmp(argv[2],"apply")||!strcmp(argv[2],"revert"))))){usage(stderr);return 2;}
  if(!strcmp(argv[2],"list")){rows=gs_display_recipes(&count);for(i=0;i<count;i++)printf("recipe=%s input_sha256=%s output_sha256=%s\n",rows[i].name,rows[i].input_sha,rows[i].output_sha?rows[i].output_sha:"to-be-pinned");rc=0;goto end;}
  if(load(argv[3],&p,&n,err,sizeof(err)))goto end;
  if(gs_display_inspect(p,n,&display,err,sizeof(err))){rc=3;goto end;}
  if(!strcmp(argv[2],"inspect")){display_report(&display);rc=0;goto end;}
  if(gs_display_transform(p,n,&display,!strcmp(argv[2],"revert"),err,sizeof(err))){rc=3;goto end;}
  w=gs_write_new_output(argv[4],p,n,err,sizeof(err));if(w<0)goto end;
  created=1;if(w==GS_OUTPUT_DIR_UNSYNCED)fprintf(stderr,"warning: %s\n",err);
  display_report(&display);rc=0;goto end;
 }
 if(argc>=2&&!strcmp(argv[1],"ada")){
  if(!((argc==3&&!strcmp(argv[2],"list"))||(argc==4&&!strcmp(argv[2],"inspect"))||(argc==5&&(!strcmp(argv[2],"apply")||!strcmp(argv[2],"revert"))))){usage(stderr);return 2;}
  if(!strcmp(argv[2],"list")){
   for(i=0;i<gs_ada_revision_count();i++){unsigned j,wd;r=gs_ada_revision_at(i);wd=r->width<sizeof(r->old_bytes)?r->width:(unsigned)sizeof(r->old_bytes);printf("%s: %s; file offset 0x%zx; width %u;",r->name,r->version,r->site,(unsigned)r->width);for(j=0;j<wd;j++)printf(" %02x",r->old_bytes[j]);fputs(" ->",stdout);for(j=0;j<wd;j++)printf(" %02x",r->new_bytes[j]);printf("; input %s; output %s\n",r->input_sha,r->output_sha);}
   rc=0;goto end;
  }
  if(load(argv[3],&p,&n,err,sizeof(err)))goto end;
  if(gs_ada_admit(p,n,!strcmp(argv[2],"apply")?GS_ADA_FORWARD:!strcmp(argv[2],"revert")?GS_ADA_BACKWARD:GS_ADA_ANY,&found,err,sizeof(err))){rc=3;goto end;}
  printf("row %zu: %s: %s; %s; file offset 0x%zx; %s\n",(size_t)(found.row-gs_ada_revision_at(0))+1,found.row->name,found.row->version,found.row->artifact,found.site,found.state==0?"file at input state":"file at output state");
  gs_sha256(p,n,SIZE_MAX,input_hash);memcpy(output_hash,input_hash,sizeof(output_hash));
  if(!strcmp(argv[2],"inspect")){ada_report(&found,input_hash,output_hash);rc=0;goto end;}
  if(gs_ada_edit(&found,p,n,!strcmp(argv[2],"revert"),err,sizeof(err))){rc=3;goto end;}
  gs_sha256(p,n,SIZE_MAX,output_hash);
  w=gs_write_new_output(argv[4],p,n,err,sizeof(err));
  if(w<0)goto end;
  ada_report(&found,input_hash,output_hash);
  goto written;
 }
 if(!((argc==3&&!strcmp(argv[1],"inspect"))||(argc==4&&!strcmp(argv[1],"apply"))||(argc==5&&!strcmp(argv[1],"revert")))){usage(stderr);return 2;}
 if(load(argv[2],&p,&n,err,sizeof(err)))goto end;
 if(gs_patch_inspect(p,n,&info,err,sizeof(err))){rc=3;goto end;}
 gs_sha256(p,n,SIZE_MAX,input_hash);memcpy(output_hash,input_hash,sizeof(output_hash));
 if(!strcmp(argv[1],"apply")){
  if(info.patched){snprintf(err,sizeof(err),"anchor scheduler.state: already patched");rc=3;goto end;}
  p[info.offset]=0x90;p[info.offset+1]=0x90;gs_sha256(p,n,SIZE_MAX,output_hash);
 }else if(!strcmp(argv[1],"revert")){
  if(load(argv[3],&stock,&sn,err,sizeof(err)))goto end;
  if(gs_patch_revert_check(p,n,stock,sn,err,sizeof(err))){rc=3;goto end;}
  gs_sha256(stock,sn,SIZE_MAX,output_hash);
 }
 printf("admission=%s\nstate=%s\nrevision=scheduler\nrecipe=%s\ninput_sha256=%s\noutput_sha256=%s\nfunction=%s\nfunction_offset=0x%zx\nfile_offset=0x%zx\n",info.digest?"digest":"anchor",info.digest&&info.measured?"confirmed":"unconfirmed:unknown",info.version,input_hash,output_hash,info.function,info.function_offset,info.offset);
 if(!strcmp(argv[1],"inspect")){rc=0;goto end;}
 w=!strcmp(argv[1],"apply")?gs_write_new_output(argv[3],p,n,err,sizeof(err)):gs_write_new_output(argv[4],stock,sn,err,sizeof(err));
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
