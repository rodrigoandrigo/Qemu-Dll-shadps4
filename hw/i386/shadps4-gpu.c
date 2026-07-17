/*
 * shadPS4 Liverpool GPU command frontend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/i386/shadps4-gpu.h"
#ifdef CONFIG_WIN32
#include "hw/i386/shadps4-gpu-d3d12.h"
#endif
#define QEMU_HOST_INTERNAL
#include "qemu/qemu-host.h"
#include "qemu/units.h"

#define SHADPS4_VBLANK_PERIOD_NS 16666667
#define SHADPS4_GPU_MAX_COMMAND_DWORDS 0xfffff
#define SHADPS4_GPU_MAX_FRAME_SIZE (64 * MiB)

#define SHADPS4_PM4_TYPE0 0
#define SHADPS4_PM4_TYPE1 1
#define SHADPS4_PM4_TYPE2 2
#define SHADPS4_PM4_TYPE3 3
#define SHADPS4_PM4_NOP 0x10
#define SHADPS4_PM4_INDIRECT_BUFFER_CONST 0x33
#define SHADPS4_PM4_WRITE_DATA 0x37
#define SHADPS4_PM4_INDIRECT_BUFFER 0x3f
#define SHADPS4_PM4_EVENT_WRITE_EOP 0x47
#define SHADPS4_PM4_DMA_DATA 0x50
#define SHADPS4_PM4_SET_CONTEXT_REG 0x69
#define SHADPS4_PM4_SET_SH_REG 0x76
#define SHADPS4_PM4_MAX_IB_DEPTH 8
#define SHADPS4_PM4_MAX_DMA_SIZE (2 * MiB)

static bool shadps4_gpu_guest_rw(CPUState *cs, uint64_t addr, void *data,
                                 size_t size, bool write)
{
    if (!size) {
        return true;
    }
    return cs && addr && data && addr <= UINT64_MAX - (size - 1) &&
           cpu_memory_rw_debug(cs, addr, data, size, write) == 0;
}

static bool shadps4_gpu_set_registers(uint32_t *registers,
                                      size_t register_count,
                                      const uint32_t *payload,
                                      uint32_t payload_count)
{
    uint32_t offset;
    uint32_t i;

    if (payload_count < 2) {
        return false;
    }
    offset = le32_to_cpu(payload[0]);
    if (offset >= register_count ||
        payload_count - 1 > register_count - offset) {
        return false;
    }
    for (i = 1; i < payload_count; i++) {
        registers[offset + i - 1] = le32_to_cpu(payload[i]);
    }
    return true;
}

static bool shadps4_gpu_copy_guest(CPUState *cs, uint64_t destination,
                                   uint64_t source, uint32_t size)
{
    g_autofree uint8_t *buffer = NULL;

    if (!size) {
        return true;
    }
    if (size > SHADPS4_PM4_MAX_DMA_SIZE || !destination || !source) {
        return false;
    }
    buffer = g_malloc(size);
    return shadps4_gpu_guest_rw(cs, source, buffer, size, false) &&
           shadps4_gpu_guest_rw(cs, destination, buffer, size, true);
}

static bool shadps4_gpu_execute_pm4_memory(CPUState *cs,
                                           const uint32_t *commands,
                                           uint32_t command_dwords,
                                           uint32_t depth)
{
    uint32_t offset = 0;

    if (depth > SHADPS4_PM4_MAX_IB_DEPTH) {
        return false;
    }
    while (offset < command_dwords) {
        uint32_t header = le32_to_cpu(commands[offset]);
        uint32_t type = header >> 30;
        uint32_t payload_count;
        uint32_t opcode;
        const uint32_t *payload;

        if (type == SHADPS4_PM4_TYPE2) {
            offset++;
            continue;
        }
        if (type == SHADPS4_PM4_TYPE0 || type == SHADPS4_PM4_TYPE1) {
            payload_count = type == SHADPS4_PM4_TYPE0 ?
                            ((header >> 16) & 0x3fff) + 1 : 2;
            if (payload_count > command_dwords - offset - 1) {
                return false;
            }
            offset += payload_count + 1;
            continue;
        }
        opcode = (header >> 8) & 0xff;
        payload_count = ((header >> 16) & 0x3fff) + 1;
        if (payload_count > command_dwords - offset - 1) {
            return false;
        }
        payload = commands + offset + 1;
        switch (opcode) {
        case SHADPS4_PM4_WRITE_DATA:
            if (payload_count >= 4) {
                uint32_t control = le32_to_cpu(payload[0]);
                uint32_t dst_sel = (control >> 8) & 0xf;
                bool one_address = (control >> 16) & 1;
                uint64_t address = le32_to_cpu(payload[1]) |
                                   (uint64_t)le32_to_cpu(payload[2]) << 32;
                uint32_t words = payload_count - 3;

                if ((dst_sel != 2 && dst_sel != 5) || !address) {
                    return false;
                }
                for (uint32_t i = 0; i < words; i++) {
                    uint32_t value = payload[3 + i];
                    uint64_t write_address = address +
                        (one_address ? 0 : (uint64_t)i * sizeof(value));

                    if (!shadps4_gpu_guest_rw(cs, write_address, &value,
                                              sizeof(value), true)) {
                        return false;
                    }
                }
            }
            break;
        case SHADPS4_PM4_DMA_DATA:
            if (payload_count >= 6) {
                uint32_t control = le32_to_cpu(payload[0]);
                uint32_t src_sel = (control >> 29) & 3;
                uint32_t dst_sel = (control >> 20) & 3;
                uint64_t source = le32_to_cpu(payload[1]) |
                                  (uint64_t)le32_to_cpu(payload[2]) << 32;
                uint64_t destination = le32_to_cpu(payload[3]) |
                                       (uint64_t)le32_to_cpu(payload[4]) << 32;
                uint32_t size = le32_to_cpu(payload[5]) & 0x1fffff;

                if ((dst_sel != 0 && dst_sel != 3) ||
                    size > SHADPS4_PM4_MAX_DMA_SIZE) {
                    return false;
                }
                if (src_sel == 2) {
                    uint32_t value = payload[1];
                    uint32_t done = 0;

                    while (done < size) {
                        uint32_t chunk = MIN((uint32_t)sizeof(value),
                                             size - done);
                        if (!shadps4_gpu_guest_rw(cs, destination + done,
                                                  &value, chunk, true)) {
                            return false;
                        }
                        done += chunk;
                    }
                } else if ((src_sel == 0 || src_sel == 3) &&
                           !shadps4_gpu_copy_guest(cs, destination,
                                                   source, size)) {
                    return false;
                } else if (src_sel != 0 && src_sel != 3) {
                    return false;
                }
            }
            break;
        case SHADPS4_PM4_EVENT_WRITE_EOP:
            if (payload_count >= 5) {
                uint32_t control = le32_to_cpu(payload[2]);
                uint32_t data_sel = (control >> 29) & 7;
                uint64_t address = le32_to_cpu(payload[1]) |
                                   (uint64_t)(control & 0xffff) << 32;
                uint64_t value = le32_to_cpu(payload[3]) |
                                 (uint64_t)le32_to_cpu(payload[4]) << 32;
                size_t size = 0;

                if (data_sel == 1) {
                    size = sizeof(uint32_t);
                } else if (data_sel == 2) {
                    size = sizeof(uint64_t);
                } else if (data_sel == 3 || data_sel == 4) {
                    value = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                    size = sizeof(uint64_t);
                } else if (data_sel != 0) {
                    return false;
                }
                if (size && !shadps4_gpu_guest_rw(cs, address, &value,
                                                  size, true)) {
                    return false;
                }
            }
            break;
        case SHADPS4_PM4_INDIRECT_BUFFER_CONST:
        case SHADPS4_PM4_INDIRECT_BUFFER:
            if (payload_count >= 3) {
                g_autofree uint32_t *indirect = NULL;
                uint64_t address = le32_to_cpu(payload[0]) |
                                   (uint64_t)(le32_to_cpu(payload[1]) &
                                              0xffff) << 32;
                uint32_t words = le32_to_cpu(payload[2]) & 0xfffff;

                if (!address || !words ||
                    words > SHADPS4_GPU_MAX_COMMAND_DWORDS) {
                    return false;
                }
                indirect = g_new(uint32_t, words);
                if (!shadps4_gpu_guest_rw(cs, address, indirect,
                                          words * sizeof(*indirect), false) ||
                    !shadps4_gpu_execute_pm4_memory(cs, indirect, words,
                                                    depth + 1)) {
                    return false;
                }
            }
            break;
        default:
            break;
        }
        offset += payload_count + 1;
    }
    return true;
}

static bool shadps4_gpu_parse_pm4(const ShadPS4GPUState *gpu,
                                  const uint32_t *commands,
                                  uint32_t command_dwords,
                                  uint32_t *context_registers,
                                  uint32_t *shader_registers,
                                  uint64_t *parsed_packet_count)
{
    uint32_t offset = 0;

    memcpy(context_registers, gpu->context_registers,
           sizeof(gpu->context_registers));
    memcpy(shader_registers, gpu->shader_registers,
           sizeof(gpu->shader_registers));

    while (offset < command_dwords) {
        uint32_t header = le32_to_cpu(commands[offset]);
        uint32_t type = header >> 30;
        uint32_t opcode;
        uint32_t payload_count;
        const uint32_t *payload;
        bool valid = true;

        if (type == SHADPS4_PM4_TYPE2) {
            (*parsed_packet_count)++;
            offset++;
            continue;
        }
        if (type == SHADPS4_PM4_TYPE0 || type == SHADPS4_PM4_TYPE1) {
            payload_count = type == SHADPS4_PM4_TYPE0 ?
                            ((header >> 16) & 0x3fff) + 1 : 2;
            if (payload_count > command_dwords - offset - 1) {
                return false;
            }
            /* Preserve packet framing for legacy type 0/1 register writes.
             * The D3D12 translator consumes the type 3 state used by GNM. */
            (*parsed_packet_count)++;
            offset += payload_count + 1;
            continue;
        }
        opcode = (header >> 8) & 0xff;
        payload_count = ((header >> 16) & 0x3fff) + 1;
        if (payload_count > command_dwords - offset - 1) {
            return false;
        }
        payload = &commands[offset + 1];
        switch (opcode) {
        case SHADPS4_PM4_NOP:
        case SHADPS4_PM4_EVENT_WRITE_EOP:
            break;
        case SHADPS4_PM4_SET_CONTEXT_REG:
            valid = shadps4_gpu_set_registers(
                context_registers, ARRAY_SIZE(gpu->context_registers),
                payload, payload_count);
            break;
        case SHADPS4_PM4_SET_SH_REG:
            valid = shadps4_gpu_set_registers(
                shader_registers, ARRAY_SIZE(gpu->shader_registers),
                payload, payload_count);
            break;
        case SHADPS4_PM4_WRITE_DATA:
            valid = payload_count >= 3;
            break;
        default:
            /* Backend-specific execution follows after common validation. */
            break;
        }
        if (!valid) {
            return false;
        }
        (*parsed_packet_count)++;
        offset += payload_count + 1;
    }
    return true;
}

