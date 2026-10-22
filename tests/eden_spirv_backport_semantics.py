#!/usr/bin/env python3
"""Focused host-side regression checks for stage-1 Eden SPIR-V correctness backports.

No device or shader compiler dependencies are required. These checks exercise the
index/offset arithmetic and assert that the production paths emit the intended
SPIR-V operations; they are not a replacement for SPIR-V validation or APK tests.
"""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
IMAGE = (ROOT / "backend/spirv/emit_spirv_image.cpp").read_text()
CBUF = (ROOT / "backend/spirv/emit_spirv_context_get_set.cpp").read_text()


def signed32(value):
    value &= 0xFFFFFFFF
    return value if value < 0x80000000 else value - 0x100000000


def scaled(words, index, base=0):
    position = (index + base) & 0xFFFFFFFF
    return bool(words[position >> 5] & (1 << (position & 31)))


class EdenSpirvBackportTests(unittest.TestCase):
    def test_rescale_mask_uses_push_constant_word(self):
        self.assertIn(
            "const Id word_index{ctx.OpShiftRightLogical(ctx.U32[1], index_value, ctx.Const(5u))};",
            IMAGE,
        )
        self.assertIn("bit = ctx.OpBitwiseAnd(ctx.U32[1], word, bit_index_mask);", IMAGE)
        self.assertNotIn(
            "bit = ctx.OpBitFieldUExtract(ctx.U32[1], index_value, bit_index, ctx.Const(1u));",
            IMAGE,
        )
        words = [0x80000001, 0x00000010, 0x00000000, 0x00000002]
        for index, wanted in [(0, True), (1, False), (31, True), (32, False),
                              (36, True), (63, False), (64, False), (97, True)]:
            self.assertEqual(scaled(words, index), wanted)
        self.assertTrue(scaled(words, 30, 1))
        self.assertTrue(scaled(words, 32, 4))

    def test_ptp_offsets_are_signed_int_vectors(self):
        self.assertIn("ctx.TypeArray(ctx.S32[2], ctx.Const(4U))", IMAGE)
        self.assertIn("return static_cast<s32>(values[a]->Arg(b).U32());", IMAGE)
        for raw, signed in [(0, 0), (1, 1), (0xFFFFFFFE, -2),
                            (0xFFFFFFFF, -1), (0x80000000, -2147483648)]:
            self.assertEqual(signed32(raw), signed)

    def test_unsigned_cbuf_byte_offsets(self):
        self.assertIn(
            "buffer_offset = ctx.OpShiftRightLogical(ctx.U32[1], ctx.Def(offset), shift);",
            CBUF,
        )
        self.assertIn(
            "ctx.OpShiftRightLogical(ctx.U32[1], ctx.Def(offset), ctx.Const(2u))",
            CBUF,
        )
        for offset, width in [(0, 4), (20, 4), (0x80000000, 4),
                              (0xFFFFFFFC, 4), (0xFFFFFFFF, 16)]:
            self.assertEqual((offset & 0xFFFFFFFF) >> (width.bit_length() - 1),
                             (offset & 0xFFFFFFFF) // width)


if __name__ == "__main__":
    unittest.main()
