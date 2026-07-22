#!/usr/bin/env python3
"""Run opt-in smoke tests against locally installed PS4 titles."""

import argparse
import ctypes
import os
import pathlib
import shutil
import sys
import tempfile
import time


DEFAULT_TITLES = ("CUSA02456", "CUSA13032", "SLES50541")

StorageOpen = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
    ctypes.c_int, ctypes.POINTER(ctypes.c_int64))
StorageRead = ctypes.CFUNCTYPE(
    ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p,
    ctypes.c_size_t)
StorageWrite = ctypes.CFUNCTYPE(
    ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p,
    ctypes.c_size_t)
StorageClose = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                ctypes.c_int64)
StorageSeek = ctypes.CFUNCTYPE(
    ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64, ctypes.c_int64,
    ctypes.c_int)
StorageStatCallback = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)
StorageMkdir = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int)
StoragePath = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p)
StorageRename = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p)
StorageFlush = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                ctypes.c_int64)
StorageReadDir = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p,
    ctypes.c_size_t, ctypes.c_void_p)
StorageAtomicReplace = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p,
    ctypes.c_size_t)
StorageTruncate = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_int64, ctypes.c_uint64)


class StorageStat(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint64),
        ("allocated_size", ctypes.c_uint64),
        ("modified_time_ns", ctypes.c_uint64),
        ("mode", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
    ]


class StorageCallbacks(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_uint32), ("version", ctypes.c_uint32),
        ("open", StorageOpen), ("read", StorageRead),
        ("write", StorageWrite), ("close", StorageClose),
        ("seek", StorageSeek), ("stat", StorageStatCallback),
        ("mkdir", StorageMkdir), ("unlink", StoragePath),
        ("rename", StorageRename), ("flush", StorageFlush),
        ("readdir", StorageReadDir),
        ("atomic_replace", StorageAtomicReplace),
        ("cleanup", StoragePath), ("truncate", StorageTruncate),
    ]