static void shadps4_gpu_vblank(void *opaque)
{
    ShadPS4GPUState *gpu = opaque;

    if (!gpu->running) {
        return;
    }
    gpu->vblank_count++;
    timer_mod(gpu->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              SHADPS4_VBLANK_PERIOD_NS);
}

void shadps4_gpu_init(ShadPS4GPUState *gpu)
{
    memset(gpu, 0, sizeof(*gpu));
#ifdef CONFIG_WIN32
    gpu->d3d12 = shadps4_d3d12_create();
#endif
    gpu->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    shadps4_gpu_vblank, gpu);
}

void shadps4_gpu_reset(ShadPS4GPUState *gpu)
{
    gpu->submit_count = 0;
    gpu->flip_count = 0;
    gpu->vblank_count = 0;
    gpu->parsed_packet_count = 0;
    gpu->rejected_submit_count = 0;
    gpu->last_fence_value = 0;
    memset(gpu->context_registers, 0, sizeof(gpu->context_registers));
    memset(gpu->shader_registers, 0, sizeof(gpu->shader_registers));
    memset(gpu->queues, 0, sizeof(gpu->queues));
#ifdef CONFIG_WIN32
    if (gpu->d3d12) {
        shadps4_d3d12_reset(gpu->d3d12);
    }
#endif
    gpu->running = true;
    timer_mod(gpu->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              SHADPS4_VBLANK_PERIOD_NS);
}

