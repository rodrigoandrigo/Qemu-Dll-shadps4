/*
 * QEMU host embedding lifecycle API.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#define QEMU_HOST_BUILD
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/qemu-host.h"
#include "qemu/thread.h"
#include "qemu/units.h"
#include "qapi/qapi-commands-ui.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/input.h"

static GMutex host_state_lock;
static bool host_initializing;
static bool host_initialized;
static bool host_running;
static bool host_loop_exited;
static bool host_thread_started;
static bool host_join_active;
static bool host_step_active;
static bool host_cleaned;
static int host_exit_status;
static bool host_last_fault_valid;
static QemuHostFaultInfo host_last_fault;
static QemuThread host_loop_thread;
static GMutex host_callback_lock;
static QemuHostLogCallback host_log_callback;
static void *host_log_opaque;
static QemuHostVideoCallback host_video_callback;
static void *host_video_opaque;
static QemuHostAudioCallback host_audio_callback;
static void *host_audio_opaque;
static QemuHostPadOutputCallback host_pad_output_callback;
static void *host_pad_output_opaque;
static QemuHostStorageCallbacks host_storage_callbacks;
static void *host_storage_opaque;
static QemuHostBrokeredStorageCallbacks host_brokered_callbacks;
static void *host_brokered_opaque;
static QemuHostDialogRequestCallback host_dialog_callback;
static void *host_dialog_opaque;
static QemuHostNetworkCallbacks host_network_callbacks;
static void *host_network_opaque;
static QemuHostHttpCallbacks host_http_callbacks;
static void *host_http_opaque;
static QemuHostMediaCallbacks host_media_callbacks;
static void *host_media_opaque;
static QemuHostLaunchCallback host_launch_callback;
static void *host_launch_opaque;
static QemuHostHLECallback host_hle_callback;
static void *host_hle_opaque;
static QemuHostInputSink host_input_sink;
static void *host_input_sink_opaque;

typedef enum QemuHostBrokeredMountType {
    QEMU_HOST_BROKERED_FILE,
    QEMU_HOST_BROKERED_FOLDER,
} QemuHostBrokeredMountType;

typedef struct QemuHostBrokeredMount {
    char *path;
    QemuHostBrokeredMountType type;
    void *object;
    void *stream;
} QemuHostBrokeredMount;

typedef struct QemuHostBrokeredHandle {
    int64_t public_handle;
    int64_t backend_handle;
    char *virtual_path;
    char *relative_path;
} QemuHostBrokeredHandle;

static GPtrArray *host_brokered_mounts;
static GHashTable *host_brokered_handles;
static uint64_t host_next_brokered_handle = UINT64_C(0x4000000000000000);

static bool host_brokered_path_valid(const char *path)
{
    const char *component;

    if (!path || path[0] != '/' || path[1] == '\0' || strchr(path, '\\')) {
        return false;
    }
    for (component = path + 1; *component;) {
        const char *end = strchr(component, '/');
        size_t length = end ? end - component : strlen(component);

        if (!length || (length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) {
            return false;
        }
        if (!end) {
            break;
        }
        component = end + 1;
        if (!*component) {
            return false;
        }
    }
    return true;
}

static void host_brokered_mount_free(QemuHostBrokeredMount *mount)
{
    if (host_brokered_callbacks.release) {
        if (mount->stream) {
            host_brokered_callbacks.release(host_brokered_opaque,
                                             mount->stream);
        }
        host_brokered_callbacks.release(host_brokered_opaque, mount->object);
    }
    g_free(mount->path);
    g_free(mount);
}

static void host_brokered_handle_free(QemuHostBrokeredHandle *handle)
{
    g_free(handle->virtual_path);
    g_free(handle->relative_path);
    g_free(handle);
}

static void host_brokered_ensure_tables(void)
{
    if (!host_brokered_mounts) {
        host_brokered_mounts = g_ptr_array_new_with_free_func(
            (GDestroyNotify)host_brokered_mount_free);
    }
    if (!host_brokered_handles) {
        host_brokered_handles = g_hash_table_new_full(
            g_int64_hash, g_int64_equal, g_free,
            (GDestroyNotify)host_brokered_handle_free);
    }
}

static QemuHostBrokeredMount *host_brokered_find_mount_locked(
    const char *path, const char **relative_path)
{
    QemuHostBrokeredMount *best = NULL;
    size_t best_length = 0;
    guint i;

    if (!host_brokered_mounts) {
        return NULL;
    }
    for (i = 0; i < host_brokered_mounts->len; i++) {
        QemuHostBrokeredMount *mount =
            g_ptr_array_index(host_brokered_mounts, i);
        size_t length = strlen(mount->path);

        if (strncmp(path, mount->path, length) ||
            (path[length] && path[length] != '/')) {
            continue;
        }
        if (mount->type == QEMU_HOST_BROKERED_FILE && path[length]) {
            continue;
        }
        if (length > best_length) {
            best = mount;
            best_length = length;
        }
    }
    if (best && relative_path) {
        *relative_path = path + best_length;
        if (**relative_path == '/') {
            (*relative_path)++;
        }
    }
    return best;
}

static QemuHostBrokeredHandle *host_brokered_find_handle_locked(
    int64_t handle)
{
    return host_brokered_handles ?
           g_hash_table_lookup(host_brokered_handles, &handle) : NULL;
}

static int64_t host_brokered_publish_handle(int64_t backend_handle,
                                            const char *virtual_path,
                                            const char *relative_path)
{
    QemuHostBrokeredHandle *handle = g_new0(QemuHostBrokeredHandle, 1);
    int64_t *key = g_new(int64_t, 1);

    g_mutex_lock(&host_callback_lock);
    host_brokered_ensure_tables();
    do {
        handle->public_handle = host_next_brokered_handle++;
        if (host_next_brokered_handle >= INT64_MAX) {
            host_next_brokered_handle = UINT64_C(0x4000000000000000);
        }
    } while (g_hash_table_contains(host_brokered_handles,
                                   &handle->public_handle));
    handle->backend_handle = backend_handle;
    handle->virtual_path = g_strdup(virtual_path);
    handle->relative_path = g_strdup(relative_path ?: "");
    *key = handle->public_handle;
    g_hash_table_insert(host_brokered_handles, key, handle);
    g_mutex_unlock(&host_callback_lock);
    return handle->public_handle;
}

static void host_brokered_report_error(const char *operation,
                                       const char *virtual_path,
                                       const char *relative_path,
                                       int64_t result)
{
    int error = result <= INT_MIN ? EIO : -(int)result;

    error_report("brokered storage %s failed: virtual='%s' relative='%s' "
                 "errno=%d (%s)", operation, virtual_path ?: "<unknown>",
                 relative_path ?: "<unknown>", error, strerror(error));
}

typedef struct QemuHostDialogResponse {
    uint64_t request_id;
    int status;
    GBytes *payload;
} QemuHostDialogResponse;

static GQueue host_dialog_responses = G_QUEUE_INIT;
static GHashTable *host_dialog_requests;
static uint64_t host_next_dialog_id = 1;

static void host_dialog_response_free(QemuHostDialogResponse *response)
{
    g_bytes_unref(response->payload);
    g_free(response);
}

static void host_dialog_requests_ensure(void)
{
    if (!host_dialog_requests) {
        host_dialog_requests = g_hash_table_new_full(
            g_int64_hash, g_int64_equal, g_free, NULL);
    }
}

void qemu_host_register_log_callback(QemuHostLogCallback cb, void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    host_log_callback = cb;
    host_log_opaque = opaque;
    g_mutex_unlock(&host_callback_lock);
}

bool qemu_host_log_callback_enabled(void)
{
    bool enabled;

    g_mutex_lock(&host_callback_lock);
    enabled = host_log_callback != NULL;
    g_mutex_unlock(&host_callback_lock);
    return enabled;
}

void qemu_host_emit_log(QemuHostLogLevel level, const char *message)
{
    QemuHostLogCallback cb;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_log_callback;
    opaque = host_log_opaque;
    g_mutex_unlock(&host_callback_lock);
    if (cb) {
        cb(opaque, level, message);
    }
}

void qemu_host_register_video_callback(QemuHostVideoCallback cb, void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    host_video_callback = cb;
    host_video_opaque = opaque;
    g_mutex_unlock(&host_callback_lock);
}

void qemu_host_emit_video_frame(const void *pixels, int width, int height,
                                int stride, QemuHostPixelFormat format)
{
    QemuHostVideoCallback cb;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_video_callback;
    opaque = host_video_opaque;
    g_mutex_unlock(&host_callback_lock);
    if (cb) {
        cb(opaque, pixels, width, height, stride, format);
    }
}

void qemu_host_register_audio_callback(QemuHostAudioCallback cb, void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    host_audio_callback = cb;
    host_audio_opaque = opaque;
    g_mutex_unlock(&host_callback_lock);
}

void qemu_host_emit_audio_frame(const void *samples, size_t size,
                                int sample_rate, int channels,
                                QemuHostAudioFormat format)
{
    QemuHostAudioCallback cb;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_audio_callback;
    opaque = host_audio_opaque;
    g_mutex_unlock(&host_callback_lock);
    if (cb) {
        cb(opaque, samples, size, sample_rate, channels, format);
    }
}

void qemu_host_register_pad_output_callback(QemuHostPadOutputCallback cb,
                                            void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    host_pad_output_callback = cb;
    host_pad_output_opaque = opaque;
    g_mutex_unlock(&host_callback_lock);
}

void qemu_host_emit_pad_output(int controller,
                               const QemuHostPadOutput *output)
{
    QemuHostPadOutputCallback cb;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_pad_output_callback;
    opaque = host_pad_output_opaque;
    g_mutex_unlock(&host_callback_lock);
    if (cb) {
        cb(opaque, controller, output);
    }
}

void qemu_host_register_storage_callbacks(
    const QemuHostStorageCallbacks *callbacks, void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    memset(&host_storage_callbacks, 0, sizeof(host_storage_callbacks));
    host_storage_opaque = NULL;
    if (callbacks && callbacks->size >=
        offsetof(QemuHostStorageCallbacks, seek) &&
        callbacks->version >= 1 && callbacks->version <= 2 &&
        callbacks->open && callbacks->read &&
        callbacks->write && callbacks->close) {
        memcpy(&host_storage_callbacks, callbacks,
               MIN((size_t)callbacks->size, sizeof(host_storage_callbacks)));
        host_storage_opaque = opaque;
    }
    g_mutex_unlock(&host_callback_lock);
}

int qemu_host_register_brokered_storage_callbacks(
    const QemuHostBrokeredStorageCallbacks *callbacks, void *opaque)
{
    if (!callbacks || callbacks->version != 1 ||
        callbacks->size < offsetof(QemuHostBrokeredStorageCallbacks,
                                   truncate) + sizeof(callbacks->truncate) ||
        !callbacks->retain || !callbacks->release ||
        !callbacks->open_file || !callbacks->open_at || !callbacks->read ||
        !callbacks->seek || !callbacks->close || !callbacks->readdir) {
        return -EINVAL;
    }

    g_mutex_lock(&host_state_lock);
    if (host_initializing || host_initialized) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    g_mutex_lock(&host_callback_lock);
    if (host_brokered_mounts && host_brokered_mounts->len) {
        g_mutex_unlock(&host_callback_lock);
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    memset(&host_brokered_callbacks, 0, sizeof(host_brokered_callbacks));
    memcpy(&host_brokered_callbacks, callbacks,
           MIN((size_t)callbacks->size, sizeof(host_brokered_callbacks)));
    host_brokered_opaque = opaque;
    host_brokered_ensure_tables();
    g_mutex_unlock(&host_callback_lock);
    g_mutex_unlock(&host_state_lock);
    return 0;
}

static int host_mount_brokered(const char *virtual_path, void *object,
                               void *stream, QemuHostBrokeredMountType type)
{
    QemuHostBrokeredMount *mount;
    guint i;

    if (!host_brokered_path_valid(virtual_path) || !object ||
        (type == QEMU_HOST_BROKERED_FILE && !stream)) {
        return -EINVAL;
    }

    g_mutex_lock(&host_state_lock);
    if (host_initializing || host_initialized) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    g_mutex_lock(&host_callback_lock);
    if (!host_brokered_callbacks.retain) {
        g_mutex_unlock(&host_callback_lock);
        g_mutex_unlock(&host_state_lock);
        return -ENOSYS;
    }
    host_brokered_ensure_tables();
    for (i = 0; i < host_brokered_mounts->len; i++) {
        QemuHostBrokeredMount *existing =
            g_ptr_array_index(host_brokered_mounts, i);

        if (!strcmp(existing->path, virtual_path)) {
            g_mutex_unlock(&host_callback_lock);
            g_mutex_unlock(&host_state_lock);
            return -EEXIST;
        }
    }
    host_brokered_callbacks.retain(host_brokered_opaque, object);
    if (stream) {
        host_brokered_callbacks.retain(host_brokered_opaque, stream);
    }
    mount = g_new0(QemuHostBrokeredMount, 1);
    mount->path = g_strdup(virtual_path);
    mount->type = type;
    mount->object = object;
    mount->stream = stream;
    g_ptr_array_add(host_brokered_mounts, mount);
    g_mutex_unlock(&host_callback_lock);
    g_mutex_unlock(&host_state_lock);
    return 0;
}

int qemu_host_mount_brokered_file(const char *virtual_path,
                                  void *storage_file,
                                  void *random_access_stream)
{
    return host_mount_brokered(virtual_path, storage_file,
                               random_access_stream,
                               QEMU_HOST_BROKERED_FILE);
}

int qemu_host_mount_brokered_folder(const char *virtual_path,
                                    void *storage_folder)
{
    return host_mount_brokered(virtual_path, storage_folder, NULL,
                               QEMU_HOST_BROKERED_FOLDER);
}

int qemu_host_unmount_brokered_storage(const char *virtual_path)
{
    guint i;

    if (!host_brokered_path_valid(virtual_path)) {
        return -EINVAL;
    }
    g_mutex_lock(&host_state_lock);
    if (host_initializing || host_initialized) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    g_mutex_lock(&host_callback_lock);
    if (host_brokered_mounts) {
        for (i = 0; i < host_brokered_mounts->len; i++) {
            QemuHostBrokeredMount *mount =
                g_ptr_array_index(host_brokered_mounts, i);

            if (!strcmp(mount->path, virtual_path)) {
                g_ptr_array_remove_index(host_brokered_mounts, i);
                g_mutex_unlock(&host_callback_lock);
                g_mutex_unlock(&host_state_lock);
                return 0;
            }
        }
    }
    g_mutex_unlock(&host_callback_lock);
    g_mutex_unlock(&host_state_lock);
    return -ENOENT;
}

void qemu_host_register_dialog_callback(QemuHostDialogRequestCallback cb,
                                        void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    host_dialog_callback = cb;
    host_dialog_opaque = opaque;
    g_queue_clear_full(&host_dialog_responses,
                       (GDestroyNotify)host_dialog_response_free);
    if (host_dialog_requests) {
        g_hash_table_remove_all(host_dialog_requests);
    }
    host_next_dialog_id = 1;
    g_mutex_unlock(&host_callback_lock);
}

int qemu_host_complete_dialog(uint64_t request_id, int status,
                              const void *payload, size_t payload_size)
{
    QemuHostDialogResponse *response;

    if (!request_id || (!payload && payload_size) || payload_size > MiB) {
        return -EINVAL;
    }
    g_mutex_lock(&host_callback_lock);
    host_dialog_requests_ensure();
    if (!g_hash_table_contains(host_dialog_requests, &request_id)) {
        g_mutex_unlock(&host_callback_lock);
        return -ENOENT;
    }
    if (g_queue_get_length(&host_dialog_responses) >= 16) {
        g_mutex_unlock(&host_callback_lock);
        return -ENOSPC;
    }
    g_hash_table_remove(host_dialog_requests, &request_id);
    response = g_new0(QemuHostDialogResponse, 1);
    response->request_id = request_id;
    response->status = status;
    response->payload = g_bytes_new(payload, payload_size);
    g_queue_push_tail(&host_dialog_responses, response);
    g_mutex_unlock(&host_callback_lock);
    qemu_notify_event();
    return 0;
}

void qemu_host_register_network_callbacks(
    const QemuHostNetworkCallbacks *callbacks, void *opaque)
{
    size_t callbacks_size;

    g_mutex_lock(&host_callback_lock);
    memset(&host_network_callbacks, 0, sizeof(host_network_callbacks));
    host_network_opaque = NULL;
    if (callbacks && callbacks->size >=
            offsetof(QemuHostNetworkCallbacks, bind) &&
        callbacks->version == 1 && callbacks->socket && callbacks->connect &&
        callbacks->send && callbacks->recv && callbacks->close) {
        callbacks_size = MIN((size_t)callbacks->size,
                             sizeof(host_network_callbacks));
        memcpy(&host_network_callbacks, callbacks, callbacks_size);
        host_network_opaque = opaque;
    }
    g_mutex_unlock(&host_callback_lock);
}

void qemu_host_register_http_callbacks(const QemuHostHttpCallbacks *callbacks,
                                       void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    memset(&host_http_callbacks, 0, sizeof(host_http_callbacks));
    host_http_opaque = NULL;
    if (callbacks && callbacks->version == 1 &&
        callbacks->size >= offsetof(QemuHostHttpCallbacks, close) +
                          sizeof(callbacks->close) &&
        callbacks->create_request && callbacks->send &&
        callbacks->get_status && callbacks->get_headers &&
        callbacks->get_length && callbacks->read && callbacks->close) {
        memcpy(&host_http_callbacks, callbacks,
               MIN((size_t)callbacks->size, sizeof(host_http_callbacks)));
        host_http_opaque = opaque;
    }
    g_mutex_unlock(&host_callback_lock);
}

void qemu_host_register_media_callbacks(
    const QemuHostMediaCallbacks *callbacks, void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    memset(&host_media_callbacks, 0, sizeof(host_media_callbacks));
    host_media_opaque = NULL;
    if (callbacks && callbacks->version == 1 &&
        callbacks->size >= offsetof(QemuHostMediaCallbacks, get_time) +
                          sizeof(callbacks->get_time) &&
        callbacks->open && callbacks->control &&
        callbacks->get_stream_count && callbacks->get_stream_info &&
        callbacks->read_frame && callbacks->close && callbacks->get_time) {
        memcpy(&host_media_callbacks, callbacks,
               MIN((size_t)callbacks->size, sizeof(host_media_callbacks)));
        host_media_opaque = opaque;
    }
    g_mutex_unlock(&host_callback_lock);
}

void qemu_host_register_launch_callback(QemuHostLaunchCallback callback,
                                        void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    host_launch_callback = callback;
    host_launch_opaque = callback ? opaque : NULL;
    g_mutex_unlock(&host_callback_lock);
}

void qemu_host_register_hle_callback(QemuHostHLECallback callback,
                                     void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    host_hle_callback = callback;
    host_hle_opaque = callback ? opaque : NULL;
    g_mutex_unlock(&host_callback_lock);
}

bool qemu_host_hle_callback_enabled(void)
{
    bool enabled;

    g_mutex_lock(&host_callback_lock);
    enabled = host_hle_callback != NULL;
    g_mutex_unlock(&host_callback_lock);
    return enabled;
}

#define HOST_CALLBACK_SNAPSHOT(group, member, cb, opaque) do {             \
    g_mutex_lock(&host_callback_lock);                                      \
    (cb) = host_##group##_callbacks.member;                                 \
    (opaque) = host_##group##_opaque;                                       \
    g_mutex_unlock(&host_callback_lock);                                    \
} while (0)

int qemu_host_http_create_request(const char *method, const char *url,
                                  uint64_t content_length, int64_t *handle)
{
    QemuHostHttpCreateRequestCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(http, create_request, cb, opaque);
    return cb ? cb(opaque, method, url, content_length, handle) : -ENOSYS;
}

int qemu_host_http_add_header(int64_t handle, const char *name,
                              const char *value, int mode)
{
    QemuHostHttpAddHeaderCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(http, add_header, cb, opaque);
    return cb ? cb(opaque, handle, name, value, mode) : -ENOSYS;
}

int qemu_host_http_send(int64_t handle, const void *body, size_t body_size)
{
    QemuHostHttpSendCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(http, send, cb, opaque);
    return cb ? cb(opaque, handle, body, body_size) : -ENOSYS;
}

int qemu_host_http_get_status(int64_t handle, int *status_code)
{
    QemuHostHttpGetStatusCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(http, get_status, cb, opaque);
    return cb ? cb(opaque, handle, status_code) : -ENOSYS;
}

int qemu_host_http_get_headers(int64_t handle, char *buffer,
                               size_t *buffer_size)
{
    QemuHostHttpGetHeadersCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(http, get_headers, cb, opaque);
    return cb ? cb(opaque, handle, buffer, buffer_size) : -ENOSYS;
}

int qemu_host_http_get_length(int64_t handle, uint64_t *content_length)
{
    QemuHostHttpGetLengthCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(http, get_length, cb, opaque);
    return cb ? cb(opaque, handle, content_length) : -ENOSYS;
}

int64_t qemu_host_http_read(int64_t handle, void *buffer, size_t size)
{
    QemuHostHttpReadCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(http, read, cb, opaque);
    return cb ? cb(opaque, handle, buffer, size) : -ENOSYS;
}

int qemu_host_http_abort(int64_t handle)
{
    QemuHostHttpRequestCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(http, abort, cb, opaque);
    return cb ? cb(opaque, handle) : -ENOSYS;
}

int qemu_host_http_close(int64_t handle)
{
    QemuHostHttpRequestCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(http, close, cb, opaque);
    return cb ? cb(opaque, handle) : -ENOSYS;
}

int qemu_host_media_open(const char *source, int64_t *handle)
{
    QemuHostMediaOpenCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(media, open, cb, opaque);
    return cb ? cb(opaque, source, handle) : -ENOSYS;
}

int qemu_host_media_control(int64_t handle, QemuHostMediaControl control,
                            uint64_t value)
{
    QemuHostMediaControlCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(media, control, cb, opaque);
    return cb ? cb(opaque, handle, control, value) : -ENOSYS;
}

int qemu_host_media_get_stream_count(int64_t handle, uint32_t *count)
{
    QemuHostMediaStreamCountCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(media, get_stream_count, cb, opaque);
    return cb ? cb(opaque, handle, count) : -ENOSYS;
}

int qemu_host_media_get_stream_info(int64_t handle, uint32_t index,
                                    QemuHostMediaStreamInfo *info)
{
    QemuHostMediaStreamInfoCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(media, get_stream_info, cb, opaque);
    return cb ? cb(opaque, handle, index, info) : -ENOSYS;
}

int qemu_host_media_read_frame(int64_t handle, uint32_t stream_type,
                               void *buffer, size_t *buffer_size,
                               QemuHostMediaFrameInfo *info)
{
    QemuHostMediaReadFrameCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(media, read_frame, cb, opaque);
    return cb ? cb(opaque, handle, stream_type, buffer, buffer_size, info) :
                -ENOSYS;
}

int qemu_host_media_close(int64_t handle)
{
    QemuHostMediaCloseCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(media, close, cb, opaque);
    return cb ? cb(opaque, handle) : -ENOSYS;
}

int qemu_host_media_get_time(int64_t handle, uint64_t *time_us)
{
    QemuHostMediaGetTimeCallback cb;
    void *opaque;

    HOST_CALLBACK_SNAPSHOT(media, get_time, cb, opaque);
    return cb ? cb(opaque, handle, time_us) : -ENOSYS;
}

int qemu_host_launch(const char *path, const char *const *argv, size_t argc)
{
    QemuHostLaunchCallback cb;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_launch_callback;
    opaque = host_launch_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, path, argv, argc) : -ENOSYS;
}

int qemu_host_hle_invoke(const QemuHostHLERequest *request, uint64_t *result)
{
    QemuHostHLECallback cb;
    void *opaque;

    if (!request || request->size < sizeof(*request) ||
        request->version != 1 || !request->module || !request->library ||
        !request->nid || !request->read_guest || !request->write_guest ||
        !result) {
        return -EINVAL;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_hle_callback;
    opaque = host_hle_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, request, result) : -ENOSYS;
}

#undef HOST_CALLBACK_SNAPSHOT

void qemu_host_register_input_sink(const QemuHostInputSink *sink, void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    if (sink) {
        host_input_sink = *sink;
        host_input_sink_opaque = opaque;
    } else {
        memset(&host_input_sink, 0, sizeof(host_input_sink));
        host_input_sink_opaque = NULL;
    }
    g_mutex_unlock(&host_callback_lock);
}

int qemu_host_storage_open(const char *path, int flags, int mode,
                           int64_t *handle)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostBrokeredMount *mount;
    QemuHostStorageOpenCallback cb;
    g_autofree char *relative = NULL;
    void *object = NULL;
    void *stream = NULL;
    void *opaque;
    QemuHostBrokeredMountType mount_type = QEMU_HOST_BROKERED_FILE;
    int64_t backend_handle;
    int ret;

    if (!path || !handle) {
        return -EINVAL;
    }

    g_mutex_lock(&host_callback_lock);
    mount = host_brokered_find_mount_locked(path, NULL);
    if (mount) {
        const char *relative_path;

        host_brokered_find_mount_locked(path, &relative_path);
        relative = g_strdup(relative_path);
        object = mount->object;
        stream = mount->stream;
        mount_type = mount->type;
        brokered = host_brokered_callbacks;
        opaque = host_brokered_opaque;
        g_mutex_unlock(&host_callback_lock);
        ret = mount_type == QEMU_HOST_BROKERED_FILE ?
              brokered.open_file(opaque, object, stream, flags,
                                   &backend_handle) :
              brokered.open_at(opaque, object, relative, flags, mode,
                                &backend_handle);
        if (ret < 0) {
            host_brokered_report_error("open", path, relative, ret);
            return ret;
        }
        *handle = host_brokered_publish_handle(backend_handle, path, relative);
        return 0;
    }
    cb = host_storage_callbacks.open;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, path, flags, mode, handle) : -ENOSYS;
}

static bool host_brokered_handle_snapshot(
    int64_t handle, int64_t *backend_handle,
    QemuHostBrokeredStorageCallbacks *callbacks, void **opaque,
    char **virtual_path, char **relative_path)
{
    QemuHostBrokeredHandle *brokered_handle;

    g_mutex_lock(&host_callback_lock);
    brokered_handle = host_brokered_find_handle_locked(handle);
    if (brokered_handle) {
        *backend_handle = brokered_handle->backend_handle;
        *callbacks = host_brokered_callbacks;
        *opaque = host_brokered_opaque;
        if (virtual_path) {
            *virtual_path = g_strdup(brokered_handle->virtual_path);
        }
        if (relative_path) {
            *relative_path = g_strdup(brokered_handle->relative_path);
        }
    }
    g_mutex_unlock(&host_callback_lock);
    return brokered_handle != NULL;
}

int64_t qemu_host_storage_read(int64_t handle, void *buffer, size_t size)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageReadCallback cb;
    int64_t backend_handle;
    void *opaque;
    g_autofree char *virtual_path = NULL;
    g_autofree char *relative_path = NULL;
    int64_t ret;

    if (host_brokered_handle_snapshot(handle, &backend_handle,
                                      &brokered, &opaque, &virtual_path,
                                      &relative_path)) {
        ret = brokered.read(opaque, backend_handle, buffer, size);
        if (ret < 0) {
            host_brokered_report_error("read", virtual_path, relative_path,
                                       ret);
        }
        return ret;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_storage_callbacks.read;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, handle, buffer, size) : -ENOSYS;
}

int64_t qemu_host_storage_write(int64_t handle, const void *buffer,
                                 size_t size)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageWriteCallback cb;
    int64_t backend_handle;
    void *opaque;

    if (host_brokered_handle_snapshot(handle, &backend_handle,
                                      &brokered, &opaque, NULL, NULL)) {
        return brokered.write ?
               brokered.write(opaque, backend_handle, buffer, size) :
               -EROFS;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_storage_callbacks.write;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, handle, buffer, size) : -ENOSYS;
}

int qemu_host_storage_close(int64_t handle)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageCloseCallback cb;
    int64_t backend_handle;
    void *opaque;
    int ret;

    if (host_brokered_handle_snapshot(handle, &backend_handle,
                                      &brokered, &opaque, NULL, NULL)) {
        ret = brokered.close(opaque, backend_handle);
        if (ret >= 0) {
            g_mutex_lock(&host_callback_lock);
            g_hash_table_remove(host_brokered_handles, &handle);
            g_mutex_unlock(&host_callback_lock);
        }
        return ret;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_storage_callbacks.close;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, handle) : -ENOSYS;
}

int64_t qemu_host_storage_seek(int64_t handle, int64_t offset, int whence)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageSeekCallback cb;
    int64_t backend_handle;
    void *opaque;
    g_autofree char *virtual_path = NULL;
    g_autofree char *relative_path = NULL;
    int64_t ret;

    if (host_brokered_handle_snapshot(handle, &backend_handle,
                                      &brokered, &opaque, &virtual_path,
                                      &relative_path)) {
        ret = brokered.seek(opaque, backend_handle, offset, whence);
        if (ret < 0) {
            host_brokered_report_error("seek", virtual_path, relative_path,
                                       ret);
        }
        return ret;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_storage_callbacks.seek;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, handle, offset, whence) : -ENOSYS;
}

int qemu_host_storage_stat(const char *path, QemuHostStorageStat *stat)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostBrokeredMount *mount;
    QemuHostStorageStatCallback cb;
    g_autofree char *relative = NULL;
    void *object;
    void *stream;
    void *opaque;
    int ret;
    QemuHostBrokeredMountType mount_type;

    g_mutex_lock(&host_callback_lock);
    mount = path ? host_brokered_find_mount_locked(path, NULL) : NULL;
    if (mount) {
        const char *relative_path;

        host_brokered_find_mount_locked(path, &relative_path);
        relative = g_strdup(relative_path);
        object = mount->object;
        stream = mount->stream;
        mount_type = mount->type;
        brokered = host_brokered_callbacks;
        opaque = host_brokered_opaque;
        g_mutex_unlock(&host_callback_lock);
        if (mount_type == QEMU_HOST_BROKERED_FILE) {
            ret = brokered.stat_file ?
                  brokered.stat_file(opaque, object, stream, stat) : -ENOSYS;
        } else {
            ret = brokered.stat_at ?
                  brokered.stat_at(opaque, object, relative, stat) : -ENOSYS;
        }
        if (ret < 0) {
            host_brokered_report_error("stat", path, relative, ret);
        }
        return ret;
    }
    cb = host_storage_callbacks.stat;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, path, stat) : -ENOSYS;
}

bool qemu_host_storage_path_is_brokered(const char *path)
{
    bool brokered;

    g_mutex_lock(&host_callback_lock);
    brokered = path && host_brokered_find_mount_locked(path, NULL);
    g_mutex_unlock(&host_callback_lock);
    return brokered;
}

static int host_brokered_folder_snapshot(
    const char *path, void **folder, char **relative,
    QemuHostBrokeredStorageCallbacks *callbacks, void **opaque)
{
    QemuHostBrokeredMount *mount;
    const char *relative_path;

    g_mutex_lock(&host_callback_lock);
    mount = path ? host_brokered_find_mount_locked(path, &relative_path) : NULL;
    if (!mount) {
        g_mutex_unlock(&host_callback_lock);
        return -ENOENT;
    }
    if (mount->type != QEMU_HOST_BROKERED_FOLDER) {
        g_mutex_unlock(&host_callback_lock);
        return -ENOTDIR;
    }
    *folder = mount->object;
    *relative = g_strdup(relative_path);
    *callbacks = host_brokered_callbacks;
    *opaque = host_brokered_opaque;
    g_mutex_unlock(&host_callback_lock);
    return 0;
}

int qemu_host_storage_mkdir(const char *path, int mode)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageMkdirCallback cb;
    g_autofree char *relative = NULL;
    void *folder;
    void *opaque;
    int ret;

    ret = host_brokered_folder_snapshot(path, &folder, &relative,
                                        &brokered, &opaque);
    if (ret != -ENOENT) {
        if (ret == 0 && relative && !relative[0]) {
            return -EEXIST;
        }
        return ret < 0 ? ret : brokered.mkdir_at ?
               brokered.mkdir_at(opaque, folder, relative, mode) : -ENOSYS;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_storage_callbacks.mkdir;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, path, mode) : -ENOSYS;
}

int qemu_host_storage_unlink(const char *path)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStoragePathCallback cb;
    g_autofree char *relative = NULL;
    void *folder;
    void *opaque;
    int ret;

    ret = host_brokered_folder_snapshot(path, &folder, &relative,
                                        &brokered, &opaque);
    if (ret != -ENOENT) {
        return ret < 0 ? ret : brokered.unlink_at ?
               brokered.unlink_at(opaque, folder, relative) : -ENOSYS;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_storage_callbacks.unlink;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, path) : -ENOSYS;
}

int qemu_host_storage_rename(const char *old_path, const char *new_path)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageRenameCallback cb;
    g_autofree char *old_relative = NULL;
    g_autofree char *new_relative = NULL;
    QemuHostBrokeredStorageCallbacks new_brokered;
    void *old_folder;
    void *new_folder;
    void *opaque;
    void *new_opaque;
    int old_ret;
    int new_ret;

    old_ret = host_brokered_folder_snapshot(old_path, &old_folder,
                                            &old_relative, &brokered, &opaque);
    new_ret = host_brokered_folder_snapshot(new_path, &new_folder,
                                            &new_relative, &new_brokered,
                                            &new_opaque);
    if (old_ret != -ENOENT || new_ret != -ENOENT) {
        if (old_ret < 0 || new_ret < 0) {
            return -EXDEV;
        }
        if (old_folder != new_folder || opaque != new_opaque) {
            return -EXDEV;
        }
        return brokered.rename_at ?
               brokered.rename_at(opaque, old_folder, old_relative,
                                   new_relative) : -ENOSYS;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_storage_callbacks.rename;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, old_path, new_path) : -ENOSYS;
}

int qemu_host_storage_flush(int64_t handle)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageFlushCallback cb;
    int64_t backend_handle;
    void *opaque;

    if (host_brokered_handle_snapshot(handle, &backend_handle,
                                      &brokered, &opaque, NULL, NULL)) {
        return brokered.flush ? brokered.flush(opaque, backend_handle) : 0;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_storage_callbacks.flush;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, handle) : -ENOSYS;
}

int qemu_host_storage_readdir(int64_t handle, char *name, size_t name_size,
                              QemuHostStorageStat *stat)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageReadDirCallback cb;
    int64_t backend_handle;
    void *opaque;
    g_autofree char *virtual_path = NULL;
    g_autofree char *relative_path = NULL;
    int ret;

    if (host_brokered_handle_snapshot(handle, &backend_handle,
                                      &brokered, &opaque, &virtual_path,
                                      &relative_path)) {
        ret = brokered.readdir(opaque, backend_handle, name, name_size, stat);
        if (ret < 0) {
            host_brokered_report_error("readdir", virtual_path,
                                       relative_path, ret);
        }
        return ret;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_storage_callbacks.readdir;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, handle, name, name_size, stat) : -ENOSYS;
}

int qemu_host_storage_atomic_replace(const char *path, const void *data,
                                     size_t size)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageAtomicReplaceCallback cb;
    g_autofree char *relative = NULL;
    void *folder;
    void *opaque;
    int ret;

    ret = host_brokered_folder_snapshot(path, &folder, &relative,
                                        &brokered, &opaque);
    if (ret != -ENOENT) {
        return ret < 0 ? ret : brokered.atomic_replace_at ?
               brokered.atomic_replace_at(opaque, folder, relative, data,
                                           size) : -ENOSYS;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_storage_callbacks.atomic_replace;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, path, data, size) : -ENOSYS;
}

int qemu_host_storage_cleanup(const char *path)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStoragePathCallback cb;
    g_autofree char *relative = NULL;
    void *folder;
    void *opaque;
    int ret;

    ret = host_brokered_folder_snapshot(path, &folder, &relative,
                                        &brokered, &opaque);
    if (ret != -ENOENT) {
        return ret < 0 ? ret : brokered.cleanup_at ?
               brokered.cleanup_at(opaque, folder, relative) : -ENOSYS;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_storage_callbacks.cleanup;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, path) : -ENOSYS;
}

int qemu_host_storage_truncate(int64_t handle, uint64_t size)
{
    QemuHostBrokeredStorageCallbacks brokered;
    QemuHostStorageTruncateCallback cb;
    int64_t backend_handle;
    void *opaque;

    if (host_brokered_handle_snapshot(handle, &backend_handle,
                                      &brokered, &opaque, NULL, NULL)) {
        return brokered.truncate ?
               brokered.truncate(opaque, backend_handle, size) : -ENOSYS;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_storage_callbacks.truncate;
    opaque = host_storage_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, handle, size) : -ENOSYS;
}

int qemu_host_dialog_request(uint32_t type, const void *payload, size_t size,
                             uint64_t *request_id)
{
    QemuHostDialogRequestCallback cb;
    void *opaque;
    uint64_t *active_id;
    uint64_t id;
    int ret;

    if ((!payload && size) || size > MiB || !request_id) {
        return -EINVAL;
    }
    g_mutex_lock(&host_callback_lock);
    cb = host_dialog_callback;
    opaque = host_dialog_opaque;
    if (!cb) {
        g_mutex_unlock(&host_callback_lock);
        return -ENOSYS;
    }
    host_dialog_requests_ensure();
    id = host_next_dialog_id++;
    if (!host_next_dialog_id) {
        host_next_dialog_id = 1;
    }
    active_id = g_new(uint64_t, 1);
    *active_id = id;
    g_hash_table_add(host_dialog_requests, active_id);
    g_mutex_unlock(&host_callback_lock);
    *request_id = id;
    ret = cb(opaque, id, type, payload, size);
    if (ret < 0) {
        g_mutex_lock(&host_callback_lock);
        g_hash_table_remove(host_dialog_requests, &id);
        g_mutex_unlock(&host_callback_lock);
    }
    return ret;
}

int qemu_host_dialog_take_response(uint64_t *request_id, int *status,
                                   void *payload, size_t *payload_size)
{
    QemuHostDialogResponse *response;
    gconstpointer data;
    gsize size;

    if (!request_id || !status || !payload_size) {
        return -EINVAL;
    }
    g_mutex_lock(&host_callback_lock);
    response = g_queue_peek_head(&host_dialog_responses);
    if (!response) {
        g_mutex_unlock(&host_callback_lock);
        return -EAGAIN;
    }
    data = g_bytes_get_data(response->payload, &size);
    if (*payload_size < size || (!payload && size)) {
        *payload_size = size;
        g_mutex_unlock(&host_callback_lock);
        return -ENOSPC;
    }
    *request_id = response->request_id;
    *status = response->status;
    *payload_size = size;
    if (size) {
        memcpy(payload, data, size);
    }
    g_queue_pop_head(&host_dialog_responses);
    g_mutex_unlock(&host_callback_lock);
    host_dialog_response_free(response);
    return 0;
}

int qemu_host_network_socket(int domain, int type, int protocol,
                             int64_t *handle)
{
    QemuHostNetworkSocketCallback cb;
    uint32_t capabilities;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_network_callbacks.socket;
    capabilities = host_network_callbacks.capabilities;
    opaque = host_network_opaque;
    g_mutex_unlock(&host_callback_lock);
    if (!cb || !(capabilities & QEMU_HOST_NETWORK_INTERNET_CLIENT)) {
        return -EACCES;
    }
    return cb(opaque, domain, type, protocol, handle);
}

static bool host_network_address_is_private(const char *address)
{
    unsigned a;
    unsigned b;

    if (sscanf(address, "%u.%u", &a, &b) != 2) {
        return false;
    }
    return a == 10 || a == 127 || (a == 192 && b == 168) ||
           (a == 172 && b >= 16 && b <= 31) || (a == 169 && b == 254);
}

int qemu_host_network_connect(int64_t handle, const char *address,
                              uint16_t port)
{
    QemuHostNetworkConnectCallback cb;
    uint32_t capabilities;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_network_callbacks.connect;
    capabilities = host_network_callbacks.capabilities;
    opaque = host_network_opaque;
    g_mutex_unlock(&host_callback_lock);
    if (!cb) {
        return -ENOSYS;
    }
    if (host_network_address_is_private(address) &&
        !(capabilities & QEMU_HOST_NETWORK_PRIVATE_CLIENT)) {
        return -EACCES;
    }
    return cb(opaque, handle, address, port);
}

int64_t qemu_host_network_send(int64_t handle, const void *buffer, size_t size,
                               int flags)
{
    QemuHostNetworkSendCallback cb;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_network_callbacks.send;
    opaque = host_network_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, handle, buffer, size, flags) : -ENOSYS;
}

int64_t qemu_host_network_recv(int64_t handle, void *buffer, size_t size,
                               int flags)
{
    QemuHostNetworkRecvCallback cb;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_network_callbacks.recv;
    opaque = host_network_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, handle, buffer, size, flags) : -ENOSYS;
}

int qemu_host_network_close(int64_t handle)
{
    QemuHostNetworkCloseCallback cb;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_network_callbacks.close;
    opaque = host_network_opaque;
    g_mutex_unlock(&host_callback_lock);
    return cb ? cb(opaque, handle) : -ENOSYS;
}

#define HOST_NETWORK_CALL(field, args)                                      \
    do {                                                                    \
        typeof(host_network_callbacks.field) cb;                            \
        void *opaque;                                                       \
        g_mutex_lock(&host_callback_lock);                                  \
        cb = host_network_callbacks.field;                                  \
        opaque = host_network_opaque;                                       \
        g_mutex_unlock(&host_callback_lock);                                \
        return cb ? cb args : -ENOSYS;                                      \
    } while (0)

int qemu_host_network_bind(int64_t handle, const void *address,
                           size_t address_size)
{
    HOST_NETWORK_CALL(bind, (opaque, handle, address, address_size));
}

int qemu_host_network_listen(int64_t handle, int backlog)
{
    HOST_NETWORK_CALL(listen, (opaque, handle, backlog));
}

int qemu_host_network_accept(int64_t handle, void *address,
                             size_t *address_size, int64_t *accepted_handle)
{
    HOST_NETWORK_CALL(accept, (opaque, handle, address, address_size,
                               accepted_handle));
}

int qemu_host_network_shutdown(int64_t handle, int how)
{
    HOST_NETWORK_CALL(shutdown, (opaque, handle, how));
}

int64_t qemu_host_network_send_to(int64_t handle, const void *buffer,
                                  size_t size, int flags,
                                  const void *address, size_t address_size)
{
    HOST_NETWORK_CALL(send_to, (opaque, handle, buffer, size, flags,
                                address, address_size));
}

int64_t qemu_host_network_recv_from(int64_t handle, void *buffer, size_t size,
                                    int flags, void *address,
                                    size_t *address_size)
{
    HOST_NETWORK_CALL(recv_from, (opaque, handle, buffer, size, flags,
                                  address, address_size));
}

int qemu_host_network_get_name(int64_t handle, bool peer, void *address,
                               size_t *address_size)
{
    HOST_NETWORK_CALL(get_name, (opaque, handle, peer, address, address_size));
}

int qemu_host_network_get_option(int64_t handle, int level, int option,
                                 void *value, size_t *value_size)
{
    HOST_NETWORK_CALL(get_option, (opaque, handle, level, option, value,
                                   value_size));
}

int qemu_host_network_set_option(int64_t handle, int level, int option,
                                 const void *value, size_t value_size)
{
    HOST_NETWORK_CALL(set_option, (opaque, handle, level, option, value,
                                   value_size));
}

int qemu_host_network_socket_pair(int domain, int type, int protocol,
                                  int64_t handles[2])
{
    HOST_NETWORK_CALL(socket_pair, (opaque, domain, type, protocol, handles));
}

int qemu_host_network_resolve(const char *hostname, int family,
                              void *address, size_t *address_size)
{
    HOST_NETWORK_CALL(resolve, (opaque, hostname, family, address,
                                address_size));
}

#undef HOST_NETWORK_CALL

static bool host_is_ready(void)
{
    bool ready;

    g_mutex_lock(&host_state_lock);
    ready = host_initialized && !host_cleaned;
    g_mutex_unlock(&host_state_lock);
    return ready;
}

static int host_capture_pointer_abs_locked(void)
{
    MouseInfoList *mice;
    MouseInfoList *mouse;
    int ret = -ENODEV;

    mice = qmp_query_mice(NULL);
    for (mouse = mice; mouse; mouse = mouse->next) {
        if (!mouse->value->absolute) {
            continue;
        }
        ret = qemu_mouse_set(mouse->value->index, NULL) ? 0 : -EINVAL;
        break;
    }
    qapi_free_MouseInfoList(mice);
    return ret;
}

static void host_set_running(bool running)
{
    g_mutex_lock(&host_state_lock);
    host_running = running;
    if (!running) {
        host_loop_exited = true;
    }
    g_mutex_unlock(&host_state_lock);
}

static void *qemu_host_main_loop_thread(void *opaque)
{
    int status;

    replay_mutex_lock();
    bql_lock();
    status = qemu_main_loop();
    bql_unlock();
    replay_mutex_unlock();

    g_mutex_lock(&host_state_lock);
    host_exit_status = status;
    g_mutex_unlock(&host_state_lock);

    host_set_running(false);
    return NULL;
}

int qemu_host_init(int argc, char **argv)
{
    int i;

    if (argc <= 0 || !argv) {
        return -EINVAL;
    }
    for (i = 0; i < argc; i++) {
        if (!argv[i]) {
            return -EINVAL;
        }
    }
    g_mutex_lock(&host_state_lock);
    if (host_initializing || host_initialized) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    host_initializing = true;
    host_last_fault_valid = false;
    memset(&host_last_fault, 0, sizeof(host_last_fault));
    g_mutex_unlock(&host_state_lock);

    qemu_init(argc, argv);

    /*
     * qemu_init() returns with BQL and replay mutex held, matching the normal
     * executable path. The embedding API gives ownership to start()/step().
     */
    bql_unlock();
    replay_mutex_unlock();

    g_mutex_lock(&host_state_lock);
    host_initializing = false;
    host_initialized = true;
    host_running = false;
    host_loop_exited = false;
    host_thread_started = false;
    host_join_active = false;
    host_step_active = false;
    host_cleaned = false;
    host_exit_status = 0;
    g_mutex_unlock(&host_state_lock);

    return 0;
}

