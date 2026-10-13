"""Site discovery and classification for the Haswell instruction rewrite."""
from paths import ROOT
import unittest
import isa_rewrite

BLSR_ECX = bytes.fromhex('c4e270f3c9')          # blsr ecx, ecx            (5)
MOVBE_STORE = bytes.fromhex('0f38f108')          # movbe [rax], ecx         (4)
BEXTR_RIP = bytes.fromhex('c4e270f7050c000000')  # bextr eax, [rip+0xc], ecx (9)
LZCNT_AX = bytes.fromhex('66f30fbdc1')           # lzcnt ax, cx             (5)
TZCNT_RAX = bytes.fromhex('f3480fbcc1')          # tzcnt rax, rcx           (5)
NOP, RET, JMP_SELF = b'\x90', b'\xc3', bytes.fromhex('ebfe')
MOV_RIP = bytes.fromhex('8b0500000000')          # mov eax, [rip+0]         (6)
MOV_FS = bytes.fromhex('648b042500000000')       # mov eax, fs:[0]          (8): not a site


@unittest.skipUnless(isa_rewrite.iced_x86, 'pip install iced-x86')
class ScanSegmentTests(unittest.TestCase):
    def test_blsr_is_a_jmp_site(self):
        s, = isa_rewrite.scan_segment(BLSR_ECX, 0x1000)
        self.assertEqual((s.offset, s.length, s.kind, s.width, s.mode), (0x1000, 5, isa_rewrite.KIND_BLSR, 32, isa_rewrite.MODE_JMP))
        self.assertEqual((s.dst, s.src1, s.src2, s.mem_slot), (1, 1, isa_rewrite.NO_REG, 0))
        self.assertEqual((s.original, s.relocated), (BLSR_ECX, b''))

    def test_bextr_rip_relative_memory_source(self):
        s, = isa_rewrite.scan_segment(BEXTR_RIP, 0x1000)
        self.assertEqual((s.kind, s.width, s.dst, s.src1, s.src2, s.mem_slot), (isa_rewrite.KIND_BEXTR, 32, 0, isa_rewrite.NO_REG, 1, 2))
        self.assertEqual((s.base, s.index, s.mem_flags, s.disp), (isa_rewrite.NO_REG, isa_rewrite.NO_REG, 1, 0x1000 + 9 + 0xc))

    def test_sixteen_bit_lzcnt_and_sixty_four_bit_tzcnt(self):
        a, b = isa_rewrite.scan_segment(LZCNT_AX + TZCNT_RAX, 0)
        self.assertEqual((a.kind, a.width, a.length), (isa_rewrite.KIND_LZCNT, 16, 5))
        self.assertEqual((b.kind, b.width, b.dst, b.src1), (isa_rewrite.KIND_TZCNT, 64, 0, 1))

    def test_four_byte_store_relocates_a_plain_next_instruction(self):
        s, = isa_rewrite.scan_segment(MOVBE_STORE + NOP + RET, 0)
        self.assertEqual((s.kind, s.mode, s.mem_slot, s.base, s.dst, s.src1), (isa_rewrite.KIND_MOVBE_STORE, isa_rewrite.MODE_JMP_RELOCATE, 1, 0, isa_rewrite.NO_REG, 1))
        self.assertEqual(s.relocated, NOP)

    def test_four_byte_site_falls_back_to_int3(self):
        for tail in (JMP_SELF, MOV_RIP, BLSR_ECX, b''):       # branch, RIP-relative, another site, end of segment
            s = isa_rewrite.scan_segment(MOVBE_STORE + tail, 0)[0]
            self.assertEqual((s.mode, s.relocated), (isa_rewrite.MODE_INT3, b''), tail.hex())

    def test_next_instruction_that_is_a_branch_target_blocks_relocation(self):
        code = bytes.fromhex('eb04') + MOVBE_STORE + NOP + RET   # jmp +4 lands on the nop
        s, = isa_rewrite.scan_segment(code, 0)
        self.assertEqual(s.mode, isa_rewrite.MODE_INT3)

    def test_unsupported_encodings(self):
        for code, why in ((b'\x64' + BEXTR_RIP, 'segment prefix'), (b'\x67' + MOVBE_STORE, '32-bit address'),
                          (bytes.fromhex('c4e2791310'), 'vcvtph2ps xmm2, [rax]')):
            s, = isa_rewrite.scan_segment(code + RET, 0)
            self.assertEqual(s.mode, isa_rewrite.MODE_UNSUPPORTED, why)
        self.assertEqual(isa_rewrite.scan_segment(bytes.fromhex('c4e2791310') + RET, 0)[0].kind, isa_rewrite.KIND_VCVTPH2PS)

    def test_non_sites_are_ignored(self):
        self.assertEqual(isa_rewrite.scan_segment(MOV_FS + RET, 0), [])


if __name__ == '__main__':
    unittest.main()
