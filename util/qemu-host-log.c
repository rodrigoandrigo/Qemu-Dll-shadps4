/*
 * QEMU host embedding callbacks shared by system targets and tools.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#define QEMU_HOST_BUILD
#include "qemu/osdep.h"
#include "qemu/qemu-host.h"

static GMutex host_callback_lock;
static QemuHostLogCallback host_log_cb;
static void *host_log_opaque;
static QemuHostVideoCallback host_video_cb;
static void *host_video_opaque;
static QemuHostAudioCallback host_audio_cb;
static void *host_audio_opaque;

void qemu_host_register_log_callback(QemuHostLogCallback cb, void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    host_log_cb = cb;
    host_log_opaque = opaque;
    g_mutex_unlock(&host_callback_lock);
}

bool qemu_host_log_callback_enabled(void)
{
    bool enabled;

    g_mutex_lock(&host_callback_lock);
    enabled = host_log_cb != NULL;
    g_mutex_unlock(&host_callback_lock);
    return enabled;
}

void qemu_host_emit_log(QemuHostLogLevel level, const char *message)
{
    QemuHostLogCallback cb;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_log_cb;
    opaque = host_log_opaque;
    g_mutex_unlock(&host_callback_lock);

    if (cb) {
        cb(opaque, level, message ? message : "");
    }
}

void qemu_host_register_video_callback(QemuHostVideoCallback cb, void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    host_video_cb = cb;
    host_video_opaque = opaque;
    g_mutex_unlock(&host_callback_lock);
}

void qemu_host_emit_video_frame(const void *pixels,
                                int width,
                                int height,
                                int stride,
                                QemuHostPixelFormat format)
{
    QemuHostVideoCallback cb;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_video_cb;
    opaque = host_video_opaque;
    g_mutex_unlock(&host_callback_lock);

    if (cb) {
        cb(opaque, pixels, width, height, stride, format);
    }
}

void qemu_host_register_audio_callback(QemuHostAudioCallback cb, void *opaque)
{
    g_mutex_lock(&host_callback_lock);
    host_audio_cb = cb;
    host_audio_opaque = opaque;
    g_mutex_unlock(&host_callback_lock);
}

void qemu_host_emit_audio_frame(const void *samples,
                                size_t size,
                                int sample_rate,
                                int channels,
                                QemuHostAudioFormat format)
{
    QemuHostAudioCallback cb;
    void *opaque;

    g_mutex_lock(&host_callback_lock);
    cb = host_audio_cb;
    opaque = host_audio_opaque;
    g_mutex_unlock(&host_callback_lock);

    if (cb) {
        cb(opaque, samples, size, sample_rate, channels, format);
    }
}
