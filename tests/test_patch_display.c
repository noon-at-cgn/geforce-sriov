/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "display.h"
#include "patch.h"
#include "safe_output.h"
#include <elf.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
static unsigned char b[4096], original[4096];
static int checks,failures;
#define CHECK(x) do { checks++; if(!(x)){ failures++; fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); } } while(0)
static const unsigned char context[]={0x41,0x89,0xc5,0x85,0xc0,0x0f,0x85,0xc1,0,0,0,0xe8,0,0,0,0,0x84,0xc0,0x74,0x18};
static void fixture(size_t text,size_t call)
{
 Elf64_Ehdr eh={0};Elf64_Shdr sh[7]={{0}};Elf64_Sym sy[3]={{0}};Elf64_Rela re={0};
 const char names[]="\0.text\0.shstrtab\0.strtab\0.symtab\0.rela.text\0.rela.other\0";
 const char symbols[]="\0_nv045867rm\0_nv036611rm\0";
 memset(b,0,sizeof(b));memcpy(eh.e_ident,ELFMAG,SELFMAG);eh.e_ident[EI_CLASS]=ELFCLASS64;eh.e_ident[EI_DATA]=ELFDATA2LSB;eh.e_ident[EI_VERSION]=EV_CURRENT;eh.e_type=ET_REL;eh.e_machine=EM_X86_64;eh.e_version=EV_CURRENT;eh.e_ehsize=sizeof(eh);eh.e_shoff=64;eh.e_shentsize=sizeof(sh[0]);eh.e_shnum=7;eh.e_shstrndx=2;
 sh[1].sh_name=1;sh[1].sh_type=SHT_PROGBITS;sh[1].sh_flags=SHF_ALLOC|SHF_EXECINSTR;sh[1].sh_offset=text;sh[1].sh_size=512;sh[1].sh_addralign=16;
 sh[2].sh_name=7;sh[2].sh_type=SHT_STRTAB;sh[2].sh_offset=1600;sh[2].sh_size=sizeof(names);sh[2].sh_addralign=1;
 sh[3].sh_name=17;sh[3].sh_type=SHT_STRTAB;sh[3].sh_offset=1700;sh[3].sh_size=sizeof(symbols);sh[3].sh_addralign=1;
 sh[4].sh_name=25;sh[4].sh_type=SHT_SYMTAB;sh[4].sh_offset=1800;sh[4].sh_size=sizeof(sy);sh[4].sh_entsize=sizeof(sy[0]);sh[4].sh_link=3;sh[4].sh_info=1;sh[4].sh_addralign=8;
 sh[5].sh_name=33;sh[5].sh_type=SHT_RELA;sh[5].sh_offset=2000;sh[5].sh_size=sizeof(re);sh[5].sh_entsize=sizeof(re);sh[5].sh_link=4;sh[5].sh_info=1;sh[5].sh_addralign=8;
 sh[6]=sh[5];sh[6].sh_name=44;sh[6].sh_offset=2100;sh[6].sh_size=0;
 sy[1].st_name=1;sy[1].st_shndx=1;sy[1].st_info=ELF64_ST_INFO(STB_GLOBAL,STT_FUNC);sy[1].st_size=256;
 sy[2].st_name=13;sy[2].st_shndx=1;sy[2].st_info=ELF64_ST_INFO(STB_GLOBAL,STT_FUNC);sy[2].st_value=300;sy[2].st_size=9;
 re.r_offset=call+1;re.r_info=ELF64_R_INFO(2,R_X86_64_PC32);re.r_addend=-4;
 memcpy(b,&eh,sizeof(eh));memcpy(b+64,sh,sizeof(sh));memcpy(b+text+call-11,context,sizeof(context));memcpy(b+1600,names,sizeof(names));memcpy(b+1700,symbols,sizeof(symbols));memcpy(b+1800,sy,sizeof(sy));memcpy(b+2000,&re,sizeof(re));
}
static void relocation(Elf64_Rela r){memcpy(b+2000,&r,sizeof(r));}
int main(void)
{
 struct gs_display_info info;struct gs_display_recipe row;Elf64_Rela r;Elf64_Shdr s;Elf64_Sym sym;char err[256],hash[65],output[65],dir[]="/tmp/gs-display-XXXXXX",path[256];
 fixture(512,79);memcpy(original,b,sizeof(b));
 CHECK(!gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))&&!info.digest&&info.function_offset==79);
 CHECK(!gs_display_transform(b,sizeof(b),&info,0,err,sizeof(err)));memcpy(output,info.output_sha,65);
 { size_t differences=0;for(size_t j=0;j<sizeof(b);j++)if(b[j]!=original[j])differences++;CHECK(differences==6); }
 CHECK(!gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))&&info.patched&&!info.digest);
 CHECK(gs_display_transform(b,sizeof(b),&info,0,err,sizeof(err))<0);
 CHECK(!gs_display_transform(b,sizeof(b),&info,1,err,sizeof(err))&&!memcmp(b,original,sizeof(b)));
 gs_sha256(b,sizeof(b),SIZE_MAX,hash);row=(struct gs_display_recipe){"synthetic",hash,output,79};
 CHECK(!gs_display_inspect_in(b,sizeof(b),&row,1,&info,err,sizeof(err))&&info.digest&&info.confirmed);
 CHECK(!gs_display_transform(b,sizeof(b),&info,0,err,sizeof(err)));
 CHECK(!gs_display_inspect_in(b,sizeof(b),&row,1,&info,err,sizeof(err))&&info.digest&&info.confirmed&&info.patched);
 CHECK(!gs_display_transform(b,sizeof(b),&info,1,err,sizeof(err))&&!memcmp(b,original,sizeof(b)));
 row.output_sha="0000000000000000000000000000000000000000000000000000000000000000";
 CHECK(!gs_display_inspect_in(b,sizeof(b),&row,1,&info,err,sizeof(err)));
 CHECK(gs_display_transform(b,sizeof(b),&info,0,err,sizeof(err))<0&&!memcmp(b,original,sizeof(b)));
 b[1500]^=1;CHECK(!gs_display_inspect_in(b,sizeof(b),&row,1,&info,err,sizeof(err))&&!info.digest&&!info.confirmed);b[1500]^=1;
 { const struct gs_display_recipe *rows;size_t count;
  rows=gs_display_recipes(&count);
  CHECK(count==2);
  CHECK(rows[0].output_sha!=NULL);
  CHECK(!strcmp(rows[0].input_sha,"946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29"));
  CHECK(!strcmp(rows[0].output_sha,"83ce12abba0441812aa135d4f099636513d709090c3da8999cda9243bd3b67ff"));
  CHECK(rows[0].function_offset==0x4f);
  /* The display revision after the ada-placement row (an Ada card selected). */
  CHECK(!strcmp(rows[1].input_sha,"20eaebcac4f5220d21f64bcd5145143abe8f806a25cb9e249088a1b98df7d157"));
  CHECK(!strcmp(rows[1].output_sha,"48fd6ddd2df13a1131a98facd4a80be1d6d17877eb900f3c945dfa483347627b"));
  CHECK(rows[1].function_offset==rows[0].function_offset);
 }
 fixture(1024,100);CHECK(!gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))&&info.file_offset==1124&&info.function_offset==100);
 fixture(512,79);b[512+79]=0xff;CHECK(gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))<0);
 fixture(512,79);b[512+79-1]^=1;CHECK(gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))<0);
 fixture(512,79);memcpy(b+512+130,context,sizeof(context));CHECK(gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))<0);
 fixture(512,79);memcpy(&s,b+64+6*sizeof(s),sizeof(s));s.sh_size=sizeof(r);memcpy(b+64+6*sizeof(s),&s,sizeof(s));memcpy(&r,b+2000,sizeof(r));r.r_offset=77;memcpy(b+2100,&r,sizeof(r));CHECK(gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))<0);
 fixture(512,79);memcpy(&s,b+64+4*sizeof(s),sizeof(s));s.sh_size+=sizeof(sym);memcpy(b+64+4*sizeof(s),&s,sizeof(s));memcpy(b+1800+3*sizeof(sym),b+1800+sizeof(sym),sizeof(sym));CHECK(gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))<0);
 fixture(512,79);memcpy(&sym,b+1800+sizeof(sym),sizeof(sym));sym.st_shndx=SHN_UNDEF;memcpy(b+1800+sizeof(sym),&sym,sizeof(sym));CHECK(gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))<0);
 fixture(512,79);memcpy(&s,b+64+5*sizeof(s),sizeof(s));s.sh_size=0;memcpy(b+64+5*sizeof(s),&s,sizeof(s));CHECK(gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))<0);
 fixture(512,79);memcpy(&r,b+2000,sizeof(r));r.r_info=ELF64_R_INFO(1,R_X86_64_PC32);relocation(r);CHECK(gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))<0);
 fixture(512,79);memcpy(&r,b+2000,sizeof(r));r.r_info=ELF64_R_INFO(2,R_X86_64_32);relocation(r);CHECK(gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))<0);
 fixture(512,79);memcpy(&r,b+2000,sizeof(r));r.r_addend=0;relocation(r);CHECK(gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))<0);
 fixture(512,79);memcpy(&r,b+2000,sizeof(r));r.r_offset++;relocation(r);CHECK(gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))<0);
 fixture(512,79);memcpy(&s,b+64+6*sizeof(s),sizeof(s));s.sh_size=sizeof(r);memcpy(b+64+6*sizeof(s),&s,sizeof(s));memcpy(b+2100,b+2000,sizeof(r));CHECK(gs_display_inspect(b,sizeof(b),&info,err,sizeof(err))<0);
 fixture(512,79);CHECK(gs_display_inspect(b,2001,&info,err,sizeof(err))<0);
 CHECK(mkdtemp(dir)!=NULL);snprintf(path,sizeof(path),"%s/out",dir);CHECK(gs_write_new_output(path,b,sizeof(b),err,sizeof(err))>=0);CHECK(gs_write_new_output(path,b,sizeof(b),err,sizeof(err))<0);CHECK(!unlink(path));CHECK(!symlink("missing",path));CHECK(gs_write_new_output(path,b,sizeof(b),err,sizeof(err))<0);CHECK(!unlink(path));CHECK(!rmdir(dir));
 printf("test_patch_display: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
