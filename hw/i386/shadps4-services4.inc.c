/* Stateful UWP-safe HLE for package, online identity, IME and trophies. */

#define SHADPS4_PLAYGO_ERROR_INVALID_ARGUMENT 0x80b20004u
#define SHADPS4_PLAYGO_ERROR_NOT_INITIALIZED 0x80b20005u
#define SHADPS4_PLAYGO_ERROR_ALREADY_INITIALIZED 0x80b20006u
#define SHADPS4_PLAYGO_ERROR_BAD_HANDLE 0x80b20009u
#define SHADPS4_PLAYGO_ERROR_BAD_POINTER 0x80b2000au
#define SHADPS4_PLAYGO_ERROR_BAD_SIZE 0x80b2000bu
#define SHADPS4_PLAYGO_ERROR_BAD_LOCUS 0x80b20010u

#define SHADPS4_NP_AUTH_ERROR_INVALID_ARGUMENT 0x80550301u
#define SHADPS4_NP_AUTH_ERROR_INVALID_SIZE 0x80550302u
#define SHADPS4_NP_AUTH_ERROR_ABORTED 0x80550304u
#define SHADPS4_NP_AUTH_ERROR_REQUEST_MAX 0x80550305u
#define SHADPS4_NP_AUTH_ERROR_REQUEST_NOT_FOUND 0x80550306u
#define SHADPS4_NP_AUTH_ERROR_INVALID_ID 0x80550307u
#define SHADPS4_NP_ERROR_USER_NOT_FOUND 0x80550007u

#define SHADPS4_IME_ERROR_BUSY 0x80bc0001u
#define SHADPS4_IME_ERROR_NOT_OPENED 0x80bc0002u
#define SHADPS4_IME_ERROR_CONNECTION_FAILED 0x80bc0004u
#define SHADPS4_IME_ERROR_INVALID_USER_ID 0x80bc0010u
#define SHADPS4_IME_ERROR_INVALID_TYPE 0x80bc0011u
#define SHADPS4_IME_ERROR_INVALID_OPTION 0x80bc0015u
#define SHADPS4_IME_ERROR_INVALID_MAX_LENGTH 0x80bc0016u
#define SHADPS4_IME_ERROR_INVALID_INPUT_BUFFER 0x80bc0017u
#define SHADPS4_IME_ERROR_INVALID_HANDLER 0x80bc0022u
#define SHADPS4_IME_ERROR_INVALID_PARAM 0x80bc0030u
#define SHADPS4_IME_ERROR_INVALID_ADDRESS 0x80bc0031u
#define SHADPS4_IME_ERROR_INVALID_RESERVED 0x80bc0032u

#define SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT 0x80551604u
#define SHADPS4_TROPHY_ERROR_INVALID_HANDLE 0x80551608u
#define SHADPS4_TROPHY_ERROR_INVALID_CONTEXT 0x80551609u
#define SHADPS4_TROPHY_ERROR_INVALID_TROPHY_ID 0x8055160au
#define SHADPS4_TROPHY_ERROR_INVALID_GROUP_ID 0x8055160bu
#define SHADPS4_TROPHY_ERROR_ALREADY_UNLOCKED 0x8055160cu
#define SHADPS4_TROPHY_ERROR_NOT_REGISTERED 0x8055160fu
#define SHADPS4_TROPHY_ERROR_ALREADY_REGISTERED 0x80551610u
#define SHADPS4_TROPHY_ERROR_ICON_NOT_FOUND 0x80551614u

static bool shadps4_services4_read(CPUState *cs, uint64_t address,
                                   void *value, size_t size)
{
    return address && shadps4_guest_rw(cs, address, value, size, false);
}

static bool shadps4_services4_write(CPUState *cs, uint64_t address,
                                    const void *value, size_t size)
{
    return address && shadps4_guest_rw(cs, address, (void *)value, size, true);
}

static bool shadps4_services4_write_u32(CPUState *cs, uint64_t address,
                                        uint32_t value)
{
    value = cpu_to_le32(value);
    return shadps4_services4_write(cs, address, &value, sizeof(value));
}

static bool shadps4_services4_write_u64(CPUState *cs, uint64_t address,
                                        uint64_t value)
{
    value = cpu_to_le64(value);
    return shadps4_services4_write(cs, address, &value, sizeof(value));
}

static int shadps4_services4_find(ShadPS4HLEState *hle, uint8_t type)
{
    uint32_t slot;

    for (slot = 1; slot < SHADPS4_HLE_MAX_SERVICE_OBJECTS; slot++) {
        if (hle->service_objects[slot] == type) {
            return 0x400 + slot;
        }
    }
    return -1;
}

