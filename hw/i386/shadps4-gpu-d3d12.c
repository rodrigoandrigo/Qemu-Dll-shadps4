/*
 * shadPS4 D3D12 GPU backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#define CINTERFACE
#define COBJMACROS
#include <initguid.h>
#include <d3d12.h>
#include "hw/i386/shadps4-gpu-d3d12.h"
#include "hw/i386/shadps4-gpu-dxil.h"

#define SHADPS4_D3D12_MAX_SURFACES 16
#define SHADPS4_PM4_TYPE3 3
#define SHADPS4_PM4_DRAW_INDIRECT 0x24
#define SHADPS4_PM4_DRAW_INDEX_INDIRECT 0x25
#define SHADPS4_PM4_DRAW_INDEX2 0x27
#define SHADPS4_PM4_DRAW_INDEX_AUTO 0x2d
#define SHADPS4_PM4_DRAW_INDEX_OFFSET2 0x35
#define SHADPS4_PM4_DRAW_INDEX_INDIRECT_MULTI 0x38
#define SHADPS4_PM4_DISPATCH_DIRECT 0x15
#define SHADPS4_PM4_DISPATCH_INDIRECT 0x16
#define SHADPS4_PM4_DRAW_INDEX_INDIRECT_COUNT_MULTI 0x9d
#define SHADPS4_PM4_SET_CONTEXT_REG 0x69
#define SHADPS4_PM4_SET_SH_REG 0x76
#define SHADPS4_CONTEXT_CB_COLOR0_BASE 0x318
#define SHADPS4_CONTEXT_CB_COLOR_STRIDE 0xf
#define SHADPS4_CONTEXT_CB_COUNT 8
#define SHADPS4_SHADER_PS_PGM_LO 0x008
#define SHADPS4_SHADER_VS_PGM_LO 0x048
#define SHADPS4_SHADER_CS_PGM_LO 0x20c
#define SHADPS4_SHADER_SCAN_BYTES (64 * 1024)
#define SHADPS4_SHADER_MAX_BYTES (16 * 1024 * 1024)

typedef struct ShadPS4ShaderBinaryInfo {
    uint64_t address;
    uint64_t hash;
    uint32_t length;
    bool valid;
} ShadPS4ShaderBinaryInfo;

typedef struct ShadPS4D3D12Surface {
    ID3D12Resource *texture;
    ID3D12Resource *readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    D3D12_RESOURCE_STATES state;
    uint64_t readback_size;
    uint64_t guest_address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint32_t tiling_mode;
    bool rendered;
} ShadPS4D3D12Surface;

struct ShadPS4D3D12State {
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    ID3D12DescriptorHeap *rtv_heap;
    UINT rtv_stride;
    ShadPS4D3D12Surface surfaces[SHADPS4_D3D12_MAX_SURFACES];
    HANDLE fence_event;
    uint64_t fence_value;
    uint64_t submit_count;
    uint64_t draw_count;
    uint64_t dispatch_count;
    uint64_t unmatched_target_count;
    uint64_t unsupported_opcode_count;
    uint64_t invalid_shader_count;
    uint64_t reported_opcodes[4];
    bool ready;
};

static uint64_t shadps4_d3d12_shader_address(const uint32_t *registers,
                                              uint32_t register_count,
                                              uint32_t pgm_lo)
{
    uint64_t encoded;

    if (!registers || pgm_lo + 1 >= register_count) {
        return 0;
    }
    encoded = registers[pgm_lo] |
              ((uint64_t)(registers[pgm_lo + 1] & 0xff) << 32);
    return encoded << 8;
}

static bool shadps4_d3d12_read_shader_info(CPUState *cs, uint64_t address,
                                           ShadPS4ShaderBinaryInfo *info)
{
    static const uint8_t signature[] = { 'O', 'r', 'b', 'S', 'h', 'd', 'r' };
    g_autofree uint8_t *data = NULL;

    memset(info, 0, sizeof(*info));
    info->address = address;
    if (!cs || !address || address > UINT64_MAX - SHADPS4_SHADER_SCAN_BYTES) {
        return false;
    }
    data = g_malloc(SHADPS4_SHADER_SCAN_BYTES);
    if (cpu_memory_rw_debug(cs, address, data, SHADPS4_SHADER_SCAN_BYTES,
                            false) != 0) {
        return false;
    }
    for (uint32_t offset = 0;
         offset + 28 <= SHADPS4_SHADER_SCAN_BYTES; offset += 4) {
        uint32_t attributes;

        if (memcmp(data + offset, signature, sizeof(signature))) {
            continue;
        }
        memcpy(&attributes, data + offset + 8, sizeof(attributes));
        attributes = le32_to_cpu(attributes);
        info->length = attributes >> 8;
        memcpy(&info->hash, data + offset + 16, sizeof(info->hash));
        info->hash = le64_to_cpu(info->hash);
        info->valid = info->length != 0 &&
                      info->length <= SHADPS4_SHADER_MAX_BYTES &&
                      !(info->length & 3);
        return info->valid;
    }
    return false;
}

static void shadps4_d3d12_validate_shader(
    ShadPS4D3D12State *d3d12, CPUState *cs, const uint32_t *registers,
    uint32_t register_count, uint32_t pgm_lo, const char *stage)
{
    ShadPS4ShaderBinaryInfo info;
    uint64_t address = shadps4_d3d12_shader_address(
        registers, register_count, pgm_lo);

    if (!address) {
        return;
    }
    if (!shadps4_d3d12_read_shader_info(cs, address, &info)) {
        d3d12->invalid_shader_count++;
        if (!(d3d12->invalid_shader_count &
              (d3d12->invalid_shader_count - 1))) {
            warn_report("shadPS4 D3D12: invalid %s shader at guest=%#" PRIx64
                        "; failures=%" PRIu64,
                        stage, address, d3d12->invalid_shader_count);
        }
        return;
    }
    if (d3d12->draw_count + d3d12->dispatch_count == 0) {
        info_report("shadPS4 D3D12: %s shader guest=%#" PRIx64
                    " length=%u hash=%#" PRIx64,
                    stage, info.address, info.length, info.hash);
    }
}

static bool shadps4_d3d12_is_draw(uint32_t opcode)
{
    switch (opcode) {
    case SHADPS4_PM4_DRAW_INDIRECT:
    case SHADPS4_PM4_DRAW_INDEX_INDIRECT:
    case SHADPS4_PM4_DRAW_INDEX2:
    case SHADPS4_PM4_DRAW_INDEX_AUTO:
    case SHADPS4_PM4_DRAW_INDEX_OFFSET2:
    case SHADPS4_PM4_DRAW_INDEX_INDIRECT_MULTI:
    case SHADPS4_PM4_DRAW_INDEX_INDIRECT_COUNT_MULTI:
        return true;
    default:
        return false;
    }
}

static bool shadps4_d3d12_is_dispatch(uint32_t opcode)
{
    return opcode == SHADPS4_PM4_DISPATCH_DIRECT ||
           opcode == SHADPS4_PM4_DISPATCH_INDIRECT;
}

static int shadps4_d3d12_find_color_target(
    const ShadPS4D3D12State *d3d12, const uint32_t *context_registers,
    uint32_t context_register_count)
{
    uint32_t cb;

    for (cb = 0; cb < SHADPS4_CONTEXT_CB_COUNT; cb++) {
        uint32_t reg = SHADPS4_CONTEXT_CB_COLOR0_BASE +
                       cb * SHADPS4_CONTEXT_CB_COLOR_STRIDE;
        uint64_t address;

        if (reg >= context_register_count || !context_registers[reg]) {
            continue;
        }
        address = (uint64_t)context_registers[reg] << 8;
        for (uint32_t i = 0; i < SHADPS4_D3D12_MAX_SURFACES; i++) {
            if (d3d12->surfaces[i].texture &&
                (d3d12->surfaces[i].guest_address & ~UINT64_C(0xff)) ==
                (address & ~UINT64_C(0xff))) {
                return i;
            }
        }
    }
    return -1;
}

static void shadps4_d3d12_report_opcode_once(ShadPS4D3D12State *d3d12,
                                              uint32_t opcode)
{
    uint64_t mask = UINT64_C(1) << (opcode & 63);
    uint64_t *word = &d3d12->reported_opcodes[opcode >> 6];

    d3d12->unsupported_opcode_count++;
    if (!(*word & mask)) {
        *word |= mask;
        warn_report("shadPS4 D3D12: PM4 opcode %#x requires GCN/DXIL "
                    "translation and was not executed", opcode);
    }
}

static void shadps4_d3d12_report(const char *operation, HRESULT hr)
{
    warn_report("shadPS4 D3D12: %s failed: HRESULT=%#lx",
                operation, (unsigned long)hr);
}

static void shadps4_d3d12_release(ShadPS4D3D12State *d3d12)
{
    for (uint32_t i = 0; i < SHADPS4_D3D12_MAX_SURFACES; i++) {
        if (d3d12->surfaces[i].readback) {
            ID3D12Resource_Release(d3d12->surfaces[i].readback);
        }
        if (d3d12->surfaces[i].texture) {
            ID3D12Resource_Release(d3d12->surfaces[i].texture);
        }
        memset(&d3d12->surfaces[i], 0, sizeof(d3d12->surfaces[i]));
    }
    if (d3d12->rtv_heap) {
        ID3D12DescriptorHeap_Release(d3d12->rtv_heap);
        d3d12->rtv_heap = NULL;
    }
    if (d3d12->fence_event) {
        CloseHandle(d3d12->fence_event);
        d3d12->fence_event = NULL;
    }
    if (d3d12->fence) {
        ID3D12Fence_Release(d3d12->fence);
        d3d12->fence = NULL;
    }
    if (d3d12->list) {
        ID3D12GraphicsCommandList_Release(d3d12->list);
        d3d12->list = NULL;
    }
    if (d3d12->allocator) {
        ID3D12CommandAllocator_Release(d3d12->allocator);
        d3d12->allocator = NULL;
    }
    if (d3d12->queue) {
        ID3D12CommandQueue_Release(d3d12->queue);
        d3d12->queue = NULL;
    }
    if (d3d12->device) {
        ID3D12Device_Release(d3d12->device);
        d3d12->device = NULL;
    }
    d3d12->ready = false;
}

ShadPS4D3D12State *shadps4_d3d12_create(void)
{
    ShadPS4D3D12State *d3d12 = g_new0(ShadPS4D3D12State, 1);
    D3D12_COMMAND_QUEUE_DESC queue_desc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = SHADPS4_D3D12_MAX_SURFACES,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0,
    };
    HRESULT hr;
    uint64_t dxil_version;
    char *dxil_error = NULL;

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                           &IID_ID3D12Device, (void **)&d3d12->device);
    if (FAILED(hr)) {
        shadps4_d3d12_report("D3D12CreateDevice", hr);
        goto fail;
    }
    hr = ID3D12Device_CreateCommandQueue(
        d3d12->device, &queue_desc, &IID_ID3D12CommandQueue,
        (void **)&d3d12->queue);
    if (FAILED(hr)) {
        shadps4_d3d12_report("CreateCommandQueue", hr);
        goto fail;
    }
    hr = ID3D12Device_CreateCommandAllocator(
        d3d12->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&d3d12->allocator);
    if (FAILED(hr)) {
        shadps4_d3d12_report("CreateCommandAllocator", hr);
        goto fail;
    }
    hr = ID3D12Device_CreateCommandList(
        d3d12->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        d3d12->allocator, NULL, &IID_ID3D12GraphicsCommandList,
        (void **)&d3d12->list);
    if (FAILED(hr)) {
        shadps4_d3d12_report("CreateCommandList", hr);
        goto fail;
    }
    hr = ID3D12GraphicsCommandList_Close(d3d12->list);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Close initial command list", hr);
        goto fail;
    }
    hr = ID3D12Device_CreateFence(d3d12->device, 0,
                                  D3D12_FENCE_FLAG_NONE,
                                  &IID_ID3D12Fence,
                                  (void **)&d3d12->fence);
    if (FAILED(hr)) {
        shadps4_d3d12_report("CreateFence", hr);
        goto fail;
    }
    hr = ID3D12Device_CreateDescriptorHeap(
        d3d12->device, &heap_desc, &IID_ID3D12DescriptorHeap,
        (void **)&d3d12->rtv_heap);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create RTV descriptor heap", hr);
        goto fail;
    }
    d3d12->rtv_stride = ID3D12Device_GetDescriptorHandleIncrementSize(
        d3d12->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    d3d12->fence_event = CreateEventExW(NULL, NULL, 0,
                                        EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!d3d12->fence_event) {
        warn_report("shadPS4 D3D12: CreateEventExW failed: error=%lu",
                    GetLastError());
        goto fail;
    }
    d3d12->ready = true;
    if (shadps4_dxil_available(&dxil_version) &&
        shadps4_dxil_self_test(&dxil_error)) {
        info_report("shadPS4 D3D12 backend initialized: feature-level=11_0 "
                    "spirv-to-dxil=%#" PRIx64 " self-test=ok", dxil_version);
    } else {
        warn_report("shadPS4 D3D12: SPIR-V to DXIL translator unavailable: "
                    "%s; draw and dispatch translation is disabled",
                    dxil_error ?: "library not linked");
    }
    g_free(dxil_error);
    return d3d12;

fail:
    shadps4_d3d12_release(d3d12);
    g_free(d3d12);
    return NULL;
}

void shadps4_d3d12_destroy(ShadPS4D3D12State *d3d12)
{
    if (!d3d12) {
        return;
    }
    shadps4_d3d12_finish(d3d12);
    shadps4_d3d12_release(d3d12);
    g_free(d3d12);
}

bool shadps4_d3d12_reset(ShadPS4D3D12State *d3d12)
{
    if (!d3d12 || !d3d12->ready || !shadps4_d3d12_finish(d3d12)) {
        return false;
    }
    d3d12->submit_count = 0;
    d3d12->draw_count = 0;
    d3d12->dispatch_count = 0;
    d3d12->unmatched_target_count = 0;
    d3d12->unsupported_opcode_count = 0;
    d3d12->invalid_shader_count = 0;
    memset(d3d12->reported_opcodes, 0, sizeof(d3d12->reported_opcodes));
    for (uint32_t i = 0; i < SHADPS4_D3D12_MAX_SURFACES; i++) {
        d3d12->surfaces[i].rendered = false;
    }
    return true;
}

bool shadps4_d3d12_submit(ShadPS4D3D12State *d3d12,
                          CPUState *cs,
                          const uint32_t *commands,
                          uint32_t command_dwords, uint32_t queue_id,
                          const uint32_t *context_registers,
                          uint32_t context_register_count,
                          const uint32_t *shader_registers,
                          uint32_t shader_register_count)
{
    ID3D12CommandList *lists[1];
    uint32_t current_context[0x400];
    uint32_t current_shader[0x400];
    uint32_t offset = 0;
    uint64_t draws = 0;
    uint64_t dispatches = 0;
    HRESULT hr;

    if (!d3d12 || !d3d12->ready || !commands || !command_dwords ||
        queue_id >= 64 || !context_registers ||
        context_register_count > ARRAY_SIZE(current_context) ||
        !shader_registers ||
        shader_register_count > ARRAY_SIZE(current_shader)) {
        return false;
    }
    memcpy(current_context, context_registers,
           context_register_count * sizeof(*current_context));
    memcpy(current_shader, shader_registers,
           shader_register_count * sizeof(*current_shader));
    while (offset < command_dwords) {
        uint32_t header = le32_to_cpu(commands[offset]);
        uint32_t type = header >> 30;
        uint32_t payload_count;
        uint32_t opcode;

        if (type == 2) {
            offset++;
            continue;
        }
        if (type == 0 || type == 1) {
            payload_count = type == 0 ? ((header >> 16) & 0x3fff) + 1 : 2;
        } else if (type == SHADPS4_PM4_TYPE3) {
            payload_count = ((header >> 16) & 0x3fff) + 1;
            opcode = (header >> 8) & 0xff;
            if (payload_count > command_dwords - offset - 1) {
                return false;
            }
            if (opcode == SHADPS4_PM4_SET_CONTEXT_REG &&
                payload_count >= 2) {
                uint32_t reg = le32_to_cpu(commands[offset + 1]);
                uint32_t values = payload_count - 1;

                if (reg >= context_register_count ||
                    values > context_register_count - reg) {
                    return false;
                }
                for (uint32_t i = 0; i < values; i++) {
                    current_context[reg + i] =
                        le32_to_cpu(commands[offset + 2 + i]);
                }
            }
            if (opcode == SHADPS4_PM4_SET_SH_REG && payload_count >= 2) {
                uint32_t reg = le32_to_cpu(commands[offset + 1]);
                uint32_t values = payload_count - 1;

                if (reg >= shader_register_count ||
                    values > shader_register_count - reg) {
                    return false;
                }
                for (uint32_t i = 0; i < values; i++) {
                    current_shader[reg + i] =
                        le32_to_cpu(commands[offset + 2 + i]);
                }
            }
            if (shadps4_d3d12_is_draw(opcode)) {
                int target_index = shadps4_d3d12_find_color_target(
                    d3d12, current_context, context_register_count);

                draws++;
                shadps4_d3d12_validate_shader(
                    d3d12, cs, current_shader, shader_register_count,
                    SHADPS4_SHADER_VS_PGM_LO, "VS");
                shadps4_d3d12_validate_shader(
                    d3d12, cs, current_shader, shader_register_count,
                    SHADPS4_SHADER_PS_PGM_LO, "PS");
                shadps4_d3d12_report_opcode_once(d3d12, opcode);
                if (target_index < 0) {
                    d3d12->unmatched_target_count++;
                }
            } else if (shadps4_d3d12_is_dispatch(opcode)) {
                dispatches++;
                shadps4_d3d12_validate_shader(
                    d3d12, cs, current_shader, shader_register_count,
                    SHADPS4_SHADER_CS_PGM_LO, "CS");
                shadps4_d3d12_report_opcode_once(d3d12, opcode);
            }
        } else {
            return false;
        }
        if (payload_count > command_dwords - offset - 1) {
            return false;
        }
        offset += payload_count + 1;
    }
    if (d3d12->unmatched_target_count &&
        !(d3d12->unmatched_target_count &
          (d3d12->unmatched_target_count - 1))) {
        warn_report("shadPS4 D3D12: no registered VideoOut surface "
                    "matches CB_COLOR_BASE; unmatched=%" PRIu64,
                    d3d12->unmatched_target_count);
    }
    if (!shadps4_d3d12_finish(d3d12)) {
        return false;
    }
    hr = ID3D12CommandAllocator_Reset(d3d12->allocator);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Reset command allocator", hr);
        return false;
    }
    hr = ID3D12GraphicsCommandList_Reset(d3d12->list,
                                         d3d12->allocator, NULL);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Reset command list", hr);
        return false;
    }
    /* PM4 translation records D3D12 commands into this list.  Submitting an
     * empty list here establishes ordering and fence semantics immediately. */
    hr = ID3D12GraphicsCommandList_Close(d3d12->list);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Close command list", hr);
        return false;
    }
    lists[0] = (ID3D12CommandList *)d3d12->list;
    ID3D12CommandQueue_ExecuteCommandLists(d3d12->queue, 1, lists);
    d3d12->submit_count++;
    d3d12->draw_count += draws;
    d3d12->dispatch_count += dispatches;
    return true;
}

