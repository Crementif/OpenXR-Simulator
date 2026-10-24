#!/usr/bin/env python3
"""Write a separate, build-checked copy with FFX custom-present type guards."""

import argparse
import hashlib
from pathlib import Path
import struct


SOURCE_SHA256 = "8b3f942dfda29a6bbb59c553eaa725b2013b96bb335f31c9908c0e798f07b19e"
FFX_VTABLE = 0x70B74B8
# Each site calls FRHIViewport::GetCustomPresent, then consumes its result.
# The null-only check used by the plugin also accepts an OpenXR custom present.
SITES = {
    0x4FE625F: "ff50304c8be8",
    0x4FE6783: "ff5030488bd8",
    0x4FEA92F: "ff5030488bf0",
    0x4FF11BF: "ff50304885c0",
    0x4FF5A3F: "ff50304c8bf0",
    0x4FF85B2: "ff50308b9354010000",
    0x4FF9152: "ff50304c8bf8",
}


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def patch(source):
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA256:
        raise ValueError("Unsupported executable build; no output was written")
    data = bytearray(source)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    section_table = optional + optional_size
    if machine != 0x8664 or struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("Expected an x64 PE32+ executable")
    section_alignment, file_alignment = struct.unpack_from("<II", data, optional + 32)
    sections = [
        struct.unpack_from("<IIII", data, section_table + 40 * index + 8)
        for index in range(count)
    ]

    def offset(rva):
        for _, address, raw_size, raw_offset in sections:
            if address <= rva < address + raw_size:
                return raw_offset + rva - address
        raise ValueError(f"Unbacked image address: {rva:#x}")

    exception_directory = optional + 112 + 3 * 8
    exception_rva, exception_size = struct.unpack_from("<II", data, exception_directory)
    entries = [
        struct.unpack_from("<III", data, offset(exception_rva) + index)
        for index in range(0, exception_size, 12)
    ]
    address = align(
        max(va + max(virtual_size, raw_size) for virtual_size, va, raw_size, _ in sections),
        section_alignment,
    )
    raw_offset = align(len(data), file_alignment)
    body = bytearray()
    added_entries = []
    for site, expected in SITES.items():
        original = bytes.fromhex(expected)
        if data[offset(site):offset(site) + len(original)] != original:
            raise ValueError(f"Instruction mismatch at {site:#x}")
        body.extend(b"\xcc" * (align(len(body), 16) - len(body)))
        stub = address + len(body)
        code = bytearray(original[:3])  # Original virtual call, with its original stack.
        code.extend(bytes.fromhex("4885c0740e"))  # test rax,rax; jz accepted
        lea = stub + len(code)
        code.extend(bytes.fromhex("4c8d1d"))  # lea r11,[rip + FFX vtable]
        code.extend(struct.pack("<i", FFX_VTABLE - (lea + 7)))
        # cmp [rax],r11; je accepted; xor eax,eax. R11 is caller-saved.
        code.extend(bytes.fromhex("4c3918740231c0"))
        code.extend(original[3:])
        jump = stub + len(code)
        code.extend(b"\xe9" + struct.pack("<i", site + len(original) - (jump + 5)))
        body.extend(code)
        end = address + len(body)
        body.extend(bytes(align(len(body), 4) - len(body)))
        unwind = address + len(body)
        parent = next(entry for entry in entries if entry[0] <= site < entry[1])
        # This separated code uses the original function's already established frame.
        # Chain its unwind record instead of treating the stub as a leaf function.
        frame_register = data[offset(parent[2]) + 3]
        body.extend(bytes([0x21, 0, 0, frame_register]))
        body.extend(struct.pack("<III", *parent))
        added_entries.append((stub, end, unwind))
        replacement = b"\xe9" + struct.pack("<i", stub - (site + 5))
        replacement += b"\x90" * (len(original) - 5)
        data[offset(site):offset(site) + len(original)] = replacement

    body.extend(bytes(align(len(body), 4) - len(body)))
    new_exception_rva = address + len(body)
    for entry in sorted(entries + added_entries):
        body.extend(struct.pack("<III", *entry))
    if section_table + (count + 1) * 40 > min(raw for _, _, size, raw in sections if size):
        raise ValueError("No room for the compatibility section header")
    raw_size = align(len(body), file_alignment)
    header = struct.pack(
        "<8sIIIIIIHHI", b".simxr\0\0", len(body), address, raw_size, raw_offset,
        0, 0, 0, 0, 0x60000020,
    )
    data[section_table + count * 40:section_table + (count + 1) * 40] = header
    struct.pack_into("<H", data, pe + 6, count + 1)
    struct.pack_into("<I", data, optional + 56, align(address + len(body), section_alignment))
    struct.pack_into("<II", data, exception_directory, new_exception_rva,
                     12 * (len(entries) + len(added_entries)))
    struct.pack_into("<I", data, optional + 64, 0)
    code_size = struct.unpack_from("<I", data, optional + 4)[0]
    struct.pack_into("<I", data, optional + 4, code_size + raw_size)
    data.extend(bytes(raw_offset - len(data)))
    data.extend(body)
    data.extend(bytes(raw_size - len(body)))
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Original TheMidnightWalk-Win64-Shipping.exe")
    parser.add_argument("output", type=Path, help="New executable path; must not already exist")
    args = parser.parse_args()
    try:
        result = patch(args.source.read_bytes())
        # Exclusive creation also prevents overwriting the source or an existing copy.
        with args.output.open("xb") as output:
            output.write(result)
        print(f"Wrote seven presenter guards: {args.output}")
        print(f"SHA256: {hashlib.sha256(result).hexdigest()}")
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
