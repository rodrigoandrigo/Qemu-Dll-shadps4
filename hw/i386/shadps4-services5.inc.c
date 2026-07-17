/* UWP-safe HLE for decoder, dialogs, SSL compatibility and VR tracking. */

#define SHADPS4_COMMON_DIALOG_NOT_INITIALIZED 0x80b80003u
#define SHADPS4_COMMON_DIALOG_ALREADY_INITIALIZED 0x80b80004u
#define SHADPS4_COMMON_DIALOG_NOT_FINISHED 0x80b80005u
#define SHADPS4_COMMON_DIALOG_INVALID_STATE 0x80b80006u
#define SHADPS4_COMMON_DIALOG_PARAM_INVALID 0x80b8000au
#define SHADPS4_COMMON_DIALOG_NOT_RUNNING 0x80b8000bu
#define SHADPS4_COMMON_DIALOG_ALREADY_CLOSE 0x80b8000cu
#define SHADPS4_COMMON_DIALOG_ARG_NULL 0x80b8000du

#define SHADPS4_VIDEODEC2_API_FAIL 0x811d0100u
#define SHADPS4_VIDEODEC2_STRUCT_SIZE 0x811d0101u
#define SHADPS4_VIDEODEC2_ARGUMENT_POINTER 0x811d0102u
#define SHADPS4_VIDEODEC2_DECODER_INSTANCE 0x811d0103u
#define SHADPS4_VIDEODEC2_MEMORY_POINTER 0x811d0105u
#define SHADPS4_VIDEODEC2_CONFIG_INFO 0x811d0200u
#define SHADPS4_VIDEODEC2_COMPUTE_PIPE_ID 0x811d020au
#define SHADPS4_VIDEODEC2_COMPUTE_QUEUE_ID 0x811d020bu

#define SHADPS4_VR_NOT_INIT 0x81260801u
#define SHADPS4_VR_ALREADY_INITIALIZED 0x81260802u
#define SHADPS4_VR_DEVICE_NOT_REGISTERED 0x81260803u
#define SHADPS4_VR_DEVICE_ALREADY_REGISTERED 0x81260804u
#define SHADPS4_VR_ARGUMENT_INVALID 0x81260806u
#define SHADPS4_VR_NOT_EXECUTE_GPU_SUBMIT 0x81260815u

static bool shadps4_services5_rw(CPUState *cs, uint64_t address, void *data,
                                 size_t size, bool write)
{
    return address && shadps4_guest_rw(cs, address, data, size, write);
}