static uint64_t shadps4_services4_playgo(ShadPS4HLEState *hle, CPUState *cs,
                                         uint64_t number, uint64_t a0,
                                         uint64_t a1, uint64_t a2,
                                         uint64_t a3)
{
    uint8_t init[16];
    uint64_t buffer;
    uint32_t size;

    switch (number) {
    case SHADPS4_HLE_PLAYGO_INITIALIZE:
        if (!shadps4_services4_read(cs, a0, init, sizeof(init))) {
            return SHADPS4_PLAYGO_ERROR_BAD_POINTER;
        }
        buffer = ldq_le_p(init);
        size = ldl_le_p(init + 8);
        if (!buffer) {
            return SHADPS4_PLAYGO_ERROR_BAD_POINTER;
        }
        if (size < 0x200000) {
            return SHADPS4_PLAYGO_ERROR_BAD_SIZE;
        }
        if (hle->playgo_initialized) {
            return SHADPS4_PLAYGO_ERROR_ALREADY_INITIALIZED;
        }
        hle->playgo_initialized = true;
        hle->playgo_open = false;
        hle->playgo_speed = 2;
        hle->playgo_language_mask = 1ULL << 63;
        return 0;
    case SHADPS4_HLE_PLAYGO_TERMINATE:
        if (!hle->playgo_initialized) {
            return SHADPS4_PLAYGO_ERROR_NOT_INITIALIZED;
        }
        hle->playgo_initialized = false;
        hle->playgo_open = false;
        return 0;
    case SHADPS4_HLE_PLAYGO_OPEN:
        if (!a0) {
            return SHADPS4_PLAYGO_ERROR_BAD_POINTER;
        }
        if (a1) {
            return SHADPS4_PLAYGO_ERROR_INVALID_ARGUMENT;
        }
        if (!hle->playgo_initialized) {
            return SHADPS4_PLAYGO_ERROR_NOT_INITIALIZED;
        }
        if (!shadps4_services4_write_u32(cs, a0, 1)) {
            return SHADPS4_PLAYGO_ERROR_BAD_POINTER;
        }
        hle->playgo_open = true;
        return 0;
    case SHADPS4_HLE_PLAYGO_CLOSE:
        if (a0 != 1) {
            return SHADPS4_PLAYGO_ERROR_BAD_HANDLE;
        }
        if (!hle->playgo_initialized) {
            return SHADPS4_PLAYGO_ERROR_NOT_INITIALIZED;
        }
        hle->playgo_open = false;
        return 0;
    default:
        break;
    }

    if (a0 != 1) {
        return SHADPS4_PLAYGO_ERROR_BAD_HANDLE;
    }
    if (!hle->playgo_initialized) {
        return SHADPS4_PLAYGO_ERROR_NOT_INITIALIZED;
    }
    switch (number) {
    case SHADPS4_HLE_PLAYGO_GET_CHUNK_ID:
        if (!a3 || (a1 && !a2)) {
            return a3 ? SHADPS4_PLAYGO_ERROR_BAD_SIZE :
                        SHADPS4_PLAYGO_ERROR_BAD_POINTER;
        }
        return shadps4_services4_write_u32(cs, a3, 0) ? 0 :
               SHADPS4_PLAYGO_ERROR_BAD_POINTER;
    case SHADPS4_HLE_PLAYGO_GET_ETA:
        if (!a1 || !a3) {
            return SHADPS4_PLAYGO_ERROR_BAD_POINTER;
        }
        if (!a2) {
            return SHADPS4_PLAYGO_ERROR_BAD_SIZE;
        }
        return shadps4_services4_write_u64(cs, a3, 0) ? 0 :
               SHADPS4_PLAYGO_ERROR_BAD_POINTER;
    case SHADPS4_HLE_PLAYGO_GET_SPEED:
        return a1 && shadps4_services4_write_u32(cs, a1,
                                                 hle->playgo_speed) ? 0 :
               SHADPS4_PLAYGO_ERROR_BAD_POINTER;
    case SHADPS4_HLE_PLAYGO_GET_LANGUAGE:
        return a1 && shadps4_services4_write_u64(
                   cs, a1, hle->playgo_language_mask) ? 0 :
               SHADPS4_PLAYGO_ERROR_BAD_POINTER;
    case SHADPS4_HLE_PLAYGO_GET_LOCUS: {
        uint32_t i;
        uint32_t locus = cpu_to_le32(3);

        if (!a1 || !a3) {
            return SHADPS4_PLAYGO_ERROR_BAD_POINTER;
        }
        if (!a2) {
            return SHADPS4_PLAYGO_ERROR_BAD_SIZE;
        }
        for (i = 0; i < a2; i++) {
            if (!shadps4_services4_write(cs, a3 + i * 4,
                                         &locus, sizeof(locus))) {
                return SHADPS4_PLAYGO_ERROR_BAD_POINTER;
            }
        }
        return 0;
    }
    case SHADPS4_HLE_PLAYGO_GET_PROGRESS: {
        uint64_t progress[2] = { 0, 0 };

        if (!a1 || !a3) {
            return SHADPS4_PLAYGO_ERROR_BAD_POINTER;
        }
        if (!a2) {
            return SHADPS4_PLAYGO_ERROR_BAD_SIZE;
        }
        return shadps4_services4_write(cs, a3, progress,
                                       sizeof(progress)) ? 0 :
               SHADPS4_PLAYGO_ERROR_BAD_POINTER;
    }
    case SHADPS4_HLE_PLAYGO_GET_TODO:
        if (!a1 || !a3) {
            return SHADPS4_PLAYGO_ERROR_BAD_POINTER;
        }
        if (!a2) {
            return SHADPS4_PLAYGO_ERROR_BAD_SIZE;
        }
        return shadps4_services4_write_u32(cs, a3, 0) ? 0 :
               SHADPS4_PLAYGO_ERROR_BAD_POINTER;
    case SHADPS4_HLE_PLAYGO_PREFETCH:
        if (!a1) {
            return SHADPS4_PLAYGO_ERROR_BAD_POINTER;
        }
        if (!a2) {
            return SHADPS4_PLAYGO_ERROR_BAD_SIZE;
        }
        return a3 == 0 || a3 == 2 || a3 == 3 ? 0 :
               SHADPS4_PLAYGO_ERROR_BAD_LOCUS;
    case SHADPS4_HLE_PLAYGO_SET_SPEED:
        if (a1 > 2) {
            return SHADPS4_PLAYGO_ERROR_INVALID_ARGUMENT;
        }
        hle->playgo_speed = a1;
        return 0;
    case SHADPS4_HLE_PLAYGO_SET_LANGUAGE:
        hle->playgo_language_mask = a1;
        return 0;
    case SHADPS4_HLE_PLAYGO_SET_TODO:
        if (!a1) {
            return SHADPS4_PLAYGO_ERROR_BAD_POINTER;
        }
        return a2 ? 0 : SHADPS4_PLAYGO_ERROR_BAD_SIZE;
    default:
        return SHADPS4_PLAYGO_ERROR_INVALID_ARGUMENT;
    }
}

