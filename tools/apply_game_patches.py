"""Apply config/game_patches.json to the generated code as runtime choices.

Each patch replaces the 16-bit immediate of one LUI/ORI/ADDIU/DADDIU. The generated line for
that instruction becomes `if (option) <patched> else <original>`, and the
option is a flag the port sets at startup (src/game_patches.cpp). Idempotent:
lines already rewritten are recognised and left alone. Run after generation."""
from pathlib import Path
import json, re, struct

PORT=Path(__file__).resolve().parents[1]
MARK='// game patch: '

def translation(word,immediate):
    # The recompiler's text for these (ps2xRecomp instruction translator).
    opcode,rs,rt=word>>26,(word>>21)&31,(word>>16)&31
    if opcode==0x0F:
        return f'SET_GPR_S32(ctx, {rt}, (int32_t)((uint32_t){immediate} << 16));'
    if opcode==0x0D:
        assert rs==rt,'ORI patches are expected to update their source register'
        return f'SET_GPR_U64(ctx, {rt}, GPR_U64(ctx, {rs}) | (uint64_t)(uint16_t){immediate});'
    if opcode==0x09:
        assert not immediate&0x8000,'ADDIU patches are expected to be positive'
        return f'SET_GPR_S32(ctx, {rt}, (int32_t)ADD32(GPR_U32(ctx, {rs}), {immediate}));'
    if opcode==0x19:
        assert not immediate&0x8000,'DADDIU patches are expected to be positive'
        return f'SET_GPR_S64(ctx, {rt}, (int64_t)GPR_S64(ctx, {rs}) + (int64_t)(int32_t){immediate});'
    raise AssertionError(f'unsupported patched instruction 0x{word:08x}')

def ensure():
    spec=json.loads((PORT/'config/game_patches.json').read_text())
    elf=(PORT/'local/SLUS_216.62').read_bytes()
    generated=PORT/'generated'
    sources=sorted(generated.glob('*.cpp'))
    changed=0
    for name,option in spec['options'].items():
        flag=f'OrochiGamePatches::{name}'
        for patch in option['patches']:
            address,original,value=(int(patch[key],0) for key in ('address','original','value'))
            # Verified ELF PT_LOAD mapping: vaddr 0x100000 -> file offset 0x800.
            assert struct.unpack_from('<I',elf,address-0x100000+0x800)[0]==original,f'0x{address:x} differs in the ELF'
            assert original>>16==value>>16,f'0x{address:x}: only the immediate may change'
            before=translation(original,original&0xFFFF)
            after=translation(value,value&0xFFFF)
            header=re.compile(rf'^    // 0x{address:x}: 0x{original:x} ',re.M)
            matches=[path for path in sources if header.search(path.read_text(encoding='utf8'))]
            assert len(matches)==1,f'0x{address:x}: expected one generated function, found {len(matches)}'
            path=matches[0];text=path.read_text(encoding='utf8')
            newline='\r\n' if b'\r\n' in path.read_bytes() else '\n'
            plain=f'    ctx->pc = 0x{address:x}u;\n    {before}\n'
            patched=(f'    ctx->pc = 0x{address:x}u;\n'
                     f'    if ({flag}) {{ {after} }} else {{ {before} }} {MARK}{name}, {patch["note"]}\n')
            if patched in text:
                continue
            assert text.count(plain)==1,f'0x{address:x}: generated line not found in {path.name}'
            text=text.replace(plain,patched)
            declaration=f'namespace OrochiGamePatches {{ extern bool {name}; }}\n'
            if declaration not in text:
                anchor='#include "ps2_stubs.h"\n'
                assert text.count(anchor)==1,f'{path.name}: include block not found'
                text=text.replace(anchor,anchor+declaration)
            path.write_text(text,encoding='utf8',newline=newline)
            changed+=1
    print(f'Game patches: {changed} generated line(s) rewritten',flush=True)

if __name__=='__main__':
    ensure()
