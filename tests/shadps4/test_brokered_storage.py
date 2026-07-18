#!/usr/bin/env python3
"""Focused smoke test for the brokered-storage embedding ABI."""

import argparse
import ctypes
import errno
import os
import struct
import time
from pathlib import Path


Retain = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)
Release = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)
OpenFile = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
    ctypes.c_int, ctypes.POINTER(ctypes.c_int64))
OpenAt = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
    ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int64))
Read = ctypes.CFUNCTYPE(
    ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p,
    ctypes.c_size_t)
Write = ctypes.CFUNCTYPE(
    ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p,
    ctypes.c_size_t)
Seek = ctypes.CFUNCTYPE(
    ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64, ctypes.c_int64,
    ctypes.c_int)
Close = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int64)
StatFile = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
    ctypes.c_void_p)
StatAt = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
    ctypes.c_void_p)
MkdirAt = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
    ctypes.c_int)
PathAt = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p)
RenameAt = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
    ctypes.c_char_p)
Flush = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int64)
ReadDir = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p,
    ctypes.c_size_t, ctypes.c_void_p)
AtomicReplaceAt = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
    ctypes.c_void_p, ctypes.c_size_t)
Truncate = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_int64, ctypes.c_uint64)


class BrokeredCallbacks(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("retain", Retain),
        ("release", Release),
        ("open_file", OpenFile),
        ("open_at", OpenAt),
        ("read", Read),
        ("write", Write),
        ("seek", Seek),
        ("close", Close),
        ("stat_file", StatFile),
        ("stat_at", StatAt),
        ("mkdir_at", MkdirAt),
        ("unlink_at", PathAt),
        ("rename_at", RenameAt),
        ("flush", Flush),
        ("readdir", ReadDir),
        ("atomic_replace_at", AtomicReplaceAt),
        ("cleanup_at", PathAt),
        ("truncate", Truncate),
    ]


class FaultInfo(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("vector", ctypes.c_uint32),
        ("error_code_valid", ctypes.c_uint32),
        ("instruction_size", ctypes.c_uint32),
        ("cpu_index", ctypes.c_uint32),
        ("error_code", ctypes.c_uint64),
        ("rip", ctypes.c_uint64),
        ("rsp", ctypes.c_uint64),
        ("rbp", ctypes.c_uint64),
        ("cr2", ctypes.c_uint64),
        ("cr3", ctypes.c_uint64),
        ("rflags", ctypes.c_uint64),
        ("return_address", ctypes.c_uint64),
        ("last_hle_number", ctypes.c_uint64),
        ("last_hle_args", ctypes.c_uint64 * 7),
        ("guest_thread_id", ctypes.c_uint64),
        ("guest_process_id", ctypes.c_uint64),
        ("instruction", ctypes.c_uint8 * 16),
        ("exception_type", ctypes.c_char * 32),
        ("image", ctypes.c_char * 128),
        ("last_hle_nid", ctypes.c_char * 12),
        ("last_hle_library", ctypes.c_char * 64),
        ("last_hle_module", ctypes.c_char * 64),
        ("last_hle_result", ctypes.c_int64),
        ("rax", ctypes.c_uint64),
        ("rbx", ctypes.c_uint64),
        ("rcx", ctypes.c_uint64),
        ("rdx", ctypes.c_uint64),
        ("rsi", ctypes.c_uint64),
        ("rdi", ctypes.c_uint64),
        ("r8", ctypes.c_uint64),
        ("r9", ctypes.c_uint64),
        ("r10", ctypes.c_uint64),
        ("r11", ctypes.c_uint64),
        ("r12", ctypes.c_uint64),
        ("r13", ctypes.c_uint64),
        ("r14", ctypes.c_uint64),
        ("r15", ctypes.c_uint64),
    ]


def minimal_orbis_elf():
    image = bytearray(0x1001)
    ident = bytearray(16)
    ident[:4] = b"\x7fELF"
    ident[4:7] = bytes((2, 1, 1))
    ident[7] = 9
    image[:16] = ident
    struct.pack_into("<HHIQQQIHHHHHH", image, 16,
                     0xFE10, 62, 1, 0x1000, 64, 0, 0,
                     64, 56, 1, 0, 0, 0)
    struct.pack_into("<IIQQQQQQ", image, 64,
                     1, 5, 0x1000, 0x1000, 0, 1, 1, 0x1000)
    image[0x1000] = 0xC3
    return bytes(image)