static bool shadps4_services4_webapi_parent(ShadPS4HLEState *hle,
                                            uint64_t parent, uint64_t child,
                                            uint8_t type)
{
    return shadps4_hle_service_is(hle, child, type) &&
           hle->service_parents[child - 0x400] == parent;
}

static bool shadps4_services4_read_push_id(CPUState *cs, uint64_t address,
                                           uint64_t *handle)
{
    uint64_t raw;

    if (!shadps4_services4_read(cs, address, &raw, sizeof(raw))) {
        return false;
    }
    *handle = le64_to_cpu(raw);
    return true;
}

static uint64_t shadps4_services4_webapi2(ShadPS4HLEState *hle, CPUState *cs,
                                          uint64_t number, uint64_t a0,
                                          uint64_t a1, uint64_t a2,
                                          uint64_t a3, uint64_t a4,
                                          uint64_t a5)
{
    int handle;

    switch (number) {
    case SHADPS4_HLE_WEBAPI2_PUSH_INTERNAL_INIT: {
        uint8_t args[40];
        uint64_t size;

        if (!a0 || !shadps4_services4_read(cs, a0, args, 32)) {
            return SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
        }
        size = ldq_le_p(args + 24);
        if (size != 32) {
            if (!shadps4_services4_read(cs, a0, args, sizeof(args)) ||
                ldq_le_p(args + 32) != sizeof(args)) {
                return SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
            }
        }
        if (!shadps4_hle_service_is(hle, ldl_le_p(args),
                                    SHADPS4_SERVICE_HTTP_CONTEXT)) {
            return SHADPS4_NP_WEBAPI2_ERROR_INVALID_LIB_CONTEXT_ID;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_WEBAPI_CONTEXT, ldl_le_p(args));
        if (handle >= 0) {
            hle->service_value[handle - 0x400] = ldq_le_p(args + 8);
        }
        return handle < 0 ? SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
               handle;
    }
    case SHADPS4_HLE_WEBAPI2_PUSH_CREATE_HANDLE:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_CONTEXT)) {
            return SHADPS4_NP_WEBAPI2_ERROR_INVALID_LIB_CONTEXT_ID;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_WEBAPI_HANDLE, a0);
        return handle < 0 ? SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
               handle;
    case SHADPS4_HLE_WEBAPI2_PUSH_DELETE_HANDLE:
        if (!shadps4_services4_webapi_parent(
                hle, a0, a1, SHADPS4_SERVICE_WEBAPI_HANDLE)) {
            return SHADPS4_NP_WEBAPI2_ERROR_CONTEXT_NOT_FOUND;
        }
        return shadps4_hle_service_delete(
            hle, a1, SHADPS4_SERVICE_WEBAPI_HANDLE);
    case SHADPS4_HLE_WEBAPI2_PUSH_ABORT_HANDLE:
        if (!shadps4_services4_webapi_parent(
                hle, a0, a1, SHADPS4_SERVICE_WEBAPI_HANDLE)) {
            return SHADPS4_NP_WEBAPI2_ERROR_CONTEXT_NOT_FOUND;
        }
        hle->service_active[a1 - 0x400] = false;
        return 0;
    case SHADPS4_HLE_WEBAPI2_PUSH_SET_TIMEOUT:
        if (!shadps4_services4_webapi_parent(
                hle, a0, a1, SHADPS4_SERVICE_WEBAPI_HANDLE)) {
            return SHADPS4_NP_WEBAPI2_ERROR_CONTEXT_NOT_FOUND;
        }
        hle->service_value[a1 - 0x400] = a2;
        return 0;
    case SHADPS4_HLE_WEBAPI2_PUSH_CREATE_FILTER:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_CONTEXT) ||
            !shadps4_services4_webapi_parent(
                hle, a0, a1, SHADPS4_SERVICE_WEBAPI_HANDLE) ||
            (a2 && (int32_t)a3 == -1) || !a4 || !a5) {
            return SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_WEBAPI_FILTER, a0);
        if (handle >= 0) {
            hle->service_value[handle - 0x400] = a1;
        }
        return handle < 0 ? SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
               handle;
    case SHADPS4_HLE_WEBAPI2_PUSH_DELETE_FILTER:
        if (!shadps4_services4_webapi_parent(
                hle, a0, a1, SHADPS4_SERVICE_WEBAPI_FILTER)) {
            return SHADPS4_NP_WEBAPI2_ERROR_CONTEXT_NOT_FOUND;
        }
        return shadps4_hle_service_delete(
            hle, a1, SHADPS4_SERVICE_WEBAPI_FILTER);
    case SHADPS4_HLE_WEBAPI2_PUSH_REGISTER_CALLBACK:
    case SHADPS4_HLE_WEBAPI2_PUSH_REGISTER_CONTEXT_CALLBACK:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_USER) || !a2 ||
            (a1 && !shadps4_hle_service_is(
                hle, a1, SHADPS4_SERVICE_WEBAPI_FILTER))) {
            return SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_WEBAPI_CALLBACK, a0);
        if (handle >= 0) {
            hle->service_value[handle - 0x400] = a1;
            hle->service_user_data[handle - 0x400] = a2;
            hle->service_aux_value[handle - 0x400] = a3;
            hle->service_active[handle - 0x400] =
                number == SHADPS4_HLE_WEBAPI2_PUSH_REGISTER_CONTEXT_CALLBACK;
        }
        return handle < 0 ? SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
               handle;
    case SHADPS4_HLE_WEBAPI2_PUSH_UNREGISTER_CALLBACK:
        if (!shadps4_services4_webapi_parent(
                hle, a0, a1, SHADPS4_SERVICE_WEBAPI_CALLBACK)) {
            return SHADPS4_NP_WEBAPI2_ERROR_CONTEXT_NOT_FOUND;
        }
        return shadps4_hle_service_delete(
            hle, a1, SHADPS4_SERVICE_WEBAPI_CALLBACK);
    case SHADPS4_HLE_WEBAPI2_PUSH_CREATE_CONTEXT: {
        uint8_t id[16] = { 0 };

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_USER) || !a1) {
            return SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_WEBAPI_PUSH_CONTEXT, a0);
        if (handle < 0) {
            return SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
        }
        stq_le_p(id, handle);
        if (!shadps4_services4_write(cs, a1, id, sizeof(id))) {
            shadps4_hle_service_delete(
                hle, handle, SHADPS4_SERVICE_WEBAPI_PUSH_CONTEXT);
            return SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
        }
        return 0;
    }
    case SHADPS4_HLE_WEBAPI2_PUSH_DELETE_CONTEXT:
    case SHADPS4_HLE_WEBAPI2_PUSH_START_CONTEXT: {
        uint64_t push;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_USER) ||
            !shadps4_services4_read_push_id(cs, a1, &push) ||
            !shadps4_services4_webapi_parent(
                hle, a0, push, SHADPS4_SERVICE_WEBAPI_PUSH_CONTEXT)) {
            return SHADPS4_NP_WEBAPI2_ERROR_CONTEXT_NOT_FOUND;
        }
        if (number == SHADPS4_HLE_WEBAPI2_PUSH_START_CONTEXT) {
            hle->service_active[push - 0x400] = true;
            return 0;
        }
        return shadps4_hle_service_delete(
            hle, push, SHADPS4_SERVICE_WEBAPI_PUSH_CONTEXT);
    }
    default:
        return SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
    }
}

