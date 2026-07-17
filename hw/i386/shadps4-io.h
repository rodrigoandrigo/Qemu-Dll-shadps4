/*
 * shadPS4 pad and AudioOut bridge
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I386_SHADPS4_IO_H
#define HW_I386_SHADPS4_IO_H

#include "hw/core/cpu.h"
#include "hw/core/sysbus.h"
#include "qemu/audio.h"
#include "qemu/qemu-host.h"
#include "ui/input.h"

#define TYPE_SHADPS4_IO "shadps4-io"
OBJECT_DECLARE_SIMPLE_TYPE(ShadPS4IOState, SHADPS4_IO)

typedef struct ShadPS4PadState {
    uint32_t buttons;
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
    int32_t pointer_x;
    int32_t pointer_y;
    uint32_t reserved;
    uint64_t sequence;
    uint16_t rumble_small;
    uint16_t rumble_large;
    uint32_t reserved2;
} ShadPS4PadState;

typedef struct ShadPS4PadDeviceState {
    ShadPS4PadState legacy;
    uint8_t left_trigger;
    uint8_t right_trigger;
    bool connected;
    QemuHostTouchState touch;
    QemuHostMotionState motion;
    QemuHostPadOutput output;
    uint64_t timestamp_ns;
} ShadPS4PadDeviceState;

struct ShadPS4IOState {
    SysBusDevice parent_obj;
    QemuInputHandlerState *input_handler;
    ShadPS4PadDeviceState pads[QEMU_HOST_MAX_GAMEPADS];
    AudioBackend *audio_be;
    SWVoiceOut *audio_voice;
    GByteArray *audio_queue;
    size_t audio_queue_offset;
    GByteArray *audio_input_queue;
    size_t audio_input_offset;
    int audio_rate;
    int audio_channels;
    QemuHostAudioFormat audio_format;
    uint8_t audio_volume[8];
    bool audio_muted;
    int audio_input_rate;
    int audio_input_channels;
    QemuHostAudioFormat audio_input_format;
};

bool shadps4_io_get_pad(ShadPS4IOState *io, int controller,
                        ShadPS4PadDeviceState *pad);
bool shadps4_io_set_pad_output(ShadPS4IOState *io, int controller,
                               const QemuHostPadOutput *output);
bool shadps4_io_configure_audio(ShadPS4IOState *io, int rate, int channels,
                                QemuHostAudioFormat format);
bool shadps4_io_emit_audio(ShadPS4IOState *io, CPUState *cs,
                           uint64_t guest_addr, size_t size);
bool shadps4_io_drain_audio(ShadPS4IOState *io);
bool shadps4_io_set_audio_volume(ShadPS4IOState *io, bool muted,
                                 int channels, const uint8_t *volume);
size_t shadps4_io_audio_queued(const ShadPS4IOState *io);
size_t shadps4_io_read_audio_input(ShadPS4IOState *io, void *buffer,
                                   size_t size);

#endif /* HW_I386_SHADPS4_IO_H */