bool shadps4_d3d12_finish(ShadPS4D3D12State *d3d12)
{
    HRESULT hr;
    DWORD wait_result;

    if (!d3d12 || !d3d12->ready) {
        return false;
    }
    d3d12->fence_value++;
    hr = ID3D12CommandQueue_Signal(d3d12->queue, d3d12->fence,
                                   d3d12->fence_value);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Signal", hr);
        return false;
    }
    if (ID3D12Fence_GetCompletedValue(d3d12->fence) >=
        d3d12->fence_value) {
        return true;
    }
    hr = ID3D12Fence_SetEventOnCompletion(d3d12->fence,
                                          d3d12->fence_value,
                                          d3d12->fence_event);
    if (FAILED(hr)) {
        shadps4_d3d12_report("SetEventOnCompletion", hr);
        return false;
    }
    wait_result = WaitForSingleObjectEx(d3d12->fence_event, INFINITE, FALSE);
    if (wait_result != WAIT_OBJECT_0) {
        warn_report("shadPS4 D3D12: fence wait failed: result=%lu error=%lu",
                    wait_result, GetLastError());
        return false;
    }
    return true;
}

bool shadps4_d3d12_is_ready(const ShadPS4D3D12State *d3d12)
{
    return d3d12 && d3d12->ready;
}

