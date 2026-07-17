/* UWP-safe HLE services that must not acquire desktop device dependencies. */

#define SHADPS4_USBD_ERROR_INVALID_PARAM 0x80240002u
#define SHADPS4_USBD_ERROR_NO_DEVICE 0x80240004u
#define SHADPS4_NP_ERROR_SIGNED_OUT 0x80550006u
#define SHADPS4_NP_ERROR_INVALID_SIZE 0x80550011u
#define SHADPS4_NP_ERROR_ABORTED 0x80550012u
#define SHADPS4_NP_ERROR_REQUEST_MAX 0x80550013u
#define SHADPS4_NP_ERROR_REQUEST_NOT_FOUND 0x80550014u
#define SHADPS4_NP_ERROR_CALLBACK_NOT_REGISTERED 0x80550009u
#define SHADPS4_CAMERA_ERROR_PARAM 0x802e0000u
#define SHADPS4_CAMERA_ERROR_NOT_INIT 0x802e0002u
#define SHADPS4_CAMERA_ERROR_NOT_OPEN 0x802e0004u
#define SHADPS4_CAMERA_ERROR_NOT_CONNECTED 0x802e0010u
#define SHADPS4_CAMERA_ERROR_FATAL 0x802e00ffu
#define SHADPS4_HMD_ERROR_ALREADY_INITIALIZED 0x81110001u
#define SHADPS4_HMD_ERROR_NOT_INITIALIZED 0x81110002u
#define SHADPS4_HMD_ERROR_INVALID_HANDLE 0x81110003u
#define SHADPS4_HMD_ERROR_DEVICE_DISCONNECTED 0x81110004u
#define SHADPS4_HMD_ERROR_ALREADY_OPENED 0x81110006u
#define SHADPS4_HMD_ERROR_NULL_PARAMETER 0x81110008u
#define SHADPS4_HMD_ERROR_INVALID_PARAMETER 0x81110009u
#define SHADPS4_AUDIO3D_ERROR_INVALID_PORT 0x80ea0002u
#define SHADPS4_AUDIO3D_ERROR_INVALID_OBJECT 0x80ea0003u
#define SHADPS4_AUDIO3D_ERROR_INVALID_PARAMETER 0x80ea0004u
#define SHADPS4_AUDIO3D_ERROR_OUT_OF_RESOURCES 0x80ea0006u
#define SHADPS4_AUDIO3D_ERROR_NOT_READY 0x80ea0007u
#define SHADPS4_AUDIO3D_ERROR_NOT_SUPPORTED 0x80ea0008u

static bool shadps4_services3_write_u32(CPUState *cs, uint64_t address,
                                        uint32_t value)
{
    value = cpu_to_le32(value);
    return address && shadps4_guest_rw(cs, address, &value, sizeof(value),
                                       true);
}

static bool shadps4_services3_write_u64(CPUState *cs, uint64_t address,
                                        uint64_t value)
{
    value = cpu_to_le64(value);
    return address && shadps4_guest_rw(cs, address, &value, sizeof(value),
                                       true);
}

