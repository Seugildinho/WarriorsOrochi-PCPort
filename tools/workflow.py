"""Repeatable local steps; generated/game files remain private and ignored."""
from pathlib import Path
import argparse, csv, datetime, hashlib, json, os, re, shutil, struct, subprocess, sys, tomllib
from runtime_patches import synchronize

PORT=Path(__file__).resolve().parents[1]
ROOT=PORT.parent
VENDOR=PORT/'vendor/PS2Recomp'
BUILD=PORT/'build-tools'
LOGS=PORT/'logs'
PIN=json.loads((PORT/'config/toolchain.json').read_text())

def cmake():
    found=shutil.which('cmake')
    if found:return found
    candidate=Path(os.environ.get('ProgramFiles(x86)','C:/Program Files (x86)'))/'Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    if candidate.exists():return str(candidate)
    raise RuntimeError('CMake / Visual Studio C++ tools not found')

def run(args,name):
    LOGS.mkdir(exist_ok=True)
    print('Running:',name,flush=True)
    with (LOGS/f'{name}.log').open('w',encoding='utf8') as log:
        result=subprocess.run([str(x) for x in args],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
    if result.returncode:
        print((LOGS/f'{name}.log').read_text(encoding='utf8',errors='replace')[-10000:])
        raise SystemExit(result.returncode)
    print('Completed:',name,flush=True)

def check_pin():
    actual=subprocess.check_output(['git','-C',str(VENDOR),'rev-parse','HEAD'],text=True).strip()
    if actual!=PIN['ps2recomp_commit']:raise RuntimeError(f'Unexpected PS2Recomp revision: {actual}')

def apply_runtime_patches():
    # Later patches can change earlier context, invalidating individual reverse
    # checks. Verify the entire chain against the pinned sources instead.
    synchronize(PORT, VENDOR, PIN['ps2recomp_commit'], apply=True)

def configure(native=False):
    check_pin()
    if native:apply_runtime_patches()
    if native:
        from generate_supplemental import ensure
        ensure()
        import apply_game_patches
        apply_game_patches.ensure()
    args=[cmake(),'-S',VENDOR,'-B',BUILD,'-G','Visual Studio 17 2022','-A','x64',
          '-DPS2X_BUILD_TEST=OFF','-DPS2X_BUILD_STUDIO=OFF',
          '-DFETCHCONTENT_UPDATES_DISCONNECTED=ON',
          f'-DPS2X_BUILD_RUNTIME={"ON" if native else "OFF"}']
    if native:
        args += [f'-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES={(PORT/"config/attach_game.cmake").as_posix()}',
                 '-DPS2X_ENABLE_DEBUG_UI=OFF','-DPS2X_ENABLE_FFMPEG=ON',
                 # Function entry/exit tracing flushes a file for every call.
                 # Keep ordinary error/runtime logs, disable this expensive trace.
                 '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF','-DPS2X_ENABLE_RUNNER_UNITY_BUILD=ON',
                 '-DPS2X_RUNNER_UNITY_BUILD_BATCH_SIZE=16']
    else:
        args += ['-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=']
    run(args,'configure-native' if native else 'configure-tools')

def tools():
    configure()
    run([cmake(),'--build',BUILD,'--config','Release','--target','ps2_analyzer','ps2_recomp','--parallel','4'],'build-tools')

def analyze():
    check_pin()
    output=PORT/'local/analysis';output.mkdir(parents=True,exist_ok=True)
    run([BUILD/'ps2xAnalyzer/Release/ps2_analyzer.exe',PORT/'local/SLUS_216.62',output/'orochi.auto.toml'],'analyze')

def split_jump_table_cases(rows,elf,profile):
    # A discontiguous Ghidra body can contain multiple computed-jump cases in
    # one range. The recompiler must expose each proven case as a guest entry.
    targets=set()
    for table in profile.get('jump_tables',[]):
        start,end=int(table['start'],0),int(table['end'],0)
        assert 0<end-start<=4096 and start%4==end%4==0
        entries=struct.unpack_from('<'+str((end-start)//4)+'I',elf,start-0x100000+0x800)
        assert list(entries)==[int(x,0) for x in table['expected_targets']]
        branch=int(table['branch'],0)
        instruction=struct.unpack_from('<I',elf,branch-0x100000+0x800)[0]
        assert instruction&0xfc1fffff==8,'Expected original JR through the verified table'
        targets.update(entries)
    result=[]; covered=set()
    for row in rows:
        start,end=int(row['Start'],0),int(row['End'],0)
        local=sorted(target for target in targets if start<=target<end)
        assert all(target%4==0 for target in local)
        covered.update(local)
        boundaries=sorted({start,end,*local})
        for first,last in zip(boundaries,boundaries[1:]):
            name=row['Name'] if first==start else f'orochi_case_{first:x}'
            result.append(dict(Name=name,Start=f'0x{first:08X}',End=f'0x{last:08X}',Size=str(last-first)))
    assert covered==targets,'A proven switch case is outside decoded instruction ranges'
    return result

SELF_MODIFYING_PATCH=re.compile(r'^\s*\{ address = "0x[0-9a-fA-F]+", value = "0x0" \},\s*# Potential self-modifying code\n',re.M)

def generate():
    check_pin()
    raw=(PORT/'local/analysis/orochi.auto.toml').read_text(encoding='utf8')
    input_elf=PORT/'local/SLUS_216.62'
    overlay_spec=PORT/'config/overlay_00.json'
    overlay_enabled=overlay_spec.exists() and json.loads(overlay_spec.read_text()).get('enabled',False)
    if overlay_enabled:
        from analyze_overlay import prepare
        prepare()
        input_elf=PORT/'local/overlays/SLUS-with-overlay-00.elf'
    raw=re.sub(r'^input\s*=.*$',f'input = "{input_elf.as_posix()}"',raw,flags=re.M)
    raw=re.sub(r'^output\s*=.*$',f'output = "{(PORT/"generated").as_posix()}"',raw,flags=re.M)
    ranges_csv=PORT/'local/analysis/orochi.ranges.csv'
    if not ranges_csv.exists() and (PORT/'config/orochi.ranges.csv').exists():
        ranges_csv.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(PORT/'config/orochi.ranges.csv',ranges_csv)
    if not ranges_csv.exists():
        raise RuntimeError('Export orochi.ranges.csv with analyze_ghidra.py before generating; legacy spans can overlap')
    ghidra_csv=ranges_csv
    if ghidra_csv.exists():
        # The upstream exporter also emits every internal branch label as a
        # function. Keep functions and named switch cases; generated resumable
        # entry points cover anonymous internal labels without decoding the gaps.
        rows=list(csv.DictReader(ghidra_csv.open(encoding='utf8')))
        if overlay_enabled:
            overlay_ranges=PORT/'config/overlay_00.ranges.csv'
            rows+=list(csv.DictReader(overlay_ranges.open(encoding='utf8')))
            rows.sort(key=lambda row:int(row['Start'],0))
        profile=json.loads((PORT/'config/analysis.json').read_text())
        elf=(PORT/'local/SLUS_216.62').read_bytes()
        assert hashlib.sha256(elf).hexdigest()==profile['elf_sha256']
        for leaf in profile.get('verified_return_leaves',[]):
            start=int(leaf['start'],0)
            assert struct.unpack_from('<II',elf,start-0x100000+0x800)==(0x03e00008,0)
            if not any(int(row['Start'],0)==start for row in rows):
                assert not any(int(row['Start'],0)<start<int(row['End'],0) for row in rows)
                rows.append(dict(Name=f'orochi_return_leaf_{start:x}',Start=f'0x{start:08X}',End=f'0x{start+8:08X}',Size='8'))
        rows.sort(key=lambda row:int(row['Start'],0))
        for function in profile.get('traced_functions',[]):
            start,stop=int(function['start'],0),int(function['end'],0)
            assert hashlib.sha256(elf[start-0x100000+0x800:stop-0x100000+0x800]).hexdigest()==function['sha256']
            if not any(int(row['Start'],0)==start for row in rows):
                assert not any(int(row['Start'],0)<stop and start<int(row['End'],0) for row in rows)
                rows.append(dict(Name=function['name'],Start=hex(start),End=hex(stop),Size=str(stop-start)))
        rows.sort(key=lambda row:int(row['Start'],0))
        end=0
        for row in rows:
            start,stop=int(row['Start'],0),int(row['End'],0)
            assert end<=start<stop and start%4==stop%4==0, 'Invalid or overlapping instruction ranges'
            end=stop
        for name in profile.get('recompile_sdk_functions',[]):
            raw=re.sub(r'^\s*"'+re.escape(name)+r'@0x[0-9a-fA-F]+",?\s*$', '', raw, flags=re.M)
        elf=(PORT/'local/SLUS_216.62').read_bytes()
        assert hashlib.sha256(elf).hexdigest()==profile['elf_sha256']
        rows=split_jump_table_cases(rows,elf,profile)
        # This constructor table was reached by the first native execution.
        # Ghidra labels its targets but did not create functions for them.
        start=int(profile['initializer_table_start'],0)
        end=int(profile['initializer_table_end'],0)
        # Verified ELF PT_LOAD mapping: vaddr 0x100000 -> file offset 0x800.
        targets=set(struct.unpack_from('<'+str((end-start)//4)+'I',elf,start-0x100000+0x800))
        by_start={int(row['Start'],0):row for row in rows}
        assert targets.issubset(by_start), 'A constructor target is missing from the analysis map'
        functions=rows if ghidra_csv==ranges_csv else [row for row in rows if not row['Name'].startswith('entry_') or int(row['Start'],0) in targets]
        mapped=PORT/'local/analysis/orochi.functions.csv'
        with mapped.open('w',newline='',encoding='utf8') as handle:
            writer=csv.DictWriter(handle,fieldnames=['Name','Start','End','Size'])
            writer.writeheader();writer.writerows(functions)
        raw=re.sub(r'^ghidra_output\s*=.*$',f'ghidra_output = "{mapped.as_posix()}"',raw,flags=re.M)
        print(f'Ghidra functions/named entries: {len(functions)}; anonymous labels: {len(rows)-len(functions)}',flush=True)
    output=PORT/'generated'
    if output.exists() and any(output.iterdir()):
        archive=PORT/'local'/('generated-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
        assert output.resolve()==PORT.resolve()/'generated'
        assert archive.resolve().is_relative_to((PORT/'local').resolve()) and not archive.exists()
        output.rename(archive)
        print('Previous generation archived in',archive.name,flush=True)
    # The analyzer's self-modifying heuristic counts every address of the ELF's
    # single LOAD segment as code, so it nops plain stores to data (RPC arguments).
    raw=SELF_MODIFYING_PATCH.sub('',raw)
    raw=raw.replace('single_file_output = false','single_file_output = false\nlow_memory_mode = true\noutput_worker_threads = 4')
    config=PORT/'local/orochi.toml';config.write_text(raw,encoding='utf8')
    tomllib.loads(raw)
    run([BUILD/'ps2xRecomp/Release/ps2_recomp.exe',config],'generate')
    import apply_game_patches
    apply_game_patches.ensure()

def native():
    configure(True)
    run([cmake(),'--build',BUILD,'--config','Release','--target','ps2EntryRunner','orochi_stack_probe','--parallel','4'],'build-native')

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('step',choices=['tools','analyze','generate','configure-native','native'])
    step=parser.parse_args().step
    {'tools':tools,'analyze':analyze,'generate':generate,
     'configure-native':lambda:configure(True),'native':native}[step]()