class LocalStorage:
    def __init__(self, title_root):
        self.title_root = title_root.resolve()
        self.writable = pathlib.Path(tempfile.mkdtemp(prefix="qemu-shadps4-"))
        self.handles = {}
        self.next_handle = 1
        self.callbacks = self._callbacks()

    def close(self):
        for entry in self.handles.values():
            if entry[0] == "file":
                entry[1].close()
        self.handles.clear()
        shutil.rmtree(self.writable, ignore_errors=True)

    def resolve(self, virtual_path):
        path = virtual_path.decode("utf-8") if isinstance(
            virtual_path, bytes) else virtual_path
        parts = pathlib.PurePosixPath(path).parts
        if len(parts) < 4 or parts[0] != "/" or parts[1] != "titles":
            raise OSError(22, "invalid virtual path")
        title_id = parts[2]
        mount = parts[3]
        relative = parts[4:]
        if mount == "app0":
            base = (self.title_root / title_id).resolve()
            read_only = True
        else:
            base = (self.writable / title_id / mount).resolve()
            base.mkdir(parents=True, exist_ok=True)
            read_only = False
        resolved = base.joinpath(*relative).resolve()
        if resolved != base and base not in resolved.parents:
            raise OSError(13, "path escapes mount")
        return resolved, read_only

    @staticmethod
    def errno(error):
        return -int(getattr(error, "errno", None) or 5)

    @staticmethod
    def fill_stat(path, output):
        value = ctypes.cast(output, ctypes.POINTER(StorageStat)).contents
        stat = path.stat()
        value.size = stat.st_size
        value.allocated_size = stat.st_size
        value.modified_time_ns = stat.st_mtime_ns
        value.mode = stat.st_mode
        value.type = 4 if path.is_dir() else 8

    def _callbacks(self):
        owner = self

        @StorageOpen
        def open_file(_opaque, path, flags, _mode, output):
            try:
                local, read_only = owner.resolve(path)
                directory = bool(flags & 0x40000000) or local.is_dir()
                if directory:
                    entries = iter(local.iterdir())
                    value = ("dir", entries, local, read_only)
                else:
                    write = bool(flags & 3)
                    if write and read_only:
                        return -30
                    if flags & 0x200:
                        local.parent.mkdir(parents=True, exist_ok=True)
                    mode = "r+b" if write else "rb"
                    if write and not local.exists():
                        mode = "w+b"
                    file = local.open(mode)
                    if flags & 0x400:
                        file.truncate(0)
                    if flags & 8:
                        file.seek(0, os.SEEK_END)
                    value = ("file", file, local, read_only)
                handle = owner.next_handle
                owner.next_handle += 1
                owner.handles[handle] = value
                output[0] = handle
                return 0
            except OSError as error:
                return owner.errno(error)

        @StorageRead
        def read(_opaque, handle, output, size):
            try:
                entry = owner.handles[handle]
                if entry[0] != "file":
                    return -21
                data = entry[1].read(size)
                ctypes.memmove(output, data, len(data))
                return len(data)
            except (KeyError, OSError) as error:
                return -9 if isinstance(error, KeyError) else owner.errno(error)

        @StorageWrite
        def write(_opaque, handle, data, size):
            try:
                entry = owner.handles[handle]
                if entry[0] != "file":
                    return -21
                if entry[3]:
                    return -30
                return entry[1].write(ctypes.string_at(data, size))
            except (KeyError, OSError) as error:
                return -9 if isinstance(error, KeyError) else owner.errno(error)

        @StorageClose
        def close(_opaque, handle):
            entry = owner.handles.pop(handle, None)
            if entry is None:
                return -9
            if entry[0] == "file":
                entry[1].close()
            return 0

        @StorageSeek
        def seek(_opaque, handle, offset, whence):
            try:
                entry = owner.handles[handle]
                return entry[1].seek(offset, whence) if entry[0] == "file" else -21
            except (KeyError, OSError) as error:
                return -9 if isinstance(error, KeyError) else owner.errno(error)

        @StorageStatCallback
        def stat(_opaque, path, output):
            try:
                local, _read_only = owner.resolve(path)
                owner.fill_stat(local, output)
                return 0
            except OSError as error:
                return owner.errno(error)

        @StorageMkdir
        def mkdir(_opaque, path, _mode):
            try:
                local, read_only = owner.resolve(path)
                if read_only:
                    return -30
                local.mkdir(parents=True, exist_ok=False)
                return 0
            except OSError as error:
                return owner.errno(error)

        @StoragePath
        def unlink(_opaque, path):
            try:
                local, read_only = owner.resolve(path)
                if read_only:
                    return -30
                local.rmdir() if local.is_dir() else local.unlink()
                return 0
            except OSError as error:
                return owner.errno(error)

        @StorageRename
        def rename(_opaque, old_path, new_path):
            try:
                old, old_read_only = owner.resolve(old_path)
                new, new_read_only = owner.resolve(new_path)
                if old_read_only or new_read_only:
                    return -30
                old.replace(new)
                return 0
            except OSError as error:
                return owner.errno(error)

        @StorageFlush
        def flush(_opaque, handle):
            try:
                entry = owner.handles[handle]
                if entry[0] == "file":
                    entry[1].flush()
                return 0
            except (KeyError, OSError) as error:
                return -9 if isinstance(error, KeyError) else owner.errno(error)

        @StorageReadDir
        def readdir(_opaque, handle, name, name_size, output):
            try:
                entry = owner.handles[handle]
                if entry[0] != "dir":
                    return -20
                child = next(entry[1])
                encoded = child.name.encode("utf-8")
                if len(encoded) + 1 > name_size:
                    return -34
                ctypes.memmove(name, encoded + b"\0", len(encoded) + 1)
                owner.fill_stat(child, output)
                return 1
            except StopIteration:
                return 0
            except (KeyError, OSError) as error:
                return -9 if isinstance(error, KeyError) else owner.errno(error)

        @StorageAtomicReplace
        def atomic_replace(_opaque, path, data, size):
            try:
                local, read_only = owner.resolve(path)
                if read_only:
                    return -30
                local.parent.mkdir(parents=True, exist_ok=True)
                temporary = local.with_suffix(local.suffix + ".tmp")
                temporary.write_bytes(ctypes.string_at(data, size))
                temporary.replace(local)
                return 0
            except OSError as error:
                return owner.errno(error)

        @StoragePath
        def cleanup(_opaque, path):
            try:
                local, read_only = owner.resolve(path)
                if read_only:
                    return -30
                if local.exists():
                    shutil.rmtree(local) if local.is_dir() else local.unlink()
                return 0
            except OSError as error:
                return owner.errno(error)

        @StorageTruncate
        def truncate(_opaque, handle, size):
            try:
                entry = owner.handles[handle]
                if entry[0] != "file":
                    return -21
                if entry[3]:
                    return -30
                entry[1].truncate(size)
                return 0
            except (KeyError, OSError) as error:
                return -9 if isinstance(error, KeyError) else owner.errno(error)

        return StorageCallbacks(
            ctypes.sizeof(StorageCallbacks), 1, open_file, read, write,
            close, seek, stat, mkdir, unlink, rename, flush, readdir,
            atomic_replace, cleanup, truncate)


class TitleResult:
    def __init__(self, title_id):
        self.title_id = title_id
        self.frames = 0
        self.nonblack_frames = 0
        self.last_nonblack_pixels = 0
        self.width = 0
        self.height = 0
        self.step_result = 0
        self.status = 0


