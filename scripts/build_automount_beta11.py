#!/usr/bin/env python3
"""Rebuild frozen beta11 Automount variants without downloads or SDK changes."""
from pathlib import Path
import argparse, hashlib, os, shutil, subprocess

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--variant',choices=('standard','lite','both'),default='both')
    p.add_argument('--build-dir',type=Path)
    p.add_argument('--jobs',type=int,default=4)
    p.add_argument('--check-only',action='store_true')
    a=p.parse_args();root=Path(__file__).resolve().parent.parent
    sdk=Path(os.environ['PS5_PAYLOAD_SDK']).resolve()
    crt=root/'release-inputs/sdk/crt1-r42-1360.o'
    assert hashlib.sha256(crt.read_bytes()).hexdigest()=='817750baf8ff32deb2b480f5c4640978372fe06a952697befa21bc7788edd09b'
    source=root/'third_party/shadowmountplus'
    deps=root/'release-inputs/automount-json-c'
    assert (deps/'lib/libjson-c.a').is_file() and (deps/'include/json-c/json.h').is_file()
    assert (source/'Makefile').is_file()
    if a.check_only:print('Automount beta11 source, private json-c and CRT42: PASS');return
    build=(a.build_dir or root/'build-automount-beta11').resolve()
    assert not build.exists(),'Use a fresh build directory; existing work is preserved'
    env=os.environ.copy();env['PATH']=str(sdk/'bin')+os.pathsep+env['PATH']
    for variant in ('standard','lite') if a.variant=='both' else (a.variant,):
        target=build/variant;shutil.copytree(source,target)
        # json-c's phony prerequisite is intentionally bypassed only for the
        # checksum-pinned archived library. The source patch and archive remain.
        makefile=target/'Makefile';text=makefile.read_text()
        start=text.index('$(JSON_C_LIB): FORCE ');end=text.index('\n\n',start)
        text=text[:start]+text[end:];makefile.write_text(text)
        subprocess.run(['make','-C',str(target),f'-j{a.jobs}','shadowmountplus.elf',
          'HYPERHEN_LITE='+str(int(variant=='lite')),'HYPERHEN_CRT='+str(crt),
          'JSON_C_PREFIX='+str(deps),'VERSION_TAG=1.7beta11-hyperhen',
          'BUILD_TIME=2026-10-31T00:00:00Z'],env=env,check=True)
        output=target/'shadowmountplus.elf'
        print(f'{variant}: {output}\nSHA256 {hashlib.sha256(output.read_bytes()).hexdigest()}')
    print('Retained binaries are not replaced; console validation remains separate.')

if __name__=='__main__':main()