void shadps4_gpu_cleanup(ShadPS4GPUState *gpu)
{
    if (!gpu->vblank_timer) {
        return;
    }
    gpu->running = false;
#ifdef CONFIG_WIN32
    shadps4_d3d12_destroy(gpu->d3d12);
    gpu->d3d12 = NULL;
#endif
    timer_del(gpu->vblank_timer);
    timer_free(gpu->vblank_timer);
    gpu->vblank_timer = NULL;
    g_clear_pointer(&gpu->frame, g_free);
    gpu->frame_capacity = 0;
    g_clear_pointer(&gpu->surface, g_free);
    gpu->surface_capacity = 0;
}

void shadps4_gpu_finish(ShadPS4GPUState *gpu)
{
    /* PM4 processing is synchronous in this backend.  Keep this boundary so
    * an asynchronous renderer can wait for its queue before scanout. */
#ifdef CONFIG_WIN32
    if (gpu->d3d12 && !shadps4_d3d12_finish(gpu->d3d12)) {
        warn_report("shadPS4 D3D12: failed to finish GPU queue");
    }
#else
    (void)gpu;
#endif
    smp_mb();
}

static uint32_t shadps4_gpu_display_pixel_index(uint32_t x, uint32_t y)
{
    return ((x & 1) << 0) | (((x >> 1) & 1) << 1) |
           ((y & 1) << 2) | (((x >> 2) & 1) << 3) |
           (((y >> 1) & 1) << 4) | (((y >> 2) & 1) << 5);
}

