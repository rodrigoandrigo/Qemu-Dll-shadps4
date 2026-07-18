/*
 * QEMU host embedding API
 *
 * Copyright (c) 2026
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef QEMU_HOST_H
#define QEMU_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined _WIN32 || defined __CYGWIN__
#ifdef QEMU_HOST_BUILD
#define QEMU_HOST_EXPORT __declspec(dllexport)
#elif defined QEMU_HOST_INTERNAL
#define QEMU_HOST_EXPORT
#else
#define QEMU_HOST_EXPORT __declspec(dllimport)
#endif
#else
#define QEMU_HOST_EXPORT __attribute__((visibility("default")))
#endif

typedef enum QemuHostLogLevel {
    QEMU_HOST_LOG_ERROR = 0,
    QEMU_HOST_LOG_WARNING = 1,
    QEMU_HOST_LOG_INFO = 2,
    QEMU_HOST_LOG_DEBUG = 3,
} QemuHostLogLevel;

#define QEMU_HOST_API_VERSION_MAJOR 1U
#define QEMU_HOST_API_VERSION_MINOR 1U
#define QEMU_HOST_API_VERSION \
    ((QEMU_HOST_API_VERSION_MAJOR << 16) | QEMU_HOST_API_VERSION_MINOR)

typedef enum QemuHostPixelFormat {
    QEMU_HOST_PIXEL_FORMAT_UNKNOWN = 0,
    QEMU_HOST_PIXEL_FORMAT_BGRA8888 = 1,
    QEMU_HOST_PIXEL_FORMAT_RGBA8888 = 2,
    QEMU_HOST_PIXEL_FORMAT_RGBX8888 = 3,
    QEMU_HOST_PIXEL_FORMAT_BGRX8888 = 4,
} QemuHostPixelFormat;

typedef enum QemuHostAudioFormat {
    QEMU_HOST_AUDIO_FORMAT_U8 = 0,
    QEMU_HOST_AUDIO_FORMAT_S16 = 1,
    QEMU_HOST_AUDIO_FORMAT_S32 = 2,
    QEMU_HOST_AUDIO_FORMAT_F32 = 3,
} QemuHostAudioFormat;

typedef enum QemuHostPointerButton {
    QEMU_HOST_POINTER_BUTTON_LEFT = 0,
    QEMU_HOST_POINTER_BUTTON_MIDDLE = 1,
    QEMU_HOST_POINTER_BUTTON_RIGHT = 2,
    QEMU_HOST_POINTER_BUTTON_WHEEL_UP = 3,
    QEMU_HOST_POINTER_BUTTON_WHEEL_DOWN = 4,
    QEMU_HOST_POINTER_BUTTON_SIDE = 5,
    QEMU_HOST_POINTER_BUTTON_EXTRA = 6,
} QemuHostPointerButton;

#define QEMU_HOST_MAX_GAMEPADS 5
#define QEMU_HOST_MAX_TOUCHES 2

typedef struct QemuHostGamepadState {
    uint32_t buttons;
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
    uint8_t left_trigger;
    uint8_t right_trigger;
    uint8_t connected;
    uint8_t reserved;
    uint64_t timestamp_ns;
} QemuHostGamepadState;

typedef struct QemuHostTouchPoint {
    uint32_t id;
    float x;
    float y;
    uint8_t active;
    uint8_t reserved[3];
} QemuHostTouchPoint;

typedef struct QemuHostTouchState {
    QemuHostTouchPoint points[QEMU_HOST_MAX_TOUCHES];
    uint64_t timestamp_ns;
} QemuHostTouchState;

typedef struct QemuHostMotionState {
    float acceleration[3];
    float angular_velocity[3];
    float orientation[4];
    uint64_t timestamp_ns;
} QemuHostMotionState;

typedef struct QemuHostPadOutput {
    uint16_t rumble_small;
    uint16_t rumble_large;
    uint8_t lightbar_red;
    uint8_t lightbar_green;
    uint8_t lightbar_blue;
    uint8_t reserved;
} QemuHostPadOutput;

typedef int (*QemuHostStorageOpenCallback)(void *opaque, const char *path,
                                           int flags, int mode,
                                           int64_t *handle);
typedef int64_t (*QemuHostStorageReadCallback)(void *opaque, int64_t handle,
                                               void *buffer, size_t size);
typedef int64_t (*QemuHostStorageWriteCallback)(void *opaque, int64_t handle,
                                                const void *buffer,
                                                size_t size);
typedef int (*QemuHostStorageCloseCallback)(void *opaque, int64_t handle);

typedef struct QemuHostStorageStat {
    uint64_t size;
    uint64_t allocated_size;
    uint64_t modified_time_ns;
    uint32_t mode;
    uint32_t type;
} QemuHostStorageStat;

#define QEMU_HOST_FAULT_INFO_VERSION 1
#define QEMU_HOST_FAULT_INSTRUCTION_MAX 16

typedef struct QemuHostFaultInfo {
    uint32_t size;
    uint32_t version;
    uint32_t vector;
    uint32_t error_code_valid;
    uint32_t instruction_size;
    uint32_t cpu_index;
    uint64_t error_code;
    uint64_t rip;
    uint64_t rsp;
    uint64_t rbp;
    uint64_t cr2;
    uint64_t cr3;
    uint64_t rflags;
    uint64_t return_address;
    uint64_t last_hle_number;
    uint64_t last_hle_args[7];
    uint64_t guest_thread_id;
    uint64_t guest_process_id;
    uint8_t instruction[QEMU_HOST_FAULT_INSTRUCTION_MAX];
    char exception_type[32];
    char image[128];
    char last_hle_nid[12];
    char last_hle_library[64];
    char last_hle_module[64];
    int64_t last_hle_result;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} QemuHostFaultInfo;

typedef int64_t (*QemuHostStorageSeekCallback)(void *opaque, int64_t handle,
                                               int64_t offset, int whence);
typedef int (*QemuHostStorageStatCallback)(void *opaque, const char *path,
                                           QemuHostStorageStat *stat);
typedef int (*QemuHostStorageMkdirCallback)(void *opaque, const char *path,
                                            int mode);
typedef int (*QemuHostStoragePathCallback)(void *opaque, const char *path);
typedef int (*QemuHostStorageRenameCallback)(void *opaque,
                                             const char *old_path,
                                             const char *new_path);
typedef int (*QemuHostStorageFlushCallback)(void *opaque, int64_t handle);
typedef int (*QemuHostStorageReadDirCallback)(
    void *opaque, int64_t handle, char *name, size_t name_size,
    QemuHostStorageStat *stat);
typedef int (*QemuHostStorageAtomicReplaceCallback)(
    void *opaque, const char *path, const void *data, size_t size);
typedef int (*QemuHostStorageTruncateCallback)(void *opaque, int64_t handle,
                                               uint64_t size);

typedef struct QemuHostStorageCallbacks {
    uint32_t size;
    uint32_t version;
    QemuHostStorageOpenCallback open;
    QemuHostStorageReadCallback read;
    QemuHostStorageWriteCallback write;
    QemuHostStorageCloseCallback close;
    QemuHostStorageSeekCallback seek;
    QemuHostStorageStatCallback stat;
    QemuHostStorageMkdirCallback mkdir;
    QemuHostStoragePathCallback unlink;
    QemuHostStorageRenameCallback rename;
    QemuHostStorageFlushCallback flush;
    QemuHostStorageReadDirCallback readdir;
    QemuHostStorageAtomicReplaceCallback atomic_replace;
    QemuHostStoragePathCallback cleanup;
    QemuHostStorageTruncateCallback truncate;
} QemuHostStorageCallbacks;

/*
 * Brokered storage objects are WinRT ABI pointers supplied by the host, such
 * as get_abi(StorageFile), get_abi(StorageFolder) and
 * get_abi(IRandomAccessStream). QEMU never interprets these pointers. The host
 * owns WinRT apartment/async handling behind this synchronous vtable.
 */
