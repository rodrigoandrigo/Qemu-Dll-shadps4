/* Remaining shadPS4 HLE exports with UWP-safe guest-memory semantics. */

#define SHADPS4_COMMON_NOT_INITIALIZED UINT32_C(0x80b80003)
#define SHADPS4_COMMON_ALREADY_INITIALIZED UINT32_C(0x80b80004)
#define SHADPS4_COMMON_INVALID_STATE UINT32_C(0x80b80006)
#define SHADPS4_COMMON_NOT_RUNNING UINT32_C(0x80b8000b)
#define SHADPS4_COMMON_ARG_NULL UINT32_C(0x80b8000d)
#define SHADPS4_MOUSE_INVALID_ARG UINT32_C(0x80df0001)
#define SHADPS4_MOUSE_INVALID_HANDLE UINT32_C(0x80df0003)
#define SHADPS4_MOUSE_ALREADY_OPENED UINT32_C(0x80df0004)
#define SHADPS4_MOUSE_NOT_INITIALIZED UINT32_C(0x80df0005)
#define SHADPS4_PNG_INVALID_ADDR UINT32_C(0x80690001)
#define SHADPS4_PNG_INVALID_SIZE UINT32_C(0x80690002)
#define SHADPS4_PNG_INVALID_PARAM UINT32_C(0x80690003)
#define SHADPS4_PNG_INVALID_HANDLE UINT32_C(0x80690004)
#define SHADPS4_PNG_INVALID_DATA UINT32_C(0x80690010)
#define SHADPS4_PNG_DECODE_ERROR UINT32_C(0x80690012)
#define SHADPS4_VDEC_API_FAIL UINT32_C(0x80c10000)
#define SHADPS4_VDEC_STRUCT_SIZE UINT32_C(0x80c10002)
#define SHADPS4_VDEC_HANDLE UINT32_C(0x80c10003)
#define SHADPS4_VDEC_CONFIG_INFO UINT32_C(0x80c1000e)
#define SHADPS4_VDEC_ARG_POINTER UINT32_C(0x80c1000f)

static bool shadps4_services6_rw(CPUState *cs, uint64_t address, void *data,
                                 size_t size, bool write)
{
    return address && shadps4_guest_rw(cs, address, data, size, write);
}

