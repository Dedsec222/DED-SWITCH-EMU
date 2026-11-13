#!/usr/bin/env python3
from pathlib import Path
import sys

p = Path("src/shader_recompiler/shader_info.h")
if not p.is_file():
    sys.exit("ERROR: run from ~/DED-SWITCH-EMU.")

s = p.read_text()
if "bool is_byte_addressed{};" in s:
    sys.exit("STOP: is_byte_addressed already exists; no changes made.")

anchor = "    u32 size_shift;\n\n    auto operator<=>(const ImageDescriptor&) const = default;"
replacement = (
    "    u32 size_shift;\n"
    "    bool is_byte_addressed{};\n\n"
    "    auto operator<=>(const ImageDescriptor&) const = default;"
)
if s.count(anchor) != 1:
    sys.exit("STOP: ImageDescriptor anchor did not match exactly; no changes made.")

p.write_text(s.replace(anchor, replacement, 1))
print("Added ImageDescriptor::is_byte_addressed.")
