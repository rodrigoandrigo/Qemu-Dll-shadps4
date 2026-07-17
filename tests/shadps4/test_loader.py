#!/usr/bin/env python3
"""Functional smoke test for the shadPS4 ELF loader and TCG bootstrap."""

import argparse
import ctypes
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


VIRTUAL_BASE = 0x800000000
TLS_BASE = 0x7FFC00000
TCB_BASE = TLS_BASE + 0x40


def pack_into(image, offset, fmt, *values):
    struct.pack_into("<" + fmt, image, offset, *values)


def symbol_nid(name):
    salt = bytes.fromhex("518d64a635ded8c1e6b039b1c3e55230")
    value = int.from_bytes(hashlib.sha1(name.encode() + salt).digest()[:8],
                           "little")
    codes = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
    return "".join(codes[(value >> (58 - index * 6)) & 0x3F]
                   for index in range(10)) + codes[(value & 0xF) * 4]


def make_relocated_elf(path):
    image = bytearray(0x3000)
    image[:16] = b"\x7fELF\x02\x01\x01\x09" + b"\0" * 8
    pack_into(image, 16, "HHIQQQIHHHHHH", 0xFE10, 62, 1, 0x1000,
              64, 0, 0, 64, 56, 4, 0, 0, 0)

    phdr = 64
    pack_into(image, phdr, "IIQQQQQQ", 1, 7, 0x1000, 0x1000, 0,
              0x2000, 0x3000, 0x1000)
    pack_into(image, phdr + 56, "IIQQQQQQ", 2, 4, 0x200, 0, 0,
              0x90, 0x90, 8)
    pack_into(image, phdr + 112, "IIQQQQQQ", 0x61000000, 4, 0x400,
              0, 0, 0x100, 0x100, 8)
    pack_into(image, phdr + 168, "IIQQQQQQ", 7, 4, 0x2900, 0x2900, 0,
              8, 0x40, 0x20)

    tags = (
        (0x61000035, 0), (0x61000037, 1),
        (0x61000039, 0x20), (0x6100003B, 24),
        (0x6100003F, 24), (0x6100002F, 0x80),
        (0x61000031, 24), (0x61000033, 24), (0, 0),
    )
    for index, entry in enumerate(tags):
        pack_into(image, 0x200 + index * 16, "QQ", *entry)

    pack_into(image, 0x480, "QQq", 0x2800, 8, 0x1234)
    pack_into(image, 0x2900, "Q", 0x1122334455667788)
    image[0x2980:0x2988] = b"/dev/gc\0"
    image[0x29A0:0x29A9] = b"/dev/pad\0"
    image[0x29B0:0x29BE] = b"/dev/audioout\0"
    image[0x29C0:0x29CD] = b"/dev/audioin\0"
    image[0x29E0:0x29EF] = b"/data/test.bin\0"
    image[0x2A00:0x2A0E] = b"/data/out.bin\0"
    image[0x2200:0x220F] = b"/app0/game.bin\0"
    image[0x2220:0x222D] = b"/data/newdir\0"
    image[0x2240:0x2252] = b"/data/renamed.bin\0"
    image[0x2260:0x226B] = b"/data/list\0"
    image[0x2280:0x228C] = b"/dev/dialog\0"
    image[0x22A0:0x22B1] = b"/data/atomic.bin\0"
    code = bytearray()
    failure_branches = []

    def emit(hex_bytes):
        code.extend(bytes.fromhex(hex_bytes))

    def jne_failure():
        emit("0f 85 00 00 00 00")
        failure_branches.append(len(code) - 4)

    def syscall(number):
        code.extend(b"\xb8" + struct.pack("<I", number) + b"\x0f\x05")

    def expect_rax_small(value):
        emit(f"48 83 f8 {value & 0xff:02x}")
        jne_failure()

    emit("66 8c c8 66 83 f8 23")       # mov ax,cs; cmp ax,0x23
    jne_failure()
    emit("31 c0 0f a2")                # CPUID vendor: AuthenticAMD
    emit("81 fb 41 75 74 68")
    jne_failure()
    emit("81 fa 65 6e 74 69")
    jne_failure()
    emit("81 f9 63 41 4d 44")
    jne_failure()
    emit("b8 01 00 00 00 0f a2")      # Family 16h, model 00h, stepping 1
    emit("3d 01 0f 10 00")
    jne_failure()
    emit("c1 eb 10 80 fb 08")          # Eight logical processors
    jne_failure()
    emit("65 48 a1 00 00 00 00 00 00 00 00")  # mov rax,gs:[0]
    emit("48 b9 40 00 c0 ff 07 00 00 00")       # mov rcx,TCB_BASE
    emit("48 39 c8")
    jne_failure()
    emit("65 48 8b 04 25 c0 ff ff ff")  # mov rax,gs:[-0x40]
    emit("48 b9 88 77 66 55 44 33 22 11")
    emit("48 39 c8")
    jne_failure()
    emit("48 83 3f 01")                # cmp qword ptr [rdi],1
    jne_failure()
    emit("48 ba 00 ff df ff 07 00 00 00")  # mov rdx,exit trampoline
    emit("48 39 d6")                    # cmp rsi,rdx
    jne_failure()
    emit("48 a1 00 28 00 00 08 00 00 00")
    emit("48 b9 34 12 00 00 08 00 00 00")
    emit("48 39 c8")
    jne_failure()
    syscall(24)                         # getuid
    expect_rax_small(0)
    emit("48 bf 20 28 00 00 08 00 00 00")  # thr_self output
    syscall(432)
    expect_rax_small(0)
    emit("48 a1 20 28 00 00 08 00 00 00")
    expect_rax_small(1)
    emit("bf 04 00 00 00")             # CLOCK_MONOTONIC
    emit("48 be 30 28 00 00 08 00 00 00")
    syscall(232)                        # clock_gettime
    expect_rax_small(0)
    emit("31 ff be 00 00 20 00 ba 03 00 00 00 45 31 d2")
    syscall(477)                        # mmap 2 MiB
    emit("48 89 c3")                    # preserve mapping in rbx
    emit("48 b9 00 00 00 00 10 00 00 00")
    emit("48 39 c8")
    jne_failure()
    emit("48 ba 78 56 34 12 00 00 00 00 48 89 13")
    emit("48 8b 03 48 39 d0")
    jne_failure()
    emit("48 bf 80 29 00 00 08 00 00 00")  # /dev/gc
    emit("31 f6 31 d2")
    syscall(5)                          # open
    emit("49 89 c4")                    # preserve fd in r12
    expect_rax_small(3)
    emit("48 8d 83 00 02 00 00")      # PM4 command address
    emit("48 89 83 00 01 00 00")
    emit("48 c7 83 08 01 00 00 02 00 00 00")  # 2 dwords, queue 0
    emit("48 8d 83 00 03 00 00")       # fence address
    emit("48 89 83 10 01 00 00")
    emit("48 c7 83 18 01 00 00 55 00 00 00")  # fence value
    emit("c7 83 00 02 00 00 00 10 00 c0")     # type-3 NOP
    emit("c7 83 04 02 00 00 00 00 00 00")
    emit("4c 89 e7 be 01 81 10 c0 48 8d 93 00 01 00 00")
    syscall(54)                         # structured GPU submit
    expect_rax_small(0)
    emit("48 83 bb 00 03 00 00 55")   # fence completed
    jne_failure()
    emit("c7 83 08 01 00 00 03 00 00 00")  # append invalid PM4 header
    emit("4c 89 e7 be 01 81 10 c0 48 8d 93 00 01 00 00")
    syscall(54)                         # reject without partial PM4 commit
    expect_rax_small(-22)
    emit("c7 83 08 01 00 00 02 00 00 00")
    emit("48 b8 00 00 00 00 00 40 00 00")      # inaccessible fence
    emit("48 89 83 10 01 00 00")
    emit("4c 89 e7 be 01 81 10 c0 48 8d 93 00 01 00 00")
    syscall(54)                         # reject without submission commit
    expect_rax_small(-22)
    emit("48 8d 83 00 10 00 00")      # framebuffer address
    emit("48 89 83 00 04 00 00")
    emit("48 b8 02 00 00 00 02 00 00 00")    # width=2, height=2
    emit("48 89 83 08 04 00 00")
    emit("48 b8 08 00 00 00 01 00 00 00")    # stride=8, BGRA
    emit("48 89 83 10 04 00 00")
    emit("48 b8 00 11 22 33 44 55 66 77")
    emit("48 89 83 00 10 00 00")
    emit("48 b8 88 99 aa bb cc dd ee ff")
    emit("48 89 83 08 10 00 00")
    emit("4c 89 e7 be 02 81 10 c0 48 8d 93 00 04 00 00")
    syscall(54)                         # structured framebuffer flip
    expect_rax_small(0)
    emit("4c 89 e7 be 03 81 30 80 48 8d 93 00 05 00 00")
    syscall(54)                         # complete GPU status
    expect_rax_small(0)
    emit("48 83 bb 00 05 00 00 01")   # submit_count == 1
    jne_failure()
    emit("48 83 bb 08 05 00 00 01")   # flip_count == 1
    jne_failure()
    emit("48 83 bb 18 05 00 00 01")   # parsed_packet_count == 1
    jne_failure()
    emit("48 83 bb 20 05 00 00 02")   # rejected_submit_count == 2
    jne_failure()
    emit("48 83 bb 28 05 00 00 55")   # last_fence_value == 0x55
    jne_failure()
    emit("4c 89 e7")
    syscall(6)                          # close
    expect_rax_small(0)
    emit("48 bf a0 29 00 00 08 00 00 00")  # /dev/pad
    emit("31 f6 31 d2")
    syscall(5)
    emit("49 89 c4")
    expect_rax_small(3)
    emit("4c 89 e7 48 8d b3 00 12 00 00 ba 68 00 00 00")
    syscall(3)                          # read complete pad state
    expect_rax_small(104)
    emit("81 bb 00 12 00 00 5a 5a a5 a5")  # host gamepad buttons
    jne_failure()
    emit("66 81 bb 28 12 00 00 20 7f")      # analog triggers
    jne_failure()
    emit("80 bb 2a 12 00 00 01")            # connected
    jne_failure()
    emit("66 81 bb 30 12 00 00 00 40")      # touch 0 x=0.25
    jne_failure()
    emit("80 bb 34 12 00 00 01")            # touch 0 active
    jne_failure()
    emit("66 81 bb 44 12 00 00 00 20")      # acceleration x=1.0
    jne_failure()
    emit("48 b8 88 77 66 55 44 33 22 11")
    emit("48 39 83 60 12 00 00")             # input timestamp
    jne_failure()
    emit("48 b8 11 11 22 22 12 34 56 00")
    emit("48 89 83 40 12 00 00")
    emit("4c 89 e7 be 03 82 08 c0 48 8d 93 40 12 00 00")
    syscall(54)                         # rumble and lightbar output
    expect_rax_small(0)
    emit("4c 89 e7 48 8d b3 00 12 00 00 ba 28 00 00 00")
    syscall(3)
    expect_rax_small(40)
    emit("81 bb 20 12 00 00 11 11 22 22")
    jne_failure()
    emit("4c 89 e7")
    syscall(6)
    expect_rax_small(0)
    emit("48 bf c0 29 00 00 08 00 00 00")  # /dev/audioin
    emit("31 f6 31 d2")
    syscall(5)
    emit("49 89 c4")
    expect_rax_small(3)
    emit("4c 89 e7 48 8d b3 60 11 00 00 ba 10 00 00 00")
    syscall(3)                          # consume host AudioIn samples
    expect_rax_small(16)
    emit("48 b8 20 21 22 23 24 25 26 27")
    emit("48 39 83 60 11 00 00")
    jne_failure()
    emit("48 b8 28 29 2a 2b 2c 2d 2e 2f")
    emit("48 39 83 68 11 00 00")
    jne_failure()
    emit("4c 89 e7")
    syscall(6)
    expect_rax_small(0)
    emit("48 bf e0 29 00 00 08 00 00 00")  # /data/test.bin
    emit("31 f6 31 d2")
    syscall(5)
    emit("49 89 c4")
    expect_rax_small(3)
    emit("4c 89 e7 48 8d b3 80 11 00 00 ba 10 00 00 00")
    syscall(3)                          # read sandboxed app data
    expect_rax_small(16)
    emit("48 b8 30 31 32 33 34 35 36 37")
    emit("48 39 83 80 11 00 00")
    jne_failure()
    emit("48 b8 38 39 3a 3b 3c 3d 3e 3f")
    emit("48 39 83 88 11 00 00")
    jne_failure()
    emit("4c 89 e7")
    syscall(6)
    expect_rax_small(0)
    emit("48 bf 00 2a 00 00 08 00 00 00")  # /data/out.bin
    emit("31 f6 31 d2")
    syscall(5)
    emit("49 89 c4")
    expect_rax_small(3)
    emit("48 b8 c0 c1 c2 c3 c4 c5 c6 c7")
    emit("48 89 83 a0 11 00 00")
    emit("48 b8 c8 c9 ca cb cc cd ce cf")
    emit("48 89 83 a8 11 00 00")
    emit("4c 89 e7 48 8d b3 a0 11 00 00 ba 10 00 00 00")
    syscall(4)                          # write sandboxed save data
    expect_rax_small(16)
    emit("4c 89 e7")
    syscall(6)
    expect_rax_small(0)
    emit("48 bf a0 22 00 00 08 00 00 00")  # /data/atomic.bin
    emit("31 f6 31 d2")
    syscall(5)
    emit("49 89 c4")
    expect_rax_small(3)
    emit("48 b8 d0 d1 d2 d3 d4 d5 d6 d7")
    emit("48 89 83 40 13 00 00")
    emit("48 b8 d8 d9 da db dc dd de df")
    emit("48 89 83 48 13 00 00")
    emit("48 8d 83 40 13 00 00")
    emit("48 89 83 60 13 00 00")
    emit("48 c7 83 68 13 00 00 10 00 00 00")
    emit("4c 89 e7 be 01 83 10 c0 48 8d 93 60 13 00 00")
    syscall(54)                         # atomic save replacement
    expect_rax_small(0)
    emit("4c 89 e7")
    syscall(6)
    expect_rax_small(0)
    emit("48 bf 00 2a 00 00 08 00 00 00")  # reopen /data/out.bin
    emit("31 f6 31 d2")
    syscall(5)
    emit("49 89 c4")
    expect_rax_small(3)
    emit("4c 89 e7")
    syscall(95)                         # fsync
    expect_rax_small(0)
    emit("4c 89 e7 31 f6 31 d2")
    syscall(478)                        # seek to start
    expect_rax_small(0)
    emit("4c 89 e7 48 8d b3 00 13 00 00")
    syscall(189)                        # fstat
    expect_rax_small(0)
    emit("48 83 bb 00 13 00 00 10")
    jne_failure()
    emit("4c 89 e7")
    syscall(6)
    expect_rax_small(0)
    emit("48 bf 00 2a 00 00 08 00 00 00")
    emit("48 be 40 22 00 00 08 00 00 00")
    syscall(128)                        # rename out.bin
    expect_rax_small(0)
    emit("48 bf 40 22 00 00 08 00 00 00")
    emit("48 8d b3 00 13 00 00")
    syscall(188)                        # stat renamed.bin
    expect_rax_small(0)
    emit("48 bf 40 22 00 00 08 00 00 00")
    syscall(10)                         # unlink renamed.bin
    expect_rax_small(0)
    emit("48 bf 20 22 00 00 08 00 00 00 be ed 01 00 00")
    syscall(136)                        # mkdir /data/newdir
    expect_rax_small(0)
    emit("48 bf 60 22 00 00 08 00 00 00 be 00 00 02 00 31 d2")
    syscall(5)                          # open directory
    emit("49 89 c4")
    expect_rax_small(3)
    emit("4c 89 e7 48 8d b3 80 13 00 00 ba 10 01 00 00")
    syscall(196)                        # getdirentries
    emit("48 3d 10 01 00 00")
    jne_failure()
    emit("48 b8 65 6e 74 72 79 2e 62 69")
    emit("48 39 83 8c 13 00 00")
    jne_failure()
    emit("4c 89 e7")
    syscall(6)
    expect_rax_small(0)
    emit("48 bf 00 22 00 00 08 00 00 00 be 01 00 00 00 31 d2")
    syscall(5)                          # app0 writable open must fail
    emit("48 83 f8 e2")                # -EROFS
    jne_failure()
    emit("48 bf 80 22 00 00 08 00 00 00 31 f6 31 d2")
    syscall(5)                          # /dev/dialog
    emit("49 89 c4")
    expect_rax_small(3)
    emit("c7 83 40 15 00 00 41 53 4b 00")
    emit("4c 89 e7 48 8d b3 40 15 00 00 ba 03 00 00 00")
    syscall(4)                          # asynchronous dialog request
    expect_rax_small(3)
    emit("4c 89 e7 48 8d b3 60 15 00 00 ba 20 00 00 00")
    syscall(3)                          # completed dialog response
    expect_rax_small(18)
    emit("83 bb 6c 15 00 00 02")
    jne_failure()
    emit("66 81 bb 70 15 00 00 4f 4b")
    jne_failure()
    emit("4c 89 e7")
    syscall(6)
    expect_rax_small(0)
    emit("bf 02 00 00 00 be 01 00 00 00 31 d2")
    syscall(97)                         # network socket
    emit("49 89 c4")
    expect_rax_small(3)
    emit("48 b8 10 02 01 bb cb 00 71 05")
    emit("48 89 83 00 15 00 00 48 c7 83 08 15 00 00 00 00 00 00")
    emit("4c 89 e7 48 8d b3 00 15 00 00 ba 10 00 00 00")
    syscall(98)                         # connect 203.0.113.5:443
    expect_rax_small(0)
    emit("c7 83 20 15 00 00 50 49 4e 47")
    emit("4c 89 e7 48 8d b3 20 15 00 00 ba 04 00 00 00")
    syscall(4)
    expect_rax_small(4)
    emit("4c 89 e7 48 8d b3 30 15 00 00 ba 04 00 00 00")
    syscall(3)
    expect_rax_small(4)
    emit("81 bb 30 15 00 00 50 4f 4e 47")
    jne_failure()
    emit("4c 89 e7")
    syscall(6)
    expect_rax_small(0)
    emit("bf 02 00 00 00 be 01 00 00 00 31 d2")
    syscall(97)
    emit("49 89 c4")
    expect_rax_small(3)
    emit("48 b8 10 02 00 50 c0 a8 01 02")
    emit("48 89 83 00 15 00 00")
    emit("4c 89 e7 48 8d b3 00 15 00 00 ba 10 00 00 00")
    syscall(98)                         # private network without capability
    emit("48 83 f8 f3")                # -EACCES
    jne_failure()
    emit("4c 89 e7")
    syscall(6)
    expect_rax_small(0)
    emit("48 bf b0 29 00 00 08 00 00 00")  # /dev/audioout
    emit("31 f6 31 d2")
    syscall(5)
    emit("49 89 c4")
    expect_rax_small(3)
    emit("c7 83 00 11 00 00 80 bb 00 00")  # 48000 Hz
    emit("c7 83 04 11 00 00 02 00 00 00")  # stereo
    emit("c7 83 08 11 00 00 01 00 00 00")  # signed 16-bit
    emit("4c 89 e7 be 01 82 0c c0 48 8d 93 00 11 00 00")
    syscall(54)                         # configure AudioOut
    expect_rax_small(0)
    emit("48 b8 00 02 80 40 ff ff ff ff")
    emit("48 89 83 40 11 00 00")
    emit("c7 83 48 11 00 00 ff ff 00 00")
    emit("4c 89 e7 be 04 82 0c c0 48 8d 93 40 11 00 00")
    syscall(54)                         # per-channel AudioOut volume
    expect_rax_small(0)
    emit("48 b8 10 11 12 13 14 15 16 17")
    emit("48 89 83 20 11 00 00")
    emit("48 b8 18 19 1a 1b 1c 1d 1e 1f")
    emit("48 89 83 28 11 00 00")
    emit("4c 89 e7 48 8d b3 20 11 00 00 ba 10 00 00 00")
    syscall(4)                          # submit AudioOut samples
    expect_rax_small(16)
    emit("4c 89 e7")
    syscall(6)
    expect_rax_small(0)
    syscall(362)                        # kqueue
    emit("49 89 c5")                    # preserve queue in r13
    emit("48 3d 00 01 00 00")
    jne_failure()
    emit("4c 89 ef 31 f6 31 d2 45 31 d2 45 31 c0")
    syscall(363)                        # nonblocking empty kevent
    expect_rax_small(0)
    emit("4c 89 ef")
    syscall(6)                          # close equeue
    expect_rax_small(0)
    emit("31 ff be 02 00 00 00")
    syscall(454)                        # _umtx_op wake
    expect_rax_small(0)
    emit("48 89 df be 00 00 20 00")
    syscall(73)                         # munmap
    expect_rax_small(0)
    emit("b8 ff ff ff 7f 0f 05")       # unknown syscall
    emit("48 83 f8 b2")                # cmp rax,-FreeBSD ENOSYS
    jne_failure()
    syscall(20)                         # getpid
    expect_rax_small(1)
    emit("31 ff")
    syscall(1)                          # exit(0)
    emit("eb fe")                       # wait for asynchronous shutdown
    failure_offset = len(code)
    emit("0f 0b")
    for displacement_offset in failure_branches:
        displacement = failure_offset - (displacement_offset + 4)
        pack_into(code, displacement_offset, "i", displacement)
    image[0x1000:0x1000 + len(code)] = code
    path.write_bytes(image)
    return failure_offset