int qemu_host_start(void)
{
    g_mutex_lock(&host_state_lock);
    if (!host_initialized || host_cleaned) {
        g_mutex_unlock(&host_state_lock);
        return -EINVAL;
    }
    if (host_running || host_thread_started || host_step_active ||
        host_loop_exited) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    host_running = true;
    host_thread_started = true;
    g_mutex_unlock(&host_state_lock);

    qemu_thread_create(&host_loop_thread, "qemu_host_main_loop",
                       qemu_host_main_loop_thread, NULL,
                       QEMU_THREAD_JOINABLE);
    return 0;
}

int qemu_host_main_loop_step(bool nonblocking, int *exit_status)
{
    int status = 0;
    bool exited;

    g_mutex_lock(&host_state_lock);
    if (!host_initialized || host_cleaned || host_thread_started) {
        g_mutex_unlock(&host_state_lock);
        return -EINVAL;
    }
    if (host_loop_exited) {
        if (exit_status) {
            *exit_status = host_exit_status;
        }
        g_mutex_unlock(&host_state_lock);
        return 1;
    }
    if (host_step_active) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    host_step_active = true;
    g_mutex_unlock(&host_state_lock);

    replay_mutex_lock();
    bql_lock();
    exited = qemu_main_loop_step(nonblocking, &status);
    bql_unlock();
    replay_mutex_unlock();

    g_mutex_lock(&host_state_lock);
    host_step_active = false;
    if (exited) {
        host_exit_status = status;
        host_loop_exited = true;
    }
    g_mutex_unlock(&host_state_lock);
    if (exited) {
        if (exit_status) {
            *exit_status = status;
        }
        return 1;
    }

    if (exit_status) {
        *exit_status = 0;
    }
    return 0;
}