static uint64_t shadps4_services5_videodec2(ShadPS4HLEState *hle,
                                             CPUState *cs, uint64_t number,
                                             uint64_t a0, uint64_t a1,
                                             uint64_t a2, uint64_t a3)
{
    uint8_t first[0x48] = { 0 };
    uint8_t second[0x48] = { 0 };
    int handle;

    (void)a3;
    switch (number) {
    case SHADPS4_HLE_VIDEODEC2_QUERY_COMPUTE:
        if (!shadps4_services5_rw(cs, a0, first, 0x18, false)) {
            return SHADPS4_VIDEODEC2_ARGUMENT_POINTER;
        }
        if (ldq_le_p(first) != 0x18) {
            return SHADPS4_VIDEODEC2_STRUCT_SIZE;
        }
        stq_le_p(first + 8, 16 * MiB);
        stq_le_p(first + 16, 0);
        return shadps4_services5_rw(cs, a0, first, 0x18, true) ? 0 :
               SHADPS4_VIDEODEC2_ARGUMENT_POINTER;
    case SHADPS4_HLE_VIDEODEC2_ALLOCATE_QUEUE:
        if (!shadps4_services5_rw(cs, a0, first, 0x10, false) ||
            !shadps4_services5_rw(cs, a1, second, 0x18, false) || !a2) {
            return SHADPS4_VIDEODEC2_ARGUMENT_POINTER;
        }
        if (ldq_le_p(first) != 0x10 || ldq_le_p(second) != 0x18) {
            return SHADPS4_VIDEODEC2_STRUCT_SIZE;
        }
        if (first[13] || lduw_le_p(first + 14)) {
            return SHADPS4_VIDEODEC2_CONFIG_INFO;
        }
        if (lduw_le_p(first + 8) > 4) {
            return SHADPS4_VIDEODEC2_COMPUTE_PIPE_ID;
        }
        if (lduw_le_p(first + 10) > 7) {
            return SHADPS4_VIDEODEC2_COMPUTE_QUEUE_ID;
        }
        if (!ldq_le_p(second + 16)) {
            return SHADPS4_VIDEODEC2_MEMORY_POINTER;
        }
        stq_le_p(first, ldq_le_p(second + 16));
        return shadps4_services5_rw(cs, a2, first, 8, true) ? 0 :
               SHADPS4_VIDEODEC2_ARGUMENT_POINTER;
    case SHADPS4_HLE_VIDEODEC2_RELEASE_QUEUE:
        return 0;
    case SHADPS4_HLE_VIDEODEC2_QUERY_DECODER:
        if (!shadps4_services5_rw(cs, a0, first, 0x48, false) ||
            !shadps4_services5_rw(cs, a1, second, 0x48, false)) {
            return SHADPS4_VIDEODEC2_ARGUMENT_POINTER;
        }
        if (ldq_le_p(first) != 0x48 || ldq_le_p(second) != 0x48) {
            return SHADPS4_VIDEODEC2_STRUCT_SIZE;
        }
        stq_le_p(second + 8, 16 * MiB);
        stq_le_p(second + 16, 0);
        stq_le_p(second + 24, 16 * MiB);
        stq_le_p(second + 32, 0);
        stq_le_p(second + 40, 16 * MiB);
        stq_le_p(second + 48, 0);
        stq_le_p(second + 56, 16 * MiB);
        stl_le_p(second + 64, 0x100);
        return shadps4_services5_rw(cs, a1, second, 0x48, true) ? 0 :
               SHADPS4_VIDEODEC2_ARGUMENT_POINTER;
    case SHADPS4_HLE_VIDEODEC2_CREATE:
        if (!shadps4_services5_rw(cs, a0, first, 0x48, false) ||
            !shadps4_services5_rw(cs, a1, second, 0x48, false) || !a2) {
            return SHADPS4_VIDEODEC2_ARGUMENT_POINTER;
        }
        if (ldq_le_p(first) != 0x48 || ldq_le_p(second) != 0x48) {
            return SHADPS4_VIDEODEC2_STRUCT_SIZE;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_VIDEODEC2_DECODER, 0);
        if (handle < 0) {
            return SHADPS4_VIDEODEC2_API_FAIL;
        }
        stq_le_p(first, handle);
        if (!shadps4_services5_rw(cs, a2, first, 8, true)) {
            shadps4_hle_service_delete(
                hle, handle, SHADPS4_SERVICE_VIDEODEC2_DECODER);
            return SHADPS4_VIDEODEC2_ARGUMENT_POINTER;
        }
        return 0;
    case SHADPS4_HLE_VIDEODEC2_DELETE:
        return shadps4_hle_service_is(
                   hle, a0, SHADPS4_SERVICE_VIDEODEC2_DECODER) ?
               shadps4_hle_service_delete(
                   hle, a0, SHADPS4_SERVICE_VIDEODEC2_DECODER) :
               SHADPS4_VIDEODEC2_DECODER_INSTANCE;
    case SHADPS4_HLE_VIDEODEC2_RESET:
        return shadps4_hle_service_is(
                   hle, a0, SHADPS4_SERVICE_VIDEODEC2_DECODER) ? 0 :
               SHADPS4_VIDEODEC2_DECODER_INSTANCE;
    case SHADPS4_HLE_VIDEODEC2_DECODE:
        if (!shadps4_hle_service_is(
                hle, a0, SHADPS4_SERVICE_VIDEODEC2_DECODER)) {
            return SHADPS4_VIDEODEC2_DECODER_INSTANCE;
        }
        if (!shadps4_services5_rw(cs, a1, first, 0x30, false) ||
            !shadps4_services5_rw(cs, a2, second, 0x20, false) || !a3) {
            return SHADPS4_VIDEODEC2_ARGUMENT_POINTER;
        }
        if (ldq_le_p(first) != 0x30 || ldq_le_p(second) != 0x20) {
            return SHADPS4_VIDEODEC2_STRUCT_SIZE;
        }
        second[24] = 0;
        shadps4_services5_rw(cs, a2, second, 0x20, true);
        return SHADPS4_VIDEODEC2_API_FAIL;
    case SHADPS4_HLE_VIDEODEC2_FLUSH:
        if (!shadps4_hle_service_is(
                hle, a0, SHADPS4_SERVICE_VIDEODEC2_DECODER)) {
            return SHADPS4_VIDEODEC2_DECODER_INSTANCE;
        }
        if (!shadps4_services5_rw(cs, a1, first, 0x20, false) ||
            !shadps4_services5_rw(cs, a2, second, 8, false)) {
            return SHADPS4_VIDEODEC2_ARGUMENT_POINTER;
        }
        if (ldq_le_p(first) != 0x20 || (ldq_le_p(second) | 8) != 0x38) {
            return SHADPS4_VIDEODEC2_STRUCT_SIZE;
        }
        first[24] = 0;
        shadps4_services5_rw(cs, a1, first, 0x20, true);
        return SHADPS4_VIDEODEC2_API_FAIL;
    case SHADPS4_HLE_VIDEODEC2_GET_AVC_INFO:
    case SHADPS4_HLE_VIDEODEC2_GET_INFO:
        if (!shadps4_services5_rw(cs, a0, first, 0x38, false)) {
            return SHADPS4_VIDEODEC2_ARGUMENT_POINTER;
        }
        if ((ldq_le_p(first) | 8) != 0x38) {
            return SHADPS4_VIDEODEC2_STRUCT_SIZE;
        }
        return first[10] ? SHADPS4_VIDEODEC2_API_FAIL : 0;
    default:
        return SHADPS4_VIDEODEC2_API_FAIL;
    }
}

