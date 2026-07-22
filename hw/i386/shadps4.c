/*
 * Hybrid shadPS4 machine
 *
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define QEMU_HOST_INTERNAL
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/qemu-host.h"
#include "qemu/thread.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "exec/cputlb.h"
#include "hw/core/boards.h"
#include "hw/i386/apic.h"
#include "hw/i386/x86.h"
#include "hw/i386/shadps4-gpu.h"
#include "hw/i386/shadps4-hle.h"
#include "hw/i386/shadps4-io.h"
#include "hw/i386/shadps4-loader.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "system/memory.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "target/i386/cpu.h"

#define TYPE_SHADPS4_MACHINE MACHINE_TYPE_NAME("shadps4")
#define SHADPS4_PML4_PHYS 0x1000
#define SHADPS4_PDPT_PHYS 0x2000
#define SHADPS4_PDPT_HIGH_PHYS 0xd000
#define SHADPS4_PD_LOW_PHYS 0x3000
#define SHADPS4_PD_STACK_PHYS 0x4000
#define SHADPS4_PD_IMAGE_PHYS 0x5000
#define SHADPS4_HLE_GATEWAY_PHYS 0x6000
#define SHADPS4_GDT_PHYS 0x7000
#define SHADPS4_TSS_PHYS 0x8000
#define SHADPS4_SYSCALL_STUB_PHYS 0x9000
#define SHADPS4_IDT_PHYS 0xa000
#define SHADPS4_EXCEPTION_STUB_PHYS 0xb000
#define SHADPS4_EXCEPTION_STUB_SIZE 16
#define SHADPS4_EXCEPTION_COMMON_PHYS 0xc000
#define SHADPS4_PS2_PML4_PHYS 0xe000
#define SHADPS4_PS2_PDPT_PHYS 0xf000
#define SHADPS4_PS2_PD_IMAGE_PHYS 0x10000
#define SHADPS4_PD_DYNAMIC_PHYS 0x200000
#define SHADPS4_PD_DYNAMIC_PAGES 1024
#define SHADPS4_DYNAMIC_VIRT_BASE 0x1000000000ULL
#define SHADPS4_DYNAMIC_PHYS_MIN (32 * MiB)
#define SHADPS4_STACK_PHYS_BASE (8 * MiB)
#define SHADPS4_STACK_VIRT_BASE (SHADPS4_IMAGE_VIRT_BASE - 2 * MiB)
#define SHADPS4_STACK_SIZE (2 * MiB)
#define SHADPS4_TLS_PHYS_BASE (6 * MiB)
#define SHADPS4_TLS_VIRT_BASE (SHADPS4_STACK_VIRT_BASE - 2 * MiB)
#define SHADPS4_TLS_SIZE (2 * MiB)
#define SHADPS4_EXIT_STUB_OFFSET (SHADPS4_TLS_SIZE - 0x100)
#define SHADPS4_EXIT_STUB_PHYS \
    (SHADPS4_TLS_PHYS_BASE + SHADPS4_EXIT_STUB_OFFSET)
#define SHADPS4_EXIT_STUB_VIRT \
    (SHADPS4_TLS_VIRT_BASE + SHADPS4_EXIT_STUB_OFFSET)
#define SHADPS4_KERNEL_STACK_TOP 0x200000
#define SHADPS4_PAGE_PRESENT (1ULL << 0)
#define SHADPS4_PAGE_WRITE (1ULL << 1)
#define SHADPS4_PAGE_USER (1ULL << 2)
#define SHADPS4_PAGE_LARGE (1ULL << 7)
#define SHADPS4_PAGE_NX (1ULL << 63)
#define SHADPS4_ELF_PF_X 1
#define SHADPS4_ELF_PF_W 2
#define SHADPS4_ELF_STT_FUNC 2
#define SHADPS4_ELF_STT_OBJECT 1
#define SHADPS4_HLE_TRAILER_SIZE 32
#define SHADPS4_HLE_BOOTSTRAP_SIZE 512
#define SHADPS4_HLE_BOOTSTRAP_SLOT_SIZE (SHADPS4_HLE_BOOTSTRAP_SIZE / 2)
#define SHADPS4_SCHEDULER_QUANTUM_NS (2 * 1000 * 1000)
OBJECT_DECLARE_SIMPLE_TYPE(ShadPS4MachineState, SHADPS4_MACHINE)

typedef enum ShadPS4Variant {
    SHADPS4_VARIANT_BASE,
    SHADPS4_VARIANT_NEO,
} ShadPS4Variant;

typedef struct ShadPS4GuestThreadContext {
    uint64_t regs[CPU_NB_REGS];
    uint64_t eip;
    uint32_t eflags;
    uint64_t fs_base;
    uint64_t gs_base;
    uint64_t cr3;
    uint8_t xmm[sizeof(((CPUX86State *)0)->xmm_regs)];
    uint8_t fpregs[sizeof(((CPUX86State *)0)->fpregs)];
    uint8_t fptags[sizeof(((CPUX86State *)0)->fptags)];
    uint8_t bnd_regs[sizeof(((CPUX86State *)0)->bnd_regs)];
    uint8_t bndcs_regs[sizeof(((CPUX86State *)0)->bndcs_regs)];
    uint64_t opmask_regs[NB_OPMASK_REGS];
    uint32_t mxcsr;
    uint16_t fpuc;
    uint16_t fpus;
    uint8_t fpstt;
    uint64_t schedule_order;
    bool valid;
    bool runnable;
} ShadPS4GuestThreadContext;

struct ShadPS4MachineState {
    X86MachineState parent_obj;
    ShadPS4ImageInfo image;
    ShadPS4ImageInfo modules[SHADPS4_MAX_MODULES - 1];
    ShadPS4ImageInfo hle_image;
    MemoryRegion hle_gateway;
    ShadPS4HLEState hle;
    ShadPS4GPUState gpu;
    ShadPS4IOState *io;
    char *title_id;
    ShadPS4Variant variant;
    uint32_t module_count;
    uint64_t mapped_image_size;
    bool image_loaded;
    bool execute;
    ShadPS4GuestThreadContext thread_contexts[
        SHADPS4_HLE_MAX_SERVICE_OBJECTS];
    uint64_t pending_thread_switch[8];
    bool hle_gateway_active[8];
    QemuMutex hle_lock;
    QemuMutex scheduler_lock;
    QEMUTimer *scheduler_timer;
    uint64_t scheduler_order;
    uint64_t scheduler_switches;
    uint64_t ps2_compiler_cr3;
    uint64_t ps2_private_phys_base;
    uint64_t ps2_compiler_image_entry;
};

static uint32_t shadps4_unresolved_relocations(ShadPS4MachineState *sms);
static bool shadps4_schedule_guest_thread(ShadPS4MachineState *sms,
                                          CPUState *cs, bool requeue,
                                          uint64_t result);
static bool shadps4_apply_guest_thread_switch(ShadPS4MachineState *sms,
                                               CPUState *cs);
static bool shadps4_block_guest_thread(ShadPS4MachineState *sms,
                                        CPUState *cs, uint64_t result);

static void shadps4_report_unresolved_image(const ShadPS4ImageInfo *image,
                                            const char *label)
{
    g_autoptr(GHashTable) reported =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    uint32_t i;

    for (i = 0; i < image->pending_relocation_count; i++) {
        const ShadPS4PendingRelocation *relocation =
            &image->pending_relocations[i];
        const ShadPS4DynamicSymbol *symbol;
        g_autofree char *key = NULL;

        if (relocation->applied || relocation->symbol_index >=
            image->symbol_count) {
            continue;
        }
        symbol = &image->symbols[relocation->symbol_index];
        key = g_strdup_printf("%s|%s|%s|%u", symbol->nid ?: "",
                              symbol->library ?: "", symbol->module ?: "",
                              relocation->type);
        if (!g_hash_table_add(reported, g_steal_pointer(&key))) {
            continue;
        }
        warn_report("shadPS4 unresolved %s import: NID=%s library=%s "
                    "module=%s type=%u",
                    label, symbol->nid ?: "<none>",
                    symbol->library ?: "<none>",
                    symbol->module ?: "<none>", relocation->type);
    }
}

static void shadps4_report_unresolved(ShadPS4MachineState *sms)
{
    uint32_t i;

    shadps4_report_unresolved_image(&sms->image, "main");
    for (i = 0; i < sms->module_count; i++) {
        char label[24];

        g_snprintf(label, sizeof(label), "module-%u", i + 1);
        shadps4_report_unresolved_image(&sms->modules[i], label);
    }
}

static uint64_t shadps4_hle_gateway_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    ShadPS4MachineState *sms = opaque;
    CPUState *cs = current_cpu;
    uint64_t result;

    if (!cs || cs->cpu_index >= ARRAY_SIZE(sms->hle.cpu_results)) {
        return sms->hle.result;
    }
    qemu_mutex_lock(&sms->scheduler_lock);
    if (sms->hle.cpu_thread_handles[cs->cpu_index] > 0x400 &&
        sms->hle.cpu_thread_handles[cs->cpu_index] - 0x400 <
            ARRAY_SIZE(sms->hle.thread_results)) {
        result = sms->hle.thread_results[
            sms->hle.cpu_thread_handles[cs->cpu_index] - 0x400];
    } else {
        result = sms->hle.cpu_results[cs->cpu_index];
    }
    sms->hle_gateway_active[cs->cpu_index] = false;
    qemu_mutex_unlock(&sms->scheduler_lock);
    return result;
}

static void shadps4_hle_gateway_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    ShadPS4MachineState *sms = opaque;
    CPUState *cs = current_cpu ? current_cpu : first_cpu;
    uint64_t nonvolatile_before[6];
    CPUX86State *env;
    uint64_t result;
    bool blocked_without_replacement = false;
    bool flush_all_tlbs = false;
    bool switched;

    if (!cs) {
        sms->hle.result = -1;
        return;
    }
    if (cs->cpu_index < ARRAY_SIZE(sms->hle_gateway_active)) {
        qemu_mutex_lock(&sms->scheduler_lock);
        sms->hle_gateway_active[cs->cpu_index] = true;
        qemu_mutex_unlock(&sms->scheduler_lock);
    }
    env = &X86_CPU(cs)->env;
    nonvolatile_before[0] = env->regs[R_EBX];
    nonvolatile_before[1] = env->regs[R_EBP];
    nonvolatile_before[2] = env->regs[R_R12];
    nonvolatile_before[3] = env->regs[R_R13];
    nonvolatile_before[4] = env->regs[R_R14];
    nonvolatile_before[5] = env->regs[R_R15];
    if (value == 3 &&
        !g_strcmp0(g_getenv("SHADPS4_HLE_TRACE"), "all")) {
        uint64_t fd = env->regs[R_EDI];

        info_report("shadPS4 read gateway: fd=%#" PRIx64
                    " type=%u addr=%#" PRIx64 " size=%#" PRIx64,
                    fd, fd < SHADPS4_HLE_MAX_FDS ?
                        sms->hle.files[fd] : 0,
                    env->regs[R_ESI], env->regs[R_EDX]);
    }
    qemu_mutex_lock(&sms->hle_lock);
    result = shadps4_hle_dispatch(&sms->hle, cs, value);
    if (nonvolatile_before[0] != env->regs[R_EBX] ||
        nonvolatile_before[1] != env->regs[R_EBP] ||
        nonvolatile_before[2] != env->regs[R_R12] ||
        nonvolatile_before[3] != env->regs[R_R13] ||
        nonvolatile_before[4] != env->regs[R_R14] ||
        nonvolatile_before[5] != env->regs[R_R15]) {
        error_report("shadPS4 HLE corrupted nonvolatile guest registers: "
                     "number=%#" PRIx64 " rbx=%#" PRIx64 "->%#" PRIx64,
                     value, nonvolatile_before[0], env->regs[R_EBX]);
    }
    sms->hle.result = result;
    if (cs->cpu_index < ARRAY_SIZE(sms->hle.cpu_thread_handles) &&
        sms->hle.cpu_thread_handles[cs->cpu_index] > 0x400 &&
        sms->hle.cpu_thread_handles[cs->cpu_index] - 0x400 <
            ARRAY_SIZE(sms->hle.thread_results)) {
        sms->hle.thread_results[
            sms->hle.cpu_thread_handles[cs->cpu_index] - 0x400] = result;
    } else if (cs->cpu_index < ARRAY_SIZE(sms->hle.cpu_results)) {
        sms->hle.cpu_results[cs->cpu_index] = result;
    }
    if (value != UINT64_MAX) {
        sms->hle.last_hle_result = result;
        shadps4_hle_complete_call(&sms->hle, result);
    }
    if (value == SHADPS4_HLE_PTHREAD_YIELD) {
        shadps4_schedule_guest_thread(sms, cs, true, result);
    } else if (cs->cpu_index <
                   ARRAY_SIZE(sms->hle.cpu_pending_wait_thread) &&
               sms->hle.cpu_pending_wait_thread[cs->cpu_index]) {
        uint64_t blocked = sms->hle.cpu_pending_wait_thread[cs->cpu_index];

        sms->hle.cpu_pending_wait_thread[cs->cpu_index] = 0;
        if (sms->hle.cpu_thread_handles[cs->cpu_index] == blocked) {
            blocked_without_replacement = cs->cpu_index &&
                !shadps4_block_guest_thread(sms, cs, result);
        }
    }
    flush_all_tlbs = sms->hle.tlb_flush_all_requested;
    sms->hle.tlb_flush_all_requested = false;
    qemu_mutex_unlock(&sms->hle_lock);
    if (flush_all_tlbs) {
        tlb_flush_all_cpus_synced(cs);
    }
    switched = shadps4_apply_guest_thread_switch(sms, cs);
    if (blocked_without_replacement) {
        cs->halted = 1;
        cpu_interrupt(cs, CPU_INTERRUPT_HALT);
    }
    if (switched || blocked_without_replacement) {
        qemu_mutex_lock(&sms->scheduler_lock);
        sms->hle_gateway_active[cs->cpu_index] = false;
        qemu_mutex_unlock(&sms->scheduler_lock);
        cpu_loop_exit(cs);
    }
}

static const MemoryRegionOps shadps4_hle_gateway_ops = {
    .read = shadps4_hle_gateway_read,
    .write = shadps4_hle_gateway_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 8,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8,
    },
};

static bool shadps4_write_guest(uint64_t physical_addr, const void *buffer,
                                size_t size, const char *description,
                                Error **errp)
{
    if (address_space_write(&address_space_memory, physical_addr,
                            MEMTXATTRS_UNSPECIFIED, buffer, size) != MEMTX_OK) {
        error_setg(errp, "failed to write %s at 0x%" PRIx64,
                   description, physical_addr);
        return false;
    }
    return true;
}

static bool shadps4_write_u64(uint64_t physical_addr, uint64_t value,
                              Error **errp)
{
    uint64_t value_le = cpu_to_le64(value);

    if (address_space_write(&address_space_memory, physical_addr,
                            MEMTXATTRS_UNSPECIFIED, &value_le,
                            sizeof(value_le)) != MEMTX_OK) {
        error_setg(errp, "failed to write page table at 0x%" PRIx64,
                   physical_addr);
        return false;
    }
    return true;
}

static bool shadps4_align_up_valid(uint64_t value, uint64_t alignment,
                                   uint64_t *result)
{
    if (!alignment || !is_power_of_2(alignment) ||
        value > UINT64_MAX - (alignment - 1)) {
        return false;
    }
    *result = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

static bool shadps4_build_page_tables(ShadPS4MachineState *sms,
                                      Error **errp)
{
    uint64_t page_flags = SHADPS4_PAGE_PRESENT | SHADPS4_PAGE_WRITE |
                          SHADPS4_PAGE_USER;
    uint64_t kernel_page_flags = SHADPS4_PAGE_PRESENT | SHADPS4_PAGE_WRITE;
    uint64_t image_pages = DIV_ROUND_UP(sms->mapped_image_size, 2 * MiB);
    uint64_t i;

    if (image_pages == 0 || image_pages > 512) {
        error_setg(errp, "initial shadPS4 image mapping exceeds 1 GiB");
        return false;
    }
    if (address_space_set(&address_space_memory, SHADPS4_PML4_PHYS, 0,
                          5 * 4096, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
        address_space_set(&address_space_memory, SHADPS4_TLS_PHYS_BASE, 0,
                          SHADPS4_TLS_SIZE,
                          MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
        address_space_set(&address_space_memory, SHADPS4_STACK_PHYS_BASE, 0,
                          SHADPS4_STACK_SIZE,
                          MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
        address_space_set(&address_space_memory, SHADPS4_PDPT_HIGH_PHYS, 0,
                          4096, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
        address_space_set(&address_space_memory, SHADPS4_PD_DYNAMIC_PHYS, 0,
                          SHADPS4_PD_DYNAMIC_PAGES * 4096,
                          MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        error_setg(errp, "failed to initialize bootstrap memory");
        return false;
    }

    if (!shadps4_write_u64(SHADPS4_PML4_PHYS,
                            SHADPS4_PDPT_PHYS | page_flags, errp) ||
        !shadps4_write_u64(SHADPS4_PML4_PHYS + sizeof(uint64_t),
                            SHADPS4_PDPT_HIGH_PHYS | page_flags, errp) ||
        !shadps4_write_u64(SHADPS4_PDPT_PHYS,
                            SHADPS4_PD_LOW_PHYS | page_flags, errp) ||
        !shadps4_write_u64(SHADPS4_PDPT_PHYS + 31 * sizeof(uint64_t),
                            SHADPS4_PD_STACK_PHYS | page_flags, errp) ||
        !shadps4_write_u64(SHADPS4_PDPT_PHYS + 32 * sizeof(uint64_t),
                            SHADPS4_PD_IMAGE_PHYS | page_flags, errp)) {
        return false;
    }

    for (i = 0; i < SHADPS4_PD_DYNAMIC_PAGES; i++) {
        if (i == 0 || i == 31 || i == 32) {
            continue;
        }
        if (!shadps4_write_u64(
                (i < 512 ? SHADPS4_PDPT_PHYS :
                           SHADPS4_PDPT_HIGH_PHYS) +
                    (i % 512) * sizeof(uint64_t),
                (SHADPS4_PD_DYNAMIC_PHYS + i * 4096) | page_flags, errp)) {
            return false;
        }
    }

    for (i = 0; i < 32; i++) {
        if (!shadps4_write_u64(
                SHADPS4_PD_LOW_PHYS + i * sizeof(uint64_t),
                i * 2 * MiB | kernel_page_flags | SHADPS4_PAGE_LARGE,
                errp)) {
            return false;
        }
    }
    if (!shadps4_write_u64(
            SHADPS4_PD_STACK_PHYS + 510 * sizeof(uint64_t),
            SHADPS4_TLS_PHYS_BASE | page_flags | SHADPS4_PAGE_LARGE |
            SHADPS4_PAGE_NX,
            errp) ||
        !shadps4_write_u64(
            SHADPS4_PD_STACK_PHYS + 511 * sizeof(uint64_t),
            SHADPS4_STACK_PHYS_BASE | page_flags | SHADPS4_PAGE_LARGE |
            SHADPS4_PAGE_NX,
            errp)) {
        return false;
    }
    for (i = 0; i < image_pages; i++) {
        uint64_t virtual_start = sms->image.virtual_base + i * 2 * MiB;
        uint64_t virtual_end = virtual_start + 2 * MiB;
        uint64_t image_flags = SHADPS4_PAGE_PRESENT | SHADPS4_PAGE_USER |
                               SHADPS4_PAGE_LARGE | SHADPS4_PAGE_NX;
        uint32_t module_index;

        for (module_index = 0; module_index <= sms->module_count + 1;
             module_index++) {
            const ShadPS4ImageInfo *image;
            uint16_t segment_index;

            if (!module_index) {
                image = &sms->image;
            } else if (module_index <= sms->module_count) {
                image = &sms->modules[module_index - 1];
            } else {
                image = &sms->hle_image;
            }

            for (segment_index = 0; segment_index < image->segment_count;
                 segment_index++) {
                const ShadPS4ImageSegment *segment =
                    &image->segments[segment_index];

                if (segment->virtual_addr < virtual_end &&
                    virtual_start < segment->virtual_addr +
                                    segment->memory_size) {
                    if (segment->flags & SHADPS4_ELF_PF_W) {
                        image_flags |= SHADPS4_PAGE_WRITE;
                    }
                    if (segment->flags & SHADPS4_ELF_PF_X) {
                        image_flags &= ~SHADPS4_PAGE_NX;
                    }
                }
            }
        }
        if (!shadps4_write_u64(
                SHADPS4_PD_IMAGE_PHYS + i * sizeof(uint64_t),
                (SHADPS4_IMAGE_PHYS_BASE + i * 2 * MiB) |
                image_flags, errp)) {
            return false;
        }
    }
    return true;
}

static bool shadps4_build_ps2_process_page_tables(
    ShadPS4MachineState *sms, uint64_t private_phys_base, Error **errp)
{
    g_autofree uint8_t *page = NULL;
    uint64_t page_flags = SHADPS4_PAGE_PRESENT | SHADPS4_PAGE_WRITE |
                          SHADPS4_PAGE_USER;
    uint64_t image_pages;
    uint64_t i;

    if (!sms->hle.ps2_compiler_entry) {
        sms->ps2_compiler_cr3 = 0;
        return true;
    }
    image_pages = DIV_ROUND_UP(sms->mapped_image_size, 2 * MiB);
    if (!image_pages || image_pages > 512 ||
        private_phys_base > MACHINE(sms)->ram_size ||
        image_pages * 2 * MiB > MACHINE(sms)->ram_size - private_phys_base) {
        error_setg(errp, "PS2 compiler private image exceeds guest RAM");
        return false;
    }
    if (address_space_set(&address_space_memory, SHADPS4_PS2_PML4_PHYS, 0,
                          3 * 4096, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
        !shadps4_write_u64(SHADPS4_PS2_PML4_PHYS,
                            SHADPS4_PS2_PDPT_PHYS | page_flags, errp) ||
        !shadps4_write_u64(SHADPS4_PS2_PML4_PHYS + sizeof(uint64_t),
                            SHADPS4_PDPT_HIGH_PHYS | page_flags, errp)) {
        return false;
    }
    page = g_malloc(2 * MiB);
    for (i = 0; i < image_pages; i++) {
        uint64_t old_entry;
        uint64_t old_entry_le;
        uint64_t new_entry;

        if (address_space_read(&address_space_memory,
                               SHADPS4_PD_IMAGE_PHYS + i * sizeof(uint64_t),
                               MEMTXATTRS_UNSPECIFIED, &old_entry_le,
                               sizeof(old_entry_le)) != MEMTX_OK ||
            address_space_read(&address_space_memory,
                               sms->image.physical_base + i * 2 * MiB,
                               MEMTXATTRS_UNSPECIFIED, page,
                               2 * MiB) != MEMTX_OK ||
            address_space_write(&address_space_memory,
                                private_phys_base + i * 2 * MiB,
                                MEMTXATTRS_UNSPECIFIED, page,
                                2 * MiB) != MEMTX_OK) {
            error_setg(errp, "failed to clone PS2 compiler image page %" PRIu64,
                       i);
            return false;
        }
        old_entry = le64_to_cpu(old_entry_le);
        new_entry = (old_entry & ~UINT64_C(0x000fffffffe00000)) |
                    (private_phys_base + i * 2 * MiB);
        if (!shadps4_write_u64(SHADPS4_PS2_PD_IMAGE_PHYS +
                                i * sizeof(uint64_t), new_entry, errp)) {
            return false;
        }
    }
    if (address_space_read(&address_space_memory, SHADPS4_PDPT_PHYS,
                           MEMTXATTRS_UNSPECIFIED, page, 4096) != MEMTX_OK ||
        address_space_write(&address_space_memory, SHADPS4_PS2_PDPT_PHYS,
                            MEMTXATTRS_UNSPECIFIED, page, 4096) != MEMTX_OK ||
        !shadps4_write_u64(SHADPS4_PS2_PDPT_PHYS + 32 * sizeof(uint64_t),
                            SHADPS4_PS2_PD_IMAGE_PHYS | page_flags, errp)) {
        error_setg(errp, "failed to build PS2 compiler page tables");
        return false;
    }
    sms->ps2_compiler_cr3 = SHADPS4_PS2_PML4_PHYS;
    sms->ps2_private_phys_base = private_phys_base;
    info_report("shadPS4 PS2 process address space: cr3=%#" PRIx64
                " image=%#" PRIx64 " size=%#" PRIx64,
                sms->ps2_compiler_cr3, private_phys_base,
                image_pages * 2 * MiB);
    return true;
}

static uint64_t shadps4_tss_descriptor_low(uint64_t base)
{
    uint64_t limit = 103;

    return (limit & 0xffff) | ((base & 0xffffff) << 16) |
           (9ULL << 40) | (1ULL << 47) |
           ((limit & 0xf0000) << 32) | ((base & 0xff000000) << 32);
}

static bool shadps4_build_descriptor_tables(CPUX86State *env, Error **errp)
{
    uint64_t gdt[7] = {
        0,
        cpu_to_le64(0x00af9a000000ffffULL),
        cpu_to_le64(0x00cf92000000ffffULL),
        cpu_to_le64(0x00cff2000000ffffULL),
        cpu_to_le64(0x00affa000000ffffULL),
        cpu_to_le64(shadps4_tss_descriptor_low(SHADPS4_TSS_PHYS)),
        0,
    };
    uint8_t idt[256 * 16] = { 0 };
    uint8_t tss[104] = { 0 };
    uint64_t rsp0_le = cpu_to_le64(SHADPS4_KERNEL_STACK_TOP);
    uint16_t io_map_le = cpu_to_le16(sizeof(tss));
    int i;

    memcpy(tss + 4, &rsp0_le, sizeof(rsp0_le));
    memcpy(tss + 102, &io_map_le, sizeof(io_map_le));
    for (i = 0; i < 256; i++) {
        uint8_t *gate = idt + i * 16;
        uint64_t handler = SHADPS4_EXCEPTION_STUB_PHYS +
                           i * SHADPS4_EXCEPTION_STUB_SIZE;
        uint16_t offset_low = cpu_to_le16(handler);
        uint16_t selector = cpu_to_le16(0x08);
        uint16_t offset_mid = cpu_to_le16(handler >> 16);
        uint32_t offset_high = cpu_to_le32(handler >> 32);

        memcpy(gate, &offset_low, sizeof(offset_low));
        memcpy(gate + 2, &selector, sizeof(selector));
        gate[5] = 0x8e;
        memcpy(gate + 6, &offset_mid, sizeof(offset_mid));
        memcpy(gate + 8, &offset_high, sizeof(offset_high));
    }
    if (!shadps4_write_guest(SHADPS4_GDT_PHYS, gdt, sizeof(gdt),
                             "GDT", errp) ||
        !shadps4_write_guest(SHADPS4_TSS_PHYS, tss, sizeof(tss),
                             "TSS", errp) ||
        !shadps4_write_guest(SHADPS4_IDT_PHYS, idt, sizeof(idt),
                             "IDT", errp)) {
        return false;
    }

    env->gdt.base = SHADPS4_GDT_PHYS;
    env->gdt.limit = sizeof(gdt) - 1;
    env->idt.base = SHADPS4_IDT_PHYS;
    env->idt.limit = sizeof(idt) - 1;
    env->tr.selector = 0x28;
    env->tr.base = SHADPS4_TSS_PHYS;
    env->tr.limit = sizeof(tss) - 1;
    env->tr.flags = DESC_P_MASK | (9 << DESC_TYPE_SHIFT);
    return true;
}

static bool shadps4_build_gateway(Error **errp)
{
    static const uint8_t syscall_stub[] = {
        0x48, 0xa3, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0xa1, 0x08, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x0f, 0x07,
    };
    static const uint8_t exception_common[] = {
        0x50,
        0x48, 0xb8, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x48, 0xa3, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xfa, 0xf4, 0xeb, 0xfd,
    };
    static const uint8_t exit_stub[] = {
        0xb8, 0x01, 0x00, 0x00, 0x00,
        0x31, 0xff,
        0x0f, 0x05,
        0xeb, 0xfe,
    };

    uint8_t exception_stubs[256 * SHADPS4_EXCEPTION_STUB_SIZE];
    int vector;

    memset(exception_stubs, 0x90, sizeof(exception_stubs));
    for (vector = 0; vector < 256; vector++) {
        uint8_t *stub = exception_stubs +
                        vector * SHADPS4_EXCEPTION_STUB_SIZE;
        bool has_error_code = vector == 8 ||
                              (vector >= 10 && vector <= 14) ||
                              vector == 17 || vector == 21 ||
                              vector == 29 || vector == 30;
        size_t offset = 0;
        int64_t next;
        int32_t relative;

        if (!has_error_code) {
            stub[offset++] = 0x6a;
            stub[offset++] = 0x00;
        }
        stub[offset++] = 0x68;
        stl_le_p(stub + offset, vector);
        offset += sizeof(uint32_t);
        stub[offset++] = 0xe9;
        next = SHADPS4_EXCEPTION_STUB_PHYS +
               vector * SHADPS4_EXCEPTION_STUB_SIZE + offset +
               sizeof(uint32_t);
        relative = SHADPS4_EXCEPTION_COMMON_PHYS - next;
        stl_le_p(stub + offset, relative);
    }

    return shadps4_write_guest(SHADPS4_SYSCALL_STUB_PHYS, syscall_stub,
                               sizeof(syscall_stub), "syscall stub", errp) &&
           shadps4_write_guest(SHADPS4_EXCEPTION_STUB_PHYS, exception_stubs,
                               sizeof(exception_stubs), "exception stubs",
                               errp) &&
           shadps4_write_guest(SHADPS4_EXCEPTION_COMMON_PHYS,
                               exception_common, sizeof(exception_common),
                               "exception common stub", errp) &&
           shadps4_write_guest(SHADPS4_EXIT_STUB_PHYS, exit_stub,
                               sizeof(exit_stub), "process exit stub", errp);
}

static bool shadps4_build_tls(ShadPS4MachineState *sms, uint64_t *gs_base,
                              Error **errp)
{
    uint64_t offsets[SHADPS4_MAX_MODULES] = { 0 };
    uint32_t module_count = sms->module_count + 1;
    uint32_t tls_count = 0;
    uint64_t static_size = 0;
    uint64_t tcb_virt;
    uint64_t tcb_phys;
    uint64_t tcb_self;
    uint64_t dtv_addr;
    uint64_t dtv_generation = cpu_to_le64(1);
    uint64_t dtv_count;
    uint32_t module_index;

    if (address_space_set(&address_space_memory, SHADPS4_TLS_PHYS_BASE, 0,
                          SHADPS4_EXIT_STUB_OFFSET,
                          MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        error_setg(errp, "failed to clear bootstrap TLS area");
        return false;
    }
    memset(sms->hle.tls_guest_addr, 0,
           sizeof(sms->hle.tls_guest_addr));
    memset(sms->hle.tls_guest_size, 0,
           sizeof(sms->hle.tls_guest_size));
    for (module_index = 0; module_index < module_count; module_index++) {
        const ShadPS4ImageInfo *image = module_index ?
            &sms->modules[module_index - 1] : &sms->image;
        uint64_t alignment = MAX(image->tls_align, 0x20);
        uint32_t tls_slot;

        if (!image->tls_present) {
            continue;
        }
        if (!image->tls_module_id ||
            image->tls_module_id > SHADPS4_HLE_MAX_TLS_MODULES) {
            error_setg(errp, "module %u has an invalid TLS module ID",
                       module_index + 1);
            return false;
        }
        tls_slot = image->tls_module_id - 1;
        tls_count = MAX(tls_count, image->tls_module_id);

        if (!is_power_of_2(alignment) ||
            image->tls_file_size > image->tls_memory_size) {
            error_setg(errp, "module %u TLS image is invalid",
                       module_index + 1);
            return false;
        }
        static_size = QEMU_ALIGN_UP(static_size, alignment);
        offsets[module_index] = static_size;
        if (image->tls_memory_size > SHADPS4_EXIT_STUB_OFFSET -
                                     static_size) {
            error_setg(errp, "module TLS images exceed bootstrap TLS area");
            return false;
        }
        sms->hle.tls_guest_addr[tls_slot] =
            SHADPS4_TLS_VIRT_BASE + static_size;
        sms->hle.tls_guest_size[tls_slot] = image->tls_memory_size;
        static_size += image->tls_memory_size;
    }
    sms->hle.tls_module_count = tls_count;
    dtv_count = cpu_to_le64(tls_count);
    static_size = QEMU_ALIGN_UP(static_size, 0x20);
    if (static_size > SHADPS4_EXIT_STUB_OFFSET - 0x40 -
                      (tls_count + 2) * sizeof(uint64_t)) {
        error_setg(errp, "module TLS DTV exceeds bootstrap TLS area");
        return false;
    }
    tcb_virt = SHADPS4_TLS_VIRT_BASE + static_size;
    tcb_phys = SHADPS4_TLS_PHYS_BASE + static_size;
    tcb_self = cpu_to_le64(tcb_virt);
    dtv_addr = cpu_to_le64(tcb_virt + 0x40);
    *gs_base = tcb_virt;
    if (!shadps4_write_guest(tcb_phys, &tcb_self,
                             sizeof(tcb_self), "TCB self pointer", errp) ||
        !shadps4_write_guest(tcb_phys + 8, &dtv_addr,
                             sizeof(dtv_addr), "TCB DTV pointer", errp) ||
        !shadps4_write_guest(tcb_phys + 0x40,
                             &dtv_generation, sizeof(dtv_generation),
                             "DTV generation", errp) ||
        !shadps4_write_guest(tcb_phys + 0x48, &dtv_count,
                             sizeof(dtv_count), "DTV module count", errp)) {
        return false;
    }
    for (module_index = 0; module_index < module_count; module_index++) {
        const ShadPS4ImageInfo *image = module_index ?
            &sms->modules[module_index - 1] : &sms->image;
        uint32_t tls_slot;
        uint64_t tls_addr;
        uint64_t source = 0;
        uint16_t segment_index;

        if (!image->tls_present) {
            continue;
        }
        tls_slot = image->tls_module_id - 1;
        tls_addr = cpu_to_le64(sms->hle.tls_guest_addr[tls_slot]);

        if (!shadps4_write_guest(
                tcb_phys + 0x50 + tls_slot * sizeof(uint64_t),
                &tls_addr, sizeof(tls_addr), "DTV TLS pointer", errp)) {
            return false;
        }
        for (segment_index = 0; segment_index < image->segment_count;
             segment_index++) {
            const ShadPS4ImageSegment *segment =
                &image->segments[segment_index];
            uint64_t offset;

            if (image->tls_addr < segment->virtual_addr) {
                continue;
            }
            offset = image->tls_addr - segment->virtual_addr;
            if (offset <= segment->file_size &&
                image->tls_file_size <= segment->file_size - offset) {
                source = segment->physical_addr + offset;
                break;
            }
        }
        if (image->tls_file_size && !source) {
            error_setg(errp, "module %u TLS is outside loaded segments",
                       module_index + 1);
            return false;
        }
        if (image->tls_file_size) {
            g_autofree uint8_t *data = g_malloc(image->tls_file_size);

            if (address_space_read(&address_space_memory, source,
                                   MEMTXATTRS_UNSPECIFIED, data,
                                   image->tls_file_size) != MEMTX_OK ||
                !shadps4_write_guest(
                    SHADPS4_TLS_PHYS_BASE + offsets[module_index], data,
                    image->tls_file_size, "module TLS image", errp)) {
                if (!*errp) {
                    error_setg(errp, "failed to copy module %u TLS image",
                               module_index + 1);
                }
                return false;
            }
        }
    }
    sms->hle.tls_template_base = SHADPS4_TLS_VIRT_BASE;
    sms->hle.tls_template_tcb = tcb_virt;
    sms->hle.tls_template_size = static_size + 0x50 +
                                 tls_count * sizeof(uint64_t);
    return true;
}

static bool shadps4_build_initial_stack(ShadPS4MachineState *sms,
                                        uint64_t *rsp, uint64_t *params,
                                        Error **errp)
{
    static const char argv0[] = "eboot.bin";
    uint64_t stack_top = SHADPS4_STACK_VIRT_BASE + SHADPS4_STACK_SIZE;
    uint64_t argv0_va = stack_top - 0x40;
    uint64_t params_va = stack_top - 0x200;
    uint64_t argv0_phys = SHADPS4_STACK_PHYS_BASE + SHADPS4_STACK_SIZE - 0x40;
    uint64_t params_phys = SHADPS4_STACK_PHYS_BASE + SHADPS4_STACK_SIZE - 0x200;
    uint64_t value;

    value = cpu_to_le64(1);
    if (!shadps4_write_guest(params_phys, &value, sizeof(value),
                             "process argc", errp)) {
        return false;
    }
    value = cpu_to_le64(argv0_va);
    if (!shadps4_write_guest(params_phys + 8, &value, sizeof(value),
                             "process argv", errp)) {
        return false;
    }
    value = cpu_to_le64(sms->image.entry);
    if (!shadps4_write_guest(params_phys + 272, &value, sizeof(value),
                             "process entry address", errp) ||
        !shadps4_write_guest(argv0_phys, argv0, sizeof(argv0),
                             "process argv[0]", errp)) {
        return false;
    }
    /* PS4 enters the executable with RSP % 16 == 8 and argc/argv on stack. */
    *rsp = params_va - 24;
    value = cpu_to_le64(1);
    if (!shadps4_write_guest(params_phys - 24, &value, sizeof(value),
                             "initial stack argc", errp)) {
        return false;
    }
    value = cpu_to_le64(argv0_va);
    if (!shadps4_write_guest(params_phys - 16, &value, sizeof(value),
                             "initial stack argv", errp)) {
        return false;
    }
    sms->hle.argv_guest_addr = params_va - 16;
    *params = params_va;
    return true;
}

static uint64_t shadps4_find_export(const ShadPS4ImageInfo *image,
                                    const char *nid)
{
    uint32_t i;

    for (i = 0; i < image->symbol_count; i++) {
        const ShadPS4DynamicSymbol *symbol = &image->symbols[i];

        if (symbol->defined && symbol->type == SHADPS4_ELF_STT_FUNC &&
            symbol->nid && !strcmp(symbol->nid, nid) &&
            symbol->value <= UINT64_MAX - image->virtual_base) {
            return image->virtual_base + symbol->value;
        }
    }
    return 0;
}

static bool shadps4_build_entry_bootstrap(ShadPS4MachineState *sms,
                                          uint64_t params, uint64_t *entry,
                                          Error **errp)
{
    uint64_t malloc_init = 0;
    uint64_t libc_init = 0;
    uint64_t offset = sms->hle_image.symbol_count * 16 +
                      SHADPS4_HLE_TRAILER_SIZE;
    uint8_t stub[SHADPS4_HLE_BOOTSTRAP_SLOT_SIZE];
    size_t pos = 0;
    uint32_t i;

    memset(stub, 0x90, sizeof(stub));

    for (i = 0; i < sms->module_count; i++) {
        if (!malloc_init) {
            malloc_init = shadps4_find_export(&sms->modules[i],
                                               "z8GPiQwaAEY");
        }
        if (!libc_init && sms->modules[i].init_present &&
            (strstr(sms->modules[i].name ?: "", "libc") ||
             strstr(sms->modules[i].name ?: "", "LibcInternal"))) {
            libc_init = sms->modules[i].init;
        }
    }
    if (!malloc_init) {
        *entry = sms->image.entry;
        return true;
    }

    /* Match shadPS4: start libc, initialize its heap, then enter eboot. */
    stub[pos++] = 0x48; stub[pos++] = 0x83; stub[pos++] = 0xec; stub[pos++] = 0x08;
    if (libc_init) {
        stub[pos++] = 0x31; stub[pos++] = 0xff;
        stub[pos++] = 0x31; stub[pos++] = 0xf6;
        stub[pos++] = 0x31; stub[pos++] = 0xd2;
        stub[pos++] = 0x48; stub[pos++] = 0xb8;
        stq_le_p(stub + pos, libc_init); pos += sizeof(uint64_t);
        stub[pos++] = 0xff; stub[pos++] = 0xd0;
    }
    stub[pos++] = 0x48; stub[pos++] = 0xb8;
    stq_le_p(stub + pos, malloc_init); pos += sizeof(uint64_t);
    stub[pos++] = 0xff; stub[pos++] = 0xd0;
    stub[pos++] = 0x48; stub[pos++] = 0x83; stub[pos++] = 0xc4; stub[pos++] = 0x08;
    stub[pos++] = 0x48; stub[pos++] = 0xbf;
    stq_le_p(stub + pos, params); pos += sizeof(uint64_t);
    stub[pos++] = 0x48; stub[pos++] = 0xbe;
    stq_le_p(stub + pos, SHADPS4_EXIT_STUB_VIRT); pos += sizeof(uint64_t);
    stub[pos++] = 0x48; stub[pos++] = 0xb8;
    stq_le_p(stub + pos, sms->image.entry); pos += sizeof(uint64_t);
    stub[pos++] = 0xff; stub[pos++] = 0xe0;

    if (pos > sizeof(stub) ||
        !shadps4_write_guest(sms->hle_image.physical_base + offset,
                             stub, sizeof(stub), "libc entry bootstrap", errp)) {
        return false;
    }
    *entry = sms->hle_image.virtual_base + offset;
    return true;
}

static bool shadps4_build_ps2_compiler_bootstrap(ShadPS4MachineState *sms,
                                                  Error **errp)
{
    uint64_t malloc_init = 0;
    uint64_t libc_init = 0;
    uint64_t offset;
    uint8_t stub[SHADPS4_HLE_BOOTSTRAP_SLOT_SIZE];
    size_t pos = 0;
    uint32_t i;

    if (!sms->ps2_compiler_image_entry) {
        return true;
    }
    for (i = 0; i < sms->module_count; i++) {
        if (!malloc_init) {
            malloc_init = shadps4_find_export(&sms->modules[i],
                                               "z8GPiQwaAEY");
        }
        if (!libc_init && sms->modules[i].init_present &&
            (strstr(sms->modules[i].name ?: "", "libc") ||
             strstr(sms->modules[i].name ?: "", "LibcInternal"))) {
            libc_init = sms->modules[i].init;
        }
    }
    if (!malloc_init) {
        sms->hle.ps2_compiler_entry = sms->ps2_compiler_image_entry;
        return true;
    }

    memset(stub, 0x90, sizeof(stub));
    /* Preserve EntryParams and keep the SysV stack aligned across calls. */
    stub[pos++] = 0x57;
    if (libc_init) {
        stub[pos++] = 0x31; stub[pos++] = 0xff;
        stub[pos++] = 0x31; stub[pos++] = 0xf6;
        stub[pos++] = 0x31; stub[pos++] = 0xd2;
        stub[pos++] = 0x48; stub[pos++] = 0xb8;
        stq_le_p(stub + pos, libc_init); pos += sizeof(uint64_t);
        stub[pos++] = 0xff; stub[pos++] = 0xd0;
    }
    stub[pos++] = 0x48; stub[pos++] = 0xb8;
    stq_le_p(stub + pos, malloc_init); pos += sizeof(uint64_t);
    stub[pos++] = 0xff; stub[pos++] = 0xd0;
    stub[pos++] = 0x5f;
    stub[pos++] = 0x48; stub[pos++] = 0xbe;
    stq_le_p(stub + pos, SHADPS4_EXIT_STUB_VIRT); pos += sizeof(uint64_t);
    stub[pos++] = 0x48; stub[pos++] = 0xb8;
    stq_le_p(stub + pos, sms->ps2_compiler_image_entry);
    pos += sizeof(uint64_t);
    stub[pos++] = 0xff; stub[pos++] = 0xe0;

    offset = sms->hle_image.symbol_count * 16 +
             SHADPS4_HLE_TRAILER_SIZE + SHADPS4_HLE_BOOTSTRAP_SLOT_SIZE;
    if (pos > sizeof(stub) ||
        !shadps4_write_guest(sms->hle_image.physical_base + offset,
                             stub, sizeof(stub),
                             "PS2 compiler libc bootstrap", errp)) {
        return false;
    }
    sms->hle.ps2_compiler_entry = sms->hle_image.virtual_base + offset;
    return true;
}

static bool shadps4_prepare_boot_cpu(ShadPS4MachineState *sms, Error **errp)
{
    CPUState *cs = first_cpu;
    X86CPU *cpu;
    CPUX86State *env;
    uint32_t code_flags;
    uint32_t data_flags;
    uint64_t gs_base;
    uint64_t rsp;
    uint64_t params;
    uint64_t entry;
    uint64_t dynamic_phys_base;
    uint32_t unresolved = shadps4_unresolved_relocations(sms);

    if (!cs) {
        error_setg(errp, "shadPS4 machine has no bootstrap CPU");
        return false;
    }
    if (unresolved) {
        shadps4_report_unresolved(sms);
        error_setg(errp, "image has %u unresolved relocations",
                   unresolved);
        return false;
    }
    if (!shadps4_build_page_tables(sms, errp)) {
        return false;
    }
    if (sms->mapped_image_size >
        UINT64_MAX - sms->image.physical_base - (2 * MiB - 1)) {
        error_setg(errp, "loaded image end address overflows");
        return false;
    }
    if (!shadps4_align_up_valid(
            sms->image.physical_base + sms->mapped_image_size,
            2 * MiB, &dynamic_phys_base)) {
        error_setg(errp, "dynamic memory base overflows");
        return false;
    }
    dynamic_phys_base = MAX(SHADPS4_DYNAMIC_PHYS_MIN, dynamic_phys_base);
    if (!shadps4_build_ps2_compiler_bootstrap(sms, errp)) {
        return false;
    }
    if (!shadps4_build_ps2_process_page_tables(
            sms, dynamic_phys_base, errp)) {
        return false;
    }
    if (sms->ps2_compiler_cr3) {
        uint64_t private_size = QEMU_ALIGN_UP(sms->mapped_image_size,
                                              2 * MiB);

        if (private_size > MACHINE(sms)->ram_size - dynamic_phys_base) {
            error_setg(errp, "PS2 compiler private image consumes guest RAM");
            return false;
        }
        dynamic_phys_base += private_size;
    }
    if (!shadps4_hle_reset(&sms->hle, dynamic_phys_base,
                           MACHINE(sms)->ram_size, errp)) {
        return false;
    }

    cpu = X86_CPU(cs);
    env = &cpu->env;
    if (!shadps4_build_descriptor_tables(env, errp) ||
        !shadps4_build_gateway(errp) ||
        !shadps4_build_tls(sms, &gs_base, errp) ||
        !shadps4_build_initial_stack(sms, &rsp, &params, errp) ||
        !shadps4_build_entry_bootstrap(sms, params, &entry, errp)) {
        return false;
    }
    env->xcr0 = XSTATE_FP_MASK | XSTATE_SSE_MASK | XSTATE_YMM_MASK;
    cpu_x86_update_cr4(env, CR4_PAE_MASK | CR4_PSE_MASK |
                       CR4_OSFXSR_MASK | CR4_OSXMMEXCPT_MASK |
                       CR4_OSXSAVE_MASK);
    env->efer = MSR_EFER_LME | MSR_EFER_NXE | MSR_EFER_SCE;
    cpu_x86_update_cr3(env, SHADPS4_PML4_PHYS);
    cpu_x86_update_cr0(env, env->cr[0] | CR0_PE_MASK | CR0_NE_MASK |
                       CR0_WP_MASK | CR0_PG_MASK);

    env->star = (0x10ULL << 48) | (0x08ULL << 32);
    env->lstar = SHADPS4_SYSCALL_STUB_PHYS;
    env->cstar = SHADPS4_SYSCALL_STUB_PHYS;
    env->fmask = IF_MASK | TF_MASK | DF_MASK;

    code_flags = DESC_P_MASK | DESC_S_MASK | DESC_CS_MASK | DESC_R_MASK |
                 DESC_A_MASK | DESC_L_MASK | DESC_G_MASK | DESC_DPL_MASK;
    data_flags = DESC_P_MASK | DESC_S_MASK | DESC_W_MASK | DESC_A_MASK |
                 DESC_B_MASK | DESC_G_MASK | DESC_DPL_MASK;
    cpu_x86_load_seg_cache(env, R_CS, 0x23, 0, UINT32_MAX, code_flags);
    cpu_x86_load_seg_cache(env, R_SS, 0x1b, 0, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_DS, 0x1b, 0, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_ES, 0x1b, 0, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_FS, 0, gs_base, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_GS, 0, gs_base, UINT32_MAX, data_flags);

    memset(env->regs, 0, sizeof(env->regs));
    env->eip = entry;
    env->regs[R_ESP] = rsp;
    env->regs[R_EDI] = params;
    env->regs[R_ESI] = SHADPS4_EXIT_STUB_VIRT;
    env->eflags = 0x2 | IF_MASK;
    cs->exception_index = -1;
    cs->halted = 0;
    cpu_reset_interrupt(cs, CPU_INTERRUPT_HALT);
    info_report("shadPS4 user CPU ready: rip=0x%" PRIx64
                " rsp=0x%" PRIx64 " tls=0x%" PRIx64 " cr3=0x%x",
                env->eip, env->regs[R_ESP], gs_base, SHADPS4_PML4_PHYS);
    return true;
}

static ShadPS4GuestThreadContext *shadps4_thread_context(
    ShadPS4MachineState *sms, uint64_t handle)
{
    if (handle <= 0x400 ||
        handle - 0x400 >= ARRAY_SIZE(sms->thread_contexts)) {
        return NULL;
    }
    return &sms->thread_contexts[handle - 0x400];
}

static void shadps4_restore_guest_eflags(CPUX86State *env, uint32_t eflags)
{
    const uint32_t cc_mask = CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C;

    env->cc_src = eflags & cc_mask;
    env->cc_dst = 0;
    env->cc_src2 = 0;
    env->cc_op = CC_OP_EFLAGS;
    env->df = eflags & DF_MASK ? -1 : 1;
    env->eflags = (eflags & ~(cc_mask | DF_MASK)) | 0x2;
}

static void shadps4_save_guest_thread(ShadPS4MachineState *sms,
                                      CPUState *cs, uint64_t handle,
                                      uint64_t result)
{
    ShadPS4GuestThreadContext *context = shadps4_thread_context(sms, handle);
    CPUX86State *env = &X86_CPU(cs)->env;

    if (!context) {
        return;
    }
    memcpy(context->regs, env->regs, sizeof(context->regs));
    context->regs[R_EAX] = result;
    context->eip = env->regs[R_ECX];
    context->eflags = env->regs[R_R11];
    context->fs_base = env->segs[R_FS].base;
    context->gs_base = env->segs[R_GS].base;
    context->cr3 = env->cr[3];
    memcpy(context->xmm, env->xmm_regs, sizeof(context->xmm));
    memcpy(context->fpregs, env->fpregs, sizeof(context->fpregs));
    memcpy(context->fptags, env->fptags, sizeof(context->fptags));
    memcpy(context->bnd_regs, env->bnd_regs, sizeof(context->bnd_regs));
    memcpy(context->bndcs_regs, &env->bndcs_regs,
           sizeof(context->bndcs_regs));
    memcpy(context->opmask_regs, env->opmask_regs,
           sizeof(context->opmask_regs));
    context->mxcsr = env->mxcsr;
    context->fpuc = env->fpuc;
    context->fpus = env->fpus;
    context->fpstt = env->fpstt;
    context->schedule_order = ++sms->scheduler_order;
    context->valid = true;
    context->runnable = true;
}

static uint64_t shadps4_pick_guest_thread(ShadPS4MachineState *sms,
                                          uint32_t cpu_index,
                                          uint64_t exclude)
{
    uint64_t selected = 0;
    uint64_t selected_order = UINT64_MAX;
    uint32_t selected_priority = UINT32_MAX;

    for (uint32_t slot = 1; slot < ARRAY_SIZE(sms->thread_contexts); slot++) {
        ShadPS4GuestThreadContext *context = &sms->thread_contexts[slot];
        ShadPS4HLEPthreadAttr *attr = &sms->hle.pthread_attrs[slot];
        uint32_t priority;

        if (0x400 + slot == exclude || !context->valid || !context->runnable ||
            !(attr->affinity & BIT(cpu_index))) {
            continue;
        }
        priority = attr->priority ?: 700;
        if (!selected || priority < selected_priority ||
            (priority == selected_priority &&
             context->schedule_order < selected_order)) {
            selected = 0x400 + slot;
            selected_priority = priority;
            selected_order = context->schedule_order;
        }
    }
    return selected;
}

static bool shadps4_schedule_guest_thread(ShadPS4MachineState *sms,
                                          CPUState *cs, bool requeue,
                                          uint64_t result)
{
    uint32_t cpu_index = cs->cpu_index;
    uint64_t current;
    uint64_t next;

    if (!cpu_index || cpu_index >= ARRAY_SIZE(sms->pending_thread_switch)) {
        return false;
    }
    qemu_mutex_lock(&sms->scheduler_lock);
    current = sms->hle.cpu_thread_handles[cpu_index];
    if (requeue) {
        shadps4_save_guest_thread(sms, cs, current, result);
    }
    next = shadps4_pick_guest_thread(sms, cpu_index, current);
    if (!next || next == current) {
        ShadPS4GuestThreadContext *context =
            shadps4_thread_context(sms, current);

        if (context) {
            context->runnable = false;
        }
        qemu_mutex_unlock(&sms->scheduler_lock);
        return false;
    }
    sms->thread_contexts[next - 0x400].runnable = false;
    sms->pending_thread_switch[cpu_index] = next;
    qemu_mutex_unlock(&sms->scheduler_lock);
    return true;
}

static bool shadps4_block_guest_thread(ShadPS4MachineState *sms,
                                        CPUState *cs, uint64_t result)
{
    uint32_t cpu_index = cs->cpu_index;
    uint64_t current;
    uint64_t next;
    ShadPS4GuestThreadContext *context;

    if (!cpu_index || cpu_index >= ARRAY_SIZE(sms->pending_thread_switch)) {
        return false;
    }
    qemu_mutex_lock(&sms->scheduler_lock);
    current = sms->hle.cpu_thread_handles[cpu_index];
    shadps4_save_guest_thread(sms, cs, current, result);
    context = shadps4_thread_context(sms, current);
    if (context) {
        context->runnable = false;
    }
    next = shadps4_pick_guest_thread(sms, cpu_index, current);
    if (!next) {
        /* Restore this saved user context, but keep it parked until wake. */
        sms->pending_thread_switch[cpu_index] = current;
        qemu_mutex_unlock(&sms->scheduler_lock);
        return false;
    }
    sms->thread_contexts[next - 0x400].runnable = false;
    sms->pending_thread_switch[cpu_index] = next;
    cs->halted = 0;
    cpu_reset_interrupt(cs, CPU_INTERRUPT_HALT);
    qemu_mutex_unlock(&sms->scheduler_lock);
    return true;
}

static bool shadps4_wake_guest_thread(void *opaque, uint32_t cpu_index,
                                      uint64_t handle, uint64_t result)
{
    ShadPS4MachineState *sms = opaque;
    ShadPS4GuestThreadContext *context;

    qemu_mutex_lock(&sms->scheduler_lock);
    if (cpu_index < ARRAY_SIZE(sms->hle.cpu_thread_handles) &&
        sms->hle.cpu_thread_handles[cpu_index] == handle) {
        qemu_mutex_unlock(&sms->scheduler_lock);
        return false;
    }
    context = shadps4_thread_context(sms, handle);
    if (!context || !context->valid) {
        qemu_mutex_unlock(&sms->scheduler_lock);
        return false;
    }
    context->regs[R_EAX] = result;
    context->schedule_order = ++sms->scheduler_order;
    context->runnable = true;
    qemu_mutex_unlock(&sms->scheduler_lock);
    return true;
}

static bool shadps4_kill_guest_thread(void *opaque, uint64_t handle,
                                      uint64_t result)
{
    ShadPS4MachineState *sms = opaque;
    ShadPS4GuestThreadContext *context =
        shadps4_thread_context(sms, handle);
    CPUState *cs;

    qemu_mutex_lock(&sms->scheduler_lock);
    if (!context || !context->valid) {
        qemu_mutex_unlock(&sms->scheduler_lock);
        return false;
    }
    memset(context, 0, sizeof(*context));
    qemu_mutex_unlock(&sms->scheduler_lock);
    CPU_FOREACH(cs) {
        bool running;

        qemu_mutex_lock(&sms->scheduler_lock);
        running = cs->cpu_index <
                      ARRAY_SIZE(sms->hle.cpu_thread_handles) &&
                  sms->hle.cpu_thread_handles[cs->cpu_index] == handle;
        if (running) {
            sms->hle.cpu_thread_handles[cs->cpu_index] = 0;
        }
        qemu_mutex_unlock(&sms->scheduler_lock);
        if (!running) {
            continue;
        }
        if (!shadps4_schedule_guest_thread(sms, cs, false, result)) {
            cpu_interrupt(cs, CPU_INTERRUPT_HALT);
        }
        qemu_cpu_kick(cs);
    }
    info_report("shadPS4 guest thread killed: handle=%#" PRIx64
                " result=%#" PRIx64, handle, result);
    return true;
}

static void shadps4_preempt_guest_thread(CPUState *cs,
                                         run_on_cpu_data data)
{
    ShadPS4MachineState *sms = data.host_ptr;
    uint32_t cpu_index = cs->cpu_index;
    uint64_t current;
    uint64_t next;
    ShadPS4GuestThreadContext *current_context;
    ShadPS4GuestThreadContext *next_context;
    CPUX86State *env;
    uint32_t data_flags = DESC_P_MASK | DESC_S_MASK | DESC_W_MASK |
        DESC_A_MASK | DESC_B_MASK | DESC_G_MASK | DESC_DPL_MASK;

    if (!cpu_index || cpu_index >= ARRAY_SIZE(sms->pending_thread_switch)) {
        return;
    }
    qemu_mutex_lock(&sms->scheduler_lock);
    if (sms->pending_thread_switch[cpu_index] ||
        sms->hle_gateway_active[cpu_index] ||
        sms->hle.cpu_pending_wait_thread[cpu_index]) {
        qemu_mutex_unlock(&sms->scheduler_lock);
        return;
    }
    current = sms->hle.cpu_thread_handles[cpu_index];
    if (current > 0x400 &&
        current - 0x400 < ARRAY_SIZE(sms->hle.wait_kind) &&
        sms->hle.wait_kind[current - 0x400] != SHADPS4_HLE_WAIT_NONE) {
        qemu_mutex_unlock(&sms->scheduler_lock);
        return;
    }
    current_context = shadps4_thread_context(sms, current);
    if (!current_context) {
        qemu_mutex_unlock(&sms->scheduler_lock);
        return;
    }
    env = &X86_CPU(cs)->env;
    if ((env->hflags & HF_CPL_MASK) != 3) {
        qemu_mutex_unlock(&sms->scheduler_lock);
        return;
    }
    memcpy(current_context->regs, env->regs, sizeof(current_context->regs));
    current_context->eip = env->eip;
    current_context->eflags = cpu_compute_eflags(env);
    current_context->fs_base = env->segs[R_FS].base;
    current_context->gs_base = env->segs[R_GS].base;
    current_context->cr3 = env->cr[3];
    memcpy(current_context->xmm, env->xmm_regs,
           sizeof(current_context->xmm));
    memcpy(current_context->fpregs, env->fpregs,
           sizeof(current_context->fpregs));
    memcpy(current_context->fptags, env->fptags,
           sizeof(current_context->fptags));
    memcpy(current_context->bnd_regs, env->bnd_regs,
           sizeof(current_context->bnd_regs));
    memcpy(current_context->bndcs_regs, &env->bndcs_regs,
           sizeof(current_context->bndcs_regs));
    memcpy(current_context->opmask_regs, env->opmask_regs,
           sizeof(current_context->opmask_regs));
    current_context->mxcsr = env->mxcsr;
    current_context->fpuc = env->fpuc;
    current_context->fpus = env->fpus;
    current_context->fpstt = env->fpstt;
    current_context->schedule_order = ++sms->scheduler_order;
    current_context->valid = true;
    current_context->runnable = true;
    next = shadps4_pick_guest_thread(sms, cpu_index, current);
    if (!next) {
        current_context->runnable = false;
        qemu_mutex_unlock(&sms->scheduler_lock);
        return;
    }
    next_context = shadps4_thread_context(sms, next);
    next_context->runnable = false;
    memcpy(env->regs, next_context->regs, sizeof(next_context->regs));
    env->eip = next_context->eip;
    shadps4_restore_guest_eflags(env, next_context->eflags);
    cpu_x86_load_seg_cache(env, R_FS, 0, next_context->fs_base,
                           UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_GS, 0, next_context->gs_base,
                           UINT32_MAX, data_flags);
    cpu_x86_update_cr3(env, next_context->cr3 ?: SHADPS4_PML4_PHYS);
    memcpy(env->xmm_regs, next_context->xmm, sizeof(next_context->xmm));
    memcpy(env->fpregs, next_context->fpregs, sizeof(next_context->fpregs));
    memcpy(env->fptags, next_context->fptags, sizeof(next_context->fptags));
    memcpy(env->bnd_regs, next_context->bnd_regs,
           sizeof(next_context->bnd_regs));
    memcpy(&env->bndcs_regs, next_context->bndcs_regs,
           sizeof(next_context->bndcs_regs));
    memcpy(env->opmask_regs, next_context->opmask_regs,
           sizeof(next_context->opmask_regs));
    cpu_set_mxcsr(env, next_context->mxcsr);
    cpu_set_fpuc(env, next_context->fpuc);
    env->fpus = next_context->fpus;
    env->fpstt = next_context->fpstt;
    sms->hle.cpu_thread_handles[cpu_index] = next;
    cs->halted = 0;
    cpu_reset_interrupt(cs, CPU_INTERRUPT_HALT);
    tlb_flush(cs);
    sms->scheduler_switches++;
    if (!(sms->scheduler_switches & (sms->scheduler_switches - 1))) {
        info_report("shadPS4 scheduler: switches=%" PRIu64
                    " cpu=%u thread=%#" PRIx64 "->%#" PRIx64,
                    sms->scheduler_switches, cpu_index, current, next);
    }
    qemu_mutex_unlock(&sms->scheduler_lock);
}

static void shadps4_scheduler_tick(void *opaque)
{
    ShadPS4MachineState *sms = opaque;
    CPUState *cs;

    if (runstate_is_running() &&
        !g_getenv("SHADPS4_DISABLE_PREEMPTION")) {
        CPU_FOREACH(cs) {
            if (cs->cpu_index) {
                async_run_on_cpu(cs, shadps4_preempt_guest_thread,
                                 RUN_ON_CPU_HOST_PTR(sms));
            }
        }
    }
    timer_mod(sms->scheduler_timer,
              qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
              SHADPS4_SCHEDULER_QUANTUM_NS);
}

static bool shadps4_apply_guest_thread_switch(ShadPS4MachineState *sms,
                                               CPUState *cs)
{
    uint32_t cpu_index = cs->cpu_index;
    uint64_t handle;
    ShadPS4GuestThreadContext *context;
    CPUX86State *env;
    uint32_t code_flags = DESC_P_MASK | DESC_S_MASK | DESC_CS_MASK |
        DESC_R_MASK | DESC_A_MASK | DESC_L_MASK | DESC_G_MASK | DESC_DPL_MASK;
    uint32_t data_flags = DESC_P_MASK | DESC_S_MASK | DESC_W_MASK |
        DESC_A_MASK | DESC_B_MASK | DESC_G_MASK | DESC_DPL_MASK;

    if (cpu_index >= ARRAY_SIZE(sms->pending_thread_switch)) {
        return false;
    }
    qemu_mutex_lock(&sms->scheduler_lock);
    handle = sms->pending_thread_switch[cpu_index];
    if (!handle) {
        qemu_mutex_unlock(&sms->scheduler_lock);
        return false;
    }
    sms->pending_thread_switch[cpu_index] = 0;
    context = shadps4_thread_context(sms, handle);
    if (!context || !context->valid) {
        qemu_mutex_unlock(&sms->scheduler_lock);
        return false;
    }
    env = &X86_CPU(cs)->env;
    memcpy(env->regs, context->regs, sizeof(context->regs));
    env->eip = context->eip;
    shadps4_restore_guest_eflags(env, context->eflags);
    cpu_x86_load_seg_cache(env, R_CS, 0x23, 0, UINT32_MAX, code_flags);
    cpu_x86_load_seg_cache(env, R_SS, 0x1b, 0, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_DS, 0x1b, 0, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_ES, 0x1b, 0, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_FS, 0, context->fs_base,
                           UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_GS, 0, context->gs_base,
                           UINT32_MAX, data_flags);
    cpu_x86_update_cr3(env, context->cr3 ?: SHADPS4_PML4_PHYS);
    memcpy(env->xmm_regs, context->xmm, sizeof(context->xmm));
    memcpy(env->fpregs, context->fpregs, sizeof(context->fpregs));
    memcpy(env->fptags, context->fptags, sizeof(context->fptags));
    memcpy(env->bnd_regs, context->bnd_regs, sizeof(context->bnd_regs));
    memcpy(&env->bndcs_regs, context->bndcs_regs,
           sizeof(context->bndcs_regs));
    memcpy(env->opmask_regs, context->opmask_regs,
           sizeof(context->opmask_regs));
    cpu_set_mxcsr(env, context->mxcsr);
    cpu_set_fpuc(env, context->fpuc);
    env->fpus = context->fpus;
    env->fpstt = context->fpstt;
    sms->hle.cpu_thread_handles[cpu_index] = handle;
    cs->halted = 0;
    cpu_reset_interrupt(cs, CPU_INTERRUPT_HALT);
    tlb_flush(cs);
    qemu_mutex_unlock(&sms->scheduler_lock);
    return true;
}

static int shadps4_start_guest_thread(void *opaque, uint64_t handle,
                                      uint64_t entry, uint64_t argument,
                                      uint64_t stack_pointer,
                                      uint64_t gs_base)
{
    ShadPS4MachineState *sms = opaque;
    CPUState *cs;
    CPUX86State *env;
    uint32_t code_flags = DESC_P_MASK | DESC_S_MASK | DESC_CS_MASK |
        DESC_R_MASK | DESC_A_MASK | DESC_L_MASK | DESC_G_MASK | DESC_DPL_MASK;
    uint32_t data_flags = DESC_P_MASK | DESC_S_MASK | DESC_W_MASK |
        DESC_A_MASK | DESC_B_MASK | DESC_G_MASK | DESC_DPL_MASK;
    Error *local_err = NULL;

    qemu_mutex_lock(&sms->scheduler_lock);
    CPU_FOREACH(cs) {
        if (cs->cpu_index &&
            !sms->hle.cpu_thread_handles[cs->cpu_index]) {
            break;
        }
    }
    if (!cs) {
        ShadPS4GuestThreadContext *context =
            shadps4_thread_context(sms, handle);

        if (!context) {
            qemu_mutex_unlock(&sms->scheduler_lock);
            return -EAGAIN;
        }
        memset(context, 0, sizeof(*context));
        context->regs[R_ESP] = stack_pointer;
        context->regs[R_EDI] = argument;
        if (sms->hle.thread_process_ids[handle - 0x400] == 2) {
            context->regs[R_ESI] = SHADPS4_EXIT_STUB_VIRT;
        }
        context->eip = entry;
        context->eflags = 0x2 | IF_MASK;
        context->fs_base = gs_base;
        context->gs_base = gs_base;
        context->cr3 = sms->hle.thread_process_ids[handle - 0x400] == 2 ?
                       sms->ps2_compiler_cr3 : SHADPS4_PML4_PHYS;
        context->mxcsr = 0x1f80;
        context->fpuc = 0x37f;
        context->schedule_order = ++sms->scheduler_order;
        context->valid = true;
        context->runnable = true;
        info_report("shadPS4 guest thread queued: handle=%#" PRIx64
                    " entry=%#" PRIx64 " stack=%#" PRIx64
                    " priority=%d affinity=%#" PRIx64,
                    handle, entry, stack_pointer,
                    sms->hle.pthread_attrs[handle - 0x400].priority,
                    sms->hle.pthread_attrs[handle - 0x400].affinity);
        qemu_mutex_unlock(&sms->scheduler_lock);
        return 0;
    }
    env = &X86_CPU(cs)->env;
    if (!shadps4_build_descriptor_tables(env, &local_err)) {
        error_report_err(local_err);
        qemu_mutex_unlock(&sms->scheduler_lock);
        return -EINVAL;
    }
    env->xcr0 = XSTATE_FP_MASK | XSTATE_SSE_MASK | XSTATE_YMM_MASK;
    cpu_x86_update_cr4(env, CR4_PAE_MASK | CR4_PSE_MASK |
                       CR4_OSFXSR_MASK | CR4_OSXMMEXCPT_MASK |
                       CR4_OSXSAVE_MASK);
    env->efer = MSR_EFER_LME | MSR_EFER_NXE | MSR_EFER_SCE;
    cpu_x86_update_cr3(
        env, sms->hle.thread_process_ids[handle - 0x400] == 2 ?
             sms->ps2_compiler_cr3 : SHADPS4_PML4_PHYS);
    cpu_x86_update_cr0(env, env->cr[0] | CR0_PE_MASK | CR0_NE_MASK |
                       CR0_WP_MASK | CR0_PG_MASK);
    env->star = (0x10ULL << 48) | (0x08ULL << 32);
    env->lstar = SHADPS4_SYSCALL_STUB_PHYS;
    env->cstar = SHADPS4_SYSCALL_STUB_PHYS;
    env->fmask = IF_MASK | TF_MASK | DF_MASK;
    cpu_x86_load_seg_cache(env, R_CS, 0x23, 0, UINT32_MAX, code_flags);
    cpu_x86_load_seg_cache(env, R_SS, 0x1b, 0, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_DS, 0x1b, 0, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_ES, 0x1b, 0, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_FS, 0, gs_base, UINT32_MAX, data_flags);
    cpu_x86_load_seg_cache(env, R_GS, 0, gs_base, UINT32_MAX, data_flags);
    memset(env->regs, 0, sizeof(env->regs));
    env->eip = entry;
    env->regs[R_ESP] = stack_pointer;
    env->regs[R_EDI] = argument;
    if (sms->hle.thread_process_ids[handle - 0x400] == 2) {
        env->regs[R_ESI] = SHADPS4_EXIT_STUB_VIRT;
    }
    env->eflags = 0x2 | IF_MASK;
    cs->exception_index = -1;
    sms->hle.cpu_thread_handles[cs->cpu_index] = handle;
    memset(&sms->thread_contexts[handle - 0x400], 0,
           sizeof(sms->thread_contexts[handle - 0x400]));
    memcpy(sms->thread_contexts[handle - 0x400].regs, env->regs,
           sizeof(env->regs));
    sms->thread_contexts[handle - 0x400].eip = env->eip;
    sms->thread_contexts[handle - 0x400].eflags = cpu_compute_eflags(env);
    sms->thread_contexts[handle - 0x400].fs_base = gs_base;
    sms->thread_contexts[handle - 0x400].gs_base = gs_base;
    sms->thread_contexts[handle - 0x400].mxcsr = env->mxcsr;
    sms->thread_contexts[handle - 0x400].fpuc = env->fpuc;
    sms->thread_contexts[handle - 0x400].fpus = env->fpus;
    sms->thread_contexts[handle - 0x400].fpstt = env->fpstt;
    sms->thread_contexts[handle - 0x400].valid = true;
    cs->halted = 0;
    cpu_reset_interrupt(cs, CPU_INTERRUPT_HALT);
    tlb_flush(cs);
    qemu_mutex_unlock(&sms->scheduler_lock);
    qemu_cpu_kick(cs);
    info_report("shadPS4 guest thread started: handle=%#" PRIx64
                " cpu=%d entry=%#" PRIx64 " stack=%#" PRIx64
                " priority=%d affinity=%#" PRIx64,
                handle, cs->cpu_index, entry, stack_pointer,
                sms->hle.pthread_attrs[handle - 0x400].priority,
                sms->hle.pthread_attrs[handle - 0x400].affinity);
    return 0;
}

static void shadps4_exit_guest_thread(void *opaque, CPUState *cs,
                                      uint64_t handle, uint64_t result)
{
    ShadPS4MachineState *sms = opaque;
    ShadPS4GuestThreadContext *context =
        shadps4_thread_context(sms, handle);

    qemu_mutex_lock(&sms->scheduler_lock);
    if (context) {
        memset(context, 0, sizeof(*context));
    }
    qemu_mutex_unlock(&sms->scheduler_lock);
    if (!shadps4_schedule_guest_thread(sms, cs, false, result)) {
        cpu_interrupt(cs, CPU_INTERRUPT_HALT);
    }
    info_report("shadPS4 guest thread exited: handle=%#" PRIx64
                " cpu=%d result=%#" PRIx64,
                handle, cs->cpu_index, result);
}

static bool shadps4_machine_get_execute(Object *obj, Error **errp)
{
    return SHADPS4_MACHINE(obj)->execute;
}

static void shadps4_machine_set_execute(Object *obj, bool value, Error **errp)
{
    SHADPS4_MACHINE(obj)->execute = value;
}

static bool shadps4_configure_jaguar_cpu(Object *cpu, MachineState *machine,
                                         ShadPS4Variant variant, Error **errp)
{
    CPUX86State *env = &X86_CPU(cpu)->env;
    static const char *const features[] = {
        "pclmulqdq", "ssse3", "sse4.1", "sse4.2", "movbe", "popcnt",
        "aes", "xsave", "avx", "f16c", "abm", "sse4a", "bmi1", "ht",
    };
    const char *model_id = variant == SHADPS4_VARIANT_NEO ?
        "AMD Jaguar (PS4 Pro Neo)" : "AMD Jaguar (PS4 Liverpool)";
    size_t i;

    object_property_set_str(cpu, "vendor", CPUID_VENDOR_AMD, errp);
    if (errp && *errp) {
        return false;
    }
    object_property_set_int(cpu, "family", 16, errp);
    if (errp && *errp) {
        return false;
    }
    object_property_set_int(cpu, "model", 0, errp);
    if (errp && *errp) {
        return false;
    }
    object_property_set_int(cpu, "stepping", 1, errp);
    if (errp && *errp) {
        return false;
    }
    object_property_set_str(cpu, "model-id", model_id, errp);
    if (errp && *errp) {
        return false;
    }
    env->topo_info.dies_per_pkg = machine->smp.dies;
    env->topo_info.modules_per_die = machine->smp.modules;
    env->topo_info.cores_per_module = machine->smp.cores;
    env->topo_info.threads_per_core = machine->smp.threads;
    for (i = 0; i < ARRAY_SIZE(features); i++) {
        object_property_set_bool(cpu, features[i], true, errp);
        if (errp && *errp) {
            return false;
        }
    }
    return true;
}

static bool shadps4_cpus_init(X86MachineState *x86ms, Error **errp)
{
    MachineState *machine = MACHINE(x86ms);
    ShadPS4MachineState *sms = SHADPS4_MACHINE(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    const CPUArchIdList *possible_cpus;
    int i;

    x86_cpu_set_default_version(CPU_VERSION_LATEST);
    x86ms->apic_id_limit = x86_cpu_apic_id_from_index(
        x86ms, machine->smp.max_cpus - 1) + 1;
    apic_set_max_apic_id(x86ms->apic_id_limit);

    possible_cpus = mc->possible_cpu_arch_ids(machine);
    for (i = 0; i < machine->smp.cpus; i++) {
        Object *cpu = object_new(machine->cpu_type);

        if (!shadps4_configure_jaguar_cpu(cpu, machine, sms->variant,
                                          errp)) {
            object_unref(cpu);
            return false;
        }
        object_property_set_uint(cpu, "apic-id",
                                 possible_cpus->cpus[i].arch_id,
                                 errp);
        if ((errp && *errp) || !qdev_realize(DEVICE(cpu), NULL, errp)) {
            object_unref(cpu);
            return false;
        }
        object_unref(cpu);
    }
    return true;
}

static void shadps4_halt_cpus(void)
{
    CPUState *cs;

    CPU_FOREACH(cs) {
        cs->halted = 1;
    }
}

typedef struct ShadPS4HLEExport {
    const char *nid;
    const char *library;
    const char *module;
    uint32_t dispatch;
    uint8_t data_kind;
} ShadPS4HLEExport;

typedef enum ShadPS4HLEDataKind {
    SHADPS4_HLE_DATA_NONE,
    SHADPS4_HLE_DATA_STACK_CHK_GUARD,
    SHADPS4_HLE_DATA_ENVIRON,
    SHADPS4_HLE_DATA_PROGNAME,
    SHADPS4_HLE_DATA_SIGINTR,
} ShadPS4HLEDataKind;

#define SHADPS4_HLE_EXPORT(nid_, library_, dispatch_) \
    { (nid_), (library_), "libkernel", (dispatch_), SHADPS4_HLE_DATA_NONE }
#define SHADPS4_HLE_OBJECT(nid_, kind_) \
    { (nid_), "libkernel", "libkernel", 0, (kind_) }
#define SHADPS4_HLE_LIBC_EXPORT(nid_, dispatch_) \
    { (nid_), "libc", "libc", (dispatch_), SHADPS4_HLE_DATA_NONE }

static const ShadPS4HLEExport shadps4_hle_exports[] = {
    SHADPS4_HLE_LIBC_EXPORT("gQX+4GDQjpM", SHADPS4_HLE_LIBC_MALLOC),
    SHADPS4_HLE_LIBC_EXPORT("tIhsqj0qsFE", SHADPS4_HLE_LIBC_FREE),
    SHADPS4_HLE_LIBC_EXPORT("2X5agFjKxMc", SHADPS4_HLE_LIBC_CALLOC),
    SHADPS4_HLE_LIBC_EXPORT("Y7aJ1uydPMo", SHADPS4_HLE_LIBC_REALLOC),
    SHADPS4_HLE_LIBC_EXPORT("Ujf3KzMvRmI", SHADPS4_HLE_LIBC_MEMALIGN),
    SHADPS4_HLE_LIBC_EXPORT("cVSk9y8URbc", SHADPS4_HLE_LIBC_POSIX_MEMALIGN),
    SHADPS4_HLE_LIBC_EXPORT("NDcSfcYZRC8",
                            SHADPS4_HLE_LIBC_MALLOC_USABLE_SIZE),
    SHADPS4_HLE_EXPORT("6c3rCVE-fTU", "libkernel", 5),
    SHADPS4_HLE_EXPORT("wuCroIGjt2g", "libScePosix", 5),
    SHADPS4_HLE_EXPORT("wuCroIGjt2g", "libkernel", 5),
    SHADPS4_HLE_EXPORT("1G3lF1Gg1k8", "libkernel", 5),
    SHADPS4_HLE_EXPORT("NNtFaKJbPt0", "libkernel", 6),
    SHADPS4_HLE_EXPORT("bY-PO6JhzhQ", "libScePosix", 6),
    SHADPS4_HLE_EXPORT("bY-PO6JhzhQ", "libkernel", 6),
    SHADPS4_HLE_EXPORT("UK2Tl2DWUns", "libkernel", 6),
    SHADPS4_HLE_EXPORT("FxVZqBAA7ks", "libkernel", 4),
    SHADPS4_HLE_EXPORT("FN4gaPmuFV8", "libScePosix", 4),
    SHADPS4_HLE_EXPORT("FN4gaPmuFV8", "libkernel", 4),
    SHADPS4_HLE_EXPORT("4wSze92BhLI", "libkernel", 4),
    SHADPS4_HLE_EXPORT("DRuBt2pvICk", "libkernel", 3),
    SHADPS4_HLE_EXPORT("AqBioC2vF3I", "libScePosix", 3),
    SHADPS4_HLE_EXPORT("AqBioC2vF3I", "libkernel", 3),
    SHADPS4_HLE_EXPORT("Cg4srZ6TKbU", "libkernel", 3),
    SHADPS4_HLE_EXPORT("Oy6IpwgtYOk", "libScePosix", 478),
    SHADPS4_HLE_EXPORT("Oy6IpwgtYOk", "libkernel", 478),
    SHADPS4_HLE_EXPORT("oib76F-12fk", "libkernel", 478),
    SHADPS4_HLE_EXPORT("JGMio+21L4c", "libScePosix", 136),
    SHADPS4_HLE_EXPORT("JGMio+21L4c", "libkernel", 136),
    SHADPS4_HLE_EXPORT("1-LFLmRFxxM", "libkernel", 136),
    SHADPS4_HLE_EXPORT("E6ao34wPw+U", "libScePosix", 188),
    SHADPS4_HLE_EXPORT("E6ao34wPw+U", "libkernel", 188),
    SHADPS4_HLE_EXPORT("eV9wAD2riIA", "libkernel", 188),
    SHADPS4_HLE_EXPORT("mqQMh1zPPT8", "libScePosix", 189),
    SHADPS4_HLE_EXPORT("mqQMh1zPPT8", "libkernel", 189),
    SHADPS4_HLE_EXPORT("kBwCPsYX-m4", "libkernel", 189),
    SHADPS4_HLE_EXPORT("NN01qLRhiqU", "libScePosix", 128),
    SHADPS4_HLE_EXPORT("NN01qLRhiqU", "libkernel", 128),
    SHADPS4_HLE_EXPORT("52NcYU9+lEo", "libkernel", 128),
    SHADPS4_HLE_EXPORT("juWbTNM+8hw", "libScePosix", 95),
    SHADPS4_HLE_EXPORT("juWbTNM+8hw", "libkernel", 95),
    SHADPS4_HLE_EXPORT("fTx66l5iWIA", "libkernel", 95),
    SHADPS4_HLE_EXPORT("VAzswvTOCzI", "libkernel", 10),
    SHADPS4_HLE_EXPORT("AUXVxWeJU-A", "libkernel", 10),
    SHADPS4_HLE_EXPORT("sfKygSjIbI8", "libkernel", 196),
    SHADPS4_HLE_EXPORT("2G6i6hMIUUY", "libkernel", 196),
    SHADPS4_HLE_EXPORT("taRWhTJFTgE", "libkernel", 196),
    SHADPS4_HLE_EXPORT("BPE9s9vQQXo", "libkernel", 477),
    SHADPS4_HLE_EXPORT("BPE9s9vQQXo", "libScePosix", 477),
    SHADPS4_HLE_EXPORT("UqDGjXA5yUM", "libkernel", 73),
    SHADPS4_HLE_EXPORT("UqDGjXA5yUM", "libScePosix", 73),
    SHADPS4_HLE_EXPORT("cQke9UuBQOk", "libkernel", 73),
    SHADPS4_HLE_EXPORT("nh2IFMgKTv8", "libScePosix", 362),
    SHADPS4_HLE_EXPORT("RW-GEfpnsqg", "libScePosix", 363),
    SHADPS4_HLE_EXPORT("lLMT9vJAck0", "libkernel", 232),
    SHADPS4_HLE_EXPORT("lLMT9vJAck0", "libScePosix", 232),
    SHADPS4_HLE_EXPORT("QBi7HCK03hw", "libkernel", 232),
    SHADPS4_HLE_EXPORT("n88vx3C5nW8", "libkernel", 116),
    SHADPS4_HLE_EXPORT("n88vx3C5nW8", "libScePosix", 116),
    SHADPS4_HLE_EXPORT("ejekcaNQNq0", "libkernel", 116),
    SHADPS4_HLE_EXPORT("TU-d9PfIHPM", "libkernel", 97),
    SHADPS4_HLE_EXPORT("TU-d9PfIHPM", "libScePosix", 97),
    SHADPS4_HLE_EXPORT("XVL8So3QJUk", "libkernel", 98),
    SHADPS4_HLE_EXPORT("XVL8So3QJUk", "libScePosix", 98),
    SHADPS4_HLE_EXPORT("6Z83sYWFlA8", "libkernel", 1),
    SHADPS4_HLE_EXPORT("kg4x8Prhfxw", "libkernel", 24),
    SHADPS4_HLE_EXPORT("vNe1w4diLCs", "libkernel",
                       SHADPS4_HLE_TLS_GET_ADDR),
    SHADPS4_HLE_EXPORT("4J2sUJmuHZQ", "libkernel",
                       SHADPS4_HLE_GET_PROCESS_TIME),
    SHADPS4_HLE_EXPORT("fgxnMeTNUtY", "libkernel",
                       SHADPS4_HLE_GET_PROCESS_TIME_COUNTER),
    SHADPS4_HLE_EXPORT("BNowx2l588E", "libkernel",
                       SHADPS4_HLE_GET_PROCESS_TIME_COUNTER_FREQUENCY),
    SHADPS4_HLE_EXPORT("1j3S3n-tTW4", "libkernel",
                       SHADPS4_HLE_GET_TSC_FREQUENCY),
    SHADPS4_HLE_EXPORT("WslcK1FQcGI", "libkernel",
                       SHADPS4_HLE_IS_NEO_MODE),
    SHADPS4_HLE_EXPORT("rNRtm1uioyY", "libkernel",
                       SHADPS4_HLE_HAS_NEO_MODE),
    SHADPS4_HLE_EXPORT("WB66evu8bsU", "libkernel",
                       SHADPS4_HLE_GET_COMPILED_SDK_VERSION),
    SHADPS4_HLE_EXPORT("959qrazPIrg", "libkernel",
                       SHADPS4_HLE_GET_PROC_PARAM),
    { "Up36PTk687E", "libSceVideoOut", "libSceVideoOut",
      SHADPS4_HLE_VIDEO_OPEN },
    { "uquVH4-Du78", "libSceVideoOut", "libSceVideoOut",
      SHADPS4_HLE_VIDEO_CLOSE },
    { "i6-sR91Wt-4", "libSceVideoOut", "libSceVideoOut",
      SHADPS4_HLE_VIDEO_SET_BUFFER_ATTRIBUTE },
    { "w3BY+tAEiQY", "libSceVideoOut", "libSceVideoOut",
      SHADPS4_HLE_VIDEO_REGISTER_BUFFERS },
    { "CBiu4mCE1DA", "libSceVideoOut", "libSceVideoOut",
      SHADPS4_HLE_VIDEO_SET_FLIP_RATE },
    { "SbU3dwp80lQ", "libSceVideoOut", "libSceVideoOut",
      SHADPS4_HLE_VIDEO_GET_FLIP_STATUS },
    { "HXzjK9yI30k", "libSceVideoOut", "libSceVideoOut",
      SHADPS4_HLE_VIDEO_ADD_FLIP_EVENT },
    { "U46NwOiJpys", "libSceVideoOut", "libSceVideoOut",
      SHADPS4_HLE_VIDEO_SUBMIT_FLIP },
    { "hv1luiJrqQM", "libScePad", "libScePad", SHADPS4_HLE_PAD_INIT },
    { "xk0AcarP3V4", "libScePad", "libScePad", SHADPS4_HLE_PAD_OPEN },
    { "6ncge5+l5Qs", "libScePad", "libScePad", SHADPS4_HLE_PAD_CLOSE },
    { "q1cHNfGycLI", "libScePad", "libScePad", SHADPS4_HLE_PAD_READ },
    { "yFVnOdGxvZY", "libScePad", "libScePad",
      SHADPS4_HLE_PAD_SET_VIBRATION },
    { "RR4novUEENY", "libScePad", "libScePad",
      SHADPS4_HLE_PAD_SET_LIGHT_BAR },
    { "clVvL4ZDntw", "libScePad", "libScePad",
      SHADPS4_HLE_PAD_SET_MOTION_STATE },
    { "gjP9-KQzoUk", "libScePad", "libScePad",
      SHADPS4_HLE_PAD_GET_CONTROLLER_INFO },
    { "JfEPXVxhFqA", "libSceAudioOut", "libSceAudioOut",
      SHADPS4_HLE_AUDIO_OUT_INIT },
    { "ekNvsT22rsY", "libSceAudioOut", "libSceAudioOut",
      SHADPS4_HLE_AUDIO_OUT_OPEN },
    { "s1--uE9mBFw", "libSceAudioOut", "libSceAudioOut",
      SHADPS4_HLE_AUDIO_OUT_CLOSE },
    { "QOQtbeDqsT4", "libSceAudioOut", "libSceAudioOut",
      SHADPS4_HLE_AUDIO_OUT_OUTPUT },
    { "w3PdaSTSwGE", "libSceAudioOut", "libSceAudioOut",
      SHADPS4_HLE_AUDIO_OUT_OUTPUTS },
    { "b+uAV89IlxE", "libSceAudioOut", "libSceAudioOut",
      SHADPS4_HLE_AUDIO_OUT_SET_VOLUME },
    { "5NE8Sjc7VC8", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_AUDIO_IN_OPEN },
    { "Jh6WbHhnI68", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_AUDIO_IN_CLOSE },
    { "LozEOU8+anM", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_AUDIO_IN_INPUT },
    { "nya-R5gDYhM", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_AUDIO_IN_OPEN },
    { "BohEAQ7DlUE", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_AUDIO_IN_GET_SILENT_STATE },
    { "IQtWgnrw6v8", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "8mtcsG-Qp5E", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "5qRVfxOmbno", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "gUNabrUkZNg", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "X-AQLtdxQOo", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "VoX9InuwwTg", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "48-miagyJ2I", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "kFKJ3MVcDuo", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "mhAfefP9m2g", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "KpBKoHKVKEc", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "YZ+3seW7CyY", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "FVGWf8JaHOE", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "S-rDUfQk9sg", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "NJam1-F7lNY", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "3shKmTrTw6c", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "CTh72m+IYbU", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "SxQprgjttKE", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "rmgXsZ-2Tyk", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "6QP1MzdFWhs", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "+DY07NwJb0s", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "vYFsze1SqU8", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "vyh-T6sMqnw", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "YeBSNVAELe4", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "thLNHvkWSeg", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "rcgv2ciDrtc", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "iN3KqF-8R-w", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "VAzfxqDwbQ0", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "CwBFvAlOv7k", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "tQpOPpYwv7o", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "NUWqWguYcNQ", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "U0ivfdKFZbA", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "hWMCAPpqzDo", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "nqXpw3MaN50", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "arJp991xk5k", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "DVTn+iMSpBM", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "3ULZGIl+Acc", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "4kHw99LUG3A", "libSceAudioIn", "libSceAudioIn",
      SHADPS4_HLE_SUCCESS },
    { "zwY0YV91TTI", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_SUBMIT_COMMAND_BUFFERS },
    { "xbxNatawohc", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_SUBMIT_AND_FLIP },
    { "yvZ73uQUqrk", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_SUBMIT_DONE },
    { "b08AgtPlHPg", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_ARE_SUBMITS_ALLOWED },
    { "29oKvKXzEZo", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_MAP_QUEUE },
    { "A+uGq+3KFtQ", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_MAP_QUEUE },
    { "ArSg-TGinhk", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_UNMAP_QUEUE },
    { "bX5IbRvECXk", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_DING_DONG },
    { "byXlqupd8cE", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_DING_DONG },
    { "5udAm+6boVg", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_WORKLOAD_CREATE },
    { "ihxrbsoSKWc", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_WORKLOAD_BEGIN },
    { "jRcI8VcgTz4", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_SUBMIT_WORKLOAD },
    { "Ga6r7H6Y0RI", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_SUBMIT_FLIP_WORKLOAD },
    { "KnldROUkWJY", "libSceGnmDriver", "libSceGnmDriver",
      SHADPS4_HLE_GNM_QUERY_TCA_UNITS },
    { "Nlev7Lg8k3A", "libSceNet", "libSceNet", SHADPS4_HLE_NET_INIT },
    { "cTGkc6-TBlI", "libSceNet", "libSceNet", SHADPS4_HLE_NET_TERM },
    { "Q4qBuN-c0ZM", "libSceNet", "libSceNet", SHADPS4_HLE_NET_SOCKET },
    { "45ggEzakPJQ", "libSceNet", "libSceNet", SHADPS4_HLE_NET_CLOSE },
    { "OXXX4mUk3uk", "libSceNet", "libSceNet", SHADPS4_HLE_NET_CONNECT },
    { "beRjXBn-z+o", "libSceNet", "libSceNet", SHADPS4_HLE_NET_SEND },
    { "9wO9XrMsNhc", "libSceNet", "libSceNet", SHADPS4_HLE_NET_RECV },
    { "iWQWrwiSt8A", "libSceNet", "libSceNet", SHADPS4_HLE_NET_HTONS },
    { "dgJBaeJnGpo", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_POOL_CREATE },
    { "K7RlrTkI-mw", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_POOL_DESTROY },
    { "SF47kB2MNTo", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_EPOLL_CREATE },
    { "ZVw46bsasAk", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_EPOLL_CONTROL },
    { "drjIbDbA7UQ", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_EPOLL_WAIT },
    { "HQOwnfMGipQ", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_ERRNO_LOC },
    { "8Kcp5d-q1Uo", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_INET_PTON },
    { "9vA2aW+CHuA", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_INET_NTOP },
    { "C4UgDHHPvdw", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_RESOLVER_CREATE },
    { "kJlYH5uMAWI", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_RESOLVER_DESTROY },
    { "Nd91WaWmG2w", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_RESOLVER_NTOA },
    { "TCkRD0DWNLg", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_GETPEERNAME },
    { "zJGf8xjFnQE", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_SOCKET_ABORT },
    { "TSM6whtekok", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_SHUTDOWN },
    { "xphrZusl78E", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_GETSOCKOPT },
    { "2mKX2Spso7I", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_SETSOCKOPT },
    { "bErx49PgxyY", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_BIND },
    { "kOj1HiAGE54", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_LISTEN },
    { "PIWqhn9oSxc", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_ACCEPT },
    { "304ooNZxWDY", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_RECVFROM },
    { "hLuXdjHnhiI", "libSceNet", "libSceNet",
      SHADPS4_HLE_SUCCESS },
    { "gvD1greCu0A", "libSceNet", "libSceNet",
      SHADPS4_HLE_NET_SENDTO },
    { "A9cVMUtEp4Y", "libSceHttp", "libSceHttp", SHADPS4_HLE_HTTP_INIT },
    { "Ik-KpLTlf7Q", "libSceHttp", "libSceHttp", SHADPS4_HLE_HTTP_TERM },
    { "0gYjPTR-6cY", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_CREATE_TEMPLATE },
    { "4I8vEpuEhZ8", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_DELETE_TEMPLATE },
    { "qgxDBjorUxs", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_CREATE_CONNECTION },
    { "P6A3ytpsiYc", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_DELETE_CONNECTION },
    { "Cnp77podkCU", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_CREATE_REQUEST },
    { "qe7oZ+v4PWA", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_DELETE_REQUEST },
    { "EY28T2bkN7k", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_ADD_HEADER },
    { "s2-NPIvz+iA", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_SET_NONBLOCK },
    { "6381dWF+xsQ", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_CREATE_EPOLL },
    { "wYhXVfS2Et4", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_DESTROY_EPOLL },
    { "-xm7kZQNpHI", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_SET_EPOLL },
    { "59tL1AQBb8U", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_UNSET_EPOLL },
    { "1e2BNwI-XzE", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_SEND_REQUEST },
    { "qISjDHrxONc", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_WAIT_REQUEST },
    { "0a2TBNfE3BU", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_GET_STATUS },
    { "aCYPMSUIaP8", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_GET_HEADERS },
    { "yuO2H2Uvnos", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_GET_LENGTH },
    { "P5pdoykPYTk", "libSceHttp", "libSceHttp",
      SHADPS4_HLE_HTTP_READ_DATA },
    { "ZkZhskCPXFw", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_INIT },
    { "l1NmDeDpNGU", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_INIT },
    { "TywrFKCoLGY", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_INIT },
    { "yKDy8S5yLA0", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_TERM },
    { "32HQAQdwM2o", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_MOUNT },
    { "0z45PIH+SNI", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_MOUNT2 },
    { "BMR4F-Uek3E", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_UMOUNT },
    { "85zul--eGXs", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_SET_PARAM },
    { "65VH0Qaaz6s", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_GET_MOUNT_INFO },
    { "S1GkePI17zQ", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_DELETE },
    { "dyIhnXq-0SM", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_DIR_SEARCH },
    { "j8xKtiFj0SY", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_GET_EVENT_RESULT },
    { "oQySEUfgXRA", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_MEMORY_SETUP },
    { "QwOO7vegnV8", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_MEMORY_GET },
    { "cduy9v4YmT4", "libSceSaveData", "libSceSaveData",
      SHADPS4_HLE_SAVE_DATA_MEMORY_SET },
    { "lDqxaY1UbEo", "libSceMsgDialog", "libSceMsgDialog",
      SHADPS4_HLE_DIALOG_INIT },
    { "b06Hh0DPEaE", "libSceMsgDialog", "libSceMsgDialog",
      SHADPS4_HLE_DIALOG_OPEN_MSG },
    { "CWVW78Qc3fI", "libSceMsgDialog", "libSceMsgDialog",
      SHADPS4_HLE_DIALOG_STATUS },
    { "6fIC3XKt2k0", "libSceMsgDialog", "libSceMsgDialog",
      SHADPS4_HLE_DIALOG_STATUS },
    { "Lr8ovHH9l6A", "libSceMsgDialog", "libSceMsgDialog",
      SHADPS4_HLE_DIALOG_RESULT },
    { "HTrcDKlFKuM", "libSceMsgDialog", "libSceMsgDialog",
      SHADPS4_HLE_DIALOG_CLOSE },
    { "ePw-kqZmelo", "libSceMsgDialog", "libSceMsgDialog",
      SHADPS4_HLE_DIALOG_TERM },
    { "s9e3+YpRnzw", "libSceSaveDataDialog", "libSceSaveDataDialog",
      SHADPS4_HLE_DIALOG_INIT },
    { "4tPhsP6FpDI", "libSceSaveDataDialog", "libSceSaveDataDialog",
      SHADPS4_HLE_DIALOG_OPEN_SAVE },
    { "ERKzksauAJA", "libSceSaveDataDialog", "libSceSaveDataDialog",
      SHADPS4_HLE_DIALOG_STATUS },
    { "KK3Bdg1RWK0", "libSceSaveDataDialog", "libSceSaveDataDialog",
      SHADPS4_HLE_DIALOG_STATUS },
    { "yEiJ-qqr6Cg", "libSceSaveDataDialog", "libSceSaveDataDialog",
      SHADPS4_HLE_DIALOG_RESULT },
    { "fH46Lag88XY", "libSceSaveDataDialog", "libSceSaveDataDialog",
      SHADPS4_HLE_DIALOG_CLOSE },
    { "YuH2FA7azqQ", "libSceSaveDataDialog", "libSceSaveDataDialog",
      SHADPS4_HLE_DIALOG_TERM },
    { "hay1CfTmLyA", "libSceSaveDataDialog", "libSceSaveDataDialog",
      SHADPS4_HLE_SAVEDATA_DIALOG_PROGRESS_SET },
    { "NUeBrN7hzf0", "libSceImeDialog", "libSceImeDialog",
      SHADPS4_HLE_DIALOG_OPEN_IME },
    { "IADmD4tScBY", "libSceImeDialog", "libSceImeDialog",
      SHADPS4_HLE_DIALOG_STATUS },
    { "x01jxu+vxlc", "libSceImeDialog", "libSceImeDialog",
      SHADPS4_HLE_DIALOG_RESULT },
    { "oBmw4xrmfKs", "libSceImeDialog", "libSceImeDialog",
      SHADPS4_HLE_DIALOG_CLOSE },
    { "gyTyVn+bXMw", "libSceImeDialog", "libSceImeDialog",
      SHADPS4_HLE_DIALOG_TERM },
    { "uoUpLGNkygk", "libSceCommonDialog", "libSceCommonDialog",
      SHADPS4_HLE_COMMON_DIALOG_INIT },
    { "I88KChlynSs", "libSceErrorDialog", "libSceErrorDialog",
      SHADPS4_HLE_DIALOG_INIT },
    { "M2ZF-ClLhgY", "libSceErrorDialog", "libSceErrorDialog",
      SHADPS4_HLE_DIALOG_OPEN_MSG },
    { "WWiGuh9XfgQ", "libSceErrorDialog", "libSceErrorDialog",
      SHADPS4_HLE_DIALOG_STATUS },
    { "9XAxK2PMwk8", "libSceErrorDialog", "libSceErrorDialog",
      SHADPS4_HLE_DIALOG_TERM },
    { "hdpVEUDFW3s", "libSceSsl", "libSceSsl", SHADPS4_HLE_SSL_INIT },
    { "j3YMu1MVNNo", "libSceUserService", "libSceUserService",
      SHADPS4_HLE_USER_SERVICE_INITIALIZE },
    { "bwFjS+bX9mA", "libSceUserService", "libSceUserService",
      SHADPS4_HLE_USER_SERVICE_TERMINATE },
    { "CdWp0oHWGr0", "libSceUserService", "libSceUserService",
      SHADPS4_HLE_USER_SERVICE_GET_INITIAL_USER },
    { "fPhymKNvK-A", "libSceUserService", "libSceUserService",
      SHADPS4_HLE_USER_SERVICE_GET_LOGIN_USER_ID_LIST },
    { "rPo6tV8D9bM", "libSceSystemService", "libSceSystemService",
      SHADPS4_HLE_SYSTEM_SERVICE_GET_STATUS },
    { "fZo48un7LK4", "libSceSystemService", "libSceSystemService",
      SHADPS4_HLE_SYSTEM_SERVICE_PARAM_GET_INT },
    { "Vo5V8KAwCmk", "libSceSystemService", "libSceSystemService",
      SHADPS4_HLE_SYSTEM_SERVICE_HIDE_SPLASH_SCREEN },
    { "g8cM39EUZ6o", "libSceSysmodule", "libSceSysmodule",
      SHADPS4_HLE_SYSMODULE_LOAD },
    { "eR2bZFAAU0Q", "libSceSysmodule", "libSceSysmodule",
      SHADPS4_HLE_SYSMODULE_UNLOAD },
    { "fMP5NHUOaMk", "libSceSysmodule", "libSceSysmodule",
      SHADPS4_HLE_SYSMODULE_IS_LOADED },
    { "hHrGoGoNf+s", "libSceSysmodule", "libSceSysmodule",
      SHADPS4_HLE_SYSMODULE_LOAD_INTERNAL_WITH_ARG },
    SHADPS4_HLE_EXPORT("D4yla3vx4tY", "libkernel",
                       SHADPS4_HLE_KERNEL_ERROR),
    SHADPS4_HLE_EXPORT("9BcDykPmo1I", "libkernel",
                       SHADPS4_HLE_KERNEL_ERRNO_LOCATION),
    SHADPS4_HLE_EXPORT("Ou3iL1abvng", "libkernel",
                       SHADPS4_HLE_KERNEL_STACK_CHK_FAIL),
    SHADPS4_HLE_EXPORT("k+AXqu2-eBc", "libkernel",
                       SHADPS4_HLE_KERNEL_GETPAGESIZE),
    SHADPS4_HLE_EXPORT("k+AXqu2-eBc", "libScePosix",
                       SHADPS4_HLE_KERNEL_GETPAGESIZE),
    SHADPS4_HLE_EXPORT("iKJMWrAumPE", "libkernel",
                       SHADPS4_HLE_KERNEL_GETARGC),
    SHADPS4_HLE_EXPORT("FJmglmTMdr4", "libkernel",
                       SHADPS4_HLE_KERNEL_GETARGV),
    SHADPS4_HLE_EXPORT("6xVpy0Fdq+I", "libkernel",
                       SHADPS4_HLE_KERNEL_SIGPROCMASK),
    SHADPS4_HLE_EXPORT("6XG4B33N09g", "libkernel",
                       SHADPS4_HLE_KERNEL_SCHED_YIELD),
    SHADPS4_HLE_EXPORT("HoLVWNanBBc", "libkernel",
                       SHADPS4_HLE_KERNEL_GETPID),
    SHADPS4_HLE_EXPORT("D0OdFMjp46I", "libkernel",
                       SHADPS4_HLE_KERNEL_EQUEUE_CREATE),
    SHADPS4_HLE_EXPORT("jpFjmgAC5AE", "libkernel",
                       SHADPS4_HLE_KERNEL_EQUEUE_DELETE),
    SHADPS4_HLE_EXPORT("fzyMKs9kim0", "libkernel",
                       SHADPS4_HLE_KERNEL_EQUEUE_WAIT),
    SHADPS4_HLE_EXPORT("4R6-OvI2cEA", "libkernel",
                       SHADPS4_HLE_KERNEL_EQUEUE_ADD_USER),
    SHADPS4_HLE_EXPORT("WDszmSbWuDk", "libkernel",
                       SHADPS4_HLE_KERNEL_EQUEUE_ADD_USER_EDGE),
    SHADPS4_HLE_EXPORT("F6e0kwo4cnk", "libkernel",
                       SHADPS4_HLE_KERNEL_EQUEUE_TRIGGER_USER),
    SHADPS4_HLE_EXPORT("LJDwdSNTnDg", "libkernel",
                       SHADPS4_HLE_KERNEL_EQUEUE_DELETE_USER),
    SHADPS4_HLE_EXPORT("57ZK+ODEXWY", "libkernel",
                       SHADPS4_HLE_KERNEL_EQUEUE_ADD_TIMER),
    SHADPS4_HLE_EXPORT("YWQFUyXIVdU", "libkernel",
                       SHADPS4_HLE_KERNEL_EQUEUE_DELETE_TIMER),
    SHADPS4_HLE_EXPORT("R74tt43xP6k", "libkernel",
                       SHADPS4_HLE_KERNEL_EQUEUE_ADD_HRTIMER),
    SHADPS4_HLE_EXPORT("J+LF6LwObXU", "libkernel",
                       SHADPS4_HLE_KERNEL_EQUEUE_DELETE_HRTIMER),
    SHADPS4_HLE_EXPORT("mJ7aghmgvfc", "libkernel",
                       SHADPS4_HLE_KERNEL_EVENT_GET_ID),
    SHADPS4_HLE_EXPORT("23CPPI1tyBY", "libkernel",
                       SHADPS4_HLE_KERNEL_EVENT_GET_FILTER),
    SHADPS4_HLE_EXPORT("kwGyyjohI50", "libkernel",
                       SHADPS4_HLE_KERNEL_EVENT_GET_DATA),
    SHADPS4_HLE_EXPORT("vz+pg2zdopI", "libkernel",
                       SHADPS4_HLE_KERNEL_EVENT_GET_USER_DATA),
    SHADPS4_HLE_OBJECT("f7uOxY9mM1U", SHADPS4_HLE_DATA_STACK_CHK_GUARD),
    SHADPS4_HLE_OBJECT("+2thxYZ4syk", SHADPS4_HLE_DATA_ENVIRON),
    SHADPS4_HLE_OBJECT("djxxOmW6-aw", SHADPS4_HLE_DATA_PROGNAME),
    SHADPS4_HLE_OBJECT("nQVWJEGHObc", SHADPS4_HLE_DATA_SIGINTR),
};

static uint32_t shadps4_gnm_compat_dispatch(const char *nid)
{
    static const char failure_nids[] =
        "|-Se2FY+UTsI|-zJi8Vb4Du4|05YzC2r3hHo|0O3xxFaiObw|2IJhUyK8moE|31G6PB2oRYQ|4Mv9OXypBG8|4PKnYXOhcx4|5AtqyMgO7fM|6IMbpR7nTzA|7XRH1CIfNpI|9Mv61HaMhfA|9X4SkENMS0M|ARS+TNLopyk|ASUric-2EnI|AmmYLcJGTl0|DYAC6JUeZvM|F5XJY1XHa3Y|HsLtF4jKe48|JRKSSV0YzwA|K3BKBBYKUSE|KHpZ9hJo1c0|L-owl1dSKKg|LQQN0SwQv8c|LQtzqghKQm4|Lg2isla2XeQ|MJG69Q7ti+s|O0S96YnD04U|OlFgKnBsALE|OpyolX6RwS0|PNf0G7gvFHQ|PaFw9w6f808|QA5h6Gh3r60|QEsMC+M3yjE|QJjPjlmPAL0|QLzOwOF0t+A|RNPAItiMLIg|UBv7FkVfzcQ|UHDiSFDxNao|UoBuWAhKk7U|X6yCBYPP7HA|ZFqKFl23aMc|a3tLC56vwug|aqhuK2Mj4X4|bdqdvIkLPIU|bioGsp74SLM|cMWWYeqQQlM|d-YcZX7SIQA|dewXw5roLs0|dl5u5eGBgNk|dqPBvjFVpTA|eLQbNsKeTkU|fhKwCVVj9nk|gPxYzPp2wlo|gVuGo1nBnG8|hS0MKPRdNr0|hljMAxTLNF0|j6mSQs3UgaY|jpTMyYB8UBI|jwCEzr7uEP4|k8EXkhIP+lM|kY4dsQh+SH4|kkn+iy-mhyg|lN7Gk-p9u78|lbMccQM2iqc|nO-tMnaxJiE|nvEwfYAImTs|pS2tjBxzJr4|q-qhDxP67Hg|rXV8az6X+fM|ru8cb4he6O8|suUlSjWr7CE|t0HIQWnvK9E|vbcR4Ken6AA|wJtaTpNZfH4|wYN5mmv6Ya8|xTsOqp-1bE4|yhFCnaz5daw|";
    static const char validation_nids[] =
        "|5SHGNwLXBV4|BgM3t3LvcNk|MBMa6EFu4Ko|Q7t4VEYLafI|RX7XCNSaL6I|SXw4dZEkgpA|hsZPf1lON7E|iCO804ZgzdA|qGP74T5OWJc|";
    static const char interface_nids[] =
        "|3EXdrVC7WFk|4LSXsEKPTsE|EwjWGcIOgeM|MpncRjHNYRE|ODEeJ1GfDtE|P9iKqxAGeck|t-vIc5cTEzg|";
    static const char capture_nids[] =
        "|4UFagYlfuAM|9thMn+uB1is|u9YKpRRHe-M|xeTLfxVIQO4|";
    static const char *const init_commands[] = {
        "0H2vBYbTLHI", "8lH54sfjfmU", "Idffwf3yh8s", "QhnyReteJ1M",
        "im2ZuItabu4", "nF6bFRUBRAU", "pF1HQjbmQJ0", "yb2cRhagD1I",
    };
    static const char *const commands[] = {
        "+AFvOEXrKJk", "0BzLGljcwBo", "1qXLHIpROPE", "4MgRw-bVNQU",
        "4v+otIIdjqg", "5uFKckiJYRM", "ED9-Fjr8Ta4", "FUHG8sQ3R58",
        "GGsn7jMTxw4", "GNlx+y7xPdE", "KXltnCwEJHQ", "Kx-h-nWQJ8A",
        "MYRtYhojKdA", "NfvOrNzy6sk", "UJwNuMBcUAk", "V31V01UiScY",
        "VJNjFtqiF5w", "W1Etj-jlW7Y", "X9Omw9dwv5M", "Z43vKp5k7r0",
        "aPIZJTXC+cU", "aj3L-iaFmyk", "bQVd5YzCal0", "cFCp0NX8wf0",
        "cUCo8OvArrw", "ffrNQOshows", "gAhCn6UiU4Y", "jHdPvIzlpKc",
        "jiItzS6+22g", "mLVL7N7BVBg", "nLM2i2+65hA", "oYM+YzfCm2Y",
        "stDSYW2SBVM", "thbPcG7E7qk", "vckdzbQ46SI", "wED4ZXCFJT0",
        "5q95ravnueg", "7qZVNgEu+SY", "HlTPoZ-oY7Y", "f5QQLp9rzGk",
    };
    uint32_t i;
    char token[16];

    g_snprintf(token, sizeof(token), "|%s|", nid);
    if (strstr(failure_nids, token)) {
        return SHADPS4_HLE_GNM_FAILURE;
    }
    if (strstr(validation_nids, token)) {
        return SHADPS4_HLE_GNM_VALIDATION_NOT_ENABLED;
    }
    if (strstr(interface_nids, token)) {
        return SHADPS4_HLE_GNM_INTERFACE_VERSION;
    }
    if (strstr(capture_nids, token)) {
        return SHADPS4_HLE_GNM_CAPTURE_FAILED;
    }
    if (!strcmp(nid, "d88anrgNoKY")) {
        return SHADPS4_HLE_GNM_CAPTURE_RAZOR_NOT_LOADED;
    }
    if (!strcmp(nid, "bSJFzejYrJI")) {
        return SHADPS4_HLE_GNM_DEBUG_HANDLE;
    }
    if (!strcmp(nid, "Fwvh++m9IQI")) {
        return SHADPS4_HLE_GNM_GPU_CLOCK;
    }
    if (!strcmp(nid, "FFVZcCu3zWU")) {
        return SHADPS4_HLE_GNM_OFFCHIP_BUFFER_SIZE;
    }
    if (!strcmp(nid, "RU74kek-N0c")) {
        return SHADPS4_HLE_GNM_LOGICAL_CU_MASK;
    }
    if (!strcmp(nid, "b0xyllnVY-I")) {
        return SHADPS4_HLE_GNM_ADD_EQ_EVENT;
    }
    if (!strcmp(nid, "PVT+fuoS9gU")) {
        return SHADPS4_HLE_GNM_DELETE_EQ_EVENT;
    }
    if (!strcmp(nid, "Fa3x75OOLRA")) {
        return SHADPS4_HLE_GNM_WORKLOAD_END;
    }
    if (!strcmp(nid, "UoYY0DWMC0U")) {
        return SHADPS4_HLE_KERNEL_EVENT_GET_DATA;
    }
    if (!strcmp(nid, "ln33zjBrfjk")) {
        return SHADPS4_HLE_GNM_TESS_RING_BASE;
    }
    if (!strcmp(nid, "jajhf-Gi3AI")) {
        return SHADPS4_HLE_GNM_CONTEXT_INIT_SIZE;
    }

    for (i = 0; i < ARRAY_SIZE(init_commands); i++) {
        if (!strcmp(nid, init_commands[i])) {
            return SHADPS4_HLE_GNM_COMPAT_INIT_COMMAND;
        }
    }
    for (i = 0; i < ARRAY_SIZE(commands); i++) {
        if (!strcmp(nid, commands[i])) {
            return SHADPS4_HLE_GNM_COMPAT_COMMAND;
        }
    }
    return SHADPS4_HLE_GNM_FAILURE;
}

typedef struct ShadPS4HLECompatMap {
    const char *nid;
    uint32_t dispatch;
} ShadPS4HLECompatMap;

typedef struct ShadPS4HLETrivialProviders {
    const char *module;
    const char *nids;
} ShadPS4HLETrivialProviders;

static const ShadPS4HLETrivialProviders shadps4_hle_trivial_providers[] = {
#include "shadps4-trivial-providers.inc"
};

static const ShadPS4HLETrivialProviders
shadps4_hle_reviewed_providers[] G_GNUC_UNUSED = {
#include "shadps4-reviewed-providers.inc"
};

static bool shadps4_hle_requires_semantic_provider(const char *module)
{
    static const char *const stateful_modules[] = {
        "libSceAudio3d", "libSceAvPlayer", "libSceCompanionHttpd",
        "libSceGameLiveStreaming", "libSceHttp", "libSceHttp2",
        "libSceNet", "libSceNetCtl", "libSceNpParty", "libSceNpScore",
        "libSceNpTus", "libSceNpWebApi", "libSceNpWebApi2",
        "libSceRemoteplay", "libSceRudp", "libSceSaveData",
        "libSceSsl", "libSceVoice",
    };

    for (uint32_t i = 0; i < ARRAY_SIZE(stateful_modules); i++) {
        if (!g_strcmp0(module, stateful_modules[i])) {
            return true;
        }
    }
    return false;
}

static bool shadps4_hle_is_trivial_provider(const char *module,
                                            const char *nid)
{
    char token[32];
    size_t i;

    if (!module || !nid ||
        g_snprintf(token, sizeof(token), "|%s|", nid) <= 0) {
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(shadps4_hle_trivial_providers); i++) {
        const ShadPS4HLETrivialProviders *providers =
            &shadps4_hle_trivial_providers[i];

        if (!strcmp(module, providers->module) &&
            strstr(providers->nids, token)) {
            return true;
        }
    }
    return false;
}

static uint32_t shadps4_hle_reviewed_provider_dispatch(const char *module,
                                                       const char *nid)
{
#if SHADPS4_HLE_REVIEWED_PROVIDER_COUNT
    char token[32];
    size_t i;

    if (!module || !nid ||
        g_snprintf(token, sizeof(token), "|%s|", nid) <= 0) {
        return UINT32_MAX;
    }
    for (i = 0; i < ARRAY_SIZE(shadps4_hle_reviewed_providers); i++) {
        const ShadPS4HLETrivialProviders *providers =
            &shadps4_hle_reviewed_providers[i];

        if (strcmp(module, providers->module) ||
            !strstr(providers->nids, token)) {
            continue;
        }
        if (!strcmp(module, "libSceGnmDriver")) {
            return shadps4_gnm_compat_dispatch(nid);
        }
        if (!strcmp(module, "libSceFont") ||
            !strcmp(module, "libSceFontFt")) {
            return SHADPS4_HLE_FONT_UNSUPPORTED;
        }
        return SHADPS4_HLE_PROVIDER_UNSUPPORTED;
    }
#else
    (void)module;
    (void)nid;
#endif
    return UINT32_MAX;
}

static bool shadps4_hle_compat_module(const char *module)
{
    static const char *const modules[] = {
        "libSceGnmDriver", "libSceLibcInternal", "libSceHttp2",
        "libSceNetCtl", "libSceNgs2", "libSceAjm", "libSceAvPlayer",
        "libSceUserService", "libSceSystemService", "libSceSysmodule",
        "libSceNet", "libkernel", "libc",
    };
    uint32_t i;

    for (i = 0; i < ARRAY_SIZE(modules); i++) {
        if (!g_strcmp0(module, modules[i])) {
            return true;
        }
    }
    return module && (g_str_has_prefix(module, "libSce") ||
                      !strcmp(module, "libkernel"));
}

static uint32_t shadps4_hle_map_dispatch(
    const ShadPS4HLECompatMap *map, size_t count, const char *nid,
    uint32_t fallback)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (!strcmp(map[i].nid, nid)) {
            return map[i].dispatch;
        }
    }
    return fallback;
}

#define HLE_MAP(nid_, dispatch_) { (nid_), (dispatch_) }

static uint32_t shadps4_hle_compat_dispatch(const char *library,
                                            const char *module,
                                            const char *nid)
{
#define HLE_COMPAT_UNKNOWN UINT32_MAX
    static const ShadPS4HLECompatMap ps2_classics[] = {
        /* libkernel and libkernel_ps2emu imports used by PS2 Classics. */
        HLE_MAP("5dgOEPsEGqw", SHADPS4_HLE_PTHREAD_BARRIER_INIT),
        HLE_MAP("t9vVyTglqHQ", SHADPS4_HLE_PTHREAD_BARRIER_WAIT),
        HLE_MAP("HudB2Jv2MPY", SHADPS4_HLE_PTHREAD_BARRIER_DESTROY),
        HLE_MAP("YKT49TOLQWs", SHADPS4_HLE_JIT_MAP_SHARED_MEMORY),
        HLE_MAP("avvJ3J0H0EY", SHADPS4_HLE_JIT_CREATE_SHARED_MEMORY),
        HLE_MAP("MR221Mwo0Pc", SHADPS4_HLE_JIT_CREATE_ALIAS),

        /* Optional services imported by the PS2 emulator executable. */
        HLE_MAP("MEJ7tc7ThwM", SHADPS4_HLE_COREDUMP_ATTACH_MEMORY),
        HLE_MAP("5nc2gdLNsok", SHADPS4_HLE_COREDUMP_ATTACH_FILE),
        HLE_MAP("dei8oUx6DbU", SHADPS4_HLE_COREDUMP_DEBUG_TEXT),
        HLE_MAP("Dbbkj6YHWdo", SHADPS4_HLE_COREDUMP_WRITE_USER_DATA),
        HLE_MAP("eVkl4XZTS6M", SHADPS4_HLE_PS2_DIALOG_GET_RESULT),
        HLE_MAP("F70KBaPW924", SHADPS4_HLE_PS2_DIALOG_INITIALIZE),
        HLE_MAP("KVDCpwJXoxw", SHADPS4_HLE_PS2_DIALOG_UPDATE_STATUS),
        HLE_MAP("s1zGYYF-xC0", SHADPS4_HLE_PS2_DIALOG_TERMINATE),
        HLE_MAP("coiMIPkR+Ro", SHADPS4_HLE_PS2_DIALOG_OPEN),
        HLE_MAP("KHvkPQJDMLk", SHADPS4_HLE_VIDEO_RECORDING_CLOSE),
        HLE_MAP("OOFxrMY+mfI", SHADPS4_HLE_VIDEO_RECORDING_STOP),
        HLE_MAP("fZJQzFK4Gv4", SHADPS4_HLE_VIDEO_RECORDING_GET_STATUS),
        HLE_MAP("tWoe9IlGAhs", SHADPS4_HLE_VIDEO_RECORDING_START),
        HLE_MAP("tIdXUhSLyOU", SHADPS4_HLE_PS2_PROCESS_ADD),
        HLE_MAP("qhPJ1EfqLjQ", SHADPS4_HLE_PS2_PROCESS_GET_PARENT_SOCKET),
        HLE_MAP("fKqJTnoZ8C8", SHADPS4_HLE_PS2_PROCESS_KILL),
        HLE_MAP("YtDk7X3FF08", SHADPS4_HLE_PS2_PROCESS_SHOW_MENU),
    };
    static const char ssl_stub_nids[] =
        "|Pgt0gg14ewU|wJ5jCpkCv-c|Vc2tb-mWu78|IizpdlgPdpU|Y-5sBnpVclY|jb6LuBv9weg|ExsvtKwhWoM|AvoadUUK03A|S0DCFBqmhQY|Xt+SprLPiVQ|4HzS6Vkd-uU|W80mmhRKtH8|7+F9pr5g26Q|KsvuhF--f6k|Md+HYkCBZB4|rFiChDgHkGQ|9bKYzKP6kYU|xXCqbDBx6mA|xakUpzS9qv0|m7EXDQRv7NU|64t1HKepy1Q|d7AAqdK2IDo|PysF6pUcK-o|ipLIammTj2Q|C05CUtDViqU|tq511UiaNlE|1e46hRscIE8|5U2j47T1l70|+oCOy8+4at8|YMbRl6PNq5U|O+JTn8Dwan8|he6CvWiX3iM|w5ZBRGN1lzY|5e5rj-coUv8|6nH53ruuckc|MB3EExhoaJQ|sDUV9VsqJD8|FXCfp5CwcPk|szJ8gsZdoHE|1aewkTBcGEY|gdWmmelQC1k|6Z-n6acrhTs|p12OhhUCGEE|5G+Z9vXPWYU|WZCBPnvf0fw|AvjnXHAa7G0|goUd71Bv0lk|tf3dP8kVauc|noRFMfbcI-g|Xy4cdu44Xr0|2FPKT8OxHxo|xyd+kSAhtSw|BQIv6mcPFRM|nxcdqUGDgW8|u82YRvIENeo|HBWarJFXoCM|8Lemumnt1-8|JhanUiHOg-M|6ocfVwswH-E|8FqgR3V7gHs|sRIARmcXPHE|ABAA2f3PM8k|CATkBsr20tY|JpnKObUJsxQ|jp75ki1UzRU|prSVrFdvQiU|8+UPqcEgsYg|X-rqVhPnKJI|Pt3o1t+hh1g|oNJNApmHV+M|GCPUCV9k1Mg|lCB1AE4xSkE|+7U74Zy7gKg|hOABTkhp6NM|3CECWZfBTVg|OP-VhFdtkmo|0iwGE4M4DU8|pWg3+mTkoTI|HofoEUZ5mOM|w2lGr-89zLc|OeGeb9Njons|"
        "N+UDju8zxtE|pIZfvPaYmrs|D6QBgLq-nlc|uAHc6pgeFaQ|xdxuhUkYalI|OcZJcxANLfw|gu0eRZMqTu8|s1tJ1zBkky4|4aXDehFZLDA|K-g87UhrYQ8|ULOVCAVPJE4|uS9P+bSWOC0|k3RI-YRkW-M|AloU5nLupdU|gAHkf68L6+0|w2CtqF+x7og|GTSbNvpE1fQ|j6Wk8AtmVQM|wdl-XapuxKU|BQah1z-QS-w|GPRMLcwyslw|CAgB8oEGwsY|3wferxuMV6Y|UO2a3+5CCCs|PRWr3-ytpdg|cW7VCIMCh9A|u+brAYVFGUs|pOmcRglskbI|uBqy-2-dQ-A|U3NHH12yORo|pBwtarKd7eg|1VM0h1JrUfA|viRXSHZYd0c|zXvd6iNyfgc|P14ATpXc4J8|hwrHV6Pprk4|iLKz4+ukLqk|-WqxBRAUVM4|w1+L-27nYas|m-zPyAsIpco|g-zCwUKstEQ|qIvLs0gYxi0|+DzXseDVkeI|RwXD8grHZHM|TDfQqO-gMbY|qOn+wm28wmA|7whYpYfHP74|-PoIzr3PEk0|R1ePzopYPYM|7RBSTKGrmDA|AzUipl-DpIw|xHpt6+2pGYk|Eo0S65Jy28Q|DOwXL+FQMEY|0XcZknp7-Wc|dQReuBX9sD8|Ab7+DH+gYyM|3-643mGVFJo|hi0veU3L2pU|50R2xYaYZwE|p5bM5PPufFY|QWSxBzf6lAg|bKaEtQnoUuQ|E4a-ahM57QQ|lnHFrZV5zAY|0K1yQ6Lv-Yc|UQ+3Qu7v3cA|26lYor6xrR4|iHBiYOSciqY|budJurAYNHc|dCRcdgdoIEI|KI5jhdvg2S8|hk+NcQTQlqI|rKD5kXcvN0E|Fxq5MuWRkSw|vCpt1jyL6C4|wZp1hBtjV1I|P+O-4XCIODs|GfDzwBDRl3M|oM5w6Fb4TWM|dim5NDlc7Vs|Qq0o-+hobOI|y+ZFCsZYNME|5g9cNS3IFCk|i9AvJK-l5Jk|mgs+n71u35Y|4hPwsDmVKZc|"
        "yUd2ukhZLJI|J7LWSdYo0Zg|kRb0lquIrj0|sSD8SHia8Zc|eT7n5lcEYCc|2Irwf6Oqt4E|s9qIeprVILk|NRoSvM1VPm8|dHosoPLXaMw|7QgvTqUGFlU|ufoBDuHGOlM|EAoybreRrGU|ElUzZAXIvY0|Wi9eDU54UCU|BSqmh5B4KTg|xIFe7m4wqX4|zlMZOG3VDYg|fje5RYUa+2g|IKENWUUd8bk|n6-12LafAeA|H4Z3ShBNjSA|9PTAJclcW50|NrZz0ZgQrao|SHInb+l58Bs|f0MBRCQeOEg|6J0PLGaYl0Y|MoaZ6-hDS-k|H02lfd0hCG0|nXlhepw9ztI|Bf0pzkQc6CU|dSP1n53RtVw|kNIvrkD-XJk|pbTq-nEsN1w|-UDxVMs9h9M|nH9FVvfZhCs|2Bd7UoCRhQ8|wcVuyTUr5ys|IuduYLwFh9c|KPh5GncdOcc|";
    static const char http_stub_nids[] =
        "|mNan6QSnpeY|JM58a21mtrQ|lGAjftanhFs|Y1DCjN-s2BA|zzB0StvRab4|wF0KcxK20BE|A7n9nNg7NBg|nOkViL17ZOo|seCvUt91WHY|pFnXDxo3aog|Lffcxao-QMM|6gyx-I0Oob4|fzzBpJjm9Kw|VmqSnjZ5mE4|KJtUHtp6y0U|oEuPssSYskA|L2gM3qptqHs|pxBsD-X9eH0|9m8EcOGzcIQ|L-DwVoHXLtU|+G+UsJpeXPc|iSZjWw1TGiA|xkymWiGdMiI|IQOP6McWJcY|16sMmVuOvgU|hkcfqAl+82w|u05NnI+P+KY|4fgkfVeVsGU|qFg2SuyTJJY|jf4TB2nUO40|PDxS48xGQLs|XNUoD2B9a6A|pM--+kIeW-8|Kp6juCJUJGQ|7Y4364GBras|Kh6bS2HQKbo|GnVDzYfy-KI|pHc3bxUzivU|22buO-UufJY|LG1YW1Uhkgo|pk0AuomQM1o|gZ9TpeFQ7Gk|2NeZnMEP3-0|h9wmFZX4i-4|vO4B-42ef-k|POJ0azHZX3w|JBN6N-EY+3M|jUjp+yqMNdQ|htyBOoWeS58|U5ExQGyyx9s|CR-l-yI-o7o|";
    static const char font_stub_nids[] =
        "|coCrV6IWplE|lVSR5ftvNag|vzHs3C8lWJk|MpKSBaYKluo|WBNBaj9XiJU|4So0MC3oBIM|NlO5Qlhjkng|cYrMGk1wrMA|7rogx92EEyc|8h-SOB-asgk|5QG71IjgOpQ|zZQD3EwJo3c|hWE4AwNixqY|PEjv7CVDRYs|UuY-OJF+f0k|5kx49CAlO-M|OINC0X9HGBY|ZB8xRemRRG8|4X14YSK4Ldk|eb9S3zNlV5o|tiIlroGki+g|3hVv3SNoL6E|gVQpMBuB7fE|BozJej5T6fs|ryPlnDDI3rU|8REoLjNGCpM|IrXeG0Lc6nA|7-miUT6pNQw|8-zmgsxkBek|oO33Uex4Ui0|RmkXfBcZnrM|r4KEihtwxGs|n22d-HIdmMg|RL2cAQgyXR8|dUmIK6QjT7E|X2Vl3yU19Zw|DOmdOwV3Aqw|zdYdKRQC3rw|UkMUIoj-e9s|DJURdcnVUqo|eQac6ftmBQQ|PEYQJa+MWnk|21g4m4kYF6g|pJzji5FvdxU|scaro-xEuUM|"
        "W66Kqtt0xU0|FzpLsBQEegQ|W80hs0g5d+E|S48+njg9p-o|wcOQ8Fz73+M|YBaw2Yyfd5E|qkySrQ4FGe0|qzNjJYKVli0|9iRbHCtcx-o|KZ3qPyz5Opc|LqclbpVzRvM|Wl4FiI4qKY0|WC7s95TccVo|zC6I4ty37NA|drZUF0XKTEI|MEAmHMynQXE|XRUOmQhnYO4|98XGr2Bkklg|Nj-ZUVOVAvc|p0avT2ggev0|0C5aKg9KghY|4pA3qqAcYco|cpjgdlMYdOM|774Mee21wKk|Hp3NIFhUXvQ|bhmZlml6NBs|5sAWgysOBfE|W4e8obm+w6o|EgIn3QBajPs|MnUYAs2jVuU|R-oVDMusYbc|b9R+HQuHSMI|IN4P5pJADQY|U+LLXdr2DxM|yStTYSeb4NM|eDxmMoxE5xU|Ax6LQJJq6HQ|I5Rf2rXvBKQ|PxSR9UfJ+SQ|SnsZua35ngs|71w5DzObuZI|IPoYwwlMx-g|olSmXY+XP1E|H-FNq8isKE0|1+DgKL0haWQ|"
        "JQKWIsS9joE|nlU2VnfpqTM|+FYcYefsVX0|wyKFUOWdu3Q|APTXePHIjLM|A8ZQAl+7Dec|B+q4oWOyfho|CUCOiOT5fOM|CfkpBe2CqBQ|DRQs7hqyGr4|FL0unhGcFvI|GsU8nt6ujXU|HUARhdXiTD0|HoPNIMLMmW8|MUsfdluf54o|NQ5nJf7eKeE|Pbdz8KYEvzk|T-Sd0h4xGxw|UmKHZkpJOYE|VcpxjbyEpuk|Vj-F8HBqi00|Vp4uzTQpD0U|WgR3W2vkdoU|X9k7yrb3l1A|YrU5j4ZL07Q|b5AQKU2CI2c|d1fpR0I6emc|fga6Ugd-VPo|k7Nt6gITEdY|lLCJHnERWYo|l4XJEowv580|l9+8m2X7wOE|rNlxdAXX08o|sZqK7D-U8W8|wQ9IitfPED0|0Mi1-0poJsc|5I080Bw0KjM|6slrIYa3HhQ|-keIqW70YlY|-n5a6V0wWPU|";
    static const char font_ft_stub_nids[] =
        "|e60aorDdpB8|BxcmiMc3UaA|MEWjebIzDEI|ZcQL0iSjvFw|LADHEyFTxRQ|+jqQjsancTs|oakL15-mBtc|dcQeaDr8UJc|2KXS-HkZT3c|H0mJnhKwV-s|S2mw3sYplAI|+ehNXJPUyhk|4BAhDLdrzUI|Utlzbdf+g9o|nAfQ6qaL1fU|X9+pzrGtBus|w0hI3xsK-hc|w5sfH9r8ZJ4|ojW+VKl4Ehs|";
    static const char shell_core_stub_nids[] =
        "|5SfMtsW8h7A|Uku2JpZmoqc|qVBNhnqUz-4|TfVHoRVX2HM|fBuukeGZ2FE|mpkohyVqCRM|fkcM5YcqjV8|x5hqKRKziYU|jktCMQNgyFc|xIMClZZz50k|MRVnLsn-GRI|CZrOHqt6oCY|ibXh+Mc4wbs|wtNEh1E9ALA|v81dfnaMfUY|9VDzY7m1NN8|AgYSGAQGtXs|l5bdg4tUTGc|RnY2HTwqz3A|SYSL4KtzcAU|KTCPKqvFTok|F20xA1NsG9s|XlcBqhyaJyI|yO7OIU45UnQ|4SgLbJPUxNw|ctTYL9lomv8|gYXxtLzFU8Y|0QN4BUnzF14|WN1v3xYoGDw|A3wbbLmrQV4|5YNnX5Pfquo|9plZCCRm9x4|SOmyRqRpKIM|+jVaKSG0nHk|0g6-uh4JTP8|dtx5tcGFVII|F-g-G0oJegs|UG9I-iHI-ME|LlFmfrkpjW0|FmjFl9Nvwcw|WISL-JH-6Ic|XGxXS135WR8|V9b3HfN19vM|u474-bA7ul0|kyFOaxSaP0A|f5Z7FIeoHdw|dZ3RfDzgmCY|ZIKGk+35UDU|5gIVIzipgsw|lAvSrKAjxCA|EwfSRaPlCE4|gAyT42nwElM|Mg3P1Z4Xavs|FcAheKO8u7c|jCJ+gks483A|"
        "-ROAAenn4Xg|V9LadIvu5Ko|J5OPALFNdFE|368es-zmQuc|NTttBlD2Xbk|4YQ-w9Xwn7s|L6R0jU7yTTQ|oINHTqU1qvY|vPxKoc5MyxQ|rkV5b-p490g|guf+xcMoCas|ZbY5LxmH6uA|1qbUFXlBXFw|-g0pBZ2JdAc|g8T4x0GD9Wo|beQ90Sx6c8g|ns8HVzVqaNo|kn3vBOTg-ok|97L0D3+iBDE|NZWLG-imDlU|RM1Xb5Hcq4w|93trbeNjx7c|Ac3I81u9ky4|39lewWn5+G0|kuErIHXWIpc|wFvgq-KXT0Q|m5OsHQx9Ni4|qEUJBsB7yMk|lgbdvT36kTE|bTmtBchzFps|lXlP+jhO8QI|CKTyfq2tb7k|VxWJ7DUrEIQ|3M1tCF9VfoE|x6STXhIEG0M|1G3xnMBZpYI|zS6ZPLgQJcA|9coQ7gTfwhA|ai7LcBcf6Rs|HeZzL2xbRJY|L5mESo+Iq+k|hQClZK9mdkk|fRurGDbUulc|nG+HNBwQ4sw|WH6O4hriE-0|PPQxiE4lbyY|9i+R1rj6Trk|WKxOVIkISxA|W5HtGRCZ1iE|D-6S9JHI6A0|ZGbkd2hWhJU|vq8ubGb2XjQ|fORZmlh1TQo|E4I7uCoWbkM|plK52OfeEIc|"
        "VbEHW7RrJ+w|0y01ndm0BA8|oh68H-4hEAE|DviyPC-JJ1k|2b-b5AouLv4|soq7GTbVMkw|vYHJtZyhhEI|-Lpr5gHkHkc|mpeGML7ulA8|PGsAGnnRstY|KyQY2KfMxKw|izo3BrmWZDM|wCbG33VsbqQ|mTZxVC3pebc|44PCVgTBBCw|KH0InA0uStg|3JNHzrEDnrk|CWcxjT6X+1c|WIEUJ61AwvU|gWMlFq4N9Lw|GEZ9sIz3wuM|vzWoetyaUuA|4dsNPwVODKM|IHHSdVBTwBc|GYUk4t27Myw|nENvUAsAKdY|2rOYe6lVCVQ|-Sp1aaqI1SQ|aCkM+OaGv3g|juqlPZWkJGc|qNe8uNe3EpQ|bRCLw49N4hE|1e7CmFlJveU|n9xRQPtUP0g|Mi9-CweviUo|V-5cjs+9kI0|VQRWOxYGays|roUQwCYYegE|4CzZUVleMcE|awBTm0vNaos|dk-PIxWMp8k|IldAc7Eq5-c|Tgs5zOUfQSc|pb2XPMV5beI|-hrMXdpZuDU|fCeSFo0IM-w|cZCJTMamDOE|PGHjjtZxKvs|K-QFvDXYSbg|HBA-tCWUXP8|EFhM9SF2aSQ|cfJZThTSxQ0|jKnwOdgck5g|yO-ekZ5toaQ|lF96Sr8Jf0s|"
        "-yYPJb0ejO8|8+CmlQdq7u8|+2paAsKqXOQ|7JgSJnaByIs|IzQN+F5q3wg|7yUQmZWoqVg|xKSgaSVX1io|dS1+1D1LRHs|l96YlUEtMPk|bC8vo608P2E|K33+EwitWlo|m65uKv7IAkI|MeboioVomns|zd4oVXWGD2Y|4Pd0g-lGEM0|TJp3kdSGsIw|qtjjorW1V94|g787tMBA1TE|jqj5vbglbZU|l22TAIbbtFw|IWSCO20RwIY|nA5rRwLrgIU|2Pms7iCE-Fo|kfyuElAEnis|Ujz25JX-jPM|GB19cfR-Tis|atiUTsTFJ3k|-9djWj1NU4E|lW+8pdTQMmg|0ptZiu0jBJs|chZFHnGa9x4|yxiUUPJoyYI|GjOxjIVZA1Y|R013D1VIETQ|nu542EmGFD4|HhBo--ix7Lg|kozqEeuRwrk|9dvVBukqOsw|dbwyzALlKOQ|DWVv0MlE1sw|VxRZE4CZQw8|CSl1MAdUbYs|lcp9E77DAB4|T9xeifEUF3w|qqL5VYwFLgo|YvCj4cb1-jU|oeyHRt5PP+Q|JTctYix8NXU|Hlylpx+n8Cg|bUNkT3XDg0Y|c5+4Scso9EU|sgYo-zXHQRE|";
    static const char hmd_stub_nids[] =
        "|rU3HK9Q0r8o|riPQfAdebHk|wHnZU1qtiqw|NuEjeN8WCBA|QasPTUPWVZE|Wr5KVtyVDG0|whRxl6Hhrzg|w8BEUsIYn8w|0cQDAbkOt2A|Asczi8gw1NM|6+v7m1vwE+0|E0BLvy57IiQ|UTqrWB+1+SU|ego1YdqNGpI|WR7XsLdjcqQ|eMI1Hq+NEwY|dI3StPLQlMM|lqPT-Bf1s4I|QxhJs6zHUmU|A2jWOLPzHHE|E9scVxt0DNg|6RclvsKxr3I|cE99PJR6b8w|SuE90Qscg0s|5f-6lp7L5cY|dv2RqD7ZBd4|pN0HjRU86Jo|mdc++HCXSsQ|gjyqnphjGZE|bl4MkWNLxKs|a1LmvXhZ6TM|+UzzSnc0z9A|uQc9P8Hrr6U|nK1g+MwMV10|L5WZgOTw41Y|3w8SkMfCHY0|1Xmb76MHXug|S0ITgPRkfUg|mxjolbeBa78|RFIi20Wp9j0|P04LQJQZ43Y|-u82z1UhOq4|iINSFzCIaB8|Csuvq2MMXHU|UhFPniZvm8U|aTg7K0466r8|9exeDpk7JU8|yNtYRsxZ6-A|EKn+IFVsz0M|AxQ6HtktYfQ|ynKv9QCSbto|3jcyx7XOm7A|+PDyXnclP5w|67q17ERGBuw|uGyN1CkvwYU|"
        "p9lSvZujLuo|-Z+-9u98m9o|df+b0FQnnVQ|i6yROd9ygJs|Aajiktl6JXU|GwFVF2KkIT4|LWQpWHOSUvk|YiIVBPLxmfE|LMlWs+oKHTg|nBv4CKUGX0Y|4hTD8I3CyAk|EJwPtSSZykY|r7f7M5q3snU|gCjTEtEsOOw|HAr740Mt9Hs|9-jaAXUNG-A|1gkbLH5+kxU|6kHBllapJas|k1W6RPkd0mc|dp1wu22jSGc|d2TeoKeqM5U|WxsnAsjPF7Q|eOOeG9SpEuc|gA4Xnn+NSGk|stQ7AsondmE|jfnS-OoDayM|roHN4ml+tB8|0z2qLqedQH0|xhx5rVZEpnw|e7laRxRGCHc|CRyJ7Q-ap3g|dG4XPW4juU4|rAXmGoO-VmE|lu9I7jnUvWQ|hyATMTuQSoQ|c4mSi64bXUw|U9kPT4g1mFE|dX-MVpXIPwQ|4KIjvAf8PCA|NbxTfUKO184|NnRKjf+hxW4|4AP0X9qGhqw|Mzzz2HPWM+8|LkBkse9Pit0|v243mvYg0Y0|EwXvkZpo9Go|g3DKNOy1tYw|mjMsl838XM8|8IS0KLkDNQY|afhK5KcJOJY|+zPvzIiB+BU|9z8Lc64NF1c|s5EqYh5kbwM|a1LMFZtK9b0|-6FjKlMA+Yc|"
        "IC0NGmh-zS8|NY2-gYo9ihI|XMutp2-o9A4|Y9QDFn3AjPA|eRVgwy9PbWg|fJVZYeqFttM|mVIneDkja6c|ox9NqLO9LhI|qS18I6w2SZM|sWZSZB-mnw4|-Bk71lPyry4|-y4OUwFf4jE|za4xJfzCBcM|grCYks4m8Jw|goi5ASvH-V8|mP2ZcYmDg-o|thTykLZ-tZs|NTIbBpSH9ik|94+Ggm38KCg|mdyFbaJj66M|MdV0akauNow|ymiwVjPB5+k|ZrV5YIqD09I|utHD2Ab-Ixo|OuygGEWkins|BTrQnC6fcAk|6CRWGc-evO4|E+dPfjeQLHI|LjdLRysHU6Y|knyIhlkpLgE|7as0CjXW1B8|dntZTJ7meIU|q3e8+nEguyE|RrvyU1pjb9A|XZ5QUzb4ae0|8gH1aLgty5I|gqAG7JYeE7A|3JyuejcNhC0|mKa8scOc4-k|kcldQ7zLYQQ|vzMEkwBQciM|F7Sndm5teWw|PAa6cUL5bR4|0wnZViigP9o|iGNNpDDjcwo|oxoDINgOrZk|uab6BzXsfkk|";
    static const char np_tus_stub_nids[] =
        "|lL+Z3zCKNTs|f2Pe4LGS2II|IVSbAEOxJ6I|k5NZIzggbuk|2eq1bMwgZYo|cRVmNrJDbG8|wPFah4-5Xec|2dB427dT3Iw|Q2UmHdK04c8|Nt1runsPVJc|GjlEgLCh4DY|EPeq43CQKxY|mXZi1D2xwZE|4VLlu7EIjzk|6Lu9geO5TiA|ukr6FBSrkJw|lliK9T6ylJg|wjNhItL2wzg|hhy8+oecGac|0DT5bP6YzBo|iXzUOM9sXU0|6-+Yqc-NppQ|OCozl1ZtxRY|xutwCvsydkk|zDeH4tr+0cQ|mYhbiRtkE1Y|pwnE9Oa1uF8|NQIw7tzo0Ow|0nDVqcYECoM|o02Mtf8G6V0|WCzd3cxhubo|iaH+Sxlw32k|uoFvgzwawAY|1TE3OvH61qo|CFPx3eyaT34|-LxFGYCJwww|B7rBR0CoYLI|GQHCksS7aLs|5R6kI-8f+Hk|DXigwIBTjWE|yixh7HDKWfk|OheijxY5RYE|LUwvy0MOSqw|TDoqRD+CE+M|68B6XDgSANk|cy+pAALkHp8|C8TY-UnQoXg|wrImtTqUSGM|YFYWOwYI6DY|mD6s8HtMdpk|FabW3QpY3gQ|pgcNwFHoOL4|Qyek420uZmM|azmjx3jBAZA|668Ij9MYKEU|"
        "DgpRToHWN40|LQ6CoHcp+ug|KBfBmtxCdmI|4UF2uu2eDCo|NGCeFUl5ckM|bHWFSg6jvXc|GDXlRTxgd+M|2BnPSY1Oxd8|AsziNQ9X2uk|y-DJK+d+leg|m9XZnxw9AmE|DFlBYT+Lm2I|wTuuw4-6HI8|DPcu0qWsd7Q|uFxVYJEkcmc|qp-rTrq1klk|NvHjFkx2rnU|lxNDPDnWfMc|kt+k6jegYZ8|0zkr0T+NYvI|fJU2TZId210|WBh3zfrjS38|cVeBif6zdZ4|lq0Anwhj0wY|w-c7U0MW2KY|H6sQJ99usfE|xwJIlK0bHgA|I5dlIKkHNkQ|6G9+4eIb+cY|Gjixv5hqRVY|eGunerNP9n0|YRje5yEXS0U|fVvocpq4mG4|V8ZA3hHrAbw|Q5uQeScvTPE|oZ8DMeTU-50|Djuj2+1VNL0|82RP7itI-zI|zB0vaHTzA6g|xZXQuNSTC6o|kbWqOt3QjKU|Fmx4tapJGzo|+RhzSuuXwxo|E4BCVfx-YfM|c6aYoa47YgI|cf-WMA0jYCc|ypMObSwfcns|1Cz0hTJFyh4|CJAxTxQdwHM|6GKDdRCFx8c|KMlHj+tgfdQ|ukC55HsotJ4|0up4MP1wNtc|bGTjTkHPHTE|xQfR51i4kck|"
        "oGIcxlUabSA|uf77muc5Bog|MGvSJEHwyL8|JKGYZ2F1yT8|fcCwKpi4CbU|CjVIpztpTNc|ZbitD262GhY|trZ6QGW6jHs|";
    static const char lnc_util_stub_nids[] =
        "|V350H0h35IU|GmKMHwvxLlo|mC3BKJFlbNI|4dWfNKOT1sg|j72lst7BFuc|u1JVDP28ycg|MxXZ-poqGNs|93MnzhkAAgk|uaqZvga3Fkg|4oofFQudfx0|GHUqRrCB2hM|GkcNZBoiDcs|AGnsy1zV34o|UukL0EXLQls|vquYrvImjPg|NS-XWAN9uoc|i-esdF3Kz-g|vbMEQcz6O8g|i+1kluDITlQ|MVF+elex8Sw|Wu+zDz8VIFk|ppWFdoDMMSs|oYQC9Quj6No|DxRki7T2E44|cyO5ShJxdnE|g0wTG9KImzI|1AQf7o8gpHc|7yXjWLWJFHU|CgVdl9Sp1G0|deCYc7iaC5Q|yUh0BIPbhVo|ZucoOmNsb7w|ojmvNKQZNUw|wGobSSrBM4s|HRXjUojlG70|kOd75qDlxBM|LZs6hfPMnso|f-Q8Nd33FBc|PyNH7p4LVw8|IGrJsPNL6n4|teGoPWnEgd4|iUsONHVCDbQ|i4tm7MB0ZK0|Ry4u8KxkVY4|gNn+EZtm1i0|SZ2uH5Abws8|RBlEzB--JeA|IhlLdSAX+Jk|+nRJUD-7qCk|wwpRNgW81Cs|+8LJld9LIt4|HKZmLmGfkd4|-3moAnxKYkc|P563r-eGAh4|CJ45DLRQOD8|"
        "Qn5JIRI6ZNU|V25-9U+YauY|awS+eYVuXJA|QvUYLdPhylQ|1PQhPdyNCj8|QsLhZ+8WvSM|lD-k3hDhlqA|XaC9s-Nr2u4|v7DYuX0G5TQ|3mHuKF7fsd8|X8gYbyLG1wk|NJYAQeP3z7c|3+64z-ckBS8|r07vD4SP2sc|Y8onQYjuvOU|8vYXkdXmh-Q|rd+-SzL202E|Kt1k5aBzrcE|cCod+B3EdhI|msW-hp1U0zo|iRZduYIV1hs|aVRNp1nOOKY|BnMaW5wfnlQ|cqui4JUJtbY|";
    static const char np_trophy_stub_nids[] =
        "|aTnHs7W-9Uk|cqGkYAN-gRw|lhE4XS9OJXs|qJ3IvrOoXg0|zDjF2G+6tI0|7Kh86vJqtxw|ndLeNWExeZE|6EOfS5SDgoo|MW5ygoZqEBs|3tWKpNKn5+I|iqYfxC12sak|w4uMPmErD4I|Ht6MNTl-je4|u9plkqa2e0k|pE5yhroy9m0|edPIOFpEAvU|DSh3EXpqAQ4|sng98qULzPA|t3CQzag7-zs|jF-mCgGuvbQ|PeAyBjC5kp8|PEo09Dkqv0o|kF9zjnlAzIA|UXiyfabxFNQ|hvdThnVvwdY|ITUmvpBPaG0|BSoSgiMVHnY|d9jpdPz5f-8|JzJdh-JLtu0|z8RCP536GOM|Rd2FBOQE094|Q182x0rT75I|lGnm5Kg-zpA|20wAMbXP-u0|sKGFFY59ksY|JMSapEtDH9Q|dk27olS4CEE|cBzXEdzVzvs|8aLlLHKP+No|NobVwD8qcQY|yXJlgXljItk|U0TOSinfuvw|-LC9hudmD+Y|q6eAMucXIEM|WdCUUJLQodM|4QYFwC7tn4U|OcllHFFcQkI|tQ3tXfVZreU|g0dxBNTspC0|sJSDnJRJHhI|X47s4AamPGg|7WPj4KCF3D8|pzL+aAk0tQA|Ro4sI9xgYl4|7+OR1TU5QOA|"
        "aXhvf2OmbiE|Rkt0bVyaa4Y|nXr5Rho8Bqk|eV1rtLr+eys|SsGLKTfWfm0|XqLLsvl48kA|-qjm2fFE64M|50BvYYzPTsY|yDJ-r-8f4S4|mWtsnHY8JZg|tAxnXpzDgFw|tV18n8OcheI|kV4DP0OTMNo|lZSZoN8BstI|nytN-3-pdvI|JsRnDKRzvRw|FJZW2oHUHFk|n4AHGHb-pfY|+O9vU1CpGZA|+not13BEdVI|";
    static const char np_webapi_stub_nids[] =
        "|KQIkDGf80PQ|f-pgaNSd1zc|UJ8H+7kVQUE|3OnubUs02UM|gRiilVCvfAI|BkxO0e2+ueg|B4OVXU6VY9o|Gm138-2DI6g|HgaTom-g+VQ|JKm18ddwAM8|JKqm9Q5MI2E|JNiFPWtH-Hk|J5s+nHxKncU|KEYeKen41pc|PCliRwT6ueA|PwJ4BO0uwR4|QGbJTngpl80|R8hTVoFdvpA|T86AZUN+O4c|U2KAvj2rtSE|V6DhvHJCGfM|WBl0nAQLZjc|YZjQyCXoYxk|YfK56KsJN0M|a8OI5hE-DUQ|dQDwxPjcLRY|daA4FMfpA58|eJ1gJsUhQW4|fe1j0GOZ7-8|flWi3MA9OVo|fmyPn7hpZ-Q|fwS31KfUHoA|jhZyUt+lyVc|ldAEblBOOwk|lyhL-aTxj98|meMsH0c36rQ|nP9mHqC8v4M|nrDh9GesOyk|ojGP5vur+qM|ugei4b97OXE|vQgD7uDMKaA|vm9OVSS7E18|wNSQ60gepNA|wXXTksptCEo|zQE2rxZdLy8|0cCtt7Uv6rU|4yR2XRjuTRI|54n5gNkHtlM|+aMuhoVidDY|";
    static const char game_live_stub_nids[] =
        "|NqkTzemliC0|PC4jq87+YQI|FcHBfHjFXkA|lZ2Sd0uEvpo|6c2zGtThFww|dWM80AX39o4|wBOQWjbWMfU|aRSQNqbats4|CoPMx369EqM|lK8dLBNp9OE|OIIm19xu+NM|PMx7N4WqNdo|yeQKjHETi40|kvYEw2lBndk|ysWfX5PPbfc|cvRCb7DTAig|K0QxEbD7q+c|-EHnU68gExU|hggKhPySVgI|nFP8qT9YXbo|b5RaMD2J0So|hBdd8n6kuvE|uhCmn81s-mU|fo5B8RUaBxQ|iorzW0pKOiA|gDSvt78H3Oo|HE93dr-5rx4|3PSiwAzFISE|TwuUzTKKeek|Gw6S4oqlY7E|QmQYwQ7OTJI|Sb5bAXyUt5c|q-kxuaF7URU|hUY-mSOyGL0|ycodiP2I0xo|x6deXUpQbBo|mCoz3k3zPmA|ZuX+zzz2DkA|MLvYI86FFAo|y0KkAydy9xE|Y1WxX7dPMCw|D7dg5QJ4FlE|bYuGUBuIsaY|9yK6Fk8mKOQ|5XHaH3kL+bA|";
    static const char remoteplay_stub_nids[] =
        "|xQeIryTX7dY|IYZ+Mu+8tPo|ZYUsJtcAnqA|cCheyCbF7qw|tPYT-kGbZh8|6Lg4BNleJWc|j98LdSGy4eY|L+cL-M-DP3w|g4K51cY+PEw|3eBNV9A0BUM|ufesWMVX6iU|DxU4JGh4S2k|n5OxFJEvPlc|Cekhs6LSHC0|ig1ocbR7Ptw|gV9-8cJPM3I|cMk57DZXe6c|-gwkQpOCl68|58v9tSlRxc8|C3r2zT5ebMg|oB730zwoz0s|rOTg1Nljp8w|k1SwgkMSOM8|R8RZC1ZIkzU|uYhiELUtLgA|d-BBSEq1nfc|Yytq7NE38R8|Wg-w8xjMZA4|yheulqylKwI|t5ZvUiZ1hpE|mrNh78tBpmg|7QLrixwVHcU|-ThIlThsN80|0Z-Pm5rZJOI|xSrhtSLIjOc|5-2agAeaE+c|Rf0XMVR7xPw|n4l3FTZtNQM|-BPcEQ1w8xc|BOwybKVa3Do|HV7jZe1frbM|";
    static const char ime_stub_nids[] =
        "|mN+ZoSN-8hQ|uTW+63goeJs|Lf3DeGWC6xg|zHuMUGb-AQI|OTb0Mg+1i1k|Ho5NVQzpKHo|P5dPeiLwm-M|tKLmVIUkpyM|NYDsL9a0oEo|l01GKoyiQrY|E2OcGgi-FPY|JAiMBkOTYKI|JoPdCUXOzMU|FuEl46uHDyo|E+f1n8e8DAw|evjOsE18yuI|wVkehxutK-U|oYkJlMK51SA|ua+13Hk9kKs|3Hx2Uw9xnv8|16UI54cWRQk|TQaogSaqkEk|oOwl47ouxoM|gtoTsGM9vEY|wTKF4mUlSew|rM-1hkuOhh0|42xMaQ+GLeQ|ZmmV6iukhyo|EQBusz6Uhp8|LBicRa-hj3A|-IAOwd2nO7g|qDagOjvJdNk|tNOlmxee-Nk|rASXozKkQ9g|idvMaIu5H+k|ga5GOgThbjo|RuSca8rS6yA|J7COZrgSFRA|WqAayyok5p0|O7Fdd+Oc-qQ|fwcPR7+7Rks|";
    static const char np_tus_compat_stub_nids[] =
        "|cRVmNrJDbG8|Q2UmHdK04c8|ukr6FBSrkJw|lliK9T6ylJg|0DT5bP6YzBo|OCozl1ZtxRY|mYhbiRtkE1Y|0nDVqcYECoM|GQHCksS7aLs|5R6kI-8f+Hk|DXigwIBTjWE|LUwvy0MOSqw|cy+pAALkHp8|YFYWOwYI6DY|pgcNwFHoOL4|Qyek420uZmM|NGCeFUl5ckM|bHWFSg6jvXc|uFxVYJEkcmc|qp-rTrq1klk|NvHjFkx2rnU|0zkr0T+NYvI|xwJIlK0bHgA|I5dlIKkHNkQ|6G9+4eIb+cY|YRje5yEXS0U|zB0vaHTzA6g|xZXQuNSTC6o|+RhzSuuXwxo|E4BCVfx-YfM|c6aYoa47YgI|ukC55HsotJ4|xQfR51i4kck|ZbitD262GhY|trZ6QGW6jHs|";
    static const char razor_cpu_stub_nids[] =
        "|JFzLJBlYIJE|SfRTRZ1Sh+U|gVioM9cbiDs|G90IIOtgFQ0|PAytDtFGpqY|sPhrQD31ClM|B782NptkGUc|EH9Au2RlSrE|A7oRMdaOJP8|NFwh-J-BrI0|ElNyedXaa4o|EboejOQvLL4|dnEdyY4+klQ|KP+TBWGHlgs|9FowWFMEIM8|XCuZoBSVFG8|njGikRrxkC0|YpkGsMXP3ew|zw+celG7zSI|uZrOwuNJX-M|D0yUjM33QqU|jqYWaTfgZs0|DJsHcEb94n0|EZtqozPTS4M|emklx7RK-LY|TIytAjYeaik|jWpkVWdMrsM|Ax7NjOzctIM|we3oTKSPSTw|vyjdThnQfQQ|0yNHPIkVTmw|Crha9LvwvJM|q1GxBfGHO0s|6rUvx-6QmYc|G3brhegfyNg|";
    static const char rudp_stub_nids[] =
        "|uQiK7fjU6y8|J-6d0WTjzMc|l4SLBpKUDK4|CAbbX6BuQZ0|6PBNpsgyaxw|fJ51weR1WAI|3hBvwqEwqj8|Ms0cLK8sTtE|wIJsiqY+BMk|2G7-vVz9SIg|vfrL8gPlm2Y|Px0miD2LuW0|mCQIhSmCP6o|Qignjmfgha0|sAZqO2+5Qqo|fRc1ahQppR4|i3STzxuwPx0|amuBfI-AQc4|szEVu+edXV4|tYVWcWDnctE|+BJ9svDmjYs|vPzJldDSxXc|yzeXuww-UWg|haMpc7TFx0A|MVbmLASjn5M|LjwbHpEeW0A|M6ggviwXpLs|9U9m1YH0ScQ|rZqWV3eXgOA|SUEVes8gvmw|beAsSTVWVPQ|0yzYdZf0IwE|OMYRTU0uc4w|KaPL3fbTLCA|";
    static const char voice_stub_nids[] =
        "|oV9GAdJ23Gw|nXpje5yNpaE|b7kJI+nx2hg|ajVj3QG2um4|Oo0S5PH7FIQ|cJLufzou6bc|Pc4z1QjForU|elcxZTEfHZM|CrLqDwWLoXM|Z6QV6j7igvE|jjkCjneOYSs|9TrhuGzberQ|IPHvnM5+g04|x0slGBQW+wY|Dinob0yMRl8|cQ6DGsQEjV4|udAxvCePkUs|gAgN+HkiEzY|jbkJFmOZ9U0|TexwmOHQsDg|gwUynkEgNFY|oUha0S-Ij9Q|clyKUyi3RYU|QBFoAIjJoXQ|54phPH2LZls|Ao2YNSA7-Qo|jSZNP7xJrcw|hg9T73LlRiU|wFeAxEeEi-8|YeJl6yDlhW0|";
    static const char system_state_stub_nids[] =
        "|6gtqLPVTdJY|7qf7mhzOQPo|88y5DztlXBE|Ap5dJ0zHRVY|asLBe0esmIY|cmjuYpVujQs|eBFzDYThras|FzjISMWw5Xg|geg26leOsvw|gK3EX6ZKtKc|gPx1b36zyMY|GvqPsPX4EUI|H2f6ZwIqLJg|ifJiF5witJ4|j3IrOCL+DmM|Laac0S4FuhE|PcJ5DLzZXSs|rIqPq0oWlrg|rSquvOtwQmk|texLPLDXDso|U1dZXAjkBVo|uR1wFHXX1XQ|wlxvESTUplk|ypl-BoZZKOM|YWftBq50hcA|ze0ky5Q1yE8|ZwhQSHTqGpE|";
    static const char camera_stub_nids[] =
        "|0wnf2a60FqI|3VJOpzKoIeM|5Oie5RArfWs|8MjO05qk5hA|8WtmqmE4edw|B260o9pSzM8|eTywOSWsEiI|-H3UwGQvNZI|hawKak+Auw4|hHA1frlMxYE|IAz2HgZQWzE|j5isFVIlZLk|jeTpU0MqKU0|jTJCdyv9GLU|LEMk5cTHKEA|lS0tM6n+Q5E|nnR7KAIDPv8|NVITuK83Z7o|olojYZKYiYs|py8p6kZcHmA|QhjrPkRPUZQ|ULxbwqiYYuU|vejouEusC7g|wpeyFwJ+UEI|wQfd7kfRZvo|Y0pCDajzkVQ|";
    static const char np_party_stub_nids[] =
        "|+v4fVHMwFWc|3e4k2mzLkmc|4gOMfNYzllw|aEzKdJzATZ0|DRA3ay-1DFQ|EjyAI+QNgFw|EKi1jx59SP4|F1P+-wpxQow|J8jAi-tfJHc|kA88gbv71ao|-lc6XZnQXvM|lhYCTQmBkds|-MFiL7hEnPE|nazKyHygHhY|nOZRy-slBoA|o7grRhiGHYI|oLYkibiHqRA|RXNCDw2GDEg|T2UOKf00ZN0|TaNw7W25QJw|U6VdUe-PNAY|v2RYVGrJDkM|XQSUbbnpPBA|yARHEYLajs0|zo4G5WWYpKg|zQ7gIvt11Pc|";
    static const char app_content_stub_nids[] =
        "|+OlXCu8qxUk|3rHWaV-1KC4|5bvvbUSiFs4|74-1x3lyZK8|7bOLX66Iz-U|7gxh+5QubhY|9Gq5rOkWzNU|a5N7lAG0y2Q|AS45QoYHjc4|B5gVeVurdUA|bcolXMmp6qQ|bVtF7v2uqT0|CN7EbEV7MFU|D3H+cjfzzFY|EqMtBHWu-5M|Gl6w5i0JokY|gpGZDB4ZlrI|kUeYucqnb7o|QuApZnMo9MM|S5eMvWnbbXg|SaKib2Ug0yI|TVM-aYIsG9k|xhb-r8etmAA|ZiATpP9gEkA|";
    static const char app_content_base_stub_nids[] =
        "|xmhnAoxN3Wk|xZo2-418Wdo|kJmjt81mXKQ|efX3lrPwdKA|z9hgjLd1SGA|3wUaDTGmjcQ|TCqT7kPuGx0|";
    static const char share_play_stub_nids[] =
        "|+MCXJlWdi+s|3Oaux9ITEtY|6-1fKaa5HlY|6egMR0eB8RU|891hmdoV7UQ|9zwJpai7jGc|aGlema+JxUU|co2NCj--pnc|-F6NddfUsa4|ggnCfalLU-8|isruqthpYcw|KADsbjNCgPo|LpPA6mprZ8Q|Md7Mdkr8LBc|OOrLKB0bSDs|QZy+KmyqKPU|rWVNHNnEx6g|U28jAuLHj6c|UaLjloJinow|vUMkWXQff3w|VUW2V9cUTP4|wcI2co2I4Xc|XL0WwUJoQPg|zEDkUWLVwFI|";
    static const char system_gesture_stub_nids[] =
        "|0KrW5eMnrwY|1MMK0W-kMgA|3pcAvmwKCvM|3QYCmMlOlCY|4WOA1eTx3V8|ELvBVG-LKT0|fLTseA7XiWY|FWF8zkhr854|GgFMb22sbbI|h8uongcBNVs|j4h82CQWENo|j4yXIA2jJ68|JhwByySf9FY|KAeP0+cQPVU|L8YmemOeSNY|lpsXm7tzeoc|o11J529VaAE|oBuH3zFWYIg|qpo-mEOwje0|TSKvgSz5ChU|wPJGwI2RM2I|yBaQ0h9m1NM|";
    static const char vr_tracker_stub_nids[] =
        "|5IFOAYv-62g|76OBvrrQXUc|9fvHMUbsom4|ARhgpXvwoR0|bDGZVTwwZ1A|CtWUbFgmq+I|D6TJSfjTAk4|E0P0sN-wy+4|EUCaQtXXYNI|gkGuO9dd57M|KFxq-AnEL34|lgWSHQ8p4i4|mmzbIQNmT4o|NhPkY3V8E+8|qBjnR0HtMYI|tNJrfYsY3wY|TVegDMLaBB8|VItTwN8DmS8|vpsLLotiSUg|zvyKP0Z3UvU|";
    static const char netctl_ap_ipc_stub_nids[] =
        "|3pxwYqHzGcw|5oLJoOVBbGU|6uvAl4RlEyk|8eyH37Ns8tk|9Dxg7XSlr2s|amqSGH8l--s|DufQZgH5ISc|HMvaHoZWsn8|LEn8FGztKWc|mjFgpqNavHg|ofGsK+xoAaM|qhZbOi+2qLY|R-4a9Yh4tG8|sgWeDrEt24U|VQl16Q+qXeY|YtTwZ3pa4aQ|";
    static const char companion_httpd_stub_nids[] =
        "|+-du9tWgE9s|-0c9TCTwnGs|0SCgzfVQHpo|0SySxcuVNG0|8pWltDG7h6A|B-QBMeFdNgY|fHNmij7kAUM|h3OvVxzX4qM|k7F0FcDM-Xc|OA6FbORefbo|OaWw+IVEdbI|r-2-a0c7Kfc|w7oz0AWHpT4|xweOi2QT-BE|ykNpWs3ktLY|ZSHiUfYK+QI|";
    static const char audio3d_stub_nids[] =
        "|8hm6YdoQgwg|Aacl5qkRU6U|CKHlRW2E9dA|flPcUaXVXcw|iRX6GJs9tvE|kEqqyDkmgdI|lvWMW6vEqFU|Mw9mRQtWepY|psv2gbihC1A|-pzYDZozm+M|QfNXBrKZeI0|-R1DukFq7Dk|-Re+pCWvwjQ|SEggctIeTcI|uJ0VhGcxCTQ|yEYXcbAGK14|";
    static const char screenshot_stub_nids[] =
        "|2xxUtuC-RzE|73WQ4Jj0nJI|ahHhOf+QNkQ|AS45QoYHjc4|BDUaqlVdSAY|G7KlmIYFIZc|hNmK4SdhPT0|ICNJ-1POs84|JuMLLmmvRgk|-SV-oTNGFQk|tIYf0W5VTi8|VlAQIgXa2R0|ysfza71rm9M|";
    static const char np_score_stub_nids[] =
        "|3Ybj4E1qNtY|AgcxgceaH8k|AZ4eAlGDy-Q|dTXC+YcePtM|Kc+3QK84AKM|m6F7sE1HQZU|nRoYV2yeUuw|nXaF1Bxb-Nw|oXjVieH6ZGQ|qW9M0bQ-Zx0|S3xZj35v8Z8|yxK68584JAU|";
    static const char netctl_ap_stub_nids[] =
        "|19Ec7WkMFfQ|4jkLJc954+Q|AKZOzsb9whc|cv5Y2efOTeg|FdN+edNRtiw|hfkLVdXmfnU|LXADzTIzM9I|meFMaDpdsVI|NpTcFtaQ-0E|pmjobSVHuY0|r-pOyN6AhsM|";
    static const char mouse_stub_nids[] =
        "|1FeceR5YhAo|6aANndpS0Wo|BRXOoXQtb+k|crkFfp-cmFo|eDQTFHbgeTU|ghLUU2Z5Lcg|jJP1vYMEPd4|QA9Qupz3Zjw|WiGKINCZWkc|Ymyy1HSSJLQ|";
    static const char app_messaging_stub_nids[] =
        "|+zuv20FsXrA|5ygy1IPUh5c|90unWbnI0qE|alZfRdr2RP8|hdoMbMFIDdE|HIwEvx4kf6o|iKNXKsUtOjY|jKgAUl6cLy0|yOiZq+9-ZMQ|ZVRXXqj1n80|";
    static const char net_bwe_stub_nids[] =
        "|0lViPaTB-R8|c+aYh130SV0|G4vltQ0Vs+0|GqETL5+INhU|mEUt-phGd5E|ouyROWhGUbM|pQLJV5SEAqk|XtClSOC1xcU|YALqoY4aeY0|";
    static const char web_browser_dialog_stub_nids[] =
        "|Cya+jvTtPqg|O7dIZQrwVFY|PSK+Eik919Q|RLhKBOoNyXY|TZnDVkP91Rg|uYELOMVnmNQ|vCaW0fgVQmc|Wit4LjeoeX4|";
    static const char signin_dialog_stub_nids[] =
        "|2m077aeC+PA|Bw31liTFT3A|JlpJVoRWv7U|LXlmS6PvJdU|M3OkENHcyiU|mlYGfmqE3fQ|nqG7rqnYw1U|";
    static const char activate_hevc_stub_nids[] =
        "|+2uXfrrQCyk|2HHfdrT+rnQ|-9LzYPdangA|BgjPgbXKYjE|E9FdusyklCA|tImUgGSSHpc|VXA8STT529w|";
    static const char activate_hevc_soft_stub_nids[] =
        "|djVe06YjzkI|f-WtMqIKo20|MyDvxh8+ckI|P-awBIrXrTQ|PNO2xlDVdzg|s6ucQ90BW3g|ytMU6x1nlmU|";
    static const char hmd_setup_dialog_stub_nids[] =
        "|+z4OJmFreZc|6lVRHMV5LY0|J9eBpW1udl4|NB1Y2kA2jCY|nmHzU4Gh0xs|NNgiV4T+akU|Ud7j3+RDIBg|";
    static const char activate_mpeg2_stub_nids[] =
        "|-7zMNJ1Ap1c|aVZb961bWBU|F-nn3DvNKww|JjIspXDbL6o|PkRTWNBI4IQ|W-U8F5o2SHg|";
    static const char np_party_compat_stub_nids[] =
        "|F1P+-wpxQow|kA88gbv71ao|-MFiL7hEnPE|nOZRy-slBoA|o7grRhiGHYI|T2UOKf00ZN0|";
    static const char hmd_distortion_stub_nids[] =
        "|8A4T5ahi790|ao8NZ+FRYJE|gEokC+OGI8g|HT8qWOTOGmo|Vkkhy8RFIuk|";
    static const char netctl_v6_stub_nids[] =
        "|+lxqIKeU9UY|1NE9OWdBIww|H5yARg37U5g|hIUVeUNxAwc|Jy1EO5GdlcM|";
    static const char gnm_resource_stub_nids[] =
        "|+RaJBCVJZVM|5R1E24FRI4w|gQNwGezNDgE|HEOIaxbuVTA|v7QcBXR48L8|";
    static const char vr_gpu_test_stub_nids[] =
        "|5ucmy8hcSPk|9kB+RsZt84M|ERmwvjmfN+c|hj7zLvyw+pw|SSi0OBa8RA0|";
    static const char np_webapi2_stub_nids[] =
        "|2hlBNB96saE|bltDCAskmfE|Io7kh1LHDoM|qaMcX2+6ZiA|qmINYLuqzaA|";
    static const char companion_util_stub_nids[] =
        "|cE5Msy11WhU|H1fYQd5lFAI|IPN-FRSrafk|MaVrz79mT5o|xb1xlIhf0QY|";
    static const char vr_live_capture_stub_nids[] =
        "|3YCwwpHkHIg|lm6T1Ur6JRk|qa1+CeXKDPc|rvCywCbc7Pk|sBkAqyF5Gns|";
    static const char common_dialog_stub_nids[] =
        "|2RdicdHhtGA|I+tdxsCap08|v4+gzuTkv6k|CwCzG0nnLg8|Ib1SMmbr07k|6TIMpGvsrC4|+UyKxWAnqIU|bUCx72-9f0g|xZtXq554Lbg|C-EZ3PkhibQ|70niEKUAnZ0|mdJgdwoM0Mo|87GekE1nowg|6ljeTSi+fjs|W2MzrWix2mM|D-V35OhFeIM|QXFsLON5QWw|SDpCfY9uB0g|aUS4PgJye98|afLdI6i0lQw|mVRnPerBcK0|p9TTq4bLdFU|reTFEla4NQw|txNJzxX6yrA|yxjgDvqUbGQ|8q7icGBWIrA|";
    static const char np_manager_stub_nids[] =
        "|uqcPJLWL08M|KO+11cgC7N0|A2CQ3kgSopQ|Ec63y59l9tw|JELHf4xPufo|0c7HbXRKUt4|";
    static const char usbd_stub_nids[] =
        "|ZfbvM+OP-1A|l-BWutkKrec|xVEEozs1smQ|1WtDBgcgseA|";
    static const char content_export_stub_nids[] =
        "|FzEWeYnAFlI|0GnN4QCgIfs|+KDWny9Y-6k|";
    static const char np_partner_stub_nids[] =
        "|pMxXhNozUX8|pQfYTZHznMc|7CxI50-xlCk|";
    static const char videodec_stub_nids[] =
        "|U0kpGF1cl90|kg+lH0V61hM|f8AgDv-1X8A|";
    static const char error_dialog_stub_nids[] =
        "|jrpnVQfJYgQ|wktCiyWoDTI|";
    static const char playgo_stub_nids[] =
        "|uEqMfMITvEI|vU+FqrH+pEY|";
    static const char pngdec_stub_nids[] = "|cJ--1xAbj-I|";
    static const char jpeg_enc_stub_nids[] = "|QbrU0cUghEM|";
    static const char np_auth_stub_nids[] = "|PM3IZCw-7m0|";
    static const char np_profile_dialog_stub_nids[] = "|nrQRlLKzdwE|";
    static const char np_sns_facebook_dialog_stub_nids[] = "|fjV7C8H0Y8k|";
    static const char video_out_stub_nids[] = "|MTxxrOCeSig|";
    static const char video_recording_stub_nids[] = "|Fc8qxlKINYQ|";
    char ssl_nid_token[16];
    uint32_t static_index;
    static const ShadPS4HLECompatMap libc[] = {
        HLE_MAP("Q3VBxCXhUHs", SHADPS4_HLE_LIBC_MEMCPY),
        HLE_MAP("NFLs+dRJGNg", SHADPS4_HLE_LIBC_MEMCPY_S),
        HLE_MAP("8zTFvBIAIN8", SHADPS4_HLE_LIBC_MEMSET),
        HLE_MAP("DfivPArhucg", SHADPS4_HLE_LIBC_MEMCMP),
        HLE_MAP("j4ViWNHEgww", SHADPS4_HLE_LIBC_STRLEN),
        HLE_MAP("Ovb2dSJOAuE", SHADPS4_HLE_LIBC_STRCMP),
        HLE_MAP("aesyjrHVWy4", SHADPS4_HLE_LIBC_STRNCMP),
        HLE_MAP("ob5xAW4ln-0", SHADPS4_HLE_LIBC_STRCHR),
        HLE_MAP("6sJWiWSRuqk", SHADPS4_HLE_LIBC_STRNCPY),
        HLE_MAP("Ls4tzzhimqQ", SHADPS4_HLE_LIBC_STRCAT),
        HLE_MAP("5Xa2ACNECdo", SHADPS4_HLE_LIBC_STRCPY_S),
        HLE_MAP("K+gcnFFJKVc", SHADPS4_HLE_LIBC_STRCAT_S),
        HLE_MAP("YNzNkJzYqEg", SHADPS4_HLE_LIBC_STRNCPY_S),
        HLE_MAP("H8ya2H00jbI", SHADPS4_HLE_LIBC_MATH_SIN),
        HLE_MAP("Q4rRL34CEeE", SHADPS4_HLE_LIBC_MATHF_SIN),
        HLE_MAP("2WE3BTYVwKM", SHADPS4_HLE_LIBC_MATH_COS),
        HLE_MAP("-P6FNMzk2Kc", SHADPS4_HLE_LIBC_MATHF_COS),
        HLE_MAP("jMB7EFyu30Y", SHADPS4_HLE_LIBC_MATH_SINCOS),
        HLE_MAP("pztV4AF18iI", SHADPS4_HLE_LIBC_MATHF_SINCOS),
        HLE_MAP("T7uyNqP7vQA", SHADPS4_HLE_LIBC_MATH_TAN),
        HLE_MAP("ZE6RNL+eLbk", SHADPS4_HLE_LIBC_MATHF_TAN),
        HLE_MAP("7Ly52zaL44Q", SHADPS4_HLE_LIBC_MATH_ASIN),
        HLE_MAP("GZWjF-YIFFk", SHADPS4_HLE_LIBC_MATHF_ASIN),
        HLE_MAP("JBcgYuW8lPU", SHADPS4_HLE_LIBC_MATH_ACOS),
        HLE_MAP("QI-x0SL8jhw", SHADPS4_HLE_LIBC_MATHF_ACOS),
        HLE_MAP("OXmauLdQ8kY", SHADPS4_HLE_LIBC_MATH_ATAN),
        HLE_MAP("weDug8QD-lE", SHADPS4_HLE_LIBC_MATHF_ATAN),
        HLE_MAP("HUbZmOnT-Dg", SHADPS4_HLE_LIBC_MATH_ATAN2),
        HLE_MAP("EH-x713A99c", SHADPS4_HLE_LIBC_MATHF_ATAN2),
        HLE_MAP("NVadfnzQhHQ", SHADPS4_HLE_LIBC_MATH_EXP),
        HLE_MAP("8zsu04XNsZ4", SHADPS4_HLE_LIBC_MATHF_EXP),
        HLE_MAP("dnaeGXbjP6E", SHADPS4_HLE_LIBC_MATH_EXP2),
        HLE_MAP("wuAQt-j+p4o", SHADPS4_HLE_LIBC_MATHF_EXP2),
        HLE_MAP("9LCjpWyQ5Zc", SHADPS4_HLE_LIBC_MATH_POW),
        HLE_MAP("1D0H2KNjshE", SHADPS4_HLE_LIBC_MATHF_POW),
        HLE_MAP("rtV7-jWC6Yg", SHADPS4_HLE_LIBC_MATH_LOG),
        HLE_MAP("RQXLbdT2lc4", SHADPS4_HLE_LIBC_MATHF_LOG),
        HLE_MAP("WuMbPBKN1TU", SHADPS4_HLE_LIBC_MATH_LOG10),
        HLE_MAP("lhpd6Wk6ccs", SHADPS4_HLE_LIBC_MATHF_LOG10),
        HLE_MAP("MUjC4lbHrK4", SHADPS4_HLE_LIBC_FFLUSH),
        HLE_MAP("z7STeF6abuU", SHADPS4_HLE_LIBC_MUTEX_INIT),
        HLE_MAP("pE4Ot3CffW0", SHADPS4_HLE_LIBC_MUTEX_LOCK),
        HLE_MAP("cMwgSSmpE5o", SHADPS4_HLE_LIBC_MUTEX_UNLOCK),
        HLE_MAP("LaPaA6mYA38", SHADPS4_HLE_LIBC_MUTEX_DESTROY),
        HLE_MAP("xGT4Mc55ViQ", SHADPS4_HLE_LIBC_FILE_FIND),
        HLE_MAP("dREVnZkAKRE", SHADPS4_HLE_LIBC_FILE_PREP),
        HLE_MAP("sQL8D-jio7U", SHADPS4_HLE_LIBC_FILE_OPEN_RAW),
        HLE_MAP("A+Y3xfrWLLo", SHADPS4_HLE_LIBC_FILE_POSITION),
        HLE_MAP("Ss3108pBuZY", SHADPS4_HLE_LIBC_FILE_AVAILABLE),
        HLE_MAP("9s3P+LCvWP8", SHADPS4_HLE_LIBC_FILE_READ_PREP),
        HLE_MAP("jVDuvE3s5Bs", SHADPS4_HLE_LIBC_FILE_FREE),
        HLE_MAP("vZkmJmvqueY", SHADPS4_HLE_LIBC_FILE_LOCK),
        HLE_MAP("0x7rx8TKy2Y", SHADPS4_HLE_LIBC_FILE_LOCK),
        HLE_MAP("xeYO4u7uyJ0", SHADPS4_HLE_LIBC_FOPEN),
        HLE_MAP("rQFVBXp-Cxg", SHADPS4_HLE_LIBC_FSEEK),
        HLE_MAP("lbB+UlZqVG0", SHADPS4_HLE_LIBC_FREAD),
        HLE_MAP("uodLYyUip20", SHADPS4_HLE_LIBC_FCLOSE),
        HLE_MAP("eLdDw6l0-bU", SHADPS4_HLE_LIBC_SNPRINTF),
        HLE_MAP("NWtTN10cJzE", SHADPS4_HLE_LIBC_HEAP_TRACE_INFO),
    };
    static const ShadPS4HLECompatMap pad[] = {
        HLE_MAP("AcslpN1jHR8", SHADPS4_HLE_PAD_GET_DEVICE_CLASS_INFO),
        HLE_MAP("hGbf2QTBmqc", SHADPS4_HLE_PAD_GET_EXT_CONTROLLER_INFO),
        HLE_MAP("u1GRHp+oWoY", SHADPS4_HLE_PAD_GET_HANDLE),
        HLE_MAP("1Odcw19nADw", SHADPS4_HLE_PAD_GET_INFO),
        HLE_MAP("WFIiSfXGUq8", SHADPS4_HLE_PAD_OPEN),
        HLE_MAP("YndgXqQVV7c", SHADPS4_HLE_PAD_READ_STATE),
        HLE_MAP("DscD1i9HX1w", SHADPS4_HLE_PAD_RESET_LIGHT_BAR),
        HLE_MAP("rIZnR6eSpvk", SHADPS4_HLE_PAD_RESET_ORIENTATION),
        HLE_MAP("iHuOWdvQVpg", SHADPS4_HLE_PAD_SET_LIGHT_BAR),
        /* Exact no-op providers marked STUBBED by shadPS4. */
        HLE_MAP("kazv1NzSB8c", SHADPS4_HLE_SUCCESS),
        HLE_MAP("IHPqcbc0zCA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("d7bXuEBycDI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("0aziJjRZxqQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("pnZXireDoeI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("9ez71nWSvD0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("77ooWxGOIVs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("+cE4Jx431wc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("E1KEw5XMGQQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("DD-KiRLBqkQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Q66U8FdrMaw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("qtasqbvwgV4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Uq6LgTJEmQs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("hDgisSGkOgw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("4rS5zG7RFaM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("1DmZjZAuzEM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("PZSoY8j0Pko", SHADPS4_HLE_SUCCESS),
        HLE_MAP("kiA9bZhbnAg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("4x5Im8pr0-4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("vegw8qax5MI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("WPIB7zBWxVE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("k4+nDV9vbT0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("do-JDWX+zRs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("QuOaoOcSOw0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("bi0WNvZ1nug", SHADPS4_HLE_SUCCESS),
        HLE_MAP("mEC+xJKyIjQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("d2Qk-i8wGak", SHADPS4_HLE_SUCCESS),
        HLE_MAP("4y9RNPSBsqg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("9e56uLgk5y0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("pFTi-yOrVeQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("CfwUlQtCFi4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("s7CvzS+9ZIs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("71E9e6n+2R8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("DrUu8cPrje8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("fm1r2vv5+OU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("QjwkT2Ycmew", SHADPS4_HLE_SUCCESS),
        HLE_MAP("2NhkFTRnXHk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("3u4M8ck9vJM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("5Wf4q349s+Q", SHADPS4_HLE_SUCCESS),
        HLE_MAP("+4c9xRLmiXQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("+Yp6+orqf1M", SHADPS4_HLE_SUCCESS),
        HLE_MAP("jbAqAvLEP4A", SHADPS4_HLE_SUCCESS),
        HLE_MAP("KLmYx9ij2h0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("r44mAxdSG+U", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ew647HuKi2Y", SHADPS4_HLE_SUCCESS),
        HLE_MAP("MbTt1EHYCTg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("MLA06oNfF+4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("bsbHFI0bl5s", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xqgVCEflEDY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("lrjFx4xWnY8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("dhQXEvmrVNQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("etaQhgPHDRY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("o-6Y99a8dKU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("flYYxek1wy8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("DmBx8K+jDWw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("FbxEpTRDou8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("yah8Bk4TcYY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("vDLMoJLde8I", SHADPS4_HLE_SUCCESS),
        HLE_MAP("z+GEemoTxOo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("8BOObG94-tc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("--jrY4SHfm8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("zFJ35q3RVnY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("80XdmVYsNPA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("gAHvg6JPIic", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Oi7FzRWFr0Y", SHADPS4_HLE_SUCCESS),
        HLE_MAP("0MB5x-ieRGI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("N7tpsjWQ87s", SHADPS4_HLE_SUCCESS),
        HLE_MAP("PFec14-UhEQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("pjPCronWdxI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("LKXfw7VJYqg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("IWOyO5jKuZg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("KY0hSB+Uyfo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("UeUUvNOgXKU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ickjfjk9okM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("7xA+hFtvBCA", SHADPS4_HLE_SUCCESS),
    };
    static const ShadPS4HLECompatMap video_out[] = {
        HLE_MAP("Xru92wHJRmg", SHADPS4_HLE_VIDEO_ADD_VBLANK_EVENT),
        HLE_MAP("6kPnj51T62Y", SHADPS4_HLE_VIDEO_GET_RESOLUTION_STATUS),
        HLE_MAP("zgXifHT9ErY", SHADPS4_HLE_VIDEO_IS_FLIP_PENDING),
        HLE_MAP("N5KDtkIjjJ4", SHADPS4_HLE_VIDEO_UNREGISTER_BUFFERS),
        HLE_MAP("OcQybQejHEY", SHADPS4_HLE_VIDEO_GET_BUFFER_LABEL_ADDRESS),
        HLE_MAP("1FZBKy8HeNU", SHADPS4_HLE_VIDEO_GET_VBLANK_STATUS),
        HLE_MAP("kGVLc3htQE8", SHADPS4_HLE_VIDEO_GET_DEVICE_CAPABILITY),
        HLE_MAP("j6RaAUlaLv0", SHADPS4_HLE_VIDEO_WAIT_VBLANK),
        HLE_MAP("U2JJtSqNKZI", SHADPS4_HLE_VIDEO_GET_EVENT_ID),
        HLE_MAP("rWUTcKdkUzQ", SHADPS4_HLE_VIDEO_GET_EVENT_DATA),
        HLE_MAP("Mt4QHHkxkOc", SHADPS4_HLE_VIDEO_GET_EVENT_COUNT),
        HLE_MAP("DYhhWbJSeRg", SHADPS4_HLE_VIDEO_SET_GAMMA),
        HLE_MAP("pv9CI5VC+R0", SHADPS4_HLE_VIDEO_ADJUST_COLOR),
        HLE_MAP("-Ozn0F1AFRg", SHADPS4_HLE_VIDEO_DELETE_FLIP_EVENT),
        HLE_MAP("oNOQn3knW6s", SHADPS4_HLE_VIDEO_DELETE_VBLANK_EVENT),
        HLE_MAP("pjkDsgxli6c", SHADPS4_HLE_VIDEO_MODE_SET_ANY),
        HLE_MAP("N1bEoJ4SRw4", SHADPS4_HLE_VIDEO_CONFIGURE_MODE),
        HLE_MAP("IOdgHlCGU-k", SHADPS4_HLE_VIDEO_CHANGE_BUFFER_ATTRIBUTE),
    };
    static const ShadPS4HLECompatMap fiber[] = {
        HLE_MAP("hVYD7Ou2pCQ", SHADPS4_HLE_FIBER_INITIALIZE),
        HLE_MAP("7+OJIpko9RY", SHADPS4_HLE_FIBER_INITIALIZE_IMPL),
        HLE_MAP("asjUJJ+aa8s", SHADPS4_HLE_FIBER_OPT_INITIALIZE),
        HLE_MAP("JeNX5F-NzQU", SHADPS4_HLE_FIBER_FINALIZE),
        HLE_MAP("a0LLrZWac0M", SHADPS4_HLE_FIBER_RUN),
        HLE_MAP("PFT2S-tJ7Uk", SHADPS4_HLE_FIBER_SWITCH),
        HLE_MAP("p+zLIOg27zU", SHADPS4_HLE_FIBER_GET_SELF),
        HLE_MAP("B0ZX2hx9DMw", SHADPS4_HLE_FIBER_RETURN_TO_THREAD),
        HLE_MAP("avfGJ94g36Q", SHADPS4_HLE_FIBER_RUN_IMPL),
        HLE_MAP("ZqhZFuzKT6U", SHADPS4_HLE_FIBER_SWITCH_IMPL),
        HLE_MAP("uq2Y5BFz0PE", SHADPS4_HLE_FIBER_GET_INFO),
        HLE_MAP("Lcqty+QNWFc", SHADPS4_HLE_FIBER_START_SIZE_CHECK),
        HLE_MAP("Kj4nXMpnM8Y", SHADPS4_HLE_FIBER_STOP_SIZE_CHECK),
        HLE_MAP("JzyT91ucGDc", SHADPS4_HLE_FIBER_RENAME),
        HLE_MAP("0dy4JtMUcMQ", SHADPS4_HLE_FIBER_GET_FRAME_POINTER),
    };
    static const ShadPS4HLECompatMap save_data[] = {
        /* Exact no-op providers marked STUBBED by shadPS4. */
        HLE_MAP("dQ2GohUHXzk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("kLJQ3XioYiU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("hHHCPRqA3+g", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ykwIZfVD08s", SHADPS4_HLE_SUCCESS),
        HLE_MAP("G0hFeOdRCUs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("rYvLW1z2poM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("v1TrX+3ZB10", SHADPS4_HLE_SUCCESS),
        HLE_MAP("-eczr5e4dsI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("4OPOZxfVkHA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("1i0rfc+mfa8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("p6A1adyQi3E", SHADPS4_HLE_SUCCESS),
        HLE_MAP("S49B+I96kpk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("YbCO38BOOl4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("kbIIP9aXK9A", SHADPS4_HLE_SUCCESS),
        HLE_MAP("gW6G4HxBBXA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("bYCnxLexU7M", SHADPS4_HLE_SUCCESS),
        HLE_MAP("hVDqYB8+jkk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("K9gXXlrVLNI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("5yHFvMwZX2o", SHADPS4_HLE_SUCCESS),
        HLE_MAP("UGTldPVEdB4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("AYBQmnRplrg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("SQWusLoK8Pw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("pJrlpCgR8h4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("fU43mJUgKcM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("uZqc4JpFdeY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xJ5NFWC3m+k", SHADPS4_HLE_SUCCESS),
        HLE_MAP("h1nP9EYv3uc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("A1ThglSGUwA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("KuXcrMAQIMQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("itZ46iH14Vs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("PL20kjAXZZ4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("G12foE0S77E", SHADPS4_HLE_SUCCESS),
        HLE_MAP("PzDtD6eBXIM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("tu0SDPl+h88", SHADPS4_HLE_SUCCESS),
        HLE_MAP("6lZYZqQPfkY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("CWlBd2Ay1M4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("eBSSNIG6hMk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("UMpxor4AlKQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("pc4guaUPVqA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("SN7rTPHS+Cg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("+bRDRotfj0Y", SHADPS4_HLE_SUCCESS),
        HLE_MAP("3luF0xq0DkQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("DwAvlQGvf1o", SHADPS4_HLE_SUCCESS),
        HLE_MAP("kb24-4DLyNo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("OYmnApJ9q+U", SHADPS4_HLE_SUCCESS),
        HLE_MAP("g9uwUI3BlQ8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("voAQW45oKuo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ieP6jP138Qo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xz0YMi6BfNk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("msCER7Iibm8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("-XYmdxjOqyA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("uNu7j3pL2mQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("SgIY-XYA2Xg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("hsKd5c21sQc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("HuToUt1GQ8w", SHADPS4_HLE_SUCCESS),
        HLE_MAP("aoZKKNjlq3Y", SHADPS4_HLE_SUCCESS),
        HLE_MAP("0VFHv-Fa4w8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("52pL2GKkdjA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("v3vg2+cooYw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("zMgXM79jRhw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("+orZm32HB1s", SHADPS4_HLE_SUCCESS),
        HLE_MAP("LMSQUTxmGVg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("2-8NWLS8QSA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("v-AK1AxQhS0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("COwz3WBj+5s", SHADPS4_HLE_SUCCESS),
        HLE_MAP("AuTE0gFxZCI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Wz-4JZfeO9g", SHADPS4_HLE_SAVE_DATA_CLEAR_PROGRESS),
        HLE_MAP("XgvSuIdnMlw", SHADPS4_HLE_SAVE_DATA_GET_PARAM),
        HLE_MAP("ANmSWUiyyGQ", SHADPS4_HLE_SAVE_DATA_GET_PROGRESS),
        HLE_MAP("7Bt5pBC-Aco", SHADPS4_HLE_SAVE_DATA_MEMORY_GET_LEGACY),
        HLE_MAP("cGjO3wM3V28", SHADPS4_HLE_SAVE_DATA_ICON_LOAD),
        HLE_MAP("c88Yy54Mx0w", SHADPS4_HLE_SAVE_DATA_ICON_SAVE),
        HLE_MAP("h3YURzXGSVQ", SHADPS4_HLE_SAVE_DATA_MEMORY_SET_LEGACY),
        HLE_MAP("v7AAAMo0Lz4", SHADPS4_HLE_SAVE_DATA_MEMORY_SETUP_LEGACY),
        HLE_MAP("wiT9jeC7xPw", SHADPS4_HLE_SAVE_DATA_MEMORY_SYNC),
        HLE_MAP("VwadwBBBJ80", SHADPS4_HLE_SAVE_DATA_UMOUNT),
        HLE_MAP("z1JA8-iJt3k", SHADPS4_HLE_SAVE_DATA_BACKUP),
        HLE_MAP("RQOqDbk3bSU", SHADPS4_HLE_SAVE_DATA_CHECK_BACKUP),
        HLE_MAP("lU9YRFsgwSU", SHADPS4_HLE_SAVE_DATA_RESTORE_BACKUP),
        HLE_MAP("WAzWTZm1H+I", SHADPS4_HLE_SAVE_DATA_TRANSFERRING_MOUNT),
    };
    static const ShadPS4HLECompatMap http[] = {
        HLE_MAP("hvG6GfBMXg8", SHADPS4_HLE_HTTP_ABORT_REQUEST),
        HLE_MAP("JKl06ZIAl6A", SHADPS4_HLE_HTTP_ABORT_REQUEST),
        HLE_MAP("sWQiqKvYTVA", SHADPS4_HLE_HTTP_ABORT_REQUEST),
        HLE_MAP("Kiwv9r4IZCc", SHADPS4_HLE_HTTP_CREATE_CONNECTION),
        HLE_MAP("tsGVru3hCe8", SHADPS4_HLE_HTTP_CREATE_REQUEST_METHOD),
        HLE_MAP("rGNm+FjIXKk", SHADPS4_HLE_HTTP_CREATE_REQUEST),
        HLE_MAP("Aeu5wVKkF9w", SHADPS4_HLE_HTTP_CREATE_REQUEST_METHOD),
        HLE_MAP("1rpZqxdMRwQ", SHADPS4_HLE_HTTP_GET_OPTION),
        HLE_MAP("mmLexUbtnfY", SHADPS4_HLE_HTTP_GET_OPTION),
        HLE_MAP("7j9VcwnrZo4", SHADPS4_HLE_HTTP_GET_EPOLL),
        HLE_MAP("0onIrKx9NIE", SHADPS4_HLE_HTTP_GET_LAST_ERRNO),
        HLE_MAP("Wq4RNB3snSQ", SHADPS4_HLE_HTTP_GET_NONBLOCK),
        HLE_MAP("mSQCxzWTwVI", SHADPS4_HLE_HTTP_DISABLE_FLAGS),
        HLE_MAP("zJYi5br6ZiQ", SHADPS4_HLE_HTTP_DISABLE_FLAGS),
        HLE_MAP("f42K37mm5RM", SHADPS4_HLE_HTTP_ENABLE_FLAGS),
        HLE_MAP("I4+4hKttt1w", SHADPS4_HLE_HTTP_ENABLE_FLAGS),
        HLE_MAP("HRX1iyDoKR8", SHADPS4_HLE_HTTP_SET_OPTION),
        HLE_MAP("T-mGo9f3Pu4", SHADPS4_HLE_HTTP_SET_OPTION),
        HLE_MAP("0S9tTH0uqTU", SHADPS4_HLE_HTTP_SET_OPTION),
        HLE_MAP("8kzIXsRy1bY", SHADPS4_HLE_HTTP_SET_OPTION),
        HLE_MAP("i9mhafzkEi8", SHADPS4_HLE_HTTP_SET_OPTION),
        HLE_MAP("i+quCZCL+D8", SHADPS4_HLE_HTTP_SET_OPTION),
        HLE_MAP("mMcB2XIDoV4", SHADPS4_HLE_HTTP_SET_OPTION),
        HLE_MAP("yigr4V0-HTM", SHADPS4_HLE_HTTP_SET_OPTION),
        HLE_MAP("PTiFIUxCpJc", SHADPS4_HLE_HTTP_SET_CONTENT_LENGTH),
        HLE_MAP("K1d1LqZRQHQ", SHADPS4_HLE_HTTP_SET_OPTION),
        HLE_MAP("Tc-hAYDKtQc", SHADPS4_HLE_HTTP_SET_OPTION),
        HLE_MAP("a4VsZ4oqn68", SHADPS4_HLE_HTTP_SET_OPTION),
        HLE_MAP("xegFfZKBVlw", SHADPS4_HLE_HTTP_SET_OPTION),
        HLE_MAP("V-noPEjSB8c", SHADPS4_HLE_HTTP_GET_NONBLOCK),
        HLE_MAP("fmOs6MzCRqk", SHADPS4_HLE_HTTP_SET_NONBLOCK),
        HLE_MAP("hPTXo3bICzI", SHADPS4_HLE_HTTP_PARSE_HEADER),
        HLE_MAP("Qq8SfuJJJqE", SHADPS4_HLE_HTTP_PARSE_STATUS),
        HLE_MAP("zNGh-zoQTD0", SHADPS4_HLE_HTTP_REMOVE_HEADER),
        HLE_MAP("7WcNoAI9Zcw", SHADPS4_HLE_HTTPS_FREE_CA_LIST),
        HLE_MAP("gcUjwU3fa0M", SHADPS4_HLE_HTTPS_GET_CA_LIST),
        HLE_MAP("DK+GoXCNT04", SHADPS4_HLE_HTTPS_LOAD_CERT),
        HLE_MAP("zXqcE0fizz0", SHADPS4_HLE_HTTPS_UNLOAD_CERT),
        HLE_MAP("5LZA+KPISVA", SHADPS4_HLE_HTTP_URI_BUILD),
        HLE_MAP("YuOW3dDAKYc", SHADPS4_HLE_HTTP_URI_ESCAPE),
        HLE_MAP("3lgQ5Qk42ok", SHADPS4_HLE_HTTP_URI_MERGE),
        HLE_MAP("IWalAn-guFs", SHADPS4_HLE_HTTP_URI_PARSE),
        HLE_MAP("mUU363n4yc0", SHADPS4_HLE_HTTP_URI_SWEEP),
        HLE_MAP("thTS+57zoLM", SHADPS4_HLE_HTTP_URI_UNESCAPE),
    };
    static const ShadPS4HLECompatMap app_content[] = {
        HLE_MAP("VANhIWcqYak", SHADPS4_HLE_APP_CONTENT_MOUNT),
        HLE_MAP("99b82IKXpH4", SHADPS4_HLE_APP_CONTENT_GET_PARAM_INT),
        HLE_MAP("m47juOmH0VE", SHADPS4_HLE_APP_CONTENT_GET_INFO),
        HLE_MAP("xnd8BJzAxmk", SHADPS4_HLE_APP_CONTENT_GET_INFO_LIST),
        HLE_MAP("XTWR0UXvcgs", SHADPS4_HLE_APP_CONTENT_GET_KEY),
        HLE_MAP("R9lA82OraNs", SHADPS4_HLE_APP_CONTENT_INIT),
        HLE_MAP("buYbeLOGWmA", SHADPS4_HLE_APP_CONTENT_TEMP_MOUNT),
    };
    static const ShadPS4HLECompatMap move[] = {
        HLE_MAP("j1ITE-EoJmE", SHADPS4_HLE_MOVE_INIT),
        HLE_MAP("HzC60MfjJxU", SHADPS4_HLE_MOVE_OPEN),
        HLE_MAP("GWXTyxs4QbE", SHADPS4_HLE_MOVE_QUERY),
        HLE_MAP("ttU+JOhShl4", SHADPS4_HLE_MOVE_QUERY),
        HLE_MAP("f2bcpK6kJfg", SHADPS4_HLE_MOVE_QUERY),
        HLE_MAP("y5h7f8H1Jnk", SHADPS4_HLE_MOVE_QUERY),
        HLE_MAP("IFQwtT2CeY0", SHADPS4_HLE_MOVE_QUERY),
        HLE_MAP("T8KYHPs1JE8", SHADPS4_HLE_MOVE_QUERY),
        HLE_MAP("zuxWAg3HAac", SHADPS4_HLE_MOVE_RESET),
        HLE_MAP("XX6wlxpHyeo", SHADPS4_HLE_MOVE_CLOSE),
        HLE_MAP("tsZi60H4ypY", SHADPS4_HLE_MOVE_TERM),
    };
    static const ShadPS4HLECompatMap zlib_hle[] = {
        HLE_MAP("m1YErdIXCp4", SHADPS4_HLE_ZLIB_INIT),
        HLE_MAP("6na+Sa-B83w", SHADPS4_HLE_ZLIB_FINALIZE),
        HLE_MAP("TLar1HULv1Q", SHADPS4_HLE_ZLIB_INFLATE),
        HLE_MAP("uB8VlDD4e0s", SHADPS4_HLE_ZLIB_WAIT),
        HLE_MAP("2eDcGHC0YaM", SHADPS4_HLE_ZLIB_RESULT),
    };
    static const ShadPS4HLECompatMap png_enc[] = {
        HLE_MAP("7aGTPfrqT9s", SHADPS4_HLE_PNG_ENC_CREATE),
        HLE_MAP("RUrWdwTWZy8", SHADPS4_HLE_PNG_ENC_DELETE),
        HLE_MAP("xgDjJKpcyHo", SHADPS4_HLE_PNG_ENC_ENCODE),
        HLE_MAP("9030RnBDoh4", SHADPS4_HLE_PNG_ENC_QUERY),
    };
    static const ShadPS4HLECompatMap disc_map[] = {
        HLE_MAP("fl1eoDnwQ4s", SHADPS4_HLE_DISC_MAP_NO_BITMAP),
        HLE_MAP("lbQKqsERhtE", SHADPS4_HLE_DISC_MAP_NO_BITMAP),
        HLE_MAP("fJgP+wqifno", SHADPS4_HLE_DISC_MAP_ZERO),
        HLE_MAP("ioKMruft1ek", SHADPS4_HLE_DISC_MAP_NO_BITMAP),
        HLE_MAP("5+vOlukvkfg", SHADPS4_HLE_DISC_MAP_NO_BITMAP),
    };
    static const ShadPS4HLECompatMap font_core[] = {
        HLE_MAP("nWrfPI4Okmg", SHADPS4_HLE_FONT_CREATE_LIBRARY),
        HLE_MAP("n590hj5Oe-k", SHADPS4_HLE_FONT_CREATE_LIBRARY_EDITION),
        HLE_MAP("u5fZd3KZcs0", SHADPS4_HLE_FONT_CREATE_RENDERER),
        HLE_MAP("WaSFJoRWXaI", SHADPS4_HLE_FONT_CREATE_RENDERER_EDITION),
        HLE_MAP("FXP359ygujs", SHADPS4_HLE_FONT_DESTROY_LIBRARY),
        HLE_MAP("exAxkyVLt0s", SHADPS4_HLE_FONT_DESTROY_RENDERER),
        HLE_MAP("RvXyHMUiLhE", SHADPS4_HLE_FONT_OPEN),
        HLE_MAP("KXUpebrFk1U", SHADPS4_HLE_FONT_OPEN),
        HLE_MAP("cKYtVmeSTcw", SHADPS4_HLE_FONT_OPEN),
        HLE_MAP("JzCH3SCFnAU", SHADPS4_HLE_FONT_OPEN_INSTANCE),
        HLE_MAP("CkVmLoCNN-8", SHADPS4_HLE_FONT_GET_SCALE),
        HLE_MAP("GoF2bhB7LYk", SHADPS4_HLE_FONT_GET_SCALE),
        HLE_MAP("EY38A01lq2k", SHADPS4_HLE_FONT_GET_SCALE),
        HLE_MAP("FEafYUcxEGo", SHADPS4_HLE_FONT_GET_SCALE),
        HLE_MAP("N1EBMeGhf7E", SHADPS4_HLE_FONT_SET_SCALE),
        HLE_MAP("sw65+7wXCKE", SHADPS4_HLE_FONT_SET_SCALE),
        HLE_MAP("6vGCkkQJOcI", SHADPS4_HLE_FONT_SET_SCALE),
        HLE_MAP("nMZid4oDfi4", SHADPS4_HLE_FONT_SET_SCALE),
        HLE_MAP("ynSqYL8VpoA", SHADPS4_HLE_FONT_GET_SLANT),
        HLE_MAP("Gqa5Pp7y4MU", SHADPS4_HLE_FONT_GET_SLANT),
        HLE_MAP("TMtqoFQjjbA", SHADPS4_HLE_FONT_SET_SLANT),
        HLE_MAP("lz9y9UFO2UU", SHADPS4_HLE_FONT_SET_SLANT),
        HLE_MAP("d7dDgRY+Bzw", SHADPS4_HLE_FONT_GET_WEIGHT),
        HLE_MAP("woOjHrkjIYg", SHADPS4_HLE_FONT_GET_WEIGHT),
        HLE_MAP("v0phZwa4R5o", SHADPS4_HLE_FONT_SET_WEIGHT),
        HLE_MAP("XIGorvLusDQ", SHADPS4_HLE_FONT_SET_WEIGHT),
        HLE_MAP("L97d+3OgMlE", SHADPS4_HLE_FONT_GET_METRICS),
        HLE_MAP("IQtleGLL5pQ", SHADPS4_HLE_FONT_GET_METRICS),
        HLE_MAP("imxVx8lm+KM", SHADPS4_HLE_FONT_GET_LAYOUT_H),
        HLE_MAP("3BrWWFU+4ts", SHADPS4_HLE_FONT_GET_LAYOUT_V),
        HLE_MAP("sDuhHGNhHvE", SHADPS4_HLE_FONT_GET_KERNING),
        HLE_MAP("6DFUkCwQLa8", SHADPS4_HLE_FONT_CHAR_BIDI),
        HLE_MAP("zN3+nuA0SFQ", SHADPS4_HLE_FONT_CHAR_CODE),
        HLE_MAP("mxgmMj-Mq-o", SHADPS4_HLE_FONT_CHAR_ORDER),
        HLE_MAP("-P6X35Rq2-E", SHADPS4_HLE_FONT_CHAR_FORMAT),
        HLE_MAP("SaRlqtqaCew", SHADPS4_HLE_FONT_CHAR_SPACE),
        HLE_MAP("6Gqlv5KdTbU", SHADPS4_HLE_FONT_CHAR_PREV),
        HLE_MAP("BkjBP+YC19w", SHADPS4_HLE_FONT_CHAR_NEXT),
        HLE_MAP("la2AOWnHEAc", SHADPS4_HLE_FONT_STYLE_INIT),
        HLE_MAP("2QfqfeLblbg", SHADPS4_HLE_FONT_STYLE_GET_SCALE),
        HLE_MAP("7x2xKiiB7MA", SHADPS4_HLE_FONT_STYLE_GET_SCALE),
        HLE_MAP("da4rQ4-+p-4", SHADPS4_HLE_FONT_STYLE_SET_SCALE),
        HLE_MAP("O997laxY-Ys", SHADPS4_HLE_FONT_STYLE_SET_SCALE),
        HLE_MAP("VSw18Aqzl0U", SHADPS4_HLE_FONT_STYLE_GET_DPI),
        HLE_MAP("dB4-3Wdwls8", SHADPS4_HLE_FONT_STYLE_SET_DPI),
        HLE_MAP("lOfduYnjgbo", SHADPS4_HLE_FONT_STYLE_GET_SLANT),
        HLE_MAP("394sckksiCU", SHADPS4_HLE_FONT_STYLE_SET_SLANT),
        HLE_MAP("dUmABkAnVgk", SHADPS4_HLE_FONT_STYLE_UNSET_SLANT),
        HLE_MAP("HIUdjR-+Wl8", SHADPS4_HLE_FONT_STYLE_GET_WEIGHT),
        HLE_MAP("faw77-pEBmU", SHADPS4_HLE_FONT_STYLE_SET_WEIGHT),
        HLE_MAP("hwsuXgmKdaw", SHADPS4_HLE_FONT_STYLE_UNSET_WEIGHT),
        HLE_MAP("bePC0L0vQWY", SHADPS4_HLE_FONT_STYLE_UNSET_SCALE),
        HLE_MAP("gdUCnU0gHdI", SHADPS4_HLE_FONT_SURFACE_INIT),
        HLE_MAP("vRxf4d0ulPs", SHADPS4_HLE_FONT_SURFACE_SCISSOR),
        HLE_MAP("3OdRkSjOcog", SHADPS4_HLE_FONT_BIND_RENDERER),
        HLE_MAP("Z2cdsqJH+5k", SHADPS4_HLE_FONT_REBIND_RENDERER),
        HLE_MAP("1QjhKxrsOB8", SHADPS4_HLE_FONT_UNBIND_RENDERER),
        HLE_MAP("LzmHDnlcwfQ", SHADPS4_HLE_FONT_GET_LIBRARY),
        HLE_MAP("CUKn5pX-NVY", SHADPS4_HLE_FONT_LIBRARY_OPTION),
        HLE_MAP("I9R5VC6eZWo", SHADPS4_HLE_FONT_LIBRARY_OPTION),
        HLE_MAP("kihFGYJee7o", SHADPS4_HLE_FONT_LIBRARY_OPTION),
        HLE_MAP("mz2iTY0MK4A", SHADPS4_HLE_FONT_LIBRARY_OPTION),
        HLE_MAP("SsRbbCiWoGw", SHADPS4_HLE_FONT_LIBRARY_OPTION),
        HLE_MAP("amcmrY62BD4", SHADPS4_HLE_FONT_RENDERER_GET_OUTLINE_SIZE),
        HLE_MAP("ai6AfGrBs4o", SHADPS4_HLE_FONT_RENDERER_OPTION),
        HLE_MAP("ydF+WuH0fAk", SHADPS4_HLE_FONT_RENDERER_OPTION),
        HLE_MAP("I1acwR7Qp8E", SHADPS4_HLE_FONT_SET_DPI),
        HLE_MAP("whrS4oksXc4", SHADPS4_HLE_FONT_MEMORY_INIT),
        HLE_MAP("h6hIgxXEiEc", SHADPS4_HLE_FONT_MEMORY_TERM),
        HLE_MAP("oaJ1BpN2FQk", SHADPS4_HLE_FONT_TEXT_SOURCE_INIT),
        HLE_MAP("VRFd3diReec", SHADPS4_HLE_FONT_TEXT_SOURCE_REWIND),
        HLE_MAP("eCRMCSk96NU", SHADPS4_HLE_FONT_TEXT_SOURCE_SET_FONT),
        HLE_MAP("OqQKX0h5COw", SHADPS4_HLE_FONT_TEXT_SOURCE_SET_FORM),
        HLE_MAP("MO24vDhmS4E", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("LHDoRWVFGqk", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("SSCaczu2aMQ", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("C-4Qw5Srlyw", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("PXlA0M8ax40", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("XUfSWpLhrUw", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("lNnUqa1zA-M", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("ntrc3bEWlvQ", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("9kTbF59TjLs", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("nJavPEdMDvM", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("JCnVgZgcucs", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("R1T4i+DOhNY", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("3G4zhgKuxE8", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("kAenWy1Zw5o", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("i6UNdSig1uE", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("0hr-w30SjiI", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("ObkDGDBsVtw", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("+B-xlbiWDJ4", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("o1vIEHeb6tw", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("hq5LffQjz-s", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("Avv7OApgCJk", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("fljdejMcG1c", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("fD5rqhEXKYQ", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("W-2WOXEHGck", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("f4Onl7efPEY", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("BbCZjJizU4A", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("oM+XCzVG3oM", SHADPS4_HLE_FONT_UNSUPPORTED),
        HLE_MAP("Xx974EW-QFY", SHADPS4_HLE_FONT_UNSUPPORTED),
    };
    static const ShadPS4HLECompatMap audio_out[] = {
        HLE_MAP("cx2dYFbzIAg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("tKumjQSzhys", SHADPS4_HLE_SUCCESS),
        HLE_MAP("5ChfcHOf3SM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Iz9X7ISldhs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("9RVIoocOVAo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("n7KgxE8rOuE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("WBAO6-n0-4M", SHADPS4_HLE_SUCCESS),
        HLE_MAP("O3FM2WXIJaI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ol4LbeTG8mc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("r1V9IFEE+Ts", SHADPS4_HLE_SUCCESS),
        HLE_MAP("wZakRQsWGos", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xjjhT5uw08o", SHADPS4_HLE_SUCCESS),
        HLE_MAP("DsST7TNsyfo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("4UlW3CSuCa4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Xcj8VTtnZw0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("I3Fwcmkg5Po", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Y3lXfCFEWFY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("-00OAutAw+c", SHADPS4_HLE_SUCCESS),
        HLE_MAP("RqmKxBqB8B4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Ptlts326pds", SHADPS4_HLE_AUDIO_OUT_GET_LAST_OUTPUT_TIME),
        HLE_MAP("GrQ9s4IrNaQ", SHADPS4_HLE_AUDIO_OUT_GET_PORT_STATE),
        HLE_MAP("c7mVozxJkPU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("pWmS7LajYlo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("oPLghhAWgMM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("5+r7JYHpkXg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("R5hemoKKID8", SHADPS4_HLE_AUDIO_OUT_GET_SYSTEM_STATE),
        HLE_MAP("n16Kdoxnvl0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("r+qKw+ueD+Q", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xX4RLegarbg", SHADPS4_HLE_AUDIO_OUT_MASTERING_INIT),
        HLE_MAP("4055yaUg3EY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("RVWtUgoif5o", SHADPS4_HLE_SUCCESS),
        HLE_MAP("-LXhcGARw3k", SHADPS4_HLE_SUCCESS),
        HLE_MAP("qLpSK75lXI4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("MapHTgeogbk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("YZaq+UKbriQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xyT8IUCL3CI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("o4OLQQqqA90", SHADPS4_HLE_SUCCESS),
        HLE_MAP("QHq2ylFOZ0k", SHADPS4_HLE_SUCCESS),
        HLE_MAP("r9KGqGpwTpg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("08MKi2E-RcE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("18IVGrIQDU4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("h0o+D4YYr1k", SHADPS4_HLE_SUCCESS),
        HLE_MAP("KI9cl22to7E", SHADPS4_HLE_SUCCESS),
        HLE_MAP("wVwPU50pS1c", SHADPS4_HLE_AUDIO_OUT_SET_MIX_LEVEL_PAD_SPK),
        HLE_MAP("eeRsbeGYe20", SHADPS4_HLE_SUCCESS),
        HLE_MAP("IZrItPnflBM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Gy0ReOgXW00", SHADPS4_HLE_SUCCESS),
        HLE_MAP("oRBFflIrCg0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ae-IVPMSWjU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("d3WL2uPE1eE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("X7Cfsiujm8Y", SHADPS4_HLE_SUCCESS),
        HLE_MAP("rho9DH-0ehs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("I91P0HAPpjw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("uo+eoPzdQ-s", SHADPS4_HLE_SUCCESS),
        HLE_MAP("AImiaYFrKdc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("teCyKKZPjME", SHADPS4_HLE_SUCCESS),
        HLE_MAP("95bdtHdNUic", SHADPS4_HLE_SUCCESS),
        HLE_MAP("oRJZnXxok-M", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Tf9-yOJwF-A", SHADPS4_HLE_SUCCESS),
        HLE_MAP("y2-hP-KoTMI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("YV+bnMvMfYg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("JEHhANREcLs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("9CHWVv6r3Dg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Mt7JB3lOyJk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("7UsdDOEvjlk", SHADPS4_HLE_SUCCESS),
    };
    static const ShadPS4HLECompatMap net[] = {
        HLE_MAP("9T2pDF2Ryqg", SHADPS4_HLE_NET_HTONL),
        HLE_MAP("3CHi1K1wsCQ", SHADPS4_HLE_NET_HTONLL),
        HLE_MAP("pQGpHYopAIY", SHADPS4_HLE_NET_NTOHL),
        HLE_MAP("tOrRi-v3AOM", SHADPS4_HLE_NET_NTOHLL),
        HLE_MAP("Rbvt+5Y2iEw", SHADPS4_HLE_NET_NTOHS),
        HLE_MAP("2eKbgcboJso", SHADPS4_HLE_NET_SENDMSG),
        HLE_MAP("wvuUDv0jrMI", SHADPS4_HLE_NET_RECVMSG),
        HLE_MAP("6Oc0bLsIYe0", SHADPS4_HLE_NET_GET_MAC_ADDRESS),
        HLE_MAP("Inp1lfL+Jdw", SHADPS4_HLE_NET_EPOLL_DESTROY),
        HLE_MAP("J5i3hiLJMPk", SHADPS4_HLE_NET_RESOLVER_GET_ERROR),
        HLE_MAP("RCCY01Xd+58", SHADPS4_HLE_NET_RESOLVER_NTOA_MULTIPLE),
        HLE_MAP("zl35YNs9jnI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("sT4nBQKUPqM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Apb4YDxKsRI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("zvzWA5IZMsg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Xn2TA2QhxHc", SHADPS4_HLE_NET_INET_PTON_EX),
        HLE_MAP("hoOAofhhRvE", SHADPS4_HLE_NET_GETSOCKNAME),
        /* Explicit no-op providers mirrored from shadPS4. */
        HLE_MAP("+3KMyS93TOs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("+S-2-jlpaBo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("+ezgWao0wo8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("-Mi5hNiWC4c", SHADPS4_HLE_SUCCESS),
        HLE_MAP("0-XSSp1kEFM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("0239JNsI6PE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("0MT2l3uIX7c", SHADPS4_HLE_SUCCESS),
        HLE_MAP("0g0qIuPN3ZQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("0sesmAYH3Lk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("15Ywg-ZsSl0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("18KNgSvYx+Y", SHADPS4_HLE_SUCCESS),
        HLE_MAP("1j4DZ5dXbeQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("221fvqVs+sQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("273-I-zD8+8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("2ee14ktE1lw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("2uSWyOKYc1M", SHADPS4_HLE_SUCCESS),
        HLE_MAP("3T5aIe-7L84", SHADPS4_HLE_SUCCESS),
        HLE_MAP("3WzWV86AJ3w", SHADPS4_HLE_SUCCESS),
        HLE_MAP("3qG7UJy2Fq8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("3zRdT3O2Kxo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("4wDGvfhmkmk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("4zLOHbt3UFk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("5Yl1uuh5i-A", SHADPS4_HLE_SUCCESS),
        HLE_MAP("5isaotjMWlA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("5lrSEHdqyos", SHADPS4_HLE_SUCCESS),
        HLE_MAP("6A6EweB3Dto", SHADPS4_HLE_SUCCESS),
        HLE_MAP("6AJE2jKg-c0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("6AN7OlSMWk0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("6HNbayHPL7c", SHADPS4_HLE_SUCCESS),
        HLE_MAP("6MojQ8uFHEI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("6Nx1hIQL9h8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("6uYcvVjH7Ms", SHADPS4_HLE_SUCCESS),
        HLE_MAP("7Z1hhsEmkQU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("7xYdUWg1WdY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("84MgU4MMTLQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("8Kh+1eidI3c", SHADPS4_HLE_SUCCESS),
        HLE_MAP("8s+T0bJeyLQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("9mIcUExH34w", SHADPS4_HLE_SUCCESS),
        HLE_MAP("9oiOWQ5FMws", SHADPS4_HLE_SUCCESS),
        HLE_MAP("AzqoBha7js4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("B-M6KjO8-+w", SHADPS4_HLE_SUCCESS),
        HLE_MAP("BTUvkWzrP68", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Bu+L5r1lKRg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("C-+JPjaEhdA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Cidi9Y65mP8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Cyjl1yzi1qY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("DnB6WJ91HGg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("DrZuCQDnm3w", SHADPS4_HLE_SUCCESS),
        HLE_MAP("E3oH1qsdqCA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("E8dTcvQw3hg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("EAl7xvi7nXg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Ea2NaVMQNO8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Eh+Vqkrrc00", SHADPS4_HLE_SUCCESS),
        HLE_MAP("FDHr4Iz7dQU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("FIV95WE1EuE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("FU6NK4RHQVE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("G3O2j9f5z00", SHADPS4_HLE_SUCCESS),
        HLE_MAP("GA5ZDaLtUBE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("GAtITrgxKDE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("H5WHYRfDkR0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("HJt+4x-CnY0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("HKIa-WH0AZ4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("HoV-GJyx7YY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("IkBCxG+o4Nk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("JK1oZe4UysY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("JQk8ck8vnPY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("K4o48GTNbSc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("K7RlrTkI-mw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("KhQxhlEslo0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("KirVfZbqniw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("MDbg-oAj8Aw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("MzA1YrRE6rA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("NP5gxDeYhIM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("P+0ePpDfUAQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("P3AeWBvPrkg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("P4zZXE7bpsA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("PDkapOwggRw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("PNDDxnqqtk4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("POrSEl8zySw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("PcdLABhYga4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("PgNI+j4zxzM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Pkx0lwWVzmQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Q6T-zIblNqk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Q7ee2Uav5f8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("QCbvCx9HL30", SHADPS4_HLE_SUCCESS),
        HLE_MAP("QFPjG6rqeZg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("QGOqGPnk5a4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("QJbV3vfBQ8Q", SHADPS4_HLE_SUCCESS),
        HLE_MAP("QO7+2E3cD-U", SHADPS4_HLE_SUCCESS),
        HLE_MAP("QlRJWya+dtE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("QltDK6wWqF0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("RuVwHEW6dM4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("TCZyE2YI1uM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("TwjkDIPdZ1Q", SHADPS4_HLE_SUCCESS),
        HLE_MAP("U1q6DrPbY6k", SHADPS4_HLE_SUCCESS),
        HLE_MAP("UMlVCy7RX1s", SHADPS4_HLE_SUCCESS),
        HLE_MAP("V5q6gvEJpw4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("VZgoeBxPXUQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("WxislcDAW5I", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Wzv6dngR-DQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("XCuA-GqjA-k", SHADPS4_HLE_SUCCESS),
        HLE_MAP("XfV-XBCuhDo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Xma8yHmV+TQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("YWTpt45PxbI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Yr3UeApLWTY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Z-8Jda650Vk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ZLdJyQJUMkM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ZRAJo-A-ukc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ZvKgNrrLCCQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("a6sS6iSE0IA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ahiOMqoYYMc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("atGfzCaXMak", SHADPS4_HLE_SUCCESS),
        HLE_MAP("b+LixqREH6A", SHADPS4_HLE_SUCCESS),
        HLE_MAP("b-bFZvNV59I", SHADPS4_HLE_SUCCESS),
        HLE_MAP("b9Ft65tqvLk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("bghgkeLKq1Q", SHADPS4_HLE_SUCCESS),
        HLE_MAP("bjrzRLFali0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("bonnMiDoOZg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("c8IRpl4L74I", SHADPS4_HLE_SUCCESS),
        HLE_MAP("cEMX1VcPpQ8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("cMA8f6jI6s0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("cTGkc6-TBlI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("cWGGXoeZUzA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("cYW1ISGlOmo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("dbrSNEuZfXI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ejwa0hWWhDs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("eszLdtIMfQE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("eyLyLJrdEOU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("fCa7-ihdRdc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("fHr45B97n0U", SHADPS4_HLE_SUCCESS),
        HLE_MAP("g4DKkzV2qC4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ge7g15Sqhks", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ghqRRVQxqKo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("hLuXdjHnhiI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("hvCXMwd45oc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ix4LWXd12F0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("j-Op3ibRJaQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("j6pkkO2zJtg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("jzP0MoZpYnI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("k1V1djYpk7k", SHADPS4_HLE_SUCCESS),
        HLE_MAP("kJlYH5uMAWI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("lDTIbqNs0ps", SHADPS4_HLE_SUCCESS),
        HLE_MAP("lFJb+BlPK1c", SHADPS4_HLE_SUCCESS),
        HLE_MAP("mCLdiNIKtW0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("mOUkgTaSkJU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("mqoB+LN0pW8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("n-IAZb7QB1Y", SHADPS4_HLE_SUCCESS),
        HLE_MAP("nCL0NyZsd5A", SHADPS4_HLE_SUCCESS),
        HLE_MAP("nTJqXsbSS1I", SHADPS4_HLE_SUCCESS),
        HLE_MAP("oGEBX0eXGFs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("p2vxsE2U3RQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("pF3Vy1iZ5bs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("pRbEzaV30qI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("pfn3Fha1ydc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("q8j9OSdnN1Y", SHADPS4_HLE_SUCCESS),
        HLE_MAP("rMyh97BU5pY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("rX30iWQqqzg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("s31rYkpIMMQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("sAleh-BoxLA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("tB3BB8AsrjU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("tk0p0JmiBkM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("toi8xxcSfJ0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("tvdzQkm+UaY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("uNTluLfYgS8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("v6M4txecCuo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("vbZLomImmEE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("vjwKTGa21f0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("w21YgGGNtBk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("wIGold7Lro0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("wmoIm94hqik", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xHq87H78dho", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xTcttXJ3Utg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xZ54Il-u1vs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xaOTiuxIQNY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xwWm8jzrpeM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("yaVAdLDxUj0", SHADPS4_HLE_SUCCESS),
    };
    static const ShadPS4HLECompatMap netctl[] = {
        HLE_MAP("gky0+oaNM4k", SHADPS4_HLE_NETCTL_INIT),
        HLE_MAP("Z4wwCFiBELQ", SHADPS4_HLE_NETCTL_TERM),
        HLE_MAP("uBPlr0lbuiI", SHADPS4_HLE_NETCTL_GET_STATE),
        HLE_MAP("+lxqIKeU9UY", SHADPS4_HLE_NETCTL_GET_STATE),
        HLE_MAP("0cBgduPRR+M", SHADPS4_HLE_NETCTL_GET_RESULT),
        HLE_MAP("H5yARg37U5g", SHADPS4_HLE_NETCTL_GET_RESULT),
        HLE_MAP("obuxdTiwkF8", SHADPS4_HLE_NETCTL_GET_INFO),
        HLE_MAP("Jy1EO5GdlcM", SHADPS4_HLE_NETCTL_GET_INFO),
        HLE_MAP("JO4yuTuMoKI", SHADPS4_HLE_NETCTL_GET_NAT_INFO),
        HLE_MAP("UJ+Z7Q+4ck0", SHADPS4_HLE_NETCTL_REGISTER_CALLBACK),
        HLE_MAP("1NE9OWdBIww", SHADPS4_HLE_NETCTL_REGISTER_CALLBACK),
        HLE_MAP("Rqm2OnZMCz0", SHADPS4_HLE_NETCTL_UNREGISTER_CALLBACK),
        HLE_MAP("hIUVeUNxAwc", SHADPS4_HLE_NETCTL_UNREGISTER_CALLBACK),
        HLE_MAP("iQw3iQPhvUQ", SHADPS4_HLE_NETCTL_CHECK_CALLBACK),
        HLE_MAP("wIsKy+TfeLs", SHADPS4_HLE_NETCTL_REGISTER_CALLBACK),
        /* Explicit no-op providers mirrored from shadPS4. */
        HLE_MAP("0lViPaTB-R8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("19Ec7WkMFfQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("1HSvkN9oxO4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("2EfjPXVPk3s", SHADPS4_HLE_SUCCESS),
        HLE_MAP("2Ny2lzU3o9w", SHADPS4_HLE_SUCCESS),
        HLE_MAP("2oUqKR5odGc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("3pxwYqHzGcw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("4jkLJc954+Q", SHADPS4_HLE_SUCCESS),
        HLE_MAP("5oLJoOVBbGU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("6uvAl4RlEyk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("8OJ86vFucfo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("8eyH37Ns8tk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("9Dxg7XSlr2s", SHADPS4_HLE_SUCCESS),
        HLE_MAP("9y4IcsJdTCc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("AKZOzsb9whc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("BXW9b3R1Nw4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("DjuqqqV08Nk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("DufQZgH5ISc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("FEdkOG1VbQo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("FdN+edNRtiw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("G4vltQ0Vs+0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("GqETL5+INhU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("HCD46HVTyQg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("HMvaHoZWsn8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Hjxpy28aID8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ID+Gq3Ddzbg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("JXlI9EZVjf4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("L97eAHI0xxs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("LEn8FGztKWc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("LJYiiIS4HB0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("LXADzTIzM9I", SHADPS4_HLE_SUCCESS),
        HLE_MAP("NEtnusbZyAs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("NpTcFtaQ-0E", SHADPS4_HLE_SUCCESS),
        HLE_MAP("O8Fk4w5MWss", SHADPS4_HLE_SUCCESS),
        HLE_MAP("R-4a9Yh4tG8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("UF6H6+kjyQs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("VQl16Q+qXeY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("WRvDk2syatE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Wn-+887Lt2s", SHADPS4_HLE_SUCCESS),
        HLE_MAP("XtClSOC1xcU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("YALqoY4aeY0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("YtAnCkTR0K4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("YtTwZ3pa4aQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("aPpic8K75YA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("amqSGH8l--s", SHADPS4_HLE_SUCCESS),
        HLE_MAP("arAQRFlwqaA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("by9cbB7JGJE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("c+aYh130SV0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("cv5Y2efOTeg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("eCUIlA2t5CE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("gvnJPMkSoAY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("hfkLVdXmfnU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("hhTsdv99azU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ipqlpcIqRsQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("irV8voIAHDw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("mEUt-phGd5E", SHADPS4_HLE_SUCCESS),
        HLE_MAP("meFMaDpdsVI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("mjFgpqNavHg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ofGsK+xoAaM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ouyROWhGUbM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("pQLJV5SEAqk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("pmjobSVHuY0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("qOefcpoSs0k", SHADPS4_HLE_SUCCESS),
        HLE_MAP("qhZbOi+2qLY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("r-pOyN6AhsM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("reIsHryCDx4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("rqkh2kXvLSw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("saYB0b2ZWtI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("sgWeDrEt24U", SHADPS4_HLE_SUCCESS),
        HLE_MAP("teuK4QnJTGg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("u5oqtlIP+Fw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("urWaUWkEGZg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("vdsTa93atXY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("vv6g8zoanL4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("wP0Ab2maR1Y", SHADPS4_HLE_SUCCESS),
        HLE_MAP("x+cnsAxKSHo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("x9bSmRSE+hc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xstcTqAhTys", SHADPS4_HLE_SUCCESS),
    };
    static const ShadPS4HLECompatMap http2[] = {
        HLE_MAP("3JCe3lCbQ8A", SHADPS4_HLE_HTTP_INIT),
        HLE_MAP("YiBUtz-pGkc", SHADPS4_HLE_HTTP_TERM),
        HLE_MAP("+wCt7fCijgk", SHADPS4_HLE_HTTP_CREATE_TEMPLATE),
        HLE_MAP("pDom5-078DA", SHADPS4_HLE_HTTP_DELETE_TEMPLATE),
        HLE_MAP("mmyOCxQMVYQ", SHADPS4_HLE_HTTP2_CREATE_REQUEST),
        HLE_MAP("c8D9qIjo8EY", SHADPS4_HLE_HTTP_DELETE_REQUEST),
        HLE_MAP("nrPfOE8TQu0", SHADPS4_HLE_HTTP_ADD_HEADER),
        HLE_MAP("IZ-qjhRqvjk", SHADPS4_HLE_HTTP2_ABORT_REQUEST),
        HLE_MAP("FSAFOzi0FpM", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("jHdP0CS4ZlA", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("jjFahkBPCYs", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("Wwj6HbB2mOo", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("b9AvoIaOuHI", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("n8hMLe31OPA", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("-HIO4VT87v8", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("jrVHsKCXA0g", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("uRosf8GQbHQ", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("09tk+kIA1Ns", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("UL4Fviw+IAM", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("izvHhqgDt44", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("BJgi0CH7al4", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("Gcjh+CisAZM", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("ACjtE27aErY", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("XPtW45xiLHk", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("YrWX+DhPHQY", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("VYMxTcBqSE0", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("B37SruheQ5Y", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("EWcwMpbr5F8", SHADPS4_HLE_HTTP2_REQUEST_OPTION),
        HLE_MAP("rbqZig38AT8", SHADPS4_HLE_HTTP_SEND_REQUEST),
        HLE_MAP("A+NVAFu4eCg", SHADPS4_HLE_HTTP_SEND_REQUEST),
        HLE_MAP("QygCNNmbGss", SHADPS4_HLE_HTTP_READ_DATA),
        HLE_MAP("bGN-6zbo7ms", SHADPS4_HLE_HTTP_READ_DATA),
        HLE_MAP("9XYJwCf3lEA", SHADPS4_HLE_HTTP_GET_STATUS),
        HLE_MAP("-rdXUi2XW90", SHADPS4_HLE_HTTP_GET_HEADERS),
        HLE_MAP("o0DBQpFE13o", SHADPS4_HLE_HTTP_GET_LENGTH),
        /* Explicit no-op providers mirrored from shadPS4. */
        HLE_MAP("5VlQSzXW-SQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("6a0N6GPD7RM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("B5ibZI5UlzU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("GQFGj0rYX+A", SHADPS4_HLE_SUCCESS),
        HLE_MAP("IX23slKvtQI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("JlFGR4v50Kw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("MOp-AUhdfi8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("McYmUpQ3-DY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("N4UfjvWJsMw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("O9ync3F-JVI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("WeuDjj5m4YU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("eij7UzkUqK8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("flPxnowtvWY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("klwUy2Wg+q8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("m-OL13q8AI8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("mPKVhQqh2Es", SHADPS4_HLE_SUCCESS),
        HLE_MAP("o7+WXe4WadE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("od5QCZhZSfw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("otUQuZa-mv0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("zdtXKn9X7no", SHADPS4_HLE_SUCCESS),
    };
    static const ShadPS4HLECompatMap ajm[] = {
        HLE_MAP("dl+4eHSzUu4", SHADPS4_HLE_AJM_INITIALIZE),
        HLE_MAP("MHur6qCsUus", SHADPS4_HLE_AJM_FINALIZE),
        HLE_MAP("Q3dyFuwGn64", SHADPS4_HLE_AJM_MODULE_REGISTER),
        HLE_MAP("AxoDrINp4J8", SHADPS4_HLE_AJM_INSTANCE_CREATE),
        HLE_MAP("RbLbuKv8zho", SHADPS4_HLE_AJM_INSTANCE_DESTROY),
        HLE_MAP("diXjQNiMu-s", SHADPS4_HLE_AJM_INSTANCE_CODEC_TYPE),
        HLE_MAP("bkRHEYG6lEM", SHADPS4_HLE_AJM_MEMORY),
        HLE_MAP("pIpGiaYkHkM", SHADPS4_HLE_AJM_MEMORY),
        HLE_MAP("fFFkk0xfGWs", SHADPS4_HLE_AJM_BATCH_START),
        HLE_MAP("-qLsfDAywIY", SHADPS4_HLE_AJM_BATCH_WAIT),
        HLE_MAP("NVDXiUesSbA", SHADPS4_HLE_AJM_BATCH_CANCEL),
        HLE_MAP("dmDybN--Fn8", SHADPS4_HLE_AJM_JOB_CONTROL),
        HLE_MAP("stlghnic3Jc", SHADPS4_HLE_AJM_JOB_INLINE),
        HLE_MAP("ElslOCpOIns", SHADPS4_HLE_AJM_JOB_RUN),
        HLE_MAP("7jdAXK+2fMo", SHADPS4_HLE_AJM_JOB_RUN_SPLIT),
        HLE_MAP("eDFeTyi+G3Y", SHADPS4_HLE_AJM_MP3_PARSE),
        /* Explicit no-op providers mirrored from shadPS4. */
        HLE_MAP("1t3ixYNXyuc", SHADPS4_HLE_SUCCESS),
        HLE_MAP("AxhcqVv5AYU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("WfAiBW8Wcek", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Wi7DtlLV+KI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("YDFR0dDVGAg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("rgLjmfdXocI", SHADPS4_HLE_SUCCESS),
    };
    static const ShadPS4HLECompatMap ngs2[] = {
        HLE_MAP("koBbCMvOKWw", SHADPS4_HLE_NGS2_SYSTEM_CREATE),
        HLE_MAP("mPYgU4oYpuY", SHADPS4_HLE_NGS2_SYSTEM_CREATE),
        HLE_MAP("u-WrYDaJA3k", SHADPS4_HLE_NGS2_SYSTEM_DESTROY),
        HLE_MAP("4lFaRxd-aLs", SHADPS4_HLE_NGS2_SYSTEM_USER_GET),
        HLE_MAP("GZB2v0XnG0k", SHADPS4_HLE_NGS2_SYSTEM_USER_SET),
        HLE_MAP("gThZqM5PYlQ", SHADPS4_HLE_NGS2_SYSTEM_LOCK),
        HLE_MAP("JXRC5n0RQls", SHADPS4_HLE_NGS2_SYSTEM_UNLOCK),
        HLE_MAP("pgFAiLR5qT4", SHADPS4_HLE_NGS2_SYSTEM_QUERY_SIZE),
        HLE_MAP("i0VnXM-C9fc", SHADPS4_HLE_NGS2_SYSTEM_RENDER),
        HLE_MAP("vubFP0T6MP0", SHADPS4_HLE_NGS2_SYSTEM_ENUM),
        HLE_MAP("U-+7HsswcIs", SHADPS4_HLE_NGS2_SYSTEM_ENUM_RACKS),
        HLE_MAP("vU7TQ62pItw", SHADPS4_HLE_NGS2_SYSTEM_INFO),
        HLE_MAP("-tbc2SxQD60", SHADPS4_HLE_NGS2_SYSTEM_SET_SAMPLE_RATE),
        HLE_MAP("l4Q2dWEH6UM", SHADPS4_HLE_NGS2_SYSTEM_SET_GRAIN),
        HLE_MAP("AQkj7C0f3PY", SHADPS4_HLE_NGS2_SYSTEM_RESET_OPTION),
        HLE_MAP("cLV4aiT9JpA", SHADPS4_HLE_NGS2_RACK_CREATE),
        HLE_MAP("U546k6orxQo", SHADPS4_HLE_NGS2_RACK_CREATE),
        HLE_MAP("lCqD7oycmIM", SHADPS4_HLE_NGS2_RACK_DESTROY),
        HLE_MAP("Mn4XNDg03XY", SHADPS4_HLE_NGS2_RACK_USER_GET),
        HLE_MAP("JNTMIaBIbV4", SHADPS4_HLE_NGS2_RACK_USER_SET),
        HLE_MAP("MzTa7VLjogY", SHADPS4_HLE_NGS2_RACK_LOCK),
        HLE_MAP("++YZ7P9e87U", SHADPS4_HLE_NGS2_RACK_UNLOCK),
        HLE_MAP("M4LYATRhRUE", SHADPS4_HLE_NGS2_RACK_INFO),
        HLE_MAP("0eFLVCfWVds", SHADPS4_HLE_NGS2_RACK_QUERY_SIZE),
        HLE_MAP("uBIN24Tv2MI", SHADPS4_HLE_NGS2_REPORT_REGISTER),
        HLE_MAP("nPzb7Ly-VjE", SHADPS4_HLE_NGS2_REPORT_UNREGISTER),
        HLE_MAP("3pCNbVM11UA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("0lbbayqDNoE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("1WsleK-MTkE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("6qN1zaEZuN0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("7Lcfo8SmpsU", SHADPS4_HLE_SUCCESS),
        HLE_MAP("W-Z8wWMBnhk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("WCayTgob7-o", SHADPS4_HLE_SUCCESS),
        HLE_MAP("eF8yRCC6W64", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ekGJmmoc8j4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("gbMKV+8Enuo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("hyVLT2VlOYk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("iprCTXPVWMI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("jjBVvPN9964", SHADPS4_HLE_SUCCESS),
        HLE_MAP("rEh728kXk3w", SHADPS4_HLE_SUCCESS),
        HLE_MAP("t9T0QM17Kvo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("uu94irFOGpA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("xa8oL9dmXkM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("-TOuuAQ-buE", SHADPS4_HLE_SUCCESS),
        /* Explicit no-op providers mirrored from shadPS4. */
        HLE_MAP("-YNfTO6KOMY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("3oIK7y7O4k0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("9eic4AmjGVI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("AbYvTOZ8Pts", SHADPS4_HLE_SUCCESS),
        HLE_MAP("BcoPfWfpvVI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("D8eCqBxSojA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("EEemGEQCjO8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("I+RLwaauggA", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Kg1MA5j7KFk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("MI2VmBx2RbM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("MwmHz8pAdAo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("TZqb8E-j3dY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("TaoNtmMKkXQ", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Wdlx0ZFTV9s", SHADPS4_HLE_SUCCESS),
        HLE_MAP("bfoMXnTRtwE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("dxulc33msHM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("gXiormHoZZ4", SHADPS4_HLE_SUCCESS),
        HLE_MAP("q+2W8YdK0F8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("qQHCi9pjDps", SHADPS4_HLE_SUCCESS),
        HLE_MAP("rfw6ufRsmow", SHADPS4_HLE_SUCCESS),
        HLE_MAP("sU2St3agdjg", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ve6bZi+1sYQ", SHADPS4_HLE_SUCCESS),
    };
    static const ShadPS4HLECompatMap avplayer[] = {
        HLE_MAP("aS66RI0gGgo", SHADPS4_HLE_AVPLAYER_INIT),
        HLE_MAP("o9eWRkSL+M4", SHADPS4_HLE_AVPLAYER_INIT_EX),
        HLE_MAP("NkJwDzKmIlw", SHADPS4_HLE_AVPLAYER_CLOSE),
        HLE_MAP("KMcEa+rHsIo", SHADPS4_HLE_AVPLAYER_ADD_SOURCE),
        HLE_MAP("x8uvuFOPZhU", SHADPS4_HLE_AVPLAYER_ADD_SOURCE_EX),
        HLE_MAP("ET4Gr-Uu07s", SHADPS4_HLE_AVPLAYER_START),
        HLE_MAP("ZC17w3vB5Lo", SHADPS4_HLE_AVPLAYER_STOP),
        HLE_MAP("9y5v+fGN4Wk", SHADPS4_HLE_AVPLAYER_PAUSE),
        HLE_MAP("w5moABNwnRY", SHADPS4_HLE_AVPLAYER_RESUME),
        HLE_MAP("XC9wM+xULz8", SHADPS4_HLE_AVPLAYER_JUMP),
        HLE_MAP("OVths0xGfho", SHADPS4_HLE_AVPLAYER_SET_LOOPING),
        HLE_MAP("BOVKAzRmuTQ", SHADPS4_HLE_AVPLAYER_OPTION),
        HLE_MAP("ODJK2sn9w4A", SHADPS4_HLE_AVPLAYER_OPTION),
        HLE_MAP("HD1YKVU26-M", SHADPS4_HLE_AVPLAYER_OPTION),
        HLE_MAP("k-q+xOxdc3E", SHADPS4_HLE_AVPLAYER_OPTION),
        HLE_MAP("av8Z++94rs0", SHADPS4_HLE_AVPLAYER_OPTION),
        HLE_MAP("wwM99gjFf1Y", SHADPS4_HLE_AVPLAYER_CURRENT_TIME),
        HLE_MAP("UbQoYawOsfY", SHADPS4_HLE_AVPLAYER_IS_ACTIVE),
        HLE_MAP("Wnp1OVcrZgk", SHADPS4_HLE_AVPLAYER_GET_AUDIO_FRAME),
        HLE_MAP("o3+RWnHViSg", SHADPS4_HLE_AVPLAYER_GET_VIDEO_FRAME),
        HLE_MAP("JdksQu8pNdQ", SHADPS4_HLE_AVPLAYER_GET_VIDEO_FRAME_EX),
        HLE_MAP("hdTyRzCXQeQ", SHADPS4_HLE_AVPLAYER_STREAM_COUNT),
        HLE_MAP("d8FcbzfAdQw", SHADPS4_HLE_AVPLAYER_STREAM_INFO),
        /* Explicit no-op providers mirrored from shadPS4. */
        HLE_MAP("agig-iDRrTE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("buMCiJftcfw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("eBTreZ84JFY", SHADPS4_HLE_SUCCESS),
        HLE_MAP("yN7Jhuv8g24", SHADPS4_HLE_SUCCESS),
    };
    static const ShadPS4HLECompatMap user_service[] = {
        HLE_MAP("j3YMu1MVNNo", SHADPS4_HLE_USER_SERVICE_INITIALIZE),
        HLE_MAP("bwFjS+bX9mA", SHADPS4_HLE_USER_SERVICE_TERMINATE),
        HLE_MAP("yH17Q6NWtVg", SHADPS4_HLE_USER_SERVICE_GET_EVENT),
        HLE_MAP("CdWp0oHWGr0", SHADPS4_HLE_USER_SERVICE_GET_INITIAL_USER),
        HLE_MAP("fPhymKNvK-A", SHADPS4_HLE_USER_SERVICE_GET_LOGIN_USER_ID_LIST),
        HLE_MAP("5EiQCnL2G1Y",
                SHADPS4_HLE_USER_SERVICE_GET_REGISTERED_USER_ID_LIST),
        HLE_MAP("lUoqwTQu4Go", SHADPS4_HLE_USER_SERVICE_GET_USER_COLOR),
        HLE_MAP("1xxcMiGu2fo", SHADPS4_HLE_USER_SERVICE_GET_USER_NAME),
    };
    static const ShadPS4HLECompatMap system_service[] = {
        HLE_MAP("1n37q1Bvc5Y", SHADPS4_HLE_SYSTEM_SERVICE_GET_SAFE_AREA),
        HLE_MAP("rPo6tV8D9bM", SHADPS4_HLE_SYSTEM_SERVICE_GET_STATUS),
        HLE_MAP("Vo5V8KAwCmk", SHADPS4_HLE_SYSTEM_SERVICE_HIDE_SPLASH_SCREEN),
        HLE_MAP("JoBqSQt1yyA", SHADPS4_HLE_SYSTEM_SERVICE_LOAD_EXEC),
        HLE_MAP("fZo48un7LK4", SHADPS4_HLE_SYSTEM_SERVICE_PARAM_GET_INT),
        HLE_MAP("SsC-m-S9JTA", SHADPS4_HLE_SYSTEM_SERVICE_PARAM_GET_STRING),
        HLE_MAP("656LMQSrg6U", SHADPS4_HLE_SYSTEM_SERVICE_RECEIVE_EVENT),
    };
    static const ShadPS4HLECompatMap sysmodule[] = {
        HLE_MAP("D8cuU4d72xM", SHADPS4_HLE_SYSMODULE_GET_HANDLE_INTERNAL),
        HLE_MAP("4fU5yvOkVG4", SHADPS4_HLE_SYSMODULE_GET_INFO_UNWIND),
        HLE_MAP("ctfO7dQ7geg", SHADPS4_HLE_SYSMODULE_STUB),
        HLE_MAP("no6T3EfiS3E", SHADPS4_HLE_SYSMODULE_STUB),
        HLE_MAP("fMP5NHUOaMk", SHADPS4_HLE_SYSMODULE_IS_LOADED),
        HLE_MAP("ynFKQ5bfGks", SHADPS4_HLE_SYSMODULE_IS_LOADED_INTERNAL),
        HLE_MAP("g8cM39EUZ6o", SHADPS4_HLE_SYSMODULE_LOAD),
        HLE_MAP("CU8m+Qs+HN4", SHADPS4_HLE_SYSMODULE_STUB),
        HLE_MAP("39iV5E1HoCk", SHADPS4_HLE_SYSMODULE_LOAD_INTERNAL),
        HLE_MAP("hHrGoGoNf+s",
                SHADPS4_HLE_SYSMODULE_LOAD_INTERNAL_WITH_ARG),
        HLE_MAP("lZ6RvVl0vo0", SHADPS4_HLE_SYSMODULE_STUB),
        HLE_MAP("DOO+zuW1lrE", SHADPS4_HLE_SYSMODULE_PRELOAD),
        HLE_MAP("eR2bZFAAU0Q", SHADPS4_HLE_SYSMODULE_UNLOAD),
        HLE_MAP("vpTHmA6Knvg", SHADPS4_HLE_SYSMODULE_STUB),
        HLE_MAP("vXZhrtJxkGc", SHADPS4_HLE_SYSMODULE_STUB),
        HLE_MAP("aKa6YfBKZs4", SHADPS4_HLE_SYSMODULE_STUB),
    };
    static const ShadPS4HLECompatMap np_common[] = {
        HLE_MAP("i8UmXTSq7N4", SHADPS4_HLE_NP_COMMON_CMP_NP_ID),
        HLE_MAP("TcwEFnakiSc", SHADPS4_HLE_NP_COMMON_CMP_NP_ID_ORDER),
        HLE_MAP("dj+O5aD2a0Q", SHADPS4_HLE_NP_COMMON_CMP_ONLINE_ID),
        HLE_MAP("1a+iY5YUJcI", SHADPS4_HLE_COND_DESTROY),
        HLE_MAP("18j+qk6dRwk", SHADPS4_HLE_MUTEX_LOCK),
        HLE_MAP("1CiXI-MyEKs", SHADPS4_HLE_MUTEX_INIT),
        HLE_MAP("4zxevggtYrQ", SHADPS4_HLE_MUTEX_DESTROY),
        HLE_MAP("CQG2oyx1-nM", SHADPS4_HLE_MUTEX_UNLOCK),
        HLE_MAP("DuslmoqQ+nk", SHADPS4_HLE_NP_COMMON_MUTEX_TRYLOCK),
        HLE_MAP("EjMsfO3GCIA", SHADPS4_HLE_PTHREAD_JOIN),
        HLE_MAP("fhJ5uKzcn0w", SHADPS4_HLE_NP_COMMON_THREAD_CREATE),
        HLE_MAP("hkeX9iuCwlI", SHADPS4_HLE_NP_COMMON_VALID_ONLINE_ID),
        HLE_MAP("hp0kVgu5Fxw", SHADPS4_HLE_NP_COMMON_MUTEX_TRYLOCK),
        HLE_MAP("lQ11BpMM4LU", SHADPS4_HLE_MUTEX_DESTROY),
        HLE_MAP("oZyb9ktuCpA", SHADPS4_HLE_MUTEX_UNLOCK),
        HLE_MAP("PVVsRmMkO1g", SHADPS4_HLE_NP_COMMON_CLOCK_USEC),
        HLE_MAP("q2tsVO3lM4A", SHADPS4_HLE_COND_INIT),
        HLE_MAP("r9Bet+s6fKc", SHADPS4_HLE_MUTEX_LOCK),
        HLE_MAP("sXVQUIGmk2U", SHADPS4_HLE_NP_COMMON_PLATFORM_TYPE),
        HLE_MAP("ss2xO9IJxKQ", SHADPS4_HLE_NP_COMMON_COND_TIMEDWAIT),
        HLE_MAP("uEwag-0YZPc", SHADPS4_HLE_MUTEX_INIT),
        HLE_MAP("uMJFOA62mVU", SHADPS4_HLE_COND_SIGNAL),
        HLE_MAP("Pglk7zFj0DI", SHADPS4_HLE_NP_COMMON_SDK_VERSION),
        HLE_MAP("9+m5nRdJ-wQ", SHADPS4_HLE_NP_CALLOUT_INIT),
        HLE_MAP("AqJ4xkWsV+I", SHADPS4_HLE_NP_CALLOUT_TERM),
        HLE_MAP("fClnlkZmA6k", SHADPS4_HLE_NP_CALLOUT_START),
        HLE_MAP("in19gH7G040", SHADPS4_HLE_NP_CALLOUT_STOP),
        HLE_MAP("lpr66Gby8dQ", SHADPS4_HLE_NP_CALLOUT_START64),
    };
    static const ShadPS4HLECompatMap rtc[] = {
        HLE_MAP("lPEBYdVX0XQ", SHADPS4_HLE_RTC_CHECK_VALID),
        HLE_MAP("fNaZ4DbzHAE", SHADPS4_HLE_RTC_COMPARE_TICK),
        HLE_MAP("8Yr143yEnRo", SHADPS4_HLE_RTC_COPY_TICK),
        HLE_MAP("M1TvFst-jrM", SHADPS4_HLE_RTC_COPY_TICK),
        HLE_MAP("8SljQx6pDP8", SHADPS4_HLE_SUCCESS),
        HLE_MAP("eiuobaF-hK4", SHADPS4_HLE_RTC_FORMAT_RFC2822),
        HLE_MAP("AxHBk3eat04", SHADPS4_HLE_RTC_FORMAT_RFC2822_LOCAL),
        HLE_MAP("WJ3rqFwymew", SHADPS4_HLE_RTC_FORMAT_RFC3339),
        HLE_MAP("DwuHIlLGW8I", SHADPS4_HLE_RTC_FORMAT_RFC3339_LOCAL),
        HLE_MAP("lja0nNPWojg", SHADPS4_HLE_RTC_FORMAT_RFC3339),
        HLE_MAP("tOZ6fwwHZOA", SHADPS4_HLE_RTC_FORMAT_RFC3339_LOCAL),
        HLE_MAP("LN3Zcb72Q0c", SHADPS4_HLE_RTC_CURRENT_AD_TICK),
        HLE_MAP("8lfvnRMqwEM", SHADPS4_HLE_RTC_CURRENT_CLOCK),
        HLE_MAP("ZPD1YOKI+Kw", SHADPS4_HLE_RTC_CURRENT_CLOCK_LOCAL),
        HLE_MAP("Ot1DE3gif84", SHADPS4_HLE_RTC_CURRENT_DEBUG_TICK),
        HLE_MAP("zO9UL3qIINQ", SHADPS4_HLE_RTC_CURRENT_NETWORK_TICK),
        HLE_MAP("HWxHOdbM-Pg", SHADPS4_HLE_RTC_CURRENT_RAW_NETWORK_TICK),
        HLE_MAP("18B2NS1y9UU", SHADPS4_HLE_RTC_CURRENT_TICK),
        HLE_MAP("CyIK-i4XdgQ", SHADPS4_HLE_RTC_GET_DAY_OF_WEEK),
        HLE_MAP("3O7Ln8AqJ1o", SHADPS4_HLE_RTC_GET_DAYS_IN_MONTH),
        HLE_MAP("E7AR4o7Ny7E", SHADPS4_HLE_RTC_GET_DOS_TIME),
        HLE_MAP("8w-H19ip48I", SHADPS4_HLE_RTC_GET_TICK),
        HLE_MAP("jMNwqYr4R-k", SHADPS4_HLE_RTC_GET_TICK_RESOLUTION),
        HLE_MAP("BtqmpTRXHgk", SHADPS4_HLE_RTC_GET_TIME_T),
        HLE_MAP("jfRO0uTjtzA", SHADPS4_HLE_RTC_GET_FILETIME),
        HLE_MAP("LlodCMDbk3o", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Ug8pCwQvh0c", SHADPS4_HLE_RTC_IS_LEAP_YEAR),
        HLE_MAP("NxEI1KByvCI", SHADPS4_HLE_RTC_PARSE_DATETIME),
        HLE_MAP("99bMGglFW3I", SHADPS4_HLE_RTC_PARSE_RFC3339),
        HLE_MAP("fFLgmNUpChg", SHADPS4_HLE_RTC_SET_CONF),
        HLE_MAP("sV2tK+yOhBU", SHADPS4_HLE_RTC_SET_CURRENT_AD_TICK),
        HLE_MAP("VLDUPKmw5L8", SHADPS4_HLE_RTC_SET_CURRENT_DEBUG_TICK),
        HLE_MAP("qhDBtIo+auw", SHADPS4_HLE_RTC_SET_CURRENT_NETWORK_TICK),
        HLE_MAP("d4fHLCGmY80", SHADPS4_HLE_RTC_SET_CURRENT_TICK),
        HLE_MAP("aYPCd1cChyg", SHADPS4_HLE_RTC_SET_DOS_TIME),
        HLE_MAP("ueega6v3GUw", SHADPS4_HLE_RTC_SET_TICK),
        HLE_MAP("bDEVVP4bTjQ", SHADPS4_HLE_RTC_SET_TIME_T),
        HLE_MAP("n5JiAJXsbcs", SHADPS4_HLE_RTC_SET_FILETIME),
        HLE_MAP("NR1J0N7L2xY", SHADPS4_HLE_RTC_ADD_DAYS),
        HLE_MAP("MDc5cd8HfCA", SHADPS4_HLE_RTC_ADD_HOURS),
        HLE_MAP("XPIiw58C+GM", SHADPS4_HLE_RTC_ADD_MICROSECONDS),
        HLE_MAP("mn-tf4QiFzk", SHADPS4_HLE_RTC_ADD_MINUTES),
        HLE_MAP("CL6y9q-XbuQ", SHADPS4_HLE_RTC_ADD_MONTHS),
        HLE_MAP("07O525HgICs", SHADPS4_HLE_RTC_ADD_SECONDS),
        HLE_MAP("AqVMssr52Rc", SHADPS4_HLE_RTC_ADD_TICKS),
        HLE_MAP("gI4t194c2W8", SHADPS4_HLE_RTC_ADD_WEEKS),
        HLE_MAP("-5y2uJ62qS8", SHADPS4_HLE_RTC_ADD_YEARS),
    };
    static const ShadPS4HLECompatMap np_webapi[] = {
        HLE_MAP("JzhYTP2fG18", SHADPS4_HLE_HTTP2_ABORT_REQUEST),
        HLE_MAP("joRjtRXTFoc", SHADPS4_HLE_HTTP_ADD_HEADER),
        HLE_MAP("gVNNyxf-1Sg", SHADPS4_HLE_WEBAPI_CHECK_TIMEOUT),
        HLE_MAP("x1Y7yiYSk7c", SHADPS4_HLE_WEBAPI_CREATE_USER_ONLINE),
        HLE_MAP("zk6c65xoyO0", SHADPS4_HLE_WEBAPI_CREATE_USER_ID),
        HLE_MAP("79M-JqvvGo0", SHADPS4_HLE_WEBAPI_CREATE_HANDLE),
        HLE_MAP("KBxgeNpoRIQ", SHADPS4_HLE_WEBAPI_CREATE_MULTIPART_REQUEST),
        HLE_MAP("rdgs5Z1MyFw", SHADPS4_HLE_WEBAPI_CREATE_REQUEST),
        HLE_MAP("XUjdsSTTZ3U", SHADPS4_HLE_WEBAPI_DELETE_USER),
        HLE_MAP("5Mn7TYwpl30", SHADPS4_HLE_WEBAPI_DELETE_HANDLE),
        HLE_MAP("noQgleu+KLE", SHADPS4_HLE_HTTP_DELETE_REQUEST),
        HLE_MAP("2qSZ0DgwTsc", SHADPS4_HLE_WEBAPI_GET_ERROR_CODE),
        HLE_MAP("VwJ5L0Higg0", SHADPS4_HLE_WEBAPI_GET_HEADER_VALUE),
        HLE_MAP("743ZzEBzlV8", SHADPS4_HLE_WEBAPI_GET_HEADER_LENGTH),
        HLE_MAP("k210oKgP80Y", SHADPS4_HLE_HTTP_GET_STATUS),
        HLE_MAP("G3AnLNdRBjE", SHADPS4_HLE_WEBAPI_INIT),
        HLE_MAP("FkuwsD64zoQ", SHADPS4_HLE_WEBAPI_INIT),
        HLE_MAP("CQtPRSF6Ds8", SHADPS4_HLE_HTTP_READ_DATA),
        HLE_MAP("KCItz6QkeGs", SHADPS4_HLE_WEBAPI_SEND_MULTIPART),
        HLE_MAP("DsPOTEvSe7M", SHADPS4_HLE_WEBAPI_SEND_MULTIPART),
        HLE_MAP("kVbL4hL3K7w", SHADPS4_HLE_HTTP_SEND_REQUEST),
        HLE_MAP("KjNeZ-29ysQ", SHADPS4_HLE_WEBAPI_SEND_WITH_INFO),
        HLE_MAP("i0dr6grIZyc", SHADPS4_HLE_WEBAPI_OPTION),
        HLE_MAP("qWcbJkBj1Lg", SHADPS4_HLE_WEBAPI_OPTION),
        HLE_MAP("asz3TtIqGF8", SHADPS4_HLE_WEBAPI_DELETE_CONTEXT),
        HLE_MAP("uRsskUhAfnM", SHADPS4_HLE_WEBAPI_INIT),
        HLE_MAP("WKcm4PeyJww", SHADPS4_HLE_WEBAPI_ABORT_HANDLE),
        HLE_MAP("19KgfJXgM+U", SHADPS4_HLE_WEBAPI_ADD_MULTIPART),
        HLE_MAP("y5Ta5JCzQHY", SHADPS4_HLE_WEBAPI_FILTER_CREATE),
        HLE_MAP("sIFx734+xys", SHADPS4_HLE_WEBAPI_FILTER_CREATE),
        HLE_MAP("M2BUB+DNEGE", SHADPS4_HLE_WEBAPI_FILTER_CREATE),
        HLE_MAP("zE+R6Rcx3W0", SHADPS4_HLE_WEBAPI_FILTER_DELETE),
        HLE_MAP("PfQ+f6ws764", SHADPS4_HLE_WEBAPI_FILTER_DELETE),
        HLE_MAP("pfaJtb7SQ80", SHADPS4_HLE_WEBAPI_FILTER_DELETE),
        HLE_MAP("vrM02A5Gy1M", SHADPS4_HLE_WEBAPI_CALLBACK_REGISTER),
        HLE_MAP("HVgWmGIOKdk", SHADPS4_HLE_WEBAPI_CALLBACK_REGISTER_DIRECT),
        HLE_MAP("PfSTDCgNMgc", SHADPS4_HLE_WEBAPI_CALLBACK_REGISTER),
        HLE_MAP("kJQJE0uKm5w", SHADPS4_HLE_WEBAPI_CALLBACK_REGISTER),
        HLE_MAP("jhXKGQJ4egI", SHADPS4_HLE_WEBAPI_CALLBACK_REGISTER),
        HLE_MAP("VjVukb2EWPc", SHADPS4_HLE_WEBAPI_CALLBACK_REGISTER),
        HLE_MAP("sfq23ZVHVEw", SHADPS4_HLE_WEBAPI_CALLBACK_REGISTER),
        HLE_MAP("wjYEvo4xbcA", SHADPS4_HLE_WEBAPI_CALLBACK_UNREGISTER_DIRECT),
        HLE_MAP("qK4o2656W4w", SHADPS4_HLE_WEBAPI_CALLBACK_UNREGISTER),
        HLE_MAP("2edrkr0c-wg", SHADPS4_HLE_WEBAPI_CALLBACK_UNREGISTER),
        HLE_MAP("PqCY25FMzPs", SHADPS4_HLE_WEBAPI_CALLBACK_UNREGISTER),
        HLE_MAP("6g6q-g1i4XU", SHADPS4_HLE_WEBAPI_SET_HANDLE_TIMEOUT),
        HLE_MAP("or0e885BlXo", SHADPS4_HLE_WEBAPI_PARSE_NP_ID),
        HLE_MAP("8Vjplhyyc44", SHADPS4_HLE_WEBAPI_INTERNAL_INIT),
        HLE_MAP("c1pKoztonB8", SHADPS4_HLE_WEBAPI_FILTER_CREATE),
        HLE_MAP("N2Jbx4tIaQ4", SHADPS4_HLE_WEBAPI_INTERNAL_CREATE_REQUEST),
        HLE_MAP("TZSep4xB4EY", SHADPS4_HLE_WEBAPI_FILTER_CREATE),
    };
    static const ShadPS4HLECompatMap np_matching2[] = {
        HLE_MAP("10t3e5+JPnU", SHADPS4_HLE_NP_MATCHING_INIT),
        HLE_MAP("Mqp3lJ+sjy4", SHADPS4_HLE_NP_MATCHING_TERM),
        HLE_MAP("YfmpW719rMo", SHADPS4_HLE_NP_MATCHING_CREATE_CONTEXT),
        HLE_MAP("ajvzc8e2upo", SHADPS4_HLE_NP_MATCHING_CREATE_CONTEXT),
        HLE_MAP("Nz-ZE7ur32I", SHADPS4_HLE_NP_MATCHING_DESTROY_CONTEXT),
        HLE_MAP("7vjNQ6Z1op0", SHADPS4_HLE_NP_MATCHING_START),
        HLE_MAP("-f6M4caNe8k", SHADPS4_HLE_NP_MATCHING_STOP),
        HLE_MAP("pFzhpCMlJXQ", SHADPS4_HLE_NP_MATCHING_STOP),
        HLE_MAP("fQQfP87I7hs", SHADPS4_HLE_NP_MATCHING_REGISTER_CALLBACK),
        HLE_MAP("4Nj7u5B5yCA", SHADPS4_HLE_NP_MATCHING_REGISTER_CALLBACK),
        HLE_MAP("p+2EnxmaAMM", SHADPS4_HLE_NP_MATCHING_REGISTER_CALLBACK),
        HLE_MAP("0UMeWRGnZKA", SHADPS4_HLE_NP_MATCHING_REGISTER_CALLBACK),
        HLE_MAP("DnPUsBAe8oI", SHADPS4_HLE_NP_MATCHING_REGISTER_CALLBACK),
        HLE_MAP("uBESzz4CQws", SHADPS4_HLE_NP_MATCHING_REGISTER_CALLBACK),
        HLE_MAP("LhCPctIICxQ", SHADPS4_HLE_NP_MATCHING_GET_SERVER),
        HLE_MAP("gpSAvdheZ0Q", SHADPS4_HLE_NP_MATCHING_MEMORY),
        HLE_MAP("8btynvj0KNA", SHADPS4_HLE_NP_MATCHING_MEMORY),
        HLE_MAP("+8e7wXLmjds", SHADPS4_HLE_NP_MATCHING_REGISTER_CALLBACK),
        HLE_MAP("V6KSpKv9XJE", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("zCWZmXXN600", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("CSIMDsVjs-g", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("rJNPJqDCpiI", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("BD6kfx442Do", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("VqZX7POg2Mk", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("Iw2h0Jrrb5U", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("meEjIdbjAA0", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("q7GK98-nYSE", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("S9D8JSYIrjE", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("tHD5FPFXtu4", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("KC+GnHzrK2o", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("vbtWT3lZBOM", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("cgQhq3E0eGo", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("wyvlEgZ-55w", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("1JtbJ0kxm3E", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("1Z4Xxumgm+Y", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("26vWrPAWJfM", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("Jraxifmoet4", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("dMQ+xGvTdqM", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("5lhvOqheFBA", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("GyI2f9yDUXM", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("qeF-q5KDtAc", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("NCP3bLGPt+o", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("n5JmImxTiZU", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("gQ6cUriNpgs", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("AUVfU6byg3c", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("BBbJ92uUdCg", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("K+KtxhPsMZ4", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("opDpl74pi2E", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("ir2CzSs9K-g", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("HoqTrkS9c5Q", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("ES3UMUWWj9U", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("8CqniKDzjvg", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("wUmwXZHaX1w", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("GNSN5849fjU", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("nNeC3F8-g+4", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("twVupeaYYrk", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("380EWm2DrVg", SHADPS4_HLE_NP_MATCHING_OFFLINE),
        HLE_MAP("CTy4PBhpWDw", SHADPS4_HLE_NP_MATCHING_OFFLINE),
    };
    static const ShadPS4HLECompatMap np_score[] = {
        HLE_MAP("KnNA1TEgtBI", SHADPS4_HLE_NP_SCORE_CREATE_TITLE),
        HLE_MAP("GWnWQNXZH5M", SHADPS4_HLE_NP_SCORE_CREATE_TITLE_USER),
        HLE_MAP("gW8qyjYrUbk", SHADPS4_HLE_NP_SCORE_CREATE_REQUEST),
        HLE_MAP("G0pE+RNCwfk", SHADPS4_HLE_NP_SCORE_DELETE_TITLE),
        HLE_MAP("dK8-SgYf6r4", SHADPS4_HLE_NP_SCORE_DELETE_REQUEST),
        HLE_MAP("1i7kmKbX6hk", SHADPS4_HLE_NP_SCORE_ABORT),
        HLE_MAP("m1DfNRstkSQ", SHADPS4_HLE_NP_SCORE_POLL),
        HLE_MAP("fqk8SC63p1U", SHADPS4_HLE_NP_SCORE_WAIT),
        HLE_MAP("bygbKdHmjn4", SHADPS4_HLE_NP_SCORE_SET_PC_ID),
        HLE_MAP("2b3TI0mDYiI", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("4eOvDyN-aZc", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("LoVMVrijVOk", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("Q0Avi9kebsY", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("8kuIzUw6utQ", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("gMbOn+-6eXA", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("6-G9OxL5DKg", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("7SuMUlN7Q6I", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("zKoVok6FFEI", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("JjOFRVPdQWc", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("Lmtc9GljeUA", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("PP9jx8s0574", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("K9tlODTQx3c", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("dRszNNyGWkw", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("wJPWycVGzrs", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("bFVjDgxFapc", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("9mZEgoiEq6Y", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("Rd27dqUFZV8", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("ETS-uM-vH9Q", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("FsouSN0ykN8", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("KBHxDjyk-jA", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("MA9vSt7JImY", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("y5ja7WI05rs", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("rShmqXHwoQE", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("bcoVwcBjQ9E", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("1gL5PwYzrrw", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("zT0XBtgtOSI", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("ANJssPz3mY0", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("r4oAo9in0TA", SHADPS4_HLE_NP_SCORE_OFFLINE),
        HLE_MAP("3UVqGJeDf30", SHADPS4_HLE_NP_SCORE_OFFLINE),
    };
    static const ShadPS4HLECompatMap np_tus[] = {
        HLE_MAP("sRVb2Cf0GHg", SHADPS4_HLE_NP_TUS_CREATE_TITLE),
        HLE_MAP("lBtrk+7lk14", SHADPS4_HLE_NP_TUS_CREATE_TITLE_USER),
        HLE_MAP("BIkMmUfNKWM", SHADPS4_HLE_NP_TUS_CREATE_TITLE),
        HLE_MAP("1n-dGukBgnY", SHADPS4_HLE_NP_TUS_CREATE_TITLE_USER),
        HLE_MAP("3bh2aBvvmvM", SHADPS4_HLE_NP_TUS_CREATE_REQUEST),
        HLE_MAP("H3uq7x0sZOI", SHADPS4_HLE_NP_TUS_DELETE_TITLE),
        HLE_MAP("CcIH40dYS88", SHADPS4_HLE_NP_TUS_DELETE_REQUEST),
        HLE_MAP("t7b6dmpQNiI", SHADPS4_HLE_NP_TUS_POLL),
        HLE_MAP("hYPJFWzFPjA", SHADPS4_HLE_NP_TUS_WAIT),
        HLE_MAP("-SUR+UoLS6c", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("DS2yu3Sjj1o", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("XOzszO4ONWU", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("yWEHUFkY1qI", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("xzG8mG9YlKY", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("uHtKS5V1T5k", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("833Y2TnyonE", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("7uLPqiNvNLc", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("DgpRToHWN40", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("F+eQlfcka98", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("bcPB2rnhQqo", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("4NrufkNCkiE", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("VzxN3tOouj8", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("4u58d6g6uwU", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("G68xdfQuiyU", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("5J9GGMludxY", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("oGIcxlUabSA", SHADPS4_HLE_NP_TUS_OFFLINE),
    };
    static const ShadPS4HLECompatMap np_signaling[] = {
        HLE_MAP("3KOuC4RmZZU", SHADPS4_HLE_NP_SIGNALING_INIT),
        HLE_MAP("NPhw0UXaNrk", SHADPS4_HLE_NP_SIGNALING_TERM),
        HLE_MAP("5yYjEdd4t8Y", SHADPS4_HLE_NP_SIGNALING_CREATE_CONTEXT),
        HLE_MAP("dDLNFdY8dws", SHADPS4_HLE_NP_SIGNALING_CREATE_CONTEXT_USER),
        HLE_MAP("hx+LIg-1koI", SHADPS4_HLE_NP_SIGNALING_DELETE_CONTEXT),
        HLE_MAP("npU5V56id34", SHADPS4_HLE_NP_SIGNALING_GET_OPTION),
        HLE_MAP("IHRDvZodPYY", SHADPS4_HLE_NP_SIGNALING_SET_OPTION),
        HLE_MAP("tOpqyDyMje4", SHADPS4_HLE_NP_SIGNALING_MEMORY),
        HLE_MAP("0UvTFeomAUM", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("ZPLavCKqAB0", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("X1G4kkN2R-8", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("6UEembipgrM", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("GQ0hqmzj0F4", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("CkPxQjSm018", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("B7cT9aVby7A", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("AN3h0EBSX7A", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("rcylknsUDwg", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("C6ZNCDTj00Y", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("bD-JizUb3JM", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("U8AQMlOFBc8", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("zFgFHId7vAE", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("Shr7bZq8QHY", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("2HajCEGgG4s", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
        HLE_MAP("b4qaXPzMJxo", SHADPS4_HLE_NP_SIGNALING_OFFLINE),
    };
    static const ShadPS4HLECompatMap usbd[] = {
        HLE_MAP("0ktE1PhzGFU", SHADPS4_HLE_USBD_ALLOC_TRANSFER),
        HLE_MAP("BKMEGvfCPyU", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("fotb7DzeHYw", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("-KNh1VFIzlM", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("MlW6deWfPp0", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("AE+mHBHneyk", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("3tPPMo4QRdY", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("HarYYlaFGJY", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("RRKFcKQ1Ka4", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("XUWtxI31YEY", SHADPS4_HLE_USBD_CONTROL_DATA),
        HLE_MAP("SEdQo8CFmus", SHADPS4_HLE_USBD_CONTROL_SETUP),
        HLE_MAP("Y5go+ha6eDs", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("Vw8Hg1CN028", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("e7gp1xhu6RI", SHADPS4_HLE_USBD_EVENT_OK),
        HLE_MAP("Fq6+0Fm55xU", SHADPS4_HLE_USBD_EXIT),
        HLE_MAP("oHCade-0qQ0", SHADPS4_HLE_USBD_FILL_IO),
        HLE_MAP("8KrqbaaPkE0", SHADPS4_HLE_USBD_FILL_SETUP),
        HLE_MAP("7VGfMerK6m0", SHADPS4_HLE_USBD_FILL_CONTROL),
        HLE_MAP("t3J5pXxhJlI", SHADPS4_HLE_USBD_FILL_IO),
        HLE_MAP("xqmkjHCEOSY", SHADPS4_HLE_USBD_FILL_IO),
        HLE_MAP("Hvd3S--n25w", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("EQ6SCLMqzkM", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("-sgi7EeLSO8", SHADPS4_HLE_USBD_FREE_TRANSFER),
        HLE_MAP("S1o1C6yOt5g", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("t7WE9mb1TB8", SHADPS4_HLE_USBD_NULL),
        HLE_MAP("Dkm5qe8j3XE", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("GQsAVJuy8gM", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("L7FoTZp3bZs", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("-JBoEtvTxvA", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("rsl9KQ-agyA", SHADPS4_HLE_USBD_NULL),
        HLE_MAP("GjlCrU4GcIY", SHADPS4_HLE_USBD_NULL),
        HLE_MAP("bhomgbiQgeo", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("8qB9Ar4P5nc", SHADPS4_HLE_USBD_GET_DEVICE_LIST),
        HLE_MAP("e1UWb8cWPJM", SHADPS4_HLE_USBD_NULL),
        HLE_MAP("vokkJ0aDf54", SHADPS4_HLE_USBD_ISO_BUFFER),
        HLE_MAP("nuIRlpbxauM", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("YJ0cMAlLuxQ", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("g2oYm1DitDg", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("t4gUfGsjk+g", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("EkqGLxWC-S0", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("rt-WeUGibfg", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("+wU6CGuZcWk", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("TOhg7P6kTH4", SHADPS4_HLE_USBD_INIT),
        HLE_MAP("rxi1nCOKWc8", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("RLf56F-WjKQ", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("u9yKks02-rA", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("AeGaY8JrAV4", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("VJ6oMq-Di2U", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("vrQXYRo1Gwk", SHADPS4_HLE_USBD_NULL),
        HLE_MAP("U1t1SoJvV-A", SHADPS4_HLE_USBD_NULL),
        HLE_MAP("REfUTmTchMw", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("hvMn0QJXj5g", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("FhU9oYrbXoA", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("DVCQW9o+ki0", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("dJxro8Nzcjk", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("L0EHgZZNVas", SHADPS4_HLE_USBD_NO_DEVICE),
        HLE_MAP("TcXVGc-LPbQ", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("RA2D9rFH-Uw", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("1DkGvUQYFKI", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("OULgIo1zAsA", SHADPS4_HLE_USBD_EVENT),
        HLE_MAP("ys2e9VRBPrY", SHADPS4_HLE_USBD_EVENT),
    };
    static const ShadPS4HLECompatMap np_manager[] = {
        HLE_MAP("0c7HbXRKUt4", SHADPS4_HLE_NP_MANAGER_REGISTER_CALLBACK),
        HLE_MAP("JELHf4xPufo", SHADPS4_HLE_NP_MANAGER_CHECK_CALLBACK),
        HLE_MAP("uqcPJLWL08M", SHADPS4_HLE_NP_MANAGER_WAIT_ASYNC),
        HLE_MAP("A2CQ3kgSopQ", SHADPS4_HLE_NP_MANAGER_SET_RESTRICTION),
        HLE_MAP("Ec63y59l9tw", SHADPS4_HLE_NP_MANAGER_SET_TITLE),
        HLE_MAP("KO+11cgC7N0", SHADPS4_HLE_NP_MANAGER_SET_PRESENCE),
        HLE_MAP("GpLQDNKICac", SHADPS4_HLE_NP_MANAGER_CREATE_REQUEST),
        HLE_MAP("eiqMCt9UshI", SHADPS4_HLE_NP_MANAGER_CREATE_ASYNC_REQUEST),
        HLE_MAP("2rsFmlGWleQ", SHADPS4_HLE_NP_MANAGER_ASYNC_OFFLINE),
        HLE_MAP("8Z2Jc5GvGDI", SHADPS4_HLE_NP_MANAGER_ASYNC_OFFLINE),
        HLE_MAP("KfGZg2y73oM", SHADPS4_HLE_NP_MANAGER_ASYNC_OFFLINE),
        HLE_MAP("r6MyYJkryz8", SHADPS4_HLE_NP_MANAGER_ASYNC_OFFLINE),
        HLE_MAP("KZ1Mj9yEGYc", SHADPS4_HLE_NP_MANAGER_ASYNC_OFFLINE),
        HLE_MAP("TPMbgIxvog0", SHADPS4_HLE_NP_MANAGER_ASYNC_OFFLINE),
        HLE_MAP("ilwLM4zOmu4", SHADPS4_HLE_NP_MANAGER_ASYNC_OFFLINE),
        HLE_MAP("m9L3O6yst-U", SHADPS4_HLE_NP_MANAGER_ASYNC_OFFLINE),
        HLE_MAP("OzKvTvg3ZYU", SHADPS4_HLE_NP_MANAGER_ABORT_REQUEST),
        HLE_MAP("-QglDeRr8D8", SHADPS4_HLE_NP_MANAGER_SET_TIMEOUT),
        HLE_MAP("jyi5p9XWUSs", SHADPS4_HLE_NP_MANAGER_WAIT_ASYNC),
        HLE_MAP("S7QTn72PrDw", SHADPS4_HLE_NP_MANAGER_DELETE_REQUEST),
        HLE_MAP("Ghz9iWDUtC4", SHADPS4_HLE_NP_MANAGER_SIGNED_OUT),
        HLE_MAP("JT+t00a3TxA", SHADPS4_HLE_NP_MANAGER_SIGNED_OUT),
        HLE_MAP("8VBTeRf1ZwI", SHADPS4_HLE_NP_MANAGER_SIGNED_OUT),
        HLE_MAP("q3M7XzBKC3s", SHADPS4_HLE_NP_MANAGER_SIGNED_OUT),
        HLE_MAP("IPb1hd1wAGc", SHADPS4_HLE_NP_MANAGER_GET_BOOL),
        HLE_MAP("oPO9U42YpgI", SHADPS4_HLE_NP_MANAGER_GET_BOOL),
        HLE_MAP("C0gNCiRIi4U", SHADPS4_HLE_NP_MANAGER_SIGNED_OUT),
        HLE_MAP("uFJpaKNBAj4", SHADPS4_HLE_NP_MANAGER_REGISTER_CALLBACK),
        HLE_MAP("KswxLxk4c1Y", SHADPS4_HLE_NP_MANAGER_REGISTER_CALLBACK),
        HLE_MAP("aJZyCcHxzu4", SHADPS4_HLE_NP_MANAGER_UNREGISTER_CALLBACK),
        HLE_MAP("Ybu6AxV6S0o", SHADPS4_HLE_NP_MANAGER_GET_BOOL),
        HLE_MAP("GImICnh+boA", SHADPS4_HLE_NP_MANAGER_REGISTER_CALLBACK),
        HLE_MAP("xViqJdDgKl0", SHADPS4_HLE_NP_MANAGER_UNREGISTER_CALLBACK),
        HLE_MAP("e-ZuhGEoeC4", SHADPS4_HLE_NP_MANAGER_GET_REACHABILITY),
        HLE_MAP("a8R9-75u4iM", SHADPS4_HLE_NP_MANAGER_SIGNED_OUT),
        HLE_MAP("rbknaUjpqWo", SHADPS4_HLE_NP_MANAGER_SIGNED_OUT),
        HLE_MAP("p-o74CnoNzY", SHADPS4_HLE_NP_MANAGER_SIGNED_OUT),
        HLE_MAP("XDncXQIJUSk", SHADPS4_HLE_NP_MANAGER_SIGNED_OUT),
        HLE_MAP("eQH7nWPcAgc", SHADPS4_HLE_NP_MANAGER_GET_STATE),
        HLE_MAP("VgYczPGB5ss", SHADPS4_HLE_NP_MANAGER_SIGNED_OUT),
        HLE_MAP("F6E4ycq9Dbg", SHADPS4_HLE_NP_MANAGER_SIGNED_OUT),
        HLE_MAP("Oad3rvY-NJQ", SHADPS4_HLE_NP_MANAGER_GET_BOOL),
        HLE_MAP("3Zl8BePTh9Y", SHADPS4_HLE_NP_MANAGER_CHECK_CALLBACK),
        HLE_MAP("VfRSmPmj8Q8", SHADPS4_HLE_NP_MANAGER_REGISTER_CALLBACK),
        HLE_MAP("mjjTXh+NHWY", SHADPS4_HLE_NP_MANAGER_UNREGISTER_CALLBACK),
        HLE_MAP("qQJfO8HAiaY", SHADPS4_HLE_NP_MANAGER_REGISTER_CALLBACK),
        HLE_MAP("M3wFXbYQtAA", SHADPS4_HLE_NP_MANAGER_UNREGISTER_CALLBACK),
        HLE_MAP("hw5KNqAAels", SHADPS4_HLE_NP_MANAGER_REGISTER_CALLBACK),
        HLE_MAP("cRILAEvn+9M", SHADPS4_HLE_NP_MANAGER_UNREGISTER_CALLBACK),
        HLE_MAP("YIvqqvJyjEc", SHADPS4_HLE_NP_MANAGER_UNREGISTER_CALLBACK),
    };
    static const ShadPS4HLECompatMap camera[] = {
        HLE_MAP("hHA1frlMxYE", SHADPS4_HLE_CAMERA_CALIB_DATA),
        HLE_MAP("5Oie5RArfWs", SHADPS4_HLE_CAMERA_CALIB_DATA),
        HLE_MAP("B260o9pSzM8", SHADPS4_HLE_CAMERA_CONNECTED_COUNT),
        HLE_MAP("ULxbwqiYYuU", SHADPS4_HLE_CAMERA_PRODUCT_INFO),
        HLE_MAP("hawKak+Auw4", SHADPS4_HLE_CAMERA_PRODUCT_INFO),
        HLE_MAP("0wnf2a60FqI", SHADPS4_HLE_CAMERA_REGISTRY_INIT),
        HLE_MAP("vejouEusC7g", SHADPS4_HLE_CAMERA_DEBUG_STOP),
        HLE_MAP("wpeyFwJ+UEI", SHADPS4_HLE_CAMERA_VIDEO_SYNC),
        HLE_MAP("UFonL7xopFM", SHADPS4_HLE_CAMERA_AUDIO),
        HLE_MAP("fkZE7Hup2ro", SHADPS4_HLE_CAMERA_AUDIO),
        HLE_MAP("hftC5A1C8OQ", SHADPS4_HLE_CAMERA_AUDIO),
        HLE_MAP("DhqqFiBU+6g", SHADPS4_HLE_CAMERA_AUDIO),
        HLE_MAP("wyU98EXAYxU", SHADPS4_HLE_CAMERA_AUDIO),
        HLE_MAP("OMS9LlcrvBo", SHADPS4_HLE_CAMERA_CLOSE),
        HLE_MAP("ztqH5qNTpTk", SHADPS4_HLE_CAMERA_CLOSE),
        HLE_MAP("nBH6i2s4Glc", SHADPS4_HLE_CAMERA_OPEN),
        HLE_MAP("0btIPD5hg5A", SHADPS4_HLE_CAMERA_GET_VALUE),
        HLE_MAP("oEi6vM-3E2c", SHADPS4_HLE_CAMERA_GET_VALUE),
        HLE_MAP("qTPRMh4eY60", SHADPS4_HLE_CAMERA_GET_VALUE),
        HLE_MAP("RHYJ7GKOSMg", SHADPS4_HLE_CAMERA_GET_DATA),
        HLE_MAP("ZaqmGEtYuL0", SHADPS4_HLE_CAMERA_GET_DATA),
        HLE_MAP("a5xFueMZIMs", SHADPS4_HLE_CAMERA_GET_VALUE),
        HLE_MAP("tslCukqFE+E", SHADPS4_HLE_CAMERA_GET_VALUE),
        HLE_MAP("DSOLCrc3Kh8", SHADPS4_HLE_CAMERA_GET_DATA),
        HLE_MAP("n+rFeP1XXyM", SHADPS4_HLE_CAMERA_GET_DATA),
        HLE_MAP("WZpxnSAM-ds", SHADPS4_HLE_CAMERA_GET_DATA),
        HLE_MAP("ObIste7hqdk", SHADPS4_HLE_CAMERA_GET_VALUE),
        HLE_MAP("mxgMmR+1Kr0", SHADPS4_HLE_CAMERA_GET_DATA),
        HLE_MAP("WVox2rwGuSc", SHADPS4_HLE_CAMERA_GET_VALUE),
        HLE_MAP("zrIUDKZx0iE", SHADPS4_HLE_CAMERA_GET_VALUE),
        HLE_MAP("XqYRHc4aw3w", SHADPS4_HLE_CAMERA_GET_VALUE),
        HLE_MAP("RTDOsWWqdME", SHADPS4_HLE_CAMERA_GET_VALUE),
        HLE_MAP("c6Fp9M1EXXc", SHADPS4_HLE_CAMERA_GET_VALUE),
        HLE_MAP("HX5524E5tMY", SHADPS4_HLE_CAMERA_GET_VALUE),
        HLE_MAP("p6n3Npi3YY4", SHADPS4_HLE_CAMERA_IS_ATTACHED),
        HLE_MAP("U3BVwQl2R5Q", SHADPS4_HLE_CAMERA_GET_DATA),
        HLE_MAP("BHn83xrF92E", SHADPS4_HLE_CAMERA_OPEN),
        HLE_MAP("doPlf33ab-U", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("96F7zp1Xo+k", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("yfSdswDaElo", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("zIKL4kZleuc", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("VQ+5kAqsE2Q", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("9+SNhbctk64", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("3i5MEzrC1pg", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("jMv40y2A23g", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("vER3cIMBHqI", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("wgBMXJJA6K4", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("lhEIsHzB8r4", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("QI8GVJUy2ZY", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("K7W7H4ZRwbc", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("eHa3vhGu2rQ", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("bSKEi2PzzXI", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("P-7MVfzvpsM", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("k3zPIcgFNv0", SHADPS4_HLE_CAMERA_SET_VALUE),
        HLE_MAP("9EpRYMy7rHU", SHADPS4_HLE_CAMERA_START),
        HLE_MAP("cLxF1QtHch0", SHADPS4_HLE_CAMERA_START),
        HLE_MAP("2G2C0nmd++M", SHADPS4_HLE_CAMERA_STOP),
        HLE_MAP("+X1Kgnn3bzg", SHADPS4_HLE_CAMERA_STOP),
    };
    static const ShadPS4HLECompatMap hmd[] = {
        HLE_MAP("1cS7W5J-v3k", SHADPS4_HLE_HMD_DISTORT_ALIGN),
        HLE_MAP("1PNiQR-7L6k", SHADPS4_HLE_HMD_STUB),
        HLE_MAP("1pxQfif1rkE", SHADPS4_HLE_HMD_DEVICE_INFO),
        HLE_MAP("36xDKk+Hw7o", SHADPS4_HLE_HMD_DISTORT_SIZE),
        HLE_MAP("6biw1XHTSqQ", SHADPS4_HLE_HMD_CLOSE),
        HLE_MAP("8Ick-e6cDVY", SHADPS4_HLE_HMD_DISTORT_ALIGN),
        HLE_MAP("BWY-qKM5hxE", SHADPS4_HLE_HMD_EYE_OFFSET),
        HLE_MAP("d2g5Ij7EUzo", SHADPS4_HLE_HMD_OPEN),
        HLE_MAP("D5JfdpJKvXk", SHADPS4_HLE_HMD_DISTORT_SIZE),
        HLE_MAP("ER2ar8yUmbk", SHADPS4_HLE_HMD_STUB),
        HLE_MAP("IWybWbR-xvA", SHADPS4_HLE_HMD_ONION_ALIGN),
        HLE_MAP("K4KnH0QkT2c", SHADPS4_HLE_HMD_INITIALIZE),
        HLE_MAP("kLUAkN6a1e8", SHADPS4_HLE_HMD_ONION_SIZE),
        HLE_MAP("miduc55U7q8", SHADPS4_HLE_HMD_STUB),
        HLE_MAP("nlAZlOKJy+c", SHADPS4_HLE_HMD_STUB),
        HLE_MAP("NPQwYFqi0bs", SHADPS4_HLE_HMD_FIELD_OF_VIEW),
        HLE_MAP("PPCqsD8B5uM", SHADPS4_HLE_HMD_DEVICE_STATUS),
        HLE_MAP("rczCXLh2-b4", SHADPS4_HLE_HMD_STUB),
        HLE_MAP("s-J66ar9g50", SHADPS4_HLE_HMD_INITIALIZE),
        HLE_MAP("smQw6nT8PcA", SHADPS4_HLE_HMD_STUB),
        HLE_MAP("thDt9upZlp8", SHADPS4_HLE_HMD_DEVICE_INFO),
        HLE_MAP("TkcANcGM0s8", SHADPS4_HLE_HMD_GARLIC_ALIGN),
        HLE_MAP("Yx+CuF11D3Q", SHADPS4_HLE_HMD_ASSY_ERROR),
        HLE_MAP("z0KtN1vqF2E", SHADPS4_HLE_HMD_GARLIC_SIZE),
        HLE_MAP("z-RMILqP6tE", SHADPS4_HLE_HMD_TERMINATE),
        HLE_MAP("rU3HK9Q0r8o", SHADPS4_HLE_HMD_INERTIAL_DATA),
        HLE_MAP("aTg7K0466r8", SHADPS4_HLE_HMD_INERTIAL_DATA),
        HLE_MAP("ao8NZ+FRYJE", SHADPS4_HLE_HMD_DISTORT_INITIALIZE),
    };
    static const ShadPS4HLECompatMap audio3d[] = {
        HLE_MAP("pZlOm1aF3aA", SHADPS4_HLE_AUDIO3D_AUDIO_OUT),
        HLE_MAP("ucEsi62soTo", SHADPS4_HLE_AUDIO3D_AUDIO_OUT),
        HLE_MAP("7NYEzJ9SJbM", SHADPS4_HLE_AUDIO3D_AUDIO_OUT),
        HLE_MAP("HbxYY27lK6E", SHADPS4_HLE_AUDIO3D_AUDIO_OUT),
        HLE_MAP("9tEwE0GV0qo", SHADPS4_HLE_AUDIO3D_PORT_WRITE),
        HLE_MAP("xH4Q9UILL3o", SHADPS4_HLE_AUDIO3D_PORT_WRITE),
        HLE_MAP("Im+jOoa5WAI", SHADPS4_HLE_AUDIO3D_DEFAULT_PARAMS),
        HLE_MAP("UmCvjSmuZIw", SHADPS4_HLE_AUDIO3D_INITIALIZE),
        HLE_MAP("jO2tec4dJ2M", SHADPS4_HLE_AUDIO3D_OBJECT_RESERVE),
        HLE_MAP("V1FBFpNIAzk", SHADPS4_HLE_AUDIO3D_OBJECT_UPDATE),
        HLE_MAP("4uyHN9q4ZeU", SHADPS4_HLE_AUDIO3D_OBJECT_UPDATE),
        HLE_MAP("1HXxo-+1qCw", SHADPS4_HLE_AUDIO3D_OBJECT_UNRESERVE),
        HLE_MAP("lw0qrdSjZt8", SHADPS4_HLE_AUDIO3D_PORT_ADVANCE),
        HLE_MAP("OyVqOeVNtSk", SHADPS4_HLE_AUDIO3D_PORT_CLOSE),
        HLE_MAP("UHFOgVNz0kk", SHADPS4_HLE_AUDIO3D_PORT_CREATE),
        HLE_MAP("ZOGrxWLgQzE", SHADPS4_HLE_AUDIO3D_PORT_FLUSH),
        HLE_MAP("9ZA23Ia46Po", SHADPS4_HLE_AUDIO3D_PORT_GET_ATTRIBUTES),
        HLE_MAP("YaaDbDwKpFM", SHADPS4_HLE_AUDIO3D_PORT_GET_QUEUE),
        HLE_MAP("XeDDK0xJWQA", SHADPS4_HLE_AUDIO3D_PORT_OPEN),
        HLE_MAP("VEVhZ9qd4ZY", SHADPS4_HLE_AUDIO3D_PORT_PUSH),
        HLE_MAP("Yq9bfUQ0uJg", SHADPS4_HLE_AUDIO3D_PORT_SET_ATTRIBUTE),
        HLE_MAP("WW1TS2iz5yc", SHADPS4_HLE_AUDIO3D_TERMINATE),
    };
    static const ShadPS4HLECompatMap playgo[] = {
        HLE_MAP("ts6GlZOKRrE", SHADPS4_HLE_PLAYGO_INITIALIZE),
        HLE_MAP("MPe0EeBGM-E", SHADPS4_HLE_PLAYGO_TERMINATE),
        HLE_MAP("M1Gma1ocrGE", SHADPS4_HLE_PLAYGO_OPEN),
        HLE_MAP("Uco1I0dlDi8", SHADPS4_HLE_PLAYGO_CLOSE),
        HLE_MAP("73fF1MFU8hA", SHADPS4_HLE_PLAYGO_GET_CHUNK_ID),
        HLE_MAP("v6EZ-YWRdMs", SHADPS4_HLE_PLAYGO_GET_ETA),
        HLE_MAP("rvBSfTimejE", SHADPS4_HLE_PLAYGO_GET_SPEED),
        HLE_MAP("3OMbYZBaa50", SHADPS4_HLE_PLAYGO_GET_LANGUAGE),
        HLE_MAP("uWIYLFkkwqk", SHADPS4_HLE_PLAYGO_GET_LOCUS),
        HLE_MAP("-RJWNMK3fC8", SHADPS4_HLE_PLAYGO_GET_PROGRESS),
        HLE_MAP("Nn7zKwnA5q0", SHADPS4_HLE_PLAYGO_GET_TODO),
        HLE_MAP("-Q1-u1a7p0g", SHADPS4_HLE_PLAYGO_PREFETCH),
        HLE_MAP("4AAcTU9R3XM", SHADPS4_HLE_PLAYGO_SET_SPEED),
        HLE_MAP("LosLlHOpNqQ", SHADPS4_HLE_PLAYGO_SET_LANGUAGE),
        HLE_MAP("gUPGiOQ1tmQ", SHADPS4_HLE_PLAYGO_SET_TODO),
    };
    static const ShadPS4HLECompatMap np_auth[] = {
        HLE_MAP("6bwFkosYRQg", SHADPS4_HLE_NP_AUTH_CREATE_REQUEST),
        HLE_MAP("N+mr7GjTvr8", SHADPS4_HLE_NP_AUTH_CREATE_ASYNC_REQUEST),
        HLE_MAP("KxGkOrQJTqY", SHADPS4_HLE_NP_AUTH_GET_AUTH_CODE),
        HLE_MAP("qAUXQ9GdWp8", SHADPS4_HLE_NP_AUTH_GET_AUTH_CODE_A),
        HLE_MAP("KI4dHLlTNl0", SHADPS4_HLE_NP_AUTH_GET_AUTH_CODE_A),
        HLE_MAP("uaB-LoJqHis", SHADPS4_HLE_NP_AUTH_GET_ID_TOKEN),
        HLE_MAP("CocbHVIKPE8", SHADPS4_HLE_NP_AUTH_GET_ID_TOKEN_A),
        HLE_MAP("RdsFVsgSpZY", SHADPS4_HLE_NP_AUTH_GET_ID_TOKEN_A),
        HLE_MAP("cE7wIsqXdZ8", SHADPS4_HLE_NP_AUTH_ABORT_REQUEST),
        HLE_MAP("SK-S7daqJSE", SHADPS4_HLE_NP_AUTH_WAIT_ASYNC),
        HLE_MAP("gjSyfzSsDcE", SHADPS4_HLE_NP_AUTH_POLL_ASYNC),
        HLE_MAP("H8wG9Bk-nPc", SHADPS4_HLE_NP_AUTH_DELETE_REQUEST),
    };
    static const ShadPS4HLECompatMap ime[] = {
        HLE_MAP("WmYDzdC4EHI", SHADPS4_HLE_IME_PARAM_INIT),
        HLE_MAP("RPydv-Jr1bc", SHADPS4_HLE_IME_OPEN),
        HLE_MAP("TmVP8LzcFcY", SHADPS4_HLE_IME_CLOSE),
        HLE_MAP("-4GCfYdNF1s", SHADPS4_HLE_IME_UPDATE),
        HLE_MAP("ieCNrVrzKd4", SHADPS4_HLE_IME_SET_TEXT),
        HLE_MAP("WLxUN2WMim8", SHADPS4_HLE_IME_SET_CARET),
        HLE_MAP("TXYHFRuL8UY", SHADPS4_HLE_IME_SET_TEXT_GEOMETRY),
        HLE_MAP("ziPDcIjO0Vk", SHADPS4_HLE_IME_GET_PANEL_SIZE),
        HLE_MAP("T6FYjZXG93o", SHADPS4_HLE_IME_GET_PANEL_POSITION),
        HLE_MAP("eaFXjfJv3xs", SHADPS4_HLE_IME_KEYBOARD_OPEN),
        HLE_MAP("PMVehSlfZ94", SHADPS4_HLE_IME_KEYBOARD_CLOSE),
        HLE_MAP("dKadqZFgKKQ", SHADPS4_HLE_IME_KEYBOARD_GET_RESOURCE),
        HLE_MAP("VkqLPArfFdc", SHADPS4_HLE_IME_KEYBOARD_GET_INFO),
    };
    static const ShadPS4HLECompatMap np_trophy[] = {
        HLE_MAP("XbkjbobZlCY", SHADPS4_HLE_TROPHY_CREATE_CONTEXT),
        HLE_MAP("E1Wrwd07Lr8", SHADPS4_HLE_TROPHY_DESTROY_CONTEXT),
        HLE_MAP("q7U6tEAQf7c", SHADPS4_HLE_TROPHY_CREATE_HANDLE),
        HLE_MAP("GNcF4oidY0Y", SHADPS4_HLE_TROPHY_DESTROY_HANDLE),
        HLE_MAP("TJCAxto9SEU", SHADPS4_HLE_TROPHY_REGISTER_CONTEXT),
        HLE_MAP("28xmRUFao68", SHADPS4_HLE_TROPHY_UNLOCK),
        HLE_MAP("LHuSmO3SLd8", SHADPS4_HLE_TROPHY_GET_UNLOCK_STATE),
        HLE_MAP("HLwz1fRIycA", SHADPS4_HLE_TROPHY_GET_GAME_ICON),
        HLE_MAP("eBL+l6HG9xk", SHADPS4_HLE_TROPHY_GET_TROPHY_ICON),
        HLE_MAP("YYP3f2W09og", SHADPS4_HLE_TROPHY_GET_GAME_INFO),
        HLE_MAP("wTUwGfspKic", SHADPS4_HLE_TROPHY_GET_GROUP_INFO),
        HLE_MAP("qqUVGDgQBm0", SHADPS4_HLE_TROPHY_GET_TROPHY_INFO),
    };
    static const ShadPS4HLECompatMap videodec2[] = {
        HLE_MAP("eD+X2SmxUt4", SHADPS4_HLE_VIDEODEC2_ALLOCATE_QUEUE),
        HLE_MAP("CNNRoRYd8XI", SHADPS4_HLE_VIDEODEC2_CREATE),
        HLE_MAP("852F5+q6+iM", SHADPS4_HLE_VIDEODEC2_DECODE),
        HLE_MAP("jwImxXRGSKA", SHADPS4_HLE_VIDEODEC2_DELETE),
        HLE_MAP("l1hXwscLuCY", SHADPS4_HLE_VIDEODEC2_FLUSH),
        HLE_MAP("kjrLbcyhEiw", SHADPS4_HLE_VIDEODEC2_GET_AVC_INFO),
        HLE_MAP("NtXRa3dRzU0", SHADPS4_HLE_VIDEODEC2_GET_INFO),
        HLE_MAP("RnDibcGCPKw", SHADPS4_HLE_VIDEODEC2_QUERY_COMPUTE),
        HLE_MAP("qqMCwlULR+E", SHADPS4_HLE_VIDEODEC2_QUERY_DECODER),
        HLE_MAP("UvtA3FAiF4Y", SHADPS4_HLE_VIDEODEC2_RELEASE_QUEUE),
        HLE_MAP("wJXikG6QFN8", SHADPS4_HLE_VIDEODEC2_RESET),
    };
    static const ShadPS4HLECompatMap commerce[] = {
        HLE_MAP("NU3ckGHMFXo", SHADPS4_HLE_COMMERCE_CLOSE),
        HLE_MAP("r42bWcQbtZY", SHADPS4_HLE_COMMERCE_RESULT),
        HLE_MAP("CCbC+lqqvF0", SHADPS4_HLE_COMMERCE_STATUS),
        HLE_MAP("0aR2aWmQal4", SHADPS4_HLE_COMMERCE_INIT),
        HLE_MAP("9ZiLXAGG5rg", SHADPS4_HLE_COMMERCE_INIT),
        HLE_MAP("DfSCDRA3EjY", SHADPS4_HLE_COMMERCE_OPEN),
        HLE_MAP("m-I92Ab50W8", SHADPS4_HLE_COMMERCE_TERM),
        HLE_MAP("LR5cwFMMCVE", SHADPS4_HLE_COMMERCE_UPDATE),
        HLE_MAP("dsqCVsNM0Zg", SHADPS4_HLE_COMMERCE_HIDE_ICON),
        HLE_MAP("uKTDW8hk-ts", SHADPS4_HLE_COMMERCE_ICON_LAYOUT),
        HLE_MAP("DHmwsa6S8Tc", SHADPS4_HLE_COMMERCE_SHOW_ICON),
    };
    static const ShadPS4HLECompatMap ssl2[] = {
        HLE_MAP("4O7+bRkRUe8", SHADPS4_HLE_SSL2_GET_ALPN),
        HLE_MAP("brRtwGBu4A8", SHADPS4_HLE_SSL2_GET_FINGERPRINT),
        HLE_MAP("HJ1n138CQ2g", SHADPS4_HLE_SSL2_DELETE),
        HLE_MAP("iNjkt9Poblw", SHADPS4_HLE_SSL2_WRITE),
        HLE_MAP("jltWpVKtetg", SHADPS4_HLE_SSL2_READ),
        HLE_MAP("kLB5aGoUJXg", SHADPS4_HLE_SSL2_GET_PEM),
        HLE_MAP("po1X86mgHDU", SHADPS4_HLE_SSL2_ENABLE_VERIFY),
        HLE_MAP("PwsHbErG+e8", SHADPS4_HLE_SSL2_DISABLE_VERIFY),
        HLE_MAP("-TbZc8pwPNc", SHADPS4_HLE_SSL2_GET_PEER_CERT),
        HLE_MAP("TL86glUrmUw", SHADPS4_HLE_SSL2_SET_ALPN),
        HLE_MAP("tuscfitnhEo", SHADPS4_HLE_SSL2_CREATE),
    };
    static const ShadPS4HLECompatMap ime_dialog[] = {
        HLE_MAP("-2WqB87KKGg", SHADPS4_HLE_IME_DIALOG_SET_POSITION),
        HLE_MAP("8jqzzPioYl8", SHADPS4_HLE_IME_DIALOG_GET_POSITION),
        HLE_MAP("bX4H+sxPI-o", SHADPS4_HLE_IME_DIALOG_FORCE_CLOSE),
        HLE_MAP("CRD+jSErEJQ", SHADPS4_HLE_IME_DIALOG_GET_SIZE_EXT),
        HLE_MAP("fy6ntM25pEc", SHADPS4_HLE_IME_DIALOG_GET_STAR),
        HLE_MAP("IoKIpNf9EK0", SHADPS4_HLE_IME_DIALOG_INIT_INTERNAL3),
        HLE_MAP("KR6QDasuKco", SHADPS4_HLE_IME_DIALOG_INIT_INTERNAL),
        HLE_MAP("oe92cnJQ9HE", SHADPS4_HLE_IME_DIALOG_INIT_INTERNAL2),
        HLE_MAP("UFcyYDf+e88", SHADPS4_HLE_IME_DIALOG_TEST),
        HLE_MAP("wqsJvRXwl58", SHADPS4_HLE_IME_DIALOG_GET_SIZE),
    };
    static const ShadPS4HLECompatMap invitation_dialog[] = {
        HLE_MAP("WWtCL5lzi7Y", SHADPS4_HLE_INVITATION_CLOSE),
        HLE_MAP("8XKR6wa64iQ", SHADPS4_HLE_INVITATION_RESULT),
        HLE_MAP("WuuUhuKOxwQ", SHADPS4_HLE_INVITATION_RESULT_A),
        HLE_MAP("EiF92YDNHRA", SHADPS4_HLE_INVITATION_STATUS),
        HLE_MAP("XvA5KS56wcs", SHADPS4_HLE_INVITATION_INIT),
        HLE_MAP("0zU0G+wiVLA", SHADPS4_HLE_INVITATION_OPEN),
        HLE_MAP("sAxbHhAWMXM", SHADPS4_HLE_INVITATION_OPEN_A),
        HLE_MAP("B6HVJtDYxEE", SHADPS4_HLE_INVITATION_TERM),
        HLE_MAP("9+g9iOq+7kg", SHADPS4_HLE_INVITATION_UPDATE),
    };
    static const ShadPS4HLECompatMap playgo_dialog[] = {
        HLE_MAP("fbigNQiZpm0", SHADPS4_HLE_PLAYGO_DIALOG_CLOSE),
        HLE_MAP("wx9TDplJKB4", SHADPS4_HLE_PLAYGO_DIALOG_RESULT),
        HLE_MAP("NOAMxY2EGS0", SHADPS4_HLE_PLAYGO_DIALOG_STATUS),
        HLE_MAP("fECamTJKpsM", SHADPS4_HLE_PLAYGO_DIALOG_INIT),
        HLE_MAP("kHd72ukqbxw", SHADPS4_HLE_PLAYGO_DIALOG_OPEN),
        HLE_MAP("okgIGdr5Iz0", SHADPS4_HLE_PLAYGO_DIALOG_TERM),
        HLE_MAP("Yb60K7BST48", SHADPS4_HLE_PLAYGO_DIALOG_UPDATE),
    };
    static const ShadPS4HLECompatMap profile_dialog[] = {
        HLE_MAP("wkwjz0Xdo2A", SHADPS4_HLE_PROFILE_DIALOG_CLOSE),
        HLE_MAP("8rhLl1-0W-o", SHADPS4_HLE_PROFILE_DIALOG_RESULT),
        HLE_MAP("3BqoiFOjSsk", SHADPS4_HLE_PROFILE_DIALOG_STATUS),
        HLE_MAP("Lg+NCE6pTwQ", SHADPS4_HLE_PROFILE_DIALOG_INIT),
        HLE_MAP("uj9Cz7Tk0cc", SHADPS4_HLE_PROFILE_DIALOG_OPEN),
        HLE_MAP("nrQRlLKzdwE", SHADPS4_HLE_PROFILE_DIALOG_OPEN),
        HLE_MAP("0Sp9vJcB1-w", SHADPS4_HLE_PROFILE_DIALOG_TERM),
        HLE_MAP("haVZE9FgKqE", SHADPS4_HLE_PROFILE_DIALOG_UPDATE),
    };
    static const ShadPS4HLECompatMap vr_tracker[] = {
        HLE_MAP("IBv4P3q1pQ0", SHADPS4_HLE_VR_TRACKER_TERM),
        HLE_MAP("K7yhYrsIBPc", SHADPS4_HLE_VR_TRACKER_QUERY_MEMORY),
        HLE_MAP("Q8skQqEwn5c", SHADPS4_HLE_VR_TRACKER_UNREGISTER),
        HLE_MAP("QkRl7pART9M", SHADPS4_HLE_VR_TRACKER_INIT),
        HLE_MAP("sIh8GwcevaQ", SHADPS4_HLE_VR_TRACKER_REGISTER),
        HLE_MAP("ufexf4aNiwg", SHADPS4_HLE_VR_TRACKER_REGISTER_INTERNAL),
        HLE_MAP("XoeWzXlrnMw", SHADPS4_HLE_VR_TRACKER_GET_TIME),
        HLE_MAP("jGqEkPy0iLU", SHADPS4_HLE_VR_TRACKER_REJECTION),
        HLE_MAP("24kDA+A0Ox0", SHADPS4_HLE_VR_TRACKER_REGISTER2),
        HLE_MAP("TVegDMLaBB8", SHADPS4_HLE_VR_TRACKER_GPU_SUBMIT),
        HLE_MAP("gkGuO9dd57M", SHADPS4_HLE_VR_TRACKER_GPU_WAIT),
        HLE_MAP("ARhgpXvwoR0", SHADPS4_HLE_VR_TRACKER_GPU_WAIT_CPU),
        HLE_MAP("EUCaQtXXYNI", SHADPS4_HLE_VR_TRACKER_RECALIBRATE),
        HLE_MAP("qBjnR0HtMYI", SHADPS4_HLE_VR_TRACKER_SET_DURATION),
    };
    static const ShadPS4HLECompatMap residual[] = {
        HLE_MAP("Gl6w5i0JokY", SHADPS4_HLE_APP_CONTENT_AVAILABLE_SPACE),
        HLE_MAP("SaKib2Ug0yI", SHADPS4_HLE_APP_CONTENT_AVAILABLE_SPACE),
        HLE_MAP("FzEWeYnAFlI", SHADPS4_HLE_CONTENT_EXPORT_INIT),
        HLE_MAP("0GnN4QCgIfs", SHADPS4_HLE_CONTENT_EXPORT_INIT2),
        HLE_MAP("+KDWny9Y-6k", SHADPS4_HLE_CONTENT_EXPORT_TERM),
        HLE_MAP("cJLufzou6bc", SHADPS4_HLE_VOICE_GET_BITRATE),
        HLE_MAP("CrLqDwWLoXM", SHADPS4_HLE_VOICE_GET_PORT_INFO),
        HLE_MAP("F1P+-wpxQow", SHADPS4_HLE_NP_PARTY_NOT_IN_PARTY),
        HLE_MAP("T2UOKf00ZN0", SHADPS4_HLE_NP_PARTY_NOT_IN_PARTY),
        HLE_MAP("TaNw7W25QJw", SHADPS4_HLE_NP_PARTY_NOT_IN_PARTY),
        HLE_MAP("aEzKdJzATZ0", SHADPS4_HLE_NP_PARTY_NOT_IN_PARTY),
        HLE_MAP("v2RYVGrJDkM", SHADPS4_HLE_NP_PARTY_NOT_IN_PARTY),
        HLE_MAP("pQfYTZHznMc", SHADPS4_HLE_NP_PARTNER_ABORT),
        HLE_MAP("cE5Msy11WhU", SHADPS4_HLE_COMPANION_GET_EVENT),
        HLE_MAP("BQ3tey0JmQM", SHADPS4_HLE_COMMON_DIALOG_IS_USED),
        HLE_MAP("Vku4big+IYM", SHADPS4_HLE_COMPANION_GET_EVENT),
        HLE_MAP("ekXHb1kDBl0", SHADPS4_HLE_ERROR_DIALOG_CLOSE),
        HLE_MAP("t2FvHRXzgqk", SHADPS4_HLE_ERROR_DIALOG_GET_STATUS),
        HLE_MAP("I88KChlynSs", SHADPS4_HLE_ERROR_DIALOG_INIT),
        HLE_MAP("M2ZF-ClLhgY", SHADPS4_HLE_ERROR_DIALOG_OPEN),
        HLE_MAP("9XAxK2PMwk8", SHADPS4_HLE_ERROR_DIALOG_TERM),
        HLE_MAP("WWiGuh9XfgQ", SHADPS4_HLE_ERROR_DIALOG_UPDATE),
        HLE_MAP("caqgDl+V9qA", SHADPS4_HLE_GAME_LIVE_START_DEBUG),
        HLE_MAP("0i8Lrllxwow", SHADPS4_HLE_GAME_LIVE_STOP_DEBUG),
        HLE_MAP("K+rocojkr-I", SHADPS4_HLE_JPEG_CREATE),
        HLE_MAP("j1LyMdaM+C0", SHADPS4_HLE_JPEG_DELETE),
        HLE_MAP("QbrU0cUghEM", SHADPS4_HLE_JPEG_ENCODE),
        HLE_MAP("o6ZgXfFdWXQ", SHADPS4_HLE_JPEG_QUERY_MEMORY),
        HLE_MAP("cAnT0Rw-IwU", SHADPS4_HLE_MOUSE_CLOSE),
        HLE_MAP("Qs0wWulgl7U", SHADPS4_HLE_MOUSE_INIT),
        HLE_MAP("RaqxZIf6DvE", SHADPS4_HLE_MOUSE_OPEN),
        HLE_MAP("x8qnXqh-tiM", SHADPS4_HLE_MOUSE_READ),
        HLE_MAP("Gc5k1qcK4fs", SHADPS4_HLE_MSG_PROGRESS_INC),
        HLE_MAP("6H-71OdrpXM", SHADPS4_HLE_MSG_PROGRESS_SET_MSG),
        HLE_MAP("wTpfglkmv34", SHADPS4_HLE_MSG_PROGRESS_SET_VALUE),
        HLE_MAP("7CxI50-xlCk", SHADPS4_HLE_NP_PARTNER_INIT),
        HLE_MAP("pMxXhNozUX8", SHADPS4_HLE_NP_PARTNER_TERM),
        HLE_MAP("+OnbUs1CV0M", SHADPS4_HLE_NP_PARTNER_SUBSCRIPTION),
        HLE_MAP("m0uW+8pFyaw", SHADPS4_HLE_PNG_CREATE),
        HLE_MAP("WC216DD3El4", SHADPS4_HLE_PNG_DECODE),
        HLE_MAP("QbD+eENEwo8", SHADPS4_HLE_PNG_DELETE),
        HLE_MAP("U6h4e5JRPaQ", SHADPS4_HLE_PNG_PARSE_HEADER),
        HLE_MAP("-6srIGbLTIU", SHADPS4_HLE_PNG_QUERY_MEMORY),
        HLE_MAP("PI7jIZj4pcE", SHADPS4_HLE_RANDOM_GET),
        HLE_MAP("g3PNjYKWqnQ", SHADPS4_HLE_REMOTEPLAY_STATUS),
        HLE_MAP("en7gNVnh878", SHADPS4_HLE_SAVEDATA_DIALOG_READY),
        HLE_MAP("V-uEeFKARJU", SHADPS4_HLE_SAVEDATA_DIALOG_PROGRESS_INC),
        HLE_MAP("itlWFWV3Tzc", SHADPS4_HLE_SCREENSHOT_SET_DRC),
        HLE_MAP("qkgRiwHyheU", SHADPS4_HLE_VIDEODEC_CREATE),
        HLE_MAP("q0W5GJMovMs", SHADPS4_HLE_VIDEODEC_DECODE),
        HLE_MAP("U0kpGF1cl90", SHADPS4_HLE_VIDEODEC_DELETE),
        HLE_MAP("jeigLlKdp5I", SHADPS4_HLE_VIDEODEC_FLUSH),
        HLE_MAP("leCAscipfFY", SHADPS4_HLE_VIDEODEC_QUERY),
        HLE_MAP("f8AgDv-1X8A", SHADPS4_HLE_VIDEODEC_RESET),
        HLE_MAP("PSK+Eik919Q", SHADPS4_HLE_WEB_DIALOG_CLOSE),
        HLE_MAP("CFTG6a8TjOU", SHADPS4_HLE_WEB_DIALOG_GET_STATUS),
        HLE_MAP("jqb7HntFQFc", SHADPS4_HLE_WEB_DIALOG_INIT),
        HLE_MAP("FraP7debcdg", SHADPS4_HLE_WEB_DIALOG_OPEN),
        HLE_MAP("ocHtyBwHfys", SHADPS4_HLE_WEB_DIALOG_TERM),
        HLE_MAP("h1dR-t5ISgg", SHADPS4_HLE_WEB_DIALOG_UPDATE),
        HLE_MAP("8r4EJ3FiX4w", SHADPS4_HLE_WEB_DIALOG_LIMITED),
        HLE_MAP("BG26hBGiNlw", SHADPS4_HLE_ULOBJ_REGISTER),
        HLE_MAP("Smf+fUNblPc", SHADPS4_HLE_ULOBJ_UNREGISTER),
        HLE_MAP("HZ9Q2c+4BU4", SHADPS4_HLE_ULOBJ_SUCCESS),
        HLE_MAP("SweJO7t3pkk", SHADPS4_HLE_ULOBJ_VALIDATE),
        HLE_MAP("CoPMx369EqM", SHADPS4_HLE_GAME_LIVE_STATUS),
        HLE_MAP("6lVRHMV5LY0", SHADPS4_HLE_HMD_SETUP_RESULT),
        HLE_MAP("J9eBpW1udl4", SHADPS4_HLE_DIALOG_STATUS_FINISHED),
        HLE_MAP("Ud7j3+RDIBg", SHADPS4_HLE_DIALOG_STATUS_FINISHED),
        HLE_MAP("2m077aeC+PA", SHADPS4_HLE_DIALOG_STATUS_FINISHED),
        HLE_MAP("Bw31liTFT3A", SHADPS4_HLE_DIALOG_STATUS_FINISHED),
        HLE_MAP("OOrLKB0bSDs", SHADPS4_HLE_SHAREPLAY_CONNECTION_INFO),
        HLE_MAP("TDfQqO-gMbY", SHADPS4_HLE_SSL_GET_CA_CERTS),
        HLE_MAP("qIvLs0gYxi0", SHADPS4_HLE_SSL_FREE_CA_CERTS),
        HLE_MAP("Fc8qxlKINYQ", SHADPS4_HLE_VIDEO_RECORDING_SET_INFO),
        HLE_MAP("fjV7C8H0Y8k", SHADPS4_HLE_SUCCESS),
        HLE_MAP("0zkr0T+NYvI", SHADPS4_HLE_NP_TUS_DATA),
        HLE_MAP("tKLmVIUkpyM", SHADPS4_HLE_SUCCESS),
        HLE_MAP("ua+13Hk9kKs", SHADPS4_HLE_SUCCESS),
        HLE_MAP("TQaogSaqkEk", SHADPS4_HLE_SUCCESS),
    };
    static const ShadPS4HLECompatMap np_webapi2[] = {
        HLE_MAP("zpiPsH7dbFQ", SHADPS4_HLE_HTTP2_ABORT_REQUEST),
        HLE_MAP("egOOvrnF6mI", SHADPS4_HLE_HTTP_ADD_HEADER),
        HLE_MAP("MgsTa76wlEk", SHADPS4_HLE_WEBAPI2_OPTION),
        HLE_MAP("3Tt9zL3tkoc", SHADPS4_HLE_WEBAPI_CHECK_TIMEOUT),
        HLE_MAP("+nz1Vq-NrDA", SHADPS4_HLE_WEBAPI2_CREATE_MULTIPART_REQUEST),
        HLE_MAP("3EI-OSJ65Xc", SHADPS4_HLE_WEBAPI2_CREATE_REQUEST),
        HLE_MAP("sk54bi6FtYM", SHADPS4_HLE_WEBAPI2_CREATE_USER_ID),
        HLE_MAP("vvzWO-DvG1s", SHADPS4_HLE_HTTP_DELETE_REQUEST),
        HLE_MAP("9X9+cneTGUU", SHADPS4_HLE_WEBAPI_DELETE_USER),
        HLE_MAP("hksbskNToEA", SHADPS4_HLE_WEBAPI2_GET_HEADER_LENGTH),
        HLE_MAP("HwP3aM+c85c", SHADPS4_HLE_WEBAPI2_GET_HEADER_VALUE),
        HLE_MAP("Xweb+naPZ8Y", SHADPS4_HLE_WEBAPI2_GET_MEMORY_STATS),
        HLE_MAP("+o9816YQhqQ", SHADPS4_HLE_WEBAPI2_INIT),
        HLE_MAP("dowMWFgowXY", SHADPS4_HLE_WEBAPI2_INIT),
        HLE_MAP("OOY9+ObfKec", SHADPS4_HLE_HTTP_READ_DATA),
        HLE_MAP("NKCwS8+5Fx8", SHADPS4_HLE_WEBAPI2_SEND_MULTIPART),
        HLE_MAP("lQOCF84lvzw", SHADPS4_HLE_WEBAPI2_SEND_WITH_INFO),
        HLE_MAP("TjAutbrkr60", SHADPS4_HLE_WEBAPI2_OPTION),
        HLE_MAP("bEvXpcEk200", SHADPS4_HLE_WEBAPI_DELETE_CONTEXT),
        HLE_MAP("A9IoYzANK3M", SHADPS4_HLE_SUCCESS),
        HLE_MAP("lylvdXiq1UE", SHADPS4_HLE_SUCCESS),
        HLE_MAP("4N85o28Ifbk", SHADPS4_HLE_SUCCESS),
        HLE_MAP("zXaFo7euxsQ", SHADPS4_HLE_WEBAPI2_PUSH_INTERNAL_INIT),
        HLE_MAP("9KSGFMRnp3k", SHADPS4_HLE_WEBAPI2_PUSH_INTERNAL_INIT),
        HLE_MAP("WV1GwM32NgY", SHADPS4_HLE_WEBAPI2_PUSH_CREATE_HANDLE),
        HLE_MAP("fIATVMo4Y1w", SHADPS4_HLE_WEBAPI2_PUSH_DELETE_HANDLE),
        HLE_MAP("1OLgvahaSco", SHADPS4_HLE_WEBAPI2_PUSH_ABORT_HANDLE),
        HLE_MAP("KWkc6Q3tjXc", SHADPS4_HLE_WEBAPI2_PUSH_SET_TIMEOUT),
        HLE_MAP("MsaFhR+lPE4", SHADPS4_HLE_WEBAPI2_PUSH_CREATE_FILTER),
        HLE_MAP("KJdPcOGmK58", SHADPS4_HLE_WEBAPI2_PUSH_DELETE_FILTER),
        HLE_MAP("fY3QqeNkF8k", SHADPS4_HLE_WEBAPI2_PUSH_REGISTER_CALLBACK),
        HLE_MAP("lxtHJMwBsaU", SHADPS4_HLE_WEBAPI2_PUSH_REGISTER_CONTEXT_CALLBACK),
        HLE_MAP("hOnIlcGrO6g", SHADPS4_HLE_WEBAPI2_PUSH_UNREGISTER_CALLBACK),
        HLE_MAP("PmyrbbJSFz0", SHADPS4_HLE_WEBAPI2_PUSH_UNREGISTER_CALLBACK),
        HLE_MAP("NNVf18SlbT8", SHADPS4_HLE_WEBAPI2_PUSH_CREATE_CONTEXT),
        HLE_MAP("QafxeZM3WK4", SHADPS4_HLE_WEBAPI2_PUSH_DELETE_CONTEXT),
        HLE_MAP("AAj9X+4aGYA", SHADPS4_HLE_WEBAPI2_PUSH_START_CONTEXT),
    };
    static const ShadPS4HLECompatMap kernel_sync[] = {
        HLE_MAP("wtkt-teR1so", SHADPS4_HLE_PTHREAD_ATTR_INIT),
        HLE_MAP("nsYoNRywwNg", SHADPS4_HLE_PTHREAD_ATTR_INIT),
        HLE_MAP("zHchY8ft5pk", SHADPS4_HLE_PTHREAD_ATTR_DESTROY),
        HLE_MAP("62KCwEMmzcM", SHADPS4_HLE_PTHREAD_ATTR_DESTROY),
        HLE_MAP("Ucsu-OK+els", SHADPS4_HLE_PTHREAD_ATTR_GET_NP),
        HLE_MAP("x1X76arYMxU", SHADPS4_HLE_PTHREAD_ATTR_GET_NP),
        HLE_MAP("-wzZ7dvA7UU", SHADPS4_HLE_PTHREAD_ATTR_GET_AFFINITY),
        HLE_MAP("8+s5BzZjxSg", SHADPS4_HLE_PTHREAD_ATTR_GET_AFFINITY),
        HLE_MAP("o8pd4juNbgc", SHADPS4_HLE_PTHREAD_ATTR_SET_AFFINITY),
        HLE_MAP("3qxgM4ezETA", SHADPS4_HLE_PTHREAD_ATTR_SET_AFFINITY),
        HLE_MAP("VUT1ZSrHT0I", SHADPS4_HLE_PTHREAD_ATTR_GET_DETACH),
        HLE_MAP("JaRMy+QcpeU", SHADPS4_HLE_PTHREAD_ATTR_GET_DETACH),
        HLE_MAP("E+tyo3lp5Lw", SHADPS4_HLE_PTHREAD_ATTR_SET_DETACH),
        HLE_MAP("-Wreprtu0Qs", SHADPS4_HLE_PTHREAD_ATTR_SET_DETACH),
        HLE_MAP("JNkVVsVDmOk", SHADPS4_HLE_PTHREAD_ATTR_GET_GUARD),
        HLE_MAP("txHtngJ+eyc", SHADPS4_HLE_PTHREAD_ATTR_GET_GUARD),
        HLE_MAP("JKyG3SWyA10", SHADPS4_HLE_PTHREAD_ATTR_SET_GUARD),
        HLE_MAP("El+cQ20DynU", SHADPS4_HLE_PTHREAD_ATTR_SET_GUARD),
        HLE_MAP("oLjPqUKhzes", SHADPS4_HLE_PTHREAD_ATTR_GET_INHERIT),
        HLE_MAP("lpMP8HhkBbg", SHADPS4_HLE_PTHREAD_ATTR_GET_INHERIT),
        HLE_MAP("7ZlAakEf0Qg", SHADPS4_HLE_PTHREAD_ATTR_SET_INHERIT),
        HLE_MAP("eXbUSpEaTsA", SHADPS4_HLE_PTHREAD_ATTR_SET_INHERIT),
        HLE_MAP("qlk9pSLsUmM", SHADPS4_HLE_PTHREAD_ATTR_GET_SCHEDPARAM),
        HLE_MAP("FXPWHNk8Of0", SHADPS4_HLE_PTHREAD_ATTR_GET_SCHEDPARAM),
        HLE_MAP("euKRgm0Vn2M", SHADPS4_HLE_PTHREAD_ATTR_SET_SCHEDPARAM),
        HLE_MAP("DzES9hQF4f4", SHADPS4_HLE_PTHREAD_ATTR_SET_SCHEDPARAM),
        HLE_MAP("RtLRV-pBTTY", SHADPS4_HLE_PTHREAD_ATTR_GET_POLICY),
        HLE_MAP("NMyIQ9WgWbU", SHADPS4_HLE_PTHREAD_ATTR_GET_POLICY),
        HLE_MAP("JarMIy8kKEY", SHADPS4_HLE_PTHREAD_ATTR_SET_POLICY),
        HLE_MAP("4+h9EzwKF4I", SHADPS4_HLE_PTHREAD_ATTR_SET_POLICY),
        HLE_MAP("e2G+cdEkOmU", SHADPS4_HLE_PTHREAD_ATTR_GET_SCOPE),
        HLE_MAP("+7B2AEKKns8", SHADPS4_HLE_PTHREAD_ATTR_GET_SCOPE),
        HLE_MAP("xesmlSI-KCI", SHADPS4_HLE_PTHREAD_ATTR_SET_SCOPE),
        HLE_MAP("YdZfEZfRnPk", SHADPS4_HLE_PTHREAD_ATTR_SET_SCOPE),
        HLE_MAP("vQm4fDEsWi8", SHADPS4_HLE_PTHREAD_ATTR_GET_STACK),
        HLE_MAP("-quPa4SEJUw", SHADPS4_HLE_PTHREAD_ATTR_GET_STACK),
        HLE_MAP("-SrbXpGR1f0", SHADPS4_HLE_PTHREAD_ATTR_SET_STACK),
        HLE_MAP("Bvn74vj6oLo", SHADPS4_HLE_PTHREAD_ATTR_SET_STACK),
        HLE_MAP("DxmIMUQ-wXY", SHADPS4_HLE_PTHREAD_ATTR_GET_STACKADDR),
        HLE_MAP("Ru36fiTtJzA", SHADPS4_HLE_PTHREAD_ATTR_GET_STACKADDR),
        HLE_MAP("suCrEbr0xIQ", SHADPS4_HLE_PTHREAD_ATTR_SET_STACKADDR),
        HLE_MAP("F+yfmduIBB8", SHADPS4_HLE_PTHREAD_ATTR_SET_STACKADDR),
        HLE_MAP("0qOtCR-ZHck", SHADPS4_HLE_PTHREAD_ATTR_GET_STACKSIZE),
        HLE_MAP("-fA+7ZlGDQs", SHADPS4_HLE_PTHREAD_ATTR_GET_STACKSIZE),
        HLE_MAP("2Q0z6rnBrTE", SHADPS4_HLE_PTHREAD_ATTR_SET_STACKSIZE),
        HLE_MAP("UTXzJbWhhTE", SHADPS4_HLE_PTHREAD_ATTR_SET_STACKSIZE),
        HLE_MAP("Q2y5IqSDZGs", SHADPS4_HLE_PTHREAD_ATTR_SET_SUSPEND),
        HLE_MAP("GZSR0Ooae9Q", SHADPS4_HLE_PTHREAD_ATTR_SET_SUSPEND),

        HLE_MAP("dQHWEsJtoE4", SHADPS4_HLE_MUTEX_ATTR_INIT),
        HLE_MAP("n2MMpvU8igI", SHADPS4_HLE_MUTEX_ATTR_INIT),
        HLE_MAP("F8bUHwAG284", SHADPS4_HLE_MUTEX_ATTR_INIT),
        HLE_MAP("HF7lK46xzjY", SHADPS4_HLE_MUTEX_ATTR_DESTROY),
        HLE_MAP("smWEktiyyG0", SHADPS4_HLE_MUTEX_ATTR_DESTROY),
        HLE_MAP("U6SNV+RnyLQ", SHADPS4_HLE_MUTEX_ATTR_GET),
        HLE_MAP("+m8+quqOwhM", SHADPS4_HLE_MUTEX_ATTR_GET),
        HLE_MAP("yDaWxUE50s0", SHADPS4_HLE_MUTEX_ATTR_GET),
        HLE_MAP("PmL-TwKUzXI", SHADPS4_HLE_MUTEX_ATTR_GET),
        HLE_MAP("GZFlI7RhuQo", SHADPS4_HLE_MUTEX_ATTR_GET),
        HLE_MAP("rH2mWEndluc", SHADPS4_HLE_MUTEX_ATTR_GET),
        HLE_MAP("SgjMpyH9Z9I", SHADPS4_HLE_MUTEX_ATTR_GET),
        HLE_MAP("GoTmFeui+hQ", SHADPS4_HLE_MUTEX_ATTR_GET),
        HLE_MAP("losEubHc64c", SHADPS4_HLE_MUTEX_ATTR_GET),
        HLE_MAP("gquEhBrS2iw", SHADPS4_HLE_MUTEX_ATTR_GET),
        HLE_MAP("J9rlRuQ8H5s", SHADPS4_HLE_MUTEX_ATTR_SET),
        HLE_MAP("ZLvf6lVAc4M", SHADPS4_HLE_MUTEX_ATTR_SET),
        HLE_MAP("5txKfcMUAok", SHADPS4_HLE_MUTEX_ATTR_SET),
        HLE_MAP("EXv3ztGqtDM", SHADPS4_HLE_MUTEX_ATTR_SET),
        HLE_MAP("mDmgMOGVUqg", SHADPS4_HLE_MUTEX_ATTR_SET),
        HLE_MAP("UWZbVSFze24", SHADPS4_HLE_MUTEX_ATTR_SET),
        HLE_MAP("532IaQguwMg", SHADPS4_HLE_MUTEX_ATTR_SET),
        HLE_MAP("1FGvU0i9saQ", SHADPS4_HLE_MUTEX_ATTR_SET),
        HLE_MAP("mxKx9bxXF2I", SHADPS4_HLE_MUTEX_ATTR_SET),
        HLE_MAP("iMp8QpE+XO4", SHADPS4_HLE_MUTEX_ATTR_SET),
        HLE_MAP("ttHNfU+qDBU", SHADPS4_HLE_MUTEX_INIT),
        HLE_MAP("cmo1RIYva9o", SHADPS4_HLE_MUTEX_INIT),
        HLE_MAP("qH1gXoq71RY", SHADPS4_HLE_MUTEX_INIT),
        HLE_MAP("ltCfaGr2JGE", SHADPS4_HLE_MUTEX_DESTROY),
        HLE_MAP("2Of0f+3mhhE", SHADPS4_HLE_MUTEX_DESTROY),
        HLE_MAP("7H0iTOciTLo", SHADPS4_HLE_MUTEX_LOCK),
        HLE_MAP("Io9+nTKXZtA", SHADPS4_HLE_MUTEX_TIMEDLOCK),
        HLE_MAP("9UK1vLZQft4", SHADPS4_HLE_MUTEX_LOCK),
        HLE_MAP("IafI2PxcPnQ", SHADPS4_HLE_MUTEX_TIMEDLOCK),
        HLE_MAP("K-jXhbt2gn4", SHADPS4_HLE_MUTEX_TRYLOCK),
        HLE_MAP("upoVrzMHFeE", SHADPS4_HLE_MUTEX_TRYLOCK),
        HLE_MAP("2Z+PpY6CaJg", SHADPS4_HLE_MUTEX_UNLOCK),
        HLE_MAP("tn3VlD0hG60", SHADPS4_HLE_MUTEX_UNLOCK),
        HLE_MAP("gKqzW-zWhvY", SHADPS4_HLE_MUTEX_ISOWNED),
        HLE_MAP("W6OrTBO95UY", SHADPS4_HLE_MUTEX_ISOWNED),
        HLE_MAP("x4vQj3JKKmc", SHADPS4_HLE_MUTEX_GET_LOOPS),
        HLE_MAP("OxEIUqkByy4", SHADPS4_HLE_MUTEX_GET_LOOPS),
        HLE_MAP("pOmNmyRKlIE", SHADPS4_HLE_MUTEX_GET_LOOPS),
        HLE_MAP("AWS3NyViL9o", SHADPS4_HLE_MUTEX_GET_LOOPS),
        HLE_MAP("5-ncLMtL5+g", SHADPS4_HLE_MUTEX_SET_LOOPS),
        HLE_MAP("frFuGprJmPc", SHADPS4_HLE_MUTEX_SET_LOOPS),
        HLE_MAP("42YkUouoMI0", SHADPS4_HLE_MUTEX_SET_LOOPS),
        HLE_MAP("bP+cqFmBW+A", SHADPS4_HLE_MUTEX_SET_LOOPS),

        HLE_MAP("mKoTx03HRWA", SHADPS4_HLE_COND_ATTR_INIT),
        HLE_MAP("m5-2bsNfv7s", SHADPS4_HLE_COND_ATTR_INIT),
        HLE_MAP("dJcuQVn6-Iw", SHADPS4_HLE_COND_ATTR_DESTROY),
        HLE_MAP("waPcxYiR3WA", SHADPS4_HLE_COND_ATTR_DESTROY),
        HLE_MAP("h0qUqSuOmC8", SHADPS4_HLE_COND_ATTR_GET),
        HLE_MAP("cTDYxTUNPhM", SHADPS4_HLE_COND_ATTR_GET),
        HLE_MAP("Dn-DRWi9t54", SHADPS4_HLE_COND_ATTR_GET),
        HLE_MAP("6qM3kO5S3Oo", SHADPS4_HLE_COND_ATTR_GET),
        HLE_MAP("3BpP850hBT4", SHADPS4_HLE_COND_ATTR_SET),
        HLE_MAP("EjllaAqAPZo", SHADPS4_HLE_COND_ATTR_SET),
        HLE_MAP("6xMew9+rZwI", SHADPS4_HLE_COND_ATTR_SET),
        HLE_MAP("c-bxj027czs", SHADPS4_HLE_COND_ATTR_SET),
        HLE_MAP("0TyVk4MSLt0", SHADPS4_HLE_COND_INIT),
        HLE_MAP("2Tb92quprl0", SHADPS4_HLE_COND_INIT),
        HLE_MAP("RXXqi4CtF8w", SHADPS4_HLE_COND_DESTROY),
        HLE_MAP("g+PZd2hiacg", SHADPS4_HLE_COND_DESTROY),
        HLE_MAP("K953PF5u6Pc", SHADPS4_HLE_COND_WAIT),
        HLE_MAP("27bAgiJmOh0", SHADPS4_HLE_COND_WAIT),
        HLE_MAP("Op8TBGY5KHg", SHADPS4_HLE_COND_WAIT),
        HLE_MAP("BmMjYxmew1w", SHADPS4_HLE_COND_WAIT),
        HLE_MAP("WKAXJ4XBPQ4", SHADPS4_HLE_COND_WAIT),
        HLE_MAP("2MOy+rUfuhQ", SHADPS4_HLE_COND_SIGNAL),
        HLE_MAP("CI6Qy73ae10", SHADPS4_HLE_COND_SIGNAL),
        HLE_MAP("mkx2fVhNMsg", SHADPS4_HLE_COND_SIGNAL),
        HLE_MAP("kDh-NfxgMtE", SHADPS4_HLE_COND_SIGNAL),
        HLE_MAP("o69RpYO-Mu0", SHADPS4_HLE_COND_SIGNAL),
        HLE_MAP("JGgj7Uvrl+A", SHADPS4_HLE_COND_SIGNAL),

        HLE_MAP("xFebsA4YsFI", SHADPS4_HLE_RWLOCK_ATTR_INIT),
        HLE_MAP("yOfGg-I1ZII", SHADPS4_HLE_RWLOCK_ATTR_INIT),
        HLE_MAP("qsdmgXjqSgk", SHADPS4_HLE_RWLOCK_ATTR_DESTROY),
        HLE_MAP("i2ifZ3fS2fo", SHADPS4_HLE_RWLOCK_ATTR_DESTROY),
        HLE_MAP("VqEMuCv-qHY", SHADPS4_HLE_RWLOCK_ATTR_GET),
        HLE_MAP("l+bG5fsYkhg", SHADPS4_HLE_RWLOCK_ATTR_GET),
        HLE_MAP("LcOZBHGqbFk", SHADPS4_HLE_RWLOCK_ATTR_GET),
        HLE_MAP("Kyls1ChFyrc", SHADPS4_HLE_RWLOCK_ATTR_GET),
        HLE_MAP("OuKg+kRDD7U", SHADPS4_HLE_RWLOCK_ATTR_SET),
        HLE_MAP("8NuOHiTr1Vw", SHADPS4_HLE_RWLOCK_ATTR_SET),
        HLE_MAP("-ZvQH18j10c", SHADPS4_HLE_RWLOCK_ATTR_SET),
        HLE_MAP("h-OifiouBd8", SHADPS4_HLE_RWLOCK_ATTR_SET),
        HLE_MAP("ytQULN-nhL4", SHADPS4_HLE_RWLOCK_INIT),
        HLE_MAP("6ULAa0fq4jA", SHADPS4_HLE_RWLOCK_INIT),
        HLE_MAP("1471ajPzxh0", SHADPS4_HLE_RWLOCK_DESTROY),
        HLE_MAP("BB+kb08Tl9A", SHADPS4_HLE_RWLOCK_DESTROY),
        HLE_MAP("iGjsr1WAtI0", SHADPS4_HLE_RWLOCK_RDLOCK),
        HLE_MAP("lb8lnYo-o7k", SHADPS4_HLE_RWLOCK_RDLOCK),
        HLE_MAP("Ox9i0c7L5w0", SHADPS4_HLE_RWLOCK_RDLOCK),
        HLE_MAP("iPtZRWICjrM", SHADPS4_HLE_RWLOCK_RDLOCK),
        HLE_MAP("SFxTMOfuCkE", SHADPS4_HLE_RWLOCK_TRYRDLOCK),
        HLE_MAP("XD3mDeybCnk", SHADPS4_HLE_RWLOCK_TRYRDLOCK),
        HLE_MAP("sIlRvQqsN2Y", SHADPS4_HLE_RWLOCK_WRLOCK),
        HLE_MAP("9zklzAl9CGM", SHADPS4_HLE_RWLOCK_WRLOCK),
        HLE_MAP("mqdNorrB+gI", SHADPS4_HLE_RWLOCK_WRLOCK),
        HLE_MAP("adh--6nIqTk", SHADPS4_HLE_RWLOCK_WRLOCK),
        HLE_MAP("XhWHn6P5R7U", SHADPS4_HLE_RWLOCK_TRYWRLOCK),
        HLE_MAP("bIHoZCTomsI", SHADPS4_HLE_RWLOCK_TRYWRLOCK),
        HLE_MAP("EgmLo6EWgso", SHADPS4_HLE_RWLOCK_UNLOCK),
        HLE_MAP("+L98PIbGttk", SHADPS4_HLE_RWLOCK_UNLOCK),

        HLE_MAP("pDuPEf3m4fI", SHADPS4_HLE_SEM_INIT),
        HLE_MAP("GEnUkDZoUwY", SHADPS4_HLE_SEM_INIT),
        HLE_MAP("cDW233RAwWo", SHADPS4_HLE_SEM_DESTROY),
        HLE_MAP("Vwc+L05e6oE", SHADPS4_HLE_SEM_DESTROY),
        HLE_MAP("YCV5dGGBcCo", SHADPS4_HLE_SEM_WAIT),
        HLE_MAP("w5IHyvahg-o", SHADPS4_HLE_SEM_WAIT),
        HLE_MAP("C36iRE0F5sE", SHADPS4_HLE_SEM_WAIT),
        HLE_MAP("fjN6NQHhK8k", SHADPS4_HLE_SEM_WAIT),
        HLE_MAP("WBWzsRifCEA", SHADPS4_HLE_SEM_TRYWAIT),
        HLE_MAP("H2a+IN9TP0E", SHADPS4_HLE_SEM_TRYWAIT),
        HLE_MAP("IKP8typ0QUk", SHADPS4_HLE_SEM_POST),
        HLE_MAP("aishVAiFaYM", SHADPS4_HLE_SEM_POST),
        HLE_MAP("Bq+LRV-N6Hk", SHADPS4_HLE_SEM_GETVALUE),
        HLE_MAP("DjpBvGlaWbQ", SHADPS4_HLE_SEM_GETVALUE),
        HLE_MAP("188x57JYp0g", SHADPS4_HLE_KERNEL_SEMA_CREATE),
        HLE_MAP("R1Jvn8bSCW8", SHADPS4_HLE_KERNEL_SEMA_DELETE),
        HLE_MAP("Zxa0VhQVTsk", SHADPS4_HLE_KERNEL_SEMA_WAIT),
        HLE_MAP("12wOHk8ywb0", SHADPS4_HLE_KERNEL_SEMA_POLL),
        HLE_MAP("4czppHBiriw", SHADPS4_HLE_KERNEL_SEMA_SIGNAL),
        HLE_MAP("4DM06U2BNEY", SHADPS4_HLE_KERNEL_SEMA_CANCEL),

        HLE_MAP("mqULNdimTn0", SHADPS4_HLE_PTHREAD_KEY_CREATE),
        HLE_MAP("geDaqgH9lTg", SHADPS4_HLE_PTHREAD_KEY_CREATE),
        HLE_MAP("6BpEZuDT7YI", SHADPS4_HLE_PTHREAD_KEY_DELETE),
        HLE_MAP("PrdHuuDekhY", SHADPS4_HLE_PTHREAD_KEY_DELETE),
        HLE_MAP("0-KXaS70xy4", SHADPS4_HLE_PTHREAD_GETSPECIFIC),
        HLE_MAP("eoht7mQOCmo", SHADPS4_HLE_PTHREAD_GETSPECIFIC),
        HLE_MAP("WrOLvHU0yQM", SHADPS4_HLE_PTHREAD_SETSPECIFIC),
        HLE_MAP("+BzXYkqYeLE", SHADPS4_HLE_PTHREAD_SETSPECIFIC),

        HLE_MAP("+WRlkKjZvag", SHADPS4_HLE_FS_READV),
        HLE_MAP("YSHRBRLn2pI", SHADPS4_HLE_FS_WRITEV),
        HLE_MAP("kAt6VDbHmro", SHADPS4_HLE_FS_WRITEV),
        HLE_MAP("ezv-RSBNKqI", SHADPS4_HLE_FS_PREAD),
        HLE_MAP("+r3rMFwItV4", SHADPS4_HLE_FS_PREAD),
        HLE_MAP("C2kJ-byS5rM", SHADPS4_HLE_FS_PWRITE),
        HLE_MAP("nKWi-N2HBV4", SHADPS4_HLE_FS_PWRITE),
        HLE_MAP("yTj62I7kw4s", SHADPS4_HLE_FS_PREADV),
        HLE_MAP("FCcmRZhWtOk", SHADPS4_HLE_FS_PWRITEV),
        HLE_MAP("mBd4AfLP+u8", SHADPS4_HLE_FS_PWRITEV),
        HLE_MAP("c7ZnT7V1B98", SHADPS4_HLE_FS_RMDIR),
        HLE_MAP("naInUjYt3so", SHADPS4_HLE_FS_RMDIR),
        HLE_MAP("ih4CD9-gghM", SHADPS4_HLE_FS_FTRUNCATE),
        HLE_MAP("VW3TVZiM4-E", SHADPS4_HLE_FS_FTRUNCATE),
        HLE_MAP("uWyW3v98sU4", SHADPS4_HLE_FS_CHECK_REACHABILITY),
        HLE_MAP("T8fER+tIGgk", SHADPS4_HLE_FS_SELECT),

        HLE_MAP("NhpspxdjEKU", SHADPS4_HLE_TIME_NANOSLEEP),
        HLE_MAP("yS8U2TGCe1A", SHADPS4_HLE_TIME_NANOSLEEP),
        HLE_MAP("QvsZxomvUHs", SHADPS4_HLE_TIME_NANOSLEEP),
        HLE_MAP("QcteRwbsnV0", SHADPS4_HLE_TIME_USLEEP),
        HLE_MAP("1jfXLRVzisc", SHADPS4_HLE_TIME_USLEEP),
        HLE_MAP("0wu33hunNdE", SHADPS4_HLE_TIME_SLEEP),
        HLE_MAP("-ZR+hG7aDHw", SHADPS4_HLE_TIME_SLEEP),
        HLE_MAP("smIj7eqzZE8", SHADPS4_HLE_TIME_CLOCK_GETRES),
        HLE_MAP("wRYVA5Zolso", SHADPS4_HLE_TIME_CLOCK_GETRES),
        HLE_MAP("-2IRUCO--PM", SHADPS4_HLE_TIME_READ_TSC),
        HLE_MAP("kOcnerypnQA", SHADPS4_HLE_TIME_GET_TIMEZONE),
        HLE_MAP("0NTHN1NKONI", SHADPS4_HLE_TIME_LOCAL_TO_UTC),
        HLE_MAP("-o5uEDpN+oY", SHADPS4_HLE_TIME_UTC_TO_LOCAL),

        HLE_MAP("xeu-pV8wkKs", SHADPS4_HLE_PROCESS_IS_SANDBOXED),
        HLE_MAP("0vTn5IDMU9A", SHADPS4_HLE_PROCESS_GET_SOC_ID),
        HLE_MAP("VOx8NGmHXTs", SHADPS4_HLE_PROCESS_GET_CPU_MODE),
        HLE_MAP("g0VTBxfJyu0", SHADPS4_HLE_PROCESS_GET_CURRENT_CPU),

        HLE_MAP("usHTMoFoBTM", SHADPS4_HLE_MEMORY_ENABLE_ALIASING),
        HLE_MAP("rTXw65xmLIA", SHADPS4_HLE_MEMORY_ALLOCATE_DIRECT),
        HLE_MAP("B+vc2AO2Zrc", SHADPS4_HLE_MEMORY_ALLOCATE_MAIN_DIRECT),
        HLE_MAP("C0f7TJcbfac", SHADPS4_HLE_MEMORY_AVAILABLE_DIRECT),
        HLE_MAP("hwVSPCmp5tM", SHADPS4_HLE_MEMORY_RELEASE_DIRECT),
        HLE_MAP("MBuItvba6z8", SHADPS4_HLE_MEMORY_RELEASE_DIRECT),
        HLE_MAP("pO96TwzOm5E", SHADPS4_HLE_MEMORY_GET_DIRECT_SIZE),
        HLE_MAP("BC+OG5m9+bw", SHADPS4_HLE_MEMORY_DIRECT_QUERY),
        HLE_MAP("BHouLQzh0X0", SHADPS4_HLE_MEMORY_DIRECT_QUERY),
        HLE_MAP("rVjRvHJ0X6c", SHADPS4_HLE_MEMORY_VIRTUAL_QUERY),
        HLE_MAP("7oxv3PPCumo", SHADPS4_HLE_MEMORY_RESERVE_VIRTUAL),
        HLE_MAP("NcaWUxfMNIQ", SHADPS4_HLE_MEMORY_MAP_DIRECT),
        HLE_MAP("L-Q3LEjIbgA", SHADPS4_HLE_MEMORY_MAP_DIRECT),
        HLE_MAP("BQQniolj9tQ", SHADPS4_HLE_MEMORY_MAP_DIRECT2),
        HLE_MAP("PGhQHd-dzv8", SHADPS4_HLE_MEMORY_MMAP),
        HLE_MAP("mL8NDH86iQI", SHADPS4_HLE_MEMORY_MAP_FLEXIBLE),
        HLE_MAP("kc+LEEIYakc", SHADPS4_HLE_MEMORY_MAP_FLEXIBLE),
        HLE_MAP("IWIBBdTHit4", SHADPS4_HLE_MEMORY_MAP_FLEXIBLE),
        HLE_MAP("aNz11fnnzi4", SHADPS4_HLE_MEMORY_AVAILABLE_FLEXIBLE),
        HLE_MAP("n1-v6FgU7MQ", SHADPS4_HLE_MEMORY_CONFIGURED_FLEXIBLE),
        HLE_MAP("WFcfL2lzido", SHADPS4_HLE_MEMORY_QUERY_PROTECTION),
        HLE_MAP("vSMAm3cxYTY", SHADPS4_HLE_MEMORY_PROTECT),
        HLE_MAP("YQOfxL4QfeU", SHADPS4_HLE_MEMORY_PROTECT),
        HLE_MAP("9bfdLIyuwCY", SHADPS4_HLE_MEMORY_PROTECT),
        HLE_MAP("yDBwVAolDgg", SHADPS4_HLE_MEMORY_IS_STACK),
        HLE_MAP("jh+8XiK4LeE", SHADPS4_HLE_MEMORY_IS_ASAN),
        HLE_MAP("2SKEx6bSq-4", SHADPS4_HLE_MEMORY_BATCH_MAP),
        HLE_MAP("kBJzF8x4SyE", SHADPS4_HLE_MEMORY_BATCH_MAP),
        HLE_MAP("DGMG3JshrZU", SHADPS4_HLE_MEMORY_SET_NAME),
        HLE_MAP("qCSfqDILlns", SHADPS4_HLE_MEMORY_POOL),
        HLE_MAP("pU-QydtGcGY", SHADPS4_HLE_MEMORY_POOL),
        HLE_MAP("Vzl66WmfLvk", SHADPS4_HLE_MEMORY_POOL),
        HLE_MAP("LXo1tpFqJGs", SHADPS4_HLE_MEMORY_POOL),
        HLE_MAP("YN878uKRBbE", SHADPS4_HLE_MEMORY_POOL),
        HLE_MAP("bvD+95Q6asU", SHADPS4_HLE_MEMORY_POOL),
        HLE_MAP("3k6kx-zOOSQ", SHADPS4_HLE_MEMORY_MLOCK),
        HLE_MAP("tZY4+SZNFhA", SHADPS4_HLE_MEMORY_MSYNC),
        HLE_MAP("BohYr-F7-is", SHADPS4_HLE_MEMORY_SET_PRT),
        HLE_MAP("L0v2Go5jOuM", SHADPS4_HLE_MEMORY_GET_PRT),
        HLE_MAP("p5EcQeEeJAE", SHADPS4_HLE_MEMORY_SET_HEAP_API),

        HLE_MAP("Z4QosVuAsA0", SHADPS4_HLE_PTHREAD_ONCE),
        HLE_MAP("EotR8a3ASf4", SHADPS4_HLE_PTHREAD_SELF),
        HLE_MAP("aI+OeCz8xrQ", SHADPS4_HLE_PTHREAD_SELF),
        HLE_MAP("OxhIB8LB-PQ", SHADPS4_HLE_PTHREAD_CREATE),
        HLE_MAP("Jmi+9w9u0E4", SHADPS4_HLE_PTHREAD_CREATE),
        HLE_MAP("+U1R4WtXvoc", SHADPS4_HLE_PTHREAD_DETACH),
        HLE_MAP("7Xl257M4VNI", SHADPS4_HLE_PTHREAD_EQUAL),
        HLE_MAP("3PtV6p3QNX4", SHADPS4_HLE_PTHREAD_EQUAL),
        HLE_MAP("h9CcP3J0oVM", SHADPS4_HLE_PTHREAD_JOIN),
        HLE_MAP("lZzFeSxPl08", SHADPS4_HLE_PTHREAD_CANCEL_STATE),
        HLE_MAP("CBNtXOoef-E", SHADPS4_HLE_PTHREAD_SCHED_PRIORITY),
        HLE_MAP("m0iS6jNsXds", SHADPS4_HLE_PTHREAD_SCHED_PRIORITY),
        HLE_MAP("Xs9hdiD7sAA", SHADPS4_HLE_PTHREAD_SET_SCHEDPARAM),
        HLE_MAP("Jb2uGFMr688", SHADPS4_HLE_PTHREAD_GET_AFFINITY),
        HLE_MAP("5KWrg7-ZqvE", SHADPS4_HLE_PTHREAD_SET_AFFINITY),
        HLE_MAP("3eqs37G74-s", SHADPS4_HLE_PTHREAD_GET_TID),
        HLE_MAP("EI-5-jlq2dE", SHADPS4_HLE_PTHREAD_GET_TID),
        HLE_MAP("3kg7rT0NQIs", SHADPS4_HLE_PTHREAD_EXIT),
        HLE_MAP("oxMp8uPqa+U", SHADPS4_HLE_PTHREAD_SET_NAME),
        HLE_MAP("T72hz6ffq08", SHADPS4_HLE_PTHREAD_YIELD),
        HLE_MAP("1tKyG7RlMJo", SHADPS4_HLE_PTHREAD_GET_PRIORITY),
        HLE_MAP("rNhWz+lvOMU", SHADPS4_HLE_PTHREAD_SET_DTORS),
        HLE_MAP("1xvtUVx1-Sg", SHADPS4_HLE_PTHREAD_CLEANUP),
        HLE_MAP("iWsFlYMf3Kw", SHADPS4_HLE_PTHREAD_CLEANUP),

        HLE_MAP("BpFoboUJoZU", SHADPS4_HLE_EVENT_FLAG_CREATE),
        HLE_MAP("8mql9OcQnd4", SHADPS4_HLE_EVENT_FLAG_DELETE),
        HLE_MAP("1vDaenmJtyA", SHADPS4_HLE_EVENT_FLAG_OPEN_CLOSE),
        HLE_MAP("s9-RaxukuzQ", SHADPS4_HLE_EVENT_FLAG_OPEN_CLOSE),
        HLE_MAP("IOnSvHzqu6A", SHADPS4_HLE_EVENT_FLAG_SET),
        HLE_MAP("7uhBFWRAS60", SHADPS4_HLE_EVENT_FLAG_CLEAR),
        HLE_MAP("JTvBflhYazQ", SHADPS4_HLE_EVENT_FLAG_WAIT),
        HLE_MAP("9lvj5DjHZiA", SHADPS4_HLE_EVENT_FLAG_POLL),
        HLE_MAP("PZku4ZrXJqg", SHADPS4_HLE_EVENT_FLAG_CANCEL),

        HLE_MAP("vYU8P9Td2Zo", SHADPS4_HLE_AIO_INIT),
        HLE_MAP("nu4a0-arQis", SHADPS4_HLE_AIO_OPTION),
        HLE_MAP("9WK-vhNXimw", SHADPS4_HLE_AIO_OPTION),
        HLE_MAP("fR521KIGgb8", SHADPS4_HLE_AIO_CANCEL_ONE),
        HLE_MAP("3Lca1XBrQdY", SHADPS4_HLE_AIO_CANCEL_MANY),
        HLE_MAP("5TgME6AYty4", SHADPS4_HLE_AIO_DELETE_ONE),
        HLE_MAP("Ft3EtsZzAoY", SHADPS4_HLE_AIO_DELETE_MANY),
        HLE_MAP("2pOuoWoCxdk", SHADPS4_HLE_AIO_POLL_ONE),
        HLE_MAP("o7O4z3jwKzo", SHADPS4_HLE_AIO_POLL_MANY),
        HLE_MAP("KOF-oJbQVvc", SHADPS4_HLE_AIO_WAIT_ONE),
        HLE_MAP("lgK+oIWkJyA", SHADPS4_HLE_AIO_WAIT_MANY),
        HLE_MAP("HgX7+AORI58", SHADPS4_HLE_AIO_SUBMIT_READ),
        HLE_MAP("lXT0m3P-vs4", SHADPS4_HLE_AIO_SUBMIT_READ_MANY),
        HLE_MAP("XQ8C8y+de+E", SHADPS4_HLE_AIO_SUBMIT_WRITE),
        HLE_MAP("xT3Cpz0yh6Y", SHADPS4_HLE_AIO_SUBMIT_WRITE_MANY),

        HLE_MAP("14bOACANTBo", SHADPS4_HLE_PTHREAD_ONCE),
        HLE_MAP("GBUY7ywdULE", SHADPS4_HLE_PTHREAD_SET_NAME),
        HLE_MAP("6UgtwV+0zb4", SHADPS4_HLE_PTHREAD_CREATE),
        HLE_MAP("4qGrR6eoP9Y", SHADPS4_HLE_PTHREAD_DETACH),
        HLE_MAP("onNY9Byn-W8", SHADPS4_HLE_PTHREAD_JOIN),
        HLE_MAP("P41kTWUS3EI", SHADPS4_HLE_PTHREAD_GET_SCHEDPARAM),
        HLE_MAP("oIRFTjoILbg", SHADPS4_HLE_PTHREAD_SET_SCHEDPARAM),
        HLE_MAP("How7B8Oet6k", SHADPS4_HLE_PTHREAD_GET_NAME),
        HLE_MAP("W0Hpm2X0uPE", SHADPS4_HLE_PTHREAD_SET_PRIORITY),
        HLE_MAP("rcrVFJsQWRY", SHADPS4_HLE_PTHREAD_GET_AFFINITY_MASK),
        HLE_MAP("bt3CTBKmGyI", SHADPS4_HLE_PTHREAD_SET_AFFINITY_MASK),

        HLE_MAP("WkwEd3N7w0Y", SHADPS4_HLE_SIGNAL_INSTALL_HANDLER),
        HLE_MAP("Qhv5ARAoOEc", SHADPS4_HLE_SIGNAL_REMOVE_HANDLER),
        HLE_MAP("KiJEPEWRyUY", SHADPS4_HLE_SIGNAL_ACTION),
        HLE_MAP("VADc3MNQ3cM", SHADPS4_HLE_SIGNAL_SET_HANDLER),
        HLE_MAP("+F7C-hdk7+E", SHADPS4_HLE_SIGNAL_EMPTY_SET),
        HLE_MAP("VkTAsrZDcJ0", SHADPS4_HLE_SIGNAL_FILL_SET),
        HLE_MAP("JUimFtKe0Kc", SHADPS4_HLE_SIGNAL_ADD_SET),
        HLE_MAP("Nd-u09VFSCA", SHADPS4_HLE_SIGNAL_DELETE_SET),
        HLE_MAP("JnNl8Xr-z4Y", SHADPS4_HLE_SIGNAL_IS_MEMBER),
        HLE_MAP("aPcyptbOiZs", SHADPS4_HLE_SIGNAL_MASK),
        HLE_MAP("yH-uQW3LbX0", SHADPS4_HLE_SIGNAL_THREAD_KILL),
        HLE_MAP("sHziAegVp74", SHADPS4_HLE_SIGNAL_ALT_STACK),
        HLE_MAP("OMDRKKAZ8I4", SHADPS4_HLE_SIGNAL_RAISE),
        HLE_MAP("zE-wXIZjLoM", SHADPS4_HLE_SIGNAL_RAISE),

        HLE_MAP("9JYNqN6jAKI", SHADPS4_HLE_KERNEL_DEBUG_TEXT),
        HLE_MAP("YeU23Szo3BM", SHADPS4_HLE_KERNEL_GET_ALLOWED_SDK),
        HLE_MAP("Mv1zUObHvXI", SHADPS4_HLE_KERNEL_GET_SW_VERSION),
        HLE_MAP("igMefp4SAv0", SHADPS4_HLE_KERNEL_GET_AUTHINFO),
        HLE_MAP("G-MYv5erXaU", SHADPS4_HLE_KERNEL_GET_APP_INFO),
        HLE_MAP("1yca4VvfcNA", SHADPS4_HLE_KERNEL_TITLE_WORKAROUND),
        HLE_MAP("+g+UP8Pyfmo", SHADPS4_HLE_KERNEL_GET_PROCESS_TYPE),
        HLE_MAP("PfccT7qURYE", SHADPS4_HLE_KERNEL_IOCTL),
        HLE_MAP("wW+k21cmbwQ", SHADPS4_HLE_KERNEL_IOCTL),
        HLE_MAP("JGfTMBOdUJo", SHADPS4_HLE_KERNEL_SANDBOX_WORD),
        HLE_MAP("Xjoosiw+XPI", SHADPS4_HLE_KERNEL_UUID_CREATE),
        HLE_MAP("7NwggrWJ5cA", SHADPS4_HLE_KERNEL_REGMGR),
        HLE_MAP("mkawd0NA9ts", SHADPS4_HLE_KERNEL_SYSCONF),
        HLE_MAP("4oXYe9Xmk0Q", SHADPS4_HLE_KERNEL_GPI),
        HLE_MAP("ca7v6Cxulzs", SHADPS4_HLE_KERNEL_GPI),
        HLE_MAP("pG70GT5yRo4", 97),
        HLE_MAP("K1S8oc61xiM", SHADPS4_HLE_KERNEL_HTONL),
        HLE_MAP("jogUIsOV3-U", SHADPS4_HLE_NET_HTONS),
        HLE_MAP("fZOeZIOEmLw", SHADPS4_HLE_NET_SEND),
        HLE_MAP("Ez8xjo9UF4E", SHADPS4_HLE_NET_RECV),
        HLE_MAP("KuOmgKoqCdY", SHADPS4_HLE_NET_BIND),
        HLE_MAP("6O8EwYOgH9Y", SHADPS4_HLE_NET_GETSOCKOPT),
        HLE_MAP("fFxGkxF2bVo", SHADPS4_HLE_NET_SETSOCKOPT),
        HLE_MAP("pxnCmagrtao", SHADPS4_HLE_NET_LISTEN),
        HLE_MAP("3e+4Iv7IJ8U", SHADPS4_HLE_NET_ACCEPT),
        HLE_MAP("TUuiYS2kE8s", SHADPS4_HLE_NET_SHUTDOWN),
        HLE_MAP("MZb0GKT3mo8", SHADPS4_HLE_NET_SOCKET_PAIR),
        HLE_MAP("oBr313PppNE", SHADPS4_HLE_NET_SENDTO),
        HLE_MAP("lUk6wrGXyMw", SHADPS4_HLE_NET_RECVFROM),

        HLE_MAP("wzvqT4UqKX8", SHADPS4_HLE_KERNEL_MODULE_LOAD),
        HLE_MAP("LwG8g3niqwA", SHADPS4_HLE_KERNEL_DLSYM),
        HLE_MAP("RpQJJVKTiFM", SHADPS4_HLE_KERNEL_MODULE_INFO_UNWIND),
        HLE_MAP("f7KBOafysXo", SHADPS4_HLE_KERNEL_MODULE_INFO_FROM_ADDR),
        HLE_MAP("kUpgrXIrz7Q", SHADPS4_HLE_KERNEL_MODULE_INFO),
        HLE_MAP("QgsKEUfkqMA", SHADPS4_HLE_KERNEL_MODULE_INFO2),
        HLE_MAP("HZO7xOos4xc", SHADPS4_HLE_KERNEL_MODULE_INFO_INTERNAL),
        HLE_MAP("IuxnUuXk6Bg", SHADPS4_HLE_KERNEL_MODULE_LIST),
        HLE_MAP("ZzzC3ZGVAkc", SHADPS4_HLE_KERNEL_MODULE_LIST2),
        HLE_MAP("ChCOChPU-YM", SHADPS4_HLE_KERNEL_SET_TIME),
        HLE_MAP("j2AIqSqJP0w", SHADPS4_HLE_FS_GETDENTS),
        HLE_MAP("qBDmpCyGssE", SHADPS4_HLE_PTHREAD_CANCEL_STATE),
        HLE_MAP("WlyEA-sLDf0", SHADPS4_HLE_FS_TRUNCATE),
        HLE_MAP("kbw4UHHSYy0", SHADPS4_HLE_SUCCESS),
        HLE_MAP("pB-yGZ2nQ9o", SHADPS4_HLE_SUCCESS),
        HLE_MAP("WhCc1w3EhSI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("py6L8jiVAN8", SHADPS4_HLE_MEMORY_SET_HEAP_API),
        HLE_MAP("-YTW+qXc3CQ", SHADPS4_HLE_KERNEL_MODULE_INFO_INTERNAL),
        HLE_MAP("Wl2o5hOVZdw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("bnZxYgAFeA0", SHADPS4_HLE_MEMORY_SET_HEAP_API),
        HLE_MAP("Fjc4-n1+y2g", SHADPS4_HLE_SUCCESS),
        HLE_MAP("hHlZQUnlxSM", SHADPS4_HLE_KERNEL_GETRUSAGE),
        HLE_MAP("8OnWXlgQlvo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("Tz4RNUCBbGI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("crb5j7mkk1c", SHADPS4_HLE_SUCCESS),
        HLE_MAP("mo0bFmWppIw", SHADPS4_HLE_SUCCESS),
        HLE_MAP("QKd0qM58Qes", SHADPS4_HLE_SUCCESS),
        HLE_MAP("DFmMT80xcNI", SHADPS4_HLE_SUCCESS),
        HLE_MAP("OjWstbIRPUo", SHADPS4_HLE_SUCCESS),
        HLE_MAP("nh2IFMgKTv8", 362),
        HLE_MAP("RW-GEfpnsqg", 363),
        HLE_MAP("hI7oVeOluPM", SHADPS4_HLE_NET_RECVMSG),
        HLE_MAP("TXFFFiNldU8", SHADPS4_HLE_NET_GETPEERNAME),
        HLE_MAP("RenI1lL1WFk", SHADPS4_HLE_NET_GETSOCKNAME),
        HLE_MAP("5jRCs2axtr4", SHADPS4_HLE_NET_INET_NTOP),
        HLE_MAP("4n51s0zEf0c", SHADPS4_HLE_NET_INET_PTON),
        HLE_MAP("aNeavPDNKzA", SHADPS4_HLE_NET_SENDMSG),
        HLE_MAP("VdXIDAbJ3tQ", SHADPS4_HLE_KERNEL_SET_TIME),
        HLE_MAP("d7nUj1LOdDU", SHADPS4_HLE_KERNEL_SET_TIME),
        HLE_MAP("8zLSfEfW5AU", SHADPS4_HLE_COREDUMP_REGISTER),
        HLE_MAP("fFkhOgztiCA", SHADPS4_HLE_COREDUMP_UNREGISTER),
        HLE_MAP("il03nluKfMk", SHADPS4_HLE_SIGNAL_THREAD_KILL),
        HLE_MAP("B5GmVDKwpn0", SHADPS4_HLE_PTHREAD_YIELD),
        HLE_MAP("FJrT5LuUBAU", SHADPS4_HLE_PTHREAD_EXIT),
        HLE_MAP("a2P9wYGeZvc", SHADPS4_HLE_PTHREAD_SET_PRIORITY),
        HLE_MAP("9vyP6Z7bqzc", SHADPS4_HLE_PTHREAD_SET_NAME),
        HLE_MAP("FIs3-UQT9sg", SHADPS4_HLE_PTHREAD_GET_SCHEDPARAM),
        HLE_MAP("4ZeZWcMsAV0", SHADPS4_HLE_PTHREAD_CLEANUP),
        HLE_MAP("RVxb0Ssa5t0", SHADPS4_HLE_PTHREAD_CLEANUP),
    };

    (void)library;

    {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            ps2_classics, ARRAY_SIZE(ps2_classics), nid,
            HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }

    for (static_index = 0;
         static_index < ARRAY_SIZE(shadps4_hle_exports); static_index++) {
        const ShadPS4HLEExport *export = &shadps4_hle_exports[static_index];

        if (export->data_kind == SHADPS4_HLE_DATA_NONE &&
            !strcmp(export->nid, nid) &&
            !g_strcmp0(export->module, module)) {
            return export->dispatch;
        }
    }

    /* Exact upstream success stubs are safe only when no concrete export won. */
    if (shadps4_hle_is_trivial_provider(module, nid)) {
        return SHADPS4_HLE_SUCCESS;
    }
    {
        uint32_t dispatch = shadps4_hle_reviewed_provider_dispatch(
            module, nid);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }

    {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            residual, ARRAY_SIZE(residual), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceGnmDriver")) {
        return shadps4_gnm_compat_dispatch(nid);
    }
    if (!g_strcmp0(module, "libSceLibcInternal") ||
        !g_strcmp0(module, "libc")) {
        return shadps4_hle_map_dispatch(libc, ARRAY_SIZE(libc), nid,
                                        SHADPS4_HLE_LIBC_UNSUPPORTED);
    }
    /* These firmware modules are optional in embedded deployments. */
    if (!g_strcmp0(module, "libSceUlt") ||
        !g_strcmp0(module, "libSceFios2") ||
        !g_strcmp0(module, "libSceAudiodec")) {
        return SHADPS4_HLE_SUCCESS;
    }
    if (!g_strcmp0(module, "libSceAudioOut")) {
        return shadps4_hle_map_dispatch(audio_out, ARRAY_SIZE(audio_out),
                                        nid, HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceMove")) {
        return shadps4_hle_map_dispatch(move, ARRAY_SIZE(move), nid,
                                        HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceZlib")) {
        return shadps4_hle_map_dispatch(zlib_hle, ARRAY_SIZE(zlib_hle), nid,
                                        HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libScePngEnc")) {
        return shadps4_hle_map_dispatch(png_enc, ARRAY_SIZE(png_enc), nid,
                                        HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceDiscMap")) {
        return shadps4_hle_map_dispatch(disc_map, ARRAY_SIZE(disc_map), nid,
                                        HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceVideodec2")) {
        return shadps4_hle_map_dispatch(
            videodec2, ARRAY_SIZE(videodec2), nid, HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceNpCommerce")) {
        return shadps4_hle_map_dispatch(
            commerce, ARRAY_SIZE(commerce), nid, HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceSsl")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            ssl2, ARRAY_SIZE(ssl2), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
        if (!strcmp(nid, "hdpVEUDFW3s")) {
            return SHADPS4_HLE_SSL_INIT;
        }
        if (g_snprintf(ssl_nid_token, sizeof(ssl_nid_token), "|%s|", nid) > 0 &&
            strstr(ssl_stub_nids, ssl_nid_token)) {
            return SHADPS4_HLE_SUCCESS;
        }
        return SHADPS4_HLE_PROVIDER_UNSUPPORTED;
    }
    if (!g_strcmp0(module, "libSceHttp")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            http, ARRAY_SIZE(http), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
        if (g_snprintf(ssl_nid_token, sizeof(ssl_nid_token), "|%s|", nid) > 0 &&
            strstr(http_stub_nids, ssl_nid_token)) {
            return SHADPS4_HLE_SUCCESS;
        }
        return SHADPS4_HLE_PROVIDER_UNSUPPORTED;
    }
    if (!g_strcmp0(module, "libSceNpMatching2")) {
        return shadps4_hle_map_dispatch(
            np_matching2, ARRAY_SIZE(np_matching2), nid,
            HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceNpSignaling")) {
        return shadps4_hle_map_dispatch(
            np_signaling, ARRAY_SIZE(np_signaling), nid,
            HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceUsbd")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            usbd, ARRAY_SIZE(usbd), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceNpManager") ||
        !g_strcmp0(module, "libSceNpManagerCompat")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            np_manager, ARRAY_SIZE(np_manager), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceCamera")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            camera, ARRAY_SIZE(camera), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceHmd")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            hmd, ARRAY_SIZE(hmd), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceAudio3d")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            audio3d, ARRAY_SIZE(audio3d), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceNpScore") ||
        !g_strcmp0(module, "libSceNpScoreCompat")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            np_score, ARRAY_SIZE(np_score), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceNpTus") ||
        !g_strcmp0(module, "libSceNpTusCompat")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            np_tus, ARRAY_SIZE(np_tus), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceFiber")) {
        return shadps4_hle_map_dispatch(fiber, ARRAY_SIZE(fiber), nid,
                                        HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceFont") ||
        !g_strcmp0(module, "libSceFontFt")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            font_core, ARRAY_SIZE(font_core), nid, HLE_COMPAT_UNKNOWN);
        const char *font_nids = !g_strcmp0(module, "libSceFont") ?
                                font_stub_nids : font_ft_stub_nids;

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
        if (g_snprintf(ssl_nid_token, sizeof(ssl_nid_token), "|%s|", nid) > 0 &&
            strstr(font_nids, ssl_nid_token)) {
            return SHADPS4_HLE_SUCCESS;
        }
        return SHADPS4_HLE_FONT_UNSUPPORTED;
    }
    if (!g_strcmp0(module, "libScePlayGo")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            playgo, ARRAY_SIZE(playgo), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceNpAuth") ||
        !g_strcmp0(module, "libSceNpAuthCompat")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            np_auth, ARRAY_SIZE(np_auth), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
        if (!g_strcmp0(module, "libSceNpAuthCompat")) {
            return HLE_COMPAT_UNKNOWN;
        }
    }
    if (!g_strcmp0(module, "libSceIme")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            ime, ARRAY_SIZE(ime), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceImeDialog")) {
        return shadps4_hle_map_dispatch(
            ime_dialog, ARRAY_SIZE(ime_dialog), nid, HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceInvitationDialog") ||
        !g_strcmp0(module, "libSceInvitationDialogCompat")) {
        return shadps4_hle_map_dispatch(invitation_dialog,
                                        ARRAY_SIZE(invitation_dialog), nid,
                                        HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libScePlayGoDialog")) {
        return shadps4_hle_map_dispatch(
            playgo_dialog, ARRAY_SIZE(playgo_dialog), nid,
            HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceNpProfileDialog") ||
        !g_strcmp0(module, "libSceNpProfileDialogCompat")) {
        return shadps4_hle_map_dispatch(
            profile_dialog, ARRAY_SIZE(profile_dialog), nid,
            HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceVrTracker")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            vr_tracker, ARRAY_SIZE(vr_tracker), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceVrTrackerDeviceRejection") &&
        !strcmp(nid, "jGqEkPy0iLU")) {
        return SHADPS4_HLE_VR_TRACKER_REJECTION;
    }
    if (!g_strcmp0(module, "libSceVrTrackerFourDeviceAllowed") &&
        !strcmp(nid, "24kDA+A0Ox0")) {
        return SHADPS4_HLE_VR_TRACKER_REGISTER2;
    }
    if (!g_strcmp0(module, "libSceNpTrophy")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            np_trophy, ARRAY_SIZE(np_trophy), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceShellCoreUtil") ||
        !g_strcmp0(module, "libSceHmd") ||
        !g_strcmp0(module, "libSceNpTus")) {
        const char *stub_nids = !g_strcmp0(module, "libSceShellCoreUtil") ?
                                shell_core_stub_nids :
                                !g_strcmp0(module, "libSceHmd") ?
                                hmd_stub_nids : np_tus_stub_nids;

        if (g_snprintf(ssl_nid_token, sizeof(ssl_nid_token), "|%s|", nid) > 0 &&
            strstr(stub_nids, ssl_nid_token)) {
            return SHADPS4_HLE_SUCCESS;
        }
        return HLE_COMPAT_UNKNOWN;
    }
    if (!g_strcmp0(module, "libSceLncUtil") ||
        !g_strcmp0(module, "libSceNpTrophy")) {
        const char *stub_nids = !g_strcmp0(module, "libSceLncUtil") ?
                                lnc_util_stub_nids : np_trophy_stub_nids;

        if (g_snprintf(ssl_nid_token, sizeof(ssl_nid_token), "|%s|", nid) > 0 &&
            strstr(stub_nids, ssl_nid_token)) {
            return SHADPS4_HLE_SUCCESS;
        }
        return HLE_COMPAT_UNKNOWN;
    }
    if (!g_strcmp0(module, "libSceNpWebApi") ||
        !g_strcmp0(module, "libSceNpWebApiCompat")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            np_webapi, ARRAY_SIZE(np_webapi), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceNpWebApi") ||
        !g_strcmp0(module, "libSceGameLiveStreaming") ||
        !g_strcmp0(module, "libSceRemoteplay") ||
        !g_strcmp0(module, "libSceIme") ||
        !g_strcmp0(module, "libSceNpTusCompat") ||
        !g_strcmp0(module, "libSceRazorCpu") ||
        !g_strcmp0(module, "libSceRudp") ||
        !g_strcmp0(module, "libSceVoice")) {
        const char *stub_nids =
            !g_strcmp0(module, "libSceNpWebApi") ? np_webapi_stub_nids :
            !g_strcmp0(module, "libSceGameLiveStreaming") ? game_live_stub_nids :
            !g_strcmp0(module, "libSceRemoteplay") ? remoteplay_stub_nids :
            !g_strcmp0(module, "libSceIme") ? ime_stub_nids :
            !g_strcmp0(module, "libSceNpTusCompat") ? np_tus_compat_stub_nids :
            !g_strcmp0(module, "libSceRazorCpu") ? razor_cpu_stub_nids :
            !g_strcmp0(module, "libSceRudp") ? rudp_stub_nids : voice_stub_nids;

        if (g_snprintf(ssl_nid_token, sizeof(ssl_nid_token), "|%s|", nid) > 0 &&
            strstr(stub_nids, ssl_nid_token)) {
            return SHADPS4_HLE_SUCCESS;
        }
        return HLE_COMPAT_UNKNOWN;
    }
    if (!g_strcmp0(module, "libSceSystemStateMgr") ||
        !g_strcmp0(module, "libSceCamera") ||
        !g_strcmp0(module, "libSceNpParty") ||
        !g_strcmp0(module, "libSceAppContentUtil") ||
        !g_strcmp0(module, "libSceAppContent") ||
        !g_strcmp0(module, "libSceSharePlay") ||
        !g_strcmp0(module, "libSceSystemGesture") ||
        !g_strcmp0(module, "libSceVrTracker")) {
        uint32_t dispatch;
        const char *stub_nids =
            !g_strcmp0(module, "libSceSystemStateMgr") ? system_state_stub_nids :
            !g_strcmp0(module, "libSceCamera") ? camera_stub_nids :
            !g_strcmp0(module, "libSceNpParty") ? np_party_stub_nids :
            !g_strcmp0(module, "libSceAppContentUtil") ? app_content_stub_nids :
            !g_strcmp0(module, "libSceAppContent") ? app_content_base_stub_nids :
            !g_strcmp0(module, "libSceSharePlay") ? share_play_stub_nids :
            !g_strcmp0(module, "libSceSystemGesture") ? system_gesture_stub_nids :
            vr_tracker_stub_nids;

        if (!g_strcmp0(module, "libSceAppContentUtil")) {
            dispatch = shadps4_hle_map_dispatch(
                app_content, ARRAY_SIZE(app_content), nid,
                HLE_COMPAT_UNKNOWN);
            if (dispatch != HLE_COMPAT_UNKNOWN) {
                return dispatch;
            }
        }

        if (g_snprintf(ssl_nid_token, sizeof(ssl_nid_token), "|%s|", nid) > 0 &&
            strstr(stub_nids, ssl_nid_token)) {
            return SHADPS4_HLE_SUCCESS;
        }
        return HLE_COMPAT_UNKNOWN;
    }
    if (!g_strcmp0(module, "libSceNetCtlApIpcInt") ||
        !g_strcmp0(module, "libSceCompanionHttpd") ||
        !g_strcmp0(module, "libSceAudio3d") ||
        !g_strcmp0(module, "libSceScreenShot") ||
        !g_strcmp0(module, "libSceNpScore") ||
        !g_strcmp0(module, "libSceNetCtlAp") ||
        !g_strcmp0(module, "libSceMouse") ||
        !g_strcmp0(module, "libSceAppMessaging") ||
        !g_strcmp0(module, "libSceNetBwe")) {
        const char *stub_nids =
            !g_strcmp0(module, "libSceNetCtlApIpcInt") ? netctl_ap_ipc_stub_nids :
            !g_strcmp0(module, "libSceCompanionHttpd") ? companion_httpd_stub_nids :
            !g_strcmp0(module, "libSceAudio3d") ? audio3d_stub_nids :
            !g_strcmp0(module, "libSceScreenShot") ? screenshot_stub_nids :
            !g_strcmp0(module, "libSceNpScore") ? np_score_stub_nids :
            !g_strcmp0(module, "libSceNetCtlAp") ? netctl_ap_stub_nids :
            !g_strcmp0(module, "libSceMouse") ? mouse_stub_nids :
            !g_strcmp0(module, "libSceAppMessaging") ? app_messaging_stub_nids :
            net_bwe_stub_nids;

        if (g_snprintf(ssl_nid_token, sizeof(ssl_nid_token), "|%s|", nid) > 0 &&
            strstr(stub_nids, ssl_nid_token)) {
            return SHADPS4_HLE_SUCCESS;
        }
        return HLE_COMPAT_UNKNOWN;
    }
    if (!g_strcmp0(module, "libSceNpWebApi2")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            np_webapi2, ARRAY_SIZE(np_webapi2), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceVideoOut")) {
        uint32_t dispatch = shadps4_hle_map_dispatch(
            video_out, ARRAY_SIZE(video_out), nid, HLE_COMPAT_UNKNOWN);

        if (dispatch != HLE_COMPAT_UNKNOWN) {
            return dispatch;
        }
    }
    if (!g_strcmp0(module, "libSceWebBrowserDialog") ||
        !g_strcmp0(module, "libSceSigninDialog") ||
        !g_strcmp0(module, "libSceSystemServiceActivateHevc") ||
        !g_strcmp0(module, "libSceSystemServiceActivateHevcSoft") ||
        !g_strcmp0(module, "libSceHmdSetupDialog") ||
        !g_strcmp0(module, "libSceSystemServiceActivateMpeg2") ||
        !g_strcmp0(module, "libSceNpPartyCompat") ||
        !g_strcmp0(module, "libSceHmdDistortion") ||
        !g_strcmp0(module, "libSceNetCtlV6") ||
        !g_strcmp0(module, "libSceGnmDriverResourceRegistration") ||
        !g_strcmp0(module, "libSceVrTrackerGpuTest") ||
        !g_strcmp0(module, "libSceNpWebApi2") ||
        !g_strcmp0(module, "libSceCompanionUtil") ||
        !g_strcmp0(module, "libSceVrTrackerLiveCapture")) {
        const char *stub_nids =
            !g_strcmp0(module, "libSceWebBrowserDialog") ? web_browser_dialog_stub_nids :
            !g_strcmp0(module, "libSceSigninDialog") ? signin_dialog_stub_nids :
            !g_strcmp0(module, "libSceSystemServiceActivateHevc") ? activate_hevc_stub_nids :
            !g_strcmp0(module, "libSceSystemServiceActivateHevcSoft") ? activate_hevc_soft_stub_nids :
            !g_strcmp0(module, "libSceHmdSetupDialog") ? hmd_setup_dialog_stub_nids :
            !g_strcmp0(module, "libSceSystemServiceActivateMpeg2") ? activate_mpeg2_stub_nids :
            !g_strcmp0(module, "libSceNpPartyCompat") ? np_party_compat_stub_nids :
            !g_strcmp0(module, "libSceHmdDistortion") ? hmd_distortion_stub_nids :
            !g_strcmp0(module, "libSceNetCtlV6") ? netctl_v6_stub_nids :
            !g_strcmp0(module, "libSceGnmDriverResourceRegistration") ? gnm_resource_stub_nids :
            !g_strcmp0(module, "libSceVrTrackerGpuTest") ? vr_gpu_test_stub_nids :
            !g_strcmp0(module, "libSceNpWebApi2") ? np_webapi2_stub_nids :
            !g_strcmp0(module, "libSceCompanionUtil") ? companion_util_stub_nids :
            vr_live_capture_stub_nids;

        if (g_snprintf(ssl_nid_token, sizeof(ssl_nid_token), "|%s|", nid) > 0 &&
            strstr(stub_nids, ssl_nid_token)) {
            return SHADPS4_HLE_SUCCESS;
        }
        return HLE_COMPAT_UNKNOWN;
    }
    if (!g_strcmp0(module, "libSceCommonDialog") ||
        !g_strcmp0(module, "libSceNpManager") ||
        !g_strcmp0(module, "libSceUsbd") ||
        !g_strcmp0(module, "libSceContentExport") ||
        !g_strcmp0(module, "libSceNpPartner001") ||
        !g_strcmp0(module, "libSceVideodec") ||
        !g_strcmp0(module, "libSceErrorDialog") ||
        !g_strcmp0(module, "libScePlayGo") ||
        !g_strcmp0(module, "libScePngDec") ||
        !g_strcmp0(module, "libSceJpegEnc") ||
        !g_strcmp0(module, "libSceNpAuth") ||
        !g_strcmp0(module, "libSceNpProfileDialog") ||
        !g_strcmp0(module, "libSceNpSnsFacebookDialog") ||
        !g_strcmp0(module, "libSceVideoOut") ||
        !g_strcmp0(module, "libSceVideoRecording")) {
        const char *stub_nids =
            !g_strcmp0(module, "libSceCommonDialog") ? common_dialog_stub_nids :
            !g_strcmp0(module, "libSceNpManager") ? np_manager_stub_nids :
            !g_strcmp0(module, "libSceUsbd") ? usbd_stub_nids :
            !g_strcmp0(module, "libSceContentExport") ? content_export_stub_nids :
            !g_strcmp0(module, "libSceNpPartner001") ? np_partner_stub_nids :
            !g_strcmp0(module, "libSceVideodec") ? videodec_stub_nids :
            !g_strcmp0(module, "libSceErrorDialog") ? error_dialog_stub_nids :
            !g_strcmp0(module, "libScePlayGo") ? playgo_stub_nids :
            !g_strcmp0(module, "libScePngDec") ? pngdec_stub_nids :
            !g_strcmp0(module, "libSceJpegEnc") ? jpeg_enc_stub_nids :
            !g_strcmp0(module, "libSceNpAuth") ? np_auth_stub_nids :
            !g_strcmp0(module, "libSceNpProfileDialog") ? np_profile_dialog_stub_nids :
            !g_strcmp0(module, "libSceNpSnsFacebookDialog") ?
                np_sns_facebook_dialog_stub_nids :
            !g_strcmp0(module, "libSceVideoOut") ? video_out_stub_nids :
            video_recording_stub_nids;

        if (g_snprintf(ssl_nid_token, sizeof(ssl_nid_token), "|%s|", nid) > 0 &&
            strstr(stub_nids, ssl_nid_token)) {
            return SHADPS4_HLE_SUCCESS;
        }
        return HLE_COMPAT_UNKNOWN;
    }
    if (!g_strcmp0(module, "libSceNpCommon")) {
        return shadps4_hle_map_dispatch(
            np_common, ARRAY_SIZE(np_common), nid, HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceRtc")) {
        return shadps4_hle_map_dispatch(rtc, ARRAY_SIZE(rtc), nid,
                                        HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libScePad")) {
        return shadps4_hle_map_dispatch(pad, ARRAY_SIZE(pad), nid,
                                        HLE_COMPAT_UNKNOWN);
    }
    if (!g_strcmp0(module, "libSceSaveData")) {
        return shadps4_hle_map_dispatch(save_data, ARRAY_SIZE(save_data),
                                        nid, SHADPS4_HLE_PROVIDER_UNSUPPORTED);
    }
    if (!g_strcmp0(module, "libSceNet")) {
        return shadps4_hle_map_dispatch(net, ARRAY_SIZE(net), nid,
                                        SHADPS4_HLE_NET_UNSUPPORTED_SOCKET);
    }
    if (!g_strcmp0(module, "libSceNetCtl")) {
        return shadps4_hle_map_dispatch(netctl, ARRAY_SIZE(netctl), nid,
                                        SHADPS4_HLE_NETCTL_UNSUPPORTED);
    }
    if (!g_strcmp0(module, "libSceHttp2")) {
        return shadps4_hle_map_dispatch(http2, ARRAY_SIZE(http2), nid,
                                        SHADPS4_HLE_HTTP2_UNSUPPORTED);
    }
    if (!g_strcmp0(module, "libSceAjm")) {
        return shadps4_hle_map_dispatch(ajm, ARRAY_SIZE(ajm), nid,
                                        SHADPS4_HLE_AJM_UNSUPPORTED);
    }
    if (!g_strcmp0(module, "libSceNgs2")) {
        return shadps4_hle_map_dispatch(ngs2, ARRAY_SIZE(ngs2), nid,
                                        SHADPS4_HLE_NGS2_UNSUPPORTED);
    }
    if (!g_strcmp0(module, "libSceAvPlayer")) {
        return shadps4_hle_map_dispatch(avplayer, ARRAY_SIZE(avplayer), nid,
                                        SHADPS4_HLE_AVPLAYER_UNSUPPORTED);
    }
    if (!g_strcmp0(module, "libSceUserService")) {
        return shadps4_hle_map_dispatch(
            user_service, ARRAY_SIZE(user_service), nid,
            SHADPS4_HLE_USER_SERVICE_STUB);
    }
    if (!g_strcmp0(module, "libSceSystemService")) {
        return shadps4_hle_map_dispatch(
            system_service, ARRAY_SIZE(system_service), nid,
            SHADPS4_HLE_SYSTEM_SERVICE_STUB);
    }
    if (!g_strcmp0(module, "libSceSysmodule")) {
        return shadps4_hle_map_dispatch(
            sysmodule, ARRAY_SIZE(sysmodule), nid,
            SHADPS4_HLE_SYSMODULE_STUB);
    }
    if (!g_strcmp0(module, "libkernel")) {
        return shadps4_hle_map_dispatch(
            kernel_sync, ARRAY_SIZE(kernel_sync), nid,
            HLE_COMPAT_UNKNOWN);
    }
    if (shadps4_hle_requires_semantic_provider(module)) {
        return SHADPS4_HLE_PROVIDER_UNSUPPORTED;
    }
    return HLE_COMPAT_UNKNOWN;
}

#undef HLE_MAP

static bool shadps4_build_hle_image(ShadPS4MachineState *sms,
                                    uint64_t offset, Error **errp)
{
    ShadPS4ImageInfo *image = &sms->hle_image;
    MachineState *machine = MACHINE(sms);
    g_autoptr(GPtrArray) compat_symbols = g_ptr_array_new();
    g_autoptr(GHashTable) compat_keys =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    uint32_t static_count = ARRAY_SIZE(shadps4_hle_exports);
    uint64_t symbol_count;
    uint64_t size;
    uint32_t source_image;
    uint32_t i;

    sms->hle.external_nid_count = 0;
    memset(sms->hle.external_nids, 0, sizeof(sms->hle.external_nids));

    for (source_image = 0; source_image <= sms->module_count;
         source_image++) {
        ShadPS4ImageInfo *source = source_image ?
            &sms->modules[source_image - 1] : &sms->image;
        uint32_t symbol_index;

        for (symbol_index = 0; symbol_index < source->symbol_count;
             symbol_index++) {
            ShadPS4DynamicSymbol *symbol = &source->symbols[symbol_index];
            bool is_static = false;
            g_autofree char *key = NULL;
            uint32_t export_index;

            if (symbol->defined || !symbol->nid ||
                !shadps4_hle_compat_module(symbol->module)) {
                continue;
            }
            for (export_index = 0; export_index < static_count;
                 export_index++) {
                const ShadPS4HLEExport *export =
                    &shadps4_hle_exports[export_index];

                if (!strcmp(export->nid, symbol->nid) &&
                    !g_strcmp0(export->library, symbol->library) &&
                    !g_strcmp0(export->module, symbol->module)) {
                    is_static = true;
                    break;
                }
            }
            if (is_static) {
                continue;
            }
            if (shadps4_hle_compat_dispatch(symbol->library,
                                             symbol->module,
                                             symbol->nid) == UINT32_MAX &&
                (symbol->type == SHADPS4_ELF_STT_OBJECT ||
                 !qemu_host_hle_callback_enabled())) {
                continue;
            }
            key = g_strdup_printf("%s|%s|%s", symbol->nid,
                                  symbol->library,
                                  symbol->module ?: "libSceGnmDriver");
            if (g_hash_table_contains(compat_keys, key)) {
                continue;
            }
            g_hash_table_add(compat_keys, g_steal_pointer(&key));
            g_ptr_array_add(compat_symbols, symbol);
        }
    }
    symbol_count = (uint64_t)static_count + compat_symbols->len;
    if (symbol_count > UINT32_MAX ||
        symbol_count > (UINT64_MAX - 32) / 16) {
        error_setg(errp, "too many shadPS4 HLE exports");
        return false;
    }
    size = symbol_count * 16 + SHADPS4_HLE_TRAILER_SIZE +
           SHADPS4_HLE_BOOTSTRAP_SIZE +
           SHADPS4_HLE_SERVICE_GUEST_ARENA_SIZE;
    if (machine->ram_size < SHADPS4_IMAGE_PHYS_BASE ||
        offset > machine->ram_size - SHADPS4_IMAGE_PHYS_BASE ||
        size > machine->ram_size - SHADPS4_IMAGE_PHYS_BASE - offset ||
        SHADPS4_IMAGE_VIRT_BASE > UINT64_MAX - offset) {
        error_setg(errp, "shadPS4 HLE image exceeds guest address space");
        return false;
    }

    shadps4_image_cleanup(image);
    memset(image, 0, sizeof(*image));
    image->virtual_base = SHADPS4_IMAGE_VIRT_BASE + offset;
    image->physical_base = SHADPS4_IMAGE_PHYS_BASE + offset;
    image->image_size = size;
    image->symbol_count = symbol_count;
    image->exported_symbol_count = image->symbol_count;
    image->symbols = g_new0(ShadPS4DynamicSymbol, image->symbol_count);
    image->segment_count = 1;
    image->segments[0].virtual_addr = image->virtual_base;
    image->segments[0].physical_addr = image->physical_base;
    image->segments[0].file_size = size;
    image->segments[0].memory_size = size;
    image->segments[0].flags = 7;

    {
        uint8_t trailer[32] = { 0 };

        memcpy(trailer + 8, "eboot.bin", sizeof("eboot.bin"));
        if (!shadps4_write_guest(
                image->physical_base + image->symbol_count * 16,
                trailer, sizeof(trailer), "HLE data trailer", errp)) {
            shadps4_image_cleanup(image);
            return false;
        }
    }

    for (i = 0; i < image->symbol_count; i++) {
        const ShadPS4HLEExport *export = i < static_count ?
            &shadps4_hle_exports[i] : NULL;
        const ShadPS4DynamicSymbol *compat = export ? NULL :
            g_ptr_array_index(compat_symbols, i - static_count);
        ShadPS4DynamicSymbol *symbol = &image->symbols[i];
        uint8_t stub[16] = {
            0x49, 0x89, 0xca,
            0xb8, 0, 0, 0, 0,
            0x0f, 0x05, 0xc3,
            0x90, 0x90, 0x90, 0x90, 0x90,
        };
        bool is_object = (export &&
                          export->data_kind != SHADPS4_HLE_DATA_NONE) ||
                         (compat && compat->type == SHADPS4_ELF_STT_OBJECT);

        if (is_object) {
            uint64_t value = 0;

            if (export &&
                export->data_kind == SHADPS4_HLE_DATA_STACK_CHK_GUARD) {
                value = 0xdeadbeef54321abcULL;
            } else if (export &&
                       export->data_kind == SHADPS4_HLE_DATA_ENVIRON) {
                value = image->virtual_base + image->symbol_count * 16;
            } else if (export &&
                       export->data_kind == SHADPS4_HLE_DATA_PROGNAME) {
                value = image->virtual_base + image->symbol_count * 16 + 8;
            }
            value = cpu_to_le64(value);
            memset(stub, 0, sizeof(stub));
            memcpy(stub, &value, sizeof(value));
        } else {
            uint32_t dispatch = export ? export->dispatch :
                shadps4_hle_compat_dispatch(compat->library,
                                            compat->module, compat->nid);
            uint32_t dispatch_le;

            if (dispatch == HLE_COMPAT_UNKNOWN) {
                ShadPS4HLEExternalNID *external;

                if (sms->hle.external_nid_count >=
                    SHADPS4_HLE_MAX_EXTERNAL_NIDS) {
                    error_setg(errp, "too many external shadPS4 HLE NIDs");
                    shadps4_image_cleanup(image);
                    return false;
                }
                external = &sms->hle.external_nids[
                    sms->hle.external_nid_count];
                g_strlcpy(external->nid, compat->nid,
                          sizeof(external->nid));
                g_strlcpy(external->library, compat->library ?: "",
                          sizeof(external->library));
                g_strlcpy(external->module, compat->module ?: "",
                          sizeof(external->module));
                dispatch = SHADPS4_HLE_EXTERNAL_BASE +
                           sms->hle.external_nid_count++;
            }
            dispatch_le = cpu_to_le32(dispatch);

            memcpy(stub + 4, &dispatch_le, sizeof(dispatch_le));
        }
        if (!shadps4_write_guest(image->physical_base + i * sizeof(stub),
                                 stub, sizeof(stub), "HLE export stub",
                                 errp)) {
            shadps4_image_cleanup(image);
            return false;
        }
        symbol->nid = g_strdup(export ? export->nid : compat->nid);
        symbol->library = g_strdup(export ? export->library : compat->library);
        symbol->module = g_strdup(export ? export->module : compat->module);
        symbol->value = i * sizeof(stub);
        symbol->library_version = 1;
        symbol->type = is_object ? SHADPS4_ELF_STT_OBJECT :
                                  SHADPS4_ELF_STT_FUNC;
        symbol->binding = 1;
        symbol->defined = true;
    }
    return true;
}

static gint shadps4_compare_module_names(gconstpointer left,
                                         gconstpointer right)
{
    const char *const *left_name = left;
    const char *const *right_name = right;

    return g_strcmp0(*left_name, *right_name);
}

static void shadps4_cleanup_images(ShadPS4MachineState *sms)
{
    uint32_t i;

    shadps4_hle_set_modules(&sms->hle, NULL, NULL, 0, NULL);
    shadps4_image_cleanup(&sms->image);
    for (i = 0; i < sms->module_count; i++) {
        shadps4_image_cleanup(&sms->modules[i]);
    }
    shadps4_image_cleanup(&sms->hle_image);
    sms->module_count = 0;
    sms->mapped_image_size = 0;
    sms->image_loaded = false;
}

static bool shadps4_read_proc_param(ShadPS4MachineState *sms, Error **errp)
{
    uint8_t header[24];
    uint64_t physical_addr;
    uint64_t size;
    uint64_t sdk_version;

    sms->hle.proc_param_guest_addr = sms->image.proc_param_addr;
    sms->hle.compiled_sdk_version = 0;
    if (!sms->image.proc_param_addr) {
        return true;
    }
    physical_addr = sms->image.physical_base +
                    (sms->image.proc_param_addr - sms->image.virtual_base);
    if (address_space_read(&address_space_memory, physical_addr,
                           MEMTXATTRS_UNSPECIFIED, header,
                           sizeof(header)) != MEMTX_OK) {
        error_setg(errp, "failed to read PT_SCE_PROCPARAM");
        return false;
    }
    size = ldq_le_p(header);
    sdk_version = ldq_le_p(header + 16);
    if (size < sizeof(header) || size > sms->image.proc_param_size ||
        sdk_version > UINT32_MAX) {
        error_setg(errp, "PT_SCE_PROCPARAM header is invalid");
        return false;
    }
    sms->hle.compiled_sdk_version = sdk_version;
    return true;
}

static uint32_t shadps4_unresolved_relocations(ShadPS4MachineState *sms)
{
    uint32_t unresolved = sms->image.unresolved_relocation_count;
    uint32_t i;

    for (i = 0; i < sms->module_count; i++) {
        unresolved += sms->modules[i].unresolved_relocation_count;
    }
    return unresolved;
}

static bool shadps4_load_images(ShadPS4MachineState *sms,
                                const char *filename, Error **errp)
{
    MachineState *machine = MACHINE(sms);
    bool brokered = qemu_host_storage_path_is_brokered(filename);
    g_autofree char *root = NULL;
    g_autofree char *module_dir = NULL;
    g_autofree char *ps2_compiler = NULL;
    const char *main_filename = filename;
    g_autoptr(GPtrArray) module_names =
        g_ptr_array_new_with_free_func(g_free);
    ShadPS4ImageInfo *images[SHADPS4_MAX_MODULES + 1];
    uint64_t next_offset;
    uint32_t next_tls_id;
    uint32_t i;
    uint32_t module_entries_found = 0;
    uint32_t module_entries_ignored = 0;
    uint32_t module_entries_invalid = 0;
    bool ps2_compiler_exists = false;
    bool preload_ps2_compiler = false;

    if (brokered) {
        const char *separator = strrchr(filename, '/');

        root = separator == filename ? g_strdup("/") :
               g_strndup(filename, separator - filename);
        module_dir = !strcmp(root, "/") ? g_strdup("/sce_module") :
                     g_strdup_printf("%s/sce_module", root);
        ps2_compiler = !strcmp(root, "/") ?
                       g_strdup("/ps2-emu-compiler.self") :
                       g_strdup_printf("%s/ps2-emu-compiler.self", root);
    } else {
        root = g_path_get_dirname(filename);
        module_dir = g_build_filename(root, "sce_module", NULL);
        ps2_compiler = g_build_filename(root, "ps2-emu-compiler.self", NULL);
    }

    if (g_str_has_suffix(filename, "eboot.bin")) {
        if (brokered) {
            QemuHostStorageStat stat;

            ps2_compiler_exists =
                qemu_host_storage_stat(ps2_compiler, &stat) == 0 &&
                stat.type == 8; /* DT_REG in the brokered storage ABI. */
        } else {
            ps2_compiler_exists =
                g_file_test(ps2_compiler, G_FILE_TEST_IS_REGULAR);
        }
        if (ps2_compiler_exists && g_getenv("SHADPS4_PS2_COMPILER_DIRECT")) {
            main_filename = ps2_compiler;
            info_report("shadPS4 PS2 compiler diagnostic mode selected: '%s' "
                        "(launcher='%s')", main_filename, filename);
        } else if (ps2_compiler_exists) {
            preload_ps2_compiler = true;
            info_report("shadPS4 PS2 Classics launcher detected: '%s' "
                        "compiler='%s'", filename, ps2_compiler);
        }
    }

    shadps4_cleanup_images(sms);
    sms->ps2_compiler_image_entry = 0;
    sms->hle.ps2_compiler_entry = 0;
    sms->hle.ps2_compiler_proc_param = 0;
    if (!shadps4_load_module(main_filename, &address_space_memory,
                             SHADPS4_IMAGE_VIRT_BASE,
                             SHADPS4_IMAGE_PHYS_BASE, machine->ram_size, 1,
                             &sms->image, errp)) {
        return false;
    }
    if (!shadps4_read_proc_param(sms, errp)) {
        shadps4_cleanup_images(sms);
        return false;
    }
    if (!shadps4_align_up_valid(sms->image.image_size, 2 * MiB,
                                &next_offset)) {
        error_setg(errp, "main image size overflows its aligned mapping");
        shadps4_cleanup_images(sms);
        return false;
    }
    next_tls_id = sms->image.tls_present ? 2 : 1;
    if (qemu_host_storage_path_is_brokered(module_dir)) {
        int64_t directory_handle;
        int ret = qemu_host_storage_open(
            module_dir, O_RDONLY | QEMU_HOST_STORAGE_OPEN_DIRECTORY, 0,
            &directory_handle);

        if (ret < 0 && ret != -ENOENT) {
            error_setg_errno(errp, -ret,
                             "failed to open brokered module directory");
            shadps4_cleanup_images(sms);
            return false;
        }
        if (ret == 0) {
            for (;;) {
                QemuHostStorageStat stat;
                char name[256];

                ret = qemu_host_storage_readdir(directory_handle, name,
                                                sizeof(name), &stat);
                if (ret <= 0) {
                    break;
                }
                module_entries_found++;
                name[sizeof(name) - 1] = 0;
                if (!name[0] || strchr(name, '/') || strchr(name, '\\')) {
                    module_entries_invalid++;
                    warn_report("shadPS4 sce_module entry ignored: "
                                "reason=invalid-name name='%s'", name);
                    ret = -EIO;
                    break;
                }
                if (g_str_has_suffix(name, ".prx") ||
                    g_str_has_suffix(name, ".sprx")) {
                    g_ptr_array_add(module_names, g_strdup(name));
                } else {
                    module_entries_ignored++;
                }
            }
            if (qemu_host_storage_close(directory_handle) < 0 && ret >= 0) {
                ret = -EIO;
            }
            if (ret < 0) {
                info_report("shadPS4 sce_module enumeration: found=%u "
                            "loadable=%u ignored=%u invalid=%u "
                            "status=failed",
                            module_entries_found, module_names->len,
                            module_entries_ignored + module_entries_invalid,
                            module_entries_invalid);
                error_setg_errno(errp, -ret,
                                 "failed to enumerate brokered modules");
                shadps4_cleanup_images(sms);
                return false;
            }
        }
    } else {
        GError *gerror = NULL;
        GDir *dir = g_dir_open(module_dir, 0, &gerror);

        if (dir) {
            const char *name;

            while ((name = g_dir_read_name(dir))) {
                module_entries_found++;
                if (g_str_has_suffix(name, ".prx") ||
                    g_str_has_suffix(name, ".sprx")) {
                    g_ptr_array_add(module_names, g_strdup(name));
                } else {
                    module_entries_ignored++;
                }
            }
            g_dir_close(dir);
        } else if (!g_error_matches(gerror, G_FILE_ERROR,
                                     G_FILE_ERROR_NOENT)) {
            error_setg(errp, "failed to enumerate module directory '%s': %s",
                       module_dir, gerror->message);
            g_clear_error(&gerror);
            shadps4_cleanup_images(sms);
            return false;
        }
        g_clear_error(&gerror);
    }
    info_report("shadPS4 sce_module enumeration: found=%u loadable=%u "
                "ignored=%u invalid=%u ignored_unsupported_extension=%u",
                module_entries_found, module_names->len,
                module_entries_ignored + module_entries_invalid,
                module_entries_invalid, module_entries_ignored);
    g_ptr_array_sort(module_names, shadps4_compare_module_names);

    for (i = 0; i < module_names->len; i++) {
        const char *name = g_ptr_array_index(module_names, i);
        g_autofree char *path = brokered ?
            g_strdup_printf("%s/%s", module_dir, name) :
            g_build_filename(module_dir, name, NULL);
        ShadPS4ImageInfo *module;

        if (sms->module_count == SHADPS4_MAX_MODULES - 1) {
            error_setg(errp, "title provides more than %u loadable modules",
                       SHADPS4_MAX_MODULES - 1);
            shadps4_cleanup_images(sms);
            return false;
        }
        if (machine->ram_size < SHADPS4_IMAGE_PHYS_BASE ||
            next_offset > machine->ram_size - SHADPS4_IMAGE_PHYS_BASE ||
            SHADPS4_IMAGE_VIRT_BASE > UINT64_MAX - next_offset) {
            error_setg(errp, "module address space exceeds guest RAM");
            shadps4_cleanup_images(sms);
            return false;
        }
        module = &sms->modules[sms->module_count];
        if (!shadps4_load_module(
                path, &address_space_memory,
                SHADPS4_IMAGE_VIRT_BASE + next_offset,
                SHADPS4_IMAGE_PHYS_BASE + next_offset, machine->ram_size,
                next_tls_id, module, errp)) {
            shadps4_cleanup_images(sms);
            return false;
        }
        info_report("shadPS4 module loaded: %s base=0x%" PRIx64
                    " size=0x%" PRIx64 " TLS=%u init=%s0x%" PRIx64
                    " entry=0x%" PRIx64,
                    name, module->virtual_base, module->image_size,
                    module->tls_module_id,
                    module->init_present ? "" : "absent/",
                    module->init, module->entry);
        sms->module_count++;
        if (module->tls_present) {
            next_tls_id++;
        }
        if (module->image_size > UINT64_MAX - next_offset ||
            !shadps4_align_up_valid(next_offset + module->image_size,
                                    2 * MiB, &next_offset)) {
            error_setg(errp, "module mapping size overflows");
            shadps4_cleanup_images(sms);
            return false;
        }
    }
    if (preload_ps2_compiler) {
        ShadPS4ImageInfo *compiler;

        if (sms->module_count == SHADPS4_MAX_MODULES - 1) {
            error_setg(errp, "no module slot remains for ps2-emu-compiler.self");
            shadps4_cleanup_images(sms);
            return false;
        }
        compiler = &sms->modules[sms->module_count];
        if (!shadps4_load_module(
                ps2_compiler, &address_space_memory,
                SHADPS4_IMAGE_VIRT_BASE + next_offset,
                SHADPS4_IMAGE_PHYS_BASE + next_offset, machine->ram_size,
                next_tls_id, compiler, errp)) {
            shadps4_cleanup_images(sms);
            return false;
        }
        sms->ps2_compiler_image_entry = compiler->entry;
        sms->hle.ps2_compiler_entry = compiler->entry;
        sms->hle.ps2_compiler_proc_param = compiler->proc_param_addr;
        info_report("shadPS4 PS2 compiler preloaded: base=0x%" PRIx64
                    " entry=0x%" PRIx64 " proc_param=0x%" PRIx64,
                    compiler->virtual_base, compiler->entry,
                    compiler->proc_param_addr);
        sms->module_count++;
        if (compiler->tls_present) {
            next_tls_id++;
        }
        if (compiler->image_size > UINT64_MAX - next_offset ||
            !shadps4_align_up_valid(next_offset + compiler->image_size,
                                    2 * MiB, &next_offset)) {
            error_setg(errp, "PS2 compiler mapping size overflows");
            shadps4_cleanup_images(sms);
            return false;
        }
    }
    images[0] = &sms->image;
    for (i = 0; i < sms->module_count; i++) {
        images[i + 1] = &sms->modules[i];
    }
    if (!shadps4_build_hle_image(sms, next_offset, errp)) {
        shadps4_cleanup_images(sms);
        return false;
    }
    images[sms->module_count + 1] = &sms->hle_image;
    if (sms->hle_image.image_size > UINT64_MAX - next_offset ||
        !shadps4_align_up_valid(next_offset + sms->hle_image.image_size,
                                2 * MiB, &next_offset) ||
        machine->ram_size < SHADPS4_IMAGE_PHYS_BASE ||
        next_offset > machine->ram_size - SHADPS4_IMAGE_PHYS_BASE) {
        error_setg(errp, "combined image mapping exceeds guest RAM");
        shadps4_cleanup_images(sms);
        return false;
    }
    if (!shadps4_link_modules(&address_space_memory, images,
                              sms->module_count + 2, errp)) {
        shadps4_cleanup_images(sms);
        return false;
    }
    sms->mapped_image_size = next_offset;
    sms->image_loaded = true;
    shadps4_hle_set_modules(&sms->hle, &sms->image, sms->modules,
                            sms->module_count, &sms->hle_image);
    return true;
}

static void shadps4_machine_init(MachineState *machine)
{
    ShadPS4MachineState *sms = SHADPS4_MACHINE(machine);
    X86MachineState *x86ms = X86_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    Error *local_err = NULL;

    if (sms->execute && !sms->title_id) {
        error_report("shadps4 execute mode requires a title-id");
        sms->execute = false;
    }
    info_report("hardware profile: %s, AMD Jaguar, 8 cores, %s",
                sms->variant == SHADPS4_VARIANT_NEO ?
                "PS4 Pro (Neo)" : "PS4 (Liverpool/base)",
                tcg_enabled() ? "TCG" : "native accelerator");

    x86ms->below_4g_mem_size = MIN(machine->ram_size, 4 * GiB);
    x86ms->above_4g_mem_size = machine->ram_size - x86ms->below_4g_mem_size;

    memory_region_add_subregion(system_memory, 0, machine->ram);
    shadps4_gpu_init(&sms->gpu);
    sms->io = SHADPS4_IO(qdev_new(TYPE_SHADPS4_IO));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sms->io), &local_err);
    if (local_err) {
        sms->io = NULL;
        error_report_err(local_err);
        return;
    }
    shadps4_hle_init(&sms->hle, &address_space_memory, &sms->gpu, sms->io,
                     sms->title_id ? sms->title_id : "UNCONFIGURED",
                     SHADPS4_PD_DYNAMIC_PHYS + 64 * 4096,
                     SHADPS4_DYNAMIC_VIRT_BASE);
    shadps4_hle_set_thread_callbacks(&sms->hle, sms,
                                     shadps4_start_guest_thread,
                                     shadps4_exit_guest_thread,
                                     shadps4_wake_guest_thread,
                                     shadps4_kill_guest_thread);
    sms->hle.neo_mode = sms->variant == SHADPS4_VARIANT_NEO;
    memory_region_init_io(&sms->hle_gateway, OBJECT(sms),
                          &shadps4_hle_gateway_ops, sms,
                          "shadps4-hle-gateway", 4096);
    memory_region_add_subregion_overlap(system_memory,
                                        SHADPS4_HLE_GATEWAY_PHYS,
                                        &sms->hle_gateway, 10);
    if (!shadps4_cpus_init(x86ms, &local_err)) {
        shadps4_halt_cpus();
        error_report_err(local_err);
        return;
    }
    /* CPUs stay parked until the hybrid loader installs a process. */
    shadps4_halt_cpus();
    sms->scheduler_timer = timer_new_ns(
        QEMU_CLOCK_REALTIME, shadps4_scheduler_tick, sms);
    timer_mod(sms->scheduler_timer,
              qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
              SHADPS4_SCHEDULER_QUANTUM_NS);

    if (machine->kernel_filename) {
        sms->image_loaded = shadps4_load_images(
            sms, machine->kernel_filename, &local_err);
        if (!sms->image_loaded) {
            error_report_err(local_err);
        } else {
            info_report("shadPS4 image loaded: entry=0x%" PRIx64
                        " physical=0x%" PRIx64 " size=0x%" PRIx64
                        " symbols=%u relocations=%u/%u unresolved=%u",
                        sms->image.entry, sms->image.physical_entry,
                        sms->image.image_size, sms->image.symbol_count,
                        sms->image.applied_relocation_count,
                        sms->image.relocation_count,
                        shadps4_unresolved_relocations(sms));
        }
    } else if (sms->execute) {
        error_report("shadps4 execute mode requires a kernel image");
    }
}

static void shadps4_machine_reset(MachineState *machine, ResetType type)
{
    ShadPS4MachineState *sms = SHADPS4_MACHINE(machine);
    CPUState *cs;
    Error *local_err = NULL;

    qemu_devices_reset(type);
    CPU_FOREACH(cs) {
        x86_cpu_after_reset(X86_CPU(cs));
    }
    shadps4_halt_cpus();
    qemu_mutex_lock(&sms->scheduler_lock);
    memset(sms->thread_contexts, 0, sizeof(sms->thread_contexts));
    memset(sms->pending_thread_switch, 0,
           sizeof(sms->pending_thread_switch));
    memset(sms->hle_gateway_active, 0, sizeof(sms->hle_gateway_active));
    sms->scheduler_order = 0;
    sms->scheduler_switches = 0;
    qemu_mutex_unlock(&sms->scheduler_lock);
    if (sms->execute && sms->image_loaded &&
        !shadps4_prepare_boot_cpu(sms, &local_err)) {
        error_report_err(local_err);
    }
}

static void shadps4_machine_instance_init(Object *obj)
{
    ShadPS4MachineState *sms = SHADPS4_MACHINE(obj);

    qemu_mutex_init(&sms->hle_lock);
    qemu_mutex_init(&sms->scheduler_lock);
    sms->execute = false;
    sms->variant = SHADPS4_VARIANT_BASE;
}

static void shadps4_machine_instance_finalize(Object *obj)
{
    ShadPS4MachineState *sms = SHADPS4_MACHINE(obj);

    if (sms->scheduler_timer) {
        timer_free(sms->scheduler_timer);
        sms->scheduler_timer = NULL;
    }
    shadps4_hle_cleanup(&sms->hle);
    shadps4_gpu_cleanup(&sms->gpu);
    shadps4_cleanup_images(sms);
    g_free(sms->title_id);
    qemu_mutex_destroy(&sms->scheduler_lock);
    qemu_mutex_destroy(&sms->hle_lock);
}

static char *shadps4_machine_get_title_id(Object *obj, Error **errp)
{
    return g_strdup(SHADPS4_MACHINE(obj)->title_id);
}

static void shadps4_machine_set_title_id(Object *obj, const char *value,
                                         Error **errp)
{
    ShadPS4MachineState *sms = SHADPS4_MACHINE(obj);
    size_t i;

    if (!value || !value[0] || strlen(value) > 32) {
        error_setg(errp, "title-id must contain between 1 and 32 characters");
        return;
    }
    for (i = 0; value[i]; i++) {
        if (!(g_ascii_isupper(value[i]) || g_ascii_isdigit(value[i]) ||
              value[i] == '_' || value[i] == '-')) {
            error_setg(errp, "title-id contains an invalid character");
            return;
        }
    }
    g_free(sms->title_id);
    sms->title_id = g_strdup(value);
}

static char *shadps4_machine_get_variant(Object *obj, Error **errp)
{
    return g_strdup(SHADPS4_MACHINE(obj)->variant == SHADPS4_VARIANT_NEO ?
                    "neo" : "base");
}

static void shadps4_machine_set_variant(Object *obj, const char *value,
                                        Error **errp)
{
    ShadPS4MachineState *sms = SHADPS4_MACHINE(obj);

    if (!g_strcmp0(value, "base")) {
        sms->variant = SHADPS4_VARIANT_BASE;
    } else if (!g_strcmp0(value, "neo")) {
        sms->variant = SHADPS4_VARIANT_NEO;
    } else {
        error_setg(errp, "variant must be 'base' or 'neo'");
    }
}

static void shadps4_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = shadps4_machine_init;
    mc->reset = shadps4_machine_reset;
    mc->family = "Sony PlayStation 4";
    mc->desc = "PS4 Liverpool/base or PS4 Pro Neo hybrid HLE machine";
    mc->default_cpu_type = TARGET_DEFAULT_CPU_TYPE;
    mc->default_cpus = 8;
    mc->min_cpus = 8;
    mc->max_cpus = 8;
    mc->default_ram_size = 5248 * MiB;
    mc->default_ram_id = "shadps4.ram";
    mc->has_hotpluggable_cpus = false;
    mc->auto_enable_numa_with_memhp = false;
    mc->auto_enable_numa_with_memdev = false;
    mc->nvdimm_supported = false;
    mc->no_serial = true;
    mc->no_parallel = true;
    mc->no_floppy = true;
    mc->no_cdrom = true;
    mc->auto_create_sdcard = false;
    mc->default_boot_order = "";

    object_class_property_add_bool(oc, "execute",
                                   shadps4_machine_get_execute,
                                   shadps4_machine_set_execute);
    object_class_property_set_description(
        oc, "execute", "Enter x86_64 long mode and start the loaded image");
    object_class_property_add_str(oc, "title-id",
                                  shadps4_machine_get_title_id,
                                  shadps4_machine_set_title_id);
    object_class_property_set_description(
        oc, "title-id", "Required sandbox identifier for the guest title");
    object_class_property_add_str(oc, "variant",
                                  shadps4_machine_get_variant,
                                  shadps4_machine_set_variant);
    object_class_property_set_description(
        oc, "variant", "Hardware variant: base (Liverpool) or neo (PS4 Pro)");
}

static const TypeInfo shadps4_machine_info = {
    .name = TYPE_SHADPS4_MACHINE,
    .parent = TYPE_X86_MACHINE,
    .instance_size = sizeof(ShadPS4MachineState),
    .instance_init = shadps4_machine_instance_init,
    .instance_finalize = shadps4_machine_instance_finalize,
    .class_init = shadps4_machine_class_init,
};

static void shadps4_machine_register_types(void)
{
    type_register_static(&shadps4_machine_info);
}

type_init(shadps4_machine_register_types)