static uint32_t shadps4_gpu_display_tiled_offset(uint32_t x, uint32_t y,
                                                 uint32_t pitch)
{
    const uint32_t macro_tile_pitch = 128;
    const uint32_t macro_tile_height = 64;
    const uint32_t macro_tile_bytes = 256;
    uint32_t tx = x / 64;
    uint32_t ty = y / 8;
    uint32_t pipe = (((x >> 3) ^ (y >> 3) ^ (x >> 4)) & 1) |
                    ((((x >> 5) ^ (y >> 4)) & 1) << 1) |
                    ((((x >> 6) ^ (y >> 5)) & 1) << 2);
    uint32_t bank = (((tx >> 0) ^ (ty >> 3)) & 1) |
                    ((((tx >> 1) ^ (ty >> 2) ^ (ty >> 3)) & 1) << 1) |
                    ((((tx >> 2) ^ (ty >> 1)) & 1) << 2) |
                    ((((tx >> 3) ^ (ty >> 0)) & 1) << 3);
    uint32_t macro_offset =
        ((y / macro_tile_height) * (pitch / macro_tile_pitch) +
         x / macro_tile_pitch) * macro_tile_bytes;
    uint32_t total = macro_offset +
                     shadps4_gpu_display_pixel_index(x, y) * 4;

    return (total & 0xff) | (pipe << 8) | (bank << 11) |
           ((total >> 8) << 15);
}

static void shadps4_gpu_convert_pixel(uint8_t *dst, const uint8_t *src,
                                      QemuHostPixelFormat format)
{
    if (format == QEMU_HOST_PIXEL_FORMAT_RGBA8888 ||
        format == QEMU_HOST_PIXEL_FORMAT_RGBX8888) {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
    } else {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
    }
    dst[3] = (format == QEMU_HOST_PIXEL_FORMAT_RGBX8888 ||
              format == QEMU_HOST_PIXEL_FORMAT_BGRX8888) ? 0xff : src[3];
}

bool shadps4_gpu_register_surface(ShadPS4GPUState *gpu, uint32_t index,
                                  uint64_t guest_address, uint32_t width,
                                  uint32_t height, uint32_t pitch,
                                  uint32_t format, uint32_t tiling_mode)
{
#ifdef CONFIG_WIN32
    if (gpu->d3d12) {
        return shadps4_d3d12_register_surface(gpu->d3d12, index,
                                               guest_address, width, height,
                                               pitch, format, tiling_mode);
    }
#else
    (void)gpu;
    (void)index;
    (void)guest_address;
    (void)width;
    (void)height;
    (void)pitch;
    (void)format;
    (void)tiling_mode;
#endif
    return true;
}