static uint64_t shadps4_services5_dialog_init(ShadPS4HLEState *hle,
                                               uint32_t kind)
{
    if (hle->dialog_status != 0) {
        return SHADPS4_COMMON_DIALOG_ALREADY_INITIALIZED;
    }
    hle->dialog_status = 1;
    hle->dialog_kind = kind;
    return 0;
}

static uint64_t shadps4_services5_dialog_open(ShadPS4HLEState *hle,
                                               CPUState *cs, uint64_t param,
                                               uint32_t kind)
{
    if (hle->dialog_status != 1 && hle->dialog_status != 3) {
        return SHADPS4_COMMON_DIALOG_INVALID_STATE;
    }
    if (!param) {
        return SHADPS4_COMMON_DIALOG_ARG_NULL;
    }
    hle->dialog_kind = kind;
    hle->dialog_result_status = 1;
    hle->dialog_result_size = 0;
    return shadps4_hle_dialog_open(hle, cs, param, kind);
}

static uint64_t shadps4_services5_dialog_close(ShadPS4HLEState *hle)
{
    if (!hle->dialog_status) {
        return SHADPS4_COMMON_DIALOG_NOT_INITIALIZED;
    }
    if (hle->dialog_status == 3) {
        return SHADPS4_COMMON_DIALOG_ALREADY_CLOSE;
    }
    if (hle->dialog_status != 2) {
        return SHADPS4_COMMON_DIALOG_NOT_RUNNING;
    }
    hle->dialog_status = 3;
    return 0;
}

static uint64_t shadps4_services5_dialog_term(ShadPS4HLEState *hle)
{
    if (!hle->dialog_status) {
        return SHADPS4_COMMON_DIALOG_NOT_INITIALIZED;
    }
    hle->dialog_status = 0;
    hle->dialog_kind = 0;
    hle->dialog_request_id = 0;
    return 0;
}

