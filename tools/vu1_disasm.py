"""Disassemble VU1 microcode images captured from the running game.

Usage: python tools/vu1_disasm.py <vu1code.bin> <start_pc_hex> [...]
Reachable code is followed from each start PC through branches until the
E bit's delay slot, so the listing covers one complete microprogram.
"""
import struct
import sys

UPPER_BC = 'xyzw'
FIELDS = 'xyzw'


def dest_str(instr):
    d = (instr >> 21) & 0xF
    return ''.join(c for i, c in enumerate(FIELDS) if d & (8 >> i))


def upper(instr):
    op = instr & 0x3F
    ft, fs, fd = (instr >> 16) & 31, (instr >> 11) & 31, (instr >> 6) & 31
    d = dest_str(instr)
    flags = ''.join(f for f, b in (('I', 31), ('E', 30), ('M', 29), ('D', 28), ('T', 27)) if instr >> b & 1)
    bc = UPPER_BC[op & 3]
    names = {0x00: 'ADD', 0x04: 'SUB', 0x08: 'MADD', 0x0C: 'MSUB', 0x10: 'MAX', 0x14: 'MINI', 0x18: 'MUL'}
    if op < 0x1C:
        text = f'{names[op & 0x1C]}{bc}.{d} vf{fd}, vf{fs}, vf{ft}{bc}'
    elif op < 0x30:
        simple = {0x1C: ('MULq', 'q'), 0x1D: ('MAXi', 'i'), 0x1E: ('MULi', 'i'), 0x1F: ('MINIi', 'i'),
                  0x20: ('ADDq', 'q'), 0x21: ('MADDq', 'q'), 0x22: ('ADDi', 'i'), 0x23: ('MADDi', 'i'),
                  0x24: ('SUBq', 'q'), 0x25: ('MSUBq', 'q'), 0x26: ('SUBi', 'i'), 0x27: ('MSUBi', 'i'),
                  0x28: ('ADD', None), 0x29: ('MADD', None), 0x2A: ('MUL', None), 0x2B: ('MAX', None),
                  0x2C: ('SUB', None), 0x2D: ('MSUB', None), 0x2E: ('OPMSUB', None), 0x2F: ('MINI', None)}
        name, src = simple[op]
        text = f'{name}.{d} vf{fd}, vf{fs}, ' + (src if src else f'vf{ft}')
    elif op >= 0x3C:
        sop = ((instr >> 6) & 0x1F) << 2 | (instr & 3)
        bcs = UPPER_BC[sop & 3]
        snames = {0x00: 'ADDA', 0x04: 'SUBA', 0x08: 'MADDA', 0x0C: 'MSUBA', 0x18: 'MULA'}
        if sop < 0x10 or 0x18 <= sop < 0x1C:
            text = f'{snames[sop & 0x1C]}{bcs}.{d} ACC, vf{fs}, vf{ft}{bcs}'
        else:
            special = {0x10: 'ITOF0', 0x11: 'ITOF4', 0x12: 'ITOF12', 0x13: 'ITOF15',
                       0x14: 'FTOI0', 0x15: 'FTOI4', 0x16: 'FTOI12', 0x17: 'FTOI15',
                       0x1C: 'MULAq', 0x1D: 'ABS', 0x1E: 'MULAi', 0x1F: 'CLIP',
                       0x20: 'ADDAq', 0x21: 'MADDAq', 0x22: 'ADDAi', 0x23: 'MADDAi',
                       0x24: 'SUBAq', 0x25: 'MSUBAq', 0x26: 'SUBAi', 0x27: 'MSUBAi',
                       0x28: 'ADDA', 0x29: 'MADDA', 0x2A: 'MULA', 0x2C: 'SUBA', 0x2D: 'MSUBA',
                       0x2E: 'OPMULA', 0x2F: 'NOP'}
            name = special.get(sop, f'UPPER_SPECIAL_{sop:02x}')
            if name == 'NOP':
                text = 'NOP'
            elif name == 'CLIP':
                text = f'CLIPw.xyz vf{fs}, vf{ft}w'
            elif name.startswith(('ITOF', 'FTOI', 'ABS')):
                text = f'{name}.{d} vf{ft}, vf{fs}'
            else:
                text = f'{name}.{d} ACC, vf{fs}, vf{ft}'
    else:
        text = f'UPPER_{op:02x}'
    return text + (f'  [{flags}]' if flags else '')