static bool shadps4_gpu_submit_commands(ShadPS4GPUState *gpu, CPUState *cs,
                                        const uint32_t *commands,
                                        uint32_t command_dwords,
                                        uint32_t queue_id,
                                        uint64_t fence_addr,
                                        uint64_t fence_value)
{
    uint32_t context_registers[ARRAY_SIZE(gpu->context_registers)];
    uint32_t shader_registers[ARRAY_SIZE(gpu->shader_registers)];
    uint64_t parsed_packet_count = 0;
    uint64_t fence;
    bool parsed;

    if (!commands || !command_dwords ||
        command_dwords > SHADPS4_GPU_MAX_COMMAND_DWORDS ||
        queue_id >= SHADPS4_GPU_MAX_QUEUES) {
        gpu->rejected_submit_count++;
        return false;
    }
    parsed = shadps4_gpu_parse_pm4(gpu, commands, command_dwords,
                                   context_registers, shader_registers,
                                   &parsed_packet_count);
    if (!parsed) {
        gpu->rejected_submit_count++;
        return false;
    }
#ifdef CONFIG_WIN32
    if (gpu->d3d12 &&
        !shadps4_d3d12_submit(gpu->d3d12, cs, commands,
                              command_dwords, queue_id,
                              gpu->context_registers,
                              ARRAY_SIZE(gpu->context_registers),
                              gpu->shader_registers,
                              ARRAY_SIZE(gpu->shader_registers))) {
        warn_report("shadPS4 D3D12: command submission failed; using CPU "
                    "scanout fallback");
    }
    if (gpu->d3d12 && !shadps4_d3d12_finish(gpu->d3d12)) {
        gpu->rejected_submit_count++;
        return false;
    }
#endif
    if (!shadps4_gpu_execute_pm4_memory(cs, commands, command_dwords, 0)) {
        gpu->rejected_submit_count++;
        return false;
    }
    if (fence_addr) {
        fence = cpu_to_le64(fence_value);
        if (!shadps4_gpu_guest_rw(cs, fence_addr, &fence,
                                  sizeof(fence), true)) {
            gpu->rejected_submit_count++;
            return false;
        }
    }
    memcpy(gpu->context_registers, context_registers,
           sizeof(context_registers));
    memcpy(gpu->shader_registers, shader_registers,
           sizeof(shader_registers));
    gpu->parsed_packet_count += parsed_packet_count;
    gpu->submit_count++;
    gpu->queues[queue_id].submit_count++;
    gpu->queues[queue_id].completed_fence = fence_value;
    gpu->last_fence_value = fence_value;
    return true;
}

bool shadps4_gpu_submit(ShadPS4GPUState *gpu, CPUState *cs,
                        const ShadPS4GPUSubmit *submit)
{
    g_autofree uint32_t *commands = NULL;

    if (!submit) {
        gpu->submit_count++;
        return true;
    }
    if (!submit->command_dwords ||
        submit->command_dwords > SHADPS4_GPU_MAX_COMMAND_DWORDS ||
        submit->queue_id >= SHADPS4_GPU_MAX_QUEUES) {
        gpu->rejected_submit_count++;
        return false;
    }
    commands = g_new(uint32_t, submit->command_dwords);
    if (!shadps4_gpu_guest_rw(cs, submit->command_addr, commands,
                              submit->command_dwords * sizeof(uint32_t),
                              false)) {
        gpu->rejected_submit_count++;
        return false;
    }
    return shadps4_gpu_submit_commands(gpu, cs, commands,
                                       submit->command_dwords,
                                       submit->queue_id, submit->fence_addr,
                                       submit->fence_value);
}