def make_relocated_self(path):
    make_relocated_elf(path)
    elf = bytearray(path.read_bytes())

    # Real Orbis SELF files place PT_DYNAMIC near the end of the blocked
    # PT_SCE_DYNLIBDATA payload instead of giving it a dedicated SELF segment.
    elf[0x500:0x590] = elf[0x200:0x290]
    pack_into(elf, 64 + 56 + 8, "Q", 0x500)
    pack_into(elf, 64 + 112 + 32, "QQ", 0x200, 0x200)

    self_image = bytearray(0x3200)
    pack_into(self_image, 0, "IBBBBBBHHHIIHHI",
              0x1D3D154F, 0, 1, 1, 0x12, 1, 1, 0,
              0x60, 0, len(self_image), 0, 2, 0x22, 0)
    pack_into(self_image, 32, "QQQQ", 0x804, 0x1000, 0x2000, 0x2000)
    pack_into(self_image, 64, "QQQQ", 0x200804, 0x3000, 0x200, 0x200)
    self_image[0x60:0x260] = elf[:0x200]
    self_image[0x1000:0x3000] = elf[0x1000:0x3000]
    self_image[0x3000:0x3200] = elf[0x400:0x600]
    path.write_bytes(self_image)


def make_linked_module_pair(root):
    module_dir = root / "sce_module"
    module_dir.mkdir()
    main_path = root / "linked-main.elf"
    module_path = module_dir / "libTest.prx"
    main = bytearray(0x2000)
    provider = bytearray(0x2000)

    def make_header(image, elf_type, entry, relocation_size):
        image[:16] = b"\x7fELF\x02\x01\x01\x09" + b"\0" * 8
        pack_into(image, 16, "HHIQQQIHHHHHH", elf_type, 62, 1, entry,
                  64, 0, 0, 64, 56, 3, 0, 0, 0)
        pack_into(image, 64, "IIQQQQQQ", 1, 7, 0x1000,
                  0x1000 if elf_type == 0xFE10 else 0, 0,
                  0x1000, 0x1000, 0x1000)
        pack_into(image, 120, "IIQQQQQQ", 2, 4, 0x200, 0, 0,
                  0xE0, 0xE0, 8)
        pack_into(image, 176, "IIQQQQQQ", 0x61000000, 4, 0x400,
                  0, 0, 0x400, 0x400, 8)
        tags = [
            (0x61000035, 0), (0x61000037, 0x40),
            (0x61000039, 0x80), (0x6100003B, 24),
            (0x6100003F, 48), (0x6100002F, 0x100),
            (0x61000031, relocation_size), (0x61000033, 24),
        ]
        identity_tag = 0x6100000F if elf_type == 0xFE10 else 0x6100000D
        library_tag = 0x61000015 if elf_type == 0xFE10 else 0x61000013
        tags += [
            (identity_tag, (1 << 48) | (1 << 40) | (1 << 32) | 0x19),
            (library_tag, (1 << 48) | (1 << 32) | 0x11),
            (0, 0),
        ]
        for index, tag in enumerate(tags):
            pack_into(image, 0x200 + index * 16, "QQ", *tag)
        strings = b"\0LINKTESTNID#B#B\0libTest\0modTest\0"
        image[0x400:0x400 + len(strings)] = strings

    make_header(main, 0xFE10, 0x1000, 24)
    pack_into(main, 0x400 + 0x80 + 24, "IBBHQQ",
              1, 0x12, 0, 0, 0, 0)
    pack_into(main, 0x400 + 0x100, "QQq",
              0x1800, (1 << 32) | 1, 0)
    provider_address = VIRTUAL_BASE + 0x200000 + 0x100
    slot_address = VIRTUAL_BASE + 0x1800
    code = bytearray(b"\x48\xa1" + struct.pack("<Q", slot_address))
    code += bytes.fromhex("ff d0 48 83 f8 2a 75 0b")
    code += bytes.fromhex("b8 01 00 00 00 31 ff 0f 05 eb fe")
    failure_offset = len(code)
    code += bytes.fromhex("0f 0b eb fc")
    main[0x1000:0x1000 + len(code)] = code

    make_header(provider, 0xFE18, 0x100, 0)
    pack_into(provider, 0x400 + 0x80 + 24, "IBBHQQ",
              1, 0x12, 0, 1, 0x100, 6)
    provider[0x1100:0x1106] = bytes.fromhex("b8 2a 00 00 00 c3")
    main_path.write_bytes(main)
    module_path.write_bytes(provider)
    return main_path, failure_offset, provider_address


