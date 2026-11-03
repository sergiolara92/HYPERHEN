from pathlib import Path
import subprocess, json, hashlib, shutil, os
ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tools/plugin-game-runtime-tests'
UP=WORK
V=ROOT/'third_party/tachen-plugin-runtime/game-runtime'
T=ROOT/'runtime-host-tests';T.mkdir(exist_ok=True)
import sys
build=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/'build-test'
blob=build/'libonion_sprx/worker/worker.bin'
results=[]
def run(args,name):
 r=subprocess.run([str(x) for x in args],capture_output=True,text=True,timeout=90)
 (WORK/f'runtime-test-{name}.log').write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(f'{name}: {r.returncode}\n{r.stdout[-2000:]}\n{r.stderr[-4000:]}')
 return r.stdout.strip()
for mode,flags in [('release',['-O2','-DNDEBUG']),('sanitized',['-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
 exe=T/('gate-'+mode)
 run(['g++','-std=c++20','-pthread','-DHDE_DECODE_ONLY','-I',V,'-I',WORK,*flags,
      WORK/'gate_blob_test.cpp',V/'game-trampoline.cpp',V/'relocate.cpp',V/'hde64.cpp','-o',exe],f'gate-build-{mode}')
 result=run([exe,blob],f'gate-run-{mode}')
 results.append({'case':'actual_native_callback_blob','mode':mode,'result':result})
 for label,runner,sources in [
  ('publication','publication-runner.c',['publication.c']),
  ('imports','game-imports-runner.c',['game-imports.c'])]:
  exe=T/(label+'-'+mode)
  run(['gcc','-std=gnu17','-I',V,'-I',WORK,*flags,WORK/runner,*[V/x for x in sources],'-o',exe],f'{label}-build-{mode}')
  result=run([exe],f'{label}-run-{mode}')
  results.append({'case':label,'mode':mode,'result':result})
 # Real filesystem receipt reconciliation and production coordinator; console
 # memory APIs only are mocked. Avoid relying on callback-only coverage.
 src=ROOT/'source';stubs=ROOT/'tools/plugin-runtime-stubs'
 assert stubs.exists(), 'Missing source-host stubs'
 receipts=T/('receipts-'+mode);legacy=T/('legacy-'+mode)
 # Every test run gets a new temporary root without touching prior outputs.
 import tempfile
 fixture=Path(tempfile.mkdtemp(prefix='receipt-',dir=T));receipts=fixture/'current';legacy=fixture/'legacy'
 sha=T/('sha-'+mode+'.o')
 run(['gcc',*flags,'-c',ROOT/'third_party/cheat_support/sha256.c','-o',sha],f'receipt-sha-{mode}')
 exe=T/('receipts-'+mode)
 run(['g++','-std=c++20','-D_DEFAULT_SOURCE','-DONION_HOST_TEST=1','-DONION_LOG_COMPILE_LEVEL=0',
      f'-DHYPERHEN_PLUGIN_ATTEMPT_ROOT="{receipts}"',f'-DHYPERHEN_PLUGIN_LEGACY_ROOT="{legacy}"',
      '-I',stubs,'-I',src/'libonion_sprx/include','-I',src/'libonion_platform/include','-I',src/'libonion_proc/include','-I',src/'include',
      '-I',ROOT/'third_party/tachen-plugin-runtime','-I',ROOT/'third_party/cheat_support',*flags,
      WORK/'receipt_transport_test.cpp',src/'libonion_sprx/source/ptrace_sprx_runtime.cpp',sha,'-pthread','-o',exe],f'receipts-build-{mode}')
 result=run([exe],f'receipts-run-{mode}')
 results.append({'case':'production_receipt_coordinator','mode':mode,'result':result})
for mode,flags in [('release',['-O2','-DNDEBUG']),('sanitized',['-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
 exe=T/('images-'+mode)
 run(['gcc','-std=gnu17','-D_DEFAULT_SOURCE','-I',ROOT/'third_party/tachen-plugin-runtime',*flags,
      WORK/'image_check.c',ROOT/'third_party/tachen-plugin-runtime/elf-image.c',
      ROOT/'third_party/tachen-plugin-runtime/prx-image.c',ROOT/'third_party/tachen-plugin-runtime/game-info.c','-o',exe],f'images-build-{mode}')
 results.append({'case':'image_bounds_and_sdk_adaptation','mode':mode,'result':run([exe],f'images-run-{mode}')})
shutil.copyfile(blob,WORK/'tested-game-callback.bin')
report={'result':'PASS','native_callback_sha256':hashlib.sha256(blob.read_bytes()).hexdigest(),
 'native_callback_bytes':blob.stat().st_size,'checks':results,
 'hardware':'PENDING; these are actual x86-64 callback and host publication tests, not PS5 execution'}
(WORK/'runtime-tests-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
