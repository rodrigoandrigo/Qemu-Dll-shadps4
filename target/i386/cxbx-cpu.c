/* Raw i386 TCG embedding backend used by Cxbx-Reloaded UWP. */
#include "qemu/osdep.h"
#include "qemu/qemu-cxbx-cpu.h"
#include "qapi/error.h"
#define QEMU_HOST_INTERNAL
#include "qemu/qemu-host.h"

#include "accel/tcg/tcg-accel-ops.h"
#include "accel/tcg/tcg-accel-ops-icount.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "exec/icount.h"
#include "exec/tb-flush.h"
#include "hw/core/cpu.h"
#include "qemu/rcu.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "system/system.h"
#include "target/i386/cpu.h"
#include "target/i386/tcg/helper-tcg.h"

typedef struct CxbxMemoryMapping {
    QTAILQ_ENTRY(CxbxMemoryMapping) link;
    MemoryRegion region;
    uint32_t address;
    uint64_t size;
    void *host;
    uint32_t flags;
} CxbxMemoryMapping;

/* Architectural state which is not part of the deliberately small public
 * embedding ABI.  Every Cxbx Xbox thread owns one of these snapshots while
 * all of them time-share QEMU's single physical i386 TCG CPU.  Do not put
 * pointer-bearing CPU fields (TLBs, breakpoints, devices) in this structure. */
typedef struct CxbxCpuThreadState {
    target_ulong cc_dst;
    target_ulong cc_src;
    target_ulong cc_src2;
    uint32_t cc_op;
    int32_t df;
    uint32_t hflags;
    uint32_t hflags2;
    SegmentCache segs[6];
    SegmentCache ldt;
    SegmentCache tr;
    SegmentCache gdt;
    SegmentCache idt;
    unsigned int fpstt;
    uint16_t fpus;
    uint16_t fpuc;
    uint8_t fptags[8];
    FPReg fpregs[8];
    uint16_t fpop;
    uint16_t fpcs;
    uint16_t fpds;
    uint64_t fpip;
    uint64_t fpdp;
    float_status fp_status;
    floatx80 ft0;
    float_status mmx_status;
    float_status sse_status;
    uint32_t mxcsr;
    ZMMReg xmm_regs[CPU_NB_EREGS] QEMU_ALIGNED(16);
    ZMMReg xmm_t0 QEMU_ALIGNED(16);
    MMXReg mmx_t0;
    target_ulong dr[8];
    int error_code;
    int exception_is_int;
    target_ulong exception_next_eip;
    int old_exception;
    int exception_index;
} CxbxCpuThreadState;

struct QemuCxbxCpu {
    X86CPU *x86;
    QemuCxbxCpuConfig config;
    QemuCxbxCpuRegisters registers;
    QemuCxbxCpuRegisters snapshot_registers;
    CxbxCpuThreadState thread_state;
    QemuMutex lock;
    int pending_interrupts;
    bool stop_requested;
    bool destroyed;
    bool registers_valid;
    bool thread_state_valid;
};

typedef struct CxbxRunRequest {
    QemuCxbxCpu *cpu;
    QemuCxbxCpuRunResult *result;
    int status;
} CxbxRunRequest;

static int cxbx_cpu_execute_slice(QemuCxbxCpu *cpu,
                                  QemuCxbxCpuRunResult *result);

static QemuMutex cxbx_global_lock;
static bool cxbx_global_lock_ready;
static bool cxbx_qemu_ready;
static X86CPU *cxbx_shared_x86;
static uint32_t cxbx_shared_x86_users;
extern bool tcg_cxbx_intercept_guest_exceptions;
static bool cxbx_gateway_stubs_ready;
static MemoryRegion cxbx_gateway_region;
static uint8_t *cxbx_gateway_stubs;
static MemoryRegion cxbx_mmio_region;
static MemoryRegion cxbx_io_region;
static QTAILQ_HEAD(, CxbxMemoryMapping) cxbx_mappings =
    QTAILQ_HEAD_INITIALIZER(cxbx_mappings);
