/* Portable service helpers for the second structural HLE batch. */

#define SHADPS4_FIBER_ERROR_NULL UINT32_C(0x80590001)
#define SHADPS4_FIBER_ERROR_ALIGNMENT UINT32_C(0x80590002)
#define SHADPS4_FIBER_ERROR_RANGE UINT32_C(0x80590003)
#define SHADPS4_FIBER_ERROR_INVALID UINT32_C(0x80590004)
#define SHADPS4_FIBER_ERROR_PERMISSION UINT32_C(0x80590005)
#define SHADPS4_FIBER_ERROR_STATE UINT32_C(0x80590006)

#define SHADPS4_SAVE_ERROR_PARAMETER UINT32_C(0x809f0000)
#define SHADPS4_SAVE_ERROR_NOT_INITIALIZED UINT32_C(0x809f0001)
#define SHADPS4_SAVE_ERROR_NOT_FOUND UINT32_C(0x809f0008)
#define SHADPS4_SAVE_ERROR_INTERNAL UINT32_C(0x809f000b)

static uint64_t shadps4_hle_fiber_initialize(ShadPS4HLEState *hle,
                                             CPUState *cs,
                                             uint64_t fiber_address,
                                             uint64_t name_address,
                                             uint64_t entry,
                                             uint64_t initial_argument,
                                             uint64_t context_address,
                                             uint64_t context_size,
                                             bool internal)
{
    uint8_t fiber[112] = { 0 };
    char name[32] = { 0 };
    uint64_t option_address;
    uint64_t flags = 0;
    uint64_t build_version;
    uint32_t option_magic;
    uint64_t signature = cpu_to_le64(UINT64_C(0x7149f2ca7149f2ca));

    if (!shadps4_hle_argument(cs, 6, &option_address) ||
        !shadps4_hle_argument(cs, 7, internal ? &flags : &build_version) ||
        (internal && !shadps4_hle_argument(cs, 8, &build_version))) {
        return SHADPS4_FIBER_ERROR_INVALID;
    }
    if (!fiber_address || !name_address || !entry) {
        return SHADPS4_FIBER_ERROR_NULL;
    }
    if ((fiber_address & 7) || (context_address & 15) ||
        (option_address & 7)) {
        return SHADPS4_FIBER_ERROR_ALIGNMENT;
    }
    if ((context_size && context_size < 512) || context_size > 64 * MiB) {
        return SHADPS4_FIBER_ERROR_RANGE;
    }
    if ((context_size & 15) || (!context_address != !context_size) ||
        context_address > UINT64_MAX - context_size) {
        return SHADPS4_FIBER_ERROR_INVALID;
    }
    if (option_address &&
        (!shadps4_guest_rw(cs, option_address, &option_magic,
                           sizeof(option_magic), false) ||
         le32_to_cpu(option_magic) != UINT32_C(0xbb40e64d))) {
        return SHADPS4_FIBER_ERROR_INVALID;
    }
    if (!shadps4_guest_read_string(cs, name_address, name, sizeof(name))) {
        return SHADPS4_FIBER_ERROR_NULL;
    }
    stl_le_p(fiber, UINT32_C(0xdef1649c));
    stl_le_p(fiber + 4, 2);
    stq_le_p(fiber + 8, entry);
    stq_le_p(fiber + 16, initial_argument);
    stq_le_p(fiber + 24, context_address);
    stq_le_p(fiber + 32, context_size);
    memcpy(fiber + 40, name, sizeof(name));
    if (build_version >= UINT32_C(0x03500000)) {
        flags |= 0x100;
    }
    if (hle->fiber_context_size_check) {
        flags |= 0x10;
    }
    stl_le_p(fiber + 80, flags);
    stq_le_p(fiber + 88, context_address);
    stq_le_p(fiber + 96, context_address + context_size);
    stl_le_p(fiber + 104, UINT32_C(0xb37592a0));
    if (context_address &&
        !shadps4_guest_rw(cs, context_address, &signature,
                          sizeof(signature), true)) {
        return SHADPS4_FIBER_ERROR_INVALID;
    }
    return shadps4_guest_rw(cs, fiber_address, fiber, sizeof(fiber), true) ?
           0 : SHADPS4_FIBER_ERROR_NULL;
}