int qemu_host_request_shutdown(void)
{
    if (!host_is_ready()) {
        return -EINVAL;
    }

    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
    qemu_notify_event();
    return 0;
}

int qemu_host_reset(void)
{
    if (!host_is_ready()) {
        return -EINVAL;
    }

    qemu_system_reset_request(SHUTDOWN_CAUSE_HOST_UI);
    qemu_notify_event();
    return 0;
}

int qemu_host_join(int *exit_status)
{
    g_mutex_lock(&host_state_lock);
    if (host_join_active) {
        g_mutex_unlock(&host_state_lock);
        return -EBUSY;
    }
    if (!host_thread_started) {
        g_mutex_unlock(&host_state_lock);
        return -EINVAL;
    }
    host_join_active = true;
    g_mutex_unlock(&host_state_lock);

    qemu_thread_join(&host_loop_thread);

    g_mutex_lock(&host_state_lock);
    host_thread_started = false;
    host_join_active = false;
    if (exit_status) {
        *exit_status = host_exit_status;
    }
    g_mutex_unlock(&host_state_lock);
    return 0;
}

static void host_brokered_cleanup(void)
{
    QemuHostBrokeredStorageCallbacks callbacks;
    g_autoptr(GArray) handles = g_array_new(false, false, sizeof(int64_t));
    GPtrArray *mounts;
    GHashTableIter iter;
    gpointer value;
    void *opaque;
    guint i;

    g_mutex_lock(&host_callback_lock);
    callbacks = host_brokered_callbacks;
    opaque = host_brokered_opaque;
    if (host_brokered_handles) {
        g_hash_table_iter_init(&iter, host_brokered_handles);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            QemuHostBrokeredHandle *handle = value;

            g_array_append_val(handles, handle->backend_handle);
        }
        g_clear_pointer(&host_brokered_handles, g_hash_table_unref);
    }
    mounts = host_brokered_mounts;
    host_brokered_mounts = NULL;
    g_mutex_unlock(&host_callback_lock);

    if (callbacks.close) {
        for (i = 0; i < handles->len; i++) {
            callbacks.close(opaque, g_array_index(handles, int64_t, i));
        }
    }
    if (mounts) {
        g_ptr_array_unref(mounts);
    }

    g_mutex_lock(&host_callback_lock);
    host_brokered_callbacks = (QemuHostBrokeredStorageCallbacks) { 0 };
    host_brokered_opaque = NULL;
    host_next_brokered_handle = UINT64_C(0x4000000000000000);
    g_mutex_unlock(&host_callback_lock);
}