static uint64_t shadps4_services5_commerce(ShadPS4HLEState *hle,
                                            CPUState *cs, uint64_t number,
                                            uint64_t a0)
{
    uint8_t data[0x80] = { 0 };

    switch (number) {
    case SHADPS4_HLE_COMMERCE_INIT:
        return shadps4_services5_dialog_init(hle, 4);
    case SHADPS4_HLE_COMMERCE_TERM:
        return shadps4_services5_dialog_term(hle);
    case SHADPS4_HLE_COMMERCE_CLOSE:
        return shadps4_services5_dialog_close(hle);
    case SHADPS4_HLE_COMMERCE_STATUS:
    case SHADPS4_HLE_COMMERCE_UPDATE:
        shadps4_hle_dialog_poll(hle);
        return hle->dialog_status;
    case SHADPS4_HLE_COMMERCE_OPEN:
        if (hle->dialog_status != 1 && hle->dialog_status != 3) {
            return SHADPS4_COMMON_DIALOG_NOT_INITIALIZED;
        }
        if (!shadps4_services5_rw(cs, a0, data, sizeof(data), false)) {
            return SHADPS4_COMMON_DIALOG_ARG_NULL;
        }
        if (ldl_le_p(data + 56) > 5) {
            hle->dialog_status = 3;
            return SHADPS4_COMMON_DIALOG_PARAM_INVALID;
        }
        hle->dialog_user_data = ldq_le_p(data + 88);
        return shadps4_services5_dialog_open(hle, cs, a0, 4);
    case SHADPS4_HLE_COMMERCE_RESULT:
        shadps4_hle_dialog_poll(hle);
        if (!a0) {
            return SHADPS4_COMMON_DIALOG_ARG_NULL;
        }
        if (hle->dialog_status != 3) {
            return SHADPS4_COMMON_DIALOG_NOT_FINISHED;
        }
        memset(data, 0, 0x30);
        stl_le_p(data, hle->dialog_result_status ? 1 : 0);
        stq_le_p(data + 8, hle->dialog_user_data);
        return shadps4_services5_rw(cs, a0, data, 0x30, true) ?
               (uint32_t)ldl_le_p(data) : SHADPS4_COMMON_DIALOG_ARG_NULL;
    case SHADPS4_HLE_COMMERCE_HIDE_ICON:
        hle->commerce_icon_visible = false;
        return 0;
    case SHADPS4_HLE_COMMERCE_ICON_LAYOUT:
        if ((int32_t)a0 >= 0 && a0 <= 2) {
            hle->commerce_icon_layout = a0;
        }
        return 0;
    case SHADPS4_HLE_COMMERCE_SHOW_ICON:
        hle->commerce_icon_visible = true;
        hle->commerce_icon_position = a0 <= 2 ? a0 : 1;
        return 0;
    default:
        return SHADPS4_COMMON_DIALOG_PARAM_INVALID;
    }
}

static uint64_t shadps4_services5_ime_dialog(ShadPS4HLEState *hle,
                                              CPUState *cs, uint64_t number,
                                              uint64_t a0, uint64_t a1,
                                              uint64_t a2, uint64_t a3)
{
    uint8_t data[28] = { 0 };
    float value;

    switch (number) {
    case SHADPS4_HLE_IME_DIALOG_INIT_INTERNAL:
    case SHADPS4_HLE_IME_DIALOG_INIT_INTERNAL2:
    case SHADPS4_HLE_IME_DIALOG_INIT_INTERNAL3:
        if (!hle->dialog_status) {
            hle->dialog_status = 1;
        }
        return shadps4_hle_dialog_open(hle, cs, a0, 3);
    case SHADPS4_HLE_IME_DIALOG_FORCE_CLOSE:
        if (hle->dialog_kind != 3 || !hle->dialog_status) {
            return 0x80bc0107u;
        }
        hle->dialog_status = 3;
        return 0;
    case SHADPS4_HLE_IME_DIALOG_TEST:
        return 0x80bc00ffu;
    case SHADPS4_HLE_IME_DIALOG_GET_STAR:
        if (hle->dialog_kind != 3 || !hle->dialog_status) {
            return 0x80bc0107u;
        }
        stl_le_p(data, 0);
        return shadps4_services5_rw(cs, a0, data, 4, true) ? 0 :
               0x80bc0031u;
    case SHADPS4_HLE_IME_DIALOG_SET_POSITION:
        if (hle->dialog_kind != 3 || !hle->dialog_status) {
            return 0x80bc0107u;
        }
        if ((int32_t)a0 < 0 || a0 >= 1920) {
            return 0x80bc0018u;
        }
        if ((int32_t)a1 < 0 || a1 >= 1080) {
            return 0x80bc0019u;
        }
        hle->ime_dialog_pos_x = a0;
        hle->ime_dialog_pos_y = a1;
        return 0;
    case SHADPS4_HLE_IME_DIALOG_GET_POSITION:
        if (hle->dialog_kind != 3 || !hle->dialog_status) {
            return 0x80bc0107u;
        }
        stl_le_p(data, 2);
        value = hle->ime_dialog_pos_x;
        memcpy(data + 4, &value, 4);
        value = hle->ime_dialog_pos_y;
        memcpy(data + 8, &value, 4);
        stl_le_p(data + 20, 0x319);
        stl_le_p(data + 24, 0x198);
        return shadps4_services5_rw(cs, a0, data, sizeof(data), true) ? 0 :
               0x80bc0031u;
    case SHADPS4_HLE_IME_DIALOG_GET_SIZE:
        return shadps4_services4_ime(hle, cs, SHADPS4_HLE_IME_GET_PANEL_SIZE,
                                     a0, a1, a2, 0, 0);
    case SHADPS4_HLE_IME_DIALOG_GET_SIZE_EXT:
        return shadps4_services4_ime(hle, cs, SHADPS4_HLE_IME_GET_PANEL_SIZE,
                                     a0, a2, a3, 0, 0);
    default:
        return 0x80bc0030u;
    }
}

