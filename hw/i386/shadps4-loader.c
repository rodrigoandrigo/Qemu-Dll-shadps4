/*
 * shadPS4 hybrid ELF/SELF image loader
 *
 * The SELF container layout and SCE program-header values are based on the
 * GPL-2.0-or-later shadPS4 loader. Encrypted and compressed SELF segments are
 * deliberately rejected until a host-provided, legally sourced decoder is
 * defined.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define QEMU_HOST_INTERNAL
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/qemu-host.h"
#include "qemu/units.h"
#include "elf.h"
#include "shadps4-loader.h"

#define SHADPS4_SELF_MAGIC 0x1d3d154fU
#define SHADPS4_ELF_OSABI_FREEBSD 9
#define SHADPS4_EI_ABIVERSION 8
#define SHADPS4_ET_SCE_EXEC 0xfe00
#define SHADPS4_ET_SCE_DYNEXEC 0xfe10
#define SHADPS4_ET_SCE_DYNAMIC 0xfe18
#define SHADPS4_PT_SCE_RELRO 0x61000010U
#define SHADPS4_PT_SCE_DYNLIBDATA 0x61000000U
#define SHADPS4_PT_SCE_PROCPARAM 0x61000001U
#define SHADPS4_PT_TLS 7
#define SHADPS4_PT_GNU_EH_FRAME 0x6474e550U
#define SHADPS4_DT_SCE_JMPREL 0x61000029ULL
#define SHADPS4_DT_SCE_PLTREL 0x6100002bULL
#define SHADPS4_DT_SCE_PLTRELSZ 0x6100002dULL
#define SHADPS4_DT_SCE_RELA 0x6100002fULL
#define SHADPS4_DT_SCE_RELASZ 0x61000031ULL
#define SHADPS4_DT_SCE_RELAENT 0x61000033ULL
#define SHADPS4_DT_SCE_STRTAB 0x61000035ULL
#define SHADPS4_DT_SCE_STRSZ 0x61000037ULL
#define SHADPS4_DT_SCE_SYMTAB 0x61000039ULL
#define SHADPS4_DT_SCE_SYMENT 0x6100003bULL
#define SHADPS4_DT_SCE_SYMTABSZ 0x6100003fULL
#define SHADPS4_DT_SCE_MODULE_INFO 0x6100000dULL
#define SHADPS4_DT_SCE_NEEDED_MODULE 0x6100000fULL
#define SHADPS4_DT_SCE_EXPORT_LIB 0x61000013ULL
#define SHADPS4_DT_SCE_IMPORT_LIB 0x61000015ULL
#define SHADPS4_R_X86_64_DTPMOD64 16
#define SHADPS4_SELF_SEG_BLOCKED 0x800ULL
#define SHADPS4_SELF_SEG_ENCRYPTED 0x2ULL
#define SHADPS4_SELF_SEG_COMPRESSED 0x8ULL
#define SHADPS4_COPY_CHUNK (1 * MiB)

typedef struct QEMU_PACKED ShadPS4SelfHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t mode;
    uint8_t endian;
    uint8_t attributes;
    uint8_t category;
    uint8_t program_type;
    uint16_t padding1;
    uint16_t header_size;
    uint16_t meta_size;
    uint32_t file_size;
    uint32_t padding2;
    uint16_t segment_count;
    uint16_t unknown1a;
    uint32_t padding3;
} ShadPS4SelfHeader;

typedef struct QEMU_PACKED ShadPS4SelfSegment {
    uint64_t flags;
    uint64_t file_offset;
    uint64_t file_size;
    uint64_t memory_size;
} ShadPS4SelfSegment;

typedef struct ShadPS4DynamicInfo {
    uint64_t strtab;
    uint64_t strsz;
    uint64_t symtab;
    uint64_t syment;
    uint64_t symtabsz;
    uint64_t rela;
    uint64_t relasz;
    uint64_t relaent;
    uint64_t jmprel;
    uint64_t pltrelsz;
    uint64_t pltrel;
    uint64_t init;
    uint64_t fini;
    bool init_present;
    bool fini_present;
} ShadPS4DynamicInfo;

static void shadps4_encode_id(uint16_t value, char encoded[4])
{
    static const char codes[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    size_t length;

    if (value < 0x40) {
        length = 1;
    } else if (value < 0x1000) {
        length = 2;
    } else {
        length = 3;
    }
    encoded[length] = '\0';
    while (length) {
        encoded[--length] = codes[value & 0x3f];
        value >>= 6;
    }
}

static bool shadps4_range_valid(uint64_t offset, uint64_t size,
                                uint64_t limit)
{
    return offset <= limit && size <= limit - offset;
}

static bool shadps4_add_valid(uint64_t left, uint64_t right,
                              uint64_t *result)
{
    if (right > UINT64_MAX - left) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool shadps4_add_signed_valid(uint64_t base, int64_t addend,
                                     uint64_t *result)
{
    uint64_t magnitude;

    if (addend >= 0) {
        return shadps4_add_valid(base, addend, result);
    }
    magnitude = (uint64_t)(-(addend + 1)) + 1;
    if (base < magnitude) {
        return false;
    }
    *result = base - magnitude;
    return true;
}

typedef struct ShadPS4HostFile {
    bool brokered;
    int fd;
    int64_t handle;
    const char *path;
} ShadPS4HostFile;

static int shadps4_host_file_open(ShadPS4HostFile *file, const char *path,
                                  Error **errp)
{
    int ret;

    *file = (ShadPS4HostFile) { .fd = -1, .handle = -1 };
    file->path = path;
    if (qemu_host_storage_path_is_brokered(path)) {
        ret = qemu_host_storage_open(path, O_RDONLY | O_BINARY, 0,
                                     &file->handle);
        if (ret < 0) {
            error_setg_errno(errp, -ret,
                             "failed to open brokered image '%s'", path);
            return ret;
        }
        file->brokered = true;
        return 0;
    }
    file->fd = qemu_open(path, O_RDONLY | O_BINARY, errp);
    return file->fd < 0 ? -errno : 0;
}

static int64_t shadps4_host_file_seek(ShadPS4HostFile *file, int64_t offset,
                                      int whence)
{
    return file->brokered ? qemu_host_storage_seek(file->handle, offset,
                                                    whence) :
                            lseek(file->fd, offset, whence);
}

static int64_t shadps4_host_file_read(ShadPS4HostFile *file, void *buffer,
                                      size_t size)
{
    return file->brokered ? qemu_host_storage_read(file->handle, buffer, size) :
                            read(file->fd, buffer, size);
}

static void shadps4_host_file_close(ShadPS4HostFile *file)
{
    if (file->brokered) {
        qemu_host_storage_close(file->handle);
    } else if (file->fd >= 0) {
        close(file->fd);
    }
    file->fd = -1;
    file->handle = -1;
}

static bool shadps4_read_exact(ShadPS4HostFile *file, uint64_t offset,
                               void *buffer,
                               size_t size, Error **errp)
{
    uint8_t *cursor = buffer;

    if (offset > INT64_MAX ||
        shadps4_host_file_seek(file, offset, SEEK_SET) < 0) {
        error_setg(errp, "failed to seek in image '%s' at offset %#" PRIx64,
                   file->path ?: "<unknown>", offset);
        return false;
    }
    while (size) {
        int64_t bytes = shadps4_host_file_read(file, cursor, size);

        if (bytes < 0) {
            error_setg(errp, "failed to read image '%s' at offset %#" PRIx64,
                       file->path ?: "<unknown>", offset);
            return false;
        }
        if (bytes == 0) {
            error_setg(errp, "unexpected end of image");
            return false;
        }
        cursor += bytes;
        size -= bytes;
    }
    return true;
}

static bool shadps4_elf_type_valid(uint16_t type)
{
    return type == SHADPS4_ET_SCE_EXEC ||
           type == SHADPS4_ET_SCE_DYNEXEC ||
           type == SHADPS4_ET_SCE_DYNAMIC;
}

static bool shadps4_self_segment_offset(const ShadPS4SelfSegment *segments,
                                        uint16_t segment_count,
                                        uint16_t phdr_index,
                                        uint64_t file_size,
                                        uint64_t *offset, Error **errp)
{
    uint16_t i;

    for (i = 0; i < segment_count; i++) {
        uint64_t flags = le64_to_cpu(segments[i].flags);
        uint32_t id = (flags >> 20) & 0xfff;
        uint64_t segment_size = le64_to_cpu(segments[i].file_size);
        uint64_t segment_offset = le64_to_cpu(segments[i].file_offset);

        if (!(flags & SHADPS4_SELF_SEG_BLOCKED) || id != phdr_index) {
            continue;
        }
        if (flags & (SHADPS4_SELF_SEG_ENCRYPTED |
                     SHADPS4_SELF_SEG_COMPRESSED)) {
            error_setg(errp,
                       "SELF segment %u is encrypted or compressed",
                       phdr_index);
            return false;
        }
        if (file_size > segment_size) {
            error_setg(errp, "SELF segment %u is shorter than PT_LOAD",
                       phdr_index);
            return false;
        }
        *offset = segment_offset;
        return true;
    }

    error_setg(errp, "SELF has no data segment for program header %u",
               phdr_index);
    return false;
}

static bool shadps4_self_range_offset(const ShadPS4SelfSegment *segments,
                                      uint16_t segment_count,
                                      const Elf64_Phdr *phdrs,
                                      uint16_t phnum, uint64_t file_offset,
                                      uint64_t file_size, uint64_t *offset,
                                      Error **errp)
{
    uint64_t range_end;
    uint16_t i;

    if (!shadps4_add_valid(file_offset, file_size, &range_end)) {
        error_setg(errp, "SELF program data range overflows");
        return false;
    }

    for (i = 0; i < segment_count; i++) {
        uint64_t flags = le64_to_cpu(segments[i].flags);
        uint32_t id = (flags >> 20) & 0xfff;
        uint64_t phdr_offset;
        uint64_t phdr_size;
        uint64_t relative_offset;
        uint64_t segment_size;
        uint64_t segment_offset;

        if (!(flags & SHADPS4_SELF_SEG_BLOCKED)) {
            continue;
        }
        if (id >= phnum) {
            error_setg(errp,
                       "SELF data segment references program header %u", id);
            return false;
        }
        phdr_offset = le64_to_cpu(phdrs[id].p_offset);
        phdr_size = le64_to_cpu(phdrs[id].p_filesz);
        if (file_offset < phdr_offset) {
            continue;
        }
        relative_offset = file_offset - phdr_offset;
        if (!shadps4_range_valid(relative_offset, file_size, phdr_size)) {
            continue;
        }
        if (flags & (SHADPS4_SELF_SEG_ENCRYPTED |
                     SHADPS4_SELF_SEG_COMPRESSED)) {
            error_setg(errp,
                       "SELF segment %u is encrypted or compressed", id);
            return false;
        }
        segment_size = le64_to_cpu(segments[i].file_size);
        segment_offset = le64_to_cpu(segments[i].file_offset);
        if (!shadps4_range_valid(relative_offset, file_size, segment_size) ||
            !shadps4_add_valid(segment_offset, relative_offset, offset)) {
            error_setg(errp, "SELF segment %u is shorter than program data",
                       id);
            return false;
        }
        return true;
    }

    error_setg(errp, "SELF has no data segment for file range 0x%" PRIx64
               "..0x%" PRIx64, file_offset, range_end);
    return false;
}

static bool shadps4_copy_segment(ShadPS4HostFile *file, uint64_t file_offset,
                                 uint64_t file_size, AddressSpace *as,
                                 uint64_t physical_addr, Error **errp)
{
    g_autofree uint8_t *buffer = g_malloc(SHADPS4_COPY_CHUNK);
    uint64_t copied = 0;

    while (copied < file_size) {
        size_t chunk = MIN(file_size - copied, SHADPS4_COPY_CHUNK);
        MemTxResult result;

        if (!shadps4_read_exact(file, file_offset + copied, buffer, chunk,
                                errp)) {
            return false;
        }
        result = address_space_write(as, physical_addr + copied,
                                     MEMTXATTRS_UNSPECIFIED, buffer, chunk);
        if (result != MEMTX_OK) {
            error_setg(errp, "failed to write image at physical address "
                       "0x%" PRIx64, physical_addr + copied);
            return false;
        }
        copied += chunk;
    }
    return true;
}

static bool shadps4_program_offset(bool is_self,
                                   const ShadPS4SelfSegment *self_segments,
                                   uint16_t self_segment_count,
                                   const Elf64_Phdr *phdrs, uint16_t phnum,
                                   uint16_t phdr_index,
                                   const Elf64_Phdr *phdr,
                                   uint64_t elf_offset, uint64_t *offset,
                                   Error **errp)
{
    if (is_self) {
        return shadps4_self_range_offset(
            self_segments, self_segment_count, phdrs, phnum,
            le64_to_cpu(phdr->p_offset), le64_to_cpu(phdr->p_filesz),
            offset, errp);
    }
    if (!shadps4_add_valid(elf_offset, le64_to_cpu(phdr->p_offset),
                           offset)) {
        error_setg(errp, "ELF program header %u file offset overflows",
                   phdr_index);
        return false;
    }
    return true;
}

static bool shadps4_dynamic_range(uint64_t offset, uint64_t size,
                                  uint64_t data_size, const char *name,
                                  Error **errp)
{
    if (!shadps4_range_valid(offset, size, data_size)) {
        error_setg(errp, "%s exceeds PT_SCE_DYNLIBDATA", name);
        return false;
    }
    return true;
}

static bool shadps4_parse_dynamic(const uint8_t *dynamic,
                                  uint64_t dynamic_size,
                                  ShadPS4DynamicInfo *info, Error **errp)
{
    uint64_t offset;
    bool terminated = false;

    if (dynamic_size == 0 || dynamic_size % sizeof(Elf64_Dyn)) {
        error_setg(errp, "PT_DYNAMIC has an invalid size");
        return false;
    }

    memset(info, 0, sizeof(*info));
    for (offset = 0; offset < dynamic_size; offset += sizeof(Elf64_Dyn)) {
        const Elf64_Dyn *entry = (const Elf64_Dyn *)(dynamic + offset);
        uint64_t tag = le64_to_cpu(entry->d_tag);
        uint64_t value = le64_to_cpu(entry->d_un.d_val);

        if (tag == DT_NULL) {
            terminated = true;
            break;
        }
        switch (tag) {
        case DT_INIT:
            info->init = value;
            info->init_present = true;
            break;
        case DT_FINI:
            info->fini = value;
            info->fini_present = true;
            break;
        case SHADPS4_DT_SCE_STRTAB:
            info->strtab = value;
            break;
        case SHADPS4_DT_SCE_STRSZ:
            info->strsz = value;
            break;
        case SHADPS4_DT_SCE_SYMTAB:
            info->symtab = value;
            break;
        case SHADPS4_DT_SCE_SYMENT:
            info->syment = value;
            break;
        case SHADPS4_DT_SCE_SYMTABSZ:
            info->symtabsz = value;
            break;
        case SHADPS4_DT_SCE_RELA:
            info->rela = value;
            break;
        case SHADPS4_DT_SCE_RELASZ:
            info->relasz = value;
            break;
        case SHADPS4_DT_SCE_RELAENT:
            info->relaent = value;
            break;
        case SHADPS4_DT_SCE_JMPREL:
            info->jmprel = value;
            break;
        case SHADPS4_DT_SCE_PLTRELSZ:
            info->pltrelsz = value;
            break;
        case SHADPS4_DT_SCE_PLTREL:
            info->pltrel = value;
            break;
        default:
            break;
        }
    }
    if (!terminated) {
        error_setg(errp, "PT_DYNAMIC is not DT_NULL terminated");
        return false;
    }
    if ((info->symtabsz && info->syment != sizeof(Elf64_Sym)) ||
        (info->relasz && info->relaent != sizeof(Elf64_Rela)) ||
        (info->pltrelsz && info->pltrel != DT_RELA)) {
        error_setg(errp, "SCE dynamic table entry sizes are invalid");
        return false;
    }
    return true;
}

static bool shadps4_dynamic_identity(const uint8_t *dynamic,
                                     uint64_t dynamic_size,
                                     const uint8_t *strings,
                                     uint64_t string_size,
                                     const char *encoded_id,
                                     bool library, char **name,
                                     uint16_t *version, Error **errp)
{
    uint64_t import_tag = library ? SHADPS4_DT_SCE_IMPORT_LIB :
                                   SHADPS4_DT_SCE_NEEDED_MODULE;
    uint64_t export_tag = library ? SHADPS4_DT_SCE_EXPORT_LIB :
                                   SHADPS4_DT_SCE_MODULE_INFO;
    uint64_t offset;

    for (offset = 0; offset < dynamic_size; offset += sizeof(Elf64_Dyn)) {
        const Elf64_Dyn *entry = (const Elf64_Dyn *)(dynamic + offset);
        uint64_t tag = le64_to_cpu(entry->d_tag);
        uint64_t value = le64_to_cpu(entry->d_un.d_val);
        uint32_t name_offset;
        char current_id[4];

        if (tag == DT_NULL) {
            break;
        }
        if (tag != import_tag && tag != export_tag) {
            continue;
        }
        shadps4_encode_id(value >> 48, current_id);
        if (strcmp(current_id, encoded_id)) {
            continue;
        }
        name_offset = value;
        if (name_offset >= string_size ||
            !memchr(strings + name_offset, '\0', string_size - name_offset)) {
            error_setg(errp, "SCE dynamic identity name is outside the "
                       "string table");
            return false;
        }
        *name = g_strdup((const char *)strings + name_offset);
        *version = library ? (value >> 32) & 0xffff : 0;
        return true;
    }

    return true;
}

static bool shadps4_symbol_identity(const uint8_t *dynamic,
                                    uint64_t dynamic_size,
                                    const uint8_t *strings,
                                    uint64_t string_size,
                                    const char *encoded,
                                    ShadPS4DynamicSymbol *symbol,
                                    Error **errp)
{
    g_auto(GStrv) ids = g_strsplit(encoded, "#", 4);
    uint16_t ignored_version = 0;

    if (!ids[0] || !ids[1] || !ids[2] || ids[3]) {
        return true;
    }
    symbol->nid = g_strdup(ids[0]);
    if (!shadps4_dynamic_identity(dynamic, dynamic_size, strings,
                                  string_size, ids[1], true,
                                  &symbol->library,
                                  &symbol->library_version, errp) ||
        !shadps4_dynamic_identity(dynamic, dynamic_size, strings,
                                  string_size, ids[2], false,
                                  &symbol->module, &ignored_version, errp)) {
        return false;
    }
    return true;
}

static bool shadps4_relocation_target(const ShadPS4ImageInfo *info,
                                      uint64_t offset,
                                      uint64_t *physical_addr)
{
    uint64_t virtual_addr;
    uint16_t i;

    if (!shadps4_add_valid(info->virtual_base, offset, &virtual_addr)) {
        return false;
    }
    for (i = 0; i < info->segment_count; i++) {
        const ShadPS4ImageSegment *segment = &info->segments[i];
        uint64_t segment_offset;

        if (virtual_addr < segment->virtual_addr) {
            continue;
        }
        segment_offset = virtual_addr - segment->virtual_addr;
        if (segment_offset <= segment->memory_size &&
            sizeof(uint64_t) <= segment->memory_size - segment_offset) {
            *physical_addr = segment->physical_addr + segment_offset;
            return true;
        }
    }
    return false;
}

static bool shadps4_apply_relocations(const uint8_t *table,
                                      uint64_t table_size,
                                      const uint8_t *dynamic_data,
                                      uint64_t dynamic_data_size,
                                      const ShadPS4DynamicInfo *dynamic,
                                      AddressSpace *as,
                                      ShadPS4ImageInfo *image, Error **errp)
{
    const Elf64_Sym *symbols =
        (const Elf64_Sym *)(dynamic_data + dynamic->symtab);
    uint64_t symbol_count = dynamic->symtabsz / sizeof(*symbols);
    uint64_t offset;

    for (offset = 0; offset < table_size; offset += sizeof(Elf64_Rela)) {
        const Elf64_Rela *rela = (const Elf64_Rela *)(table + offset);
        uint64_t relocation_offset = le64_to_cpu(rela->r_offset);
        uint64_t relocation_info = le64_to_cpu(rela->r_info);
        int64_t addend = (int64_t)le64_to_cpu(rela->r_addend);
        uint32_t type = ELF64_R_TYPE(relocation_info);
        uint32_t symbol_index = ELF64_R_SYM(relocation_info);
        uint64_t physical_addr;
        uint64_t value;
        uint64_t value_le;

        image->relocation_count++;
        if (!shadps4_relocation_target(image, relocation_offset,
                                       &physical_addr)) {
            error_setg(errp, "relocation target is outside loaded segments");
            return false;
        }

        switch (type) {
        case R_X86_64_RELATIVE:
            if (!shadps4_add_signed_valid(image->virtual_base, addend,
                                          &value)) {
                error_setg(errp, "relative relocation value overflows");
                return false;
            }
            break;
        case SHADPS4_R_X86_64_DTPMOD64:
            value = image->tls_module_id;
            break;
        case R_X86_64_64:
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: {
            const Elf64_Sym *symbol;
            uint8_t binding;

            if (symbol_index >= symbol_count) {
                error_setg(errp, "relocation symbol index is out of range");
                return false;
            }
            symbol = &symbols[symbol_index];
            binding = ELF64_ST_BIND(symbol->st_info);
            if (le16_to_cpu(symbol->st_shndx) == SHN_UNDEF) {
                ShadPS4PendingRelocation *pending =
                    &image->pending_relocations[
                        image->pending_relocation_count++];

                pending->physical_addr = physical_addr;
                pending->addend = addend;
                pending->symbol_index = symbol_index;
                pending->type = type;
                pending->weak = binding == STB_WEAK;
                image->unresolved_relocation_count++;
                continue;
            }
            if (!shadps4_add_valid(image->virtual_base,
                                   le64_to_cpu(symbol->st_value), &value)) {
                error_setg(errp, "relocation symbol value overflows");
                return false;
            }
            if (type == R_X86_64_64) {
                if (!shadps4_add_signed_valid(value, addend, &value)) {
                    error_setg(errp, "absolute relocation value overflows");
                    return false;
                }
            }
            break;
        }
        default:
            error_setg(errp, "unsupported x86_64 relocation type %u", type);
            return false;
        }

        value_le = cpu_to_le64(value);
        if (g_getenv("SHADPS4_LINK_TRACE") &&
            relocation_offset == 0x5ff6d0) {
            const ShadPS4DynamicSymbol *resolved_symbol =
                symbol_index < image->symbol_count ?
                &image->symbols[symbol_index] : NULL;

            info_report("shadPS4 relocation trace: image='%s' "
                        "offset=%#" PRIx64 " type=%u symbol=%u NID='%s' "
                        "defined=%d value=%#" PRIx64,
                        image->name ?: "<anonymous>", relocation_offset,
                        type, symbol_index,
                        resolved_symbol && resolved_symbol->nid ?
                            resolved_symbol->nid : "",
                        resolved_symbol ? resolved_symbol->defined : 0,
                        value);
        }
        if (address_space_write(as, physical_addr, MEMTXATTRS_UNSPECIFIED,
                                &value_le, sizeof(value_le)) != MEMTX_OK) {
            error_setg(errp, "failed to write relocated value");
            return false;
        }
        image->applied_relocation_count++;
    }
    return true;
}

static bool shadps4_process_dynamic(const uint8_t *dynamic,
                                    uint64_t dynamic_size,
                                    const uint8_t *dynamic_data,
                                    uint64_t dynamic_data_size,
                                    AddressSpace *as,
                                    ShadPS4ImageInfo *image, Error **errp)
{
    ShadPS4DynamicInfo info;
    const Elf64_Sym *symbols;
    uint64_t symbol_count;
    uint64_t i;

    if (!shadps4_parse_dynamic(dynamic, dynamic_size, &info, errp)) {
        return false;
    }
    image->init_present = info.init_present;
    image->fini_present = info.fini_present;
    if ((info.init_present &&
         !shadps4_add_valid(image->virtual_base, info.init, &image->init)) ||
        (info.fini_present &&
         !shadps4_add_valid(image->virtual_base, info.fini, &image->fini))) {
        error_setg(errp, "module initializer address overflows");
        return false;
    }
    if (!shadps4_dynamic_range(info.strtab, info.strsz, dynamic_data_size,
                               "SCE string table", errp) ||
        !shadps4_dynamic_range(info.symtab, info.symtabsz, dynamic_data_size,
                               "SCE symbol table", errp) ||
        !shadps4_dynamic_range(info.rela, info.relasz, dynamic_data_size,
                               "SCE relocation table", errp) ||
        !shadps4_dynamic_range(info.jmprel, info.pltrelsz,
                               dynamic_data_size,
                               "SCE PLT relocation table", errp)) {
        return false;
    }
    if (info.symtabsz % sizeof(Elf64_Sym) ||
        info.relasz % sizeof(Elf64_Rela) ||
        info.pltrelsz % sizeof(Elf64_Rela)) {
        error_setg(errp, "SCE dynamic table sizes are not entry aligned");
        return false;
    }

    symbols = (const Elf64_Sym *)(dynamic_data + info.symtab);
    symbol_count = info.symtabsz / sizeof(*symbols);
    image->symbol_count = symbol_count;
    image->symbols = g_new0(ShadPS4DynamicSymbol, symbol_count);
    image->pending_relocations = g_new0(
        ShadPS4PendingRelocation,
        (info.relasz + info.pltrelsz) / sizeof(Elf64_Rela));
    for (i = 0; i < symbol_count; i++) {
        uint32_t name = le32_to_cpu(symbols[i].st_name);
        ShadPS4DynamicSymbol *symbol = &image->symbols[i];
        const char *encoded_name;

        if (name >= info.strsz ||
            !memchr(dynamic_data + info.strtab + name, '\0',
                    info.strsz - name)) {
            error_setg(errp, "SCE symbol name is outside the string table");
            return false;
        }
        encoded_name = (const char *)dynamic_data + info.strtab + name;
        symbol->value = le64_to_cpu(symbols[i].st_value);
        symbol->type = ELF64_ST_TYPE(symbols[i].st_info);
        symbol->binding = ELF64_ST_BIND(symbols[i].st_info);
        symbol->defined = le16_to_cpu(symbols[i].st_shndx) != SHN_UNDEF;
        if (!shadps4_symbol_identity(dynamic, dynamic_size,
                                     dynamic_data + info.strtab, info.strsz,
                                     encoded_name, symbol, errp)) {
            return false;
        }
        if (!symbol->defined) {
            image->imported_symbol_count++;
        } else {
            image->exported_symbol_count++;
        }
    }

    if (!shadps4_apply_relocations(dynamic_data + info.rela, info.relasz,
                                    dynamic_data, dynamic_data_size, &info,
                                    as, image, errp) ||
        !shadps4_apply_relocations(dynamic_data + info.jmprel,
                                    info.pltrelsz, dynamic_data,
                                    dynamic_data_size, &info, as, image,
                                    errp)) {
        return false;
    }
    return true;
}

bool shadps4_load_module(const char *filename, AddressSpace *as,
                         uint64_t virtual_base, uint64_t physical_base,
                         uint64_t physical_limit, uint32_t tls_module_id,
                         ShadPS4ImageInfo *info, Error **errp)
{
    ShadPS4SelfHeader self_header;
    g_autofree ShadPS4SelfSegment *self_segments = NULL;
    g_autofree Elf64_Phdr *phdrs = NULL;
    g_autofree uint8_t *dynamic = NULL;
    g_autofree uint8_t *dynamic_data = NULL;
    Elf64_Ehdr ehdr;
    uint64_t elf_offset = 0;
    uint64_t image_size = 0;
    uint64_t entry_offset;
    uint64_t file_length;
    bool entry_is_executable = false;
    bool tls_found = false;
    bool proc_param_found = false;
    bool eh_frame_hdr_found = false;
    uint16_t phnum;
    uint16_t i;
    int dynamic_index = -1;
    int dynamic_data_index = -1;
    ShadPS4HostFile file;
    bool success = false;

    memset(info, 0, sizeof(*info));
    info->name = g_path_get_basename(filename);
    info->tls_module_id = tls_module_id;
    if (shadps4_host_file_open(&file, filename, errp) < 0) {
        return false;
    }

    file_length = shadps4_host_file_seek(&file, 0, SEEK_END);
    if (file_length == (uint64_t)-1) {
        error_setg_errno(errp, errno, "failed to determine image size");
        goto out;
    }
    if (!shadps4_range_valid(0, sizeof(self_header), file_length) ||
        !shadps4_read_exact(&file, 0, &self_header, sizeof(self_header), errp)) {
        goto out;
    }

    if (le32_to_cpu(self_header.magic) == SHADPS4_SELF_MAGIC) {
        uint16_t self_segment_count = le16_to_cpu(self_header.segment_count);
        uint64_t segment_table_size;

        if (self_header.version != 0 || self_header.mode != 1 ||
            self_header.endian != 1 || self_header.attributes != 0x12 ||
            self_header.category != 1 || self_header.program_type != 1) {
            error_setg(errp, "unsupported SELF container header");
            goto out;
        }
        if (self_segment_count == 0 || self_segment_count > 4096) {
            error_setg(errp, "invalid SELF segment count %u",
                       self_segment_count);
            goto out;
        }
        segment_table_size = self_segment_count * sizeof(*self_segments);
        if (!shadps4_range_valid(sizeof(self_header), segment_table_size,
                                 file_length)) {
            error_setg(errp, "SELF segment table exceeds image size");
            goto out;
        }
        self_segments = g_malloc(segment_table_size);
        if (!shadps4_read_exact(&file, sizeof(self_header), self_segments,
                                segment_table_size, errp)) {
            goto out;
        }
        elf_offset = sizeof(self_header) + segment_table_size;
        info->is_self = true;
    }

    if (!shadps4_range_valid(elf_offset, sizeof(ehdr), file_length) ||
        !shadps4_read_exact(&file, elf_offset, &ehdr, sizeof(ehdr), errp)) {
        goto out;
    }
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr.e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr.e_ident[EI_VERSION] != EV_CURRENT) {
        error_setg(errp, "image is not a supported ELF64 little-endian file");
        goto out;
    }
    if (ehdr.e_ident[EI_OSABI] != SHADPS4_ELF_OSABI_FREEBSD ||
        ehdr.e_ident[SHADPS4_EI_ABIVERSION] != 0) {
        error_setg(errp, "ELF does not use the Orbis FreeBSD ABI");
        goto out;
    }
    if (le16_to_cpu(ehdr.e_machine) != EM_X86_64 ||
        !shadps4_elf_type_valid(le16_to_cpu(ehdr.e_type))) {
        error_setg(errp, "ELF has an unsupported machine or SCE type");
        goto out;
    }
    if (le16_to_cpu(ehdr.e_ehsize) != sizeof(ehdr) ||
        le16_to_cpu(ehdr.e_phentsize) != sizeof(*phdrs)) {
        error_setg(errp, "ELF header sizes are invalid");
        goto out;
    }

    phnum = le16_to_cpu(ehdr.e_phnum);
    if (phnum == 0 || phnum > 4096) {
        error_setg(errp, "invalid ELF program header count %u", phnum);
        goto out;
    }
    uint64_t phdr_offset;

    if (!shadps4_add_valid(elf_offset, le64_to_cpu(ehdr.e_phoff),
                           &phdr_offset) ||
        !shadps4_range_valid(phdr_offset, phnum * sizeof(*phdrs),
                             file_length)) {
        error_setg(errp, "ELF program header table exceeds image size");
        goto out;
    }
    phdrs = g_new(Elf64_Phdr, phnum);
    if (!shadps4_read_exact(&file, phdr_offset, phdrs,
                            phnum * sizeof(*phdrs), errp)) {
        goto out;
    }

    info->elf_type = le16_to_cpu(ehdr.e_type);
    info->virtual_base = virtual_base;
    info->physical_base = physical_base;
    entry_offset = le64_to_cpu(ehdr.e_entry);

    for (i = 0; i < phnum; i++) {
        uint32_t type = le32_to_cpu(phdrs[i].p_type);
        uint64_t vaddr = le64_to_cpu(phdrs[i].p_vaddr);
        uint64_t filesz = le64_to_cpu(phdrs[i].p_filesz);
        uint64_t memsz = le64_to_cpu(phdrs[i].p_memsz);
        uint64_t source_offset;
        uint64_t physical_addr;

        if (type == PT_DYNAMIC) {
            if (dynamic_index >= 0) {
                error_setg(errp, "ELF has multiple PT_DYNAMIC headers");
                goto out;
            }
            dynamic_index = i;
        } else if (type == SHADPS4_PT_SCE_DYNLIBDATA) {
            if (dynamic_data_index >= 0) {
                error_setg(errp,
                           "ELF has multiple PT_SCE_DYNLIBDATA headers");
                goto out;
            }
            dynamic_data_index = i;
        }

        if (type == SHADPS4_PT_SCE_PROCPARAM) {
            if (proc_param_found || memsz < 24 ||
                !shadps4_add_valid(info->virtual_base, vaddr,
                                    &info->proc_param_addr)) {
                error_setg(errp, "ELF has an invalid PT_SCE_PROCPARAM");
                goto out;
            }
            info->proc_param_size = memsz;
            proc_param_found = true;
        }

        if (type == SHADPS4_PT_GNU_EH_FRAME) {
            if (eh_frame_hdr_found || filesz > UINT32_MAX ||
                !shadps4_add_valid(info->virtual_base, vaddr,
                                    &info->eh_frame_hdr_addr)) {
                error_setg(errp, "ELF has an invalid PT_GNU_EH_FRAME");
                goto out;
            }
            info->eh_frame_hdr_size = filesz;
            eh_frame_hdr_found = true;
        }

        if (type == SHADPS4_PT_TLS) {
            uint64_t align = le64_to_cpu(phdrs[i].p_align);

            if (tls_found) {
                error_setg(errp, "ELF has multiple PT_TLS headers");
                goto out;
            }
            if (filesz > memsz || (align && !is_power_of_2(align))) {
                error_setg(errp,
                           "ELF TLS header has an invalid size or alignment");
                goto out;
            }
            if (!shadps4_add_valid(info->virtual_base, vaddr,
                                    &info->tls_addr)) {
                error_setg(errp, "ELF TLS address overflows");
                goto out;
            }
            info->tls_file_size = filesz;
            info->tls_memory_size = memsz;
            info->tls_align = align;
            info->tls_present = true;
            tls_found = true;
        }
        if (type != PT_LOAD && type != SHADPS4_PT_SCE_RELRO) {
            continue;
        }
        if (memsz == 0 || filesz > memsz ||
            !shadps4_add_valid(physical_base, vaddr, &physical_addr) ||
            !shadps4_add_valid(vaddr, memsz, &source_offset)) {
            error_setg(errp, "invalid ELF segment %u address or size", i);
            goto out;
        }
        if (!shadps4_range_valid(physical_addr, memsz, physical_limit)) {
            error_setg(errp, "ELF segment %u does not fit in guest RAM", i);
            goto out;
        }
        if (info->segment_count == SHADPS4_MAX_SEGMENTS) {
            error_setg(errp, "ELF has more than %u loadable segments",
                       SHADPS4_MAX_SEGMENTS);
            goto out;
        }
        for (uint16_t j = 0; j < info->segment_count; j++) {
            const ShadPS4ImageSegment *other = &info->segments[j];

            if (physical_addr < other->physical_addr + other->memory_size &&
                other->physical_addr < physical_addr + memsz) {
                error_setg(errp, "ELF loadable segments overlap");
                goto out;
            }
        }

        if (info->is_self) {
            if (!shadps4_self_segment_offset(
                    self_segments, le16_to_cpu(self_header.segment_count), i,
                    filesz, &source_offset, errp)) {
                goto out;
            }
        } else {
            if (!shadps4_add_valid(elf_offset,
                                   le64_to_cpu(phdrs[i].p_offset),
                                   &source_offset)) {
                error_setg(errp, "ELF segment %u file offset overflows", i);
                goto out;
            }
        }
        if (!shadps4_range_valid(source_offset, filesz, file_length)) {
            error_setg(errp, "ELF segment %u exceeds image size", i);
            goto out;
        }
        if (!shadps4_copy_segment(&file, source_offset, filesz, as,
                                  physical_addr, errp)) {
            goto out;
        }
        if (memsz > filesz &&
            address_space_set(as, physical_addr + filesz, 0, memsz - filesz,
                              MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            error_setg(errp, "failed to clear ELF segment %u BSS", i);
            goto out;
        }

        image_size = MAX(image_size, vaddr + memsz);
        if ((le32_to_cpu(phdrs[i].p_flags) & PF_X) &&
            entry_offset >= vaddr && entry_offset < vaddr + memsz) {
            entry_is_executable = true;
        }
        ShadPS4ImageSegment *segment =
            &info->segments[info->segment_count++];

        if (!shadps4_add_valid(info->virtual_base, vaddr,
                               &segment->virtual_addr)) {
            error_setg(errp, "ELF segment %u virtual address overflows", i);
            goto out;
        }
        segment->physical_addr = physical_addr;
        segment->file_size = filesz;
        segment->memory_size = memsz;
        segment->flags = le32_to_cpu(phdrs[i].p_flags);
    }

    if (info->segment_count == 0) {
        error_setg(errp, "ELF has no loadable segments");
        goto out;
    }
    if (eh_frame_hdr_found) {
        uint8_t header[12];

        for (i = 0; i < info->segment_count; i++) {
            const ShadPS4ImageSegment *segment = &info->segments[i];
            uint64_t offset;

            if (info->eh_frame_hdr_addr < segment->virtual_addr) {
                continue;
            }
            offset = info->eh_frame_hdr_addr - segment->virtual_addr;
            if (offset > segment->file_size ||
                sizeof(header) > segment->file_size - offset) {
                continue;
            }
            if (address_space_read(as, segment->physical_addr + offset,
                                   MEMTXATTRS_UNSPECIFIED, header,
                                   sizeof(header)) != MEMTX_OK) {
                break;
            }
            if (header[0] == 1 && header[1] == 0x1b) {
                int32_t relative = ldl_le_p(header + 4);
                uint64_t field = info->eh_frame_hdr_addr + 4;

                if (shadps4_add_signed_valid(field, relative,
                                              &info->eh_frame_addr) &&
                    info->eh_frame_addr >= segment->virtual_addr &&
                    info->eh_frame_addr - segment->virtual_addr <=
                        segment->memory_size) {
                    uint64_t remaining = segment->memory_size -
                        (info->eh_frame_addr - segment->virtual_addr);

                    info->eh_frame_size = MIN(remaining, UINT32_MAX);
                } else {
                    info->eh_frame_addr = 0;
                }
            }
            break;
        }
    }
    if (proc_param_found) {
        bool mapped = false;

        for (i = 0; i < info->segment_count; i++) {
            const ShadPS4ImageSegment *segment = &info->segments[i];

            if (info->proc_param_addr >= segment->virtual_addr &&
                info->proc_param_addr - segment->virtual_addr <=
                    segment->file_size &&
                24 <= segment->file_size -
                      (info->proc_param_addr - segment->virtual_addr)) {
                mapped = true;
                break;
            }
        }
        if (!mapped) {
            error_setg(errp, "PT_SCE_PROCPARAM is outside loaded file data");
            goto out;
        }
    }
    if (!entry_is_executable ||
        !shadps4_add_valid(info->virtual_base, entry_offset, &info->entry) ||
        !shadps4_add_valid(physical_base, entry_offset,
                           &info->physical_entry)) {
        error_setg(errp, "ELF entry point is not in an executable segment");
        goto out;
    }

    if (!tls_found) {
        info->tls_module_id = 0;
    }

    if (dynamic_index >= 0 || dynamic_data_index >= 0) {
        uint64_t dynamic_size;
        uint64_t dynamic_data_size;
        uint64_t dynamic_offset;
        uint64_t dynamic_data_offset;

        if (dynamic_index < 0 || dynamic_data_index < 0) {
            error_setg(errp, "ELF must provide both PT_DYNAMIC and "
                       "PT_SCE_DYNLIBDATA");
            goto out;
        }
        dynamic_size = le64_to_cpu(phdrs[dynamic_index].p_filesz);
        dynamic_data_size =
            le64_to_cpu(phdrs[dynamic_data_index].p_filesz);
        if (dynamic_size > 64 * MiB || dynamic_data_size > 64 * MiB) {
            error_setg(errp, "ELF dynamic metadata is unreasonably large");
            goto out;
        }
        if (!shadps4_program_offset(
                info->is_self, self_segments,
                le16_to_cpu(self_header.segment_count), phdrs, phnum,
                dynamic_index,
                &phdrs[dynamic_index], elf_offset, &dynamic_offset, errp) ||
            !shadps4_program_offset(
                info->is_self, self_segments,
                le16_to_cpu(self_header.segment_count), phdrs, phnum,
                dynamic_data_index,
                &phdrs[dynamic_data_index], elf_offset,
                &dynamic_data_offset, errp) ||
            !shadps4_range_valid(dynamic_offset, dynamic_size, file_length) ||
            !shadps4_range_valid(dynamic_data_offset, dynamic_data_size,
                                  file_length)) {
            if (!errp || !*errp) {
                error_setg(errp, "ELF dynamic metadata exceeds image size");
            }
            goto out;
        }
        dynamic = g_malloc(dynamic_size);
        dynamic_data = g_malloc(dynamic_data_size);
        if (!shadps4_read_exact(&file, dynamic_offset, dynamic, dynamic_size,
                                errp) ||
            !shadps4_read_exact(&file, dynamic_data_offset, dynamic_data,
                                dynamic_data_size, errp) ||
            !shadps4_process_dynamic(dynamic, dynamic_size, dynamic_data,
                                     dynamic_data_size, as, info, errp)) {
            goto out;
        }
    }
    info->image_size = image_size;
    success = true;

out:
    shadps4_host_file_close(&file);
    if (!success) {
        shadps4_image_cleanup(info);
    }
    return success;
}

static bool shadps4_symbols_match(const ShadPS4DynamicSymbol *import,
                                  const ShadPS4DynamicSymbol *export)
{
    return !import->defined && export->defined && import->nid &&
           import->library && import->module && export->nid &&
           export->library && export->module &&
           import->type == export->type &&
           import->library_version == export->library_version &&
           !strcmp(import->nid, export->nid) &&
           !strcmp(import->library, export->library) &&
           !strcmp(import->module, export->module);
}

static bool shadps4_prefer_hle_heap_provider(
    const ShadPS4ImageInfo *consumer, const ShadPS4DynamicSymbol *import)
{
    static const char *const heap_nids[] = {
        "gQX+4GDQjpM", /* malloc */
        "tIhsqj0qsFE", /* free */
        "2X5agFjKxMc", /* calloc */
        "Y7aJ1uydPMo", /* realloc */
        "Ujf3KzMvRmI", /* memalign */
        "cVSk9y8URbc", /* posix_memalign */
        "NDcSfcYZRC8", /* malloc_usable_size */
    };
    size_t i;

    /*
     * The PS2 compiler runs as a separately bootstrapped guest process and
     * cannot use the title's libc allocator instance.  Other images must keep
     * their allocator family inside their own libc; mixing an HLE allocation
     * with a libc free corrupts libc's heap metadata.
     */
    if (!consumer || g_strcmp0(consumer->name, "ps2-emu-compiler.self") ||
        !import->nid ||
        (g_strcmp0(import->module, "libc") &&
         g_strcmp0(import->module, "libSceLibcInternal"))) {
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(heap_nids); i++) {
        if (!strcmp(import->nid, heap_nids[i])) {
            return true;
        }
    }
    return false;
}