static bool shadps4_hle_fiber_read(CPUState *cs, uint64_t address,
                                   uint8_t fiber[112])
{
    return address && !(address & 7) &&
           shadps4_guest_rw(cs, address, fiber, 112, false) &&
           ldl_le_p(fiber) == UINT32_C(0xdef1649c) &&
           ldl_le_p(fiber + 104) == UINT32_C(0xb37592a0);
}

static uint64_t shadps4_hle_fiber_get_info(CPUState *cs,
                                           uint64_t fiber_address,
                                           uint64_t info_address)
{
    uint8_t fiber[112];
    uint8_t info[128] = { 0 };
    uint64_t size;

    if (!fiber_address || !info_address) {
        return SHADPS4_FIBER_ERROR_NULL;
    }
    if ((fiber_address & 7) || (info_address & 7)) {
        return SHADPS4_FIBER_ERROR_ALIGNMENT;
    }
    if (!shadps4_hle_fiber_read(cs, fiber_address, fiber) ||
        !shadps4_guest_rw(cs, info_address, &size, sizeof(size), false) ||
        le64_to_cpu(size) != sizeof(info)) {
        return SHADPS4_FIBER_ERROR_INVALID;
    }
    stq_le_p(info, sizeof(info));
    memcpy(info + 8, fiber + 8, 64);
    stq_le_p(info + 72, UINT64_MAX);
    return shadps4_guest_rw(cs, info_address, info, sizeof(info), true) ?
           0 : SHADPS4_FIBER_ERROR_NULL;
}

static int shadps4_hle_storage_copy_file(const char *source,
                                         const char *destination,
                                         uint64_t *total)
{
    g_autofree uint8_t *buffer = g_malloc(64 * KiB);
    int64_t source_handle = -1;
    int64_t destination_handle = -1;
    int ret;

    ret = qemu_host_storage_open(source, 0, 0, &source_handle);
    if (ret < 0) {
        return ret;
    }
    ret = qemu_host_storage_open(destination,
                                 SHADPS4_O_WRONLY | SHADPS4_O_CREAT |
                                 SHADPS4_O_TRUNC, 0666, &destination_handle);
    if (ret < 0) {
        qemu_host_storage_close(source_handle);
        return ret;
    }
    for (;;) {
        int64_t size = qemu_host_storage_read(source_handle, buffer, 64 * KiB);

        if (size < 0 || size > 64 * KiB || *total > 512 * MiB ||
            (uint64_t)size > 512 * MiB - *total ||
            (size && qemu_host_storage_write(destination_handle,
                                              buffer, size) != size)) {
            ret = -EIO;
            break;
        }
        if (!size) {
            ret = qemu_host_storage_flush(destination_handle);
            break;
        }
        *total += size;
    }
    qemu_host_storage_close(destination_handle);
    qemu_host_storage_close(source_handle);
    return ret;
}

static int shadps4_hle_storage_copy_tree(const char *source,
                                         const char *destination,
                                         unsigned depth, uint64_t *total)
{
    int64_t directory;
    int ret;

    if (depth > 16) {
        return -ELOOP;
    }
    ret = qemu_host_storage_mkdir(destination, 0777);
    if (ret < 0 && ret != -EEXIST) {
        return ret;
    }
    ret = qemu_host_storage_open(source, QEMU_HOST_STORAGE_OPEN_DIRECTORY,
                                 0, &directory);
    if (ret < 0) {
        return ret;
    }
    for (;;) {
        QemuHostStorageStat stat;
        char name[256];
        char child_source[512];
        char child_destination[512];

        ret = qemu_host_storage_readdir(directory, name, sizeof(name), &stat);
        if (ret <= 0) {
            ret = ret < 0 ? ret : 0;
            break;
        }
        name[sizeof(name) - 1] = 0;
        if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") ||
            strchr(name, '/') || strchr(name, '\\') ||
            g_snprintf(child_source, sizeof(child_source), "%s/%s",
                       source, name) >= sizeof(child_source) ||
            g_snprintf(child_destination, sizeof(child_destination), "%s/%s",
                       destination, name) >= sizeof(child_destination)) {
            ret = -EINVAL;
            break;
        }
        ret = stat.type == 4 ?
              shadps4_hle_storage_copy_tree(child_source, child_destination,
                                            depth + 1, total) :
              stat.type == 8 ?
              shadps4_hle_storage_copy_file(child_source, child_destination,
                                            total) : -EINVAL;
        if (ret < 0) {
            break;
        }
    }
    qemu_host_storage_close(directory);
    return ret;
}