int qemu_host_cleanup(void)
{
    int status;

    g_mutex_lock(&host_state_lock);
    if (!host_initialized || host_cleaned || host_running ||
        host_thread_started || host_join_active || host_step_active) {
        g_mutex_unlock(&host_state_lock);
        return -EINVAL;
    }
    status = host_exit_status;
    host_cleaned = true;
    g_mutex_unlock(&host_state_lock);

    replay_mutex_lock();
    bql_lock();
    qemu_cleanup(status);
    bql_unlock();
    replay_mutex_unlock();

    host_brokered_cleanup();

    g_mutex_lock(&host_callback_lock);
    host_input_sink = (QemuHostInputSink) { 0 };
    host_input_sink_opaque = NULL;
    host_log_callback = NULL;
    host_log_opaque = NULL;
    host_video_callback = NULL;
    host_video_opaque = NULL;
    host_audio_callback = NULL;
    host_audio_opaque = NULL;
    host_pad_output_callback = NULL;
    host_pad_output_opaque = NULL;
    host_storage_callbacks = (QemuHostStorageCallbacks) { 0 };
    host_storage_opaque = NULL;
    host_dialog_callback = NULL;
    host_dialog_opaque = NULL;
    host_network_callbacks = (QemuHostNetworkCallbacks) { 0 };
    host_network_opaque = NULL;
    host_http_callbacks = (QemuHostHttpCallbacks) { 0 };
    host_http_opaque = NULL;
    host_media_callbacks = (QemuHostMediaCallbacks) { 0 };
    host_media_opaque = NULL;
    host_launch_callback = NULL;
    host_launch_opaque = NULL;
    host_hle_callback = NULL;
    host_hle_opaque = NULL;
    g_queue_clear_full(&host_dialog_responses,
                       (GDestroyNotify)host_dialog_response_free);
    g_clear_pointer(&host_dialog_requests, g_hash_table_unref);
    g_mutex_unlock(&host_callback_lock);

    return 0;
}

