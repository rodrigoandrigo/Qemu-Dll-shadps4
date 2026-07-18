/*
 * shadPS4 pad and AudioOut bridge
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#define QEMU_HOST_INTERNAL
#include "hw/i386/shadps4-io.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"

#define SHADPS4_PAD_CROSS (1U << 0)
#define SHADPS4_PAD_CIRCLE (1U << 1)
#define SHADPS4_PAD_OPTIONS (1U << 2)
#define SHADPS4_PAD_UP (1U << 4)
#define SHADPS4_PAD_DOWN (1U << 5)
#define SHADPS4_PAD_LEFT (1U << 6)
#define SHADPS4_PAD_RIGHT (1U << 7)
#define SHADPS4_PAD_POINTER (1U << 8)
#define SHADPS4_AUDIO_MAX_CHUNK (256 * KiB)
#define SHADPS4_AUDIO_MAX_QUEUE MiB

static uint32_t shadps4_io_key_button(int qcode)
{
    switch (qcode) {
    case Q_KEY_CODE_SPC:
        return SHADPS4_PAD_CROSS;
    case Q_KEY_CODE_ESC:
        return SHADPS4_PAD_CIRCLE;
    case Q_KEY_CODE_RET:
        return SHADPS4_PAD_OPTIONS;
    case Q_KEY_CODE_UP:
    case Q_KEY_CODE_W:
        return SHADPS4_PAD_UP;
    case Q_KEY_CODE_DOWN:
    case Q_KEY_CODE_S:
        return SHADPS4_PAD_DOWN;
    case Q_KEY_CODE_LEFT:
    case Q_KEY_CODE_A:
        return SHADPS4_PAD_LEFT;
    case Q_KEY_CODE_RIGHT:
    case Q_KEY_CODE_D:
        return SHADPS4_PAD_RIGHT;
    default:
        return 0;
    }
}

static void shadps4_io_input_event(DeviceState *dev, QemuConsole *src,
                                   InputEvent *event)
{
    ShadPS4IOState *io = SHADPS4_IO(dev);
    ShadPS4PadState *pad = &io->pads[0].legacy;
    uint32_t button = 0;

    switch (event->type) {
    case INPUT_EVENT_KIND_KEY: {
        InputKeyEvent *key = event->u.key.data;
        int qcode = qemu_input_key_value_to_qcode(key->key);

        button = shadps4_io_key_button(qcode);
        if (key->down) {
            pad->buttons |= button;
        } else {
            pad->buttons &= ~button;
        }
        break;
    }
    case INPUT_EVENT_KIND_BTN: {
        InputBtnEvent *btn = event->u.btn.data;

        if (btn->button == INPUT_BUTTON_LEFT) {
            button = SHADPS4_PAD_POINTER;
        } else if (btn->button == INPUT_BUTTON_RIGHT) {
            button = SHADPS4_PAD_CIRCLE;
        }
        if (btn->down) {
            pad->buttons |= button;
        } else {
            pad->buttons &= ~button;
        }
        break;
    }
    case INPUT_EVENT_KIND_ABS: {
        InputMoveEvent *move = event->u.abs.data;

        if (move->axis == INPUT_AXIS_X) {
            pad->pointer_x = CLAMP(move->value, INT32_MIN, INT32_MAX);
        } else if (move->axis == INPUT_AXIS_Y) {
            pad->pointer_y = CLAMP(move->value, INT32_MIN, INT32_MAX);
        }
        break;
    }
    case INPUT_EVENT_KIND_REL: {
        InputMoveEvent *move = event->u.rel.data;
        int64_t value;

        if (move->axis == INPUT_AXIS_X) {
            value = (int64_t)pad->pointer_x + move->value;
            pad->pointer_x = CLAMP(value, INT32_MIN, INT32_MAX);
        } else if (move->axis == INPUT_AXIS_Y) {
            value = (int64_t)pad->pointer_y + move->value;
            pad->pointer_y = CLAMP(value, INT32_MIN, INT32_MAX);
        }
        break;
    }
    default:
        break;
    }
}

static void shadps4_io_input_sync(DeviceState *dev)
{
    ShadPS4PadDeviceState *pad = &SHADPS4_IO(dev)->pads[0];

    pad->legacy.sequence++;
    pad->timestamp_ns = qemu_clock_get_ns(QEMU_CLOCK_HOST);
}

static const QemuInputHandler shadps4_io_input_handler = {
    .name = "shadPS4 virtual pad",
    .mask = INPUT_EVENT_MASK_KEY | INPUT_EVENT_MASK_BTN |
            INPUT_EVENT_MASK_REL | INPUT_EVENT_MASK_ABS,
    .event = shadps4_io_input_event,
    .sync = shadps4_io_input_sync,
};

static int shadps4_io_gamepad(void *opaque, int controller,
                              const QemuHostGamepadState *state)
{
    ShadPS4IOState *io = opaque;
    ShadPS4PadDeviceState *pad;

    if (!state || controller < 0 || controller >= QEMU_HOST_MAX_GAMEPADS) {
        return -EINVAL;
    }
    pad = &io->pads[controller];

    pad->legacy.buttons = state->buttons;
    pad->legacy.left_x = state->left_x;
    pad->legacy.left_y = state->left_y;
    pad->legacy.right_x = state->right_x;
    pad->legacy.right_y = state->right_y;
    pad->left_trigger = state->left_trigger;
    pad->right_trigger = state->right_trigger;
    pad->connected = state->connected;
    pad->timestamp_ns = state->timestamp_ns;
    pad->legacy.sequence++;
    return 0;
}

static int shadps4_io_touch(void *opaque, int controller,
                            const QemuHostTouchState *state)
{
    ShadPS4PadDeviceState *pad;
    int i;

    if (!state || controller < 0 || controller >= QEMU_HOST_MAX_GAMEPADS) {
        return -EINVAL;
    }
    pad = &((ShadPS4IOState *)opaque)->pads[controller];

    pad->touch = *state;
    for (i = 0; i < QEMU_HOST_MAX_TOUCHES; i++) {
        pad->touch.points[i].x =
            isfinite(pad->touch.points[i].x) ?
            CLAMP(pad->touch.points[i].x, 0.0f, 1.0f) : 0.0f;
        pad->touch.points[i].y =
            isfinite(pad->touch.points[i].y) ?
            CLAMP(pad->touch.points[i].y, 0.0f, 1.0f) : 0.0f;
    }
    pad->timestamp_ns = state->timestamp_ns;
    pad->legacy.sequence++;
    return 0;
}

static int shadps4_io_motion(void *opaque, int controller,
                             const QemuHostMotionState *state)
{
    ShadPS4PadDeviceState *pad;
    size_t i;

    if (!state || controller < 0 || controller >= QEMU_HOST_MAX_GAMEPADS) {
        return -EINVAL;
    }
    pad = &((ShadPS4IOState *)opaque)->pads[controller];

    pad->motion = *state;
    for (i = 0; i < 3; i++) {
        if (!isfinite(pad->motion.acceleration[i])) {
            pad->motion.acceleration[i] = 0.0f;
        }
        if (!isfinite(pad->motion.angular_velocity[i])) {
            pad->motion.angular_velocity[i] = 0.0f;
        }
    }
    for (i = 0; i < 4; i++) {
        if (!isfinite(pad->motion.orientation[i])) {
            pad->motion.orientation[i] = 0.0f;
        }
    }
    pad->timestamp_ns = state->timestamp_ns;
    pad->legacy.sequence++;
    return 0;
}

static int shadps4_io_audio_input(void *opaque, const void *samples,
                                  size_t size, int sample_rate, int channels,
                                  QemuHostAudioFormat format)
{
    ShadPS4IOState *io = opaque;
    size_t queued = io->audio_input_queue->len - io->audio_input_offset;

    if (size > SHADPS4_AUDIO_MAX_QUEUE - queued) {
        return -ENOSPC;
    }
    if (queued && (sample_rate != io->audio_input_rate ||
                   channels != io->audio_input_channels ||
                   format != io->audio_input_format)) {
        g_byte_array_set_size(io->audio_input_queue, 0);
        io->audio_input_offset = 0;
        queued = 0;
    }
    if (!queued) {
        g_byte_array_set_size(io->audio_input_queue, 0);
        io->audio_input_offset = 0;
    }
    io->audio_input_rate = sample_rate;
    io->audio_input_channels = channels;
    io->audio_input_format = format;
    g_byte_array_append(io->audio_input_queue, samples, size);
    return 0;
}

static const QemuHostInputSink shadps4_io_host_input_sink = {
    .gamepad = shadps4_io_gamepad,
    .touch = shadps4_io_touch,
    .motion = shadps4_io_motion,
    .audio_input = shadps4_io_audio_input,
};

static AudioFormat shadps4_io_audio_format(QemuHostAudioFormat format)
{
    switch (format) {
    case QEMU_HOST_AUDIO_FORMAT_U8:
        return AUDIO_FORMAT_U8;
    case QEMU_HOST_AUDIO_FORMAT_S16:
        return AUDIO_FORMAT_S16;
    case QEMU_HOST_AUDIO_FORMAT_S32:
        return AUDIO_FORMAT_S32;
    case QEMU_HOST_AUDIO_FORMAT_F32:
        return AUDIO_FORMAT_F32;
    default:
        return AUDIO_FORMAT__MAX;
    }
}

static void shadps4_io_audio_out(void *opaque, int available)
{
    ShadPS4IOState *io = opaque;

    while (available > 0 && io->audio_queue_offset < io->audio_queue->len) {
        size_t queued = io->audio_queue->len - io->audio_queue_offset;
        size_t chunk = MIN(queued, available);
        size_t written = audio_be_write(io->audio_be, io->audio_voice,
                                        io->audio_queue->data +
                                        io->audio_queue_offset, chunk);

        if (!written) {
            break;
        }
        if (written > chunk) {
            warn_report("shadPS4 audio backend consumed too many bytes");
            break;
        }
        io->audio_queue_offset += written;
        available -= written;
    }
    if (io->audio_queue_offset == io->audio_queue->len) {
        g_byte_array_set_size(io->audio_queue, 0);
        io->audio_queue_offset = 0;
    }
}

static void shadps4_io_close_audio(ShadPS4IOState *io)
{
    if (io->audio_voice) {
        audio_be_set_active_out(io->audio_be, io->audio_voice, false);
        audio_be_close_out(io->audio_be, io->audio_voice);
        io->audio_voice = NULL;
    }
}

static void shadps4_io_reset(DeviceState *dev)
{
    ShadPS4IOState *io = SHADPS4_IO(dev);

    shadps4_io_close_audio(io);
    memset(io->pads, 0, sizeof(io->pads));
    io->pads[0].connected = true;
    if (io->audio_queue) {
        g_byte_array_set_size(io->audio_queue, 0);
    }
    io->audio_queue_offset = 0;
    if (io->audio_input_queue) {
        g_byte_array_set_size(io->audio_input_queue, 0);
    }
    io->audio_input_offset = 0;
    io->audio_rate = 48000;
    io->audio_channels = 2;
    io->audio_format = QEMU_HOST_AUDIO_FORMAT_S16;
    memset(io->audio_volume, UINT8_MAX, sizeof(io->audio_volume));
    io->audio_muted = false;
    io->audio_input_rate = 48000;
    io->audio_input_channels = 2;
    io->audio_input_format = QEMU_HOST_AUDIO_FORMAT_S16;
}

static void shadps4_io_realize(DeviceState *dev, Error **errp)
{
    ShadPS4IOState *io = SHADPS4_IO(dev);
    Error *local_err = NULL;

    io->audio_queue = g_byte_array_new();
    io->audio_input_queue = g_byte_array_new();
    if (!io->audio_be) {
        io->audio_be = audio_be_by_name("shadps4", NULL);
    }
    if (!audio_be_check(&io->audio_be, &local_err)) {
        warn_report_err(local_err);
        io->audio_be = NULL;
    }
    io->input_handler = qemu_input_handler_register(
        dev, &shadps4_io_input_handler);
    qemu_host_register_input_sink(&shadps4_io_host_input_sink, io);
    shadps4_io_reset(dev);
}

static void shadps4_io_unrealize(DeviceState *dev)
{
    ShadPS4IOState *io = SHADPS4_IO(dev);

    qemu_host_register_input_sink(NULL, NULL);
    shadps4_io_close_audio(io);
    if (io->input_handler) {
        qemu_input_handler_unregister(io->input_handler);
        io->input_handler = NULL;
    }
    g_clear_pointer(&io->audio_queue, g_byte_array_unref);
    g_clear_pointer(&io->audio_input_queue, g_byte_array_unref);
}

bool shadps4_io_get_pad(ShadPS4IOState *io, int controller,
                        ShadPS4PadDeviceState *pad)
{
    if (controller < 0 || controller >= QEMU_HOST_MAX_GAMEPADS) {
        return false;
    }
    *pad = io->pads[controller];
    return true;
}

bool shadps4_io_set_pad_output(ShadPS4IOState *io, int controller,
                               const QemuHostPadOutput *output)
{
    ShadPS4PadDeviceState *pad;

    if (controller < 0 || controller >= QEMU_HOST_MAX_GAMEPADS || !output) {
        return false;
    }
    pad = &io->pads[controller];
    pad->output = *output;
    pad->legacy.rumble_small = output->rumble_small;
    pad->legacy.rumble_large = output->rumble_large;
    qemu_host_emit_pad_output(controller, output);
    return true;
}

bool shadps4_io_configure_audio(ShadPS4IOState *io, int rate, int channels,
                                QemuHostAudioFormat format)
{
    struct audsettings settings;
    AudioFormat audio_format;

    if (rate < 8000 || rate > 192000 || channels < 1 || channels > 8 ||
        format < QEMU_HOST_AUDIO_FORMAT_U8 ||
        format > QEMU_HOST_AUDIO_FORMAT_F32) {
        return false;
    }
    audio_format = shadps4_io_audio_format(format);
    if (audio_format == AUDIO_FORMAT__MAX) {
        return false;
    }
    shadps4_io_close_audio(io);
    io->audio_rate = rate;
    io->audio_channels = channels;
    io->audio_format = format;
    if (io->audio_be) {
        settings.freq = rate;
        settings.nchannels = channels;
        settings.fmt = audio_format;
        settings.big_endian = false;
        io->audio_voice = audio_be_open_out(
            io->audio_be, NULL, "shadps4.audioout", io,
            shadps4_io_audio_out, &settings);
        if (io->audio_voice) {
            audio_be_set_active_out(io->audio_be, io->audio_voice, true);
            shadps4_io_set_audio_volume(io, io->audio_muted, channels,
                                        io->audio_volume);
        } else {
            return false;
        }
    }
    return true;
}

bool shadps4_io_emit_audio(ShadPS4IOState *io, CPUState *cs,
                           uint64_t guest_addr, size_t size)
{
    return shadps4_io_emit_audio_format(
        io, cs, guest_addr, size, io->audio_rate, io->audio_channels,
        io->audio_format, true);
}

bool shadps4_io_emit_audio_format(ShadPS4IOState *io, CPUState *cs,
                                  uint64_t guest_addr, size_t size,
                                  int rate, int channels,
                                  QemuHostAudioFormat format,
                                  bool local_playback)
{
    g_autofree uint8_t *samples = NULL;
    size_t sample_size = format == QEMU_HOST_AUDIO_FORMAT_U8 ? 1 :
                         format == QEMU_HOST_AUDIO_FORMAT_S16 ? 2 : 4;

    if (rate < 8000 || rate > 192000 || channels < 1 || channels > 8 ||
        format < QEMU_HOST_AUDIO_FORMAT_U8 ||
        format > QEMU_HOST_AUDIO_FORMAT_F32 || !size ||
        size > SHADPS4_AUDIO_MAX_CHUNK || size % (sample_size * channels)) {
        return false;
    }
    samples = g_malloc(size);
    if (cpu_memory_rw_debug(cs, guest_addr, samples, size, false)) {
        return false;
    }
    if (local_playback &&
        size > SHADPS4_AUDIO_MAX_QUEUE - shadps4_io_audio_queued(io)) {
        return false;
    }
    qemu_host_emit_audio_frame(samples, size, rate, channels, format);
    if (local_playback && io->audio_voice) {
        if (!shadps4_io_audio_queued(io)) {
            g_byte_array_set_size(io->audio_queue, 0);
            io->audio_queue_offset = 0;
        }
        g_byte_array_append(io->audio_queue, samples, size);
        shadps4_io_audio_out(io, audio_be_get_buffer_size_out(
                                io->audio_be, io->audio_voice));
    }
    return true;
}

bool shadps4_io_drain_audio(ShadPS4IOState *io)
{
    if (io->audio_voice) {
        shadps4_io_audio_out(io, audio_be_get_buffer_size_out(
                                io->audio_be, io->audio_voice));
    }
    return shadps4_io_audio_queued(io) == 0;
}

bool shadps4_io_set_audio_volume(ShadPS4IOState *io, bool muted,
                                 int channels, const uint8_t *volume)
{
    Volume qemu_volume = {
        .mute = muted,
        .channels = channels,
    };

    if (channels < 1 || channels > 8 || !volume) {
        return false;
    }
    memcpy(io->audio_volume, volume, channels);
    io->audio_muted = muted;
    memcpy(qemu_volume.vol, volume, channels);
    if (io->audio_voice) {
        audio_be_set_volume_out(io->audio_be, io->audio_voice,
                                &qemu_volume);
    }
    return true;
}

size_t shadps4_io_audio_queued(const ShadPS4IOState *io)
{
    return io->audio_queue->len - io->audio_queue_offset;
}

size_t shadps4_io_read_audio_input(ShadPS4IOState *io, void *buffer,
                                   size_t size)
{
    size_t queued = io->audio_input_queue->len - io->audio_input_offset;
    size_t copied = MIN(size, queued);

    if (copied) {
        memcpy(buffer, io->audio_input_queue->data + io->audio_input_offset,
               copied);
    }
    io->audio_input_offset += copied;
    if (io->audio_input_offset == io->audio_input_queue->len) {
        g_byte_array_set_size(io->audio_input_queue, 0);
        io->audio_input_offset = 0;
    }
    return copied;
}

static void shadps4_io_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    static const Property properties[] = {
        DEFINE_AUDIO_PROPERTIES(ShadPS4IOState, audio_be),
    };

    dc->realize = shadps4_io_realize;
    dc->unrealize = shadps4_io_unrealize;
    device_class_set_legacy_reset(dc, shadps4_io_reset);
    device_class_set_props(dc, properties);
}

static const TypeInfo shadps4_io_info = {
    .name = TYPE_SHADPS4_IO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ShadPS4IOState),
    .class_init = shadps4_io_class_init,
};

static void shadps4_io_register_types(void)
{
    type_register_static(&shadps4_io_info);
}

type_init(shadps4_io_register_types)
