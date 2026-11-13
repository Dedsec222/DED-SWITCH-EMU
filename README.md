# DED NX2 read-trap integration patch

This patch is based on the exact six source files supplied in `DED-NX2-PATCH-SOURCE.tar.gz`.

## What it changes
- Adds `DeviceMemoryManager::ArmReadTraps` and maps device-address pages back to their owning guest process.
- Adds the GPU-device callback that forwards read-trap ranges into the guest memory subsystem.
- Adds one-shot read protection for rasterizer-cached pages in `Core::Memory`.
- Keeps the feature disabled when fastmem is unavailable, reactive flushing is enabled, or the CPU backend is NCE.
- Handles rasterizer reads from non-CPU/service threads without indexing per-core arrays out of range.
- Uses `EDEN_NX2_READ_TRAPS=0` as an opt-out.

## Apply
From the repository root, after the existing Lichii NX2 changes are already present:

```sh
git apply --check DED-NX2-readtraps.patch
git apply DED-NX2-readtraps.patch
```

## Verification status
The patch was generated against the uploaded source snapshot and its anchors were checked while generating it. It has **not** been compiled or runtime-tested here. A successful `git apply --check` and Android NDK build on the target Termux tree are still required.

The uploaded archive did not include the complete repository, Android build logs, or upstream `ReadTrapTracker`/full buffer-cache implementation. This patch addresses the concrete missing `ArmReadTraps` API and the associated memory-side mechanism; it does not claim that all NX2 integration issues are resolved.
