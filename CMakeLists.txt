cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(BrawlVita C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

# One VPK per staged test build (m3-loader, then m4-offline). Title id: 4 letters + 5 digits.
set(BV_STAGE "0.1 beta" CACHE STRING "stage label shown in LiveArea and the log")
set(VITA_APP_NAME "BrawlVita")
set(VITA_TITLEID "BRVT00001")
set(VITA_VERSION "00.10")  # 0.1 beta

set(KUBRIDGE_STUB "${CMAKE_SOURCE_DIR}/third_party/kubridge_stub/build/libkubridge_stub.a")
if(NOT EXISTS "${KUBRIDGE_STUB}")
  message(FATAL_ERROR "Missing ${KUBRIDGE_STUB}: build the kubridge stub first (docs/BUILD.md, step 2)")
endif()

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -O2 -g -fno-strict-aliasing)
add_compile_definitions(BV_STAGE="${BV_STAGE}" BV_PUBLIC_CONFIG=1)

add_executable(brawlvita
  loader/main.c
  loader/log.c
  loader/elf_loader.c
  loader/vita_mem.c
  loader/unresolved.S
  loader/bytestream.c
  loader/bmsg.c
  loader/offline_b.c
  loader/passdata.c
  loader/profile.c
  loader/hook_b.c
  loader/gameapp.c
  loader/rawsys.c
  loader/rawsys_vita.c
  loader/antitamper.c
  loader/gamefix.c
  loader/gtrace.c
  loader/antitamper_vita.c
  loader/touch.c
  loader/touch_vita.c
  loader/vpad.c
  loader/botnames.c
  loader/botworld.c
  loader/maprot.c
  loader/csvpatch.c
  loader/offertitles.c
  loader/buttons.c
  loader/buttons_vita.c
  server/bv_server.c
  gfx/etc1.c
  gfx/gl_init.c
  gfx/ime_vita.c
  jni/fake_jni.c
  shims/shim_table.c
  shims/shim_libc.c
  shims/shim_io.c
  shims/shim_math.c
  shims/shim_pthread.c
  shims/shim_gxm.c
  shims/shim_net.c
  shims/vsock.c
  shims/vrandom.c
  shims/vsys.c
  shims/texshrink.c
  shims/shim_extra.c
  shims/shim_asset.c
  shims/shim_gles.c
  shims/shim_egl.c
  shims/shim_fmod.c
  shims/shim_line.c
  audio/audiotrack.c
)
target_include_directories(brawlvita PRIVATE loader shims jni server gfx audio)
# keeps room for vita-elf-create's import tables between the code and the data segment (see the file)
target_link_options(brawlvita PRIVATE "-Wl,-T,${CMAKE_SOURCE_DIR}/loader/sce_gap.ld")
# cycle 5 (ISSUES A2): count the game thread's sleeps per caller (shims/shim_pthread.c __wrap_sceKernelDelayThread)
target_link_options(brawlvita PRIVATE "-Wl,--wrap=sceKernelDelayThread" "-Wl,--wrap=sceKernelDelayThreadCB" "-Wl,--wrap=sem_destroy"
  "-Wl,--wrap=sceGxmCreateRenderTarget" "-Wl,--wrap=sceGxmDestroyRenderTarget" "-Wl,--wrap=sceGxmBeginScene" "-Wl,--wrap=sceGxmEndScene")  # update 94 (A5): shims/shim_gxm.c
target_link_libraries(brawlvita
  ${KUBRIDGE_STUB}
  vitaGL stdc++ vitashark mathneon SceShaccCgExt taihen_stub SceShaccCg_stub SceKernelDmacMgr_stub SceGxm_stub SceDisplay_stub
  SceAppMgr_stub SceSysmodule_stub SceCommonDialog_stub SceTouch_stub SceCtrl_stub
  pthread m
  SceLibKernel_stub SceKernelThreadMgr_stub SceKernelModulemgr_stub SceProcessmgr_stub SceSysmem_stub
  SceIofilemgr_stub ScePower_stub SceAudio_stub
)

# UNSAFE: the loader allocates memory at fixed addresses through the kubridge kernel plugin.
vita_create_self(brawlvita.self brawlvita UNSAFE)

# Packaged with direct vita-mksfoex / vita-pack-vpk calls: vita_create_vpk() joins its arguments into one unquoted
# string, which breaks on paths with spaces (e.g. a Windows user folder).
set(LA "${CMAKE_SOURCE_DIR}/assets/livearea")
set(SFO "${CMAKE_BINARY_DIR}/param.sfo")
add_custom_command(OUTPUT "${SFO}"
  COMMAND "${VITASDK}/bin/vita-mksfoex" -s APP_VER=${VITA_VERSION} -s TITLE_ID=${VITA_TITLEID} "${VITA_APP_NAME}" "${SFO}"
  VERBATIM)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/brawlvita.vpk"
  COMMAND "${VITASDK}/bin/vita-pack-vpk"
    -s "${SFO}" -b "${CMAKE_BINARY_DIR}/brawlvita.self"
    -a "${LA}/icon0.png=sce_sys/icon0.png"
    -a "${LA}/bg.png=sce_sys/livearea/contents/bg.png"
    -a "${LA}/startup.png=sce_sys/livearea/contents/startup.png"
    -a "${LA}/template.xml=sce_sys/livearea/contents/template.xml"
    "${CMAKE_BINARY_DIR}/brawlvita.vpk"
  DEPENDS "${SFO}" "${CMAKE_BINARY_DIR}/brawlvita.self" "${LA}/icon0.png" "${LA}/bg.png" "${LA}/startup.png" "${LA}/template.xml"
  VERBATIM)
add_custom_target(vpk ALL DEPENDS "${CMAKE_BINARY_DIR}/brawlvita.vpk")