def make_hle_import_elf(path, nid="1j3S3n-tTW4",
                        library="libkernel", setup=b"", checks=None,
                        symbol_type=2, invoke=True, module=None,
                        export_nid=None, export_value=0x1A00):
    module = module or library
    image = bytearray(0x2000)
    image[:16] = b"\x7fELF\x02\x01\x01\x09" + b"\0" * 8
    pack_into(image, 16, "HHIQQQIHHHHHH", 0xFE10, 62, 1, 0x1000,
              64, 0, 0, 64, 56, 3, 0, 0, 0)
    pack_into(image, 64, "IIQQQQQQ", 1, 7, 0x1000, 0x1000, 0,
              0x1000, 0x1000, 0x1000)
    pack_into(image, 120, "IIQQQQQQ", 2, 4, 0x200, 0, 0,
              0xE0, 0xE0, 8)
    pack_into(image, 176, "IIQQQQQQ", 0x61000000, 4, 0x400,
              0, 0, 0x400, 0x400, 8)
    symbol_name = f"{nid}#B#B".encode("ascii") + b"\0"
    strings = b"\0" + symbol_name
    library_offset = len(strings)
    strings += library.encode("ascii") + b"\0"
    module_offset = len(strings)
    strings += module.encode("ascii") + b"\0"
    export_name_offset = 0
    if export_nid:
        export_name_offset = len(strings)
        strings += f"{export_nid}#B#B".encode("ascii") + b"\0"
    symbol_table_size = 72 if export_nid else 48
    tags = [
        (0x61000035, 0), (0x61000037, len(strings)),
        (0x61000039, 0x80), (0x6100003B, 24),
        (0x6100003F, symbol_table_size), (0x6100002F, 0x100),
        (0x61000031, 24), (0x61000033, 24),
        (0x6100000F, (1 << 48) | (1 << 40) | (1 << 32) |
         module_offset),
        (0x61000015, (1 << 48) | (1 << 32) | library_offset),
    ]
    if export_nid:
        tags += [
            (0x6100000D, (1 << 48) | (1 << 40) | (1 << 32) |
             module_offset),
            (0x61000013, (1 << 48) | (1 << 32) | library_offset),
        ]
    tags.append((0, 0))
    for index, tag in enumerate(tags):
        pack_into(image, 0x200 + index * 16, "QQ", *tag)
    image[0x400:0x400 + len(strings)] = strings
    pack_into(image, 0x400 + 0x80 + 24, "IBBHQQ",
              1, 0x10 | symbol_type, 0, 0, 0, 0)
    if export_nid:
        pack_into(image, 0x400 + 0x80 + 48, "IBBHQQ",
                  export_name_offset, 0x12, 0, 1, export_value, 1)
        image[export_value:export_value + 1] = b"\xc3"
    pack_into(image, 0x400 + 0x100, "QQq",
              0x1800, (1 << 32) | 1, 0)

    slot_address = VIRTUAL_BASE + 0x1800
    code = bytearray(setup)
    code += b"\x48\xa1" + struct.pack("<Q", slot_address)
    if invoke:
        code += bytes.fromhex("ff d0")
    if checks is None:
        checks = [bytes.fromhex("48 3d 00 ca 9a 3b")]
    failure_branches = []
    for check in checks:
        code += check
        code += bytes.fromhex("0f 85 00 00 00 00")
        failure_branches.append(len(code) - 4)
    code += bytes.fromhex("b8 01 00 00 00 31 ff 0f 05 eb fe")
    failure_offset = len(code)
    code += bytes.fromhex("0f 0b eb fc")
    for branch in failure_branches:
        pack_into(code, branch, "i", failure_offset - (branch + 4))
    image[0x1000:0x1000 + len(code)] = code
    path.write_bytes(image)
    return failure_offset


def make_tls_module_pair(root):
    module_dir = root / "sce_module"
    module_dir.mkdir()
    main_path = root / "tls-main.elf"
    module_path = module_dir / "libTls.prx"
    main = bytearray(0x2000)
    module = bytearray(0x2000)

    main[:16] = b"\x7fELF\x02\x01\x01\x09" + b"\0" * 8
    pack_into(main, 16, "HHIQQQIHHHHHH", 0xFE10, 62, 1, 0x1000,
              64, 0, 0, 64, 56, 4, 0, 0, 0)
    pack_into(main, 64, "IIQQQQQQ", 1, 7, 0x1000, 0x1000, 0,
              0x1000, 0x1000, 0x1000)
    pack_into(main, 120, "IIQQQQQQ", 2, 4, 0x200, 0, 0,
              0xE0, 0xE0, 8)
    pack_into(main, 176, "IIQQQQQQ", 0x61000000, 4, 0x400,
              0, 0, 0x400, 0x400, 8)
    pack_into(main, 232, "IIQQQQQQ", 7, 4, 0x1900, 0x1900, 0,
              1, 0x20, 0x20)
    tags = [
        (0x61000035, 0), (0x61000037, 0x40),
        (0x61000039, 0x80), (0x6100003B, 24),
        (0x6100003F, 48), (0x6100002F, 0x100),
        (0x61000031, 24), (0x61000033, 24),
        (0x6100000F, (1 << 48) | (1 << 40) | (1 << 32) | 0x1B),
        (0x61000015, (1 << 48) | (1 << 32) | 0x11),
        (0, 0),
    ]
    for index, tag in enumerate(tags):
        pack_into(main, 0x200 + index * 16, "QQ", *tag)
    strings = b"\0" + b"vNe1w4diLCs#B#B\0libkernel\0libkernel\0"
    main[0x400:0x400 + len(strings)] = strings
    pack_into(main, 0x400 + 0x80 + 24, "IBBHQQ",
              1, 0x12, 0, 0, 0, 0)
    pack_into(main, 0x400 + 0x100, "QQq",
              0x1800, (1 << 32) | 1, 0)
    slot_address = VIRTUAL_BASE + 0x1800
    index_address = VIRTUAL_BASE + 0x1810
    module_base = VIRTUAL_BASE + 0x200000
    code = bytearray(b"\x48\xa1" + struct.pack("<Q", slot_address))
    code += b"\x48\xbf" + struct.pack("<Q", index_address)
    code += bytes.fromhex("ff d0 80 38 5a 75 1b")
    code += b"\x48\xa1" + struct.pack("<Q", module_base + 0x800)
    code += bytes.fromhex("48 83 f8 02 75 0b")
    code += bytes.fromhex("b8 01 00 00 00 31 ff 0f 05 eb fe")
    failure_offset = len(code)
    code += bytes.fromhex("0f 0b eb fc")
    main[0x1000:0x1000 + len(code)] = code
    pack_into(main, 0x1810, "QQ", 2, 0)
    main[0x1900] = 0x33

    module[:16] = b"\x7fELF\x02\x01\x01\x09" + b"\0" * 8
    pack_into(module, 16, "HHIQQQIHHHHHH", 0xFE18, 62, 1, 0x100,
              64, 0, 0, 64, 56, 4, 0, 0, 0)
    pack_into(module, 64, "IIQQQQQQ", 1, 7, 0x1000, 0, 0,
              0x1000, 0x1000, 0x1000)
    pack_into(module, 120, "IIQQQQQQ", 2, 4, 0x200, 0, 0,
              0x90, 0x90, 8)
    pack_into(module, 176, "IIQQQQQQ", 0x61000000, 4, 0x400,
              0, 0, 0x200, 0x200, 8)
    pack_into(module, 232, "IIQQQQQQ", 7, 4, 0x1900, 0x900, 0,
              1, 0x20, 0x20)
    module_tags = [
        (0x61000035, 0), (0x61000037, 1),
        (0x61000039, 0x20), (0x6100003B, 24),
        (0x6100003F, 24), (0x6100002F, 0x80),
        (0x61000031, 24), (0x61000033, 24), (0, 0),
    ]
    for index, tag in enumerate(module_tags):
        pack_into(module, 0x200 + index * 16, "QQ", *tag)
    pack_into(module, 0x400 + 0x80, "QQq", 0x800, 16, 0)
    module[0x1100] = 0xC3
    module[0x1900] = 0x5A
    main_path.write_bytes(main)
    module_path.write_bytes(module)
    return main_path, failure_offset


