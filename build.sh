#!/bin/sh
# Builds BrawlVita.vpk with Docker (the first run downloads the vitasdk image, about 1.6 GB).
set -e
cd "$(dirname "$0")"
docker run --rm -v "$(pwd):/src" -w /src vitasdk/vitasdk /bin/bash -c "tr -d '\r' < tools/docker_build.sh | bash"
echo "DONE. New VPK: $(pwd)/BrawlVita.vpk"
