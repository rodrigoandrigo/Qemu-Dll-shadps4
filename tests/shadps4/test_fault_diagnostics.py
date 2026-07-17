#!/usr/bin/env python3
"""Exercise the structured shadPS4 fault snapshot with a synthetic #UD."""

import argparse
import ctypes
import os
import struct
import time
from pathlib import Path

from test_brokered_storage import FaultInfo


def fault_elf(code):
    image = bytearray(0x1000 + len(code))
    ident = bytearray(16)
    ident[:4] = b"\x7fELF"
    ident[4:7] = bytes((2, 1, 1))
    ident[7] = 9
    image[:16] = ident
    struct.pack_into("<HHIQQQIHHHHHH", image, 16,
                     0xFE10, 62, 1, 0x1000, 64, 0, 0,
                     64, 56, 1, 0, 0, 0)
    struct.pack_into("<IIQQQQQQ", image, 64,
                     1, 5, 0x1000, 0x1000, 0, len(code), len(code), 0x1000)
    image[0x1000:] = code
    return bytes(image)


def run(dll_path, image_path, kind):
    os.add_dll_directory(str(dll_path.parent))
    msys_bin = Path("C:/msys64/ucrt64/bin")
    if msys_bin.is_dir():
        os.add_dll_directory(str(msys_bin))
    qemu = ctypes.CDLL(str(dll_path))
    qemu.qemu_host_init.argtypes = (
        ctypes.c_int, ctypes.POINTER(ctypes.c_char_p))
    qemu.qemu_host_init.restype = ctypes.c_int
    qemu.qemu_host_main_loop_step.argtypes = (
        ctypes.c_bool, ctypes.POINTER(ctypes.c_int))
    qemu.qemu_host_main_loop_step.restype = ctypes.c_int
    qemu.qemu_host_get_last_fault.argtypes = (ctypes.POINTER(FaultInfo),)
    qemu.qemu_host_get_last_fault.restype = ctypes.c_int
    qemu.qemu_host_cleanup.restype = ctypes.c_int

    rbx_sentinel = 0x1122334455667788
    if kind == "invalid-opcode":
        code = b"\x0f\x0b"
        expected_instruction = code
        expected_vector = 6
    elif kind == "page-fault":
        code = b"\x48\xa1" + bytes(8)
        expected_instruction = code
        expected_vector = 14
    else:
        code = (b"\x48\xbb" + struct.pack("<Q", rbx_sentinel) +
                b"\xb8\x14\x00\x00\x00\x0f\x05\x0f\x0b")
        expected_instruction = b"\x0f\x0b"
        expected_vector = 6
    image_path.write_bytes(fault_elf(code))
    arguments = [
        "qemu-system-shadps4", "-M",
        "shadps4,variant=base,execute=on,title-id=TEST00002",
        "-accel", "tcg,thread=single", "-smp", "8", "-m", "64M",
        "-audiodev", "none,id=shadps4", "-display", "none",
        "-nodefaults", "-kernel", str(image_path),
    ]
    encoded = [argument.encode("utf-8") for argument in arguments]
    argv = (ctypes.c_char_p * len(encoded))(*encoded)
    try:
        assert qemu.qemu_host_init(len(encoded), argv) == 0
        status = ctypes.c_int()
        for _ in range(2000):
            if qemu.qemu_host_main_loop_step(True, ctypes.byref(status)) == 1:
                break
            time.sleep(0.001)
        else:
            raise AssertionError("synthetic invalid-opcode guest did not stop")

        fault = FaultInfo(size=ctypes.sizeof(FaultInfo), version=1)
        assert qemu.qemu_host_get_last_fault(ctypes.byref(fault)) == 0
        assert fault.vector == expected_vector
        expected_type = b"page-fault" if kind == "page-fault" else \
                        b"invalid-opcode"
        assert fault.exception_type.rstrip(b"\0") == expected_type
        assert fault.error_code_valid == (kind == "page-fault")
        expected_rip = 0x800001000 + (len(code) - 2 if
                                      kind == "syscall-registers" else 0)
        assert fault.rip == expected_rip
        assert bytes(fault.instruction[:len(expected_instruction)]) == \
               expected_instruction
        assert fault.instruction_size == 16
        assert fault.guest_process_id == 1
        assert fault.guest_thread_id == 1
        if kind == "syscall-registers":
            assert fault.last_hle_number == 20
            assert fault.last_hle_result == 1
            assert fault.rax == 1
            assert fault.rbx == rbx_sentinel
        assert qemu.qemu_host_cleanup() == 0
    finally:
        image_path.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--kind", choices=("invalid-opcode", "page-fault",
                                           "syscall-registers"),
                        default="invalid-opcode")
    args = parser.parse_args()
    run(args.dll.resolve(), args.image.resolve(), args.kind)


if __name__ == "__main__":
    main()