bool qemu_host_is_initialized(void)
{
    bool initialized;

    g_mutex_lock(&host_state_lock);
    initialized = host_initialized && !host_cleaned;
    g_mutex_unlock(&host_state_lock);
    return initialized;
}

bool qemu_host_is_running(void)
{
    bool running;

    g_mutex_lock(&host_state_lock);
    running = host_running || host_step_active;
    g_mutex_unlock(&host_state_lock);
    return running;
}

int qemu_host_get_exit_status(void)
{
    int status;

    g_mutex_lock(&host_state_lock);
    status = host_exit_status;
    g_mutex_unlock(&host_state_lock);
    return status;
}

int qemu_host_get_last_fault(QemuHostFaultInfo *info)
{
    uint32_t caller_size;
    size_t copy_size;

    if (!info || info->version != QEMU_HOST_FAULT_INFO_VERSION ||
        info->size < offsetof(QemuHostFaultInfo, last_hle_result)) {
        return -EINVAL;
    }
    caller_size = info->size;
    copy_size = MIN((size_t)caller_size, sizeof(*info));
    g_mutex_lock(&host_state_lock);
    if (!host_last_fault_valid) {
        g_mutex_unlock(&host_state_lock);
        return -ENOENT;
    }
    memcpy(info, &host_last_fault, copy_size);
    info->size = caller_size;
    g_mutex_unlock(&host_state_lock);
    return 0;
}