def run_worker(dll_path, elf_path, trace_path, mode):
    if os.name != "nt":
        raise RuntimeError("this embedding smoke test requires Windows")

    dll_directories = [os.add_dll_directory(str(dll_path.parent))]
    msys_bin = Path("C:/msys64/ucrt64/bin")
    if msys_bin.is_dir():
        dll_directories.append(os.add_dll_directory(str(msys_bin)))
    qemu = ctypes.CDLL(str(dll_path))
    qemu.qemu_host_init.argtypes = (
        ctypes.c_int, ctypes.POINTER(ctypes.c_char_p))
    qemu.qemu_host_init.restype = ctypes.c_int
    qemu.qemu_host_main_loop_step.argtypes = (
        ctypes.c_bool, ctypes.POINTER(ctypes.c_int))
    qemu.qemu_host_main_loop_step.restype = ctypes.c_int
    qemu.qemu_host_request_shutdown.restype = ctypes.c_int
    qemu.qemu_host_cleanup.restype = ctypes.c_int

    video_frames = []
    audio_frames = []
    pad_outputs = []
    storage = {
        "/titles/TEST00001/data/test.bin": bytearray(range(0x30, 0x40)),
        "/titles/TEST00001/data/out.bin": bytearray(),
        "/titles/TEST00001/data/atomic.bin": bytearray(),
        "/titles/TEST00001/app0/game.bin": bytearray(b"READONLY"),
        "/titles/TEST00001/data/list/entry.bin": bytearray(b"ENTRY"),
    }
    storage_dirs = {
        "/titles/TEST00001/data/",
        "/titles/TEST00001/data/list",
    }
    storage_cleanups = []
    storage_handles = {}
    next_handle = [1]
    video_type = ctypes.CFUNCTYPE(
        None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_int)
    audio_type = ctypes.CFUNCTYPE(
        None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
        ctypes.c_int, ctypes.c_int, ctypes.c_int)
    pad_output_type = ctypes.CFUNCTYPE(
        None, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p)
    storage_open_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
        ctypes.c_int, ctypes.POINTER(ctypes.c_int64))
    storage_read_type = ctypes.CFUNCTYPE(
        ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p,
        ctypes.c_size_t)
    storage_write_type = ctypes.CFUNCTYPE(
        ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p,
        ctypes.c_size_t)
    storage_close_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_int64)
    storage_seek_type = ctypes.CFUNCTYPE(
        ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64, ctypes.c_int64,
        ctypes.c_int)
    storage_stat_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)
    storage_mkdir_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int)
    storage_path_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p)
    storage_rename_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p)
    storage_flush_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_int64)
    storage_readdir_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p,
        ctypes.c_size_t, ctypes.c_void_p)
    storage_atomic_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p,
        ctypes.c_size_t)
    dialog_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint32,
        ctypes.c_void_p, ctypes.c_size_t)
    network_socket_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.POINTER(ctypes.c_int64))
    network_connect_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_int64, ctypes.c_char_p,
        ctypes.c_uint16)
    network_send_type = ctypes.CFUNCTYPE(
        ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p,
        ctypes.c_size_t, ctypes.c_int)
    network_recv_type = ctypes.CFUNCTYPE(
        ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p,
        ctypes.c_size_t, ctypes.c_int)
    network_close_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_int64)
    guest_read_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p,
        ctypes.c_size_t)
    guest_write_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p,
        ctypes.c_size_t)

    class HLERequest(ctypes.Structure):
        _fields_ = [
            ("size", ctypes.c_uint32), ("version", ctypes.c_uint32),
            ("module", ctypes.c_char_p), ("library", ctypes.c_char_p),
            ("nid", ctypes.c_char_p), ("args", ctypes.c_uint64 * 7),
            ("memory_opaque", ctypes.c_void_p),
            ("read_guest", guest_read_type),
            ("write_guest", guest_write_type),
        ]

    hle_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(HLERequest),
        ctypes.POINTER(ctypes.c_uint64))
    launch_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_char_p), ctypes.c_size_t)

    class GamepadState(ctypes.Structure):
        _fields_ = [
            ("buttons", ctypes.c_uint32),
            ("left_x", ctypes.c_int16), ("left_y", ctypes.c_int16),
            ("right_x", ctypes.c_int16), ("right_y", ctypes.c_int16),
            ("left_trigger", ctypes.c_uint8),
            ("right_trigger", ctypes.c_uint8),
            ("connected", ctypes.c_uint8), ("reserved", ctypes.c_uint8),
            ("timestamp_ns", ctypes.c_uint64),
        ]

    class TouchPoint(ctypes.Structure):
        _fields_ = [
            ("id", ctypes.c_uint32), ("x", ctypes.c_float),
            ("y", ctypes.c_float), ("active", ctypes.c_uint8),
            ("reserved", ctypes.c_uint8 * 3),
        ]

    class TouchState(ctypes.Structure):
        _fields_ = [
            ("points", TouchPoint * 2), ("timestamp_ns", ctypes.c_uint64),
        ]

    class MotionState(ctypes.Structure):
        _fields_ = [
            ("acceleration", ctypes.c_float * 3),
            ("angular_velocity", ctypes.c_float * 3),
            ("orientation", ctypes.c_float * 4),
            ("timestamp_ns", ctypes.c_uint64),
        ]

    class PadOutput(ctypes.Structure):
        _fields_ = [
            ("rumble_small", ctypes.c_uint16),
            ("rumble_large", ctypes.c_uint16),
            ("red", ctypes.c_uint8), ("green", ctypes.c_uint8),
            ("blue", ctypes.c_uint8), ("reserved", ctypes.c_uint8),
        ]

    class StorageCallbacks(ctypes.Structure):
        _fields_ = [
            ("size", ctypes.c_uint32), ("version", ctypes.c_uint32),
            ("open", storage_open_type), ("read", storage_read_type),
            ("write", storage_write_type), ("close", storage_close_type),
            ("seek", storage_seek_type), ("stat", storage_stat_type),
            ("mkdir", storage_mkdir_type), ("unlink", storage_path_type),
            ("rename", storage_rename_type), ("flush", storage_flush_type),
            ("readdir", storage_readdir_type),
            ("atomic_replace", storage_atomic_type),
            ("cleanup", storage_path_type),
        ]

    class StorageStat(ctypes.Structure):
        _fields_ = [
            ("size", ctypes.c_uint64),
            ("allocated_size", ctypes.c_uint64),
            ("modified_time_ns", ctypes.c_uint64),
            ("mode", ctypes.c_uint32), ("type", ctypes.c_uint32),
        ]

    class NetworkCallbacks(ctypes.Structure):
        _fields_ = [
            ("size", ctypes.c_uint32), ("version", ctypes.c_uint32),
            ("capabilities", ctypes.c_uint32),
            ("reserved", ctypes.c_uint32),
            ("socket", network_socket_type),
            ("connect", network_connect_type),
            ("send", network_send_type), ("recv", network_recv_type),
            ("close", network_close_type),
        ]

    @video_type
    def video_callback(_opaque, pixels, width, height, stride, pixel_format):
        video_frames.append((ctypes.string_at(pixels, stride * height),
                             width, height, stride, pixel_format))

    @audio_type
    def audio_callback(_opaque, samples, size, rate, channels, audio_format):
        audio_frames.append((ctypes.string_at(samples, size), rate, channels,
                             audio_format))

    @pad_output_type
    def pad_output_callback(_opaque, controller, output_ptr):
        output = ctypes.cast(output_ptr, ctypes.POINTER(PadOutput)).contents
        pad_outputs.append((controller, output.rumble_small,
                            output.rumble_large, output.red, output.green,
                            output.blue))

    @storage_open_type
    def storage_open(_opaque, path_ptr, flags, _mode, handle_ptr):
        path = path_ptr.decode()
        is_directory = bool(flags & 0x40000000)
        if (is_directory and path not in storage_dirs) or (
                not is_directory and path not in storage):
            return -9
        handle = next_handle[0]
        next_handle[0] += 1
        storage_handles[handle] = [path, 0]
        handle_ptr[0] = handle
        return 0

    @storage_read_type
    def storage_read(_opaque, handle, buffer, size):
        if handle not in storage_handles:
            return -9
        path, offset = storage_handles[handle]
        data = bytes(storage[path][offset:offset + size])
        if data:
            ctypes.memmove(buffer, data, len(data))
        storage_handles[handle][1] += len(data)
        return len(data)

    @storage_write_type
    def storage_write(_opaque, handle, buffer, size):
        if handle not in storage_handles:
            return -9
        path, offset = storage_handles[handle]
        data = ctypes.string_at(buffer, size)
        end = offset + len(data)
        if end > len(storage[path]):
            storage[path].extend(b"\0" * (end - len(storage[path])))
        storage[path][offset:end] = data
        storage_handles[handle][1] = end
        return len(data)

    @storage_close_type
    def storage_close(_opaque, handle):
        return 0 if storage_handles.pop(handle, None) is not None else -9

    @storage_seek_type
    def storage_seek(_opaque, handle, offset, whence):
        if handle not in storage_handles:
            return -9
        path, position = storage_handles[handle]
        if path not in storage:
            return -22
        base = (0, position, len(storage[path]))[whence] if whence < 3 else -1
        position = base + offset
        if position < 0:
            return -22
        storage_handles[handle][1] = position
        return position

    def fill_storage_stat(path, stat_ptr):
        if path not in storage and path not in storage_dirs:
            return -9
        stat = ctypes.cast(stat_ptr, ctypes.POINTER(StorageStat)).contents
        size = len(storage[path]) if path in storage else 0
        stat.size = size
        stat.allocated_size = size
        stat.modified_time_ns = 0x123456789
        stat.mode = 0o444 if "/app0/" in path else 0o666
        stat.type = 1 if path in storage else 2
        return 0

    @storage_stat_type
    def storage_stat(_opaque, path_ptr, stat_ptr):
        return fill_storage_stat(path_ptr.decode(), stat_ptr)

    @storage_mkdir_type
    def storage_mkdir(_opaque, path_ptr, _mode):
        path = path_ptr.decode()
        storage_dirs.add(path)
        return 0

    @storage_path_type
    def storage_unlink(_opaque, path_ptr):
        path = path_ptr.decode()
        return 0 if storage.pop(path, None) is not None else -9

    @storage_rename_type
    def storage_rename(_opaque, old_ptr, new_ptr):
        old_path = old_ptr.decode()
        new_path = new_ptr.decode()
        if old_path not in storage:
            return -9
        storage[new_path] = storage.pop(old_path)
        return 0

    @storage_flush_type
    def storage_flush(_opaque, handle):
        return 0 if handle in storage_handles else -9

    @storage_readdir_type
    def storage_readdir(_opaque, handle, name_ptr, name_size, stat_ptr):
        if handle not in storage_handles:
            return -9
        path, index = storage_handles[handle]
        prefix = path.rstrip("/") + "/"
        names = sorted(
            key[len(prefix):] for key in storage
            if key.startswith(prefix) and "/" not in key[len(prefix):])
        if index >= len(names):
            return 0
        name = names[index].encode() + b"\0"
        if len(name) > name_size:
            return -22
        ctypes.memmove(name_ptr, name, len(name))
        storage_handles[handle][1] += 1
        return fill_storage_stat(prefix + names[index], stat_ptr) or 1

    @storage_atomic_type
    def storage_atomic(_opaque, path_ptr, data_ptr, size):
        storage[path_ptr.decode()] = bytearray(ctypes.string_at(data_ptr, size))
        return 0

    @storage_path_type
    def storage_cleanup(_opaque, path_ptr):
        storage_cleanups.append(path_ptr.decode())
        return 0

    network_events = []
    dialog_events = []
    network_handles = {}
    next_network_handle = [100]
    hle_events = []
    launch_events = []

    @hle_type
    def hle_callback(_opaque, request_ptr, result_ptr):
        request = request_ptr.contents
        module = request.module.decode()
        library = request.library.decode()
        nid = request.nid.decode()
        hle_events.append((module, library, nid, tuple(request.args)))
        if nid != "TESTEXTNID0":
            return -38
        if request.read_guest(request.memory_opaque, 0, None, 0) != 0:
            return -14
        if request.write_guest(request.memory_opaque, 0, None, 0) != 0:
            return -14
        source = ctypes.create_string_buffer(4)
        if request.read_guest(request.memory_opaque, request.args[0], source,
                              4) != 0 or source.raw != b"UWP\0":
            return -14
        output = ctypes.create_string_buffer(b"HOST")
        if request.write_guest(request.memory_opaque, request.args[1], output,
                               4) != 0:
            return -14
        result_ptr[0] = 42
        return 0

    @launch_type
    def launch_callback(_opaque, path, argv, argc):
        launch_events.append((path.decode(),
                              tuple(argv[i].decode() for i in range(argc))))
        return 0

    @network_socket_type
    def network_socket(_opaque, domain, sock_type, protocol, handle_ptr):
        handle = next_network_handle[0]
        next_network_handle[0] += 1
        network_handles[handle] = bytearray(b"PONG")
        handle_ptr[0] = handle
        network_events.append(("socket", domain, sock_type, protocol))
        return 0

    @network_connect_type
    def network_connect(_opaque, handle, address, port):
        if handle not in network_handles:
            return -9
        network_events.append(("connect", address.decode(), port))
        return 0

    @network_send_type
    def network_send(_opaque, handle, buffer, size, _flags):
        if handle not in network_handles:
            return -9
        network_events.append(("send", ctypes.string_at(buffer, size)))
        return size

    @network_recv_type
    def network_recv(_opaque, handle, buffer, size, _flags):
        if handle not in network_handles:
            return -9
        data = bytes(network_handles[handle][:size])
        del network_handles[handle][:len(data)]
        ctypes.memmove(buffer, data, len(data))
        return len(data)

    @network_close_type
    def network_close(_opaque, handle):
        return 0 if network_handles.pop(handle, None) is not None else -9

    @dialog_type
    def dialog_request(_opaque, request_id, dialog_kind, payload, size):
        assert dialog_kind == 1
        assert ctypes.string_at(payload, size) == b"ASK"
        dialog_events.append((request_id, dialog_kind, b"ASK"))
        response = ctypes.create_string_buffer(b"OK")
        result = qemu.qemu_host_complete_dialog(request_id, 0, response, 2)
        assert qemu.qemu_host_complete_dialog(
            request_id, 0, response, 2) == -2
        return result

    storage_callbacks = StorageCallbacks(
        ctypes.sizeof(StorageCallbacks), 2, storage_open, storage_read,
        storage_write, storage_close, storage_seek, storage_stat,
        storage_mkdir, storage_unlink, storage_rename, storage_flush,
        storage_readdir, storage_atomic, storage_cleanup)
    network_callbacks = NetworkCallbacks(
        ctypes.sizeof(NetworkCallbacks), 1, 1, 0, network_socket,
        network_connect, network_send, network_recv, network_close)

    qemu.qemu_host_register_video_callback.argtypes = (
        video_type, ctypes.c_void_p)
    qemu.qemu_host_register_audio_callback.argtypes = (
        audio_type, ctypes.c_void_p)
    qemu.qemu_host_register_pad_output_callback.argtypes = (
        pad_output_type, ctypes.c_void_p)
    qemu.qemu_host_register_storage_callbacks.argtypes = (
        ctypes.POINTER(StorageCallbacks), ctypes.c_void_p)
    qemu.qemu_host_register_dialog_callback.argtypes = (
        dialog_type, ctypes.c_void_p)
    qemu.qemu_host_complete_dialog.argtypes = (
        ctypes.c_uint64, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t)
    qemu.qemu_host_complete_dialog.restype = ctypes.c_int
    qemu.qemu_host_register_network_callbacks.argtypes = (
        ctypes.POINTER(NetworkCallbacks), ctypes.c_void_p)
    qemu.qemu_host_send_key_number.argtypes = (ctypes.c_int, ctypes.c_bool)
    qemu.qemu_host_send_key_number.restype = ctypes.c_int
    qemu.qemu_host_send_key_qcode.argtypes = (ctypes.c_int, ctypes.c_bool)
    qemu.qemu_host_send_key_qcode.restype = ctypes.c_int
    qemu.qemu_host_send_pointer_abs.argtypes = (
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int)
    qemu.qemu_host_send_pointer_abs.restype = ctypes.c_int
    qemu.qemu_host_pointer_is_absolute.restype = ctypes.c_bool
    qemu.qemu_host_send_gamepad_state.argtypes = (
        ctypes.c_int, ctypes.POINTER(GamepadState))
    qemu.qemu_host_send_gamepad_state.restype = ctypes.c_int
    qemu.qemu_host_send_touch_state.argtypes = (
        ctypes.c_int, ctypes.POINTER(TouchState))
    qemu.qemu_host_send_touch_state.restype = ctypes.c_int
    qemu.qemu_host_send_motion_state.argtypes = (
        ctypes.c_int, ctypes.POINTER(MotionState))
    qemu.qemu_host_send_motion_state.restype = ctypes.c_int
    qemu.qemu_host_send_audio_input.argtypes = (
        ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
        ctypes.c_int)
    qemu.qemu_host_send_audio_input.restype = ctypes.c_int
    qemu.qemu_host_register_hle_callback.argtypes = (hle_type, ctypes.c_void_p)
    qemu.qemu_host_register_launch_callback.argtypes = (
        launch_type, ctypes.c_void_p)

    audio_driver = os.environ.get("SHADPS4_TEST_AUDIODEV", "none")
    variant = "neo" if mode == "fault" else "base"
    arguments = [
        "qemu-system-shadps4", "-M",
        f"shadps4,variant={variant},execute=on,title-id=TEST00001",
        "-accel", "tcg,thread=single", "-smp", "8", "-m", "64M",
        "-audiodev", f"{audio_driver},id=shadps4",
        "-display", "none", "-nodefaults", "-kernel", str(elf_path),
        "-d", "in_asm", "-D", str(trace_path),
    ]
    encoded = [argument.encode() for argument in arguments]
    argv = (ctypes.c_char_p * len(encoded))(*encoded)
    status = ctypes.c_int()

    qemu.qemu_host_register_hle_callback(hle_callback, None)
    qemu.qemu_host_register_launch_callback(launch_callback, None)
    assert qemu.qemu_host_init(len(encoded), argv) == 0
    if mode == "invalid":
        assert qemu.qemu_host_request_shutdown() == 0
        for _ in range(1000):
            if qemu.qemu_host_main_loop_step(
                    True, ctypes.byref(status)) == 1:
                break
            time.sleep(0.001)
        else:
            raise AssertionError("invalid-image worker did not stop")
        assert qemu.qemu_host_cleanup() == 0
        return
    qemu.qemu_host_register_video_callback(video_callback, None)
    qemu.qemu_host_register_audio_callback(audio_callback, None)
    qemu.qemu_host_register_pad_output_callback(pad_output_callback, None)
    qemu.qemu_host_register_storage_callbacks(
        ctypes.byref(storage_callbacks), None)
    qemu.qemu_host_register_dialog_callback(dialog_request, None)
    qemu.qemu_host_register_network_callbacks(
        ctypes.byref(network_callbacks), None)
    assert qemu.qemu_host_send_key_number(57, True) == 0
    assert qemu.qemu_host_send_key_number(-1, True) == -22
    assert qemu.qemu_host_send_key_qcode(-1, True) == -22
    assert qemu.qemu_host_send_pointer_abs(25, 50, 100, 100) == 0
    assert qemu.qemu_host_pointer_is_absolute()
    gamepad = GamepadState(0xA5A55A5A, 1234, -2345, 3456, -4567,
                           0x20, 0x7F, 1, 0, 0x1122334455667788)
    touch = TouchState()
    touch.points[0] = TouchPoint(7, 0.25, 0.75, 1, (ctypes.c_uint8 * 3)())
    touch.timestamp_ns = gamepad.timestamp_ns
    motion = MotionState((ctypes.c_float * 3)(1.0, -1.0, 0.5),
                         (ctypes.c_float * 3)(0.25, 0.5, -0.25),
                         (ctypes.c_float * 4)(0.0, 0.0, 0.0, 1.0),
                         gamepad.timestamp_ns)
    assert qemu.qemu_host_send_gamepad_state(
        -1, ctypes.byref(gamepad)) == -22
    assert qemu.qemu_host_send_gamepad_state(0, ctypes.byref(gamepad)) == 0
    assert qemu.qemu_host_send_touch_state(0, ctypes.byref(touch)) == 0
    assert qemu.qemu_host_send_motion_state(0, ctypes.byref(motion)) == 0
    audio_input = bytes(range(0x20, 0x30))
    audio_input_buffer = ctypes.create_string_buffer(audio_input)
    assert qemu.qemu_host_send_audio_input(
        audio_input_buffer, len(audio_input) - 1, 48000, 2, 1) == -22
    assert qemu.qemu_host_send_audio_input(
        audio_input_buffer, len(audio_input), 48000, 2, 1) == 0
    stopped = False
    for _ in range(1000):
        result = qemu.qemu_host_main_loop_step(True, ctypes.byref(status))
        assert result in (0, 1)
        if result == 1:
            stopped = True
            break
        time.sleep(0.01)
    if not stopped:
        assert qemu.qemu_host_request_shutdown() == 0
        for _ in range(1000):
            if qemu.qemu_host_main_loop_step(True,
                                             ctypes.byref(status)) == 1:
                stopped = True
                break
    assert stopped, "QEMU main loop did not stop"
    assert status.value == 0
    if mode == "functional":
        assert video_frames == [(
            bytes.fromhex("00 11 22 33 44 55 66 77 "
                          "88 99 aa bb cc dd ee ff"),
            2, 2, 8, 1)]
        assert audio_frames == [(
            bytes(range(0x10, 0x20)), 48000, 2, 1)]
        assert pad_outputs == [(0, 0x1111, 0x2222, 0x12, 0x34, 0x56)]
        assert storage[
            "/titles/TEST00001/data/atomic.bin"] == bytes(range(0xD0, 0xE0))
        assert "/titles/TEST00001/data/newdir" in storage_dirs
        assert "/titles/TEST00001/data/out.bin" not in storage
        assert "/titles/TEST00001/data/renamed.bin" not in storage
        assert storage[
            "/titles/TEST00001/app0/game.bin"] == b"READONLY"
        assert len(dialog_events) == 1
        assert ("connect", "203.0.113.5", 443) in network_events
        assert ("send", b"PING") in network_events
    assert qemu.qemu_host_cleanup() == 0
    if mode == "functional":
        assert storage_cleanups == [
            "/titles/TEST00001/temp/",
            "/titles/TEST00001/temp0/",
        ], storage_cleanups
    if mode == "external-hle":
        assert len(hle_events) == 1
        assert hle_events[0][:3] == (
            "libSceExternalTest", "libSceExternalTest", "TESTEXTNID0")
    if mode == "launch":
        assert launch_events == [("/app0/next.elf", ("--resume", "slot1"))]


