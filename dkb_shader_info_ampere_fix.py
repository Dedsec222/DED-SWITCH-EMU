#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path.cwd()
info_path = root / "src/shader_recompiler/shader_info.h"
header_path = root / "src/shader_recompiler/frontend/ampere/instruction.h"

if not info_path.is_file():
    sys.exit("ERROR: run from ~/DED-SWITCH-EMU; shader_info.h not found.")

info = info_path.read_text()
anchor = "    static constexpr size_t MAX_SSBOS{32};"
fields = "    bool is_task{}; ///< Switch 2 task program (Stage::Task)\n    bool is_mesh{}; ///< Switch 2 mesh program (Stage::Mesh)\n"

if "bool is_task{};" in info or "bool is_mesh{};" in info:
    sys.exit("STOP: is_task or is_mesh already exists in shader_info.h; no files changed.")
if info.count(anchor) != 1:
    sys.exit("STOP: expected unique MAX_SSBOS anchor not found; no files changed.")
if header_path.exists():
    sys.exit("STOP: Ampere instruction.h already exists; refusing to overwrite.")

header = """// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>

#include "common/common_types.h"

namespace Shader::Ampere {

struct Instruction {
    std::array<u32, 4> words;

    [[nodiscard]] u32 Bits(u32 offset, u32 count) const {
        u64 bits = words[offset / 32];
        if (offset / 32 + 1 < words.size()) {
            bits |= u64{words[offset / 32 + 1]} << 32;
        }
        return static_cast<u32>((bits >> (offset % 32)) & ((u64{1} << count) - 1));
    }
};

static_assert(sizeof(Instruction) == 16);

} // namespace Shader::Ampere
"""

new_info = info.replace(anchor, anchor + "\n" + fields.rstrip(), 1)
header_path.parent.mkdir(parents=True, exist_ok=True)

# All validations completed; now make the two narrowly scoped changes.
info_path.write_text(new_info)
header_path.write_text(header)

print("Added Shader::Info::is_task and Shader::Info::is_mesh.")
print(f"Created {header_path.relative_to(root)}.")
print("No existing files were overwritten.")