void qemu_host_record_last_fault(const QemuHostFaultInfo *info)
{
    if (!info) {
        return;
    }
    g_mutex_lock(&host_state_lock);
    host_last_fault = *info;
    host_last_fault.size = sizeof(host_last_fault);
    host_last_fault.version = QEMU_HOST_FAULT_INFO_VERSION;
    host_last_fault_valid = true;
    g_mutex_unlock(&host_state_lock);
}

int qemu_host_send_key_number(int key_number, bool down)
{
    if (!host_is_ready() || key_number < 0 ||
        !qemu_input_key_number_to_qcode(key_number)) {
        return -EINVAL;
    }

    replay_mutex_lock();
    bql_lock();
    qemu_input_event_send_key_number(NULL, key_number, down);
    bql_unlock();
    replay_mutex_unlock();
    qemu_notify_event();
    return 0;
}

int qemu_host_send_key_qcode(int qcode, bool down)
{
    if (!host_is_ready() || qcode <= Q_KEY_CODE_UNMAPPED ||
        qcode >= Q_KEY_CODE__MAX) {
        return -EINVAL;
    }

    replay_mutex_lock();
    bql_lock();
    qemu_input_event_send_key_qcode(NULL, (QKeyCode)qcode, down);
    bql_unlock();
    replay_mutex_unlock();
    qemu_notify_event();
    return 0;
}