int shadps4_gpu_map_queue(ShadPS4GPUState *gpu, CPUState *cs,
                          uint32_t pipe_id, uint32_t queue_id,
                          uint64_t ring_addr, uint32_t ring_dwords,
                          uint64_t read_ptr_addr)
{
    uint32_t index = pipe_id * 8 + queue_id;
    uint32_t read_offset = 0;

    if (pipe_id >= 8 || queue_id >= 8 || index >= SHADPS4_GPU_MAX_QUEUES ||
        !ring_addr || (ring_addr & 0xff) || !ring_dwords ||
        ring_dwords > SHADPS4_GPU_MAX_COMMAND_DWORDS ||
        (ring_dwords & (ring_dwords - 1)) || !read_ptr_addr ||
        ring_addr > UINT64_MAX - (uint64_t)ring_dwords * sizeof(uint32_t) ||
        (read_ptr_addr & 3) || gpu->queues[index].mapped ||
        !shadps4_gpu_guest_rw(cs, read_ptr_addr, &read_offset,
                              sizeof(read_offset), true)) {
        return -1;
    }
    gpu->queues[index].ring_addr = ring_addr;
    gpu->queues[index].read_ptr_addr = read_ptr_addr;
    gpu->queues[index].ring_dwords = ring_dwords;
    gpu->queues[index].read_offset = 0;
    gpu->queues[index].mapped = true;
    return index + 1;
}

bool shadps4_gpu_unmap_queue(ShadPS4GPUState *gpu, uint32_t queue_id)
{
    if (!queue_id || queue_id > SHADPS4_GPU_MAX_QUEUES ||
        !gpu->queues[queue_id - 1].mapped) {
        return false;
    }
    memset(&gpu->queues[queue_id - 1], 0,
           sizeof(gpu->queues[queue_id - 1]));
    return true;
}

bool shadps4_gpu_ding_dong(ShadPS4GPUState *gpu, CPUState *cs,
                           uint32_t queue_id, uint32_t next_offset)
{
    ShadPS4GPUQueue *queue;
    g_autofree uint32_t *commands = NULL;
    uint32_t command_dwords;
    uint32_t tail_dwords;
    uint32_t read_offset_le;

    if (!queue_id || queue_id > SHADPS4_GPU_MAX_QUEUES) {
        return false;
    }
    queue = &gpu->queues[queue_id - 1];
    if (!queue->mapped || next_offset >= queue->ring_dwords) {
        return false;
    }
    command_dwords = next_offset >= queue->read_offset ?
                     next_offset - queue->read_offset :
                     queue->ring_dwords - queue->read_offset + next_offset;
    if (command_dwords) {
        commands = g_new(uint32_t, command_dwords);
        tail_dwords = MIN(command_dwords,
                          queue->ring_dwords - queue->read_offset);
        if (!shadps4_gpu_guest_rw(
                cs, queue->ring_addr + queue->read_offset * 4, commands,
                tail_dwords * sizeof(uint32_t), false) ||
            (command_dwords > tail_dwords &&
             !shadps4_gpu_guest_rw(
                 cs, queue->ring_addr, commands + tail_dwords,
                 (command_dwords - tail_dwords) * sizeof(uint32_t), false))) {
            gpu->rejected_submit_count++;
            return false;
        }
        if (!shadps4_gpu_submit_commands(gpu, cs, commands, command_dwords,
                                         queue_id - 1, 0, 0)) {
            return false;
        }
        queue->read_offset = next_offset;
    }
    read_offset_le = cpu_to_le32(queue->read_offset);
    return shadps4_gpu_guest_rw(cs, queue->read_ptr_addr, &read_offset_le,
                                sizeof(read_offset_le), true);
}

