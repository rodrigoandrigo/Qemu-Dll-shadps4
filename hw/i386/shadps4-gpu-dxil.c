/*
 * shadPS4 SPIR-V to DXIL bridge
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i386/shadps4-gpu-dxil.h"

#ifdef CONFIG_SHADPS4_SPIRV_TO_DXIL
#include "hw/i386/shadps4-spirv-to-dxil.h"

typedef struct ShadPS4DxcBlob ShadPS4DxcBlob;
typedef struct ShadPS4DxcOperationResult ShadPS4DxcOperationResult;
typedef struct ShadPS4DxcValidator ShadPS4DxcValidator;

typedef struct ShadPS4DxcBlobVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ShadPS4DxcBlob *, REFIID,
                                                void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(ShadPS4DxcBlob *);
    ULONG (STDMETHODCALLTYPE *Release)(ShadPS4DxcBlob *);
    void *(STDMETHODCALLTYPE *GetBufferPointer)(ShadPS4DxcBlob *);
    SIZE_T (STDMETHODCALLTYPE *GetBufferSize)(ShadPS4DxcBlob *);
} ShadPS4DxcBlobVtbl;

struct ShadPS4DxcBlob {
    const ShadPS4DxcBlobVtbl *lpVtbl;
    void *data;
    size_t size;
};

typedef struct ShadPS4DxcOperationResultVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ShadPS4DxcOperationResult *,
                                                REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(ShadPS4DxcOperationResult *);
    ULONG (STDMETHODCALLTYPE *Release)(ShadPS4DxcOperationResult *);
    HRESULT (STDMETHODCALLTYPE *GetStatus)(ShadPS4DxcOperationResult *,
                                           HRESULT *);
    HRESULT (STDMETHODCALLTYPE *GetResult)(ShadPS4DxcOperationResult *,
                                           ShadPS4DxcBlob **);
    HRESULT (STDMETHODCALLTYPE *GetErrorBuffer)(ShadPS4DxcOperationResult *,
                                                ShadPS4DxcBlob **);
} ShadPS4DxcOperationResultVtbl;

struct ShadPS4DxcOperationResult {
    const ShadPS4DxcOperationResultVtbl *lpVtbl;
};

typedef struct ShadPS4DxcValidatorVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ShadPS4DxcValidator *,
                                                REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(ShadPS4DxcValidator *);
    ULONG (STDMETHODCALLTYPE *Release)(ShadPS4DxcValidator *);
    HRESULT (STDMETHODCALLTYPE *Validate)(ShadPS4DxcValidator *,
                                          ShadPS4DxcBlob *, UINT32,
                                          ShadPS4DxcOperationResult **);
} ShadPS4DxcValidatorVtbl;

struct ShadPS4DxcValidator {
    const ShadPS4DxcValidatorVtbl *lpVtbl;
};

typedef HRESULT (WINAPI *ShadPS4DxcCreateInstance)(REFCLSID, REFIID, void **);
typedef HMODULE (WINAPI *ShadPS4LoadLibraryExW)(LPCWSTR, HANDLE, DWORD);

static const GUID shadps4_clsid_dxc_validator = {
    0x8ca3e215, 0xf728, 0x4cf3,
    { 0x8c, 0xdd, 0x88, 0xaf, 0x91, 0x75, 0x87, 0xa1 }
};
static const GUID shadps4_iid_dxc_validator = {
    0xa6e82bd2, 0x1fd7, 0x4826,
    { 0x98, 0x11, 0x28, 0x57, 0xe7, 0x97, 0xf4, 0x9a }
};

static HRESULT STDMETHODCALLTYPE shadps4_dxc_blob_query(
    ShadPS4DxcBlob *blob, REFIID iid, void **object)
{
    (void)blob;
    (void)iid;
    if (object) {
        *object = NULL;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE shadps4_dxc_blob_add_ref(ShadPS4DxcBlob *blob)
{
    (void)blob;
    return 1;
}

static ULONG STDMETHODCALLTYPE shadps4_dxc_blob_release(ShadPS4DxcBlob *blob)
{
    (void)blob;
    return 0;
}

static void *STDMETHODCALLTYPE shadps4_dxc_blob_pointer(ShadPS4DxcBlob *blob)
{
    return blob->data;
}

static SIZE_T STDMETHODCALLTYPE shadps4_dxc_blob_size(ShadPS4DxcBlob *blob)
{
    return blob->size;
}

static const ShadPS4DxcBlobVtbl shadps4_dxc_blob_vtbl = {
    shadps4_dxc_blob_query,
    shadps4_dxc_blob_add_ref,
    shadps4_dxc_blob_release,
    shadps4_dxc_blob_pointer,
    shadps4_dxc_blob_size,
};

static HMODULE shadps4_dxil_load_validator_module(void)
{
    static const wchar_t *names[] = {
#if defined(_GAMING_XBOX_SCARLETT)
        L"dxcompiler_xs.dll",
#elif defined(_GAMING_XBOX)
        L"dxcompiler_x.dll",
#else
        L"DXIL.dll",
#endif
    };
    HMODULE module = NULL;

    for (size_t i = 0; i < ARRAY_SIZE(names) && !module; i++) {
        module = GetModuleHandleW(names[i]);
        if (!module) {
            SetLastError(ERROR_SUCCESS);
            module = LoadPackagedLibrary(names[i], 0);
        }
        if (!module && GetLastError() == APPMODEL_ERROR_NO_PACKAGE) {
            HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
            ShadPS4LoadLibraryExW load_library = kernel ?
                (ShadPS4LoadLibraryExW)GetProcAddress(
                    kernel, "LoadLibraryExW") : NULL;

            if (load_library) {
                module = load_library(names[i], NULL,
                                      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            }
        }
    }
    return module;
}

static bool shadps4_dxil_validate(void *data, size_t size, char **error)
{
    static ShadPS4DxcValidator *validator;
    static bool attempted;
    static GMutex lock;
    ShadPS4DxcOperationResult *result = NULL;
    ShadPS4DxcBlob blob = {
        .lpVtbl = &shadps4_dxc_blob_vtbl,
        .data = data,
        .size = size,
    };
    HRESULT status = E_FAIL;
    HRESULT hr;

    g_mutex_lock(&lock);
    if (!attempted) {
        HMODULE module = shadps4_dxil_load_validator_module();
        ShadPS4DxcCreateInstance create = module ?
            (ShadPS4DxcCreateInstance)GetProcAddress(
                module, "DxcCreateInstance") : NULL;

        attempted = true;
        if (create) {
            create(&shadps4_clsid_dxc_validator,
                   &shadps4_iid_dxc_validator, (void **)&validator);
        }
    }
    if (!validator) {
        g_mutex_unlock(&lock);
        if (error) {
            *error = g_strdup("DXIL validator is unavailable; package "
                              "DXIL.dll with the application");
        }
        return false;
    }
    hr = validator->lpVtbl->Validate(validator, &blob, 1, &result);
    if (SUCCEEDED(hr) && result) {
        hr = result->lpVtbl->GetStatus(result, &status);
    }
    if (result) {
        result->lpVtbl->Release(result);
    }
    g_mutex_unlock(&lock);
    if (FAILED(hr) || FAILED(status)) {
        if (error) {
            *error = g_strdup_printf(
                "DXIL validation failed: call=%#lx status=%#lx",
                (unsigned long)hr, (unsigned long)status);
        }
        return false;
    }
    return true;
}

static void shadps4_dxil_log(void *opaque, const char *message)
{
    GString *log = opaque;

    if (!message || !*message) {
        return;
    }
    if (log->len) {
        g_string_append_c(log, '\n');
    }
    g_string_append(log, message);
}
#endif

bool shadps4_dxil_available(uint64_t *version)
{
#ifdef CONFIG_SHADPS4_SPIRV_TO_DXIL
    uint64_t detected = spirv_to_dxil_get_version();

    if (version) {
        *version = detected;
    }
    return true;
#else
    if (version) {
        *version = 0;
    }
    return false;
#endif
}

bool shadps4_dxil_compile(const uint32_t *spirv, size_t word_count,
                          ShadPS4DxilStage stage,
                          ShadPS4DxilBinary *binary, char **error)
{
    if (error) {
        *error = NULL;
    }
    if (!binary) {
        return false;
    }
    shadps4_dxil_binary_clear(binary);
    if (!spirv || word_count < 5 || spirv[0] != UINT32_C(0x07230203) ||
        stage < SHADPS4_DXIL_STAGE_VERTEX ||
        stage > SHADPS4_DXIL_STAGE_COMPUTE) {
        if (error) {
            *error = g_strdup("invalid SPIR-V module or shader stage");
        }
        return false;
    }

#ifdef CONFIG_SHADPS4_SPIRV_TO_DXIL
    struct dxil_spirv_runtime_conf conf = {
        .runtime_data_cbv = {
            .register_space = 31,
            .base_shader_register = 0,
        },
        .push_constant_cbv = {
            .register_space = 30,
            .base_shader_register = 0,
        },
        .first_vertex_and_base_instance_mode = DXIL_SPIRV_SYSVAL_TYPE_ZERO,
        .workgroup_id_mode = DXIL_SPIRV_SYSVAL_TYPE_NATIVE,
        .declared_read_only_images_as_srvs = true,
        .inferred_read_only_images_as_srvs = true,
        .shader_model_max = SHADER_MODEL_6_2,
    };
    struct dxil_spirv_debug_options debug = { 0 };
    struct dxil_spirv_object object = { 0 };
    GString *messages = g_string_new(NULL);
    struct dxil_spirv_logger logger = {
        .priv = messages,
        .log = shadps4_dxil_log,
    };
    bool compiled;

    compiled = spirv_to_dxil(spirv, word_count, NULL, 0,
                             (dxil_spirv_shader_stage)stage, "main",
                             DXIL_VALIDATOR_1_4, &debug, &conf, &logger,
                             &object);
    if (!compiled || !object.binary.buffer || !object.binary.size) {
        if (error) {
            *error = g_string_free(messages, false);
            if (!**error) {
                g_free(*error);
                *error = g_strdup("SPIR-V to DXIL translation failed");
            }
        } else {
            g_string_free(messages, true);
        }
        spirv_to_dxil_free(&object);
        return false;
    }

    binary->data = g_memdup2(object.binary.buffer, object.binary.size);
    binary->size = object.binary.size;
    binary->requires_runtime_data = object.metadata.requires_runtime_data;
    binary->needs_draw_sysvals = object.metadata.needs_draw_sysvals;
    spirv_to_dxil_free(&object);
    g_string_free(messages, true);
    if (!shadps4_dxil_validate(binary->data, binary->size, error)) {
        shadps4_dxil_binary_clear(binary);
        return false;
    }
    return true;
#else
    if (error) {
        *error = g_strdup("SPIR-V to DXIL translator is not linked");
    }
    return false;
#endif
}

bool shadps4_dxil_self_test(char **error)
{
    static const uint32_t compute_spirv[] = {
        0x07230203, 0x00010000, 0, 5, 0,
        0x00020011, 1,
        0x0003000e, 0, 1,
        0x0005000f, 5, 3, 0x6e69616d, 0,
        0x00060010, 3, 17, 1, 1, 1,
        0x00020013, 1,
        0x00030021, 2, 1,
        0x00050036, 1, 3, 0, 2,
        0x000200f8, 4,
        0x000100fd,
        0x00010038,
    };
    ShadPS4DxilBinary binary = { 0 };
    bool valid;

    if (!shadps4_dxil_compile(compute_spirv, ARRAY_SIZE(compute_spirv),
                              SHADPS4_DXIL_STAGE_COMPUTE, &binary, error)) {
        return false;
    }
    valid = binary.size >= 4 && !memcmp(binary.data, "DXBC", 4);
    if (!valid && error) {
        *error = g_strdup("translator returned an invalid DXIL container");
    }
    shadps4_dxil_binary_clear(&binary);
    return valid;
}

void shadps4_dxil_binary_clear(ShadPS4DxilBinary *binary)
{
    if (!binary) {
        return;
    }
    g_free(binary->data);
    memset(binary, 0, sizeof(*binary));
}
