#!/usr/bin/env python3
from pathlib import Path
import re
import shutil
import sys

path = Path("src/video_core/host1x/nx2_video_decoder.cpp")
if not path.is_file():
    sys.exit(f"ERROR: {path} not found. Run from ~/DED-SWITCH-EMU.")

source = path.read_text()

# Optional codec-name logging uses NDK APIs introduced in Android 28.
name_pattern = re.compile(
    r'(?m)^[ \t]*char\* name = nullptr;[ \t]*\n'
    r'[ \t]*if \(AMediaCodec_getName\(codec, &name\) == AMEDIA_OK && name != nullptr\) \{[ \t]*\n'
    r'.*?^[ \t]*\}[ \t]*\n(?=[ \t]*return decoder;)',
    re.S,
)

# Replace API-28 rectangle metadata getter with four integer metadata reads.
crop_pattern = re.compile(
    r'(?m)^[ \t]*s32 left\{\}, top\{\}, right\{\}, bottom\{\};[ \t]*\n'
    r'[ \t]*if \(AMediaFormat_getRect\(format, "crop", &left, &top, &right, &bottom\) && right > left &&[ \t]*\n'
    r'[ \t]*bottom > top\) \{[ \t]*\n'
    r'[ \t]*crop_left = left;[ \t]*\n'
    r'[ \t]*crop_top = top;[ \t]*\n'
    r'[ \t]*visible_width = right - left \+ 1;[ \t]*\n'
    r'[ \t]*visible_height = bottom - top \+ 1;[ \t]*\n'
    r'[ \t]*\} else \{[ \t]*\n'
    r'[ \t]*crop_left = crop_top = 0;[ \t]*\n'
    r'[ \t]*visible_width = width;[ \t]*\n'
    r'[ \t]*visible_height = height;[ \t]*\n'
    r'[ \t]*\}',
    re.S,
)

new_crop = """        s32 left{}, top{}, right{}, bottom{};
        const bool has_crop =
            AMediaFormat_getInt32(format, "crop-left", &left) &&
            AMediaFormat_getInt32(format, "crop-top", &top) &&
            AMediaFormat_getInt32(format, "crop-right", &right) &&
            AMediaFormat_getInt32(format, "crop-bottom", &bottom);
        if (has_crop && right >= left && bottom >= top) {
            crop_left = left;
            crop_top = top;
            visible_width = right - left + 1;
            visible_height = bottom - top + 1;
        } else {
            crop_left = crop_top = 0;
            visible_width = width;
            visible_height = height;
        }"""

source2, name_count = name_pattern.subn("", source, count=1)
if name_count != 1:
    sys.exit(f"STOP: expected one codec-name block, found {name_count}. No file changed.")

source3, crop_count = crop_pattern.subn(new_crop, source2, count=1)
if crop_count != 1:
    sys.exit(f"STOP: expected one getRect crop block, found {crop_count}. No file changed.")

if any(token in source3 for token in (
    "AMediaCodec_getName", "AMediaCodec_releaseName", "AMediaFormat_getRect"
)):
    sys.exit("STOP: API-28 call remains. No file changed.")

backup = path.with_suffix(path.suffix + ".pre-api26-fix.bak")
if backup.exists():
    sys.exit(f"STOP: backup already exists at {backup}; refusing to overwrite it.")

shutil.copy2(path, backup)
path.write_text(source3)
print(f"Updated {path}")
print(f"Backup: {backup}")
print("API-28 codec-name logging removed; crop metadata now uses integer keys.")
