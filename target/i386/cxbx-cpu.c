/* Raw i386 TCG embedding backend used by Cxbx-Reloaded UWP. */
#include "qemu/osdep.h"
#include "qemu/qemu-cxbx-cpu.h"
#include "qapi/error.h"
#define QEMU_HOST_INTERNAL
#include "qemu/qemu-host.h"

#include "accel/tcg/tcg-accel-ops.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "exec/tb-flush.h"
#include "hw/core/cpu.h"
#include "qemu/rcu.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/i386/cpu.h"
#include "target/i386/tcg/helper-tcg.h"

typedef struct CxbxMemoryMapping {
    QTAILQ_ENTRY(CxbxMemoryMapping) link;
    MemoryRegion region;
    uint32_t address;
    uint64_t size;
} CxbxMemoryMapping;

struct QemuCxbxCpu {
    X86CPU *x86;
    QemuCxbxCpuConfig config;
    QemuMutex lock;
    bool stop_requested;
    bool destroyed;
};

static QemuMutex cxbx_global_lock;
static bool cxbx_global_lock_ready;
static bool cxbx_qemu_ready;
static MemoryRegion cxbx_mmio_region;
static MemoryRegion cxbx_io_region;
static QTAILQ_HEAD(, CxbxMemoryMapping) cxbx_mappings =
    QTAILQ_HEAD_INITIALIZER(cxbx_mappings);
static __thread QemuCxbxCpu *cxbx_current;
static __thread bool cxbx_rcu_registered;

static void cxbx_log(QemuCxbxCpu *cpu, uint32_t level, const char *message)
{
    if (cpu && cpu->config.log) {
        cpu->config.log(cpu->config.opaque, level, message);
    }
}

static uint64_t cxbx_mmio_read(void *opaque, hwaddr address, unsigned size)
{
    QemuCxbxCpu *cpu = cxbx_current;
    return cpu && cpu->config.mmio_read ?
        cpu->config.mmio_read(cpu->config.opaque, (uint32_t)address, size) : 0;
}

static void cxbx_mmio_write(void *opaque, hwaddr address, uint64_t value,
                            unsigned size)
{
    QemuCxbxCpu *cpu = cxbx_current;
    if (cpu && cpu->config.mmio_write) {
        cpu->config.mmio_write(cpu->config.opaque, (uint32_t)address,
                               (uint32_t)value, size);
    }
}

static uint64_t cxbx_io_read(void *opaque, hwaddr address, unsigned size)
{
    QemuCxbxCpu *cpu = cxbx_current;
    return cpu && cpu->config.io_read ?
        cpu->config.io_read(cpu->config.opaque, (uint16_t)address, size) : 0;
}

static void cxbx_io_write(void *opaque, hwaddr address, uint64_t value,
                          unsigned size)
{
    QemuCxbxCpu *cpu = cxbx_current;
    if (cpu && cpu->config.io_write) {
        cpu->config.io_write(cpu->config.opaque, (uint16_t)address,
                             (uint32_t)value, size);
    }
}

