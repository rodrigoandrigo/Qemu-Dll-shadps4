/*
 * shadPS4 D3D12 GPU backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I386_SHADPS4_GPU_D3D12_H
#define HW_I386_SHADPS4_GPU_D3D12_H

#include "hw/core/cpu.h"

typedef struct ShadPS4D3D12State ShadPS4D3D12State;

ShadPS4D3D12State *shadps4_d3d12_create(void);
void shadps4_d3d12_destroy(ShadPS4D3D12State *d3d12);
bool shadps4_d3d12_reset(ShadPS4D3D12State *d3d12);
bool shadps4_d3d12_submit(ShadPS4D3D12State *d3d12,
                          CPUState *cs,
                          const uint32_t *commands,
                          uint32_t command_dwords, uint32_t queue_id,
                          const uint32_t *context_registers,
                          uint32_t context_register_count,
                          const uint32_t *shader_registers,
                          uint32_t shader_register_count);
bool shadps4_d3d12_finish(ShadPS4D3D12State *d3d12);
bool shadps4_d3d12_is_ready(const ShadPS4D3D12State *d3d12);
bool shadps4_d3d12_register_surface(ShadPS4D3D12State *d3d12,
                                    uint32_t index, uint64_t guest_address,
                                    uint32_t width, uint32_t height,
                                    uint32_t pitch, uint32_t format,
                                    uint32_t tiling_mode);
bool shadps4_d3d12_read_surface(ShadPS4D3D12State *d3d12,
                                uint32_t index, uint8_t *pixels,
                                uint32_t stride);
bool shadps4_d3d12_publish_surface(ShadPS4D3D12State *d3d12,
                                   uint32_t index, uint64_t frame_id);

#endif /* HW_I386_SHADPS4_GPU_D3D12_H */
