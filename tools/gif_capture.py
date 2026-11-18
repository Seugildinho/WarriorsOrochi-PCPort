"""Read a GIF frame capture (OROCHI_GIF_CAPTURE, src/pcsx2_gs_bridge.cpp) and
list what was drawn: one line per primitive kick with its context state.

usage: gif_capture.py <capture> [--y Y0 Y1] [--x X0 X1] [--uploads]
--y/--x keep primitives whose screen box overlaps the range (pixels);
--uploads lists the IMAGE transfers (BITBLTBUF/TRXPOS/TRXREG)."""
import argparse, struct

REGS = ['pmode','smode1','smode2','srfsh','synch1','synch2','syncv','dispfb1','display1','dispfb2','display2','bgcolor','csr']
PRIM_NAMES = ['point','line','linestrip','tri','tristrip','trifan','sprite','?']
VERTS = {0:1, 1:2, 2:2, 3:3, 4:3, 5:3, 6:2}

def frames(data):
    assert data[:4] == b'OGIF', 'not a GIF capture'
    at = 4
    while at < len(data):
        registers = dict(zip(REGS, struct.unpack_from('<13Q', data, at))); at += 13 * 8
        size, = struct.unpack_from('<I', data, at); at += 4
        yield registers, data[at:at + size]; at += size

class State:
    def __init__(self):
        self.r = {}
        self.prim = 0; self.verts = []; self.rgbaq = 0; self.uv = (0, 0); self.st = (0.0, 0.0)
        self.kicks = []; self.uploads = []

    def ctx(self):
        return (self.prim >> 9) & 1

    def reg(self, name, base, context=None):
        address = base + (self.ctx() if context is None else context)
        return self.r.get(address, 0)

    def vertex(self, x, y, z, kick):
        offset = self.reg('XYOFFSET', 0x18)
        ox, oy = offset & 0xFFFF, (offset >> 32) & 0xFFFF
        self.verts.append(((x - ox) / 16.0, (y - oy) / 16.0, z, self.uv, self.st, self.rgbaq))
        kind = self.prim & 7
        need = VERTS.get(kind, 1)
        if len(self.verts) >= need:
            if kick:
                self.emit(self.verts[-need:])
            if kind in (2, 4):          # strips keep the last vertices
                self.verts = self.verts[-(need - 1):]
            elif kind == 5:             # fan keeps the first and last
                self.verts = [self.verts[0], self.verts[-1]]
            else:
                self.verts = []

    def emit(self, verts):
        tex0 = self.reg('TEX0', 0x06); frame = self.reg('FRAME', 0x4C)
        alpha = self.reg('ALPHA', 0x42); test = self.reg('TEST', 0x47); scissor = self.reg('SCISSOR', 0x40)
        clamp = self.reg('CLAMP', 0x08); tex1 = self.reg('TEX1', 0x14)
        xs = [v[0] for v in verts]; ys = [v[1] for v in verts]
        self.kicks.append(dict(
            prim=self.prim, kind=PRIM_NAMES[self.prim & 7], box=(min(xs), min(ys), max(xs), max(ys)), verts=verts,
            tme=(self.prim >> 4) & 1, abe=(self.prim >> 6) & 1, fst=(self.prim >> 8) & 1,
            fbp=(frame & 0x1FF) * 32, fbw=((frame >> 16) & 0x3F) * 64, fpsm=(frame >> 24) & 0x3F,
            fbmsk=frame >> 32,
            tbp=tex0 & 0x3FFF, tbw=((tex0 >> 14) & 0x3F) * 64, tpsm=(tex0 >> 20) & 0x3F,
            tw=1 << ((tex0 >> 26) & 0xF), th=1 << ((tex0 >> 30) & 0xF), tcc=(tex0 >> 34) & 1, tfx=(tex0 >> 35) & 3,
            cbp=(tex0 >> 37) & 0x3FFF, alpha=alpha, test=test, scissor=scissor, clamp=clamp,
            filt=(tex1 >> 5) & 1))

def walk(stream, state):
    at = 0
    while at + 16 <= len(stream):
        lo, hi = struct.unpack_from('<QQ', stream, at); at += 16
        nloop = lo & 0x7FFF; pre = (lo >> 46) & 1; prim = (lo >> 47) & 0x7FF
        flg = (lo >> 58) & 3; nreg = (lo >> 60) & 0xF or 16
        regs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
        if pre and flg == 0:
            state.prim = prim; state.verts = []
        if flg == 0:
            for _ in range(nloop):
                for reg in regs:
                    a, b = struct.unpack_from('<QQ', stream, at); at += 16
                    packed(state, reg, a, b)
        elif flg == 1:
            words = nloop * nreg
            for index in range(words):
                value, = struct.unpack_from('<Q', stream, at); at += 8
                ad(state, regs[index % nreg], value)
            if words & 1: at += 8
        else:
            state.uploads.append(dict(bitbltbuf=state.r.get(0x50, 0), trxpos=state.r.get(0x51, 0),
                                      trxreg=state.r.get(0x52, 0), bytes=nloop * 16))
            at += nloop * 16