static __thread QemuCxbxCpu *cxbx_current;

/* cxbx_global_lock and the BQL must be held. */
static CxbxMemoryMapping *cxbx_add_mapping_locked(uint32_t address,
                                                  uint64_t size, void *host,
                                                  uint32_t flags)
{
    CxbxMemoryMapping *mapping = g_new0(CxbxMemoryMapping, 1);
    mapping->address = address;
    mapping->size = size;
    mapping->host = host;
    mapping->flags = flags;
    memory_region_init_ram_ptr(&mapping->region, NULL, "cxbx-host-memory",
                               size, host);
    memory_region_set_readonly(&mapping->region,
                               !(flags & QEMU_CXBX_CPU_MEMORY_WRITE));
    memory_region_add_subregion_overlap(get_system_memory(), address,
                                        &mapping->region, 1000);
    QTAILQ_INSERT_TAIL(&cxbx_mappings, mapping, link);
    return mapping;
}

/* cxbx_global_lock and the BQL must be held. */
static void cxbx_remove_mapping_locked(CxbxMemoryMapping *mapping)
{
    memory_region_del_subregion(get_system_memory(), &mapping->region);
    QTAILQ_REMOVE(&cxbx_mappings, mapping, link);
    object_unparent(OBJECT(&mapping->region));
    g_free(mapping);
}

/* Deterministic instruction budget for one logical Xbox thread turn. */
#define CXBX_TCG_INSN_BUDGET INT64_C(32768)
#define CXBX_GATEWAY_REGION_SIZE UINT32_C(0x40000)

static void cxbx_write_gateway_trap(uint32_t address)
{
    uint32_t offset = address - QEMU_CXBX_KERNEL_GATEWAY_BASE;
    cxbx_gateway_stubs[offset] = 0x0f;
    cxbx_gateway_stubs[offset + 1] = 0x0b; /* UD2 */
}

static void cxbx_initialize_gateway_stubs(void)
{
    uint32_t id;

    if (cxbx_gateway_stubs_ready) {
        return;
    }
    cxbx_gateway_stubs = g_malloc(CXBX_GATEWAY_REGION_SIZE);
    /* INT3 padding keeps accidental jumps between 16-byte entries visible as
     * genuine guest faults instead of aliasing them to a nearby gateway. */
    memset(cxbx_gateway_stubs, 0xcc, CXBX_GATEWAY_REGION_SIZE);
    for (id = 0; id < QEMU_CXBX_KERNEL_ORDINAL_COUNT; ++id) {
        cxbx_write_gateway_trap(QEMU_CXBX_KERNEL_GATEWAY_BASE +
                                id * QEMU_CXBX_GATEWAY_STRIDE);
    }
    for (id = 0; id < UINT32_C(0x10000) / QEMU_CXBX_GATEWAY_STRIDE; ++id) {
        cxbx_write_gateway_trap(QEMU_CXBX_XDK_GATEWAY_BASE +
                                id * QEMU_CXBX_GATEWAY_STRIDE);
    }
    cxbx_write_gateway_trap(QEMU_CXBX_THREAD_RETURN_GATEWAY);
    memory_region_init_ram_ptr(&cxbx_gateway_region, NULL,
                               "cxbx-gateway-stubs",
                               CXBX_GATEWAY_REGION_SIZE,
                               cxbx_gateway_stubs);
    memory_region_set_readonly(&cxbx_gateway_region, true);
    memory_region_add_subregion_overlap(get_system_memory(),
        QEMU_CXBX_KERNEL_GATEWAY_BASE, &cxbx_gateway_region, 3000);
    cxbx_gateway_stubs_ready = true;
}

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
    /* The original Xbox has one physical CPU.  Logical Cxbx threads save and
     * restore their register sets around this single TCG CPU. */
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
    static char arg14[] = "-icount";
    static char arg15[] = "shift=0,sleep=off";
    static char *argv[] = {
        arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8,
        arg9, arg10, arg11, arg12, arg13, arg14, arg15, NULL
    };

    if (cxbx_qemu_ready) {
        return QEMU_CXBX_CPU_OK;
    }
    if (qemu_host_init(ARRAY_SIZE(argv) - 1, argv) != 0) {
        return QEMU_CXBX_CPU_HOST_ERROR;
    }
    tcg_cxbx_intercept_guest_exceptions = true;
    bql_lock();
    memory_region_init_io(&cxbx_mmio_region, NULL, &cxbx_mmio_ops, NULL,
                          "cxbx-mmio", UINT64_C(1) << 32);
    memory_region_add_subregion_overlap(get_system_memory(), 0,
                                        &cxbx_mmio_region, -1000);
    memory_region_init_io(&cxbx_io_region, NULL, &cxbx_io_ops, NULL,
                          "cxbx-io", UINT64_C(1) << 16);
    memory_region_add_subregion_overlap(get_system_io(), 0,
                                        &cxbx_io_region, -1000);
    cxbx_initialize_gateway_stubs();
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

