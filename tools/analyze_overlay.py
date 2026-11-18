"""Extract the verified disc overlay and build a private code-generation ELF.

The running prototype still loads the unmodified original ELF and ISO. This
additional ELF is only input to the static recompiler, never distributed.
"""
from pathlib import Path
import argparse, hashlib, json, os, shutil, struct, subprocess

PORT=Path(__file__).resolve().parents[1]

def prepare(index=0):
    spec=json.loads((PORT/f'config/overlay_{index:02}.json').read_text())
    source=PORT.parent/'Warriors Orochi (USA).iso'
    output=PORT/'local/overlays'; output.mkdir(parents=True,exist_ok=True)
    with source.open('rb') as disc:
        disc.seek(spec['iso_offset']); blob=disc.read(spec['bytes'])
    assert hashlib.sha256(blob).hexdigest()==spec['sha256'],'Overlay disc bytes differ'
    header=struct.unpack_from('<8I',blob)
    assert header[0]==0x336f574d and header[2]==int(spec['base'],0) and header[3]<=len(blob)
    (output/f'ovl_{index:02}.bin').write_bytes(blob)
    original=(PORT/'local/SLUS_216.62').read_bytes()
    profile=json.loads((PORT/'config/analysis.json').read_text())
    assert hashlib.sha256(original).hexdigest()==profile['elf_sha256']
    elf=bytearray(original)
    phoff,shoff=struct.unpack_from('<II',elf,28)
    phsize,phnum,shsize,shnum=struct.unpack_from('<HHHH',elf,42)
    assert phsize==32 and shsize==40 and phoff+(phnum+1)*phsize<=0x800
    sections=original[shoff:shoff+shnum*shsize]
    elf.extend(b'\0'*((-len(elf))%2048)); offset=len(elf); elf.extend(blob)
    struct.pack_into('<8I',elf,phoff+phnum*phsize,1,offset,header[2],header[2],len(blob),len(blob)+header[5],7,0x800)
    struct.pack_into('<H',elf,44,phnum+1)
    elf.extend(b'\0'*((-len(elf))%16)); new_shoff=len(elf)
    elf.extend(sections)
    elf.extend(struct.pack('<10I',0,1,6,header[2],offset,len(blob),0,0,16,0))
    struct.pack_into('<I',elf,32,new_shoff); struct.pack_into('<H',elf,48,shnum+1)
    (output/f'SLUS-with-overlay-{index:02}.elf').write_bytes(elf)
    print('Verified overlay extracted; private code-generation ELF prepared.',flush=True)
    return output,spec

def analyze(output,spec,index=0):
    paths=json.loads((PORT/'local/analysis_tools_paths.json').read_text())
    scripts=PORT/'local/ghidra-scripts'; scripts.mkdir(parents=True,exist_ok=True)
    for name in ['SeedOrochiFunctions.java','ExportOrochiFunctions.java']:
        shutil.copy2(PORT/'tools/ghidra'/name,scripts/name)
    project=PORT/'local/ghidra-overlays';project.mkdir(exist_ok=True)
    env=os.environ.copy();env['JAVA_HOME']=paths['java_home']
    env['PATH']=str(Path(paths['java_home'])/'bin')+os.pathsep+env['PATH']
    env['JAVA_TOOL_OPTIONS']=f'-Duser.home={(PORT/"local/ghidra-home").as_posix()} -Djava.io.tmpdir={(PORT/"local/tmp").as_posix()}'
    seeds=spec.get('entries',[spec.get('entry_reached')])
    args=[str(Path(paths['ghidra'])/'support/analyzeHeadless.bat'),str(project),f'OrochiOverlay{index:02}',
          '-import',str(output/f'ovl_{index:02}.bin'),'-overwrite','-loader','BinaryLoader',
          '-loader-baseAddr',spec['base'],'-processor','r5900:LE:32:default',
          '-scriptPath',str(scripts),'-preScript','SeedOrochiFunctions.java',*seeds,
          '-postScript','ExportOrochiFunctions.java',str(output/f'ovl_{index:02}.ranges.csv'),
          hex(int(spec['base'],0)+spec.get('code_offset',0x480)),spec.get('code_end',hex(int(spec['base'],0)+spec['bytes'])),
          '-analysisTimeoutPerFile','180','-max-cpu','4']
    (PORT/'logs').mkdir(exist_ok=True)
    print('Analysing overlay with Ghidra; see logs/ghidra-overlay.log',flush=True)
    with (PORT/'logs/ghidra-overlay.log').open('w',encoding='utf8') as log:
        result=subprocess.run(args,env=env,cwd=PORT,stdout=log,stderr=subprocess.STDOUT)
    print((PORT/'logs/ghidra-overlay.log').read_text(encoding='utf8',errors='replace')[-4500:])
    if result.returncode:raise SystemExit(result.returncode)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--prepare-only',action='store_true')
    parser.add_argument('--index',type=int,default=0)
    args=parser.parse_args();output,spec=prepare(args.index)
    if not args.prepare_only:analyze(output,spec,args.index)