bool shadps4_link_modules(AddressSpace *as, ShadPS4ImageInfo **images,
                          uint32_t image_count, Error **errp)
{
    uint32_t image_index;

    for (image_index = 0; image_index < image_count; image_index++) {
        ShadPS4ImageInfo *image = images[image_index];
        uint32_t relocation_index;

        for (relocation_index = 0;
             relocation_index < image->pending_relocation_count;
             relocation_index++) {
            ShadPS4PendingRelocation *relocation =
                &image->pending_relocations[relocation_index];
            const ShadPS4DynamicSymbol *import;
            uint64_t value = 0;
            uint64_t value_le;
            bool resolved = false;
            bool prefer_hle;
            uint32_t provider_order;
            uint32_t provider_index;

            if (relocation->applied) {
                continue;
            }

            if (relocation->symbol_index >= image->symbol_count) {
                error_setg(errp, "pending relocation symbol is out of range");
                return false;
            }
            import = &image->symbols[relocation->symbol_index];
            prefer_hle = shadps4_prefer_hle_heap_provider(image, import);
            for (provider_order = 0;
                 provider_order < image_count && !resolved;
                 provider_order++) {
                /* shadps4.c appends the synthetic HLE image last. */
                provider_index = prefer_hle ?
                    (provider_order ? provider_order - 1 : image_count - 1) :
                    provider_order;
                const ShadPS4ImageInfo *provider = images[provider_index];
                uint32_t symbol_index;

                for (symbol_index = 0;
                     symbol_index < provider->symbol_count; symbol_index++) {
                    const ShadPS4DynamicSymbol *export =
                        &provider->symbols[symbol_index];

                    if (!shadps4_symbols_match(import, export) ||
                        !shadps4_add_valid(provider->virtual_base,
                                           export->value, &value)) {
                        continue;
                    }
                    const char *link_trace = g_getenv("SHADPS4_LINK_TRACE");

                    if (link_trace &&
                        (!strcmp(link_trace, "all") ||
                         strstr(import->library ?: "", "Ipmi") ||
                         strstr(import->module ?: "", "Ipmi"))) {
                        info_report("shadPS4 link: image='%s' NID='%s' "
                                    "library='%s' module='%s' provider='%s' "
                                    "offset=%#" PRIx64 " target=%#" PRIx64,
                                    image->name ?: "<anonymous>",
                                    import->nid ?: "", import->library ?: "",
                                    import->module ?: "",
                                    provider->name ?: "<HLE>",
                                    relocation->physical_addr -
                                        image->physical_base,
                                    value);
                    }
                    resolved = true;
                    break;
                }
            }
            if (!resolved) {
                if (!relocation->weak) {
                    continue;
                }
                value = 0;
                if (g_getenv("SHADPS4_LINK_TRACE")) {
                    info_report("shadPS4 weak import unresolved: image='%s' "
                                "NID='%s' library='%s' module='%s' "
                                "offset=%#" PRIx64,
                                image->name ?: "<anonymous>",
                                import->nid ?: "", import->library ?: "",
                                import->module ?: "",
                                relocation->physical_addr -
                                    image->physical_base);
                }
            }
            if (relocation->type == R_X86_64_64 &&
                !shadps4_add_signed_valid(value, relocation->addend,
                                          &value)) {
                error_setg(errp, "linked relocation value overflows");
                return false;
            }
            value_le = cpu_to_le64(value);
            if (address_space_write(as, relocation->physical_addr,
                                    MEMTXATTRS_UNSPECIFIED, &value_le,
                                    sizeof(value_le)) != MEMTX_OK) {
                error_setg(errp, "failed to write linked relocation");
                return false;
            }
            relocation->applied = true;
            image->applied_relocation_count++;
            image->unresolved_relocation_count--;
        }
    }
    return true;
}

void shadps4_image_cleanup(ShadPS4ImageInfo *info)
{
    uint32_t i;

    for (i = 0; i < info->symbol_count; i++) {
        g_free(info->symbols[i].nid);
        g_free(info->symbols[i].library);
        g_free(info->symbols[i].module);
    }
    g_clear_pointer(&info->name, g_free);
    g_clear_pointer(&info->symbols, g_free);
    g_clear_pointer(&info->pending_relocations, g_free);
    info->symbol_count = 0;
    info->pending_relocation_count = 0;
}

bool shadps4_load_image(const char *filename, AddressSpace *as,
                        uint64_t physical_base, uint64_t physical_limit,
                        ShadPS4ImageInfo *info, Error **errp)
{
    return shadps4_load_module(filename, as, SHADPS4_IMAGE_VIRT_BASE,
                               physical_base, physical_limit, 1, info, errp);
}
