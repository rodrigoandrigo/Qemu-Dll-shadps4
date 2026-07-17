/*
 * shadPS4 hybrid kernel HLE
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <zlib.h>
#include <math.h>
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "exec/cputlb.h"
#define QEMU_HOST_INTERNAL
#include "hw/i386/shadps4-hle.h"
#include "system/runstate.h"
#include "target/i386/cpu.h"

#define SHADPS4_SYS_EXIT 1
#define SHADPS4_SYS_READ 3
#define SHADPS4_SYS_WRITE 4
#define SHADPS4_SYS_OPEN 5
#define SHADPS4_SYS_CLOSE 6
#define SHADPS4_SYS_UNLINK 10
#define SHADPS4_SYS_GETPID 20
#define SHADPS4_SYS_GETUID 24
#define SHADPS4_SYS_GETEUID 25
#define SHADPS4_SYS_GETEGID 43
#define SHADPS4_SYS_GETGID 47
#define SHADPS4_SYS_IOCTL 54
#define SHADPS4_SYS_MUNMAP 73
#define SHADPS4_SYS_FSYNC 95
#define SHADPS4_SYS_SOCKET 97
#define SHADPS4_SYS_CONNECT 98
#define SHADPS4_SYS_GETTIMEOFDAY 116
#define SHADPS4_SYS_RENAME 128
#define SHADPS4_SYS_MKDIR 136
#define SHADPS4_SYS_STAT 188
#define SHADPS4_SYS_FSTAT 189
#define SHADPS4_SYS_GETDIRENTRIES 196
#define SHADPS4_SYS_CLOCK_GETTIME 232
#define SHADPS4_SYS_KQUEUE 362
#define SHADPS4_SYS_KEVENT 363
#define SHADPS4_SYS_THR_EXIT 431
#define SHADPS4_SYS_THR_SELF 432
#define SHADPS4_SYS_UMTX_OP 454
#define SHADPS4_SYS_MMAP 477
#define SHADPS4_SYS_LSEEK 478

#define SHADPS4_GUEST_EBADF 9
#define SHADPS4_GUEST_ENOENT 2
#define SHADPS4_GUEST_EACCES 13
#define SHADPS4_GUEST_EIO 5
#define SHADPS4_GUEST_ENOMEM 12
#define SHADPS4_GUEST_EFAULT 14
#define SHADPS4_GUEST_EINVAL 22
#define SHADPS4_GUEST_EBUSY 16
#define SHADPS4_GUEST_EAGAIN 35
#define SHADPS4_GUEST_ENOSYS 78
#define SHADPS4_GUEST_EROFS 30
#define SHADPS4_GUEST_USER_MIN (64 * MiB)

#define SHADPS4_USER_SERVICE_ERROR_INVALID_ARGUMENT 0x80960005U
#define SHADPS4_USER_SERVICE_ERROR_NO_EVENT 0x80960007U
#define SHADPS4_USER_SERVICE_ERROR_BUFFER_TOO_SHORT 0x8096000aU
#define SHADPS4_SYSTEM_SERVICE_ERROR_PARAMETER 0x80a10003U
#define SHADPS4_SYSTEM_SERVICE_ERROR_NO_EVENT 0x80a10004U
#define SHADPS4_SYSMODULE_INVALID_ID 0x805a1000U
#define SHADPS4_SYSMODULE_NOT_LOADED 0x805a1001U
#define SHADPS4_KERNEL_ERROR_UNKNOWN 0x80020000U
#define SHADPS4_KERNEL_ERROR_EPERM 0x80020001U
#define SHADPS4_KERNEL_ERROR_ENOENT 0x80020002U
#define SHADPS4_KERNEL_ERROR_ESRCH 0x80020003U
#define SHADPS4_KERNEL_ERROR_EBADF 0x80020009U
#define SHADPS4_KERNEL_ERROR_EFAULT 0x8002000eU
#define SHADPS4_KERNEL_ERROR_EINVAL 0x80020016U
#define SHADPS4_KERNEL_ERROR_ENOMEM 0x8002000cU
#define SHADPS4_KERNEL_ERROR_ETIMEDOUT 0x8002003cU
#define SHADPS4_NP_ERROR_INVALID_ARGUMENT 0x80550003U
#define SHADPS4_NP_ERROR_INVALID_PLATFORM_TYPE 0x80550004U
#define SHADPS4_NP_UTIL_ERROR_NOT_MATCH 0x80550609U
#define SHADPS4_NP_LW_COND_ERROR_TIMEDOUT 0x8055800bU
#define SHADPS4_NP_LW_MUTEX_ERROR_BUSY 0x8055800fU
#define SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT 0x80552902U
#define SHADPS4_NP_WEBAPI_ERROR_INVALID_LIB_CONTEXT_ID 0x80552903U
#define SHADPS4_NP_WEBAPI_ERROR_CONTEXT_NOT_FOUND 0x80552904U
#define SHADPS4_NP_WEBAPI_ERROR_USER_NOT_FOUND 0x80552905U
#define SHADPS4_NP_WEBAPI_ERROR_REQUEST_NOT_FOUND 0x80552906U
#define SHADPS4_NP_WEBAPI_ERROR_INVALID_CONTENT 0x80552908U
#define SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT 0x80553402U
#define SHADPS4_NP_WEBAPI2_ERROR_INVALID_LIB_CONTEXT_ID 0x80553403U
#define SHADPS4_NP_WEBAPI2_ERROR_CONTEXT_NOT_FOUND 0x80553404U
#define SHADPS4_NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND 0x80553406U
#define SHADPS4_NP_WEBAPI2_ERROR_INVALID_CONTENT 0x80553408U
#define SHADPS4_NP_COMMUNITY_ERROR_INVALID_ARGUMENT 0x80550704U
#define SHADPS4_NP_COMMUNITY_ERROR_NO_LOGIN 0x80550705U
#define SHADPS4_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS 0x80550706U
#define SHADPS4_NP_COMMUNITY_ERROR_ABORTED 0x80550707U
#define SHADPS4_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT 0x8055070cU
#define SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID 0x8055070eU
#define SHADPS4_NP_MATCHING_ERROR_ALREADY_INITIALIZED 0x80550c02U
#define SHADPS4_NP_MATCHING_ERROR_NOT_INITIALIZED 0x80550c03U
#define SHADPS4_NP_MATCHING_ERROR_CONTEXT_NOT_FOUND 0x80550c06U
#define SHADPS4_NP_MATCHING_ERROR_CONTEXT_ALREADY_STARTED 0x80550c07U
#define SHADPS4_NP_MATCHING_ERROR_CONTEXT_NOT_STARTED 0x80550c08U
#define SHADPS4_NP_MATCHING_ERROR_INVALID_ARGUMENT 0x80550c0aU
#define SHADPS4_NP_MATCHING_ERROR_SERVER_NOT_AVAILABLE 0x80550c28U
#define SHADPS4_NP_SIGNALING_ERROR_NOT_INITIALIZED 0x80552701U
#define SHADPS4_NP_SIGNALING_ERROR_ALREADY_INITIALIZED 0x80552702U
#define SHADPS4_NP_SIGNALING_ERROR_CONTEXT_NOT_FOUND 0x80552705U
#define SHADPS4_NP_SIGNALING_ERROR_CONNECTION_NOT_FOUND 0x8055270eU
#define SHADPS4_NP_SIGNALING_ERROR_INVALID_ARGUMENT 0x80552715U
#define SHADPS4_KERNEL_ERROR_ENAMETOOLONG 0x8002003fU
#define SHADPS4_RTC_ERROR_DATETIME_UNINITIALIZED 0x7ffef9feU
#define SHADPS4_RTC_ERROR_INVALID_POINTER 0x80b50002U
#define SHADPS4_RTC_ERROR_INVALID_VALUE 0x80b50003U
#define SHADPS4_RTC_ERROR_BAD_PARSE 0x80b50007U
#define SHADPS4_RTC_ERROR_INVALID_YEAR 0x80b50008U
#define SHADPS4_RTC_ERROR_INVALID_MONTH 0x80b50009U
#define SHADPS4_RTC_ERROR_INVALID_DAY 0x80b5000aU
#define SHADPS4_RTC_ERROR_INVALID_HOUR 0x80b5000bU
#define SHADPS4_RTC_ERROR_INVALID_MINUTE 0x80b5000cU
#define SHADPS4_RTC_ERROR_INVALID_SECOND 0x80b5000dU
#define SHADPS4_RTC_ERROR_INVALID_MICROSECOND 0x80b5000eU

#define SHADPS4_RTC_DAY_TICKS 86400000000ULL
#define SHADPS4_RTC_UNIX_EPOCH_TICKS 62135596800000000ULL
#define SHADPS4_RTC_FILETIME_EPOCH_TICKS 50491123200000000ULL
#define SHADPS4_RTC_MAX_TICKS 315537897599999999ULL

#define SHADPS4_PAGE_PRESENT (1ULL << 0)
#define SHADPS4_PAGE_WRITE (1ULL << 1)
#define SHADPS4_PAGE_USER (1ULL << 2)
#define SHADPS4_PAGE_LARGE (1ULL << 7)
#define SHADPS4_PAGE_NX (1ULL << 63)

#define SHADPS4_GPU_IOCTL_SUBMIT 0xc0108101ULL
#define SHADPS4_GPU_IOCTL_FLIP 0xc0108102ULL
#define SHADPS4_GPU_IOCTL_STATUS_LEGACY 0x80188103ULL
#define SHADPS4_GPU_IOCTL_STATUS 0x80308103ULL
#define SHADPS4_AUDIO_IOCTL_CONFIG 0xc00c8201ULL
#define SHADPS4_AUDIO_IOCTL_DRAIN 0x20008202ULL
#define SHADPS4_AUDIO_IOCTL_QUEUED 0x80088203ULL
#define SHADPS4_AUDIO_IOCTL_VOLUME 0xc00c8204ULL
#define SHADPS4_PAD_IOCTL_RUMBLE 0xc0048202ULL
#define SHADPS4_PAD_IOCTL_OUTPUT 0xc0088203ULL
#define SHADPS4_STORAGE_IOCTL_ATOMIC_REPLACE 0xc0108301ULL
#define SHADPS4_HLE_INTERNAL_FAULT UINT64_MAX
#define SHADPS4_ELF_STT_FUNC 2
#define SHADPS4_O_ACCMODE 3
#define SHADPS4_O_WRONLY 1
#define SHADPS4_O_RDWR 2
#define SHADPS4_O_APPEND 8
#define SHADPS4_O_CREAT 0x200
#define SHADPS4_O_TRUNC 0x400

typedef struct ShadPS4GuestTime {
    uint64_t seconds;
    uint64_t fraction;
} ShadPS4GuestTime;

typedef struct ShadPS4GuestIovec {
    uint64_t base;
    uint64_t length;
} ShadPS4GuestIovec;

typedef struct ShadPS4GuestVirtualQueryInfo {
    uint64_t start;
    uint64_t end;
    uint64_t offset;
    uint32_t protection;
    uint32_t memory_type;
    uint8_t attributes;
    uint8_t reserved[7];
    char name[32];
} ShadPS4GuestVirtualQueryInfo;

typedef struct ShadPS4GuestAioRequest {
    int64_t offset;
    int64_t size;
    uint64_t buffer;
    uint64_t result;
    int32_t fd;
    uint32_t reserved;
} ShadPS4GuestAioRequest;

typedef struct ShadPS4GuestAioResult {
    int64_t value;
    uint32_t state;
    uint32_t reserved;
} ShadPS4GuestAioResult;

typedef struct ShadPS4GuestSigaction {
    uint64_t handler;
    uint32_t flags;
    uint32_t mask[4];
    uint32_t reserved;
} ShadPS4GuestSigaction;

typedef struct ShadPS4GuestSwVersion {
    uint64_t size;
    char text[28];
    uint32_t version;
} ShadPS4GuestSwVersion;

typedef struct ShadPS4GuestAppInfo {
    int32_t app_id;
    int32_t mmap_flags;
    int32_t attribute_exe;
    int32_t attribute2;
    char title_id[10];
    uint8_t flags[6];
    uint64_t preload_prx_flags;
    int32_t attribute1;
    int32_t has_param_sfo;
    int32_t workaround_version;
    int32_t workaround_align;
    uint64_t workaround_ids[2];
} ShadPS4GuestAppInfo;

typedef struct ShadPS4GuestTLSIndex {
    uint64_t module;
    uint64_t offset;
} ShadPS4GuestTLSIndex;

typedef struct ShadPS4GuestEvent {
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    int64_t data;
    uint64_t user_data;
} ShadPS4GuestEvent;

typedef struct ShadPS4AudioConfig {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;
} ShadPS4AudioConfig;

typedef struct ShadPS4AudioVolume {
    uint8_t muted;
    uint8_t channels;
    uint8_t volume[8];
    uint8_t reserved[2];
} ShadPS4AudioVolume;

typedef struct ShadPS4GuestTouch {
    uint32_t id;
    uint16_t x;
    uint16_t y;
    uint8_t active;
    uint8_t reserved[3];
} ShadPS4GuestTouch;

typedef struct ShadPS4GuestPadState {
    ShadPS4PadState legacy;
    uint8_t left_trigger;
    uint8_t right_trigger;
    uint8_t connected;
    uint8_t reserved;
    ShadPS4GuestTouch touches[QEMU_HOST_MAX_TOUCHES];
    int16_t acceleration[3];
    int16_t angular_velocity[3];
    int16_t orientation[4];
    QemuHostPadOutput output;
    uint64_t timestamp_ns;
} ShadPS4GuestPadState;

typedef struct ShadPS4GuestStorageStat {
    uint64_t size;
    uint64_t allocated_size;
    uint64_t modified_time_ns;
    uint32_t mode;
    uint32_t type;
} ShadPS4GuestStorageStat;

typedef struct ShadPS4GuestDirent {
    uint64_t inode;
    uint16_t record_size;
    uint8_t type;
    uint8_t name_length;
    char name[256];
} ShadPS4GuestDirent;

typedef struct ShadPS4GuestDialogResponse {
    uint64_t request_id;
    int32_t status;
    uint32_t payload_size;
} ShadPS4GuestDialogResponse;

typedef struct QEMU_PACKED ShadPS4GuestSystemServiceStatus {
    uint32_t event_num;
    uint8_t is_system_ui_overlaid;
    uint8_t is_in_background_execution;
    uint8_t is_cpu_mode7_cpu_normal;
    uint8_t is_game_live_streaming_on_air;
    uint8_t is_out_of_vr_play_area;
    uint8_t reserved[3];
} ShadPS4GuestSystemServiceStatus;

QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestSystemServiceStatus) != 12);

typedef struct QEMU_PACKED ShadPS4GuestModuleSegmentInfo {
    uint64_t address;
    uint32_t size;
    int32_t protection;
} ShadPS4GuestModuleSegmentInfo;

typedef struct QEMU_PACKED ShadPS4GuestModuleInfo {
    uint64_t size;
    char name[256];
    ShadPS4GuestModuleSegmentInfo segments[4];
    uint32_t segment_count;
    uint8_t fingerprint[20];
} ShadPS4GuestModuleInfo;

typedef struct QEMU_PACKED ShadPS4GuestModuleInfoEx {
    uint64_t size;
    char name[256];
    int32_t id;
    uint32_t tls_index;
    uint64_t tls_init_addr;
    uint32_t tls_init_size;
    uint32_t tls_size;
    uint32_t tls_offset;
    uint32_t tls_align;
    uint64_t init_proc_addr;
    uint64_t fini_proc_addr;
    uint64_t reserved1;
    uint64_t reserved2;
    uint64_t eh_frame_hdr_addr;
    uint64_t eh_frame_addr;
    uint32_t eh_frame_hdr_size;
    uint32_t eh_frame_size;
    ShadPS4GuestModuleSegmentInfo segments[4];
    uint32_t segment_count;
    uint32_t padding;
} ShadPS4GuestModuleInfoEx;

typedef struct QEMU_PACKED ShadPS4GuestModuleInfoForUnwind {
    uint64_t size;
    char name[256];
    uint64_t eh_frame_hdr_addr;
    uint64_t eh_frame_addr;
    uint64_t eh_frame_size;
    uint64_t segment_addr;
    uint64_t segment_size;
} ShadPS4GuestModuleInfoForUnwind;

QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestModuleSegmentInfo) != 16);
QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestModuleInfo) != 352);
QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestModuleInfoEx) != 424);
QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestModuleInfoForUnwind) != 304);

typedef struct ShadPS4GuestAtomicReplace {
    uint64_t data;
    uint64_t size;
} ShadPS4GuestAtomicReplace;

typedef struct ShadPS4GuestSockaddrIn {
    uint8_t length;
    uint8_t family;
    uint8_t port[2];
    uint8_t address[4];
    uint8_t zero[8];
} ShadPS4GuestSockaddrIn;

typedef struct QEMU_PACKED ShadPS4GuestMsgHdr {
    uint64_t name;
    uint32_t name_length;
    uint32_t padding0;
    uint64_t iov;
    int32_t iov_count;
    uint32_t padding1;
    uint64_t control;
    uint32_t control_length;
    int32_t flags;
} ShadPS4GuestMsgHdr;

QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestMsgHdr) != 48);

typedef struct QEMU_PACKED ShadPS4GuestVideoBufferAttribute {
    uint32_t pixel_format;
    int32_t tiling_mode;
    int32_t aspect_ratio;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t option;
    uint32_t reserved0;
    uint64_t reserved1;
} ShadPS4GuestVideoBufferAttribute;

typedef struct QEMU_PACKED ShadPS4GuestVideoFlipStatus {
    uint64_t count;
    uint64_t process_time;
    uint64_t tsc;
    int64_t flip_arg;
    uint64_t submit_tsc;
    uint64_t reserved0;
    int32_t gc_queue_num;
    int32_t flip_pending_num;
    int32_t current_buffer;
    uint32_t reserved1;
} ShadPS4GuestVideoFlipStatus;

typedef struct QEMU_PACKED ShadPS4GuestPadData {
    uint32_t buttons;
    uint8_t left_x;
    uint8_t left_y;
    uint8_t right_x;
    uint8_t right_y;
    uint8_t left_trigger;
    uint8_t right_trigger;
    uint8_t analog_padding[2];
    float orientation[4];
    float acceleration[3];
    float angular_velocity[3];
    uint8_t touch_count;
    uint8_t touch_reserved[3];
    uint32_t touch_time;
    struct {
        uint16_t x;
        uint16_t y;
        uint8_t id;
        uint8_t reserved[3];
    } touches[QEMU_HOST_MAX_TOUCHES];
    uint8_t connected;
    uint8_t timestamp_padding[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t reserved[2];
    uint8_t unique_data_length;
    uint8_t unique_data[12];
} ShadPS4GuestPadData;

typedef struct QEMU_PACKED ShadPS4GuestPadControllerInfo {
    float pixel_density;
    uint16_t touch_width;
    uint16_t touch_height;
    uint8_t dead_zone_left;
    uint8_t dead_zone_right;
    uint8_t connection_type;
    uint8_t connected_count;
    uint8_t connected;
    uint8_t device_class;
    uint8_t reserved[8];
    uint8_t alignment[2];
} ShadPS4GuestPadControllerInfo;

typedef struct QEMU_PACKED ShadPS4GuestPadExtendedControllerInfo {
    ShadPS4GuestPadControllerInfo base;
    uint16_t pad_type1;
    uint16_t pad_type2;
    uint8_t capability;
    uint8_t class_data[8];
    uint8_t alignment[3];
} ShadPS4GuestPadExtendedControllerInfo;

typedef struct QEMU_PACKED ShadPS4GuestPadDeviceClassInfo {
    int32_t device_class;
    uint8_t reserved[4];
    uint8_t class_data[12];
} ShadPS4GuestPadDeviceClassInfo;

typedef struct QEMU_PACKED ShadPS4GuestPadInfo {
    uint32_t unknown1;
    uint32_t unknown2;
    uint32_t pad_handle;
    uint32_t unknown3;
    uint32_t unknown4;
    uint32_t unknown5;
    uint32_t colour;
    uint32_t unknown6;
    uint32_t unknown[30];
} ShadPS4GuestPadInfo;

typedef struct QEMU_PACKED ShadPS4GuestAudioOutputParam {
    int32_t handle;
    uint32_t padding;
    uint64_t data;
} ShadPS4GuestAudioOutputParam;

typedef struct QEMU_PACKED ShadPS4GuestAudioPortState {
    uint16_t output;
    uint8_t channels;
    uint8_t reserved8;
    int16_t volume;
    uint16_t reroute_counter;
    uint64_t flags;
    uint64_t reserved64[2];
} ShadPS4GuestAudioPortState;

typedef struct QEMU_PACKED ShadPS4GuestAudioSystemState {
    uint32_t loudness;
    uint8_t reserved8[4];
    uint64_t reserved64[3];
} ShadPS4GuestAudioSystemState;

QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestAudioPortState) != 32);
QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestAudioSystemState) != 32);
QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestPadControllerInfo) != 24);
QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestPadExtendedControllerInfo) != 40);
QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestPadDeviceClassInfo) != 20);
QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestPadInfo) != 152);

typedef struct QEMU_PACKED ShadPS4GuestSaveMountResult {
    char mount_point[16];
    uint64_t required_blocks;
    uint32_t unused;
    uint32_t mount_status;
    uint8_t reserved[28];
    uint32_t padding;
} ShadPS4GuestSaveMountResult;

typedef struct QEMU_PACKED ShadPS4GuestSaveMountInfo {
    uint64_t blocks;
    uint64_t free_blocks;
    uint8_t reserved[32];
} ShadPS4GuestSaveMountInfo;

typedef struct QEMU_PACKED ShadPS4GuestSaveMemoryData {
    uint64_t buffer;
    uint64_t size;
    int64_t offset;
    uint8_t reserved[40];
} ShadPS4GuestSaveMemoryData;

typedef struct QEMU_PACKED ShadPS4GuestSaveMemoryGet {
    int32_t user_id;
    uint32_t padding;
    uint64_t data;
    uint64_t param;
    uint64_t icon;
    uint32_t slot;
    uint8_t reserved[28];
} ShadPS4GuestSaveMemoryGet;

typedef struct QEMU_PACKED ShadPS4GuestSaveMemorySet {
    int32_t user_id;
    uint32_t padding;
    uint64_t data;
    uint64_t param;
    uint64_t icon;
    uint32_t data_count;
    uint32_t slot;
    uint8_t reserved[32];
} ShadPS4GuestSaveMemorySet;

typedef struct QEMU_PACKED ShadPS4GuestSaveMemorySetup {
    uint32_t option;
    int32_t user_id;
    uint64_t memory_size;
    uint64_t icon_memory_size;
    uint64_t initial_param;
    uint64_t initial_icon;
    uint32_t slot;
    uint8_t reserved[20];
} ShadPS4GuestSaveMemorySetup;

typedef struct QEMU_PACKED ShadPS4GuestDialogResult {
    uint32_t mode;
    uint32_t result;
    uint32_t button_id;
    uint8_t reserved[32];
} ShadPS4GuestDialogResult;

QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestPadState) != 104);
QEMU_BUILD_BUG_ON(sizeof(ShadPS4GuestPadData) != 120);

static bool shadps4_debug_rw(CPUState *cs, uint64_t addr, void *data,
                             size_t size, bool write)
{
    if (!size) {
        return true;
    }
    return cs && addr && data && addr <= UINT64_MAX - (size - 1) &&
           cpu_memory_rw_debug(cs, addr, data, size, write) == 0;
}

static bool shadps4_guest_rw(CPUState *cs, uint64_t addr, void *data,
                             size_t size, bool write)
{
    return !size || (addr >= SHADPS4_GUEST_USER_MIN &&
                     shadps4_debug_rw(cs, addr, data, size, write));
}

static bool shadps4_hle_sysmodule_id_valid(const ShadPS4HLEState *hle,
                                            uint32_t id)
{
    if (!id || id > SHADPS4_HLE_MAX_SYSMODULE_ID ||
        id == 5 || id == 8 || id == 64 || id == 84 || id == 123 ||
        id == 135 || id == 137 || id == 144 || id == 148 || id == 158 ||
        id == 161 || id == 187 || id == 191 || id == 219 || id == 236 ||
        id == 244 || id == 276 ||
        id == 0x19 || id == 0x25 || id == 0x3e || id == 0x48 ||
        id == 0x75 || id == 0x9f || id == 0xa3 || id == 0xca ||
        id == 0x109 ||
        (id >= 74 && id <= 79) || (id >= 126 && id <= 127) ||
        (id >= 266 && id <= 267) || (id >= 270 && id <= 273) ||
        (id >= 279 && id <= 302) || (id >= 304 && id <= 308) ||
        (id >= 310 && id <= 311)) {
        return false;
    }
    if ((id == 0x80 && hle->compiled_sdk_version >= 0x03000000) ||
        (id == 0xb0 && hle->compiled_sdk_version >= 0x07000000) ||
        (id == 0xb8 && hle->compiled_sdk_version >= 0x07500000)) {
        return false;
    }
    return true;
}

static bool shadps4_hle_argument(CPUState *cs, unsigned int index,
                                 uint64_t *value)
{
    CPUX86State *env = &X86_CPU(cs)->env;
    uint64_t offset;
    static const int registers[] = {
        R_EDI, R_ESI, R_EDX, R_R10, R_R8, R_R9,
    };

    if (index < ARRAY_SIZE(registers)) {
        *value = env->regs[registers[index]];
        return true;
    }
    offset = sizeof(uint64_t) *
             (index - ARRAY_SIZE(registers) + 1);

    if (env->regs[R_ESP] > UINT64_MAX - offset) {
        return false;
    }
    return shadps4_guest_rw(cs, env->regs[R_ESP] + offset, value,
                            sizeof(*value), false);
}

static bool shadps4_guest_read_string(CPUState *cs, uint64_t addr,
                                      char *buffer, size_t size)
{
    size_t i;

    if (!addr || size < 2) {
        return false;
    }
    for (i = 0; i < size - 1; i++) {
        if (addr > UINT64_MAX - i) {
            return false;
        }
        if (!shadps4_guest_rw(cs, addr + i, &buffer[i], 1, false)) {
            return false;
        }
        if (!buffer[i]) {
            return true;
        }
    }
    buffer[size - 1] = 0;
    return false;
}

static const ShadPS4ImageInfo *shadps4_hle_module_by_handle(
    const ShadPS4HLEState *hle, uint64_t handle)
{
    return handle < hle->module_image_count ? hle->module_images[handle] :
                                             NULL;
}

static const ShadPS4ImageInfo *shadps4_hle_module_by_address(
    const ShadPS4HLEState *hle, uint64_t address, uint32_t *handle)
{
    uint32_t i;

    for (i = 0; i < hle->module_image_count; i++) {
        const ShadPS4ImageInfo *image = hle->module_images[i];
        uint32_t segment;

        for (segment = 0; image && segment < image->segment_count; segment++) {
            const ShadPS4ImageSegment *candidate = &image->segments[segment];

            if (address >= candidate->virtual_addr &&
                address - candidate->virtual_addr < candidate->memory_size) {
                if (handle) {
                    *handle = i;
                }
                return image;
            }
        }
    }
    return NULL;
}

static int32_t shadps4_hle_module_protection(uint32_t flags)
{
    return ((flags & 4) ? 1 : 0) |
           ((flags & 2) ? 2 : 0) |
           ((flags & 1) ? 4 : 0);
}

static void shadps4_hle_module_segment(
    ShadPS4GuestModuleSegmentInfo *out, const ShadPS4ImageSegment *segment)
{
    out->address = cpu_to_le64(segment->virtual_addr);
    out->size = cpu_to_le32(MIN(segment->memory_size, UINT32_MAX));
    out->protection = cpu_to_le32(
        shadps4_hle_module_protection(segment->flags));
}

static uint64_t shadps4_hle_module_info_basic(
    ShadPS4HLEState *hle, CPUState *cs, uint64_t handle, uint64_t output,
    bool reject_system)
{
    const ShadPS4ImageInfo *image = shadps4_hle_module_by_handle(hle, handle);
    ShadPS4GuestModuleInfo info = { 0 };
    uint64_t requested_size;
    uint32_t i;

    if (!output || !shadps4_guest_rw(cs, output, &requested_size,
                                     sizeof(requested_size), false)) {
        return SHADPS4_KERNEL_ERROR_EFAULT;
    }
    if (le64_to_cpu(requested_size) != sizeof(info)) {
        return SHADPS4_KERNEL_ERROR_EINVAL;
    }
    if (!image) {
        return SHADPS4_KERNEL_ERROR_ESRCH;
    }
    /* All images currently exposed here come from the title, not sys_modules. */
    (void)reject_system;
    info.size = cpu_to_le64(sizeof(info));
    pstrcpy(info.name, sizeof(info.name), image->name ?: "<anonymous>");
    info.segment_count = cpu_to_le32(MIN(image->segment_count, 4));
    for (i = 0; i < MIN(image->segment_count, 4); i++) {
        shadps4_hle_module_segment(&info.segments[i], &image->segments[i]);
    }
    return shadps4_guest_rw(cs, output, &info, sizeof(info), true) ?
           0 : SHADPS4_KERNEL_ERROR_EFAULT;
}

static uint64_t shadps4_hle_module_info_extended(
    ShadPS4HLEState *hle, CPUState *cs, const ShadPS4ImageInfo *image,
    uint32_t handle, uint64_t output, bool validate_size)
{
    ShadPS4GuestModuleInfoEx info = { 0 };
    uint64_t requested_size;
    uint32_t i;

    if (!output || !shadps4_guest_rw(cs, output, &requested_size,
                                     sizeof(requested_size), false)) {
        return SHADPS4_KERNEL_ERROR_EFAULT;
    }
    if (validate_size && le64_to_cpu(requested_size) != sizeof(info)) {
        return SHADPS4_KERNEL_ERROR_EINVAL;
    }
    if (!image) {
        return SHADPS4_KERNEL_ERROR_ESRCH;
    }
    info.size = cpu_to_le64(sizeof(info));
    pstrcpy(info.name, sizeof(info.name), image->name ?: "<anonymous>");
    info.id = cpu_to_le32(handle);
    info.tls_index = cpu_to_le32(image->tls_module_id);
    info.tls_init_addr = cpu_to_le64(image->tls_addr);
    info.tls_init_size = cpu_to_le32(MIN(image->tls_file_size, UINT32_MAX));
    info.tls_size = cpu_to_le32(MIN(image->tls_memory_size, UINT32_MAX));
    info.tls_align = cpu_to_le32(MIN(image->tls_align, UINT32_MAX));
    info.eh_frame_hdr_addr = cpu_to_le64(image->eh_frame_hdr_addr);
    info.eh_frame_addr = cpu_to_le64(image->eh_frame_addr);
    info.eh_frame_hdr_size = cpu_to_le32(image->eh_frame_hdr_size);
    info.eh_frame_size = cpu_to_le32(image->eh_frame_size);
    info.segment_count = cpu_to_le32(MIN(image->segment_count, 4));
    for (i = 0; i < MIN(image->segment_count, 4); i++) {
        shadps4_hle_module_segment(&info.segments[i], &image->segments[i]);
    }
    return shadps4_guest_rw(cs, output, &info, sizeof(info), true) ?
           0 : SHADPS4_KERNEL_ERROR_EFAULT;
}

static uint64_t shadps4_hle_module_info_unwind(
    ShadPS4HLEState *hle, CPUState *cs, uint64_t address, uint64_t flags,
    uint64_t output, bool hide_name)
{
    const ShadPS4ImageInfo *image;
    ShadPS4GuestModuleInfoForUnwind info = { 0 };
    uint64_t requested_size;

    if (!output || !shadps4_guest_rw(cs, output, &requested_size,
                                     sizeof(requested_size), false)) {
        return SHADPS4_KERNEL_ERROR_EFAULT;
    }
    if (flags >= 3 || le64_to_cpu(requested_size) < sizeof(info)) {
        return SHADPS4_KERNEL_ERROR_EINVAL;
    }
    image = shadps4_hle_module_by_address(hle, address, NULL);
    if (!image) {
        return SHADPS4_KERNEL_ERROR_ESRCH;
    }
    if (!hide_name) {
        pstrcpy(info.name, sizeof(info.name), image->name ?: "<anonymous>");
    }
    info.eh_frame_hdr_addr = cpu_to_le64(image->eh_frame_hdr_addr);
    info.eh_frame_addr = cpu_to_le64(image->eh_frame_addr);
    info.eh_frame_size = cpu_to_le64(image->eh_frame_size);
    if (image->segment_count) {
        info.segment_addr = cpu_to_le64(image->segments[0].virtual_addr);
        info.segment_size = cpu_to_le64(image->segments[0].memory_size);
    }
    return shadps4_guest_rw(cs, output, &info, sizeof(info), true) ?
           0 : SHADPS4_KERNEL_ERROR_EFAULT;
}

static void shadps4_hle_symbol_nid(const char *name, char nid[12])
{
    static const uint8_t salt[] = {
        0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1,
        0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30,
    };
    static const char codes[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA1);
    uint8_t digest[20];
    gsize digest_size = sizeof(digest);
    uint64_t value;
    uint32_t i;

    g_checksum_update(checksum, (const uint8_t *)name, strlen(name));
    g_checksum_update(checksum, salt, sizeof(salt));
    g_checksum_get_digest(checksum, digest, &digest_size);
    value = ldq_le_p(digest);
    for (i = 0; i < 10; i++) {
        nid[i] = codes[(value >> (58 - i * 6)) & 0x3f];
    }
    nid[10] = codes[(value & 0xf) * 4];
    nid[11] = '\0';
}

static uint64_t shadps4_hle_load_start_module(
    ShadPS4HLEState *hle, CPUState *cs, uint64_t path_addr, uint64_t flags,
    uint64_t result_addr)
{
    char path[512];
    const char *name;
    const char *slash;
    const char *backslash;
    uint32_t i;

    if (!shadps4_guest_read_string(cs, path_addr, path, sizeof(path))) {
        return SHADPS4_KERNEL_ERROR_EFAULT;
    }
    if (flags) {
        return SHADPS4_KERNEL_ERROR_EINVAL;
    }
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    name = slash && (!backslash || slash > backslash) ? slash + 1 :
           backslash ? backslash + 1 : path;
    for (i = 0; i < hle->module_image_count; i++) {
        const ShadPS4ImageInfo *image = hle->module_images[i];

        if (image && !g_strcmp0(name, image->name)) {
            uint32_t result = 0;

            if (result_addr && !shadps4_guest_rw(cs, result_addr, &result,
                                                 sizeof(result), true)) {
                return SHADPS4_KERNEL_ERROR_EFAULT;
            }
            return i;
        }
    }
    return SHADPS4_KERNEL_ERROR_ENOENT;
}

static uint64_t shadps4_hle_dlsym(ShadPS4HLEState *hle, CPUState *cs,
                                  uint64_t handle, uint64_t name_addr,
                                  uint64_t output)
{
    const ShadPS4ImageInfo *image = shadps4_hle_module_by_handle(hle, handle);
    char name[512];
    char nid[12];
    uint32_t i;

    if (!image) {
        return SHADPS4_KERNEL_ERROR_ESRCH;
    }
    if (!output || !shadps4_guest_read_string(cs, name_addr, name,
                                               sizeof(name))) {
        return SHADPS4_KERNEL_ERROR_EFAULT;
    }
    shadps4_hle_symbol_nid(name, nid);
    for (i = 0; i < image->symbol_count; i++) {
        const ShadPS4DynamicSymbol *symbol = &image->symbols[i];

        if (symbol->defined && symbol->nid && !strcmp(symbol->nid, nid)) {
            uint64_t address = cpu_to_le64(image->virtual_base +
                                           symbol->value);

            return shadps4_guest_rw(cs, output, &address, sizeof(address),
                                    true) ? 0 : SHADPS4_KERNEL_ERROR_EFAULT;
        }
    }
    return SHADPS4_KERNEL_ERROR_ESRCH;
}

static bool shadps4_hle_storage_path(ShadPS4HLEState *hle, const char *path,
                                     char *scoped, size_t scoped_size,
                                     bool *read_only)
{
    static const char *const mounts[] = {
        "/app0/", "/data/", "/temp/", "/temp0/", "/download0/",
    };
    const char *component;
    const char *save_suffix = NULL;
    bool save_data = false;
    size_t i;

    if (strchr(path, '\\') || strchr(path, ':') || strstr(path, "//")) {
        return false;
    }
    if (hle->save_data_mounted &&
        (!strcmp(path, "/savedata0") ||
         g_str_has_prefix(path, "/savedata0/"))) {
        save_data = true;
        save_suffix = path + strlen("/savedata0");
    } else {
        for (i = 0; i < ARRAY_SIZE(mounts); i++) {
            if (g_str_has_prefix(path, mounts[i])) {
                break;
            }
        }
        if (i == ARRAY_SIZE(mounts)) {
            return false;
        }
    }
    for (component = path; component; ) {
        const char *end = strchr(component, '/');
        size_t length = end ? (size_t)(end - component) : strlen(component);

        if ((length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) {
            return false;
        }
        component = end ? end + 1 : NULL;
    }
    *read_only = !save_data && g_str_has_prefix(path, "/app0/");
    if ((save_data &&
         g_snprintf(scoped, scoped_size, "/titles/%s/savedata/%s%s",
                    hle->title_id, hle->save_data_dir, save_suffix) >=
                    scoped_size) ||
        (!save_data &&
         g_snprintf(scoped, scoped_size, "/titles/%s%s",
                    hle->title_id, path) >= scoped_size)) {
        return false;
    }
    return true;
}

static uint64_t shadps4_hle_open(ShadPS4HLEState *hle, CPUState *cs,
                                 uint64_t path_addr, uint64_t flags,
                                 uint64_t mode)
{
    char path[128];
    char scoped_path[192] = { 0 };
    ShadPS4HLEFileType type;
    int64_t storage_handle = -1;
    bool storage_read_only = false;
    int unit = 0;
    int fd;

    if (!shadps4_guest_read_string(cs, path_addr, path, sizeof(path))) {
        return -SHADPS4_GUEST_EFAULT;
    }
    if (!strcmp(path, "/dev/null")) {
        type = SHADPS4_HLE_FD_NULL;
    } else if (!strcmp(path, "/dev/zero")) {
        type = SHADPS4_HLE_FD_ZERO;
    } else if (!strcmp(path, "/dev/gc")) {
        type = SHADPS4_HLE_FD_GPU;
    } else if (!strcmp(path, "/dev/pad")) {
        type = SHADPS4_HLE_FD_PAD;
    } else if (strlen(path) == strlen("/dev/pad0") &&
               g_str_has_prefix(path, "/dev/pad") &&
               path[8] >= '0' && path[8] < '0' + QEMU_HOST_MAX_GAMEPADS) {
        unit = path[8] - '0';
        type = SHADPS4_HLE_FD_PAD;
    } else if (!strcmp(path, "/dev/audioout")) {
        type = SHADPS4_HLE_FD_AUDIO;
    } else if (!strcmp(path, "/dev/audioin")) {
        type = SHADPS4_HLE_FD_AUDIO_IN;
    } else if (!strcmp(path, "/dev/dialog")) {
        type = SHADPS4_HLE_FD_DIALOG;
    } else if (shadps4_hle_storage_path(hle, path, scoped_path,
                                        sizeof(scoped_path),
                                        &storage_read_only)) {
        int ret;

        if (storage_read_only &&
            (((flags & SHADPS4_O_ACCMODE) == SHADPS4_O_WRONLY) ||
             ((flags & SHADPS4_O_ACCMODE) == SHADPS4_O_RDWR) ||
             (flags & (SHADPS4_O_APPEND | SHADPS4_O_CREAT |
                       SHADPS4_O_TRUNC)))) {
            return -SHADPS4_GUEST_EROFS;
        }
        ret = qemu_host_storage_open(
            scoped_path,
            flags | ((flags & 0x20000) ? QEMU_HOST_STORAGE_OPEN_DIRECTORY : 0),
            mode,
                                     &storage_handle);

        if (ret < 0) {
            return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
        }
        type = SHADPS4_HLE_FD_STORAGE;
    } else {
        return -SHADPS4_GUEST_EBADF;
    }
    for (fd = 3; fd < SHADPS4_HLE_MAX_FDS; fd++) {
        if (hle->files[fd] == SHADPS4_HLE_FD_FREE) {
            hle->files[fd] = type;
            hle->file_units[fd] = unit;
            hle->storage_handles[fd] = storage_handle;
            hle->storage_read_only[fd] = storage_read_only;
            pstrcpy(hle->storage_paths[fd], sizeof(hle->storage_paths[fd]),
                    scoped_path);
            return fd;
        }
    }
    if (type == SHADPS4_HLE_FD_STORAGE) {
        qemu_host_storage_close(storage_handle);
    }
    return -SHADPS4_GUEST_ENOMEM;
}

static uint64_t shadps4_hle_close(ShadPS4HLEState *hle, uint64_t fd)
{
    if (fd >= 0x100 && fd < 0x100 + SHADPS4_HLE_MAX_EQUEUES &&
        hle->equeues[fd - 0x100]) {
        hle->equeues[fd - 0x100] = false;
        return 0;
    }
    if (fd < 3 || fd >= SHADPS4_HLE_MAX_FDS ||
        hle->files[fd] == SHADPS4_HLE_FD_FREE) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (hle->files[fd] == SHADPS4_HLE_FD_STORAGE &&
        qemu_host_storage_close(hle->storage_handles[fd]) < 0) {
        return -SHADPS4_GUEST_EIO;
    }
    if (hle->files[fd] == SHADPS4_HLE_FD_NETWORK &&
        qemu_host_network_close(hle->network_handles[fd]) < 0) {
        return -SHADPS4_GUEST_EIO;
    }
    hle->files[fd] = SHADPS4_HLE_FD_FREE;
    hle->file_units[fd] = 0;
    hle->storage_handles[fd] = -1;
    hle->network_handles[fd] = -1;
    hle->storage_read_only[fd] = false;
    hle->storage_paths[fd][0] = 0;
    return 0;
}

static void shadps4_hle_pad_legacy_to_guest(ShadPS4PadState *pad)
{
    pad->buttons = cpu_to_le32(pad->buttons);
    pad->left_x = cpu_to_le16(pad->left_x);
    pad->left_y = cpu_to_le16(pad->left_y);
    pad->right_x = cpu_to_le16(pad->right_x);
    pad->right_y = cpu_to_le16(pad->right_y);
    pad->pointer_x = cpu_to_le32(pad->pointer_x);
    pad->pointer_y = cpu_to_le32(pad->pointer_y);
    pad->reserved = 0;
    pad->sequence = cpu_to_le64(pad->sequence);
    pad->rumble_small = cpu_to_le16(pad->rumble_small);
    pad->rumble_large = cpu_to_le16(pad->rumble_large);
    pad->reserved2 = 0;
}

static int16_t shadps4_hle_motion_value(float value, float scale)
{
    return cpu_to_le16((int16_t)lrintf(CLAMP(value * scale,
                                             -32768.0f, 32767.0f)));
}

static void shadps4_hle_pad_to_guest(const ShadPS4PadDeviceState *pad,
                                     ShadPS4GuestPadState *guest)
{
    int i;

    memset(guest, 0, sizeof(*guest));
    guest->legacy = pad->legacy;
    shadps4_hle_pad_legacy_to_guest(&guest->legacy);
    guest->left_trigger = pad->left_trigger;
    guest->right_trigger = pad->right_trigger;
    guest->connected = pad->connected;
    for (i = 0; i < QEMU_HOST_MAX_TOUCHES; i++) {
        guest->touches[i].id = cpu_to_le32(pad->touch.points[i].id);
        guest->touches[i].x = cpu_to_le16(lrintf(
            pad->touch.points[i].x * UINT16_MAX));
        guest->touches[i].y = cpu_to_le16(lrintf(
            pad->touch.points[i].y * UINT16_MAX));
        guest->touches[i].active = pad->touch.points[i].active;
    }
    for (i = 0; i < 3; i++) {
        guest->acceleration[i] = shadps4_hle_motion_value(
            pad->motion.acceleration[i], 8192.0f);
        guest->angular_velocity[i] = shadps4_hle_motion_value(
            pad->motion.angular_velocity[i], 1024.0f);
    }
    for (i = 0; i < 4; i++) {
        guest->orientation[i] = shadps4_hle_motion_value(
            pad->motion.orientation[i], 32767.0f);
    }
    guest->output = pad->output;
    guest->output.rumble_small = cpu_to_le16(pad->output.rumble_small);
    guest->output.rumble_large = cpu_to_le16(pad->output.rumble_large);
    guest->timestamp_ns = cpu_to_le64(pad->timestamp_ns);
}

static uint64_t shadps4_hle_read(ShadPS4HLEState *hle, CPUState *cs,
                                 uint64_t fd, uint64_t addr, uint64_t size)
{
    uint8_t zeros[256] = { 0 };
    uint64_t done = 0;

    if (size > 16 * MiB) {
        return -SHADPS4_GUEST_EINVAL;
    }
    if (size && addr > UINT64_MAX - size) {
        return -SHADPS4_GUEST_EFAULT;
    }
    if (fd >= SHADPS4_HLE_MAX_FDS ||
        hle->files[fd] == SHADPS4_HLE_FD_FREE) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (hle->files[fd] == SHADPS4_HLE_FD_PAD) {
        ShadPS4PadDeviceState pad;
        ShadPS4GuestPadState guest;

        if (size < sizeof(pad.legacy) ||
            !shadps4_io_get_pad(hle->io, hle->file_units[fd], &pad)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        shadps4_hle_pad_to_guest(&pad, &guest);
        if (size >= sizeof(guest)) {
            return shadps4_guest_rw(cs, addr, &guest, sizeof(guest), true) ?
                   sizeof(guest) : -SHADPS4_GUEST_EFAULT;
        }
        return shadps4_guest_rw(cs, addr, &guest.legacy,
                                sizeof(guest.legacy), true) ?
               sizeof(guest.legacy) : -SHADPS4_GUEST_EFAULT;
    }
    if (hle->files[fd] == SHADPS4_HLE_FD_AUDIO_IN) {
        g_autofree uint8_t *samples = g_malloc(size);
        size_t copied = shadps4_io_read_audio_input(hle->io, samples, size);

        return !copied || shadps4_guest_rw(cs, addr, samples, copied, true) ?
               copied : -SHADPS4_GUEST_EFAULT;
    }
    if (hle->files[fd] == SHADPS4_HLE_FD_DIALOG) {
        g_autofree uint8_t *response = NULL;
        ShadPS4GuestDialogResponse *header;
        uint64_t request_id;
        size_t payload_size;
        int status;
        int ret;

        if (size < sizeof(*header) || size > 64 * KiB) {
            return -SHADPS4_GUEST_EINVAL;
        }
        response = g_malloc0(size);
        header = (ShadPS4GuestDialogResponse *)response;
        payload_size = size - sizeof(*header);
        ret = qemu_host_dialog_take_response(
            &request_id, &status, response + sizeof(*header), &payload_size);
        if (ret < 0) {
            return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
        }
        header->request_id = cpu_to_le64(request_id);
        header->status = cpu_to_le32(status);
        header->payload_size = cpu_to_le32(payload_size);
        size = sizeof(*header) + payload_size;
        return shadps4_guest_rw(cs, addr, response, size, true) ? size :
               -SHADPS4_GUEST_EFAULT;
    }
    if (hle->files[fd] == SHADPS4_HLE_FD_NETWORK) {
        g_autofree uint8_t *buffer = g_malloc(size);
        int64_t read = qemu_host_network_recv(hle->network_handles[fd],
                                              buffer, size, 0);

        if (read < 0) {
            return read >= -255 ? read : -SHADPS4_GUEST_EIO;
        }
        if ((uint64_t)read > size) {
            return -SHADPS4_GUEST_EIO;
        }
        return !read || shadps4_guest_rw(cs, addr, buffer, read, true) ?
               read : -SHADPS4_GUEST_EFAULT;
    }
    if (hle->files[fd] == SHADPS4_HLE_FD_STORAGE) {
        g_autofree uint8_t *buffer = g_malloc(size);
        int64_t read = qemu_host_storage_read(hle->storage_handles[fd],
                                              buffer, size);

        if (read < 0) {
            return read >= -255 ? read : -SHADPS4_GUEST_EIO;
        }
        if ((uint64_t)read > size) {
            return -SHADPS4_GUEST_EIO;
        }
        return !read || shadps4_guest_rw(cs, addr, buffer, read, true) ?
               read : -SHADPS4_GUEST_EFAULT;
    }
    if (hle->files[fd] != SHADPS4_HLE_FD_ZERO) {
        return 0;
    }
    while (done < size) {
        size_t chunk = MIN(size - done, sizeof(zeros));

        if (!shadps4_guest_rw(cs, addr + done, zeros, chunk, true)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        done += chunk;
    }
    return size;
}

static uint64_t shadps4_hle_write(ShadPS4HLEState *hle, CPUState *cs,
                                  uint64_t fd, uint64_t addr, uint64_t size)
{
    char buffer[257];
    size_t chunk;

    if (size > 16 * MiB) {
        return -SHADPS4_GUEST_EINVAL;
    }
    if (size && addr > UINT64_MAX - size) {
        return -SHADPS4_GUEST_EFAULT;
    }
    if (fd >= SHADPS4_HLE_MAX_FDS ||
        hle->files[fd] == SHADPS4_HLE_FD_FREE) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (hle->files[fd] != SHADPS4_HLE_FD_CONSOLE) {
        if (hle->files[fd] == SHADPS4_HLE_FD_AUDIO) {
            return shadps4_io_emit_audio(hle->io, cs, addr, size) ?
                   size : -SHADPS4_GUEST_EINVAL;
        }
        if (hle->files[fd] == SHADPS4_HLE_FD_STORAGE) {
            g_autofree uint8_t *storage_buffer = g_malloc(size);
            int64_t written;

            if (hle->storage_read_only[fd]) {
                return -SHADPS4_GUEST_EROFS;
            }
            if (!shadps4_guest_rw(cs, addr, storage_buffer, size, false)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            written = qemu_host_storage_write(hle->storage_handles[fd],
                                               storage_buffer, size);
            if (written < 0) {
                return written >= -255 ? written : -SHADPS4_GUEST_EIO;
            }
            return (uint64_t)written <= size ? written :
                   -SHADPS4_GUEST_EIO;
        }
        if (hle->files[fd] == SHADPS4_HLE_FD_DIALOG) {
            g_autofree uint8_t *payload = g_malloc(size);
            uint64_t request_id;
            int ret;

            if (!size || size > 64 * KiB) {
                return -SHADPS4_GUEST_EINVAL;
            }
            if (!shadps4_guest_rw(cs, addr, payload, size, false)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            ret = qemu_host_dialog_request(1, payload, size, &request_id);
            return ret < 0 ? (ret >= -255 ? ret : -SHADPS4_GUEST_EIO) :
                   size;
        }
        if (hle->files[fd] == SHADPS4_HLE_FD_NETWORK) {
            g_autofree uint8_t *network_buffer = g_malloc(size);
            int64_t written;

            if (!shadps4_guest_rw(cs, addr, network_buffer, size, false)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            written = qemu_host_network_send(hle->network_handles[fd],
                                              network_buffer, size, 0);
            if (written < 0) {
                return written >= -255 ? written : -SHADPS4_GUEST_EIO;
            }
            return (uint64_t)written <= size ? written :
                   -SHADPS4_GUEST_EIO;
        }
        return size;
    }
    for (uint64_t done = 0; done < size; done += chunk) {
        chunk = MIN(size - done, sizeof(buffer) - 1);
        if (!shadps4_guest_rw(cs, addr + done, buffer, chunk, false)) {
            return done ? done : -SHADPS4_GUEST_EFAULT;
        }
        buffer[chunk] = 0;
        qemu_log_mask(LOG_GUEST_ERROR, "shadPS4 guest: %s", buffer);
        qemu_host_emit_log(fd == 2 ? QEMU_HOST_LOG_WARNING :
                           QEMU_HOST_LOG_INFO, buffer);
    }
    return size;
}

static uint64_t shadps4_hle_positional_io(ShadPS4HLEState *hle,
                                          CPUState *cs, uint64_t fd,
                                          uint64_t addr, uint64_t size,
                                          int64_t offset, bool write)
{
    int64_t saved;
    uint64_t result;

    if (fd >= SHADPS4_HLE_MAX_FDS ||
        hle->files[fd] != SHADPS4_HLE_FD_STORAGE || offset < 0) {
        return -SHADPS4_GUEST_EBADF;
    }
    saved = qemu_host_storage_seek(hle->storage_handles[fd], 0, SEEK_CUR);
    if (saved < 0 || qemu_host_storage_seek(
            hle->storage_handles[fd], offset, SEEK_SET) < 0) {
        return -SHADPS4_GUEST_EIO;
    }
    result = write ? shadps4_hle_write(hle, cs, fd, addr, size) :
                     shadps4_hle_read(hle, cs, fd, addr, size);
    if (qemu_host_storage_seek(hle->storage_handles[fd], saved,
                               SEEK_SET) < 0 && (int64_t)result >= 0) {
        return -SHADPS4_GUEST_EIO;
    }
    return result;
}

static uint64_t shadps4_hle_vector_io(ShadPS4HLEState *hle, CPUState *cs,
                                      uint64_t fd, uint64_t iov_addr,
                                      uint64_t iov_count, bool write,
                                      bool positional, int64_t offset)
{
    uint64_t total = 0;
    uint64_t i;

    if (!iov_addr || !iov_count || iov_count > 1024 ||
        iov_addr > UINT64_MAX - iov_count * sizeof(ShadPS4GuestIovec) ||
        (positional && (offset < 0 ||
                        (uint64_t)offset > INT64_MAX - 64 * MiB))) {
        return -SHADPS4_GUEST_EINVAL;
    }
    for (i = 0; i < iov_count; i++) {
        ShadPS4GuestIovec guest;
        uint64_t base;
        uint64_t length;
        uint64_t result;

        if (!shadps4_guest_rw(cs, iov_addr + i * sizeof(guest), &guest,
                              sizeof(guest), false)) {
            return total ? total : -SHADPS4_GUEST_EFAULT;
        }
        base = le64_to_cpu(guest.base);
        length = le64_to_cpu(guest.length);
        if (length > 16 * MiB || total > 64 * MiB - length) {
            return total ? total : -SHADPS4_GUEST_EINVAL;
        }
        if (!length) {
            continue;
        }
        result = positional ? shadps4_hle_positional_io(
                                  hle, cs, fd, base, length, offset + total,
                                  write) :
                              (write ? shadps4_hle_write(
                                           hle, cs, fd, base, length) :
                                       shadps4_hle_read(
                                           hle, cs, fd, base, length));
        if ((int64_t)result < 0) {
            return total ? total : result;
        }
        total += result;
        if (result != length) {
            break;
        }
    }
    return total;
}

static uint64_t shadps4_hle_aio_submit(ShadPS4HLEState *hle, CPUState *cs,
                                       uint64_t requests, uint64_t count,
                                       uint64_t ids, bool write,
                                       bool multiple)
{
    uint32_t common_id;
    uint64_t i;

    if (!requests || !ids || !count || count > SHADPS4_HLE_MAX_AIO_REQUESTS ||
        requests > UINT64_MAX - count * sizeof(ShadPS4GuestAioRequest) ||
        (multiple && ids > UINT64_MAX - count * sizeof(uint32_t))) {
        return SHADPS4_KERNEL_ERROR_EFAULT;
    }
    common_id = hle->aio_next_id++;
    if (!common_id || common_id >= SHADPS4_HLE_MAX_AIO_REQUESTS) {
        common_id = hle->aio_next_id = 1;
        hle->aio_next_id++;
    }
    hle->aio_states[common_id] = 2;
    for (i = 0; i < count; i++) {
        ShadPS4GuestAioRequest request;
        ShadPS4GuestAioResult result = { 0 };
        uint32_t id = common_id;
        uint64_t transferred;

        if (!shadps4_guest_rw(cs, requests + i * sizeof(request), &request,
                              sizeof(request), false)) {
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
        if (multiple) {
            id = hle->aio_next_id++;
            if (!id || id >= SHADPS4_HLE_MAX_AIO_REQUESTS) {
                id = hle->aio_next_id = 1;
                hle->aio_next_id++;
            }
            hle->aio_states[id] = 2;
        }
        transferred = shadps4_hle_positional_io(
            hle, cs, le32_to_cpu(request.fd), le64_to_cpu(request.buffer),
            le64_to_cpu(request.size), le64_to_cpu(request.offset), write);
        result.value = cpu_to_le64(transferred);
        result.state = cpu_to_le32((int64_t)transferred < 0 ? 4 : 3);
        hle->aio_states[id] = le32_to_cpu(result.state);
        if (!request.result || !shadps4_guest_rw(
                cs, le64_to_cpu(request.result), &result,
                sizeof(result), true)) {
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
        id = cpu_to_le32(id);
        if (multiple && !shadps4_guest_rw(
                cs, ids + i * sizeof(id), &id, sizeof(id), true)) {
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
    }
    if (!multiple) {
        common_id = cpu_to_le32(common_id);
        if (!shadps4_guest_rw(cs, ids, &common_id,
                              sizeof(common_id), true)) {
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
        hle->aio_states[le32_to_cpu(common_id)] = 3;
    }
    return 0;
}

static uint64_t shadps4_hle_aio_states(ShadPS4HLEState *hle, CPUState *cs,
                                       uint64_t ids, uint64_t count,
                                       uint64_t states, bool many,
                                       uint32_t replacement)
{
    uint64_t i;

    if (!states || !count || count > SHADPS4_HLE_MAX_AIO_REQUESTS ||
        states > UINT64_MAX - count * sizeof(uint32_t) ||
        (many && (!ids || ids > UINT64_MAX - count * sizeof(uint32_t)))) {
        return SHADPS4_KERNEL_ERROR_EFAULT;
    }
    for (i = 0; i < count; i++) {
        uint32_t id;
        uint32_t state;

        if (many) {
            if (!ids || !shadps4_guest_rw(cs, ids + i * sizeof(id), &id,
                                          sizeof(id), false)) {
                return SHADPS4_KERNEL_ERROR_EFAULT;
            }
            id = le32_to_cpu(id);
        } else {
            id = ids;
        }
        if (id >= SHADPS4_HLE_MAX_AIO_REQUESTS) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        if (replacement) {
            hle->aio_states[id] = replacement;
        }
        state = cpu_to_le32(hle->aio_states[id]);
        if (!shadps4_guest_rw(cs, states + i * sizeof(state), &state,
                              sizeof(state), true)) {
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
    }
    return 0;
}

static uint64_t shadps4_hle_mmap(ShadPS4HLEState *hle, CPUState *cs,
                                 uint64_t addr, uint64_t size, uint64_t prot)
{
    uint32_t needed;
    uint32_t start;
    uint32_t i;
    uint64_t start64;

    if (!size || size > (uint64_t)hle->dynamic_slot_count * 2 * MiB) {
        return -SHADPS4_GUEST_EINVAL;
    }
    needed = DIV_ROUND_UP(size, 2 * MiB);
    if (addr) {
        if (addr < hle->dynamic_virt_base ||
            (addr - hle->dynamic_virt_base) % (2 * MiB)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        start64 = (addr - hle->dynamic_virt_base) / (2 * MiB);
        if (start64 > hle->dynamic_slot_count ||
            needed > hle->dynamic_slot_count - start64) {
            return -SHADPS4_GUEST_ENOMEM;
        }
        start = start64;
    } else {
        for (start = 0; start + needed <= hle->dynamic_slot_count; start++) {
            for (i = 0; i < needed && !hle->dynamic_slots[start + i]; i++) {
                /* Search for a contiguous free run. */
            }
            if (i == needed) {
                break;
            }
            start += i;
        }
        if (start + needed > hle->dynamic_slot_count) {
            return -SHADPS4_GUEST_ENOMEM;
        }
    }
    for (i = 0; i < needed; i++) {
        if (hle->dynamic_slots[start + i]) {
            return -SHADPS4_GUEST_EINVAL;
        }
    }
    for (i = 0; i < needed; i++) {
        uint32_t slot = start + i;
        uint64_t entry;

        entry = cpu_to_le64((hle->dynamic_phys_base + slot * 2 * MiB) |
                            SHADPS4_PAGE_PRESENT | SHADPS4_PAGE_USER |
                            SHADPS4_PAGE_LARGE |
                            ((prot & 2) ? SHADPS4_PAGE_WRITE : 0) |
                            ((prot & 4) ? 0 : SHADPS4_PAGE_NX));
        if (address_space_write(hle->as,
                                hle->dynamic_pd_phys + slot * sizeof(entry),
                                MEMTXATTRS_UNSPECIFIED, &entry,
                                sizeof(entry)) != MEMTX_OK ||
            address_space_set(hle->as,
                              hle->dynamic_phys_base + slot * 2 * MiB,
                              0, 2 * MiB,
                              MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            uint32_t rollback;

            for (rollback = 0; rollback <= i; rollback++) {
                uint64_t zero = 0;

                address_space_write(
                    hle->as,
                    hle->dynamic_pd_phys +
                    (start + rollback) * sizeof(zero),
                    MEMTXATTRS_UNSPECIFIED, &zero, sizeof(zero));
                hle->dynamic_slots[start + rollback] = false;
            }
            tlb_flush(cs);
            return -SHADPS4_GUEST_EFAULT;
        }
        hle->dynamic_slots[slot] = true;
    }
    tlb_flush(cs);
    return hle->dynamic_virt_base + start * 2 * MiB;
}

static ShadPS4HLEHeapAllocation *
shadps4_hle_heap_find(ShadPS4HLEState *hle, uint64_t address)
{
    uint32_t i;

    for (i = 0; i < SHADPS4_HLE_MAX_HEAP_ALLOCS; i++) {
        if (hle->heap_allocs[i].active &&
            hle->heap_allocs[i].address == address) {
            return &hle->heap_allocs[i];
        }
    }
    return NULL;
}

static bool shadps4_hle_heap_zero(CPUState *cs, uint64_t address,
                                  uint64_t size)
{
    uint8_t zeros[4096] = { 0 };

    while (size) {
        size_t chunk = MIN(size, sizeof(zeros));

        if (!shadps4_guest_rw(cs, address, zeros, chunk, true)) {
            return false;
        }
        address += chunk;
        size -= chunk;
    }
    return true;
}

static bool shadps4_hle_heap_copy(CPUState *cs, uint64_t dest,
                                  uint64_t source, uint64_t size)
{
    uint8_t buffer[4096];

    while (size) {
        size_t chunk = MIN(size, sizeof(buffer));

        if (!shadps4_guest_rw(cs, source, buffer, chunk, false) ||
            !shadps4_guest_rw(cs, dest, buffer, chunk, true)) {
            return false;
        }
        dest += chunk;
        source += chunk;
        size -= chunk;
    }
    return true;
}

static uint64_t shadps4_hle_heap_alloc(ShadPS4HLEState *hle, CPUState *cs,
                                       uint64_t size, uint64_t alignment)
{
    ShadPS4HLEHeapAllocation *record = NULL;
    uint64_t aligned;
    uint32_t i;

    size = MAX(size, 1);
    alignment = MAX(alignment, sizeof(uint64_t));
    if (!is_power_of_2(alignment) || size > UINT64_MAX - alignment) {
        return 0;
    }
    for (i = 0; i < SHADPS4_HLE_MAX_HEAP_ALLOCS; i++) {
        ShadPS4HLEHeapAllocation *candidate = &hle->heap_allocs[i];

        if (!candidate->active && candidate->address &&
            candidate->size >= size &&
            !(candidate->address & (alignment - 1))) {
            candidate->active = true;
            return candidate->address;
        }
        if (!record && !candidate->active && !candidate->address) {
            record = candidate;
        }
    }
    if (!record) {
        return 0;
    }
    aligned = QEMU_ALIGN_UP(hle->heap_cursor, alignment);
    if (!hle->heap_cursor || aligned < hle->heap_cursor ||
        aligned > hle->heap_end ||
        size > hle->heap_end - aligned) {
        uint64_t required = size + alignment;
        uint64_t chunk;
        uint64_t mapped;

        if (required > UINT64_MAX - (2 * MiB - 1)) {
            return 0;
        }
        chunk = MAX(64 * MiB, QEMU_ALIGN_UP(required, 2 * MiB));
        if (chunk < size || (int64_t)(mapped = shadps4_hle_mmap(
                hle, cs, 0, chunk, 3)) < 0) {
            return 0;
        }
        hle->heap_cursor = mapped;
        hle->heap_end = mapped + chunk;
        aligned = QEMU_ALIGN_UP(mapped, alignment);
    }
    if (aligned > hle->heap_end || size > hle->heap_end - aligned) {
        return 0;
    }
    record->address = aligned;
    record->size = size;
    record->active = true;
    hle->heap_cursor = aligned + size;
    return aligned;
}

static uint64_t shadps4_hle_munmap(ShadPS4HLEState *hle, CPUState *cs,
                                   uint64_t addr, uint64_t size)
{
    g_autofree uint64_t *old_entries = NULL;
    uint64_t zero = 0;
    uint32_t start;
    uint32_t needed;
    uint32_t i;
    uint64_t start64;

    if (!size || addr < hle->dynamic_virt_base ||
        (addr - hle->dynamic_virt_base) % (2 * MiB) ||
        size > (uint64_t)hle->dynamic_slot_count * 2 * MiB) {
        return -SHADPS4_GUEST_EINVAL;
    }
    start64 = (addr - hle->dynamic_virt_base) / (2 * MiB);
    needed = DIV_ROUND_UP(size, 2 * MiB);
    if (start64 > hle->dynamic_slot_count ||
        needed > hle->dynamic_slot_count - start64) {
        return -SHADPS4_GUEST_EINVAL;
    }
    start = start64;
    for (i = 0; i < needed; i++) {
        if (!hle->dynamic_slots[start + i]) {
            return -SHADPS4_GUEST_EINVAL;
        }
    }
    old_entries = g_new(uint64_t, needed);
    for (i = 0; i < needed; i++) {
        if (address_space_read(hle->as,
                               hle->dynamic_pd_phys +
                               (start + i) * sizeof(old_entries[i]),
                               MEMTXATTRS_UNSPECIFIED, &old_entries[i],
                               sizeof(old_entries[i])) != MEMTX_OK) {
            return -SHADPS4_GUEST_EFAULT;
        }
    }
    for (i = 0; i < needed; i++) {
        if (address_space_write(hle->as,
                                hle->dynamic_pd_phys +
                                (start + i) * sizeof(zero),
                                MEMTXATTRS_UNSPECIFIED, &zero,
                                sizeof(zero)) != MEMTX_OK) {
            uint32_t rollback;

            for (rollback = 0; rollback < i; rollback++) {
                address_space_write(
                    hle->as, hle->dynamic_pd_phys +
                    (start + rollback) * sizeof(old_entries[rollback]),
                    MEMTXATTRS_UNSPECIFIED, &old_entries[rollback],
                    sizeof(old_entries[rollback]));
            }
            tlb_flush(cs);
            return -SHADPS4_GUEST_EFAULT;
        }
    }
    for (i = 0; i < needed; i++) {
        hle->dynamic_slots[start + i] = false;
    }
    tlb_flush(cs);
    return 0;
}

static uint64_t shadps4_hle_mprotect(ShadPS4HLEState *hle, CPUState *cs,
                                     uint64_t addr, uint64_t size,
                                     uint64_t prot)
{
    g_autofree uint64_t *old_entries = NULL;
    g_autofree uint64_t *new_entries = NULL;
    uint32_t start;
    uint32_t count;
    uint32_t i;
    uint64_t start64;

    if (!size || addr < hle->dynamic_virt_base ||
        (addr - hle->dynamic_virt_base) % (2 * MiB)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    start64 = (addr - hle->dynamic_virt_base) / (2 * MiB);
    count = DIV_ROUND_UP(size, 2 * MiB);
    if (start64 >= hle->dynamic_slot_count ||
        count > hle->dynamic_slot_count - start64) {
        return -SHADPS4_GUEST_EINVAL;
    }
    start = start64;
    old_entries = g_new(uint64_t, count);
    new_entries = g_new(uint64_t, count);
    for (i = 0; i < count; i++) {
        uint64_t entry;

        if (!hle->dynamic_slots[start + i] ||
            address_space_read(hle->as, hle->dynamic_pd_phys +
                               (start + i) * sizeof(old_entries[i]),
                               MEMTXATTRS_UNSPECIFIED, &old_entries[i],
                               sizeof(old_entries[i])) != MEMTX_OK) {
            return -SHADPS4_GUEST_EFAULT;
        }
        entry = le64_to_cpu(old_entries[i]);
        entry = (entry & ~(SHADPS4_PAGE_WRITE | SHADPS4_PAGE_NX)) |
                ((prot & 2) ? SHADPS4_PAGE_WRITE : 0) |
                ((prot & 4) ? 0 : SHADPS4_PAGE_NX);
        new_entries[i] = cpu_to_le64(entry);
    }
    for (i = 0; i < count; i++) {
        if (address_space_write(hle->as, hle->dynamic_pd_phys +
                                (start + i) * sizeof(new_entries[i]),
                                MEMTXATTRS_UNSPECIFIED, &new_entries[i],
                                sizeof(new_entries[i])) != MEMTX_OK) {
            uint32_t rollback;

            for (rollback = 0; rollback < i; rollback++) {
                address_space_write(
                    hle->as, hle->dynamic_pd_phys +
                    (start + rollback) * sizeof(old_entries[rollback]),
                    MEMTXATTRS_UNSPECIFIED, &old_entries[rollback],
                    sizeof(old_entries[rollback]));
            }
            tlb_flush(cs);
            return -SHADPS4_GUEST_EFAULT;
        }
    }
    tlb_flush(cs);
    return 0;
}

static uint64_t shadps4_hle_map_to_pointer(ShadPS4HLEState *hle,
                                           CPUState *cs,
                                           uint64_t address_pointer,
                                           uint64_t size, uint64_t prot)
{
    uint64_t requested = 0;
    uint64_t mapped;
    uint64_t mapped_le;

    if (!address_pointer || !shadps4_guest_rw(
            cs, address_pointer, &requested, sizeof(requested), false)) {
        return SHADPS4_KERNEL_ERROR_EFAULT;
    }
    requested = le64_to_cpu(requested);
    mapped = shadps4_hle_mmap(hle, cs, requested, size, prot);
    if ((int64_t)mapped < 0) {
        return SHADPS4_KERNEL_ERROR_ENOMEM;
    }
    mapped_le = cpu_to_le64(mapped);
    if (!shadps4_guest_rw(cs, address_pointer, &mapped_le,
                          sizeof(mapped_le), true)) {
        shadps4_hle_munmap(hle, cs, mapped, size);
        return SHADPS4_KERNEL_ERROR_EFAULT;
    }
    return 0;
}

static uint64_t shadps4_hle_allocate_direct(ShadPS4HLEState *hle,
                                            CPUState *cs, uint64_t start,
                                            uint64_t end, uint64_t size,
                                            uint64_t alignment,
                                            uint64_t output)
{
    uint64_t limit = (uint64_t)hle->dynamic_slot_count * 2 * MiB;
    uint64_t address;
    uint64_t address_le;

    if (!output || !size || (size & (16 * KiB - 1)) || end <= start ||
        size > end - start || (alignment &&
        (alignment & (alignment - 1)))) {
        return SHADPS4_KERNEL_ERROR_EINVAL;
    }
    alignment = MAX(alignment, 16 * KiB);
    address = MAX(hle->direct_memory_next, start);
    if (address > UINT64_MAX - (alignment - 1)) {
        return SHADPS4_KERNEL_ERROR_ENOMEM;
    }
    address = (address + alignment - 1) & ~(alignment - 1);
    if (address > MIN(end, limit) || size > MIN(end, limit) - address) {
        return SHADPS4_KERNEL_ERROR_ENOMEM;
    }
    address_le = cpu_to_le64(address);
    if (!shadps4_guest_rw(cs, output, &address_le,
                          sizeof(address_le), true)) {
        return SHADPS4_KERNEL_ERROR_EFAULT;
    }
    hle->direct_memory_next = address + size;
    return 0;
}

static uint64_t shadps4_hle_time(CPUState *cs, uint64_t addr,
                                 QEMUClockType clock, bool timeval)
{
    int64_t now = qemu_clock_get_ns(clock);
    ShadPS4GuestTime value = {
        .seconds = cpu_to_le64(now / NANOSECONDS_PER_SECOND),
        .fraction = cpu_to_le64((now % NANOSECONDS_PER_SECOND) /
                                (timeval ? 1000 : 1)),
    };

    if (!shadps4_guest_rw(cs, addr, &value, sizeof(value), true)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    return 0;
}

typedef struct ShadPS4RtcDateTime {
    uint16_t year;
    uint16_t month;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
    uint32_t microsecond;
} ShadPS4RtcDateTime;

static bool shadps4_rtc_is_leap(int64_t year)
{
    return !(year % 4) && ((year % 100) || !(year % 400));
}

static int shadps4_rtc_days_in_month(int64_t year, int64_t month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };

    return days[month - 1] + (month == 2 && shadps4_rtc_is_leap(year));
}

static uint32_t shadps4_rtc_validate(const ShadPS4RtcDateTime *date)
{
    if (!date->year || date->year > 9999) {
        return SHADPS4_RTC_ERROR_INVALID_YEAR;
    }
    if (!date->month || date->month > 12) {
        return SHADPS4_RTC_ERROR_INVALID_MONTH;
    }
    if (!date->day ||
        date->day > shadps4_rtc_days_in_month(date->year, date->month)) {
        return SHADPS4_RTC_ERROR_INVALID_DAY;
    }
    if (date->hour > 23) {
        return SHADPS4_RTC_ERROR_INVALID_HOUR;
    }
    if (date->minute > 59) {
        return SHADPS4_RTC_ERROR_INVALID_MINUTE;
    }
    if (date->second > 59) {
        return SHADPS4_RTC_ERROR_INVALID_SECOND;
    }
    if (date->microsecond > 999999) {
        return SHADPS4_RTC_ERROR_INVALID_MICROSECOND;
    }
    return 0;
}

/* Number of days relative to 1970-01-01 in the proleptic Gregorian calendar. */
static int64_t shadps4_rtc_days_from_civil(int64_t year, unsigned month,
                                           unsigned day)
{
    int64_t era;
    unsigned year_of_era;
    unsigned day_of_year;
    unsigned day_of_era;

    year -= month <= 2;
    era = (year >= 0 ? year : year - 399) / 400;
    year_of_era = year - era * 400;
    day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 +
                  day - 1;
    day_of_era = year_of_era * 365 + year_of_era / 4 -
                 year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

static void shadps4_rtc_civil_from_days(int64_t days, int64_t *year,
                                        unsigned *month, unsigned *day)
{
    int64_t era;
    unsigned day_of_era;
    unsigned year_of_era;
    unsigned day_of_year;
    unsigned month_prime;

    days += 719468;
    era = (days >= 0 ? days : days - 146096) / 146097;
    day_of_era = days - era * 146097;
    year_of_era = (day_of_era - day_of_era / 1460 +
                   day_of_era / 36524 - day_of_era / 146096) / 365;
    *year = year_of_era + era * 400;
    day_of_year = day_of_era -
                  (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    month_prime = (5 * day_of_year + 2) / 153;
    *day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    *month = month_prime + (month_prime < 10 ? 3 : -9);
    *year += *month <= 2;
}

static bool shadps4_rtc_read_date(CPUState *cs, uint64_t address,
                                  ShadPS4RtcDateTime *date)
{
    ShadPS4RtcDateTime guest;

    if (!address || !shadps4_guest_rw(cs, address, &guest,
                                      sizeof(guest), false)) {
        return false;
    }
    date->year = le16_to_cpu(guest.year);
    date->month = le16_to_cpu(guest.month);
    date->day = le16_to_cpu(guest.day);
    date->hour = le16_to_cpu(guest.hour);
    date->minute = le16_to_cpu(guest.minute);
    date->second = le16_to_cpu(guest.second);
    date->microsecond = le32_to_cpu(guest.microsecond);
    return true;
}

static bool shadps4_rtc_write_date(CPUState *cs, uint64_t address,
                                   const ShadPS4RtcDateTime *date)
{
    ShadPS4RtcDateTime guest = {
        .year = cpu_to_le16(date->year),
        .month = cpu_to_le16(date->month),
        .day = cpu_to_le16(date->day),
        .hour = cpu_to_le16(date->hour),
        .minute = cpu_to_le16(date->minute),
        .second = cpu_to_le16(date->second),
        .microsecond = cpu_to_le32(date->microsecond),
    };

    return address && shadps4_guest_rw(cs, address, &guest,
                                       sizeof(guest), true);
}

static bool shadps4_rtc_read_tick(CPUState *cs, uint64_t address,
                                  uint64_t *tick)
{
    uint64_t guest;

    if (!address || !shadps4_guest_rw(cs, address, &guest,
                                      sizeof(guest), false)) {
        return false;
    }
    *tick = le64_to_cpu(guest);
    return true;
}

static bool shadps4_rtc_write_tick(CPUState *cs, uint64_t address,
                                   uint64_t tick)
{
    tick = cpu_to_le64(tick);
    return address && shadps4_guest_rw(cs, address, &tick,
                                       sizeof(tick), true);
}

static uint32_t shadps4_rtc_date_to_tick(const ShadPS4RtcDateTime *date,
                                         uint64_t *tick)
{
    uint32_t error = shadps4_rtc_validate(date);
    uint64_t days;

    if (error) {
        return error;
    }
    days = shadps4_rtc_days_from_civil(date->year, date->month, date->day) +
           719162;
    *tick = days * SHADPS4_RTC_DAY_TICKS +
            (uint64_t)date->hour * 3600000000ULL +
            (uint64_t)date->minute * 60000000ULL +
            (uint64_t)date->second * 1000000ULL + date->microsecond;
    return 0;
}

static uint32_t shadps4_rtc_tick_to_date(uint64_t tick,
                                         ShadPS4RtcDateTime *date)
{
    uint64_t fraction;
    int64_t year;
    unsigned month;
    unsigned day;

    if (tick > SHADPS4_RTC_MAX_TICKS) {
        return SHADPS4_RTC_ERROR_INVALID_VALUE;
    }
    shadps4_rtc_civil_from_days(
                                (int64_t)(tick / SHADPS4_RTC_DAY_TICKS) -
                                719162,
                                &year, &month, &day);
    fraction = tick % SHADPS4_RTC_DAY_TICKS;
    date->year = year;
    date->month = month;
    date->day = day;
    date->hour = fraction / 3600000000ULL;
    fraction %= 3600000000ULL;
    date->minute = fraction / 60000000ULL;
    fraction %= 60000000ULL;
    date->second = fraction / 1000000ULL;
    date->microsecond = fraction % 1000000ULL;
    return 0;
}

static uint64_t shadps4_rtc_now(const ShadPS4HLEState *hle, unsigned clock)
{
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t tick = SHADPS4_RTC_UNIX_EPOCH_TICKS + ns / 1000;

    return tick + hle->rtc_tick_offset[clock];
}

static uint32_t shadps4_rtc_add(CPUState *cs, uint64_t output,
                                uint64_t input, int64_t amount,
                                uint64_t scale)
{
    uint64_t tick;
    __int128 result;

    if (!shadps4_rtc_read_tick(cs, input, &tick) || !output) {
        return SHADPS4_RTC_ERROR_INVALID_POINTER;
    }
    result = (__int128)tick + (__int128)amount * scale;
    if (result < 0 || result > SHADPS4_RTC_MAX_TICKS) {
        return SHADPS4_RTC_ERROR_INVALID_VALUE;
    }
    return shadps4_rtc_write_tick(cs, output, result) ? 0 :
           SHADPS4_RTC_ERROR_INVALID_POINTER;
}

static bool shadps4_rtc_write_string(CPUState *cs, uint64_t address,
                                     const char *value)
{
    return address && shadps4_guest_rw(cs, address, (void *)value,
                                       strlen(value) + 1, true);
}

static int shadps4_rtc_month_from_name(const char *name)
{
    static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int i;

    for (i = 0; i < 12; i++) {
        if (!strncmp(name, names + i * 3, 3)) {
            return i + 1;
        }
    }
    return 0;
}

static uint32_t shadps4_rtc_parse_rfc3339(const char *text, uint64_t *tick)
{
    ShadPS4RtcDateTime date = { 0 };
    const char *cursor;
    int timezone = 0;
    int sign = 1;
    int digits = 0;
    uint32_t error;
    __int128 utc;

    if (strlen(text) < 20 || text[4] != '-' || text[7] != '-' ||
        (text[10] != 'T' && text[10] != 't' && text[10] != ' ') ||
        text[13] != ':' || text[16] != ':' ||
        sscanf(text, "%4hu-%2hu-%2hu%*c%2hu:%2hu:%2hu",
               &date.year, &date.month, &date.day, &date.hour,
               &date.minute, &date.second) != 6) {
        return SHADPS4_RTC_ERROR_BAD_PARSE;
    }
    cursor = text + 19;
    if (*cursor == '.') {
        cursor++;
        while (g_ascii_isdigit(*cursor) && digits < 6) {
            date.microsecond = date.microsecond * 10 + (*cursor++ - '0');
            digits++;
        }
        if (!digits) {
            return SHADPS4_RTC_ERROR_BAD_PARSE;
        }
        while (digits++ < 6) {
            date.microsecond *= 10;
        }
        if (g_ascii_isdigit(*cursor)) {
            while (g_ascii_isdigit(*cursor)) {
                cursor++;
            }
        }
    }
    if (*cursor == 'Z' || *cursor == 'z') {
        cursor++;
    } else if (*cursor == '+' || *cursor == '-') {
        sign = *cursor++ == '-' ? -1 : 1;
        if (!g_ascii_isdigit(cursor[0]) || !g_ascii_isdigit(cursor[1]) ||
            cursor[2] != ':' || !g_ascii_isdigit(cursor[3]) ||
            !g_ascii_isdigit(cursor[4]) || cursor[5] ||
            cursor[0] > '2' || (cursor[0] == '2' && cursor[1] > '3') ||
            cursor[3] > '5') {
            return SHADPS4_RTC_ERROR_BAD_PARSE;
        }
        timezone = ((cursor[0] - '0') * 10 + cursor[1] - '0') * 60 +
                   (cursor[3] - '0') * 10 + cursor[4] - '0';
        timezone *= sign;
        cursor += 5;
    } else {
        return SHADPS4_RTC_ERROR_BAD_PARSE;
    }
    if (*cursor) {
        return SHADPS4_RTC_ERROR_BAD_PARSE;
    }
    error = shadps4_rtc_date_to_tick(&date, tick);
    if (error) {
        return error;
    }
    utc = (__int128)*tick - (__int128)timezone * 60000000;
    if (utc < 0 || utc > SHADPS4_RTC_MAX_TICKS) {
        return SHADPS4_RTC_ERROR_INVALID_VALUE;
    }
    *tick = utc;
    return 0;
}

static uint32_t shadps4_rtc_parse_datetime(const char *text, uint64_t *tick)
{
    ShadPS4RtcDateTime date = { 0 };
    char month[4] = { 0 };
    char zone[8] = { 0 };
    int offset = 0;
    int hour;
    int minute;
    uint32_t error;
    __int128 utc;

    if (strlen(text) >= 19 && text[4] == '-' && text[7] == '-') {
        return shadps4_rtc_parse_rfc3339(text, tick);
    }
    if (strchr(text, ',')) {
        if (sscanf(text, "%*3s, %2hu %3s %4hu %2hu:%2hu:%2hu %7s",
                   &date.day, month, &date.year, &date.hour, &date.minute,
                   &date.second, zone) != 7) {
            return SHADPS4_RTC_ERROR_BAD_PARSE;
        }
        date.month = shadps4_rtc_month_from_name(month);
        if ((zone[0] != '+' && zone[0] != '-') || strlen(zone) != 5 ||
            sscanf(zone + 1, "%2d%2d", &hour, &minute) != 2 ||
            hour > 23 || minute > 59) {
            return SHADPS4_RTC_ERROR_BAD_PARSE;
        }
        offset = (hour * 60 + minute) * (zone[0] == '-' ? -1 : 1);
    } else {
        if (sscanf(text, "%*3s %3s %2hu %2hu:%2hu:%2hu %4hu",
                   month, &date.day, &date.hour, &date.minute, &date.second,
                   &date.year) != 6) {
            return SHADPS4_RTC_ERROR_BAD_PARSE;
        }
        date.month = shadps4_rtc_month_from_name(month);
    }
    error = shadps4_rtc_date_to_tick(&date, tick);
    if (error) {
        return error;
    }
    utc = (__int128)*tick - (__int128)offset * 60000000;
    if (utc < 0 || utc > SHADPS4_RTC_MAX_TICKS) {
        return SHADPS4_RTC_ERROR_INVALID_VALUE;
    }
    *tick = utc;
    return 0;
}

static uint32_t shadps4_rtc_format(ShadPS4HLEState *hle, CPUState *cs,
                                   uint64_t output, uint64_t tick_address,
                                   int32_t timezone, bool rfc2822)
{
    static const char weekdays[] = "SunMonTueWedThuFriSat";
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    ShadPS4RtcDateTime date;
    uint64_t tick;
    __int128 local;
    int64_t days;
    int weekday;
    char value[64];
    char zone[8];
    uint32_t error;

    if (!output) {
        return SHADPS4_RTC_ERROR_INVALID_POINTER;
    }
    if (timezone < -1439 || timezone > 1439) {
        return SHADPS4_RTC_ERROR_INVALID_VALUE;
    }
    if (tick_address) {
        if (!shadps4_rtc_read_tick(cs, tick_address, &tick)) {
            return SHADPS4_RTC_ERROR_INVALID_POINTER;
        }
    } else {
        tick = shadps4_rtc_now(hle, 0);
    }
    local = (__int128)tick + (__int128)timezone * 60000000;
    if (local < 0 || local > SHADPS4_RTC_MAX_TICKS) {
        return SHADPS4_RTC_ERROR_INVALID_VALUE;
    }
    error = shadps4_rtc_tick_to_date(local, &date);
    if (error) {
        return error;
    }
    if (!timezone) {
        g_strlcpy(zone, rfc2822 ? "+0000" : "Z", sizeof(zone));
    } else if (rfc2822) {
        g_snprintf(zone, sizeof(zone), "%c%02d%02d",
                   timezone < 0 ? '-' : '+', ABS(timezone) / 60,
                   ABS(timezone) % 60);
    } else {
        g_snprintf(zone, sizeof(zone), "%c%02d:%02d",
                   timezone < 0 ? '-' : '+', ABS(timezone) / 60,
                   ABS(timezone) % 60);
    }
    if (rfc2822) {
        days = shadps4_rtc_days_from_civil(date.year, date.month, date.day);
        weekday = (days + 4) % 7;
        if (weekday < 0) {
            weekday += 7;
        }
        g_snprintf(value, sizeof(value), "%.3s, %02u %.3s %04u "
                   "%02u:%02u:%02u %s", weekdays + weekday * 3,
                   date.day, months + (date.month - 1) * 3, date.year,
                   date.hour, date.minute, date.second, zone);
    } else {
        g_snprintf(value, sizeof(value), "%04u-%02u-%02uT%02u:%02u:%02u"
                   ".%06u%s", date.year, date.month, date.day, date.hour,
                   date.minute, date.second, date.microsecond, zone);
    }
    return shadps4_rtc_write_string(cs, output, value) ? 0 :
           SHADPS4_RTC_ERROR_INVALID_POINTER;
}

static uint64_t shadps4_hle_kqueue(ShadPS4HLEState *hle)
{
    int i;

    for (i = 0; i < SHADPS4_HLE_MAX_EQUEUES; i++) {
        if (!hle->equeues[i]) {
            hle->equeues[i] = true;
            hle->equeue_vblank_seen[i] = hle->gpu->vblank_count;
            return 0x100 + i;
        }
    }
    return -SHADPS4_GUEST_ENOMEM;
}

static ShadPS4HLEEqueueEvent *shadps4_hle_equeue_find(
    ShadPS4HLEState *hle, uint64_t queue, uint64_t id, int16_t filter,
    bool free_slot)
{
    uint32_t index;
    uint32_t i;

    if (queue < 0x100 || queue >= 0x100 + SHADPS4_HLE_MAX_EQUEUES) {
        return NULL;
    }
    index = queue - 0x100;
    if (!hle->equeues[index]) {
        return NULL;
    }
    for (i = 0; i < SHADPS4_HLE_MAX_EQUEUE_EVENTS; i++) {
        ShadPS4HLEEqueueEvent *event = &hle->equeue_events[index][i];

        if (free_slot ? !event->used :
            event->used && event->id == id && event->filter == filter) {
            return event;
        }
    }
    return NULL;
}

static uint64_t shadps4_hle_equeue_add(ShadPS4HLEState *hle,
                                        uint64_t queue, uint64_t id,
                                        int16_t filter, uint64_t user_data,
                                        int64_t delay_ns, bool oneshot,
                                        bool edge)
{
    ShadPS4HLEEqueueEvent *event;

    if (queue < 0x100 || queue >= 0x100 + SHADPS4_HLE_MAX_EQUEUES ||
        !hle->equeues[queue - 0x100]) {
        return SHADPS4_KERNEL_ERROR_EBADF;
    }
    event = shadps4_hle_equeue_find(hle, queue, id, filter, false);
    if (!event) {
        event = shadps4_hle_equeue_find(hle, queue, 0, 0, true);
    }
    if (!event) {
        return SHADPS4_KERNEL_ERROR_ENOMEM;
    }
    memset(event, 0, sizeof(*event));
    event->id = id;
    event->user_data = user_data;
    event->filter = filter;
    event->interval_ns = delay_ns;
    event->deadline_ns = delay_ns ?
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns : 0;
    event->oneshot = oneshot;
    event->edge = edge;
    event->used = true;
    return 0;
}

static uint64_t shadps4_hle_equeue_delete_event(ShadPS4HLEState *hle,
                                                 uint64_t queue,
                                                 uint64_t id,
                                                 int16_t filter)
{
    ShadPS4HLEEqueueEvent *event;

    if (queue < 0x100 || queue >= 0x100 + SHADPS4_HLE_MAX_EQUEUES ||
        !hle->equeues[queue - 0x100]) {
        return SHADPS4_KERNEL_ERROR_EBADF;
    }
    event = shadps4_hle_equeue_find(hle, queue, id, filter, false);
    if (!event) {
        return SHADPS4_KERNEL_ERROR_ENOENT;
    }
    memset(event, 0, sizeof(*event));
    return 0;
}

static uint64_t shadps4_hle_equeue_wait(ShadPS4HLEState *hle,
                                        CPUState *cs, uint64_t queue,
                                        uint64_t event_addr, uint64_t count,
                                        uint64_t out_addr)
{
    uint32_t index;
    uint32_t written = 0;
    uint32_t i;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (queue < 0x100 || queue >= 0x100 + SHADPS4_HLE_MAX_EQUEUES ||
        !hle->equeues[queue - 0x100]) {
        return SHADPS4_KERNEL_ERROR_EBADF;
    }
    if (!event_addr || !out_addr) {
        return SHADPS4_KERNEL_ERROR_EFAULT;
    }
    if (!count) {
        uint32_t zero = 0;

        shadps4_guest_rw(cs, out_addr, &zero, sizeof(zero), true);
        return SHADPS4_KERNEL_ERROR_EINVAL;
    }
    index = queue - 0x100;
    for (i = 0; i < SHADPS4_HLE_MAX_EQUEUE_EVENTS && written < count; i++) {
        ShadPS4HLEEqueueEvent *source = &hle->equeue_events[index][i];
        ShadPS4GuestEvent event = { 0 };

        if (!source->used) {
            continue;
        }
        if (!source->triggered && source->deadline_ns &&
            now >= source->deadline_ns) {
            source->triggered = true;
            source->data++;
        }
        if (!source->triggered) {
            continue;
        }
        event.ident = cpu_to_le64(source->id);
        event.filter = cpu_to_le16(source->filter);
        event.flags = cpu_to_le16(1 | (source->edge ? 0x20 : 0));
        event.data = cpu_to_le64(source->data);
        event.user_data = cpu_to_le64(source->user_data);
        if (!shadps4_guest_rw(cs,
                              event_addr + written * sizeof(event),
                              &event, sizeof(event), true)) {
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
        written++;
        source->triggered = false;
        if (source->oneshot) {
            memset(source, 0, sizeof(*source));
        } else if (source->deadline_ns) {
            source->deadline_ns = now + source->interval_ns;
        }
    }
    i = cpu_to_le32(written);
    if (!shadps4_guest_rw(cs, out_addr, &i, sizeof(i), true)) {
        return SHADPS4_KERNEL_ERROR_EFAULT;
    }
    return written ? 0 : SHADPS4_KERNEL_ERROR_ETIMEDOUT;
}

static uint64_t shadps4_hle_kevent(ShadPS4HLEState *hle, CPUState *cs,
                                   uint64_t queue, uint64_t event_addr,
                                   uint64_t event_count)
{
    uint32_t index;
    ShadPS4GuestEvent event = {
        .ident = cpu_to_le64(1),
        .filter = cpu_to_le16(-7),
        .data = cpu_to_le64(hle->gpu->vblank_count),
    };

    if (queue < 0x100 || queue >= 0x100 + SHADPS4_HLE_MAX_EQUEUES) {
        return -SHADPS4_GUEST_EBADF;
    }
    index = queue - 0x100;
    if (!hle->equeues[index]) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (!event_addr || !event_count ||
        hle->equeue_vblank_seen[index] == hle->gpu->vblank_count) {
        return 0;
    }
    if (!shadps4_guest_rw(cs, event_addr, &event, sizeof(event), true)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    hle->equeue_vblank_seen[index] = hle->gpu->vblank_count;
    return 1;
}

static uint64_t shadps4_hle_ioctl(ShadPS4HLEState *hle, CPUState *cs,
                                  uint64_t fd, uint64_t command,
                                  uint64_t argument)
{
    struct {
        uint64_t submit_count;
        uint64_t flip_count;
        uint64_t vblank_count;
    } legacy_status;
    ShadPS4GPUStatus status;
    ShadPS4GPUSubmit submit;
    ShadPS4GPUFlip flip;
    ShadPS4AudioConfig audio;
    ShadPS4AudioVolume volume;
    ShadPS4GuestAtomicReplace atomic;
    QemuHostPadOutput output;
    uint64_t queued;
    uint32_t rumble;

    if (fd >= SHADPS4_HLE_MAX_FDS ||
        hle->files[fd] == SHADPS4_HLE_FD_FREE) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (hle->files[fd] == SHADPS4_HLE_FD_AUDIO) {
        if (command == SHADPS4_AUDIO_IOCTL_DRAIN) {
            return shadps4_io_drain_audio(hle->io) ? 0 :
                   -SHADPS4_GUEST_EAGAIN;
        }
        if (command == SHADPS4_AUDIO_IOCTL_QUEUED) {
            queued = cpu_to_le64(shadps4_io_audio_queued(hle->io));
            return shadps4_guest_rw(cs, argument, &queued, sizeof(queued),
                                    true) ? 0 : -SHADPS4_GUEST_EFAULT;
        }
        if (command == SHADPS4_AUDIO_IOCTL_VOLUME) {
            if (!shadps4_guest_rw(cs, argument, &volume, sizeof(volume),
                                  false)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            return shadps4_io_set_audio_volume(
                hle->io, volume.muted, volume.channels, volume.volume) ?
                0 : -SHADPS4_GUEST_EINVAL;
        }
        if (command != SHADPS4_AUDIO_IOCTL_CONFIG ||
            !shadps4_guest_rw(cs, argument, &audio, sizeof(audio), false)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_io_configure_audio(
            hle->io, le32_to_cpu(audio.sample_rate),
            le32_to_cpu(audio.channels), le32_to_cpu(audio.format)) ?
            0 : -SHADPS4_GUEST_EINVAL;
    }
    if (hle->files[fd] == SHADPS4_HLE_FD_AUDIO_IN) {
        return command == SHADPS4_AUDIO_IOCTL_CONFIG ? 0 :
               -SHADPS4_GUEST_EINVAL;
    }
    if (hle->files[fd] == SHADPS4_HLE_FD_PAD) {
        if (command == SHADPS4_PAD_IOCTL_OUTPUT) {
            if (!shadps4_guest_rw(cs, argument, &output, sizeof(output),
                                  false)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            output.rumble_small = le16_to_cpu(output.rumble_small);
            output.rumble_large = le16_to_cpu(output.rumble_large);
            return shadps4_io_set_pad_output(
                hle->io, hle->file_units[fd], &output) ? 0 :
                -SHADPS4_GUEST_EINVAL;
        }
        if (command != SHADPS4_PAD_IOCTL_RUMBLE ||
            !shadps4_guest_rw(cs, argument, &rumble, sizeof(rumble), false)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        rumble = le32_to_cpu(rumble);
        output = (QemuHostPadOutput) {
            .rumble_small = rumble & 0xffff,
            .rumble_large = rumble >> 16,
        };
        return shadps4_io_set_pad_output(
            hle->io, hle->file_units[fd], &output) ? 0 :
            -SHADPS4_GUEST_EINVAL;
    }
    if (hle->files[fd] == SHADPS4_HLE_FD_STORAGE) {
        g_autofree uint8_t *data = NULL;
        int ret;

        if (command != SHADPS4_STORAGE_IOCTL_ATOMIC_REPLACE) {
            return -SHADPS4_GUEST_EINVAL;
        }
        if (hle->storage_read_only[fd]) {
            return -SHADPS4_GUEST_EROFS;
        }
        if (!shadps4_guest_rw(cs, argument, &atomic, sizeof(atomic), false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        atomic.data = le64_to_cpu(atomic.data);
        atomic.size = le64_to_cpu(atomic.size);
        if (!atomic.size || atomic.size > 16 * MiB) {
            return -SHADPS4_GUEST_EINVAL;
        }
        data = g_malloc(atomic.size);
        if (!shadps4_guest_rw(cs, atomic.data, data, atomic.size, false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        ret = qemu_host_storage_atomic_replace(hle->storage_paths[fd], data,
                                               atomic.size);
        return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
    }
    if (hle->files[fd] != SHADPS4_HLE_FD_GPU) {
        return -SHADPS4_GUEST_EBADF;
    }
    switch (command) {
    case SHADPS4_GPU_IOCTL_SUBMIT:
        if (!argument) {
            return shadps4_gpu_submit(hle->gpu, cs, NULL) ? 0 :
                   -SHADPS4_GUEST_EINVAL;
        }
        if (!shadps4_guest_rw(cs, argument, &submit, sizeof(submit), false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        submit.command_addr = le64_to_cpu(submit.command_addr);
        submit.command_dwords = le32_to_cpu(submit.command_dwords);
        submit.queue_id = le32_to_cpu(submit.queue_id);
        submit.fence_addr = le64_to_cpu(submit.fence_addr);
        submit.fence_value = le64_to_cpu(submit.fence_value);
        return shadps4_gpu_submit(hle->gpu, cs, &submit) ? 0 :
               -SHADPS4_GUEST_EINVAL;
    case SHADPS4_GPU_IOCTL_FLIP:
        if (!argument) {
            return shadps4_gpu_flip(hle->gpu, cs, NULL) ? 0 :
                   -SHADPS4_GUEST_EINVAL;
        }
        if (!shadps4_guest_rw(cs, argument, &flip, sizeof(flip), false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        flip.pixels_addr = le64_to_cpu(flip.pixels_addr);
        flip.width = le32_to_cpu(flip.width);
        flip.height = le32_to_cpu(flip.height);
        flip.stride = le32_to_cpu(flip.stride);
        flip.format = le32_to_cpu(flip.format);
        return shadps4_gpu_flip(hle->gpu, cs, &flip) ? 0 :
               -SHADPS4_GUEST_EINVAL;
    case SHADPS4_GPU_IOCTL_STATUS_LEGACY:
        shadps4_gpu_get_status(hle->gpu, &status);
        legacy_status.submit_count = cpu_to_le64(status.submit_count);
        legacy_status.flip_count = cpu_to_le64(status.flip_count);
        legacy_status.vblank_count = cpu_to_le64(status.vblank_count);
        return shadps4_guest_rw(cs, argument, &legacy_status,
                                sizeof(legacy_status), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    case SHADPS4_GPU_IOCTL_STATUS:
        shadps4_gpu_get_status(hle->gpu, &status);
        status.submit_count = cpu_to_le64(status.submit_count);
        status.flip_count = cpu_to_le64(status.flip_count);
        status.vblank_count = cpu_to_le64(status.vblank_count);
        status.parsed_packet_count =
            cpu_to_le64(status.parsed_packet_count);
        status.rejected_submit_count =
            cpu_to_le64(status.rejected_submit_count);
        status.last_fence_value = cpu_to_le64(status.last_fence_value);
        return shadps4_guest_rw(cs, argument, &status, sizeof(status), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    default:
        return -SHADPS4_GUEST_EINVAL;
    }
}

static bool shadps4_hle_scoped_guest_path(ShadPS4HLEState *hle, CPUState *cs,
                                           uint64_t path_addr, char *scoped,
                                           size_t scoped_size,
                                           bool *read_only)
{
    char path[128];

    return shadps4_guest_read_string(cs, path_addr, path, sizeof(path)) &&
           shadps4_hle_storage_path(hle, path, scoped, scoped_size,
                                    read_only);
}

static void shadps4_hle_stat_to_guest(const QemuHostStorageStat *stat,
                                      ShadPS4GuestStorageStat *guest)
{
    guest->size = cpu_to_le64(stat->size);
    guest->allocated_size = cpu_to_le64(stat->allocated_size);
    guest->modified_time_ns = cpu_to_le64(stat->modified_time_ns);
    guest->mode = cpu_to_le32(stat->mode);
    guest->type = cpu_to_le32(stat->type);
}

static uint64_t shadps4_hle_stat(ShadPS4HLEState *hle, CPUState *cs,
                                 uint64_t path_addr, uint64_t stat_addr)
{
    char scoped[192];
    bool read_only;
    QemuHostStorageStat stat;
    ShadPS4GuestStorageStat guest;
    int ret;

    if (!shadps4_hle_scoped_guest_path(hle, cs, path_addr, scoped,
                                        sizeof(scoped), &read_only)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    ret = qemu_host_storage_stat(scoped, &stat);
    if (ret < 0) {
        return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
    }
    shadps4_hle_stat_to_guest(&stat, &guest);
    return shadps4_guest_rw(cs, stat_addr, &guest, sizeof(guest), true) ? 0 :
           -SHADPS4_GUEST_EFAULT;
}

static uint64_t shadps4_hle_fstat(ShadPS4HLEState *hle, CPUState *cs,
                                  uint64_t fd, uint64_t stat_addr)
{
    QemuHostStorageStat stat;
    ShadPS4GuestStorageStat guest;
    int ret;

    if (fd >= SHADPS4_HLE_MAX_FDS ||
        hle->files[fd] != SHADPS4_HLE_FD_STORAGE) {
        return -SHADPS4_GUEST_EBADF;
    }
    ret = qemu_host_storage_stat(hle->storage_paths[fd], &stat);
    if (ret < 0) {
        return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
    }
    shadps4_hle_stat_to_guest(&stat, &guest);
    return shadps4_guest_rw(cs, stat_addr, &guest, sizeof(guest), true) ? 0 :
           -SHADPS4_GUEST_EFAULT;
}

static uint64_t shadps4_hle_mkdir(ShadPS4HLEState *hle, CPUState *cs,
                                  uint64_t path_addr, uint64_t mode)
{
    char scoped[192];
    bool read_only;
    int ret;

    if (!shadps4_hle_scoped_guest_path(hle, cs, path_addr, scoped,
                                        sizeof(scoped), &read_only)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    if (read_only) {
        return -SHADPS4_GUEST_EROFS;
    }
    ret = qemu_host_storage_mkdir(scoped, mode);
    return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
}

static uint64_t shadps4_hle_unlink(ShadPS4HLEState *hle, CPUState *cs,
                                   uint64_t path_addr)
{
    char scoped[192];
    bool read_only;
    int ret;

    if (!shadps4_hle_scoped_guest_path(hle, cs, path_addr, scoped,
                                        sizeof(scoped), &read_only)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    if (read_only) {
        return -SHADPS4_GUEST_EROFS;
    }
    ret = qemu_host_storage_unlink(scoped);
    return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
}

static uint64_t shadps4_hle_rename(ShadPS4HLEState *hle, CPUState *cs,
                                   uint64_t old_addr, uint64_t new_addr)
{
    char old_path[192];
    char new_path[192];
    bool old_read_only;
    bool new_read_only;
    int ret;

    if (!shadps4_hle_scoped_guest_path(hle, cs, old_addr, old_path,
                                        sizeof(old_path), &old_read_only) ||
        !shadps4_hle_scoped_guest_path(hle, cs, new_addr, new_path,
                                        sizeof(new_path), &new_read_only)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    if (old_read_only || new_read_only) {
        return -SHADPS4_GUEST_EROFS;
    }
    ret = qemu_host_storage_rename(old_path, new_path);
    return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
}

static uint64_t shadps4_hle_lseek(ShadPS4HLEState *hle, uint64_t fd,
                                  uint64_t offset, uint64_t whence)
{
    int64_t result;

    if (fd >= SHADPS4_HLE_MAX_FDS ||
        hle->files[fd] != SHADPS4_HLE_FD_STORAGE || whence > 2) {
        return -SHADPS4_GUEST_EBADF;
    }
    result = qemu_host_storage_seek(hle->storage_handles[fd], offset, whence);
    return result >= -255 ? result : -SHADPS4_GUEST_EIO;
}

static uint64_t shadps4_hle_fsync(ShadPS4HLEState *hle, uint64_t fd)
{
    int ret;

    if (fd >= SHADPS4_HLE_MAX_FDS ||
        hle->files[fd] != SHADPS4_HLE_FD_STORAGE) {
        return -SHADPS4_GUEST_EBADF;
    }
    ret = qemu_host_storage_flush(hle->storage_handles[fd]);
    return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
}

static uint64_t shadps4_hle_ftruncate(ShadPS4HLEState *hle, uint64_t fd,
                                      uint64_t length)
{
    QemuHostStorageStat stat;
    int64_t saved;
    uint8_t zero = 0;

    if (fd >= SHADPS4_HLE_MAX_FDS ||
        hle->files[fd] != SHADPS4_HLE_FD_STORAGE) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (hle->storage_read_only[fd]) {
        return -SHADPS4_GUEST_EROFS;
    }
    {
        int result = qemu_host_storage_truncate(hle->storage_handles[fd],
                                                length);

        if (result != -ENOSYS) {
            return result >= -255 ? result : -SHADPS4_GUEST_EIO;
        }
    }
    if (qemu_host_storage_stat(hle->storage_paths[fd], &stat) < 0) {
        return -SHADPS4_GUEST_EIO;
    }
    if (length == stat.size) {
        return 0;
    }
    if (length < stat.size) {
        return -SHADPS4_GUEST_ENOSYS;
    }
    saved = qemu_host_storage_seek(hle->storage_handles[fd], 0, SEEK_CUR);
    if (saved < 0 || !length || qemu_host_storage_seek(
            hle->storage_handles[fd], length - 1, SEEK_SET) < 0 ||
        qemu_host_storage_write(hle->storage_handles[fd], &zero, 1) != 1) {
        return -SHADPS4_GUEST_EIO;
    }
    return qemu_host_storage_seek(hle->storage_handles[fd], saved,
                                  SEEK_SET) < 0 ?
           -SHADPS4_GUEST_EIO : 0;
}

static uint64_t shadps4_hle_check_reachability(ShadPS4HLEState *hle,
                                               CPUState *cs,
                                               uint64_t path_addr)
{
    char scoped[192];
    bool read_only;
    QemuHostStorageStat stat;
    int ret;

    if (!shadps4_hle_scoped_guest_path(hle, cs, path_addr, scoped,
                                        sizeof(scoped), &read_only)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    ret = qemu_host_storage_stat(scoped, &stat);
    return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
}

static uint64_t shadps4_hle_getdirentries(ShadPS4HLEState *hle, CPUState *cs,
                                          uint64_t fd, uint64_t buffer_addr,
                                          uint64_t size)
{
    QemuHostStorageStat stat;
    ShadPS4GuestDirent entry = { 0 };
    char name[sizeof(entry.name)];
    size_t name_length;
    int ret;

    if (fd >= SHADPS4_HLE_MAX_FDS ||
        hle->files[fd] != SHADPS4_HLE_FD_STORAGE) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (size < sizeof(entry)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    ret = qemu_host_storage_readdir(hle->storage_handles[fd], name,
                                    sizeof(name), &stat);
    if (ret <= 0) {
        return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
    }
    name[sizeof(name) - 1] = 0;
    name_length = strlen(name);
    if (!name_length || strchr(name, '/') || strchr(name, '\\')) {
        return -SHADPS4_GUEST_EIO;
    }
    entry.record_size = cpu_to_le16(sizeof(entry));
    entry.type = stat.type;
    entry.name_length = name_length;
    memcpy(entry.name, name, name_length + 1);
    return shadps4_guest_rw(cs, buffer_addr, &entry, sizeof(entry), true) ?
           sizeof(entry) : -SHADPS4_GUEST_EFAULT;
}

static uint64_t shadps4_hle_socket(ShadPS4HLEState *hle, uint64_t domain,
                                   uint64_t type, uint64_t protocol)
{
    int64_t handle;
    int ret;
    int fd;

    ret = qemu_host_network_socket(domain, type, protocol, &handle);
    if (ret < 0) {
        return ret >= -255 ? ret : -SHADPS4_GUEST_EACCES;
    }
    for (fd = 3; fd < SHADPS4_HLE_MAX_FDS; fd++) {
        if (hle->files[fd] == SHADPS4_HLE_FD_FREE) {
            hle->files[fd] = SHADPS4_HLE_FD_NETWORK;
            hle->network_handles[fd] = handle;
            return fd;
        }
    }
    qemu_host_network_close(handle);
    return -SHADPS4_GUEST_ENOMEM;
}

static int shadps4_hle_network_adopt(ShadPS4HLEState *hle, int64_t handle)
{
    int fd;

    for (fd = 3; fd < SHADPS4_HLE_MAX_FDS; fd++) {
        if (hle->files[fd] == SHADPS4_HLE_FD_FREE) {
            hle->files[fd] = SHADPS4_HLE_FD_NETWORK;
            hle->network_handles[fd] = handle;
            return fd;
        }
    }
    qemu_host_network_close(handle);
    return -SHADPS4_GUEST_ENOMEM;
}

static bool shadps4_hle_network_fd(ShadPS4HLEState *hle, uint64_t fd)
{
    return fd < SHADPS4_HLE_MAX_FDS &&
           hle->files[fd] == SHADPS4_HLE_FD_NETWORK;
}

static int64_t shadps4_hle_network_result(ShadPS4HLEState *hle,
                                          int64_t result)
{
    if (result < 0) {
        hle->net_errno = -result <= 255 ? -result : SHADPS4_GUEST_EIO;
        return result >= -255 ? result : -SHADPS4_GUEST_EIO;
    }
    return result;
}

static bool shadps4_hle_read_sockaddr(CPUState *cs, uint64_t address,
                                      uint64_t supplied_size,
                                      uint8_t buffer[128], size_t *size)
{
    uint8_t header[2];

    if (!address || supplied_size < sizeof(header) || supplied_size > 128 ||
        !shadps4_guest_rw(cs, address, header, sizeof(header), false)) {
        return false;
    }
    *size = header[0] ? header[0] : supplied_size;
    if (*size < sizeof(header) || *size > supplied_size || *size > 128) {
        return false;
    }
    return shadps4_guest_rw(cs, address, buffer, *size, false);
}

static uint64_t shadps4_hle_network_address_call(
    ShadPS4HLEState *hle, CPUState *cs, uint64_t fd, uint64_t address,
    uint64_t address_size, bool bind)
{
    uint8_t buffer[128];
    size_t size;
    int result;

    if (!shadps4_hle_network_fd(hle, fd)) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (!shadps4_hle_read_sockaddr(cs, address, address_size,
                                   buffer, &size)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    result = bind ? qemu_host_network_bind(hle->network_handles[fd], buffer,
                                           size) :
                    -ENOSYS;
    return shadps4_hle_network_result(hle, result);
}

static uint64_t shadps4_hle_network_accept(ShadPS4HLEState *hle,
                                           CPUState *cs, uint64_t fd,
                                           uint64_t address,
                                           uint64_t address_size_ptr)
{
    uint8_t buffer[128] = { 0 };
    uint32_t guest_size = 0;
    size_t size = 0;
    int64_t handle;
    int result;

    if (!shadps4_hle_network_fd(hle, fd)) {
        return -SHADPS4_GUEST_EBADF;
    }
    if ((address || address_size_ptr) && (!address || !address_size_ptr ||
        !shadps4_guest_rw(cs, address_size_ptr, &guest_size,
                          sizeof(guest_size), false))) {
        return -SHADPS4_GUEST_EFAULT;
    }
    guest_size = le32_to_cpu(guest_size);
    if (guest_size > sizeof(buffer)) {
        guest_size = sizeof(buffer);
    }
    size = guest_size;
    result = qemu_host_network_accept(hle->network_handles[fd],
                                      address ? buffer : NULL,
                                      address ? &size : NULL, &handle);
    if (result < 0) {
        return shadps4_hle_network_result(hle, result);
    }
    if (address && ((!size || size > guest_size) ||
        !shadps4_guest_rw(cs, address, buffer, size, true))) {
        qemu_host_network_close(handle);
        return -SHADPS4_GUEST_EFAULT;
    }
    if (address_size_ptr) {
        guest_size = cpu_to_le32(size);
        if (!shadps4_guest_rw(cs, address_size_ptr, &guest_size,
                              sizeof(guest_size), true)) {
            qemu_host_network_close(handle);
            return -SHADPS4_GUEST_EFAULT;
        }
    }
    return shadps4_hle_network_adopt(hle, handle);
}

static int shadps4_hle_resolve_ipv4(ShadPS4HLEState *hle, CPUState *cs,
                                    uint64_t hostname_address,
                                    uint8_t address[4])
{
    char hostname[256];
    size_t size = 4;
    int result;

    if (!shadps4_guest_read_string(cs, hostname_address, hostname,
                                    sizeof(hostname))) {
        return -SHADPS4_GUEST_EFAULT;
    }
    if (inet_pton(AF_INET, hostname, address) == 1) {
        return 0;
    }
    result = qemu_host_network_resolve(hostname, AF_INET, address, &size);
    if (result < 0 || size != 4) {
        result = result < 0 ? result : -EIO;
        return shadps4_hle_network_result(hle, result);
    }
    return 0;
}

static uint64_t shadps4_hle_connect(ShadPS4HLEState *hle, CPUState *cs,
                                    uint64_t fd, uint64_t address,
                                    uint64_t length)
{
    ShadPS4GuestSockaddrIn sockaddr;
    char ip[16];
    uint16_t port;
    int ret;

    if (fd >= SHADPS4_HLE_MAX_FDS ||
        hle->files[fd] != SHADPS4_HLE_FD_NETWORK) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (length < sizeof(sockaddr) ||
        !shadps4_guest_rw(cs, address, &sockaddr, sizeof(sockaddr), false) ||
        sockaddr.family != 2) {
        return -SHADPS4_GUEST_EINVAL;
    }
    g_snprintf(ip, sizeof(ip), "%u.%u.%u.%u", sockaddr.address[0],
               sockaddr.address[1], sockaddr.address[2], sockaddr.address[3]);
    port = ((uint16_t)sockaddr.port[0] << 8) | sockaddr.port[1];
    ret = qemu_host_network_connect(hle->network_handles[fd], ip, port);
    return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
}

static uint8_t shadps4_hle_stick(int16_t value)
{
    return ((int32_t)value + 32768) * 255 / 65535;
}

static uint64_t shadps4_hle_pad_read(ShadPS4HLEState *hle, CPUState *cs,
                                     uint64_t handle, uint64_t data_addr,
                                     uint64_t count)
{
    ShadPS4PadDeviceState pad;
    ShadPS4GuestPadData data = { 0 };
    uint32_t controller;
    uint32_t i;

    if (!handle || handle > QEMU_HOST_MAX_GAMEPADS || !data_addr || !count) {
        return -SHADPS4_GUEST_EINVAL;
    }
    controller = handle - 1;
    if (!hle->pad_open[controller] ||
        !shadps4_io_get_pad(hle->io, controller, &pad)) {
        return -SHADPS4_GUEST_EBADF;
    }
    data.buttons = cpu_to_le32(pad.legacy.buttons);
    data.left_x = shadps4_hle_stick(pad.legacy.left_x);
    data.left_y = shadps4_hle_stick(pad.legacy.left_y);
    data.right_x = shadps4_hle_stick(pad.legacy.right_x);
    data.right_y = shadps4_hle_stick(pad.legacy.right_y);
    data.left_trigger = pad.left_trigger;
    data.right_trigger = pad.right_trigger;
    memcpy(data.orientation, pad.motion.orientation, sizeof(data.orientation));
    memcpy(data.acceleration, pad.motion.acceleration,
           sizeof(data.acceleration));
    memcpy(data.angular_velocity, pad.motion.angular_velocity,
           sizeof(data.angular_velocity));
    for (i = 0; i < QEMU_HOST_MAX_TOUCHES; i++) {
        if (!pad.touch.points[i].active) {
            continue;
        }
        data.touches[data.touch_count].x = cpu_to_le16(
            pad.touch.points[i].x * 1920.0f);
        data.touches[data.touch_count].y = cpu_to_le16(
            pad.touch.points[i].y * 942.0f);
        data.touches[data.touch_count].id = pad.touch.points[i].id;
        data.touch_count++;
    }
    data.connected = pad.connected;
    data.timestamp = cpu_to_le64(pad.timestamp_ns / 1000);
    data.connected_count = pad.connected ? 1 : 0;
    return shadps4_guest_rw(cs, data_addr, &data, sizeof(data), true) ?
           1 : -SHADPS4_GUEST_EFAULT;
}

static uint64_t shadps4_hle_pad_output(ShadPS4HLEState *hle, CPUState *cs,
                                       uint64_t handle, uint64_t param_addr,
                                       bool lightbar)
{
    ShadPS4PadDeviceState pad;
    QemuHostPadOutput output;
    uint8_t param[4] = { 0 };
    uint32_t controller;

    if (!handle || handle > QEMU_HOST_MAX_GAMEPADS || !param_addr) {
        return -SHADPS4_GUEST_EINVAL;
    }
    controller = handle - 1;
    if (!hle->pad_open[controller] ||
        !shadps4_io_get_pad(hle->io, controller, &pad) ||
        !shadps4_guest_rw(cs, param_addr, param,
                          lightbar ? 4 : 2, false)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    output = pad.output;
    if (lightbar) {
        output.lightbar_red = param[0];
        output.lightbar_green = param[1];
        output.lightbar_blue = param[2];
    } else {
        output.rumble_large = param[0] * 257;
        output.rumble_small = param[1] * 257;
    }
    return shadps4_io_set_pad_output(hle->io, controller, &output) ?
           0 : -SHADPS4_GUEST_EINVAL;
}

static uint64_t shadps4_hle_pad_set_default_lightbar(ShadPS4HLEState *hle,
                                                      uint64_t handle)
{
    ShadPS4PadDeviceState pad;
    QemuHostPadOutput output;
    uint32_t controller;

    if (!handle || handle > QEMU_HOST_MAX_GAMEPADS) {
        return -SHADPS4_GUEST_EINVAL;
    }
    controller = handle - 1;
    if (!hle->pad_open[controller] ||
        !shadps4_io_get_pad(hle->io, controller, &pad)) {
        return -SHADPS4_GUEST_EBADF;
    }
    output = pad.output;
    output.lightbar_red = 0;
    output.lightbar_green = 0;
    output.lightbar_blue = 255;
    return shadps4_io_set_pad_output(hle->io, controller, &output) ?
           0 : -SHADPS4_GUEST_EINVAL;
}

static bool shadps4_hle_audio_format(uint32_t format, uint32_t *channels,
                                     uint32_t *sample_size,
                                     QemuHostAudioFormat *host_format)
{
    switch (format & 0xff) {
    case 0:
        *channels = 1;
        *sample_size = 2;
        *host_format = QEMU_HOST_AUDIO_FORMAT_S16;
        return true;
    case 1:
        *channels = 2;
        *sample_size = 2;
        *host_format = QEMU_HOST_AUDIO_FORMAT_S16;
        return true;
    case 2:
    case 6:
        *channels = 8;
        *sample_size = 2;
        *host_format = QEMU_HOST_AUDIO_FORMAT_S16;
        return true;
    case 3:
        *channels = 1;
        *sample_size = 4;
        *host_format = QEMU_HOST_AUDIO_FORMAT_F32;
        return true;
    case 4:
        *channels = 2;
        *sample_size = 4;
        *host_format = QEMU_HOST_AUDIO_FORMAT_F32;
        return true;
    case 5:
    case 7:
        *channels = 8;
        *sample_size = 4;
        *host_format = QEMU_HOST_AUDIO_FORMAT_F32;
        return true;
    default:
        return false;
    }
}

static uint64_t shadps4_hle_audio_out_open(ShadPS4HLEState *hle,
                                            uint64_t port_type,
                                            uint64_t frames,
                                            uint64_t sample_rate,
                                            uint64_t format)
{
    QemuHostAudioFormat host_format;
    uint32_t channels;
    uint32_t sample_size;
    uint32_t i;

    if ((port_type > 4 && port_type != 126 && port_type != 127) ||
        !frames || frames > 8192 || sample_rate != 48000 ||
        !shadps4_hle_audio_format(format, &channels, &sample_size,
                                  &host_format) ||
        !shadps4_io_configure_audio(hle->io, sample_rate, channels,
                                    host_format)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    for (i = 0; i < SHADPS4_HLE_MAX_AUDIO_PORTS; i++) {
        if (!hle->audio_out[i].open) {
            hle->audio_out[i] = (ShadPS4HLEAudioPort) {
                .frames = frames,
                .channels = channels,
                .sample_size = sample_size,
                .format = host_format,
                .port_type = port_type,
                .mix_level = 11626,
                .open = true,
            };
            return i + 1;
        }
    }
    return -SHADPS4_GUEST_ENOMEM;
}

static uint64_t shadps4_hle_audio_out_output(ShadPS4HLEState *hle,
                                              CPUState *cs, uint64_t handle,
                                              uint64_t data_addr)
{
    ShadPS4HLEAudioPort *port;
    size_t size;

    if (!handle || handle > SHADPS4_HLE_MAX_AUDIO_PORTS) {
        return -SHADPS4_GUEST_EBADF;
    }
    port = &hle->audio_out[handle - 1];
    if (!port->open) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (!data_addr) {
        return shadps4_io_drain_audio(hle->io) ? 0 :
               -SHADPS4_GUEST_EAGAIN;
    }
    size = (size_t)port->frames * port->channels * port->sample_size;
    if (!shadps4_io_emit_audio(hle->io, cs, data_addr, size)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    port->last_output_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000;
    return 0;
}

static uint64_t shadps4_hle_audio_in_open(ShadPS4HLEState *hle,
                                           uint64_t frames,
                                           uint64_t sample_rate,
                                           uint64_t format)
{
    uint32_t channels = format == 0 ? 1 : format == 2 ? 2 : 0;
    uint32_t i;

    if (!channels || !frames || frames > 8192 || sample_rate != 48000) {
        return -SHADPS4_GUEST_EINVAL;
    }
    for (i = 0; i < SHADPS4_HLE_MAX_AUDIO_PORTS; i++) {
        if (!hle->audio_in[i].open) {
            hle->audio_in[i] = (ShadPS4HLEAudioPort) {
                .frames = frames,
                .channels = channels,
                .sample_size = 2,
                .format = QEMU_HOST_AUDIO_FORMAT_S16,
                .open = true,
            };
            return i + 1;
        }
    }
    return -SHADPS4_GUEST_ENOMEM;
}

static uint64_t shadps4_hle_audio_in_input(ShadPS4HLEState *hle,
                                            CPUState *cs, uint64_t handle,
                                            uint64_t data_addr)
{
    ShadPS4HLEAudioPort *port;
    g_autofree uint8_t *buffer = NULL;
    size_t size;
    size_t copied;

    if (!handle || handle > SHADPS4_HLE_MAX_AUDIO_PORTS || !data_addr) {
        return -SHADPS4_GUEST_EINVAL;
    }
    port = &hle->audio_in[handle - 1];
    if (!port->open) {
        return -SHADPS4_GUEST_EBADF;
    }
    size = (size_t)port->frames * port->channels * port->sample_size;
    buffer = g_malloc0(size);
    copied = shadps4_io_read_audio_input(hle->io, buffer, size);
    if (copied < size) {
        memset(buffer + copied, 0, size - copied);
    }
    return shadps4_guest_rw(cs, data_addr, buffer, size, true) ?
           0 : -SHADPS4_GUEST_EFAULT;
}

static bool shadps4_hle_video_format(uint32_t orbis_format,
                                     uint32_t *host_format)
{
    switch (orbis_format) {
    case 0x80000000:
        *host_format = QEMU_HOST_PIXEL_FORMAT_BGRA8888;
        return true;
    case 0x80002200:
        *host_format = QEMU_HOST_PIXEL_FORMAT_RGBA8888;
        return true;
    default:
        return false;
    }
}

static uint64_t shadps4_hle_video_register_buffers(
    ShadPS4HLEState *hle, CPUState *cs, uint64_t handle,
    uint64_t start_index, uint64_t addresses_addr, uint64_t count,
    uint64_t attribute_addr)
{
    ShadPS4GuestVideoBufferAttribute attribute;
    uint32_t host_format;
    uint32_t i;

    if (handle != 1 || !hle->video_open || !count ||
        start_index >= SHADPS4_HLE_MAX_VIDEO_BUFFERS ||
        count > SHADPS4_HLE_MAX_VIDEO_BUFFERS - start_index ||
        !shadps4_guest_rw(cs, attribute_addr, &attribute,
                          sizeof(attribute), false)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    attribute.pixel_format = le32_to_cpu(attribute.pixel_format);
    attribute.tiling_mode = le32_to_cpu(attribute.tiling_mode);
    attribute.width = le32_to_cpu(attribute.width);
    attribute.height = le32_to_cpu(attribute.height);
    attribute.pitch = le32_to_cpu(attribute.pitch);
    if (!shadps4_hle_video_format(attribute.pixel_format, &host_format) ||
        (attribute.tiling_mode != 0 && attribute.tiling_mode != 1) ||
        !attribute.width || !attribute.height || !attribute.pitch ||
        attribute.pitch < attribute.width || attribute.width > 4096 ||
        attribute.height > 2160 || attribute.pitch > 65536 / 4 ||
        addresses_addr > UINT64_MAX - count * sizeof(uint64_t)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    for (i = 0; i < count; i++) {
        ShadPS4HLEVideoBuffer *buffer =
            &hle->video_buffers[start_index + i];
        uint64_t address;

        if (!shadps4_guest_rw(cs, addresses_addr + i * sizeof(address),
                              &address, sizeof(address), false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        address = le64_to_cpu(address);
        if (!address) {
            return -SHADPS4_GUEST_EINVAL;
        }
        *buffer = (ShadPS4HLEVideoBuffer) {
            .address = address,
            .width = attribute.width,
            .height = attribute.height,
            .pitch = attribute.pitch,
            .format = host_format,
            .tiling_mode = attribute.tiling_mode,
            .group = start_index,
            .registered = true,
        };
        if (!shadps4_gpu_register_surface(hle->gpu, start_index + i,
                                          address, attribute.width,
                                          attribute.height, attribute.pitch,
                                          host_format,
                                          attribute.tiling_mode)) {
            warn_report("shadPS4 D3D12: failed to register VideoOut surface "
                        "%" PRIu64, start_index + i);
        }
    }
    return start_index;
}

static uint64_t shadps4_hle_video_flip(ShadPS4HLEState *hle, CPUState *cs,
                                       uint64_t handle, uint64_t index,
                                       int64_t flip_arg)
{
    const ShadPS4HLEVideoBuffer *buffer;
    ShadPS4GPUFlip flip;

    if (handle != 1 || !hle->video_open ||
        index >= SHADPS4_HLE_MAX_VIDEO_BUFFERS ||
        !hle->video_buffers[index].registered) {
        hle->video_flip_failures++;
        if (!(hle->video_flip_failures & (hle->video_flip_failures - 1))) {
            warn_report("shadPS4 VideoOut flip rejected: count=%" PRIu64
                        " handle=%#" PRIx64 " open=%d index=%" PRIu64
                        " registered=%d",
                        hle->video_flip_failures, handle, hle->video_open,
                        index, index < SHADPS4_HLE_MAX_VIDEO_BUFFERS &&
                               hle->video_buffers[index].registered);
        }
        return -SHADPS4_GUEST_EINVAL;
    }
    buffer = &hle->video_buffers[index];
    flip = (ShadPS4GPUFlip) {
        .pixels_addr = buffer->address,
        .width = buffer->width,
        .height = buffer->height,
        .stride = buffer->pitch * 4,
        .format = buffer->format,
        .buffer_index = index,
        .tiling_mode = buffer->tiling_mode,
    };
    shadps4_gpu_finish(hle->gpu);
    if (!shadps4_gpu_flip(hle->gpu, cs, &flip)) {
        hle->video_flip_failures++;
        if (!(hle->video_flip_failures & (hle->video_flip_failures - 1))) {
            warn_report("shadPS4 VideoOut framebuffer rejected: count=%" PRIu64
                        " address=%#" PRIx64 " width=%u height=%u"
                        " stride=%u format=%u",
                        hle->video_flip_failures, flip.pixels_addr,
                        flip.width, flip.height, flip.stride, flip.format);
        }
        return -SHADPS4_GUEST_EFAULT;
    }
    hle->video_current_buffer = index;
    hle->video_flip_arg = flip_arg;
    for (uint32_t queue = 0; queue < SHADPS4_HLE_MAX_EQUEUES; queue++) {
        ShadPS4HLEEqueueEvent *event = shadps4_hle_equeue_find(
            hle, 0x100 + queue, 6, -13, false);

        if (event) {
            event->data = ((uint64_t)flip_arg << 16) |
                          (hle->gpu->flip_count & 0xf);
            event->triggered = true;
        }
    }
    return 0;
}

static uint64_t shadps4_hle_gnm_submit(ShadPS4HLEState *hle, CPUState *cs,
                                       uint64_t count, uint64_t addresses,
                                       uint64_t sizes, uint32_t queue)
{
    uint32_t i;

    if (!count || count > 64 || !addresses || !sizes ||
        addresses > UINT64_MAX - count * sizeof(uint64_t) ||
        sizes > UINT64_MAX - count * sizeof(uint32_t)) {
        hle->gnm_submit_failures++;
        if (!(hle->gnm_submit_failures & (hle->gnm_submit_failures - 1))) {
            warn_report("shadPS4 GNM submit arguments rejected: count=%" PRIu64
                        " addresses=%#" PRIx64 " sizes=%#" PRIx64,
                        count, addresses, sizes);
        }
        return -SHADPS4_GUEST_EINVAL;
    }
    for (i = 0; i < count; i++) {
        ShadPS4GPUSubmit submit = { .queue_id = queue };
        uint64_t address;
        uint32_t size;

        if (!shadps4_guest_rw(cs, addresses + i * sizeof(address),
                              &address, sizeof(address), false) ||
            !shadps4_guest_rw(cs, sizes + i * sizeof(size),
                              &size, sizeof(size), false)) {
            hle->gnm_submit_failures++;
            if (!(hle->gnm_submit_failures &
                  (hle->gnm_submit_failures - 1))) {
                warn_report("shadPS4 GNM submit array unreadable: count=%" PRIu64
                            " entry=%u addresses=%#" PRIx64
                            " sizes=%#" PRIx64,
                            hle->gnm_submit_failures, i, addresses, sizes);
            }
            return -SHADPS4_GUEST_EFAULT;
        }
        submit.command_addr = le64_to_cpu(address);
        size = le32_to_cpu(size);
        if (!submit.command_addr || !size || (size & 3)) {
            hle->gnm_submit_failures++;
            if (!(hle->gnm_submit_failures &
                  (hle->gnm_submit_failures - 1))) {
                warn_report("shadPS4 GNM command rejected: count=%" PRIu64
                            " entry=%u address=%#" PRIx64 " size=%u",
                            hle->gnm_submit_failures, i,
                            submit.command_addr, size);
            }
            return -SHADPS4_GUEST_EINVAL;
        }
        submit.command_dwords = size / 4;
        if (!shadps4_gpu_submit(hle->gpu, cs, &submit)) {
            hle->gnm_submit_failures++;
            if (!(hle->gnm_submit_failures &
                  (hle->gnm_submit_failures - 1))) {
                warn_report("shadPS4 GNM command buffer rejected: count=%" PRIu64
                            " entry=%u queue=%u address=%#" PRIx64
                            " dwords=%u",
                            hle->gnm_submit_failures, i, queue,
                            submit.command_addr, submit.command_dwords);
            }
            return -SHADPS4_GUEST_EINVAL;
        }
    }
    return 0;
}

static uint64_t shadps4_hle_gnm_compat_command(CPUState *cs,
                                               uint64_t command_addr,
                                               uint64_t command_dwords)
{
    g_autofree uint32_t *commands = NULL;

    if (!command_addr || command_dwords < 2 || command_dwords > 0x4001) {
        return 0;
    }
    commands = g_new0(uint32_t, command_dwords);
    commands[0] = cpu_to_le32(0xc0001000U |
                              ((command_dwords - 2) << 16));
    return shadps4_guest_rw(cs, command_addr, commands,
                            command_dwords * sizeof(*commands), true) ?
           0 : -SHADPS4_GUEST_EFAULT;
}

static uint64_t shadps4_hle_net_transfer(ShadPS4HLEState *hle, CPUState *cs,
                                         uint64_t fd, uint64_t data_addr,
                                         uint64_t size, uint64_t flags,
                                         bool send)
{
    g_autofree uint8_t *buffer = NULL;
    int64_t result;

    if (fd >= SHADPS4_HLE_MAX_FDS ||
        hle->files[fd] != SHADPS4_HLE_FD_NETWORK || !data_addr ||
        size > 16 * MiB) {
        return -SHADPS4_GUEST_EBADF;
    }
    buffer = g_malloc(size ? size : 1);
    if (send) {
        if (!shadps4_guest_rw(cs, data_addr, buffer, size, false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        result = qemu_host_network_send(hle->network_handles[fd], buffer,
                                        size, flags);
    } else {
        result = qemu_host_network_recv(hle->network_handles[fd], buffer,
                                        size, flags);
        if (result > 0 && ((uint64_t)result > size ||
            !shadps4_guest_rw(cs, data_addr, buffer, result, true))) {
            return -SHADPS4_GUEST_EFAULT;
        }
    }
    return result >= -255 ? result : -SHADPS4_GUEST_EIO;
}

static uint64_t shadps4_hle_net_datagram(
    ShadPS4HLEState *hle, CPUState *cs, uint64_t fd, uint64_t data_addr,
    uint64_t size, uint64_t flags, uint64_t address,
    uint64_t address_size_or_ptr, bool send)
{
    g_autofree uint8_t *data = NULL;
    uint8_t sockaddr[128] = { 0 };
    size_t sockaddr_size = 0;
    uint32_t guest_size = 0;
    int64_t result;

    if (!shadps4_hle_network_fd(hle, fd) || (!data_addr && size) ||
        size > 16 * MiB) {
        return -SHADPS4_GUEST_EBADF;
    }
    data = g_malloc(size ? size : 1);
    if (send) {
        if ((size && !shadps4_guest_rw(cs, data_addr, data, size, false)) ||
            !shadps4_hle_read_sockaddr(cs, address, address_size_or_ptr,
                                       sockaddr, &sockaddr_size)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        result = qemu_host_network_send_to(hle->network_handles[fd], data,
                                           size, flags, sockaddr,
                                           sockaddr_size);
    } else {
        if ((address || address_size_or_ptr) &&
            (!address || !address_size_or_ptr ||
             !shadps4_guest_rw(cs, address_size_or_ptr, &guest_size,
                               sizeof(guest_size), false))) {
            return -SHADPS4_GUEST_EFAULT;
        }
        guest_size = le32_to_cpu(guest_size);
        sockaddr_size = MIN((size_t)guest_size, sizeof(sockaddr));
        result = qemu_host_network_recv_from(
            hle->network_handles[fd], data, size, flags,
            address ? sockaddr : NULL, address ? &sockaddr_size : NULL);
        if (result > 0 && ((uint64_t)result > size ||
            !shadps4_guest_rw(cs, data_addr, data, result, true))) {
            return -SHADPS4_GUEST_EFAULT;
        }
        if (address && (sockaddr_size > guest_size ||
            !shadps4_guest_rw(cs, address, sockaddr, sockaddr_size, true))) {
            return -SHADPS4_GUEST_EFAULT;
        }
        if (address_size_or_ptr) {
            guest_size = cpu_to_le32(sockaddr_size);
            if (!shadps4_guest_rw(cs, address_size_or_ptr, &guest_size,
                                  sizeof(guest_size), true)) {
                return -SHADPS4_GUEST_EFAULT;
            }
        }
    }
    return shadps4_hle_network_result(hle, result);
}

static uint64_t shadps4_hle_net_get_name(ShadPS4HLEState *hle, CPUState *cs,
                                         uint64_t fd, uint64_t address,
                                         uint64_t size_ptr, bool peer)
{
    uint8_t buffer[128] = { 0 };
    uint32_t guest_size;
    size_t size;
    int result;

    if (!shadps4_hle_network_fd(hle, fd)) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (!address || !size_ptr ||
        !shadps4_guest_rw(cs, size_ptr, &guest_size,
                          sizeof(guest_size), false)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    size = MIN((size_t)le32_to_cpu(guest_size), sizeof(buffer));
    result = qemu_host_network_get_name(hle->network_handles[fd], peer,
                                        buffer, &size);
    if (result < 0) {
        return shadps4_hle_network_result(hle, result);
    }
    if (size > le32_to_cpu(guest_size) ||
        !shadps4_guest_rw(cs, address, buffer, size, true)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    guest_size = cpu_to_le32(size);
    return shadps4_guest_rw(cs, size_ptr, &guest_size,
                            sizeof(guest_size), true) ? 0 :
           -SHADPS4_GUEST_EFAULT;
}

static uint64_t shadps4_hle_net_option(ShadPS4HLEState *hle, CPUState *cs,
                                       uint64_t fd, uint64_t level,
                                       uint64_t option, uint64_t value_addr,
                                       uint64_t size_or_ptr, bool set)
{
    g_autofree uint8_t *value = NULL;
    uint32_t guest_size;
    size_t size;
    int result;

    if (!shadps4_hle_network_fd(hle, fd)) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (set) {
        size = size_or_ptr;
    } else if (!size_or_ptr || !shadps4_guest_rw(
                   cs, size_or_ptr, &guest_size, sizeof(guest_size), false)) {
        return -SHADPS4_GUEST_EFAULT;
    } else {
        size = le32_to_cpu(guest_size);
    }
    if (size > 64 * KiB || (!value_addr && size)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    value = g_malloc(size ? size : 1);
    if (set && size && !shadps4_guest_rw(cs, value_addr, value, size, false)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    result = set ? qemu_host_network_set_option(
                       hle->network_handles[fd], level, option, value, size) :
                   qemu_host_network_get_option(
                       hle->network_handles[fd], level, option, value, &size);
    if (result < 0) {
        return shadps4_hle_network_result(hle, result);
    }
    if (!set) {
        if (size > le32_to_cpu(guest_size) || (size && !shadps4_guest_rw(
                cs, value_addr, value, size, true))) {
            return -SHADPS4_GUEST_EFAULT;
        }
        guest_size = cpu_to_le32(size);
        if (!shadps4_guest_rw(cs, size_or_ptr, &guest_size,
                              sizeof(guest_size), true)) {
            return -SHADPS4_GUEST_EFAULT;
        }
    }
    return 0;
}

static uint64_t shadps4_hle_net_message(ShadPS4HLEState *hle, CPUState *cs,
                                        uint64_t fd, uint64_t message_addr,
                                        uint64_t flags, bool send)
{
    ShadPS4GuestMsgHdr message;
    g_autofree uint8_t *buffer = NULL;
    uint64_t total = 0;
    uint64_t done = 0;
    uint32_t count;
    uint32_t i;
    int64_t result;

    if (!shadps4_hle_network_fd(hle, fd) || !message_addr ||
        !shadps4_guest_rw(cs, message_addr, &message, sizeof(message), false)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    count = le32_to_cpu(message.iov_count);
    if (count > 1024 || le64_to_cpu(message.control) ||
        le32_to_cpu(message.control_length)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    for (i = 0; i < count; i++) {
        ShadPS4GuestIovec iov;

        if (!shadps4_guest_rw(cs, le64_to_cpu(message.iov) + i * sizeof(iov),
                              &iov, sizeof(iov), false) ||
            le64_to_cpu(iov.length) > 16 * MiB - total) {
            return -SHADPS4_GUEST_EFAULT;
        }
        total += le64_to_cpu(iov.length);
    }
    buffer = g_malloc(total ? total : 1);
    if (send) {
        for (i = 0; i < count; i++) {
            ShadPS4GuestIovec iov;
            uint64_t length;

            shadps4_guest_rw(cs, le64_to_cpu(message.iov) + i * sizeof(iov),
                             &iov, sizeof(iov), false);
            length = le64_to_cpu(iov.length);
            if (length && !shadps4_guest_rw(cs, le64_to_cpu(iov.base),
                                            buffer + done, length, false)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            done += length;
        }
    }
    if (le64_to_cpu(message.name)) {
        uint8_t sockaddr[128];
        size_t sockaddr_size = le32_to_cpu(message.name_length);

        if (sockaddr_size > sizeof(sockaddr) || !shadps4_guest_rw(
                cs, le64_to_cpu(message.name), sockaddr, sockaddr_size,
                false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        result = send ? qemu_host_network_send_to(
                            hle->network_handles[fd], buffer, total, flags,
                            sockaddr, sockaddr_size) :
                        qemu_host_network_recv_from(
                            hle->network_handles[fd], buffer, total, flags,
                            sockaddr, &sockaddr_size);
        if (!send && result >= 0) {
            if (sockaddr_size > le32_to_cpu(message.name_length) ||
                !shadps4_guest_rw(cs, le64_to_cpu(message.name), sockaddr,
                                  sockaddr_size, true)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            message.name_length = cpu_to_le32(sockaddr_size);
        }
    } else {
        result = send ? qemu_host_network_send(hle->network_handles[fd],
                                                buffer, total, flags) :
                        qemu_host_network_recv(hle->network_handles[fd],
                                                buffer, total, flags);
    }
    if (!send && result > 0) {
        uint64_t remaining = result;

        if ((uint64_t)result > total) {
            return -SHADPS4_GUEST_EIO;
        }
        done = 0;
        for (i = 0; i < count && remaining; i++) {
            ShadPS4GuestIovec iov;
            uint64_t length;

            shadps4_guest_rw(cs, le64_to_cpu(message.iov) + i * sizeof(iov),
                             &iov, sizeof(iov), false);
            length = MIN(le64_to_cpu(iov.length), remaining);
            if (length && !shadps4_guest_rw(cs, le64_to_cpu(iov.base),
                                            buffer + done, length, true)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            done += length;
            remaining -= length;
        }
        message.flags = 0;
        if (!shadps4_guest_rw(cs, message_addr, &message,
                              sizeof(message), true)) {
            return -SHADPS4_GUEST_EFAULT;
        }
    }
    return shadps4_hle_network_result(hle, result);
}

static uint32_t shadps4_ajm_chunk_header(uint32_t ident, uint32_t payload)
{
    return cpu_to_le32((ident & 0x3f) | ((payload & 0xfffff) << 6));
}

static uint64_t shadps4_hle_guest_copy(CPUState *cs, uint64_t dest,
                                       uint64_t source, uint64_t size);
static uint64_t shadps4_hle_write_u64(CPUState *cs, uint64_t address,
                                      uint64_t value);

static bool shadps4_ajm_write_buffer_chunk(CPUState *cs, uint64_t address,
                                           uint32_t ident, uint32_t size,
                                           uint64_t buffer)
{
    uint8_t chunk[16] = { 0 };
    uint32_t value32 = shadps4_ajm_chunk_header(ident, 0);
    uint64_t value64 = cpu_to_le64(buffer);

    memcpy(chunk, &value32, sizeof(value32));
    value32 = cpu_to_le32(size);
    memcpy(chunk + 4, &value32, sizeof(value32));
    memcpy(chunk + 8, &value64, sizeof(value64));
    return shadps4_guest_rw(cs, address, chunk, sizeof(chunk), true);
}

static bool shadps4_ajm_write_flags_chunk(CPUState *cs, uint64_t address,
                                          uint32_t ident, uint64_t flags)
{
    uint32_t chunk[2];

    chunk[0] = shadps4_ajm_chunk_header(ident, flags >> 32);
    chunk[1] = cpu_to_le32(flags);
    return shadps4_guest_rw(cs, address, chunk, sizeof(chunk), true);
}

static bool shadps4_ajm_write_job_header(CPUState *cs, uint64_t address,
                                         uint32_t ident, uint32_t instance,
                                         uint32_t size)
{
    uint32_t job[2] = {
        shadps4_ajm_chunk_header(ident, instance), cpu_to_le32(size),
    };

    return shadps4_guest_rw(cs, address, job, sizeof(job), true);
}

static uint64_t shadps4_hle_ajm_job(CPUState *cs, uint32_t number,
                                    uint64_t a0, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t args[10] = { a0, a1, a2, a3, a4, a5 };
    uint64_t current;
    uint64_t flags;
    uint64_t return_address;
    uint32_t i;

    if (!a0) {
        return 0;
    }
    if (number == SHADPS4_HLE_AJM_JOB_INLINE) {
        uint64_t aligned;

        if ((!a1 && a2) || !a3 || a2 > 16 * MiB) {
            return 0;
        }
        aligned = QEMU_ALIGN_UP(a2, 8);
        if (!shadps4_ajm_write_job_header(cs, a0, 7, 0, aligned) ||
            (int64_t)shadps4_hle_write_u64(cs, a3, a0 + 8) < 0 ||
            (a2 && !shadps4_hle_guest_copy(cs, a0 + 8, a1, a2))) {
            return 0;
        }
        if (aligned > a2) {
            uint64_t zero = 0;

            if (!shadps4_guest_rw(cs, a0 + 8 + a2, &zero,
                                  aligned - a2, true)) {
                return 0;
            }
        }
        return a0 + 8 + aligned;
    }

    for (i = 6; i < ARRAY_SIZE(args); i++) {
        if (!shadps4_hle_argument(cs, i, &args[i])) {
            return 0;
        }
    }

    current = a0 + 8;
    return_address = args[number == SHADPS4_HLE_AJM_JOB_CONTROL ? 7 : 9];
    if (return_address) {
        if (!shadps4_ajm_write_buffer_chunk(cs, current, 6, 0,
                                            return_address)) {
            return 0;
        }
        current += 16;
    }
    if (number == SHADPS4_HLE_AJM_JOB_CONTROL) {
        flags = a2 & (a1 == 0x80000 ? 0x00000000c0018007ULL :
                                      0x000060000000e7ffULL);
        if (a4 > UINT32_MAX || args[6] > UINT32_MAX ||
            !shadps4_ajm_write_buffer_chunk(cs, current, 2, a4, a3)) {
            return 0;
        }
        current += 16;
        if (!shadps4_ajm_write_flags_chunk(cs, current, 3, flags)) {
            return 0;
        }
        current += 8;
        if (!shadps4_ajm_write_buffer_chunk(cs, current, 18, args[6], a5)) {
            return 0;
        }
        current += 16;
    } else if (number == SHADPS4_HLE_AJM_JOB_RUN) {
        flags = a2 & 0x0000e00000001fffULL;
        if (a4 > UINT32_MAX || args[6] > UINT32_MAX ||
            args[8] > UINT32_MAX ||
            !shadps4_ajm_write_buffer_chunk(cs, current, 1, a4, a3)) {
            return 0;
        }
        current += 16;
        if (!shadps4_ajm_write_flags_chunk(cs, current, 4, flags)) {
            return 0;
        }
        current += 8;
        if (!shadps4_ajm_write_buffer_chunk(cs, current, 17, args[6], a5)) {
            return 0;
        }
        current += 16;
        if (!shadps4_ajm_write_buffer_chunk(cs, current, 18, args[8],
                                            args[7])) {
            return 0;
        }
        current += 16;
    } else {
        flags = a2 & 0x0000e00000001fffULL;
        if (a4 > 1024 || args[6] > 1024) {
            return 0;
        }
        for (i = 0; i < a4; i++) {
            ShadPS4GuestIovec buffer;

            if (!shadps4_guest_rw(cs, a3 + i * sizeof(buffer), &buffer,
                                  sizeof(buffer), false) ||
                le64_to_cpu(buffer.length) > UINT32_MAX ||
                !shadps4_ajm_write_buffer_chunk(
                    cs, current, 1, le64_to_cpu(buffer.length),
                    le64_to_cpu(buffer.base))) {
                return 0;
            }
            current += 16;
        }
        if (!shadps4_ajm_write_flags_chunk(cs, current, 4, flags)) {
            return 0;
        }
        current += 8;
        for (i = 0; i < args[6]; i++) {
            ShadPS4GuestIovec buffer;

            if (!shadps4_guest_rw(cs, a5 + i * sizeof(buffer), &buffer,
                                  sizeof(buffer), false) ||
                le64_to_cpu(buffer.length) > UINT32_MAX ||
                !shadps4_ajm_write_buffer_chunk(
                    cs, current, 17, le64_to_cpu(buffer.length),
                    le64_to_cpu(buffer.base))) {
                return 0;
            }
            current += 16;
        }
        if (args[8] > UINT32_MAX ||
            !shadps4_ajm_write_buffer_chunk(cs, current, 18, args[8],
                                            args[7])) {
            return 0;
        }
        current += 16;
    }
    return shadps4_ajm_write_job_header(cs, a0, 0, a1,
                                        current - a0 - 8) ? current : 0;
}

enum {
    SHADPS4_SERVICE_NET_POOL = 1,
    SHADPS4_SERVICE_NET_EPOLL,
    SHADPS4_SERVICE_NET_RESOLVER,
    SHADPS4_SERVICE_HTTP_CONTEXT,
    SHADPS4_SERVICE_HTTP_TEMPLATE,
    SHADPS4_SERVICE_HTTP_CONNECTION,
    SHADPS4_SERVICE_HTTP_REQUEST,
    SHADPS4_SERVICE_HTTP_EPOLL,
    SHADPS4_SERVICE_WEBAPI_CONTEXT,
    SHADPS4_SERVICE_WEBAPI_USER,
    SHADPS4_SERVICE_WEBAPI_HANDLE,
    SHADPS4_SERVICE_WEBAPI_FILTER,
    SHADPS4_SERVICE_WEBAPI_CALLBACK,
    SHADPS4_SERVICE_AJM_CONTEXT,
    SHADPS4_SERVICE_AJM_INSTANCE,
    SHADPS4_SERVICE_AJM_BATCH,
    SHADPS4_SERVICE_NGS2_SYSTEM,
    SHADPS4_SERVICE_NGS2_RACK,
    SHADPS4_SERVICE_NGS2_REPORT,
    SHADPS4_SERVICE_AVPLAYER,
    SHADPS4_SERVICE_LIBC_MUTEX,
    SHADPS4_SERVICE_PTHREAD_ATTR,
    SHADPS4_SERVICE_MUTEX_ATTR,
    SHADPS4_SERVICE_MUTEX,
    SHADPS4_SERVICE_COND_ATTR,
    SHADPS4_SERVICE_COND,
    SHADPS4_SERVICE_RWLOCK_ATTR,
    SHADPS4_SERVICE_RWLOCK,
    SHADPS4_SERVICE_SEM,
    SHADPS4_SERVICE_KERNEL_SEMA,
    SHADPS4_SERVICE_THREAD,
    SHADPS4_SERVICE_EVENT_FLAG,
    SHADPS4_SERVICE_MOVE,
    SHADPS4_SERVICE_ZLIB_REQUEST,
    SHADPS4_SERVICE_PNG_ENCODER,
    SHADPS4_SERVICE_FONT_LIBRARY,
    SHADPS4_SERVICE_FONT_RENDERER,
    SHADPS4_SERVICE_FONT_HANDLE,
    SHADPS4_SERVICE_NP_MATCHING_CONTEXT,
    SHADPS4_SERVICE_NP_SCORE_TITLE,
    SHADPS4_SERVICE_NP_SCORE_REQUEST,
    SHADPS4_SERVICE_NP_TUS_TITLE,
    SHADPS4_SERVICE_NP_TUS_REQUEST,
    SHADPS4_SERVICE_NP_SIGNALING_CONTEXT,
    SHADPS4_SERVICE_USBD_TRANSFER,
    SHADPS4_SERVICE_NP_MANAGER_REQUEST,
    SHADPS4_SERVICE_NP_MANAGER_CALLBACK,
    SHADPS4_SERVICE_CAMERA_HANDLE,
    SHADPS4_SERVICE_HMD_HANDLE,
    SHADPS4_SERVICE_AUDIO3D_PORT,
    SHADPS4_SERVICE_AUDIO3D_OBJECT,
    SHADPS4_SERVICE_WEBAPI_PUSH_CONTEXT,
    SHADPS4_SERVICE_NP_AUTH_REQUEST,
    SHADPS4_SERVICE_IME_SESSION,
    SHADPS4_SERVICE_IME_KEYBOARD,
    SHADPS4_SERVICE_TROPHY_CONTEXT,
    SHADPS4_SERVICE_TROPHY_HANDLE,
    SHADPS4_SERVICE_VIDEODEC2_DECODER,
    SHADPS4_SERVICE_SSL2_CONNECTION,
    SHADPS4_SERVICE_PNG_DECODER,
    SHADPS4_SERVICE_VIDEODEC_DECODER,
};

static int shadps4_hle_service_alloc(ShadPS4HLEState *hle, uint8_t type,
                                     uint32_t parent)
{
    uint32_t i;

    for (i = 1; i < SHADPS4_HLE_MAX_SERVICE_OBJECTS; i++) {
        if (!hle->service_objects[i]) {
            hle->service_objects[i] = type;
            hle->service_parents[i] = parent;
            hle->service_host_handles[i] = -1;
            return 0x400 + i;
        }
    }
    return -SHADPS4_GUEST_ENOMEM;
}

static bool shadps4_hle_service_is(ShadPS4HLEState *hle, uint64_t handle,
                                   uint8_t type)
{
    uint64_t slot = handle - 0x400;

    return handle > 0x400 && slot < SHADPS4_HLE_MAX_SERVICE_OBJECTS &&
           hle->service_objects[slot] == type;
}

static uint64_t shadps4_hle_service_delete(ShadPS4HLEState *hle,
                                           uint64_t handle, uint8_t type)
{
    uint64_t slot = handle - 0x400;
    uint32_t i;

    if (!shadps4_hle_service_is(hle, handle, type)) {
        return -SHADPS4_GUEST_EBADF;
    }
    if (hle->service_host_handles[slot] >= 0) {
        if (type == SHADPS4_SERVICE_HTTP_REQUEST) {
            qemu_host_http_close(hle->service_host_handles[slot]);
        } else if (type == SHADPS4_SERVICE_AVPLAYER) {
            qemu_host_media_close(hle->service_host_handles[slot]);
        }
    }
    hle->service_objects[slot] = 0;
    hle->service_parents[slot] = 0;
    hle->service_nonblock[slot] = false;
    hle->service_user_data[slot] = 0;
    hle->service_guest_addr[slot] = 0;
    hle->service_value[slot] = 0;
    hle->service_aux_value[slot] = 0;
    hle->service_content_length[slot] = 0;
    hle->service_host_handles[slot] = -1;
    hle->service_active[slot] = false;
    memset(&hle->pthread_attrs[slot], 0,
           sizeof(hle->pthread_attrs[slot]));
    for (i = 1; i < SHADPS4_HLE_MAX_SERVICE_OBJECTS; i++) {
        if (hle->service_objects[i] &&
            hle->service_parents[i] == handle) {
            shadps4_hle_service_delete(hle, 0x400 + i,
                                       hle->service_objects[i]);
        }
    }
    return 0;
}

static uint64_t shadps4_hle_guest_copy(CPUState *cs, uint64_t dest,
                                       uint64_t source, uint64_t size)
{
    g_autofree uint8_t *buffer = NULL;

    if ((!dest || !source) && size) {
        return 0;
    }
    if (size > 64 * MiB) {
        return 0;
    }
    buffer = g_malloc(size ? size : 1);
    if (size && (!shadps4_guest_rw(cs, source, buffer, size, false) ||
                 !shadps4_guest_rw(cs, dest, buffer, size, true))) {
        return 0;
    }
    return dest;
}

static bool shadps4_hle_guest_string_length(CPUState *cs, uint64_t address,
                                            uint64_t limit, uint64_t *length)
{
    uint8_t byte;
    uint64_t i;

    if (!address) {
        return false;
    }
    limit = MIN(limit, 16 * MiB);
    for (i = 0; i < limit; i++) {
        if (!shadps4_guest_rw(cs, address + i, &byte, 1, false)) {
            return false;
        }
        if (!byte) {
            *length = i;
            return true;
        }
    }
    return false;
}

#define SHADPS4_LIBC_FILE_STRIDE 512
#define SHADPS4_LIBC_FILE_FIRST 5

static int shadps4_hle_libc_file_index(const ShadPS4HLEState *hle,
                                       uint64_t address)
{
    uint64_t offset;
    uint64_t index;

    if (!hle->libc_file_arena || address < hle->libc_file_arena) {
        return -1;
    }
    offset = address - hle->libc_file_arena;
    if (offset % SHADPS4_LIBC_FILE_STRIDE) {
        return -1;
    }
    index = offset / SHADPS4_LIBC_FILE_STRIDE;
    return index < ARRAY_SIZE(hle->libc_file_used) &&
           hle->libc_file_used[index] ? index : -1;
}

static bool shadps4_hle_libc_file_header(CPUState *cs, uint64_t address,
                                         uint16_t mode, uint8_t index,
                                         int32_t fd)
{
    uint8_t header[8] = { 0 };

    stw_le_p(header, mode);
    header[2] = index;
    stl_le_p(header + 4, fd);
    return shadps4_guest_rw(cs, address, header, sizeof(header), true);
}

static uint64_t shadps4_hle_libc_file_alloc(ShadPS4HLEState *hle,
                                             CPUState *cs)
{
    uint32_t index;

    if (!hle->libc_file_arena) {
        uint64_t address = shadps4_hle_mmap(hle, cs, 0, 2 * MiB, 3);

        if ((int64_t)address < 0) {
            return 0;
        }
        hle->libc_file_arena = address;
    }
    for (index = SHADPS4_LIBC_FILE_FIRST;
         index < ARRAY_SIZE(hle->libc_file_used); index++) {
        uint64_t address;
        uint8_t zero[SHADPS4_LIBC_FILE_STRIDE] = { 0 };

        if (hle->libc_file_used[index]) {
            continue;
        }
        address = hle->libc_file_arena +
                  index * SHADPS4_LIBC_FILE_STRIDE;
        if (!shadps4_guest_rw(cs, address, zero, sizeof(zero), true) ||
            !shadps4_hle_libc_file_header(cs, address, 0x80, index, -1)) {
            return 0;
        }
        hle->libc_file_used[index] = true;
        hle->libc_file_fd[index] = -1;
        return address;
    }
    return 0;
}

static void shadps4_hle_libc_file_free(ShadPS4HLEState *hle, CPUState *cs,
                                        uint64_t address, int index)
{
    uint8_t zero[SHADPS4_LIBC_FILE_STRIDE] = { 0 };

    hle->libc_file_used[index] = false;
    hle->libc_file_fd[index] = -1;
    shadps4_guest_rw(cs, address, zero, sizeof(zero), true);
}

static bool shadps4_hle_libc_mode(CPUState *cs, uint64_t mode_address,
                                  uint16_t *file_mode, uint32_t *open_flags)
{
    char mode[8];
    bool update;

    if (!shadps4_guest_read_string(cs, mode_address, mode, sizeof(mode))) {
        return false;
    }
    update = strchr(mode, '+') != NULL;
    switch (mode[0]) {
    case 'r':
        *file_mode = 0x81;
        *open_flags = update ? SHADPS4_O_RDWR : 0;
        break;
    case 'w':
        *file_mode = 0x9a;
        *open_flags = (update ? SHADPS4_O_RDWR : SHADPS4_O_WRONLY) |
                      SHADPS4_O_CREAT | SHADPS4_O_TRUNC;
        break;
    case 'a':
        *file_mode = 0x96;
        *open_flags = (update ? SHADPS4_O_RDWR : SHADPS4_O_WRONLY) |
                      SHADPS4_O_CREAT | SHADPS4_O_APPEND;
        break;
    default:
        return false;
    }
    if (update) {
        *file_mode |= 3;
    }
    return true;
}

static uint64_t shadps4_hle_libc_prepare(ShadPS4HLEState *hle, CPUState *cs,
                                         uint64_t path, uint64_t mode_address,
                                         uint64_t file, int64_t supplied_fd,
                                         uint64_t private_mode)
{
    uint32_t flags;
    uint16_t mode;
    uint64_t result;
    int index = shadps4_hle_libc_file_index(hle, file);

    if (index < 0 || !shadps4_hle_libc_mode(cs, mode_address, &mode,
                                            &flags)) {
        return 0;
    }
    result = !path && supplied_fd >= 0 ? supplied_fd :
             shadps4_hle_open(hle, cs, path, flags,
                              private_mode == 0x55 ? 0600 : 0666);
    if ((int64_t)result < 0 || result >= SHADPS4_HLE_MAX_FDS) {
        shadps4_hle_libc_file_header(cs, file, 0x80, index, -1);
        return 0;
    }
    hle->libc_file_fd[index] = result;
    if (!shadps4_hle_libc_file_header(cs, file, mode, index, result)) {
        if (path) {
            shadps4_hle_close(hle, result);
        }
        hle->libc_file_fd[index] = -1;
        return 0;
    }
    return file;
}

static uint64_t shadps4_hle_libc_format(CPUState *cs, uint64_t output,
                                         uint64_t capacity,
                                         uint64_t format_address)
{
    CPUX86State *env = &X86_CPU(cs)->env;
    g_autoptr(GString) result = g_string_sized_new(128);
    char format[64 * KiB];
    unsigned int argument = 3;
    unsigned int floating_argument = 0;
    size_t i;

    if (!shadps4_guest_read_string(cs, format_address, format,
                                    sizeof(format))) {
        return -SHADPS4_GUEST_EFAULT;
    }
    for (i = 0; format[i]; i++) {
        char conversion[32] = "%";
        size_t conversion_length = 1;
        uint64_t value;
        char specifier;
        bool long_double = false;

        if (format[i] != '%') {
            g_string_append_c(result, format[i]);
            continue;
        }
        if (format[i + 1] == '%') {
            g_string_append_c(result, '%');
            i++;
            continue;
        }
        while (format[++i] && strchr("-+ #0'123456789.*hljztL", format[i])) {
            if (format[i] == '*') {
                return -SHADPS4_GUEST_EINVAL;
            }
            if (!strchr("hljztL", format[i]) &&
                conversion_length + 4 < sizeof(conversion)) {
                conversion[conversion_length++] = format[i];
            }
            long_double |= format[i] == 'L';
        }
        specifier = format[i];
        if (!specifier || !strchr("diuoxXpcsfFeEgGaA", specifier)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        if (strchr("fFeEgGaA", specifier)) {
            value = 0;
        } else if (!shadps4_hle_argument(cs, argument++, &value)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        conversion[conversion_length++] = specifier;
        conversion[conversion_length] = 0;
        switch (specifier) {
        case 'd':
        case 'i':
            conversion[conversion_length - 1] = 'l';
            conversion[conversion_length++] = 'l';
            conversion[conversion_length++] = specifier;
            conversion[conversion_length] = 0;
            g_string_append_printf(result, conversion, (int64_t)value);
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            conversion[conversion_length - 1] = 'l';
            conversion[conversion_length++] = 'l';
            conversion[conversion_length++] = specifier;
            conversion[conversion_length] = 0;
            g_string_append_printf(result, conversion, value);
            break;
        case 'p':
            g_string_append_printf(result, conversion, (void *)(uintptr_t)value);
            break;
        case 'c':
            g_string_append_printf(result, conversion, (int)value);
            break;
        case 's': {
            char string[64 * KiB];

            if (!value) {
                g_string_append(result, "(null)");
            } else if (!shadps4_guest_read_string(cs, value, string,
                                                   sizeof(string))) {
                return -SHADPS4_GUEST_EFAULT;
            } else {
                g_string_append_printf(result, conversion, string);
            }
            break;
        }
        default: {
            double floating;
            uint64_t bits;

            if (long_double || floating_argument >= 8) {
                return -SHADPS4_GUEST_ENOSYS;
            }
            bits = env->xmm_regs[floating_argument++].ZMM_Q(0);
            memcpy(&floating, &bits, sizeof(floating));
            g_string_append_printf(result, conversion, floating);
            break;
        }
        }
        if (result->len > 16 * MiB) {
            return -SHADPS4_GUEST_EINVAL;
        }
    }
    if (capacity && output) {
        size_t copy = MIN((uint64_t)result->len, capacity - 1);
        char zero = 0;

        if ((copy && !shadps4_guest_rw(cs, output, result->str, copy, true)) ||
            !shadps4_guest_rw(cs, output + copy, &zero, 1, true)) {
            return -SHADPS4_GUEST_EFAULT;
        }
    } else if (capacity && !output) {
        return -SHADPS4_GUEST_EFAULT;
    }
    return result->len;
}

static int shadps4_hle_guest_string_compare(CPUState *cs, uint64_t left,
                                            uint64_t right, uint64_t limit)
{
    uint8_t a;
    uint8_t b;
    uint64_t i;

    if (!left || !right) {
        return left == right ? 0 : left ? 1 : -1;
    }
    limit = MIN(limit, 16 * MiB);
    for (i = 0; i < limit; i++) {
        if (!shadps4_guest_rw(cs, left + i, &a, 1, false) ||
            !shadps4_guest_rw(cs, right + i, &b, 1, false)) {
            return 0;
        }
        if (a != b || !a) {
            return (int)a - (int)b;
        }
    }
    return 0;
}

static uint64_t shadps4_hle_write_u64(CPUState *cs, uint64_t address,
                                      uint64_t value)
{
    value = cpu_to_le64(value);
    return address && shadps4_guest_rw(cs, address, &value,
                                       sizeof(value), true) ?
           0 : -SHADPS4_GUEST_EFAULT;
}

static bool shadps4_hle_object_handle(ShadPS4HLEState *hle, CPUState *cs,
                                      uint64_t address, uint8_t type,
                                      uint64_t *handle)
{
    uint64_t value;

    if (!address || !shadps4_guest_rw(cs, address, &value,
                                      sizeof(value), false)) {
        return false;
    }
    value = le64_to_cpu(value);
    if (!shadps4_hle_service_is(hle, value, type)) {
        uint32_t i;

        for (i = 1; i < SHADPS4_HLE_MAX_SERVICE_OBJECTS; i++) {
            if (hle->service_objects[i] == type &&
                hle->service_guest_addr[i] == value) {
                *handle = 0x400 + i;
                return true;
            }
        }
        return false;
    }
    *handle = value;
    return true;
}

static uint64_t shadps4_hle_object_create(ShadPS4HLEState *hle,
                                          CPUState *cs, uint64_t address,
                                          uint8_t type, uint32_t parent)
{
    int handle;
    uint64_t guest_object = 0;
    uint64_t stored_value;

    if (!address) {
        return SHADPS4_GUEST_EINVAL;
    }
    handle = shadps4_hle_service_alloc(hle, type, parent);
    if (handle < 0) {
        return -handle;
    }
    if (type == SHADPS4_SERVICE_MUTEX_ATTR ||
        type == SHADPS4_SERVICE_MUTEX) {
        uint32_t default_type = cpu_to_le32(1);
        uint64_t slot = handle - 0x400;

        guest_object = hle->service_guest_base +
                       slot * SHADPS4_HLE_SERVICE_GUEST_STRIDE;
        if (!hle->service_guest_base ||
            !shadps4_hle_heap_zero(cs, guest_object,
                                   SHADPS4_HLE_SERVICE_GUEST_STRIDE) ||
            !shadps4_guest_rw(cs, guest_object +
                              (type == SHADPS4_SERVICE_MUTEX ? 0x20 : 0),
                              &default_type, sizeof(default_type), true)) {
            shadps4_hle_service_delete(hle, handle, type);
            return SHADPS4_GUEST_ENOMEM;
        }
        hle->service_guest_addr[handle - 0x400] = guest_object;
    }
    stored_value = guest_object ?: (uint64_t)handle;
    if (shadps4_hle_write_u64(cs, address, stored_value)) {
        shadps4_hle_service_delete(hle, handle, type);
        return SHADPS4_GUEST_EFAULT;
    }
    return 0;
}

static uint64_t shadps4_hle_object_destroy(ShadPS4HLEState *hle,
                                           CPUState *cs, uint64_t address,
                                           uint8_t type)
{
    uint64_t handle;
    uint64_t zero = 0;

    if (!shadps4_hle_object_handle(hle, cs, address, type, &handle)) {
        return SHADPS4_GUEST_EINVAL;
    }
    shadps4_hle_service_delete(hle, handle, type);
    return shadps4_guest_rw(cs, address, &zero, sizeof(zero), true) ?
           0 : SHADPS4_GUEST_EFAULT;
}

static uint64_t shadps4_hle_write_u32(CPUState *cs, uint64_t address,
                                      uint32_t value)
{
    value = cpu_to_le32(value);
    return address && shadps4_guest_rw(cs, address, &value,
                                       sizeof(value), true) ?
           0 : SHADPS4_GUEST_EFAULT;
}

static uint64_t shadps4_hle_http_string(CPUState *cs, uint64_t address)
{
    char value[2048];

    return address && shadps4_guest_read_string(cs, address, value,
                                                 sizeof(value)) ?
           0 : -SHADPS4_GUEST_EFAULT;
}

static int64_t shadps4_hle_host_result(int64_t result)
{
    if (result >= 0) {
        return result;
    }
    switch (-result) {
    case ENOENT: return -SHADPS4_GUEST_ENOENT;
    case EIO: return -SHADPS4_GUEST_EIO;
    case EBADF: return -SHADPS4_GUEST_EBADF;
    case ENOMEM: return -SHADPS4_GUEST_ENOMEM;
    case EFAULT: return -SHADPS4_GUEST_EFAULT;
    case EINVAL: return -SHADPS4_GUEST_EINVAL;
    case EBUSY: return -SHADPS4_GUEST_EBUSY;
    case EACCES: return -SHADPS4_GUEST_EACCES;
    case EROFS: return -SHADPS4_GUEST_EROFS;
    case EAGAIN: return -SHADPS4_GUEST_EAGAIN;
    case ENOSYS: return -SHADPS4_GUEST_ENOSYS;
    default: return -SHADPS4_GUEST_EIO;
    }
}

static uint64_t shadps4_hle_arena_alloc(ShadPS4HLEState *hle, CPUState *cs,
                                        uint64_t *arena, uint32_t *offset,
                                        uint32_t capacity, size_t size)
{
    uint32_t aligned;
    uint64_t address;

    if (!size || size > capacity) {
        return 0;
    }
    if (!*arena) {
        address = shadps4_hle_mmap(hle, cs, 0, capacity, 3);
        if ((int64_t)address < 0) {
            return 0;
        }
        *arena = address;
    }
    aligned = ROUND_UP(*offset, 16);
    if (size > capacity - aligned) {
        aligned = 0;
    }
    *offset = aligned + size;
    return *arena + aligned;
}

static int shadps4_hle_http_create_request_values(ShadPS4HLEState *hle,
                                                  uint64_t parent,
                                                  const char *method,
                                                  const char *url,
                                                  uint64_t content_length)
{
    int64_t host_handle;
    int handle;
    int ret;

    ret = qemu_host_http_create_request(method, url, content_length,
                                        &host_handle);
    if (ret < 0) {
        return shadps4_hle_host_result(ret);
    }
    handle = shadps4_hle_service_alloc(hle, SHADPS4_SERVICE_HTTP_REQUEST,
                                       parent);
    if (handle < 0) {
        qemu_host_http_close(host_handle);
        return handle;
    }
    hle->service_host_handles[handle - 0x400] = host_handle;
    return handle;
}

static int shadps4_hle_http_create_request(ShadPS4HLEState *hle,
                                           CPUState *cs, uint64_t parent,
                                           uint64_t method_addr,
                                           uint64_t url_addr,
                                           uint64_t content_length)
{
    char method[64];
    char url[2048];

    if (!shadps4_guest_read_string(cs, method_addr, method, sizeof(method)) ||
        !shadps4_guest_read_string(cs, url_addr, url, sizeof(url))) {
        return -SHADPS4_GUEST_EFAULT;
    }
    return shadps4_hle_http_create_request_values(
        hle, parent, method, url, content_length);
}

static int shadps4_hle_webapi_create_request(ShadPS4HLEState *hle,
                                              CPUState *cs, uint64_t parent,
                                              uint64_t group_address,
                                              uint64_t path_address,
                                              const char *method,
                                              uint64_t content_length,
                                              uint64_t output,
                                              uint32_t invalid_argument)
{
    char group[512];
    char path[1024];
    char url[2048];
    uint64_t handle_le;
    int handle;

    if (!shadps4_hle_service_is(hle, parent,
                                SHADPS4_SERVICE_WEBAPI_USER) || !output ||
        !shadps4_guest_read_string(cs, group_address, group, sizeof(group)) ||
        !shadps4_guest_read_string(cs, path_address, path, sizeof(path))) {
        return invalid_argument;
    }
    if (g_str_has_prefix(group, "http://") ||
        g_str_has_prefix(group, "https://")) {
        if (g_snprintf(url, sizeof(url), "%s%s", group, path) >= sizeof(url)) {
            return invalid_argument;
        }
    } else if (g_snprintf(url, sizeof(url), "https://%s%s", group, path) >=
               sizeof(url)) {
        return invalid_argument;
    }
    handle = shadps4_hle_http_create_request_values(
        hle, parent, method, url, content_length);
    if (handle < 0) {
        return handle;
    }
    handle_le = cpu_to_le64(handle);
    if (!shadps4_guest_rw(cs, output, &handle_le, sizeof(handle_le), true)) {
        shadps4_hle_service_delete(hle, handle,
                                   SHADPS4_SERVICE_HTTP_REQUEST);
        return invalid_argument;
    }
    return 0;
}

static int shadps4_hle_webapi_header(ShadPS4HLEState *hle, CPUState *cs,
                                     uint64_t request, uint64_t name_address,
                                     uint64_t value_address, uint64_t value_size,
                                     uint64_t length_address,
                                     uint32_t invalid_argument)
{
    g_autofree char *headers = NULL;
    char name[256];
    const char *line;
    size_t headers_size = 0;
    size_t name_length;
    uint8_t zero = 0;
    int ret;

    if (!shadps4_hle_service_is(hle, request,
                                SHADPS4_SERVICE_HTTP_REQUEST) ||
        !shadps4_guest_read_string(cs, name_address, name, sizeof(name)) ||
        (!value_address && !length_address)) {
        return invalid_argument;
    }
    ret = qemu_host_http_get_headers(
        hle->service_host_handles[request - 0x400], NULL, &headers_size);
    if ((ret < 0 && ret != -ENOSPC) || headers_size > 2 * MiB) {
        return ret < 0 ? shadps4_hle_host_result(ret) :
               -SHADPS4_GUEST_EIO;
    }
    headers = g_malloc(headers_size + 1);
    ret = qemu_host_http_get_headers(
        hle->service_host_handles[request - 0x400], headers, &headers_size);
    if (ret < 0) {
        return shadps4_hle_host_result(ret);
    }
    headers[headers_size] = '\0';
    name_length = strlen(name);
    for (line = headers; *line;) {
        const char *end = strstr(line, "\r\n");
        const char *colon = strchr(line, ':');
        const char *value;
        size_t length;

        if (!end) {
            end = line + strlen(line);
        }
        if (colon && colon < end && colon - line == name_length &&
            !g_ascii_strncasecmp(line, name, name_length)) {
            value = colon + 1;
            while (value < end && g_ascii_isspace(*value)) {
                value++;
            }
            length = end - value;
            if (length_address &&
                shadps4_hle_write_u64(cs, length_address, length)) {
                return invalid_argument;
            }
            if (value_address) {
                if (!value_size || length + 1 > value_size ||
                    !shadps4_guest_rw(cs, value_address, (void *)value,
                                      length, true)) {
                    return invalid_argument;
                }
                if (!shadps4_guest_rw(cs, value_address + length, &zero, 1,
                                      true)) {
                    return invalid_argument;
                }
            }
            return 0;
        }
        line = *end ? end + 2 : end;
    }
    return invalid_argument;
}

static int shadps4_hle_avplayer_open(ShadPS4HLEState *hle, uint64_t handle,
                                     const char *source)
{
    uint64_t slot = handle - 0x400;
    int64_t host_handle;
    int ret;

    ret = qemu_host_media_open(source, &host_handle);
    if (ret < 0) {
        return shadps4_hle_host_result(ret);
    }
    if (hle->service_host_handles[slot] >= 0) {
        qemu_host_media_close(hle->service_host_handles[slot]);
    }
    hle->service_host_handles[slot] = host_handle;
    hle->service_value[slot] = 1;
    return 0;
}

static bool shadps4_hle_avplayer_frame(ShadPS4HLEState *hle, CPUState *cs,
                                      uint64_t handle, uint64_t info_address,
                                      uint32_t stream_type, bool extended)
{
    QemuHostMediaFrameInfo host_info = { 0 };
    g_autofree uint8_t *data = NULL;
    uint8_t guest_info[104] = { 0 };
    uint64_t guest_data;
    size_t size = 0;
    size_t capacity;
    uint32_t float_bits;
    float aspect;
    int ret;

    if (!shadps4_hle_service_is(hle, handle,
                                SHADPS4_SERVICE_AVPLAYER) || !info_address ||
        hle->service_host_handles[handle - 0x400] < 0) {
        return false;
    }
    ret = qemu_host_media_read_frame(
        hle->service_host_handles[handle - 0x400], stream_type, NULL, &size,
        &host_info);
    if ((ret < 0 && ret != -ENOSPC) || !size || size > 32 * MiB) {
        return false;
    }
    capacity = size;
    data = g_malloc(capacity);
    ret = qemu_host_media_read_frame(
        hle->service_host_handles[handle - 0x400], stream_type, data, &size,
        &host_info);
    if (ret < 0 || !size || size > capacity) {
        return false;
    }
    guest_data = shadps4_hle_arena_alloc(
        hle, cs, &hle->media_frame_arena, &hle->media_frame_offset,
        32 * MiB, size);
    if (!guest_data || !shadps4_guest_rw(cs, guest_data, data, size, true)) {
        return false;
    }
    stq_le_p(guest_info, guest_data);
    stq_le_p(guest_info + 16, host_info.timestamp_us / 1000);
    if (stream_type == QEMU_HOST_MEDIA_STREAM_AUDIO) {
        stw_le_p(guest_info + 24, host_info.channels);
        stl_le_p(guest_info + 28, host_info.sample_rate);
        stl_le_p(guest_info + 32, size);
    } else {
        stl_le_p(guest_info + 24, host_info.width);
        stl_le_p(guest_info + 28, host_info.height);
        aspect = host_info.height ?
                 (float)host_info.width / host_info.height : 0.0f;
        memcpy(&float_bits, &aspect, sizeof(float_bits));
        stl_le_p(guest_info + 32, float_bits);
        if (extended) {
            stl_le_p(guest_info + 60, host_info.stride);
        }
    }
    return shadps4_guest_rw(cs, info_address, guest_info,
                            extended ? sizeof(guest_info) : 40, true);
}

static uint64_t shadps4_hle_save_mount(ShadPS4HLEState *hle, CPUState *cs,
                                       uint64_t mount_addr,
                                       uint64_t result_addr, bool version2)
{
    ShadPS4GuestSaveMountResult result = { 0 };
    char scoped[192];
    char directory[33];
    uint64_t directory_addr;
    uint64_t offset = version2 ? 8 : 16;
    int ret;

    if (!hle->save_data_initialized || !mount_addr || !result_addr ||
        !shadps4_guest_rw(cs, mount_addr + offset, &directory_addr,
                          sizeof(directory_addr), false)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    directory_addr = le64_to_cpu(directory_addr);
    if (!shadps4_guest_read_string(cs, directory_addr, directory,
                                    sizeof(directory)) ||
        !directory[0] || strchr(directory, '/') || strchr(directory, '\\') ||
        !strcmp(directory, ".") || !strcmp(directory, "..")) {
        return -SHADPS4_GUEST_EINVAL;
    }
    ret = g_snprintf(scoped, sizeof(scoped), "/titles/%s", hle->title_id);
    if (ret < 0 || (size_t)ret >= sizeof(scoped)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    ret = qemu_host_storage_mkdir(scoped, 0777);
    if (ret < 0 && ret != -EEXIST) {
        return -SHADPS4_GUEST_EIO;
    }
    ret = g_snprintf(scoped, sizeof(scoped), "/titles/%s/savedata",
                     hle->title_id);
    ret = ret >= sizeof(scoped) ? -SHADPS4_GUEST_EINVAL :
          qemu_host_storage_mkdir(scoped, 0777);
    if (ret < 0 && ret != -EEXIST) {
        return -SHADPS4_GUEST_EIO;
    }
    ret = g_snprintf(scoped, sizeof(scoped), "/titles/%s/savedata/%s",
                     hle->title_id, directory);
    if (ret >= sizeof(scoped)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    ret = qemu_host_storage_mkdir(scoped, 0777);
    if (ret < 0 && ret != -EEXIST) {
        return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
    }
    hle->save_data_mounted = true;
    pstrcpy(hle->save_data_dir, sizeof(hle->save_data_dir), directory);
    pstrcpy(result.mount_point, sizeof(result.mount_point), "/savedata0");
    result.mount_status = cpu_to_le32(ret == 0 ? 1 : 0);
    return shadps4_guest_rw(cs, result_addr, &result, sizeof(result), true) ?
           0 : -SHADPS4_GUEST_EFAULT;
}

static uint64_t shadps4_hle_save_delete(ShadPS4HLEState *hle, CPUState *cs,
                                        uint64_t delete_addr)
{
    char directory[33];
    char scoped[192];
    uint64_t directory_addr;

    if (!delete_addr ||
        !shadps4_guest_rw(cs, delete_addr + 16, &directory_addr,
                          sizeof(directory_addr), false)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    directory_addr = le64_to_cpu(directory_addr);
    if (!shadps4_guest_read_string(cs, directory_addr, directory,
                                    sizeof(directory)) ||
        !directory[0] || strchr(directory, '/') || strchr(directory, '\\')) {
        return -SHADPS4_GUEST_EINVAL;
    }
    if (g_snprintf(scoped, sizeof(scoped), "/titles/%s/savedata/%s",
                   hle->title_id, directory) >= sizeof(scoped)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    return qemu_host_storage_cleanup(scoped);
}

static bool shadps4_hle_save_memory_path(ShadPS4HLEState *hle,
                                         uint32_t slot, char *path,
                                         size_t path_size)
{
    int length = g_snprintf(path, path_size,
                            "/titles/%s/savedata/memory%u.bin",
                            hle->title_id, slot);

    return length >= 0 && (size_t)length < path_size;
}

static uint64_t shadps4_hle_save_memory_setup(ShadPS4HLEState *hle,
                                              CPUState *cs,
                                              uint64_t setup_addr,
                                              uint64_t result_addr)
{
    ShadPS4GuestSaveMemorySetup setup;
    QemuHostStorageStat stat;
    char path[192];
    char directory[160];
    uint64_t memory_size;
    uint64_t existed_size = 0;
    uint64_t existed_size_le;
    int64_t handle;
    uint32_t slot;
    uint8_t zero = 0;
    int ret;

    if (!hle->save_data_initialized || !setup_addr ||
        !shadps4_guest_rw(cs, setup_addr, &setup, sizeof(setup), false)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    slot = le32_to_cpu(setup.slot);
    memory_size = le64_to_cpu(setup.memory_size);
    if (slot >= ARRAY_SIZE(hle->save_memory_ready) || !memory_size ||
        memory_size > 64 * MiB ||
        !shadps4_hle_save_memory_path(hle, slot, path, sizeof(path))) {
        return -SHADPS4_GUEST_EINVAL;
    }
    g_snprintf(directory, sizeof(directory), "/titles/%s", hle->title_id);
    ret = qemu_host_storage_mkdir(directory, 0777);
    if (ret < 0 && ret != -EEXIST) {
        return -SHADPS4_GUEST_EIO;
    }
    g_snprintf(directory, sizeof(directory), "/titles/%s/savedata",
               hle->title_id);
    ret = qemu_host_storage_mkdir(directory, 0777);
    if (ret < 0 && ret != -EEXIST) {
        return -SHADPS4_GUEST_EIO;
    }
    if (qemu_host_storage_stat(path, &stat) == 0) {
        existed_size = MIN(stat.size, memory_size);
        if (stat.size < memory_size) {
            ret = qemu_host_storage_open(path, SHADPS4_O_RDWR, 0666,
                                         &handle);
            if (ret < 0 ||
                qemu_host_storage_seek(handle, memory_size - 1,
                                       SEEK_SET) < 0 ||
                qemu_host_storage_write(handle, &zero, 1) != 1) {
                if (ret >= 0) {
                    qemu_host_storage_close(handle);
                }
                return -SHADPS4_GUEST_EIO;
            }
            qemu_host_storage_close(handle);
        }
    } else {
        ret = qemu_host_storage_open(path, SHADPS4_O_RDWR |
                                     SHADPS4_O_CREAT, 0666, &handle);

        if (ret < 0) {
            return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
        }
        if (qemu_host_storage_seek(handle, memory_size - 1, SEEK_SET) < 0 ||
            qemu_host_storage_write(handle, &zero, 1) != 1) {
            qemu_host_storage_close(handle);
            return -SHADPS4_GUEST_EIO;
        }
        qemu_host_storage_close(handle);
    }
    hle->save_memory_ready[slot] = true;
    hle->save_memory_size[slot] = memory_size;
    if (!result_addr) {
        return 0;
    }
    existed_size_le = cpu_to_le64(existed_size);
    return shadps4_guest_rw(cs, result_addr, &existed_size_le,
                            sizeof(existed_size_le), true) ?
           0 : -SHADPS4_GUEST_EFAULT;
}

static uint64_t shadps4_hle_save_memory_data(ShadPS4HLEState *hle,
                                             CPUState *cs,
                                             uint64_t param_addr, bool write)
{
    ShadPS4GuestSaveMemoryGet get;
    ShadPS4GuestSaveMemorySet set;
    uint64_t data_addr;
    uint32_t count;
    uint32_t slot;
    char path[192];
    int64_t handle;
    uint32_t i;
    int ret;

    if (!hle->save_data_initialized || !param_addr) {
        return -SHADPS4_GUEST_EINVAL;
    }
    if (write) {
        if (!shadps4_guest_rw(cs, param_addr, &set, sizeof(set), false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        slot = le32_to_cpu(set.slot);
        count = MAX(le32_to_cpu(set.data_count), 1);
        data_addr = le64_to_cpu(set.data);
    } else {
        if (!shadps4_guest_rw(cs, param_addr, &get, sizeof(get), false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        slot = le32_to_cpu(get.slot);
        count = 1;
        data_addr = le64_to_cpu(get.data);
    }
    if (slot >= ARRAY_SIZE(hle->save_memory_ready) ||
        !hle->save_memory_ready[slot] || count > 16 ||
        !shadps4_hle_save_memory_path(hle, slot, path, sizeof(path))) {
        return -SHADPS4_GUEST_EINVAL;
    }
    if (!data_addr) {
        return 0;
    }
    ret = qemu_host_storage_open(path,
                                 write ? SHADPS4_O_RDWR : 0,
                                 0, &handle);
    if (ret < 0) {
        return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
    }
    for (i = 0; i < count; i++) {
        ShadPS4GuestSaveMemoryData data;
        g_autofree uint8_t *buffer = NULL;
        uint64_t buffer_addr;
        uint64_t size;
        int64_t offset;
        int64_t transferred;

        if (!shadps4_guest_rw(cs, data_addr + i * sizeof(data), &data,
                              sizeof(data), false)) {
            ret = -SHADPS4_GUEST_EFAULT;
            break;
        }
        buffer_addr = le64_to_cpu(data.buffer);
        size = le64_to_cpu(data.size);
        offset = le64_to_cpu(data.offset);
        if (!buffer_addr || size > 16 * MiB || offset < 0 ||
            (uint64_t)offset > hle->save_memory_size[slot] ||
            size > hle->save_memory_size[slot] - offset) {
            ret = -SHADPS4_GUEST_EINVAL;
            break;
        }
        buffer = g_malloc0(size ? size : 1);
        if (qemu_host_storage_seek(handle, offset, SEEK_SET) < 0) {
            ret = -SHADPS4_GUEST_EIO;
            break;
        }
        if (write) {
            if (!shadps4_guest_rw(cs, buffer_addr, buffer, size, false)) {
                ret = -SHADPS4_GUEST_EFAULT;
                break;
            }
            transferred = qemu_host_storage_write(handle, buffer, size);
        } else {
            transferred = qemu_host_storage_read(handle, buffer, size);
        }
        if (transferred < 0 || (uint64_t)transferred != size) {
            ret = -SHADPS4_GUEST_EIO;
            break;
        }
        if (!write && size &&
            !shadps4_guest_rw(cs, buffer_addr, buffer, size, true)) {
            ret = -SHADPS4_GUEST_EFAULT;
            break;
        }
        ret = 0;
    }
    qemu_host_storage_close(handle);
    return ret;
}

static uint64_t shadps4_hle_save_memory_legacy(ShadPS4HLEState *hle,
                                               CPUState *cs,
                                               uint64_t buffer_addr,
                                               uint64_t size,
                                               int64_t offset, bool write)
{
    g_autofree uint8_t *buffer = NULL;
    char path[192];
    int64_t handle;
    int64_t transferred;
    int ret;

    if (!hle->save_data_initialized || !hle->save_memory_ready[0] ||
        (!buffer_addr && size) || size > 16 * MiB || offset < 0 ||
        (uint64_t)offset > hle->save_memory_size[0] ||
        size > hle->save_memory_size[0] - offset ||
        !shadps4_hle_save_memory_path(hle, 0, path, sizeof(path))) {
        return -SHADPS4_GUEST_EINVAL;
    }
    ret = qemu_host_storage_open(path, write ? SHADPS4_O_RDWR : 0, 0,
                                 &handle);
    if (ret < 0) {
        return shadps4_hle_host_result(ret);
    }
    buffer = g_malloc0(size ? size : 1);
    if (qemu_host_storage_seek(handle, offset, SEEK_SET) < 0 ||
        (write && size && !shadps4_guest_rw(cs, buffer_addr, buffer,
                                             size, false))) {
        qemu_host_storage_close(handle);
        return -SHADPS4_GUEST_EFAULT;
    }
    transferred = write ? qemu_host_storage_write(handle, buffer, size) :
                          qemu_host_storage_read(handle, buffer, size);
    qemu_host_storage_close(handle);
    if (transferred < 0 || (uint64_t)transferred != size) {
        return -SHADPS4_GUEST_EIO;
    }
    if (!write && size &&
        !shadps4_guest_rw(cs, buffer_addr, buffer, size, true)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    return 0;
}

static uint64_t shadps4_hle_save_memory_setup_legacy(ShadPS4HLEState *hle,
                                                     uint64_t memory_size)
{
    QemuHostStorageStat stat;
    char directory[160];
    char path[192];
    int64_t handle;
    uint8_t zero = 0;
    int ret;

    if (!hle->save_data_initialized || !memory_size ||
        memory_size > 64 * MiB ||
        !shadps4_hle_save_memory_path(hle, 0, path, sizeof(path))) {
        return -SHADPS4_GUEST_EINVAL;
    }
    g_snprintf(directory, sizeof(directory), "/titles/%s", hle->title_id);
    ret = qemu_host_storage_mkdir(directory, 0777);
    if (ret < 0 && ret != -EEXIST) {
        return shadps4_hle_host_result(ret);
    }
    g_snprintf(directory, sizeof(directory), "/titles/%s/savedata",
               hle->title_id);
    ret = qemu_host_storage_mkdir(directory, 0777);
    if (ret < 0 && ret != -EEXIST) {
        return shadps4_hle_host_result(ret);
    }
    ret = qemu_host_storage_stat(path, &stat);
    if (ret < 0) {
        ret = qemu_host_storage_open(path, SHADPS4_O_RDWR | SHADPS4_O_CREAT,
                                     0666, &handle);
    } else if (stat.size < memory_size) {
        ret = qemu_host_storage_open(path, SHADPS4_O_RDWR, 0666, &handle);
    } else {
        hle->save_memory_ready[0] = true;
        hle->save_memory_size[0] = memory_size;
        return 0;
    }
    if (ret < 0) {
        return shadps4_hle_host_result(ret);
    }
    if (qemu_host_storage_seek(handle, memory_size - 1, SEEK_SET) < 0 ||
        qemu_host_storage_write(handle, &zero, 1) != 1) {
        qemu_host_storage_close(handle);
        return -SHADPS4_GUEST_EIO;
    }
    qemu_host_storage_close(handle);
    hle->save_memory_ready[0] = true;
    hle->save_memory_size[0] = memory_size;
    return 0;
}

static bool shadps4_hle_save_mount_point(ShadPS4HLEState *hle, CPUState *cs,
                                         uint64_t address)
{
    char mount_point[16];

    return hle->save_data_initialized && hle->save_data_mounted && address &&
           shadps4_guest_rw(cs, address, mount_point, sizeof(mount_point),
                            false) &&
           memchr(mount_point, '\0', sizeof(mount_point)) &&
           !strcmp(mount_point, "/savedata0");
}

static uint64_t shadps4_hle_save_icon(ShadPS4HLEState *hle, CPUState *cs,
                                      uint64_t mount_addr,
                                      uint64_t icon_addr, bool write)
{
    uint64_t icon[3];
    g_autofree uint8_t *data = NULL;
    QemuHostStorageStat stat;
    char path[192];
    int64_t handle;
    int64_t transferred;
    uint64_t size;
    int ret;

    if (!shadps4_hle_save_mount_point(hle, cs, mount_addr) || !icon_addr ||
        !shadps4_guest_rw(cs, icon_addr, icon, sizeof(icon), false)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    icon[0] = le64_to_cpu(icon[0]);
    icon[1] = le64_to_cpu(icon[1]);
    icon[2] = le64_to_cpu(icon[2]);
    if (!icon[0] || icon[1] > 4 * MiB ||
        g_snprintf(path, sizeof(path), "/titles/%s/savedata/%s/icon0.png",
                   hle->title_id, hle->save_data_dir) >= sizeof(path)) {
        return -SHADPS4_GUEST_EINVAL;
    }
    if (write) {
        size = MIN(icon[2], icon[1]);
        if (!size) {
            return -SHADPS4_GUEST_EINVAL;
        }
        data = g_malloc(size);
        if (!shadps4_guest_rw(cs, icon[0], data, size, false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        ret = qemu_host_storage_open(path, SHADPS4_O_RDWR | SHADPS4_O_CREAT |
                                     SHADPS4_O_TRUNC, 0666, &handle);
    } else {
        ret = qemu_host_storage_stat(path, &stat);
        if (ret < 0) {
            return shadps4_hle_host_result(ret);
        }
        size = MIN(stat.size, icon[1]);
        data = g_malloc(size ? size : 1);
        ret = qemu_host_storage_open(path, 0, 0, &handle);
    }
    if (ret < 0) {
        return shadps4_hle_host_result(ret);
    }
    transferred = write ? qemu_host_storage_write(handle, data, size) :
                          qemu_host_storage_read(handle, data, size);
    qemu_host_storage_close(handle);
    if (transferred < 0 || (uint64_t)transferred != size) {
        return -SHADPS4_GUEST_EIO;
    }
    if (!write && size && !shadps4_guest_rw(cs, icon[0], data, size, true)) {
        return -SHADPS4_GUEST_EFAULT;
    }
    icon[2] = cpu_to_le64(write ? icon[2] : stat.size);
    return shadps4_guest_rw(cs, icon_addr + 16, &icon[2], sizeof(icon[2]),
                            true) ? 0 : -SHADPS4_GUEST_EFAULT;
}

static void shadps4_hle_dialog_poll(ShadPS4HLEState *hle)
{
    uint64_t request_id;
    size_t size = sizeof(hle->dialog_result);
    int status;

    if (hle->dialog_status != 2 ||
        qemu_host_dialog_take_response(&request_id, &status,
                                       hle->dialog_result, &size) < 0) {
        return;
    }
    if (request_id != hle->dialog_request_id) {
        return;
    }
    hle->dialog_result_status = status;
    hle->dialog_result_size = size;
    hle->dialog_status = 3;
}

static uint64_t shadps4_hle_dialog_open(ShadPS4HLEState *hle, CPUState *cs,
                                        uint64_t param_addr, uint32_t kind)
{
    char payload[512] = { 0 };
    uint64_t message_addr = 0;
    uint64_t user_param_addr = 0;
    size_t payload_size;
    int ret;

    if ((hle->dialog_status != 1 && hle->dialog_status != 3) ||
        !param_addr) {
        return -SHADPS4_GUEST_EINVAL;
    }
    if (kind == 1 &&
        shadps4_guest_rw(cs, param_addr + 64, &user_param_addr,
                         sizeof(user_param_addr), false)) {
        user_param_addr = le64_to_cpu(user_param_addr);
        if (user_param_addr &&
            shadps4_guest_rw(cs, user_param_addr + 8, &message_addr,
                             sizeof(message_addr), false)) {
            message_addr = le64_to_cpu(message_addr);
            shadps4_guest_read_string(cs, message_addr, payload,
                                      sizeof(payload));
        }
    }
    if (!payload[0]) {
        g_snprintf(payload, sizeof(payload), "shadPS4 dialog type %u", kind);
    }
    payload_size = strlen(payload) + 1;
    ret = qemu_host_dialog_request(kind, payload, payload_size,
                                   &hle->dialog_request_id);
    if (ret < 0) {
        return ret >= -255 ? ret : -SHADPS4_GUEST_EIO;
    }
    hle->dialog_kind = kind;
    hle->dialog_status = 2;
    hle->dialog_result_size = 0;
    hle->error_dialog_status = 0;
    hle->web_dialog_status = 0;
    hle->msg_dialog_progress = 0;
    hle->dialog_result_status = 0;
    return 0;
}

void shadps4_hle_init(ShadPS4HLEState *hle, AddressSpace *as,
                      ShadPS4GPUState *gpu, ShadPS4IOState *io,
                      const char *title_id,
                      uint64_t dynamic_pd_phys, uint64_t dynamic_virt_base)
{
    memset(hle, 0, sizeof(*hle));
    hle->as = as;
    hle->gpu = gpu;
    hle->io = io;
    pstrcpy(hle->title_id, sizeof(hle->title_id), title_id);
    hle->dynamic_pd_phys = dynamic_pd_phys;
    hle->dynamic_virt_base = dynamic_virt_base;
    hle->cleanup_done = true;
}

void shadps4_hle_set_modules(ShadPS4HLEState *hle,
                             const ShadPS4ImageInfo *main_image,
                             const ShadPS4ImageInfo *modules,
                             uint32_t module_count,
                             const ShadPS4ImageInfo *hle_image)
{
    uint32_t i;

    memset(hle->module_images, 0, sizeof(hle->module_images));
    hle->module_image_count = 0;
    hle->hle_image = hle_image;
    hle->service_guest_base = hle_image &&
        hle_image->image_size >= SHADPS4_HLE_SERVICE_GUEST_ARENA_SIZE ?
        hle_image->virtual_base + hle_image->image_size -
            SHADPS4_HLE_SERVICE_GUEST_ARENA_SIZE : 0;
    if (!main_image) {
        return;
    }
    hle->module_images[hle->module_image_count++] = main_image;
    for (i = 0; i < module_count &&
                hle->module_image_count < SHADPS4_MAX_MODULES; i++) {
        hle->module_images[hle->module_image_count++] = &modules[i];
    }
}

void shadps4_hle_cleanup(ShadPS4HLEState *hle)
{
    char path[96];
    int fd;

    if (hle->cleanup_done) {
        return;
    }
    hle->cleanup_done = true;

    for (fd = 3; fd < SHADPS4_HLE_MAX_FDS; fd++) {
        if (hle->files[fd] == SHADPS4_HLE_FD_STORAGE) {
            qemu_host_storage_close(hle->storage_handles[fd]);
        } else if (hle->files[fd] == SHADPS4_HLE_FD_NETWORK) {
            qemu_host_network_close(hle->network_handles[fd]);
        }
    }
    for (fd = 1; fd < SHADPS4_HLE_MAX_SERVICE_OBJECTS; fd++) {
        if (hle->service_host_handles[fd] < 0) {
            continue;
        }
        if (hle->service_objects[fd] == SHADPS4_SERVICE_HTTP_REQUEST) {
            qemu_host_http_close(hle->service_host_handles[fd]);
        } else if (hle->service_objects[fd] == SHADPS4_SERVICE_AVPLAYER) {
            qemu_host_media_close(hle->service_host_handles[fd]);
        }
        hle->service_host_handles[fd] = -1;
    }
    if (hle->title_id[0]) {
        g_snprintf(path, sizeof(path), "/titles/%s/temp/", hle->title_id);
        qemu_host_storage_cleanup(path);
        g_snprintf(path, sizeof(path), "/titles/%s/temp0/", hle->title_id);
        qemu_host_storage_cleanup(path);
    }
}

bool shadps4_hle_reset(ShadPS4HLEState *hle, uint64_t dynamic_phys_base,
                       uint64_t physical_limit, Error **errp)
{
    uint64_t available;

    if (dynamic_phys_base >= physical_limit) {
        error_setg(errp, "no RAM remains for shadPS4 dynamic mappings");
        return false;
    }
    available = physical_limit - dynamic_phys_base;
    hle->dynamic_phys_base = dynamic_phys_base;
    hle->dynamic_slot_count = MIN(available / (2 * MiB),
                                  SHADPS4_HLE_MAX_MMAP_SLOTS);
    if (!hle->dynamic_slot_count) {
        error_setg(errp, "shadPS4 dynamic mapping area is empty");
        return false;
    }
    hle->result = 0;
    hle->last_hle_number = 0;
    hle->last_hle_result = 0;
    memset(hle->last_hle_args, 0, sizeof(hle->last_hle_args));
    hle->last_hle_nid[0] = '\0';
    hle->last_hle_library[0] = '\0';
    hle->last_hle_module[0] = '\0';
    hle->last_hle_return_address = 0;
    hle->hle_history_head = 0;
    hle->hle_history_count = 0;
    memset(hle->hle_history, 0, sizeof(hle->hle_history));
    shadps4_hle_cleanup(hle);
    hle->cleanup_done = false;
    memset(hle->dynamic_slots, 0, sizeof(hle->dynamic_slots));
    hle->direct_memory_next = 0;
    hle->prt_aperture_address = 0;
    hle->prt_aperture_size = 0;
    hle->sandbox_word_addr = 0;
    hle->heap_cursor = 0;
    hle->heap_end = 0;
    memset(hle->heap_allocs, 0, sizeof(hle->heap_allocs));
    hle->gnm_tessellation_ring_addr = 0;
    hle->libc_file_arena = 0;
    memset(hle->libc_file_used, 0, sizeof(hle->libc_file_used));
    memset(hle->libc_file_fd, 0xff, sizeof(hle->libc_file_fd));
    memset(hle->files, 0, sizeof(hle->files));
    memset(hle->file_units, 0, sizeof(hle->file_units));
    memset(hle->storage_handles, 0xff, sizeof(hle->storage_handles));
    memset(hle->network_handles, 0xff, sizeof(hle->network_handles));
    memset(hle->storage_read_only, 0, sizeof(hle->storage_read_only));
    memset(hle->storage_paths, 0, sizeof(hle->storage_paths));
    memset(hle->equeues, 0, sizeof(hle->equeues));
    memset(hle->equeue_vblank_seen, 0, sizeof(hle->equeue_vblank_seen));
    memset(hle->equeue_events, 0, sizeof(hle->equeue_events));
    hle->video_open = false;
    hle->gnm_submit_failures = 0;
    hle->video_flip_failures = 0;
    hle->video_flip_rate = 0;
    hle->video_current_buffer = -1;
    hle->video_flip_arg = -1;
    hle->video_label_arena = 0;
    hle->video_label_offset = 0;
    hle->video_gamma = 1.0f;
    memset(hle->video_buffers, 0, sizeof(hle->video_buffers));
    memset(hle->pad_open, 0, sizeof(hle->pad_open));
    memset(hle->pad_motion_enabled, 0, sizeof(hle->pad_motion_enabled));
    hle->pad_initialized = false;
    memset(hle->pad_user_id, 0, sizeof(hle->pad_user_id));
    memset(hle->pad_port_type, 0, sizeof(hle->pad_port_type));
    memset(hle->pad_port_index, 0, sizeof(hle->pad_port_index));
    memset(hle->audio_out, 0, sizeof(hle->audio_out));
    memset(hle->audio_in, 0, sizeof(hle->audio_in));
    hle->audio_mastering_initialized = false;
    hle->common_dialog_initialized = false;
    hle->coredump_handler = 0;
    hle->coredump_common = 0;
    hle->rtc_minuteswest = 0;
    hle->rtc_dsttime = 0;
    hle->webapi_last_error = 0;
    memset(hle->service_objects, 0, sizeof(hle->service_objects));
    memset(hle->service_parents, 0, sizeof(hle->service_parents));
    memset(hle->service_nonblock, 0, sizeof(hle->service_nonblock));
    memset(hle->service_user_data, 0, sizeof(hle->service_user_data));
    memset(hle->service_guest_addr, 0, sizeof(hle->service_guest_addr));
    memset(hle->service_value, 0, sizeof(hle->service_value));
    memset(hle->service_aux_value, 0, sizeof(hle->service_aux_value));
    memset(hle->service_content_length, 0,
           sizeof(hle->service_content_length));
    memset(hle->service_host_handles, 0xff,
           sizeof(hle->service_host_handles));
    memset(hle->service_active, 0, sizeof(hle->service_active));
    hle->http_headers_arena = 0;
    hle->http_headers_offset = 0;
    hle->media_frame_arena = 0;
    hle->media_frame_offset = 0;
    hle->ssl_next_id = 0;
    memset(hle->pthread_attrs, 0, sizeof(hle->pthread_attrs));
    memset(hle->pthread_keys, 0, sizeof(hle->pthread_keys));
    memset(hle->pthread_key_destructors, 0,
           sizeof(hle->pthread_key_destructors));
    memset(hle->pthread_key_values, 0, sizeof(hle->pthread_key_values));
    memset(hle->aio_states, 0, sizeof(hle->aio_states));
    hle->aio_next_id = 0;
    memset(hle->signal_handlers, 0, sizeof(hle->signal_handlers));
    memset(hle->signal_mask, 0, sizeof(hle->signal_mask));
    memset(hle->rtc_tick_offset, 0, sizeof(hle->rtc_tick_offset));
    hle->netctl_initialized = false;
    memset(hle->netctl_callbacks, 0, sizeof(hle->netctl_callbacks));
    memset(hle->netctl_callback_args, 0,
           sizeof(hle->netctl_callback_args));
    hle->net_errno = 0;
    hle->np_matching_initialized = false;
    hle->np_signaling_initialized = false;
    hle->usbd_initialized = false;
    hle->hmd_initialized = false;
    hle->audio3d_initialized = false;
    hle->playgo_initialized = false;
    hle->playgo_open = false;
    hle->playgo_speed = 0;
    hle->playgo_language_mask = 0;
    memset(hle->trophy_flags, 0, sizeof(hle->trophy_flags));
    hle->save_data_initialized = false;
    hle->save_data_mounted = false;
    hle->save_data_dir[0] = '\0';
    hle->save_data_progress = 0.0f;
    memset(hle->save_memory_ready, 0, sizeof(hle->save_memory_ready));
    memset(hle->save_memory_size, 0, sizeof(hle->save_memory_size));
    hle->app_content_initialized = false;
    hle->content_export_initialized = false;
    hle->move_initialized = false;
    hle->zlib_initialized = false;
    hle->https_cert_loaded = false;
    hle->fiber_context_size_check = false;
    hle->vr_tracker_initialized = false;
    for (uint32_t i = 0; i < ARRAY_SIZE(hle->vr_tracker_handles); i++) {
        hle->vr_tracker_handles[i] = -1;
    }
    hle->commerce_icon_visible = false;
    hle->commerce_icon_layout = 0;
    hle->commerce_icon_position = 1;
    hle->ime_dialog_pos_x = 0;
    hle->ime_dialog_pos_y = 0;
    hle->dialog_user_data = 0;
    hle->dialog_status = 0;
    hle->dialog_kind = 0;
    hle->dialog_request_id = 0;
    hle->dialog_result_status = 0;
    hle->dialog_result_size = 0;
    memset(hle->dialog_result, 0, sizeof(hle->dialog_result));
    hle->error_dialog_status = 0;
    hle->web_dialog_status = 0;
    hle->msg_dialog_progress = 0;
    hle->mouse_initialized = false;
    memset(hle->mouse_open, 0, sizeof(hle->mouse_open));
    hle->mouse_merged = false;
    hle->np_partner_initialized = false;
    hle->user_service_initialized = false;
    hle->user_event_head = 0;
    hle->user_event_count = 0;
    memset(hle->user_event_types, 0, sizeof(hle->user_event_types));
    memset(hle->user_event_ids, 0, sizeof(hle->user_event_ids));
    hle->splash_visible = true;
    hle->system_event_head = 0;
    hle->system_event_count = 0;
    memset(hle->system_event_types, 0, sizeof(hle->system_event_types));
    memset(hle->sysmodule_load_count, 0,
           sizeof(hle->sysmodule_load_count));
    hle->files[0] = SHADPS4_HLE_FD_CONSOLE;
    hle->files[1] = SHADPS4_HLE_FD_CONSOLE;
    hle->files[2] = SHADPS4_HLE_FD_CONSOLE;
    shadps4_gpu_reset(hle->gpu);
    return true;
}

static int shadps4_hle_external_read(void *opaque, uint64_t guest_address,
                                     void *buffer, size_t size)
{
    CPUState *cs = opaque;

    return (!size || buffer) &&
           shadps4_guest_rw(cs, guest_address, buffer, size, false) ?
           0 : -EFAULT;
}

static int shadps4_hle_external_write(void *opaque, uint64_t guest_address,
                                      const void *buffer, size_t size)
{
    CPUState *cs = opaque;

    return (!size || buffer) &&
           shadps4_guest_rw(cs, guest_address, (void *)buffer,
                            size, true) ? 0 : -EFAULT;
}

#include "shadps4-codecs.inc.c"
#include "shadps4-http.inc.c"
#include "shadps4-services2.inc.c"
#include "shadps4-services3.inc.c"
#include "shadps4-services4.inc.c"
#include "shadps4-services5.inc.c"
#include "shadps4-services6.inc.c"

static const char *shadps4_exception_type(uint32_t vector)
{
    static const char *const names[] = {
        "divide-error", "debug", "nmi", "breakpoint", "overflow",
        "bound-range", "invalid-opcode", "device-not-available",
        "double-fault", "coprocessor-overrun", "invalid-tss",
        "segment-not-present", "stack-segment", "general-protection",
        "page-fault", "reserved", "x87-floating-point",
        "alignment-check", "machine-check", "simd-floating-point",
        "virtualization", "control-protection",
    };

    return vector < ARRAY_SIZE(names) ? names[vector] : "unknown";
}

static bool shadps4_exception_has_error_code(uint32_t vector)
{
    return vector == 8 || (vector >= 10 && vector <= 14) ||
           vector == 17 || vector == 21 || vector == 29 || vector == 30;
}

static const ShadPS4ImageInfo *shadps4_hle_image_for_address(
    const ShadPS4HLEState *hle, uint64_t address)
{
    uint32_t i;

    for (i = 0; i < hle->module_image_count; i++) {
        const ShadPS4ImageInfo *image = hle->module_images[i];

        if (address >= image->virtual_base &&
            address - image->virtual_base < image->image_size) {
            return image;
        }
    }
    if (hle->hle_image && address >= hle->hle_image->virtual_base &&
        address - hle->hle_image->virtual_base < hle->hle_image->image_size) {
        return hle->hle_image;
    }
    return NULL;
}

static void shadps4_hle_record_call(ShadPS4HLEState *hle,
                                    CPUState *cs, CPUX86State *env,
                                    uint64_t number,
                                    const uint64_t args[7])
{
    const ShadPS4DynamicSymbol *symbol = NULL;
    uint64_t return_address = env->regs[R_ECX];
    uint32_t i;

    hle->last_hle_number = number;
    memcpy(hle->last_hle_args, args, sizeof(hle->last_hle_args));
    hle->last_hle_nid[0] = '\0';
    hle->last_hle_library[0] = '\0';
    hle->last_hle_module[0] = '\0';
    hle->last_hle_return_address = 0;
    if (env->regs[R_ESP] >= SHADPS4_GUEST_USER_MIN) {
        uint64_t guest_return = 0;

        if (shadps4_guest_rw(cs, env->regs[R_ESP], &guest_return,
                             sizeof(guest_return), false)) {
            hle->last_hle_return_address = le64_to_cpu(guest_return);
        }
    }

    if (number >= SHADPS4_HLE_EXTERNAL_BASE &&
        number - SHADPS4_HLE_EXTERNAL_BASE < hle->external_nid_count) {
        const ShadPS4HLEExternalNID *external = &hle->external_nids[
            number - SHADPS4_HLE_EXTERNAL_BASE];

        pstrcpy(hle->last_hle_nid, sizeof(hle->last_hle_nid), external->nid);
        pstrcpy(hle->last_hle_library, sizeof(hle->last_hle_library),
                external->library);
        pstrcpy(hle->last_hle_module, sizeof(hle->last_hle_module),
                external->module);
        return;
    }
    if (!hle->hle_image || return_address < hle->hle_image->virtual_base) {
        return;
    }
    for (i = 0; i < hle->hle_image->symbol_count; i++) {
        const ShadPS4DynamicSymbol *candidate = &hle->hle_image->symbols[i];

        if (candidate->type == SHADPS4_ELF_STT_FUNC &&
            return_address == hle->hle_image->virtual_base +
                              candidate->value + 10) {
            symbol = candidate;
            break;
        }
    }
    if (symbol) {
        pstrcpy(hle->last_hle_nid, sizeof(hle->last_hle_nid),
                symbol->nid ?: "");
        pstrcpy(hle->last_hle_library, sizeof(hle->last_hle_library),
                symbol->library ?: "");
        pstrcpy(hle->last_hle_module, sizeof(hle->last_hle_module),
                symbol->module ?: "");
    }
}

void shadps4_hle_complete_call(ShadPS4HLEState *hle, int64_t result)
{
    ShadPS4HLECallHistory *entry =
        &hle->hle_history[hle->hle_history_head];

    /* Allocators lock on almost every operation. Keep the fault history
     * useful while last_hle_* still records the exact latest call. */
    if (hle->last_hle_number == SHADPS4_HLE_MUTEX_LOCK ||
        hle->last_hle_number == SHADPS4_HLE_MUTEX_UNLOCK) {
        hle->last_hle_result = result;
        return;
    }

    entry->number = hle->last_hle_number;
    entry->result = result;
    entry->return_address = hle->last_hle_return_address;
    memcpy(entry->args, hle->last_hle_args, sizeof(entry->args));
    pstrcpy(entry->nid, sizeof(entry->nid), hle->last_hle_nid);
    hle->hle_history_head =
        (hle->hle_history_head + 1) % SHADPS4_HLE_HISTORY_SIZE;
    hle->hle_history_count = MIN(hle->hle_history_count + 1,
                                 SHADPS4_HLE_HISTORY_SIZE);
}

static void shadps4_hle_report_fault(ShadPS4HLEState *hle, CPUState *cs)
{
    CPUX86State *env = &X86_CPU(cs)->env;
    QemuHostFaultInfo fault = {
        .size = sizeof(fault),
        .version = QEMU_HOST_FAULT_INFO_VERSION,
        .cpu_index = cs->cpu_index,
        .rbp = env->regs[R_EBP],
        .cr2 = env->cr[2],
        .cr3 = env->cr[3],
        .last_hle_number = hle->last_hle_number,
        .last_hle_result = hle->last_hle_result,
        .guest_thread_id = 1,
        .guest_process_id = 1,
        .rax = env->regs[R_EAX],
        .rbx = env->regs[R_EBX],
        .rcx = env->regs[R_ECX],
        .rdx = env->regs[R_EDX],
        .rsi = env->regs[R_ESI],
        .rdi = env->regs[R_EDI],
        .r8 = env->regs[R_R8],
        .r9 = env->regs[R_R9],
        .r10 = env->regs[R_R10],
        .r11 = env->regs[R_R11],
        .r12 = env->regs[R_R12],
        .r13 = env->regs[R_R13],
        .r14 = env->regs[R_R14],
        .r15 = env->regs[R_R15],
    };
    uint64_t frame[8] = { 0 };
    uint64_t return_address_le = 0;
    const ShadPS4ImageInfo *image;
    g_autoptr(GString) instruction = g_string_new(NULL);
    uint32_t i;

    memcpy(fault.last_hle_args, hle->last_hle_args,
           sizeof(fault.last_hle_args));
    pstrcpy(fault.last_hle_nid, sizeof(fault.last_hle_nid),
            hle->last_hle_nid);
    pstrcpy(fault.last_hle_library, sizeof(fault.last_hle_library),
            hle->last_hle_library);
    pstrcpy(fault.last_hle_module, sizeof(fault.last_hle_module),
            hle->last_hle_module);

    if (shadps4_debug_rw(cs, env->regs[R_ESP], frame,
                         sizeof(frame), false)) {
        for (i = 0; i < ARRAY_SIZE(frame); i++) {
            frame[i] = le64_to_cpu(frame[i]);
        }
        fault.rax = frame[0];
        fault.vector = frame[1];
        fault.error_code_valid =
            shadps4_exception_has_error_code(fault.vector);
        fault.error_code = frame[2];
        fault.rip = frame[3];
        fault.rflags = frame[5];
        fault.rsp = (frame[4] & 3) ? frame[6] :
                    env->regs[R_ESP] + 6 * sizeof(uint64_t);
    } else {
        fault.vector = UINT32_MAX;
        fault.rip = env->eip;
        fault.rsp = env->regs[R_ESP];
        fault.rflags = env->eflags;
    }
    pstrcpy(fault.exception_type, sizeof(fault.exception_type),
            shadps4_exception_type(fault.vector));

    if (fault.rip) {
        for (i = 0; i < sizeof(fault.instruction) &&
                    fault.rip <= UINT64_MAX - i; i++) {
            if (!shadps4_guest_rw(cs, fault.rip + i,
                                  &fault.instruction[i], 1, false)) {
                break;
            }
            fault.instruction_size++;
        }
    }
    if (fault.rsp && shadps4_guest_rw(cs, fault.rsp, &return_address_le,
                                      sizeof(return_address_le), false)) {
        fault.return_address = le64_to_cpu(return_address_le);
    }
    image = shadps4_hle_image_for_address(hle, fault.rip);
    pstrcpy(fault.image, sizeof(fault.image),
            image ? (image->name ?: "<HLE image>") : "<unmapped>");
    for (i = 0; i < fault.instruction_size; i++) {
        g_string_append_printf(instruction, "%02x%s", fault.instruction[i],
                               i + 1 == fault.instruction_size ? "" : " ");
    }

    qemu_host_record_last_fault(&fault);
    error_report("shadPS4 internal fault: vector=%u type=%s error=%#" PRIx64
                 " valid=%u rip=%#" PRIx64 " rsp=%#" PRIx64
                 " rbp=%#" PRIx64 " cr2=%#" PRIx64 " cr3=%#" PRIx64
                 " rflags=%#" PRIx64,
                 fault.vector, fault.exception_type, fault.error_code,
                 fault.error_code_valid, fault.rip, fault.rsp, fault.rbp,
                 fault.cr2, fault.cr3, fault.rflags);
    error_report("shadPS4 fault context: image='%s' return=%#" PRIx64
                 " instruction=[%s] cpu=%u pid=%#" PRIx64
                 " tid=%#" PRIx64,
                 fault.image, fault.return_address, instruction->str,
                 fault.cpu_index, fault.guest_process_id,
                 fault.guest_thread_id);
    error_report("shadPS4 fault registers: rax=%#" PRIx64
                 " rbx=%#" PRIx64 " rcx=%#" PRIx64 " rdx=%#" PRIx64
                 " rsi=%#" PRIx64 " rdi=%#" PRIx64 " r8=%#" PRIx64
                 " r9=%#" PRIx64 " r10=%#" PRIx64 " r11=%#" PRIx64
                 " r12=%#" PRIx64 " r13=%#" PRIx64 " r14=%#" PRIx64
                 " r15=%#" PRIx64,
                 fault.rax, fault.rbx, fault.rcx, fault.rdx, fault.rsi,
                 fault.rdi, fault.r8, fault.r9, fault.r10, fault.r11,
                 fault.r12, fault.r13, fault.r14, fault.r15);
    {
        uint64_t rbp = fault.rbp;

        for (i = 0; i < 8 && rbp; i++) {
            uint64_t links[2];
            uint64_t next_rbp;
            uint64_t caller;
            const ShadPS4ImageInfo *caller_image;

            if (!shadps4_debug_rw(cs, rbp, links, sizeof(links), false)) {
                break;
            }
            next_rbp = le64_to_cpu(links[0]);
            caller = le64_to_cpu(links[1]);
            caller_image = shadps4_hle_image_for_address(hle, caller);
            error_report("shadPS4 backtrace[%u]: rbp=%#" PRIx64
                         " return=%#" PRIx64 " image='%s'",
                         i, rbp, caller,
                         caller_image ?
                             (caller_image->name ?: "<HLE image>") :
                             "<unmapped>");
            if (next_rbp <= rbp || next_rbp - rbp > 16 * MiB) {
                break;
            }
            rbp = next_rbp;
        }
    }
    error_report("shadPS4 last HLE: number=%#" PRIx64
                 " result=%#" PRIx64 " result_signed=%" PRId64
                 " NID='%s' library='%s' module='%s' args=[%#" PRIx64
                 ",%#" PRIx64 ",%#" PRIx64 ",%#" PRIx64 ",%#" PRIx64
                 ",%#" PRIx64 ",%#" PRIx64 "]",
                 fault.last_hle_number, (uint64_t)fault.last_hle_result,
                 fault.last_hle_result, fault.last_hle_nid,
                 fault.last_hle_library, fault.last_hle_module,
                 fault.last_hle_args[0], fault.last_hle_args[1],
                 fault.last_hle_args[2], fault.last_hle_args[3],
                 fault.last_hle_args[4], fault.last_hle_args[5],
                 fault.last_hle_args[6]);
    for (i = 0; i < hle->hle_history_count; i++) {
        uint32_t index = (hle->hle_history_head + SHADPS4_HLE_HISTORY_SIZE -
                          hle->hle_history_count + i) %
                         SHADPS4_HLE_HISTORY_SIZE;
        const ShadPS4HLECallHistory *entry = &hle->hle_history[index];

        error_report("shadPS4 HLE history[%u]: number=%#" PRIx64
                     " result=%#" PRIx64 " NID='%s' return=%#" PRIx64
                     " args=[%#" PRIx64 ",%#" PRIx64 ",%#" PRIx64
                     ",%#" PRIx64 ",%#" PRIx64 ",%#" PRIx64
                     ",%#" PRIx64 "]",
                     i, entry->number, (uint64_t)entry->result, entry->nid,
                     entry->return_address, entry->args[0], entry->args[1],
                     entry->args[2], entry->args[3], entry->args[4],
                     entry->args[5], entry->args[6]);
    }
}

uint64_t shadps4_hle_dispatch(ShadPS4HLEState *hle, CPUState *cs,
                              uint64_t number)
{
    CPUX86State *env = &X86_CPU(cs)->env;
    uint64_t a0 = env->regs[R_EDI];
    uint64_t a1 = env->regs[R_ESI];
    uint64_t a2 = env->regs[R_EDX];
    uint64_t a3 = env->regs[R_R10];
    uint64_t a4 = env->regs[R_R8];
    uint64_t a5 = env->regs[R_R9];
    uint64_t a6 = 0;
    uint64_t hle_args[7];

    if (env->regs[R_ESP] <= UINT64_MAX - 8) {
        shadps4_guest_rw(cs, env->regs[R_ESP] + 8,
                         &a6, sizeof(a6), false);
    }
    a6 = le64_to_cpu(a6);
    hle_args[0] = a0;
    hle_args[1] = a1;
    hle_args[2] = a2;
    hle_args[3] = a3;
    hle_args[4] = a4;
    hle_args[5] = a5;
    hle_args[6] = a6;
    if (number != SHADPS4_HLE_INTERNAL_FAULT) {
        shadps4_hle_record_call(hle, cs, env, number, hle_args);
    }

    if (number >= SHADPS4_HLE_EXTERNAL_BASE &&
        number - SHADPS4_HLE_EXTERNAL_BASE < hle->external_nid_count) {
        const ShadPS4HLEExternalNID *external = &hle->external_nids[
            number - SHADPS4_HLE_EXTERNAL_BASE];
        QemuHostHLERequest request = {
            .size = sizeof(request),
            .version = 1,
            .module = external->module,
            .library = external->library,
            .nid = external->nid,
            .args = { a0, a1, a2, a3, a4, a5, a6 },
            .memory_opaque = cs,
            .read_guest = shadps4_hle_external_read,
            .write_guest = shadps4_hle_external_write,
        };
        uint64_t result = 0;
        int ret = qemu_host_hle_invoke(&request, &result);

        return ret < 0 ? shadps4_hle_host_result(ret) : result;
    }

    if ((number > SHADPS4_HLE_USBD_BEGIN &&
         number < SHADPS4_HLE_USBD_END) ||
        (number > SHADPS4_HLE_NP_MANAGER_BEGIN &&
         number < SHADPS4_HLE_NP_MANAGER_END) ||
        (number > SHADPS4_HLE_CAMERA_BEGIN &&
         number < SHADPS4_HLE_CAMERA_END) ||
        (number > SHADPS4_HLE_HMD_BEGIN &&
         number < SHADPS4_HLE_HMD_END) ||
        (number > SHADPS4_HLE_AUDIO3D_BEGIN &&
         number < SHADPS4_HLE_AUDIO3D_END)) {
        return shadps4_hle_dispatch_services3(hle, cs, number, a0, a1,
                                               a2, a3, a4, a5, a6);
    }
    if ((number > SHADPS4_HLE_PLAYGO_BEGIN &&
         number < SHADPS4_HLE_PLAYGO_END) ||
        (number > SHADPS4_HLE_WEBAPI2_PUSH_BEGIN &&
         number < SHADPS4_HLE_WEBAPI2_PUSH_END) ||
        (number > SHADPS4_HLE_NP_AUTH_BEGIN &&
         number < SHADPS4_HLE_NP_AUTH_END) ||
        (number > SHADPS4_HLE_IME_BEGIN &&
         number < SHADPS4_HLE_IME_END) ||
        (number > SHADPS4_HLE_TROPHY_BEGIN &&
         number < SHADPS4_HLE_TROPHY_END)) {
        return shadps4_hle_dispatch_services4(hle, cs, number, a0, a1,
                                               a2, a3, a4, a5, a6);
    }
    if (number > SHADPS4_HLE_VIDEODEC2_BEGIN &&
        number < SHADPS4_HLE_VR_TRACKER_END) {
        return shadps4_hle_dispatch_services5(hle, cs, number, a0, a1,
                                               a2, a3, a4, a5, a6);
    }
    if (number > SHADPS4_HLE_RESIDUAL_BEGIN &&
        number < SHADPS4_HLE_RESIDUAL_END) {
        return shadps4_hle_dispatch_services6(hle, cs, number, a0, a1,
                                               a2, a3, a4, a5, a6);
    }

    switch (number) {
    case SHADPS4_HLE_INTERNAL_FAULT:
        shadps4_hle_report_fault(hle, cs);
        shadps4_hle_cleanup(hle);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        return 0;
    case SHADPS4_HLE_TLS_GET_ADDR: {
        ShadPS4GuestTLSIndex index;
        uint32_t module;

        if (!shadps4_guest_rw(cs, a0, &index, sizeof(index), false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        module = le64_to_cpu(index.module);
        index.offset = le64_to_cpu(index.offset);
        if (!module || module > hle->tls_module_count ||
            index.offset >= hle->tls_guest_size[module - 1]) {
            return 0;
        }
        return hle->tls_guest_addr[module - 1] + index.offset;
    }
    case SHADPS4_HLE_GET_PROCESS_TIME:
        return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000;
    case SHADPS4_HLE_GET_PROCESS_TIME_COUNTER:
        return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    case SHADPS4_HLE_GET_PROCESS_TIME_COUNTER_FREQUENCY:
    case SHADPS4_HLE_GET_TSC_FREQUENCY:
        return NANOSECONDS_PER_SECOND;
    case SHADPS4_HLE_IS_NEO_MODE:
    case SHADPS4_HLE_HAS_NEO_MODE:
        return hle->neo_mode;
    case SHADPS4_HLE_GET_COMPILED_SDK_VERSION: {
        uint32_t version = cpu_to_le32(hle->compiled_sdk_version);

        if (!a0) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_guest_rw(cs, a0, &version, sizeof(version), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_GET_PROC_PARAM:
        return hle->proc_param_guest_addr;
    case SHADPS4_HLE_VIDEO_OPEN:
        if (a1 != 0 || a2 != 0 || hle->video_open) {
            return -SHADPS4_GUEST_EINVAL;
        }
        hle->video_open = true;
        return 1;
    case SHADPS4_HLE_VIDEO_CLOSE:
        if (a0 != 1 || !hle->video_open) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->video_open = false;
        memset(hle->video_buffers, 0, sizeof(hle->video_buffers));
        return 0;
    case SHADPS4_HLE_VIDEO_SET_BUFFER_ATTRIBUTE: {
        ShadPS4GuestVideoBufferAttribute attribute = { 0 };
        uint64_t pitch;

        if (!shadps4_hle_argument(cs, 6, &pitch) || !a0 || !a4 || !a5 ||
            !pitch || a4 > 4096 || a5 > 2160 || pitch < a4) {
            return -SHADPS4_GUEST_EINVAL;
        }
        attribute.pixel_format = cpu_to_le32(a1);
        attribute.tiling_mode = cpu_to_le32(a2);
        attribute.aspect_ratio = cpu_to_le32(a3);
        attribute.width = cpu_to_le32(a4);
        attribute.height = cpu_to_le32(a5);
        attribute.pitch = cpu_to_le32(pitch);
        return shadps4_guest_rw(cs, a0, &attribute, sizeof(attribute), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_VIDEO_REGISTER_BUFFERS:
        return shadps4_hle_video_register_buffers(hle, cs, a0, a1, a2,
                                                   a3, a4);
    case SHADPS4_HLE_VIDEO_SET_FLIP_RATE:
        if (a0 != 1 || !hle->video_open || a1 > 2) {
            return -SHADPS4_GUEST_EINVAL;
        }
        hle->video_flip_rate = a1;
        return 0;
    case SHADPS4_HLE_VIDEO_GET_FLIP_STATUS: {
        ShadPS4GuestVideoFlipStatus status = {
            .count = cpu_to_le64(hle->gpu->flip_count),
            .process_time = cpu_to_le64(
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000),
            .tsc = cpu_to_le64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)),
            .flip_arg = cpu_to_le64(hle->video_flip_arg),
            .submit_tsc = cpu_to_le64(
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)),
            .current_buffer = cpu_to_le32(hle->video_current_buffer),
        };

        if (a0 != 1 || !hle->video_open) {
            return -SHADPS4_GUEST_EBADF;
        }
        return shadps4_guest_rw(cs, a1, &status, sizeof(status), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_VIDEO_ADD_FLIP_EVENT:
        if (a0 < 0x100 || a0 >= 0x100 + SHADPS4_HLE_MAX_EQUEUES ||
            !hle->equeues[a0 - 0x100] || a1 != 1 || !hle->video_open) {
            return UINT32_C(0x8029000c);
        }
        return shadps4_hle_equeue_add(hle, a0, 6, -13, a2, 0,
                                      false, false);
    case SHADPS4_HLE_VIDEO_SUBMIT_FLIP:
        return shadps4_hle_video_flip(hle, cs, a0, a1, a3);
    case SHADPS4_HLE_VIDEO_ADD_VBLANK_EVENT:
        if (a1 != 1 || !hle->video_open) {
            return UINT32_C(0x8029000b);
        }
        return shadps4_hle_equeue_add(
            hle, a0, 7, -13, a2, NANOSECONDS_PER_SECOND / 60,
            false, false);
    case SHADPS4_HLE_VIDEO_DELETE_FLIP_EVENT:
        if (a1 != 1 || !hle->video_open) {
            return UINT32_C(0x8029000b);
        }
        return shadps4_hle_equeue_delete_event(hle, a0, 6, -13);
    case SHADPS4_HLE_VIDEO_DELETE_VBLANK_EVENT:
        if (a1 != 1 || !hle->video_open) {
            return UINT32_C(0x8029000b);
        }
        return shadps4_hle_equeue_delete_event(hle, a0, 7, -13);
    case SHADPS4_HLE_VIDEO_GET_RESOLUTION_STATUS: {
        uint8_t status[48] = { 0 };
        float inches = 50.0f;

        if (a0 != 1 || !hle->video_open) {
            return UINT32_C(0x8029000b);
        }
        if (!a1) {
            return UINT32_C(0x80290002);
        }
        stl_le_p(status, 1280);
        stl_le_p(status + 4, 720);
        stl_le_p(status + 8, 1280);
        stl_le_p(status + 12, 720);
        stq_le_p(status + 16, 3);
        memcpy(status + 24, &inches, sizeof(inches));
        return shadps4_guest_rw(cs, a1, status, sizeof(status), true) ?
               0 : UINT32_C(0x80290002);
    }
    case SHADPS4_HLE_VIDEO_IS_FLIP_PENDING:
        return a0 == 1 && hle->video_open ? 0 : UINT32_C(0x8029000b);
    case SHADPS4_HLE_VIDEO_UNREGISTER_BUFFERS: {
        bool found = false;
        uint32_t i;

        if (a0 != 1 || !hle->video_open) {
            return UINT32_C(0x8029000b);
        }
        for (i = 0; i < SHADPS4_HLE_MAX_VIDEO_BUFFERS; i++) {
            if (hle->video_buffers[i].registered &&
                hle->video_buffers[i].group == a1) {
                memset(&hle->video_buffers[i], 0,
                       sizeof(hle->video_buffers[i]));
                found = true;
            }
        }
        return found ? 0 : UINT32_C(0x8029000a);
    }
    case SHADPS4_HLE_VIDEO_GET_BUFFER_LABEL_ADDRESS: {
        uint8_t zero[64] = { 0 };

        if (a0 != 1 || !hle->video_open) {
            return UINT32_C(0x8029000b);
        }
        if (!a1) {
            return UINT32_C(0x80290002);
        }
        if (!hle->video_label_arena) {
            hle->video_label_arena = shadps4_hle_mmap(hle, cs, 0,
                                                       2 * MiB, 3);
            if ((int64_t)hle->video_label_arena < 0 ||
                !shadps4_guest_rw(cs, hle->video_label_arena, zero,
                                  sizeof(zero), true)) {
                hle->video_label_arena = 0;
                return UINT32_C(0x8029100c);
            }
        }
        return shadps4_hle_write_u64(cs, a1, hle->video_label_arena) ?
               UINT32_C(0x80290002) : 16;
    }
    case SHADPS4_HLE_VIDEO_GET_VBLANK_STATUS: {
        uint8_t status[40] = { 0 };
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        if (a0 != 1 || !hle->video_open) {
            return UINT32_C(0x8029000b);
        }
        if (!a1) {
            return UINT32_C(0x80290002);
        }
        stq_le_p(status, hle->gpu->vblank_count);
        stq_le_p(status + 8, now / 1000);
        stq_le_p(status + 16, now);
        return shadps4_guest_rw(cs, a1, status, sizeof(status), true) ?
               0 : UINT32_C(0x80290002);
    }
    case SHADPS4_HLE_VIDEO_GET_DEVICE_CAPABILITY:
        if (a0 != 1 || !hle->video_open) {
            return UINT32_C(0x8029000b);
        }
        return shadps4_hle_write_u64(cs, a1, 0) ?
               UINT32_C(0x80290002) : 0;
    case SHADPS4_HLE_VIDEO_WAIT_VBLANK:
        return a0 == 1 && hle->video_open ? 0 : UINT32_C(0x8029000b);
    case SHADPS4_HLE_VIDEO_GET_EVENT_ID:
    case SHADPS4_HLE_VIDEO_GET_EVENT_DATA:
    case SHADPS4_HLE_VIDEO_GET_EVENT_COUNT: {
        ShadPS4GuestEvent event;
        uint64_t data;

        if (!a0 || !shadps4_guest_rw(cs, a0, &event,
                                     sizeof(event), false)) {
            return UINT32_C(0x80290002);
        }
        if (le16_to_cpu(event.filter) != (uint16_t)-13) {
            return UINT32_C(0x8029000d);
        }
        data = le64_to_cpu(event.data);
        if (number == SHADPS4_HLE_VIDEO_GET_EVENT_DATA) {
            return !a1 || shadps4_hle_write_u64(cs, a1,
                                                (int64_t)data >> 16) ?
                   UINT32_C(0x80290002) : 0;
        }
        if (number == SHADPS4_HLE_VIDEO_GET_EVENT_COUNT) {
            return (data >> 12) & 0xf;
        }
        switch (le64_to_cpu(event.ident)) {
        case 6: return 0;
        case 7:
        case 0x63: return 1;
        case 0x59: return 2;
        case 0x51: return 8;
        case 0x58: return 12;
        default: return UINT32_C(0x8029000d);
        }
    }
    case SHADPS4_HLE_VIDEO_SET_GAMMA: {
        float gamma;
        uint32_t bits = env->xmm_regs[0].ZMM_L(0);

        memcpy(&gamma, &bits, sizeof(gamma));
        if (!a0) {
            return UINT32_C(0x80290002);
        }
        if (gamma < 0.1f || gamma > 2.0f) {
            return UINT32_C(0x80290001);
        }
        return shadps4_guest_rw(cs, a0, &bits, sizeof(bits), true) ?
               0 : UINT32_C(0x80290002);
    }
    case SHADPS4_HLE_VIDEO_ADJUST_COLOR: {
        uint32_t bits;
        float gamma;

        if (a0 != 1 || !hle->video_open) {
            return UINT32_C(0x8029000b);
        }
        if (!a1 || !shadps4_guest_rw(cs, a1, &bits, sizeof(bits), false)) {
            return UINT32_C(0x80290002);
        }
        bits = le32_to_cpu(bits);
        memcpy(&gamma, &bits, sizeof(bits));
        if (gamma < 0.1f || gamma > 2.0f) {
            return UINT32_C(0x80290001);
        }
        hle->video_gamma = gamma;
        return 0;
    }
    case SHADPS4_HLE_VIDEO_MODE_SET_ANY: {
        g_autofree uint8_t *mode = NULL;

        if (!a0 || a1 < 32 || a1 > 4096) {
            return 0;
        }
        mode = g_malloc(a1);
        memset(mode, 0xff, a1);
        stl_le_p(mode, a1);
        shadps4_guest_rw(cs, a0, mode, a1, true);
        return 0;
    }
    case SHADPS4_HLE_VIDEO_CONFIGURE_MODE: {
        uint8_t mode[32];

        if (a0 != 1 || !hle->video_open) {
            return UINT32_C(0x8029000b);
        }
        if (a1 || !a2 || a4 < sizeof(mode) ||
            !shadps4_guest_rw(cs, a2, mode, sizeof(mode), false) ||
            (mode[6] != 0xff && mode[6] != 12)) {
            return UINT32_C(0x80290001);
        }
        return 0;
    }
    case SHADPS4_HLE_VIDEO_CHANGE_BUFFER_ATTRIBUTE: {
        ShadPS4GuestVideoBufferAttribute attribute;
        uint32_t format;
        uint32_t i;

        if (a0 != 1 || !hle->video_open) {
            return UINT32_C(0x8029000b);
        }
        if (!a2 || !shadps4_guest_rw(cs, a2, &attribute,
                                     sizeof(attribute), false) ||
            !shadps4_hle_video_format(le32_to_cpu(attribute.pixel_format),
                                      &format) ||
            (le32_to_cpu(attribute.tiling_mode) != 0 &&
             le32_to_cpu(attribute.tiling_mode) != 1) ||
            !le32_to_cpu(attribute.width) ||
            !le32_to_cpu(attribute.height) ||
            le32_to_cpu(attribute.pitch) < le32_to_cpu(attribute.width) ||
            le32_to_cpu(attribute.width) > 4096 ||
            le32_to_cpu(attribute.height) > 2160 ||
            le32_to_cpu(attribute.pitch) > 65536 / 4) {
            return UINT32_C(0x80290003);
        }
        for (i = 0; i < SHADPS4_HLE_MAX_VIDEO_BUFFERS; i++) {
            ShadPS4HLEVideoBuffer *buffer = &hle->video_buffers[i];

            if (buffer->registered && buffer->group == a1) {
                buffer->width = le32_to_cpu(attribute.width);
                buffer->height = le32_to_cpu(attribute.height);
                buffer->pitch = le32_to_cpu(attribute.pitch);
                buffer->format = format;
                buffer->tiling_mode = le32_to_cpu(attribute.tiling_mode);
            }
        }
        return 0;
    }
    case SHADPS4_HLE_PAD_INIT:
        hle->pad_initialized = true;
        return 0;
    case SHADPS4_HLE_PAD_OPEN:
        if (!hle->pad_initialized || (int64_t)a0 < 0 ||
            a2 >= QEMU_HOST_MAX_GAMEPADS || hle->pad_open[a2]) {
            return -SHADPS4_GUEST_EINVAL;
        }
        hle->pad_open[a2] = true;
        hle->pad_user_id[a2] = a0;
        hle->pad_port_type[a2] = a1;
        hle->pad_port_index[a2] = a2;
        return a2 + 1;
    case SHADPS4_HLE_PAD_CLOSE:
        if (!a0 || a0 > QEMU_HOST_MAX_GAMEPADS || !hle->pad_open[a0 - 1]) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->pad_open[a0 - 1] = false;
        hle->pad_user_id[a0 - 1] = 0;
        hle->pad_port_type[a0 - 1] = 0;
        hle->pad_port_index[a0 - 1] = 0;
        return 0;
    case SHADPS4_HLE_PAD_READ:
        return shadps4_hle_pad_read(hle, cs, a0, a1, a2);
    case SHADPS4_HLE_PAD_SET_VIBRATION:
        return shadps4_hle_pad_output(hle, cs, a0, a1, false);
    case SHADPS4_HLE_PAD_SET_LIGHT_BAR:
        return shadps4_hle_pad_output(hle, cs, a0, a1, true);
    case SHADPS4_HLE_PAD_SET_MOTION_STATE:
        if (!a0 || a0 > QEMU_HOST_MAX_GAMEPADS || !hle->pad_open[a0 - 1]) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->pad_motion_enabled[a0 - 1] = a1;
        return 0;
    case SHADPS4_HLE_PAD_GET_CONTROLLER_INFO: {
        ShadPS4PadDeviceState pad;
        ShadPS4GuestPadControllerInfo info = {
            .pixel_density = 1.0f,
            .touch_width = cpu_to_le16(1920),
            .touch_height = cpu_to_le16(942),
            .dead_zone_left = 2,
            .dead_zone_right = 2,
            .connection_type = 0,
            .connected_count = 1,
            .device_class = 0,
        };

        if (!a0 || a0 > QEMU_HOST_MAX_GAMEPADS || !hle->pad_open[a0 - 1] ||
            !shadps4_io_get_pad(hle->io, a0 - 1, &pad)) {
            return -SHADPS4_GUEST_EBADF;
        }
        info.connected = pad.connected;
        return shadps4_guest_rw(cs, a1, &info, sizeof(info), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_PAD_GET_EXT_CONTROLLER_INFO: {
        ShadPS4PadDeviceState pad;
        ShadPS4GuestPadExtendedControllerInfo info = { 0 };

        if (!a0 || a0 > QEMU_HOST_MAX_GAMEPADS ||
            !hle->pad_open[a0 - 1] ||
            !shadps4_io_get_pad(hle->io, a0 - 1, &pad)) {
            return -SHADPS4_GUEST_EBADF;
        }
        info.base.pixel_density = 1.0f;
        info.base.touch_width = cpu_to_le16(1920);
        info.base.touch_height = cpu_to_le16(942);
        info.base.dead_zone_left = 2;
        info.base.dead_zone_right = 2;
        info.base.connection_type = 0;
        info.base.connected_count = 1;
        info.base.connected = pad.connected;
        info.base.device_class = 0;
        return shadps4_guest_rw(cs, a1, &info, sizeof(info), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_PAD_GET_DEVICE_CLASS_INFO: {
        ShadPS4GuestPadDeviceClassInfo info = { 0 };

        if (!a0 || a0 > QEMU_HOST_MAX_GAMEPADS ||
            !hle->pad_open[a0 - 1]) {
            return -SHADPS4_GUEST_EBADF;
        }
        return shadps4_guest_rw(cs, a1, &info, sizeof(info), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_PAD_GET_HANDLE:
    {
        uint32_t i;

        if (!hle->pad_initialized || (int64_t)a0 == -1) {
            return -SHADPS4_GUEST_EBADF;
        }
        for (i = 0; i < QEMU_HOST_MAX_GAMEPADS; i++) {
            if (hle->pad_open[i] && hle->pad_user_id[i] == (int32_t)a0 &&
                hle->pad_port_type[i] == (int32_t)a1 &&
                hle->pad_port_index[i] == (int32_t)a2) {
                return i + 1;
            }
        }
        return -SHADPS4_GUEST_EBADF;
    }
    case SHADPS4_HLE_PAD_GET_INFO: {
        ShadPS4GuestPadInfo info = {
            .unknown1 = cpu_to_le32(1),
            .pad_handle = cpu_to_le32(1),
            .unknown3 = cpu_to_le32(0x101),
            .colour = cpu_to_le32(0xff0000),
        };

        return a0 && shadps4_guest_rw(cs, a0, &info, sizeof(info), true) ?
               0 : -SHADPS4_GUEST_EINVAL;
    }
    case SHADPS4_HLE_PAD_READ_STATE: {
        uint64_t ret = shadps4_hle_pad_read(hle, cs, a0, a1, 1);

        return ret == 1 ? 0 : ret;
    }
    case SHADPS4_HLE_PAD_RESET_LIGHT_BAR:
        return shadps4_hle_pad_set_default_lightbar(hle, a0);
    case SHADPS4_HLE_PAD_RESET_ORIENTATION:
        if (!a0 || a0 > QEMU_HOST_MAX_GAMEPADS ||
            !hle->pad_open[a0 - 1]) {
            return -SHADPS4_GUEST_EBADF;
        }
        return 0;
    case SHADPS4_HLE_AUDIO_OUT_INIT:
        return 0;
    case SHADPS4_HLE_AUDIO_OUT_OPEN:
        return shadps4_hle_audio_out_open(hle, a1, a3, a4, a5);
    case SHADPS4_HLE_AUDIO_OUT_CLOSE:
        if (!a0 || a0 > SHADPS4_HLE_MAX_AUDIO_PORTS ||
            !hle->audio_out[a0 - 1].open) {
            return -SHADPS4_GUEST_EBADF;
        }
        memset(&hle->audio_out[a0 - 1], 0,
               sizeof(hle->audio_out[a0 - 1]));
        return 0;
    case SHADPS4_HLE_AUDIO_OUT_OUTPUT:
        return shadps4_hle_audio_out_output(hle, cs, a0, a1);
    case SHADPS4_HLE_AUDIO_OUT_OUTPUTS: {
        uint32_t i;

        if (!a0 || !a1 || a1 > SHADPS4_HLE_MAX_AUDIO_PORTS) {
            return -SHADPS4_GUEST_EINVAL;
        }
        for (i = 0; i < a1; i++) {
            ShadPS4GuestAudioOutputParam param;
            uint64_t result;

            if (!shadps4_guest_rw(cs, a0 + i * sizeof(param), &param,
                                  sizeof(param), false)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            result = shadps4_hle_audio_out_output(
                hle, cs, le32_to_cpu(param.handle), le64_to_cpu(param.data));
            if (result) {
                return result;
            }
        }
        return 0;
    }
    case SHADPS4_HLE_AUDIO_OUT_SET_VOLUME: {
        ShadPS4HLEAudioPort *port;
        uint8_t volume[8];
        uint32_t i;

        if (!a0 || a0 > SHADPS4_HLE_MAX_AUDIO_PORTS ||
            !hle->audio_out[a0 - 1].open || !a2) {
            return -SHADPS4_GUEST_EBADF;
        }
        port = &hle->audio_out[a0 - 1];
        memset(volume, 255, sizeof(volume));
        for (i = 0; i < port->channels; i++) {
            int32_t level;

            if (!(a1 & (1U << i))) {
                continue;
            }
            if (!shadps4_guest_rw(cs, a2 + i * sizeof(level), &level,
                                  sizeof(level), false)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            level = le32_to_cpu(level);
            volume[i] = MIN(MAX(level, 0), 32768) * 255 / 32768;
        }
        return shadps4_io_set_audio_volume(hle->io, false, port->channels,
                                            volume) ?
               0 : -SHADPS4_GUEST_EINVAL;
    }
    case SHADPS4_HLE_AUDIO_OUT_GET_LAST_OUTPUT_TIME: {
        uint64_t value;

        if (!a0 || a0 > SHADPS4_HLE_MAX_AUDIO_PORTS || !a1 ||
            !hle->audio_out[a0 - 1].open) {
            return -SHADPS4_GUEST_EBADF;
        }
        value = cpu_to_le64(hle->audio_out[a0 - 1].last_output_time);
        return shadps4_guest_rw(cs, a1, &value, sizeof(value), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_AUDIO_OUT_GET_PORT_STATE: {
        ShadPS4HLEAudioPort *port;
        ShadPS4GuestAudioPortState state = { 0 };

        if (!a0 || a0 > SHADPS4_HLE_MAX_AUDIO_PORTS || !a1 ||
            !hle->audio_out[a0 - 1].open) {
            return -SHADPS4_GUEST_EBADF;
        }
        port = &hle->audio_out[a0 - 1];
        state.volume = cpu_to_le16(-1);
        if (port->port_type == 2 || port->port_type == 3) {
            state.output = cpu_to_le16(0x40);
            state.channels = 1;
        } else if (port->port_type == 4) {
            state.output = cpu_to_le16(0x04);
            state.channels = 1;
            state.volume = cpu_to_le16(127);
        } else if (port->port_type == 127) {
            state.output = cpu_to_le16(0x80);
        } else {
            state.output = cpu_to_le16(0x01);
            state.channels = MIN(port->channels, 2);
        }
        return shadps4_guest_rw(cs, a1, &state, sizeof(state), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_AUDIO_OUT_GET_SYSTEM_STATE: {
        ShadPS4GuestAudioSystemState state = { 0 };

        return a0 && shadps4_guest_rw(cs, a0, &state,
                                      sizeof(state), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_AUDIO_OUT_SET_MIX_LEVEL_PAD_SPK:
        if (!a0 || a0 > SHADPS4_HLE_MAX_AUDIO_PORTS ||
            !hle->audio_out[a0 - 1].open ||
            hle->audio_out[a0 - 1].port_type != 4 || (int64_t)a1 > 32768) {
            return -SHADPS4_GUEST_EINVAL;
        }
        hle->audio_out[a0 - 1].mix_level = a1;
        return 0;
    case SHADPS4_HLE_AUDIO_OUT_MASTERING_INIT:
        if (a0) {
            return UINT32_C(0x80260201);
        }
        hle->audio_mastering_initialized = true;
        return 0;
    case SHADPS4_HLE_AUDIO_IN_OPEN:
        return shadps4_hle_audio_in_open(hle, a3, a4, a5);
    case SHADPS4_HLE_AUDIO_IN_CLOSE:
        if (!a0 || a0 > SHADPS4_HLE_MAX_AUDIO_PORTS ||
            !hle->audio_in[a0 - 1].open) {
            return -SHADPS4_GUEST_EBADF;
        }
        memset(&hle->audio_in[a0 - 1], 0,
               sizeof(hle->audio_in[a0 - 1]));
        return 0;
    case SHADPS4_HLE_AUDIO_IN_INPUT:
        return shadps4_hle_audio_in_input(hle, cs, a0, a1);
    case SHADPS4_HLE_AUDIO_IN_GET_SILENT_STATE:
        if (!a0 || a0 > SHADPS4_HLE_MAX_AUDIO_PORTS ||
            !hle->audio_in[a0 - 1].open) {
            return -SHADPS4_GUEST_EBADF;
        }
        return 0;
    case SHADPS4_HLE_SSL_INIT:
        if (a0 > 256 * MiB || hle->ssl_next_id == INT32_MAX) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return ++hle->ssl_next_id;
    case SHADPS4_HLE_GNM_SUBMIT_COMMAND_BUFFERS: {
        uint64_t result = shadps4_hle_gnm_submit(hle, cs, a0, a1, a2, 0);

        if (!result && a3 && a4) {
            result = shadps4_hle_gnm_submit(hle, cs, a0, a3, a4, 1);
        }
        return result;
    }
    case SHADPS4_HLE_GNM_SUBMIT_AND_FLIP: {
        uint64_t packed_flip;
        uint64_t flip_arg;
        uint64_t result = shadps4_hle_gnm_submit(hle, cs, a0, a1, a2, 0);

        if (!result && a3 && a4) {
            result = shadps4_hle_gnm_submit(hle, cs, a0, a3, a4, 1);
        }
        /* Orbis packs the two trailing u32 arguments into one stack slot:
         * low=buffer index, high=flip mode.  flip_arg follows that slot. */
        if (!shadps4_hle_argument(cs, 6, &packed_flip) ||
            !shadps4_hle_argument(cs, 7, &flip_arg)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        return result ? result : shadps4_hle_video_flip(
            hle, cs, (uint32_t)a5, (uint32_t)packed_flip, flip_arg);
    }
    case SHADPS4_HLE_GNM_SUBMIT_DONE:
        return 0;
    case SHADPS4_HLE_GNM_ARE_SUBMITS_ALLOWED:
        return 1;
    case SHADPS4_HLE_GNM_MAP_QUEUE:
        return shadps4_gpu_map_queue(hle->gpu, cs, a0, a1, a2, a3, a4);
    case SHADPS4_HLE_GNM_UNMAP_QUEUE:
        return shadps4_gpu_unmap_queue(hle->gpu, a0) ?
               0 : -SHADPS4_GUEST_EINVAL;
    case SHADPS4_HLE_GNM_DING_DONG:
        return shadps4_gpu_ding_dong(hle->gpu, cs, a0, a1) ?
               0 : -SHADPS4_GUEST_EINVAL;
    case SHADPS4_HLE_GNM_COMPAT_COMMAND:
        return shadps4_hle_gnm_compat_command(cs, a0, a1);
    case SHADPS4_HLE_GNM_COMPAT_INIT_COMMAND: {
        uint64_t result = shadps4_hle_gnm_compat_command(cs, a0, a1);

        return result ? result : a1;
    }
    case SHADPS4_HLE_GNM_WORKLOAD_CREATE: {
        uint32_t stream = cpu_to_le32(1);

        if (!a0 || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_guest_rw(cs, a1, &stream, sizeof(stream), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_GNM_WORKLOAD_BEGIN: {
        uint64_t workload = cpu_to_le64(a0 < 16 ? 1 : 0);

        if (!a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_guest_rw(cs, a1, &workload,
                                sizeof(workload), true) ?
               (a0 >= 16) : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_GNM_SUBMIT_WORKLOAD: {
        uint64_t result = shadps4_hle_gnm_submit(hle, cs, a1, a2, a3, 0);

        if (!result && a4 && a5) {
            result = shadps4_hle_gnm_submit(hle, cs, a1, a4, a5, 1);
        }
        return result;
    }
    case SHADPS4_HLE_GNM_SUBMIT_FLIP_WORKLOAD: {
        uint64_t packed_video;
        uint64_t flip_arg;
        uint64_t result = shadps4_hle_gnm_submit(hle, cs, a1, a2, a3, 0);

        if (!shadps4_hle_argument(cs, 6, &packed_video) ||
            !shadps4_hle_argument(cs, 8, &flip_arg)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        if (!result && a4 && a5) {
            result = shadps4_hle_gnm_submit(hle, cs, a1, a4, a5, 1);
        }
        return result ? result : shadps4_hle_video_flip(
            hle, cs, (uint32_t)packed_video, packed_video >> 32, flip_arg);
    }
    case SHADPS4_HLE_GNM_QUERY_TCA_UNITS:
        return hle->neo_mode ? 4 : 2;
    case SHADPS4_HLE_GNM_FAILURE:
        return UINT32_C(0x8eee00ff);
    case SHADPS4_HLE_GNM_VALIDATION_NOT_ENABLED:
        return UINT32_C(0x80d13fff);
    case SHADPS4_HLE_GNM_CAPTURE_FAILED:
        return UINT32_C(0x80d1500f);
    case SHADPS4_HLE_GNM_CAPTURE_RAZOR_NOT_LOADED:
        return UINT32_C(0x80d15001);
    case SHADPS4_HLE_GNM_INTERFACE_VERSION:
        return UINT32_C(0x80000000);
    case SHADPS4_HLE_GNM_DEBUG_HANDLE:
        return UINT64_MAX;
    case SHADPS4_HLE_GNM_GPU_CLOCK:
        return hle->neo_mode ? UINT64_C(911000000) : UINT64_C(800000000);
    case SHADPS4_HLE_GNM_OFFCHIP_BUFFER_SIZE:
        return UINT32_C(0x800000);
    case SHADPS4_HLE_GNM_LOGICAL_CU_MASK:
        return (int32_t)a1;
    case SHADPS4_HLE_GNM_ADD_EQ_EVENT:
        return shadps4_hle_equeue_add(hle, a0, a1, -14, a2, 0,
                                      false, false);
    case SHADPS4_HLE_GNM_DELETE_EQ_EVENT:
        return shadps4_hle_equeue_delete_event(hle, a0, a1, -14);
    case SHADPS4_HLE_GNM_WORKLOAD_END:
        return a0 && ((a0 >> 56) & 0xff) < 16 ? 0 : 2;
    case SHADPS4_HLE_GNM_TESS_RING_BASE:
        if (!hle->gnm_tessellation_ring_addr) {
            uint64_t address = shadps4_hle_mmap(hle, cs, 0, 256 * MiB, 3);

            if ((int64_t)address < 0) {
                return 0;
            }
            hle->gnm_tessellation_ring_addr = address;
        }
        return hle->gnm_tessellation_ring_addr;
    case SHADPS4_HLE_GNM_CONTEXT_INIT_SIZE:
        return 0x100;
    case SHADPS4_HLE_NET_INIT:
    case SHADPS4_HLE_NET_TERM:
        return 0;
    case SHADPS4_HLE_NET_SOCKET:
        return shadps4_hle_socket(hle, a1, a2, a3);
    case SHADPS4_HLE_NET_CLOSE:
        return shadps4_hle_close(hle, a0);
    case SHADPS4_HLE_NET_CONNECT:
        return shadps4_hle_connect(hle, cs, a0, a1, a2);
    case SHADPS4_HLE_NET_SEND:
        return shadps4_hle_net_transfer(hle, cs, a0, a1, a2, a3, true);
    case SHADPS4_HLE_NET_RECV:
        return shadps4_hle_net_transfer(hle, cs, a0, a1, a2, a3, false);
    case SHADPS4_HLE_NET_BIND:
        return shadps4_hle_network_address_call(hle, cs, a0, a1, a2, true);
    case SHADPS4_HLE_NET_LISTEN:
        return shadps4_hle_network_fd(hle, a0) ?
               shadps4_hle_network_result(
                   hle, qemu_host_network_listen(hle->network_handles[a0],
                                                  a1)) :
               -SHADPS4_GUEST_EBADF;
    case SHADPS4_HLE_NET_ACCEPT:
        return shadps4_hle_network_accept(hle, cs, a0, a1, a2);
    case SHADPS4_HLE_NET_SHUTDOWN:
        return shadps4_hle_network_fd(hle, a0) ?
               shadps4_hle_network_result(
                   hle, qemu_host_network_shutdown(hle->network_handles[a0],
                                                    a1)) :
               -SHADPS4_GUEST_EBADF;
    case SHADPS4_HLE_NET_SENDTO:
        return shadps4_hle_net_datagram(hle, cs, a0, a1, a2, a3, a4, a5,
                                        true);
    case SHADPS4_HLE_NET_RECVFROM:
        return shadps4_hle_net_datagram(hle, cs, a0, a1, a2, a3, a4, a5,
                                        false);
    case SHADPS4_HLE_NET_SENDMSG:
        return shadps4_hle_net_message(hle, cs, a0, a1, a2, true);
    case SHADPS4_HLE_NET_RECVMSG:
        return shadps4_hle_net_message(hle, cs, a0, a1, a2, false);
    case SHADPS4_HLE_NET_GETSOCKNAME:
        return shadps4_hle_net_get_name(hle, cs, a0, a1, a2, false);
    case SHADPS4_HLE_NET_GETPEERNAME:
        return shadps4_hle_net_get_name(hle, cs, a0, a1, a2, true);
    case SHADPS4_HLE_NET_GETSOCKOPT:
        return shadps4_hle_net_option(hle, cs, a0, a1, a2, a3, a4, false);
    case SHADPS4_HLE_NET_SETSOCKOPT:
        return shadps4_hle_net_option(hle, cs, a0, a1, a2, a3, a4, true);
    case SHADPS4_HLE_NET_SOCKET_ABORT:
        return shadps4_hle_network_fd(hle, a0) ?
               shadps4_hle_network_result(
                   hle, qemu_host_network_shutdown(hle->network_handles[a0],
                                                    2)) :
               -SHADPS4_GUEST_EBADF;
    case SHADPS4_HLE_NET_SOCKET_PAIR: {
        int64_t handles[2] = { -1, -1 };
        uint32_t guest_fds[2];
        int first;
        int second;
        int result;

        if (!a3) {
            return -SHADPS4_GUEST_EFAULT;
        }
        result = qemu_host_network_socket_pair(a0, a1, a2, handles);
        if (result < 0) {
            return shadps4_hle_network_result(hle, result);
        }
        first = shadps4_hle_network_adopt(hle, handles[0]);
        if (first < 0) {
            qemu_host_network_close(handles[1]);
            return first;
        }
        second = shadps4_hle_network_adopt(hle, handles[1]);
        if (second < 0) {
            shadps4_hle_close(hle, first);
            return second;
        }
        guest_fds[0] = cpu_to_le32(first);
        guest_fds[1] = cpu_to_le32(second);
        if (!shadps4_guest_rw(cs, a3, guest_fds,
                              sizeof(guest_fds), true)) {
            shadps4_hle_close(hle, first);
            shadps4_hle_close(hle, second);
            return -SHADPS4_GUEST_EFAULT;
        }
        return 0;
    }
    case SHADPS4_HLE_NET_HTONL:
    case SHADPS4_HLE_NET_NTOHL:
        return bswap32(a0);
    case SHADPS4_HLE_NET_HTONLL:
    case SHADPS4_HLE_NET_NTOHLL:
        return bswap64(a0);
    case SHADPS4_HLE_NET_HTONS:
    case SHADPS4_HLE_NET_NTOHS:
        return bswap16(a0);
    case SHADPS4_HLE_NET_POOL_CREATE:
        if (!a0 || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_hle_service_alloc(hle, SHADPS4_SERVICE_NET_POOL, 0);
    case SHADPS4_HLE_NET_POOL_DESTROY:
        return shadps4_hle_service_delete(hle, a0,
                                           SHADPS4_SERVICE_NET_POOL);
    case SHADPS4_HLE_NET_EPOLL_CREATE:
        return shadps4_hle_service_alloc(hle, SHADPS4_SERVICE_NET_EPOLL, 0);
    case SHADPS4_HLE_NET_EPOLL_CONTROL:
        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_NET_EPOLL) ?
               0 : -SHADPS4_GUEST_EBADF;
    case SHADPS4_HLE_NET_EPOLL_WAIT:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NET_EPOLL) ||
            (a2 && !a1)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return 0;
    case SHADPS4_HLE_NET_EPOLL_DESTROY:
        return shadps4_hle_service_delete(hle, a0,
                                           SHADPS4_SERVICE_NET_EPOLL);
    case SHADPS4_HLE_NET_ERRNO_LOC: {
        uint32_t error = cpu_to_le32(hle->net_errno);
        uint64_t address = env->segs[R_GS].base + 0x20;

        return shadps4_guest_rw(cs, address, &error, sizeof(error), true) ?
               address : 0;
    }
    case SHADPS4_HLE_NET_INET_PTON:
    case SHADPS4_HLE_NET_INET_PTON_EX: {
        char source[64];
        uint8_t address[16];
        size_t size = a0 == AF_INET ? 4 : a0 == AF_INET6 ? 16 : 0;

        if (!size || !shadps4_guest_read_string(cs, a1, source,
                                                 sizeof(source)) ||
            inet_pton(a0, source, address) != 1) {
            return 0;
        }
        return shadps4_guest_rw(cs, a2, address, size, true) ?
               1 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_NET_INET_NTOP: {
        uint8_t address[16];
        char result[INET6_ADDRSTRLEN];
        size_t size = a0 == AF_INET ? 4 : a0 == AF_INET6 ? 16 : 0;

        if (!size || !a2 || a3 < INET_ADDRSTRLEN ||
            !shadps4_guest_rw(cs, a1, address, size, false) ||
            !inet_ntop(a0, address, result, sizeof(result)) ||
            strlen(result) + 1 > a3) {
            return 0;
        }
        return shadps4_guest_rw(cs, a2, result, strlen(result) + 1, true) ?
               a2 : 0;
    }
    case SHADPS4_HLE_NET_GET_MAC_ADDRESS: {
        static const uint8_t address[6] = { 0x02, 0, 0, 0, 0, 1 };

        return a0 && shadps4_guest_rw(cs, a0, (void *)address,
                                      sizeof(address), true) ? 0 :
               -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_NET_RESOLVER_CREATE:
        return shadps4_hle_service_alloc(hle,
                                         SHADPS4_SERVICE_NET_RESOLVER, a0);
    case SHADPS4_HLE_NET_RESOLVER_DESTROY:
        return shadps4_hle_service_delete(hle, a0,
                                           SHADPS4_SERVICE_NET_RESOLVER);
    case SHADPS4_HLE_NET_RESOLVER_GET_ERROR: {
        uint32_t status = 0;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NET_RESOLVER)) {
            return -SHADPS4_GUEST_EBADF;
        }
        return a1 && shadps4_guest_rw(cs, a1, &status,
                                      sizeof(status), true) ? 0 :
               -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_NET_RESOLVER_NTOA:
    case SHADPS4_HLE_NET_RESOLVER_NTOA_MULTIPLE: {
        uint8_t address[4];
        int result;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NET_RESOLVER) || !a2) {
            return -SHADPS4_GUEST_EBADF;
        }
        result = shadps4_hle_resolve_ipv4(hle, cs, a1, address);
        if (result < 0) {
            return result;
        }
        if (number == SHADPS4_HLE_NET_RESOLVER_NTOA) {
            return shadps4_guest_rw(cs, a2, address,
                                    sizeof(address), true) ? 0 :
                   -SHADPS4_GUEST_EFAULT;
        } else {
            uint8_t info[384] = { 0 };

            memcpy(info, address, sizeof(address));
            stl_le_p(info + 16, AF_INET);
            stl_le_p(info + 320, 1);
            stl_le_p(info + 324, 1);
            return shadps4_guest_rw(cs, a2, info, sizeof(info), true) ? 0 :
                   -SHADPS4_GUEST_EFAULT;
        }
    }
    case SHADPS4_HLE_NET_UNSUPPORTED_SOCKET:
        hle->net_errno = SHADPS4_GUEST_ENOSYS;
        return -SHADPS4_GUEST_ENOSYS;
    case SHADPS4_HLE_HTTP_INIT:
        return shadps4_hle_service_alloc(hle,
                                         SHADPS4_SERVICE_HTTP_CONTEXT, 0);
    case SHADPS4_HLE_HTTP_TERM:
        return shadps4_hle_service_delete(hle, a0,
                                           SHADPS4_SERVICE_HTTP_CONTEXT);
    case SHADPS4_HLE_HTTP_CREATE_TEMPLATE:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_CONTEXT) ||
            shadps4_hle_http_string(cs, a1)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_hle_service_alloc(hle,
                                         SHADPS4_SERVICE_HTTP_TEMPLATE, a0);
    case SHADPS4_HLE_HTTP_DELETE_TEMPLATE:
        return shadps4_hle_service_delete(hle, a0,
                                           SHADPS4_SERVICE_HTTP_TEMPLATE);
    case SHADPS4_HLE_HTTP_CREATE_CONNECTION:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_TEMPLATE) ||
            shadps4_hle_http_string(cs, a1)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_HTTP_CONNECTION, a0);
    case SHADPS4_HLE_HTTP_DELETE_CONNECTION:
        return shadps4_hle_service_delete(
            hle, a0, SHADPS4_SERVICE_HTTP_CONNECTION);
    case SHADPS4_HLE_HTTP_CREATE_REQUEST:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_CONNECTION) ||
            shadps4_hle_http_string(cs, a1) ||
            shadps4_hle_http_string(cs, a2)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_hle_http_create_request(hle, cs, a0, a1, a2, a3);
    case SHADPS4_HLE_HTTP_DELETE_REQUEST: {
        int64_t host_handle;
        int ret;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST)) {
            return -SHADPS4_GUEST_EBADF;
        }
        host_handle = hle->service_host_handles[a0 - 0x400];
        ret = qemu_host_http_close(host_handle);
        if (ret < 0) {
            return shadps4_hle_host_result(ret);
        }
        hle->service_host_handles[a0 - 0x400] = -1;
        return shadps4_hle_service_delete(hle, a0,
                                           SHADPS4_SERVICE_HTTP_REQUEST);
    }
    case SHADPS4_HLE_HTTP_ADD_HEADER: {
        char name[256];
        char value[2048];
        int ret;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST) ||
            !shadps4_guest_read_string(cs, a1, name, sizeof(name)) ||
            !shadps4_guest_read_string(cs, a2, value, sizeof(value))) {
            return -SHADPS4_GUEST_EINVAL;
        }
        ret = qemu_host_http_add_header(
            hle->service_host_handles[a0 - 0x400], name, value, a3);
        return shadps4_hle_host_result(ret);
    }
    case SHADPS4_HLE_HTTP_SET_NONBLOCK:
        if (a0 <= 0x400 || a0 - 0x400 >= SHADPS4_HLE_MAX_SERVICE_OBJECTS ||
            !hle->service_objects[a0 - 0x400]) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->service_nonblock[a0 - 0x400] = a1 != 0;
        return 0;
    case SHADPS4_HLE_HTTP_CREATE_EPOLL: {
        int handle;
        uint64_t handle_le;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_CONTEXT) || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        handle = shadps4_hle_service_alloc(hle,
                                           SHADPS4_SERVICE_HTTP_EPOLL, a0);
        if (handle < 0) {
            return handle;
        }
        handle_le = cpu_to_le64(handle);
        return shadps4_guest_rw(cs, a1, &handle_le,
                                sizeof(handle_le), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_HTTP_DESTROY_EPOLL:
        return shadps4_hle_service_delete(hle, a1,
                                           SHADPS4_SERVICE_HTTP_EPOLL);
    case SHADPS4_HLE_HTTP_SET_EPOLL:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST) ||
            !shadps4_hle_service_is(hle, a1,
                                    SHADPS4_SERVICE_HTTP_EPOLL)) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->service_user_data[a0 - 0x400] = a1;
        hle->service_aux_value[a0 - 0x400] = a2;
        return 0;
    case SHADPS4_HLE_HTTP_UNSET_EPOLL:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST)) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->service_user_data[a0 - 0x400] = 0;
        hle->service_aux_value[a0 - 0x400] = 0;
        return 0;
    case SHADPS4_HLE_HTTP_WAIT_REQUEST:
        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_HTTP_EPOLL) ?
               0 : -SHADPS4_GUEST_EBADF;
    case SHADPS4_HLE_HTTP_SEND_REQUEST: {
        g_autofree void *body = NULL;
        int ret;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST)) {
            return -SHADPS4_GUEST_EBADF;
        }
        if (a2 > 32 * MiB || (!a1 && a2)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        if (a2) {
            body = g_malloc(a2);
            if (!shadps4_guest_rw(cs, a1, body, a2, false)) {
                return -SHADPS4_GUEST_EFAULT;
            }
        }
        ret = qemu_host_http_send(
            hle->service_host_handles[a0 - 0x400], body, a2);
        return shadps4_hle_host_result(ret);
    }
    case SHADPS4_HLE_HTTP_GET_STATUS: {
        int status;
        int ret;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST) || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        ret = qemu_host_http_get_status(
            hle->service_host_handles[a0 - 0x400], &status);
        return ret < 0 ? shadps4_hle_host_result(ret) :
               shadps4_hle_write_u32(cs, a1, status);
    }
    case SHADPS4_HLE_HTTP_GET_HEADERS: {
        g_autofree char *headers = NULL;
        uint64_t guest_address;
        uint64_t address_le;
        uint64_t size_le;
        size_t size = 0;
        size_t capacity;
        int ret;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST) ||
            !a1 || !a2) {
            return -SHADPS4_GUEST_EINVAL;
        }
        ret = qemu_host_http_get_headers(
            hle->service_host_handles[a0 - 0x400], NULL, &size);
        if ((ret < 0 && ret != -ENOSPC) || !size || size > 2 * MiB - 1) {
            return ret < 0 ? shadps4_hle_host_result(ret) :
                   -SHADPS4_GUEST_EIO;
        }
        capacity = size;
        headers = g_malloc(capacity + 1);
        ret = qemu_host_http_get_headers(
            hle->service_host_handles[a0 - 0x400], headers, &size);
        if (ret < 0 || size > capacity) {
            return ret < 0 ? shadps4_hle_host_result(ret) :
                   -SHADPS4_GUEST_EIO;
        }
        headers[size] = '\0';
        guest_address = shadps4_hle_arena_alloc(
            hle, cs, &hle->http_headers_arena, &hle->http_headers_offset,
            2 * MiB, size + 1);
        if (!guest_address ||
            !shadps4_guest_rw(cs, guest_address, headers, size + 1, true)) {
            return -SHADPS4_GUEST_ENOMEM;
        }
        address_le = cpu_to_le64(guest_address);
        size_le = cpu_to_le64(size);
        return shadps4_guest_rw(cs, a1, &address_le, sizeof(address_le), true) &&
               shadps4_guest_rw(cs, a2, &size_le, sizeof(size_le), true) ? 0 :
               -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_HTTP_GET_LENGTH: {
        uint64_t length;
        int ret;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST) ||
            !a1 || !a2) {
            return -SHADPS4_GUEST_EINVAL;
        }
        ret = qemu_host_http_get_length(
            hle->service_host_handles[a0 - 0x400], &length);
        if (ret < 0) {
            return shadps4_hle_host_result(ret);
        }
        return shadps4_hle_write_u32(cs, a1, 0) ||
               shadps4_hle_write_u64(cs, a2, length) ?
               -SHADPS4_GUEST_EFAULT : 0;
    }
    case SHADPS4_HLE_HTTP_READ_DATA: {
        g_autofree void *data = NULL;
        int64_t read;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST) ||
            (!a1 && a2) || a2 > 32 * MiB) {
            return -SHADPS4_GUEST_EINVAL;
        }
        data = g_malloc(MAX(a2, 1));
        read = qemu_host_http_read(
            hle->service_host_handles[a0 - 0x400], data, a2);
        if (read < 0) {
            return shadps4_hle_host_result(read);
        }
        if ((uint64_t)read > a2 ||
            (read && !shadps4_guest_rw(cs, a1, data, read, true))) {
            return -SHADPS4_GUEST_EFAULT;
        }
        return read;
    }
    case SHADPS4_HLE_HTTP_ABORT_REQUEST:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST)) {
            return -SHADPS4_GUEST_EBADF;
        }
        return shadps4_hle_host_result(qemu_host_http_abort(
            hle->service_host_handles[a0 - 0x400]));
    case SHADPS4_HLE_HTTP_CREATE_REQUEST_METHOD: {
        static const char *const methods[] = {
            "GET", "POST", "HEAD", "OPTIONS", "PUT", "DELETE",
            "TRACE", "CONNECT",
        };
        char url[2048];

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_CONNECTION) ||
            a1 >= ARRAY_SIZE(methods) ||
            !shadps4_guest_read_string(cs, a2, url, sizeof(url))) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_hle_http_create_request_values(
            hle, a0, methods[a1], url, a3);
    }
    case SHADPS4_HLE_HTTP_GET_NONBLOCK:
        if (a0 <= 0x400 || a0 - 0x400 >= SHADPS4_HLE_MAX_SERVICE_OBJECTS ||
            !hle->service_objects[a0 - 0x400] || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_hle_write_u32(
            cs, a1, hle->service_nonblock[a0 - 0x400] ? 1 : 0);
    case SHADPS4_HLE_HTTP_GET_EPOLL:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST) || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        if (shadps4_hle_write_u64(cs, a1,
                                 hle->service_user_data[a0 - 0x400])) {
            return -SHADPS4_GUEST_EFAULT;
        }
        return !a2 || !shadps4_hle_write_u64(
            cs, a2, hle->service_aux_value[a0 - 0x400]) ?
            0 : -SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_HTTP_GET_LAST_ERRNO:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST) || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_hle_write_u32(cs, a1, 0);
    case SHADPS4_HLE_HTTP_SET_OPTION:
        if (a0 <= 0x400 || a0 - 0x400 >= SHADPS4_HLE_MAX_SERVICE_OBJECTS ||
            !hle->service_objects[a0 - 0x400]) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->service_value[a0 - 0x400] = a1;
        return 0;
    case SHADPS4_HLE_HTTP_GET_OPTION:
        if (a0 <= 0x400 || a0 - 0x400 >= SHADPS4_HLE_MAX_SERVICE_OBJECTS ||
            !hle->service_objects[a0 - 0x400] || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_hle_write_u32(
            cs, a1, hle->service_value[a0 - 0x400] != 0);
    case SHADPS4_HLE_HTTP_ENABLE_FLAGS:
    case SHADPS4_HLE_HTTP_DISABLE_FLAGS:
        if (a0 <= 0x400 || a0 - 0x400 >= SHADPS4_HLE_MAX_SERVICE_OBJECTS ||
            !hle->service_objects[a0 - 0x400]) {
            return -SHADPS4_GUEST_EBADF;
        }
        if (number == SHADPS4_HLE_HTTP_ENABLE_FLAGS) {
            hle->service_value[a0 - 0x400] |= a1;
        } else {
            hle->service_value[a0 - 0x400] &= ~a1;
        }
        return 0;
    case SHADPS4_HLE_HTTP_SET_CONTENT_LENGTH:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST)) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->service_content_length[a0 - 0x400] = a1;
        return 0;
    case SHADPS4_HLE_HTTP_REMOVE_HEADER: {
        char name[256];

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST)) {
            return -SHADPS4_GUEST_EBADF;
        }
        if (!shadps4_guest_read_string(cs, a1, name, sizeof(name))) {
            return -SHADPS4_GUEST_EFAULT;
        }
        return shadps4_hle_host_result(qemu_host_http_add_header(
            hle->service_host_handles[a0 - 0x400], name, "", 0));
    }
    case SHADPS4_HLE_HTTP_PARSE_HEADER:
        return shadps4_http_parse_header(cs, a0, a1, a2, a3, a4);
    case SHADPS4_HLE_HTTP_PARSE_STATUS:
        return shadps4_http_parse_status(cs, a0, a1, a2, a3, a4, a5);
    case SHADPS4_HLE_HTTP_URI_BUILD:
        return shadps4_http_uri_build(cs, a0, a1, a2, a3, a4);
    case SHADPS4_HLE_HTTP_URI_ESCAPE:
        return shadps4_http_uri_transform(cs, a0, a1, a2, a3, true);
    case SHADPS4_HLE_HTTP_URI_MERGE:
        return shadps4_http_uri_merge(cs, a0, a1, a2, a3, a4, a5);
    case SHADPS4_HLE_HTTP_URI_PARSE:
        return shadps4_http_uri_parse(cs, a0, a1, a2, a3, a4);
    case SHADPS4_HLE_HTTP_URI_SWEEP:
        return shadps4_http_uri_sweep(cs, a0, a1, a2);
    case SHADPS4_HLE_HTTP_URI_UNESCAPE:
        return shadps4_http_uri_transform(cs, a0, a1, a2, a3, false);
    case SHADPS4_HLE_HTTPS_GET_CA_LIST: {
        uint8_t list[16] = { 0 };

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_CONTEXT)) {
            return -SHADPS4_GUEST_EBADF;
        }
        return a1 && shadps4_guest_rw(cs, a1, list, sizeof(list), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_HTTPS_FREE_CA_LIST:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_CONTEXT)) {
            return -SHADPS4_GUEST_EBADF;
        }
        return a1 ? 0 : -SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_HTTPS_LOAD_CERT:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_CONTEXT)) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->https_cert_loaded = true;
        return 0;
    case SHADPS4_HLE_HTTPS_UNLOAD_CERT:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_CONTEXT)) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->https_cert_loaded = false;
        return 0;
    case SHADPS4_HLE_SAVE_DATA_INIT:
        hle->save_data_initialized = true;
        return 0;
    case SHADPS4_HLE_SAVE_DATA_TERM:
        hle->save_data_initialized = false;
        hle->save_data_mounted = false;
        hle->save_data_dir[0] = '\0';
        return 0;
    case SHADPS4_HLE_SAVE_DATA_MOUNT:
        return shadps4_hle_save_mount(hle, cs, a0, a1, false);
    case SHADPS4_HLE_SAVE_DATA_MOUNT2:
        return shadps4_hle_save_mount(hle, cs, a0, a1, true);
    case SHADPS4_HLE_SAVE_DATA_UMOUNT: {
        char mount_point[16];

        if (!hle->save_data_initialized || !hle->save_data_mounted ||
            !shadps4_guest_rw(cs, a0, mount_point, sizeof(mount_point),
                              false) ||
            !memchr(mount_point, '\0', sizeof(mount_point)) ||
            strcmp(mount_point, "/savedata0")) {
            return -SHADPS4_GUEST_EINVAL;
        }
        hle->save_data_mounted = false;
        hle->save_data_dir[0] = '\0';
        return 0;
    }
    case SHADPS4_HLE_SAVE_DATA_SET_PARAM:
        if (!hle->save_data_initialized || !hle->save_data_mounted ||
            !a0 || !a2) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return 0;
    case SHADPS4_HLE_SAVE_DATA_GET_MOUNT_INFO: {
        ShadPS4GuestSaveMountInfo info = {
            .blocks = cpu_to_le64(32768),
            .free_blocks = cpu_to_le64(32768),
        };

        if (!hle->save_data_initialized || !hle->save_data_mounted ||
            !a0 || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_guest_rw(cs, a1, &info, sizeof(info), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_SAVE_DATA_DELETE:
        return hle->save_data_initialized ?
               shadps4_hle_save_delete(hle, cs, a0) :
               -SHADPS4_GUEST_EINVAL;
    case SHADPS4_HLE_SAVE_DATA_DIR_SEARCH: {
        uint32_t empty_result[5] = { 0 };

        if (!hle->save_data_initialized || !a0 || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_guest_rw(cs, a1, empty_result,
                                sizeof(empty_result), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_SAVE_DATA_GET_EVENT_RESULT:
        if (!hle->save_data_initialized || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return -SHADPS4_GUEST_ENOENT;
    case SHADPS4_HLE_SAVE_DATA_MEMORY_SETUP:
        return shadps4_hle_save_memory_setup(hle, cs, a0, a1);
    case SHADPS4_HLE_SAVE_DATA_MEMORY_GET:
        return shadps4_hle_save_memory_data(hle, cs, a0, false);
    case SHADPS4_HLE_SAVE_DATA_MEMORY_SET:
        return shadps4_hle_save_memory_data(hle, cs, a0, true);
    case SHADPS4_HLE_SAVE_DATA_MEMORY_SETUP_LEGACY:
        return shadps4_hle_save_memory_setup_legacy(hle, a1);
    case SHADPS4_HLE_SAVE_DATA_MEMORY_GET_LEGACY:
        return shadps4_hle_save_memory_legacy(hle, cs, a1, a2, a3, false);
    case SHADPS4_HLE_SAVE_DATA_MEMORY_SET_LEGACY:
        return shadps4_hle_save_memory_legacy(hle, cs, a1, a2, a3, true);
    case SHADPS4_HLE_SAVE_DATA_MEMORY_SYNC: {
        uint32_t sync[3];

        if (!hle->save_data_initialized || !a0 ||
            !shadps4_guest_rw(cs, a0, sync, sizeof(sync), false)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return le32_to_cpu(sync[1]) < ARRAY_SIZE(hle->save_memory_ready) &&
               hle->save_memory_ready[le32_to_cpu(sync[1])] ?
               0 : -SHADPS4_GUEST_EINVAL;
    }
    case SHADPS4_HLE_SAVE_DATA_GET_PROGRESS: {
        uint32_t progress;

        if (!hle->save_data_initialized || !a0) {
            return -SHADPS4_GUEST_EINVAL;
        }
        memcpy(&progress, &hle->save_data_progress, sizeof(progress));
        progress = cpu_to_le32(progress);
        return shadps4_guest_rw(cs, a0, &progress, sizeof(progress), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_SAVE_DATA_CLEAR_PROGRESS:
        if (!hle->save_data_initialized) {
            return -SHADPS4_GUEST_EINVAL;
        }
        hle->save_data_progress = 0.0f;
        return 0;
    case SHADPS4_HLE_SAVE_DATA_GET_PARAM: {
        static const uint32_t sizes[] = { 1328, 128, 128, 1024, 4, 8 };
        g_autofree uint8_t *value = NULL;
        uint32_t size;

        if (!shadps4_hle_save_mount_point(hle, cs, a0) ||
            a1 >= ARRAY_SIZE(sizes) || !a2) {
            return -SHADPS4_GUEST_EINVAL;
        }
        size = sizes[a1];
        if (a3 < size) {
            return -SHADPS4_GUEST_EINVAL;
        }
        value = g_malloc0(size);
        if (!shadps4_guest_rw(cs, a2, value, size, true)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        return !a4 || !shadps4_hle_write_u64(cs, a4, size) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_SAVE_DATA_ICON_LOAD:
        return shadps4_hle_save_icon(hle, cs, a0, a1, false);
    case SHADPS4_HLE_SAVE_DATA_ICON_SAVE:
        return shadps4_hle_save_icon(hle, cs, a0, a1, true);
    case SHADPS4_HLE_SAVE_DATA_BACKUP:
    case SHADPS4_HLE_SAVE_DATA_CHECK_BACKUP:
    case SHADPS4_HLE_SAVE_DATA_RESTORE_BACKUP:
        return shadps4_hle_save_backup_operation(hle, cs, a0, number);
    case SHADPS4_HLE_SAVE_DATA_TRANSFERRING_MOUNT: {
        ShadPS4GuestSaveMountResult result = { 0 };
        QemuHostStorageStat stat;
        char directory[33];
        char path[192];

        if (!hle->save_data_initialized || !a1 ||
            !shadps4_hle_save_request(hle, cs, a0, directory,
                                      sizeof(directory))) {
            return SHADPS4_SAVE_ERROR_PARAMETER;
        }
        g_snprintf(path, sizeof(path), "/titles/%s/savedata/%s",
                   hle->title_id, directory);
        if (qemu_host_storage_stat(path, &stat) < 0) {
            return SHADPS4_SAVE_ERROR_NOT_FOUND;
        }
        hle->save_data_mounted = true;
        pstrcpy(hle->save_data_dir, sizeof(hle->save_data_dir), directory);
        pstrcpy(result.mount_point, sizeof(result.mount_point), "/savedata0");
        return shadps4_guest_rw(cs, a1, &result, sizeof(result), true) ?
               0 : SHADPS4_SAVE_ERROR_PARAMETER;
    }
    case SHADPS4_HLE_APP_CONTENT_INIT: {
        uint32_t zero = 0;

        if (hle->app_content_initialized) {
            return UINT32_C(0x80d90003);
        }
        hle->app_content_initialized = true;
        return !a1 || shadps4_guest_rw(cs, a1 + 4, &zero, sizeof(zero),
                                       true) ?
               0 : UINT32_C(0x80d90002);
    }
    case SHADPS4_HLE_APP_CONTENT_GET_PARAM_INT:
        if (!a1 || a0 > 4) {
            return UINT32_C(0x80d90002);
        }
        if (a0 != 0) {
            return UINT32_C(0x80d90005);
        }
        return shadps4_hle_write_u32(cs, a1, 3) ?
               UINT32_C(0x80d90002) : 0;
    case SHADPS4_HLE_APP_CONTENT_GET_INFO:
    case SHADPS4_HLE_APP_CONTENT_GET_KEY:
    case SHADPS4_HLE_APP_CONTENT_MOUNT:
        if (!a1 || !a2) {
            return UINT32_C(0x80d90002);
        }
        return UINT32_C(0x80d90007);
    case SHADPS4_HLE_APP_CONTENT_GET_INFO_LIST:
        if ((!a1 || !a2) && !a3) {
            return UINT32_C(0x80d90002);
        }
        return !a3 || !shadps4_hle_write_u32(cs, a3, 0) ?
               0 : UINT32_C(0x80d90002);
    case SHADPS4_HLE_APP_CONTENT_TEMP_MOUNT: {
        static const char mount_point[16] = "/temp0";

        if (!a1) {
            return UINT32_C(0x80d90002);
        }
        return shadps4_guest_rw(cs, a1, (void *)mount_point,
                                sizeof(mount_point), true) ?
               0 : UINT32_C(0x80d90002);
    }
    case SHADPS4_HLE_APP_CONTENT_AVAILABLE_SPACE:
        return a0 && !shadps4_hle_write_u64(cs, a0, UINT64_C(1048576)) ?
               0 : UINT32_C(0x80d90002);
    case SHADPS4_HLE_MOVE_INIT:
        if (hle->move_initialized) {
            return UINT32_C(0x80ee0002);
        }
        hle->move_initialized = true;
        return 0;
    case SHADPS4_HLE_MOVE_OPEN:
        if (!hle->move_initialized) {
            return UINT32_C(0x80ee0001);
        }
        return shadps4_hle_service_alloc(hle, SHADPS4_SERVICE_MOVE, 0);
    case SHADPS4_HLE_MOVE_QUERY:
        return hle->move_initialized ? 1 : UINT32_C(0x80ee0001);
    case SHADPS4_HLE_MOVE_RESET:
        return hle->move_initialized ? 0 : UINT32_C(0x80ee0001);
    case SHADPS4_HLE_MOVE_CLOSE:
        if (!hle->move_initialized) {
            return UINT32_C(0x80ee0001);
        }
        return shadps4_hle_service_delete(hle, a0,
                                           SHADPS4_SERVICE_MOVE) ?
               UINT32_C(0x80ee0004) : 0;
    case SHADPS4_HLE_MOVE_TERM: {
        uint32_t i;

        if (!hle->move_initialized) {
            return UINT32_C(0x80ee0001);
        }
        for (i = 1; i < SHADPS4_HLE_MAX_SERVICE_OBJECTS; i++) {
            if (hle->service_objects[i] == SHADPS4_SERVICE_MOVE) {
                shadps4_hle_service_delete(hle, 0x400 + i,
                                           SHADPS4_SERVICE_MOVE);
            }
        }
        hle->move_initialized = false;
        return 0;
    }
    case SHADPS4_HLE_ZLIB_INIT:
        if (hle->zlib_initialized) {
            return UINT32_C(0x81120033);
        }
        hle->zlib_initialized = true;
        return 0;
    case SHADPS4_HLE_ZLIB_FINALIZE: {
        uint32_t i;

        if (!hle->zlib_initialized) {
            return UINT32_C(0x81120032);
        }
        for (i = 1; i < SHADPS4_HLE_MAX_SERVICE_OBJECTS; i++) {
            if (hle->service_objects[i] == SHADPS4_SERVICE_ZLIB_REQUEST) {
                shadps4_hle_service_delete(
                    hle, 0x400 + i, SHADPS4_SERVICE_ZLIB_REQUEST);
            }
        }
        hle->zlib_initialized = false;
        return 0;
    }
    case SHADPS4_HLE_ZLIB_INFLATE: {
        g_autofree uint8_t *source = NULL;
        g_autofree uint8_t *dest = NULL;
        uLongf dest_size = a3;
        int request;
        int zret;

        if (!hle->zlib_initialized) {
            return UINT32_C(0x81120032);
        }
        if (!a0 || !a1 || !a2 || !a3 || !a4 || a3 > 64 * KiB ||
            a3 % (2 * KiB) || a1 > 64 * MiB) {
            return UINT32_C(0x81120016);
        }
        source = g_malloc(a1);
        dest = g_malloc(a3);
        if (!shadps4_guest_rw(cs, a0, source, a1, false)) {
            return UINT32_C(0x8112000e);
        }
        zret = uncompress(dest, &dest_size, source, a1);
        if (zret == Z_OK &&
            !shadps4_guest_rw(cs, a2, dest, dest_size, true)) {
            return UINT32_C(0x8112000e);
        }
        request = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_ZLIB_REQUEST, 0);
        if (request < 0) {
            return UINT32_C(0x811200ff);
        }
        hle->service_value[request - 0x400] = dest_size;
        hle->service_user_data[request - 0x400] =
            zret == Z_OK ? 0 : zret == Z_BUF_ERROR ?
            UINT32_C(0x8112001c) : UINT32_C(0x811200ff);
        hle->service_active[request - 0x400] = true;
        return shadps4_hle_write_u64(cs, a4, request) ?
               UINT32_C(0x8112000e) : 0;
    }
    case SHADPS4_HLE_ZLIB_WAIT: {
        uint32_t i;

        if (!hle->zlib_initialized) {
            return UINT32_C(0x81120032);
        }
        if (!a0) {
            return UINT32_C(0x81120016);
        }
        for (i = 1; i < SHADPS4_HLE_MAX_SERVICE_OBJECTS; i++) {
            if (hle->service_objects[i] == SHADPS4_SERVICE_ZLIB_REQUEST &&
                hle->service_active[i]) {
                hle->service_active[i] = false;
                return shadps4_hle_write_u64(cs, a0, 0x400 + i) ?
                       UINT32_C(0x8112000e) : 0;
            }
        }
        return a1 ? UINT32_C(0x81120027) : UINT32_C(0x81120002);
    }
    case SHADPS4_HLE_ZLIB_RESULT:
        if (!hle->zlib_initialized) {
            return UINT32_C(0x81120032);
        }
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_ZLIB_REQUEST)) {
            return UINT32_C(0x81120002);
        }
        if (!a1 || !a2 || shadps4_hle_write_u32(
                cs, a1, hle->service_value[a0 - 0x400]) ||
            shadps4_hle_write_u32(
                cs, a2, hle->service_user_data[a0 - 0x400])) {
            return UINT32_C(0x81120016);
        }
        return 0;
    case SHADPS4_HLE_PNG_ENC_CREATE: {
        uint8_t param[16];
        int handle;

        if (!a0 || !a1 || !a3 ||
            !shadps4_guest_rw(cs, a0, param, sizeof(param), false)) {
            return UINT32_C(0x80690101);
        }
        if (ldl_le_p(param + 4) || !ldl_le_p(param + 8) ||
            ldl_le_p(param + 8) > 1000000) {
            return UINT32_C(0x80690102);
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_PNG_ENCODER, 0);
        return handle < 0 ? UINT32_C(0x80690120) :
               shadps4_hle_write_u64(cs, a3, handle) ?
               UINT32_C(0x80690101) : 0;
    }
    case SHADPS4_HLE_PNG_ENC_DELETE:
        return shadps4_hle_service_delete(
            hle, a0, SHADPS4_SERVICE_PNG_ENCODER) ?
            UINT32_C(0x80690104) : 0;
    case SHADPS4_HLE_PNG_ENC_ENCODE:
        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_PNG_ENCODER) ?
               shadps4_hle_png_encode(cs, a1, a2) :
               UINT32_C(0x80690104);
    case SHADPS4_HLE_PNG_ENC_QUERY: {
        uint8_t param[16];

        if (!a0 || !shadps4_guest_rw(cs, a0, param, sizeof(param), false)) {
            return UINT32_C(0x80690101);
        }
        if (ldl_le_p(param + 4) || ldl_le_p(param + 12) > 5) {
            return UINT32_C(0x80690103);
        }
        return ldl_le_p(param + 8) && ldl_le_p(param + 8) <= 1000000 ?
               16 : UINT32_C(0x80690102);
    }
    case SHADPS4_HLE_DISC_MAP_NO_BITMAP:
        return UINT32_C(0x81100004);
    case SHADPS4_HLE_DISC_MAP_ZERO:
        if (!a3 || !a4 || !a5 ||
            shadps4_hle_write_u32(cs, a3, 0) ||
            shadps4_hle_write_u32(cs, a4, 0) ||
            shadps4_hle_write_u32(cs, a5, 0)) {
            return UINT32_C(0x81100001);
        }
        return 0;
    case SHADPS4_HLE_FONT_CREATE_LIBRARY:
    case SHADPS4_HLE_FONT_CREATE_LIBRARY_EDITION:
    case SHADPS4_HLE_FONT_CREATE_RENDERER:
    case SHADPS4_HLE_FONT_CREATE_RENDERER_EDITION: {
        bool library = number == SHADPS4_HLE_FONT_CREATE_LIBRARY ||
                       number == SHADPS4_HLE_FONT_CREATE_LIBRARY_EDITION;
        bool edition = number == SHADPS4_HLE_FONT_CREATE_LIBRARY_EDITION ||
                       number == SHADPS4_HLE_FONT_CREATE_RENDERER_EDITION;
        uint64_t output = edition ? a3 : a2;
        int handle;

        if (!output) {
            return UINT32_C(0x80460002);
        }
        handle = shadps4_hle_service_alloc(
            hle, library ? SHADPS4_SERVICE_FONT_LIBRARY :
                           SHADPS4_SERVICE_FONT_RENDERER, 0);
        return handle < 0 ? UINT32_C(0x80460010) :
               shadps4_hle_write_u64(cs, output, handle) ?
               UINT32_C(0x80460002) : 0;
    }
    case SHADPS4_HLE_FONT_DESTROY_LIBRARY:
    case SHADPS4_HLE_FONT_DESTROY_RENDERER: {
        uint64_t handle;
        uint8_t type = number == SHADPS4_HLE_FONT_DESTROY_LIBRARY ?
                       SHADPS4_SERVICE_FONT_LIBRARY :
                       SHADPS4_SERVICE_FONT_RENDERER;

        if (!a0 || !shadps4_guest_rw(cs, a0, &handle, sizeof(handle), false)) {
            return UINT32_C(0x80460002);
        }
        handle = le64_to_cpu(handle);
        if (shadps4_hle_service_delete(hle, handle, type)) {
            return number == SHADPS4_HLE_FONT_DESTROY_LIBRARY ?
                   UINT32_C(0x80460004) : UINT32_C(0x80460007);
        }
        handle = 0;
        return shadps4_guest_rw(cs, a0, &handle, sizeof(handle), true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_OPEN:
    case SHADPS4_HLE_FONT_OPEN_INSTANCE: {
        uint64_t output = number == SHADPS4_HLE_FONT_OPEN_INSTANCE ? a2 : a4;
        uint64_t parent = a0;
        int handle;
        uint32_t scale = 0x41800000;

        if (!output ||
            (number == SHADPS4_HLE_FONT_OPEN &&
             !shadps4_hle_service_is(hle, parent,
                                     SHADPS4_SERVICE_FONT_LIBRARY)) ||
            (number == SHADPS4_HLE_FONT_OPEN_INSTANCE &&
             !shadps4_hle_service_is(hle, parent,
                                     SHADPS4_SERVICE_FONT_HANDLE))) {
            return UINT32_C(0x80460005);
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_FONT_HANDLE, parent);
        if (handle < 0) {
            return UINT32_C(0x80460010);
        }
        hle->service_value[handle - 0x400] =
            ((uint64_t)scale << 32) | scale;
        return shadps4_hle_write_u64(cs, output, handle) ?
               UINT32_C(0x80460002) : 0;
    }
    case SHADPS4_HLE_FONT_GET_SCALE: {
        uint32_t width;
        uint32_t height;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE)) {
            return UINT32_C(0x80460005);
        }
        if (!a1 || !a2) {
            return UINT32_C(0x80460002);
        }
        width = hle->service_value[a0 - 0x400];
        height = hle->service_value[a0 - 0x400] >> 32;
        return shadps4_guest_rw(cs, a1, &width, 4, true) &&
               shadps4_guest_rw(cs, a2, &height, 4, true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_SET_SCALE:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE)) {
            return UINT32_C(0x80460005);
        }
        hle->service_value[a0 - 0x400] =
            ((uint64_t)env->xmm_regs[1].ZMM_L(0) << 32) |
            env->xmm_regs[0].ZMM_L(0);
        return 0;
    case SHADPS4_HLE_FONT_GET_SLANT: {
        uint32_t value;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE)) {
            return UINT32_C(0x80460005);
        }
        value = hle->service_user_data[a0 - 0x400];
        return a1 && shadps4_guest_rw(cs, a1, &value, 4, true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_SET_SLANT:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE)) {
            return UINT32_C(0x80460005);
        }
        hle->service_user_data[a0 - 0x400] =
            env->xmm_regs[0].ZMM_L(0);
        return 0;
    case SHADPS4_HLE_FONT_GET_WEIGHT: {
        uint32_t x;
        uint32_t y;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE)) {
            return UINT32_C(0x80460005);
        }
        x = hle->service_aux_value[a0 - 0x400];
        y = hle->service_aux_value[a0 - 0x400] >> 32;
        if (!a1 || !a2 || !a3 ||
            !shadps4_guest_rw(cs, a1, &x, 4, true) ||
            !shadps4_guest_rw(cs, a2, &y, 4, true) ||
            shadps4_hle_write_u32(cs, a3, 0)) {
            return UINT32_C(0x80460002);
        }
        return 0;
    }
    case SHADPS4_HLE_FONT_SET_WEIGHT:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE)) {
            return UINT32_C(0x80460005);
        }
        hle->service_aux_value[a0 - 0x400] =
            ((uint64_t)env->xmm_regs[1].ZMM_L(0) << 32) |
            env->xmm_regs[0].ZMM_L(0);
        return 0;
    case SHADPS4_HLE_FONT_GET_METRICS: {
        uint32_t bits_w;
        uint32_t bits_h;
        float width, height;
        float metrics[8] = { 0 };

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE) || !a2) {
            return UINT32_C(0x80460005);
        }
        bits_w = hle->service_value[a0 - 0x400];
        bits_h = hle->service_value[a0 - 0x400] >> 32;
        memcpy(&width, &bits_w, 4);
        memcpy(&height, &bits_h, 4);
        metrics[0] = width * 0.6f;
        metrics[1] = height;
        metrics[3] = height * 0.8f;
        metrics[4] = metrics[0];
        metrics[6] = height * 0.5f;
        metrics[7] = height;
        return shadps4_guest_rw(cs, a2, metrics, sizeof(metrics), true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_GET_LAYOUT_H:
    case SHADPS4_HLE_FONT_GET_LAYOUT_V: {
        uint32_t bits_h;
        float height;
        float layout[3];

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE) || !a1) {
            return UINT32_C(0x80460005);
        }
        bits_h = hle->service_value[a0 - 0x400] >> 32;
        memcpy(&height, &bits_h, 4);
        layout[0] = height * 0.8f;
        layout[1] = height * 1.2f;
        layout[2] = height;
        return shadps4_guest_rw(cs, a1, layout, sizeof(layout), true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_GET_KERNING: {
        uint8_t zero[16] = { 0 };

        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_FONT_HANDLE) && a3 &&
               shadps4_guest_rw(cs, a3, zero, sizeof(zero), true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_CHAR_BIDI:
        if (!a0 || !a1) {
            return UINT32_C(0x80460002);
        }
        return shadps4_hle_write_u32(cs, a1, 0) ?
               UINT32_C(0x80460002) : 0;
    case SHADPS4_HLE_FONT_CHAR_CODE: {
        uint8_t character[64];

        if (!a0 || !a1 || !a2 ||
            !shadps4_guest_rw(cs, a0, character, sizeof(character), false) ||
            !shadps4_guest_rw(cs, a1, character + 24, 8, true) ||
            !shadps4_guest_rw(cs, a2, character + 40, 4, true)) {
            return UINT32_C(0x80460002);
        }
        return 0;
    }
    case SHADPS4_HLE_FONT_CHAR_ORDER: {
        uint64_t order;

        if (!a0 || !a1 ||
            !shadps4_guest_rw(cs, a0 + 16, &order, 8, false) ||
            !shadps4_guest_rw(cs, a1, &order, 8, true)) {
            return UINT32_C(0x80460002);
        }
        return 0;
    }
    case SHADPS4_HLE_FONT_CHAR_FORMAT:
    case SHADPS4_HLE_FONT_CHAR_SPACE: {
        uint8_t character[64];
        uint32_t code;
        uint64_t flags;

        if (!a0 || !shadps4_guest_rw(cs, a0, character,
                                     sizeof(character), false)) {
            return 0;
        }
        code = ldl_le_p(character + 40);
        flags = ldq_le_p(character + 56);
        return number == SHADPS4_HLE_FONT_CHAR_FORMAT ?
               (flags & 1 ? code : 0) :
               ((flags >> 8) & 0xff) == 0x0e ? code : 0;
    }
    case SHADPS4_HLE_FONT_CHAR_PREV:
    case SHADPS4_HLE_FONT_CHAR_NEXT: {
        uint64_t address = 0;

        if (a0) {
            shadps4_guest_rw(cs, a0 +
                (number == SHADPS4_HLE_FONT_CHAR_NEXT ? 8 : 0),
                &address, 8, false);
        }
        return le64_to_cpu(address);
    }
    case SHADPS4_HLE_FONT_STYLE_INIT: {
        uint8_t frame[96] = { 0 };

        if (!a0) {
            return UINT32_C(0x80460002);
        }
        stw_le_p(frame, 0x0f09);
        stl_le_p(frame + 4, 72);
        stl_le_p(frame + 8, 72);
        return shadps4_guest_rw(cs, a0, frame, sizeof(frame), true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_STYLE_GET_SCALE:
    case SHADPS4_HLE_FONT_STYLE_GET_DPI:
    case SHADPS4_HLE_FONT_STYLE_GET_SLANT:
    case SHADPS4_HLE_FONT_STYLE_GET_WEIGHT: {
        uint8_t frame[96];
        uint32_t zero = 0;

        if (!a0 ||
            !shadps4_guest_rw(cs, a0, frame, sizeof(frame), false) ||
            lduw_le_p(frame) != 0x0f09) {
            return UINT32_C(0x80460002);
        }
        if (number == SHADPS4_HLE_FONT_STYLE_GET_SCALE) {
            return a1 && a2 &&
                   shadps4_guest_rw(cs, a1, frame + 20, 4, true) &&
                   shadps4_guest_rw(cs, a2, frame + 24, 4, true) ?
                   0 : UINT32_C(0x80460002);
        }
        if (number == SHADPS4_HLE_FONT_STYLE_GET_DPI) {
            return a1 && a2 &&
                   shadps4_guest_rw(cs, a1, frame + 4, 4, true) &&
                   shadps4_guest_rw(cs, a2, frame + 8, 4, true) ?
                   0 : UINT32_C(0x80460002);
        }
        if (number == SHADPS4_HLE_FONT_STYLE_GET_SLANT) {
            return a1 && shadps4_guest_rw(cs, a1, frame + 36, 4, true) ?
                   0 : UINT32_C(0x80460002);
        }
        return a1 && a2 && a3 &&
               shadps4_guest_rw(cs, a1, frame + 28, 4, true) &&
               shadps4_guest_rw(cs, a2, frame + 32, 4, true) &&
               shadps4_guest_rw(cs, a3, &zero, 4, true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_STYLE_SET_SCALE:
    case SHADPS4_HLE_FONT_STYLE_SET_DPI:
    case SHADPS4_HLE_FONT_STYLE_SET_SLANT:
    case SHADPS4_HLE_FONT_STYLE_SET_WEIGHT:
    case SHADPS4_HLE_FONT_STYLE_UNSET_SLANT:
    case SHADPS4_HLE_FONT_STYLE_UNSET_WEIGHT:
    case SHADPS4_HLE_FONT_STYLE_UNSET_SCALE: {
        uint8_t frame[96];

        if (!a0 || !shadps4_guest_rw(cs, a0, frame, sizeof(frame), false) ||
            lduw_le_p(frame) != 0x0f09) {
            return UINT32_C(0x80460002);
        }
        if (number == SHADPS4_HLE_FONT_STYLE_SET_SCALE) {
            stl_le_p(frame + 20, env->xmm_regs[0].ZMM_L(0));
            stl_le_p(frame + 24, env->xmm_regs[1].ZMM_L(0));
            frame[2] |= 1;
        } else if (number == SHADPS4_HLE_FONT_STYLE_SET_DPI) {
            stl_le_p(frame + 4, a1);
            stl_le_p(frame + 8, a2);
        } else if (number == SHADPS4_HLE_FONT_STYLE_SET_SLANT) {
            stl_le_p(frame + 36, env->xmm_regs[0].ZMM_L(0));
            frame[2] |= 2;
        } else if (number == SHADPS4_HLE_FONT_STYLE_SET_WEIGHT) {
            stl_le_p(frame + 28, env->xmm_regs[0].ZMM_L(0));
            stl_le_p(frame + 32, env->xmm_regs[1].ZMM_L(0));
            frame[2] |= 4;
        } else if (number == SHADPS4_HLE_FONT_STYLE_UNSET_SLANT) {
            frame[2] &= ~2;
            memset(frame + 36, 0, 4);
        } else if (number == SHADPS4_HLE_FONT_STYLE_UNSET_WEIGHT) {
            frame[2] &= ~4;
            memset(frame + 28, 0, 8);
        } else {
            frame[2] &= ~1;
            memset(frame + 12, 0, 16);
        }
        return shadps4_guest_rw(cs, a0, frame, sizeof(frame), true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_SURFACE_INIT: {
        uint8_t surface[128] = { 0 };

        if (!a0) {
            return 0;
        }
        stq_le_p(surface, a1);
        stl_le_p(surface + 8, a2);
        surface[12] = a3;
        stl_le_p(surface + 16, a4);
        stl_le_p(surface + 20, a5);
        stl_le_p(surface + 32, a4);
        stl_le_p(surface + 36, a5);
        shadps4_guest_rw(cs, a0, surface, sizeof(surface), true);
        return 0;
    }
    case SHADPS4_HLE_FONT_SURFACE_SCISSOR: {
        uint32_t scissor[4] = {
            cpu_to_le32(a1), cpu_to_le32(a2),
            cpu_to_le32(a1 + a3), cpu_to_le32(a2 + a4),
        };

        if (a0) {
            shadps4_guest_rw(cs, a0 + 24, scissor, sizeof(scissor), true);
        }
        return 0;
    }
    case SHADPS4_HLE_FONT_BIND_RENDERER:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE)) {
            return UINT32_C(0x80460005);
        }
        if (!shadps4_hle_service_is(hle, a1,
                                    SHADPS4_SERVICE_FONT_RENDERER)) {
            return UINT32_C(0x80460007);
        }
        hle->service_aux_value[a0 - 0x400] = a1;
        return 0;
    case SHADPS4_HLE_FONT_REBIND_RENDERER:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE)) {
            return UINT32_C(0x80460005);
        }
        return shadps4_hle_service_is(
                   hle, hle->service_aux_value[a0 - 0x400],
                   SHADPS4_SERVICE_FONT_RENDERER) ? 0 :
               UINT32_C(0x80460007);
    case SHADPS4_HLE_FONT_UNBIND_RENDERER:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE)) {
            return UINT32_C(0x80460005);
        }
        hle->service_aux_value[a0 - 0x400] = 0;
        return 0;
    case SHADPS4_HLE_FONT_GET_LIBRARY: {
        uint64_t library;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE) || !a1) {
            return UINT32_C(0x80460005);
        }
        library = cpu_to_le64(hle->service_parents[a0 - 0x400]);
        return shadps4_guest_rw(cs, a1, &library, sizeof(library), true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_LIBRARY_OPTION:
        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_FONT_LIBRARY) ?
               0 : UINT32_C(0x80460004);
    case SHADPS4_HLE_FONT_RENDERER_GET_OUTLINE_SIZE:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_RENDERER)) {
            return UINT32_C(0x80460007);
        }
        return a1 && !shadps4_hle_write_u32(cs, a1, 0) ?
               0 : UINT32_C(0x80460002);
    case SHADPS4_HLE_FONT_RENDERER_OPTION:
        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_FONT_RENDERER) ?
               0 : UINT32_C(0x80460007);
    case SHADPS4_HLE_FONT_SET_DPI:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_FONT_HANDLE)) {
            return UINT32_C(0x80460005);
        }
        hle->service_content_length[a0 - 0x400] = a1;
        return 0;
    case SHADPS4_HLE_FONT_MEMORY_INIT: {
        uint8_t memory[64] = { 0 };

        if (!a0 || (!a3 && (!a1 || !a2))) {
            return UINT32_C(0x80460002);
        }
        stw_le_p(memory, 0x0f00);
        stl_le_p(memory + 4, a2);
        stq_le_p(memory + 8, a1);
        stq_le_p(memory + 16, a4);
        stq_le_p(memory + 24, a3);
        stq_le_p(memory + 32, a5);
        stq_le_p(memory + 40, a6);
        stq_le_p(memory + 56, a4);
        return shadps4_guest_rw(cs, a0, memory, sizeof(memory), true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_MEMORY_TERM: {
        uint8_t memory[64];

        if (!a0 || !shadps4_guest_rw(cs, a0, memory, sizeof(memory), false)) {
            return UINT32_C(0x80460002);
        }
        if (lduw_le_p(memory) != 0x0f00) {
            return UINT32_C(0x80460003);
        }
        memset(memory, 0, sizeof(memory));
        return shadps4_guest_rw(cs, a0, memory, sizeof(memory), true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_TEXT_SOURCE_INIT: {
        uint8_t source[96] = { 0 };
        uint64_t end = a2 ? a1 + a2 : 0;

        if (!a0 || !a3 || (a2 && end < a1)) {
            return UINT32_C(0x80460002);
        }
        stq_le_p(source, UINT64_C(0x1000000f04));
        stq_le_p(source + 8, a1);
        stq_le_p(source + 16, end);
        stq_le_p(source + 24, a1);
        stq_le_p(source + 32, a3);
        stq_le_p(source + 40, a4);
        stq_le_p(source + 56, a1);
        stq_le_p(source + 64, end);
        stq_le_p(source + 72, a3);
        stq_le_p(source + 80, a4);
        return shadps4_guest_rw(cs, a0, source, sizeof(source), true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_TEXT_SOURCE_REWIND:
    case SHADPS4_HLE_FONT_TEXT_SOURCE_SET_FONT:
    case SHADPS4_HLE_FONT_TEXT_SOURCE_SET_FORM: {
        uint8_t source[96];

        if (!a0 || !shadps4_guest_rw(cs, a0, source, sizeof(source), false) ||
            (ldq_le_p(source) & 0xffff) != 0x0f04) {
            return UINT32_C(0x80460002);
        }
        if (number == SHADPS4_HLE_FONT_TEXT_SOURCE_REWIND) {
            memcpy(source + 8, source + 56, 16);
            memcpy(source + 24, source + 56, 8);
            memcpy(source + 32, source + 72, 16);
            memcpy(source + 48, source + 88, 8);
        } else if (number == SHADPS4_HLE_FONT_TEXT_SOURCE_SET_FONT) {
            stq_le_p(source + 48, a1);
            stq_le_p(source + 88, a1);
        } else {
            if ((int32_t)a1 < 0x10 || a1 > 0x12) {
                return UINT32_C(0x80460002);
            }
            stq_le_p(source, (a1 << 32) | UINT64_C(0x0f04));
        }
        return shadps4_guest_rw(cs, a0, source, sizeof(source), true) ?
               0 : UINT32_C(0x80460002);
    }
    case SHADPS4_HLE_FONT_UNSUPPORTED:
        return UINT32_C(0x80460020);
    case SHADPS4_HLE_FIBER_INITIALIZE:
        return shadps4_hle_fiber_initialize(hle, cs, a0, a1, a2, a3,
                                            a4, a5, false);
    case SHADPS4_HLE_FIBER_INITIALIZE_IMPL:
        return shadps4_hle_fiber_initialize(hle, cs, a0, a1, a2, a3,
                                            a4, a5, true);
    case SHADPS4_HLE_FIBER_OPT_INITIALIZE:
        if (!a0) {
            return SHADPS4_FIBER_ERROR_NULL;
        }
        if (a0 & 7) {
            return SHADPS4_FIBER_ERROR_ALIGNMENT;
        }
        return shadps4_hle_write_u32(cs, a0, UINT32_C(0xbb40e64d)) ?
               SHADPS4_FIBER_ERROR_NULL : 0;
    case SHADPS4_HLE_FIBER_FINALIZE: {
        uint8_t fiber[112];

        if (!a0) {
            return SHADPS4_FIBER_ERROR_NULL;
        }
        if (a0 & 7) {
            return SHADPS4_FIBER_ERROR_ALIGNMENT;
        }
        if (!shadps4_hle_fiber_read(cs, a0, fiber)) {
            return SHADPS4_FIBER_ERROR_INVALID;
        }
        if (ldl_le_p(fiber + 4) != 2) {
            return SHADPS4_FIBER_ERROR_STATE;
        }
        stl_le_p(fiber + 4, 3);
        return shadps4_guest_rw(cs, a0, fiber, sizeof(fiber), true) ?
               0 : SHADPS4_FIBER_ERROR_NULL;
    }
    case SHADPS4_HLE_FIBER_RUN:
    case SHADPS4_HLE_FIBER_SWITCH:
    case SHADPS4_HLE_FIBER_RUN_IMPL:
    case SHADPS4_HLE_FIBER_SWITCH_IMPL: {
        uint8_t fiber[112];

        if (!a0) {
            return SHADPS4_FIBER_ERROR_NULL;
        }
        if (a0 & 7) {
            return SHADPS4_FIBER_ERROR_ALIGNMENT;
        }
        if (!shadps4_hle_fiber_read(cs, a0, fiber)) {
            return SHADPS4_FIBER_ERROR_INVALID;
        }
        if (ldl_le_p(fiber + 4) != 2) {
            return SHADPS4_FIBER_ERROR_STATE;
        }
        return SHADPS4_FIBER_ERROR_PERMISSION;
    }
    case SHADPS4_HLE_FIBER_GET_SELF:
        return a0 ? SHADPS4_FIBER_ERROR_PERMISSION :
               SHADPS4_FIBER_ERROR_NULL;
    case SHADPS4_HLE_FIBER_RETURN_TO_THREAD:
    case SHADPS4_HLE_FIBER_GET_FRAME_POINTER:
        return SHADPS4_FIBER_ERROR_PERMISSION;
    case SHADPS4_HLE_FIBER_GET_INFO:
        return shadps4_hle_fiber_get_info(cs, a0, a1);
    case SHADPS4_HLE_FIBER_START_SIZE_CHECK:
        if (a0) {
            return SHADPS4_FIBER_ERROR_INVALID;
        }
        if (hle->fiber_context_size_check) {
            return SHADPS4_FIBER_ERROR_STATE;
        }
        hle->fiber_context_size_check = true;
        return 0;
    case SHADPS4_HLE_FIBER_STOP_SIZE_CHECK:
        if (!hle->fiber_context_size_check) {
            return SHADPS4_FIBER_ERROR_STATE;
        }
        hle->fiber_context_size_check = false;
        return 0;
    case SHADPS4_HLE_FIBER_RENAME: {
        uint8_t fiber[112];
        char name[32] = { 0 };

        if (!a0 || !a1) {
            return SHADPS4_FIBER_ERROR_NULL;
        }
        if (a0 & 7) {
            return SHADPS4_FIBER_ERROR_ALIGNMENT;
        }
        if (!shadps4_hle_fiber_read(cs, a0, fiber)) {
            return SHADPS4_FIBER_ERROR_INVALID;
        }
        if (!shadps4_guest_read_string(cs, a1, name, sizeof(name))) {
            return SHADPS4_FIBER_ERROR_NULL;
        }
        memcpy(fiber + 40, name, sizeof(name));
        return shadps4_guest_rw(cs, a0, fiber, sizeof(fiber), true) ?
               0 : SHADPS4_FIBER_ERROR_NULL;
    }
    case SHADPS4_HLE_DIALOG_INIT:
        if (hle->dialog_status != 0) {
            return -SHADPS4_GUEST_EBUSY;
        }
        hle->dialog_status = 1;
        return 0;
    case SHADPS4_HLE_DIALOG_OPEN_MSG:
        return shadps4_hle_dialog_open(hle, cs, a0, 1);
    case SHADPS4_HLE_DIALOG_OPEN_SAVE:
        return shadps4_hle_dialog_open(hle, cs, a0, 2);
    case SHADPS4_HLE_DIALOG_OPEN_IME:
        if (hle->dialog_status == 0) {
            hle->dialog_status = 1;
        }
        return shadps4_hle_dialog_open(hle, cs, a0, 3);
    case SHADPS4_HLE_DIALOG_STATUS:
        shadps4_hle_dialog_poll(hle);
        if (hle->dialog_kind == 3) {
            return hle->dialog_status == 2 ? 1 :
                   hle->dialog_status == 3 ? 2 : 0;
        }
        return hle->dialog_status;
    case SHADPS4_HLE_DIALOG_RESULT: {
        ShadPS4GuestDialogResult result = {
            .result = cpu_to_le32(hle->dialog_result_status ? 1 : 0),
            .button_id = cpu_to_le32(hle->dialog_result_status),
        };

        shadps4_hle_dialog_poll(hle);
        if (hle->dialog_status != 3 || !a0) {
            return -SHADPS4_GUEST_EINVAL;
        }
        if (hle->dialog_kind == 3) {
            uint32_t ime_result = cpu_to_le32(
                hle->dialog_result_status ? 1 : 0);

            return shadps4_guest_rw(cs, a0, &ime_result,
                                    sizeof(ime_result), true) ?
                   0 : -SHADPS4_GUEST_EFAULT;
        }
        if (hle->dialog_kind == 2) {
            uint8_t save_result[72] = { 0 };
            uint32_t status = cpu_to_le32(
                hle->dialog_result_status ? 1 : 0);
            uint32_t button = cpu_to_le32(hle->dialog_result_status);

            memcpy(save_result + 4, &status, sizeof(status));
            memcpy(save_result + 8, &button, sizeof(button));
            return shadps4_guest_rw(cs, a0, save_result,
                                    sizeof(save_result), true) ?
                   0 : -SHADPS4_GUEST_EFAULT;
        }
        return shadps4_guest_rw(cs, a0, &result, sizeof(result), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_DIALOG_CLOSE:
        if (hle->dialog_status != 2) {
            return -SHADPS4_GUEST_EINVAL;
        }
        hle->dialog_status = 3;
        hle->dialog_result_status = a0 ? 1 : 0;
        return 0;
    case SHADPS4_HLE_DIALOG_TERM:
        hle->dialog_status = 0;
        hle->dialog_kind = 0;
        hle->dialog_request_id = 0;
        return 0;
    case SHADPS4_HLE_USER_SERVICE_INITIALIZE:
        if (!hle->user_service_initialized) {
            hle->user_event_head = 0;
            hle->user_event_count = 1;
            hle->user_event_types[0] = 0;
            hle->user_event_ids[0] = 1;
        }
        hle->user_service_initialized = true;
        return 0;
    case SHADPS4_HLE_USER_SERVICE_TERMINATE:
        hle->user_service_initialized = false;
        return 0;
    case SHADPS4_HLE_USER_SERVICE_GET_INITIAL_USER: {
        uint32_t user_id = cpu_to_le32(1);

        if (!a0) {
            return SHADPS4_USER_SERVICE_ERROR_INVALID_ARGUMENT;
        }
        return shadps4_guest_rw(cs, a0, &user_id, sizeof(user_id), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_USER_SERVICE_GET_LOGIN_USER_ID_LIST: {
        uint32_t user_ids[4] = {
            cpu_to_le32(1), cpu_to_le32(UINT32_MAX),
            cpu_to_le32(UINT32_MAX), cpu_to_le32(UINT32_MAX),
        };

        if (!a0) {
            return SHADPS4_USER_SERVICE_ERROR_INVALID_ARGUMENT;
        }
        return shadps4_guest_rw(cs, a0, user_ids, sizeof(user_ids), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_USER_SERVICE_GET_EVENT: {
        uint32_t event[2];
        uint32_t slot;

        if (!a0) {
            return SHADPS4_USER_SERVICE_ERROR_INVALID_ARGUMENT;
        }
        if (!hle->user_event_count) {
            return SHADPS4_USER_SERVICE_ERROR_NO_EVENT;
        }
        slot = hle->user_event_head;
        event[0] = cpu_to_le32(hle->user_event_types[slot]);
        event[1] = cpu_to_le32(hle->user_event_ids[slot]);
        if (!shadps4_guest_rw(cs, a0, event, sizeof(event), true)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        hle->user_event_head = (slot + 1) % SHADPS4_HLE_MAX_SERVICE_EVENTS;
        hle->user_event_count--;
        return 0;
    }
    case SHADPS4_HLE_USER_SERVICE_GET_REGISTERED_USER_ID_LIST: {
        uint32_t user_ids[16];
        uint32_t i;

        if (!a0) {
            return SHADPS4_USER_SERVICE_ERROR_INVALID_ARGUMENT;
        }
        for (i = 0; i < ARRAY_SIZE(user_ids); i++) {
            user_ids[i] = cpu_to_le32(i ? UINT32_MAX : 1);
        }
        return shadps4_guest_rw(cs, a0, user_ids, sizeof(user_ids), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_USER_SERVICE_GET_USER_COLOR: {
        uint32_t color = 0;

        if (a0 != 1 || !a1) {
            return SHADPS4_USER_SERVICE_ERROR_INVALID_ARGUMENT;
        }
        return shadps4_guest_rw(cs, a1, &color, sizeof(color), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_USER_SERVICE_GET_USER_NAME: {
        static const char name[] = "Player 1";

        if (a0 != 1 || !a1) {
            return SHADPS4_USER_SERVICE_ERROR_INVALID_ARGUMENT;
        }
        if (a2 < sizeof(name)) {
            return SHADPS4_USER_SERVICE_ERROR_BUFFER_TOO_SHORT;
        }
        return shadps4_guest_rw(cs, a1, (void *)name, sizeof(name), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_USER_SERVICE_STUB:
        return 0;
    case SHADPS4_HLE_SYSTEM_SERVICE_GET_STATUS: {
        ShadPS4GuestSystemServiceStatus status = {
            .event_num = cpu_to_le32(hle->system_event_count),
            .is_cpu_mode7_cpu_normal = 1,
        };

        if (!a0) {
            return SHADPS4_SYSTEM_SERVICE_ERROR_PARAMETER;
        }
        return shadps4_guest_rw(cs, a0, &status, sizeof(status), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_SYSTEM_SERVICE_PARAM_GET_INT: {
        uint32_t value;

        if (!a1) {
            return SHADPS4_SYSTEM_SERVICE_ERROR_PARAMETER;
        }
        switch (a0) {
        case 1: /* Language: English (United States). */
            value = 1;
            break;
        case 2: /* DD/MM/YYYY. */
        case 3: /* 24-hour clock. */
        case 5: /* Daylight saving time enabled. */
        case 1000: /* Cross is the enter button. */
            value = 1;
            break;
        case 4: /* UTC+02:00, matching shadPS4's current default. */
            value = 120;
            break;
        case 7: /* Parental controls disabled. */
        default:
            value = 0;
            break;
        }
        value = cpu_to_le32(value);
        return shadps4_guest_rw(cs, a1, &value, sizeof(value), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_SYSTEM_SERVICE_PARAM_GET_STRING: {
        static const char system_name[] = "PlayStation 4";

        if (a0 != 6 || !a1 || !a2) {
            return SHADPS4_SYSTEM_SERVICE_ERROR_PARAMETER;
        }
        if (a2 < sizeof(system_name)) {
            return SHADPS4_SYSTEM_SERVICE_ERROR_PARAMETER;
        }
        return shadps4_guest_rw(cs, a1, (void *)system_name,
                                sizeof(system_name), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_SYSTEM_SERVICE_GET_SAFE_AREA: {
        uint32_t ratio = cpu_to_le32(0x3f800000);
        uint8_t info[132] = { 0 };

        if (!a0) {
            return SHADPS4_SYSTEM_SERVICE_ERROR_PARAMETER;
        }
        memcpy(info, &ratio, sizeof(ratio));
        return shadps4_guest_rw(cs, a0, info, sizeof(info), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_SYSTEM_SERVICE_RECEIVE_EVENT:
        if (!a0) {
            return SHADPS4_SYSTEM_SERVICE_ERROR_PARAMETER;
        }
        if (!hle->system_event_count) {
            return SHADPS4_SYSTEM_SERVICE_ERROR_NO_EVENT;
        } else {
            g_autofree uint8_t *event = g_malloc0(8196);
            uint32_t slot = hle->system_event_head;
            uint32_t type = cpu_to_le32(hle->system_event_types[slot]);

            memcpy(event, &type, sizeof(type));
            if (!shadps4_guest_rw(cs, a0, event, 8196, true)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            hle->system_event_head =
                (slot + 1) % SHADPS4_HLE_MAX_SERVICE_EVENTS;
            hle->system_event_count--;
            return 0;
        }
    case SHADPS4_HLE_SYSTEM_SERVICE_LOAD_EXEC: {
        g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
        char path[2048];
        uint32_t i;
        int ret;

        if (!a0 || !shadps4_guest_read_string(cs, a0, path, sizeof(path))) {
            return SHADPS4_SYSTEM_SERVICE_ERROR_PARAMETER;
        }
        if (a1) {
            for (i = 0; i < 64; i++) {
                uint64_t argument_address;
                char argument[2048];

                if (!shadps4_guest_rw(cs, a1 + i * sizeof(uint64_t),
                                      &argument_address,
                                      sizeof(argument_address), false)) {
                    return -SHADPS4_GUEST_EFAULT;
                }
                argument_address = le64_to_cpu(argument_address);
                if (!argument_address) {
                    break;
                }
                if (!shadps4_guest_read_string(cs, argument_address, argument,
                                                sizeof(argument))) {
                    return -SHADPS4_GUEST_EFAULT;
                }
                g_ptr_array_add(argv, g_strdup(argument));
            }
            if (i == 64) {
                return SHADPS4_SYSTEM_SERVICE_ERROR_PARAMETER;
            }
        }
        ret = qemu_host_launch(path, (const char *const *)argv->pdata,
                               argv->len);
        return shadps4_hle_host_result(ret);
    }
    case SHADPS4_HLE_SYSTEM_SERVICE_HIDE_SPLASH_SCREEN:
        hle->splash_visible = false;
        return 0;
    case SHADPS4_HLE_SYSTEM_SERVICE_STUB:
        return 0;
    case SHADPS4_HLE_SYSMODULE_LOAD:
    case SHADPS4_HLE_SYSMODULE_LOAD_INTERNAL:
    case SHADPS4_HLE_SYSMODULE_UNLOAD:
    case SHADPS4_HLE_SYSMODULE_IS_LOADED:
    case SHADPS4_HLE_SYSMODULE_IS_LOADED_INTERNAL:
    case SHADPS4_HLE_SYSMODULE_LOAD_INTERNAL_WITH_ARG: {
        uint32_t id = a0 & 0x7fffffffU;
        uint16_t *load_count;
        bool incremented = false;

        if (!shadps4_hle_sysmodule_id_valid(hle, id)) {
            return SHADPS4_SYSMODULE_INVALID_ID;
        }
        if (number == SHADPS4_HLE_SYSMODULE_LOAD_INTERNAL_WITH_ARG && a3) {
            return SHADPS4_SYSMODULE_INVALID_ID;
        }
        load_count = &hle->sysmodule_load_count[id - 1];
        if (number == SHADPS4_HLE_SYSMODULE_IS_LOADED ||
            number == SHADPS4_HLE_SYSMODULE_IS_LOADED_INTERNAL) {
            return *load_count ? 0 : SHADPS4_SYSMODULE_NOT_LOADED;
        }
        if (number == SHADPS4_HLE_SYSMODULE_UNLOAD) {
            if (!*load_count) {
                return SHADPS4_SYSMODULE_NOT_LOADED;
            }
            (*load_count)--;
            return 0;
        }
        if (*load_count != UINT16_MAX) {
            (*load_count)++;
            incremented = true;
        }
        if (number == SHADPS4_HLE_SYSMODULE_LOAD_INTERNAL_WITH_ARG && a4) {
            uint32_t result = 0;

            if (!shadps4_guest_rw(cs, a4, &result, sizeof(result), true)) {
                if (incremented) {
                    (*load_count)--;
                }
                return -SHADPS4_GUEST_EFAULT;
            }
        }
        return 0;
    }
    case SHADPS4_HLE_SYSMODULE_GET_HANDLE_INTERNAL: {
        uint32_t id = a0 & 0x7fffffffU;
        uint32_t handle;

        if (!a1 || !shadps4_hle_sysmodule_id_valid(hle, id)) {
            return SHADPS4_SYSMODULE_INVALID_ID;
        }
        if (!hle->sysmodule_load_count[id - 1]) {
            return SHADPS4_SYSMODULE_NOT_LOADED;
        }
        handle = cpu_to_le32(0x1000 + id);
        return shadps4_guest_rw(cs, a1, &handle, sizeof(handle), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_SYSMODULE_GET_INFO_UNWIND:
        return shadps4_hle_module_info_unwind(hle, cs, a0, a1, a2, true);
    case SHADPS4_HLE_SYSMODULE_PRELOAD: {
        /* Retail g_preload_list_3 entries translated from table indexes. */
        static const uint16_t modules[] = {
            0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x52,
            0x01, 0x02, 0x23, 0x24, 0x1a, 0x09, 0x0a, 0x0b,
            0x8c, 0x0c, 0x0d, 0x8d, 0x0e, 0x8f, 0x0f, 0x10,
            0x11, 0x18, 0x26,
        };
        uint32_t i;

        for (i = 0; i < ARRAY_SIZE(modules); i++) {
            uint32_t id = modules[i];

            if (shadps4_hle_sysmodule_id_valid(hle, id) &&
                hle->sysmodule_load_count[id - 1] != UINT16_MAX) {
                hle->sysmodule_load_count[id - 1]++;
            }
        }
        return 0;
    }
    case SHADPS4_HLE_SYSMODULE_STUB:
        return 0;
    case SHADPS4_HLE_KERNEL_ERROR:
        return a0 ? (uint32_t)(a0 + SHADPS4_KERNEL_ERROR_UNKNOWN) : 0;
    case SHADPS4_HLE_KERNEL_ERRNO_LOCATION:
        return env->segs[R_GS].base + 0x20;
    case SHADPS4_HLE_KERNEL_STACK_CHK_FAIL:
        error_report("shadPS4 guest stack protector check failed");
        shadps4_hle_cleanup(hle);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
        return 0;
    case SHADPS4_HLE_KERNEL_GETPAGESIZE:
        return 16 * KiB;
    case SHADPS4_HLE_KERNEL_GETARGC:
        return 1;
    case SHADPS4_HLE_KERNEL_GETARGV:
        return hle->argv_guest_addr;
    case SHADPS4_HLE_KERNEL_SIGPROCMASK:
    case SHADPS4_HLE_KERNEL_SCHED_YIELD:
        return 0;
    case SHADPS4_HLE_KERNEL_GETPID:
        return 1;
    case SHADPS4_HLE_KERNEL_EQUEUE_CREATE: {
        char name[34];
        uint64_t queue;

        if (!a0 || !a1) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        if (!shadps4_guest_read_string(cs, a1, name, sizeof(name))) {
            return SHADPS4_KERNEL_ERROR_ENAMETOOLONG;
        }
        queue = shadps4_hle_kqueue(hle);
        if ((int64_t)queue < 0) {
            return SHADPS4_KERNEL_ERROR_ENOMEM;
        }
        queue = cpu_to_le64(queue);
        if (!shadps4_guest_rw(cs, a0, &queue, sizeof(queue), true)) {
            hle->equeues[le64_to_cpu(queue) - 0x100] = false;
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
        return 0;
    }
    case SHADPS4_HLE_KERNEL_EQUEUE_DELETE:
        if (a0 < 0x100 || a0 >= 0x100 + SHADPS4_HLE_MAX_EQUEUES ||
            !hle->equeues[a0 - 0x100]) {
            return SHADPS4_KERNEL_ERROR_EBADF;
        }
        hle->equeues[a0 - 0x100] = false;
        memset(hle->equeue_events[a0 - 0x100], 0,
               sizeof(hle->equeue_events[a0 - 0x100]));
        return 0;
    case SHADPS4_HLE_KERNEL_EQUEUE_WAIT:
        return shadps4_hle_equeue_wait(hle, cs, a0, a1, a2, a3);
    case SHADPS4_HLE_KERNEL_EQUEUE_ADD_USER:
    case SHADPS4_HLE_KERNEL_EQUEUE_ADD_USER_EDGE:
        return shadps4_hle_equeue_add(
            hle, a0, a1, -11, 0, 0, false,
            number == SHADPS4_HLE_KERNEL_EQUEUE_ADD_USER_EDGE);
    case SHADPS4_HLE_KERNEL_EQUEUE_TRIGGER_USER: {
        ShadPS4HLEEqueueEvent *event =
            shadps4_hle_equeue_find(hle, a0, a1, -11, false);

        if (a0 < 0x100 || a0 >= 0x100 + SHADPS4_HLE_MAX_EQUEUES ||
            !hle->equeues[a0 - 0x100]) {
            return SHADPS4_KERNEL_ERROR_EBADF;
        }
        if (!event) {
            return SHADPS4_KERNEL_ERROR_ENOENT;
        }
        event->user_data = a2;
        event->triggered = true;
        return 0;
    }
    case SHADPS4_HLE_KERNEL_EQUEUE_DELETE_USER:
        return shadps4_hle_equeue_delete_event(hle, a0, a1, -11);
    case SHADPS4_HLE_KERNEL_EQUEUE_ADD_TIMER:
        if (a2 > INT64_MAX / 1000) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        return shadps4_hle_equeue_add(hle, a0, a1, -7, a3,
                                      MAX((int64_t)a2 * 1000, 1),
                                      false, false);
    case SHADPS4_HLE_KERNEL_EQUEUE_DELETE_TIMER:
        return shadps4_hle_equeue_delete_event(hle, a0, a1, -7);
    case SHADPS4_HLE_KERNEL_EQUEUE_ADD_HRTIMER: {
        ShadPS4GuestTime time;
        uint64_t seconds;
        uint64_t nanoseconds;

        if (!shadps4_guest_rw(cs, a2, &time, sizeof(time), false)) {
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
        seconds = le64_to_cpu(time.seconds);
        nanoseconds = le64_to_cpu(time.fraction);
        if (nanoseconds >= NANOSECONDS_PER_SECOND ||
            seconds > INT64_MAX / NANOSECONDS_PER_SECOND ||
            nanoseconds > INT64_MAX - seconds * NANOSECONDS_PER_SECOND) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        return shadps4_hle_equeue_add(
            hle, a0, a1, -15, a3,
            MAX((int64_t)(seconds * NANOSECONDS_PER_SECOND + nanoseconds), 1),
            true, false);
    }
    case SHADPS4_HLE_KERNEL_EQUEUE_DELETE_HRTIMER:
        return shadps4_hle_equeue_delete_event(hle, a0, a1, -15);
    case SHADPS4_HLE_KERNEL_EVENT_GET_ID:
    case SHADPS4_HLE_KERNEL_EVENT_GET_FILTER:
    case SHADPS4_HLE_KERNEL_EVENT_GET_DATA:
    case SHADPS4_HLE_KERNEL_EVENT_GET_USER_DATA: {
        ShadPS4GuestEvent event;

        if (!shadps4_guest_rw(cs, a0, &event, sizeof(event), false)) {
            return 0;
        }
        if (number == SHADPS4_HLE_KERNEL_EVENT_GET_ID) {
            return le64_to_cpu(event.ident);
        }
        if (number == SHADPS4_HLE_KERNEL_EVENT_GET_FILTER) {
            return (uint32_t)(int32_t)(int16_t)le16_to_cpu(event.filter);
        }
        if (number == SHADPS4_HLE_KERNEL_EVENT_GET_DATA) {
            return le64_to_cpu(event.data);
        }
        return le64_to_cpu(event.user_data);
    }
    case SHADPS4_HLE_PTHREAD_ATTR_INIT: {
        uint64_t result = shadps4_hle_object_create(
            hle, cs, a0, SHADPS4_SERVICE_PTHREAD_ATTR, 0);
        uint64_t handle;

        if (result || !shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_PTHREAD_ATTR, &handle)) {
            return result ?: SHADPS4_GUEST_EFAULT;
        }
        hle->pthread_attrs[handle - 0x400] = (ShadPS4HLEPthreadAttr) {
            .stack_size = 512 * KiB,
            .guard_size = 16 * KiB,
            .affinity = 0xff,
        };
        return 0;
    }
    case SHADPS4_HLE_PTHREAD_ATTR_DESTROY:
        return shadps4_hle_object_destroy(
            hle, cs, a0, SHADPS4_SERVICE_PTHREAD_ATTR);
    case SHADPS4_HLE_PTHREAD_ATTR_GET_NP: {
        uint64_t result = shadps4_hle_object_create(
            hle, cs, a1, SHADPS4_SERVICE_PTHREAD_ATTR, 0);
        uint64_t handle;

        if (!result && shadps4_hle_object_handle(
                hle, cs, a1, SHADPS4_SERVICE_PTHREAD_ATTR, &handle)) {
            hle->pthread_attrs[handle - 0x400] = (ShadPS4HLEPthreadAttr) {
                .stack_size = 512 * KiB,
                .guard_size = 16 * KiB,
                .affinity = 0xff,
            };
        }
        return result;
    }
    case SHADPS4_HLE_PTHREAD_ATTR_GET_AFFINITY:
    case SHADPS4_HLE_PTHREAD_ATTR_GET_DETACH:
    case SHADPS4_HLE_PTHREAD_ATTR_GET_INHERIT:
    case SHADPS4_HLE_PTHREAD_ATTR_GET_POLICY:
    case SHADPS4_HLE_PTHREAD_ATTR_GET_SCOPE:
    case SHADPS4_HLE_PTHREAD_ATTR_GET_SCHEDPARAM:
    case SHADPS4_HLE_PTHREAD_ATTR_GET_GUARD:
    case SHADPS4_HLE_PTHREAD_ATTR_GET_STACKADDR:
    case SHADPS4_HLE_PTHREAD_ATTR_GET_STACKSIZE: {
        ShadPS4HLEPthreadAttr *attr;
        uint64_t handle;
        uint64_t output = a1;
        uint64_t value;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_PTHREAD_ATTR, &handle)) {
            return SHADPS4_GUEST_EINVAL;
        }
        attr = &hle->pthread_attrs[handle - 0x400];
        if (number == SHADPS4_HLE_PTHREAD_ATTR_GET_AFFINITY) {
            output = a2 ?: a1;
            value = attr->affinity;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_GET_DETACH) {
            value = attr->detach_state;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_GET_INHERIT) {
            value = attr->inherit_sched;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_GET_POLICY) {
            value = attr->policy;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_GET_SCOPE) {
            value = attr->scope;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_GET_SCHEDPARAM) {
            value = attr->priority;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_GET_GUARD) {
            value = attr->guard_size;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_GET_STACKADDR) {
            value = attr->stack_addr;
        } else {
            value = attr->stack_size;
        }
        return value > UINT32_MAX ||
               number == SHADPS4_HLE_PTHREAD_ATTR_GET_GUARD ||
               number == SHADPS4_HLE_PTHREAD_ATTR_GET_STACKADDR ||
               number == SHADPS4_HLE_PTHREAD_ATTR_GET_STACKSIZE ?
               shadps4_hle_write_u64(cs, output, value) :
               shadps4_hle_write_u32(cs, output, value);
    }
    case SHADPS4_HLE_PTHREAD_ATTR_GET_STACK: {
        ShadPS4HLEPthreadAttr *attr;
        uint64_t handle;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_PTHREAD_ATTR, &handle)) {
            return SHADPS4_GUEST_EINVAL;
        }
        attr = &hle->pthread_attrs[handle - 0x400];
        return shadps4_hle_write_u64(cs, a1, attr->stack_addr) ?:
               shadps4_hle_write_u64(cs, a2, attr->stack_size);
    }
    case SHADPS4_HLE_PTHREAD_ATTR_SET_AFFINITY:
    case SHADPS4_HLE_PTHREAD_ATTR_SET_DETACH:
    case SHADPS4_HLE_PTHREAD_ATTR_SET_INHERIT:
    case SHADPS4_HLE_PTHREAD_ATTR_SET_POLICY:
    case SHADPS4_HLE_PTHREAD_ATTR_SET_SCOPE:
    case SHADPS4_HLE_PTHREAD_ATTR_SET_SCHEDPARAM:
    case SHADPS4_HLE_PTHREAD_ATTR_SET_GUARD:
    case SHADPS4_HLE_PTHREAD_ATTR_SET_STACKADDR:
    case SHADPS4_HLE_PTHREAD_ATTR_SET_STACKSIZE:
    case SHADPS4_HLE_PTHREAD_ATTR_SET_SUSPEND: {
        ShadPS4HLEPthreadAttr *attr;
        uint64_t handle;
        uint64_t value = a1;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_PTHREAD_ATTR, &handle)) {
            return SHADPS4_GUEST_EINVAL;
        }
        attr = &hle->pthread_attrs[handle - 0x400];
        if (number == SHADPS4_HLE_PTHREAD_ATTR_SET_AFFINITY) {
            uint64_t affinity = 0;
            uint64_t source = a2 ?: a1;
            size_t size = a2 ? MIN(a1, sizeof(affinity)) : sizeof(uint64_t);

            if (!source || !shadps4_guest_rw(cs, source, &affinity,
                                              size, false) || !affinity) {
                return SHADPS4_GUEST_EINVAL;
            }
            attr->affinity = le64_to_cpu(affinity) & 0xff;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_SET_DETACH) {
            if (value > 1) return SHADPS4_GUEST_EINVAL;
            attr->detach_state = value;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_SET_INHERIT) {
            if (value > 1) return SHADPS4_GUEST_EINVAL;
            attr->inherit_sched = value;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_SET_POLICY) {
            attr->policy = value;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_SET_SCOPE) {
            attr->scope = value;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_SET_SCHEDPARAM) {
            uint32_t priority;

            if (!a1 || !shadps4_guest_rw(cs, a1, &priority,
                                          sizeof(priority), false)) {
                return SHADPS4_GUEST_EFAULT;
            }
            attr->priority = le32_to_cpu(priority);
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_SET_GUARD) {
            attr->guard_size = value;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_SET_STACKADDR) {
            attr->stack_addr = value;
        } else if (number == SHADPS4_HLE_PTHREAD_ATTR_SET_STACKSIZE) {
            if (value < 16 * KiB) return SHADPS4_GUEST_EINVAL;
            attr->stack_size = value;
        } else {
            attr->create_suspended = value != 0;
        }
        return 0;
    }
    case SHADPS4_HLE_PTHREAD_ATTR_SET_STACK: {
        uint64_t handle;

        if (!a1 || a2 < 16 * KiB || !shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_PTHREAD_ATTR, &handle)) {
            return SHADPS4_GUEST_EINVAL;
        }
        hle->pthread_attrs[handle - 0x400].stack_addr = a1;
        hle->pthread_attrs[handle - 0x400].stack_size = a2;
        return 0;
    }
    case SHADPS4_HLE_MUTEX_ATTR_INIT:
        return shadps4_hle_object_create(
            hle, cs, a0, SHADPS4_SERVICE_MUTEX_ATTR, 0);
    case SHADPS4_HLE_MUTEX_ATTR_DESTROY:
        return shadps4_hle_object_destroy(
            hle, cs, a0, SHADPS4_SERVICE_MUTEX_ATTR);
    case SHADPS4_HLE_MUTEX_ATTR_GET:
    case SHADPS4_HLE_COND_ATTR_GET:
    case SHADPS4_HLE_RWLOCK_ATTR_GET: {
        uint8_t type = number == SHADPS4_HLE_MUTEX_ATTR_GET ?
                       SHADPS4_SERVICE_MUTEX_ATTR :
                       number == SHADPS4_HLE_COND_ATTR_GET ?
                       SHADPS4_SERVICE_COND_ATTR :
                       SHADPS4_SERVICE_RWLOCK_ATTR;
        uint64_t handle;

        return shadps4_hle_object_handle(hle, cs, a0, type, &handle) ?
               shadps4_hle_write_u32(
                   cs, a1, hle->service_value[handle - 0x400]) :
               SHADPS4_GUEST_EINVAL;
    }
    case SHADPS4_HLE_MUTEX_ATTR_SET:
    case SHADPS4_HLE_COND_ATTR_SET:
    case SHADPS4_HLE_RWLOCK_ATTR_SET: {
        uint8_t type = number == SHADPS4_HLE_MUTEX_ATTR_SET ?
                       SHADPS4_SERVICE_MUTEX_ATTR :
                       number == SHADPS4_HLE_COND_ATTR_SET ?
                       SHADPS4_SERVICE_COND_ATTR :
                       SHADPS4_SERVICE_RWLOCK_ATTR;
        uint64_t handle;

        if (!shadps4_hle_object_handle(hle, cs, a0, type, &handle)) {
            return SHADPS4_GUEST_EINVAL;
        }
        hle->service_value[handle - 0x400] = a1;
        return 0;
    }
    case SHADPS4_HLE_MUTEX_INIT:
        return shadps4_hle_object_create(
            hle, cs, a0, SHADPS4_SERVICE_MUTEX, 0);
    case SHADPS4_HLE_MUTEX_DESTROY:
        return shadps4_hle_object_destroy(
            hle, cs, a0, SHADPS4_SERVICE_MUTEX);
    case SHADPS4_HLE_MUTEX_LOCK:
    case SHADPS4_HLE_MUTEX_TIMEDLOCK:
    case SHADPS4_HLE_MUTEX_TRYLOCK: {
        uint64_t handle = 0;
        uint64_t result;
        bool object_handle_result;
        ShadPS4GuestTime timeout;

        object_handle_result = shadps4_hle_object_handle(
            hle, cs, a0, SHADPS4_SERVICE_MUTEX, &handle);
        if (!object_handle_result && a0 >= SHADPS4_GUEST_USER_MIN) {
            uint64_t initial_value;

            if (shadps4_guest_rw(cs, a0, &initial_value,
                                 sizeof(initial_value), false) &&
                le64_to_cpu(initial_value) == 0 &&
                shadps4_hle_object_create(
                    hle, cs, a0, SHADPS4_SERVICE_MUTEX, 0) == 0) {
                object_handle_result = shadps4_hle_object_handle(
                    hle, cs, a0, SHADPS4_SERVICE_MUTEX, &handle);
            }
        }
        if (!object_handle_result) {
            result = SHADPS4_GUEST_EINVAL;
        } else if (number == SHADPS4_HLE_MUTEX_TIMEDLOCK &&
                   (!a1 || !shadps4_guest_rw(cs, a1, &timeout,
                                              sizeof(timeout), false) ||
                    (int64_t)le64_to_cpu(timeout.seconds) < 0 ||
                    le64_to_cpu(timeout.fraction) >= NANOSECONDS_PER_SECOND)) {
            result = SHADPS4_GUEST_EINVAL;
        } else if (hle->service_active[handle - 0x400] &&
                   hle->service_user_data[handle - 0x400] != 1) {
            result = number == SHADPS4_HLE_MUTEX_TRYLOCK ?
                     SHADPS4_GUEST_EBUSY : SHADPS4_GUEST_EAGAIN;
        } else {
            hle->service_active[handle - 0x400] = true;
            hle->service_user_data[handle - 0x400] = 1;
            hle->service_value[handle - 0x400]++;
            result = 0;
        }
        if (result) {
            warn_report("shadPS4 mutex %s: address=%#" PRIx64
                        " object_handle=%s handle=%#" PRIx64
                        " guest_return=%#" PRIx64 " (%" PRIu64 ")",
                        number == SHADPS4_HLE_MUTEX_TRYLOCK ? "trylock" :
                        number == SHADPS4_HLE_MUTEX_TIMEDLOCK ? "timedlock" :
                        "lock", a0, object_handle_result ? "resolved" :
                        "invalid", handle, result, result);
        }
        return result;
    }
    case SHADPS4_HLE_MUTEX_UNLOCK: {
        uint64_t handle;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_MUTEX, &handle) ||
            !hle->service_active[handle - 0x400] ||
            !hle->service_value[handle - 0x400]) {
            return SHADPS4_GUEST_EINVAL;
        }
        if (!--hle->service_value[handle - 0x400]) {
            hle->service_active[handle - 0x400] = false;
            hle->service_user_data[handle - 0x400] = 0;
        }
        return 0;
    }
    case SHADPS4_HLE_MUTEX_ISOWNED: {
        uint64_t handle;

        return shadps4_hle_object_handle(
                   hle, cs, a0, SHADPS4_SERVICE_MUTEX, &handle) &&
               hle->service_active[handle - 0x400];
    }
    case SHADPS4_HLE_MUTEX_GET_LOOPS: {
        uint64_t handle;

        return shadps4_hle_object_handle(
                   hle, cs, a0, SHADPS4_SERVICE_MUTEX, &handle) ?
               hle->service_parents[handle - 0x400] : 0;
    }
    case SHADPS4_HLE_MUTEX_SET_LOOPS: {
        uint64_t handle;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_MUTEX, &handle)) {
            return SHADPS4_GUEST_EINVAL;
        }
        hle->service_parents[handle - 0x400] = a1;
        return 0;
    }
    case SHADPS4_HLE_COND_ATTR_INIT:
        return shadps4_hle_object_create(
            hle, cs, a0, SHADPS4_SERVICE_COND_ATTR, 0);
    case SHADPS4_HLE_COND_ATTR_DESTROY:
        return shadps4_hle_object_destroy(
            hle, cs, a0, SHADPS4_SERVICE_COND_ATTR);
    case SHADPS4_HLE_COND_INIT:
        return shadps4_hle_object_create(
            hle, cs, a0, SHADPS4_SERVICE_COND, 0);
    case SHADPS4_HLE_COND_DESTROY:
        return shadps4_hle_object_destroy(
            hle, cs, a0, SHADPS4_SERVICE_COND);
    case SHADPS4_HLE_COND_WAIT: {
        uint64_t cond;
        uint64_t mutex;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_COND, &cond) ||
            !shadps4_hle_object_handle(
                hle, cs, a1, SHADPS4_SERVICE_MUTEX, &mutex)) {
            return SHADPS4_GUEST_EINVAL;
        }
        return SHADPS4_KERNEL_ERROR_ETIMEDOUT;
    }
    case SHADPS4_HLE_COND_SIGNAL: {
        uint64_t handle;

        return shadps4_hle_object_handle(
                   hle, cs, a0, SHADPS4_SERVICE_COND, &handle) ?
               0 : SHADPS4_GUEST_EINVAL;
    }
    case SHADPS4_HLE_RWLOCK_ATTR_INIT:
        return shadps4_hle_object_create(
            hle, cs, a0, SHADPS4_SERVICE_RWLOCK_ATTR, 0);
    case SHADPS4_HLE_RWLOCK_ATTR_DESTROY:
        return shadps4_hle_object_destroy(
            hle, cs, a0, SHADPS4_SERVICE_RWLOCK_ATTR);
    case SHADPS4_HLE_RWLOCK_INIT:
        return shadps4_hle_object_create(
            hle, cs, a0, SHADPS4_SERVICE_RWLOCK, 0);
    case SHADPS4_HLE_RWLOCK_DESTROY:
        return shadps4_hle_object_destroy(
            hle, cs, a0, SHADPS4_SERVICE_RWLOCK);
    case SHADPS4_HLE_RWLOCK_RDLOCK:
    case SHADPS4_HLE_RWLOCK_TRYRDLOCK: {
        uint64_t handle;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_RWLOCK, &handle)) {
            return SHADPS4_GUEST_EINVAL;
        }
        if (hle->service_active[handle - 0x400]) {
            return number == SHADPS4_HLE_RWLOCK_TRYRDLOCK ?
                   SHADPS4_GUEST_EBUSY : SHADPS4_GUEST_EAGAIN;
        }
        hle->service_value[handle - 0x400]++;
        return 0;
    }
    case SHADPS4_HLE_RWLOCK_WRLOCK:
    case SHADPS4_HLE_RWLOCK_TRYWRLOCK: {
        uint64_t handle;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_RWLOCK, &handle)) {
            return SHADPS4_GUEST_EINVAL;
        }
        if (hle->service_active[handle - 0x400] ||
            hle->service_value[handle - 0x400]) {
            return number == SHADPS4_HLE_RWLOCK_TRYWRLOCK ?
                   SHADPS4_GUEST_EBUSY : SHADPS4_GUEST_EAGAIN;
        }
        hle->service_active[handle - 0x400] = true;
        return 0;
    }
    case SHADPS4_HLE_RWLOCK_UNLOCK: {
        uint64_t handle;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_RWLOCK, &handle)) {
            return SHADPS4_GUEST_EINVAL;
        }
        if (hle->service_active[handle - 0x400]) {
            hle->service_active[handle - 0x400] = false;
        } else if (hle->service_value[handle - 0x400]) {
            hle->service_value[handle - 0x400]--;
        } else {
            return SHADPS4_GUEST_EINVAL;
        }
        return 0;
    }
    case SHADPS4_HLE_SEM_INIT:
        if (a2 > INT32_MAX) return SHADPS4_GUEST_EINVAL;
        if (shadps4_hle_object_create(
                hle, cs, a0, SHADPS4_SERVICE_SEM, 0)) {
            return SHADPS4_GUEST_ENOMEM;
        } else {
            uint64_t handle;
            shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_SEM, &handle);
            hle->service_value[handle - 0x400] = a2;
            return 0;
        }
    case SHADPS4_HLE_SEM_DESTROY:
        return shadps4_hle_object_destroy(
            hle, cs, a0, SHADPS4_SERVICE_SEM);
    case SHADPS4_HLE_SEM_WAIT:
    case SHADPS4_HLE_SEM_TRYWAIT: {
        uint64_t handle;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_SEM, &handle)) {
            return SHADPS4_GUEST_EINVAL;
        }
        if (!hle->service_value[handle - 0x400]) {
            return number == SHADPS4_HLE_SEM_TRYWAIT ?
                   SHADPS4_GUEST_EAGAIN : SHADPS4_KERNEL_ERROR_ETIMEDOUT;
        }
        hle->service_value[handle - 0x400]--;
        return 0;
    }
    case SHADPS4_HLE_SEM_POST: {
        uint64_t handle;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_SEM, &handle) ||
            hle->service_value[handle - 0x400] == INT32_MAX) {
            return SHADPS4_GUEST_EINVAL;
        }
        hle->service_value[handle - 0x400]++;
        return 0;
    }
    case SHADPS4_HLE_SEM_GETVALUE: {
        uint64_t handle;

        return shadps4_hle_object_handle(
                   hle, cs, a0, SHADPS4_SERVICE_SEM, &handle) ?
               shadps4_hle_write_u32(
                   cs, a1, hle->service_value[handle - 0x400]) :
               SHADPS4_GUEST_EINVAL;
    }
    case SHADPS4_HLE_KERNEL_SEMA_CREATE: {
        uint64_t result;
        uint64_t handle;

        if (!a0 || a3 > a4 || a4 > INT32_MAX) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        result = shadps4_hle_object_create(
            hle, cs, a0, SHADPS4_SERVICE_KERNEL_SEMA, 0);
        if (result || !shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_KERNEL_SEMA, &handle)) {
            return SHADPS4_KERNEL_ERROR_ENOMEM;
        }
        hle->service_value[handle - 0x400] = a3;
        hle->service_user_data[handle - 0x400] = a4;
        return 0;
    }
    case SHADPS4_HLE_KERNEL_SEMA_DELETE:
        return shadps4_hle_object_destroy(
                   hle, cs, a0, SHADPS4_SERVICE_KERNEL_SEMA) ?
               SHADPS4_KERNEL_ERROR_EINVAL : 0;
    case SHADPS4_HLE_KERNEL_SEMA_WAIT:
    case SHADPS4_HLE_KERNEL_SEMA_POLL: {
        uint64_t handle;

        if (!a1 || !shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_KERNEL_SEMA, &handle)) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        if (hle->service_value[handle - 0x400] < a1) {
            return number == SHADPS4_HLE_KERNEL_SEMA_POLL ?
                   SHADPS4_KERNEL_ERROR_ETIMEDOUT :
                   SHADPS4_KERNEL_ERROR_ETIMEDOUT;
        }
        hle->service_value[handle - 0x400] -= a1;
        return 0;
    }
    case SHADPS4_HLE_KERNEL_SEMA_SIGNAL: {
        uint64_t handle;

        if (!a1 || !shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_KERNEL_SEMA, &handle) ||
            a1 > hle->service_user_data[handle - 0x400] -
                 hle->service_value[handle - 0x400]) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        hle->service_value[handle - 0x400] += a1;
        return 0;
    }
    case SHADPS4_HLE_KERNEL_SEMA_CANCEL: {
        uint64_t handle;
        uint32_t waiters = 0;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_KERNEL_SEMA, &handle)) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        hle->service_value[handle - 0x400] = MIN(
            a1, hle->service_user_data[handle - 0x400]);
        return !a2 || shadps4_guest_rw(
                   cs, a2, &waiters, sizeof(waiters), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_PTHREAD_KEY_CREATE: {
        uint32_t i;

        if (!a0) return SHADPS4_GUEST_EINVAL;
        for (i = 0; i < SHADPS4_HLE_MAX_PTHREAD_KEYS; i++) {
            if (!hle->pthread_keys[i]) {
                hle->pthread_keys[i] = true;
                hle->pthread_key_destructors[i] = a1;
                return shadps4_hle_write_u32(cs, a0, i);
            }
        }
        return SHADPS4_GUEST_ENOMEM;
    }
    case SHADPS4_HLE_PTHREAD_KEY_DELETE:
        if (a0 >= SHADPS4_HLE_MAX_PTHREAD_KEYS || !hle->pthread_keys[a0]) {
            return SHADPS4_GUEST_EINVAL;
        }
        hle->pthread_keys[a0] = false;
        hle->pthread_key_destructors[a0] = 0;
        hle->pthread_key_values[a0] = 0;
        return 0;
    case SHADPS4_HLE_PTHREAD_GETSPECIFIC:
        return a0 < SHADPS4_HLE_MAX_PTHREAD_KEYS && hle->pthread_keys[a0] ?
               hle->pthread_key_values[a0] : 0;
    case SHADPS4_HLE_PTHREAD_SETSPECIFIC:
        if (a0 >= SHADPS4_HLE_MAX_PTHREAD_KEYS || !hle->pthread_keys[a0]) {
            return SHADPS4_GUEST_EINVAL;
        }
        hle->pthread_key_values[a0] = a1;
        return 0;
    case SHADPS4_HLE_FS_READV:
        return shadps4_hle_vector_io(hle, cs, a0, a1, a2, false,
                                     false, 0);
    case SHADPS4_HLE_FS_WRITEV:
        return shadps4_hle_vector_io(hle, cs, a0, a1, a2, true,
                                     false, 0);
    case SHADPS4_HLE_FS_PREAD:
        return shadps4_hle_positional_io(hle, cs, a0, a1, a2, a3, false);
    case SHADPS4_HLE_FS_PWRITE:
        return shadps4_hle_positional_io(hle, cs, a0, a1, a2, a3, true);
    case SHADPS4_HLE_FS_PREADV:
        return shadps4_hle_vector_io(hle, cs, a0, a1, a2, false,
                                     true, a3);
    case SHADPS4_HLE_FS_PWRITEV:
        return shadps4_hle_vector_io(hle, cs, a0, a1, a2, true,
                                     true, a3);
    case SHADPS4_HLE_FS_RMDIR:
        return shadps4_hle_unlink(hle, cs, a0);
    case SHADPS4_HLE_FS_FTRUNCATE:
        return shadps4_hle_ftruncate(hle, a0, a1);
    case SHADPS4_HLE_FS_CHECK_REACHABILITY:
        return shadps4_hle_check_reachability(hle, cs, a0);
    case SHADPS4_HLE_FS_SELECT:
        return 0;
    case SHADPS4_HLE_TIME_NANOSLEEP: {
        ShadPS4GuestTime requested;
        ShadPS4GuestTime remaining = { 0 };

        if (!a0 || !shadps4_guest_rw(cs, a0, &requested,
                                     sizeof(requested), false) ||
            le64_to_cpu(requested.fraction) >= NANOSECONDS_PER_SECOND) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return !a1 || shadps4_guest_rw(cs, a1, &remaining,
                                       sizeof(remaining), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_TIME_USLEEP:
        return a0 <= UINT32_MAX ? 0 : -SHADPS4_GUEST_EINVAL;
    case SHADPS4_HLE_TIME_SLEEP:
        return 0;
    case SHADPS4_HLE_TIME_CLOCK_GETRES: {
        ShadPS4GuestTime resolution = {
            .seconds = 0,
            .fraction = cpu_to_le64(1),
        };

        return a1 && shadps4_guest_rw(cs, a1, &resolution,
                                      sizeof(resolution), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_TIME_READ_TSC:
        return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    case SHADPS4_HLE_TIME_GET_TIMEZONE: {
        uint32_t timezone[2] = { 0, 0 };

        return a0 && shadps4_guest_rw(cs, a0, timezone,
                                      sizeof(timezone), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_TIME_LOCAL_TO_UTC: {
        uint64_t seconds = cpu_to_le64(a0);
        uint32_t timezone[2] = { 0, 0 };
        uint32_t dst = 0;

        if (!a3) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        if ((a2 && !shadps4_guest_rw(cs, a2, &seconds,
                                     sizeof(seconds), true)) ||
            !shadps4_guest_rw(cs, a3, timezone, sizeof(timezone), true) ||
            (a4 && !shadps4_guest_rw(cs, a4, &dst, sizeof(dst), true))) {
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
        return 0;
    }
    case SHADPS4_HLE_TIME_UTC_TO_LOCAL: {
        uint64_t local = cpu_to_le64(a0);
        uint64_t timesec[3] = { cpu_to_le64(a0), 0, 0 };
        uint64_t dst = 0;

        if (!a1 || !shadps4_guest_rw(cs, a1, &local,
                                     sizeof(local), true) ||
            (a2 && !shadps4_guest_rw(cs, a2, timesec,
                                     sizeof(timesec), true)) ||
            (a3 && !shadps4_guest_rw(cs, a3, &dst, sizeof(dst), true))) {
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
        return 0;
    }
    case SHADPS4_HLE_RTC_CHECK_VALID: {
        ShadPS4RtcDateTime date;

        return shadps4_rtc_read_date(cs, a0, &date) ?
               shadps4_rtc_validate(&date) :
               SHADPS4_RTC_ERROR_INVALID_POINTER;
    }
    case SHADPS4_HLE_RTC_COMPARE_TICK: {
        uint64_t left;
        uint64_t right;

        if (!shadps4_rtc_read_tick(cs, a0, &left) ||
            !shadps4_rtc_read_tick(cs, a1, &right)) {
            return SHADPS4_RTC_ERROR_INVALID_POINTER;
        }
        return left <= right;
    }
    case SHADPS4_HLE_RTC_COPY_TICK: {
        uint64_t tick;

        if (!shadps4_rtc_read_tick(cs, a0, &tick) || !a1) {
            return SHADPS4_RTC_ERROR_INVALID_POINTER;
        }
        return shadps4_rtc_write_tick(cs, a1, tick) ? 0 :
               SHADPS4_RTC_ERROR_INVALID_POINTER;
    }
    case SHADPS4_HLE_RTC_FORMAT_RFC2822:
        return shadps4_rtc_format(hle, cs, a0, a1, (int32_t)a2, true);
    case SHADPS4_HLE_RTC_FORMAT_RFC2822_LOCAL:
        return shadps4_rtc_format(hle, cs, a0, a1, 0, true);
    case SHADPS4_HLE_RTC_FORMAT_RFC3339:
        return shadps4_rtc_format(hle, cs, a0, a1, (int32_t)a2, false);
    case SHADPS4_HLE_RTC_FORMAT_RFC3339_LOCAL:
        return shadps4_rtc_format(hle, cs, a0, a1, 0, false);
    case SHADPS4_HLE_RTC_CURRENT_TICK:
    case SHADPS4_HLE_RTC_CURRENT_NETWORK_TICK:
    case SHADPS4_HLE_RTC_CURRENT_DEBUG_TICK:
    case SHADPS4_HLE_RTC_CURRENT_AD_TICK:
    case SHADPS4_HLE_RTC_CURRENT_RAW_NETWORK_TICK: {
        unsigned clock = number == SHADPS4_HLE_RTC_CURRENT_NETWORK_TICK ? 1 :
                         number == SHADPS4_HLE_RTC_CURRENT_DEBUG_TICK ? 2 :
                         number == SHADPS4_HLE_RTC_CURRENT_AD_TICK ? 3 : 0;

        return shadps4_rtc_write_tick(cs, a0, shadps4_rtc_now(hle, clock)) ?
               0 : SHADPS4_RTC_ERROR_DATETIME_UNINITIALIZED;
    }
    case SHADPS4_HLE_RTC_CURRENT_CLOCK:
    case SHADPS4_HLE_RTC_CURRENT_CLOCK_LOCAL: {
        ShadPS4RtcDateTime date;
        int32_t timezone = number == SHADPS4_HLE_RTC_CURRENT_CLOCK ?
                           (int32_t)a1 : 0;
        __int128 tick = (__int128)shadps4_rtc_now(hle, 0) +
                        (__int128)timezone * 60000000;
        uint32_t error;

        if (!a0) {
            return SHADPS4_RTC_ERROR_DATETIME_UNINITIALIZED;
        }
        if (timezone < -1439 || timezone > 1439 || tick < 0 ||
            tick > SHADPS4_RTC_MAX_TICKS) {
            return SHADPS4_RTC_ERROR_INVALID_VALUE;
        }
        error = shadps4_rtc_tick_to_date(tick, &date);
        return error ? error :
               (shadps4_rtc_write_date(cs, a0, &date) ? 0 :
                SHADPS4_RTC_ERROR_INVALID_POINTER);
    }
    case SHADPS4_HLE_RTC_GET_DAY_OF_WEEK: {
        ShadPS4RtcDateTime date = {
            .year = (uint16_t)a0,
            .month = (uint16_t)a1,
            .day = (uint16_t)a2,
        };
        int64_t days;
        int weekday;
        uint32_t error = shadps4_rtc_validate(&date);

        if ((int64_t)a0 < 1 || (int64_t)a0 > 9999) {
            return SHADPS4_RTC_ERROR_INVALID_YEAR;
        }
        if (error) {
            return error;
        }
        days = shadps4_rtc_days_from_civil(date.year, date.month, date.day);
        weekday = (days + 4) % 7;
        return weekday < 0 ? weekday + 7 : weekday;
    }
    case SHADPS4_HLE_RTC_GET_DAYS_IN_MONTH:
        if ((int64_t)a0 < 1 || (int64_t)a0 > 9999) {
            return SHADPS4_RTC_ERROR_INVALID_YEAR;
        }
        if ((int64_t)a1 < 1 || (int64_t)a1 > 12) {
            return SHADPS4_RTC_ERROR_INVALID_MONTH;
        }
        return shadps4_rtc_days_in_month(a0, a1);
    case SHADPS4_HLE_RTC_GET_DOS_TIME: {
        ShadPS4RtcDateTime date;
        uint32_t value;
        uint32_t error;

        if (!shadps4_rtc_read_date(cs, a0, &date) || !a1) {
            return SHADPS4_RTC_ERROR_INVALID_POINTER;
        }
        error = shadps4_rtc_validate(&date);
        if (error) {
            return error;
        }
        if (date.year < 1980 || date.year > 2107) {
            return SHADPS4_RTC_ERROR_INVALID_YEAR;
        }
        value = date.second / 2 | date.minute << 5 | date.hour << 11 |
                date.day << 16 | date.month << 21 |
                (date.year - 1980) << 25;
        value = cpu_to_le32(value);
        return shadps4_guest_rw(cs, a1, &value, sizeof(value), true) ? 0 :
               SHADPS4_RTC_ERROR_INVALID_POINTER;
    }
    case SHADPS4_HLE_RTC_GET_TICK: {
        ShadPS4RtcDateTime date;
        uint64_t tick;
        uint32_t error;

        if (!shadps4_rtc_read_date(cs, a0, &date) || !a1) {
            return SHADPS4_RTC_ERROR_INVALID_POINTER;
        }
        error = shadps4_rtc_date_to_tick(&date, &tick);
        return error ? error :
               (shadps4_rtc_write_tick(cs, a1, tick) ? 0 :
                SHADPS4_RTC_ERROR_INVALID_POINTER);
    }
    case SHADPS4_HLE_RTC_GET_TICK_RESOLUTION:
        return 1000000;
    case SHADPS4_HLE_RTC_GET_TIME_T:
    case SHADPS4_HLE_RTC_GET_FILETIME: {
        ShadPS4RtcDateTime date;
        uint64_t tick;
        uint64_t value;
        uint64_t epoch = number == SHADPS4_HLE_RTC_GET_TIME_T ?
                         SHADPS4_RTC_UNIX_EPOCH_TICKS :
                         SHADPS4_RTC_FILETIME_EPOCH_TICKS;
        uint32_t error;

        if (!shadps4_rtc_read_date(cs, a0, &date) || !a1) {
            return SHADPS4_RTC_ERROR_INVALID_POINTER;
        }
        error = shadps4_rtc_date_to_tick(&date, &tick);
        if (error) {
            return error;
        }
        value = tick < epoch ? 0 : (tick - epoch) *
                (number == SHADPS4_HLE_RTC_GET_TIME_T ? 1 : 10);
        if (number == SHADPS4_HLE_RTC_GET_TIME_T) {
            value /= 1000000;
        }
        value = cpu_to_le64(value);
        return shadps4_guest_rw(cs, a1, &value, sizeof(value), true) ? 0 :
               SHADPS4_RTC_ERROR_INVALID_POINTER;
    }
    case SHADPS4_HLE_RTC_IS_LEAP_YEAR:
        if ((int64_t)a0 < 1 || (int64_t)a0 > 9999) {
            return SHADPS4_RTC_ERROR_INVALID_YEAR;
        }
        return shadps4_rtc_is_leap(a0);
    case SHADPS4_HLE_RTC_PARSE_DATETIME:
    case SHADPS4_HLE_RTC_PARSE_RFC3339: {
        char text[64];
        uint64_t tick;
        uint32_t error;

        if (!a0 || !a1 ||
            !shadps4_guest_read_string(cs, a1, text, sizeof(text))) {
            return SHADPS4_RTC_ERROR_INVALID_POINTER;
        }
        error = number == SHADPS4_HLE_RTC_PARSE_RFC3339 ?
                shadps4_rtc_parse_rfc3339(text, &tick) :
                shadps4_rtc_parse_datetime(text, &tick);
        return error ? error :
               (shadps4_rtc_write_tick(cs, a0, tick) ? 0 :
                SHADPS4_RTC_ERROR_INVALID_POINTER);
    }
    case SHADPS4_HLE_RTC_SET_CURRENT_TICK:
    case SHADPS4_HLE_RTC_SET_CURRENT_NETWORK_TICK:
    case SHADPS4_HLE_RTC_SET_CURRENT_DEBUG_TICK:
    case SHADPS4_HLE_RTC_SET_CURRENT_AD_TICK: {
        unsigned clock = number == SHADPS4_HLE_RTC_SET_CURRENT_NETWORK_TICK ? 1 :
                         number == SHADPS4_HLE_RTC_SET_CURRENT_DEBUG_TICK ? 2 :
                         number == SHADPS4_HLE_RTC_SET_CURRENT_AD_TICK ? 3 : 0;
        uint64_t tick;
        uint64_t now;

        if (!shadps4_rtc_read_tick(cs, a0, &tick)) {
            return SHADPS4_RTC_ERROR_INVALID_POINTER;
        }
        if (tick <= SHADPS4_RTC_UNIX_EPOCH_TICKS ||
            tick > SHADPS4_RTC_MAX_TICKS) {
            return SHADPS4_RTC_ERROR_INVALID_VALUE;
        }
        now = shadps4_rtc_now(hle, clock);
        hle->rtc_tick_offset[clock] += (int64_t)tick - (int64_t)now;
        return 0;
    }
    case SHADPS4_HLE_RTC_SET_CONF:
        hle->rtc_minuteswest = (int32_t)a2;
        hle->rtc_dsttime = (int32_t)a3;
        return 0;
    case SHADPS4_HLE_RTC_SET_DOS_TIME: {
        ShadPS4RtcDateTime date = {
            .year = ((a1 >> 25) & 0x7f) + 1980,
            .month = (a1 >> 21) & 0xf,
            .day = (a1 >> 16) & 0x1f,
            .hour = (a1 >> 11) & 0x1f,
            .minute = (a1 >> 5) & 0x3f,
            .second = (a1 & 0x1f) * 2,
        };
        uint32_t error = shadps4_rtc_validate(&date);

        return error ? error :
               (shadps4_rtc_write_date(cs, a0, &date) ? 0 :
                SHADPS4_RTC_ERROR_INVALID_POINTER);
    }
    case SHADPS4_HLE_RTC_SET_TICK: {
        ShadPS4RtcDateTime date;
        uint64_t tick;
        uint32_t error;

        if (!a0 || !shadps4_rtc_read_tick(cs, a1, &tick)) {
            return SHADPS4_RTC_ERROR_INVALID_POINTER;
        }
        error = shadps4_rtc_tick_to_date(tick, &date);
        return error ? error :
               (shadps4_rtc_write_date(cs, a0, &date) ? 0 :
                SHADPS4_RTC_ERROR_INVALID_POINTER);
    }
    case SHADPS4_HLE_RTC_SET_TIME_T:
    case SHADPS4_HLE_RTC_SET_FILETIME: {
        ShadPS4RtcDateTime date;
        __int128 tick;
        uint32_t error;

        if (!a0) {
            return SHADPS4_RTC_ERROR_INVALID_POINTER;
        }
        if ((int64_t)a1 < 0) {
            return SHADPS4_RTC_ERROR_INVALID_VALUE;
        }
        tick = number == SHADPS4_HLE_RTC_SET_TIME_T ?
               (__int128)a1 * 1000000 + SHADPS4_RTC_UNIX_EPOCH_TICKS :
               (__int128)a1 / 10 + SHADPS4_RTC_FILETIME_EPOCH_TICKS;
        if (tick > SHADPS4_RTC_MAX_TICKS) {
            return SHADPS4_RTC_ERROR_INVALID_VALUE;
        }
        error = shadps4_rtc_tick_to_date(tick, &date);
        return error ? error :
               (shadps4_rtc_write_date(cs, a0, &date) ? 0 :
                SHADPS4_RTC_ERROR_INVALID_POINTER);
    }
    case SHADPS4_HLE_RTC_ADD_DAYS:
        return shadps4_rtc_add(cs, a0, a1, (int32_t)a2,
                               SHADPS4_RTC_DAY_TICKS);
    case SHADPS4_HLE_RTC_ADD_HOURS:
        return shadps4_rtc_add(cs, a0, a1, (int32_t)a2, 3600000000ULL);
    case SHADPS4_HLE_RTC_ADD_MICROSECONDS:
    case SHADPS4_HLE_RTC_ADD_TICKS:
        return shadps4_rtc_add(cs, a0, a1, (int64_t)a2, 1);
    case SHADPS4_HLE_RTC_ADD_MINUTES:
        return shadps4_rtc_add(cs, a0, a1, (int64_t)a2, 60000000ULL);
    case SHADPS4_HLE_RTC_ADD_SECONDS:
        return shadps4_rtc_add(cs, a0, a1, (int64_t)a2, 1000000ULL);
    case SHADPS4_HLE_RTC_ADD_WEEKS:
        return shadps4_rtc_add(cs, a0, a1, (int32_t)a2,
                               7 * SHADPS4_RTC_DAY_TICKS);
    case SHADPS4_HLE_RTC_ADD_MONTHS:
    case SHADPS4_HLE_RTC_ADD_YEARS: {
        ShadPS4RtcDateTime date;
        uint64_t tick;
        int64_t year;
        int64_t month;
        int64_t total;
        int32_t amount = a2;
        uint32_t error;

        if (!a0 || !shadps4_rtc_read_tick(cs, a1, &tick)) {
            return SHADPS4_RTC_ERROR_INVALID_POINTER;
        }
        error = shadps4_rtc_tick_to_date(tick, &date);
        if (error) {
            return error;
        }
        if (number == SHADPS4_HLE_RTC_ADD_MONTHS) {
            total = (int64_t)date.year * 12 + date.month - 1 + amount;
            year = total >= 0 ? total / 12 : (total - 11) / 12;
            month = total - year * 12 + 1;
        } else {
            year = (int64_t)date.year + amount;
            month = date.month;
        }
        if (year < 1 || year > 9999) {
            return SHADPS4_RTC_ERROR_INVALID_YEAR;
        }
        date.year = year;
        date.month = month;
        if (date.day > shadps4_rtc_days_in_month(year, month)) {
            date.day = shadps4_rtc_days_in_month(year, month);
        }
        error = shadps4_rtc_date_to_tick(&date, &tick);
        return error ? error :
               (shadps4_rtc_write_tick(cs, a0, tick) ? 0 :
                SHADPS4_RTC_ERROR_INVALID_POINTER);
    }
    case SHADPS4_HLE_PROCESS_IS_SANDBOXED:
        return 1;
    case SHADPS4_HLE_PROCESS_GET_SOC_ID:
        return hle->neo_mode ? 1 : 0;
    case SHADPS4_HLE_PROCESS_GET_CPU_MODE:
        return hle->neo_mode ? 1 : 0;
    case SHADPS4_HLE_PROCESS_GET_CURRENT_CPU:
        return 0;
    case SHADPS4_HLE_MEMORY_ENABLE_ALIASING:
    case SHADPS4_HLE_MEMORY_SET_HEAP_API:
        return 0;
    case SHADPS4_HLE_MEMORY_ALLOCATE_DIRECT:
        return shadps4_hle_allocate_direct(hle, cs, a0, a1, a2, a3, a5);
    case SHADPS4_HLE_MEMORY_ALLOCATE_MAIN_DIRECT:
        return shadps4_hle_allocate_direct(
            hle, cs, 0, (uint64_t)hle->dynamic_slot_count * 2 * MiB,
            a0, a1, a3);
    case SHADPS4_HLE_MEMORY_AVAILABLE_DIRECT: {
        uint64_t limit = (uint64_t)hle->dynamic_slot_count * 2 * MiB;
        uint64_t start = ROUND_UP(MAX(a0, hle->direct_memory_next),
                                  MAX(a2, 16 * KiB));
        uint64_t size = start < MIN(a1, limit) ? MIN(a1, limit) - start : 0;
        uint64_t start_le = cpu_to_le64(start);
        uint64_t size_le = cpu_to_le64(size);

        if (a2 && (a2 & (a2 - 1))) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        if (!a3 || !a4 || !size) {
            return SHADPS4_KERNEL_ERROR_ENOMEM;
        }
        return shadps4_guest_rw(cs, a3, &start_le, sizeof(start_le), true) &&
               shadps4_guest_rw(cs, a4, &size_le, sizeof(size_le), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_MEMORY_RELEASE_DIRECT:
        return (a0 & (16 * KiB - 1)) || (a1 & (16 * KiB - 1)) ?
               SHADPS4_KERNEL_ERROR_EINVAL : 0;
    case SHADPS4_HLE_MEMORY_GET_DIRECT_SIZE:
        return (uint64_t)hle->dynamic_slot_count * 2 * MiB;
    case SHADPS4_HLE_MEMORY_MAP_DIRECT:
        return shadps4_hle_map_to_pointer(hle, cs, a0, a1, a2);
    case SHADPS4_HLE_MEMORY_MAP_DIRECT2:
        return shadps4_hle_map_to_pointer(hle, cs, a0, a1, a3);
    case SHADPS4_HLE_MEMORY_MMAP:
        return shadps4_hle_map_to_pointer(hle, cs, a6, a1, a2);
    case SHADPS4_HLE_MEMORY_MAP_FLEXIBLE:
        return shadps4_hle_map_to_pointer(hle, cs, a0, a1, a2);
    case SHADPS4_HLE_MEMORY_AVAILABLE_FLEXIBLE:
    case SHADPS4_HLE_MEMORY_CONFIGURED_FLEXIBLE: {
        uint64_t size = cpu_to_le64(
            (uint64_t)hle->dynamic_slot_count * 2 * MiB);

        return a0 && shadps4_guest_rw(cs, a0, &size, sizeof(size), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_MEMORY_RESERVE_VIRTUAL:
        return shadps4_hle_map_to_pointer(hle, cs, a0, a1, 0);
    case SHADPS4_HLE_MEMORY_VIRTUAL_QUERY: {
        ShadPS4GuestVirtualQueryInfo info = { 0 };
        uint64_t slot;

        if (!a2 || a3 < sizeof(info) || a0 < hle->dynamic_virt_base) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        slot = (a0 - hle->dynamic_virt_base) / (2 * MiB);
        if (slot >= hle->dynamic_slot_count || !hle->dynamic_slots[slot]) {
            return SHADPS4_KERNEL_ERROR_ENOENT;
        }
        info.start = cpu_to_le64(hle->dynamic_virt_base + slot * 2 * MiB);
        info.end = cpu_to_le64(le64_to_cpu(info.start) + 2 * MiB);
        info.protection = cpu_to_le32(3);
        info.memory_type = cpu_to_le32(1);
        info.attributes = 0x10;
        memcpy(info.name, "qemu-hle", sizeof("qemu-hle"));
        return shadps4_guest_rw(cs, a2, &info, sizeof(info), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_MEMORY_QUERY_PROTECTION: {
        uint32_t protection = cpu_to_le32(3);

        return a1 && shadps4_guest_rw(cs, a1, &protection,
                                      sizeof(protection), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_MEMORY_DIRECT_QUERY: {
        uint64_t end;
        uint64_t query[3] = {
            cpu_to_le64(a0 & ~((uint64_t)16 * KiB - 1)),
            0,
            0,
        };

        if (a0 == UINT64_MAX ||
            a0 + 1 > UINT64_MAX - (16 * KiB - 1)) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        end = ROUND_UP(a0 + 1, 16 * KiB);
        query[1] = cpu_to_le64(end);

        return a2 && a3 >= sizeof(query) && shadps4_guest_rw(
                   cs, a2, query, sizeof(query), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_MEMORY_PROTECT:
        return shadps4_hle_mprotect(hle, cs, a0, a1, a2) ?
               SHADPS4_KERNEL_ERROR_EINVAL : 0;
    case SHADPS4_HLE_MEMORY_IS_STACK:
    case SHADPS4_HLE_MEMORY_IS_ASAN:
        return 0;
    case SHADPS4_HLE_MEMORY_BATCH_MAP:
    case SHADPS4_HLE_MEMORY_SET_NAME:
    case SHADPS4_HLE_MEMORY_MLOCK:
    case SHADPS4_HLE_MEMORY_MSYNC:
        return 0;
    case SHADPS4_HLE_MEMORY_POOL:
        return SHADPS4_KERNEL_ERROR_ENOMEM;
    case SHADPS4_HLE_MEMORY_SET_PRT:
        hle->prt_aperture_address = a0;
        hle->prt_aperture_size = a1;
        return 0;
    case SHADPS4_HLE_MEMORY_GET_PRT: {
        uint64_t address = cpu_to_le64(hle->prt_aperture_address);
        uint64_t size = cpu_to_le64(hle->prt_aperture_size);

        return a0 && a1 && shadps4_guest_rw(
                   cs, a0, &address, sizeof(address), true) &&
               shadps4_guest_rw(cs, a1, &size, sizeof(size), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_PTHREAD_ONCE: {
        uint32_t state;

        if (!a0 || !shadps4_guest_rw(cs, a0, &state, sizeof(state), false)) {
            return SHADPS4_GUEST_EINVAL;
        }
        if (!le32_to_cpu(state)) {
            state = cpu_to_le32(1);
            if (!shadps4_guest_rw(cs, a0, &state, sizeof(state), true)) {
                return SHADPS4_GUEST_EFAULT;
            }
        }
        return 0;
    }
    case SHADPS4_HLE_PTHREAD_SELF:
        return 1;
    case SHADPS4_HLE_PTHREAD_CREATE: {
        uint64_t result = shadps4_hle_object_create(
            hle, cs, a0, SHADPS4_SERVICE_THREAD, 0);
        uint64_t handle;

        if (result || !shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_THREAD, &handle)) {
            return result ?: SHADPS4_GUEST_EFAULT;
        }
        hle->service_user_data[handle - 0x400] = a2;
        hle->service_value[handle - 0x400] = a3;
        hle->service_active[handle - 0x400] = false;
        return 0;
    }
    case SHADPS4_HLE_PTHREAD_DETACH:
        if (a0 == 1) return 0;
        if (!shadps4_hle_service_is(hle, a0, SHADPS4_SERVICE_THREAD)) {
            return SHADPS4_GUEST_EINVAL;
        }
        hle->service_nonblock[a0 - 0x400] = true;
        return 0;
    case SHADPS4_HLE_PTHREAD_EQUAL:
        return a0 == a1;
    case SHADPS4_HLE_PTHREAD_JOIN: {
        uint64_t result = 0;

        if (a0 == 1 || !shadps4_hle_service_is(
                hle, a0, SHADPS4_SERVICE_THREAD) ||
            hle->service_nonblock[a0 - 0x400]) {
            return SHADPS4_GUEST_EINVAL;
        }
        if (a1 && !shadps4_guest_rw(cs, a1, &result,
                                    sizeof(result), true)) {
            return SHADPS4_GUEST_EFAULT;
        }
        return shadps4_hle_service_delete(
            hle, a0, SHADPS4_SERVICE_THREAD) ? SHADPS4_GUEST_EINVAL : 0;
    }
    case SHADPS4_HLE_PTHREAD_CANCEL_STATE: {
        uint32_t old = 0;

        return !a1 || shadps4_guest_rw(cs, a1, &old, sizeof(old), true) ?
               0 : SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_PTHREAD_SCHED_PRIORITY:
        return a0 == 0 ? 767 : 256;
    case SHADPS4_HLE_PTHREAD_SET_SCHEDPARAM:
    case SHADPS4_HLE_PTHREAD_SET_AFFINITY:
    case SHADPS4_HLE_PTHREAD_SET_NAME:
    case SHADPS4_HLE_PTHREAD_SET_DTORS:
    case SHADPS4_HLE_PTHREAD_CLEANUP:
    case SHADPS4_HLE_PTHREAD_YIELD:
        return 0;
    case SHADPS4_HLE_PTHREAD_GET_AFFINITY: {
        uint64_t affinity = cpu_to_le64(0xff);
        uint64_t output = a2 ?: a1;
        size_t size = a2 ? MIN(a1, sizeof(affinity)) : sizeof(affinity);

        return output && shadps4_guest_rw(cs, output, &affinity,
                                          size, true) ?
               0 : SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_PTHREAD_GET_TID:
        return 1;
    case SHADPS4_HLE_PTHREAD_EXIT:
        return 0;
    case SHADPS4_HLE_PTHREAD_GET_PRIORITY:
        return shadps4_hle_write_u32(cs, a1, 700);
    case SHADPS4_HLE_PTHREAD_GET_SCHEDPARAM:
        return shadps4_hle_write_u32(cs, a1, 0) ?:
               shadps4_hle_write_u32(cs, a2, 700);
    case SHADPS4_HLE_PTHREAD_GET_NAME: {
        static const char name[] = "main";

        if (!a1 || !a2) return SHADPS4_GUEST_EINVAL;
        return shadps4_guest_rw(cs, a1, (void *)name,
                                MIN(a2, sizeof(name)), true) ?
               0 : SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_EVENT_FLAG_CREATE: {
        int handle;
        uint64_t handle_le;

        if (!a0 || !a1 || a4) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_EVENT_FLAG, 0);
        if (handle < 0) {
            return SHADPS4_KERNEL_ERROR_ENOMEM;
        }
        hle->service_value[handle - 0x400] = a3;
        handle_le = cpu_to_le64(handle);
        if (!shadps4_guest_rw(cs, a0, &handle_le,
                              sizeof(handle_le), true)) {
            shadps4_hle_service_delete(
                hle, handle, SHADPS4_SERVICE_EVENT_FLAG);
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
        return 0;
    }
    case SHADPS4_HLE_EVENT_FLAG_DELETE:
        return shadps4_hle_service_delete(
                   hle, a0, SHADPS4_SERVICE_EVENT_FLAG) ?
               SHADPS4_KERNEL_ERROR_EINVAL : 0;
    case SHADPS4_HLE_EVENT_FLAG_OPEN_CLOSE:
        return 0;
    case SHADPS4_HLE_EVENT_FLAG_SET:
    case SHADPS4_HLE_EVENT_FLAG_CLEAR:
        if (!shadps4_hle_service_is(
                hle, a0, SHADPS4_SERVICE_EVENT_FLAG)) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        if (number == SHADPS4_HLE_EVENT_FLAG_SET) {
            hle->service_value[a0 - 0x400] |= a1;
        } else {
            hle->service_value[a0 - 0x400] &= a1;
        }
        return 0;
    case SHADPS4_HLE_EVENT_FLAG_WAIT:
    case SHADPS4_HLE_EVENT_FLAG_POLL: {
        uint64_t current;
        bool matched;

        if (!a1 || !shadps4_hle_service_is(
                hle, a0, SHADPS4_SERVICE_EVENT_FLAG)) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        current = hle->service_value[a0 - 0x400];
        matched = (a2 & 0xf) == 1 ? (current & a1) == a1 :
                                    (current & a1) != 0;
        if (a3) {
            uint64_t result = cpu_to_le64(current);
            if (!shadps4_guest_rw(cs, a3, &result, sizeof(result), true)) {
                return SHADPS4_KERNEL_ERROR_EFAULT;
            }
        }
        if (!matched) {
            return number == SHADPS4_HLE_EVENT_FLAG_POLL ?
                   0x80020010U : SHADPS4_KERNEL_ERROR_ETIMEDOUT;
        }
        if ((a2 & 0xf0) == 0x10) {
            hle->service_value[a0 - 0x400] = 0;
        } else if ((a2 & 0xf0) == 0x20) {
            hle->service_value[a0 - 0x400] &= ~a1;
        }
        return 0;
    }
    case SHADPS4_HLE_EVENT_FLAG_CANCEL: {
        uint32_t waiters = 0;

        if (!shadps4_hle_service_is(
                hle, a0, SHADPS4_SERVICE_EVENT_FLAG)) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        hle->service_value[a0 - 0x400] = a1;
        return !a2 || shadps4_guest_rw(
                   cs, a2, &waiters, sizeof(waiters), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_AIO_INIT:
        memset(hle->aio_states, 0, sizeof(hle->aio_states));
        hle->aio_next_id = 1;
        return 0;
    case SHADPS4_HLE_AIO_CANCEL_ONE:
        return shadps4_hle_aio_states(hle, cs, a0, 1, a1, false, 4);
    case SHADPS4_HLE_AIO_CANCEL_MANY:
        return shadps4_hle_aio_states(hle, cs, a0, a1, a2, true, 4);
    case SHADPS4_HLE_AIO_DELETE_ONE:
        return shadps4_hle_aio_states(hle, cs, a0, 1, a1, false, 4);
    case SHADPS4_HLE_AIO_DELETE_MANY:
        return shadps4_hle_aio_states(hle, cs, a0, a1, a2, true, 4);
    case SHADPS4_HLE_AIO_POLL_ONE:
    case SHADPS4_HLE_AIO_WAIT_ONE:
        return shadps4_hle_aio_states(hle, cs, a0, 1, a1, false, 0);
    case SHADPS4_HLE_AIO_POLL_MANY:
    case SHADPS4_HLE_AIO_WAIT_MANY:
        return shadps4_hle_aio_states(hle, cs, a0, a1, a2, true, 0);
    case SHADPS4_HLE_AIO_SUBMIT_READ:
        return shadps4_hle_aio_submit(hle, cs, a0, a1, a3, false, false);
    case SHADPS4_HLE_AIO_SUBMIT_READ_MANY:
        return shadps4_hle_aio_submit(hle, cs, a0, a1, a3, false, true);
    case SHADPS4_HLE_AIO_SUBMIT_WRITE:
        return shadps4_hle_aio_submit(hle, cs, a0, a1, a3, true, false);
    case SHADPS4_HLE_AIO_SUBMIT_WRITE_MANY:
        return shadps4_hle_aio_submit(hle, cs, a0, a1, a3, true, true);
    case SHADPS4_HLE_AIO_OPTION:
        return 0;
    case SHADPS4_HLE_SIGNAL_INSTALL_HANDLER:
        if (a0 >= ARRAY_SIZE(hle->signal_handlers) || !a1) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        hle->signal_handlers[a0] = a1;
        return 0;
    case SHADPS4_HLE_SIGNAL_REMOVE_HANDLER:
        if (a0 >= ARRAY_SIZE(hle->signal_handlers)) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        hle->signal_handlers[a0] = 0;
        return 0;
    case SHADPS4_HLE_SIGNAL_ACTION: {
        ShadPS4GuestSigaction action = { 0 };
        ShadPS4GuestSigaction old = { 0 };

        if (!a0 || a0 >= ARRAY_SIZE(hle->signal_handlers)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        old.handler = cpu_to_le64(hle->signal_handlers[a0]);
        if (a2 && !shadps4_guest_rw(cs, a2, &old, sizeof(old), true)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        if (a1) {
            if (!shadps4_guest_rw(cs, a1, &action, sizeof(action), false)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            hle->signal_handlers[a0] = le64_to_cpu(action.handler);
        }
        return 0;
    }
    case SHADPS4_HLE_SIGNAL_SET_HANDLER: {
        uint64_t old;

        if (!a0 || a0 >= ARRAY_SIZE(hle->signal_handlers)) {
            return UINT64_MAX;
        }
        old = hle->signal_handlers[a0];
        hle->signal_handlers[a0] = a1;
        return old;
    }
    case SHADPS4_HLE_SIGNAL_EMPTY_SET:
    case SHADPS4_HLE_SIGNAL_FILL_SET: {
        uint64_t set[2] = { 0, 0 };

        if (number == SHADPS4_HLE_SIGNAL_FILL_SET) {
            set[0] = set[1] = UINT64_MAX;
        }
        return a0 && shadps4_guest_rw(cs, a0, set, sizeof(set), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_SIGNAL_ADD_SET:
    case SHADPS4_HLE_SIGNAL_DELETE_SET:
    case SHADPS4_HLE_SIGNAL_IS_MEMBER: {
        uint64_t set[2];
        uint64_t bit;
        uint32_t word;

        if (!a0 || !a1 || a1 > 128 || !shadps4_guest_rw(
                cs, a0, set, sizeof(set), false)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        word = (a1 - 1) / 64;
        bit = 1ULL << ((a1 - 1) % 64);
        set[0] = le64_to_cpu(set[0]);
        set[1] = le64_to_cpu(set[1]);
        if (number == SHADPS4_HLE_SIGNAL_IS_MEMBER) {
            return !!(set[word] & bit);
        }
        if (number == SHADPS4_HLE_SIGNAL_ADD_SET) {
            set[word] |= bit;
        } else {
            set[word] &= ~bit;
        }
        set[0] = cpu_to_le64(set[0]);
        set[1] = cpu_to_le64(set[1]);
        return shadps4_guest_rw(cs, a0, set, sizeof(set), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_SIGNAL_MASK: {
        uint64_t old[2] = {
            cpu_to_le64(hle->signal_mask[0]),
            cpu_to_le64(hle->signal_mask[1]),
        };

        if (a2 && !shadps4_guest_rw(cs, a2, old, sizeof(old), true)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        if (a1) {
            uint64_t set[2];
            if (!shadps4_guest_rw(cs, a1, set, sizeof(set), false)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            hle->signal_mask[0] = le64_to_cpu(set[0]);
            hle->signal_mask[1] = le64_to_cpu(set[1]);
        }
        return 0;
    }
    case SHADPS4_HLE_SIGNAL_THREAD_KILL:
        return a0 && a1 < 128 ? 0 : SHADPS4_GUEST_EINVAL;
    case SHADPS4_HLE_SIGNAL_ALT_STACK:
        return 0;
    case SHADPS4_HLE_SIGNAL_RAISE:
        return a0 < 128 ? 0 : SHADPS4_KERNEL_ERROR_EINVAL;
    case SHADPS4_HLE_KERNEL_DEBUG_TEXT: {
        uint64_t length;

        return a1 && shadps4_hle_guest_string_length(
                   cs, a1, 64 * KiB, &length) ?
               shadps4_hle_write(hle, cs, 1, a1, length) : 0;
    }
    case SHADPS4_HLE_KERNEL_GET_ALLOWED_SDK: {
        uint32_t version = cpu_to_le32(0x13520fff);

        return a0 && shadps4_guest_rw(cs, a0, &version,
                                      sizeof(version), true) ?
               0 : SHADPS4_KERNEL_ERROR_EINVAL;
    }
    case SHADPS4_HLE_KERNEL_GET_SW_VERSION: {
        ShadPS4GuestSwVersion version = {
            .size = cpu_to_le64(sizeof(version)),
            .text = "13.520.001",
            .version = cpu_to_le32(0x13520001),
        };

        return !a0 || shadps4_guest_rw(cs, a0, &version,
                                       sizeof(version), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_KERNEL_GET_AUTHINFO: {
        uint64_t auth[17] = { 0 };

        auth[1] = cpu_to_le64(0x2000000000000000ULL);
        return a1 && shadps4_guest_rw(cs, a1, auth, sizeof(auth), true) ?
               0 : UINT64_MAX;
    }
    case SHADPS4_HLE_KERNEL_GET_APP_INFO: {
        ShadPS4GuestAppInfo info = { 0 };

        memcpy(info.title_id, hle->title_id,
               MIN(strlen(hle->title_id), sizeof(info.title_id)));
        info.has_param_sfo = cpu_to_le32(1);
        return !a1 || shadps4_guest_rw(cs, a1, &info, sizeof(info), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_KERNEL_TITLE_WORKAROUND: {
        uint64_t ids[2];
        uint32_t result;

        if (!a0 || !a2 || a1 >= 0x3a || !shadps4_guest_rw(
                cs, a0 + 8, ids, sizeof(ids), false)) {
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
        result = cpu_to_le32(
            (le64_to_cpu(ids[a1 / 64]) >> (a1 % 64)) & 1);
        return shadps4_guest_rw(cs, a2, &result, sizeof(result), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_KERNEL_GET_PROCESS_TYPE:
        return 0;
    case SHADPS4_HLE_KERNEL_IOCTL:
        return shadps4_hle_ioctl(hle, cs, a0, a1, a2);
    case SHADPS4_HLE_KERNEL_SANDBOX_WORD:
        if (!hle->sandbox_word_addr) {
            uint64_t address = shadps4_hle_mmap(hle, cs, 0, 2 * MiB, 1);
            static const char word[] = "sys";

            if ((int64_t)address < 0 || !shadps4_guest_rw(
                    cs, address, (void *)word, sizeof(word), true)) {
                return 0;
            }
            hle->sandbox_word_addr = address;
        }
        return hle->sandbox_word_addr;
    case SHADPS4_HLE_KERNEL_UUID_CREATE: {
        uint8_t uuid[16];
        uint64_t seed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) ^
                        (uintptr_t)hle;
        uint32_t i;

        if (!a0) return SHADPS4_KERNEL_ERROR_EINVAL;
        for (i = 0; i < sizeof(uuid); i++) {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            uuid[i] = seed;
        }
        uuid[6] = (uuid[6] & 0x0f) | 0x40;
        uuid[8] = (uuid[8] & 0x3f) | 0x80;
        return shadps4_guest_rw(cs, a0, uuid, sizeof(uuid), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_KERNEL_REGMGR:
    case SHADPS4_HLE_KERNEL_GPI:
        return 0;
    case SHADPS4_HLE_KERNEL_SYSCONF:
        switch (a0) {
        case 0: return 0x20000;
        case 1: return 0x588bc000;
        case 2: return 100;
        case 3: return 32;
        case 4: return 1604;
        case 5: return UINT64_MAX;
        default: return 1;
        }
    case SHADPS4_HLE_KERNEL_HTONL:
        return bswap32(a0);
    case SHADPS4_HLE_KERNEL_NETWORK_UNSUPPORTED:
        return -SHADPS4_GUEST_ENOSYS;
    case SHADPS4_HLE_KERNEL_MODULE_LOAD:
        return shadps4_hle_load_start_module(hle, cs, a0, a3, a5);
    case SHADPS4_HLE_KERNEL_DLSYM:
        return shadps4_hle_dlsym(hle, cs, a0, a1, a2);
    case SHADPS4_HLE_KERNEL_MODULE_INFO_UNWIND:
        return shadps4_hle_module_info_unwind(hle, cs, a0, a1, a2, false);
    case SHADPS4_HLE_KERNEL_MODULE_INFO_FROM_ADDR: {
        uint32_t handle = 0;
        const ShadPS4ImageInfo *image =
            shadps4_hle_module_by_address(hle, a0, &handle);

        if (a1 >= 3) {
            return SHADPS4_KERNEL_ERROR_EINVAL;
        }
        return shadps4_hle_module_info_extended(
            hle, cs, image, handle, a2, false);
    }
    case SHADPS4_HLE_KERNEL_MODULE_INFO:
        return shadps4_hle_module_info_basic(hle, cs, a0, a1, false);
    case SHADPS4_HLE_KERNEL_MODULE_INFO2:
        return shadps4_hle_module_info_basic(hle, cs, a0, a1, true);
    case SHADPS4_HLE_KERNEL_MODULE_INFO_INTERNAL:
        return shadps4_hle_module_info_extended(
            hle, cs, shadps4_hle_module_by_handle(hle, a0), a0, a1, true);
    case SHADPS4_HLE_KERNEL_MODULE_LIST:
    case SHADPS4_HLE_KERNEL_MODULE_LIST2: {
        uint64_t count = cpu_to_le64(hle->module_image_count);
        uint32_t i;

        if (!a0 || !a2) {
            return SHADPS4_KERNEL_ERROR_EFAULT;
        }
        if (a1 < hle->module_image_count) {
            return SHADPS4_KERNEL_ERROR_ENOMEM;
        }
        for (i = 0; i < hle->module_image_count; i++) {
            uint32_t handle = cpu_to_le32(i);

            if (!shadps4_guest_rw(cs, a0 + i * sizeof(handle), &handle,
                                  sizeof(handle), true)) {
                return SHADPS4_KERNEL_ERROR_EFAULT;
            }
        }
        return shadps4_guest_rw(cs, a2, &count, sizeof(count), true) ?
               0 : SHADPS4_KERNEL_ERROR_EFAULT;
    }
    case SHADPS4_HLE_KERNEL_SET_TIME:
        return 0x80020001U;
    case SHADPS4_HLE_FS_GETDENTS:
        return shadps4_hle_getdirentries(hle, cs, a0, a1, a2);
    case SHADPS4_HLE_FS_TRUNCATE: {
        uint64_t fd = shadps4_hle_open(hle, cs, a0, SHADPS4_O_RDWR, 0);
        uint64_t result;

        if ((int64_t)fd < 0) {
            return fd;
        }
        result = shadps4_hle_ftruncate(hle, fd, a1);
        if ((int64_t)shadps4_hle_close(hle, fd) < 0 &&
            (int64_t)result >= 0) {
            return -SHADPS4_GUEST_EIO;
        }
        return result;
    }
    case SHADPS4_HLE_KERNEL_GETRUSAGE: {
        uint8_t usage[144] = { 0 };

        return a1 && shadps4_guest_rw(cs, a1, usage, sizeof(usage), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_LIBC_MALLOC:
        return shadps4_hle_heap_alloc(hle, cs, a0, sizeof(uint64_t));
    case SHADPS4_HLE_LIBC_FREE: {
        ShadPS4HLEHeapAllocation *allocation;

        if (!a0) {
            return 0;
        }
        allocation = shadps4_hle_heap_find(hle, a0);
        if (allocation) {
            allocation->active = false;
        }
        return 0;
    }
    case SHADPS4_HLE_LIBC_CALLOC: {
        uint64_t address;
        uint64_t size;

        if (a0 && a1 > UINT64_MAX / a0) {
            return 0;
        }
        size = a0 * a1;
        address = shadps4_hle_heap_alloc(hle, cs, size, sizeof(uint64_t));
        if (address && size && !shadps4_hle_heap_zero(cs, address, size)) {
            ShadPS4HLEHeapAllocation *allocation =
                shadps4_hle_heap_find(hle, address);

            if (allocation) {
                allocation->active = false;
            }
            return 0;
        }
        return address;
    }
    case SHADPS4_HLE_LIBC_REALLOC: {
        ShadPS4HLEHeapAllocation *old_allocation;
        uint64_t address;

        if (!a0) {
            return shadps4_hle_heap_alloc(hle, cs, a1, sizeof(uint64_t));
        }
        old_allocation = shadps4_hle_heap_find(hle, a0);
        if (!a1) {
            if (old_allocation) {
                old_allocation->active = false;
            }
            return 0;
        }
        if (!old_allocation) {
            return 0;
        }
        if (a1 <= old_allocation->size) {
            old_allocation->size = a1;
            return a0;
        }
        address = shadps4_hle_heap_alloc(hle, cs, a1, sizeof(uint64_t));
        if (!address || !shadps4_hle_heap_copy(
                cs, address, a0, old_allocation->size)) {
            ShadPS4HLEHeapAllocation *new_allocation =
                shadps4_hle_heap_find(hle, address);

            if (new_allocation) {
                new_allocation->active = false;
            }
            return 0;
        }
        old_allocation->active = false;
        return address;
    }
    case SHADPS4_HLE_LIBC_MEMALIGN:
        return shadps4_hle_heap_alloc(hle, cs, a1, a0);
    case SHADPS4_HLE_LIBC_POSIX_MEMALIGN: {
        uint64_t address;
        uint64_t value;

        if (!a0 || a1 < sizeof(uint64_t) || !is_power_of_2(a1)) {
            return SHADPS4_GUEST_EINVAL;
        }
        address = shadps4_hle_heap_alloc(hle, cs, a2, a1);
        if (!address) {
            return SHADPS4_GUEST_ENOMEM;
        }
        value = cpu_to_le64(address);
        if (!shadps4_guest_rw(cs, a0, &value, sizeof(value), true)) {
            ShadPS4HLEHeapAllocation *allocation =
                shadps4_hle_heap_find(hle, address);

            if (allocation) {
                allocation->active = false;
            }
            return SHADPS4_GUEST_EFAULT;
        }
        return 0;
    }
    case SHADPS4_HLE_LIBC_MALLOC_USABLE_SIZE: {
        ShadPS4HLEHeapAllocation *allocation =
            shadps4_hle_heap_find(hle, a0);

        return allocation ? allocation->size : 0;
    }
    case SHADPS4_HLE_LIBC_MEMCPY:
        return shadps4_hle_guest_copy(cs, a0, a1, a2);
    case SHADPS4_HLE_LIBC_MEMCPY_S:
        if (a3 > a1 || a3 > 64 * MiB) {
            if (a0 && a1) {
                uint8_t zero = 0;
                shadps4_guest_rw(cs, a0, &zero, 1, true);
            }
            return SHADPS4_GUEST_EINVAL;
        }
        return shadps4_hle_guest_copy(cs, a0, a2, a3) ?
               0 : SHADPS4_GUEST_EFAULT;
    case SHADPS4_HLE_LIBC_MEMSET: {
        g_autofree uint8_t *buffer = NULL;

        if ((!a0 && a2) || a2 > 64 * MiB) {
            return 0;
        }
        buffer = g_malloc(a2 ? a2 : 1);
        memset(buffer, a1, a2);
        return !a2 || shadps4_guest_rw(cs, a0, buffer, a2, true) ? a0 : 0;
    }
    case SHADPS4_HLE_LIBC_MEMCMP: {
        g_autofree uint8_t *left = NULL;
        g_autofree uint8_t *right = NULL;

        if ((!a0 || !a1) && a2) {
            return 0;
        }
        if (a2 > 64 * MiB) {
            return 0;
        }
        left = g_malloc(a2 ? a2 : 1);
        right = g_malloc(a2 ? a2 : 1);
        if (a2 && (!shadps4_guest_rw(cs, a0, left, a2, false) ||
                   !shadps4_guest_rw(cs, a1, right, a2, false))) {
            return 0;
        }
        return (int64_t)memcmp(left, right, a2);
    }
    case SHADPS4_HLE_LIBC_STRLEN: {
        uint64_t length;

        return shadps4_hle_guest_string_length(cs, a0, 16 * MiB, &length) ?
               length : 0;
    }
    case SHADPS4_HLE_LIBC_STRCMP:
        return (int64_t)shadps4_hle_guest_string_compare(
            cs, a0, a1, 16 * MiB);
    case SHADPS4_HLE_LIBC_STRNCMP:
        return (int64_t)shadps4_hle_guest_string_compare(cs, a0, a1, a2);
    case SHADPS4_HLE_LIBC_STRCHR: {
        uint8_t byte;
        uint64_t i;

        if (!a0) {
            return 0;
        }
        for (i = 0; i < 16 * MiB; i++) {
            if (!shadps4_guest_rw(cs, a0 + i, &byte, 1, false)) {
                return 0;
            }
            if (byte == (uint8_t)a1) {
                return a0 + i;
            }
            if (!byte) {
                return 0;
            }
        }
        return 0;
    }
    case SHADPS4_HLE_LIBC_STRNCPY: {
        uint64_t length;
        g_autofree uint8_t *buffer = NULL;

        if (!a0 || (!a1 && a2) || a2 > 16 * MiB) {
            return 0;
        }
        buffer = g_malloc0(a2 ? a2 : 1);
        if (a2 && shadps4_hle_guest_string_length(cs, a1, a2, &length)) {
            length = MIN(length, a2);
        } else {
            length = a2;
        }
        if (length && !shadps4_guest_rw(cs, a1, buffer, length, false)) {
            return 0;
        }
        return !a2 || shadps4_guest_rw(cs, a0, buffer, a2, true) ? a0 : 0;
    }
    case SHADPS4_HLE_LIBC_STRCAT: {
        uint64_t left_len;
        uint64_t right_len;

        if (!shadps4_hle_guest_string_length(cs, a0, 16 * MiB, &left_len) ||
            !shadps4_hle_guest_string_length(cs, a1, 16 * MiB, &right_len) ||
            left_len + right_len + 1 > 16 * MiB) {
            return 0;
        }
        return shadps4_hle_guest_copy(cs, a0 + left_len, a1,
                                      right_len + 1) ? a0 : 0;
    }
    case SHADPS4_HLE_LIBC_MATH_SIN:
    case SHADPS4_HLE_LIBC_MATH_COS:
    case SHADPS4_HLE_LIBC_MATH_TAN:
    case SHADPS4_HLE_LIBC_MATH_ASIN:
    case SHADPS4_HLE_LIBC_MATH_ACOS:
    case SHADPS4_HLE_LIBC_MATH_ATAN:
    case SHADPS4_HLE_LIBC_MATH_EXP:
    case SHADPS4_HLE_LIBC_MATH_EXP2:
    case SHADPS4_HLE_LIBC_MATH_LOG:
    case SHADPS4_HLE_LIBC_MATH_LOG10: {
        double input;
        double output;
        uint64_t bits = env->xmm_regs[0].ZMM_Q(0);

        memcpy(&input, &bits, sizeof(input));
        switch (number) {
        case SHADPS4_HLE_LIBC_MATH_SIN: output = sin(input); break;
        case SHADPS4_HLE_LIBC_MATH_COS: output = cos(input); break;
        case SHADPS4_HLE_LIBC_MATH_TAN: output = tan(input); break;
        case SHADPS4_HLE_LIBC_MATH_ASIN: output = asin(input); break;
        case SHADPS4_HLE_LIBC_MATH_ACOS: output = acos(input); break;
        case SHADPS4_HLE_LIBC_MATH_ATAN: output = atan(input); break;
        case SHADPS4_HLE_LIBC_MATH_EXP: output = exp(input); break;
        case SHADPS4_HLE_LIBC_MATH_EXP2: output = exp2(input); break;
        case SHADPS4_HLE_LIBC_MATH_LOG: output = log(input); break;
        default: output = log10(input); break;
        }
        memcpy(&bits, &output, sizeof(bits));
        env->xmm_regs[0].ZMM_Q(0) = bits;
        return 0;
    }
    case SHADPS4_HLE_LIBC_MATH_ATAN2:
    case SHADPS4_HLE_LIBC_MATH_POW: {
        double left;
        double right;
        double output;
        uint64_t bits0 = env->xmm_regs[0].ZMM_Q(0);
        uint64_t bits1 = env->xmm_regs[1].ZMM_Q(0);

        memcpy(&left, &bits0, sizeof(left));
        memcpy(&right, &bits1, sizeof(right));
        output = number == SHADPS4_HLE_LIBC_MATH_ATAN2 ?
                 atan2(left, right) : pow(left, right);
        memcpy(&bits0, &output, sizeof(bits0));
        env->xmm_regs[0].ZMM_Q(0) = bits0;
        return 0;
    }
    case SHADPS4_HLE_LIBC_MATH_SINCOS: {
        double input;
        double sin_value;
        double cos_value;
        uint64_t bits = env->xmm_regs[0].ZMM_Q(0);

        memcpy(&input, &bits, sizeof(input));
        sin_value = sin(input);
        cos_value = cos(input);
        return shadps4_guest_rw(cs, a0, &sin_value, sizeof(sin_value), true) &&
               shadps4_guest_rw(cs, a1, &cos_value, sizeof(cos_value), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_LIBC_MATHF_SIN:
    case SHADPS4_HLE_LIBC_MATHF_COS:
    case SHADPS4_HLE_LIBC_MATHF_TAN:
    case SHADPS4_HLE_LIBC_MATHF_ASIN:
    case SHADPS4_HLE_LIBC_MATHF_ACOS:
    case SHADPS4_HLE_LIBC_MATHF_ATAN:
    case SHADPS4_HLE_LIBC_MATHF_EXP:
    case SHADPS4_HLE_LIBC_MATHF_EXP2:
    case SHADPS4_HLE_LIBC_MATHF_LOG:
    case SHADPS4_HLE_LIBC_MATHF_LOG10: {
        float input;
        float output;
        uint32_t bits = env->xmm_regs[0].ZMM_L(0);

        memcpy(&input, &bits, sizeof(input));
        switch (number) {
        case SHADPS4_HLE_LIBC_MATHF_SIN: output = sinf(input); break;
        case SHADPS4_HLE_LIBC_MATHF_COS: output = cosf(input); break;
        case SHADPS4_HLE_LIBC_MATHF_TAN: output = tanf(input); break;
        case SHADPS4_HLE_LIBC_MATHF_ASIN: output = asinf(input); break;
        case SHADPS4_HLE_LIBC_MATHF_ACOS: output = acosf(input); break;
        case SHADPS4_HLE_LIBC_MATHF_ATAN: output = atanf(input); break;
        case SHADPS4_HLE_LIBC_MATHF_EXP: output = expf(input); break;
        case SHADPS4_HLE_LIBC_MATHF_EXP2: output = exp2f(input); break;
        case SHADPS4_HLE_LIBC_MATHF_LOG: output = logf(input); break;
        default: output = log10f(input); break;
        }
        memcpy(&bits, &output, sizeof(bits));
        env->xmm_regs[0].ZMM_L(0) = bits;
        return 0;
    }
    case SHADPS4_HLE_LIBC_MATHF_ATAN2:
    case SHADPS4_HLE_LIBC_MATHF_POW: {
        float left;
        float right;
        float output;
        uint32_t bits0 = env->xmm_regs[0].ZMM_L(0);
        uint32_t bits1 = env->xmm_regs[1].ZMM_L(0);

        memcpy(&left, &bits0, sizeof(left));
        memcpy(&right, &bits1, sizeof(right));
        output = number == SHADPS4_HLE_LIBC_MATHF_ATAN2 ?
                 atan2f(left, right) : powf(left, right);
        memcpy(&bits0, &output, sizeof(bits0));
        env->xmm_regs[0].ZMM_L(0) = bits0;
        return 0;
    }
    case SHADPS4_HLE_LIBC_MATHF_SINCOS: {
        float input;
        float sin_value;
        float cos_value;
        uint32_t bits = env->xmm_regs[0].ZMM_L(0);

        memcpy(&input, &bits, sizeof(input));
        sin_value = sinf(input);
        cos_value = cosf(input);
        return shadps4_guest_rw(cs, a0, &sin_value, sizeof(sin_value), true) &&
               shadps4_guest_rw(cs, a1, &cos_value, sizeof(cos_value), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_LIBC_STRCPY_S:
    case SHADPS4_HLE_LIBC_STRCAT_S:
    case SHADPS4_HLE_LIBC_STRNCPY_S: {
        uint64_t source_len;
        uint64_t dest_len = 0;
        uint64_t copy_len;

        if (!a0 || !a1 || !a2 ||
            !shadps4_hle_guest_string_length(cs, a2, 16 * MiB,
                                               &source_len)) {
            return SHADPS4_GUEST_EINVAL;
        }
        if (number == SHADPS4_HLE_LIBC_STRCAT_S &&
            !shadps4_hle_guest_string_length(cs, a0, a1, &dest_len)) {
            return SHADPS4_GUEST_EINVAL;
        }
        copy_len = source_len + 1;
        if (number == SHADPS4_HLE_LIBC_STRNCPY_S) {
            copy_len = MIN(copy_len, a3);
        }
        if (!copy_len || dest_len + copy_len > a1 ||
            !shadps4_hle_guest_copy(cs, a0 + dest_len, a2, copy_len)) {
            uint8_t zero = 0;

            shadps4_guest_rw(cs, a0, &zero, 1, true);
            return SHADPS4_GUEST_EINVAL;
        }
        if (number == SHADPS4_HLE_LIBC_STRNCPY_S &&
            copy_len == a3 && copy_len <= source_len) {
            uint8_t zero = 0;

            if (!shadps4_guest_rw(cs, a0 + copy_len - 1,
                                  &zero, 1, true)) {
                return SHADPS4_GUEST_EFAULT;
            }
        }
        return 0;
    }
    case SHADPS4_HLE_LIBC_MUTEX_INIT: {
        int handle;

        if (!a0) {
            return 1;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_LIBC_MUTEX, 0);
        return handle < 0 || shadps4_hle_write_u64(cs, a0, handle) ? 1 : 0;
    }
    case SHADPS4_HLE_LIBC_MUTEX_LOCK:
    case SHADPS4_HLE_LIBC_MUTEX_UNLOCK:
    case SHADPS4_HLE_LIBC_MUTEX_DESTROY: {
        uint64_t handle;

        if (!a0 || !shadps4_guest_rw(cs, a0, &handle,
                                      sizeof(handle), false)) {
            return 1;
        }
        handle = le64_to_cpu(handle);
        if (!shadps4_hle_service_is(hle, handle,
                                    SHADPS4_SERVICE_LIBC_MUTEX)) {
            return 1;
        }
        if (number == SHADPS4_HLE_LIBC_MUTEX_DESTROY) {
            return shadps4_hle_service_delete(
                hle, handle, SHADPS4_SERVICE_LIBC_MUTEX) ? 1 : 0;
        }
        hle->service_active[handle - 0x400] =
            number == SHADPS4_HLE_LIBC_MUTEX_LOCK;
        return 0;
    }
    case SHADPS4_HLE_LIBC_FFLUSH:
        return 0;
    case SHADPS4_HLE_LIBC_FILE_FIND:
        return shadps4_hle_libc_file_alloc(hle, cs);
    case SHADPS4_HLE_LIBC_FILE_PREP:
        return shadps4_hle_libc_prepare(hle, cs, a0, a1, a2,
                                        (int32_t)a3, a4);
    case SHADPS4_HLE_LIBC_FILE_OPEN_RAW: {
        uint32_t flags = ((a1 << 5) & SHADPS4_O_CREAT) |
                         ((a1 & 8) ? SHADPS4_O_TRUNC : 0) |
                         ((a1 & 4) ? SHADPS4_O_APPEND : 0) |
                         MAX((int)(a1 & 3) - 1, 0);

        return shadps4_hle_open(hle, cs, a0, flags,
                                a2 ? 0600 : 0666);
    }
    case SHADPS4_HLE_LIBC_FILE_POSITION:
    case SHADPS4_HLE_LIBC_FSEEK: {
        int index = shadps4_hle_libc_file_index(hle, a0);
        int64_t offset = number == SHADPS4_HLE_LIBC_FILE_POSITION ?
                         (int64_t)a2 : (int64_t)a1;
        uint64_t whence = number == SHADPS4_HLE_LIBC_FILE_POSITION ? a3 : a2;
        uint64_t result;

        if (index < 0 || hle->libc_file_fd[index] < 0 || whence > 2) {
            return UINT64_MAX;
        }
        if (number == SHADPS4_HLE_LIBC_FILE_POSITION && a1) {
            uint64_t position;

            if (!shadps4_guest_rw(cs, a1, &position,
                                  sizeof(position), false)) {
                return UINT64_MAX;
            }
            offset += le64_to_cpu(position);
        }
        result = shadps4_hle_lseek(hle, hle->libc_file_fd[index],
                                    offset, whence);
        return (int64_t)result < 0 ? UINT64_MAX : 0;
    }
    case SHADPS4_HLE_LIBC_FILE_AVAILABLE:
        return a1 < a2 ? a2 - a1 : 0;
    case SHADPS4_HLE_LIBC_FILE_READ_PREP: {
        int index = shadps4_hle_libc_file_index(hle, a0);

        return index >= 0 && hle->libc_file_fd[index] >= 0 ? 1 : UINT64_MAX;
    }
    case SHADPS4_HLE_LIBC_FILE_FREE: {
        int index = shadps4_hle_libc_file_index(hle, a0);

        if (index >= 0) {
            shadps4_hle_libc_file_free(hle, cs, a0, index);
        }
        return 0;
    }
    case SHADPS4_HLE_LIBC_FILE_LOCK:
        return 0;
    case SHADPS4_HLE_LIBC_FOPEN: {
        uint64_t file = shadps4_hle_libc_file_alloc(hle, cs);

        if (!file) {
            return 0;
        }
        if (!shadps4_hle_libc_prepare(hle, cs, a0, a1, file, -1, 0)) {
            int index = shadps4_hle_libc_file_index(hle, file);

            if (index >= 0) {
                shadps4_hle_libc_file_free(hle, cs, file, index);
            }
            return 0;
        }
        return file;
    }
    case SHADPS4_HLE_LIBC_FREAD: {
        int index = shadps4_hle_libc_file_index(hle, a3);
        uint64_t result;
        uint64_t total;

        if (!a1 || !a2) {
            return 0;
        }
        if (index < 0 || hle->libc_file_fd[index] < 0 ||
            a1 > 16 * MiB || a2 > (16 * MiB) / a1) {
            return 0;
        }
        total = a1 * a2;
        result = shadps4_hle_read(hle, cs, hle->libc_file_fd[index],
                                   a0, total);
        return (int64_t)result < 0 ? 0 : result / a1;
    }
    case SHADPS4_HLE_LIBC_FCLOSE: {
        int index = shadps4_hle_libc_file_index(hle, a0);
        uint64_t result;

        if (index < 0 || hle->libc_file_fd[index] < 0) {
            return UINT64_MAX;
        }
        result = shadps4_hle_close(hle, hle->libc_file_fd[index]);
        shadps4_hle_libc_file_free(hle, cs, a0, index);
        return (int64_t)result < 0 ? UINT64_MAX : 0;
    }
    case SHADPS4_HLE_LIBC_SNPRINTF:
        return shadps4_hle_libc_format(cs, a0, a1, a2);
    case SHADPS4_HLE_LIBC_HEAP_TRACE_INFO: {
        uint8_t info[32] = { 0 };

        if (!a0 || !shadps4_guest_rw(cs, a0, info, sizeof(info), false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        memset(info + 12, 0, sizeof(info) - 12);
        return shadps4_guest_rw(cs, a0, info, sizeof(info), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_LIBC_UNSUPPORTED:
        return -SHADPS4_GUEST_ENOSYS;
    case SHADPS4_HLE_NETCTL_INIT:
        hle->netctl_initialized = true;
        return 0;
    case SHADPS4_HLE_NETCTL_TERM:
        hle->netctl_initialized = false;
        memset(hle->netctl_callbacks, 0, sizeof(hle->netctl_callbacks));
        memset(hle->netctl_callback_args, 0,
               sizeof(hle->netctl_callback_args));
        return 0;
    case SHADPS4_HLE_NETCTL_GET_STATE: {
        uint32_t state = cpu_to_le32(3);

        if (!hle->netctl_initialized || !a0) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_guest_rw(cs, a0, &state, sizeof(state), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_NETCTL_GET_RESULT: {
        uint32_t result = 0;

        if (!hle->netctl_initialized || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_guest_rw(cs, a1, &result, sizeof(result), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_NETCTL_GET_INFO: {
        uint8_t info[256] = { 0 };
        size_t size = sizeof(uint32_t);

        if (!hle->netctl_initialized || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        switch (a0) {
        case 1: stl_le_p(info, 0); break;
        case 2: info[0] = 0x02; info[5] = 0x01; size = 6; break;
        case 3: stl_le_p(info, 1500); break;
        case 4: stl_le_p(info, 1); break;
        case 11: stl_le_p(info, 1); break;
        case 14: pstrcpy((char *)info, 16, "127.0.0.1"); size = 16; break;
        case 15: pstrcpy((char *)info, 16, "255.0.0.0"); size = 16; break;
        case 16: pstrcpy((char *)info, 16, "127.0.0.1"); size = 16; break;
        case 17:
        case 18: pstrcpy((char *)info, 16, "1.1.1.1"); size = 16; break;
        case 19: stl_le_p(info, 0); break;
        case 20: size = 1; break;
        case 21: stw_le_p(info, 0); size = 2; break;
        default: return -SHADPS4_GUEST_ENOSYS;
        }
        return shadps4_guest_rw(cs, a1, info, size, true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_NETCTL_GET_NAT_INFO: {
        uint32_t nat[4];

        if (!hle->netctl_initialized || !a0 ||
            !shadps4_guest_rw(cs, a0, nat, sizeof(nat), false) ||
            le32_to_cpu(nat[0]) != sizeof(nat)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        nat[1] = cpu_to_le32(0);
        nat[2] = cpu_to_le32(3);
        nat[3] = cpu_to_le32(0x0100007f);
        return shadps4_guest_rw(cs, a0, nat, sizeof(nat), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_NETCTL_REGISTER_CALLBACK: {
        uint32_t i;
        uint32_t cid;

        if (!hle->netctl_initialized || !a0 || !a2) {
            return -SHADPS4_GUEST_EINVAL;
        }
        for (i = 0; i < SHADPS4_HLE_MAX_NETCTL_CALLBACKS; i++) {
            if (!hle->netctl_callbacks[i]) {
                hle->netctl_callbacks[i] = a0;
                hle->netctl_callback_args[i] = a1;
                cid = cpu_to_le32(i + 1);
                return shadps4_guest_rw(cs, a2, &cid, sizeof(cid), true) ?
                       0 : -SHADPS4_GUEST_EFAULT;
            }
        }
        return -SHADPS4_GUEST_ENOMEM;
    }
    case SHADPS4_HLE_NETCTL_UNREGISTER_CALLBACK:
        if (!a0 || a0 > SHADPS4_HLE_MAX_NETCTL_CALLBACKS ||
            !hle->netctl_callbacks[a0 - 1]) {
            return -SHADPS4_GUEST_EINVAL;
        }
        hle->netctl_callbacks[a0 - 1] = 0;
        hle->netctl_callback_args[a0 - 1] = 0;
        return 0;
    case SHADPS4_HLE_NETCTL_CHECK_CALLBACK:
        return hle->netctl_initialized ? 0 : -SHADPS4_GUEST_EINVAL;
    case SHADPS4_HLE_NETCTL_UNSUPPORTED:
        return -SHADPS4_GUEST_ENOSYS;
    case SHADPS4_HLE_HTTP2_CREATE_REQUEST:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_TEMPLATE) ||
            shadps4_hle_http_string(cs, a2)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_hle_http_create_request(hle, cs, a0, a1, a2, a3);
    case SHADPS4_HLE_HTTP2_ABORT_REQUEST:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST)) {
            return -SHADPS4_GUEST_EBADF;
        }
        return shadps4_hle_host_result(qemu_host_http_abort(
            hle->service_host_handles[a0 - 0x400]));
    case SHADPS4_HLE_HTTP2_REQUEST_OPTION:
        if (a0 <= 0x400 || a0 - 0x400 >= SHADPS4_HLE_MAX_SERVICE_OBJECTS) {
            return -SHADPS4_GUEST_EBADF;
        }
        switch (hle->service_objects[a0 - 0x400]) {
        case SHADPS4_SERVICE_HTTP_CONTEXT:
        case SHADPS4_SERVICE_HTTP_TEMPLATE:
        case SHADPS4_SERVICE_HTTP_REQUEST:
            return 0;
        default:
            return -SHADPS4_GUEST_EBADF;
        }
    case SHADPS4_HLE_HTTP2_UNSUPPORTED:
        return -SHADPS4_GUEST_ENOSYS;
    case SHADPS4_HLE_WEBAPI_INIT:
    case SHADPS4_HLE_WEBAPI2_INIT: {
        int handle;
        uint32_t invalid = number == SHADPS4_HLE_WEBAPI2_INIT ?
                           SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
                           SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_CONTEXT)) {
            return invalid;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_WEBAPI_CONTEXT, a0);
        if (handle >= 0) {
            hle->service_value[handle - 0x400] = a1;
        }
        return handle;
    }
    case SHADPS4_HLE_WEBAPI_CHECK_TIMEOUT:
        return 0;
    case SHADPS4_HLE_WEBAPI_GET_ERROR_CODE:
        return (uint32_t)hle->webapi_last_error;
    case SHADPS4_HLE_WEBAPI_CREATE_USER_ONLINE:
    case SHADPS4_HLE_WEBAPI_CREATE_USER_ID:
    case SHADPS4_HLE_WEBAPI2_CREATE_USER_ID:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_CONTEXT)) {
            return number == SHADPS4_HLE_WEBAPI2_CREATE_USER_ID ?
                   SHADPS4_NP_WEBAPI2_ERROR_INVALID_LIB_CONTEXT_ID :
                   SHADPS4_NP_WEBAPI_ERROR_INVALID_LIB_CONTEXT_ID;
        }
        if ((number == SHADPS4_HLE_WEBAPI_CREATE_USER_ONLINE && !a1) ||
            ((number == SHADPS4_HLE_WEBAPI_CREATE_USER_ID ||
              number == SHADPS4_HLE_WEBAPI2_CREATE_USER_ID) &&
             (int32_t)a1 == -1)) {
            return number == SHADPS4_HLE_WEBAPI2_CREATE_USER_ID ?
                   SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
                   SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        }
        return shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_WEBAPI_USER, a0);
    case SHADPS4_HLE_WEBAPI_CREATE_HANDLE:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_CONTEXT)) {
            return SHADPS4_NP_WEBAPI_ERROR_INVALID_LIB_CONTEXT_ID;
        }
        return shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_WEBAPI_HANDLE, a0);
    case SHADPS4_HLE_WEBAPI_CREATE_REQUEST:
    case SHADPS4_HLE_WEBAPI_CREATE_MULTIPART_REQUEST: {
        static const char *const methods[] = {
            "GET", "POST", "PUT", "DELETE", "PATCH",
        };
        uint64_t content_length = 0;
        uint64_t output = number == SHADPS4_HLE_WEBAPI_CREATE_REQUEST ?
                          a5 : a4;

        if (a3 >= ARRAY_SIZE(methods)) {
            return SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        }
        if (number == SHADPS4_HLE_WEBAPI_CREATE_REQUEST && a4) {
            uint64_t length_le;
            uint64_t type_le;

            if (!shadps4_guest_rw(cs, a4, &length_le, sizeof(length_le),
                                  false) ||
                !shadps4_guest_rw(cs, a4 + 8, &type_le, sizeof(type_le),
                                  false)) {
                return SHADPS4_NP_WEBAPI_ERROR_INVALID_CONTENT;
            }
            content_length = le64_to_cpu(length_le);
            if (content_length && !le64_to_cpu(type_le)) {
                return SHADPS4_NP_WEBAPI_ERROR_INVALID_CONTENT;
            }
        }
        return shadps4_hle_webapi_create_request(
            hle, cs, a0, a1, a2, methods[a3], content_length, output,
            SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT);
    }
    case SHADPS4_HLE_WEBAPI2_CREATE_REQUEST:
    case SHADPS4_HLE_WEBAPI2_CREATE_MULTIPART_REQUEST: {
        char method[32];
        uint64_t content_length = 0;
        uint64_t output = number == SHADPS4_HLE_WEBAPI2_CREATE_REQUEST ?
                          a5 : a4;

        if (!shadps4_guest_read_string(cs, a3, method, sizeof(method))) {
            return SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
        }
        if (number == SHADPS4_HLE_WEBAPI2_CREATE_REQUEST && a4) {
            uint64_t length_le;
            uint64_t type_le;

            if (!shadps4_guest_rw(cs, a4, &length_le, sizeof(length_le),
                                  false) ||
                !shadps4_guest_rw(cs, a4 + 8, &type_le, sizeof(type_le),
                                  false)) {
                return SHADPS4_NP_WEBAPI2_ERROR_INVALID_CONTENT;
            }
            content_length = le64_to_cpu(length_le);
            if (content_length && !le64_to_cpu(type_le)) {
                return SHADPS4_NP_WEBAPI2_ERROR_INVALID_CONTENT;
            }
        }
        return shadps4_hle_webapi_create_request(
            hle, cs, a0, a1, a2, method, content_length, output,
            SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT);
    }
    case SHADPS4_HLE_WEBAPI_DELETE_CONTEXT:
        return shadps4_hle_service_delete(
            hle, a0, SHADPS4_SERVICE_WEBAPI_CONTEXT);
    case SHADPS4_HLE_WEBAPI_DELETE_USER:
        return shadps4_hle_service_delete(
            hle, a0, SHADPS4_SERVICE_WEBAPI_USER);
    case SHADPS4_HLE_WEBAPI_DELETE_HANDLE:
        return shadps4_hle_service_delete(
            hle, a1, SHADPS4_SERVICE_WEBAPI_HANDLE);
    case SHADPS4_HLE_WEBAPI_GET_HEADER_VALUE:
    case SHADPS4_HLE_WEBAPI2_GET_HEADER_VALUE:
        return shadps4_hle_webapi_header(
            hle, cs, a0, a1, a2, a3, 0,
            number == SHADPS4_HLE_WEBAPI2_GET_HEADER_VALUE ?
            SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
            SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT);
    case SHADPS4_HLE_WEBAPI_GET_HEADER_LENGTH:
    case SHADPS4_HLE_WEBAPI2_GET_HEADER_LENGTH:
        return shadps4_hle_webapi_header(
            hle, cs, a0, a1, 0, 0, a2,
            number == SHADPS4_HLE_WEBAPI2_GET_HEADER_LENGTH ?
            SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
            SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT);
    case SHADPS4_HLE_WEBAPI_GET_MEMORY_STATS:
    case SHADPS4_HLE_WEBAPI2_GET_MEMORY_STATS: {
        uint64_t stats[4] = { 0 };
        bool webapi2 = number == SHADPS4_HLE_WEBAPI2_GET_MEMORY_STATS;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_CONTEXT) || !a1) {
            return webapi2 ? SHADPS4_NP_WEBAPI2_ERROR_CONTEXT_NOT_FOUND :
                   SHADPS4_NP_WEBAPI_ERROR_CONTEXT_NOT_FOUND;
        }
        stats[0] = cpu_to_le64(hle->service_value[a0 - 0x400]);
        return shadps4_guest_rw(cs, a1, stats, sizeof(stats), true) ? 0 :
               (webapi2 ? SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
                SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT);
    }
    case SHADPS4_HLE_WEBAPI_SEND_WITH_INFO:
    case SHADPS4_HLE_WEBAPI2_SEND_WITH_INFO: {
        g_autofree void *body = NULL;
        uint8_t response[32] = { 0 };
        uint64_t length = 0;
        int status = 0;
        int ret;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST) ||
            (!a1 && a2) || a2 > 32 * MiB) {
            return number == SHADPS4_HLE_WEBAPI2_SEND_WITH_INFO ?
                   SHADPS4_NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND :
                   SHADPS4_NP_WEBAPI_ERROR_REQUEST_NOT_FOUND;
        }
        if (a2) {
            body = g_malloc(a2);
            if (!shadps4_guest_rw(cs, a1, body, a2, false)) {
                return number == SHADPS4_HLE_WEBAPI2_SEND_WITH_INFO ?
                       SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
                       SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
            }
        }
        ret = qemu_host_http_send(
            hle->service_host_handles[a0 - 0x400], body, a2);
        if (ret < 0) {
            return shadps4_hle_host_result(ret);
        }
        if (a3) {
            qemu_host_http_get_status(
                hle->service_host_handles[a0 - 0x400], &status);
            qemu_host_http_get_length(
                hle->service_host_handles[a0 - 0x400], &length);
            stl_le_p(response, status);
            stq_le_p(response + 24, length);
            if (!shadps4_guest_rw(cs, a3, response, sizeof(response), true)) {
                return number == SHADPS4_HLE_WEBAPI2_SEND_WITH_INFO ?
                       SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
                       SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
            }
        }
        return 0;
    }
    case SHADPS4_HLE_WEBAPI_SEND_MULTIPART:
    case SHADPS4_HLE_WEBAPI2_SEND_MULTIPART: {
        g_autofree void *body = NULL;
        int ret;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST) ||
            (int32_t)a1 <= 0 || !a2 || !a3 || a3 > 32 * MiB) {
            return number == SHADPS4_HLE_WEBAPI2_SEND_MULTIPART ?
                   SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
                   SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        }
        body = g_malloc(a3);
        if (!shadps4_guest_rw(cs, a2, body, a3, false)) {
            return number == SHADPS4_HLE_WEBAPI2_SEND_MULTIPART ?
                   SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
                   SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        }
        ret = qemu_host_http_send(
            hle->service_host_handles[a0 - 0x400], body, a3);
        return ret < 0 ? shadps4_hle_host_result(ret) : 0;
    }
    case SHADPS4_HLE_WEBAPI_OPTION:
    case SHADPS4_HLE_WEBAPI2_OPTION:
        if (a0 <= 0x400 || a0 - 0x400 >= SHADPS4_HLE_MAX_SERVICE_OBJECTS) {
            return number == SHADPS4_HLE_WEBAPI2_OPTION ?
                   SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
                   SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        }
        switch (hle->service_objects[a0 - 0x400]) {
        case SHADPS4_SERVICE_WEBAPI_CONTEXT:
        case SHADPS4_SERVICE_WEBAPI_USER:
        case SHADPS4_SERVICE_WEBAPI_HANDLE:
        case SHADPS4_SERVICE_HTTP_REQUEST:
            hle->service_value[a0 - 0x400] = a1;
            return 0;
        default:
            return number == SHADPS4_HLE_WEBAPI2_OPTION ?
                   SHADPS4_NP_WEBAPI2_ERROR_INVALID_ARGUMENT :
                   SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        }
    case SHADPS4_HLE_WEBAPI_ABORT_HANDLE:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_CONTEXT) ||
            !shadps4_hle_service_is(hle, a1,
                                    SHADPS4_SERVICE_WEBAPI_HANDLE) ||
            hle->service_parents[a1 - 0x400] != a0) {
            return SHADPS4_NP_WEBAPI_ERROR_CONTEXT_NOT_FOUND;
        }
        hle->service_active[a1 - 0x400] = false;
        return 0;
    case SHADPS4_HLE_WEBAPI_ADD_MULTIPART: {
        uint32_t index = 0;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_HTTP_REQUEST) ||
            !a1 || !a2) {
            return SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        }
        return shadps4_guest_rw(cs, a2, &index, sizeof(index), true) ? 0 :
               SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
    }
    case SHADPS4_HLE_WEBAPI_FILTER_CREATE: {
        int handle;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_CONTEXT) ||
            !a1 || !a2) {
            return SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_WEBAPI_FILTER, a0);
        return handle < 0 ? SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT : handle;
    }
    case SHADPS4_HLE_WEBAPI_FILTER_DELETE:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_CONTEXT) ||
            !shadps4_hle_service_is(hle, a1,
                                    SHADPS4_SERVICE_WEBAPI_FILTER) ||
            hle->service_parents[a1 - 0x400] != a0) {
            return SHADPS4_NP_WEBAPI_ERROR_CONTEXT_NOT_FOUND;
        }
        return shadps4_hle_service_delete(
            hle, a1, SHADPS4_SERVICE_WEBAPI_FILTER);
    case SHADPS4_HLE_WEBAPI_CALLBACK_REGISTER: {
        int callback;

        if ((!shadps4_hle_service_is(hle, a0,
                                     SHADPS4_SERVICE_WEBAPI_USER) &&
             !shadps4_hle_service_is(hle, a0,
                                     SHADPS4_SERVICE_WEBAPI_CONTEXT)) || !a2) {
            return SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        }
        if (a1 && !shadps4_hle_service_is(
                hle, a1, SHADPS4_SERVICE_WEBAPI_FILTER)) {
            return SHADPS4_NP_WEBAPI_ERROR_CONTEXT_NOT_FOUND;
        }
        callback = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_WEBAPI_CALLBACK, a0);
        if (callback >= 0) {
            hle->service_user_data[callback - 0x400] = a2;
            hle->service_aux_value[callback - 0x400] = a3;
            hle->service_value[callback - 0x400] = a1;
            hle->service_active[callback - 0x400] = true;
        }
        return callback < 0 ? SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT :
               callback;
    }
    case SHADPS4_HLE_WEBAPI_CALLBACK_REGISTER_DIRECT: {
        int callback;

        if ((!shadps4_hle_service_is(hle, a0,
                                     SHADPS4_SERVICE_WEBAPI_USER) &&
             !shadps4_hle_service_is(hle, a0,
                                     SHADPS4_SERVICE_WEBAPI_CONTEXT)) || !a1) {
            return SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        }
        callback = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_WEBAPI_CALLBACK, a0);
        if (callback >= 0) {
            hle->service_user_data[callback - 0x400] = a1;
            hle->service_aux_value[callback - 0x400] = a2;
            hle->service_active[callback - 0x400] = true;
        }
        return callback < 0 ? SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT :
               callback;
    }
    case SHADPS4_HLE_WEBAPI_CALLBACK_UNREGISTER:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_USER) ||
            !shadps4_hle_service_is(hle, a1,
                                    SHADPS4_SERVICE_WEBAPI_CALLBACK) ||
            hle->service_parents[a1 - 0x400] != a0) {
            return SHADPS4_NP_WEBAPI_ERROR_USER_NOT_FOUND;
        }
        return shadps4_hle_service_delete(
            hle, a1, SHADPS4_SERVICE_WEBAPI_CALLBACK);
    case SHADPS4_HLE_WEBAPI_CALLBACK_UNREGISTER_DIRECT: {
        uint32_t i;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_USER)) {
            return SHADPS4_NP_WEBAPI_ERROR_USER_NOT_FOUND;
        }
        for (i = 1; i < SHADPS4_HLE_MAX_SERVICE_OBJECTS; i++) {
            if (hle->service_objects[i] == SHADPS4_SERVICE_WEBAPI_CALLBACK &&
                hle->service_parents[i] == a0 &&
                hle->service_value[i] == 0) {
                shadps4_hle_service_delete(
                    hle, 0x400 + i, SHADPS4_SERVICE_WEBAPI_CALLBACK);
            }
        }
        return 0;
    }
    case SHADPS4_HLE_WEBAPI_SET_HANDLE_TIMEOUT:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_WEBAPI_CONTEXT) ||
            !shadps4_hle_service_is(hle, a1,
                                    SHADPS4_SERVICE_WEBAPI_HANDLE) ||
            hle->service_parents[a1 - 0x400] != a0) {
            return SHADPS4_NP_WEBAPI_ERROR_CONTEXT_NOT_FOUND;
        }
        hle->service_value[a1 - 0x400] = a2;
        return 0;
    case SHADPS4_HLE_WEBAPI_INTERNAL_INIT: {
        uint64_t args[4];
        int handle;

        if (!a0 || !shadps4_guest_rw(cs, a0, args, sizeof(args), false) ||
            ldq_le_p((uint8_t *)args + 24) != sizeof(args) ||
            !shadps4_hle_service_is(hle, ldl_le_p(args),
                                    SHADPS4_SERVICE_HTTP_CONTEXT)) {
            return SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_WEBAPI_CONTEXT,
            ldl_le_p(args));
        if (handle >= 0) {
            hle->service_value[handle - 0x400] = le64_to_cpu(args[1]);
        }
        return handle;
    }
    case SHADPS4_HLE_WEBAPI_INTERNAL_CREATE_REQUEST: {
        static const char *const methods[] = {
            "GET", "POST", "PUT", "DELETE", "PATCH",
        };
        uint64_t content_length = 0;

        if (a3 >= ARRAY_SIZE(methods)) {
            return SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        }
        if (a4) {
            uint64_t length_le;
            uint64_t type_le;

            if (!shadps4_guest_rw(cs, a4, &length_le, sizeof(length_le),
                                  false) ||
                !shadps4_guest_rw(cs, a4 + 8, &type_le, sizeof(type_le),
                                  false)) {
                return SHADPS4_NP_WEBAPI_ERROR_INVALID_CONTENT;
            }
            content_length = le64_to_cpu(length_le);
            if (content_length && !le64_to_cpu(type_le)) {
                return SHADPS4_NP_WEBAPI_ERROR_INVALID_CONTENT;
            }
        }
        return shadps4_hle_webapi_create_request(
            hle, cs, a0, a1, a2, methods[a3], content_length, a6,
            SHADPS4_NP_WEBAPI_ERROR_INVALID_ARGUMENT);
    }
    case SHADPS4_HLE_WEBAPI_PARSE_NP_ID: {
        g_autofree guchar *decoded = NULL;
        char encoded[256];
        uint8_t np_id[36] = { 0 };
        gsize decoded_size = 0;
        gsize handle_size;
        gsize opt_size = 0;
        gsize i;
        guchar *separator;

        if (!a0 || !a1 ||
            !shadps4_guest_read_string(cs, a0, encoded, sizeof(encoded))) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        decoded = g_base64_decode(encoded, &decoded_size);
        if (!decoded || !decoded_size) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        separator = memchr(decoded, '@', decoded_size);
        handle_size = separator ? separator - decoded : decoded_size;
        if (!handle_size || handle_size > 16) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        memcpy(np_id, decoded, handle_size);
        if (separator) {
            for (i = separator - decoded + 1;
                 i < decoded_size && opt_size < 8; i++) {
                if (decoded[i] != '/' && decoded[i] != '.') {
                    np_id[20 + opt_size++] = decoded[i];
                }
            }
        }
        np_id[28] = 1;
        return shadps4_guest_rw(cs, a1, np_id, sizeof(np_id), true) ? 0 :
               SHADPS4_NP_ERROR_INVALID_ARGUMENT;
    }
    case SHADPS4_HLE_NP_MATCHING_INIT: {
        uint64_t size;

        if (hle->np_matching_initialized) {
            return SHADPS4_NP_MATCHING_ERROR_ALREADY_INITIALIZED;
        }
        if (!a0 || !shadps4_guest_rw(cs, a0, &size, sizeof(size), false) ||
            (le64_to_cpu(size) != 0x28 && le64_to_cpu(size) != 0x30)) {
            return SHADPS4_NP_MATCHING_ERROR_INVALID_ARGUMENT;
        }
        hle->np_matching_initialized = true;
        return 0;
    }
    case SHADPS4_HLE_NP_MATCHING_TERM: {
        uint32_t i;

        if (!hle->np_matching_initialized) {
            return SHADPS4_NP_MATCHING_ERROR_NOT_INITIALIZED;
        }
        for (i = 1; i < SHADPS4_HLE_MAX_SERVICE_OBJECTS; i++) {
            if (hle->service_objects[i] ==
                SHADPS4_SERVICE_NP_MATCHING_CONTEXT) {
                shadps4_hle_service_delete(
                    hle, 0x400 + i, SHADPS4_SERVICE_NP_MATCHING_CONTEXT);
            }
        }
        hle->np_matching_initialized = false;
        return 0;
    }
    case SHADPS4_HLE_NP_MATCHING_CREATE_CONTEXT: {
        int handle;
        uint16_t context;

        if (!hle->np_matching_initialized) {
            return SHADPS4_NP_MATCHING_ERROR_NOT_INITIALIZED;
        }
        if (!a0 || !a1) {
            return SHADPS4_NP_MATCHING_ERROR_INVALID_ARGUMENT;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_NP_MATCHING_CONTEXT, 0);
        if (handle < 0) {
            return SHADPS4_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
        }
        context = cpu_to_le16(handle);
        if (!shadps4_guest_rw(cs, a1, &context, sizeof(context), true)) {
            shadps4_hle_service_delete(
                hle, handle, SHADPS4_SERVICE_NP_MATCHING_CONTEXT);
            return SHADPS4_NP_MATCHING_ERROR_INVALID_ARGUMENT;
        }
        return 0;
    }
    case SHADPS4_HLE_NP_MATCHING_DESTROY_CONTEXT:
        if (!hle->np_matching_initialized) {
            return SHADPS4_NP_MATCHING_ERROR_NOT_INITIALIZED;
        }
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_MATCHING_CONTEXT)) {
            return SHADPS4_NP_MATCHING_ERROR_CONTEXT_NOT_FOUND;
        }
        return shadps4_hle_service_delete(
            hle, a0, SHADPS4_SERVICE_NP_MATCHING_CONTEXT);
    case SHADPS4_HLE_NP_MATCHING_START:
        if (!hle->np_matching_initialized) {
            return SHADPS4_NP_MATCHING_ERROR_NOT_INITIALIZED;
        }
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_MATCHING_CONTEXT)) {
            return SHADPS4_NP_MATCHING_ERROR_CONTEXT_NOT_FOUND;
        }
        if (hle->service_active[a0 - 0x400]) {
            return SHADPS4_NP_MATCHING_ERROR_CONTEXT_ALREADY_STARTED;
        }
        return 0x804101e2U;
    case SHADPS4_HLE_NP_MATCHING_STOP:
        if (!hle->np_matching_initialized) {
            return SHADPS4_NP_MATCHING_ERROR_NOT_INITIALIZED;
        }
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_MATCHING_CONTEXT)) {
            return SHADPS4_NP_MATCHING_ERROR_CONTEXT_NOT_FOUND;
        }
        if (!hle->service_active[a0 - 0x400]) {
            return SHADPS4_NP_MATCHING_ERROR_CONTEXT_NOT_STARTED;
        }
        hle->service_active[a0 - 0x400] = false;
        return 0;
    case SHADPS4_HLE_NP_MATCHING_REGISTER_CALLBACK:
        if (!hle->np_matching_initialized) {
            return SHADPS4_NP_MATCHING_ERROR_NOT_INITIALIZED;
        }
        if (a0 && shadps4_hle_service_is(
                hle, a0, SHADPS4_SERVICE_NP_MATCHING_CONTEXT)) {
            hle->service_user_data[a0 - 0x400] = a1;
            hle->service_aux_value[a0 - 0x400] = a2;
            return 0;
        }
        if (a0 && !shadps4_hle_service_is(
                hle, a0, SHADPS4_SERVICE_NP_MATCHING_CONTEXT)) {
            return 0;
        }
        return SHADPS4_NP_MATCHING_ERROR_INVALID_ARGUMENT;
    case SHADPS4_HLE_NP_MATCHING_GET_SERVER: {
        uint16_t server = cpu_to_le16(1);

        if (!hle->np_matching_initialized) {
            return SHADPS4_NP_MATCHING_ERROR_NOT_INITIALIZED;
        }
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_MATCHING_CONTEXT) ||
            !a1) {
            return SHADPS4_NP_MATCHING_ERROR_INVALID_ARGUMENT;
        }
        return shadps4_guest_rw(cs, a1, &server, sizeof(server), true) ? 0 :
               SHADPS4_NP_MATCHING_ERROR_INVALID_ARGUMENT;
    }
    case SHADPS4_HLE_NP_MATCHING_MEMORY:
        return hle->np_matching_initialized ? 0 :
               SHADPS4_NP_MATCHING_ERROR_NOT_INITIALIZED;
    case SHADPS4_HLE_NP_MATCHING_OFFLINE:
        return hle->np_matching_initialized ?
               SHADPS4_NP_MATCHING_ERROR_SERVER_NOT_AVAILABLE :
               SHADPS4_NP_MATCHING_ERROR_NOT_INITIALIZED;
    case SHADPS4_HLE_NP_SCORE_CREATE_TITLE: {
        int handle;

        if ((uint8_t)a0 == 0xff) {
            return SHADPS4_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
        }
        if (!a1) {
            return SHADPS4_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_NP_SCORE_TITLE, 0);
        if (handle >= 0) {
            hle->service_value[handle - 0x400] = a0;
        }
        return handle < 0 ? SHADPS4_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS :
               handle;
    }
    case SHADPS4_HLE_NP_SCORE_CREATE_TITLE_USER:
        if ((uint8_t)a0 == 0xff) {
            return SHADPS4_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
        }
        if ((int32_t)a1 == -1) {
            return SHADPS4_NP_COMMUNITY_ERROR_NO_LOGIN;
        }
        return shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_NP_SCORE_TITLE, 0);
    case SHADPS4_HLE_NP_SCORE_CREATE_REQUEST:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_SCORE_TITLE)) {
            return SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
        }
        return shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_NP_SCORE_REQUEST, a0);
    case SHADPS4_HLE_NP_SCORE_DELETE_TITLE:
        return shadps4_hle_service_is(
            hle, a0, SHADPS4_SERVICE_NP_SCORE_TITLE) ?
            shadps4_hle_service_delete(
                hle, a0, SHADPS4_SERVICE_NP_SCORE_TITLE) :
            SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
    case SHADPS4_HLE_NP_SCORE_DELETE_REQUEST:
        return shadps4_hle_service_is(
            hle, a0, SHADPS4_SERVICE_NP_SCORE_REQUEST) ?
            shadps4_hle_service_delete(
                hle, a0, SHADPS4_SERVICE_NP_SCORE_REQUEST) :
            SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
    case SHADPS4_HLE_NP_SCORE_ABORT:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_SCORE_REQUEST)) {
            return SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
        }
        hle->service_active[a0 - 0x400] = false;
        hle->service_value[a0 - 0x400] =
            SHADPS4_NP_COMMUNITY_ERROR_ABORTED;
        return 0;
    case SHADPS4_HLE_NP_SCORE_POLL:
    case SHADPS4_HLE_NP_SCORE_WAIT: {
        uint32_t result;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_SCORE_REQUEST)) {
            return SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
        }
        if (number == SHADPS4_HLE_NP_SCORE_POLL &&
            hle->service_active[a0 - 0x400]) {
            return 1;
        }
        result = cpu_to_le32(hle->service_value[a0 - 0x400]);
        return !a1 || shadps4_guest_rw(cs, a1, &result, sizeof(result), true) ?
               0 : SHADPS4_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    case SHADPS4_HLE_NP_SCORE_SET_PC_ID:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_SCORE_TITLE)) {
            return SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
        }
        hle->service_aux_value[a0 - 0x400] = a1;
        return 0;
    case SHADPS4_HLE_NP_SCORE_OFFLINE:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_SCORE_REQUEST)) {
            return SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
        }
        hle->service_active[a0 - 0x400] = false;
        hle->service_value[a0 - 0x400] =
            SHADPS4_NP_COMMUNITY_ERROR_NO_LOGIN;
        return SHADPS4_NP_COMMUNITY_ERROR_NO_LOGIN;
    case SHADPS4_HLE_NP_TUS_CREATE_TITLE:
        if (!a1) {
            return SHADPS4_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT;
        }
        if ((uint8_t)a0 == 0xff) {
            return SHADPS4_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
        }
        return shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_NP_TUS_TITLE, 0);
    case SHADPS4_HLE_NP_TUS_CREATE_TITLE_USER:
        if ((uint8_t)a0 == 0xff || (int32_t)a1 == -1) {
            return SHADPS4_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
        }
        return shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_NP_TUS_TITLE, 0);
    case SHADPS4_HLE_NP_TUS_CREATE_REQUEST:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_TUS_TITLE)) {
            return SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
        }
        return shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_NP_TUS_REQUEST, a0);
    case SHADPS4_HLE_NP_TUS_DELETE_TITLE:
        return shadps4_hle_service_is(
            hle, a0, SHADPS4_SERVICE_NP_TUS_TITLE) ?
            shadps4_hle_service_delete(
                hle, a0, SHADPS4_SERVICE_NP_TUS_TITLE) :
            SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
    case SHADPS4_HLE_NP_TUS_DELETE_REQUEST:
        return shadps4_hle_service_is(
            hle, a0, SHADPS4_SERVICE_NP_TUS_REQUEST) ?
            shadps4_hle_service_delete(
                hle, a0, SHADPS4_SERVICE_NP_TUS_REQUEST) :
            SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
    case SHADPS4_HLE_NP_TUS_DATA:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_TUS_REQUEST)) {
            return SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
        }
        hle->service_active[a0 - 0x400] = false;
        hle->service_value[a0 - 0x400] = 0;
        return 0;
    case SHADPS4_HLE_NP_TUS_POLL:
    case SHADPS4_HLE_NP_TUS_WAIT: {
        uint32_t result = 0;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_TUS_REQUEST)) {
            return SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
        }
        if (number == SHADPS4_HLE_NP_TUS_POLL &&
            hle->service_active[a0 - 0x400]) {
            return 1;
        }
        return !a1 || shadps4_guest_rw(cs, a1, &result, sizeof(result), true) ?
               0 : SHADPS4_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    case SHADPS4_HLE_NP_TUS_OFFLINE:
        return shadps4_hle_service_is(
            hle, a0, SHADPS4_SERVICE_NP_TUS_REQUEST) ?
            SHADPS4_NP_COMMUNITY_ERROR_NO_LOGIN :
            SHADPS4_NP_COMMUNITY_ERROR_INVALID_ID;
    case SHADPS4_HLE_NP_SIGNALING_INIT:
        if (hle->np_signaling_initialized) {
            return SHADPS4_NP_SIGNALING_ERROR_ALREADY_INITIALIZED;
        }
        hle->np_signaling_initialized = true;
        return 0;
    case SHADPS4_HLE_NP_SIGNALING_TERM: {
        uint32_t i;

        if (!hle->np_signaling_initialized) {
            return SHADPS4_NP_SIGNALING_ERROR_NOT_INITIALIZED;
        }
        for (i = 1; i < SHADPS4_HLE_MAX_SERVICE_OBJECTS; i++) {
            if (hle->service_objects[i] ==
                SHADPS4_SERVICE_NP_SIGNALING_CONTEXT) {
                shadps4_hle_service_delete(
                    hle, 0x400 + i, SHADPS4_SERVICE_NP_SIGNALING_CONTEXT);
            }
        }
        hle->np_signaling_initialized = false;
        return 0;
    }
    case SHADPS4_HLE_NP_SIGNALING_CREATE_CONTEXT:
    case SHADPS4_HLE_NP_SIGNALING_CREATE_CONTEXT_USER: {
        int handle;
        uint32_t context;

        if (!hle->np_signaling_initialized) {
            return SHADPS4_NP_SIGNALING_ERROR_NOT_INITIALIZED;
        }
        if (!a0 || !a3 ||
            (number == SHADPS4_HLE_NP_SIGNALING_CREATE_CONTEXT_USER &&
             (int32_t)a0 == -1)) {
            return SHADPS4_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_NP_SIGNALING_CONTEXT, 0);
        if (handle < 0) {
            return SHADPS4_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
        }
        hle->service_user_data[handle - 0x400] = a1;
        hle->service_aux_value[handle - 0x400] = a2;
        context = cpu_to_le32(handle);
        if (!shadps4_guest_rw(cs, a3, &context, sizeof(context), true)) {
            shadps4_hle_service_delete(
                hle, handle, SHADPS4_SERVICE_NP_SIGNALING_CONTEXT);
            return SHADPS4_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
        }
        return 0;
    }
    case SHADPS4_HLE_NP_SIGNALING_DELETE_CONTEXT:
        if (!hle->np_signaling_initialized) {
            return SHADPS4_NP_SIGNALING_ERROR_NOT_INITIALIZED;
        }
        return shadps4_hle_service_is(
            hle, a0, SHADPS4_SERVICE_NP_SIGNALING_CONTEXT) ?
            shadps4_hle_service_delete(
                hle, a0, SHADPS4_SERVICE_NP_SIGNALING_CONTEXT) :
            SHADPS4_NP_SIGNALING_ERROR_CONTEXT_NOT_FOUND;
    case SHADPS4_HLE_NP_SIGNALING_SET_OPTION:
        if (!hle->np_signaling_initialized) {
            return SHADPS4_NP_SIGNALING_ERROR_NOT_INITIALIZED;
        }
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_SIGNALING_CONTEXT) ||
            a1 != 1 || a2 > 1) {
            return SHADPS4_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
        }
        hle->service_value[a0 - 0x400] = a2;
        return 0;
    case SHADPS4_HLE_NP_SIGNALING_GET_OPTION: {
        uint32_t option;

        if (!hle->np_signaling_initialized) {
            return SHADPS4_NP_SIGNALING_ERROR_NOT_INITIALIZED;
        }
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_SIGNALING_CONTEXT) ||
            a1 != 1 || !a2) {
            return SHADPS4_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
        }
        option = cpu_to_le32(hle->service_value[a0 - 0x400]);
        return shadps4_guest_rw(cs, a2, &option, sizeof(option), true) ? 0 :
               SHADPS4_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    }
    case SHADPS4_HLE_NP_SIGNALING_MEMORY: {
        uint64_t info[3] = { 0, 0, cpu_to_le64(0x20000) };

        if (!hle->np_signaling_initialized) {
            return SHADPS4_NP_SIGNALING_ERROR_NOT_INITIALIZED;
        }
        return a0 && shadps4_guest_rw(cs, a0, info, sizeof(info), true) ? 0 :
               SHADPS4_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    }
    case SHADPS4_HLE_NP_SIGNALING_OFFLINE:
        if (!hle->np_signaling_initialized) {
            return SHADPS4_NP_SIGNALING_ERROR_NOT_INITIALIZED;
        }
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NP_SIGNALING_CONTEXT)) {
            return SHADPS4_NP_SIGNALING_ERROR_CONTEXT_NOT_FOUND;
        }
        return SHADPS4_NP_SIGNALING_ERROR_CONNECTION_NOT_FOUND;
    case SHADPS4_HLE_AJM_INITIALIZE: {
        int handle;
        uint32_t handle_le;

        if (a0 || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_AJM_CONTEXT, 0);
        if (handle < 0) {
            return handle;
        }
        handle_le = cpu_to_le32(handle);
        return shadps4_guest_rw(cs, a1, &handle_le,
                                sizeof(handle_le), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_AJM_FINALIZE: {
        uint32_t i;

        for (i = 1; i < SHADPS4_HLE_MAX_SERVICE_OBJECTS; i++) {
            if (hle->service_objects[i] == SHADPS4_SERVICE_AJM_CONTEXT) {
                shadps4_hle_service_delete(
                    hle, 0x400 + i, SHADPS4_SERVICE_AJM_CONTEXT);
            }
        }
        return 0;
    }
    case SHADPS4_HLE_AJM_MODULE_REGISTER:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AJM_CONTEXT) || a2) {
            return -SHADPS4_GUEST_EINVAL;
        }
        hle->service_value[a0 - 0x400] |= 1ULL << (a1 & 31);
        return 0;
    case SHADPS4_HLE_AJM_INSTANCE_CREATE: {
        int handle;
        uint32_t handle_le;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AJM_CONTEXT) || !a3 ||
            !(hle->service_value[a0 - 0x400] & (1ULL << (a1 & 31)))) {
            return -SHADPS4_GUEST_EINVAL;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_AJM_INSTANCE, a0);
        if (handle < 0) {
            return handle;
        }
        hle->service_value[handle - 0x400] = a1;
        handle_le = cpu_to_le32(handle);
        return shadps4_guest_rw(cs, a3, &handle_le,
                                sizeof(handle_le), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_AJM_INSTANCE_DESTROY:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AJM_CONTEXT) ||
            !shadps4_hle_service_is(hle, a1,
                                    SHADPS4_SERVICE_AJM_INSTANCE) ||
            hle->service_parents[a1 - 0x400] != a0) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_hle_service_delete(
            hle, a1, SHADPS4_SERVICE_AJM_INSTANCE);
    case SHADPS4_HLE_AJM_INSTANCE_CODEC_TYPE:
        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_AJM_INSTANCE) ?
               hle->service_value[a0 - 0x400] : 0;
    case SHADPS4_HLE_AJM_MEMORY:
        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_AJM_CONTEXT) && a1 ?
               0 : -SHADPS4_GUEST_EINVAL;
    case SHADPS4_HLE_AJM_BATCH_START: {
        uint8_t header[8];
        uint32_t handle_le;
        int handle;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AJM_CONTEXT) || !a1 ||
            a2 < sizeof(header) || a2 > 16 * MiB || !a5 ||
            !shadps4_guest_rw(cs, a1, header, sizeof(header), false)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_AJM_BATCH, a0);
        if (handle < 0) {
            return handle;
        }
        hle->service_user_data[handle - 0x400] = a1;
        hle->service_value[handle - 0x400] = a2;
        handle_le = cpu_to_le32(handle);
        if (a4) {
            uint8_t error[32] = { 0 };

            if (!shadps4_guest_rw(cs, a4, error, sizeof(error), true)) {
                shadps4_hle_service_delete(
                    hle, handle, SHADPS4_SERVICE_AJM_BATCH);
                return -SHADPS4_GUEST_EFAULT;
            }
        }
        if (!shadps4_guest_rw(cs, a5, &handle_le,
                              sizeof(handle_le), true)) {
            shadps4_hle_service_delete(
                hle, handle, SHADPS4_SERVICE_AJM_BATCH);
            return -SHADPS4_GUEST_EFAULT;
        }
        return 0;
    }
    case SHADPS4_HLE_AJM_BATCH_WAIT:
    case SHADPS4_HLE_AJM_BATCH_CANCEL:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AJM_CONTEXT) ||
            !shadps4_hle_service_is(hle, a1,
                                    SHADPS4_SERVICE_AJM_BATCH) ||
            hle->service_parents[a1 - 0x400] != a0) {
            return -SHADPS4_GUEST_EINVAL;
        }
        if (number == SHADPS4_HLE_AJM_BATCH_WAIT && a3) {
            uint8_t error[32] = { 0 };

            if (!shadps4_guest_rw(cs, a3, error, sizeof(error), true)) {
                return -SHADPS4_GUEST_EFAULT;
            }
        }
        return shadps4_hle_service_delete(
            hle, a1, SHADPS4_SERVICE_AJM_BATCH);
    case SHADPS4_HLE_AJM_JOB_CONTROL:
    case SHADPS4_HLE_AJM_JOB_INLINE:
    case SHADPS4_HLE_AJM_JOB_RUN:
    case SHADPS4_HLE_AJM_JOB_RUN_SPLIT:
        return shadps4_hle_ajm_job(cs, number, a0, a1, a2, a3, a4, a5);
    case SHADPS4_HLE_AJM_MP3_PARSE: {
        static const uint32_t rates[4][4] = {
            { 11025, 12000, 8000, 0 }, { 0 },
            { 22050, 24000, 16000, 0 },
            { 44100, 48000, 32000, 0 },
        };
        static const uint16_t bitrates[4][16] = {
            { 0, 8, 16, 24, 32, 40, 48, 56, 64 }, { 0 },
            { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
              144, 160, 0 },
            { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192,
              224, 256, 320, 0 },
        };
        uint8_t bytes[4];
        uint8_t frame[40] = { 0 };
        uint32_t header;
        uint32_t version;
        uint32_t bitrate;
        uint32_t rate;
        uint32_t channels;
        uint32_t samples;
        uint64_t frame_size;

        if (!a0 || a1 < sizeof(bytes) || !a3 ||
            !shadps4_guest_rw(cs, a0, bytes, sizeof(bytes), false)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        header = ldl_be_p(bytes);
        version = (header >> 19) & 3;
        bitrate = bitrates[version][(header >> 12) & 15] * 1000;
        rate = rates[version][(header >> 10) & 3];
        if ((header >> 21) != 0x7ff || ((header >> 17) & 3) != 1 ||
            !bitrate || !rate) {
            return -SHADPS4_GUEST_EINVAL;
        }
        channels = ((header >> 6) & 3) == 3 ? 1 : 2;
        samples = version == 3 ? 1152 : 576;
        frame_size = ((version == 3 ? 144 : 72) * bitrate) / rate +
                     ((header >> 9) & 1);
        frame_size = cpu_to_le64(frame_size);
        channels = cpu_to_le32(channels);
        samples = cpu_to_le32(samples);
        bitrate = cpu_to_le32(bitrate);
        rate = cpu_to_le32(rate);
        memcpy(frame, &frame_size, sizeof(frame_size));
        memcpy(frame + 8, &channels, sizeof(channels));
        memcpy(frame + 12, &samples, sizeof(samples));
        memcpy(frame + 16, &bitrate, sizeof(bitrate));
        memcpy(frame + 20, &rate, sizeof(rate));
        return shadps4_guest_rw(cs, a3, frame, sizeof(frame), true) ? 0 :
               -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_AJM_UNSUPPORTED:
        return -SHADPS4_GUEST_ENOSYS;
    case SHADPS4_HLE_NGS2_SYSTEM_CREATE:
    case SHADPS4_HLE_NGS2_RACK_CREATE: {
        uint8_t type = number == SHADPS4_HLE_NGS2_SYSTEM_CREATE ?
                       SHADPS4_SERVICE_NGS2_SYSTEM :
                       SHADPS4_SERVICE_NGS2_RACK;
        uint64_t parent = type == SHADPS4_SERVICE_NGS2_RACK ? a0 : 0;
        uint64_t out = type == SHADPS4_SERVICE_NGS2_RACK ? a4 : a2;
        int handle;

        if (!out || (parent && !shadps4_hle_service_is(
                         hle, parent, SHADPS4_SERVICE_NGS2_SYSTEM))) {
            return -SHADPS4_GUEST_EINVAL;
        }
        handle = shadps4_hle_service_alloc(hle, type, parent);
        if (handle >= 0 && type == SHADPS4_SERVICE_NGS2_SYSTEM) {
            hle->service_value[handle - 0x400] =
                ((uint64_t)256 << 32) | 48000;
        } else if (handle >= 0) {
            hle->service_value[handle - 0x400] = a1;
        }
        return handle < 0 ? handle : shadps4_hle_write_u64(cs, out, handle);
    }
    case SHADPS4_HLE_NGS2_SYSTEM_DESTROY:
        return shadps4_hle_service_delete(
            hle, a0, SHADPS4_SERVICE_NGS2_SYSTEM);
    case SHADPS4_HLE_NGS2_RACK_DESTROY:
        return shadps4_hle_service_delete(
            hle, a0, SHADPS4_SERVICE_NGS2_RACK);
    case SHADPS4_HLE_NGS2_SYSTEM_USER_GET:
    case SHADPS4_HLE_NGS2_RACK_USER_GET: {
        uint8_t type = number == SHADPS4_HLE_NGS2_SYSTEM_USER_GET ?
                       SHADPS4_SERVICE_NGS2_SYSTEM :
                       SHADPS4_SERVICE_NGS2_RACK;

        return shadps4_hle_service_is(hle, a0, type) ?
               shadps4_hle_write_u64(
                   cs, a1, hle->service_user_data[a0 - 0x400]) :
               -SHADPS4_GUEST_EBADF;
    }
    case SHADPS4_HLE_NGS2_SYSTEM_USER_SET:
    case SHADPS4_HLE_NGS2_RACK_USER_SET: {
        uint8_t type = number == SHADPS4_HLE_NGS2_SYSTEM_USER_SET ?
                       SHADPS4_SERVICE_NGS2_SYSTEM :
                       SHADPS4_SERVICE_NGS2_RACK;

        if (!shadps4_hle_service_is(hle, a0, type)) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->service_user_data[a0 - 0x400] = a1;
        return 0;
    }
    case SHADPS4_HLE_NGS2_SYSTEM_LOCK:
    case SHADPS4_HLE_NGS2_SYSTEM_UNLOCK:
    case SHADPS4_HLE_NGS2_RACK_LOCK:
    case SHADPS4_HLE_NGS2_RACK_UNLOCK: {
        bool system = number == SHADPS4_HLE_NGS2_SYSTEM_LOCK ||
                      number == SHADPS4_HLE_NGS2_SYSTEM_UNLOCK;
        bool lock = number == SHADPS4_HLE_NGS2_SYSTEM_LOCK ||
                    number == SHADPS4_HLE_NGS2_RACK_LOCK;
        uint8_t type = system ? SHADPS4_SERVICE_NGS2_SYSTEM :
                       SHADPS4_SERVICE_NGS2_RACK;

        if (!shadps4_hle_service_is(hle, a0, type)) {
            return -SHADPS4_GUEST_EBADF;
        }
        if (hle->service_active[a0 - 0x400] == lock) {
            return -SHADPS4_GUEST_EINVAL;
        }
        hle->service_active[a0 - 0x400] = lock;
        return 0;
    }
    case SHADPS4_HLE_NGS2_SYSTEM_RENDER:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NGS2_SYSTEM) ||
            (a2 && !a1) || a2 > 64) {
            return -SHADPS4_GUEST_EINVAL;
        }
        for (uint32_t i = 0; i < a2; i++) {
            uint64_t buffer_info[3];
            g_autofree uint8_t *silence = NULL;
            uint64_t address;
            uint64_t size;

            if (!shadps4_guest_rw(cs, a1 + i * sizeof(buffer_info),
                                  buffer_info, sizeof(buffer_info), false)) {
                return -SHADPS4_GUEST_EFAULT;
            }
            address = le64_to_cpu(buffer_info[0]);
            size = le64_to_cpu(buffer_info[1]);
            if ((!address && size) || size > 64 * MiB) {
                return -SHADPS4_GUEST_EINVAL;
            }
            silence = g_malloc0(size ? size : 1);
            if (size && !shadps4_guest_rw(cs, address, silence, size, true)) {
                return -SHADPS4_GUEST_EFAULT;
            }
        }
        return 0;
    case SHADPS4_HLE_NGS2_SYSTEM_ENUM:
    case SHADPS4_HLE_NGS2_SYSTEM_ENUM_RACKS: {
        uint8_t type = number == SHADPS4_HLE_NGS2_SYSTEM_ENUM ?
                       SHADPS4_SERVICE_NGS2_SYSTEM :
                       SHADPS4_SERVICE_NGS2_RACK;
        uint32_t count = 0;
        uint32_t i;

        if (number == SHADPS4_HLE_NGS2_SYSTEM_ENUM_RACKS &&
            !shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NGS2_SYSTEM)) {
            return -SHADPS4_GUEST_EBADF;
        }
        for (i = 0; i < SHADPS4_HLE_MAX_SERVICE_OBJECTS; i++) {
            uint64_t handle = cpu_to_le64(0x400 + i);

            if (hle->service_objects[i] != type ||
                (type == SHADPS4_SERVICE_NGS2_RACK &&
                 hle->service_parents[i] != a0)) {
                continue;
            }
            if (count < (number == SHADPS4_HLE_NGS2_SYSTEM_ENUM ? a1 : a2)) {
                uint64_t output = number == SHADPS4_HLE_NGS2_SYSTEM_ENUM ?
                                  a0 : a1;

                if (!output || !shadps4_guest_rw(
                        cs, output + count * sizeof(handle), &handle,
                        sizeof(handle), true)) {
                    return -SHADPS4_GUEST_EFAULT;
                }
            }
            count++;
        }
        return count;
    }
    case SHADPS4_HLE_NGS2_SYSTEM_INFO:
    case SHADPS4_HLE_NGS2_RACK_INFO: {
        bool system = number == SHADPS4_HLE_NGS2_SYSTEM_INFO;
        uint8_t type = system ? SHADPS4_SERVICE_NGS2_SYSTEM :
                       SHADPS4_SERVICE_NGS2_RACK;
        size_t expected = system ? 136 : 168;
        g_autofree uint8_t *info = NULL;
        uint64_t handle;

        if (!shadps4_hle_service_is(hle, a0, type) || !a1 ||
            a2 < expected || a2 > 64 * KiB) {
            return -SHADPS4_GUEST_EINVAL;
        }
        info = g_malloc0(a2);
        handle = cpu_to_le64(a0);
        memcpy(info + 16, &handle, sizeof(handle));
        if (system) {
            uint32_t rack_count = 0;
            uint32_t sample_rate = cpu_to_le32(
                hle->service_value[a0 - 0x400] & 0xffffffffU);
            uint32_t grain = cpu_to_le32(
                hle->service_value[a0 - 0x400] >> 32);

            for (uint32_t i = 0; i < SHADPS4_HLE_MAX_SERVICE_OBJECTS; i++) {
                rack_count += hle->service_objects[i] ==
                                  SHADPS4_SERVICE_NGS2_RACK &&
                              hle->service_parents[i] == a0;
            }
            rack_count = cpu_to_le32(rack_count);
            memcpy(info + 104, &rack_count, sizeof(rack_count));
            memcpy(info + 128, &sample_rate, sizeof(sample_rate));
            memcpy(info + 132, &grain, sizeof(grain));
        } else {
            uint64_t parent = cpu_to_le64(
                hle->service_parents[a0 - 0x400]);

            memcpy(info + 88, &parent, sizeof(parent));
        }
        return shadps4_guest_rw(cs, a1, info, a2, true) ? 0 :
               -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_NGS2_SYSTEM_SET_SAMPLE_RATE:
    case SHADPS4_HLE_NGS2_SYSTEM_SET_GRAIN:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_NGS2_SYSTEM) || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        if (number == SHADPS4_HLE_NGS2_SYSTEM_SET_SAMPLE_RATE) {
            hle->service_value[a0 - 0x400] =
                (hle->service_value[a0 - 0x400] & 0xffffffff00000000ULL) |
                (uint32_t)a1;
        } else {
            hle->service_value[a0 - 0x400] =
                (hle->service_value[a0 - 0x400] & 0xffffffffULL) |
                ((uint64_t)(uint32_t)a1 << 32);
        }
        return 0;
    case SHADPS4_HLE_NGS2_SYSTEM_RESET_OPTION: {
        uint8_t option[64] = { 0 };
        uint64_t option_size = cpu_to_le64(sizeof(option));
        uint32_t max_grain = cpu_to_le32(512);
        uint32_t grain = cpu_to_le32(256);
        uint32_t sample_rate = cpu_to_le32(48000);

        if (!a0) {
            return -SHADPS4_GUEST_EFAULT;
        }
        memcpy(option, &option_size, sizeof(option_size));
        memcpy(option + 28, &max_grain, sizeof(max_grain));
        memcpy(option + 32, &grain, sizeof(grain));
        memcpy(option + 36, &sample_rate, sizeof(sample_rate));
        return shadps4_guest_rw(cs, a0, option, sizeof(option), true) ? 0 :
               -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_NGS2_RACK_QUERY_SIZE: {
        uint64_t info[8] = { 0 };

        if (!a2) {
            return -SHADPS4_GUEST_EINVAL;
        }
        info[1] = cpu_to_le64(8);
        return shadps4_guest_rw(cs, a2, info, sizeof(info), true) ? 0 :
               -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_NGS2_REPORT_REGISTER: {
        int handle;

        if (!a1 || !a3) {
            return -SHADPS4_GUEST_EINVAL;
        }
        handle = shadps4_hle_service_alloc(
            hle, SHADPS4_SERVICE_NGS2_REPORT, a0);
        return handle < 0 ? handle : shadps4_hle_write_u64(cs, a3, handle);
    }
    case SHADPS4_HLE_NGS2_REPORT_UNREGISTER:
        return shadps4_hle_service_delete(
            hle, a0, SHADPS4_SERVICE_NGS2_REPORT);
    case SHADPS4_HLE_NGS2_SYSTEM_QUERY_SIZE: {
        uint64_t info[8] = { 0 };

        if (!a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        info[1] = cpu_to_le64(8);
        return shadps4_guest_rw(cs, a1, info, sizeof(info), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_NGS2_UNSUPPORTED:
        return -SHADPS4_GUEST_ENOSYS;
    case SHADPS4_HLE_AVPLAYER_INIT: {
        if (!a0) {
            return 0;
        }
        return shadps4_hle_service_alloc(hle, SHADPS4_SERVICE_AVPLAYER, 0);
    }
    case SHADPS4_HLE_AVPLAYER_INIT_EX: {
        int handle;

        if (!a0 || !a1) {
            return -SHADPS4_GUEST_EINVAL;
        }
        handle = shadps4_hle_service_alloc(hle,
                                           SHADPS4_SERVICE_AVPLAYER, 0);
        return handle < 0 ? handle : shadps4_hle_write_u64(cs, a1, handle);
    }
    case SHADPS4_HLE_AVPLAYER_CLOSE:
        return shadps4_hle_service_delete(hle, a0,
                                           SHADPS4_SERVICE_AVPLAYER);
    case SHADPS4_HLE_AVPLAYER_ADD_SOURCE: {
        char source[2048];

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AVPLAYER) ||
            !shadps4_guest_read_string(cs, a1, source, sizeof(source))) {
            return -SHADPS4_GUEST_EINVAL;
        }
        return shadps4_hle_avplayer_open(hle, a0, source);
    }
    case SHADPS4_HLE_AVPLAYER_ADD_SOURCE_EX:
    {
        g_autofree char *source = NULL;
        uint8_t uri[16];
        uint64_t source_address;
        uint32_t source_length;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AVPLAYER) || a1 || !a2 ||
            !shadps4_guest_rw(cs, a2, uri, sizeof(uri), false)) {
            return -SHADPS4_GUEST_EINVAL;
        }
        source_address = ldq_le_p(uri);
        source_length = ldl_le_p(uri + 8);
        if (!source_address || !source_length || source_length > 2047) {
            return -SHADPS4_GUEST_EINVAL;
        }
        source = g_malloc(source_length + 1);
        if (!shadps4_guest_rw(cs, source_address, source, source_length,
                              false)) {
            return -SHADPS4_GUEST_EFAULT;
        }
        source[source_length] = '\0';
        return shadps4_hle_avplayer_open(hle, a0, source);
    }
    case SHADPS4_HLE_AVPLAYER_START:
    case SHADPS4_HLE_AVPLAYER_STOP:
    case SHADPS4_HLE_AVPLAYER_PAUSE:
    case SHADPS4_HLE_AVPLAYER_RESUME:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AVPLAYER)) {
            return -SHADPS4_GUEST_EBADF;
        }
        if (number == SHADPS4_HLE_AVPLAYER_START &&
            !hle->service_value[a0 - 0x400]) {
            return -SHADPS4_GUEST_EINVAL;
        }
        {
            QemuHostMediaControl control = QEMU_HOST_MEDIA_START;
            int ret;

            if (hle->service_host_handles[a0 - 0x400] < 0) {
                return -SHADPS4_GUEST_EINVAL;
            }
            if (number == SHADPS4_HLE_AVPLAYER_STOP) {
                control = QEMU_HOST_MEDIA_STOP;
            } else if (number == SHADPS4_HLE_AVPLAYER_PAUSE) {
                control = QEMU_HOST_MEDIA_PAUSE;
            } else if (number == SHADPS4_HLE_AVPLAYER_RESUME) {
                control = QEMU_HOST_MEDIA_RESUME;
            }
            ret = qemu_host_media_control(
                hle->service_host_handles[a0 - 0x400], control, 0);
            if (ret < 0) {
                return shadps4_hle_host_result(ret);
            }
        }
        hle->service_active[a0 - 0x400] =
            number == SHADPS4_HLE_AVPLAYER_START ||
            number == SHADPS4_HLE_AVPLAYER_RESUME;
        return 0;
    case SHADPS4_HLE_AVPLAYER_JUMP:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AVPLAYER)) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->service_user_data[a0 - 0x400] = a1;
        return shadps4_hle_host_result(qemu_host_media_control(
            hle->service_host_handles[a0 - 0x400], QEMU_HOST_MEDIA_JUMP,
            a1));
    case SHADPS4_HLE_AVPLAYER_SET_LOOPING:
        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AVPLAYER)) {
            return -SHADPS4_GUEST_EBADF;
        }
        hle->service_nonblock[a0 - 0x400] = a1 != 0;
        return shadps4_hle_host_result(qemu_host_media_control(
            hle->service_host_handles[a0 - 0x400],
            QEMU_HOST_MEDIA_SET_LOOPING, a1 != 0));
    case SHADPS4_HLE_AVPLAYER_OPTION:
        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_AVPLAYER) ?
               0 : -SHADPS4_GUEST_EBADF;
    case SHADPS4_HLE_AVPLAYER_CURRENT_TIME: {
        uint64_t time_us;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AVPLAYER) ||
            qemu_host_media_get_time(
                hle->service_host_handles[a0 - 0x400], &time_us) < 0) {
            return 0;
        }
        return time_us / 1000;
    }
    case SHADPS4_HLE_AVPLAYER_IS_ACTIVE:
        return shadps4_hle_service_is(hle, a0,
                                      SHADPS4_SERVICE_AVPLAYER) &&
               hle->service_active[a0 - 0x400];
    case SHADPS4_HLE_AVPLAYER_GET_AUDIO_FRAME:
        return shadps4_hle_avplayer_frame(
            hle, cs, a0, a1, QEMU_HOST_MEDIA_STREAM_AUDIO, false);
    case SHADPS4_HLE_AVPLAYER_GET_VIDEO_FRAME:
        return shadps4_hle_avplayer_frame(
            hle, cs, a0, a1, QEMU_HOST_MEDIA_STREAM_VIDEO, false);
    case SHADPS4_HLE_AVPLAYER_GET_VIDEO_FRAME_EX:
        return shadps4_hle_avplayer_frame(
            hle, cs, a0, a1, QEMU_HOST_MEDIA_STREAM_VIDEO, true);
    case SHADPS4_HLE_AVPLAYER_STREAM_COUNT: {
        uint32_t count;
        int ret;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AVPLAYER)) {
            return -SHADPS4_GUEST_EBADF;
        }
        ret = qemu_host_media_get_stream_count(
            hle->service_host_handles[a0 - 0x400], &count);
        return ret < 0 ? shadps4_hle_host_result(ret) : count;
    }
    case SHADPS4_HLE_AVPLAYER_STREAM_INFO: {
        QemuHostMediaStreamInfo host_info = { 0 };
        uint8_t guest_info[40] = { 0 };
        uint32_t type;
        uint32_t float_bits;
        float aspect;
        int ret;

        if (!shadps4_hle_service_is(hle, a0,
                                    SHADPS4_SERVICE_AVPLAYER) || !a2) {
            return -SHADPS4_GUEST_EINVAL;
        }
        ret = qemu_host_media_get_stream_info(
            hle->service_host_handles[a0 - 0x400], a1, &host_info);
        if (ret < 0) {
            return shadps4_hle_host_result(ret);
        }
        type = host_info.type >= QEMU_HOST_MEDIA_STREAM_VIDEO &&
               host_info.type <= QEMU_HOST_MEDIA_STREAM_TIMED_TEXT ?
               host_info.type - QEMU_HOST_MEDIA_STREAM_VIDEO : 3;
        stl_le_p(guest_info, type);
        if (host_info.type == QEMU_HOST_MEDIA_STREAM_AUDIO) {
            stw_le_p(guest_info + 8, host_info.channels);
            stl_le_p(guest_info + 12, host_info.sample_rate);
        } else if (host_info.type == QEMU_HOST_MEDIA_STREAM_VIDEO) {
            stl_le_p(guest_info + 8, host_info.width);
            stl_le_p(guest_info + 12, host_info.height);
            aspect = host_info.height ?
                     (float)host_info.width / host_info.height : 0.0f;
            memcpy(&float_bits, &aspect, sizeof(float_bits));
            stl_le_p(guest_info + 16, float_bits);
        }
        stq_le_p(guest_info + 24, host_info.duration_us / 1000);
        stq_le_p(guest_info + 32, host_info.start_time_us / 1000);
        return shadps4_guest_rw(cs, a2, guest_info, sizeof(guest_info), true) ?
               0 : -SHADPS4_GUEST_EFAULT;
    }
    case SHADPS4_HLE_AVPLAYER_UNSUPPORTED:
        return -SHADPS4_GUEST_ENOSYS;
    case SHADPS4_HLE_NP_COMMON_CMP_NP_ID:
    case SHADPS4_HLE_NP_COMMON_CMP_NP_ID_ORDER: {
        uint8_t left[36];
        uint8_t right[36];
        int compare;

        if (!a0 || !a1 ||
            !shadps4_guest_rw(cs, a0, left, sizeof(left), false) ||
            !shadps4_guest_rw(cs, a1, right, sizeof(right), false)) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        compare = memcmp(left, right, 16);
        if (!compare) {
            compare = memcmp(left + 20, right + 20, 8);
        }
        if (!compare) {
            compare = memcmp(left + 28, right + 28, 8);
        }
        if (number == SHADPS4_HLE_NP_COMMON_CMP_NP_ID_ORDER) {
            uint32_t result = compare < 0 ? UINT32_MAX : compare > 0 ? 1 : 0;

            return a2 && !shadps4_hle_write_u32(cs, a2, result) ? 0 :
                   SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        return compare ? SHADPS4_NP_UTIL_ERROR_NOT_MATCH : 0;
    }
    case SHADPS4_HLE_NP_COMMON_CMP_ONLINE_ID: {
        uint8_t left[16];
        uint8_t right[16];

        if (!a0 || !a1 ||
            !shadps4_guest_rw(cs, a0, left, sizeof(left), false) ||
            !shadps4_guest_rw(cs, a1, right, sizeof(right), false)) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        return memcmp(left, right, sizeof(left)) ?
               SHADPS4_NP_UTIL_ERROR_NOT_MATCH : 0;
    }
    case SHADPS4_HLE_NP_COMMON_MUTEX_TRYLOCK: {
        uint64_t handle;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_MUTEX, &handle)) {
            return SHADPS4_GUEST_EINVAL;
        }
        if (hle->service_active[handle - 0x400] &&
            hle->service_user_data[handle - 0x400] != 1) {
            return SHADPS4_NP_LW_MUTEX_ERROR_BUSY;
        }
        hle->service_active[handle - 0x400] = true;
        hle->service_user_data[handle - 0x400] = 1;
        hle->service_value[handle - 0x400]++;
        return 0;
    }
    case SHADPS4_HLE_NP_COMMON_THREAD_CREATE: {
        uint64_t result = shadps4_hle_object_create(
            hle, cs, a0, SHADPS4_SERVICE_THREAD, 0);
        uint64_t handle;

        if (result || !shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_THREAD, &handle)) {
            return result ?: SHADPS4_GUEST_EFAULT;
        }
        hle->service_user_data[handle - 0x400] = a1;
        hle->service_value[handle - 0x400] = a2;
        hle->service_active[handle - 0x400] = false;
        return 0;
    }
    case SHADPS4_HLE_NP_COMMON_VALID_ONLINE_ID: {
        uint8_t id[20];
        size_t length = 0;
        size_t i;

        if (!a0 || !shadps4_guest_rw(cs, a0, id, sizeof(id), false) ||
            id[16]) {
            return 0;
        }
        while (length < 16 && id[length]) {
            length++;
        }
        if (length < 3 ||
            !((id[0] >= '0' && id[0] <= '9') ||
              (id[0] >= 'A' && id[0] <= 'Z') ||
              (id[0] >= 'a' && id[0] <= 'z'))) {
            return 0;
        }
        for (i = 1; i < length; i++) {
            if ((id[i] >= '0' && id[i] <= '9') ||
                (id[i] >= 'A' && id[i] <= 'Z') ||
                (id[i] >= 'a' && id[i] <= 'z') ||
                id[i] == '_' || id[i] == '-') {
                continue;
            }
            return 0;
        }
        return 1;
    }
    case SHADPS4_HLE_NP_COMMON_CLOCK_USEC: {
        uint64_t usec = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);

        return a0 && !shadps4_hle_write_u64(cs, a0, usec) ? 0 :
               SHADPS4_NP_ERROR_INVALID_ARGUMENT;
    }
    case SHADPS4_HLE_NP_COMMON_PLATFORM_TYPE: {
        uint8_t npid[36];
        uint32_t tag;

        if (!a0 || !shadps4_guest_rw(cs, a0, npid, sizeof(npid), false)) {
            return SHADPS4_NP_ERROR_INVALID_ARGUMENT;
        }
        if (!npid[24]) {
            return 0;
        }
        tag = ldl_le_p(npid + 24);
        if (tag == 0x00337370) {
            return 1;
        }
        if (tag == 0x32707370) {
            return 2;
        }
        if (tag == 0x00347370) {
            return 3;
        }
        return SHADPS4_NP_ERROR_INVALID_PLATFORM_TYPE;
    }
    case SHADPS4_HLE_NP_COMMON_COND_TIMEDWAIT: {
        uint64_t cond;
        uint64_t mutex;

        if (!shadps4_hle_object_handle(
                hle, cs, a0, SHADPS4_SERVICE_COND, &cond) ||
            !shadps4_hle_object_handle(
                hle, cs, a1, SHADPS4_SERVICE_MUTEX, &mutex)) {
            return SHADPS4_GUEST_EINVAL;
        }
        return SHADPS4_NP_LW_COND_ERROR_TIMEDOUT;
    }
    case SHADPS4_HLE_NP_COMMON_SDK_VERSION: {
        static const char version[8] = "11.00";

        return a0 && shadps4_guest_rw(
                         cs, a0, (void *)version, sizeof(version), true) ?
               0 : SHADPS4_NP_ERROR_INVALID_ARGUMENT;
    }
    case SHADPS4_HLE_NP_CALLOUT_INIT:
        return shadps4_hle_np_callout(hle, cs, number, a0, 0, 0, 0, 0);
    case SHADPS4_HLE_NP_CALLOUT_TERM:
        return shadps4_hle_np_callout(hle, cs, number, a0, 0, 0, 0, 0);
    case SHADPS4_HLE_NP_CALLOUT_START:
    case SHADPS4_HLE_NP_CALLOUT_START64:
        return shadps4_hle_np_callout(hle, cs, number, a0, a1, a2,
                                      a3, a4);
    case SHADPS4_HLE_NP_CALLOUT_STOP:
        return shadps4_hle_np_callout(hle, cs, number, a0, a1, a2, 0, 0);
    case SHADPS4_HLE_SUCCESS:
        return 0;
    case SHADPS4_SYS_EXIT:
    case SHADPS4_SYS_THR_EXIT:
        shadps4_hle_cleanup(hle);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        return 0;
    case SHADPS4_SYS_READ:
        return shadps4_hle_read(hle, cs, a0, a1, a2);
    case SHADPS4_SYS_WRITE:
        return shadps4_hle_write(hle, cs, a0, a1, a2);
    case SHADPS4_SYS_OPEN:
        return shadps4_hle_open(hle, cs, a0, a1, a2);
    case SHADPS4_SYS_CLOSE:
        return shadps4_hle_close(hle, a0);
    case SHADPS4_SYS_UNLINK:
        return shadps4_hle_unlink(hle, cs, a0);
    case SHADPS4_SYS_GETPID:
    case SHADPS4_SYS_THR_SELF:
        if (number == SHADPS4_SYS_THR_SELF) {
            uint64_t tid = cpu_to_le64(1);

            return shadps4_guest_rw(cs, a0, &tid, sizeof(tid), true) ?
                   0 : -SHADPS4_GUEST_EFAULT;
        }
        return 1;
    case SHADPS4_SYS_GETUID:
    case SHADPS4_SYS_GETEUID:
    case SHADPS4_SYS_GETGID:
    case SHADPS4_SYS_GETEGID:
        return 0;
    case SHADPS4_SYS_IOCTL:
        return shadps4_hle_ioctl(hle, cs, a0, a1, a2);
    case SHADPS4_SYS_MUNMAP:
        return shadps4_hle_munmap(hle, cs, a0, a1);
    case SHADPS4_SYS_FSYNC:
        return shadps4_hle_fsync(hle, a0);
    case SHADPS4_SYS_SOCKET:
        return shadps4_hle_socket(hle, a0, a1, a2);
    case SHADPS4_SYS_CONNECT:
        return shadps4_hle_connect(hle, cs, a0, a1, a2);
    case SHADPS4_SYS_GETTIMEOFDAY:
        return shadps4_hle_time(cs, a0, QEMU_CLOCK_REALTIME, true);
    case SHADPS4_SYS_RENAME:
        return shadps4_hle_rename(hle, cs, a0, a1);
    case SHADPS4_SYS_MKDIR:
        return shadps4_hle_mkdir(hle, cs, a0, a1);
    case SHADPS4_SYS_STAT:
        return shadps4_hle_stat(hle, cs, a0, a1);
    case SHADPS4_SYS_FSTAT:
        return shadps4_hle_fstat(hle, cs, a0, a1);
    case SHADPS4_SYS_GETDIRENTRIES:
        return shadps4_hle_getdirentries(hle, cs, a0, a1, a2);
    case SHADPS4_SYS_CLOCK_GETTIME:
        return shadps4_hle_time(cs, a1,
                                a0 == 0 ? QEMU_CLOCK_REALTIME :
                                QEMU_CLOCK_VIRTUAL, false);
    case SHADPS4_SYS_KQUEUE:
        return shadps4_hle_kqueue(hle);
    case SHADPS4_SYS_KEVENT:
        return shadps4_hle_kevent(hle, cs, a0, a3, a4);
    case SHADPS4_SYS_UMTX_OP:
        return a1 == 2 ? 0 : -SHADPS4_GUEST_EAGAIN;
    case SHADPS4_SYS_MMAP:
        return shadps4_hle_mmap(hle, cs, a0, a1, a2);
    case SHADPS4_SYS_LSEEK:
        return shadps4_hle_lseek(hle, a0, a1, a2);
    default:
        warn_report("unimplemented shadPS4 HLE syscall: 0x%" PRIx64,
                    number);
        return -SHADPS4_GUEST_ENOSYS;
    }
}