def run_test(dll_path):
    with tempfile.TemporaryDirectory(prefix="qemu-shadps4-") as temp:
        temp = Path(temp)
        elf_path = temp / "relocated-orbis.elf"
        trace_path = temp / "tcg.log"
        failure_offset = make_relocated_elf(elf_path)
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(elf_path), str(trace_path), "functional"], check=True)
        trace = trace_path.read_text(encoding="utf-8")
        assert f"0x{VIRTUAL_BASE + 0x1000:x}" in trace
        assert "0x00009000" in trace
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" not in trace

        self_path = temp / "relocated-orbis.self"
        self_trace = temp / "self.log"
        make_relocated_self(self_path)
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(self_path), str(self_trace), "functional"], check=True)
        trace = self_trace.read_text(encoding="utf-8")
        assert f"0x{VIRTUAL_BASE + 0x1000:x}" in trace

        linked_root = temp / "linked"
        linked_root.mkdir()
        linked_elf, failure_offset, provider_address = \
            make_linked_module_pair(linked_root)
        linked_trace = temp / "linked.log"
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(linked_elf), str(linked_trace), "linked"], check=True)
        trace = linked_trace.read_text(encoding="utf-8")
        assert f"0x{provider_address:x}" in trace
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" not in trace

        hle_elf = temp / "hle-import.elf"
        hle_trace = temp / "hle.log"
        failure_offset = make_hle_import_elf(hle_elf)
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(hle_elf), str(hle_trace), "linked"], check=True)
        trace = hle_trace.read_text(encoding="utf-8")
        trace_addresses = (int(match, 16) for match in
                           re.findall(r"0x([0-9a-fA-F]+)", trace))
        assert any(VIRTUAL_BASE + 0x200000 <= address <
                   VIRTUAL_BASE + 0x210000 for address in trace_addresses)
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" not in trace

        external_elf = temp / "external-hle.elf"
        external_trace = temp / "external-hle.log"
        setup = (b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 b"\x48\xbe" + struct.pack("<Q", VIRTUAL_BASE + 0x1910))
        checks = [
            bytes.fromhex("48 83 f8 2a"),
            b"\x48\xa1" + struct.pack("<Q", VIRTUAL_BASE + 0x1910) +
            bytes.fromhex("3d 48 4f 53 54"),
        ]
        failure_offset = make_hle_import_elf(
            external_elf, "TESTEXTNID0", "libSceExternalTest", setup,
            checks, module="libSceExternalTest")
        image = bytearray(external_elf.read_bytes())
        image[0x1900:0x1904] = b"UWP\0"
        external_elf.write_bytes(image)
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(external_elf), str(external_trace), "external-hle"],
            check=True)
        trace = external_trace.read_text(encoding="utf-8")
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" not in trace

        launch_elf = temp / "system-load-exec.elf"
        launch_trace = temp / "system-load-exec.log"
        setup = (b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 b"\x48\xbe" + struct.pack("<Q", VIRTUAL_BASE + 0x1940))
        failure_offset = make_hle_import_elf(
            launch_elf, "JoBqSQt1yyA", "libSceSystemService", setup,
            [bytes.fromhex("48 83 f8 00")])
        image = bytearray(launch_elf.read_bytes())
        image[0x1900:0x190F] = b"/app0/next.elf\0"
        image[0x1920:0x1929] = b"--resume\0"
        image[0x1930:0x1936] = b"slot1\0"
        pack_into(image, 0x1940, "QQQ", VIRTUAL_BASE + 0x1920,
                  VIRTUAL_BASE + 0x1930, 0)
        launch_elf.write_bytes(image)
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(launch_elf), str(launch_trace), "launch"], check=True)
        trace = launch_trace.read_text(encoding="utf-8")
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" not in trace

        service_cases = [
            (
                "user-service", "CdWp0oHWGr0", "libSceUserService",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("83 3f 01")],
            ),
            (
                "user-service-name", "1xxcMiGu2fo", "libSceUserService",
                bytes.fromhex("bf 01 00 00 00") + b"\x48\xbe" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                bytes.fromhex("ba 10 00 00 00"),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 b"\x48\xb8Player 1" + bytes.fromhex("48 39 07")],
            ),
            (
                "user-service-stub", "GC18r56Bp7Y", "libSceUserService", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "system-service", "fZo48un7LK4", "libSceSystemService",
                bytes.fromhex("bf 04 00 00 00") + b"\x48\xbe" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbe" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("81 3e 78 00 00 00")],
            ),
            (
                "system-service-safe-area", "1n37q1Bvc5Y",
                "libSceSystemService",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("81 3f 00 00 80 3f")],
            ),
            (
                "system-service-stub", "t5ShV0jWEFE",
                "libSceSystemService", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "sysmodule", "g8cM39EUZ6o", "libSceSysmodule",
                bytes.fromhex("bf 2f 01 00 00"),
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "sysmodule-preload", "DOO+zuW1lrE", "libSceSysmodule", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "libc-internal", "j4ViWNHEgww", "libSceLibcInternal",
                b"\x48\xb8test\0\0\0\0" + b"\x48\xa3" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1900) + b"\x48\xbf" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 83 f8 04")],
            ),
            (
                "http2", "3JCe3lCbQ8A", "libSceHttp2", b"",
                [bytes.fromhex("48 3d 01 04 00 00")],
            ),
            (
                "netctl", "gky0+oaNM4k", "libSceNetCtl", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "ajm", "dl+4eHSzUu4", "libSceAjm",
                bytes.fromhex("31 ff") + b"\x48\xbe" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("81 3f 01 04 00 00")],
            ),
            (
                "ngs2", "koBbCMvOKWw", "libSceNgs2",
                bytes.fromhex("31 ff 31 f6") + b"\x48\xba" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("48 81 3f 01 04 00 00")],
            ),
            (
                "avplayer", "aS66RI0gGgo", "libSceAvPlayer",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 3d 01 04 00 00")],
            ),
            (
                "kernel-module-list", "IuxnUuXk6Bg", "libkernel",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                bytes.fromhex("be 04 00 00 00") + b"\x48\xba" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1920),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("83 3f 00"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1920) +
                 bytes.fromhex("48 83 3f 01")],
            ),
            (
                "kernel-module-info", "kUpgrXIrz7Q", "libkernel",
                b"\x48\xb8" + struct.pack("<Q", 352) + b"\x48\xa3" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                bytes.fromhex("31 ff") + b"\x48\xbe" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("48 81 3f 60 01 00 00"),
                 b"\x48\xb8kernel-m" + bytes.fromhex("48 39 47 08"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900 + 328) +
                 bytes.fromhex("83 3f 01")],
            ),
            (
                "kernel-module-from-address", "f7KBOafysXo", "libkernel",
                b"\x48\xb8" + struct.pack("<Q", 424) + b"\x48\xa3" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1900) + b"\x48\xbf" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1000) +
                bytes.fromhex("31 f6") + b"\x48\xba" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900 + 264) +
                 bytes.fromhex("83 3f 00"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900 + 416) +
                 bytes.fromhex("83 3f 01")],
            ),
            (
                "kernel-module-unwind", "RpQJJVKTiFM", "libkernel",
                b"\x48\xb8" + struct.pack("<Q", 304) + b"\x48\xa3" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1900) + b"\x48\xbf" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1000) +
                bytes.fromhex("31 f6") + b"\x48\xba" +
                struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900 + 288) +
                 b"\x48\xb8" + struct.pack("<Q", VIRTUAL_BASE + 0x1000) +
                 bytes.fromhex("48 39 07")],
            ),
            (
                "pthread-attr", "nsYoNRywwNg", "libkernel",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("48 81 3f 01 04 00 00")],
            ),
            (
                "pthread-mutex", "cmo1RIYva9o", "libkernel",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                bytes.fromhex("31 f6 31 d2"),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("48 81 3f 01 04 00 00")],
            ),
            (
                "pthread-cond", "2Tb92quprl0", "libkernel",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                bytes.fromhex("31 f6 31 d2"),
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "pthread-rwlock", "6ULAa0fq4jA", "libkernel",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                bytes.fromhex("31 f6"),
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "pthread-sem", "GEnUkDZoUwY", "libkernel",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                bytes.fromhex("31 f6 ba 02 00 00 00"),
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "pthread-key", "geDaqgH9lTg", "libkernel",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                bytes.fromhex("31 f6"),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("83 3f 00")],
            ),
            (
                "kernel-sdk-version", "YeU23Szo3BM", "libkernel",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("81 3f ff 0f 52 13")],
            ),
            (
                "kernel-flex-memory", "aNz11fnnzi4", "libkernel",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "kernel-event-flag", "BpFoboUJoZU", "libkernel",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                b"\x48\xbe" + struct.pack("<Q", VIRTUAL_BASE + 0x1920) +
                bytes.fromhex("31 d2 45 31 c0"),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("48 81 3f 01 04 00 00")],
            ),
            (
                "kernel-aio-init", "vYU8P9Td2Zo", "libkernel", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "kernel-sigemptyset", "+F7C-hdk7+E", "libkernel",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("48 83 3f 00")],
            ),
            (
                "net-htonl", "9T2pDF2Ryqg", "libSceNet",
                bytes.fromhex("bf 01 02 03 04"),
                [bytes.fromhex("48 3d 04 03 02 01")],
            ),
            (
                "net-htonll", "3CHi1K1wsCQ", "libSceNet",
                bytes.fromhex("48 bf 01 02 03 04 05 06 07 08"),
                [bytes.fromhex(
                    "48 b9 08 07 06 05 04 03 02 01 48 39 c8")],
            ),
            (
                "net-ntohs", "Rbvt+5Y2iEw", "libSceNet",
                bytes.fromhex("bf 12 34 00 00"),
                [bytes.fromhex("48 3d 34 12 00 00")],
            ),
            (
                "audio-in-hq-open", "nya-R5gDYhM", "libSceAudioIn",
                bytes.fromhex(
                    "31 ff 31 f6 31 d2 b9 00 01 00 00 "
                    "41 b8 80 bb 00 00 45 31 c9"),
                [bytes.fromhex("48 83 f8 01")],
            ),
            (
                "audio-in-stub", "IQtWgnrw6v8", "libSceAudioIn", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "audio-out-system-state", "R5hemoKKID8",
                "libSceAudioOut",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("48 83 3f 00")],
            ),
            (
                "audio-out-stub", "Iz9X7ISldhs", "libSceAudioOut", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "usbd-init", "TOhg7P6kTH4", "libSceUsbd", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "np-manager-state", "eQH7nWPcAgc", "libSceNpManager",
                bytes.fromhex("bf 01 00 00 00") +
                b"\x48\xbe" + struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("83 3f 01")],
            ),
            (
                "camera-not-attached", "p6n3Npi3YY4", "libSceCamera",
                bytes.fromhex("31 ff"), [bytes.fromhex("48 85 c0")],
            ),
            (
                "hmd-distortion-align", "1cS7W5J-v3k", "libSceHmd", b"",
                [bytes.fromhex("48 3d 00 04 00 00")],
            ),
            (
                "audio3d-default-params", "Im+jOoa5WAI", "libSceAudio3d",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("83 3f 20")],
            ),
            (
                "pad-get-info", "1Odcw19nADw", "libScePad",
                b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900),
                [bytes.fromhex("48 85 c0"),
                 b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
                 bytes.fromhex("48 83 3f 01")],
            ),
            (
                "pad-stub", "kazv1NzSB8c", "libScePad", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "save-data-stub", "dQ2GohUHXzk", "libSceSaveData", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "ssl-init", "hdpVEUDFW3s", "libSceSsl",
                bytes.fromhex("bf 00 10 00 00"),
                [bytes.fromhex("48 83 f8 01")],
            ),
            (
                "ssl-stub", "Pgt0gg14ewU", "libSceSsl", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "http-stub", "mNan6QSnpeY", "libSceHttp", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "font-stub", "coCrV6IWplE", "libSceFont", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "font-ft-stub", "e60aorDdpB8", "libSceFontFt", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "shell-core-stub", "5SfMtsW8h7A", "libSceShellCoreUtil", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "hmd-stub", "rU3HK9Q0r8o", "libSceHmd", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "np-tus-stub", "lL+Z3zCKNTs", "libSceNpTus", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "lnc-util-stub", "V350H0h35IU", "libSceLncUtil", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            (
                "np-trophy-stub", "aTnHs7W-9Uk", "libSceNpTrophy", b"",
                [bytes.fromhex("48 85 c0")],
            ),
            ("np-webapi-stub", "KQIkDGf80PQ", "libSceNpWebApi", b"",
             [bytes.fromhex("48 85 c0")]),
            ("game-live-stub", "NqkTzemliC0", "libSceGameLiveStreaming", b"",
             [bytes.fromhex("48 85 c0")]),
            ("remoteplay-stub", "xQeIryTX7dY", "libSceRemoteplay", b"",
             [bytes.fromhex("48 85 c0")]),
            ("ime-stub", "mN+ZoSN-8hQ", "libSceIme", b"",
             [bytes.fromhex("48 85 c0")]),
            ("np-tus-compat-stub", "cRVmNrJDbG8", "libSceNpTusCompat", b"",
             [bytes.fromhex("48 85 c0")]),
            ("razor-cpu-stub", "JFzLJBlYIJE", "libSceRazorCpu", b"",
             [bytes.fromhex("48 85 c0")]),
            ("rudp-stub", "uQiK7fjU6y8", "libSceRudp", b"",
             [bytes.fromhex("48 85 c0")]),
            ("voice-stub", "oV9GAdJ23Gw", "libSceVoice", b"",
             [bytes.fromhex("48 85 c0")]),
            ("system-state-stub", "6gtqLPVTdJY", "libSceSystemStateMgr", b"",
             [bytes.fromhex("48 85 c0")]),
            ("camera-stub", "0wnf2a60FqI", "libSceCamera", b"",
             [bytes.fromhex("48 85 c0")]),
            ("np-party-stub", "+v4fVHMwFWc", "libSceNpParty", b"",
             [bytes.fromhex("48 85 c0")]),
            ("app-content-stub", "+OlXCu8qxUk", "libSceAppContent", b"",
             [bytes.fromhex("48 85 c0")], "libSceAppContentUtil"),
            ("app-content-base-stub", "xmhnAoxN3Wk", "libSceAppContent", b"",
             [bytes.fromhex("48 85 c0")]),
            ("share-play-stub", "+MCXJlWdi+s", "libSceSharePlay", b"",
             [bytes.fromhex("48 85 c0")]),
            ("system-gesture-stub", "0KrW5eMnrwY", "libSceSystemGesture", b"",
             [bytes.fromhex("48 85 c0")]),
            ("vr-tracker-stub", "5IFOAYv-62g", "libSceVrTracker", b"",
             [bytes.fromhex("48 85 c0")]),
            ("netctl-ap-ipc-stub", "3pxwYqHzGcw", "libSceNetCtlApIpcInt", b"",
             [bytes.fromhex("48 85 c0")]),
            ("companion-httpd-stub", "+-du9tWgE9s", "libSceCompanionHttpd", b"",
             [bytes.fromhex("48 85 c0")]),
            ("audio3d-stub", "8hm6YdoQgwg", "libSceAudio3d", b"",
             [bytes.fromhex("48 85 c0")]),
            ("screenshot-stub", "2xxUtuC-RzE", "libSceScreenShot", b"",
             [bytes.fromhex("48 85 c0")]),
            ("np-score-stub", "3Ybj4E1qNtY", "libSceNpScore", b"",
             [bytes.fromhex("48 85 c0")]),
            ("netctl-ap-stub", "19Ec7WkMFfQ", "libSceNetCtlAp", b"",
             [bytes.fromhex("48 85 c0")]),
            ("mouse-stub", "1FeceR5YhAo", "libSceMouse", b"",
             [bytes.fromhex("48 85 c0")]),
            ("app-messaging-stub", "+zuv20FsXrA", "libSceAppMessaging", b"",
             [bytes.fromhex("48 85 c0")]),
            ("net-bwe-stub", "0lViPaTB-R8", "libSceNetBwe", b"",
             [bytes.fromhex("48 85 c0")]),
            ("web-browser-dialog-stub", "Cya+jvTtPqg", "libSceWebBrowserDialog", b"", [bytes.fromhex("48 85 c0")]),
            ("signin-dialog-stub", "2m077aeC+PA", "libSceSigninDialog", b"", [bytes.fromhex("48 85 c0")]),
            ("activate-hevc-stub", "+2uXfrrQCyk", "libSceSystemServiceActivateHevc", b"", [bytes.fromhex("48 85 c0")]),
            ("activate-hevc-soft-stub", "djVe06YjzkI", "libSceSystemServiceActivateHevcSoft", b"", [bytes.fromhex("48 85 c0")]),
            ("hmd-setup-dialog-stub", "+z4OJmFreZc", "libSceHmdSetupDialog", b"", [bytes.fromhex("48 85 c0")]),
            ("activate-mpeg2-stub", "-7zMNJ1Ap1c", "libSceSystemServiceActivateMpeg2", b"", [bytes.fromhex("48 85 c0")]),
            ("np-party-compat-stub", "F1P+-wpxQow", "libSceNpPartyCompat", b"", [bytes.fromhex("48 85 c0")]),
            ("hmd-distortion-stub", "8A4T5ahi790", "libSceHmdDistortion", b"", [bytes.fromhex("48 85 c0")]),
            ("netctl-v6-stub", "+lxqIKeU9UY", "libSceNetCtlV6", b"", [bytes.fromhex("48 85 c0")]),
            ("gnm-resource-stub", "+RaJBCVJZVM", "libSceGnmDriverResourceRegistration", b"", [bytes.fromhex("48 85 c0")]),
            ("vr-gpu-test-stub", "5ucmy8hcSPk", "libSceVrTrackerGpuTest", b"", [bytes.fromhex("48 85 c0")]),
            ("np-webapi2-stub", "2hlBNB96saE", "libSceNpWebApi2", b"", [bytes.fromhex("48 85 c0")]),
            ("companion-util-stub", "cE5Msy11WhU", "libSceCompanionUtil", b"", [bytes.fromhex("48 85 c0")]),
            ("vr-live-capture-stub", "3YCwwpHkHIg", "libSceVrTrackerLiveCapture", b"", [bytes.fromhex("48 85 c0")]),
            ("common-dialog-stub", "2RdicdHhtGA", "libSceCommonDialog", b"",
             [bytes.fromhex("48 85 c0")]),
            ("np-manager-stub", "uqcPJLWL08M", "libSceNpManager", b"",
             [bytes.fromhex("48 85 c0")]),
            ("np-common-cmp", "i8UmXTSq7N4", "libSceNpCommon", b"\x48\x89\xe7\x48\x89\xe6",
             [bytes.fromhex("48 85 c0")]),
            ("np-common-cmp-invalid", "i8UmXTSq7N4", "libSceNpCommon",
             bytes.fromhex("31 ff 31 f6"), [bytes.fromhex("3d 03 00 55 80")]),
            ("np-common-valid-online-id", "hkeX9iuCwlI", "libSceNpCommon", bytes.fromhex(
                "c7 44 24 e0 61 62 63 00 48 c7 44 24 f0 00 00 00 00 48 8d 7c 24 e0"),
             [bytes.fromhex("48 83 f8 01")]),
            ("np-common-clock", "PVVsRmMkO1g", "libSceNpCommonCompat", bytes.fromhex(
                "48 8d 7c 24 e0"), [bytes.fromhex("48 85 c0")], "libSceNpCommon"),
            ("np-common-platform", "sXVQUIGmk2U", "libSceNpCommon", bytes.fromhex(
                "66 0f ef c0 0f 11 44 24 d0 0f 11 44 24 e0 0f 11 44 24 f0 48 8d 7c 24 d0"),
             [bytes.fromhex("48 85 c0")]),
            ("np-common-sdk", "Pglk7zFj0DI", "libSceNpCommon", bytes.fromhex(
                "48 8d 7c 24 e0"), [bytes.fromhex("48 85 c0")]),
            ("np-common-mutex-init", "uEwag-0YZPc", "libSceNpCommon", bytes.fromhex(
                "48 8d 7c 24 e0 31 f6 31 d2"), [bytes.fromhex("48 85 c0")]),
            ("rtc-days-in-february", "3O7Ln8AqJ1o", "libSceRtc",
             bytes.fromhex("bf e8 07 00 00 be 02 00 00 00"),
             [bytes.fromhex("48 83 f8 1d")]),
            ("rtc-leap-year", "Ug8pCwQvh0c", "libSceRtc",
             bytes.fromhex("bf d0 07 00 00"),
             [bytes.fromhex("48 83 f8 01")]),
            ("rtc-invalid-pointer", "lPEBYdVX0XQ", "libSceRtc",
             bytes.fromhex("31 ff"),
             [bytes.fromhex("3d 02 00 b5 80")]),
            ("rtc-date-to-tick", "8w-H19ip48I", "libSceRtc",
             bytes.fromhex(
                 "66 c7 44 24 c0 e8 07 66 c7 44 24 c2 02 00 "
                 "66 c7 44 24 c4 1d 00 66 c7 44 24 c6 0c 00 "
                 "66 c7 44 24 c8 22 00 66 c7 44 24 ca 38 00 "
                 "c7 44 24 cc 40 e2 01 00 48 8d 7c 24 c0 "
                 "48 8d 74 24 e0"),
             [bytes.fromhex("48 85 c0"),
              b"\x48\xb9" + struct.pack("<Q", 63844806896123456) +
              bytes.fromhex("48 39 4c 24 e0")]),
            ("rtc-add-day", "NR1J0N7L2xY", "libSceRtc",
             b"\x48\xb8" + struct.pack("<Q", 63844806896123456) +
             bytes.fromhex(
                 "48 89 44 24 c0 48 8d 7c 24 d0 48 8d 74 24 c0 "
                 "ba 01 00 00 00"),
             [bytes.fromhex("48 85 c0"),
              b"\x48\xb9" + struct.pack("<Q", 63844893296123456) +
              bytes.fromhex("48 39 4c 24 d0")]),
            ("rtc-parse-rfc3339", "99bMGglFW3I", "libSceRtc",
             b"\x48\xb8" + b"1970-01-" + bytes.fromhex("48 89 44 24 c0") +
             b"\x48\xb8" + b"01T00:00" + bytes.fromhex("48 89 44 24 c8") +
             b"\x48\xb8" + b":00Z\0\0\0\0" + bytes.fromhex(
                 "48 89 44 24 d0 48 8d 7c 24 e0 48 8d 74 24 c0"),
             [bytes.fromhex("48 85 c0"),
              b"\x48\xb9" + struct.pack("<Q", 62135596800000000) +
              bytes.fromhex("48 39 4c 24 e0")]),
            ("rtc-format-rfc3339", "WJ3rqFwymew", "libSceRtc",
             b"\x48\xb8" + struct.pack("<Q", 62135596800000000) +
             bytes.fromhex(
                 "48 89 44 24 c0 48 8d 7c 24 d0 48 8d 74 24 c0 31 d2"),
             [bytes.fromhex("48 85 c0"),
              b"\x48\xb9" + b"1970-01-" +
              bytes.fromhex("48 39 4c 24 d0")]),
            ("np-webapi-init-invalid", "G3AnLNdRBjE", "libSceNpWebApi",
             bytes.fromhex("31 ff be 00 00 10 00"),
             [bytes.fromhex("3d 02 29 55 80")]),
            ("np-matching2-init-invalid", "10t3e5+JPnU",
             "libSceNpMatching2", bytes.fromhex("31 ff"),
             [bytes.fromhex("3d 0a 0c 55 80")]),
            ("np-score-title-invalid", "KnNA1TEgtBI", "libSceNpScore",
             bytes.fromhex("31 ff 31 f6"),
             [bytes.fromhex("3d 0c 07 55 80")]),
            ("np-tus-title-invalid", "BIkMmUfNKWM", "libSceNpTus",
             bytes.fromhex("31 ff 31 f6"),
             [bytes.fromhex("3d 0c 07 55 80")]),
            ("np-signaling-create-before-init", "5yYjEdd4t8Y",
             "libSceNpSignaling", bytes.fromhex("31 ff 31 f6 31 d2 31 c9"),
             [bytes.fromhex("3d 01 27 55 80")]),
            ("np-webapi-filter-invalid", "y5Ta5JCzQHY",
             "libSceNpWebApi", bytes.fromhex("31 ff 31 f6 31 d2"),
             [bytes.fromhex("3d 02 29 55 80")]),
            ("np-webapi-create-request-invalid", "rdgs5Z1MyFw",
             "libSceNpWebApi", bytes.fromhex("31 ff 31 f6 31 d2"),
             [bytes.fromhex("3d 02 29 55 80")]),
            ("np-webapi2-user-invalid", "sk54bi6FtYM",
             "libSceNpWebApi2", bytes.fromhex("31 ff 31 f6"),
             [bytes.fromhex("3d 03 34 55 80")]),
            ("playgo-before-init", "rvBSfTimejE", "libScePlayGo",
             bytes.fromhex("bf 01 00 00 00 31 f6"),
             [bytes.fromhex("3d 05 00 b2 80")]),
            ("np-webapi2-push-invalid", "WV1GwM32NgY",
             "libSceNpWebApi2", bytes.fromhex("31 ff"),
             [bytes.fromhex("3d 03 34 55 80")]),
            ("np-auth-request-missing", "cE7wIsqXdZ8", "libSceNpAuth",
             bytes.fromhex("31 ff"),
             [bytes.fromhex("3d 06 03 55 80")]),
            ("ime-close-before-open", "TmVP8LzcFcY", "libSceIme", b"",
             [bytes.fromhex("3d 02 00 bc 80")]),
            ("trophy-context-invalid", "XbkjbobZlCY", "libSceNpTrophy",
             bytes.fromhex("31 ff 31 f6 31 d2 31 c9"),
             [bytes.fromhex("3d 04 16 55 80")]),
            ("videodec2-query-null", "RnDibcGCPKw", "libSceVideodec2",
             bytes.fromhex("31 ff"),
             [bytes.fromhex("3d 02 01 1d 81")]),
            ("np-commerce-close-before-init", "NU3ckGHMFXo",
             "libSceNpCommerce", b"",
             [bytes.fromhex("3d 03 00 b8 80")]),
            ("ssl2-alpn-reference-stub", "4O7+bRkRUe8", "libSceSsl", b"",
             [bytes.fromhex("48 85 c0")]),
            ("ime-dialog-position-before-init", "8jqzzPioYl8",
             "libSceImeDialog", bytes.fromhex("31 ff"),
             [bytes.fromhex("3d 07 01 bc 80")]),
            ("invitation-close-before-init", "WWtCL5lzi7Y",
             "libSceInvitationDialog", b"",
             [bytes.fromhex("3d 0b 00 b8 80")]),
            ("playgo-dialog-result-null", "wx9TDplJKB4",
             "libScePlayGoDialog", bytes.fromhex("31 ff"),
             [bytes.fromhex("3d 0d 00 b8 80")]),
            ("profile-dialog-close-before-init", "wkwjz0Xdo2A",
             "libSceNpProfileDialog", b"",
             [bytes.fromhex("3d 03 00 b8 80")]),
            ("vr-tracker-term-before-init", "IBv4P3q1pQ0",
             "libSceVrTracker", b"",
             [bytes.fromhex("3d 01 08 26 81")]),
            ("invitation-dialog-compat", "8XKR6wa64iQ",
             "libSceInvitationDialogCompat", bytes.fromhex("31 ff"),
             [bytes.fromhex("3d 03 00 b8 80")],
             "libSceInvitationDialog"),
            ("profile-dialog-compat", "uj9Cz7Tk0cc",
             "libSceNpProfileDialogCompat", bytes.fromhex("31 ff"),
             [bytes.fromhex("3d 06 00 b8 80")],
             "libSceNpProfileDialog"),
            ("vr-tracker-device-rejection", "jGqEkPy0iLU",
             "libSceVrTrackerDeviceRejection", b"",
             [bytes.fromhex("48 85 c0")], "libSceVrTracker"),
            ("vr-tracker-four-device-before-init", "24kDA+A0Ox0",
             "libSceVrTrackerFourDeviceAllowed",
             bytes.fromhex("31 ff 31 f6"),
             [bytes.fromhex("3d 01 08 26 81")], "libSceVrTracker"),
            ("http-get-nonblock-invalid", "Wq4RNB3snSQ", "libSceHttp",
             bytes.fromhex("31 ff 31 f6"),
             [bytes.fromhex("48 83 f8 ea")]),
            ("http-uri-escape", "YuOW3dDAKYc", "libSceHttp",
             bytes.fromhex(
                 "c7 44 24 e0 61 20 62 00 "
                 "48 8d 7c 24 c0 48 8d 74 24 d0 ba 40 00 00 00 "
                 "48 8d 4c 24 e0"),
             [bytes.fromhex("48 85 c0"),
              bytes.fromhex("48 83 7c 24 d0 06"),
              b"\x48\xb9" + b"a%20b\0\0\0" +
              bytes.fromhex("48 39 4c 24 c0")]),
            ("savedata-progress-before-init", "ANmSWUiyyGQ",
             "libSceSaveData", bytes.fromhex("31 ff"),
             [bytes.fromhex("48 83 f8 ea")]),
            ("savedata-backup-before-init", "z1JA8-iJt3k",
             "libSceSaveData", bytes.fromhex("31 ff"),
             [bytes.fromhex("3d 01 00 9f 80")]),
            ("app-content-sku", "99b82IKXpH4", "libSceAppContent",
             bytes.fromhex("31 ff") +
             b"\x48\xbe" + struct.pack("<Q", VIRTUAL_BASE + 0x1A00),
             [bytes.fromhex("48 85 c0"),
              b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1A00) +
              bytes.fromhex("83 3f 03")], "libSceAppContentUtil"),
            ("move-init", "j1ITE-EoJmE", "libSceMove", b"",
             [bytes.fromhex("48 85 c0")]),
            ("zlib-init", "m1YErdIXCp4", "libSceZlib", b"",
             [bytes.fromhex("48 85 c0")]),
            ("png-enc-query-invalid", "9030RnBDoh4", "libScePngEnc",
             bytes.fromhex("31 ff"),
             [bytes.fromhex("3d 01 01 69 80")]),
            ("png-enc-query-valid", "9030RnBDoh4", "libScePngEnc",
             b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1A00) +
             bytes.fromhex(
                 "c7 07 10 00 00 00 c7 47 04 00 00 00 00 "
                 "c7 47 08 40 00 00 00 c7 47 0c 05 00 00 00"),
             [bytes.fromhex("48 83 f8 10")]),
            ("disc-map-no-bitmap", "fl1eoDnwQ4s", "libSceDiscMap", b"",
             [bytes.fromhex("3d 04 00 10 81")]),
            ("font-style-init", "la2AOWnHEAc", "libSceFont",
             b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1A00),
             [bytes.fromhex("48 85 c0"),
              b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1A00) +
              bytes.fromhex("66 81 3f 09 0f")]),
            ("fiber-option-init", "asjUJJ+aa8s", "libSceFiber",
             b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1A00),
             [bytes.fromhex("48 85 c0"),
              b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1A00) +
              bytes.fromhex("81 3f 4d e6 40 bb")]),
            ("np-callout-init", "9+m5nRdJ-wQ", "libSceNpCommonCompat",
             b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1A00),
             [bytes.fromhex("48 85 c0"),
              b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1A18) +
              bytes.fromhex("83 3f 01")], "libSceNpCommon"),
            ("video-resolution-invalid-handle", "6kPnj51T62Y",
             "libSceVideoOut", bytes.fromhex("31 ff 31 f6"),
             [bytes.fromhex("3d 0b 00 29 80")]),
            ("usbd-stub", "ZfbvM+OP-1A", "libSceUsbd", b"",
             [bytes.fromhex("48 85 c0")]),
            ("content-export-stub", "FzEWeYnAFlI", "libSceContentExport", b"",
             [bytes.fromhex("48 85 c0")]),
            ("np-partner-stub", "pMxXhNozUX8", "libSceNpPartner001", b"",
             [bytes.fromhex("48 85 c0")]),
            ("videodec-stub", "U0kpGF1cl90", "libSceVideodec", b"",
             [bytes.fromhex("48 85 c0")]),
            ("error-dialog-stub", "jrpnVQfJYgQ", "libSceErrorDialog", b"",
             [bytes.fromhex("48 85 c0")]),
            ("playgo-stub", "uEqMfMITvEI", "libSceDbgPlayGo", b"",
             [bytes.fromhex("48 85 c0")], "libScePlayGo"),
            ("pngdec-stub", "cJ--1xAbj-I", "libScePngDec", b"",
             [bytes.fromhex("48 85 c0")]),
            ("jpeg-enc-stub", "QbrU0cUghEM", "libSceJpegEnc", b"",
             [bytes.fromhex("48 85 c0")]),
            ("np-auth-stub", "PM3IZCw-7m0", "libSceNpAuth", b"",
             [bytes.fromhex("48 85 c0")]),
            ("np-profile-dialog-stub", "nrQRlLKzdwE", "libSceNpProfileDialog", b"",
             [bytes.fromhex("48 85 c0")]),
            ("np-facebook-dialog-stub", "fjV7C8H0Y8k", "libSceNpSnsFacebookDialog", b"",
             [bytes.fromhex("48 85 c0")]),
            ("video-out-stub", "MTxxrOCeSig", "libSceVideoOut", b"",
             [bytes.fromhex("48 85 c0")]),
            ("video-recording-stub", "Fc8qxlKINYQ", "libSceVideoRecording", b"",
             [bytes.fromhex("48 85 c0")]),
            ("residual-common-unused", "BQ3tey0JmQM", "libSceCommonDialog", b"",
             [bytes.fromhex("48 85 c0")]),
            ("residual-companion-event", "Vku4big+IYM", "libSceCompanionHttpd",
             b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900),
             [bytes.fromhex("3d 08 00 e4 80"),
              b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
              bytes.fromhex("81 3f 02 00 00 10")]),
            ("residual-game-live-debug", "caqgDl+V9qA",
             "libSceGameLiveStreaming_debug", b"",
             [bytes.fromhex("48 85 c0")], "libSceGameLiveStreaming"),
            ("residual-mouse-init", "Qs0wWulgl7U", "libSceMouse", b"",
             [bytes.fromhex("48 85 c0")]),
            ("residual-random", "PI7jIZj4pcE", "libSceRandom",
             b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
             bytes.fromhex("be 08 00 00 00"),
             [bytes.fromhex("48 85 c0")]),
            ("random-invalid-size", "PI7jIZj4pcE", "libSceRandom",
             b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
             bytes.fromhex("be 41 00 00 00"),
             [bytes.fromhex("3d 16 00 7c 81")]),
            ("videodec-invalid-config", "leCAscipfFY", "libSceVideodec",
             b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
             bytes.fromhex("48 c7 07 28 00 00 00 c7 47 14 ff ff ff ff") +
             b"\x48\xbe" + struct.pack("<Q", VIRTUAL_BASE + 0x1940) +
             bytes.fromhex("48 c7 06 38 00 00 00"),
             [bytes.fromhex("3d 0e 00 c1 80")]),
            ("residual-remoteplay-status", "g3PNjYKWqnQ", "libSceRemoteplay",
             bytes.fromhex("bf 01 00 00 00") + b"\x48\xbe" +
             struct.pack("<Q", VIRTUAL_BASE + 0x1900),
             [bytes.fromhex("48 85 c0"),
              b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
              bytes.fromhex("83 3f 00")]),
            ("residual-savedata-ready", "en7gNVnh878", "libSceSaveDataDialog", b"",
             [bytes.fromhex("48 83 f8 01")]),
            ("residual-web-limited", "8r4EJ3FiX4w",
             "libSceWebBrowserDialogLimited", b"",
             [bytes.fromhex("48 85 c0")], "libSceWebBrowserDialogLimited"),
            ("residual-ulobj-success", "HZ9Q2c+4BU4", "libSceUlt", b"",
             [bytes.fromhex("48 85 c0")], "libSceUlt"),
        ]
        for service_case in service_cases:
            name, nid, library, setup, checks, *module_override = service_case
            service_elf = temp / f"{name}.elf"
            service_trace = temp / f"{name}.log"
            failure_offset = make_hle_import_elf(
                service_elf, nid, library, setup, checks,
                module=module_override[0] if module_override else None)
            subprocess.run(
                [sys.executable, __file__, "--worker", str(dll_path),
                 str(service_elf), str(service_trace), "linked"], check=True)
            trace = service_trace.read_text(encoding="utf-8")
            failure_rip = f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}"
            assert failure_rip not in trace, (
                f"service case {name} ({nid}|{library}) reached "
                f"failure RIP {failure_rip}\n{trace}"
            )

        alias_elf = temp / "kernel-library-alias.elf"
        alias_trace = temp / "kernel-library-alias.log"
        failure_offset = make_hle_import_elf(
            alias_elf, "6XG4B33N09g", "libScePosix", b"",
            [bytes.fromhex("48 85 c0")], module="libkernel")
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(alias_elf), str(alias_trace), "linked"], check=True)
        trace = alias_trace.read_text(encoding="utf-8")
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" not in trace

        load_elf = temp / "kernel-load.elf"
        load_trace = temp / "kernel-load.log"
        load_result = VIRTUAL_BASE + 0x1940
        failure_offset = make_hle_import_elf(
            load_elf, "wzvqT4UqKX8", "libkernel",
            b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
            bytes.fromhex("31 f6 31 d2 45 31 d2 45 31 c0") +
            b"\x49\xb9" + struct.pack("<Q", load_result),
            [bytes.fromhex("48 85 c0"),
             b"\x48\xbf" + struct.pack("<Q", load_result) +
             bytes.fromhex("83 3f 00")])
        image = bytearray(load_elf.read_bytes())
        image[0x1900:0x1900 + len(b"/app0/kernel-load.elf\0")] = \
            b"/app0/kernel-load.elf\0"
        load_elf.write_bytes(image)
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(load_elf), str(load_trace), "linked"], check=True)
        trace = load_trace.read_text(encoding="utf-8")
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" not in trace

        dlsym_elf = temp / "kernel-dlsym.elf"
        dlsym_trace = temp / "kernel-dlsym.log"
        dlsym_name = "test_symbol"
        dlsym_output = VIRTUAL_BASE + 0x1940
        export_value = 0x1A00
        failure_offset = make_hle_import_elf(
            dlsym_elf, "LwG8g3niqwA", "libkernel",
            bytes.fromhex("31 ff") + b"\x48\xbe" +
            struct.pack("<Q", VIRTUAL_BASE + 0x1900) + b"\x48\xba" +
            struct.pack("<Q", dlsym_output),
            [bytes.fromhex("48 85 c0"),
             b"\x48\xbf" + struct.pack("<Q", dlsym_output) +
             b"\x48\xb8" + struct.pack("<Q", VIRTUAL_BASE + export_value) +
             bytes.fromhex("48 39 07")],
            export_nid=symbol_nid(dlsym_name), export_value=export_value)
        image = bytearray(dlsym_elf.read_bytes())
        image[0x1900:0x1900 + len(dlsym_name) + 1] = \
            dlsym_name.encode() + b"\0"
        dlsym_elf.write_bytes(image)
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(dlsym_elf), str(dlsym_trace), "linked"], check=True)
        trace = dlsym_trace.read_text(encoding="utf-8")
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" not in trace

        object_elf = temp / "kernel-object.elf"
        object_trace = temp / "kernel-object.log"
        failure_offset = make_hle_import_elf(
            object_elf, "f7uOxY9mM1U", "libkernel", checks=[
                bytes.fromhex("48 8b 08") +
                b"\x48\xba" + struct.pack("<Q", 0xDEADBEEF54321ABC) +
                bytes.fromhex("48 39 d1"),
            ], symbol_type=1, invoke=False)
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(object_elf), str(object_trace), "linked"], check=True)
        trace = object_trace.read_text(encoding="utf-8")
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" not in trace

        libc_object_elf = temp / "libc-object.elf"
        libc_object_trace = temp / "libc-object.log"
        failure_offset = make_hle_import_elf(
            libc_object_elf, "ZT4ODD2Ts9o", "libSceLibcInternal",
            checks=[bytes.fromhex("48 83 38 00")],
            symbol_type=1, invoke=False)
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(libc_object_elf), str(libc_object_trace), "linked"],
            check=True)
        trace = libc_object_trace.read_text(encoding="utf-8")
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" not in trace

        equeue_elf = temp / "kernel-equeue.elf"
        equeue_trace = temp / "kernel-equeue.log"
        failure_offset = make_hle_import_elf(
            equeue_elf, "D0OdFMjp46I", "libkernel",
            b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
            b"\x48\xbe" + struct.pack("<Q", VIRTUAL_BASE + 0x1920),
            [bytes.fromhex("48 85 c0"),
             b"\x48\xbf" + struct.pack("<Q", VIRTUAL_BASE + 0x1900) +
             bytes.fromhex("48 81 3f 00 01 00 00")])
        image = bytearray(equeue_elf.read_bytes())
        image[0x1920:0x192B] = b"test-queue\0"
        equeue_elf.write_bytes(image)
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(equeue_elf), str(equeue_trace), "linked"], check=True)
        trace = equeue_trace.read_text(encoding="utf-8")
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" not in trace

        tls_root = temp / "module-tls"
        tls_root.mkdir()
        tls_elf, failure_offset = make_tls_module_pair(tls_root)
        tls_trace = temp / "module-tls.log"
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(tls_elf), str(tls_trace), "linked"], check=True)
        trace = tls_trace.read_text(encoding="utf-8")
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" not in trace

        fault_elf = temp / "faulting-orbis.elf"
        fault_trace = temp / "fault.log"
        failure_offset = make_relocated_elf(fault_elf)
        fault_image = bytearray(fault_elf.read_bytes())
        pack_into(fault_image, 0x490, "q", 0x1235)
        fault_elf.write_bytes(fault_image)
        subprocess.run(
            [sys.executable, __file__, "--worker", str(dll_path),
             str(fault_elf), str(fault_trace), "fault"], check=True)
        trace = fault_trace.read_text(encoding="utf-8")
        assert f"0x{VIRTUAL_BASE + 0x1000 + failure_offset:x}" in trace
        assert "0x0000b000" in trace

        malformed = temp / "malformed-tls-size.elf"
        make_relocated_elf(malformed)
        image = bytearray(malformed.read_bytes())
        pack_into(image, 64 + 168 + 32, "QQ", 0x41, 0x40)
        malformed.write_bytes(image)
        run_invalid_image(dll_path, malformed,
                          "ELF TLS header has an invalid size or alignment")

        malformed = temp / "malformed-tls-align.elf"
        make_relocated_elf(malformed)
        image = bytearray(malformed.read_bytes())
        pack_into(image, 64 + 168 + 48, "Q", 3)
        malformed.write_bytes(image)
        run_invalid_image(dll_path, malformed,
                          "ELF TLS header has an invalid size or alignment")

        malformed = temp / "duplicate-tls.elf"
        make_relocated_elf(malformed)
        image = bytearray(malformed.read_bytes())
        pack_into(image, 56, "H", 5)
        image[64 + 224:64 + 280] = image[64 + 168:64 + 224]
        malformed.write_bytes(image)
        run_invalid_image(dll_path, malformed,
                          "ELF has multiple PT_TLS headers")

        malformed = temp / "overflowing-relocation.elf"
        make_relocated_elf(malformed)
        image = bytearray(malformed.read_bytes())
        pack_into(image, 0x490, "q", -(1 << 63))
        malformed.write_bytes(image)
        run_invalid_image(dll_path, malformed,
                          "relative relocation value overflows")


def run_invalid_image(dll_path, elf_path, expected_error):
    trace_path = elf_path.with_suffix(".log")
    result = subprocess.run(
        [sys.executable, __file__, "--worker", str(dll_path),
         str(elf_path), str(trace_path), "invalid"],
        check=True, capture_output=True, text=True)
    assert expected_error in result.stderr, result.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dll", type=Path, nargs="?")
    parser.add_argument("--worker", nargs=4,
                        metavar=("DLL", "ELF", "TRACE", "MODE"))
    args = parser.parse_args()
    if args.worker:
        dll, elf, trace, mode = args.worker
        run_worker(Path(dll).resolve(), Path(elf).resolve(),
                   Path(trace).resolve(), mode)
        return 0
    if not args.dll:
        parser.error("DLL is required")
    run_test(args.dll.resolve())
    print("shadPS4 Phase 7 loader/HLE/TCG test: PASS")


if __name__ == "__main__":
    sys.exit(main())
