"""Translate hot VU1 microprograms ahead of time.

Reads config/vu1_programs.json, the private code images in local/vu1/ and
the interpreter's own decode (exported by orochi_vu1_dump), then writes C++
that reproduces VU1Interpreter::run() for each listed entry point:

* execUpper/execLower are still the semantics of every instruction;
* stalls, cycle counts and VF/VI/ACC commit points are resolved here by
  simulating run()'s timing model, instead of being scanned every cycle;
* blocks are specialised per pipeline state at entry (a loop head usually
  needs two versions: first entry and steady state).

usage: generate_vu1.py <orochi_vu1_dump.exe> <output_dir>
"""
from pathlib import Path
import copy
import re
import json
import subprocess
import sys

PORT = Path(__file__).resolve().parents[1]
MAX_VERSIONS_PER_PC = 24
MAX_STALL = 64


def fnv(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def lanes_of(mask):
    # laneForComponent(c) == 8 >> c  (x = 8, y = 4, z = 2, w = 1)
    return [c for c in range(4) if mask & (8 >> c)]


class Unsupported(Exception):
    pass


class State:
    """Timing state at a cycle, relative to the start of a block version."""

    def __init__(self):
        self.vf_ready = [[0] * 4 for _ in range(32)]
        self.vi_ready = [0] * 16
        self.acc_ready = [0] * 4
        self.fdiv_ready = None      # ready cycle while a DIV/SQRT/RSQRT result is pending
        self.efu_ready = []         # ready cycles of pending EFU results
        self.efu_resource = 0
        self.xgkick = False
        # Pending register writes held in generated temporaries:
        # (kind, reg, lanes, ready, seq, slot)
        self.pending = []
        self.vf_latest = {}         # (reg, lane) -> seq
        self.vi_latest = {}         # reg -> seq
        self.acc_latest = {}        # lane -> seq
        self.seq = 0
        self.ebit = False           # previous pair had the E bit: this pair ends the program
        # Ready cycles of entries the instruction itself queued in the
        # interpreter's flag/Q/P/store pipelines; commitReadyPipelines() is
        # emitted only at those cycle boundaries.
        self.queue_ready = []
        # (return pc, link VI) for BAL subroutines being executed; JR pops it.
        self.call_stack = ()
        # Branch whose delay slot is the next pair: (op, target, link VI, pc).
        self.pending_branch = None

    def key(self, base):
        """Canonical description relative to `base` for block versioning."""
        rel = lambda c: max(0, c - base)
        live_seqs = sorted({p[4] for p in self.pending})
        renum = {s: i for i, s in enumerate(live_seqs)}
        pend = tuple(sorted((p[0], p[1], p[2], rel(p[3]), renum[p[4]], p[5]) for p in self.pending))
        latest = []
        for (reg, lane), s in sorted(self.vf_latest.items()):
            if s in renum:
                latest.append(('vf', reg, lane, renum[s]))
        for reg, s in sorted(self.vi_latest.items()):
            if s in renum:
                latest.append(('vi', reg, renum[s]))
        for lane, s in sorted(self.acc_latest.items()):
            if s in renum:
                latest.append(('acc', lane, renum[s]))
        return (tuple(tuple(rel(c) for c in r) for r in self.vf_ready),
                tuple(rel(c) for c in self.vi_ready), tuple(rel(c) for c in self.acc_ready),
                None if self.fdiv_ready is None or self.fdiv_ready <= base else rel(self.fdiv_ready),
                tuple(sorted(rel(c) for c in self.efu_ready if c > base)), rel(self.efu_resource),
                self.xgkick, pend, tuple(latest), self.ebit,
                tuple(sorted(rel(c) for c in self.queue_ready if c > base)), self.call_stack,
                self.pending_branch)

    def horizon(self, base):
        """Cycles after `base` until every queued timing value has passed."""
        values = [c for r in self.vf_ready for c in r] + self.vi_ready + self.acc_ready + \
                 [p[3] for p in self.pending] + self.efu_ready + [self.efu_resource] + self.queue_ready
        if self.fdiv_ready is not None:
            values.append(self.fdiv_ready)
        return max([0] + [c - base for c in values])

    def rebased(self, base):
        """Copy of this state with cycles shifted so `base` becomes 0."""
        s = copy.deepcopy(self)
        sh = lambda c: max(0, c - base)
        s.vf_ready = [[sh(c) for c in r] for r in s.vf_ready]
        s.vi_ready = [sh(c) for c in s.vi_ready]
        s.acc_ready = [sh(c) for c in s.acc_ready]
        s.fdiv_ready = None if s.fdiv_ready is None or s.fdiv_ready <= base else s.fdiv_ready - base
        s.efu_ready = [c - base for c in s.efu_ready if c > base]
        s.efu_resource = sh(s.efu_resource)
        s.pending = [(p[0], p[1], p[2], p[3] - base, p[4], p[5]) for p in s.pending]
        s.queue_ready = [c - base for c in s.queue_ready if c > base]
        return s


class Generator:
    def __init__(self, decode, header, pc_mask=0x3FFF):
        self.decode = decode
        self.pc_mask = pc_mask      # microAddressMask(): VU1 0x3FFF, VU0 0x0FFF
        # VU0 microprograms run with a 4096-cycle VCALLMS budget (the runtime's
        # safety net; measured programs use < 250). Every VU0 block entry checks
        # it so a runaway loop still stops.
        self.budget_guard = pc_mask == 0x0FFF
        self.P = header['pipelines']
        self.acc_forward = header['accForwardLatency']
        self.versions = {}          # (pc, key) -> label
        self.version_count = {}
        self.work = []
        self.out = []
        self.max_vf_slots = 0
        self.max_vi_slots = 0
        self.max_acc_slots = 0
        self.max_flag_slots = 0
        self.max_store_slots = 0
        self.zero_reset = False     # the pair may leave VF0/VI0 modified in place
        # Direct writes: when set, the pair's inline upper/lower writes its
        # result straight into the pipeline temporary instead of writing the
        # architectural register in place and restoring it afterwards.
        self.up_target = None
        self.lo_target = None
        self.pair_flags = []        # flag entries issued by the pair being emitted

    # ----------------------------------------------------------------- utils
    def label_for(self, pc, state, base):
        key = (pc, state.key(base))
        if key not in self.versions:
            n = self.version_count.get(pc, 0)
            if n >= MAX_VERSIONS_PER_PC:
                raise Unsupported(f'too many timing versions at pc 0x{pc:04x}')
            self.version_count[pc] = n + 1
            label = f'L_{pc:04x}_{n}'
            self.versions[key] = label
            self.work.append((label, pc, state.rebased(base)))
        return self.versions[key]

    def emit(self, line):
        # VF/VI/ACC live in locals of the translated function (loadRegs at
        # entry, storeRegs before anything that reads vu.m_state registers).
        line = (line.replace('vu.m_state.vf[', 'VF[').replace('vu.m_state.vi[', 'VI[')
                .replace('vu.m_state.acc', 'ACC'))
        # The VU cycle counter is the local `cycle`; vu.m_cycle is refreshed
        # before every interpreter call that reads it (pipeline commits and
        # queues, XGKICK start, the epilogue).
        line = (line.replace('++vu.m_cycle;', '++cycle;').replace('vu.m_cycle - entryCycle', 'cycle - entryCycle')
                .replace('vu.commitReadyPipelines();', 'vu.m_cycle = cycle; commitReadyPipelinesInline(vu);')
                .replace('vu.startXgkick(', 'vu.m_cycle = cycle; vu.startXgkick(')
                .replace('vuDiv(vu,', 'vu.m_cycle = cycle; vuDiv(vu,')
                .replace('storeRegs(vu, VF, VI, ACC);',
                         'vu.m_cycle = cycle; storeRegs(vu, VF, VI, ACC); storeBranchBackup(vu, bkr, bkv);')
                .replace('loadRegs(vu, VF, VI, ACC);', 'loadRegs(vu, VF, VI, ACC); loadBranchBackup(vu, bkr, bkv);'))
        # The branch VI backup (readBranchVi/recordViWriteForBranch) lives in
        # the locals bkr (register, 0 = none) and bkv, so the compiler resolves
        # it within straight-line code; vu holds it only across calls/exits.
        line = (line.replace('vu.m_viBranchBackupValid = false;', 'bkr = 0;')
                .replace('vu.recordViWriteForBranch(', 'recordBranchVi(bkr, bkv, ')
                .replace('branchVi(vu, VI, ', 'branchVi(bkr, bkv, VI, '))
        assert 'vu.m_cycle' not in line.replace('vu.m_cycle = cycle;', ''), line
        self.out.append(line)

    def alloc_slot(self, state, kind):
        used = {p[5] for p in state.pending if p[0] == kind}
        slot = 0
        while slot in used:
            slot += 1
        if kind == 'vf':
            self.max_vf_slots = max(self.max_vf_slots, slot + 1)
        elif kind == 'vi':
            self.max_vi_slots = max(self.max_vi_slots, slot + 1)
        elif kind == 'flag':
            self.max_flag_slots = max(self.max_flag_slots, slot + 1)
        elif kind == 'store':
            self.max_store_slots = max(self.max_store_slots, slot + 1)
        else:
            self.max_acc_slots = max(self.max_acc_slots, slot + 1)
        return slot

    # Static flag pipeline (FlagPipelineEntry): bits of what an entry writes.
    WR_MAC, WR_STATUS, WR_STICKY, WR_CLIP = 1, 2, 4, 8

    def queue_flag(self, state, cycle, writes):
        """Queue a flag entry that becomes visible kFmacLatency (4) cycles later."""
        state.seq += 1
        slot = self.alloc_slot(state, 'flag')
        entry = ('flag', writes, 0, cycle + 4, state.seq, slot)
        state.pending.append(entry)
        self.pair_flags.append(state.seq)
        return slot

    def drop_same_cycle_writes(self, state, mask):
        """FSSET/FCSET cancel the status/clip write of entries issued in the same cycle."""
        updated = []
        for p in state.pending:
            if p[0] == 'flag' and p[4] in self.pair_flags and (p[1] & mask):
                p = (p[0], p[1] & ~mask, p[2], p[3], p[4], p[5])
            updated.append(p)
        state.pending = updated

    def commit_flags(self, entries):
        for _, writes, _, _, _, s in sorted(entries, key=lambda p: p[4]):
            if writes & self.WR_MAC:
                self.emit(f'    vu.m_state.mac = tmac[{s}];')
            if writes & self.WR_STATUS:
                self.emit(f'    {{ const uint32_t cur = tstat[{s}] & 0xFu; '
                          f'vu.m_state.status = (vu.m_state.status & 0xFF0u) | cur | ((cur | textra[{s}]) << 6); }}')
            if writes & self.WR_STICKY:
                self.emit(f'    vu.m_state.status = (vu.m_state.status & 0x03Fu) | (tstat[{s}] & 0xFC0u);')
            if writes & self.WR_CLIP:
                self.emit(f'    vu.m_state.clip = tclip[{s}];')

    # ------------------------------------------------------- cycle boundary
    def advance(self, state, cycle):
        """advanceOneCycle(): ++m_cycle, commit everything ready, PATH1 progress."""
        cycle += 1
        self.emit('    ++vu.m_cycle;')
        ready = [p for p in state.pending if p[3] <= cycle]
        # commitReadyPipelines() order: flags, then Q/P (FDIV also writes the
        # status DI bits), stores, VF, VI, ACC. Flags are static temporaries;
        # Q/P and stores stay in the interpreter's own queues.
        self.commit_flags([p for p in ready if p[0] == 'flag'])
        if any(c <= cycle for c in state.queue_ready):
            self.emit('    vu.commitReadyPipelines();')
            state.queue_ready = [c for c in state.queue_ready if c > cycle]
        for _, _, mask, _, _, s in sorted((p for p in ready if p[0] == 'store'), key=lambda p: p[4]):
            if mask == 0xF:
                lanes = f'std::memcpy(vu.m_activeVuData + tsa[{s}], tsw[{s}], 16);'
            else:
                lanes = ' '.join(f'std::memcpy(vu.m_activeVuData + tsa[{s}] + {4 * c}u, &tsw[{s}][{c}], 4);' for c in lanes_of(mask))
            self.emit(f'    if (tsv[{s}] && vu.m_activeVuData && tsa[{s}] + 16u <= vu.m_activeVuDataSize) {{ {lanes} }}')
        # run() commits VF, then VI, then ACC pipelines.
        for kind in ('vf', 'vi', 'acc'):
            for p in sorted((p for p in ready if p[0] == kind), key=lambda p: p[4]):
                _, reg, lanes, _, seq, slot = p
                if kind == 'vf':
                    latest = [c for c in lanes_of(lanes) if state.vf_latest.get((reg, c)) == seq]
                    if len(latest) == 4:
                        self.emit(f'    std::memcpy(vu.m_state.vf[{reg}], tvf[{slot}], 16);')
                    elif latest:
                        self.emit(f'    commitLanes<{sum(8 >> c for c in latest)}u>(vu.m_state.vf[{reg}], tvf[{slot}]);')
                elif kind == 'vi':
                    if state.vi_latest.get(reg) == seq:
                        self.emit(f'    vu.m_state.vi[{reg}] = static_cast<int16_t>(tvi[{slot}]);')
                else:
                    latest = [c for c in lanes_of(lanes) if state.acc_latest.get(c) == seq]
                    if latest:
                        self.emit(f'    commitLanes<{sum(8 >> c for c in latest)}u>(vu.m_state.acc, tacc[{slot}]);')
        state.pending = [p for p in state.pending if p[3] > cycle]
        if state.fdiv_ready is not None and state.fdiv_ready <= cycle:
            state.fdiv_ready = None
        state.efu_ready = [c for c in state.efu_ready if c > cycle]
        if state.xgkick:
            self.emit('    xgkickStep(vu);')
        return cycle

    # ------------------------------------------------------------- one pair
    def ready_cycle(self, d, state, cycle):
        ready = cycle
        for usage in (d['upperUsage'], d['lowerUsage']):
            for reg, lanes in usage['vfRead']:
                for c in lanes_of(lanes):
                    ready = max(ready, state.vf_ready[reg][c])
            for reg in range(1, 16):
                if usage['viRead'] & (1 << reg):
                    ready = max(ready, state.vi_ready[reg])
            for c in lanes_of(usage['accRead']):
                ready = max(ready, state.acc_ready[c])
        low = d['lowerUsage']
        fdiv_valid = state.fdiv_ready is not None and state.fdiv_ready > cycle
        if low['pipeline'] == self.P['fdiv'] and fdiv_valid:
            ready = max(ready, state.fdiv_ready)
        if low['pipeline'] == self.P['efu']:
            ready = max(ready, state.efu_resource)
        if low['waitQ'] and fdiv_valid:
            ready = max(ready, state.fdiv_ready)
        if low['waitP']:
            for c in state.efu_ready:
                if c > cycle:
                    ready = max(ready, c)
        if low['pipeline'] == self.P['xgkick'] and state.xgkick:
            raise Unsupported(f'PATH1 wait at XGKICK in a delay slot (internal)')
        return ready

    def queue_write(self, state, kind, reg, lanes, ready):
        state.seq += 1
        seq = state.seq
        slot = self.alloc_slot(state, kind)
        state.pending.append((kind, reg, lanes, ready, seq, slot))
        if kind == 'vf':
            for c in lanes_of(lanes):
                state.vf_latest[(reg, c)] = seq
        elif kind == 'vi':
            state.vi_latest[reg] = seq
        else:
            for c in lanes_of(lanes):
                state.acc_latest[c] = seq
        return slot

    def pair(self, pc, state, cycle):
        d = self.decode[pc]
        up, low = d['upperUsage'], d['lowerUsage']
        if up['reserved'] or (not d['iBit'] and low['reserved']):
            raise Unsupported(f'reserved instruction at 0x{pc:04x}')
        if d['dBit'] or d['tBit']:
            raise Unsupported(f'D/T bit at 0x{pc:04x}')

        ready = self.ready_cycle(d, state, cycle)
        stalls = 0
        while ready > cycle:
            cycle = self.advance(state, cycle)
            stalls += 1
            if stalls > MAX_STALL:
                raise Unsupported(f'stall too long at 0x{pc:04x}')
            ready = self.ready_cycle(d, state, cycle)

        lower, upper = d['lower'], d['upper']
        self.pair_flags = []
        self.zero_reset = False
        self.emit(f'    // 0x{pc:04x}: upper 0x{upper:08x} lower 0x{lower:08x}')
        is_branch = low['pipeline'] == self.P['branch']
        if is_branch:
            self.emit(f'    vu.m_state.pc = 0x{pc:x}u;')

        written_vi = next((r for r in range(1, 16) if low['viWrite'] & (1 << r)), 0)
        up_reg, up_lanes = up['vfWrite']
        lo_reg, lo_lanes = low['vfWrite']
        has_up = up_reg != 0
        has_lo = lo_reg != 0 and d['suppressedLowerVf'] != lo_reg
        distinct_lo = has_lo and (not has_up or lo_reg != up_reg)
        lower_reads = {reg for reg, lanes in low['vfRead'] if lanes}
        plain_lower = d['iBit'] or lower == 0 or lower == 0x8000033C or self.lower_is_inline(lower)
        # Upper result straight into its temporary: the lower must not read or
        # write that register (it would see the in-place value otherwise).
        # The temporary only receives the instruction's dest lanes, so the
        # decoded write lanes must be exactly those.
        up_dest, lo_dest = (upper >> 21) & 0xF, (lower >> 21) & 0xF
        direct_up = (has_up and up_lanes and up_lanes == up_dest and not d['upperVfShadowReg']
                     and self.upper_is_inline_fmac(upper)
                     and up_reg not in lower_reads and not (has_lo and lo_reg == up_reg) and plain_lower)
        direct_acc = (bool(up['accWrite']) and up['accWrite'] == up_dest and not d['upperVfShadowReg']
                      and self.upper_is_inline_fmac(upper))
        direct_lo = (distinct_lo and lo_lanes and lo_lanes == lo_dest and not d['iBit'] and not d['upperVfShadowReg']
                     and self.lower_is_direct_vf(lower))
        self.up_target = self.lo_target = None
        up_slot = acc_slot = lo_slot = None
        if direct_up:
            up_slot = self.queue_write(state, 'vf', up_reg, up_lanes, cycle + (up['vfLatency'] or up['latency']))
            self.up_target = f'tvf[{up_slot}]'
        if direct_lo:
            lo_slot = self.queue_write(state, 'vf', lo_reg, lo_lanes, cycle + (low['vfLatency'] or low['latency']))
            self.lo_target = f'tvf[{lo_slot}]'
        if direct_acc:
            acc_slot = self.queue_write(state, 'acc', 0, up['accWrite'], cycle + self.acc_forward)
            self.acc_target = f'tacc[{acc_slot}]'
        else:
            self.acc_target = None
        self.emit('    {')
        if written_vi:
            self.emit(f'    const int32_t oldVi = vu.m_state.vi[{written_vi}];')
        if has_up and not direct_up:
            self.emit(f'    float oldUp[4]; std::memcpy(oldUp, vu.m_state.vf[{up_reg}], 16);')
        if distinct_lo and not direct_lo:
            self.emit(f'    float oldLo[4]; std::memcpy(oldLo, vu.m_state.vf[{lo_reg}], 16);')
        if up['accWrite'] and not direct_acc:
            self.emit('    float oldAcc[4]; std::memcpy(oldAcc, vu.m_state.acc, 16);')

        if d['iBit']:
            self.emit_upper(upper, state, cycle)
            self.emit(f'    {{ uint32_t bits = 0x{lower:08x}u; float imm; std::memcpy(&imm, &bits, 4); vu.m_state.i = vu.normalizeOperand(imm); }}')
        elif d['upperVfShadowReg']:
            r = d['upperVfShadowReg']
            self.emit(f'    float shOld[4], shUp[4]; std::memcpy(shOld, vu.m_state.vf[{r}], 16);')
            self.emit_upper(upper, state, cycle)
            self.emit(f'    std::memcpy(shUp, vu.m_state.vf[{r}], 16); std::memcpy(vu.m_state.vf[{r}], shOld, 16);')
            self.emit_lower(lower, upper, state, cycle, pc)
            self.emit(f'    std::memcpy(vu.m_state.vf[{r}], shUp, 16);')
        else:
            self.emit_upper(upper, state, cycle)
            if not (lower == 0 or lower == 0x8000033C):
                self.emit_lower(lower, upper, state, cycle, pc)
        self.emit('    vu.m_viBranchBackupValid = false;')

        if direct_up:
            pass
        elif has_up and up_lanes:
            lat = up['vfLatency'] or up['latency']
            slot = self.queue_write(state, 'vf', up_reg, up_lanes, cycle + lat)
            self.emit(f'    std::memcpy(tvf[{slot}], vu.m_state.vf[{up_reg}], 16); std::memcpy(vu.m_state.vf[{up_reg}], oldUp, 16);')
        elif has_up:
            self.emit(f'    std::memcpy(vu.m_state.vf[{up_reg}], oldUp, 16);')
        if direct_lo:
            pass
        elif distinct_lo and lo_lanes:
            lat = low['vfLatency'] or low['latency']
            slot = self.queue_write(state, 'vf', lo_reg, lo_lanes, cycle + lat)
            self.emit(f'    std::memcpy(tvf[{slot}], vu.m_state.vf[{lo_reg}], 16); std::memcpy(vu.m_state.vf[{lo_reg}], oldLo, 16);')
        elif distinct_lo:
            self.emit(f'    std::memcpy(vu.m_state.vf[{lo_reg}], oldLo, 16);')
        if direct_acc:
            pass
        elif up['accWrite']:
            slot = self.queue_write(state, 'acc', 0, up['accWrite'], cycle + self.acc_forward)
            self.emit(f'    std::memcpy(tacc[{slot}], vu.m_state.acc, 16); std::memcpy(vu.m_state.acc, oldAcc, 16);')
        self.up_target = self.lo_target = self.acc_target = None
        if written_vi:
            lat = low['viLatency'] or low['latency']
            slot = self.queue_write(state, 'vi', written_vi, 0, cycle + lat)
            self.emit(f'    tvi[{slot}] = vu.m_state.vi[{written_vi}]; vu.m_state.vi[{written_vi}] = oldVi;')

        # markPairWrites()
        if lo_reg != 0 and d['suppressedLowerVf'] != lo_reg:
            lat = low['vfLatency'] or low['latency']
            for c in lanes_of(lo_lanes):
                state.vf_ready[lo_reg][c] = cycle + lat
        if up_reg != 0:
            lat = up['vfLatency'] or up['latency']
            for c in lanes_of(up_lanes):
                state.vf_ready[up_reg][c] = cycle + lat
        for reg in range(1, 16):
            if low['viWrite'] & (1 << reg):
                state.vi_ready[reg] = cycle + (low['viLatency'] or low['latency'])
        for c in lanes_of(up['accWrite']):
            state.acc_ready[c] = cycle + self.acc_forward
        if written_vi and low['delaysNextBranchRead']:
            self.emit(f'    vu.recordViWriteForBranch({written_vi}, oldVi);')
        self.emit('    }')
        # run() rewrites VF0/VI0 after every pair; only needed when this pair
        # could have written them in place (the decoder does not track reg 0).
        if self.zero_reset or (up_reg == 0 and up_lanes) or (lo_reg == 0 and lo_lanes):
            self.emit('    vu.m_state.vf[0][0] = 0.0f; vu.m_state.vf[0][1] = 0.0f; vu.m_state.vf[0][2] = 0.0f; '
                      'vu.m_state.vf[0][3] = 1.0f; vu.m_state.vi[0] = 0;')

        for delay in self.queued_entries(d):
            state.queue_ready.append(cycle + delay)
        # Scalar pipelines queued by the instruction itself.
        if not d['iBit']:
            if low['pipeline'] == self.P['fdiv']:
                state.fdiv_ready = cycle + low['latency']
            elif low['pipeline'] == self.P['efu']:
                state.efu_ready.append(cycle + low['latency'])
                state.efu_resource = cycle + max(low['latency'] - 1, 0)
            elif low['pipeline'] == self.P['xgkick']:
                state.xgkick = True
        return cycle, is_branch

    # Inline FMAC semantics, copied from execUpper (float result),
    # calculateFmacExactResult (long double classification) and
    # calculateFmacProductSticky. Keys are the upper op (or 0x100 | special
    # op for the ACC forms); values: (kind, right operand, writes ACC).
    #   kind: 'add' s+t, 'sub' s-t, 'mul' s*t, 'madd' a+s*t, 'msub' a-s*t
    #   right: 'bc' vf[ft][op&3], 'vt' vf[ft][c], 'q', 'i'
    INLINE_FMAC = {}
    for _op, _kind in ((0x00, 'add'), (0x04, 'sub'), (0x08, 'madd'), (0x0C, 'msub'), (0x18, 'mul')):
        for _bc in range(4):
            INLINE_FMAC[_op + _bc] = (_kind, 'bc', False)
            INLINE_FMAC[0x100 | (_op + _bc)] = (_kind, 'bc', True)
    for _op, _kind, _right in ((0x1C, 'mul', 'q'), (0x1E, 'mul', 'i'), (0x20, 'add', 'q'), (0x21, 'madd', 'q'),
                               (0x22, 'add', 'i'), (0x23, 'madd', 'i'), (0x24, 'sub', 'q'), (0x25, 'msub', 'q'),
                               (0x26, 'sub', 'i'), (0x27, 'msub', 'i'), (0x28, 'add', 'vt'), (0x29, 'madd', 'vt'),
                               (0x2A, 'mul', 'vt'), (0x2C, 'sub', 'vt'), (0x2D, 'msub', 'vt')):
        INLINE_FMAC[_op] = (_kind, _right, False)
        INLINE_FMAC[0x100 | _op] = (_kind, _right, True)

    def emit_upper(self, upper, state, cycle):
        """Emit execUpper(upper), inline when the operation is a plain FMAC form."""
        op = upper & 0x3F
        key = op if op < 0x30 else (0x100 | ((upper & 3) | ((upper >> 4) & 0x7C)) if op >= 0x3C else None)
        if op >= 0x3C and ((upper & 3) | ((upper >> 4) & 0x7C)) == 0x2F:
            return  # NOP: execUpper only records the instruction for flag helpers
        if op >= 0x3C and ((upper & 3) | ((upper >> 4) & 0x7C)) == 0x1F:
            self.emit_clip(upper, state, cycle)
            return
        if self.emit_nonflag_upper(upper):
            return
        spec = self.INLINE_FMAC.get(key)
        if spec is None:
            special = (upper & 3) | ((upper >> 4) & 0x7C) if op >= 0x3C else None
            if (op < 0x30 and op in self.FMAC_FLAG_OPS) or (special is not None and special in self.ACC_FLAG_SPECIALS):
                raise Unsupported(f'flag-producing upper 0x{upper:08x} is not inlined')
            self.zero_reset = True
            self.emit(f'    vu.execUpper(0x{upper:08x}u);')
            return
        kind, right, acc_dest = spec
        dest = (upper >> 21) & 0xF
        ft, fs, fd = (upper >> 16) & 31, (upper >> 11) & 31, (upper >> 6) & 31
        if fd == 0 and not acc_dest:
            self.zero_reset = True
        bc = (key & 3)
        self.emit(f'    {{ // inline FMAC 0x{upper:08x}')
        if dest:
            right_expr = {'bc': f'_mm_set1_ps(vu.m_state.vf[{ft}][{bc}])', 'vt': f'_mm_loadu_ps(vu.m_state.vf[{ft}])',
                          'q': '_mm_set1_ps(vu.m_state.q)', 'i': '_mm_set1_ps(vu.m_state.i)'}[right]
            fkind = {'add': 'FmacAdd', 'sub': 'FmacSub', 'mul': 'FmacMul', 'madd': 'FmacMadd', 'msub': 'FmacMsub'}[kind]
            if acc_dest:
                target = self.acc_target or 'vu.m_state.acc'
            else:
                target = self.up_target or f'vu.m_state.vf[{fd}]'
            # updateFmacFlags(): MAC/status from the lane flags, visible 4 cycles later.
            slot = self.queue_flag(state, cycle, self.WR_MAC | self.WR_STATUS)
            self.emit(f'    fmac<{fkind}, {dest}u>(vu.m_state.vf[{fs}], {right_expr}, vu.m_state.acc, {target}, '
                      f'tmac[{slot}], tstat[{slot}], textra[{slot}]);')
        self.emit('    }')

    def nonflag_upper(self, upper):
        """(call, target register) for execUpper's MAX/MINI/ABS/ITOF/FTOI forms."""
        op = upper & 0x3F
        dest = (upper >> 21) & 0xF
        ft, fs, fd = (upper >> 16) & 31, (upper >> 11) & 31, (upper >> 6) & 31
        src = f'vu.m_state.vf[{fs}]'
        if op < 0x3C:
            right = None
            if 0x10 <= op <= 0x17:
                right = f'_mm_set1_ps(vu.m_state.vf[{ft}][{op & 3}])'
            elif op in (0x1D, 0x1F):
                right = '_mm_set1_ps(vu.m_state.i)'
            elif op in (0x2B, 0x2F):
                right = f'_mm_loadu_ps(vu.m_state.vf[{ft}])'
            if right is None:
                return None
            fn = 'vuMax' if op in (0x10, 0x11, 0x12, 0x13, 0x1D, 0x2B) else 'vuMin'
            return (lambda out: f'{fn}<{dest}u>({src}, {right}, {out});'), fd
        special = (upper & 3) | ((upper >> 4) & 0x7C)
        if special == 0x1D:
            return (lambda out: f'vuAbs<{dest}u>({src}, {out});'), ft
        if 0x10 <= special <= 0x17:
            shift = (0, 4, 12, 15)[special & 3]
            fn = 'vuItof' if special < 0x14 else 'vuFtoi'
            return (lambda out: f'{fn}<{dest}u, {shift}>({src}, {out});'), ft
        return None

    def emit_nonflag_upper(self, upper):
        spec = self.nonflag_upper(upper)
        if spec is None:
            return False
        call, target = spec
        if target == 0:
            self.zero_reset = True
        out = self.up_target or f'vu.m_state.vf[{target}]'
        self.emit(f'    {call(out)}')
        return True

    def emit_clip(self, upper, state, cycle):
        ft, fs = (upper >> 16) & 31, (upper >> 11) & 31
        slot = self.queue_flag(state, cycle, self.WR_CLIP)
        self.emit(f'    {{ // CLIP 0x{upper:08x}')
        self.emit(f'    uint32_t wBits = 0u; std::memcpy(&wBits, &vu.m_state.vf[{ft}][3], 4);')
        self.emit('    const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;')
        self.emit('    auto beyond = [limit](float value, uint32_t signMask) { uint32_t bits = 0u; std::memcpy(&bits, &value, 4); '
                  'bits ^= signMask; int32_t ordered = 0; std::memcpy(&ordered, &bits, 4); return ordered > limit; };')
        self.emit('    uint32_t flags = 0u;')
        for c, (plus, minus) in enumerate(((0x01, 0x02), (0x04, 0x08), (0x10, 0x20))):
            self.emit(f'    if (beyond(vu.m_state.vf[{fs}][{c}], 0x00000000u)) flags |= 0x{plus:02x}u;')
            self.emit(f'    if (beyond(vu.m_state.vf[{fs}][{c}], 0x80000000u)) flags |= 0x{minus:02x}u;')
        self.emit(f'    vu.m_workingClip = ((vu.m_workingClip << 6) | (flags & 0x3Fu)) & 0xFFFFFFu; tclip[{slot}] = vu.m_workingClip; }}')

    def emit_lower(self, lower, upper, state, cycle, pc):
        hi = (lower >> 25) & 0x7F
        if hi == 0x11:      # FCSET -> queueFcset
            self.drop_same_cycle_writes(state, self.WR_CLIP)
            slot = self.queue_flag(state, cycle, self.WR_CLIP)
            self.emit(f'    vu.m_workingClip = 0x{lower & 0xFFFFFF:06x}u; tclip[{slot}] = vu.m_workingClip; // FCSET')
            return
        if hi == 0x15:      # FSSET -> queueFsset
            imm12 = (((lower >> 21) & 1) << 11) | (lower & 0x7FF)
            self.drop_same_cycle_writes(state, self.WR_STATUS)
            slot = self.queue_flag(state, cycle, self.WR_STICKY)
            self.emit(f'    tstat[{slot}] = 0x{imm12 & 0xFC0:03x}u; // FSSET')
            return
        body = self.inline_lower(lower, state, cycle, pc)
        if body is None:
            self.zero_reset = True
            self.emit('    storeRegs(vu, VF, VI, ACC);')
            self.emit(f'    vu.execLower(0x{lower:08x}u, vuData, dataSize, gs, memory, 0x{upper:08x}u);')
            self.emit('    loadRegs(vu, VF, VI, ACC);')
            return
        for line in body:
            self.emit('    ' + line)

    def upper_is_inline_fmac(self, upper):
        """Inline upper forms that write their VF/ACC result through a target expression."""
        op = upper & 0x3F
        key = op if op < 0x30 else (0x100 | ((upper & 3) | ((upper >> 4) & 0x7C)) if op >= 0x3C else None)
        return key in self.INLINE_FMAC or self.nonflag_upper(upper) is not None

    @staticmethod
    def lower_special(lower):
        if ((lower >> 25) & 0x7F) != 0x40 or (lower & 0x3F) < 0x3C:
            return None
        return (lower & 3) | ((lower >> 4) & 0x7C)

    def lower_is_direct_vf(self, lower):
        """Inline lower ops whose only VF write goes through vf_out()."""
        hi = (lower >> 25) & 0x7F
        return hi == 0x00 or self.lower_special(lower) in (0x30, 0x31, 0x34, 0x36, 0x3D)

    def lower_is_inline(self, lower):
        hi = (lower >> 25) & 0x7F
        if hi in (0x11, 0x15):
            return True
        saved = (self.zero_reset, self.lo_target)
        probe = State()
        result = self.inline_lower(lower, probe, 0, 0) is not None
        self.zero_reset, self.lo_target = saved
        return result

    # Lower operations inlined from VU1Interpreter::execLower (same order of
    # reads and writes). Stores go to static temporaries committed one cycle
    # later by advance(), like queueStore()/commitReadyPipelines().
    def queue_store(self, state, cycle, mask):
        state.seq += 1
        slot = self.alloc_slot(state, 'store')
        state.pending.append(('store', 0, mask, cycle + 1, state.seq, slot))
        return slot

    @staticmethod
    def dest_lanes(dest, target):
        if dest == 0xF:
            return [f'std::memcpy({target}, tmp, 16);']
        return [f'commitLanes<{dest}u>({target}, tmp);']

    def inline_lower(self, lower, state, cycle, pc):
        hi = (lower >> 25) & 0x7F
        ft, fs = (lower >> 16) & 31, (lower >> 11) & 31
        vit, vis, vid = (lower >> 16) & 15, (lower >> 11) & 15, (lower >> 6) & 15
        dest = (lower >> 21) & 0xF
        imm11 = lower & 0x7FF
        imm11 = imm11 - 0x800 if imm11 & 0x400 else imm11
        vi = lambda r: f'vu.m_state.vi[{r}]'
        vf = lambda r: f'vu.m_state.vf[{r}]'
        first = 0 if dest & 8 else 1 if dest & 4 else 2 if dest & 2 else 3

        vf_out = lambda r: self.lo_target or vf(r)

        def load(addr_expr, target, post=()):
            if target == 0:
                self.zero_reset = True
            if dest == 0xF:
                return ([f'{{ uint32_t addr = {addr_expr}; addr &= (dataSize - 1);',
                         f'  if (addr + 16 <= dataSize) std::memcpy({vf_out(target)}, vuData + addr, 16);']
                        + list(post) + ['}'])
            return ([f'{{ uint32_t addr = {addr_expr}; addr &= (dataSize - 1);',
                     '  if (addr + 16 <= dataSize) { float tmp[4]; std::memcpy(tmp, vuData + addr, 16);']
                    + ['    ' + l for l in self.dest_lanes(dest, vf_out(target))] + ['  }'] + list(post) + ['}'])

        def store(addr_expr, words, pre=(), post=()):
            slot = self.queue_store(state, cycle, dest)
            return (['{'] + list(pre) +
                    [f'  uint32_t addr = {addr_expr}; addr &= (dataSize - 1);',
                     f'  if (addr + 16 <= dataSize) {{ tsv[{slot}] = true; tsa[{slot}] = addr; {words(slot)} }}',
                     f'  else tsv[{slot}] = false;'] + list(post) + ['}'])

        def vf_words(reg):
            return lambda s: f'std::memcpy(tsw[{s}], {vf(reg)}, 16);'

        def vi_words(reg):
            return lambda s: (f'const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>({vi(reg)} & 0xFFFF)); '
                              f'tsw[{s}][0] = val; tsw[{s}][1] = val; tsw[{s}][2] = val; tsw[{s}][3] = val;')

        def branch(cond):
            target = (pc + 8 + imm11 * 8) & self.pc_mask
            set_ = (f'vu.m_state.branchPending = true; vu.m_state.branchTarget = 0x{target:x}u; '
                    'vu.m_state.branchDelay = 1;')
            return [set_] if cond is None else [f'if ({cond}) {{ {set_} }}']

        bvi = lambda r: f'static_cast<int16_t>(branchVi(vu, VI, {r}))'
        if hi == 0x00:      # LQ
            return load(f'(static_cast<uint32_t>(static_cast<int32_t>({vi(vis)} + ({imm11})))) * 16u', ft)
        if hi == 0x01:      # SQ
            return store(f'(static_cast<uint32_t>(static_cast<int32_t>({vi(vit)} + ({imm11})))) * 16u', vf_words(fs))
        if hi == 0x04:      # ILW
            if vit == 0:
                return []
            return [f'{{ uint32_t addr = (static_cast<uint32_t>(static_cast<int32_t>({vi(vis)} + ({imm11})))) * 16u; addr &= (dataSize - 1);',
                    f'  if (addr + 16 <= dataSize) {{ uint32_t v; std::memcpy(&v, vuData + addr + {first * 4}, 4); '
                    f'{vi(vit)} = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFF)); }} }}']
        if hi == 0x05:      # ISW
            return store(f'(static_cast<uint32_t>(static_cast<int32_t>({vi(vis)} + ({imm11})))) * 16u', vi_words(vit))
        if hi in (0x08, 0x09):  # IADDIU / ISUBIU
            imm15 = (lower & 0x7FF) | ((lower >> 10) & 0x7800)
            op = '+' if hi == 0x08 else '-'
            return [f'{vi(vit)} = static_cast<int16_t>({vi(vis)} {op} {imm15});'] if vit else []
        if hi == 0x10:      # FCEQ
            return [f'{vi(1)} = ((vu.m_state.clip & 0xFFFFFF) == 0x{lower & 0xFFFFFF:x}u) ? 1 : 0;']
        if hi == 0x12:      # FCAND
            return [f'{vi(1)} = ((vu.m_state.clip & 0x{lower & 0xFFFFFF:x}u) != 0) ? 1 : 0;']
        if hi == 0x13:      # FCOR
            return [f'{vi(1)} = ((vu.m_state.clip | 0x{lower & 0xFFFFFF:x}u) == 0xFFFFFF) ? 1 : 0;']
        if hi == 0x1A:      # FMAND
            return [f'{vi(vit)} = static_cast<int32_t>(vu.m_state.mac & static_cast<uint32_t>(static_cast<uint16_t>({vi(vis)})));'] if vit else []
        if hi == 0x1C:      # FCGET
            return [f'{vi(vit)} = static_cast<int32_t>(vu.m_state.clip & 0x0FFFu);'] if vit else []
        if hi == 0x20:      # B
            return branch(None)
        if hi == 0x21:      # BAL: link = (pc + 16) / 8, then branch
            link = [f'{vi(vit)} = {(pc + 16) // 8};'] if vit else []
            return link + branch(None)
        if hi == 0x24:      # JR: target from VI (readBranchVi), masked
            return [f'vu.m_state.branchPending = true; vu.m_state.branchTarget = '
                    f'(static_cast<uint32_t>(static_cast<uint16_t>(branchVi(vu, VI, {vis}))) * 8u) & 0x{self.pc_mask:x}u; '
                    'vu.m_state.branchDelay = 1;']
        if hi == 0x16:      # FSAND
            imm12 = (((lower >> 21) & 1) << 11) | (lower & 0x7FF)
            return [f'{vi(vit)} = static_cast<int32_t>((vu.m_state.status & 0xFFFu) & 0x{imm12:x}u);'] if vit else []
        if hi == 0x28:
            return branch(f'{bvi(vis)} == {bvi(vit)}')
        if hi == 0x29:
            return branch(f'{bvi(vis)} != {bvi(vit)}')
        if hi == 0x2C:
            return branch(f'{bvi(vis)} < 0')
        if hi == 0x2D:
            return branch(f'{bvi(vis)} > 0')
        if hi == 0x2E:
            return branch(f'{bvi(vis)} <= 0')
        if hi == 0x2F:
            return branch(f'{bvi(vis)} >= 0')
        if hi != 0x40:
            return None
        funct = lower & 0x3F
        if funct in (0x30, 0x31):   # IADD / ISUB
            op = '+' if funct == 0x30 else '-'
            return [f'{vi(vid)} = static_cast<int16_t>({vi(vis)} {op} {vi(vit)});'] if vid else []
        if funct == 0x32:           # IADDI
            imm5 = (lower >> 6) & 0x1F
            imm5 = imm5 - 32 if imm5 & 0x10 else imm5
            return [f'{vi(vit)} = static_cast<int16_t>({vi(vis)} + ({imm5}));'] if vit else []
        if funct in (0x34, 0x35):   # IAND / IOR
            op = '&' if funct == 0x34 else '|'
            return [f'{vi(vid)} = {vi(vis)} {op} {vi(vit)};'] if vid else []
        if funct < 0x3C:
            return None
        special = (lower & 3) | ((lower >> 4) & 0x7C)
        if special == 0x30:         # MOVE
            if ft == 0:
                self.zero_reset = True
            return [f'{{ float tmp[4]; std::memcpy(tmp, {vf(fs)}, 16);'] + ['  ' + l for l in self.dest_lanes(dest, vf_out(ft))] + ['}']
        if special == 0x31:         # MR32
            if ft == 0:
                self.zero_reset = True
            return ([f'{{ float tmp[4] = {{{vf(fs)}[1], {vf(fs)}[2], {vf(fs)}[3], {vf(fs)}[0]}};']
                    + ['  ' + l for l in self.dest_lanes(dest, vf_out(ft))] + ['}'])
        if special == 0x34:         # LQI
            post = [f'  {vi(vis)} = static_cast<int16_t>({vi(vis)} + 1);'] if vis else []
            return load(f'(static_cast<uint32_t>(static_cast<uint16_t>({vi(vis)}))) * 16u', ft, post)
        if special == 0x35:         # SQI
            post = [f'  {vi(vit)} = static_cast<int16_t>({vi(vit)} + 1);'] if vit else []
            return store(f'(static_cast<uint32_t>(static_cast<uint16_t>({vi(vit)}))) * 16u', vf_words(fs), post=post)
        if special == 0x36:         # LQD
            pre = [f'{vi(vis)} = static_cast<int16_t>({vi(vis)} - 1);'] if vis else []
            return pre + load(f'(static_cast<uint32_t>(static_cast<uint16_t>({vi(vis)}))) * 16u', ft)
        if special == 0x37:         # SQD
            pre = [f'  {vi(vit)} = static_cast<int16_t>({vi(vit)} - 1);'] if vit else []
            return store(f'(static_cast<uint32_t>(static_cast<uint16_t>({vi(vit)}))) * 16u', vf_words(fs), pre=pre)
        if special == 0x3C:         # MTIR
            comp = (lower >> 21) & 3
            return ([f'{{ uint32_t fval; std::memcpy(&fval, &{vf(fs)}[{comp}], 4); '
                     f'{vi(vit)} = static_cast<int32_t>(static_cast<int16_t>(fval & 0xFFFF)); }}'] if vit else [])
        if special == 0x3D:         # MFIR
            if ft == 0:
                self.zero_reset = True
            return ([f'{{ float tmp[4]; const int32_t val = static_cast<int32_t>(static_cast<int16_t>({vi(vis)} & 0xFFFF)); '
                     'std::memcpy(&tmp[0], &val, 4); tmp[1] = tmp[0]; tmp[2] = tmp[0]; tmp[3] = tmp[0];']
                    + ['  ' + l for l in self.dest_lanes(dest, vf_out(ft))] + ['}'])
        if special == 0x3E:         # ILWR
            if vit == 0:
                return []
            return [f'{{ uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>({vi(vis)}))) * 16u; addr &= (dataSize - 1);',
                    f'  if (addr + 16 <= dataSize) {{ uint32_t v; std::memcpy(&v, vuData + addr + {first * 4}, 4); '
                    f'{vi(vit)} = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFF)); }} }}']
        if special == 0x3F:         # ISWR
            return store(f'(static_cast<uint32_t>(static_cast<uint16_t>({vi(vis)}))) * 16u', vi_words(vit))
        if special == 0x3B:         # WAITQ
            return []
        if special == 0x38:         # DIV -> queueQ (Q pipeline stays in the interpreter)
            fsf, ftf = (lower >> 21) & 3, (lower >> 23) & 3
            return [f'vuDiv(vu, {vf(fs)}[{fsf}], {vf(ft)}[{ftf}]);']
        if special == 0x68:         # XTOP
            return [f'{vi(vit)} = static_cast<int32_t>(vu.m_state.top & 0x3FFu);'] if vit else []
        if special == 0x69:         # XITOP
            return [f'{vi(vit)} = static_cast<int32_t>(vu.m_state.itop & 0x3FFu);'] if vit else []
        if special == 0x6C:         # XGKICK
            return [f'vu.startXgkick(static_cast<uint32_t>(static_cast<uint16_t>({vi(vis)})));']
        return None

    FMAC_FLAG_OPS = set(range(0x00, 0x10)) | set(range(0x18, 0x1D)) | {0x1E} | set(range(0x20, 0x2B)) | {0x2C, 0x2D, 0x2E}
    ACC_FLAG_SPECIALS = set(range(0x00, 0x10)) | set(range(0x18, 0x1D)) | {0x1E} | set(range(0x20, 0x2B)) | {0x2C, 0x2D, 0x2E}

    def queued_entries(self, d):
        """Cycles after issue at which entries queued by execUpper/execLower become ready."""
        delays = []
        upper = d['upper']
        op = upper & 0x3F
        dest = (upper >> 21) & 0xF
        # Flag entries (FMAC, CLIP, FCSET, FSSET) are static temporaries now.
        if not d['iBit']:
            lower = d['lower']
            if lower in (0, 0x8000033C):
                return delays
            hi = (lower >> 25) & 0x7F
            # SQ / ISW / SQI / SQD / ISWR are static stores (inline_lower).
            if hi == 0x40 and (lower & 0x3F) >= 0x3C:
                special = (lower & 3) | ((lower >> 4) & 0x7C)
                if special in (0x38, 0x39):
                    delays.append(7)        # DIV / SQRT -> queueQ
                elif special == 0x3A:
                    delays.append(13)       # RSQRT
                elif d['lowerUsage']['pipeline'] == self.P['efu']:
                    delays.append(d['lowerUsage']['latency'])
        return delays

    def capture(self, fn):
        saved, self.out = self.out, []
        try:
            fn()
            return self.out
        finally:
            self.out = saved

    def xgkick_barrier(self, pc, state, cycle):
        """XGKICK while PATH1 may still be busy: wait dynamically, then continue
        in the block version matching the number of waited cycles."""
        horizon = state.horizon(cycle)
        states = [state]
        bodies = []
        current = copy.deepcopy(state)
        c = cycle
        for _ in range(horizon):
            nxt = copy.deepcopy(current)
            box = {}
            bodies.append(self.capture(lambda: box.setdefault('c', self.advance(nxt, c))))
            c = box['c']
            current = nxt
            states.append(current)
        self.emit('    { unsigned stall = 0u;')
        self.emit('    while (vu.m_xgkick.active) {')
        self.emit('    switch (++stall) {')
        for k, body in enumerate(bodies, start=1):
            self.emit(f'    case {k}u: {{')
            self.out += body
            self.emit('    break; }')
        self.emit('    default: ++vu.m_cycle; xgkickStep(vu); break;')
        self.emit('    } }')
        labels = []
        for k, s in enumerate(states):
            resolved = copy.deepcopy(s)
            resolved.xgkick = False
            labels.append(self.label_for(pc, resolved, cycle + k))
        self.emit('    switch (stall) {')
        for k, label in enumerate(labels[:-1]):
            self.emit(f'    case {k}u: goto {label};')
        self.emit(f'    default: goto {labels[-1]};')
        self.emit('    } }')

    # ---------------------------------------------------------------- blocks
    def dispatch_branch(self, state, cycle):
        """After the delay slot: run()'s pc update for the pending branch."""
        hi, target, link, pc = state.pending_branch
        state.pending_branch = None
        if hi == 0x21:
            # BAL: subroutine code is specialised per call site.
            if len(state.call_stack) >= 4:
                raise Unsupported(f'BAL nesting too deep at 0x{pc:04x}')
            state.call_stack = state.call_stack + (((pc + 16) & self.pc_mask, link),)
            taken = self.label_for(target, state, cycle)
            self.emit('    vu.m_state.branchPending = false; vu.m_state.branchDelay = 0;')
            self.emit(f'    goto {taken};')
            return
        if hi == 0x24:
            ret, _ = state.call_stack[-1]
            state.call_stack = state.call_stack[:-1]
            taken = self.label_for(ret, state, cycle)
            self.emit(f'    if (vu.m_state.branchTarget != 0x{ret:x}u) VU1TranslationAccess::badReturn(vu, 0x{pc:x}u, 0x{ret:x}u);')
            self.emit('    vu.m_state.branchPending = false; vu.m_state.branchDelay = 0;')
            self.emit(f'    goto {taken};')
            return
        taken = self.label_for(target, state, cycle)
        if hi == 0x20:
            self.emit('    vu.m_state.branchPending = false; vu.m_state.branchDelay = 0;')
            self.emit(f'    goto {taken};')
            return
        fall = self.label_for((pc + 16) & self.pc_mask, state, cycle)
        self.emit(f'    if (vu.m_state.branchPending) {{ vu.m_state.branchPending = false; vu.m_state.branchDelay = 0; goto {taken}; }}')
        self.emit(f'    goto {fall};')

    def block(self, label, pc, state):
        self.emit(f'{label}:')
        if self.budget_guard:
            self.emit(f'    if (vu.m_cycle - entryCycle >= maxCycles) {{ storeRegs(vu, VF, VI, ACC); budgetExceeded(vu, rounding, 0x{pc:x}u); return; }}')
        cycle = 0
        steps = 0
        while True:
            steps += 1
            if steps > 4096:
                raise Unsupported('block too long')
            d = self.decode[pc]
            if not d['iBit'] and d['lowerUsage']['pipeline'] == self.P['xgkick'] and state.xgkick:
                self.xgkick_barrier(pc, state, cycle)
                return
            if state.pending_branch is not None:
                # This pair is the delay slot of the pending branch.
                if not d['iBit'] and d['lowerUsage']['pipeline'] == self.P['branch']:
                    raise Unsupported(f'branch in delay slot at 0x{pc:04x}')
                if d['eBit'] or state.ebit:
                    raise Unsupported(f'E bit in delay slot at 0x{pc:04x}')
                cycle, _ = self.pair(pc, state, cycle)
                cycle = self.advance(state, cycle)
                self.dispatch_branch(state, cycle)
                return
            ending = state.ebit
            eflag = bool(d['eBit'])
            cycle, is_branch = self.pair(pc, state, cycle)
            if is_branch:
                if eflag or ending:
                    raise Unsupported(f'branch with E bit at 0x{pc:04x}')
                lower = d['lower']
                hi = (lower >> 25) & 0x7F
                if hi not in (0x20, 0x21, 0x24, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
                    raise Unsupported(f'unsupported branch 0x{hi:02x} at 0x{pc:04x}')
                if hi == 0x24 and not (state.call_stack and state.call_stack[-1][1] == (lower >> 11) & 31):
                    raise Unsupported(f'JR without a matching BAL at 0x{pc:04x}')
                imm = lower & 0x7FF
                imm = imm - 0x800 if imm & 0x400 else imm
                target = (pc + 8 + imm * 8) & self.pc_mask
                cycle = self.advance(state, cycle)
                state.pending_branch = (hi, target, (lower >> 16) & 31, pc)
                pc = (pc + 8) & self.pc_mask
                continue
            cycle = self.advance(state, cycle)
            if ending:
                # The pair after the E bit was the last one: drain like flushPipelines().
                while state.pending:
                    cycle = self.advance(state, cycle)
                    if cycle > 4096:
                        raise Unsupported('pipeline drain did not finish')
                self.emit('    storeRegs(vu, VF, VI, ACC);')
                self.emit(f'    VU1TranslationAccess::end(vu, rounding, 0x{(pc + 8) & self.pc_mask:x}u);')
                self.emit('    return;')
                return
            if eflag:
                state.ebit = True
            pc = (pc + 8) & self.pc_mask

    def program(self, entry):
        self.out = []
        start = self.label_for(entry, State(), 0)
        bodies = []
        while self.work:
            label, pc, state = self.work.pop(0)
            self.out = []
            self.block(label, pc, state)
            bodies.append(self.out)
        lines = [f'    goto {start};']
        for b in bodies:
            lines += b
        return lines


# ------------------------------------------------------------ flag liveness
# Most FMAC results' MAC/status flags are overwritten by a later FMAC before
# anything reads them. In programs that never read the status flags (no
# FSAND/FSEQ/FSOR/FSSET, no generic interpreter calls), a liveness pass over
# the generated statements removes the dead MAC/status commits; FMACs whose
# flags then reach nothing become fmacValue(), which only produces the value
# and ORs the sticky Z/S/U/O bits into the local `sticky`, folded into the
# status before end(). Sticky bits are only observable through the status,
# so accumulating them at issue instead of commit changes nothing here.
FMAC_CALL = re.compile(r'^(\s*)fmac<(\w+), (\d+)u>\((.*), tmac\[(\d+)\], tstat\[\5\], textra\[\5\]\);$')
MAC_COMMIT = re.compile(r'^\s*vu\.m_state\.mac = tmac\[(\d+)\];$')
STAT_COMMIT = re.compile(r'^\s*\{ const uint32_t cur = tstat\[(\d+)\] & 0xFu; vu\.m_state\.status = '
                         r'\(vu\.m_state\.status & 0xFF0u\) \| cur \| \(\(cur \| textra\[\1\]\) << 6\); \}$')
LABEL = re.compile(r'^(L_\w+):$')
GOTO = re.compile(r'goto (L_\w+);')
MAC, STAT = 1, 2


def flag_var(kind, slot):
    return 1 << (2 + 3 * slot + kind)    # kind 0 tmac, 1 tstat, 2 textra


def flag_liveness(body):
    """live-out bit sets per statement (MAC, STAT and the tmac/tstat/textra slots)."""
    labels = {}
    for i, line in enumerate(body):
        m = LABEL.match(line)
        if m:
            labels[m.group(1)] = i
    stall_switch = [i for i, line in enumerate(body) if line.strip() == 'switch (stall) {']
    loops = [i for i, line in enumerate(body) if line.strip() == 'while (vu.m_xgkick.active) {']
    succ, uses, defs = [], [], []
    for i, line in enumerate(body):
        s = line.strip()
        targets = [labels[t] for t in GOTO.findall(line)]
        if (s.startswith('goto ') and s.endswith(';') and len(targets) == 1) or s.startswith('default: goto '):
            nxt = targets
        elif s == 'return;':
            nxt = []
        else:
            nxt = targets + ([i + 1] if i + 1 < len(body) else [])
        if 'break;' in s:
            # XGKICK wait loop: back to the loop head, or out to the dispatch.
            nxt += [j for j in stall_switch if j > i][:1] + [j for j in loops if j < i][-1:]
        succ.append(nxt)
        u = d = 0
        m = FMAC_CALL.match(line)
        if m:
            d = flag_var(0, int(m.group(5))) | flag_var(1, int(m.group(5))) | flag_var(2, int(m.group(5)))
        elif MAC_COMMIT.match(line):
            u, d = flag_var(0, int(MAC_COMMIT.match(line).group(1))), MAC
        elif STAT_COMMIT.match(line):
            slot = int(STAT_COMMIT.match(line).group(1))
            u, d = flag_var(1, slot) | flag_var(2, slot), STAT
        else:
            if 'vu.m_state.mac' in line:
                u |= MAC
            if 'vu.m_state.status' in line or 'commitReadyPipelines' in line:
                u |= STAT
            if 'VU1TranslationAccess::end(' in line or 'budgetExceeded(' in line:
                u |= MAC | STAT
            for kind, name in enumerate(('tmac', 'tstat', 'textra')):
                for slot in re.findall(name + r'\[(\d+)\]', line):
                    u |= flag_var(kind, int(slot))
        uses.append(u)
        defs.append(d)
    live_in = [0] * len(body)
    live_out = [0] * len(body)
    changed = True
    while changed:
        changed = False
        for i in range(len(body) - 1, -1, -1):
            out = 0
            for j in succ[i]:
                out |= live_in[j]
            new_in = uses[i] | (out & ~defs[i])
            if out != live_out[i] or new_in != live_in[i]:
                live_out[i], live_in[i] = out, new_in
                changed = True
    return live_out


def optimize_flags(body, vu1):
    """Returns (body, uses sticky) after removing unobserved FMAC flags."""
    for line in body:
        if (('vu.m_state.status' in line and not STAT_COMMIT.match(line)) or '// FSSET' in line
                or 'vu.exec' in line):
            return body, False
    live = flag_liveness(body)
    body = [line for i, line in enumerate(body)
            if not ((MAC_COMMIT.match(line) and not live[i] & MAC) or
                    (STAT_COMMIT.match(line) and not live[i] & STAT))]
    live = flag_liveness(body)
    out = []
    for i, line in enumerate(body):
        m = FMAC_CALL.match(line)
        if m:
            indent, kind, dest, args, slot = m.groups()
            slot = int(slot)
            needed = live[i] & (flag_var(0, slot) | flag_var(1, slot) | flag_var(2, slot))
            if needed:
                line = f'{line} sticky |= tstat[{slot}] | textra[{slot}];'
            else:
                acc_normal = ', true' if vu1 and kind in ('FmacMadd', 'FmacMsub') else ''
                line = f'{indent}fmacValue<{kind}, {dest}u{acc_normal}>({args}, sticky, stickyZero, stickySign);'
        elif 'VU1TranslationAccess::end(' in line:
            out.append('    vu.m_state.status |= (foldSticky(sticky, stickyZero, stickySign) & 0xFu) << 6;')
        elif 'budgetExceeded(' in line:
            assert '{ vu.m_cycle = cycle; storeRegs' in line, line
            line = line.replace('{ vu.m_cycle = cycle; storeRegs',
                                '{ vu.m_state.status |= (foldSticky(sticky, stickyZero, stickySign) & 0xFu) << 6; '
                                'vu.m_cycle = cycle; storeRegs')
        out.append(line)
    return out, True


def main():
    dump_tool, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    spec = json.loads((PORT / 'config/vu1_programs.json').read_text())
    out_dir.mkdir(parents=True, exist_ok=True)
    functions, table = [], []
    index = 0
    for program in spec['programs']:
        code_hash = int(program['code_hash'], 16)
        vu0 = program.get('unit', 'vu1') == 'vu0'
        image = PORT / 'local/vu1' / f"{'vu0code' if vu0 else 'vu1code'}-{program['code_hash']}.bin"
        if not image.exists():
            print(f'skip {program["code_hash"]}: capture {image} first (OROCHI_VU1_CAPTURE=1)')
            continue
        data = image.read_bytes()
        assert len(data) == (0x1000 if vu0 else 0x4000) and fnv(data) == code_hash, f'{image} does not match its hash'
        lines = subprocess.run([str(dump_tool), str(image)], check=True, capture_output=True, text=True).stdout.splitlines()
        header = json.loads(lines[0])
        decode = {}
        for line in lines[1:]:
            d = json.loads(line)
            decode[d['pc']] = d
        for entry_text in program['entries']:
            entry = int(entry_text, 16)
            gen = Generator(decode, header, 0x0FFF if vu0 else 0x3FFF)
            try:
                body = gen.program(entry)
            except Unsupported as error:
                print(f'not translated {program["code_hash"]}@0x{entry:04x}: {error}')
                continue
            fmacs = sum(1 for line in body if FMAC_CALL.match(line))
            body, sticky = optimize_flags(body, not vu0)
            flagless = sum(1 for line in body if 'fmacValue<' in line)
            name = f'program<{index}>'
            fn = [f'// {program["code_hash"]} entry 0x{entry:04x}: {len(gen.versions)} block versions',
                  'template <>',
                  f'void VU1TranslationAccess::program<{index}>(Vu &vu, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t maxCycles)',
                  '{',
                  '    (void)maxCycles;',
                  '    const int rounding = begin(vu, vuData, dataSize, gs, memory);',
                  '    alignas(16) float VF[32][4]; int32_t VI[16]; alignas(16) float ACC[4];',
                  '    loadRegs(vu, VF, VI, ACC);',
                  '    int32_t bkr, bkv; loadBranchBackup(vu, bkr, bkv);',
                  '    uint64_t cycle = vu.m_cycle;',
                  '    const uint64_t entryCycle = cycle; (void)entryCycle;',
                  f'    float tvf[{max(gen.max_vf_slots, 1)}][4]; int32_t tvi[{max(gen.max_vi_slots, 1)}]; float tacc[{max(gen.max_acc_slots, 1)}][4];',
                  f'    uint32_t tmac[{max(gen.max_flag_slots, 1)}], tstat[{max(gen.max_flag_slots, 1)}], textra[{max(gen.max_flag_slots, 1)}], tclip[{max(gen.max_flag_slots, 1)}];',
                  f'    bool tsv[{max(gen.max_store_slots, 1)}]; uint32_t tsa[{max(gen.max_store_slots, 1)}], tsw[{max(gen.max_store_slots, 1)}][4];',
                  '    vu.commitReadyPipelines();']
            if sticky:
                fn.append('    uint32_t sticky = 0u; __m128i stickyZero = _mm_setzero_si128(), stickySign = _mm_setzero_si128();')
            fn += body
            fn.append('}')
            functions.append('\n'.join(fn))
            table.append(f'    {{0x{code_hash:016x}ull, 0x{entry:x}u, &VU1TranslationAccess::program<{index}>}},')
            print(f'translated {program["code_hash"]}@0x{entry:04x}: {len(gen.versions)} block versions, {len(body)} lines, '
                  f'{flagless}/{fmacs} FMACs without flags')
            index += 1
    source = ['// Generated by tools/generate_vu1.py from private VU1 code images. Do not edit.',
              '#include "vu1_translation_access.h"',
              '#include "vu1_translated.h"',
              '#include <cstring>',
              '']
    source += functions
    source += ['', 'const VU1TranslatedProgram vu1TranslatedPrograms[] = {'] + (table or ['    {0, 0, nullptr},']) + ['};',
               f'const unsigned vu1TranslatedProgramCount = {len(table)}u;', '']
    header = ['#pragma once', '#include "runtime/ps2_vu1.h"', '#include <cstdint>',
              'struct VU1TranslatedProgram { uint64_t codeHash; uint32_t entry; VU1Interpreter::TranslatedProgram program; };',
              'extern const VU1TranslatedProgram vu1TranslatedPrograms[];',
              'extern const unsigned vu1TranslatedProgramCount;', '']
    for path, text in ((out_dir / 'vu1_translated.cpp', '\n'.join(source)), (out_dir / 'vu1_translated.h', '\n'.join(header))):
        if not path.exists() or path.read_text() != text:
            path.write_text(text)


if __name__ == '__main__':
    main()
