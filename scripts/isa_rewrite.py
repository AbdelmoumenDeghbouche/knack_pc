"""Find the Haswell-only instructions (MOVBE, LZCNT, BMI1, F16C) in the linked image.

scan_segment decodes one executable segment with iced-x86 and classifies each
site: JMP (5+ bytes, patched with a jump to a stub), JMP_RELOCATE (4 bytes,
the jump also covers the plain next instruction, copied into the stub), INT3
(4 bytes, trapped at run time) or UNSUPPORTED (an encoding the stubs do not
emulate: segment override, LOCK, 32-bit addressing or F16C).
"""
from dataclasses import dataclass

try:
    import iced_x86
    from iced_x86 import Code, Decoder, FlowControl, Mnemonic, OpKind, Register, RegisterExt
except ImportError:
    iced_x86 = None

(KIND_MOVBE_LOAD, KIND_MOVBE_STORE, KIND_LZCNT, KIND_TZCNT, KIND_ANDN, KIND_BEXTR,
 KIND_BLSI, KIND_BLSMSK, KIND_BLSR, KIND_VCVTPH2PS, KIND_VCVTPS2PH) = range(1, 12)
MODE_JMP, MODE_JMP_RELOCATE, MODE_INT3, MODE_UNSUPPORTED = range(4)
NO_REG = 0xFF
SET_OF_KIND = {KIND_MOVBE_LOAD: 'MOVBE', KIND_MOVBE_STORE: 'MOVBE', KIND_LZCNT: 'LZCNT',
               KIND_TZCNT: 'BMI1', KIND_ANDN: 'BMI1', KIND_BEXTR: 'BMI1', KIND_BLSI: 'BMI1',
               KIND_BLSMSK: 'BMI1', KIND_BLSR: 'BMI1', KIND_VCVTPH2PS: 'F16C', KIND_VCVTPS2PH: 'F16C'}


@dataclass
class Site:
    offset: int
    length: int
    kind: int
    width: int
    mode: int
    dst: int
    src1: int
    src2: int
    mem_slot: int   # 0 none, 1 dst, 2 src
    base: int
    index: int
    scale: int
    mem_flags: int  # bit 0: RIP-relative, disp is the absolute target
    disp: int
    original: bytes
    relocated: bytes


def kind_of(ins):
    m = ins.mnemonic
    if m == Mnemonic.MOVBE:
        return KIND_MOVBE_LOAD if ins.op0_kind == OpKind.REGISTER else KIND_MOVBE_STORE
    return {Mnemonic.LZCNT: KIND_LZCNT, Mnemonic.TZCNT: KIND_TZCNT, Mnemonic.ANDN: KIND_ANDN,
            Mnemonic.BEXTR: KIND_BEXTR, Mnemonic.BLSI: KIND_BLSI, Mnemonic.BLSMSK: KIND_BLSMSK,
            Mnemonic.BLSR: KIND_BLSR, Mnemonic.VCVTPH2PS: KIND_VCVTPH2PS,
            Mnemonic.VCVTPS2PH: KIND_VCVTPS2PH}.get(m)


def register_number(reg):
    return NO_REG if reg == Register.NONE else RegisterExt.number(reg)


def site_of(ins, kind, original):
    """Operands in order fill dst, src1, src2; the memory operand leaves its slot NO_REG."""
    regs, mem_slot, width = [NO_REG] * 3, 0, 0
    for i in range(ins.op_count):
        if ins.op_kind(i) == OpKind.REGISTER:
            regs[i] = register_number(ins.op_register(i))
            width = width or RegisterExt.size(ins.op_register(i)) * 8
        elif ins.op_kind(i) == OpKind.MEMORY:
            mem_slot = 1 if i == 0 else 2
    if SET_OF_KIND[kind] == 'F16C':
        width = 0
    base = index = NO_REG
    scale = mem_flags = disp = 0
    unsupported = SET_OF_KIND[kind] == 'F16C' or ins.segment_prefix != Register.NONE or ins.has_lock_prefix
    if mem_slot:
        unsupported |= any(r != Register.NONE and RegisterExt.size(r) != 8 for r in (ins.memory_base, ins.memory_index))
        scale = ins.memory_index_scale
        if ins.is_ip_rel_memory_operand:
            mem_flags, disp = 1, ins.ip_rel_memory_address
        else:
            base, index, disp = register_number(ins.memory_base), register_number(ins.memory_index), ins.memory_displacement
    mode = MODE_UNSUPPORTED if unsupported else MODE_JMP if ins.len >= 5 else MODE_INT3
    return Site(ins.ip, ins.len, kind, width, mode, *regs, mem_slot, base, index, scale, mem_flags, disp, original, b'')


def is_plain(ins, end):
    """Candidate to be moved into a 4-byte site's stub (branch targets are checked later)."""
    return (ins.code != Code.INVALID and ins.flow_control == FlowControl.NEXT and not ins.is_ip_rel_memory_operand
            and not ins.has_rep_prefix and not ins.has_repne_prefix and kind_of(ins) is None and ins.next_ip <= end)


def scan_segment(code, address):
    """Sites of one executable segment loaded at address, in address order."""
    if iced_x86 is None:
        raise RuntimeError('iced_x86 is not installed')
    end = address + len(code)
    sites, targets, candidates = [], set(), []
    pending = None  # 4-byte site waiting for the instruction after it
    for ins in Decoder(64, code, ip=address):
        if ins.flow_control in (FlowControl.UNCONDITIONAL_BRANCH, FlowControl.CONDITIONAL_BRANCH, FlowControl.CALL) \
                and ins.op0_kind == OpKind.NEAR_BRANCH64:
            targets.add(ins.near_branch_target)
        if pending and is_plain(ins, end):
            candidates.append((pending, ins.ip, code[ins.ip - address:ins.next_ip - address]))
        pending = None
        kind = kind_of(ins)
        if kind is None:
            continue
        site = site_of(ins, kind, code[ins.ip - address:ins.next_ip - address])
        sites.append(site)
        if site.mode == MODE_INT3 and ins.len == 4:
            pending = site
    for site, ip, relocated in candidates:
        if ip not in targets:
            site.mode, site.relocated = MODE_JMP_RELOCATE, relocated
    return sites
