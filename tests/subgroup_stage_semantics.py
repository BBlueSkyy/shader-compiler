#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
profile = (ROOT / "profile.h").read_text()
emit = (ROOT / "backend/spirv/emit_spirv.cpp").read_text()
ctx = (ROOT / "backend/spirv/spirv_emit_context.cpp").read_text()
warp = (ROOT / "backend/spirv/emit_spirv_warp.cpp").read_text()

required = [
    ("profile", "supported_subgroup_stages", profile),
    ("profile", "SupportsSubgroupStage", profile),
    ("capabilities", "profile.SupportsSubgroupStage(ctx.stage)", emit),
    ("inputs", "info.uses_subgroup_mask && profile.SupportsSubgroupStage(stage)", ctx),
    ("inputs", "profile.SupportsSubgroupStage(stage))", ctx),
    ("warp", "StageSupportsSubgroups", warp),
    ("warp", "return pred;", warp),
    ("warp", "return ctx.true_value;", warp),
    ("warp", "SetInBoundsFlag(inst, ctx.false_value);", warp),
]
for area, token, source in required:
    if token not in source:
        raise SystemExit(f"missing subgroup-stage guard in {area}: {token}")

print("subgroup stage source contract: ok")