static int shadps4_services4_auth_slot(ShadPS4HLEState *hle,
                                       uint64_t request)
{
    uint64_t slot;

    if (request <= 0x10000000) {
        return -1;
    }
    slot = request - 0x10000000;
    return slot < SHADPS4_HLE_MAX_SERVICE_OBJECTS &&
           hle->service_objects[slot] == SHADPS4_SERVICE_NP_AUTH_REQUEST ?
           slot : -1;
}

static uint64_t shadps4_services4_auth_create(ShadPS4HLEState *hle,
                                              bool async)
{
    uint32_t count = 0;
    uint32_t slot;
    int handle;

    for (slot = 1; slot < SHADPS4_HLE_MAX_SERVICE_OBJECTS; slot++) {
        count += hle->service_objects[slot] ==
                 SHADPS4_SERVICE_NP_AUTH_REQUEST;
    }
    if (count >= 16) {
        return SHADPS4_NP_AUTH_ERROR_REQUEST_MAX;
    }
    handle = shadps4_hle_service_alloc(
        hle, SHADPS4_SERVICE_NP_AUTH_REQUEST, 0);
    if (handle < 0) {
        return SHADPS4_NP_AUTH_ERROR_REQUEST_MAX;
    }
    slot = handle - 0x400;
    hle->service_nonblock[slot] = async;
    hle->service_aux_value[slot] = 1;
    return 0x10000000u + slot;
}