static uint64_t shadps4_services6_jpeg(CPUState *cs, uint64_t number,
                                       uint64_t a0, uint64_t a1,
                                       uint64_t a2, uint64_t a3)
{
    uint8_t data[16];
    uint64_t handle;

    if (number == SHADPS4_HLE_JPEG_QUERY_MEMORY) {
        if (!shadps4_services6_rw(cs, a0, data, 8, false)) {
            return UINT32_C(0x80650101);
        }
        if (ldl_le_p(data) != 8) {
            return UINT32_C(0x80650102);
        }
        return ldl_le_p(data + 4) ? UINT32_C(0x80650103) : 0x800;
    }
    if (number == SHADPS4_HLE_JPEG_CREATE) {
        if (!shadps4_services6_rw(cs, a0, data, 8, false)) {
            return UINT32_C(0x80650101);
        }
        if (ldl_le_p(data) != 8) {
            return UINT32_C(0x80650102);
        }
        if (ldl_le_p(data + 4)) {
            return UINT32_C(0x80650103);
        }
        if (!a1 || !a3) {
            return UINT32_C(0x80650101);
        }
        if (a2 < 0x800) {
            return UINT32_C(0x80650102);
        }
        handle = (a1 + 0x1f) & ~UINT64_C(0x1f);
        if (handle < a1 || handle - a1 > a2 - sizeof(data)) {
            return UINT32_C(0x80650102);
        }
        stq_le_p(data, handle);
        stl_le_p(data + 8, 8);
        stl_le_p(data + 12, 0);
        if (!shadps4_services6_rw(cs, handle, data, sizeof(data), true)) {
            return UINT32_C(0x80650101);
        }
        stq_le_p(data, handle);
        return shadps4_services6_rw(cs, a3, data, 8, true) ? 0 :
               UINT32_C(0x80650101);
    }
    if (!a0 || (a0 & 0x1f) ||
        !shadps4_services6_rw(cs, a0, data, sizeof(data), false) ||
        ldq_le_p(data) != a0) {
        return UINT32_C(0x80650104);
    }
    if (number == SHADPS4_HLE_JPEG_ENCODE) {
        uint8_t param[48];
        uint8_t output[8];
        uint64_t image;
        uint64_t jpeg;
        uint64_t calculated_size;
        uint32_t image_size;
        uint32_t jpeg_size;
        uint32_t width;
        uint32_t height;
        uint32_t pitch;
        uint16_t pixel_format;
        uint16_t encode_mode;
        uint16_t color_space;
        uint8_t sampling_type;

        if (!shadps4_services6_rw(cs, a1, param, sizeof(param), false)) {
            return UINT32_C(0x80650101);
        }
        image = ldq_le_p(param);
        jpeg = ldq_le_p(param + 8);
        image_size = ldl_le_p(param + 16);
        jpeg_size = ldl_le_p(param + 20);
        width = ldl_le_p(param + 24);
        height = ldl_le_p(param + 28);
        pitch = ldl_le_p(param + 32);
        pixel_format = lduw_le_p(param + 36);
        encode_mode = lduw_le_p(param + 38);
        color_space = lduw_le_p(param + 40);
        sampling_type = param[42];
        if (!image || !jpeg || (pixel_format != 11 && (image & 3))) {
            return UINT32_C(0x80650101);
        }
        if (!image_size || !jpeg_size) {
            return UINT32_C(0x80650102);
        }
        calculated_size = (uint64_t)height * pitch;
        if (width > 0xffff || height > 0xffff || !pitch ||
            pitch > 0xfffffff || calculated_size > INT32_MAX ||
            calculated_size > image_size || encode_mode > 1 ||
            (color_space != 1 && color_space != 2) || sampling_type > 2 ||
            (int32_t)ldl_le_p(param + 44) > 0xffff ||
            (pixel_format != 0 && pixel_format != 1 &&
             pixel_format != 10 && pixel_format != 11) ||
            (pixel_format != 11 && (pitch & 3)) ||
            ((pixel_format == 0 || pixel_format == 1) &&
             (pitch / 4 < width || color_space != 1 || !sampling_type)) ||
            (pixel_format == 10 &&
             (pitch / 2 < QEMU_ALIGN_UP(width, 2) || color_space != 1 ||
              !sampling_type)) ||
            (pixel_format == 11 &&
             (pitch < width || color_space != 2 || sampling_type))) {
            return UINT32_C(0x80650103);
        }
        if (a2) {
            stl_le_p(output, jpeg_size);
            stl_le_p(output + 4, height);
            if (!shadps4_services6_rw(cs, a2, output, sizeof(output), true)) {
                return UINT32_C(0x80650101);
            }
        }
        return 0;
    }
    stq_le_p(data, 0);
    return shadps4_services6_rw(cs, a0, data, 8, true) ? 0 :
           UINT32_C(0x80650104);
}

static uint64_t shadps4_services6_mouse(ShadPS4HLEState *hle, CPUState *cs,
                                       uint64_t number, uint64_t a0,
                                       uint64_t a1, uint64_t a2, uint64_t a3)
{
    uint8_t data[40] = { 0 };
    int handle = (int32_t)a0;

    switch (number) {
    case SHADPS4_HLE_MOUSE_INIT:
        hle->mouse_initialized = true;
        return 0;
    case SHADPS4_HLE_MOUSE_OPEN: {
        uint8_t flag = 0;

        if (!hle->mouse_initialized) {
            return SHADPS4_MOUSE_NOT_INITIALIZED;
        }
        if ((int32_t)a0 < 0 || a1 != 0 || (int32_t)a2 < 0 || a2 > 1) {
            return SHADPS4_MOUSE_INVALID_ARG;
        }
        if (a3 && !shadps4_services6_rw(cs, a3, &flag, 1, false)) {
            return SHADPS4_MOUSE_INVALID_ARG;
        }
        if ((flag & 1) && a2 != 0) {
            return SHADPS4_MOUSE_ALREADY_OPENED;
        }
        if (hle->mouse_open[a2]) {
            return SHADPS4_MOUSE_ALREADY_OPENED;
        }
        hle->mouse_merged = flag & 1;
        hle->mouse_open[a2] = true;
        return a2;
    }
    case SHADPS4_HLE_MOUSE_CLOSE:
        if (handle < 0 || handle > 1 || !hle->mouse_open[handle]) {
            return SHADPS4_MOUSE_INVALID_HANDLE;
        }
        hle->mouse_open[handle] = false;
        return 0;
    case SHADPS4_HLE_MOUSE_READ:
        if (!a1 || (int32_t)a2 < 1 || a2 > 64) {
            return SHADPS4_MOUSE_INVALID_ARG;
        }
        if (handle < 0 || handle > 1 || !hle->mouse_open[handle]) {
            return SHADPS4_MOUSE_INVALID_HANDLE;
        }
        return shadps4_services6_rw(cs, a1, data, sizeof(data), true) ? 1 :
               SHADPS4_MOUSE_INVALID_ARG;
    default:
        return SHADPS4_MOUSE_INVALID_ARG;
    }
}