def imm11(instr):
    v = instr & 0x7FF
    return v - 0x800 if v & 0x400 else v


def lower(instr, pc):
    if instr in (0, 0x8000033C):
        return 'NOP', None
    hi = (instr >> 25) & 0x7F
    it, is_, fd = (instr >> 16) & 31, (instr >> 11) & 31, (instr >> 6) & 31
    d = dest_str(instr)
    target = (pc + 8 + imm11(instr) * 8) & 0x3FFF
    if hi == 0x00: return f'LQ.{d} vf{it}, {imm11(instr)}(vi{is_})', None
    if hi == 0x01: return f'SQ.{d} vf{is_}, {imm11(instr)}(vi{it})', None
    if hi == 0x04: return f'ILW.{d} vi{it}, {imm11(instr)}(vi{is_})', None
    if hi == 0x05: return f'ISW.{d} vi{it}, {imm11(instr)}(vi{is_})', None
    if hi in (0x08, 0x09):
        imm = (instr & 0x7FF) | ((instr >> 10) & 0x7800)
        return f'{"IADDIU" if hi == 8 else "ISUBIU"} vi{it}, vi{is_}, 0x{imm:x}', None
    flagops = {0x10: 'FCEQ', 0x11: 'FCSET', 0x12: 'FCAND', 0x13: 'FCOR', 0x14: 'FSEQ', 0x15: 'FSSET',
               0x16: 'FSAND', 0x17: 'FSOR', 0x18: 'FMEQ', 0x1A: 'FMAND', 0x1B: 'FMOR', 0x1C: 'FCGET'}
    if hi in flagops:
        if hi <= 0x13: return f'{flagops[hi]} vi1, 0x{instr & 0xFFFFFF:06x}', None
        if hi <= 0x17:
            imm12 = ((instr >> 21) & 1) << 11 | (instr & 0x7FF)
            return f'{flagops[hi]} vi{it}, 0x{imm12:03x}', None
        return f'{flagops[hi]} vi{it}, vi{is_}', None
    branches = {0x20: 'B', 0x21: 'BAL', 0x28: 'IBEQ', 0x29: 'IBNE', 0x2C: 'IBLTZ', 0x2D: 'IBGTZ',
                0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}
    if hi in branches:
        name = branches[hi]
        ops = {'B': '', 'BAL': f'vi{it}, ', 'IBEQ': f'vi{it}, vi{is_}, ', 'IBNE': f'vi{it}, vi{is_}, '}.get(name, f'vi{is_}, ')
        return f'{name} {ops}0x{target:04x}', ('cond' if hi >= 0x28 else name.lower(), target)
    if hi == 0x24: return f'JR vi{is_}', ('jr', None)
    if hi == 0x25: return f'JALR vi{it}, vi{is_}', ('jr', None)
    if hi == 0x40:
        low = instr & 0x3F
        int_ops = {0x30: 'IADD', 0x31: 'ISUB', 0x34: 'IAND', 0x35: 'IOR'}
        if low in int_ops: return f'{int_ops[low]} vi{fd}, vi{is_}, vi{it}', None
        if low == 0x32:
            imm5 = ((instr >> 6) & 0x1F); imm5 = imm5 - 32 if imm5 & 16 else imm5
            return f'IADDI vi{it}, vi{is_}, {imm5}', None
        if low >= 0x3C:
            sop = (instr & 3) | ((instr >> 4) & 0x7C)
            fsf, ftf = 'xyzw'[(instr >> 21) & 3], 'xyzw'[(instr >> 23) & 3]
            special = {
                0x30: f'MOVE.{d} vf{it}, vf{is_}', 0x31: f'MR32.{d} vf{it}, vf{is_}',
                0x34: f'LQI.{d} vf{it}, (vi{is_}++)', 0x35: f'SQI.{d} vf{is_}, (vi{it}++)',
                0x36: f'LQD.{d} vf{it}, (--vi{is_})', 0x37: f'SQD.{d} vf{is_}, (--vi{it})',
                0x38: f'DIV Q, vf{is_}{fsf}, vf{it}{ftf}', 0x39: f'SQRT Q, vf{it}{ftf}',
                0x3A: f'RSQRT Q, vf{is_}{fsf}, vf{it}{ftf}', 0x3B: 'WAITQ',
                0x3C: f'MTIR vi{it}, vf{is_}{fsf}', 0x3D: f'MFIR.{d} vf{it}, vi{is_}',
                0x3E: f'ILWR.{d} vi{it}, (vi{is_})', 0x3F: f'ISWR.{d} vi{it}, (vi{is_})',
                0x40: f'RNEXT.{d} vf{it}, R', 0x41: f'RGET.{d} vf{it}, R', 0x42: f'RINIT R, vf{is_}{fsf}',
                0x43: f'RXOR R, vf{is_}{fsf}', 0x64: f'MFP.{d} vf{it}, P', 0x68: f'XTOP vi{it}',
                0x69: f'XITOP vi{it}', 0x6C: f'XGKICK vi{is_}', 0x70: f'ESADD P, vf{is_}',
                0x71: f'ERSADD P, vf{is_}', 0x72: f'ELENG P, vf{is_}', 0x73: f'ERLENG P, vf{is_}',
                0x74: f'EATANxy P, vf{is_}', 0x75: f'EATANxz P, vf{is_}', 0x76: f'ESUM P, vf{is_}',
                0x77: f'ERSQRT P, vf{is_}{fsf}', 0x78: f'ESQRT P, vf{is_}{fsf}', 0x79: f'ESIN P, vf{is_}{fsf}',
                0x7A: f'ERCPR P, vf{is_}{fsf}', 0x7B: 'WAITP', 0x7C: f'EATAN P, vf{is_}{fsf}',
                0x7D: f'EEXP P, vf{is_}{fsf}'}
            return special.get(sop, f'LOWER_SPECIAL_{sop:02x}'), None
    return f'LOWER_{hi:02x} 0x{instr:08x}', None