static uint64_t shadps4_services3_usbd(ShadPS4HLEState *hle, CPUState *cs,
                                       uint64_t number, uint64_t a0,
                                       uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint64_t slot;
    int handle;

    switch (number) {
    case SHADPS4_HLE_USBD_INIT:
        hle->usbd_initialized = true;
        return 0;
    case SHADPS4_HLE_USBD_EXIT:
        for (slot = 1; slot < SHADPS4_HLE_MAX_SERVICE_OBJECTS; slot++) {
            if (hle->service_objects[slot] == SHADPS4_SERVICE_USBD_TRANSFER) {
                shadps4_hle_service_delete(
                    hle, 0x400 + slot, SHADPS4_SERVICE_USBD_TRANSFER);
            }
        }
        hle->usbd_initialized = false;
        return 0;
    case SHADPS4_HLE_USBD_GET_DEVICE_LIST:
        if (!a0) {
            return SHADPS4_USBD_ERROR_INVALID_PARAM;
        }
        return shadps4_services3_write_u64(cs, a0, 0) ? 0 :
               (uint64_t)-SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_USBD_ALLOC_TRANSFER:
        if ((int64_t)a0 < 0 || a0 > 256) {
            return 0;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_USBD_TRANSFER, 0);
        if (handle > 0) {
            slot = handle - 0x400;
            hle->service_content_length[slot] = a0;
        }
        return handle > 0 ? handle : 0;
    case SHADPS4_HLE_USBD_FREE_TRANSFER:
        if (shadps4_hle_service_is(hle, a0,
                                   SHADPS4_SERVICE_USBD_TRANSFER)) {
            shadps4_hle_service_delete(hle, a0,
                                       SHADPS4_SERVICE_USBD_TRANSFER);
        }
        return 0;
    case SHADPS4_HLE_USBD_FILL_SETUP: {
        uint8_t setup[8] = {
            a1, a2, a3, a3 >> 8, a4, a4 >> 8, a5, a5 >> 8,
        };
        return a0 && shadps4_guest_rw(cs, a0, setup, sizeof(setup), true) ?
               0 : (uint64_t)-SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_USBD_FILL_CONTROL:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_USBD_TRANSFER)) {
            return SHADPS4_USBD_ERROR_INVALID_PARAM;
        }
        slot = a0 - 0x400;
        hle->service_parents[slot] = a1;
        hle->service_value[slot] = a2;
        hle->service_user_data[slot] = a3;
        hle->service_aux_value[slot] = a4;
        return 0;
    case SHADPS4_HLE_USBD_FILL_IO:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_USBD_TRANSFER)) {
            return SHADPS4_USBD_ERROR_INVALID_PARAM;
        }
        slot = a0 - 0x400;
        hle->service_parents[slot] = a1;
        hle->service_value[slot] = a3;
        hle->service_content_length[slot] = a4;
        hle->service_user_data[slot] = a5;
        hle->service_aux_value[slot] = a6;
        return 0;
    case SHADPS4_HLE_USBD_CONTROL_DATA:
    case SHADPS4_HLE_USBD_CONTROL_SETUP:
    case SHADPS4_HLE_USBD_ISO_BUFFER:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_USBD_TRANSFER)) {
            return 0;
        }
        slot = a0 - 0x400;
        if (number == SHADPS4_HLE_USBD_CONTROL_DATA) {
            return hle->service_value[slot] ? hle->service_value[slot] + 8 : 0;
        }
        return hle->service_value[slot];
    case SHADPS4_HLE_USBD_EVENT_OK:
        return hle->usbd_initialized ? 1 : 0;
    case SHADPS4_HLE_USBD_EVENT:
        return 0;
    case SHADPS4_HLE_USBD_NULL:
        return 0;
    case SHADPS4_HLE_USBD_NO_DEVICE:
        return SHADPS4_USBD_ERROR_NO_DEVICE;
    default:
        return SHADPS4_USBD_ERROR_INVALID_PARAM;
    }
}