bool shadps4_d3d12_register_surface(ShadPS4D3D12State *d3d12,
                                    uint32_t index, uint64_t guest_address,
                                    uint32_t width, uint32_t height,
                                    uint32_t pitch, uint32_t format,
                                    uint32_t tiling_mode)
{
    ShadPS4D3D12Surface *surface;
    D3D12_HEAP_PROPERTIES default_heap = {
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_HEAP_PROPERTIES readback_heap = default_heap;
    D3D12_RESOURCE_DESC texture_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };
    D3D12_CLEAR_VALUE clear_value = {
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .Color = { 0.0f, 0.0f, 0.0f, 1.0f },
    };
    D3D12_RESOURCE_DESC readback_desc;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    UINT rows;
    UINT64 row_size;
    HRESULT hr;

    if (!d3d12 || !d3d12->ready || index >= SHADPS4_D3D12_MAX_SURFACES ||
        !guest_address || !width || !height || pitch < width ||
        width > 4096 || height > 2160) {
        return false;
    }
    surface = &d3d12->surfaces[index];
    if (!shadps4_d3d12_finish(d3d12)) {
        return false;
    }
    if (surface->readback) {
        ID3D12Resource_Release(surface->readback);
    }
    if (surface->texture) {
        ID3D12Resource_Release(surface->texture);
    }
    memset(surface, 0, sizeof(*surface));
    hr = ID3D12Device_CreateCommittedResource(
        d3d12->device, &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
        &IID_ID3D12Resource, (void **)&surface->texture);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create VideoOut render target", hr);
        return false;
    }
    ID3D12Device_GetCopyableFootprints(
        d3d12->device, &texture_desc, 0, 1, 0, &surface->footprint,
        &rows, &row_size, &surface->readback_size);
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    readback_desc = (D3D12_RESOURCE_DESC) {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = surface->readback_size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    hr = ID3D12Device_CreateCommittedResource(
        d3d12->device, &readback_heap, D3D12_HEAP_FLAG_NONE,
        &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
        &IID_ID3D12Resource, (void **)&surface->readback);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Create VideoOut readback", hr);
        ID3D12Resource_Release(surface->texture);
        memset(surface, 0, sizeof(*surface));
        return false;
    }
    d3d12->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        d3d12->rtv_heap, &rtv);
    rtv.ptr += (SIZE_T)index * d3d12->rtv_stride;
    ID3D12Device_CreateRenderTargetView(d3d12->device, surface->texture,
                                        NULL, rtv);
    surface->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    surface->width = width;
    surface->height = height;
    surface->guest_address = guest_address;
    surface->pitch = pitch;
    surface->format = format;
    surface->tiling_mode = tiling_mode;
    info_report("shadPS4 D3D12: VideoOut surface=%u guest=%#" PRIx64
                " size=%ux%u pitch=%u format=%u tiling=%u",
                index, guest_address, width, height, pitch, format,
                tiling_mode);
    return true;
}