static uint64_t shadps4_services4_np_auth(ShadPS4HLEState *hle,
                                          CPUState *cs, uint64_t number,
                                          uint64_t a0, uint64_t a1,
                                          uint64_t a2, uint64_t a3)
{
    uint8_t param[40];
    int slot;
    bool legacy;
    uint64_t expected;

    switch (number) {
    case SHADPS4_HLE_NP_AUTH_CREATE_REQUEST:
        return shadps4_services4_auth_create(hle, false);
    case SHADPS4_HLE_NP_AUTH_CREATE_ASYNC_REQUEST:
        if (!shadps4_services4_read(cs, a0, param, 24)) {
            return SHADPS4_NP_AUTH_ERROR_INVALID_ARGUMENT;
        }
        if (ldq_le_p(param) != 24) {
            return SHADPS4_NP_AUTH_ERROR_INVALID_SIZE;
        }
        return shadps4_services4_auth_create(hle, true);
    case SHADPS4_HLE_NP_AUTH_ABORT_REQUEST:
        slot = shadps4_services4_auth_slot(hle, a0);
        if (slot < 0) {
            return SHADPS4_NP_AUTH_ERROR_REQUEST_NOT_FOUND;
        }
        if (hle->service_aux_value[slot] != 3) {
            hle->service_aux_value[slot] = 2;
            hle->service_value[slot] = SHADPS4_NP_AUTH_ERROR_ABORTED;
        }
        return 0;
    case SHADPS4_HLE_NP_AUTH_WAIT_ASYNC:
    case SHADPS4_HLE_NP_AUTH_POLL_ASYNC:
        if (!a1) {
            return SHADPS4_NP_AUTH_ERROR_INVALID_ARGUMENT;
        }
        slot = shadps4_services4_auth_slot(hle, a0);
        if (slot < 0) {
            return SHADPS4_NP_AUTH_ERROR_REQUEST_NOT_FOUND;
        }
        if (!hle->service_nonblock[slot] ||
            hle->service_aux_value[slot] == 1) {
            return SHADPS4_NP_AUTH_ERROR_INVALID_ID;
        }
        return shadps4_services4_write_u32(
                   cs, a1, hle->service_value[slot]) ? 0 :
               SHADPS4_NP_AUTH_ERROR_INVALID_ARGUMENT;
    case SHADPS4_HLE_NP_AUTH_DELETE_REQUEST:
        slot = shadps4_services4_auth_slot(hle, a0);
        if (slot < 0) {
            return SHADPS4_NP_AUTH_ERROR_REQUEST_NOT_FOUND;
        }
        shadps4_hle_service_delete(
            hle, 0x400 + slot, SHADPS4_SERVICE_NP_AUTH_REQUEST);
        return 0;
    default:
        break;
    }

    legacy = number == SHADPS4_HLE_NP_AUTH_GET_AUTH_CODE ||
             number == SHADPS4_HLE_NP_AUTH_GET_ID_TOKEN;
    expected = number == SHADPS4_HLE_NP_AUTH_GET_AUTH_CODE ||
               number == SHADPS4_HLE_NP_AUTH_GET_AUTH_CODE_A ? 32 : 40;
    if (!a1 || !a2 || !shadps4_services4_read(cs, a1, param, expected)) {
        return SHADPS4_NP_AUTH_ERROR_INVALID_ARGUMENT;
    }
    if (ldq_le_p(param) != expected) {
        return SHADPS4_NP_AUTH_ERROR_INVALID_SIZE;
    }
    if (legacy) {
        if (!ldq_le_p(param + 8) || !ldq_le_p(param + 16) ||
            !ldq_le_p(param + expected - 8) ||
            (expected == 40 && !ldq_le_p(param + 24))) {
            return SHADPS4_NP_AUTH_ERROR_INVALID_ARGUMENT;
        }
        return SHADPS4_NP_ERROR_USER_NOT_FOUND;
    }
    if ((int32_t)ldl_le_p(param + 8) == -1 || !ldq_le_p(param + 16) ||
        !ldq_le_p(param + expected - 8) ||
        (expected == 40 && !ldq_le_p(param + 24))) {
        return SHADPS4_NP_AUTH_ERROR_INVALID_ARGUMENT;
    }
    slot = shadps4_services4_auth_slot(hle, a0);
    if (slot < 0) {
        return SHADPS4_NP_AUTH_ERROR_REQUEST_NOT_FOUND;
    }
    if (hle->service_aux_value[slot] == 3) {
        return SHADPS4_NP_AUTH_ERROR_INVALID_ARGUMENT;
    }
    if (hle->service_aux_value[slot] == 2) {
        return SHADPS4_NP_AUTH_ERROR_ABORTED;
    }
    hle->service_aux_value[slot] = 3;
    hle->service_value[slot] = SHADPS4_NP_ERROR_SIGNED_OUT;
    return hle->service_nonblock[slot] ? 0 : SHADPS4_NP_ERROR_SIGNED_OUT;
}