def run(dll_path):
    if os.name != "nt":
        raise RuntimeError("the DLL smoke test requires Windows")

    os.add_dll_directory(str(dll_path.parent))
    msys_bin = Path("C:/msys64/ucrt64/bin")
    if msys_bin.is_dir():
        os.add_dll_directory(str(msys_bin))
    qemu = ctypes.CDLL(str(dll_path))

    qemu.qemu_host_register_brokered_storage_callbacks.argtypes = (
        ctypes.POINTER(BrokeredCallbacks), ctypes.c_void_p)
    qemu.qemu_host_register_brokered_storage_callbacks.restype = ctypes.c_int
    qemu.qemu_host_mount_brokered_folder.argtypes = (
        ctypes.c_char_p, ctypes.c_void_p)
    qemu.qemu_host_mount_brokered_folder.restype = ctypes.c_int
    qemu.qemu_host_mount_brokered_file.argtypes = (
        ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p)
    qemu.qemu_host_mount_brokered_file.restype = ctypes.c_int
    qemu.qemu_host_init.argtypes = (
        ctypes.c_int, ctypes.POINTER(ctypes.c_char_p))
    qemu.qemu_host_init.restype = ctypes.c_int
    qemu.qemu_host_get_api_version.restype = ctypes.c_uint32
    qemu.qemu_host_request_shutdown.restype = ctypes.c_int
    qemu.qemu_host_main_loop_step.argtypes = (
        ctypes.c_bool, ctypes.POINTER(ctypes.c_int))
    qemu.qemu_host_main_loop_step.restype = ctypes.c_int
    qemu.qemu_host_cleanup.restype = ctypes.c_int
    qemu.qemu_host_get_last_fault.argtypes = (ctypes.POINTER(FaultInfo),)
    qemu.qemu_host_get_last_fault.restype = ctypes.c_int

    files = {"eboot.bin": minimal_orbis_elf()}
    handles = {}
    opened = []
    retained = []
    released = []
    next_handle = 1

    @Retain
    def retain(_opaque, obj):
        retained.append(obj)

    @Release
    def release(_opaque, obj):
        released.append(obj)

    @OpenFile
    def open_file(_opaque, _file, _stream, _flags, handle):
        nonlocal next_handle
        opened.append("<mounted-file>")
        handles[next_handle] = [files["eboot.bin"], 0, False]
        handle[0] = next_handle
        next_handle += 1
        return 0

    @OpenAt
    def open_at(_opaque, _folder, relative, flags, _mode, handle):
        nonlocal next_handle
        path = relative.decode("utf-8")
        opened.append(path)
        if path == "sce_module" and flags & 0x40000000:
            handles[next_handle] = [b"", 0, True]
        elif path in files:
            handles[next_handle] = [files[path], 0, False]
        else:
            return -2
        handle[0] = next_handle
        next_handle += 1
        return 0

    @Read
    def read(_opaque, handle, buffer, size):
        data, position, is_directory = handles[handle]
        if is_directory:
            return -21
        chunk = data[position:position + size]
        ctypes.memmove(buffer, chunk, len(chunk))
        handles[handle][1] += len(chunk)
        return len(chunk)

    @Write
    def write(_opaque, _handle, _buffer, _size):
        return -30

    @Seek
    def seek(_opaque, handle, offset, whence):
        data, position, is_directory = handles[handle]
        if is_directory:
            return -21
        origins = {0: 0, 1: position, 2: len(data)}
        if whence not in origins or origins[whence] + offset < 0:
            return -22
        handles[handle][1] = origins[whence] + offset
        return handles[handle][1]

    @Close
    def close(_opaque, handle):
        return 0 if handles.pop(handle, None) is not None else -9

    @ReadDir
    def readdir(_opaque, handle, _name, _name_size, _stat):
        return 0 if handles[handle][2] else -20

    callbacks = BrokeredCallbacks(
        ctypes.sizeof(BrokeredCallbacks), 1, retain, release, open_file,
        open_at, read, write, seek, close, StatFile(), StatAt(), MkdirAt(),
        PathAt(), RenameAt(), Flush(), readdir, AtomicReplaceAt(), PathAt(),
        Truncate())

    assert qemu.qemu_host_register_brokered_storage_callbacks(
        ctypes.byref(callbacks), None) == 0
    assert qemu.qemu_host_get_api_version() == 0x00010001
    folder = ctypes.c_void_p(0x1234)
    storage_file = ctypes.c_void_p(0x2345)
    stream = ctypes.c_void_p(0x3456)
    assert qemu.qemu_host_mount_brokered_folder(
        b"/brokered/title", folder) == 0
    assert qemu.qemu_host_mount_brokered_folder(b"/title", folder) == 0
    assert qemu.qemu_host_mount_brokered_file(
        b"/brokered/title/eboot.bin", storage_file, stream) == 0

    arguments = [
        "qemu-system-shadps4", "-M",
        "shadps4,variant=base,execute=off,title-id=TEST00001",
        "-accel", "tcg,thread=single", "-smp", "8", "-m", "64M",
        "-audiodev", "none,id=shadps4", "-display", "none",
        "-nodefaults", "-kernel", "/brokered/title/eboot.bin",
    ]
    encoded = [argument.encode("utf-8") for argument in arguments]
    argv = (ctypes.c_char_p * len(encoded))(*encoded)
    assert qemu.qemu_host_init(len(encoded), argv) == 0
    fault = FaultInfo(size=ctypes.sizeof(FaultInfo), version=1)
    assert qemu.qemu_host_get_last_fault(ctypes.byref(fault)) == -errno.ENOENT
    assert "<mounted-file>" in opened
    assert "sce_module" in opened
    assert qemu.qemu_host_request_shutdown() == 0
    status = ctypes.c_int()
    for _ in range(1000):
        if qemu.qemu_host_main_loop_step(True, ctypes.byref(status)) == 1:
            break
        time.sleep(0.001)
    else:
        raise AssertionError("brokered-storage worker did not stop")
    assert qemu.qemu_host_cleanup() == 0
    assert retained == [folder.value, folder.value,
                        storage_file.value, stream.value]
    assert sorted(released) == sorted(retained)
    assert not handles


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dll", type=Path, required=True)
    args = parser.parse_args()
    run(args.dll.resolve())


if __name__ == "__main__":
    main()
