/* SPDX-License-Identifier: MIT */
#include "display.h"
#include "elf_reader.h"
#include "patch.h"
#include <stdio.h>
#include <string.h>

/* Measured pair for the 580.159.01 host object with the scheduler, probe-gate and
 * VF-count revisions already applied (112,617,640 bytes). The idx 39 site is
 * .text+0x49beef, which is file offset 0x49bf2f in this object; the relocation is
 * .rela.text entry 118744 at file 0x63a5c98. Keep the two coordinate spaces apart. */
static const struct gs_display_recipe recipes[] = {
 { "idx39-580.159.01", "946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29",
   "83ce12abba0441812aa135d4f099636513d709090c3da8999cda9243bd3b67ff", 0x4f },
 { "idx39-ada-placement-580.159.01", "20eaebcac4f5220d21f64bcd5145143abe8f806a25cb9e249088a1b98df7d157",
   "48fd6ddd2df13a1131a98facd4a80be1d6d17877eb900f3c945dfa483347627b", 0x4f }
};
static const unsigned char before[] = {0xe8,0,0,0,0};
static const unsigned char after[] = {0x31,0xc0,0x90,0x90,0x90};
static const unsigned char prefix[] = {0x41,0x89,0xc5,0x85,0xc0,0x0f,0x85,0xc1,0,0,0};
static const unsigned char suffix[] = {0x84,0xc0,0x74,0x18};
static int fail(char *err,size_t n,const char *msg) { if(err&&n)snprintf(err,n,"anchor display.%s",msg);return -1; }
const struct gs_display_recipe *gs_display_recipes(size_t *count) { if(count)*count=sizeof(recipes)/sizeof(recipes[0]);return recipes; }