def disassemble(code, starts):
    seen, work, listing = set(), list(starts), {}
    while work:
        pc = work.pop()
        while pc not in seen and pc + 8 <= len(code):
            seen.add(pc)
            lo, up = struct.unpack_from('<II', code, pc)
            utext = upper(up)
            if up >> 31 & 1:
                ltext, flow = f'LOI 0x{lo:08x}', None
            else:
                ltext, flow = lower(lo, pc)
            listing[pc] = (utext, ltext)
            if flow and flow[1] is not None:
                work.append(flow[1])
            if up >> 30 & 1:  # E bit: execute the delay slot, then stop
                delay = pc + 8
                if delay not in seen:
                    seen.add(delay)
                    lo2, up2 = struct.unpack_from('<II', code, delay)
                    listing[delay] = (upper(up2), lower(lo2, delay)[0] if not (up2 >> 31 & 1) else f'LOI 0x{lo2:08x}')
                break
            if flow and flow[0] in ('b', 'jr'):
                delay = pc + 8
                if delay not in seen:
                    seen.add(delay)
                    lo2, up2 = struct.unpack_from('<II', code, delay)
                    listing[delay] = (upper(up2), lower(lo2, delay)[0] if not (up2 >> 31 & 1) else f'LOI 0x{lo2:08x}')
                break
            pc += 8
    return listing


def main():
    code = open(sys.argv[1], 'rb').read()
    starts = [int(x, 16) for x in sys.argv[2:]]
    listing = disassemble(code, starts)
    for pc in sorted(listing):
        u, l = listing[pc]
        mark = '>' if pc in starts else ' '
        print(f'{mark}{pc:04x}: {u:<40} | {l}')
    print(f'; {len(listing)} instruction pairs')


if __name__ == '__main__':
    main()
