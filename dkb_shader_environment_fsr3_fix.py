#!/usr/bin/env python3
from pathlib import Path
import sys

env_path = Path("src/shader_recompiler/environment.h")
enum_path = Path("src/common/settings_enums.h")

if not env_path.is_file() or not enum_path.is_file():
    sys.exit("ERROR: run this from ~/DED-SWITCH-EMU.")

env = env_path.read_text()
enums = enum_path.read_text()

if "virtual bool IsNativeProgram()" in env:
    sys.exit("STOP: IsNativeProgram already exists in base Environment.")
if "virtual bool IsCbufStorageBufferPointer" in env:
    sys.exit("STOP: IsCbufStorageBufferPointer already exists in base Environment.")
if "virtual bool HasNativeInstructions()" in env:
    sys.exit("STOP: HasNativeInstructions already exists in base Environment.")
if "Fsr3" in enums:
    sys.exit("STOP: ScalingFilter::Fsr3 already exists.")

anchor1 = "    [[nodiscard]] virtual u32 TranslateProgramAddress(u32 address) const { return address; }\n"
anchor2 = "    [[nodiscard]] virtual bool HasHLEMacroState() const = 0;\n"
anchor3 = "ENUM(ScalingFilter, NearestNeighbor, Bilinear, Bicubic, Gaussian, Lanczos, ScaleForce, Fsr,"

if env.count(anchor1) != 1 or env.count(anchor2) != 1:
    sys.exit("STOP: Environment.h insertion anchors do not match exactly.")
if enums.count(anchor3) != 1:
    sys.exit("STOP: ScalingFilter enum anchor does not match exactly.")

native_methods = """
    /// Switch 2 native programs are lowered to Maxwell instructions.
    [[nodiscard]] virtual bool IsNativeProgram() const { return false; }

    /// Whether a native program constant-buffer word contains a GPU storage pointer.
    [[nodiscard]] virtual bool IsCbufStorageBufferPointer(u32, u32) { return true; }
"""
env = env.replace(anchor1, anchor1 + native_methods, 1)
env = env.replace(
    anchor2,
    anchor2 + "\n    [[nodiscard]] virtual bool HasNativeInstructions() const { return false; }\n",
    1,
)

enums = enums.replace(
    anchor3,
    anchor3.replace("Fsr,", "Fsr, Fsr3,"),
    1,
)

env_path.write_text(env)
enum_path.write_text(enums)
print("Added the three missing virtual hooks to Shader::Environment.")
print("Added ScalingFilter::Fsr3.")
print("No existing declarations were overwritten.")