typedef void (*QemuHostBrokeredRetainCallback)(void *opaque, void *object);
typedef void (*QemuHostBrokeredReleaseCallback)(void *opaque, void *object);
typedef int (*QemuHostBrokeredOpenFileCallback)(
    void *opaque, void *storage_file, void *random_access_stream, int flags,
    int64_t *handle);
typedef int (*QemuHostBrokeredOpenAtCallback)(
    void *opaque, void *storage_folder, const char *relative_path, int flags,
    int mode, int64_t *handle);
typedef int (*QemuHostBrokeredStatAtCallback)(
    void *opaque, void *storage_folder, const char *relative_path,
    QemuHostStorageStat *stat);
typedef int (*QemuHostBrokeredStatFileCallback)(
    void *opaque, void *storage_file, void *random_access_stream,
    QemuHostStorageStat *stat);
typedef int (*QemuHostBrokeredPathAtCallback)(
    void *opaque, void *storage_folder, const char *relative_path);
typedef int (*QemuHostBrokeredMkdirAtCallback)(
    void *opaque, void *storage_folder, const char *relative_path, int mode);
typedef int (*QemuHostBrokeredRenameAtCallback)(
    void *opaque, void *storage_folder, const char *old_relative_path,
    const char *new_relative_path);
typedef int (*QemuHostBrokeredAtomicReplaceAtCallback)(
    void *opaque, void *storage_folder, const char *relative_path,
    const void *data, size_t size);

