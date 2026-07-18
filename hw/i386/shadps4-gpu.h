/*
 * shadPS4 Liverpool GPU command frontend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I386_SHADPS4_GPU_H
#define HW_I386_SHADPS4_GPU_H

#include "hw/core/cpu.h"
#include "qemu/timer.h"

#define SHADPS4_GPU_MAX_QUEUES 64

typedef struct ShadPS4D3D12State ShadPS4D3D12State;

typedef struct ShadPS4GPUSubmit {
    uint64_t command_addr;
    uint32_t command_dwords;
    uint32_t queue_id;
    uint64_t fence_addr;
    uint64_t fence_value;
} ShadPS4GPUSubmit;

typedef struct ShadPS4GPUFlip {
    uint64_t pixels_addr;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t buffer_index;
    uint32_t tiling_mode;
} ShadPS4GPUFlip;

typedef struct ShadPS4GPUStatus {
    uint64_t submit_count;
    uint64_t flip_count;
    uint64_t vblank_count;
    uint64_t parsed_packet_count;
    uint64_t rejected_submit_count;
    uint64_t last_fence_value;
} ShadPS4GPUStatus;

typedef struct ShadPS4GPUQueue {
    uint64_t submit_count;
    uint64_t completed_fence;
    uint64_t ring_addr;
    uint64_t read_ptr_addr;
    uint32_t ring_dwords;
    uint32_t read_offset;
    bool mapped;
} ShadPS4GPUQueue;

typedef struct ShadPS4GPUState {
    QEMUTimer *vblank_timer;
    uint64_t submit_count;
    uint64_t flip_count;
    uint64_t vblank_count;
    uint64_t parsed_packet_count;
    uint64_t rejected_submit_count;
    uint64_t last_fence_value;
    uint32_t context_registers[0x400];
    uint32_t shader_registers[0x400];
    ShadPS4GPUQueue queues[SHADPS4_GPU_MAX_QUEUES];
    ShadPS4D3D12State *d3d12;
    uint8_t *surface;
    size_t surface_capacity;
    uint8_t *frame;
    size_t frame_capacity;
    bool video_backend_valid;
    bool last_video_d3d12;
    bool running;
} ShadPS4GPUState;

void shadps4_gpu_init(ShadPS4GPUState *gpu);
void shadps4_gpu_reset(ShadPS4GPUState *gpu);
void shadps4_gpu_cleanup(ShadPS4GPUState *gpu);
bool shadps4_gpu_submit(ShadPS4GPUState *gpu, CPUState *cs,
                        const ShadPS4GPUSubmit *submit);
int shadps4_gpu_map_queue(ShadPS4GPUState *gpu, CPUState *cs,
                          uint32_t pipe_id, uint32_t queue_id,
                          uint64_t ring_addr, uint32_t ring_dwords,
                          uint64_t read_ptr_addr);
bool shadps4_gpu_unmap_queue(ShadPS4GPUState *gpu, uint32_t queue_id);
bool shadps4_gpu_ding_dong(ShadPS4GPUState *gpu, CPUState *cs,
                           uint32_t queue_id, uint32_t next_offset);
bool shadps4_gpu_flip(ShadPS4GPUState *gpu, CPUState *cs,
                      const ShadPS4GPUFlip *flip);
bool shadps4_gpu_register_surface(ShadPS4GPUState *gpu, uint32_t index,
                                  uint64_t guest_address, uint32_t width,
                                  uint32_t height, uint32_t pitch,
                                  uint32_t format, uint32_t tiling_mode);
void shadps4_gpu_finish(ShadPS4GPUState *gpu);
void shadps4_gpu_get_status(const ShadPS4GPUState *gpu,
                            ShadPS4GPUStatus *status);

#endif /* HW_I386_SHADPS4_GPU_H */
