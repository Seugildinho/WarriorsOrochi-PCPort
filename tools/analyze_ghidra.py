"""Analyse the private PS2 ELF with the pinned, portable EE toolchain."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, struct, subprocess

PORT = Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--update',action='store_true')
    parser.add_argument('--export-only',action='store_true')
    options=parser.parse_args()
    update=options.update or options.export_only
    paths = json.loads((PORT/'local/analysis_tools_paths.json').read_text())
    home = PORT/'local/ghidra-home'
    temp = PORT/'local/tmp'
    project = PORT/'local/ghidra-project'
    output = PORT/'local/analysis'
    scripts=PORT/'local/ghidra-scripts'
    for folder in (home, temp, project, output, scripts, PORT/'logs'):
        folder.mkdir(parents=True, exist_ok=True)
    shutil.copy2(PORT/'tools/ghidra/SeedOrochiFunctions.java',scripts/'SeedOrochiFunctions.java')
    shutil.copy2(PORT/'tools/ghidra/DiscoverOrochiFunctions.java',scripts/'DiscoverOrochiFunctions.java')
    shutil.copy2(PORT/'tools/ghidra/ExportOrochiFunctions.java',scripts/'ExportOrochiFunctions.java')
    shutil.copy2(PORT/'vendor/PS2Recomp/ps2xRecomp/tools/ghidra/ExportPS2Functions.java',scripts/'ExportPS2Functions.java')
    env = os.environ.copy()
    env['JAVA_HOME'] = paths['java_home']
    env['PATH'] = str(Path(paths['java_home'])/'bin')+os.pathsep+env['PATH']
    env['JAVA_TOOL_OPTIONS'] = f'-Duser.home={home.as_posix()} -Djava.io.tmpdir={temp.as_posix()}'
    profile=json.loads((PORT/'config/analysis.json').read_text())
    elf=(PORT/'local/SLUS_216.62').read_bytes()
    assert hashlib.sha256(elf).hexdigest()==profile['elf_sha256']
    tables=[{'start':profile['initializer_table_start'],'end':profile['initializer_table_end']}]+profile.get('indirect_tables',[])
    seeds={int(x,0) for x in profile.get('traced_entry_points',[])}
    seeds.update(int(x,0) for x in profile.get('overlay_direct_calls',{}))
    for table in tables:
        start,end=int(table['start'],0),int(table['end'],0)
        stride=int(table.get('stride',4))
        assert stride>=4 and stride%4==0 and (end-start)%stride==0
        offsets=table.get('pointer_offsets',[0])
        assert offsets and all(0<=offset<=stride-4 and offset%4==0 for offset in offsets)
        for address in range(start,end,stride):
            for offset in offsets:
                target=struct.unpack_from('<I',elf,address+offset-0x100000+0x800)[0]
                if target==0 and table.get('allow_null',False):continue
                assert 0x100000<=target<0x358500 and target%4==0,'Invalid callback pointer'
                seeds.add(target)
    args = [str(Path(paths['ghidra'])/'support/analyzeHeadless.bat'),str(project),'OrochiUSA']
    args += (['-process','SLUS_216.62'] if update else
             ['-import',str(PORT/'local/SLUS_216.62'),'-overwrite','-processor','r5900:LE:32:default'])
    args += ['-scriptPath',str(scripts),
            '-preScript','SeedOrochiFunctions.java']+[hex(x) for x in sorted(seeds)]
    args += [
            '-preScript','DiscoverOrochiFunctions.java',str(output/'indirect_candidates.csv'),
            '-postScript', 'ExportPS2Functions.java', str(output/'orochi.ghidra.toml'),
            str(output/'orochi.ghidra.csv'),
            '-postScript','ExportOrochiFunctions.java',str(output/'orochi.ranges.csv'),
            '-analysisTimeoutPerFile', '300',
            '-max-cpu', '4', '-log', str(PORT/'logs/ghidra-application.log')]
    if options.export_only:args+=['-noanalysis']
    print('Analysing the PS2 ELF with Ghidra; see logs/ghidra.log', flush=True)
    with (PORT/'logs/ghidra.log').open('w', encoding='utf8') as log:
        result = subprocess.run(args, env=env, cwd=PORT, stdout=log, stderr=subprocess.STDOUT)
    print((PORT/'logs/ghidra.log').read_text(encoding='utf8', errors='replace')[-6000:])
    raise SystemExit(result.returncode)

if __name__ == '__main__':
    main()
