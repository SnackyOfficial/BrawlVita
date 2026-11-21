#!/bin/sh
# Builds vitaGL (fixed commit) with our small patches into build/vitagl/vitaGL/libvitaGL.a. Runs inside the vitasdk/vitasdk container (tools/docker_build.sh calls it).
#   docker run --rm -v "${PWD}:/src" -w /src vitasdk/vitasdk sh tools/build_vitagl.sh
# Base: the commit of the vitasdk package (vitaGL 0.0.0.r1488.g2bdbe89), plus the five patch scripts below and HAVE_SHADER_CACHE=1.
set -e
COMMIT=2bdbe895ddc6a7e410a9ea55a8f53d8173c61ad8
mkdir -p /src/build/vitagl
cd /src/build/vitagl
command -v git >/dev/null || (apt-get -qq update && apt-get -qq install -y git >/dev/null)
[ -d vitaGL ] || git clone -q https://github.com/Rinnegatamante/vitaGL.git
cd vitaGL
git checkout -q "$COMMIT"
command -v python3 >/dev/null || (apk add --no-cache python3 >/dev/null 2>&1 || (apt-get -qq update && apt-get -qq install -y python3 >/dev/null))
python3 -I /src/tools/vitagl_paircache.py /src/build/vitagl/vitaGL
python3 -I /src/tools/vitagl_gate.py /src/build/vitagl/vitaGL
python3 -I /src/tools/vitagl_rtpurge.py /src/build/vitagl/vitaGL
python3 -I /src/tools/vitagl_gcnow.py /src/build/vitagl/vitaGL
python3 -I /src/tools/vitagl_dsstore.py /src/build/vitagl/vitaGL
make clean >/dev/null 2>&1 || true
make NO_DEBUG=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1 SHARED_RENDERTARGETS=${VGL_SHARED_RT:-0} -j4 > ../build.log 2>&1
ls -l libvitaGL.a
echo "vitaGL $(git log --oneline -1) built with HAVE_SHADER_CACHE=1"