def packed(state, reg, a, b):
    if reg == 0x0: state.prim = a & 0x7FF; state.verts = []
    elif reg == 0x1: state.rgbaq = (a & 0xFF, (a >> 32) & 0xFF, b & 0xFF, (b >> 32) & 0xFF)
    elif reg == 0x2: state.st = struct.unpack('<ff', struct.pack('<Q', a))
    elif reg == 0x3: state.uv = ((a & 0x3FFF) / 16.0, ((a >> 32) & 0x3FFF) / 16.0)
    elif reg in (0x4, 0xC):
        state.vertex(a & 0xFFFF, (a >> 32) & 0xFFFF, (b >> 4) & 0xFFFFFF, reg == 0x4 and not (b >> 47) & 1)
    elif reg in (0x5, 0xD):
        state.vertex(a & 0xFFFF, (a >> 32) & 0xFFFF, b & 0xFFFFFFFF, reg == 0x5 and not (b >> 47) & 1)
    elif reg == 0xE: ad(state, b & 0xFF, a)
    elif reg in (0x6, 0x7, 0x8, 0x9): state.r[reg] = a

def ad(state, address, value):
    if address == 0x00: state.prim = value & 0x7FF; state.verts = []
    elif address == 0x01: state.rgbaq = (value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF, (value >> 24) & 0xFF)
    elif address == 0x02: state.st = struct.unpack('<ff', struct.pack('<Q', value))
    elif address == 0x03: state.uv = ((value & 0x3FFF) / 16.0, ((value >> 16) & 0x3FFF) / 16.0)
    elif address in (0x04, 0x0C): state.vertex(value & 0xFFFF, (value >> 16) & 0xFFFF, (value >> 32) & 0xFFFFFF, address == 0x04)
    elif address in (0x05, 0x0D): state.vertex(value & 0xFFFF, (value >> 16) & 0xFFFF, value >> 32, address == 0x05)
    else: state.r[address] = value

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('capture'); parser.add_argument('--y', nargs=2, type=float); parser.add_argument('--x', nargs=2, type=float)
    parser.add_argument('--uploads', action='store_true')
    args = parser.parse_args()
    data = open(args.capture, 'rb').read()
    state = State()
    for index, (registers, stream) in enumerate(frames(data)):
        before = len(state.kicks)
        walk(stream, state)
        print(f'== vsync {index}: {len(stream)} bytes, {len(state.kicks) - before} kicks; ' +
              ' '.join(f'{k}={v:x}' for k, v in registers.items() if k in ('pmode','smode2','dispfb1','display1','dispfb2','display2')))
    if args.uploads:
        for u in state.uploads:
            b = u['bitbltbuf']; p = u['trxpos']; r = u['trxreg']
            print(f"upload dbp={(b>>32)&0x3FFF:#x} dbw={((b>>48)&0x3F)*64} dpsm={(b>>56)&0x3F:#x} at=({(p>>32)&0x7FF},{(p>>48)&0x7FF}) size={r&0xFFF}x{(r>>32)&0xFFF} bytes={u['bytes']}")
    for k in state.kicks:
        x0, y0, x1, y1 = k['box']
        if args.y and (y1 < args.y[0] or y0 > args.y[1]): continue
        if args.x and (x1 < args.x[0] or x0 > args.x[1]): continue
        uv = ' '.join(f"({v[3][0]:.1f},{v[3][1]:.1f})" if k['fst'] else f"(s{v[4][0]:.3f},t{v[4][1]:.3f})" for v in k['verts']) if k['tme'] else ''
        print(f"{k['kind']:8} box=({x0:.2f},{y0:.2f})-({x1:.2f},{y1:.2f}) fb={k['fbp']:#x}/{k['fbw']} psm={k['fpsm']:#x} "
              f"tex={'%#x/%d psm=%#x %dx%d cbp=%#x filt=%d' % (k['tbp'], k['tbw'], k['tpsm'], k['tw'], k['th'], k['cbp'], k['filt']) if k['tme'] else '-'} "
              f"abe={k['abe']} alpha={k['alpha']:#x} rgba={k['verts'][-1][5]} {uv}")

if __name__ == '__main__':
    main()
