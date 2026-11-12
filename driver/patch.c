/* SPDX-License-Identifier: MIT */
/* Identifies admitted NVIDIA host objects and replaces the guarded scheduler
 * callback branch (two bytes). */
#include "patch.h"
#include "elf_reader.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
struct recipe { const char *version,*callback,*writer,*installer,*hash; uint32_t flag,slot; int measured; };
static const struct recipe recipes[]={
 {"580.159.01","_nv052453rm","_nv028269rm","_nv005970rm","4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449",0x54d0,0x2d8,1},
 {"595.91.04","_nv048804rm","_nv026725rm","_nv005743rm","318f8289cd987fcc057228cbd2fa68bb477080856d4f31009e37720f12684e11",0x53d8,0x290,0}
};
static int fail(char *e,size_t n,const char *s) { if(e&&n) snprintf(e,n,"%s",s);return -1; }
static uint32_t u32(const unsigned char *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static size_t occurrences(const unsigned char *p,size_t n,const unsigned char *s,size_t z) { size_t i,c=0; if(z>n)return 0;for(i=0;i<=n-z;i++)if(!memcmp(p+i,s,z))c++;return c; }
static int function(const struct gs_elf *e,const char *name,struct gs_elf_sym *s,struct gs_elf_range *r,char *err,size_t n) {
 if(gs_elf_symbol_by_name(e,name,s,err,n)||s->type!=STT_FUNC||gs_elf_symbol_range(e,s,r,err,n))
  return fail(err,n,"required unique function missing or invalid");
 return 0;
}
static int relocation(const struct gs_elf *e,size_t section,uint64_t at,struct gs_elf_rela *out,char *err,size_t n) {
 struct gs_elf_rela_iter it;struct gs_elf_rela x;int rc,count=0;gs_elf_rela_begin(&it,section);
 while((rc=gs_elf_rela_next(e,&it,&x,err,n))>0)if(x.offset==at){*out=x;count++;}
 if(rc<0)return -1;
 return count==1?0:fail(err,n,"missing or ambiguous instruction relocation");
}
int gs_patch_structure(const unsigned char *p,size_t n,const char *version,struct gs_patch_info *out,char *err,size_t en)
{
 struct gs_elf e;struct gs_elf_sym cb,w,inst,get;struct gs_elf_range cr,wr,ir,gr;struct gs_elf_rela rel;struct gs_elf_rela_iter it;
 Elf64_Shdr sh;size_t text,i,sig=0,count=0,anchor=0,ac=0,stores=0;int rc;const struct recipe *r=NULL;
 static const unsigned char prefix[]={0x66,0x83,0x3a,0x0e,0x0f,0x85};
 static const unsigned char args[]={0x41,0xb8,1,0,0,0,0xb9,1,0,0,0,0x4c,0x89,0xef,0x48,0x89,0xc6,0xba,0,0xdf,0x34,0xd3,0x48,0x8b,0x80};
 static const unsigned char lea[]={0x8d,0x88,0x94,0,0,0}, mov[]={0x41,0xb8,0x0b,0,0,0};
 for(i=0;i<sizeof(recipes)/sizeof(recipes[0]);i++)if(!strcmp(version,recipes[i].version))r=&recipes[i];
 if(!r)return fail(err,en,"no patch recipe for this driver version");
 if(gs_elf_open(p,n,&e,err,en)||gs_elf_section_by_name(&e,".text",&text,&sh,err,en))return -1;
 if(!(sh.sh_flags&SHF_EXECINSTR)||sh.sh_type!=SHT_PROGBITS)return fail(err,en,".text is not executable file-backed code");
 if(function(&e,r->callback,&cb,&cr,err,en)||function(&e,r->writer,&w,&wr,err,en)||function(&e,r->installer,&inst,&ir,err,en))return -1;
 if(cr.shndx!=text||wr.shndx!=text||ir.shndx!=text||cr.size!=237||wr.size!=373||ir.size<11)return fail(err,en,"function section/size mismatch: another build, or a GSP object");
 /* Scan the entire text, including already-patched signatures, not just the named callback. */
 for(i=0;i+19<=sh.sh_size;i++){const unsigned char *q=p+sh.sh_offset+i;
  if(!memcmp(q,prefix,6)&&q[10]==0xf6&&q[11]==0x87&&q[16]==0x10&&((q[17]==0x75)||(q[17]==0x90&&q[18]==0x90))){sig=i;count++;}}
 if(count!=1)return fail(err,en,"patch site not found or ambiguous (open and GSP objects are not supported)");
 if(sig<cb.value||sig-cb.value>cr.size-19)return fail(err,en,"signature outside expected callback");
 i=sig-cb.value;
 if(u32(cr.bytes+i+12)!=r->flag||u32(cr.bytes+i+6)!=0xaf)return fail(err,en,"flag or conditional control-flow mismatch");
 if(cr.bytes[i+17]==0x75&&cr.bytes[i+18]!=0x76)return fail(err,en,"unsupported JNE displacement");
 for(size_t j=i+19;j+39<=cr.size&&j<=i+0xc0;j++)if(!memcmp(cr.bytes+j,args,sizeof(args))&&u32(cr.bytes+j+25)==r->slot&&cr.bytes[j+29]==0xe8&&cr.bytes[j+34]==0xe9){anchor=j;ac++;}
 if(ac!=1||anchor<5||anchor-5!=i+19+0x76||cr.bytes[anchor-5]!=0xe8)return fail(err,en,"disable block instruction/branch anchors mismatch");
 if((int64_t)(anchor+39)+(int32_t)u32(cr.bytes+anchor+35)!=(int64_t)i+19)return fail(err,en,"disable block does not return to fallthrough");
 if(relocation(&e,text,cb.value+anchor-4,&rel,err,en)||rel.type!=R_X86_64_PC32||rel.addend!=-4||gs_elf_symbol(&e,rel.sym,&get,err,en)||get.type!=STT_FUNC||get.size!=70||gs_elf_symbol_range(&e,&get,&gr,err,en)||gr.shndx!=text)return fail(err,en,"getter relocation/function mismatch");
 if(relocation(&e,text,cb.value+anchor+30,&rel,err,en)||rel.type!=R_X86_64_PC32||rel.addend!=-4||strcmp(rel.sym_name,"__x86_indirect_thunk_rax"))return fail(err,en,"indirect thunk relocation mismatch");
 if(!occurrences(wr.bytes,wr.size,lea,sizeof(lea))||!occurrences(wr.bytes,wr.size,mov,sizeof(mov)))return fail(err,en,"scheduler writer instruction anchors absent");
 gs_elf_rela_begin(&it,text);
 while((rc=gs_elf_rela_next(&e,&it,&rel,err,en))>0){
  if(rel.offset>=inst.value+7&&rel.offset-inst.value<=ir.size-4&&rel.type==R_X86_64_32S&&rel.sym==w.index&&rel.addend==0){size_t j=(size_t)(rel.offset-inst.value)-7;
   if(!memcmp(ir.bytes+j,"\x48\xc7\x87",3)&&u32(ir.bytes+j+3)==r->slot)stores++;}
  if(rel.offset<=sig+18&&rel.offset+8>sig+17)return fail(err,en,"relocation overlaps patch bytes");
 }
 if(rc<0)return -1;
 if(!stores)return fail(err,en,"installer does not store the scheduler writer at the expected slot");
 out->offset=(size_t)sh.sh_offset+sig+17;out->function_offset=sig+17-cb.value;out->patched=p[out->offset]==0x90;out->version=r->version;out->function=r->callback;out->measured=r->measured;out->digest=0;return 0;
}
int gs_patch_inspect(const unsigned char *p,size_t n,struct gs_patch_info *out,char *err,size_t en)
{
 char actual[65],normalized[65],detail[256]="no recipe family matched";size_t i,count=0;struct gs_patch_info info,match;
 gs_sha256(p,n,SIZE_MAX,actual);
 for(i=0;i<sizeof(recipes)/sizeof(recipes[0]);i++){
  if(gs_patch_structure(p,n,recipes[i].version,&info,err,en)){
   if(!strcmp(actual,recipes[i].hash))return fail(err,en,"anchor scheduler.integrity: known digest contradicts structure");
   if(err&&*err)snprintf(detail,sizeof(detail),"%.240s",err);
   continue;
  }
  gs_sha256(p,n,info.offset,normalized);
  info.digest=!strcmp(normalized,recipes[i].hash);
  match=info;count++;
 }
 if(count!=1){if(err&&en)snprintf(err,en,"anchor scheduler.family: %s",count?"ambiguous recipe families":detail);return -1;}
 *out=match;return 0;
}
int gs_patch_exact_pair(const unsigned char *patched,size_t pn,const unsigned char *stock,size_t sn,size_t site,char *err,size_t en)
{
 if(pn!=sn||site>pn||pn-site<2)
  return fail(err,en,"revert size or patch boundary mismatch");
 if(patched[site]!=0x90||patched[site+1]!=0x90||stock[site]!=0x75||stock[site+1]!=0x76)
  return fail(err,en,"revert requires exact stock JNE and patched NOP bytes");
 if(memcmp(patched,stock,site)||memcmp(patched+site+2,stock+site+2,pn-site-2))
  return fail(err,en,"objects differ outside the exact two-byte patch");
 return 0;
}
int gs_patch_revert_check(const unsigned char *patched,size_t pn,const unsigned char *stock,size_t sn,char *err,size_t en)
{
 struct gs_patch_info a,b;if(gs_patch_inspect(patched,pn,&a,err,en)||gs_patch_inspect(stock,sn,&b,err,en))return -1;
 if(!a.patched||b.patched||pn!=sn||a.offset!=b.offset)return fail(err,en,"revert requires matching patched and stock objects");
 return gs_patch_exact_pair(patched,pn,stock,sn,a.offset,err,en);
}
