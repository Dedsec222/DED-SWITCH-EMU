DED NX2 ImageDescriptor compatibility fix

From repo root:
  cd ~/DED-SWITCH-EMU
  unzip -o ~/storage/downloads/DED-NX2-image-byte-addressed-fix.zip
  python dkb_image_byte_addressed_fix.py

Then:
  git diff --check
  grep -n -A14 'struct ImageDescriptor' src/shader_recompiler/shader_info.h
  cmake --build ~/DED-NX2-ANDROID-BUILD --parallel 2

The script adds the missing bool member after size_shift, matching the Lichii
NX2 ImageDescriptor layout. It refuses to duplicate the field or edit if the
expected structure differs.