static uint64_t shadps4_services5_invitation(ShadPS4HLEState *hle,
                                              CPUState *cs, uint64_t number,
                                              uint64_t a0)
{
    uint8_t data[0x90] = { 0 };

    switch (number) {
    case SHADPS4_HLE_INVITATION_INIT:
        return shadps4_services5_dialog_init(hle, 5);
    case SHADPS4_HLE_INVITATION_TERM:
        return shadps4_services5_dialog_term(hle);
    case SHADPS4_HLE_INVITATION_CLOSE:
        if (hle->dialog_status != 2) {
            return SHADPS4_COMMON_DIALOG_NOT_RUNNING;
        }
        hle->dialog_status = 3;
        return 0;
    case SHADPS4_HLE_INVITATION_STATUS:
    case SHADPS4_HLE_INVITATION_UPDATE:
        shadps4_hle_dialog_poll(hle);
        return hle->dialog_status;
    case SHADPS4_HLE_INVITATION_OPEN:
    case SHADPS4_HLE_INVITATION_OPEN_A:
        if (number == SHADPS4_HLE_INVITATION_OPEN &&
            hle->dialog_status != 1 && hle->dialog_status != 3) {
            return SHADPS4_COMMON_DIALOG_INVALID_STATE;
        }
        if (!shadps4_services5_rw(cs, a0, data, 80, false)) {
            return SHADPS4_COMMON_DIALOG_ARG_NULL;
        }
        if (ldl_le_p(data + 52) < 1 || ldl_le_p(data + 52) > 2) {
            return SHADPS4_COMMON_DIALOG_PARAM_INVALID;
        }
        hle->dialog_user_data = ldq_le_p(data + 64);
        return shadps4_services5_dialog_open(hle, cs, a0, 5);
    case SHADPS4_HLE_INVITATION_RESULT:
    case SHADPS4_HLE_INVITATION_RESULT_A:
        shadps4_hle_dialog_poll(hle);
        if (!hle->dialog_status) {
            return SHADPS4_COMMON_DIALOG_NOT_INITIALIZED;
        }
        if (hle->dialog_status != 3) {
            return SHADPS4_COMMON_DIALOG_NOT_FINISHED;
        }
        if (!a0) {
            return SHADPS4_COMMON_DIALOG_ARG_NULL;
        }
        memset(data, 0, 0x38);
        stq_le_p(data, hle->dialog_user_data);
        stl_le_p(data + 12, hle->dialog_result_status ? 1 : 0);
        return shadps4_services5_rw(cs, a0, data, 0x38, true) ?
               (hle->dialog_result_status ? 1 : 0) :
               SHADPS4_COMMON_DIALOG_ARG_NULL;
    default:
        return SHADPS4_COMMON_DIALOG_PARAM_INVALID;
    }
}

