#!/usr/bin/env python3
"""image-check.py — static structure validation for the compiler's native
images. The gate's crossings are build-only (executing a corrupt image can
wedge the kernel — the lore in tools/native-exec.sh), so structure is the
evidence there: this script parses the image head and load commands and
refuses anything a loader would reject. No execution, no external tools.

  image-check.py <image> <target>

Targets: arm64-mac (Mach-O 64) | amd64-linux | arm64-linux (ELF 64).
Exit 0 with a one-line summary, nonzero with the reason otherwise.
"""
import struct
import sys


def fail(msg):
    print(f"image-check: {msg}")
    sys.exit(1)


def leb(data, i):
    """ULEB128 at data[i] -> (value, next index)."""
    v = 0
    sh = 0
    while True:
        b = data[i]
        i += 1
        v |= (b & 0x7F) << sh
        sh += 7
        if b < 0x80:
            return v, i


def check_macho(d, path):
    if len(d) < 32:
        fail(f"{path}: too small for a Mach-O header ({len(d)} bytes)")
    magic, cputype, cpusub, filetype, ncmds, sizeofcmds, flags = struct.unpack_from(
        "<IiiIIII", d, 0
    )
    if magic != 0xfeedfacf:
        fail(f"{path}: bad Mach-O magic 0x{magic:08x} (want 0xfeedfacf)")
    if cputype != 0x0100000C:  # CPU_TYPE_ARM64
        fail(f"{path}: cputype 0x{cputype:x} is not ARM64 (0x0100000c)")
    if filetype != 2:  # MH_EXECUTE
        fail(f"{path}: filetype {filetype} is not MH_EXECUTE (2)")
    # walk the load commands; the cursor must land exactly at their end
    off = 32
    end = 32 + sizeofcmds
    if end > len(d):
        fail(f"{path}: sizeofcmds {sizeofcmds} runs past the file ({len(d)})")
    text = None
    entry = None
    seen = 0
    while off < end:
        if off + 8 > end:
            fail(f"{path}: truncated load command at 0x{off:x}")
        cmd, cmdsize = struct.unpack_from("<II", d, off)
        if cmdsize < 8 or off + cmdsize > end:
            fail(f"{path}: load command {seen} size {cmdsize} overruns the table")
        if cmd == 0x19:  # LC_SEGMENT_64
            name = d[off + 8 : off + 24].split(b"\0", 1)[0]
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", d, off + 24)
            if name == b"__TEXT":
                if fileoff != 0 or filesize < end:
                    fail(
                        f"{path}: __TEXT must cover the header (fileoff {fileoff}, "
                        f"filesize {filesize}, header table ends at {end})"
                    )
                if vmsize < filesize:
                    fail(f"{path}: __TEXT vmsize {vmsize} < filesize {filesize}")
                text = (vmaddr, vmsize)
        elif cmd == 0x80000028:  # LC_MAIN
            entryoff = struct.unpack_from("<Q", d, off + 8 + 8)[0]
            entry = ("main", entryoff)
        elif cmd == 0x5:  # LC_UNIXTHREAD
            # arm64 thread state: the pc is the 14th u64 of the payload
            words = struct.unpack_from("<16Q", d, off + 8)
            entry = ("unixthread", words[14])
        seen += 1
        off += cmdsize
    if off != end:
        fail(f"{path}: load commands end at 0x{off:x}, header says 0x{end:x}")
    if text is None:
        fail(f"{path}: no LC_SEGMENT_64 named __TEXT")
    if entry is None:
        fail(f"{path}: no entry point (LC_MAIN or LC_UNIXTHREAD)")
    kind, at = entry
    print(
        f"ok: Mach-O arm64, {seen} load commands, __TEXT at 0x{text[0]:x} "
        f"(vmsize {text[1]:#x}), entry({kind}) 0x{at:x}"
    )


def check_elf(d, path, machine_name, machine_id):
    if len(d) < 64:
        fail(f"{path}: too small for an ELF64 header ({len(d)} bytes)")
    if d[:4] != b"\x7fELF":
        fail(f"{path}: bad ELF magic {d[:4].hex()}")
    ei_class, ei_data, ei_ver = d[4], d[5], d[6]
    if ei_class != 2:
        fail(f"{path}: EI_CLASS {ei_class} is not ELFCLASS64 (2)")
    if ei_data != 1:
        fail(f"{path}: EI_DATA {ei_data} is not little-endian (1)")
    if ei_ver != 1:
        fail(f"{path}: EI_VERSION {ei_ver} is not 1")
    (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize,
     e_phentsize, e_phnum) = struct.unpack_from("<HHIQQQIHHH", d, 16)
    if e_type != 2:
        fail(f"{path}: e_type {e_type} is not ET_EXEC (2)")
    if e_machine != machine_id:
        fail(f"{path}: e_machine {e_machine} is not {machine_name} ({machine_id})")
    if e_phnum == 0:
        fail(f"{path}: no program headers")
    if e_phoff + e_phnum * e_phentsize > len(d):
        fail(f"{path}: program header table runs past the file")
    loads = []
    for k in range(e_phnum):
        p = e_phoff + k * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, _p_paddr, p_filesz, _p_memsz, _p_align = (
            struct.unpack_from("<IIQQQQQQ", d, p)
        )
        if p_type == 1:  # PT_LOAD
            loads.append((p_offset, p_filesz, p_vaddr))
    if not loads:
        fail(f"{path}: no PT_LOAD segment")
    if not any(off == 0 and filesz >= 64 for off, filesz, _ in loads):
        fail(f"{path}: no PT_LOAD covers the ELF header (offset 0)")
    if e_entry == 0:
        fail(f"{path}: entry point is 0")
    if not any(vaddr <= e_entry < vaddr + filesz for _, filesz, vaddr in loads):
        fail(f"{path}: entry 0x{e_entry:x} lies outside every PT_LOAD")
    print(
        f"ok: ELF64 {machine_name}, {len(loads)} PT_LOAD, entry 0x{e_entry:x}"
    )


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    path, target = sys.argv[1], sys.argv[2]
    try:
        with open(path, "rb") as f:
            d = f.read()
    except OSError as e:
        fail(f"{path}: {e}")
    if target == "arm64-mac":
        check_macho(d, path)
    elif target == "amd64-linux":
        check_elf(d, path, "x86-64", 62)
    elif target == "arm64-linux":
        check_elf(d, path, "aarch64", 183)
    else:
        fail(f"unknown target {target} (arm64-mac | amd64-linux | arm64-linux)")


if __name__ == "__main__":
    main()