typedef struct QemuHostBrokeredStorageCallbacks {
    uint32_t size;
    uint32_t version;
    QemuHostBrokeredRetainCallback retain;
    QemuHostBrokeredReleaseCallback release;
    QemuHostBrokeredOpenFileCallback open_file;
    QemuHostBrokeredOpenAtCallback open_at;
    QemuHostStorageReadCallback read;
    QemuHostStorageWriteCallback write;
    QemuHostStorageSeekCallback seek;
    QemuHostStorageCloseCallback close;
    QemuHostBrokeredStatFileCallback stat_file;
    QemuHostBrokeredStatAtCallback stat_at;
    QemuHostBrokeredMkdirAtCallback mkdir_at;
    QemuHostBrokeredPathAtCallback unlink_at;
    QemuHostBrokeredRenameAtCallback rename_at;
    QemuHostStorageFlushCallback flush;
    QemuHostStorageReadDirCallback readdir;
    QemuHostBrokeredAtomicReplaceAtCallback atomic_replace_at;
    QemuHostBrokeredPathAtCallback cleanup_at;
    QemuHostStorageTruncateCallback truncate;
} QemuHostBrokeredStorageCallbacks;

#define QEMU_HOST_STORAGE_OPEN_DIRECTORY 0x40000000
#define QEMU_HOST_NETWORK_INTERNET_CLIENT (1U << 0)
#define QEMU_HOST_NETWORK_PRIVATE_CLIENT (1U << 1)

typedef int (*QemuHostDialogRequestCallback)(
    void *opaque, uint64_t request_id, uint32_t type,
    const void *payload, size_t payload_size);

typedef int (*QemuHostNetworkSocketCallback)(void *opaque, int domain,
                                             int type, int protocol,
                                             int64_t *handle);
typedef int (*QemuHostNetworkConnectCallback)(void *opaque, int64_t handle,
                                              const char *address,
                                              uint16_t port);
typedef int64_t (*QemuHostNetworkSendCallback)(void *opaque, int64_t handle,
                                               const void *buffer,
                                               size_t size, int flags);
typedef int64_t (*QemuHostNetworkRecvCallback)(void *opaque, int64_t handle,
                                               void *buffer, size_t size,
                                               int flags);
typedef int (*QemuHostNetworkCloseCallback)(void *opaque, int64_t handle);
typedef int (*QemuHostNetworkAddressCallback)(void *opaque, int64_t handle,
                                              const void *address,
                                              size_t address_size);
typedef int (*QemuHostNetworkListenCallback)(void *opaque, int64_t handle,
                                             int backlog);
typedef int (*QemuHostNetworkAcceptCallback)(void *opaque, int64_t handle,
                                             void *address,
                                             size_t *address_size,
                                             int64_t *accepted_handle);
typedef int (*QemuHostNetworkShutdownCallback)(void *opaque, int64_t handle,
                                               int how);