static uint64_t shadps4_services4_ime(ShadPS4HLEState *hle, CPUState *cs,
                                      uint64_t number, uint64_t a0,
                                      uint64_t a1, uint64_t a2,
                                      uint64_t a3, uint64_t a4)
{
    uint8_t data[96] = { 0 };
    int handle;
    uint64_t slot;

    (void)a3;
    (void)a4;

    switch (number) {
    case SHADPS4_HLE_IME_PARAM_INIT:
        if (!a0) {
            return 0;
        }
        stl_le_p(data, UINT32_MAX);
        shadps4_services4_write(cs, a0, data, sizeof(data));
        return 0;
    case SHADPS4_HLE_IME_OPEN:
        if (shadps4_services4_find(hle, SHADPS4_SERVICE_IME_SESSION) >= 0) {
            return SHADPS4_IME_ERROR_BUSY;
        }
        if (!shadps4_services4_read(cs, a0, data, sizeof(data))) {
            return SHADPS4_IME_ERROR_INVALID_ADDRESS;
        }
        if ((int32_t)ldl_le_p(data) == -1) {
            return SHADPS4_IME_ERROR_INVALID_USER_ID;
        }
        if (ldl_le_p(data + 4) > 4) {
            return SHADPS4_IME_ERROR_INVALID_TYPE;
        }
        if (ldl_le_p(data + 36) > 2048) {
            return SHADPS4_IME_ERROR_INVALID_MAX_LENGTH;
        }
        if (ldl_le_p(data + 36) && !ldq_le_p(data + 40)) {
            return SHADPS4_IME_ERROR_INVALID_INPUT_BUFFER;
        }
        if (!ldq_le_p(data + 80)) {
            return SHADPS4_IME_ERROR_INVALID_HANDLER;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_IME_SESSION, ldl_le_p(data));
        if (handle < 0) {
            return SHADPS4_IME_ERROR_BUSY;
        }
        slot = handle - 0x400;
        hle->service_value[slot] = ldq_le_p(data + 80);
        hle->service_user_data[slot] = ldq_le_p(data + 72);
        hle->service_aux_value[slot] = ldq_le_p(data + 40);
        hle->service_content_length[slot] = ldl_le_p(data + 36);
        return 0;
    case SHADPS4_HLE_IME_CLOSE:
        handle = shadps4_services4_find(hle, SHADPS4_SERVICE_IME_SESSION);
        return handle < 0 ? SHADPS4_IME_ERROR_NOT_OPENED :
               shadps4_hle_service_delete(
                   hle, handle, SHADPS4_SERVICE_IME_SESSION);
    case SHADPS4_HLE_IME_UPDATE:
        return shadps4_services4_find(hle, SHADPS4_SERVICE_IME_SESSION) < 0 &&
               shadps4_services4_find(hle, SHADPS4_SERVICE_IME_KEYBOARD) < 0 ?
               SHADPS4_IME_ERROR_NOT_OPENED : 0;
    case SHADPS4_HLE_IME_SET_TEXT: {
        g_autofree uint8_t *text = NULL;
        uint64_t bytes;

        handle = shadps4_services4_find(hle, SHADPS4_SERVICE_IME_SESSION);
        if (handle < 0) {
            return SHADPS4_IME_ERROR_NOT_OPENED;
        }
        slot = handle - 0x400;
        if (!a0 || a1 > hle->service_content_length[slot]) {
            return SHADPS4_IME_ERROR_INVALID_INPUT_BUFFER;
        }
        bytes = (a1 + 1) * 2;
        text = g_malloc0(bytes);
        if (a1 && !shadps4_services4_read(cs, a0, text, a1 * 2)) {
            return SHADPS4_IME_ERROR_INVALID_ADDRESS;
        }
        return shadps4_services4_write(
                   cs, hle->service_aux_value[slot], text, bytes) ? 0 :
               SHADPS4_IME_ERROR_INVALID_INPUT_BUFFER;
    }
    case SHADPS4_HLE_IME_SET_CARET:
        handle = shadps4_services4_find(hle, SHADPS4_SERVICE_IME_SESSION);
        if (handle < 0) {
            return SHADPS4_IME_ERROR_NOT_OPENED;
        }
        if (!shadps4_services4_read(cs, a0, data, 16)) {
            return SHADPS4_IME_ERROR_INVALID_ADDRESS;
        }
        return ldl_le_p(data + 12) <=
               hle->service_content_length[handle - 0x400] ? 0 :
               SHADPS4_IME_ERROR_INVALID_MAX_LENGTH;
    case SHADPS4_HLE_IME_SET_TEXT_GEOMETRY: {
        float x;
        float y;
        uint32_t raw;

        if (shadps4_services4_find(hle, SHADPS4_SERVICE_IME_SESSION) < 0) {
            return SHADPS4_IME_ERROR_NOT_OPENED;
        }
        if ((a0 != 2 && a0 != 3) ||
            !shadps4_services4_read(cs, a1, data, 16)) {
            return SHADPS4_IME_ERROR_INVALID_ADDRESS;
        }
        raw = ldl_le_p(data);
        memcpy(&x, &raw, sizeof(x));
        raw = ldl_le_p(data + 4);
        memcpy(&y, &raw, sizeof(y));
        return isfinite(x) && isfinite(y) && x >= 0.0f && x < 1920.0f &&
               y >= 0.0f && y < 1080.0f ? 0 :
               SHADPS4_IME_ERROR_INVALID_PARAM;
    }
    case SHADPS4_HLE_IME_GET_PANEL_SIZE: {
        uint32_t width = 0x319;
        uint32_t height = 0x198;
        uint32_t type;
        uint32_t option;

        if (!shadps4_services4_read(cs, a0, data, sizeof(data)) || !a1 ||
            !a2) {
            return SHADPS4_IME_ERROR_INVALID_ADDRESS;
        }
        type = ldl_le_p(data + 4);
        option = ldl_le_p(data + 32);
        if (type > 4) {
            return SHADPS4_IME_ERROR_INVALID_TYPE;
        }
        if (type == 4) {
            width = 0x172;
            height = hle->compiled_sdk_version > 0x16fffff ? 0x192 : 0x170;
        } else if (type == 1 || (option & 0xc0000004u) == 4) {
            height = hle->compiled_sdk_version > 0x16fffff ? 0x198 : 0x170;
        }
        if (option & 0x4000u) {
            width <<= 1;
            height <<= 1;
        }
        return shadps4_services4_write_u32(cs, a1, width) &&
               shadps4_services4_write_u32(cs, a2, height) ? 0 :
               SHADPS4_IME_ERROR_INVALID_ADDRESS;
    }
    case SHADPS4_HLE_IME_GET_PANEL_POSITION:
        memset(data, 0, 28);
        return !a0 || shadps4_services4_write(cs, a0, data, 28) ? 0 :
               SHADPS4_IME_ERROR_INVALID_ADDRESS;
    case SHADPS4_HLE_IME_KEYBOARD_OPEN:
        if (shadps4_services4_find(hle, SHADPS4_SERVICE_IME_KEYBOARD) >= 0) {
            return SHADPS4_IME_ERROR_BUSY;
        }
        if ((int32_t)a0 == -1) {
            return SHADPS4_IME_ERROR_INVALID_USER_ID;
        }
        if (!shadps4_services4_read(cs, a1, data, 32)) {
            return SHADPS4_IME_ERROR_INVALID_ADDRESS;
        }
        if (ldl_le_p(data + 4) || ldq_le_p(data + 24)) {
            return SHADPS4_IME_ERROR_INVALID_RESERVED;
        }
        if (!ldq_le_p(data + 16)) {
            return SHADPS4_IME_ERROR_INVALID_HANDLER;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_IME_KEYBOARD, a0);
        if (handle < 0) {
            return SHADPS4_IME_ERROR_BUSY;
        }
        hle->service_user_data[handle - 0x400] = ldq_le_p(data + 8);
        hle->service_value[handle - 0x400] = ldq_le_p(data + 16);
        return 0;
    case SHADPS4_HLE_IME_KEYBOARD_CLOSE:
        handle = shadps4_services4_find(hle, SHADPS4_SERVICE_IME_KEYBOARD);
        if (handle < 0) {
            return SHADPS4_IME_ERROR_NOT_OPENED;
        }
        if ((int32_t)a0 == -1 || hle->service_parents[handle - 0x400] != a0) {
            return SHADPS4_IME_ERROR_INVALID_USER_ID;
        }
        return shadps4_hle_service_delete(
            hle, handle, SHADPS4_SERVICE_IME_KEYBOARD);
    case SHADPS4_HLE_IME_KEYBOARD_GET_RESOURCE:
        if (!a1) {
            return SHADPS4_IME_ERROR_INVALID_ADDRESS;
        }
        memset(data, 0, 24);
        stl_le_p(data, a0);
        if (!shadps4_services4_write(cs, a1, data, 24)) {
            return SHADPS4_IME_ERROR_INVALID_ADDRESS;
        }
        handle = shadps4_services4_find(hle, SHADPS4_SERVICE_IME_KEYBOARD);
        return handle < 0 ? SHADPS4_IME_ERROR_NOT_OPENED :
               SHADPS4_IME_ERROR_CONNECTION_FAILED;
    case SHADPS4_HLE_IME_KEYBOARD_GET_INFO:
        memset(data, 0, 36);
        return a1 && shadps4_services4_write(cs, a1, data, 36) ? 0 :
               SHADPS4_IME_ERROR_INVALID_ADDRESS;
    default:
        return SHADPS4_IME_ERROR_INVALID_OPTION;
    }
}

