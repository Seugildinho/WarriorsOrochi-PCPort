"""Identify the exact original disc and record the PS2 boot executable layout."""
from pathlib import Path
import hashlib, json, re, struct

PORT=Path(__file__).resolve().parents[1]
ROOT=PORT.parent
SOURCE=ROOT/'Warriors Orochi (USA).iso'
EXPECTED='1c7a1ee34b56612593625ad327f38e3b955c850320b13cfaecada326f86df85e'

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        while block:=f.read(8*1024*1024):h.update(block)
    return h.hexdigest()

def inspect_elf(data):
    assert data[:7]==b'\x7fELF\x01\x01\x01'
    h=struct.unpack_from('<HHIIIIIHHHHHH',data,16)
    typ,machine,version,entry,phoff,shoff,flags,ehsize,phsize,phnum,shsize,shnum,strndx=h
    assert machine==8
    section_raw=[struct.unpack_from('<IIIIIIIIII',data,shoff+i*shsize) for i in range(shnum)]
    names_section=section_raw[strndx]
    strings=data[names_section[4]:names_section[4]+names_section[5]]
    def name(off):return strings[off:strings.find(b'\0',off)].decode('ascii','replace')
    sections=[]
    for s in section_raw:
        sections.append(dict(name=name(s[0]),type=s[1],flags=hex(s[2]),address=hex(s[3]),
                             offset=s[4],bytes=s[5],link=s[6],entry_bytes=s[9]))
    segments=[]
    for i in range(phnum):
        s=struct.unpack_from('<IIIIIIII',data,phoff+i*phsize)
        segments.append(dict(type=hex(s[0]),offset=s[1],virtual_address=hex(s[2]),
                             physical_address=hex(s[3]),file_bytes=s[4],memory_bytes=s[5],
                             flags=hex(s[6]),alignment=s[7]))
    reginfo=next((s for s in sections if s['name']=='.reginfo'),None)
    gp=struct.unpack_from('<I',data,reginfo['offset']+20)[0] if reginfo else None
    return dict(sha256=hashlib.sha256(data).hexdigest(),bytes=len(data),elf_type=typ,
                machine='MIPS R5900 / ELF32 little-endian',entry_point=hex(entry),
                initial_gp=hex(gp) if gp is not None else None,flags=hex(flags),
                has_symbol_table_section=any(s['type']==2 for s in sections),
                has_symbol_table=any(s['type']==2 and s['bytes']>0 for s in sections),sections=sections,
                segments=segments)

def main():
    PORT.joinpath('local').mkdir(exist_ok=True)
    PORT.joinpath('relatorios').mkdir(exist_ok=True)
    actual=digest(SOURCE)
    assert actual==EXPECTED,'Use the unmodified USA SLUS-21662 disc'
    entries=json.loads(PORT.joinpath('config/disc_layout.json').read_text())
    extracted={}
    with SOURCE.open('rb') as iso:
        for item in entries:
            if item['name'] not in ('SLUS_216.62','SYSTEM.CNF','MODULES/IOPRP300.IMG','MODULES/MODULE_R.BIN'):continue
            iso.seek(item['offset']);data=iso.read(item['size'])
            path=PORT/'local'/item['name'];path.parent.mkdir(exist_ok=True,parents=True)
            path.write_bytes(data)
            extracted[item['name']]=dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),
                                          embedded_elf_offsets=[m.start() for m in re.finditer(b'\x7fELF',data)])
    boot=PORT/'local/SLUS_216.62'
    report=dict(game='Warriors Orochi',serial='SLUS-21662',region='USA',
                disc_sha256=actual,disc_bytes=SOURCE.stat().st_size,
                system_cnf=PORT.joinpath('local/SYSTEM.CNF').read_text(encoding='ascii'),
                executable=inspect_elf(boot.read_bytes()),local_files=extracted,
                status='original disc identity and ELF layout verified; see teste_boot.json for runtime status')
    PORT.joinpath('relatorios/inventario_ps2.json').write_text(json.dumps(report,indent=2),encoding='utf8')
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