static uint64_t shadps4_services3_np_manager(
    ShadPS4HLEState *hle, CPUState *cs, uint64_t number, uint64_t a0,
    uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t slot;
    uint64_t request = a0;
    int handle;

    if (a0 > 0x20000000 &&
        a0 - 0x20000000 < SHADPS4_HLE_MAX_SERVICE_OBJECTS) {
        request = 0x400 + a0 - 0x20000000;
    }

    switch (number) {
    case SHADPS4_HLE_NP_MANAGER_CREATE_REQUEST:
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_NP_MANAGER_REQUEST, 0);
        return handle > 0 ? 0x20000000 + handle - 0x400 :
                            SHADPS4_NP_ERROR_REQUEST_MAX;
    case SHADPS4_HLE_NP_MANAGER_CREATE_ASYNC_REQUEST: {
        uint64_t size = 0;
        if (!a0 || !shadps4_guest_rw(cs, a0, &size, sizeof(size), false)) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        if (le64_to_cpu(size) != 24) {
            return SHADPS4_NP_ERROR_INVALID_SIZE;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_NP_MANAGER_REQUEST, 0);
        if (handle <= 0) {
            return SHADPS4_NP_ERROR_REQUEST_MAX;
        }
        hle->service_nonblock[handle - 0x400] = true;
        return 0x20000000 + handle - 0x400;
    }
    case SHADPS4_HLE_NP_MANAGER_DELETE_REQUEST:
        return shadps4_hle_service_is(
                   hle, request, SHADPS4_SERVICE_NP_MANAGER_REQUEST) ?
               shadps4_hle_service_delete(
                   hle, request, SHADPS4_SERVICE_NP_MANAGER_REQUEST) :
               SHADPS4_NP_ERROR_REQUEST_NOT_FOUND;
    case SHADPS4_HLE_NP_MANAGER_ABORT_REQUEST:
        if (!shadps4_hle_service_is(
                hle, request, SHADPS4_SERVICE_NP_MANAGER_REQUEST)) {
            return SHADPS4_NP_ERROR_REQUEST_NOT_FOUND;
        }
        slot = request - 0x400;
        hle->service_active[slot] = true;
        hle->service_value[slot] = SHADPS4_NP_ERROR_ABORTED;
        return 0;
    case SHADPS4_HLE_NP_MANAGER_SET_TIMEOUT:
        if (!shadps4_hle_service_is(
                hle, request, SHADPS4_SERVICE_NP_MANAGER_REQUEST)) {
            return SHADPS4_NP_ERROR_REQUEST_NOT_FOUND;
        }
        hle->service_aux_value[request - 0x400] = a2;
        return 0;
    case SHADPS4_HLE_NP_MANAGER_WAIT_ASYNC:
        if (!a1) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        if (!shadps4_hle_service_is(
                hle, request, SHADPS4_SERVICE_NP_MANAGER_REQUEST)) {
            return SHADPS4_NP_ERROR_REQUEST_NOT_FOUND;
        }
        slot = request - 0x400;
        if (!hle->service_nonblock[slot]) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        if (!hle->service_active[slot]) {
            hle->service_active[slot] = true;
            hle->service_value[slot] = SHADPS4_NP_ERROR_SIGNED_OUT;
        }
        return shadps4_services3_write_u32(
                   cs, a1, hle->service_value[slot]) ?
               0 : (uint64_t)-SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_NP_MANAGER_ASYNC_OFFLINE:
        if (!shadps4_hle_service_is(
                hle, request, SHADPS4_SERVICE_NP_MANAGER_REQUEST)) {
            return SHADPS4_NP_ERROR_REQUEST_NOT_FOUND;
        }
        slot = request - 0x400;
        hle->service_active[slot] = true;
        hle->service_value[slot] = SHADPS4_NP_ERROR_SIGNED_OUT;
        return hle->service_nonblock[slot] ? 0 : SHADPS4_NP_ERROR_SIGNED_OUT;
    case SHADPS4_HLE_NP_MANAGER_SIGNED_OUT:
        if (!a0 && !a1) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        return SHADPS4_NP_ERROR_SIGNED_OUT;
    case SHADPS4_HLE_NP_MANAGER_GET_STATE:
        return a1 && shadps4_services3_write_u32(cs, a1, 1) ? 0 :
               SHADPS4_NP_ERROR_INVALID_ARGUMENT;
    case SHADPS4_HLE_NP_MANAGER_GET_REACHABILITY:
        return a1 && shadps4_services3_write_u32(cs, a1, 0) ? 0 :
               SHADPS4_NP_ERROR_INVALID_ARGUMENT;
    case SHADPS4_HLE_NP_MANAGER_GET_BOOL:
        if (a1 && !shadps4_services3_write_u32(cs, a1, 0)) {
            return (uint64_t)-SHADPS4_GUEST_EFAULT;
        }
        return 0;
    case SHADPS4_HLE_NP_MANAGER_REGISTER_CALLBACK:
        if (!a0 && !a1) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_NP_MANAGER_CALLBACK, 0);
        if (handle <= 0) {
            return SHADPS4_NP_ERROR_REQUEST_MAX;
        }
        slot = handle - 0x400;
        hle->service_value[slot] = a0;
        hle->service_user_data[slot] = a1;
        hle->service_aux_value[slot] = a2;
        return 0;
    case SHADPS4_HLE_NP_MANAGER_UNREGISTER_CALLBACK:
        for (slot = 1; slot < SHADPS4_HLE_MAX_SERVICE_OBJECTS; slot++) {
            if (hle->service_objects[slot] ==
                    SHADPS4_SERVICE_NP_MANAGER_CALLBACK &&
                (!a0 || hle->service_value[slot] == a0)) {
                shadps4_hle_service_delete(
                    hle, 0x400 + slot,
                    SHADPS4_SERVICE_NP_MANAGER_CALLBACK);
                return 0;
            }
        }
        return SHADPS4_NP_ERROR_CALLBACK_NOT_REGISTERED;
    case SHADPS4_HLE_NP_MANAGER_CHECK_CALLBACK:
        return 0;
    case SHADPS4_HLE_NP_MANAGER_SET_RESTRICTION: {
        uint8_t restriction[24];
        int32_t count;

        if (!a0 || !shadps4_guest_rw(cs, a0, restriction,
                                      sizeof(restriction), false)) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        if (ldq_le_p(restriction) != sizeof(restriction)) {
            return SHADPS4_NP_ERROR_INVALID_SIZE;
        }
        count = ldl_le_p(restriction + 12);
        if ((int8_t)restriction[8] < 0 || count < 0 || count > 0x100 ||
            (count && !ldq_le_p(restriction + 16))) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        return 0;
    }
    case SHADPS4_HLE_NP_MANAGER_SET_TITLE:
        return a0 && a1 ? 0 : SHADPS4_NP_ERROR_INVALID_ARGUMENT;
    case SHADPS4_HLE_NP_MANAGER_SET_PRESENCE:
        if (!a0 || !a1) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        return shadps4_hle_service_is(
                   hle, request, SHADPS4_SERVICE_NP_MANAGER_REQUEST) ? 0 :
               SHADPS4_NP_ERROR_REQUEST_NOT_FOUND;
    default:
        return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
    }
}