static const MemoryRegionOps cxbx_mmio_ops = {
    .read = cxbx_mmio_read,
    .write = cxbx_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static const MemoryRegionOps cxbx_io_ops = {
    .read = cxbx_io_read,
    .write = cxbx_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static int cxbx_initialize_qemu(void)
{
    static char arg0[] = "qemu-cxbx-i386";
    static char arg1[] = "-machine";
    static char arg2[] = "none";
    static char arg3[] = "-accel";
    static char arg4[] = "tcg,thread=single";
    static char arg5[] = "-nodefaults";
    static char arg6[] = "-no-user-config";
    static char arg7[] = "-display";
    static char arg8[] = "none";
    static char arg9[] = "-monitor";
    static char arg10[] = "none";
    static char arg11[] = "-serial";
    static char arg12[] = "none";
    static char arg13[] = "-S";
    static char *argv[] = {
        arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8,
        arg9, arg10, arg11, arg12, arg13, NULL
    };

    if (cxbx_qemu_ready) {
        return QEMU_CXBX_CPU_OK;
    }
    if (qemu_host_init(ARRAY_SIZE(argv) - 1, argv) != 0) {
        return QEMU_CXBX_CPU_HOST_ERROR;
    }
    bql_lock();
    memory_region_init_io(&cxbx_mmio_region, NULL, &cxbx_mmio_ops, NULL,
                          "cxbx-mmio", UINT64_C(1) << 32);
    memory_region_add_subregion_overlap(get_system_memory(), 0,
                                        &cxbx_mmio_region, -1000);
    memory_region_init_io(&cxbx_io_region, NULL, &cxbx_io_ops, NULL,
                          "cxbx-io", UINT64_C(1) << 16);
    memory_region_add_subregion_overlap(get_system_io(), 0,
                                        &cxbx_io_region, -1000);
    bql_unlock();
    cxbx_qemu_ready = true;
    return QEMU_CXBX_CPU_OK;
}

static void cxbx_copy_to_env(CPUX86State *env,
                             const QemuCxbxCpuRegisters *r)
{
    const uint32_t code_flags = DESC_P_MASK | DESC_S_MASK | DESC_CS_MASK |
        DESC_R_MASK | DESC_B_MASK | DESC_G_MASK;
    const uint32_t data_flags = DESC_P_MASK | DESC_S_MASK | DESC_W_MASK |
        DESC_B_MASK | DESC_G_MASK;

    env->regs[R_EAX] = r->eax; env->regs[R_ECX] = r->ecx;
    env->regs[R_EDX] = r->edx; env->regs[R_EBX] = r->ebx;
    env->regs[R_ESP] = r->esp; env->regs[R_EBP] = r->ebp;
    env->regs[R_ESI] = r->esi; env->regs[R_EDI] = r->edi;
    env->eip = r->eip;
    cpu_x86_update_cr0(env, env->cr[0] | CR0_PE_MASK);
    cpu_x86_load_seg_cache(env, R_CS, r->cs, r->cs_base, UINT32_MAX, code_flags);
    cpu_x86_load_seg_cache(env, R_DS, r->ds, r->ds_base, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_ES, r->es, r->es_base, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_FS, r->fs, r->fs_base, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_GS, r->gs, r->gs_base, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_SS, r->ss, r->ss_base, UINT32_MAX, data_flags);
    cpu_load_eflags(env, r->eflags, UINT32_MAX);
}

static void cxbx_copy_from_env(CPUX86State *env, QemuCxbxCpuRegisters *r)
{
    r->eax = env->regs[R_EAX]; r->ecx = env->regs[R_ECX];
    r->edx = env->regs[R_EDX]; r->ebx = env->regs[R_EBX];
    r->esp = env->regs[R_ESP]; r->ebp = env->regs[R_EBP];
    r->esi = env->regs[R_ESI]; r->edi = env->regs[R_EDI];
    r->eip = env->eip;
    r->eflags = cpu_compute_eflags(env);
    r->cs = env->segs[R_CS].selector; r->cs_base = env->segs[R_CS].base;
    r->ds = env->segs[R_DS].selector; r->ds_base = env->segs[R_DS].base;
    r->es = env->segs[R_ES].selector; r->es_base = env->segs[R_ES].base;
    r->fs = env->segs[R_FS].selector; r->fs_base = env->segs[R_FS].base;
    r->gs = env->segs[R_GS].selector; r->gs_base = env->segs[R_GS].base;
    r->ss = env->segs[R_SS].selector; r->ss_base = env->segs[R_SS].base;
}

static int cxbx_guest_read(void *opaque, uint32_t address, void *buffer,
                           size_t size)
{
    QemuCxbxCpu *cpu = opaque;
    return cpu_memory_rw_debug(CPU(cpu->x86), address, buffer, size, false) == 0;
}

static int cxbx_guest_write(void *opaque, uint32_t address,
                            const void *buffer, size_t size)
{
    QemuCxbxCpu *cpu = opaque;
    return cpu_memory_rw_debug(CPU(cpu->x86), address, (void *)buffer,
                               size, true) == 0;
}

static bool cxbx_decode_gateway(uint32_t eip, uint32_t *kind, uint32_t *id)
{
    if (eip >= QEMU_CXBX_KERNEL_GATEWAY_BASE &&
        eip < QEMU_CXBX_KERNEL_GATEWAY_BASE +
              QEMU_CXBX_KERNEL_ORDINAL_COUNT * QEMU_CXBX_GATEWAY_STRIDE) {
        *kind = 1; *id = (eip - QEMU_CXBX_KERNEL_GATEWAY_BASE) /
            QEMU_CXBX_GATEWAY_STRIDE; return true;
    }
    if (eip >= QEMU_CXBX_XDK_GATEWAY_BASE &&
        eip < QEMU_CXBX_XDK_GATEWAY_BASE + UINT32_C(0x10000)) {
        *kind = 2; *id = (eip - QEMU_CXBX_XDK_GATEWAY_BASE) /
            QEMU_CXBX_GATEWAY_STRIDE; return true;
    }
    if (eip == QEMU_CXBX_THREAD_RETURN_GATEWAY) {
        *kind = 3; *id = 0; return true;
    }
    return false;
}

uint32_t QEMU_CXBX_CALL qemu_cxbx_cpu_get_api_version(void)
{
    return QEMU_CXBX_CPU_ABI_VERSION;
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_create(const QemuCxbxCpuConfig *config,
                                         QemuCxbxCpu **cpu_out)
{
    QemuCxbxCpu *cpu;
    Error *err = NULL;
    int ret;

    if (!config || !cpu_out || config->struct_size < sizeof(*config) ||
        config->version != 1) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }
    if (!cxbx_global_lock_ready) {
        qemu_mutex_init(&cxbx_global_lock);
        cxbx_global_lock_ready = true;
    }
    qemu_mutex_lock(&cxbx_global_lock);
    ret = cxbx_initialize_qemu();
    qemu_mutex_unlock(&cxbx_global_lock);
    if (ret != QEMU_CXBX_CPU_OK) {
        return ret;
    }

    cpu = g_new0(QemuCxbxCpu, 1);
    cpu->config = *config;
    qemu_mutex_init(&cpu->lock);
    bql_lock();
    cpu->x86 = X86_CPU(object_new(X86_CPU_TYPE_NAME("pentium3")));
    object_property_set_uint(OBJECT(cpu->x86), "apic-id", 0, &err);
    if (!err && !qdev_realize(DEVICE(cpu->x86), NULL, &err)) {
        /* qdev_realize supplied the error. */
    }
    bql_unlock();
    if (err) {
        cxbx_log(cpu, 3, error_get_pretty(err));
        error_free(err);
        object_unref(OBJECT(cpu->x86));
        qemu_mutex_destroy(&cpu->lock);
        g_free(cpu);
        return QEMU_CXBX_CPU_HOST_ERROR;
    }
    for (uint32_t ordinal = 0; ordinal < QEMU_CXBX_KERNEL_ORDINAL_COUNT;
         ++ordinal) {
        cpu_breakpoint_insert(CPU(cpu->x86),
            QEMU_CXBX_KERNEL_GATEWAY_BASE + ordinal * QEMU_CXBX_GATEWAY_STRIDE,
            BP_GDB, NULL);
    }
    for (uint32_t id = 0; id < UINT32_C(0x10000) / QEMU_CXBX_GATEWAY_STRIDE;
         ++id) {
        cpu_breakpoint_insert(CPU(cpu->x86),
            QEMU_CXBX_XDK_GATEWAY_BASE + id * QEMU_CXBX_GATEWAY_STRIDE,
            BP_GDB, NULL);
    }
    cpu_breakpoint_insert(CPU(cpu->x86), QEMU_CXBX_THREAD_RETURN_GATEWAY,
                          BP_GDB, NULL);
    *cpu_out = cpu;
    cxbx_log(cpu, 1, "QEMU TCG i386 CPU backend initialized");
    return QEMU_CXBX_CPU_OK;
}

void QEMU_CXBX_CALL qemu_cxbx_cpu_destroy(QemuCxbxCpu *cpu)
{
    if (!cpu) {
        return;
    }
    qemu_mutex_lock(&cpu->lock);
    cpu->destroyed = true;
    cpu_exit(CPU(cpu->x86));
    qemu_mutex_unlock(&cpu->lock);
    bql_lock();
    object_unref(OBJECT(cpu->x86));
    bql_unlock();
    qemu_mutex_destroy(&cpu->lock);
    g_free(cpu);
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_map_memory(QemuCxbxCpu *cpu,
                                             uint32_t address, uint64_t size,
                                             void *host, uint32_t flags)
{
    CxbxMemoryMapping *mapping;
    if (!cpu || !size || !host || address + size > (UINT64_C(1) << 32) ||
        !(flags & QEMU_CXBX_CPU_MEMORY_HOST_POINTER)) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }
    mapping = g_new0(CxbxMemoryMapping, 1);
    mapping->address = address;
    mapping->size = size;
    memory_region_init_ram_ptr(&mapping->region, NULL, "cxbx-host-memory",
                               size, host);
    if (!(flags & QEMU_CXBX_CPU_MEMORY_WRITE)) {
        memory_region_set_readonly(&mapping->region, true);
    }
    qemu_mutex_lock(&cxbx_global_lock);
    bql_lock();
    memory_region_add_subregion_overlap(get_system_memory(), address,
                                        &mapping->region, 1000);
    bql_unlock();
    QTAILQ_INSERT_TAIL(&cxbx_mappings, mapping, link);
    qemu_mutex_unlock(&cxbx_global_lock);
    return QEMU_CXBX_CPU_OK;
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_unmap_memory(QemuCxbxCpu *cpu,
                                               uint32_t address, uint64_t size)
{
    CxbxMemoryMapping *mapping;
    if (!cpu || !size) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }
    qemu_mutex_lock(&cxbx_global_lock);
    QTAILQ_FOREACH(mapping, &cxbx_mappings, link) {
        if (mapping->address == address && mapping->size == size) {
            bql_lock();
            memory_region_del_subregion(get_system_memory(), &mapping->region);
            bql_unlock();
            QTAILQ_REMOVE(&cxbx_mappings, mapping, link);
            object_unparent(OBJECT(&mapping->region));
            g_free(mapping);
            qemu_mutex_unlock(&cxbx_global_lock);
            return QEMU_CXBX_CPU_OK;
        }
    }
    qemu_mutex_unlock(&cxbx_global_lock);
    return QEMU_CXBX_CPU_INVALID_ARGUMENT;
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_set_registers(QemuCxbxCpu *cpu,
                                                const QemuCxbxCpuRegisters *r)
{
    if (!cpu || !r || r->struct_size < sizeof(*r) || r->version != 1) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }
    qemu_mutex_lock(&cpu->lock);
    cxbx_copy_to_env(&cpu->x86->env, r);
    qemu_mutex_unlock(&cpu->lock);
    return QEMU_CXBX_CPU_OK;
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_get_registers(QemuCxbxCpu *cpu,
                                                QemuCxbxCpuRegisters *r)
{
    if (!cpu || !r || r->struct_size < sizeof(*r) || r->version != 1) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }
    qemu_mutex_lock(&cpu->lock);
    cxbx_copy_from_env(&cpu->x86->env, r);
    qemu_mutex_unlock(&cpu->lock);
    return QEMU_CXBX_CPU_OK;
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_run(QemuCxbxCpu *cpu,
                                      QemuCxbxCpuRunResult *result)
{
    CPUState *cs;
    CPUX86State *env;
    int exec_result;
    uint32_t kind, id;

    if (!cpu || !result || result->struct_size < sizeof(*result) ||
        result->version != 1) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }
    qemu_mutex_lock(&cpu->lock);
    if (cpu->destroyed) {
        qemu_mutex_unlock(&cpu->lock);
        return QEMU_CXBX_CPU_INVALID_STATE;
    }
    memset((char *)result + offsetof(QemuCxbxCpuRunResult, reason), 0,
           sizeof(*result) - offsetof(QemuCxbxCpuRunResult, reason));
    if (cpu->stop_requested) {
        result->reason = QEMU_CXBX_CPU_RUN_HOST_REQUEST;
        qemu_mutex_unlock(&cpu->lock);
        return QEMU_CXBX_CPU_OK;
    }
    cs = CPU(cpu->x86);
    env = &cpu->x86->env;
    if (cxbx_decode_gateway((uint32_t)env->eip, &kind, &id)) {
        QemuCxbxCpuRegisters regs = { sizeof(regs), 1 };
        QemuCxbxCpuHleCall call = {
            sizeof(call), 1, kind, id, &regs, cpu,
            cxbx_guest_read, cxbx_guest_write
        };
        int hle_result;
        if (kind == 3) {
            result->reason = QEMU_CXBX_CPU_RUN_GUEST_RETURN;
            qemu_mutex_unlock(&cpu->lock);
            return QEMU_CXBX_CPU_OK;
        }
        cxbx_copy_from_env(env, &regs);
        hle_result = cpu->config.hle ?
            cpu->config.hle(cpu->config.opaque, &call) : QEMU_CXBX_CPU_UNSUPPORTED;
        if (hle_result != QEMU_CXBX_CPU_OK) {
            result->reason = QEMU_CXBX_CPU_RUN_UNHANDLED_HLE;
            qemu_mutex_unlock(&cpu->lock);
            return QEMU_CXBX_CPU_OK;
        }
        cxbx_copy_to_env(env, &regs);
    }

    if (!cxbx_rcu_registered) {
        rcu_register_thread();
        cxbx_rcu_registered = true;
    }
    cxbx_current = cpu;
    current_cpu = cs;
    cs->halted = false;
    cpu_reset_interrupt(cs, CPU_INTERRUPT_EXITTB);
    qatomic_store_release(&cs->exit_request, false);
    exec_result = tcg_cpu_exec(cs);
    current_cpu = NULL;
    cxbx_current = NULL;

    if (cpu->stop_requested) {
        result->reason = QEMU_CXBX_CPU_RUN_HOST_REQUEST;
    } else if (exec_result == EXCP_INTERRUPT) {
        result->reason = QEMU_CXBX_CPU_RUN_STOPPED;
    } else if (exec_result == EXCP_HLT) {
        result->reason = QEMU_CXBX_CPU_RUN_HALT;
    } else if (exec_result == EXCP_DEBUG) {
        result->reason = QEMU_CXBX_CPU_RUN_STOPPED;
    } else {
        result->reason = QEMU_CXBX_CPU_RUN_GUEST_EXCEPTION;
        result->exception_vector = cs->exception_index;
        result->fault_address = env->cr[2];
    }
    qemu_mutex_unlock(&cpu->lock);
    return QEMU_CXBX_CPU_OK;
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_interrupt(QemuCxbxCpu *cpu, uint32_t vector)
{
    if (!cpu || vector > UINT8_MAX) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }
    CPU(cpu->x86)->exception_index = vector;
    cpu_interrupt(CPU(cpu->x86), CPU_INTERRUPT_HARD);
    return QEMU_CXBX_CPU_OK;
}

void QEMU_CXBX_CALL qemu_cxbx_cpu_request_stop(QemuCxbxCpu *cpu)
{
    if (cpu) {
        cpu->stop_requested = true;
        cpu_exit(CPU(cpu->x86));
    }
}

void QEMU_CXBX_CALL qemu_cxbx_cpu_flush(QemuCxbxCpu *cpu,
                                         uint32_t address, uint64_t size)
{
    if (cpu) {
        tb_flush__exclusive_or_serial();
    }
}
