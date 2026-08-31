#ifndef QEMU_CXBX_CPU_H
#define QEMU_CXBX_CPU_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define QEMU_CXBX_EXPORT __declspec(dllexport)
#define QEMU_CXBX_CALL __cdecl
#else
#define QEMU_CXBX_EXPORT __attribute__((visibility("default")))
#define QEMU_CXBX_CALL
#endif

#define QEMU_CXBX_CPU_ABI_VERSION UINT32_C(0x00010000)
#define QEMU_CXBX_KERNEL_GATEWAY_BASE UINT32_C(0xA0000000)
#define QEMU_CXBX_XBDM_GATEWAY_BASE UINT32_C(0xA0010000)
#define QEMU_CXBX_XDK_GATEWAY_BASE UINT32_C(0xA0020000)
#define QEMU_CXBX_THREAD_RETURN_GATEWAY UINT32_C(0xA003FFF0)
#define QEMU_CXBX_GATEWAY_STRIDE UINT32_C(16)
#define QEMU_CXBX_KERNEL_ORDINAL_COUNT UINT32_C(379)

typedef struct QemuCxbxCpu QemuCxbxCpu;

enum {
    QEMU_CXBX_CPU_OK = 0,
    QEMU_CXBX_CPU_INVALID_ARGUMENT = -1,
    QEMU_CXBX_CPU_INVALID_STATE = -2,
    QEMU_CXBX_CPU_OUT_OF_MEMORY = -3,
    QEMU_CXBX_CPU_UNSUPPORTED = -4,
    QEMU_CXBX_CPU_GUEST_FAULT = -5,
    QEMU_CXBX_CPU_HOST_ERROR = -6,
};

typedef enum QemuCxbxCpuRunReason {
    QEMU_CXBX_CPU_RUN_STOPPED = 0,
    QEMU_CXBX_CPU_RUN_HALT,
    QEMU_CXBX_CPU_RUN_GUEST_RETURN,
    QEMU_CXBX_CPU_RUN_GUEST_EXCEPTION,
    QEMU_CXBX_CPU_RUN_HOST_REQUEST,
    QEMU_CXBX_CPU_RUN_UNHANDLED_HLE,
} QemuCxbxCpuRunReason;

enum {
    QEMU_CXBX_CPU_MEMORY_READ = 1u << 0,
    QEMU_CXBX_CPU_MEMORY_WRITE = 1u << 1,
    QEMU_CXBX_CPU_MEMORY_EXECUTE = 1u << 2,
    QEMU_CXBX_CPU_MEMORY_HOST_POINTER = 1u << 3,
};

typedef struct QemuCxbxCpuRegisters {
    uint32_t struct_size, version;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, eip, eflags;
    uint16_t cs, ds, es, fs, gs, ss;
    uint32_t cs_base, ds_base, es_base, fs_base, gs_base, ss_base;
} QemuCxbxCpuRegisters;

typedef struct QemuCxbxCpuRunResult {
    uint32_t struct_size, version;
    QemuCxbxCpuRunReason reason;
    uint32_t exception_vector, error_code, fault_address;
} QemuCxbxCpuRunResult;

typedef int (QEMU_CXBX_CALL *QemuCxbxCpuGuestRead)(void *, uint32_t, void *, size_t);
typedef int (QEMU_CXBX_CALL *QemuCxbxCpuGuestWrite)(void *, uint32_t, const void *, size_t);

typedef struct QemuCxbxCpuHleCall {
    uint32_t struct_size, version;
    uint32_t gateway_kind, gateway_id;
    QemuCxbxCpuRegisters *registers;
    void *memory_opaque;
    QemuCxbxCpuGuestRead read_guest;
    QemuCxbxCpuGuestWrite write_guest;
} QemuCxbxCpuHleCall;

typedef int (QEMU_CXBX_CALL *QemuCxbxCpuHleCallback)(void *, QemuCxbxCpuHleCall *);
typedef uint32_t (QEMU_CXBX_CALL *QemuCxbxCpuIoReadCallback)(void *, uint16_t, uint32_t);
typedef void (QEMU_CXBX_CALL *QemuCxbxCpuIoWriteCallback)(void *, uint16_t, uint32_t, uint32_t);
typedef uint32_t (QEMU_CXBX_CALL *QemuCxbxCpuMmioReadCallback)(void *, uint32_t, uint32_t);
typedef void (QEMU_CXBX_CALL *QemuCxbxCpuMmioWriteCallback)(void *, uint32_t, uint32_t, uint32_t);
typedef void (QEMU_CXBX_CALL *QemuCxbxCpuLogCallback)(void *, uint32_t, const char *);

typedef struct QemuCxbxCpuConfig {
    uint32_t struct_size, version;
    void *opaque;
    QemuCxbxCpuHleCallback hle;
    QemuCxbxCpuIoReadCallback io_read;
    QemuCxbxCpuIoWriteCallback io_write;
    QemuCxbxCpuMmioReadCallback mmio_read;
    QemuCxbxCpuMmioWriteCallback mmio_write;
    QemuCxbxCpuLogCallback log;
} QemuCxbxCpuConfig;

QEMU_CXBX_EXPORT uint32_t QEMU_CXBX_CALL qemu_cxbx_cpu_get_api_version(void);
QEMU_CXBX_EXPORT int QEMU_CXBX_CALL qemu_cxbx_cpu_create(const QemuCxbxCpuConfig *, QemuCxbxCpu **);
QEMU_CXBX_EXPORT void QEMU_CXBX_CALL qemu_cxbx_cpu_destroy(QemuCxbxCpu *);
QEMU_CXBX_EXPORT int QEMU_CXBX_CALL qemu_cxbx_cpu_map_memory(QemuCxbxCpu *, uint32_t, uint64_t, void *, uint32_t);
QEMU_CXBX_EXPORT int QEMU_CXBX_CALL qemu_cxbx_cpu_unmap_memory(QemuCxbxCpu *, uint32_t, uint64_t);
QEMU_CXBX_EXPORT int QEMU_CXBX_CALL qemu_cxbx_cpu_set_registers(QemuCxbxCpu *, const QemuCxbxCpuRegisters *);
QEMU_CXBX_EXPORT int QEMU_CXBX_CALL qemu_cxbx_cpu_get_registers(QemuCxbxCpu *, QemuCxbxCpuRegisters *);
QEMU_CXBX_EXPORT int QEMU_CXBX_CALL qemu_cxbx_cpu_run(QemuCxbxCpu *, QemuCxbxCpuRunResult *);
QEMU_CXBX_EXPORT int QEMU_CXBX_CALL qemu_cxbx_cpu_interrupt(QemuCxbxCpu *, uint32_t);
QEMU_CXBX_EXPORT void QEMU_CXBX_CALL qemu_cxbx_cpu_request_stop(QemuCxbxCpu *);
QEMU_CXBX_EXPORT void QEMU_CXBX_CALL qemu_cxbx_cpu_flush(QemuCxbxCpu *, uint32_t, uint64_t);

#endif