int qemu_host_send_pointer_rel(int dx, int dy)
{
    if (!host_is_ready()) {
        return -EINVAL;
    }

    replay_mutex_lock();
    bql_lock();
    if (dx) {
        qemu_input_queue_rel(NULL, INPUT_AXIS_X, dx);
    }
    if (dy) {
        qemu_input_queue_rel(NULL, INPUT_AXIS_Y, dy);
    }
    qemu_input_event_sync();
    bql_unlock();
    replay_mutex_unlock();
    qemu_notify_event();
    return 0;
}

int qemu_host_send_pointer_abs(int x, int y, int width, int height)
{
    int ret;

    if (!host_is_ready() || width <= 0 || height <= 0) {
        return -EINVAL;
    }

    x = MIN(MAX(x, 0), width);
    y = MIN(MAX(y, 0), height);

    replay_mutex_lock();
    bql_lock();
    ret = qemu_input_is_absolute(NULL) ? 0 :
          host_capture_pointer_abs_locked();
    if (ret < 0) {
        bql_unlock();
        replay_mutex_unlock();
        return ret;
    }
    qemu_input_queue_abs(NULL, INPUT_AXIS_X, x, 0, width);
    qemu_input_queue_abs(NULL, INPUT_AXIS_Y, y, 0, height);
    qemu_input_event_sync();
    bql_unlock();
    replay_mutex_unlock();
    qemu_notify_event();
    return 0;
}