static void cxbx_save_thread_state(CPUState *cs, CPUX86State *env,
                                   CxbxCpuThreadState *s)
{
    s->cc_dst = env->cc_dst; s->cc_src = env->cc_src;
    s->cc_src2 = env->cc_src2; s->cc_op = env->cc_op; s->df = env->df;
    s->hflags = env->hflags; s->hflags2 = env->hflags2;
    memcpy(s->segs, env->segs, sizeof(s->segs));
    s->ldt = env->ldt; s->tr = env->tr; s->gdt = env->gdt; s->idt = env->idt;
    s->fpstt = env->fpstt; s->fpus = env->fpus; s->fpuc = env->fpuc;
    memcpy(s->fptags, env->fptags, sizeof(s->fptags));
    memcpy(s->fpregs, env->fpregs, sizeof(s->fpregs));
    s->fpop = env->fpop; s->fpcs = env->fpcs; s->fpds = env->fpds;
    s->fpip = env->fpip; s->fpdp = env->fpdp;
    s->fp_status = env->fp_status; s->ft0 = env->ft0;
    s->mmx_status = env->mmx_status; s->sse_status = env->sse_status;
    s->mxcsr = env->mxcsr;
    memcpy(s->xmm_regs, env->xmm_regs, sizeof(s->xmm_regs));
    s->xmm_t0 = env->xmm_t0; s->mmx_t0 = env->mmx_t0;
    memcpy(s->dr, env->dr, sizeof(s->dr));
    s->error_code = env->error_code;
    s->exception_is_int = env->exception_is_int;
    s->exception_next_eip = env->exception_next_eip;
    s->old_exception = env->old_exception;
    s->exception_index = cs->exception_index;
}

static void cxbx_restore_thread_state(CPUState *cs, CPUX86State *env,
                                      const CxbxCpuThreadState *s)
{
    env->cc_dst = s->cc_dst; env->cc_src = s->cc_src;
    env->cc_src2 = s->cc_src2; env->cc_op = s->cc_op; env->df = s->df;
    env->hflags = s->hflags; env->hflags2 = s->hflags2;
    memcpy(env->segs, s->segs, sizeof(s->segs));
    env->ldt = s->ldt; env->tr = s->tr; env->gdt = s->gdt; env->idt = s->idt;
    env->fpstt = s->fpstt; env->fpus = s->fpus; env->fpuc = s->fpuc;
    memcpy(env->fptags, s->fptags, sizeof(s->fptags));
    memcpy(env->fpregs, s->fpregs, sizeof(s->fpregs));
    env->fpop = s->fpop; env->fpcs = s->fpcs; env->fpds = s->fpds;
    env->fpip = s->fpip; env->fpdp = s->fpdp;
    env->fp_status = s->fp_status; env->ft0 = s->ft0;
    env->mmx_status = s->mmx_status; env->sse_status = s->sse_status;
    env->mxcsr = s->mxcsr;
    memcpy(env->xmm_regs, s->xmm_regs, sizeof(s->xmm_regs));
    env->xmm_t0 = s->xmm_t0; env->mmx_t0 = s->mmx_t0;
    memcpy(env->dr, s->dr, sizeof(s->dr));
    env->error_code = s->error_code;
    env->exception_is_int = s->exception_is_int;
    env->exception_next_eip = s->exception_next_eip;
    env->old_exception = s->old_exception;
    cs->exception_index = s->exception_index;
}