typedef int64_t (*QemuHostNetworkSendToCallback)(
    void *opaque, int64_t handle, const void *buffer, size_t size, int flags,
    const void *address, size_t address_size);
typedef int64_t (*QemuHostNetworkRecvFromCallback)(
    void *opaque, int64_t handle, void *buffer, size_t size, int flags,
    void *address, size_t *address_size);
typedef int (*QemuHostNetworkGetNameCallback)(void *opaque, int64_t handle,
                                              bool peer, void *address,
                                              size_t *address_size);
typedef int (*QemuHostNetworkGetOptionCallback)(
    void *opaque, int64_t handle, int level, int option, void *value,
    size_t *value_size);
typedef int (*QemuHostNetworkSetOptionCallback)(
    void *opaque, int64_t handle, int level, int option, const void *value,
    size_t value_size);
typedef int (*QemuHostNetworkSocketPairCallback)(
    void *opaque, int domain, int type, int protocol, int64_t handles[2]);
typedef int (*QemuHostNetworkResolveCallback)(
    void *opaque, const char *hostname, int family, void *address,
    size_t *address_size);

typedef struct QemuHostNetworkCallbacks {
    uint32_t size;
    uint32_t version;
    uint32_t capabilities;
    uint32_t reserved;
    QemuHostNetworkSocketCallback socket;
    QemuHostNetworkConnectCallback connect;
    QemuHostNetworkSendCallback send;
    QemuHostNetworkRecvCallback recv;
    QemuHostNetworkCloseCallback close;
    QemuHostNetworkAddressCallback bind;
    QemuHostNetworkListenCallback listen;
    QemuHostNetworkAcceptCallback accept;
    QemuHostNetworkShutdownCallback shutdown;
    QemuHostNetworkSendToCallback send_to;
    QemuHostNetworkRecvFromCallback recv_from;
    QemuHostNetworkGetNameCallback get_name;
    QemuHostNetworkGetOptionCallback get_option;
    QemuHostNetworkSetOptionCallback set_option;
    QemuHostNetworkSocketPairCallback socket_pair;
    QemuHostNetworkResolveCallback resolve;
} QemuHostNetworkCallbacks;

typedef int (*QemuHostHttpCreateRequestCallback)(
    void *opaque, const char *method, const char *url,
    uint64_t content_length, int64_t *handle);
typedef int (*QemuHostHttpAddHeaderCallback)(
    void *opaque, int64_t handle, const char *name, const char *value,
    int mode);
typedef int (*QemuHostHttpSendCallback)(void *opaque, int64_t handle,
                                        const void *body, size_t body_size);
typedef int (*QemuHostHttpGetStatusCallback)(void *opaque, int64_t handle,
                                             int *status_code);
typedef int (*QemuHostHttpGetHeadersCallback)(void *opaque, int64_t handle,
                                              char *buffer,
                                              size_t *buffer_size);
typedef int (*QemuHostHttpGetLengthCallback)(void *opaque, int64_t handle,
                                             uint64_t *content_length);
typedef int64_t (*QemuHostHttpReadCallback)(void *opaque, int64_t handle,
                                            void *buffer, size_t size);
typedef int (*QemuHostHttpRequestCallback)(void *opaque, int64_t handle);

typedef struct QemuHostHttpCallbacks {
    uint32_t size;
    uint32_t version;
    QemuHostHttpCreateRequestCallback create_request;
    QemuHostHttpAddHeaderCallback add_header;
    QemuHostHttpSendCallback send;
    QemuHostHttpGetStatusCallback get_status;
    QemuHostHttpGetHeadersCallback get_headers;
    QemuHostHttpGetLengthCallback get_length;
    QemuHostHttpReadCallback read;
    QemuHostHttpRequestCallback abort;
    QemuHostHttpRequestCallback close;
} QemuHostHttpCallbacks;

/* get_headers/read_frame use a size query when buffer is NULL. */

typedef enum QemuHostMediaStreamType {
    QEMU_HOST_MEDIA_STREAM_UNKNOWN = 0,
    QEMU_HOST_MEDIA_STREAM_VIDEO = 1,
    QEMU_HOST_MEDIA_STREAM_AUDIO = 2,
    QEMU_HOST_MEDIA_STREAM_TIMED_TEXT = 3,
} QemuHostMediaStreamType;