static bool shadps4_hle_save_request(ShadPS4HLEState *hle, CPUState *cs,
                                     uint64_t request_address,
                                     char *directory, size_t directory_size)
{
    uint8_t request[24];
    uint64_t title_address;
    uint64_t directory_address;
    char title[16];

    if (!request_address ||
        !shadps4_guest_rw(cs, request_address, request, sizeof(request),
                          false)) {
        return false;
    }
    title_address = ldq_le_p(request + 8);
    directory_address = ldq_le_p(request + 16);
    if (title_address &&
        (!shadps4_guest_read_string(cs, title_address, title, sizeof(title)) ||
         strcmp(title, hle->title_id))) {
        return false;
    }
    return shadps4_guest_read_string(cs, directory_address, directory,
                                     directory_size) && directory[0] &&
           !strchr(directory, '/') && !strchr(directory, '\\') &&
           strcmp(directory, ".") && strcmp(directory, "..");
}

static uint64_t shadps4_hle_save_backup_operation(ShadPS4HLEState *hle,
                                                  CPUState *cs,
                                                  uint64_t request_address,
                                                  uint32_t operation)
{
    char directory[33];
    char source[256];
    char destination[256];
    char temporary[272];
    char previous[272];
    QemuHostStorageStat stat;
    uint64_t total = 0;
    int ret;

    if (!hle->save_data_initialized) {
        return SHADPS4_SAVE_ERROR_NOT_INITIALIZED;
    }
    if (!shadps4_hle_save_request(hle, cs, request_address, directory,
                                  sizeof(directory))) {
        return SHADPS4_SAVE_ERROR_PARAMETER;
    }
    g_snprintf(source, sizeof(source), "/titles/%s/savedata/%s",
               hle->title_id, directory);
    g_snprintf(destination, sizeof(destination),
               "/titles/%s/savedata-backup/%s", hle->title_id, directory);
    if (operation == SHADPS4_HLE_SAVE_DATA_CHECK_BACKUP) {
        return qemu_host_storage_stat(destination, &stat) < 0 ?
               SHADPS4_SAVE_ERROR_NOT_FOUND : 0;
    }
    if (operation == SHADPS4_HLE_SAVE_DATA_RESTORE_BACKUP) {
        char swap[sizeof(source)];

        memcpy(swap, source, sizeof(source));
        memcpy(source, destination, sizeof(source));
        memcpy(destination, swap, sizeof(destination));
    } else {
        char parent[192];

        g_snprintf(parent, sizeof(parent), "/titles/%s/savedata-backup",
                   hle->title_id);
        ret = qemu_host_storage_mkdir(parent, 0777);
        if (ret < 0 && ret != -EEXIST) {
            return SHADPS4_SAVE_ERROR_INTERNAL;
        }
    }
    if (qemu_host_storage_stat(source, &stat) < 0) {
        return SHADPS4_SAVE_ERROR_NOT_FOUND;
    }
    if (g_snprintf(temporary, sizeof(temporary), "%s.tmp", destination) >=
        sizeof(temporary) ||
        g_snprintf(previous, sizeof(previous), "%s.old", destination) >=
        sizeof(previous)) {
        return SHADPS4_SAVE_ERROR_INTERNAL;
    }
    qemu_host_storage_cleanup(temporary);
    ret = shadps4_hle_storage_copy_tree(source, temporary, 0, &total);
    if (ret >= 0) {
        bool had_destination = qemu_host_storage_stat(destination, &stat) >= 0;

        qemu_host_storage_cleanup(previous);
        if (had_destination) {
            ret = qemu_host_storage_rename(destination, previous);
        }
        if (ret >= 0) {
            ret = qemu_host_storage_rename(temporary, destination);
        }
        if (ret < 0 && had_destination) {
            qemu_host_storage_rename(previous, destination);
        } else if (ret >= 0) {
            qemu_host_storage_cleanup(previous);
        }
    }
    if (ret < 0) {
        qemu_host_storage_cleanup(temporary);
    }
    hle->save_data_progress = ret < 0 ? 0.0f : 1.0f;
    return ret < 0 ? SHADPS4_SAVE_ERROR_INTERNAL : 0;
}