static uint32_t shadps4_services4_trophy_count(ShadPS4HLEState *hle,
                                               uint32_t slot)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = 0; i < 4; i++) {
        count += ctpop32(hle->trophy_flags[slot][i]);
    }
    return count;
}

static uint64_t shadps4_services4_trophy_validate(ShadPS4HLEState *hle,
                                                  uint64_t context,
                                                  uint64_t handle,
                                                  uint32_t *slot)
{
    if (!shadps4_hle_service_is(hle, context,
                                SHADPS4_SERVICE_TROPHY_CONTEXT)) {
        return SHADPS4_TROPHY_ERROR_INVALID_CONTEXT;
    }
    if (!shadps4_hle_service_is(hle, handle,
                                SHADPS4_SERVICE_TROPHY_HANDLE)) {
        return SHADPS4_TROPHY_ERROR_INVALID_HANDLE;
    }
    *slot = context - 0x400;
    return hle->service_active[*slot] ? 0 :
           SHADPS4_TROPHY_ERROR_NOT_REGISTERED;
}

static uint64_t shadps4_services4_trophy(ShadPS4HLEState *hle, CPUState *cs,
                                         uint64_t number, uint64_t a0,
                                         uint64_t a1, uint64_t a2,
                                         uint64_t a3, uint64_t a4)
{
    uint8_t data[0x4a0] = { 0 };
    uint32_t slot;
    uint32_t count;
    uint64_t ret;
    int handle;

    switch (number) {
    case SHADPS4_HLE_TROPHY_CREATE_CONTEXT:
        if (!a0 || (int32_t)a1 == -1 || a3) {
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_TROPHY_CONTEXT, a1);
        if (handle < 0) {
            return SHADPS4_TROPHY_ERROR_INVALID_CONTEXT;
        }
        slot = handle - 0x400;
        hle->service_aux_value[slot] = a2;
        memset(hle->trophy_flags[slot], 0, sizeof(hle->trophy_flags[slot]));
        if (!shadps4_services4_write_u32(cs, a0, handle)) {
            shadps4_hle_service_delete(
                hle, handle, SHADPS4_SERVICE_TROPHY_CONTEXT);
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        return 0;
    case SHADPS4_HLE_TROPHY_DESTROY_CONTEXT:
        return shadps4_hle_service_is(
                   hle, a0, SHADPS4_SERVICE_TROPHY_CONTEXT) ?
               shadps4_hle_service_delete(
                   hle, a0, SHADPS4_SERVICE_TROPHY_CONTEXT) :
               SHADPS4_TROPHY_ERROR_INVALID_CONTEXT;
    case SHADPS4_HLE_TROPHY_CREATE_HANDLE:
        if (!a0) {
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_TROPHY_HANDLE, 0);
        if (handle < 0) {
            return SHADPS4_TROPHY_ERROR_INVALID_HANDLE;
        }
        if (!shadps4_services4_write_u32(cs, a0, handle)) {
            shadps4_hle_service_delete(
                hle, handle, SHADPS4_SERVICE_TROPHY_HANDLE);
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        return 0;
    case SHADPS4_HLE_TROPHY_DESTROY_HANDLE:
        return shadps4_hle_service_is(
                   hle, a0, SHADPS4_SERVICE_TROPHY_HANDLE) ?
               shadps4_hle_service_delete(
                   hle, a0, SHADPS4_SERVICE_TROPHY_HANDLE) :
               SHADPS4_TROPHY_ERROR_INVALID_HANDLE;
    case SHADPS4_HLE_TROPHY_REGISTER_CONTEXT:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_TROPHY_CONTEXT)) {
            return SHADPS4_TROPHY_ERROR_INVALID_CONTEXT;
        }
        if (!shadps4_hle_service_is(hle, a1,
                                    SHADPS4_SERVICE_TROPHY_HANDLE) || a2) {
            return SHADPS4_TROPHY_ERROR_INVALID_HANDLE;
        }
        slot = a0 - 0x400;
        if (hle->service_active[slot]) {
            return SHADPS4_TROPHY_ERROR_ALREADY_REGISTERED;
        }
        hle->service_active[slot] = true;
        return 0;
    default:
        break;
    }

    ret = shadps4_services4_trophy_validate(hle, a0, a1, &slot);
    if (ret) {
        return ret;
    }
    count = shadps4_services4_trophy_count(hle, slot);
    switch (number) {
    case SHADPS4_HLE_TROPHY_UNLOCK:
        if ((int32_t)a2 < 0 || a2 >= 128) {
            return SHADPS4_TROPHY_ERROR_INVALID_TROPHY_ID;
        }
        if (hle->trophy_flags[slot][a2 >> 5] & (1u << (a2 & 31))) {
            return SHADPS4_TROPHY_ERROR_ALREADY_UNLOCKED;
        }
        hle->trophy_flags[slot][a2 >> 5] |= 1u << (a2 & 31);
        return !a3 || shadps4_services4_write_u32(cs, a3, UINT32_MAX) ? 0 :
               SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
    case SHADPS4_HLE_TROPHY_GET_UNLOCK_STATE:
        if (!a2 || !a3) {
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        for (count = 0; count < 4; count++) {
            stl_le_p(data + count * 4, hle->trophy_flags[slot][count]);
        }
        return shadps4_services4_write(cs, a2, data, 16) &&
               shadps4_services4_write_u32(cs, a3, 128) ? 0 :
               SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
    case SHADPS4_HLE_TROPHY_GET_GAME_ICON:
        if (!a3) {
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        shadps4_services4_write_u64(cs, a3, 0);
        return SHADPS4_TROPHY_ERROR_ICON_NOT_FOUND;
    case SHADPS4_HLE_TROPHY_GET_TROPHY_ICON:
        if ((int32_t)a2 < 0 || a2 >= 128) {
            return SHADPS4_TROPHY_ERROR_INVALID_TROPHY_ID;
        }
        if (!a4) {
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        shadps4_services4_write_u64(cs, a4, 0);
        return SHADPS4_TROPHY_ERROR_ICON_NOT_FOUND;
    case SHADPS4_HLE_TROPHY_GET_GAME_INFO:
        if (!a2 || !a3) {
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        stq_le_p(data, 0x4a0);
        stl_le_p(data + 8, 1);
        stl_le_p(data + 12, 128);
        stl_le_p(data + 28, 128);
        memcpy(data + 32, "Trophy Set", 11);
        if (!shadps4_services4_write(cs, a2, data, 0x4a0)) {
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        memset(data, 0, 0x20);
        stq_le_p(data, 0x20);
        stl_le_p(data + 8, count);
        stl_le_p(data + 24, count);
        stl_le_p(data + 28, count * 100 / 128);
        return shadps4_services4_write(cs, a3, data, 0x20) ? 0 :
               SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
    case SHADPS4_HLE_TROPHY_GET_GROUP_INFO:
        if ((int32_t)a2 != -1) {
            return SHADPS4_TROPHY_ERROR_INVALID_GROUP_ID;
        }
        if (!a3 || !a4) {
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        stq_le_p(data, 0x4a0);
        stl_le_p(data + 8, UINT32_MAX);
        stl_le_p(data + 12, 128);
        stl_le_p(data + 28, 128);
        memcpy(data + 32, "Base Game", 10);
        if (!shadps4_services4_write(cs, a3, data, 0x4a0)) {
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        memset(data, 0, 0x28);
        stq_le_p(data, 0x28);
        stl_le_p(data + 8, UINT32_MAX);
        stl_le_p(data + 12, count);
        stl_le_p(data + 28, count);
        stl_le_p(data + 32, count * 100 / 128);
        return shadps4_services4_write(cs, a4, data, 0x28) ? 0 :
               SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
    case SHADPS4_HLE_TROPHY_GET_TROPHY_INFO:
        if ((int32_t)a2 < 0 || a2 >= 128) {
            return SHADPS4_TROPHY_ERROR_INVALID_TROPHY_ID;
        }
        if (!a3 || !a4) {
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        stq_le_p(data, 0x498);
        stl_le_p(data + 8, a2);
        stl_le_p(data + 12, 4);
        stl_le_p(data + 16, UINT32_MAX);
        g_snprintf((char *)data + 24, 128, "Trophy %u", (unsigned)a2);
        if (!shadps4_services4_write(cs, a3, data, 0x498)) {
            return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
        }
        memset(data, 0, 24);
        stq_le_p(data, 24);
        stl_le_p(data + 8, a2);
        data[12] = !!(hle->trophy_flags[slot][a2 >> 5] &
                      (1u << (a2 & 31)));
        return shadps4_services4_write(cs, a4, data, 24) ? 0 :
               SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
    default:
        return SHADPS4_TROPHY_ERROR_INVALID_ARGUMENT;
    }
}

static uint64_t shadps4_hle_dispatch_services4(
    ShadPS4HLEState *hle, CPUState *cs, uint64_t number, uint64_t a0,
    uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
    uint64_t a6)
{
    (void)a6;
    if (number > SHADPS4_HLE_PLAYGO_BEGIN &&
        number < SHADPS4_HLE_PLAYGO_END) {
        return shadps4_services4_playgo(hle, cs, number, a0, a1, a2, a3);
    }
    if (number > SHADPS4_HLE_WEBAPI2_PUSH_BEGIN &&
        number < SHADPS4_HLE_WEBAPI2_PUSH_END) {
        return shadps4_services4_webapi2(
            hle, cs, number, a0, a1, a2, a3, a4, a5);
    }
    if (number > SHADPS4_HLE_NP_AUTH_BEGIN &&
        number < SHADPS4_HLE_NP_AUTH_END) {
        return shadps4_services4_np_auth(
            hle, cs, number, a0, a1, a2, a3);
    }
    if (number > SHADPS4_HLE_IME_BEGIN && number < SHADPS4_HLE_IME_END) {
        return shadps4_services4_ime(
            hle, cs, number, a0, a1, a2, a3, a4);
    }
    return shadps4_services4_trophy(
        hle, cs, number, a0, a1, a2, a3, a4);
}
