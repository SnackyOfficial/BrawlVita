#!/usr/bin/env python3
"""Summarise a PS Vita crash dump (.psp2dmp) of the loader: which thread stopped where, and whether it stopped right after a Linux `svc #0`.

    python -I tools/psp2dmp.py <dump.psp2dmp> [--libg work/apk/lib/armeabi-v7a/libg.so] [--libg-base 0x99000000] [--libfmod-base 0x98000000] [--elf BrawlVita-m4-offline-FIXED.elf]

The dump is a gzip-compressed ELF core. Layout (from the dumps of the first hardware runs, checked against known values):
  PT_NOTE "THREAD_INFO"      header [version, count, entry size]; entry: [size, uid, name[32] ...]
  PT_NOTE "THREAD_REG_INFO"  header [version, count, entry size]; entry words: [size, uid, r0..r11, r12, sp, lr, pc, cpsr, ...]
Threads parked in the kernel have pc >= 0xe0000000; the thread of interest is the one with a user pc. With --libg the word in front of pc
is checked: `svc #0` there means the program counter was just past a Linux system call, which the console kernel does not understand
(r7 then holds the Linux call number). Only the PC/LR/SP of dumps are read; the tool never runs anything from the dump.
"""
import argparse
import gzip
import struct
import sys

SYSCALLS = {1: "exit", 3: "read", 4: "write", 5: "open", 6: "close", 20: "getpid", 45: "brk", 54: "ioctl", 78: "gettimeofday", 91: "munmap",
            120: "clone", 125: "mprotect", 162: "nanosleep", 172: "prctl", 192: "mmap2", 220: "getdents64", 221: "fcntl64", 240: "futex",
            248: "exit_group", 263: "clock_gettime", 281: "socket", 283: "connect", 290: "sendto", 292: "recvfrom", 322: "openat",
            360: "inotify_init1"}


def notes(d):
    (t, m, v, entry, phoff, shoff, flags, ehsize, phentsize, phnum, shentsize, shnum, shstr) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
    out = {}
    for i in range(phnum):
        pt, off, va, pa, fsz, msz, fl, al = struct.unpack_from("<8I", d, phoff + i * phentsize)
        if pt != 4:
            continue
        pos, end = off, min(off + fsz, len(d))  # a dump written while the app was going down can be shorter than its headers say
        if off + fsz > len(d):
            print("(note segment truncated: %d of %d bytes present)" % (max(0, len(d) - off), fsz))
        while pos + 12 <= end:
            nsz, dsz, ntype = struct.unpack_from("<III", d, pos)
            name = d[pos + 12:pos + 12 + nsz].rstrip(b"\0").decode(errors="replace")
            dpos = pos + 12 + ((nsz + 3) & ~3)
            out[name] = d[dpos:dpos + dsz]
            pos = dpos + ((dsz + 3) & ~3)
    return out


def table(blob):
    count, size = struct.unpack_from("<II", blob, 4)
    return [blob[8 + k * size:8 + (k + 1) * size] for k in range(count)]


_ELF_SYMS = []  # (start, size, name) of the loader's own functions, from --elf (the unstripped program the build leaves next to the VPK)


def load_elf_symbols(path):
    try:
        from elftools.elf.elffile import ELFFile
    except ImportError:
        print("(pip install pyelftools to read --elf)")
        return
    elf = ELFFile(open(path, "rb"))
    sec = elf.get_section_by_name(".symtab")
    if not sec:
        print("(the ELF has no symbol table: it is the stripped one)")
        return
    for sym in sec.iter_symbols():
        if sym["st_info"]["type"] == "STT_FUNC" and sym["st_value"] >= 0x81000000 and sym.name:  # the program is linked at 0x81000000; $t / $a mapping symbols are not functions
            _ELF_SYMS.append((sym["st_value"] & ~1, sym["st_size"], sym.name))
    _ELF_SYMS.sort()


def elf_name(addr, eboot_base=0x81000000):
    """The loader function that contains addr (an address in the running eboot), as name+offset; '' when unknown."""
    if not _ELF_SYMS or not (eboot_base <= addr < eboot_base + 0x800000):
        return ""
    a = addr & ~1
    best = None
    for start, size, name in _ELF_SYMS:
        if start <= a:
            best = (start, size, name)
        else:
            break
    return "%s+0x%x" % (best[2], a - best[0]) if best and (best[1] == 0 or a < best[0] + best[1] + 16) else ""


def where(addr, libg_base, libfmod_base):
    a = addr & ~1
    nm = elf_name(addr)
    if nm:
        return nm
    if libg_base <= a < libg_base + 0x700000:
        return "libg+0x%x" % (a - libg_base)
    if libfmod_base <= a < libfmod_base + 0x200000:
        return "libfmod+0x%x" % (a - libfmod_base)
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--libg")
    ap.add_argument("--libg-base", default="0x99000000")
    ap.add_argument("--libfmod-base", default="0x98000000")
    ap.add_argument("--elf", help="the unstripped program of the build (BrawlVita-m4-offline-FIXED.elf): names the loader / vitaGL / libc functions for pc and lr")
    a = ap.parse_args()
    if a.elf:
        load_elf_symbols(a.elf)
    gb, fb = int(a.libg_base, 0), int(a.libfmod_base, 0)
    raw = open(a.dump, "rb").read()
    d = gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw
    if d[:4] != b"\x7fELF":
        sys.exit("not an ELF core")
    n = notes(d)
    names = {}
    for e in table(n["THREAD_INFO"]):
        uid = struct.unpack_from("<I", e, 4)[0]
        names[uid] = e[8:40].split(b"\0")[0].decode(errors="replace")
    libg = open(a.libg, "rb").read() if a.libg else None
    found = 0
    for e in table(n["THREAD_REG_INFO"]):
        w = struct.unpack("<%dI" % (len(e) // 4), e)
        uid = w[1]
        regs = w[2:14]
        r12, sp, lr, pc, cpsr = w[14:19]
        kernel = pc >= 0xE0000000
        thumb = bool(cpsr & 0x20)
        print("%s thread 0x%08x %-22s pc=0x%08x %-14s lr=0x%08x %-14s sp=0x%08x cpsr=0x%08x %s" %
              (" " if kernel else ">", uid, names.get(uid, "?"), pc, where(pc, gb, fb), lr, where(lr, gb, fb), sp, cpsr, "(in kernel)" if kernel else ""))
        if kernel:
            continue
        found += 1
        print("      r0-r11 " + " ".join("%08x" % x for x in regs) + "  r12 %08x" % r12)
        if libg and gb <= pc < gb + len(libg):
            off = pc - gb
            if not thumb and off >= 4 and struct.unpack_from("<I", libg, off - 4)[0] == 0xEF000000:
                nr = regs[7]
                print("      stopped right after the ARM `svc #0` at libg+0x%x; r7 = %d (%s): a Linux system call run on the console kernel" %
                      (off - 4, nr, SYSCALLS.get(nr, "?")))
            elif thumb and off >= 2 and struct.unpack_from("<H", libg, (off & ~1) - 2)[0] == 0xDF00:
                nr = regs[7]
                print("      stopped right after the Thumb `svc #0` at libg+0x%x; r7 = %d (%s): a Linux system call run on the console kernel" %
                      ((off & ~1) - 2, nr, SYSCALLS.get(nr, "?")))
    if not found:
        print("no thread with a user-mode pc found")


if __name__ == "__main__":
    main()