/* Kept separate so a future shape-based resolver does not affect the CLI. */
static int resolve(const struct gs_elf *e,const char *name,struct gs_elf_sym *sym,struct gs_elf_range *range)
{
 Elf64_Shdr sh;size_t i,count=0;struct gs_elf_sym s;
 for(i=1;i<e->nsyms;i++){
  if(gs_elf_symbol(e,i,&s,NULL,0))return -1;
  if(s.shndx && !strcmp(s.name,name)){*sym=s;count++;}
 }
 if(count!=1||sym->type!=STT_FUNC||gs_elf_symbol_range(e,sym,range,NULL,0)||
    gs_elf_section(e,sym->shndx,&sh,NULL,NULL,0)||sh.sh_type!=SHT_PROGBITS||!(sh.sh_flags&SHF_EXECINSTR))return -1;
 return 0;
}
/* Relocations unknown to this reader are conservatively treated as 16-byte writes. */
static unsigned width(uint32_t type)
{
 switch(type){
 case R_X86_64_NONE:return 0;
 case R_X86_64_8:case R_X86_64_PC8:return 1;
 case R_X86_64_16:case R_X86_64_PC16:return 2;
 case R_X86_64_PC32:case R_X86_64_32:case R_X86_64_32S:case R_X86_64_PLT32:case R_X86_64_GOTPCREL:return 4;
 case R_X86_64_64:case R_X86_64_PC64:return 8;
 default:return 16;
 }
}
int gs_display_inspect_in(const unsigned char *p,size_t n,const struct gs_display_recipe *rows,size_t nr,struct gs_display_info *out,char *err,size_t en)
{
 struct gs_elf e;struct gs_elf_sym caller,stub;struct gs_elf_range cr,sr;struct gs_elf_rela_iter it;struct gs_elf_rela rel,chosen={0};
 Elf64_Shdr sh;const char *section_name;size_t i,found=0,call=0,overlaps=0,matches=0;int patched=0,rc;uint64_t target;char hash[65];const struct gs_display_recipe *recipe=NULL;
 gs_sha256(p,n,SIZE_MAX,hash);
 for(i=0;i<nr;i++)if((rows[i].input_sha&&!strcmp(hash,rows[i].input_sha))||(rows[i].output_sha&&!strcmp(hash,rows[i].output_sha))){recipe=&rows[i];matches++;}
 if(matches>1)return fail(err,en,"digest: ambiguous recipe");
 if(gs_elf_open(p,n,&e,NULL,0))return fail(err,en,"elf: invalid or truncated ELF");
 if(resolve(&e,"_nv045867rm",&caller,&cr))return fail(err,en,"caller: unique executable function not found");
 if(resolve(&e,"_nv036611rm",&stub,&sr))return fail(err,en,"stub: unique executable function not found");
 for(i=0;i+sizeof(prefix)+sizeof(before)+sizeof(suffix)<=cr.size;i++){
  const unsigned char *q=cr.bytes+i;
  if(!memcmp(q,prefix,sizeof(prefix))&&!memcmp(q+sizeof(prefix)+sizeof(before),suffix,sizeof(suffix))&&
     (!memcmp(q+sizeof(prefix),before,sizeof(before))||!memcmp(q+sizeof(prefix),after,sizeof(after)))){
   call=i+sizeof(prefix);patched=!memcmp(q+sizeof(prefix),after,sizeof(after));found++;
  }
 }
 if(found!=1)return fail(err,en,"context: unique instruction context not found");
 if(recipe && (call!=recipe->function_offset || (recipe->input_sha&&!strcmp(hash,recipe->input_sha)&&patched) || (recipe->output_sha&&!strcmp(hash,recipe->output_sha)&&!patched)))return fail(err,en,"integrity: digest contradicts site or instruction");
 target=caller.value+call;gs_elf_rela_begin(&it,caller.shndx);
 while((rc=gs_elf_rela_next(&e,&it,&rel,NULL,0))>0){
  unsigned w=width(rel.type);
  /* NONE is accepted only as the matching retained relocation for patched input. */
  if(!w && patched && rel.offset==target+1 && rel.sym==stub.index && rel.addend==-4)w=4;
  if(w && rel.offset<target+5 && (rel.offset>=target || target-rel.offset<w)) { chosen=rel;overlaps++; }
 }
 if(rc<0)return fail(err,en,"relocations: invalid relocation table");
 if(overlaps!=1)return fail(err,en,"relocations: unique overlapping relocation not found");
 if(chosen.offset!=target+1||chosen.sym!=stub.index||chosen.addend!=-4||chosen.type!=(patched?R_X86_64_NONE:R_X86_64_PC32))return fail(err,en,"relocation: expected target, type, symbol and addend not found");
 if(gs_elf_section(&e,chosen.section,&sh,&section_name,NULL,0))return fail(err,en,"relocation: section not found");
 memset(out,0,sizeof(*out));out->recipe=recipe;out->digest=recipe!=NULL;out->confirmed=recipe&&recipe->input_sha&&recipe->output_sha;out->patched=patched;out->file_offset=cr.file_offset+call;out->function_offset=call;
 out->rela_section=chosen.section;out->rela_index=chosen.index;out->rela_offset=chosen.offset;out->rela_file_offset=(size_t)sh.sh_offset+chosen.index*sizeof(Elf64_Rela);
 snprintf(out->rela_name,sizeof(out->rela_name),"%s",section_name);memcpy(out->input_sha,hash,65);memcpy(out->output_sha,hash,65);return 0;
}
int gs_display_inspect(const unsigned char *p,size_t n,struct gs_display_info *out,char *err,size_t en)
{ return gs_display_inspect_in(p,n,recipes,sizeof(recipes)/sizeof(recipes[0]),out,err,en); }
int gs_display_transform(unsigned char *p,size_t n,struct gs_display_info *info,int revert,char *err,size_t en)
{
 Elf64_Rela r,old;const char *expected;char hash[65];
 if(info->patched!=!!revert)return fail(err,en,revert?"state: input is not patched":"state: input already patched");
 if(info->file_offset>n||n-info->file_offset<5||info->rela_file_offset>n||n-info->rela_file_offset<sizeof(r))return fail(err,en,"bounds: edit outside input");
 memcpy(&r,p+info->rela_file_offset,sizeof(r));old=r;
 memcpy(p+info->file_offset,revert?before:after,5);
 r.r_info=ELF64_R_INFO(ELF64_R_SYM(r.r_info),revert?R_X86_64_PC32:R_X86_64_NONE);
 memcpy(p+info->rela_file_offset,&r,sizeof(r));gs_sha256(p,n,SIZE_MAX,hash);
 expected=info->recipe?(revert?info->recipe->input_sha:info->recipe->output_sha):NULL;
 if(expected&&strcmp(expected,hash)){
  memcpy(p+info->file_offset,revert?after:before,5);memcpy(p+info->rela_file_offset,&old,sizeof(old));return fail(err,en,"output_sha256: expected digest not found");
 }
 memcpy(info->output_sha,hash,65);return 0;
}