static uint64_t shadps4_services3_camera(ShadPS4HLEState *hle, CPUState *cs,
                                         uint64_t number, uint64_t a0,
                                         uint64_t a1, uint64_t a2,
                                         uint64_t a3)
{
    int handle;
    bool opened = false;
    uint32_t slot;

    for (slot = 1; slot < SHADPS4_HLE_MAX_SERVICE_OBJECTS; slot++) {
        if (hle->service_objects[slot] == SHADPS4_SERVICE_CAMERA_HANDLE) {
            opened = true;
            break;
        }
    }

    switch (number) {
    case SHADPS4_HLE_CAMERA_OPEN:
        if (a1 || a2 || !a3) {
            return SHADPS4_CAMERA_ERROR_PARAM;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_CAMERA_HANDLE, 0);
        return handle > 0 ? handle : SHADPS4_CAMERA_ERROR_FATAL;
    case SHADPS4_HLE_CAMERA_CLOSE:
        return shadps4_hle_service_is(
                   hle, a0, SHADPS4_SERVICE_CAMERA_HANDLE) ?
               shadps4_hle_service_delete(
                   hle, a0, SHADPS4_SERVICE_CAMERA_HANDLE) :
               SHADPS4_CAMERA_ERROR_NOT_OPEN;
    case SHADPS4_HLE_CAMERA_START:
        if (!a1) {
            return SHADPS4_CAMERA_ERROR_PARAM;
        }
        return shadps4_hle_service_is(
                   hle, a0, SHADPS4_SERVICE_CAMERA_HANDLE) ?
               SHADPS4_CAMERA_ERROR_NOT_CONNECTED :
               SHADPS4_CAMERA_ERROR_NOT_OPEN;
    case SHADPS4_HLE_CAMERA_STOP:
        return shadps4_hle_service_is(
                   hle, a0, SHADPS4_SERVICE_CAMERA_HANDLE) ?
               0 : SHADPS4_CAMERA_ERROR_NOT_OPEN;
    case SHADPS4_HLE_CAMERA_IS_ATTACHED:
        return a0 == 0 ? 0 : SHADPS4_CAMERA_ERROR_PARAM;
    case SHADPS4_HLE_CAMERA_GET_VALUE:
    case SHADPS4_HLE_CAMERA_GET_DATA:
    case SHADPS4_HLE_CAMERA_SET_VALUE:
        return shadps4_hle_service_is(
                   hle, a0, SHADPS4_SERVICE_CAMERA_HANDLE) ?
               SHADPS4_CAMERA_ERROR_NOT_CONNECTED :
               SHADPS4_CAMERA_ERROR_NOT_OPEN;
    case SHADPS4_HLE_CAMERA_AUDIO:
        return SHADPS4_CAMERA_ERROR_NOT_CONNECTED;
    case SHADPS4_HLE_CAMERA_CALIB_DATA:
        return SHADPS4_CAMERA_ERROR_NOT_CONNECTED;
    case SHADPS4_HLE_CAMERA_CONNECTED_COUNT:
        if (!opened) {
            return SHADPS4_CAMERA_ERROR_NOT_OPEN;
        }
        return a0 && shadps4_services3_write_u32(cs, a0, 0) ? 0 :
               SHADPS4_CAMERA_ERROR_PARAM;
    case SHADPS4_HLE_CAMERA_PRODUCT_INFO:
        if (!a0) {
            return SHADPS4_CAMERA_ERROR_PARAM;
        }
        return opened ? SHADPS4_CAMERA_ERROR_NOT_CONNECTED :
               SHADPS4_CAMERA_ERROR_NOT_INIT;
    case SHADPS4_HLE_CAMERA_REGISTRY_INIT:
        return opened ? 0 : SHADPS4_CAMERA_ERROR_NOT_INIT;
    case SHADPS4_HLE_CAMERA_DEBUG_STOP:
        return a0 <= 1 ? 0 : SHADPS4_CAMERA_ERROR_PARAM;
    case SHADPS4_HLE_CAMERA_VIDEO_SYNC: {
        uint8_t sync[16];

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_CAMERA_HANDLE)) {
            return SHADPS4_CAMERA_ERROR_NOT_OPEN;
        }
        if (!a1 || !shadps4_guest_rw(cs, a1, sync, sizeof(sync), false) ||
            ldl_le_p(sync) != sizeof(sync) || ldl_le_p(sync + 4) > 1 ||
            ldq_le_p(sync + 8)) {
            return SHADPS4_CAMERA_ERROR_PARAM;
        }
        return 0;
    }
    default:
        return SHADPS4_CAMERA_ERROR_PARAM;
    }
}