typedef enum QemuHostMediaControl {
    QEMU_HOST_MEDIA_START = 1,
    QEMU_HOST_MEDIA_STOP = 2,
    QEMU_HOST_MEDIA_PAUSE = 3,
    QEMU_HOST_MEDIA_RESUME = 4,
    QEMU_HOST_MEDIA_JUMP = 5,
    QEMU_HOST_MEDIA_SET_LOOPING = 6,
} QemuHostMediaControl;

typedef struct QemuHostMediaFrameInfo {
    uint64_t timestamp_us;
    uint32_t stream_type;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;
} QemuHostMediaFrameInfo;

typedef struct QemuHostMediaStreamInfo {
    uint32_t type;
    uint32_t index;
    uint64_t duration_us;
    uint64_t start_time_us;
    uint32_t width;
    uint32_t height;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;
    uint32_t reserved;
} QemuHostMediaStreamInfo;

typedef int (*QemuHostMediaOpenCallback)(void *opaque, const char *source,
                                         int64_t *handle);
typedef int (*QemuHostMediaControlCallback)(void *opaque, int64_t handle,
                                            QemuHostMediaControl control,
                                            uint64_t value);
typedef int (*QemuHostMediaStreamCountCallback)(void *opaque, int64_t handle,
                                                uint32_t *count);
typedef int (*QemuHostMediaStreamInfoCallback)(
    void *opaque, int64_t handle, uint32_t index,
    QemuHostMediaStreamInfo *info);
typedef int (*QemuHostMediaReadFrameCallback)(
    void *opaque, int64_t handle, uint32_t stream_type, void *buffer,
    size_t *buffer_size, QemuHostMediaFrameInfo *info);
typedef int (*QemuHostMediaCloseCallback)(void *opaque, int64_t handle);
typedef int (*QemuHostMediaGetTimeCallback)(void *opaque, int64_t handle,
                                            uint64_t *time_us);

typedef struct QemuHostMediaCallbacks {
    uint32_t size;
    uint32_t version;
    QemuHostMediaOpenCallback open;
    QemuHostMediaControlCallback control;
    QemuHostMediaStreamCountCallback get_stream_count;
    QemuHostMediaStreamInfoCallback get_stream_info;
    QemuHostMediaReadFrameCallback read_frame;
    QemuHostMediaCloseCallback close;
    QemuHostMediaGetTimeCallback get_time;
} QemuHostMediaCallbacks;

typedef int (*QemuHostLaunchCallback)(void *opaque, const char *path,
                                      const char *const *argv, size_t argc);

typedef int (*QemuHostGuestReadCallback)(void *memory_opaque,
                                         uint64_t guest_address,
                                         void *buffer, size_t size);
typedef int (*QemuHostGuestWriteCallback)(void *memory_opaque,
                                          uint64_t guest_address,
                                          const void *buffer, size_t size);

typedef struct QemuHostHLERequest {
    uint32_t size;
    uint32_t version;
    const char *module;
    const char *library;
    const char *nid;
    uint64_t args[7];
    void *memory_opaque;
    QemuHostGuestReadCallback read_guest;
    QemuHostGuestWriteCallback write_guest;
} QemuHostHLERequest;

/*
 * The callback is synchronous. The request, strings, memory token and guest
 * accessors are valid only until the callback returns. Register it before
 * qemu_host_init() so external imports can be included in the HLE image.
 */
typedef int (*QemuHostHLECallback)(void *opaque,
                                   const QemuHostHLERequest *request,
                                   uint64_t *result);

typedef void (*QemuHostLogCallback)(void *opaque,
                                    QemuHostLogLevel level,
                                    const char *message);
typedef void (*QemuHostVideoCallback)(void *opaque,
                                      const void *pixels,
                                      int width,
                                      int height,
                                      int stride,
                                      QemuHostPixelFormat format);

#define QEMU_HOST_D3D12_VIDEO_FRAME_VERSION 1

