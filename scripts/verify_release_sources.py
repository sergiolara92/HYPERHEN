#!/usr/bin/env python3
"""Verify archived build inputs and portable source recipes without compilation."""
from pathlib import Path
import hashlib, json

root=Path(__file__).resolve().parent.parent
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
checked=0
for folder in ('release-components','release-inputs'):
    base=root/folder
    listed=set()
    for line in (base/'SHA256SUMS').read_text().splitlines():
        expected,name=line.split(None,1);name=name.lstrip('* ')
        target=(base/name).resolve();assert target.is_relative_to(base.resolve())
        assert sha(target)==expected,name;checked+=1;listed.add(name)
    actual={p.relative_to(base).as_posix() for p in base.rglob('*') if p.is_file() and p.name not in {'MANIFEST.json','SHA256SUMS'}}
    assert actual==listed,f'Unmanifested input: {sorted(actual-listed)}'
recipe=json.loads((root/'source/hyper/build-recipe.json').read_text())
assert recipe['worker_revision']=='R52'
assert recipe['crt_sha256']=='817750baf8ff32deb2b480f5c4640978372fe06a952697befa21bc7788edd09b'
assert recipe['expected_retained_sha256']==sha(root/'release-components/hyper_assistant_r52.elf')
for command in recipe['commands']:
    source=command[command.index('-c')+1]
    assert source.startswith('@ROOT@/'),source
    assert (root/source[len('@ROOT@/'):]).is_file(),source
assert recipe['link'][-1]=='@ROOT@/release-inputs/sdk/crt1-r42-1360.o'
assert '-nostartfiles' in recipe['link']
assert (root/'third_party/kstuff-1270-r48/hyperhen-include/onion/ready.h').is_file()
assert (root/'third_party/shadowmountplus/Makefile').is_file()
assert not list(root.rglob('*.gguf'))
print(json.dumps({'result':'PASS','checksummed_inputs':checked,'worker':'R52','crt':'R42','models_included':False}))