bool shadps4_d3d12_read_surface(ShadPS4D3D12State *d3d12,
                                uint32_t index, uint8_t *pixels,
                                uint32_t stride)
{
    ShadPS4D3D12Surface *surface;
    D3D12_RESOURCE_BARRIER barriers[2];
    D3D12_TEXTURE_COPY_LOCATION source;
    D3D12_TEXTURE_COPY_LOCATION destination;
    ID3D12CommandList *lists[1];
    D3D12_RANGE read_range;
    D3D12_RANGE write_range = { 0, 0 };
    uint8_t *mapped;
    HRESULT hr;

    if (!d3d12 || !d3d12->ready || index >= SHADPS4_D3D12_MAX_SURFACES ||
        !pixels) {
        return false;
    }
    surface = &d3d12->surfaces[index];
    if (!surface->texture || !surface->readback ||
        stride < surface->width * 4 || !surface->rendered ||
        !shadps4_d3d12_finish(d3d12)) {
        return false;
    }
    hr = ID3D12CommandAllocator_Reset(d3d12->allocator);
    if (FAILED(hr)) {
        return false;
    }
    hr = ID3D12GraphicsCommandList_Reset(d3d12->list,
                                         d3d12->allocator, NULL);
    if (FAILED(hr)) {
        return false;
    }
    barriers[0] = (D3D12_RESOURCE_BARRIER) {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource = surface->texture,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = surface->state,
            .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE,
        },
    };
    barriers[1] = barriers[0];
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, barriers);
    source = (D3D12_TEXTURE_COPY_LOCATION) {
        .pResource = surface->texture,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    destination = (D3D12_TEXTURE_COPY_LOCATION) {
        .pResource = surface->readback,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = surface->footprint,
    };
    ID3D12GraphicsCommandList_CopyTextureRegion(
        d3d12->list, &destination, 0, 0, 0, &source, NULL);
    ID3D12GraphicsCommandList_ResourceBarrier(d3d12->list, 1, &barriers[1]);
    hr = ID3D12GraphicsCommandList_Close(d3d12->list);
    if (FAILED(hr)) {
        return false;
    }
    lists[0] = (ID3D12CommandList *)d3d12->list;
    ID3D12CommandQueue_ExecuteCommandLists(d3d12->queue, 1, lists);
    if (!shadps4_d3d12_finish(d3d12)) {
        return false;
    }
    read_range = (D3D12_RANGE) { 0, surface->readback_size };
    hr = ID3D12Resource_Map(surface->readback, 0, &read_range,
                            (void **)&mapped);
    if (FAILED(hr)) {
        shadps4_d3d12_report("Map VideoOut readback", hr);
        return false;
    }
    for (uint32_t y = 0; y < surface->height; y++) {
        memcpy(pixels + (size_t)y * stride,
               mapped + surface->footprint.Offset +
               (size_t)y * surface->footprint.Footprint.RowPitch,
               surface->width * 4);
    }
    ID3D12Resource_Unmap(surface->readback, 0, &write_range);
    return true;
}