/*
 * D3D12 objects are borrowed and remain owned by QEMU. The callback is
 * synchronous. A same-process host should enqueue its copy/present work on
 * command_queue before returning, leaving resource in resource_state. A host
 * using another device/queue must open the shared handles, wait for
 * fence_value, and finish reading the resource before the callback returns.
 */
typedef struct QemuHostD3D12VideoFrame {
    uint32_t size;
    uint32_t version;
    uint64_t frame_id;
    uint32_t buffer_index;
    uint32_t width;
    uint32_t height;
    uint32_t dxgi_format;
    uint32_t resource_state;
    uint32_t adapter_luid_low;
    int32_t adapter_luid_high;
    uint64_t resource_handle;
    uint64_t fence_handle;
    uint64_t fence_value;
    void *device;
    void *command_queue;
    void *resource;
    void *fence;
} QemuHostD3D12VideoFrame;

typedef void (*QemuHostD3D12VideoCallback)(
    void *opaque, const QemuHostD3D12VideoFrame *frame);
typedef void (*QemuHostAudioCallback)(void *opaque,
                                      const void *samples,
                                      size_t size,
                                      int sample_rate,
                                      int channels,
                                      QemuHostAudioFormat format);
typedef void (*QemuHostPadOutputCallback)(void *opaque, int controller,
                                         const QemuHostPadOutput *output);

QEMU_HOST_EXPORT int qemu_host_init(int argc, char **argv);
QEMU_HOST_EXPORT uint32_t qemu_host_get_api_version(void);
QEMU_HOST_EXPORT int qemu_host_start(void);
QEMU_HOST_EXPORT int qemu_host_main_loop_step(bool nonblocking,
                                              int *exit_status);
QEMU_HOST_EXPORT int qemu_host_request_shutdown(void);
QEMU_HOST_EXPORT int qemu_host_reset(void);
QEMU_HOST_EXPORT int qemu_host_join(int *exit_status);
QEMU_HOST_EXPORT int qemu_host_cleanup(void);

QEMU_HOST_EXPORT bool qemu_host_is_initialized(void);
QEMU_HOST_EXPORT bool qemu_host_is_running(void);
QEMU_HOST_EXPORT int qemu_host_get_exit_status(void);
QEMU_HOST_EXPORT int qemu_host_get_last_fault(QemuHostFaultInfo *info);

QEMU_HOST_EXPORT void qemu_host_register_log_callback(QemuHostLogCallback cb,
                                                      void *opaque);
QEMU_HOST_EXPORT bool qemu_host_log_callback_enabled(void);
QEMU_HOST_EXPORT void qemu_host_emit_log(QemuHostLogLevel level,
                                         const char *message);

QEMU_HOST_EXPORT void qemu_host_register_video_callback(
    QemuHostVideoCallback cb, void *opaque);
QEMU_HOST_EXPORT void qemu_host_emit_video_frame(const void *pixels,
                                                 int width,
                                                 int height,
                                                 int stride,
                                                 QemuHostPixelFormat format);
QEMU_HOST_EXPORT int qemu_host_register_d3d12_video_callback(
    QemuHostD3D12VideoCallback cb, void *opaque);
QEMU_HOST_EXPORT bool qemu_host_d3d12_video_callback_enabled(void);
QEMU_HOST_EXPORT void qemu_host_emit_d3d12_video_frame(
    const QemuHostD3D12VideoFrame *frame);

QEMU_HOST_EXPORT void qemu_host_register_audio_callback(
    QemuHostAudioCallback cb, void *opaque);
QEMU_HOST_EXPORT void qemu_host_emit_audio_frame(const void *samples,
                                                 size_t size,
                                                 int sample_rate,
                                                 int channels,
                                                 QemuHostAudioFormat format);

QEMU_HOST_EXPORT void qemu_host_register_pad_output_callback(
    QemuHostPadOutputCallback cb, void *opaque);
QEMU_HOST_EXPORT void qemu_host_emit_pad_output(
    int controller, const QemuHostPadOutput *output);

QEMU_HOST_EXPORT void qemu_host_register_storage_callbacks(
    const QemuHostStorageCallbacks *callbacks, void *opaque);
