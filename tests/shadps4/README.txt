shadPS4 loader and bootstrap smoke test
========================================

The test creates an original synthetic ELF64 Orbis image. It contains one
R_X86_64_RELATIVE relocation and code that branches to UD2 when a bootstrap or
runtime check fails. No firmware, keys, commercial code, or copyrighted system
modules are required.

Run after building qemu-system-shadps4.dll:

  python tests/shadps4/test_loader.py \
    build-shadps4-check/qemu-system-shadps4.dll

Optional local-title smoke tests (commercial title data is never bundled):

  python tests/shadps4/test_titles.py \
    build-shadps4-check/qemu-system-shadps4.dll

The default matrix is CUSA02456, CUSA13032 and the PS2 Classics title
SLES50541. Use --title to select one title and --duration to change the run
time. SLES50541 exercises the Orbis PS2-emulator wrapper and its internal
libkernel_ps2emu, menu-dialog, video-recording and coredump imports.
The title test fails on premature guest shutdown, a nonzero guest status,
missing video callbacks, or a run in which every delivered frame is black.

The test covers:
  - ET_SCE_DYNEXEC and FreeBSD ABI validation;
  - PT_DYNAMIC and PT_SCE_DYNLIBDATA parsing;
  - PT_TLS initialization before the TCB;
  - multiple TLS modules, DTV slots and R_X86_64_DTPMOD64;
  - __tls_get_addr through a guest HLE trampoline;
  - symbol-table bounds;
  - SELF PT_DYNAMIC data embedded in a blocked PT_SCE_DYNLIBDATA segment;
  - R_X86_64_RELATIVE application;
  - isolated PRX bases and cross-module NID relocation;
  - libkernel NID registration and execution of an HLE export under TCG;
  - x86_64 user page tables, stack, long mode, RIP and RSP;
  - CPL3 code/data selectors and supervisor-only low bootstrap memory;
  - Orbis-style entry parameters and process-exit trampoline;
  - initial TCB/DTV area and compatible FS/GS TLS bases;
  - GDT, IDT and 64-bit TSS with an exception stop handler;
  - SYSCALL/SYSRET transition through the supervisor HLE gateway;
  - identity, time, thread ID and FreeBSD ENOSYS behavior;
  - dynamic mmap/munmap with guest page-table updates;
  - virtual descriptors, /dev/gc, close and empty equeue polling;
  - minimal _umtx_op wake synchronization;
  - transactional PM4 submit, fence failure, flip and status counters;
  - complete gamepad, touch, motion, absolute pointer and pad output paths;
  - callback and XAudio2 AudioOut plus bounded AudioIn;
  - title-scoped storage, read-only app0, directories and atomic replacement;
  - asynchronous dialogs and duplicate-response rejection;
  - host-controlled IPv4 networking and private-network capability checks;
  - Fiber option initialization and explicit unsupported context switching;
  - HTTP URI escaping, SaveData backup preconditions and NP callout state;
  - NP Matching2, Score, TUS, Signaling and WebApi residual dispatch/errors;
  - UWP-safe Usbd, NP Manager, Camera, Hmd and Audio3d state/output paths;
  - PlayGo, WebApi2 push, NpAuth, IME and Trophy dispatch/error contracts;
  - Videodec2, Commerce, SSL2, IME/Invitation/PlayGo/Profile dialogs and
    VrTracker dispatch/error contracts;
  - residual VideoOut NID routing and exact invalid-handle errors;
  - exact Random invalid-size errors and bounded Videodec resource queries;
  - Liverpool/base and Neo profiles with AMD Jaguar CPUID and eight CPUs;
  - malformed TLS headers and signed relocation overflow rejection;
  - invalid host input indices, key codes and incomplete PCM frames;
  - controlled shutdown after an unhandled guest exception;
  - opt-in external HLE dispatch with module/library/NID metadata and bounded
    synchronous guest-memory accessors;
  - SystemService LoadExec forwarding with path and argv preservation;
  - getpid and exit shutdown behavior;
  - TCG execution through qemu_host_main_loop_step;
  - request_shutdown and cleanup without terminating the host process.
