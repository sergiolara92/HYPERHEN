// SPDX-License-Identifier: GPL-3.0-or-later
#include "raidougie_compat.h"
#include "sha256.h"
#include <string.h>

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} RaiProgram;
_Static_assert(sizeof(RaiProgram)==56,"ELF64 program ABI");
static const unsigned char expected_wrapper[42]={
    0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x48,0x89,0x7d,0xf8,
    0x48,0x89,0x75,0xf0,0x48,0x8b,0x75,0xf8,0x48,0x8b,0x55,0xf0,
    0xbf,0xf4,0x01,0x00,0x00,0xb0,0x00,0xe8,0x8c,0x01,0x00,0x00,
    0x48,0x83,0xc4,0x10,0x5d,0xc3
};
static const unsigned char expected_syscall[HHP_RAI_SYSCALL_BYTES]={
    0x48,0xc7,0xc0,0,0,0,0,0x49,0x89,0xca,0x0f,0x05,
    0x0f,0x82,0x01,0,0,0,0xc3,0x50,0xe8,0x07,0x03,0,0,
    0x59,0x89,0x08,0x48,0xc7,0xc0,0xff,0xff,0xff,0xff,
    0x48,0xc7,0xc2,0xff,0xff,0xff,0xff,0xc3
};
static const unsigned char expected_hash[32]={
    0x15,0xdd,0xe8,0xa5,0x61,0xa6,0xd5,0x0f,0x73,0x87,0x81,0x5e,0x16,0xc7,0x8d,0xf6,
    0x78,0xb0,0x6a,0xa7,0xbd,0xb5,0x3b,0x8e,0x0d,0x8f,0xa5,0xc4,0x4e,0x94,0xb2,0x6c
};
static const unsigned char expected_raw_hash[32]={
    0xe7,0xb2,0x5e,0xb1,0xc6,0x24,0x5f,0x1e,0xf3,0xf9,0x7a,0x30,0x1c,0x23,0xfa,0x53,
    0xba,0xa3,0x3c,0xe4,0x6a,0x09,0xc9,0x83,0x42,0x08,0x11,0x8d,0xd5,0x7b,0x53,0x58
};
static uint16_t u16(const unsigned char *p){uint16_t v;memcpy(&v,p,2);return v;}
static uint32_t u32(const unsigned char *p){uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t u64(const unsigned char *p){uint64_t v;memcpy(&v,p,8);return v;}
static int span(uint64_t at,uint64_t size,uint64_t total){return at<=total&&size<=total-at;}
static int contains(uint64_t start,uint64_t size,uint64_t at,uint64_t bytes){
    return at>=start&&at-start<=size&&bytes<=size-(at-start);
}
static int address(uint64_t at,uint64_t bytes){
    return at>=UINT64_C(0x10000)&&at<UINT64_C(0x800000000000)&&bytes<=UINT64_C(0x800000000000)-at;
}
// Map logical ELF file data through this SELF's plaintext segment descriptors.
// A logical range must have exactly one physical backing segment.
static const unsigned char *region(const unsigned char *b,size_t size,const RaiProgram ph[7],int raw,uint64_t at,uint64_t bytes){
    if(raw)return span(at,bytes,size)?b+at:NULL;
    const unsigned char *found=NULL;
    for(unsigned i=0;i<8;++i){
        const unsigned char *e=b+32+i*32;uint64_t flags=u64(e),start=u64(e+8),len=u64(e+16);
        if((flags&0xfffff)!=0x2804)continue;
        uint64_t index=flags>>20;if(index>=7||len!=u64(e+24)||len!=ph[index].filesz)return NULL;
        if(!contains(ph[index].offset,len,at,bytes))continue;
        uint64_t delta=at-ph[index].offset;
        if(!span(start,len,size)||!span(delta,bytes,len)||found)return NULL;
        found=b+start+delta;
    }
    return found;
}
static int mapped_word(const RaiProgram ph[7],uint64_t at){
    for(unsigned i=0;i<7;++i)if((ph[i].type==1||ph[i].type==0x61000010)&&contains(ph[i].vaddr,ph[i].memsz,at,8))return 1;
    return 0;
}
static int layout(const unsigned char *b,size_t size){
    if(!b)return -1;
    const int raw=size==HHP_RAI_RAW_FILE_BYTES&&u32(b)==0x464c457f;
    if(!raw){
        if(size!=HHP_RAI_FILE_BYTES||u32(b)!=0x1d3d154f||u16(b+24)!=8||u64(b+16)!=size)return -1;
        for(unsigned i=0;i<8;++i){const unsigned char *s=b+32+i*32;if(!span(u64(s+8),u64(s+16),size))return -1;}
    }
    const unsigned char *elf=b+(raw?0:288);
    if(memcmp(elf,"\177ELF\2\1\1\11\0",9)||u16(elf+16)!=0xfe18||u16(elf+18)!=62||
       u32(elf+20)!=1||u64(elf+32)!=64||u16(elf+52)!=64||u16(elf+54)!=56||u16(elf+56)!=7)return -1;
    RaiProgram ph[7];memcpy(ph,elf+64,sizeof ph);
    if(raw)for(unsigned i=0;i<7;++i)if(!span(ph[i].offset,ph[i].filesz,size))return -1;
    unsigned dynamic_count=0,metadata_count=0;RaiProgram dynamic={0},metadata={0};
    for(unsigned i=0;i<7;++i){
        if(ph[i].type==7)return -1; // This exact identity has no TLS template.
        if(ph[i].type==2){dynamic=ph[i];++dynamic_count;}
        if(ph[i].type==0x61000000){metadata=ph[i];++metadata_count;}
        if(ph[i].type==1||ph[i].type==0x61000010){
            if(ph[i].filesz>ph[i].memsz||ph[i].memsz>512u*1024u*1024u||
               ph[i].vaddr>UINT64_MAX-ph[i].memsz)return -1;
            if(ph[i].align>1&&((ph[i].align&(ph[i].align-1))||ph[i].vaddr%ph[i].align!=ph[i].offset%ph[i].align))return -1;
            for(unsigned j=0;j<i;++j)if((ph[j].type==1||ph[j].type==0x61000010)&&ph[i].memsz&&ph[j].memsz&&
                ph[i].vaddr<ph[j].vaddr+ph[j].memsz&&ph[j].vaddr<ph[i].vaddr+ph[i].memsz)return -1;
        }
    }
    if(dynamic_count!=1||metadata_count!=1||!dynamic.filesz||dynamic.filesz%16||dynamic.filesz>4096)return -1;
    if(ph[0].type!=1||ph[0].flags!=5||ph[0].vaddr||ph[0].offset!=0x4000||ph[0].filesz!=0xd8000)return -1;
    const unsigned char *wrapper=region(b,size,ph,raw,ph[0].offset+HHP_RAI_WRAPPER_VADDR,42);
    if(wrapper!=b+(raw?HHP_RAI_RAW_WRAPPER_OFFSET:HHP_RAI_WRAPPER_OFFSET)||memcmp(wrapper,expected_wrapper,42))return -1;
    const unsigned char *syscall=region(b,size,ph,raw,ph[0].offset+HHP_RAI_SYSCALL_VADDR,HHP_RAI_SYSCALL_BYTES);
    if(syscall!=b+(raw?HHP_RAI_RAW_SYSCALL_OFFSET:HHP_RAI_SYSCALL_OFFSET)||
       memcmp(syscall,expected_syscall,HHP_RAI_SYSCALL_BYTES))return -1;
    const unsigned char *dyn=region(b,size,ph,raw,dynamic.offset,dynamic.filesz);
    if(!dyn)return -1;
    const uint64_t tags[]={0x6100002f,0x61000031,0x61000033,0x61000029,0x6100002d,0x6100002b,0x61000039,0x6100003f,0x6100003b};
    uint64_t values[9]={0};unsigned seen=0;int ended=0;
    for(size_t i=0;i<dynamic.filesz;i+=16){
        uint64_t tag=u64(dyn+i),value=u64(dyn+i+8);if(!tag){ended=1;break;}
        for(unsigned j=0;j<9;++j)if(tag==tags[j]){if(seen&(1u<<j))return -1;seen|=1u<<j;values[j]=value;}
    }
    if(!ended||seen!=511||values[2]!=24||values[5]!=7||values[8]!=24||!values[7]||values[7]%24)return -1;
    if(!span(values[6],values[7],metadata.filesz)||metadata.offset>UINT64_MAX-values[6]||
       !region(b,size,ph,raw,metadata.offset+values[6],values[7]))return -1;
    uint64_t symbols=values[7]/24;
    for(unsigned table=0;table<2;++table){
        uint64_t off=values[table?3:0],bytes=values[table?4:1];
        if(!bytes||bytes%24||!span(off,bytes,metadata.filesz)||metadata.offset>UINT64_MAX-off)return -1;
        const unsigned char *rel=region(b,size,ph,raw,metadata.offset+off,bytes);if(!rel)return -1;
        for(uint64_t i=0;i<bytes;i+=24){
            uint64_t at=u64(rel+i),info=u64(rel+i+8);unsigned type=(unsigned)info;
            if((type!=6&&type!=7&&type!=8)||(info>>32)>=symbols||!mapped_word(ph,at)||
               (at<HHP_RAI_WRAPPER_VADDR+42&&HHP_RAI_WRAPPER_VADDR<at+8)||
               (at<HHP_RAI_SYSCALL_VADDR+HHP_RAI_SYSCALL_BYTES&&HHP_RAI_SYSCALL_VADDR<at+8))return -1;
        }
    }
    return 0;
}
int hhp_raidougie_supported(const char *sha){
    return sha&&(!strcmp(sha,HHP_RAIDOUGIE_SHA)||!strcmp(sha,HHP_RAIDOUGIE_RAW_SHA));
}
int hhp_raidougie_admit(const void *data,size_t size){
    const unsigned char *b=data;if(layout(b,size))return -1;
    unsigned char digest[32];SHA256_CTX ctx;sha256_init(&ctx);sha256_update(&ctx,b,size);sha256_final(&ctx,digest);
    const unsigned char *expected=size==HHP_RAI_RAW_FILE_BYTES?expected_raw_hash:expected_hash;
    return memcmp(digest,expected,sizeof digest)?-1:0;
}
static int text(const char *s,size_t cap){
    size_t n=0;while(n<cap&&s[n]){unsigned c=(unsigned char)s[n];if(c<0x20||c==0x7f)return 0;++n;}
    return n>0&&n<cap;
}
int hhp_raidougie_control_init(HhpRaiControl *out,uint64_t cookie,const HhpRaiInfo *info){
    if(!out||!info||!cookie||info->pid<=1||!address(info->base,1)||
       !text(info->name,sizeof info->name)||!text(info->path,sizeof info->path)||info->path[0]!='/'||
       !text(info->content,sizeof info->content)||!text(info->title,sizeof info->title)||
       memcmp(info->title,"CUSA",4)||info->title[9]||info->version[5]||info->version[2]!='.')return -1;
    for(unsigned i=4;i<9;++i)if(info->title[i]<'0'||info->title[i]>'9')return -1;
    for(unsigned i=0;i<5;++i)if(i!=2&&(info->version[i]<'0'||info->version[i]>'9'))return -1;
    HhpRaiControl snapshot={0};snapshot.magic=HHP_RAI_MAGIC;snapshot.cookie=cookie;
    snapshot.version=HHP_RAI_VERSION;snapshot.bytes=sizeof snapshot;memcpy(&snapshot.info,info,sizeof *info);
    memcpy(out,&snapshot,sizeof snapshot);return 0;
}
int hhp_raidougie_patch(void *bytes,size_t size,uint64_t entry,uint64_t control,uint64_t native_entry){
    if(!address(entry,4096)||!address(control,sizeof(HhpRaiControl))||(control&7)||
       !address(native_entry,36)||native_entry==entry||
       (native_entry<control+sizeof(HhpRaiControl)&&control<native_entry+36)||
       (entry<control+sizeof(HhpRaiControl)&&control<entry+4096)||hhp_raidougie_admit(bytes,size))return -1;
    // SysV: command/argument -> control/command/argument plus captured original
    // caller RBP/RSP and our own PRX slide. Every byte of the42-byte original
    // wrapper is replaced. The host never modifies the original uploaded file.
    unsigned char code[42]={0x48,0x89,0xf2,0x48,0x89,0xfe,0x48,0x89,0xe9,0x49,0x89,0xe0,
        0x4c,0x8d,0x0d,0,0,0,0,0x48,0xbf,0,0,0,0,0,0,0,0,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0,0x90};
    int32_t relative=-(int32_t)(HHP_RAI_WRAPPER_VADDR+19);memcpy(code+15,&relative,4);
    memcpy(code+21,&control,8);memcpy(code+31,&entry,8);
    const size_t offset=size==HHP_RAI_RAW_FILE_BYTES?HHP_RAI_RAW_WRAPPER_OFFSET:HHP_RAI_WRAPPER_OFFSET;
    // Call the verified bridge and return directly to the original carry test.
    // The bridge and the native gate preserve CF and the original errno path.
    unsigned char native_code[HHP_RAI_SYSCALL_PREFIX_BYTES]={0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0};
    memcpy(native_code+2,&native_entry,8);
    const size_t native_offset=size==HHP_RAI_RAW_FILE_BYTES?HHP_RAI_RAW_SYSCALL_OFFSET:HHP_RAI_SYSCALL_OFFSET;
    memcpy((unsigned char*)bytes+offset,code,sizeof code);
    memcpy((unsigned char*)bytes+native_offset,native_code,sizeof native_code);return 0;
}