QEMU_HOST_EXPORT int qemu_host_register_brokered_storage_callbacks(
    const QemuHostBrokeredStorageCallbacks *callbacks, void *opaque);
QEMU_HOST_EXPORT int qemu_host_mount_brokered_file(
    const char *virtual_path, void *storage_file, void *random_access_stream);
QEMU_HOST_EXPORT int qemu_host_mount_brokered_folder(
    const char *virtual_path, void *storage_folder);
QEMU_HOST_EXPORT int qemu_host_unmount_brokered_storage(
    const char *virtual_path);
QEMU_HOST_EXPORT void qemu_host_register_dialog_callback(
    QemuHostDialogRequestCallback cb, void *opaque);
QEMU_HOST_EXPORT int qemu_host_complete_dialog(
    uint64_t request_id, int status, const void *payload, size_t payload_size);
QEMU_HOST_EXPORT void qemu_host_register_network_callbacks(
    const QemuHostNetworkCallbacks *callbacks, void *opaque);
QEMU_HOST_EXPORT void qemu_host_register_http_callbacks(
    const QemuHostHttpCallbacks *callbacks, void *opaque);
QEMU_HOST_EXPORT void qemu_host_register_media_callbacks(
    const QemuHostMediaCallbacks *callbacks, void *opaque);
QEMU_HOST_EXPORT void qemu_host_register_launch_callback(
    QemuHostLaunchCallback callback, void *opaque);
QEMU_HOST_EXPORT void qemu_host_register_hle_callback(
    QemuHostHLECallback callback, void *opaque);
QEMU_HOST_EXPORT bool qemu_host_hle_callback_enabled(void);

QEMU_HOST_EXPORT int qemu_host_send_key_number(int key_number, bool down);
QEMU_HOST_EXPORT int qemu_host_send_key_qcode(int qcode, bool down);
QEMU_HOST_EXPORT int qemu_host_send_pointer_rel(int dx, int dy);
QEMU_HOST_EXPORT int qemu_host_send_pointer_abs(int x,
                                                int y,
                                                int width,
                                                int height);
QEMU_HOST_EXPORT int qemu_host_send_pointer_button(QemuHostPointerButton button,
                                                   bool down);
QEMU_HOST_EXPORT int qemu_host_capture_pointer_abs(void);
QEMU_HOST_EXPORT bool qemu_host_pointer_is_absolute(void);
QEMU_HOST_EXPORT int qemu_host_send_gamepad_state(
    int controller, const QemuHostGamepadState *state);
QEMU_HOST_EXPORT int qemu_host_send_touch_state(
    int controller, const QemuHostTouchState *state);
QEMU_HOST_EXPORT int qemu_host_send_motion_state(
    int controller, const QemuHostMotionState *state);
QEMU_HOST_EXPORT int qemu_host_send_audio_input(
    const void *samples, size_t size, int sample_rate, int channels,
    QemuHostAudioFormat format);

#if defined QEMU_HOST_BUILD || defined QEMU_HOST_INTERNAL
typedef struct QemuHostInputSink {
    int (*gamepad)(void *opaque, int controller,
                   const QemuHostGamepadState *state);
    int (*touch)(void *opaque, int controller,
                 const QemuHostTouchState *state);
    int (*motion)(void *opaque, int controller,
                  const QemuHostMotionState *state);
    int (*audio_input)(void *opaque, const void *samples, size_t size,
                       int sample_rate, int channels,
                       QemuHostAudioFormat format);
} QemuHostInputSink;

void qemu_host_register_input_sink(const QemuHostInputSink *sink,
                                   void *opaque);
int qemu_host_storage_open(const char *path, int flags, int mode,
                           int64_t *handle);
int64_t qemu_host_storage_read(int64_t handle, void *buffer, size_t size);
int64_t qemu_host_storage_write(int64_t handle, const void *buffer,
                                size_t size);
int qemu_host_storage_close(int64_t handle);
int64_t qemu_host_storage_seek(int64_t handle, int64_t offset, int whence);
int qemu_host_storage_stat(const char *path, QemuHostStorageStat *stat);
int qemu_host_storage_mkdir(const char *path, int mode);
int qemu_host_storage_unlink(const char *path);
int qemu_host_storage_rename(const char *old_path, const char *new_path);
int qemu_host_storage_flush(int64_t handle);
int qemu_host_storage_readdir(int64_t handle, char *name, size_t name_size,
                              QemuHostStorageStat *stat);