static uint64_t shadps4_services5_playgo_dialog(CPUState *cs,
                                                 uint64_t number,
                                                 uint64_t a0)
{
    uint8_t data[0x68] = { 0 };

    switch (number) {
    case SHADPS4_HLE_PLAYGO_DIALOG_RESULT:
        if (!a0) {
            return SHADPS4_COMMON_DIALOG_ARG_NULL;
        }
        memset(data, 0, 0x28);
        stl_le_p(data + 4, 3);
        return shadps4_services5_rw(cs, a0, data, 0x28, true) ? 0 :
               SHADPS4_COMMON_DIALOG_ARG_NULL;
    case SHADPS4_HLE_PLAYGO_DIALOG_OPEN:
        if (!shadps4_services5_rw(cs, a0, data, sizeof(data), false)) {
            return SHADPS4_COMMON_DIALOG_ARG_NULL;
        }
        return ldl_le_p(data + 48) == 0x68 ? 0 :
               SHADPS4_COMMON_DIALOG_PARAM_INVALID;
    case SHADPS4_HLE_PLAYGO_DIALOG_STATUS:
    case SHADPS4_HLE_PLAYGO_DIALOG_UPDATE:
        return 0;
    default:
        return 0;
    }
}

static uint64_t shadps4_services5_profile_dialog(ShadPS4HLEState *hle,
                                                  CPUState *cs,
                                                  uint64_t number,
                                                  uint64_t a0)
{
    uint8_t data[0x70] = { 0 };

    switch (number) {
    case SHADPS4_HLE_PROFILE_DIALOG_INIT:
        return shadps4_services5_dialog_init(hle, 6);
    case SHADPS4_HLE_PROFILE_DIALOG_TERM:
        return shadps4_services5_dialog_term(hle);
    case SHADPS4_HLE_PROFILE_DIALOG_CLOSE:
        return shadps4_services5_dialog_close(hle);
    case SHADPS4_HLE_PROFILE_DIALOG_STATUS:
    case SHADPS4_HLE_PROFILE_DIALOG_UPDATE:
        shadps4_hle_dialog_poll(hle);
        return hle->dialog_status;
    case SHADPS4_HLE_PROFILE_DIALOG_OPEN:
        if (hle->dialog_status != 1 && hle->dialog_status != 3) {
            return SHADPS4_COMMON_DIALOG_INVALID_STATE;
        }
        if (!shadps4_services5_rw(cs, a0, data, 0x70, false)) {
            return SHADPS4_COMMON_DIALOG_ARG_NULL;
        }
        if (ldl_le_p(data + 56) < 1 || ldl_le_p(data + 56) > 4) {
            return SHADPS4_COMMON_DIALOG_PARAM_INVALID;
        }
        hle->dialog_user_data = ldq_le_p(data + 88);
        return shadps4_services5_dialog_open(hle, cs, a0, 6);
    case SHADPS4_HLE_PROFILE_DIALOG_RESULT:
        shadps4_hle_dialog_poll(hle);
        if (!hle->dialog_status) {
            return SHADPS4_COMMON_DIALOG_NOT_INITIALIZED;
        }
        if (!a0) {
            return SHADPS4_COMMON_DIALOG_ARG_NULL;
        }
        if (hle->dialog_status != 3) {
            return SHADPS4_COMMON_DIALOG_NOT_FINISHED;
        }
        memset(data, 0, 0x30);
        stl_le_p(data + 4, hle->dialog_result_status ? 1 : 0);
        stq_le_p(data + 8, hle->dialog_user_data);
        return shadps4_services5_rw(cs, a0, data, 0x30, true) ? 0 :
               SHADPS4_COMMON_DIALOG_ARG_NULL;
    default:
        return SHADPS4_COMMON_DIALOG_PARAM_INVALID;
    }
}

