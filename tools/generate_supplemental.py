"""Recompile verified late-discovered functions without regenerating the game."""
from pathlib import Path
import csv, hashlib, json, re, struct, subprocess

PORT=Path(__file__).resolve().parents[1]

def overlay_module(index):
    """Recompile a whole overlay that shares its load address with overlay 0.

    Its code cannot live in the main generation (one ELF, one image per address),
    so it gets a private partial generation and prefixed symbols; the loader
    (src/native_overlay.cpp) swaps the function table when the module is loaded.
    """
    spec=json.loads((PORT/f'config/overlay_{index:02}.json').read_text())
    prefix=f'ovl{index:02}_'
    folder=PORT/'local/supplemental'/f'overlay-{index:02}';folder.mkdir(parents=True,exist_ok=True)
    combined=PORT/f'local/overlays/SLUS-with-overlay-{index:02}.elf'
    if not combined.exists():
        from analyze_overlay import prepare
        prepare(index)
    rows=list(csv.DictReader((PORT/f'config/overlay_{index:02}.ranges.csv').open(encoding='utf8')))
    temporary=bytearray(combined.read_bytes());struct.pack_into('<I',temporary,24,int(rows[0]['Start'],0))
    generation_elf=folder/'entry-only.elf';generation_elf.write_bytes(temporary)
    table=folder/'range.csv'
    with table.open('w',newline='') as out:
        writer=csv.DictWriter(out,fieldnames=['Name','Start','End','Size']);writer.writeheader()
        writer.writerows(dict(row,Name=prefix+row['Name']) for row in rows)
    output=folder/'generated'
    config=folder/'partial.toml'
    config.write_text(f'''[general]
input = "{generation_elf.as_posix()}"
ghidra_output = "{table.as_posix()}"
output = "{output.as_posix()}"
single_file_output = false
low_memory_mode = true
output_worker_threads = 4
patch_syscalls = false
patch_cop0 = true
patch_cache = true
''')
    stamp=json.dumps(dict(sha256=spec['sha256'],ranges=hashlib.sha256(table.read_bytes()).hexdigest()),sort_keys=True)
    stamp_file=folder/'generation-stamp.json'
    if not (output/'register_functions.cpp').exists() or not stamp_file.exists() or stamp_file.read_text()!=stamp:
        with (folder/'generate.log').open('w') as log:
            subprocess.run([str(PORT/'build-tools/ps2xRecomp/Release/ps2_recomp.exe'),str(config)],stdout=log,stderr=subprocess.STDOUT,check=True)
        stamp_file.write_text(stamp)
    generated=PORT/'generated'
    # zzz_ keeps these after every other source: Unity batches of the existing
    # generated files stay as they are, so adding an overlay is not a full rebuild.
    for source in output.glob(prefix+'*.cpp'):
        target=generated/('zzz_'+source.name)
        if not target.exists() or target.read_bytes()!=source.read_bytes():target.write_bytes(source.read_bytes())
    text=(output/'register_functions.cpp').read_text()
    pairs=[(int(address,16),symbol) for symbol,address in re.findall(r'= ('+prefix+r'\w+); // 0x([0-9a-fA-F]+)',text)]
    symbols=sorted({symbol for _,symbol in pairs})
    assert len(symbols)==len(rows),(len(symbols),len(rows))
    assert all((generated/f'zzz_{symbol}.cpp').exists() for symbol in symbols)
    return [f'void {symbol}(uint8_t *,R5900Context *,PS2Runtime *);' for symbol in symbols],pairs