static uint64_t shadps4_services3_hmd(ShadPS4HLEState *hle, CPUState *cs,
                                      uint64_t number, uint64_t a0,
                                      uint64_t a1, uint64_t a2,
                                      uint64_t a3)
{
    uint8_t info[32] = { 0 };
    uint32_t eye_offset[3] = { 0 };
    int handle;

    switch (number) {
    case SHADPS4_HLE_HMD_INITIALIZE:
        if (!a0) {
            return SHADPS4_HMD_ERROR_NULL_PARAMETER;
        }
        if (hle->hmd_initialized) {
            return SHADPS4_HMD_ERROR_ALREADY_INITIALIZED;
        }
        hle->hmd_initialized = true;
        return 0;
    case SHADPS4_HLE_HMD_TERMINATE:
        if (!hle->hmd_initialized) {
            return SHADPS4_HMD_ERROR_NOT_INITIALIZED;
        }
        hle->hmd_initialized = false;
        return 0;
    case SHADPS4_HLE_HMD_OPEN:
        if (!hle->hmd_initialized) {
            return SHADPS4_HMD_ERROR_NOT_INITIALIZED;
        }
        if (a1 || a2 || a3 || (int32_t)a0 == -1 || (int32_t)a0 == 255) {
            return SHADPS4_HMD_ERROR_INVALID_PARAMETER;
        }
        for (handle = 1; handle < SHADPS4_HLE_MAX_SERVICE_OBJECTS;
             handle++) {
            if (hle->service_objects[handle] == SHADPS4_SERVICE_HMD_HANDLE) {
                return SHADPS4_HMD_ERROR_ALREADY_OPENED;
            }
        }
        handle = shadps4_hle_service_alloc(hle, SHADPS4_SERVICE_HMD_HANDLE,
                                            0);
        return handle > 0 ? handle : SHADPS4_HMD_ERROR_INVALID_HANDLE;
    case SHADPS4_HLE_HMD_CLOSE:
        if (!hle->hmd_initialized) {
            return SHADPS4_HMD_ERROR_NOT_INITIALIZED;
        }
        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_HMD_HANDLE) ?
               shadps4_hle_service_delete(hle, a0,
                                           SHADPS4_SERVICE_HMD_HANDLE) :
               SHADPS4_HMD_ERROR_INVALID_HANDLE;
    case SHADPS4_HLE_HMD_DEVICE_INFO: {
        uint64_t output = shadps4_hle_service_is(
                              hle, a0, SHADPS4_SERVICE_HMD_HANDLE) ? a1 : a0;
        if (!hle->hmd_initialized) {
            return SHADPS4_HMD_ERROR_NOT_INITIALIZED;
        }
        if (!output) {
            return SHADPS4_HMD_ERROR_NULL_PARAMETER;
        }
        info[0] = 2; /* NOT_DETECTED */
        return shadps4_guest_rw(cs, output, info, sizeof(info), true) ? 0 :
               (uint64_t)-SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_HMD_DEVICE_STATUS:
        if (!hle->hmd_initialized) {
            return SHADPS4_HMD_ERROR_NOT_INITIALIZED;
        }
        return a0 && shadps4_services3_write_u32(cs, a0, 2) ? 0 :
               SHADPS4_HMD_ERROR_NULL_PARAMETER;
    case SHADPS4_HLE_HMD_DISTORT_ALIGN:
        return 0x400;
    case SHADPS4_HLE_HMD_DISTORT_SIZE:
        return 0x20000;
    case SHADPS4_HLE_HMD_GARLIC_ALIGN:
    case SHADPS4_HLE_HMD_ONION_ALIGN:
        return 0x100;
    case SHADPS4_HLE_HMD_GARLIC_SIZE:
        return 0x100000;
    case SHADPS4_HLE_HMD_ONION_SIZE:
        return 0x810;
    case SHADPS4_HLE_HMD_EYE_OFFSET:
        if (!hle->hmd_initialized) {
            return SHADPS4_HMD_ERROR_NOT_INITIALIZED;
        }
        if (!shadps4_hle_service_is(hle, a0,
                                     SHADPS4_SERVICE_HMD_HANDLE)) {
            return SHADPS4_HMD_ERROR_INVALID_HANDLE;
        }
        if (!a1 || !a2) {
            return SHADPS4_HMD_ERROR_NULL_PARAMETER;
        }
        eye_offset[0] = cpu_to_le32(UINT32_C(0xbd010625));
        if (!shadps4_guest_rw(cs, a1, eye_offset, sizeof(eye_offset), true)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        eye_offset[0] = cpu_to_le32(UINT32_C(0x3d010625));
        return shadps4_guest_rw(cs, a2, eye_offset, sizeof(eye_offset), true) ?
               0 : (uint64_t)-SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_HMD_ASSY_ERROR:
        if (!a0) {
            return SHADPS4_HMD_ERROR_NULL_PARAMETER;
        }
        return hle->hmd_initialized ? SHADPS4_HMD_ERROR_DEVICE_DISCONNECTED :
               SHADPS4_HMD_ERROR_NOT_INITIALIZED;
    case SHADPS4_HLE_HMD_FIELD_OF_VIEW:
        if (!a1) {
            return SHADPS4_HMD_ERROR_NULL_PARAMETER;
        }
        if (!shadps4_hle_service_is(hle, a0,
                                     SHADPS4_SERVICE_HMD_HANDLE)) {
            return SHADPS4_HMD_ERROR_INVALID_HANDLE;
        }
        return hle->hmd_initialized ? SHADPS4_HMD_ERROR_INVALID_HANDLE :
               SHADPS4_HMD_ERROR_NOT_INITIALIZED;
    case SHADPS4_HLE_HMD_INERTIAL_DATA:
        if (!hle->hmd_initialized) {
            return SHADPS4_HMD_ERROR_NOT_INITIALIZED;
        }
        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_HMD_HANDLE) ?
               SHADPS4_HMD_ERROR_DEVICE_DISCONNECTED :
               SHADPS4_HMD_ERROR_INVALID_HANDLE;
    case SHADPS4_HLE_HMD_DISTORT_INITIALIZE:
        hle->hmd_initialized = true;
        return 0;
    case SHADPS4_HLE_HMD_STUB:
        return 0;
    default:
        return SHADPS4_HMD_ERROR_INVALID_PARAMETER;
    }
}