static bool shadps4_services6_png_header(CPUState *cs, uint64_t address,
                                        uint32_t size, uint8_t info[16])
{
    uint8_t header[33];
    uint8_t color;
    uint16_t color_space;
    uint32_t flags;

    if (size < sizeof(header) ||
        !shadps4_services6_rw(cs, address, header, sizeof(header), false) ||
        memcmp(header, "\x89PNG\r\n\x1a\n", 8) ||
        memcmp(header + 12, "IHDR", 4)) {
        return false;
    }
    color = header[25];
    switch (color) {
    case 0: color_space = 2; break;
    case 2: color_space = 3; break;
    case 3: color_space = 4; break;
    case 4: color_space = 18; break;
    case 6: color_space = 19; break;
    default: return false;
    }
    memset(info, 0, 16);
    stl_le_p(info, ldl_be_p(header + 16));
    stl_le_p(info + 4, ldl_be_p(header + 20));
    stw_le_p(info + 8, color_space);
    stw_le_p(info + 10, header[24]);
    flags = header[28] == 1 ? 1 : 0;
    stl_le_p(info + 12, flags);
    return true;
}

static uint64_t shadps4_services6_png(ShadPS4HLEState *hle, CPUState *cs,
                                     uint64_t number, uint64_t a0,
                                     uint64_t a1, uint64_t a2, uint64_t a3)
{
    uint8_t data[32], info[16];
    int handle;

    if (number == SHADPS4_HLE_PNG_QUERY_MEMORY) {
        if (!shadps4_services6_rw(cs, a0, data, 12, false)) {
            return SHADPS4_PNG_INVALID_PARAM;
        }
        if (ldl_le_p(data + 4) > 1) {
            return SHADPS4_PNG_INVALID_ADDR;
        }
        return ldl_le_p(data + 8) - 1 > 1000000 ?
               SHADPS4_PNG_INVALID_SIZE : 16;
    }
    if (number == SHADPS4_HLE_PNG_CREATE) {
        if (!shadps4_services6_rw(cs, a0, data, 12, false) ||
            ldl_le_p(data + 4) > 1) {
            return SHADPS4_PNG_INVALID_PARAM;
        }
        if (!a1 || !a3) {
            return SHADPS4_PNG_INVALID_ADDR;
        }
        if (ldl_le_p(data + 8) - 1 > 1000000 || a2 < 16) {
            return SHADPS4_PNG_INVALID_SIZE;
        }
        handle = shadps4_hle_service_alloc(hle, SHADPS4_SERVICE_PNG_DECODER, 0);
        if (handle < 0) {
            return UINT32_C(0x80690020);
        }
        stq_le_p(data, handle);
        if (!shadps4_services6_rw(cs, a3, data, 8, true)) {
            shadps4_hle_service_delete(hle, handle,
                                       SHADPS4_SERVICE_PNG_DECODER);
            return SHADPS4_PNG_INVALID_ADDR;
        }
        return 0;
    }
    if (number == SHADPS4_HLE_PNG_DELETE) {
        return shadps4_hle_service_is(hle, a0, SHADPS4_SERVICE_PNG_DECODER) ?
               shadps4_hle_service_delete(hle, a0,
                                           SHADPS4_SERVICE_PNG_DECODER) :
               SHADPS4_PNG_INVALID_HANDLE;
    }
    if (number == SHADPS4_HLE_PNG_PARSE_HEADER) {
        if (!shadps4_services6_rw(cs, a0, data, 16, false)) {
            return SHADPS4_PNG_INVALID_PARAM;
        }
        if (!ldq_le_p(data)) {
            return SHADPS4_PNG_INVALID_ADDR;
        }
        if (!a1) {
            return SHADPS4_PNG_INVALID_PARAM;
        }
        if (!shadps4_services6_png_header(cs, ldq_le_p(data),
                                          ldl_le_p(data + 8), info)) {
            return SHADPS4_PNG_INVALID_DATA;
        }
        return shadps4_services6_rw(cs, a1, info, sizeof(info), true) ? 0 :
               SHADPS4_PNG_INVALID_ADDR;
    }
    if (!shadps4_hle_service_is(hle, a0, SHADPS4_SERVICE_PNG_DECODER)) {
        return SHADPS4_PNG_INVALID_HANDLE;
    }
    if (!shadps4_services6_rw(cs, a1, data, 32, false)) {
        return SHADPS4_PNG_INVALID_PARAM;
    }
    if (!ldq_le_p(data) || !ldq_le_p(data + 8)) {
        return SHADPS4_PNG_INVALID_ADDR;
    }
    /* No desktop codec is pulled into the UWP DLL; report capability honestly. */
    return SHADPS4_PNG_DECODE_ERROR;
}