int qemu_host_storage_atomic_replace(const char *path, const void *data,
                                     size_t size);
int qemu_host_storage_cleanup(const char *path);
int qemu_host_storage_truncate(int64_t handle, uint64_t size);
bool qemu_host_storage_path_is_brokered(const char *path);
void qemu_host_record_last_fault(const QemuHostFaultInfo *info);
int qemu_host_dialog_request(uint32_t type, const void *payload, size_t size,
                             uint64_t *request_id);
int qemu_host_dialog_take_response(uint64_t *request_id, int *status,
                                   void *payload, size_t *payload_size);
int qemu_host_network_socket(int domain, int type, int protocol,
                             int64_t *handle);
int qemu_host_network_connect(int64_t handle, const char *address,
                              uint16_t port);
int64_t qemu_host_network_send(int64_t handle, const void *buffer, size_t size,
                               int flags);
int64_t qemu_host_network_recv(int64_t handle, void *buffer, size_t size,
                               int flags);
int qemu_host_network_close(int64_t handle);
int qemu_host_network_bind(int64_t handle, const void *address,
                           size_t address_size);
int qemu_host_network_listen(int64_t handle, int backlog);
int qemu_host_network_accept(int64_t handle, void *address,
                             size_t *address_size, int64_t *accepted_handle);
int qemu_host_network_shutdown(int64_t handle, int how);
int64_t qemu_host_network_send_to(int64_t handle, const void *buffer,
                                  size_t size, int flags,
                                  const void *address, size_t address_size);
int64_t qemu_host_network_recv_from(int64_t handle, void *buffer, size_t size,
                                    int flags, void *address,
                                    size_t *address_size);
int qemu_host_network_get_name(int64_t handle, bool peer, void *address,
                               size_t *address_size);
int qemu_host_network_get_option(int64_t handle, int level, int option,
                                 void *value, size_t *value_size);
int qemu_host_network_set_option(int64_t handle, int level, int option,
                                 const void *value, size_t value_size);
int qemu_host_network_socket_pair(int domain, int type, int protocol,
                                  int64_t handles[2]);
int qemu_host_network_resolve(const char *hostname, int family,
                               void *address, size_t *address_size);
int qemu_host_http_create_request(const char *method, const char *url,
                                  uint64_t content_length, int64_t *handle);
int qemu_host_http_add_header(int64_t handle, const char *name,
                              const char *value, int mode);
int qemu_host_http_send(int64_t handle, const void *body, size_t body_size);
int qemu_host_http_get_status(int64_t handle, int *status_code);
int qemu_host_http_get_headers(int64_t handle, char *buffer,
                               size_t *buffer_size);
int qemu_host_http_get_length(int64_t handle, uint64_t *content_length);
int64_t qemu_host_http_read(int64_t handle, void *buffer, size_t size);
int qemu_host_http_abort(int64_t handle);
int qemu_host_http_close(int64_t handle);
int qemu_host_media_open(const char *source, int64_t *handle);
int qemu_host_media_control(int64_t handle, QemuHostMediaControl control,
                            uint64_t value);
int qemu_host_media_get_stream_count(int64_t handle, uint32_t *count);
int qemu_host_media_get_stream_info(int64_t handle, uint32_t index,
                                    QemuHostMediaStreamInfo *info);
int qemu_host_media_read_frame(int64_t handle, uint32_t stream_type,
                               void *buffer, size_t *buffer_size,
                               QemuHostMediaFrameInfo *info);
int qemu_host_media_close(int64_t handle);
int qemu_host_media_get_time(int64_t handle, uint64_t *time_us);
int qemu_host_launch(const char *path, const char *const *argv, size_t argc);
int qemu_host_hle_invoke(const QemuHostHLERequest *request, uint64_t *result);
#endif

#ifdef __cplusplus
}
#endif

#endif /* QEMU_HOST_H */