static uint64_t shadps4_services3_audio3d(
    ShadPS4HLEState *hle, CPUState *cs, uint64_t number, uint64_t a0,
    uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t slot;
    int handle;

    switch (number) {
    case SHADPS4_HLE_AUDIO3D_INITIALIZE:
        if (a0) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PARAMETER;
        }
        if (hle->audio3d_initialized) {
            return SHADPS4_AUDIO3D_ERROR_NOT_READY;
        }
        hle->audio3d_initialized = true;
        return 0;
    case SHADPS4_HLE_AUDIO3D_TERMINATE:
        hle->audio3d_initialized = false;
        return 0;
    case SHADPS4_HLE_AUDIO3D_DEFAULT_PARAMS: {
        uint8_t params[32] = { 0 };
        if (!a0) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PARAMETER;
        }
        stq_le_p(params, 0x20);
        stl_le_p(params + 8, 0x100);
        stl_le_p(params + 16, 512);
        stl_le_p(params + 20, 2);
        stl_le_p(params + 24, 2);
        return shadps4_guest_rw(cs, a0, params, sizeof(params), true) ? 0 :
               (uint64_t)-SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_AUDIO3D_PORT_CREATE:
        if (!hle->audio3d_initialized || !a3 || !a0 || a1 || a2) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PARAMETER;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_AUDIO3D_PORT, 0);
        if (handle <= 0) {
            return SHADPS4_AUDIO3D_ERROR_OUT_OF_RESOURCES;
        }
        slot = handle - 0x400;
        hle->service_aux_value[slot] = a0;
        hle->service_value[slot] = 2;
        return shadps4_services3_write_u32(cs, a3, handle) ? 0 :
               (uint64_t)-SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_AUDIO3D_PORT_OPEN: {
        uint32_t params[8];
        if (!hle->audio3d_initialized || !a1 || !a2 ||
            !shadps4_guest_rw(cs, a1, params, sizeof(params), false)) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PARAMETER;
        }
        if (le32_to_cpu(params[0]) != 0x20 || le32_to_cpu(params[1]) ||
            !le32_to_cpu(params[2])) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PARAMETER;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_AUDIO3D_PORT, 0);
        if (handle <= 0) {
            return SHADPS4_AUDIO3D_ERROR_OUT_OF_RESOURCES;
        }
        slot = handle - 0x400;
        hle->service_aux_value[slot] = le32_to_cpu(params[2]);
        hle->service_value[slot] = MAX(1u, le32_to_cpu(params[5]));
        return shadps4_services3_write_u32(cs, a2, handle) ? 0 :
               (uint64_t)-SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_AUDIO3D_PORT_CLOSE:
        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_AUDIO3D_PORT) ?
               shadps4_hle_service_delete(hle, a0,
                                           SHADPS4_SERVICE_AUDIO3D_PORT) :
               SHADPS4_AUDIO3D_ERROR_INVALID_PORT;
    case SHADPS4_HLE_AUDIO3D_PORT_ADVANCE:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AUDIO3D_PORT)) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PORT;
        }
        slot = a0 - 0x400;
        if (hle->service_content_length[slot] < hle->service_value[slot]) {
            hle->service_content_length[slot]++;
        }
        return 0;
    case SHADPS4_HLE_AUDIO3D_PORT_FLUSH:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AUDIO3D_PORT)) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PORT;
        }
        hle->service_content_length[a0 - 0x400] = 0;
        return 0;
    case SHADPS4_HLE_AUDIO3D_PORT_GET_ATTRIBUTES:
    {
        uint32_t count;
        uint32_t caps[3] = {
            cpu_to_le32(1), cpu_to_le32(3), cpu_to_le32(9),
        };
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AUDIO3D_PORT)) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PORT;
        }
        if (!a2 || !shadps4_guest_rw(cs, a2, &count, sizeof(count), false)) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PARAMETER;
        }
        count = le32_to_cpu(count);
        if (a1 && count &&
            !shadps4_guest_rw(cs, a1, caps,
                              MIN(count, 3u) * sizeof(caps[0]), true)) {
            return (uint64_t)-SHADPS4_GUEST_EFAULT;
        }
        return shadps4_services3_write_u32(cs, a2, a1 ? MIN(count, 3u) : 3) ?
               0 : (uint64_t)-SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_AUDIO3D_PORT_GET_QUEUE:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AUDIO3D_PORT)) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PORT;
        }
        slot = a0 - 0x400;
        if (!a1 || !shadps4_services3_write_u32(
                       cs, a1, hle->service_content_length[slot])) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PARAMETER;
        }
        if (a2) {
            shadps4_services3_write_u32(cs, a2, hle->service_value[slot]);
        }
        return 0;
    case SHADPS4_HLE_AUDIO3D_PORT_PUSH:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AUDIO3D_PORT)) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PORT;
        }
        slot = a0 - 0x400;
        if (hle->service_content_length[slot]) {
            hle->service_content_length[slot]--;
        }
        return 0;
    case SHADPS4_HLE_AUDIO3D_PORT_SET_ATTRIBUTE:
    case SHADPS4_HLE_AUDIO3D_PORT_WRITE:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AUDIO3D_PORT)) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PORT;
        }
        if (number == SHADPS4_HLE_AUDIO3D_PORT_WRITE &&
            (!a3 || !a4 || (a1 != 2 && a1 != 6 && a1 != 8) ||
             a2 > 1 || a5 > 2)) {
            return SHADPS4_AUDIO3D_ERROR_INVALID_PARAMETER;
        }
        return 0;
    case SHADPS4_HLE_AUDIO3D_OBJECT_RESERVE:
        if (!a1 || !shadps4_hle_service_is(
                       hle, a0, SHADPS4_SERVICE_AUDIO3D_PORT)) {
            return !a1 ? SHADPS4_AUDIO3D_ERROR_INVALID_PARAMETER :
                         SHADPS4_AUDIO3D_ERROR_INVALID_PORT;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_AUDIO3D_OBJECT, a0);
        return handle > 0 && shadps4_services3_write_u32(cs, a1, handle) ? 0 :
               SHADPS4_AUDIO3D_ERROR_OUT_OF_RESOURCES;
    case SHADPS4_HLE_AUDIO3D_OBJECT_UPDATE:
        return shadps4_hle_service_is(
                   hle, a0, SHADPS4_SERVICE_AUDIO3D_PORT) &&
               shadps4_hle_service_is(
                   hle, a1, SHADPS4_SERVICE_AUDIO3D_OBJECT) &&
               hle->service_parents[a1 - 0x400] == a0 ?
               0 : SHADPS4_AUDIO3D_ERROR_INVALID_OBJECT;
    case SHADPS4_HLE_AUDIO3D_OBJECT_UNRESERVE:
        return shadps4_hle_service_is(
                   hle, a1, SHADPS4_SERVICE_AUDIO3D_OBJECT) &&
               hle->service_parents[a1 - 0x400] == a0 ?
               shadps4_hle_service_delete(
                   hle, a1, SHADPS4_SERVICE_AUDIO3D_OBJECT) :
               SHADPS4_AUDIO3D_ERROR_INVALID_OBJECT;
    case SHADPS4_HLE_AUDIO3D_AUDIO_OUT:
        return SHADPS4_AUDIO3D_ERROR_NOT_SUPPORTED;
    default:
        return SHADPS4_AUDIO3D_ERROR_INVALID_PARAMETER;
    }
}

static uint64_t shadps4_hle_dispatch_services3(
    ShadPS4HLEState *hle, CPUState *cs, uint64_t number, uint64_t a0,
    uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
    uint64_t a6)
{
    if (number > SHADPS4_HLE_USBD_BEGIN && number < SHADPS4_HLE_USBD_END) {
        return shadps4_services3_usbd(hle, cs, number, a0, a1, a2, a3,
                                      a4, a5, a6);
    }
    if (number > SHADPS4_HLE_NP_MANAGER_BEGIN &&
        number < SHADPS4_HLE_NP_MANAGER_END) {
        return shadps4_services3_np_manager(hle, cs, number, a0, a1, a2,
                                            a3, a4, a5);
    }
    if (number > SHADPS4_HLE_CAMERA_BEGIN &&
        number < SHADPS4_HLE_CAMERA_END) {
        return shadps4_services3_camera(hle, cs, number, a0, a1, a2, a3);
    }
    if (number > SHADPS4_HLE_HMD_BEGIN && number < SHADPS4_HLE_HMD_END) {
        return shadps4_services3_hmd(hle, cs, number, a0, a1, a2, a3);
    }
    return shadps4_services3_audio3d(hle, cs, number, a0, a1, a2, a3,
                                     a4, a5);
}