static uint64_t shadps4_services6_videodec(ShadPS4HLEState *hle,
                                           CPUState *cs, uint64_t number,
                                           uint64_t a0, uint64_t a1,
                                           uint64_t a2, uint64_t a3)
{
    uint8_t cfg[40], resource[56], ctrl[24];
    uint8_t input[48], frame_buffer[24], picture[112];
    uint64_t handle, frame, padded, surfaces;
    int32_t width_value, height_value, dpb_value;
    uint32_t width, height, dpb;
    int allocated;

    if (number == SHADPS4_HLE_VIDEODEC_QUERY) {
        if (!shadps4_services6_rw(cs, a0, cfg, sizeof(cfg), false) ||
            !shadps4_services6_rw(cs, a1, resource, sizeof(resource), false)) {
            return SHADPS4_VDEC_ARG_POINTER;
        }
        if (ldq_le_p(cfg) != sizeof(cfg) ||
            ldq_le_p(resource) != sizeof(resource)) {
            return SHADPS4_VDEC_STRUCT_SIZE;
        }
        width_value = ldl_le_p(cfg + 20);
        height_value = ldl_le_p(cfg + 24);
        dpb_value = ldl_le_p(cfg + 28);
        if (width_value < 0 || width_value > 16384 ||
            height_value < 0 || height_value > 16384 ||
            dpb_value < 0 || dpb_value > 32) {
            return SHADPS4_VDEC_CONFIG_INFO;
        }
        width = width_value;
        height = height_value;
        dpb = dpb_value;
        dpb = dpb ? dpb : 8;
        if (!width || !height) {
            frame = padded = 16 * MiB;
            surfaces = 0;
        } else {
            width = QEMU_ALIGN_UP(width, 256);
            height = QEMU_ALIGN_UP(height, 16);
            frame = (uint64_t)width * height * 3 / 2;
            padded = QEMU_ALIGN_UP(frame, 256) + 0x4000;
            surfaces = dpb + 2;
            if (padded > (UINT64_MAX - 8 * MiB) / surfaces) {
                return SHADPS4_VDEC_CONFIG_INFO;
            }
        }
        memset(resource, 0, sizeof(resource));
        stq_le_p(resource, sizeof(resource));
        stq_le_p(resource + 8, 16 * MiB);
        stq_le_p(resource + 24, surfaces ? padded * surfaces + 8 * MiB :
                                      16 * MiB);
        stq_le_p(resource + 40, padded);
        stl_le_p(resource + 48, 0x100);
        return shadps4_services6_rw(cs, a1, resource, sizeof(resource), true) ?
               0 : SHADPS4_VDEC_ARG_POINTER;
    }
    if (number == SHADPS4_HLE_VIDEODEC_CREATE) {
        if (!shadps4_services6_rw(cs, a0, cfg, sizeof(cfg), false) ||
            !shadps4_services6_rw(cs, a1, resource, sizeof(resource), false) ||
            !a2) {
            return SHADPS4_VDEC_ARG_POINTER;
        }
        if (ldq_le_p(cfg) != sizeof(cfg) ||
            ldq_le_p(resource) != sizeof(resource)) {
            return SHADPS4_VDEC_STRUCT_SIZE;
        }
        allocated = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_VIDEODEC_DECODER, 0);
        if (allocated < 0) {
            return SHADPS4_VDEC_API_FAIL;
        }
        memset(ctrl, 0, sizeof(ctrl));
        stq_le_p(ctrl, sizeof(ctrl));
        stq_le_p(ctrl + 8, allocated);
        stq_le_p(ctrl + 16, 1);
        if (!shadps4_services6_rw(cs, a2, ctrl, sizeof(ctrl), true)) {
            shadps4_hle_service_delete(hle, allocated,
                                       SHADPS4_SERVICE_VIDEODEC_DECODER);
            return SHADPS4_VDEC_ARG_POINTER;
        }
        return 0;
    }
    if (!shadps4_services6_rw(cs, a0, ctrl, sizeof(ctrl), false)) {
        return SHADPS4_VDEC_ARG_POINTER;
    }
    if (ldq_le_p(ctrl) != sizeof(ctrl)) {
        return SHADPS4_VDEC_STRUCT_SIZE;
    }
    handle = ldq_le_p(ctrl + 8);
    if (!shadps4_hle_service_is(hle, handle,
                                 SHADPS4_SERVICE_VIDEODEC_DECODER)) {
        return SHADPS4_VDEC_HANDLE;
    }
    if (number == SHADPS4_HLE_VIDEODEC_DELETE) {
        stq_le_p(ctrl + 8, 0);
        shadps4_services6_rw(cs, a0, ctrl, sizeof(ctrl), true);
        return shadps4_hle_service_delete(
            hle, handle, SHADPS4_SERVICE_VIDEODEC_DECODER);
    }
    if (number == SHADPS4_HLE_VIDEODEC_RESET) {
        return 0;
    }
    if (number == SHADPS4_HLE_VIDEODEC_DECODE) {
        if (!shadps4_services6_rw(cs, a1, input, sizeof(input), false) ||
            !shadps4_services6_rw(cs, a2, frame_buffer,
                                  sizeof(frame_buffer), false) || !a3) {
            return SHADPS4_VDEC_ARG_POINTER;
        }
        if (ldq_le_p(input) != sizeof(input) ||
            ldq_le_p(frame_buffer) != sizeof(frame_buffer)) {
            return SHADPS4_VDEC_STRUCT_SIZE;
        }
        return SHADPS4_VDEC_API_FAIL;
    }
    if (!a1 || !a2 ||
        !shadps4_services6_rw(cs, a1, frame_buffer,
                              sizeof(frame_buffer), false) ||
        ldq_le_p(frame_buffer) != sizeof(frame_buffer) ||
        !shadps4_services6_rw(cs, a2, picture, sizeof(picture), false) ||
        ldq_le_p(picture) != sizeof(picture)) {
        return SHADPS4_VDEC_STRUCT_SIZE;
    }
    return SHADPS4_VDEC_API_FAIL;
}