static uint64_t shadps4_services5_vr(ShadPS4HLEState *hle, CPUState *cs,
                                     uint64_t number, uint64_t a0,
                                     uint64_t a1, uint64_t a2,
                                     uint64_t a3)
{
    uint8_t data[128] = { 0 };
    uint32_t type;
    uint32_t move_calibration;

    switch (number) {
    case SHADPS4_HLE_VR_TRACKER_QUERY_MEMORY:
        if (!shadps4_services5_rw(cs, a0, data, 64, false) || !a1) {
            return SHADPS4_VR_ARGUMENT_INVALID;
        }
        if (ldl_le_p(data) != 64 ||
            (ldl_le_p(data + 4) != 0 && ldl_le_p(data + 4) != 100) ||
            ldl_le_p(data + 32) > 1 || ldl_le_p(data + 36) > 1 ||
            ldl_le_p(data + 40) > 1 || ldl_le_p(data + 44) > 1) {
            return SHADPS4_VR_ARGUMENT_INVALID;
        }
        move_calibration = ldl_le_p(data + 40);
        memset(data, 0, 64);
        stl_le_p(data, 64);
        stl_le_p(data + 4, move_calibration == 1 ? 0x800000 : 0x400000);
        stl_le_p(data + 8, 0x10000);
        stl_le_p(data + 12, 0x3000000);
        stl_le_p(data + 16, 0x10000);
        stl_le_p(data + 20, 0x1000000);
        stl_le_p(data + 24, 0x10000);
        return shadps4_services5_rw(cs, a1, data, 64, true) ? 0 :
               SHADPS4_VR_ARGUMENT_INVALID;
    case SHADPS4_HLE_VR_TRACKER_INIT:
        if (hle->vr_tracker_initialized) {
            return SHADPS4_VR_ALREADY_INITIALIZED;
        }
        if (!shadps4_services5_rw(cs, a0, data, sizeof(data), false) ||
            ldl_le_p(data) != sizeof(data) ||
            (ldl_le_p(data + 4) != 0 && ldl_le_p(data + 4) != 100) ||
            ldl_le_p(data + 8) > 1 || ldl_le_p(data + 40) > 0 ||
            ldl_le_p(data + 44) > 1 || ldl_le_p(data + 48) > 1 ||
            ldl_le_p(data + 52) > 1 || !ldq_le_p(data + 72) ||
            ldl_le_p(data + 80) !=
                (ldl_le_p(data + 48) == 1 ? 0x800000 : 0x400000) ||
            ldl_le_p(data + 84) != 0x10000 || !ldq_le_p(data + 88) ||
            ldl_le_p(data + 96) != 0x3000000 ||
            ldl_le_p(data + 100) != 0x10000 || !ldq_le_p(data + 104) ||
            ldl_le_p(data + 112) != 0x1000000 ||
            ldl_le_p(data + 116) != 0x10000 || ldl_le_p(data + 120) >= 4 ||
            ldl_le_p(data + 124) >= 8) {
            return SHADPS4_VR_ARGUMENT_INVALID;
        }
        hle->vr_tracker_initialized = true;
        return 0;
    case SHADPS4_HLE_VR_TRACKER_TERM:
        if (!hle->vr_tracker_initialized) {
            return SHADPS4_VR_NOT_INIT;
        }
        hle->vr_tracker_initialized = false;
        for (type = 0; type < 4; type++) {
            hle->vr_tracker_handles[type] = -1;
        }
        return 0;
    case SHADPS4_HLE_VR_TRACKER_REGISTER:
    case SHADPS4_HLE_VR_TRACKER_REGISTER2:
    case SHADPS4_HLE_VR_TRACKER_REGISTER_INTERNAL:
        if (!hle->vr_tracker_initialized) {
            return SHADPS4_VR_NOT_INIT;
        }
        type = a0;
        if (type > 3 || (number == SHADPS4_HLE_VR_TRACKER_REGISTER_INTERNAL &&
                         (int32_t)a2 > 4)) {
            return SHADPS4_VR_ARGUMENT_INVALID;
        }
        if (hle->vr_tracker_handles[type] != -1) {
            return SHADPS4_VR_DEVICE_ALREADY_REGISTERED;
        }
        hle->vr_tracker_handles[type] = a1;
        return 0;
    case SHADPS4_HLE_VR_TRACKER_UNREGISTER:
        if (!hle->vr_tracker_initialized) {
            return SHADPS4_VR_NOT_INIT;
        }
        for (type = 0; type < 4; type++) {
            if (hle->vr_tracker_handles[type] == (int32_t)a0) {
                hle->vr_tracker_handles[type] = -1;
                return 0;
            }
        }
        return SHADPS4_VR_ARGUMENT_INVALID;
    case SHADPS4_HLE_VR_TRACKER_GET_TIME:
        if (!hle->vr_tracker_initialized) {
            return SHADPS4_VR_NOT_INIT;
        }
        stq_le_p(data, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000);
        return shadps4_services5_rw(cs, a0, data, 8, true) ? 0 :
               SHADPS4_VR_ARGUMENT_INVALID;
    case SHADPS4_HLE_VR_TRACKER_REJECTION:
        return 0;
    case SHADPS4_HLE_VR_TRACKER_GPU_SUBMIT:
        return hle->vr_tracker_initialized ? SHADPS4_VR_ARGUMENT_INVALID :
               SHADPS4_VR_NOT_INIT;
    case SHADPS4_HLE_VR_TRACKER_GPU_WAIT:
        if (!hle->vr_tracker_initialized) {
            return SHADPS4_VR_NOT_INIT;
        }
        return shadps4_services5_rw(cs, a0, data, 32, false) &&
               ldl_le_p(data) == 32 ? SHADPS4_VR_NOT_EXECUTE_GPU_SUBMIT :
               SHADPS4_VR_ARGUMENT_INVALID;
    case SHADPS4_HLE_VR_TRACKER_GPU_WAIT_CPU:
        return hle->vr_tracker_initialized ?
               SHADPS4_VR_NOT_EXECUTE_GPU_SUBMIT : SHADPS4_VR_NOT_INIT;
    case SHADPS4_HLE_VR_TRACKER_RECALIBRATE:
        if (!hle->vr_tracker_initialized) {
            return SHADPS4_VR_NOT_INIT;
        }
        if (!shadps4_services5_rw(cs, a0, data, 32, false) ||
            ldl_le_p(data) != 32 || ldl_le_p(data + 4) > 3) {
            return SHADPS4_VR_ARGUMENT_INVALID;
        }
        return hle->vr_tracker_handles[ldl_le_p(data + 4)] < 0 ?
               SHADPS4_VR_DEVICE_NOT_REGISTERED : 0;
    case SHADPS4_HLE_VR_TRACKER_SET_DURATION:
        if (!hle->vr_tracker_initialized) {
            return SHADPS4_VR_NOT_INIT;
        }
        return a0 > 3 ? SHADPS4_VR_ARGUMENT_INVALID : 0;
    default:
        return SHADPS4_VR_ARGUMENT_INVALID;
    }
}

