"""Read one existing EE or sound-IOP function from a private Ghidra project."""
from pathlib import Path
import argparse,json,os,shutil,subprocess

PORT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('address',type=lambda value:int(value,0))
    parser.add_argument('--iop',action='store_true',help='Inspect the privately imported KOEI sound IRX in OrochiIop')
    args=parser.parse_args()
    if args.iop:
        if not 0<=args.address<0x12440:parser.error('Use an unrelocated address in the sound IRX text section')
    elif not 0x100000<=args.address<0x358500:parser.error('Use an address in the original core ELF')
    paths=json.loads((PORT/'local/analysis_tools_paths.json').read_text())
    script=PORT/'local/ghidra-scripts/InspectOrochiFunction.java'
    shutil.copy2(PORT/'tools/ghidra/InspectOrochiFunction.java',script)
    output=PORT/'local/analysis/decompiled';output.mkdir(exist_ok=True)
    target=output/f'{"iop-" if args.iop else ""}{args.address:x}.c'
    previous_mtime=target.stat().st_mtime_ns if target.exists() else None
    env=os.environ.copy();env['JAVA_HOME']=paths['java_home']
    env['PATH']=str(Path(paths['java_home'])/'bin')+os.pathsep+env['PATH']
    env['JAVA_TOOL_OPTIONS']=f'-Duser.home={(PORT/"local/ghidra-home").as_posix()} -Djava.io.tmpdir={(PORT/"local/tmp").as_posix()}'
    command=[str(Path(paths['ghidra'])/'support/analyzeHeadless.bat'),str(PORT/'local/ghidra-project'),
        'OrochiIop' if args.iop else 'OrochiUSA','-process','module-1179648.irx' if args.iop else 'SLUS_216.62','-readOnly','-noanalysis',
        '-scriptPath',str(script.parent),'-postScript','InspectOrochiFunction.java',hex(args.address),str(target),'-max-cpu','2']
    with (PORT/'logs/inspect-function.log').open('w',encoding='utf8') as log:
        result=subprocess.run(command,env=env,cwd=PORT,stdout=log,stderr=subprocess.STDOUT)
    if result.returncode or not target.exists() or target.stat().st_mtime_ns==previous_mtime:
        print((PORT/'logs/inspect-function.log').read_text(encoding='utf8',errors='replace')[-5000:])
        return result.returncode or 1
    print(target)
    return 0

if __name__=='__main__':raise SystemExit(main())