static uint64_t shadps4_hle_dispatch_services6(
    ShadPS4HLEState *hle, CPUState *cs, uint64_t number, uint64_t a0,
    uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
    uint64_t a6)
{
    uint8_t data[260] = { 0 };
    uint32_t value;

    (void)a4; (void)a5; (void)a6;
    if (number >= SHADPS4_HLE_JPEG_CREATE &&
        number <= SHADPS4_HLE_JPEG_QUERY_MEMORY) {
        return shadps4_services6_jpeg(cs, number, a0, a1, a2, a3);
    }
    if (number >= SHADPS4_HLE_MOUSE_CLOSE &&
        number <= SHADPS4_HLE_MOUSE_READ) {
        return shadps4_services6_mouse(hle, cs, number, a0, a1, a2, a3);
    }
    if (number >= SHADPS4_HLE_PNG_CREATE &&
        number <= SHADPS4_HLE_PNG_QUERY_MEMORY) {
        return shadps4_services6_png(hle, cs, number, a0, a1, a2, a3);
    }
    if (number >= SHADPS4_HLE_VIDEODEC_CREATE &&
        number <= SHADPS4_HLE_VIDEODEC_RESET) {
        return shadps4_services6_videodec(hle, cs, number, a0, a1, a2, a3);
    }
    switch (number) {
    case SHADPS4_HLE_COMMON_DIALOG_IS_USED:
        return hle->dialog_status != 0 || hle->error_dialog_status != 0 ||
               hle->web_dialog_status != 0;
    case SHADPS4_HLE_COMPANION_GET_EVENT:
        stl_le_p(data, UINT32_C(0x10000002));
        return shadps4_services6_rw(cs, a0, data, sizeof(data), true) ?
               UINT32_C(0x80e40008) : -SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_ERROR_DIALOG_INIT:
        if (hle->error_dialog_status) return SHADPS4_COMMON_ALREADY_INITIALIZED;
        hle->error_dialog_status = 1;
        return 0;
    case SHADPS4_HLE_ERROR_DIALOG_OPEN:
        if (hle->error_dialog_status != 1 && hle->error_dialog_status != 3)
            return SHADPS4_COMMON_INVALID_STATE;
        if (!a0) return SHADPS4_COMMON_ARG_NULL;
        hle->error_dialog_status = 2;
        return 0;
    case SHADPS4_HLE_ERROR_DIALOG_CLOSE:
        if (hle->error_dialog_status != 2) return SHADPS4_COMMON_NOT_RUNNING;
        hle->error_dialog_status = 3;
        return 0;
    case SHADPS4_HLE_ERROR_DIALOG_GET_STATUS:
    case SHADPS4_HLE_ERROR_DIALOG_UPDATE:
        return hle->error_dialog_status;
    case SHADPS4_HLE_ERROR_DIALOG_TERM:
        if (!hle->error_dialog_status) return SHADPS4_COMMON_NOT_INITIALIZED;
        hle->error_dialog_status = 0;
        return 0;
    case SHADPS4_HLE_GAME_LIVE_START_DEBUG:
    case SHADPS4_HLE_GAME_LIVE_STOP_DEBUG:
    case SHADPS4_HLE_SCREENSHOT_SET_DRC:
    case SHADPS4_HLE_WEB_DIALOG_LIMITED:
    case SHADPS4_HLE_ULOBJ_SUCCESS:
        return 0;
    case SHADPS4_HLE_MSG_PROGRESS_INC:
    case SHADPS4_HLE_MSG_PROGRESS_SET_VALUE:
        if (hle->dialog_status != 2) return SHADPS4_COMMON_NOT_RUNNING;
        if (a0) return UINT32_C(0x80b8000a);
        hle->msg_dialog_progress = number == SHADPS4_HLE_MSG_PROGRESS_INC ?
                                   hle->msg_dialog_progress + a1 : a1;
        return 0;
    case SHADPS4_HLE_MSG_PROGRESS_SET_MSG:
        if (hle->dialog_status != 2) return SHADPS4_COMMON_NOT_RUNNING;
        return a0 ? UINT32_C(0x80b8000a) : (a1 ? 0 : SHADPS4_COMMON_ARG_NULL);
    case SHADPS4_HLE_NP_PARTNER_INIT:
        hle->np_partner_initialized = true;
        return 0;
    case SHADPS4_HLE_NP_PARTNER_TERM:
        if (!hle->np_partner_initialized) return UINT32_C(0x819d0001);
        hle->np_partner_initialized = false;
        return 0;
    case SHADPS4_HLE_NP_PARTNER_SUBSCRIPTION:
        if (!a1) return UINT32_C(0x819d0002);
        value = 0;
        return shadps4_services6_rw(cs, a1, &value, 1, true) ? 0 :
               UINT32_C(0x819d0002);
    case SHADPS4_HLE_COREDUMP_REGISTER:
        if (hle->coredump_handler) return UINT32_C(0x81180002);
        if (!a0 || a1 >= UINT64_C(0x20000000)) {
            return UINT32_C(0x81180000);
        }
        hle->coredump_handler = a0;
        hle->coredump_common = a2;
        return 0;
    case SHADPS4_HLE_COREDUMP_UNREGISTER:
        if (!hle->coredump_handler) return UINT32_C(0x81180001);
        hle->coredump_handler = 0;
        hle->coredump_common = 0;
        return 0;
    case SHADPS4_HLE_PROVIDER_UNSUPPORTED:
        return -SHADPS4_GUEST_ENOSYS;
    case SHADPS4_HLE_COMMON_DIALOG_INIT:
        if (hle->common_dialog_initialized) return UINT32_C(0x80b80002);
        hle->common_dialog_initialized = true;
        return 0;
    case SHADPS4_HLE_SAVEDATA_DIALOG_PROGRESS_SET:
        if (hle->dialog_status != 2) return SHADPS4_COMMON_NOT_RUNNING;
        if (a0) return UINT32_C(0x80b8000a);
        hle->save_data_progress = a1;
        return 0;
    case SHADPS4_HLE_CONTENT_EXPORT_INIT:
    case SHADPS4_HLE_CONTENT_EXPORT_INIT2: {
        uint64_t param[6];

        if (hle->content_export_initialized) return UINT32_C(0x809d3005);
        if (!a0 || !shadps4_services6_rw(cs, a0, param, sizeof(param), false) ||
            !ldq_le_p(param) || !ldq_le_p((uint8_t *)param + 8)) {
            return UINT32_C(0x809d3016);
        }
        if (number == SHADPS4_HLE_CONTENT_EXPORT_INIT2 &&
            (ldq_le_p((uint8_t *)param + 32) ||
             ldq_le_p((uint8_t *)param + 40) ||
             (ldq_le_p((uint8_t *)param + 24) &&
              ldq_le_p((uint8_t *)param + 24) < 0x100))) {
            return UINT32_C(0x809d3016);
        }
        hle->content_export_initialized = true;
        return 0;
    }
    case SHADPS4_HLE_CONTENT_EXPORT_TERM:
        if (!hle->content_export_initialized) return UINT32_C(0x809d3004);
        hle->content_export_initialized = false;
        return 0;
    case SHADPS4_HLE_VOICE_GET_BITRATE:
        return a1 && shadps4_services6_rw(
                         cs, a1, (uint32_t[]) { cpu_to_le32(48000) },
                         sizeof(uint32_t), true) ? 0 :
               -SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_VOICE_GET_PORT_INFO:
        memset(data, 0, 32);
        stl_le_p(data + 20, 1);
        return a1 && shadps4_services6_rw(cs, a1, data, 32, true) ? 0 :
               -SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_NP_PARTY_NOT_IN_PARTY:
        return UINT32_C(0x80552506);
    case SHADPS4_HLE_NP_PARTNER_ABORT:
        return hle->np_partner_initialized ? 0 : UINT32_C(0x819d0001);
    case SHADPS4_HLE_GAME_LIVE_STATUS:
        memset(data, 0, 72);
        return shadps4_services6_rw(cs, a0, data, 72, true) ? 0 :
               -SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_HMD_SETUP_RESULT:
        value = cpu_to_le32(1); /* CommonDialog::Result::USER_CANCELED */
        return shadps4_services6_rw(cs, a0, &value, sizeof(value), true) ? 0 :
               SHADPS4_COMMON_ARG_NULL;
    case SHADPS4_HLE_DIALOG_STATUS_FINISHED:
        return 3;
    case SHADPS4_HLE_SHAREPLAY_CONNECTION_INFO:
        memset(data, 0, 56);
        return shadps4_services6_rw(cs, a0, data, 56, true) ? 0 :
               -SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_SSL_GET_CA_CERTS:
        memset(data, 0, 24);
        return shadps4_services6_rw(cs, a1, data, 24, true) ? 0 :
               UINT32_C(0x8095f007);
    case SHADPS4_HLE_SSL_FREE_CA_CERTS:
        return a1 ? 0 : UINT32_C(0x8095f007);
    case SHADPS4_HLE_VIDEO_RECORDING_SET_INFO:
        if (!a1 || !(a0 == 2 || (a0 >= 6 && a0 <= 8) || a0 == 0x0d ||
                     a0 == 0xa01 || (a0 >= 0xa007 && a0 <= 0xa009))) {
            return UINT32_C(0x80a80003);
        }
        return 0;
    case SHADPS4_HLE_RANDOM_GET:
        if (a1 > 64 || (!a0 && a1)) return UINT32_C(0x817c0016);
        for (uint64_t i = 0; i < a1; i++) data[i] = g_random_int_range(0, 256);
        return !a1 || shadps4_services6_rw(cs, a0, data, a1, true) ? 0 :
               UINT32_C(0x817c0016);
    case SHADPS4_HLE_REMOTEPLAY_STATUS:
        value = 0;
        return shadps4_services6_rw(cs, a1, &value, sizeof(value), true) ? 0 :
               -SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_SAVEDATA_DIALOG_READY:
        return 1;
    case SHADPS4_HLE_SAVEDATA_DIALOG_PROGRESS_INC:
        if (hle->dialog_status != 2) return SHADPS4_COMMON_NOT_RUNNING;
        if (a0) return UINT32_C(0x80b8000a);
        hle->save_data_progress += a1;
        return 0;
    case SHADPS4_HLE_WEB_DIALOG_INIT:
        if (hle->web_dialog_status) return SHADPS4_COMMON_ALREADY_INITIALIZED;
        hle->web_dialog_status = 1;
        return 0;
    case SHADPS4_HLE_WEB_DIALOG_OPEN:
        if (hle->web_dialog_status != 1 && hle->web_dialog_status != 3)
            return SHADPS4_COMMON_INVALID_STATE;
        hle->web_dialog_status = 2;
        return 0;
    case SHADPS4_HLE_WEB_DIALOG_CLOSE:
        if (hle->web_dialog_status != 2) return SHADPS4_COMMON_NOT_RUNNING;
        hle->web_dialog_status = 3;
        return 0;
    case SHADPS4_HLE_WEB_DIALOG_GET_STATUS:
        return hle->web_dialog_status;
    case SHADPS4_HLE_WEB_DIALOG_UPDATE:
        if (hle->web_dialog_status == 2) hle->web_dialog_status = 3;
        return hle->web_dialog_status;
    case SHADPS4_HLE_WEB_DIALOG_TERM:
        if (!hle->web_dialog_status) return SHADPS4_COMMON_NOT_INITIALIZED;
        hle->web_dialog_status = 0;
        return 0;
    case SHADPS4_HLE_ULOBJ_REGISTER:
        if (!a0 || !a1 || !a2) return 22;
        value = 0;
        return shadps4_services6_rw(cs, a2, &value, sizeof(value), true) ? 0 : 22;
    case SHADPS4_HLE_ULOBJ_UNREGISTER:
        return a0 >= 0x4000 ? 22 : 0;
    case SHADPS4_HLE_ULOBJ_VALIDATE:
        return a0 ? 0 : 22;
    default:
        return -SHADPS4_GUEST_ENOSYS;
    }
}

#undef SHADPS4_COMMON_NOT_INITIALIZED
#undef SHADPS4_COMMON_ALREADY_INITIALIZED
#undef SHADPS4_COMMON_INVALID_STATE
#undef SHADPS4_COMMON_NOT_RUNNING
#undef SHADPS4_COMMON_ARG_NULL
#undef SHADPS4_MOUSE_INVALID_ARG
#undef SHADPS4_MOUSE_INVALID_HANDLE
#undef SHADPS4_MOUSE_ALREADY_OPENED
#undef SHADPS4_MOUSE_NOT_INITIALIZED
#undef SHADPS4_PNG_INVALID_ADDR
#undef SHADPS4_PNG_INVALID_SIZE
#undef SHADPS4_PNG_INVALID_PARAM
#undef SHADPS4_PNG_INVALID_HANDLE
#undef SHADPS4_PNG_INVALID_DATA
#undef SHADPS4_PNG_DECODE_ERROR
#undef SHADPS4_VDEC_API_FAIL
#undef SHADPS4_VDEC_STRUCT_SIZE
#undef SHADPS4_VDEC_HANDLE
#undef SHADPS4_VDEC_CONFIG_INFO
#undef SHADPS4_VDEC_ARG_POINTER
