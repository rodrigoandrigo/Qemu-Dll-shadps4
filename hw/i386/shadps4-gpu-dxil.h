/*
 * shadPS4 SPIR-V to DXIL bridge
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I386_SHADPS4_GPU_DXIL_H
#define HW_I386_SHADPS4_GPU_DXIL_H

typedef enum ShadPS4DxilStage {
    SHADPS4_DXIL_STAGE_VERTEX = 0,
    SHADPS4_DXIL_STAGE_TESS_CONTROL = 1,
    SHADPS4_DXIL_STAGE_TESS_EVALUATION = 2,
    SHADPS4_DXIL_STAGE_GEOMETRY = 3,
    SHADPS4_DXIL_STAGE_FRAGMENT = 4,
    SHADPS4_DXIL_STAGE_COMPUTE = 5,
} ShadPS4DxilStage;

typedef struct ShadPS4DxilBinary {
    uint8_t *data;
    size_t size;
    bool requires_runtime_data;
    bool needs_draw_sysvals;
} ShadPS4DxilBinary;

bool shadps4_dxil_available(uint64_t *version);
bool shadps4_dxil_self_test(char **error);
bool shadps4_dxil_compile(const uint32_t *spirv, size_t word_count,
                          ShadPS4DxilStage stage,
                          ShadPS4DxilBinary *binary, char **error);
void shadps4_dxil_binary_clear(ShadPS4DxilBinary *binary);

#endif /* HW_I386_SHADPS4_GPU_DXIL_H */