def load_library(path):
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(path.parent))
        msys = pathlib.Path("C:/msys64/ucrt64/bin")
        if msys.is_dir():
            os.add_dll_directory(str(msys))
    dll = ctypes.CDLL(str(path))
    dll.qemu_host_init.argtypes = (ctypes.c_int,
                                   ctypes.POINTER(ctypes.c_char_p))
    dll.qemu_host_init.restype = ctypes.c_int
    dll.qemu_host_main_loop_step.argtypes = (ctypes.c_bool,
                                             ctypes.POINTER(ctypes.c_int))
    dll.qemu_host_main_loop_step.restype = ctypes.c_int
    dll.qemu_host_request_shutdown.restype = ctypes.c_int
    dll.qemu_host_cleanup.restype = ctypes.c_int
    return dll


def run_title(dll, title_id, title_root, duration):
    result = TitleResult(title_id)
    storage = LocalStorage(title_root)
    video_type = ctypes.CFUNCTYPE(
        None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
        ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32)

    @video_type
    def video(_opaque, pixels, width, height, stride, _format):
        result.frames += 1
        result.width = width
        result.height = height
        data = ctypes.string_at(pixels, stride * height)
        nonblack = sum(
            1 for offset in range(0, len(data), 4)
            if data[offset] or data[offset + 1] or data[offset + 2])
        result.last_nonblack_pixels = nonblack
        if nonblack:
            result.nonblack_frames += 1
        print(f"{title_id}: frame={result.frames} size={width}x{height} "
              f"nonblack={nonblack}", flush=True)

    dll.qemu_host_register_video_callback.argtypes = (video_type,
                                                       ctypes.c_void_p)
    dll.qemu_host_register_video_callback(video, None)
    dll.qemu_host_register_storage_callbacks.argtypes = (
        ctypes.POINTER(StorageCallbacks), ctypes.c_void_p)
    dll.qemu_host_register_storage_callbacks(ctypes.byref(storage.callbacks),
                                              None)
    kernel = title_root / title_id / "eboot.bin"
    if not kernel.is_file():
        raise FileNotFoundError(kernel)
    args = [
        "qemu-system-shadps4", "-M",
        f"shadps4,variant=base,execute=on,title-id={title_id}",
        "-accel", "tcg,thread=single", "-smp", "8", "-m", "5248M",
        "-display", "host", "-audiodev", "xaudio2,id=shadps4",
        "-nodefaults", "-kernel", str(kernel),
    ]
    encoded = [arg.encode() for arg in args]
    argv = (ctypes.c_char_p * len(encoded))(*encoded)
    init_result = dll.qemu_host_init(len(encoded), argv)
    if init_result:
        raise RuntimeError(f"{title_id}: qemu_host_init={init_result}")
    status = ctypes.c_int()
    deadline = time.monotonic() + duration
    try:
        while time.monotonic() < deadline:
            step = dll.qemu_host_main_loop_step(True, ctypes.byref(status))
            if step:
                result.step_result = step
                break
    finally:
        dll.qemu_host_request_shutdown()
        while dll.qemu_host_main_loop_step(False, ctypes.byref(status)) == 0:
            pass
        cleanup_result = dll.qemu_host_cleanup()
        storage.close()
        if cleanup_result:
            raise RuntimeError(f"{title_id}: cleanup={cleanup_result}")
    result.status = status.value
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dll", type=pathlib.Path)
    parser.add_argument("--title-root", type=pathlib.Path,
                        default=pathlib.Path("C:/Users/rodri/Dev1/Projetos"))
    parser.add_argument("--title", action="append", dest="titles",
                        choices=DEFAULT_TITLES)
    parser.add_argument("--duration", type=float, default=20.0)
    args = parser.parse_args()
    titles = args.titles or DEFAULT_TITLES
    failed = False
    for title_id in titles:
        dll = load_library(args.dll.resolve())
        try:
            result = run_title(dll, title_id, args.title_root, args.duration)
            print(f"{title_id}: frames={result.frames} "
                  f"nonblack_frames={result.nonblack_frames} "
                  f"step={result.step_result} status={result.status}")
            reasons = []
            if result.step_result:
                reasons.append(f"main loop stopped early ({result.step_result})")
            if result.status:
                reasons.append(f"guest status is {result.status}")
            if not result.frames:
                reasons.append("no video frames")
            elif not result.nonblack_frames:
                reasons.append("all video frames are black")
            if reasons:
                failed = True
                print(f"{title_id}: FAIL: {'; '.join(reasons)}",
                      file=sys.stderr)
        except Exception as error:
            failed = True
            print(f"{title_id}: FAIL: {error}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