static uint64_t shadps4_hle_dispatch_services5(
    ShadPS4HLEState *hle, CPUState *cs, uint64_t number, uint64_t a0,
    uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
    uint64_t a6)
{
    (void)a4;
    (void)a5;
    (void)a6;
    if (number < SHADPS4_HLE_VIDEODEC2_END) {
        return shadps4_services5_videodec2(hle, cs, number, a0, a1, a2, a3);
    }
    if (number < SHADPS4_HLE_COMMERCE_END) {
        return shadps4_services5_commerce(hle, cs, number, a0);
    }
    if (number < SHADPS4_HLE_SSL2_END) {
        /* These entries are exact success stubs in shadPS4's ssl2.cpp. */
        return 0;
    }
    if (number < SHADPS4_HLE_IME_DIALOG_END) {
        return shadps4_services5_ime_dialog(
            hle, cs, number, a0, a1, a2, a3);
    }
    if (number < SHADPS4_HLE_INVITATION_END) {
        return shadps4_services5_invitation(hle, cs, number, a0);
    }
    if (number < SHADPS4_HLE_PLAYGO_DIALOG_END) {
        return shadps4_services5_playgo_dialog(cs, number, a0);
    }
    if (number < SHADPS4_HLE_PROFILE_DIALOG_END) {
        return shadps4_services5_profile_dialog(hle, cs, number, a0);
    }
    return shadps4_services5_vr(hle, cs, number, a0, a1, a2, a3);
}