/* HLE can edit the compact public register block between slices.  Preserve
 * descriptor limits/flags from the private snapshot while applying those
 * selector/base changes. */
static void cxbx_overlay_public_registers(CPUX86State *env,
                                         const QemuCxbxCpuRegisters *r,
                                         const QemuCxbxCpuRegisters *saved)
{
    env->regs[R_EAX] = r->eax; env->regs[R_ECX] = r->ecx;
    env->regs[R_EDX] = r->edx; env->regs[R_EBX] = r->ebx;
    env->regs[R_ESP] = r->esp; env->regs[R_EBP] = r->ebp;
    env->regs[R_ESI] = r->esi; env->regs[R_EDI] = r->edi;
    env->eip = r->eip;
    if (r->cs != saved->cs || r->cs_base != saved->cs_base) {
        env->segs[R_CS].selector = r->cs; env->segs[R_CS].base = r->cs_base;
    }
    if (r->ds != saved->ds || r->ds_base != saved->ds_base) {
        env->segs[R_DS].selector = r->ds; env->segs[R_DS].base = r->ds_base;
    }
    if (r->es != saved->es || r->es_base != saved->es_base) {
        env->segs[R_ES].selector = r->es; env->segs[R_ES].base = r->es_base;
    }
    if (r->fs != saved->fs || r->fs_base != saved->fs_base) {
        env->segs[R_FS].selector = r->fs; env->segs[R_FS].base = r->fs_base;
    }
    if (r->gs != saved->gs || r->gs_base != saved->gs_base) {
        env->segs[R_GS].selector = r->gs; env->segs[R_GS].base = r->gs_base;
    }
    if (r->ss != saved->ss || r->ss_base != saved->ss_base) {
        env->segs[R_SS].selector = r->ss; env->segs[R_SS].base = r->ss_base;
    }
    if (r->eflags != saved->eflags) {
        cpu_load_eflags(env, r->eflags, UINT32_MAX);
    }
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
              QEMU_CXBX_KERNEL_ORDINAL_COUNT * QEMU_CXBX_GATEWAY_STRIDE &&
        (eip - QEMU_CXBX_KERNEL_GATEWAY_BASE) % QEMU_CXBX_GATEWAY_STRIDE == 0) {
        *kind = 1; *id = (eip - QEMU_CXBX_KERNEL_GATEWAY_BASE) /
            QEMU_CXBX_GATEWAY_STRIDE; return true;
    }
    if (eip >= QEMU_CXBX_XDK_GATEWAY_BASE &&
        eip < QEMU_CXBX_XDK_GATEWAY_BASE + UINT32_C(0x10000) &&
        (eip - QEMU_CXBX_XDK_GATEWAY_BASE) % QEMU_CXBX_GATEWAY_STRIDE == 0) {
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
    qemu_mutex_lock(&cxbx_global_lock);
    if (!cxbx_shared_x86) {
        bql_lock();
        cxbx_shared_x86 = X86_CPU(object_new(X86_CPU_TYPE_NAME("pentium3")));
        object_property_set_uint(OBJECT(cxbx_shared_x86), "apic-id", 0, &err);
        if (!err && !qdev_realize(DEVICE(cxbx_shared_x86), NULL, &err)) {
            /* qdev_realize supplied the error. */
        }
        bql_unlock();
    }
    if (err) {
        cxbx_log(cpu, 3, error_get_pretty(err));
        error_free(err);
        object_unref(OBJECT(cxbx_shared_x86));
        cxbx_shared_x86 = NULL;
        qemu_mutex_unlock(&cxbx_global_lock);
        qemu_mutex_destroy(&cpu->lock);
        g_free(cpu);
        return QEMU_CXBX_CPU_HOST_ERROR;
    }
    cpu->x86 = cxbx_shared_x86;
    cxbx_shared_x86_users++;
    qemu_mutex_unlock(&cxbx_global_lock);
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
    qemu_mutex_lock(&cxbx_global_lock);
    if (cxbx_shared_x86_users) {
        --cxbx_shared_x86_users;
    }
    /* Keep the physical CPU alive for the process lifetime.  QEMU's global
     * address space and translated blocks are likewise process-wide, and a
     * later embedded thread may reuse them. */
    qemu_mutex_unlock(&cxbx_global_lock);
    qemu_mutex_destroy(&cpu->lock);
    g_free(cpu);
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_map_memory(QemuCxbxCpu *cpu,
                                             uint32_t address, uint64_t size,
                                             void *host, uint32_t flags)
{
    if (!cpu || !size || !host || address + size > (UINT64_C(1) << 32) ||
        !(flags & QEMU_CXBX_CPU_MEMORY_HOST_POINTER)) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }
    qemu_mutex_lock(&cxbx_global_lock);
    bql_lock();
    cxbx_add_mapping_locked(address, size, host, flags);
    bql_unlock();
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
            cxbx_remove_mapping_locked(mapping);
            bql_unlock();
            qemu_mutex_unlock(&cxbx_global_lock);
            return QEMU_CXBX_CPU_OK;
        }
    }
    qemu_mutex_unlock(&cxbx_global_lock);
    return QEMU_CXBX_CPU_INVALID_ARGUMENT;
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_guest_commit(QemuCxbxCpu *cpu,
                                               uint32_t address, uint64_t size,
                                               void *host, uint32_t flags)
{
    CxbxMemoryMapping *mapping;
    uint64_t end = (uint64_t)address + size;

    if (!cpu || !size || end > (UINT64_C(1) << 32) || !host) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }
    qemu_mutex_lock(&cxbx_global_lock);
    QTAILQ_FOREACH(mapping, &cxbx_mappings, link) {
        if (address >= mapping->address &&
            end <= (uint64_t)mapping->address + mapping->size &&
            (uint8_t *)host == (uint8_t *)mapping->host +
                               (address - mapping->address)) {
            qemu_mutex_unlock(&cxbx_global_lock);
            return QEMU_CXBX_CPU_OK;
        }
    }
    qemu_mutex_unlock(&cxbx_global_lock);
    return qemu_cxbx_cpu_map_memory(cpu, address, size, host, flags);
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_guest_protect(QemuCxbxCpu *cpu,
                                                uint32_t address, uint64_t size,
                                                uint32_t flags)
{
    CxbxMemoryMapping *mapping;

    if (!cpu || !size || address + size > (UINT64_C(1) << 32) ||
        !(flags & QEMU_CXBX_CPU_MEMORY_READ)) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }
    qemu_mutex_lock(&cxbx_global_lock);
    QTAILQ_FOREACH(mapping, &cxbx_mappings, link) {
        uint64_t mapping_end = (uint64_t)mapping->address + mapping->size;
        uint64_t end = (uint64_t)address + size;
        if (address >= mapping->address && end <= mapping_end) {
            uint32_t old_address = mapping->address;
            uint64_t old_size = mapping->size;
            uint8_t *old_host = mapping->host;
            uint32_t old_flags = mapping->flags;
            bql_lock();
            cxbx_remove_mapping_locked(mapping);
            if (address > old_address) {
                cxbx_add_mapping_locked(old_address, address - old_address,
                                        old_host, old_flags);
            }
            cxbx_add_mapping_locked(address, size,
                                    old_host + (address - old_address), flags);
            if (end < (uint64_t)old_address + old_size) {
                cxbx_add_mapping_locked((uint32_t)end,
                    (uint64_t)old_address + old_size - end,
                    old_host + (end - old_address), old_flags);
            }
            bql_unlock();
            qemu_mutex_unlock(&cxbx_global_lock);
            tb_flush__exclusive_or_serial();
            return QEMU_CXBX_CPU_OK;
        }
    }
    qemu_mutex_unlock(&cxbx_global_lock);
    return QEMU_CXBX_CPU_INVALID_ARGUMENT;
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_guest_decommit(QemuCxbxCpu *cpu,
                                                 uint32_t address,
                                                 uint64_t size)
{
    CxbxMemoryMapping *mapping;
    uint64_t end = (uint64_t)address + size;

    if (!cpu || !size || end > (UINT64_C(1) << 32)) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }
    qemu_mutex_lock(&cxbx_global_lock);
    QTAILQ_FOREACH(mapping, &cxbx_mappings, link) {
        uint64_t mapping_end = (uint64_t)mapping->address + mapping->size;
        if (address >= mapping->address && end <= mapping_end) {
            uint32_t old_address = mapping->address;
            uint64_t old_size = mapping->size;
            uint8_t *old_host = mapping->host;
            uint32_t old_flags = mapping->flags;
            bql_lock();
            cxbx_remove_mapping_locked(mapping);
            if (address > old_address) {
                cxbx_add_mapping_locked(old_address, address - old_address,
                                        old_host, old_flags);
            }
            if (end < (uint64_t)old_address + old_size) {
                cxbx_add_mapping_locked((uint32_t)end,
                    (uint64_t)old_address + old_size - end,
                    old_host + (end - old_address), old_flags);
            }
            bql_unlock();
            qemu_mutex_unlock(&cxbx_global_lock);
            tb_flush__exclusive_or_serial();
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
    cpu->registers = *r;
    cpu->registers_valid = true;
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
    if (!cpu->registers_valid) {
        qemu_mutex_unlock(&cpu->lock);
        return QEMU_CXBX_CPU_INVALID_STATE;
    }
    *r = cpu->registers;
    qemu_mutex_unlock(&cpu->lock);
    return QEMU_CXBX_CPU_OK;
}

static int cxbx_cpu_dispatch_gateway(QemuCxbxCpu *cpu,
                                     QemuCxbxCpuRunResult *result,
                                     bool *dispatched)
{
    uint32_t kind, id;
    int hle_result;

    *dispatched = false;
    qemu_mutex_lock(&cpu->lock);
    if (cpu->destroyed || !cpu->registers_valid) {
        qemu_mutex_unlock(&cpu->lock);
        return QEMU_CXBX_CPU_INVALID_STATE;
    }
    memset((char *)result + offsetof(QemuCxbxCpuRunResult, reason), 0,
           sizeof(*result) - offsetof(QemuCxbxCpuRunResult, reason));
    if (cpu->stop_requested) {
        result->reason = QEMU_CXBX_CPU_RUN_HOST_REQUEST;
        *dispatched = true;
        qemu_mutex_unlock(&cpu->lock);
        return QEMU_CXBX_CPU_OK;
    }
    if (!cxbx_decode_gateway(cpu->registers.eip, &kind, &id)) {
        qemu_mutex_unlock(&cpu->lock);
        return QEMU_CXBX_CPU_OK;
    }

    *dispatched = true;
    if (kind == 3) {
        result->reason = QEMU_CXBX_CPU_RUN_GUEST_RETURN;
        qemu_mutex_unlock(&cpu->lock);
        return QEMU_CXBX_CPU_OK;
    }
    QemuCxbxCpuHleCall call = {
        sizeof(call), 1, kind, id, &cpu->registers, cpu,
        cxbx_guest_read, cxbx_guest_write
    };
    hle_result = cpu->config.hle ?
        cpu->config.hle(cpu->config.opaque, &call) : QEMU_CXBX_CPU_UNSUPPORTED;
    result->reason = hle_result == QEMU_CXBX_CPU_OK ?
        QEMU_CXBX_CPU_RUN_STOPPED : QEMU_CXBX_CPU_RUN_UNHANDLED_HLE;
    qemu_mutex_unlock(&cpu->lock);
    return QEMU_CXBX_CPU_OK;
}

static int cxbx_cpu_execute_slice(QemuCxbxCpu *cpu,
                                  QemuCxbxCpuRunResult *result)
{
    CPUState *cs;
    CPUX86State *env;
    int exec_result;
    uint32_t kind, id;
    bool timeslice_expired = false;

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
    if (!cpu->registers_valid) {
        qemu_mutex_unlock(&cpu->lock);
        return QEMU_CXBX_CPU_INVALID_STATE;
    }
    if (cpu->thread_state_valid) {
        cxbx_restore_thread_state(cs, env, &cpu->thread_state);
        cxbx_overlay_public_registers(env, &cpu->registers,
                                      &cpu->snapshot_registers);
    } else {
        cs->exception_index = -1;
        cxbx_copy_to_env(env, &cpu->registers);
    }
    cxbx_current = cpu;
    current_cpu = cs;
    cs->halted = false;
    /* The physical CPUState is shared by all logical Xbox threads.  Never let
     * an interrupt requested for one wrapper leak into the next context (with
     * an uninitialized IDT this vectors to EIP 0).  Reapply only this logical
     * context's pending architectural interrupts after clearing worker kicks
     * and terminal state left by the previous slice. */
    cpu_reset_interrupt(cs, ~0);
    if (cpu->pending_interrupts) {
        tcg_handle_interrupt(cs, cpu->pending_interrupts);
        cpu->pending_interrupts = 0;
    }
    qatomic_store_release(&cs->exit_request, false);
    /* tcg_cpu_exec() is the low-level executor, not the complete vCPU loop.
     * Consume internal exits here so the embedding host does not spin forever
     * on an unchanged EIP.  EXCP_ATOMIC must run through QEMU's serialized
     * atomic step; EXCP_INTERRUPT only crosses the ABI at a Cxbx gateway.
     */
    for (;;) {
        icount_prepare_for_run(cs, CXBX_TCG_INSN_BUDGET);
        exec_result = tcg_cpu_exec(cs);
        timeslice_expired = exec_result == EXCP_INTERRUPT &&
            cs->neg.icount_decr.u16.low == 0 && cs->icount_extra == 0;
        icount_process_data(cs);

        if (cpu->stop_requested) {
            break;
        }
        if (exec_result == EXCP_ATOMIC) {
            cpu_exec_step_atomic(cs);
            continue;
        }
        if (exec_result == EXCP_INTERRUPT) {
            break;
        }
        if (cxbx_decode_gateway((uint32_t)env->eip, &kind, &id)) {
            break;
        }
        break;
    }
    current_cpu = NULL;
    cxbx_current = NULL;
    cxbx_copy_from_env(env, &cpu->registers);
    /* EXCP_INTERRUPT/HLT/DEBUG are completed tcg_cpu_exec() return reasons,
     * not pending architectural exceptions for the next logical context
     * turn.  Saving them would make a resumed context immediately return the
     * previous thread's terminal reason without executing an instruction. */
    if (exec_result >= EXCP_INTERRUPT) {
        cs->exception_index = -1;
    }
    cxbx_save_thread_state(cs, env, &cpu->thread_state);
    cpu->snapshot_registers = cpu->registers;
    cpu->thread_state_valid = true;

    if (cpu->stop_requested) {
        result->reason = QEMU_CXBX_CPU_RUN_HOST_REQUEST;
    } else if (cxbx_decode_gateway((uint32_t)env->eip, &kind, &id)) {
        result->reason = QEMU_CXBX_CPU_RUN_STOPPED;
    } else if (exec_result == EXCP_INTERRUPT && timeslice_expired) {
        result->reason = QEMU_CXBX_CPU_RUN_TIMESLICE;
    } else if (exec_result == EXCP_INTERRUPT) {
        result->reason = QEMU_CXBX_CPU_RUN_INTERRUPT;
    } else if (exec_result == EXCP_HLT) {
        result->reason = QEMU_CXBX_CPU_RUN_HALT;
    } else if (exec_result == EXCP_DEBUG) {
        result->reason = QEMU_CXBX_CPU_RUN_GUEST_EXCEPTION;
        result->exception_vector = EXCP01_DB;
        result->error_code = exec_result;
        result->fault_address = (uint32_t)env->eip;
    } else {
        result->reason = QEMU_CXBX_CPU_RUN_GUEST_EXCEPTION;
        result->exception_vector = cs->exception_index;
        result->error_code = exec_result;
        result->fault_address = env->cr[2];
    }
    qemu_mutex_unlock(&cpu->lock);
    return QEMU_CXBX_CPU_OK;
}

static void cxbx_run_on_vcpu(CPUState *cs, run_on_cpu_data data)
{
    CxbxRunRequest *request = data.host_ptr;

    g_assert(cs == CPU(request->cpu->x86));
    /* run_on_cpu callbacks hold the BQL.  Match QEMU's RR loop and release it
     * while translated code executes; icount temporarily reacquires it only
     * when timer processing requires that. */
    bql_unlock();
    request->status = cxbx_cpu_execute_slice(request->cpu, request->result);
    bql_lock();
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_run(QemuCxbxCpu *cpu,
                                      QemuCxbxCpuRunResult *result)
{
    CxbxRunRequest request;
    bool dispatched;
    int status;

    if (!cpu || !result || result->struct_size < sizeof(*result) ||
        result->version != 1 || !cxbx_global_lock_ready) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }

    /* Keep HLE on the originating Xbox host thread: its KPCR/TLS belongs to
     * that thread.  Only translated guest execution is sent to the persistent
     * TCG worker. */
    status = cxbx_cpu_dispatch_gateway(cpu, result, &dispatched);
    if (status != QEMU_CXBX_CPU_OK || dispatched) {
        return status;
    }

    request.cpu = cpu;
    request.result = result;
    request.status = QEMU_CXBX_CPU_INVALID_STATE;
    bql_lock();
    run_on_cpu(CPU(cpu->x86), cxbx_run_on_vcpu,
               RUN_ON_CPU_HOST_PTR(&request));
    bql_unlock();
    status = request.status;
    return status;
}

int QEMU_CXBX_CALL qemu_cxbx_cpu_interrupt(QemuCxbxCpu *cpu, uint32_t flags)
{
    int tcg_mask = 0;

    if (!cpu || !flags ||
        (flags & ~(QEMU_CXBX_CPU_INTERRUPT_HARD |
                   QEMU_CXBX_CPU_INTERRUPT_NMI))) {
        return QEMU_CXBX_CPU_INVALID_ARGUMENT;
    }
    if (flags & QEMU_CXBX_CPU_INTERRUPT_HARD) {
        tcg_mask |= CPU_INTERRUPT_HARD;
    }
    if (flags & QEMU_CXBX_CPU_INTERRUPT_NMI) {
        tcg_mask |= CPU_INTERRUPT_NMI;
    }
    qemu_mutex_lock(&cpu->lock);
    cpu->pending_interrupts |= tcg_mask;
    cpu_exit(CPU(cpu->x86));
    qemu_mutex_unlock(&cpu->lock);
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