static uint64_t shadps4_hle_np_callout(ShadPS4HLEState *hle, CPUState *cs,
                                       uint32_t operation, uint64_t context,
                                       uint64_t entry, uint64_t delay,
                                       uint64_t handler, uint64_t argument)
{
    uint8_t ctx[40];
    uint8_t item[32] = { 0 };
    uint64_t head;
    uint64_t current;
    uint64_t previous;
    uint64_t removed_address;
    int64_t deadline;
    uint32_t active;
    uint32_t i;

    (void)hle;
    if (!context || !shadps4_guest_rw(cs, context, ctx, sizeof(ctx), false)) {
        return UINT32_C(0x80558001);
    }
    active = ldl_le_p(ctx + 24);
    head = ldq_le_p(ctx + 32);
    if (operation == SHADPS4_HLE_NP_CALLOUT_INIT) {
        if (active) {
            return UINT32_C(0x80558002);
        }
        memset(ctx, 0, sizeof(ctx));
        stl_le_p(ctx + 24, 1);
        return shadps4_guest_rw(cs, context, ctx, sizeof(ctx), true) ? 0 :
               UINT32_C(0x80558001);
    }
    if (!active) {
        return UINT32_C(0x80558001);
    }
    if (operation == SHADPS4_HLE_NP_CALLOUT_TERM) {
        stl_le_p(ctx + 24, 0);
        stl_le_p(ctx + 28, 1);
        stq_le_p(ctx + 32, 0);
        return shadps4_guest_rw(cs, context, ctx, sizeof(ctx), true) ? 0 :
               UINT32_C(0x80558001);
    }
    if (!entry) {
        return UINT32_C(0x80558001);
    }
    if (operation == SHADPS4_HLE_NP_CALLOUT_START ||
        operation == SHADPS4_HLE_NP_CALLOUT_START64) {
        for (current = head, i = 0; current && i < 256; i++) {
            uint64_t next;

            if (current == entry) {
                return UINT32_C(0x80558006);
            }
            if (!shadps4_guest_rw(cs, current, &next, sizeof(next), false)) {
                return UINT32_C(0x80558001);
            }
            current = le64_to_cpu(next);
        }
        if (current) {
            return UINT32_C(0x80558006);
        }
        deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000 +
                   (operation == SHADPS4_HLE_NP_CALLOUT_START ?
                    (uint32_t)delay : (int64_t)delay);
        previous = 0;
        for (current = head, i = 0; current && i < 256; i++) {
            uint64_t next;

            if (!shadps4_guest_rw(cs, current, item, sizeof(item), false)) {
                return UINT32_C(0x80558001);
            }
            next = ldq_le_p(item);
            if ((int64_t)ldq_le_p(item + 16) >= deadline) {
                break;
            }
            previous = current;
            current = next;
        }
        if (current && i == 256) {
            return UINT32_C(0x80558006);
        }
        memset(item, 0, sizeof(item));
        stq_le_p(item, current);
        stq_le_p(item + 8, handler);
        stq_le_p(item + 16, deadline);
        stq_le_p(item + 24, argument);
        if (!shadps4_guest_rw(cs, entry, item, sizeof(item), true)) {
            return UINT32_C(0x80558001);
        }
        if (previous) {
            return shadps4_hle_write_u64(cs, previous, entry) ?
                   UINT32_C(0x80558001) : 0;
        }
        stq_le_p(ctx + 32, entry);
        return shadps4_guest_rw(cs, context, ctx, sizeof(ctx), true) ? 0 :
               UINT32_C(0x80558001);
    }
    removed_address = delay;
    for (current = head, i = 0; current && i < 256; i++) {
        uint64_t next;

        if (!shadps4_guest_rw(cs, current, item, sizeof(item), false)) {
            return UINT32_C(0x80558001);
        }
        next = ldq_le_p(item);
        if (current == entry) {
            if (current == head) {
                stq_le_p(ctx + 32, next);
                if (!shadps4_guest_rw(cs, context, ctx, sizeof(ctx), true)) {
                    return UINT32_C(0x80558001);
                }
            } else if (shadps4_hle_write_u64(cs, head, next)) {
                return UINT32_C(0x80558001);
            }
            return removed_address ?
                   shadps4_hle_write_u32(cs, removed_address, 1) : 0;
        }
        head = current;
        current = next;
    }
    return removed_address ? shadps4_hle_write_u32(cs, removed_address, 0) : 0;
}
