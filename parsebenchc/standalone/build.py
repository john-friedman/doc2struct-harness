"""Build checked-in C sources without importing ParseBench or linking Python."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--regenerate',action='store_true',help='Regenerate policy C from the frozen ParseBench installation')
    parser.add_argument('--debug',action='store_true')
    parser.add_argument('--no-lto',action='store_true')
    args=parser.parse_args()
    root=Path(__file__).resolve().parent
    if args.regenerate:
        subprocess.run([sys.executable,str(root/'compile.py')],check=True)
    dest=root.parent/'build/standalone'
    dest.mkdir(parents=True,exist_ok=True)
    env={k.upper():v for k,v in os.environ.items()}
    lto=not(args.debug or args.no_lto)
    if sys.platform=='win32':
        if not shutil.which('cl'):
            vs=Path(os.environ.get('ProgramFiles(x86)','C:/Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
            found=subprocess.check_output([str(vs),'-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],text=True).strip()
            if not found:raise SystemExit('Install Visual Studio C++ Build Tools with the x64 C compiler.')
            vcvars=Path(found)/'VC/Auxiliary/Build/vcvarsall.bat'
            raw=subprocess.check_output(f'cmd /d /s /c ""{vcvars}" x64 >nul && set"',text=True)
            for line in raw.splitlines():
                k,sep,v=line.partition('=')
                if sep and k:env[k.upper()]=v
        compiler=shutil.which('cl',path=env['PATH'])
        common=[compiler,'/nologo','/Od' if args.debug else '/O2','/fp:strict','/utf-8','/W3','/std:c11','/bigobj','/DUTF8PROC_STATIC',f'/I{root}']
        if lto:common.append('/GL')
        def compile_object(source,name,headers):
            output=dest/(name+'.obj');stamp=output.with_suffix('.stamp')
            digest=hashlib.sha256(json.dumps(common).encode())
            for path in [source,*headers]:digest.update(path.read_bytes())
            fingerprint=digest.hexdigest()
            if not output.exists() or not stamp.exists() or stamp.read_text()!=fingerprint:
                subprocess.run(common+['/c',str(source),f'/Fo:{output}'],cwd=dest,env=env,check=True)
                stamp.write_text(fingerprint)
            return output
        policy=compile_object(root/'generated/scorers.c','scorers',[root/'runtime.h',root/'vendor/utf8proc.h'])
        unicode_obj=compile_object(root/'vendor/utf8proc.c','utf8proc',[root/'vendor/utf8proc.h',root/'vendor/utf8proc_data.c'])
        native=compile_object(root/'main.c','main',list(root.glob('*.c'))+[root/'runtime.h',*sorted((root/'generated').glob('*.h')),*sorted((root/'vendor').glob('*.h'))])
        cmd=[compiler,'/nologo',str(native),str(policy),str(unicode_obj),f'/Fe:{dest/"parsebenchc.exe"}','/link','/STACK:67108864']
        if lto:cmd.append('/LTCG')
        candidates=[Path('C:/Program Files/Git/mingw64/bin/libpcre2-8-0.dll'),Path('C:/Program Files/Tesseract-OCR/libpcre2-8-0.dll')]
        found=next((p for p in candidates if p.exists()),None)
        if found:shutil.copy2(found,dest/found.name)
    else:
        cmd=[os.environ.get('CC','cc'),'-g' if args.debug else '-O3','-ffp-contract=off','-std=c11','-DUTF8PROC_STATIC',f'-I{root}',str(root/'main.c'),str(root/'generated/scorers.c'),str(root/'vendor/utf8proc.c'),'-lm','-ldl','-pthread','-o',str(dest/'parsebenchc')]
        if lto:cmd.insert(1,'-flto')
    subprocess.run(cmd,cwd=dest,env=env,check=True)
    (dest/'build.json').write_text(json.dumps({'command':cmd,'lto':lto,'debug':args.debug},indent=2),encoding='utf-8')
    print(dest/('parsebenchc.exe' if sys.platform=='win32' else 'parsebenchc'))


if __name__=='__main__':main()
