/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "patch.h"
#include "safe_output.h"
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Synthetic executable bytes model the documented structure, not NVIDIA data. */
static unsigned char buf[4096];
static int failures, checks;
/* Always evaluated, unlike assert(), so the checks also run under -DNDEBUG. */
#define CHECK(cond, ...) do { \
		checks++; \
		if (!(cond)) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fputc('\n', stderr); \
		} \
	} while (0)
static void put32(unsigned char *p,uint32_t x){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(x>>(i*8));}
static void fixture(void)
{
 Elf64_Ehdr eh={0};Elf64_Shdr sh[6]={{0}};Elf64_Sym sy[6]={{0}};Elf64_Rela re[3]={{0}};
 const char names[]="\0.text\0.shstrtab\0.strtab\0.symtab\0.rela.text\0";
 const char symbols[]="\0_nv052453rm\0_nv028269rm\0_nv005970rm\0getter\0__x86_indirect_thunk_rax\0";
 unsigned char *t=buf+512;size_t sig=23,a=165;
 memset(buf,0,sizeof(buf));memcpy(eh.e_ident,ELFMAG,SELFMAG);eh.e_ident[EI_CLASS]=ELFCLASS64;eh.e_ident[EI_DATA]=ELFDATA2LSB;eh.e_ident[EI_VERSION]=EV_CURRENT;eh.e_type=ET_REL;eh.e_machine=EM_X86_64;eh.e_version=EV_CURRENT;eh.e_ehsize=sizeof(eh);eh.e_shoff=64;eh.e_shentsize=sizeof(sh[0]);eh.e_shnum=6;eh.e_shstrndx=2;
 sh[1].sh_name=1;sh[1].sh_type=SHT_PROGBITS;sh[1].sh_flags=SHF_ALLOC|SHF_EXECINSTR;sh[1].sh_offset=512;sh[1].sh_size=1024;sh[1].sh_addralign=16;
 sh[2].sh_name=7;sh[2].sh_type=SHT_STRTAB;sh[2].sh_offset=1600;sh[2].sh_size=sizeof(names);sh[2].sh_addralign=1;
 sh[3].sh_name=17;sh[3].sh_type=SHT_STRTAB;sh[3].sh_offset=1700;sh[3].sh_size=sizeof(symbols);sh[3].sh_addralign=1;
 sh[4].sh_name=25;sh[4].sh_type=SHT_SYMTAB;sh[4].sh_offset=1800;sh[4].sh_size=sizeof(sy);sh[4].sh_entsize=sizeof(sy[0]);sh[4].sh_link=3;sh[4].sh_info=1;sh[4].sh_addralign=8;
 sh[5].sh_name=33;sh[5].sh_type=SHT_RELA;sh[5].sh_offset=2000;sh[5].sh_size=sizeof(re);sh[5].sh_entsize=sizeof(re[0]);sh[5].sh_link=4;sh[5].sh_info=1;sh[5].sh_addralign=8;
 sy[1].st_name=1;sy[1].st_size=237;sy[2].st_name=13;sy[2].st_value=256;sy[2].st_size=373;sy[3].st_name=25;sy[3].st_value=640;sy[3].st_size=64;sy[4].st_name=37;sy[4].st_value=720;sy[4].st_size=70;sy[5].st_name=44;
 for(unsigned i=1;i<6;i++){sy[i].st_info=ELF64_ST_INFO(STB_GLOBAL,STT_FUNC);if(i<5)sy[i].st_shndx=1;}
 memcpy(t+sig,"\x66\x83\x3a\x0e\x0f\x85",6);put32(t+sig+6,0xaf);t[sig+10]=0xf6;t[sig+11]=0x87;put32(t+sig+12,0x54d0);t[sig+16]=0x10;t[sig+17]=0x75;t[sig+18]=0x76;
 t[a-5]=0xe8;memcpy(t+a,"\x41\xb8\x01\0\0\0\xb9\x01\0\0\0\x4c\x89\xef\x48\x89\xc6\xba\0\xdf\x34\xd3\x48\x8b\x80",25);put32(t+a+25,0x2d8);t[a+29]=0xe8;t[a+34]=0xe9;put32(t+a+35,(uint32_t)((int)(sig+19)-(int)(a+39)));
 memcpy(t+256,"\x8d\x88\x94\0\0\0\x41\xb8\x0b\0\0\0",12);
 memcpy(t+640,"\x48\xc7\x87",3);put32(t+643,0x2d8);
 re[0].r_offset=a-4;re[0].r_info=ELF64_R_INFO(4,R_X86_64_PC32);re[0].r_addend=-4;
 re[1].r_offset=a+30;re[1].r_info=ELF64_R_INFO(5,R_X86_64_PC32);re[1].r_addend=-4;
 re[2].r_offset=647;re[2].r_info=ELF64_R_INFO(2,R_X86_64_32S);
 memcpy(buf,&eh,sizeof(eh));memcpy(buf+64,sh,sizeof(sh));memcpy(buf+1600,names,sizeof(names));memcpy(buf+1700,symbols,sizeof(symbols));memcpy(buf+1800,sy,sizeof(sy));memcpy(buf+2000,re,sizeof(re));
}
int main(void)
{
 struct gs_patch_info i;char err[512]={0},hash[65],dir[]="/tmp/gs-patch-test-XXXXXX",path[256];unsigned char original[4096];FILE *f;unsigned char check[4096]={0};
 fixture();memcpy(original,buf,sizeof(buf));CHECK(gs_patch_structure(buf,sizeof(buf),"580.159.01",&i,err,sizeof(err))==0&&i.offset==552&&!i.patched,"stock fixture: %s",err);
 buf[552]=buf[553]=0x90;CHECK(!gs_patch_structure(buf,sizeof(buf),"580.159.01",&i,err,sizeof(err))&&i.patched,"patched fixture: %s",err);
 CHECK(!gs_patch_inspect(buf,sizeof(buf),&i,err,sizeof(err))&&!i.digest,"synthetic anchor is unconfirmed");
 CHECK(!gs_patch_revert_check(buf,sizeof(buf),original,sizeof(original),err,sizeof(err)),"anchor pair");
 CHECK(!gs_patch_exact_pair(buf,sizeof(buf),original,sizeof(original),552,err,sizeof(err)),"exact pair: %s",err);
 buf[100]^=1;CHECK(gs_patch_exact_pair(buf,sizeof(buf),original,sizeof(original),552,err,sizeof(err))<0,"pair differs outside patch");buf[100]^=1;
 CHECK(gs_patch_exact_pair(buf,sizeof(buf)-1,original,sizeof(original),552,err,sizeof(err))<0,"pair size mismatch");
 CHECK(gs_patch_exact_pair(buf,sizeof(buf),original,sizeof(original),SIZE_MAX,err,sizeof(err))<0,"pair offset out of range");
 CHECK(gs_patch_exact_pair(original,sizeof(original),buf,sizeof(buf),552,err,sizeof(err))<0,"pair swapped");
 fixture();buf[512+23]=0;CHECK(gs_patch_structure(buf,sizeof(buf),"580.159.01",&i,err,sizeof(err))<0,"missing signature");
 fixture();memcpy(buf+512+800,buf+512+23,19);CHECK(gs_patch_structure(buf,sizeof(buf),"580.159.01",&i,err,sizeof(err))<0,"duplicate signature");
 fixture();buf[512+165+25]^=1;CHECK(gs_patch_structure(buf,sizeof(buf),"580.159.01",&i,err,sizeof(err))<0,"installer anchor");
 fixture();buf[2000+sizeof(Elf64_Rela)+16]=0;CHECK(gs_patch_structure(buf,sizeof(buf),"580.159.01",&i,err,sizeof(err))<0,"relocation anchor");
 fixture();CHECK(gs_patch_structure(buf,sizeof(buf),"580.178.05",&i,err,sizeof(err))<0,"unsupported version");
 for(size_t n=0;n<448;n++)CHECK(gs_patch_structure(buf,n,"580.159.01",&i,err,sizeof(err))<0,"truncated to %zu bytes",n);
 fixture();{Elf64_Shdr sh;memcpy(&sh,buf+64+sizeof(sh),sizeof(sh));sh.sh_offset=UINT64_MAX-4;memcpy(buf+64+sizeof(sh),&sh,sizeof(sh));}CHECK(gs_patch_structure(buf,sizeof(buf),"580.159.01",&i,err,sizeof(err))<0,"section offset overflow");
 gs_sha256((const unsigned char *)"abc",3,SIZE_MAX,hash);CHECK(!strcmp(hash,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),"sha256 abc");
 if(!mkdtemp(dir)){perror("mkdtemp");return 2;}
 snprintf(path,sizeof(path),"%s/output",dir);CHECK(gs_write_new_output(path,original,sizeof(original),err,sizeof(err))>=0,"new output: %s",err);CHECK(gs_write_new_output(path,buf,sizeof(buf),err,sizeof(err))<0,"existing output refused");
 f=fopen(path,"rb");CHECK(f!=NULL,"reopen output");
 if(f){CHECK(fread(check,1,sizeof(check),f)==sizeof(check),"read output");CHECK(!fclose(f),"close output");CHECK(!memcmp(check,original,sizeof(check)),"output kept first contents");}
 CHECK(!unlink(path),"unlink output");
 CHECK(!symlink("absent",path),"create dangling symlink");CHECK(gs_write_new_output(path,buf,sizeof(buf),err,sizeof(err))<0,"dangling symlink refused");CHECK(!unlink(path),"unlink symlink");CHECK(!rmdir(dir),"rmdir");
 printf("test_patch: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