bool shadps4_gpu_flip(ShadPS4GPUState *gpu, CPUState *cs,
                      const ShadPS4GPUFlip *flip)
{
    g_autofree char *message = NULL;
    size_t source_size;
    size_t frame_size;
    size_t pixel_count;
    size_t nonblack = 0;
    uint32_t source_height;
    uint32_t output_stride;
    uint32_t sample;
    uint32_t x;
    uint32_t y;
    QemuHostPixelFormat format;
    bool d3d12_frame = false;

    if (!flip) {
        gpu->flip_count++;
        return true;
    }
    if (!flip->width || !flip->height || flip->width > 4096 ||
        flip->height > 2160 || flip->stride < flip->width * 4 ||
        flip->stride > 65536 ||
        flip->height > SHADPS4_GPU_MAX_FRAME_SIZE / flip->stride) {
        return false;
    }
    switch (flip->format) {
    case QEMU_HOST_PIXEL_FORMAT_BGRA8888:
    case QEMU_HOST_PIXEL_FORMAT_RGBA8888:
    case QEMU_HOST_PIXEL_FORMAT_RGBX8888:
    case QEMU_HOST_PIXEL_FORMAT_BGRX8888:
        format = flip->format;
        break;
    default:
        return false;
    }
    if (flip->tiling_mode > 1 ||
        (flip->tiling_mode == 0 && ((flip->stride / 4) & 127))) {
        return false;
    }
    source_height = flip->tiling_mode == 0 ?
                    ROUND_UP(flip->height, 64) : flip->height;
    if (source_height > SHADPS4_GPU_MAX_FRAME_SIZE / flip->stride) {
        return false;
    }
    source_size = flip->stride * source_height;
    output_stride = flip->width * 4;
    frame_size = output_stride * flip->height;
    if (gpu->surface_capacity < source_size) {
        gpu->surface = g_realloc(gpu->surface, source_size);
        gpu->surface_capacity = source_size;
    }
    if (gpu->frame_capacity < frame_size) {
        gpu->frame = g_realloc(gpu->frame, frame_size);
        gpu->frame_capacity = frame_size;
    }
#ifdef CONFIG_WIN32
    if (gpu->d3d12 &&
        shadps4_d3d12_read_surface(gpu->d3d12, flip->buffer_index,
                                   gpu->frame, output_stride)) {
        d3d12_frame = true;
        goto frame_ready;
    }
#endif
    if (!shadps4_gpu_guest_rw(cs, flip->pixels_addr, gpu->surface,
                              source_size, false)) {
        return false;
    }
    for (y = 0; y < flip->height; y++) {
        for (x = 0; x < flip->width; x++) {
            size_t source_offset = flip->tiling_mode == 0 ?
                shadps4_gpu_display_tiled_offset(x, y, flip->stride / 4) :
                (size_t)y * flip->stride + x * 4;
            uint8_t *dst = gpu->frame + (size_t)y * output_stride + x * 4;

            if (source_offset > source_size - 4) {
                return false;
            }
            shadps4_gpu_convert_pixel(dst, gpu->surface + source_offset,
                                      format);
        }
    }
#ifdef CONFIG_WIN32
frame_ready:
#endif
    gpu->flip_count++;
    pixel_count = (size_t)flip->width * flip->height;
    for (size_t i = 0; i < pixel_count; i++) {
        uint32_t pixel;

        memcpy(&pixel, gpu->frame + i * 4, sizeof(pixel));
        nonblack += (pixel & 0x00ffffff) != 0;
    }
    memcpy(&sample, gpu->frame, sizeof(sample));
    message = g_strdup_printf(
        "host video: flip=%" PRIu64 " buffer=%u gpu=%#" PRIx64
        " cpu=%p size=%ux%u pitch=%u format=%u sample=%08x"
        " nonblack=%zu/%zu tiling=%u backend=%s",
        gpu->flip_count, flip->buffer_index, flip->pixels_addr, gpu->frame,
        flip->width, flip->height, output_stride,
        QEMU_HOST_PIXEL_FORMAT_BGRA8888, sample, nonblack, pixel_count,
        flip->tiling_mode, d3d12_frame ? "d3d12" : "guest-memory");
    qemu_host_emit_log(QEMU_HOST_LOG_INFO, message);
    qemu_host_emit_video_frame(gpu->frame, flip->width, flip->height,
                               output_stride,
                               QEMU_HOST_PIXEL_FORMAT_BGRA8888);
    return true;
}

void shadps4_gpu_get_status(const ShadPS4GPUState *gpu,
                            ShadPS4GPUStatus *status)
{
    status->submit_count = gpu->submit_count;
    status->flip_count = gpu->flip_count;
    status->vblank_count = gpu->vblank_count;
    status->parsed_packet_count = gpu->parsed_packet_count;
    status->rejected_submit_count = gpu->rejected_submit_count;
    status->last_fence_value = gpu->last_fence_value;
}