def ensure():
    spec=json.loads((PORT/'config/analysis.json').read_text())
    elf=PORT/'local/SLUS_216.62';data=elf.read_bytes()
    assert hashlib.sha256(data).hexdigest()==spec['elf_sha256']
    # Core functions are checked against the ELF at startup (ranges); overlay
    # functions are covered by the whole-overlay fingerprint the loader checks
    # (src/native_overlay.cpp), so they are generated from the private
    # core+overlay ELF and registered without a range.
    work=[(item,data,start-0x100000+0x800,True) for item in spec.get('traced_functions',[])
          for start in [int(item['start'],0)]]
    if spec.get('overlay_traced_functions'):
        overlay=json.loads((PORT/'config/overlay_00.json').read_text())
        combined=PORT/'local/overlays/SLUS-with-overlay-00.elf'
        if not combined.exists():
            from analyze_overlay import prepare
            prepare()
        merged=combined.read_bytes();base=int(overlay['base'],0)
        # The overlay's PT_LOAD entry (added by analyze_overlay.prepare) gives its file offset.
        phoff=struct.unpack_from('<I',merged,28)[0];phsize,phnum=struct.unpack_from('<HH',merged,42)
        offset=None
        for i in range(phnum):
            kind,fileOffset,vaddr,_,fileSize=struct.unpack_from('<5I',merged,phoff+i*phsize)
            if kind==1 and vaddr==base and fileSize==overlay['bytes']:offset=fileOffset
        assert offset is not None,'overlay PT_LOAD entry missing'
        work+=[(item,merged,int(item['start'],0)-base+offset,False) for item in spec['overlay_traced_functions']]
    declarations=[];entries=[];ranges=[]
    for item,data,first,checked in work:
        start,end=int(item['start'],0),int(item['end'],0)
        body=data[first:first+end-start]
        assert hashlib.sha256(body).hexdigest()==item['sha256']
        folder=PORT/'local/supplemental'/f'{start:x}';folder.mkdir(parents=True,exist_ok=True)
        # Registration emission requires an ELF entry inside this partial map.
        # Change only the private generation copy's entry metadata; the runtime
        # always loads the untouched original ELF.
        temporary=bytearray(data);struct.pack_into('<I',temporary,24,start)
        generation_elf=folder/'entry-only.elf';generation_elf.write_bytes(temporary)
        name=item['name'];symbol=f'{name}_0x{start:x}'
        table=folder/'range.csv'
        with table.open('w',newline='') as out:
            writer=csv.writer(out);writer.writerow(['Name','Start','End','Size']);writer.writerow([name,hex(start),hex(end),end-start])
        output=folder/'generated'
        config=folder/'partial.toml'
        config.write_text(f'''[general]
input = "{generation_elf.as_posix()}"
ghidra_output = "{table.as_posix()}"
output = "{output.as_posix()}"
single_file_output = false
low_memory_mode = true
output_worker_threads = 1
patch_syscalls = false
patch_cop0 = true
patch_cache = true
''')
        source=output/(symbol+'.cpp')
        stamp=json.dumps(dict(name=name,start=start,end=end,sha256=item['sha256']),sort_keys=True)
        stamp_file=folder/'generation-stamp.json'
        if not source.exists() or not (output/'register_functions.cpp').exists() or not stamp_file.exists() or stamp_file.read_text()!=stamp:
            with (folder/'generate.log').open('w') as log:
                subprocess.run([str(PORT/'build-tools/ps2xRecomp/Release/ps2_recomp.exe'),str(config)],stdout=log,stderr=subprocess.STDOUT,check=True)
            stamp_file.write_text(stamp)
        assert source.exists(),(folder/'generate.log').read_text()
        generated=PORT/'generated';generated.mkdir(exist_ok=True)
        normal=generated/source.name
        target=generated/f'zz_supplemental_{start:x}.cpp'
        if not normal.exists() and (not target.exists() or target.read_bytes()!=source.read_bytes()):
            target.write_bytes(source.read_bytes())
        text=(output/'register_functions.cpp').read_text()
        aliases=[int(x,16) for x in re.findall(r'= '+re.escape(symbol)+r'; // 0x([0-9a-fA-F]+)',text)]
        assert start in aliases and all(start<=a<end for a in aliases)
        declarations.append(f'void {symbol}(uint8_t *,R5900Context *,PS2Runtime *);')
        entries.extend(f'    {{{hex(a)}u,{symbol}}},' for a in aliases)
        if not checked:continue
        checksum=0xcbf29ce484222325
        for byte in body:checksum=((checksum^byte)*0x100000001b3)&0xffffffffffffffff
        ranges.append(f'    {{{hex(start)}u,{hex(end)}u,0x{checksum:x}ull}},')
    module_declarations,module_entries=overlay_module(1)
    header='''#pragma once
// Derived locally from verified original code; reconstruct with tools/generate_supplemental.py.
'''+ '\n'.join(declarations)+'''
struct OrochiSupplementalEntry {uint32_t address;PS2Runtime::RecompiledFunction function;};
struct OrochiSupplementalRange {uint32_t start,end;uint64_t checksum;};
'''+ '\n'.join(module_declarations)+'''
inline const OrochiSupplementalEntry orochiSupplementalEntries[]={
'''+ '\n'.join(entries)+'''
};
inline constexpr OrochiSupplementalRange orochiSupplementalRanges[]={
'''+ '\n'.join(ranges)+'''
};
// Module 1 of the shared overlay address range; see overlay_module().
inline const OrochiSupplementalEntry orochiOverlay01Entries[]={
'''+ '\n'.join(f'    {{{hex(a)}u,{symbol}}},' for a,symbol in module_entries)+'\n};\n'
    target=PORT/'generated/orochi_supplemental_entries.h'
    if not target.exists() or target.read_text()!=header:target.write_text(header)
    print('Supplemental functions:',len(work),'registered entries:',len(entries),'ovl_01 entries:',len(module_entries))

if __name__=='__main__':ensure()
