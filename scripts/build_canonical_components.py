#!/usr/bin/env python3
"""Reproduce the exact release ShellUI/HyperDeck links from canonical inputs.

The optional --recompile-version-units rebuilds the two/one changed source units
with prefix maps preserving their reviewed source/debug provenance.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component',choices=('shellui','hyperdeck','both'),default='both')
    p.add_argument('--build-dir',type=Path)
    p.add_argument('--recompile-version-units',action='store_true')
    a=p.parse_args();root=Path(__file__).resolve().parent.parent
    sdk=Path(os.environ['PS5_PAYLOAD_SDK']).resolve()
    recipes=json.loads((root/'source/canonical-components/build-recipe.json').read_text())['components']
    build=(a.build_dir or root/'build-canonical-components').resolve()
    assert not build.exists(),'Use a fresh build directory; existing work is preserved'
    def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
    for name in recipes if a.component=='both' else (a.component,):
        recipe=recipes[name];inputs=root/'release-inputs/canonical-components'/name/'objects'
        for token,expected in recipe['objects_sha256'].items():assert sha(inputs/token)==expected,token
        target=build/name;shutil.copytree(inputs,target)
        def expand(token):return token.replace('@ROOT@',str(root)).replace('@SDK@',str(sdk)).replace('@BUILD@',str(target))
        if a.recompile_version_units:
            maps=[expand(src)+'='+original for src,original in recipe['prefix_maps']]
            maps.append(str(target)+'='+recipe['original_version_build'])
            for command in recipe['commands']:
                command=[expand(t) for t in command]
                command.extend('-ffile-prefix-map='+mapping for mapping in maps)
                subprocess.run(command,cwd=target,check=True)
        link=[expand(t) for t in recipe['link']]
        assert '-nostartfiles' in link and link[-1]==str(root/'release-inputs/sdk/crt1-r42-1360.o')
        subprocess.run(link,cwd=target,check=True)
        subprocess.run([expand(t) for t in recipe['postlink']],cwd=target,check=True)
        output=Path(link[link.index('-o')+1]);actual=sha(output)
        print(f'{name}: {output}\nSHA256 {actual}')
        assert actual==recipe['expected_sha256'],name+' output differs from the release component'
    print('Canonical release component reproduction: PASS')

if __name__=='__main__':main()