static int host_pointer_button_to_qemu(QemuHostPointerButton button,
                                       InputButton *qemu_button)
{
    switch (button) {
    case QEMU_HOST_POINTER_BUTTON_LEFT:
        *qemu_button = INPUT_BUTTON_LEFT;
        return 0;
    case QEMU_HOST_POINTER_BUTTON_MIDDLE:
        *qemu_button = INPUT_BUTTON_MIDDLE;
        return 0;
    case QEMU_HOST_POINTER_BUTTON_RIGHT:
        *qemu_button = INPUT_BUTTON_RIGHT;
        return 0;
    case QEMU_HOST_POINTER_BUTTON_WHEEL_UP:
        *qemu_button = INPUT_BUTTON_WHEEL_UP;
        return 0;
    case QEMU_HOST_POINTER_BUTTON_WHEEL_DOWN:
        *qemu_button = INPUT_BUTTON_WHEEL_DOWN;
        return 0;
    case QEMU_HOST_POINTER_BUTTON_SIDE:
        *qemu_button = INPUT_BUTTON_SIDE;
        return 0;
    case QEMU_HOST_POINTER_BUTTON_EXTRA:
        *qemu_button = INPUT_BUTTON_EXTRA;
        return 0;
    default:
        return -EINVAL;
    }
}

int qemu_host_send_pointer_button(QemuHostPointerButton button, bool down)
{
    InputButton qemu_button;
    int ret;

    if (!host_is_ready()) {
        return -EINVAL;
    }

    ret = host_pointer_button_to_qemu(button, &qemu_button);
    if (ret < 0) {
        return ret;
    }

    replay_mutex_lock();
    bql_lock();
    qemu_input_queue_btn(NULL, qemu_button, down);
    qemu_input_event_sync();
    bql_unlock();
    replay_mutex_unlock();
    qemu_notify_event();
    return 0;
}

int qemu_host_capture_pointer_abs(void)
{
    int ret;

    if (!host_is_ready()) {
        return -EINVAL;
    }

    replay_mutex_lock();
    bql_lock();
    ret = host_capture_pointer_abs_locked();
    bql_unlock();
    replay_mutex_unlock();
    qemu_notify_event();
    return ret;
}

bool qemu_host_pointer_is_absolute(void)
{
    bool absolute;

    if (!host_is_ready()) {
        return false;
    }

    replay_mutex_lock();
    bql_lock();
    absolute = qemu_input_is_absolute(NULL);
    bql_unlock();
    replay_mutex_unlock();
    return absolute;
}

int qemu_host_send_gamepad_state(int controller,
                                 const QemuHostGamepadState *state)
{
    int (*send)(void *, int, const QemuHostGamepadState *);
    void *opaque;
    int ret;

    if (!host_is_ready() || !state || controller < 0 ||
        controller >= QEMU_HOST_MAX_GAMEPADS) {
        return -EINVAL;
    }
    g_mutex_lock(&host_callback_lock);
    send = host_input_sink.gamepad;
    opaque = host_input_sink_opaque;
    g_mutex_unlock(&host_callback_lock);
    if (!send) {
        return -ENODEV;
    }
    replay_mutex_lock();
    bql_lock();
    ret = send(opaque, controller, state);
    bql_unlock();
    replay_mutex_unlock();
    qemu_notify_event();
    return ret;
}

int qemu_host_send_touch_state(int controller,
                               const QemuHostTouchState *state)
{
    int (*send)(void *, int, const QemuHostTouchState *);
    void *opaque;
    int ret;

    if (!host_is_ready() || !state || controller < 0 ||
        controller >= QEMU_HOST_MAX_GAMEPADS) {
        return -EINVAL;
    }
    g_mutex_lock(&host_callback_lock);
    send = host_input_sink.touch;
    opaque = host_input_sink_opaque;
    g_mutex_unlock(&host_callback_lock);
    if (!send) {
        return -ENODEV;
    }
    replay_mutex_lock();
    bql_lock();
    ret = send(opaque, controller, state);
    bql_unlock();
    replay_mutex_unlock();
    qemu_notify_event();
    return ret;
}

int qemu_host_send_motion_state(int controller,
                                const QemuHostMotionState *state)
{
    int (*send)(void *, int, const QemuHostMotionState *);
    void *opaque;
    int ret;

    if (!host_is_ready() || !state || controller < 0 ||
        controller >= QEMU_HOST_MAX_GAMEPADS) {
        return -EINVAL;
    }
    g_mutex_lock(&host_callback_lock);
    send = host_input_sink.motion;
    opaque = host_input_sink_opaque;
    g_mutex_unlock(&host_callback_lock);
    if (!send) {
        return -ENODEV;
    }
    replay_mutex_lock();
    bql_lock();
    ret = send(opaque, controller, state);
    bql_unlock();
    replay_mutex_unlock();
    qemu_notify_event();
    return ret;
}

int qemu_host_send_audio_input(const void *samples, size_t size,
                               int sample_rate, int channels,
                               QemuHostAudioFormat format)
{
    int (*send)(void *, const void *, size_t, int, int,
                QemuHostAudioFormat);
    void *opaque;
    int ret;
    size_t sample_size;

    if (!host_is_ready() || !samples || !size || size > MiB ||
        sample_rate < 8000 || sample_rate > 192000 || channels < 1 ||
        channels > 8 || format < QEMU_HOST_AUDIO_FORMAT_U8 ||
        format > QEMU_HOST_AUDIO_FORMAT_F32) {
        return -EINVAL;
    }
    sample_size = format == QEMU_HOST_AUDIO_FORMAT_U8 ? 1 :
                  format == QEMU_HOST_AUDIO_FORMAT_S16 ? 2 : 4;
    if (size % (sample_size * channels)) {
        return -EINVAL;
    }
    g_mutex_lock(&host_callback_lock);
    send = host_input_sink.audio_input;
    opaque = host_input_sink_opaque;
    g_mutex_unlock(&host_callback_lock);
    if (!send) {
        return -ENODEV;
    }
    replay_mutex_lock();
    bql_lock();
    ret = send(opaque, samples, size, sample_rate, channels, format);
    bql_unlock();
    replay_mutex_unlock();
    qemu_notify_event();
    return ret;
}
