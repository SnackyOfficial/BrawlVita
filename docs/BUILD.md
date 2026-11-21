# Building BrawlVita

## With Docker (recommended)

1. Install Docker Desktop and start it.
2. From the repository folder run `build.bat` (Windows) or `./build.sh` (Linux / macOS).

The script runs `tools/docker_build.sh` inside the `vitasdk/vitasdk` image:

1. builds the kubridge import stub from `third_party/kubridge_stub/kubridge_nids.yml` (once),
2. clones vitaGL at a fixed commit and applies our patches (`tools/vitagl_*.py`) into `build/vitagl/` (once, a few minutes),
3. configures and builds with CMake/Ninja into `build/cmake/`,
4. checks that the patched vitaGL was linked, then copies `BrawlVita.vpk` (and the unstripped `BrawlVita.elf`) to the repository folder.

Delete the `build/` folder to rebuild everything from scratch.

## The installer

After `BrawlVita.vpk` exists, `installer/build_exe.bat` packs `BrawlVitaInstaller.exe` (needs Python 3 with Pillow and PyInstaller:
`py -3 -m pip install pillow pyinstaller`). The installer contains our VPK and code only.

## With a local VitaSDK

Set `VITASDK`, build the kubridge stub and the patched vitaGL the same way as `tools/docker_build.sh` does (copy
`build/vitagl/vitaGL/libvitaGL.a` over `$VITASDK/arm-vita-eabi/lib/libvitaGL.a`), then:

```
cmake -S . -B build/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake
```

## Host tests

The offline server, profile, bot world, shop and input logic are plain C11 and have tests that run on a PC:

```
docker run --rm -v "$(pwd):/src" -w /src vitasdk/vitasdk sh tests/run_host_tests.sh
```

## Crash dumps

If the game crashes, the Vita writes a `.psp2dmp` file to `ux0:data/`. `tools/psp2dmp.py` reads it together with `BrawlVita.elf`
from the same build:

```
python -I tools/psp2dmp.py <dump.psp2dmp> --elf BrawlVita.elf
```

Please attach the dump, `BrawlVita.elf` of your build and `ux0:data/BrawlVita/log.txt` to a bug report.
