#!/bin/bash
# Runs INSIDE the vitasdk/vitasdk container (started by build.bat / build.sh). /src is the repository folder.
set -eo pipefail
cd /src
echo "== VitaSDK $VITASDK: $(arm-vita-eabi-gcc --version | head -1)"

# 1. kubridge import stub (built once)
STUB=third_party/kubridge_stub/build/libkubridge_stub.a
if [ ! -f "$STUB" ]; then
  echo "== building the kubridge import stub"
  ( cd third_party/kubridge_stub
    rm -rf gen build
    vita-libs-gen -c kubridge_nids.yml gen
    cmake -S gen -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" > /dev/null
    cmake --build build )
fi

# 2. our patched vitaGL (render-target purge, garbage collection in long frames, kept depth/stencil, optional shader cache); built once
VGL=build/vitagl/vitaGL/libvitaGL.a
if [ ! -f "$VGL" ]; then
  echo "== building the patched vitaGL (first build only, a few minutes)"
  sh tools/build_vitagl.sh
fi
cp -f "$VGL" "$VITASDK/arm-vita-eabi/lib/libvitaGL.a"

# 3. configure + build
rm -rf build/cmake
cmake -S . -B build/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
cmake --build build/cmake 2>&1 | tail -40

# 4. the patched vitaGL must be linked (a stock vitaGL links silently without these and crashes in battles)
arm-vita-eabi-nm build/cmake/brawlvita > build/cmake/syms.txt
for sym in vgl_bv_purge_rts_now vgl_bv_gc_now vgl_bv_ds_store; do
  if ! grep -qwE "[TBD] ${sym}" build/cmake/syms.txt; then echo "BUILD CHECK FAILED: $sym missing (stock vitaGL linked?)"; exit 1; fi
done
cp -f build/cmake/brawlvita.vpk BrawlVita.vpk
cp -f build/cmake/brawlvita BrawlVita.elf   # unstripped: lets a crash dump be read by function name
ls -l BrawlVita.vpk
echo "== done: BrawlVita.vpk"
