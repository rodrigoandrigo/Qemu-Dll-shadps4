/*
 * shadPS4 hybrid ELF/SELF image loader
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I386_SHADPS4_LOADER_H
#define HW_I386_SHADPS4_LOADER_H

#include "qapi/error.h"
#include "system/memory.h"

#define SHADPS4_IMAGE_VIRT_BASE 0x800000000ULL
#define SHADPS4_IMAGE_PHYS_BASE 0x01000000ULL
#define SHADPS4_MAX_SEGMENTS 16
#define SHADPS4_MAX_MODULES 16

typedef struct ShadPS4ImageSegment {
    uint64_t virtual_addr;
    uint64_t physical_addr;
    uint64_t file_size;
    uint64_t memory_size;
    uint32_t flags;
} ShadPS4ImageSegment;

typedef struct ShadPS4DynamicSymbol {
    char *nid;
    char *library;
    char *module;
    uint64_t value;
    uint16_t library_version;
    uint8_t type;
    uint8_t binding;
    bool defined;
} ShadPS4DynamicSymbol;

typedef struct ShadPS4PendingRelocation {
    uint64_t physical_addr;
    int64_t addend;
    uint32_t symbol_index;
    uint32_t type;
    bool applied;
} ShadPS4PendingRelocation;

typedef struct ShadPS4ImageInfo {
    char *name;
    bool is_self;
    uint16_t elf_type;
    uint64_t virtual_base;
    uint64_t physical_base;
    uint64_t entry;
    uint64_t physical_entry;
    uint64_t image_size;
    uint64_t proc_param_addr;
    uint64_t proc_param_size;
    uint64_t eh_frame_hdr_addr;
    uint64_t eh_frame_addr;
    uint32_t eh_frame_hdr_size;
    uint32_t eh_frame_size;
    uint64_t tls_addr;
    uint64_t tls_file_size;
    uint64_t tls_memory_size;
    uint64_t tls_align;
    uint32_t tls_module_id;
    bool tls_present;
    uint32_t symbol_count;
    uint32_t exported_symbol_count;
    uint32_t imported_symbol_count;
    uint32_t relocation_count;
    uint32_t applied_relocation_count;
    uint32_t unresolved_relocation_count;
    uint16_t segment_count;
    ShadPS4ImageSegment segments[SHADPS4_MAX_SEGMENTS];
    ShadPS4DynamicSymbol *symbols;
    ShadPS4PendingRelocation *pending_relocations;
    uint32_t pending_relocation_count;
} ShadPS4ImageInfo;

bool shadps4_load_image(const char *filename, AddressSpace *as,
                        uint64_t physical_base, uint64_t physical_limit,
                        ShadPS4ImageInfo *info, Error **errp);
bool shadps4_load_module(const char *filename, AddressSpace *as,
                         uint64_t virtual_base, uint64_t physical_base,
                         uint64_t physical_limit, uint32_t tls_module_id,
                         ShadPS4ImageInfo *info, Error **errp);
bool shadps4_link_modules(AddressSpace *as, ShadPS4ImageInfo **images,
                          uint32_t image_count, Error **errp);
void shadps4_image_cleanup(ShadPS4ImageInfo *info);

#endif /* HW_I386_SHADPS4_LOADER_H */
