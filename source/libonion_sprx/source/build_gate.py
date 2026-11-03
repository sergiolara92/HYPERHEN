"""Build a freestanding, position-independent private userland function."""
from pathlib import Path
import hashlib,json,os,struct,subprocess,sys
if len(sys.argv)!=4:raise SystemExit('usage: build_gate.py source_dir build_dir output_header')
SOURCE,BUILD,HEADER=map(Path,sys.argv[1:])
SDK=Path(os.environ.get('PS5_PAYLOAD_SDK','/opt/ps5-payload-sdk'))
BUILD.mkdir(parents=True,exist_ok=True);HEADER.parent.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
obj=BUILD/'raidougie_bridge.o';elf=BUILD/'raidougie_bridge.elf';binary=BUILD/'raidougie_bridge.bin'
commands=[
    [str(SDK/'bin/prospero-clang'),'-std=c11','-Os','-DNDEBUG','-ffreestanding','-fno-builtin',
     '-fno-stack-protector','-fno-asynchronous-unwind-tables','-fno-unwind-tables','-fPIE',
     '-mno-red-zone','-Wall','-Wextra','-Werror','-I'+str(SOURCE),'-c',str(SOURCE/'raidougie_bridge.c'),'-o',str(obj)],
    [str(SDK/'bin/ld.lld'),'-m','elf_x86_64','--no-undefined','-static','-T',str(SOURCE/'raidougie_bridge.ld'),str(obj),'-o',str(elf)],
    [str(SDK/'bin/llvm-objcopy'),'-O','binary','--only-section=.text',str(elf),str(binary)],
]
logs=[]
for command in commands:
    result=subprocess.run(command,capture_output=True,text=True);logs.append(result.stdout+result.stderr)
    if result.returncode:raise RuntimeError(result.stderr[-7000:])
(BUILD/'gate-build.log').write_text('\n'.join(logs))
data=elf.read_bytes();assert data[:6]==b'\x7fELF\x02\x01'
assert struct.unpack_from('<H',data,18)[0]==62 and struct.unpack_from('<Q',data,24)[0]==0
shoff=struct.unpack_from('<Q',data,40)[0];entsize,number,names=struct.unpack_from('<HHH',data,58)
assert entsize==64 and shoff+number*64<=len(data)
sections=[struct.unpack_from('<IIQQQQIIQQ',data,shoff+i*64) for i in range(number)]
string=sections[names];strings=data[string[4]:string[4]+string[5]]
inventory={}
for item in sections:
    end=strings.find(b'\0',item[0]);name=strings[item[0]:end].decode()
    inventory[name]=dict(type=item[1],address=item[3],bytes=item[5])
    assert item[1] not in (4,9) or item[5]==0,'Unresolved relocation section '+name
    if item[1]==2:
        for at in range(item[4]+24,item[4]+item[5],24):
            symbol=struct.unpack_from('<IBBHQQ',data,at)
            assert symbol[3]!=0,'Undefined gate symbol'
assert inventory['.text']['address']==0 and 0<inventory['.text']['bytes']<=4096
assert all(inventory.get(name,{}).get('bytes',0)==0 for name in ('.data','.bss','.got','.got.plt','.plt'))
blob=binary.read_bytes();assert len(blob)==inventory['.text']['bytes']
rows=[', '.join('0x%02x'%b for b in blob[i:i+16]) for i in range(0,len(blob),16)]
header='// Generated from the freestanding RaiDougie process-info bridge.\n#pragma once\n#include <stddef.h>\nstatic const unsigned char hhp_rai_bridge_bytes[] = {\n    '+',\n    '.join(rows)+'\n};\n'
if not HEADER.exists() or HEADER.read_text()!=header:HEADER.write_text(header,encoding='utf-8',newline='\n')
report=dict(result='PASS',scope='Generated private userland function; no CRT, syscall or external symbol',
            blob=dict(file=str(binary),bytes=len(blob),sha256=sha(binary)),
            header=dict(file=str(HEADER),sha256=sha(HEADER)),ELF=dict(file=str(elf),sha256=sha(elf)),
            source_sha256={name:sha(SOURCE/name) for name in ('raidougie_bridge.c','raidougie_compat.h','raidougie_bridge.ld')},
            commands=commands,sections=inventory,entry_offset=0,unresolved_relocations=0,undefined_symbols=0)
(BUILD/'gate-build-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(dict(result='PASS',gate_bytes=len(blob),sha256=sha(binary))))
