#!/usr/bin/env python3
"""Rebuild the dedicated R48 firmware12.70 HyperCore in a private directory."""
from pathlib import Path
import argparse, hashlib, os, shutil, subprocess

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',type=Path)
    p.add_argument('--jobs',type=int,default=4)
    p.add_argument('--check-only',action='store_true')
    a=p.parse_args();root=Path(__file__).resolve().parent.parent
    sdk=Path(os.environ['PS5_PAYLOAD_SDK']).resolve()
    crt=root/'release-inputs/sdk/crt1-r42-1360.o'
    assert hashlib.sha256(crt.read_bytes()).hexdigest()=='817750baf8ff32deb2b480f5c4640978372fe06a952697befa21bc7788edd09b'
    source=root/'third_party/kstuff-1270-r48';assert (source/'ps5-kstuff-ldr/Makefile').is_file()
    if a.check_only:print('Dedicated R48 firmware12.70 source and CRT42: PASS');return
    build=(a.build_dir or root/'build-core-1270').resolve()
    assert not build.exists(),'Use a fresh build directory; existing work is preserved'
    shutil.copytree(source,build)
    env=os.environ.copy();env['PATH']=str(sdk/'bin')+os.pathsep+env['PATH']
    subprocess.run(['make','-C',str(build/'ps5-kstuff-ldr'),f'-j{a.jobs}','KSTUFF_OBS=0',
        'HYPERHEN_INCLUDE_DIR='+str(build/'hyperhen-include'),'HYPERHEN_CRT='+str(crt)],env=env,check=True)
    result=build/'ps5-kstuff-ldr/kstuff.elf'
    print(f'Built {result}\nSHA256 {hashlib.sha256(result.read_bytes()).hexdigest()}')
    print('Retained binary is not replaced; console validation remains separate.')

if __name__=='__main__':main()
