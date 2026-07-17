/*
 * QEMU XAudio2 audio driver
 *
 * Copyright (c) 2026
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#define COBJMACROS
#include "qemu/osdep.h"
#include "qemu/audio.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qom/object.h"

#include "audio_int.h"

#include <windows.h>
#include <roapi.h>
#include <xaudio2.h>

#define TYPE_AUDIO_XAUDIO2 "audio-xaudio2"
OBJECT_DECLARE_SIMPLE_TYPE(AudioXAudio2, AUDIO_XAUDIO2)

#define XAUDIO2_QEMU_MAX_BUFFERS 8
#define XAUDIO2_QEMU_DEFAULT_BUFFER_US 20000

static AudioBackendClass *audio_xaudio2_parent_class;

struct AudioXAudio2 {
    AudioMixengBackend parent_obj;

    IXAudio2 *xaudio;
    IXAudio2MasteringVoice *mastering_voice;
    bool winrt_initialized;
};

typedef struct XAudio2VoiceOut {
    HWVoiceOut hw;
    IXAudio2SourceVoice *source_voice;
    IXAudio2VoiceCallback callback;
    bool enabled;
} XAudio2VoiceOut;

static void STDMETHODCALLTYPE xaudio2_cb_on_voice_processing_pass_start(
    IXAudio2VoiceCallback *iface, UINT32 bytes_required)
{
}

static void STDMETHODCALLTYPE xaudio2_cb_on_voice_processing_pass_end(
    IXAudio2VoiceCallback *iface)
{
}

static void STDMETHODCALLTYPE xaudio2_cb_on_stream_end(
    IXAudio2VoiceCallback *iface)
{
}

static void STDMETHODCALLTYPE xaudio2_cb_on_buffer_start(
    IXAudio2VoiceCallback *iface, void *context)
{
}

static void STDMETHODCALLTYPE xaudio2_cb_on_buffer_end(
    IXAudio2VoiceCallback *iface, void *context)
{
    g_free(context);
}

static void STDMETHODCALLTYPE xaudio2_cb_on_loop_end(
    IXAudio2VoiceCallback *iface, void *context)
{
}

static void STDMETHODCALLTYPE xaudio2_cb_on_voice_error(
    IXAudio2VoiceCallback *iface, void *context, HRESULT error)
{
    error_report("xaudio2: voice error: 0x%lx", (unsigned long)error);
}

static IXAudio2VoiceCallbackVtbl xaudio2_voice_callback_vtbl = {
    xaudio2_cb_on_voice_processing_pass_start,
    xaudio2_cb_on_voice_processing_pass_end,
    xaudio2_cb_on_stream_end,
    xaudio2_cb_on_buffer_start,
    xaudio2_cb_on_buffer_end,
    xaudio2_cb_on_loop_end,
    xaudio2_cb_on_voice_error,
};

static const char *xaudio2_hresult_str(HRESULT hr)
{
    switch (hr) {
    case S_OK:
        return "success";
#ifdef XAUDIO2_E_INVALID_CALL
    case XAUDIO2_E_INVALID_CALL:
        return "invalid call";
#endif
#ifdef XAUDIO2_E_XMA_DECODER_ERROR
    case XAUDIO2_E_XMA_DECODER_ERROR:
        return "XMA decoder error";
#endif
#ifdef XAUDIO2_E_XAPO_CREATION_FAILED
    case XAUDIO2_E_XAPO_CREATION_FAILED:
        return "XAPO creation failed";
#endif
#ifdef XAUDIO2_E_DEVICE_INVALIDATED
    case XAUDIO2_E_DEVICE_INVALIDATED:
        return "device invalidated";
#endif
    default:
        return "unknown error";
    }
}

static void xaudio2_set_error(Error **errp, HRESULT hr, const char *msg)
{
    error_setg(errp, "%s: %s (HRESULT 0x%lx)",
               msg, xaudio2_hresult_str(hr), (unsigned long)hr);
}

static bool xaudio2_format_supported(AudioFormat fmt)
{
    return fmt == AUDIO_FORMAT_U8 ||
           fmt == AUDIO_FORMAT_S16 ||
           fmt == AUDIO_FORMAT_S32 ||
           fmt == AUDIO_FORMAT_F32;
}

static WORD xaudio2_bits_per_sample(AudioFormat fmt)
{
    switch (fmt) {
    case AUDIO_FORMAT_U8:
        return 8;
    case AUDIO_FORMAT_S16:
        return 16;
    case AUDIO_FORMAT_S32:
    case AUDIO_FORMAT_F32:
        return 32;
    default:
        return 0;
    }
}

static void xaudio2_fill_wave_format(WAVEFORMATEX *wfx,
                                     const struct audio_pcm_info *info)
{
    WORD bits = xaudio2_bits_per_sample(info->af);

    memset(wfx, 0, sizeof(*wfx));
    wfx->wFormatTag = info->af == AUDIO_FORMAT_F32
        ? WAVE_FORMAT_IEEE_FLOAT
        : WAVE_FORMAT_PCM;
    wfx->nChannels = info->nchannels;
    wfx->nSamplesPerSec = info->freq;
    wfx->wBitsPerSample = bits;
    wfx->nBlockAlign = info->nchannels * bits / 8;
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
}

static int xaudio2_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    AudioXAudio2 *s = AUDIO_XAUDIO2(hw->s);
    XAudio2VoiceOut *v = (XAudio2VoiceOut *)hw;
    AudiodevPerDirectionOptions *pdo = hw->s->dev->u.xaudio2.out;
    WAVEFORMATEX wfx;
    HRESULT hr;

    if (!xaudio2_format_supported(as->fmt)) {
        error_report("xaudio2: unsupported output format");
        return -1;
    }

    as->big_endian = false;
    audio_pcm_init_info(&hw->info, as);
    hw->samples = audio_buffer_samples(pdo, as,
                                       XAUDIO2_QEMU_DEFAULT_BUFFER_US);

    v->callback.lpVtbl = &xaudio2_voice_callback_vtbl;
    xaudio2_fill_wave_format(&wfx, &hw->info);

    hr = IXAudio2_CreateSourceVoice(s->xaudio, &v->source_voice, &wfx,
                                    0, XAUDIO2_DEFAULT_FREQ_RATIO,
                                    &v->callback, NULL, NULL);
    if (FAILED(hr)) {
        error_report("xaudio2: could not create source voice: %s (0x%lx)",
                     xaudio2_hresult_str(hr), (unsigned long)hr);
        return -1;
    }

    return 0;
}

static void xaudio2_fini_out(HWVoiceOut *hw)
{
    XAudio2VoiceOut *v = (XAudio2VoiceOut *)hw;

    if (v->source_voice) {
        IXAudio2SourceVoice_Stop(v->source_voice, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(v->source_voice);
        IXAudio2SourceVoice_DestroyVoice(v->source_voice);
        v->source_voice = NULL;
    }
}

static size_t xaudio2_write(HWVoiceOut *hw, void *buf, size_t len)
{
    XAudio2VoiceOut *v = (XAudio2VoiceOut *)hw;
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xb;
    void *copy;
    HRESULT hr;

    if (!v->enabled || !v->source_voice || !len) {
        return 0;
    }

    IXAudio2SourceVoice_GetState(v->source_voice, &state, 0);
    if (state.BuffersQueued >= XAUDIO2_QEMU_MAX_BUFFERS) {
        return 0;
    }

    if (len > UINT32_MAX) {
        len = UINT32_MAX - (UINT32_MAX % hw->info.bytes_per_frame);
    }
    if (len < hw->info.bytes_per_frame) {
        return 0;
    }

    copy = g_memdup2(buf, len);
    memset(&xb, 0, sizeof(xb));
    xb.AudioBytes = (UINT32)len;
    xb.pAudioData = copy;
    xb.pContext = copy;

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(v->source_voice, &xb, NULL);
    if (FAILED(hr)) {
        g_free(copy);
        error_report("xaudio2: SubmitSourceBuffer failed: %s (0x%lx)",
                     xaudio2_hresult_str(hr), (unsigned long)hr);
        return 0;
    }

    return len;
}

static size_t xaudio2_buffer_get_free(HWVoiceOut *hw)
{
    XAudio2VoiceOut *v = (XAudio2VoiceOut *)hw;
    XAUDIO2_VOICE_STATE state;

    if (!v->enabled || !v->source_voice) {
        return 0;
    }

    IXAudio2SourceVoice_GetState(v->source_voice, &state, 0);
    if (state.BuffersQueued >= XAUDIO2_QEMU_MAX_BUFFERS) {
        return 0;
    }

    return hw->samples * hw->info.bytes_per_frame;
}

static void xaudio2_volume_out(HWVoiceOut *hw, Volume *vol)
{
    XAudio2VoiceOut *v = (XAudio2VoiceOut *)hw;
    float levels[AUDIO_MAX_CHANNELS];
    int channels;
    int i;

    if (!v->source_voice) {
        return;
    }

    IXAudio2SourceVoice_SetVolume(v->source_voice,
                                  vol->mute ? 0.0f : 1.0f,
                                  XAUDIO2_COMMIT_NOW);

    if (vol->channels <= 0 || hw->info.nchannels > AUDIO_MAX_CHANNELS) {
        return;
    }

    channels = hw->info.nchannels;
    for (i = 0; i < channels; i++) {
        int volume_channel = MIN(i, vol->channels - 1);

        levels[i] = vol->vol[volume_channel] / 255.0f;
    }

    IXAudio2SourceVoice_SetChannelVolumes(v->source_voice, channels, levels,
                                          XAUDIO2_COMMIT_NOW);
}

static void xaudio2_enable_out(HWVoiceOut *hw, bool enable)
{
    XAudio2VoiceOut *v = (XAudio2VoiceOut *)hw;

    if (!v->source_voice) {
        return;
    }

    v->enabled = enable;
    if (enable) {
        IXAudio2SourceVoice_Start(v->source_voice, 0, XAUDIO2_COMMIT_NOW);
    } else {
        IXAudio2SourceVoice_Stop(v->source_voice, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(v->source_voice);
    }
}

static void audio_xaudio2_finalize(Object *obj)
{
    AudioXAudio2 *s = AUDIO_XAUDIO2(obj);

    if (s->mastering_voice) {
        IXAudio2MasteringVoice_DestroyVoice(s->mastering_voice);
        s->mastering_voice = NULL;
    }
    if (s->xaudio) {
        IXAudio2_Release(s->xaudio);
        s->xaudio = NULL;
    }
    if (s->winrt_initialized) {
        RoUninitialize();
        s->winrt_initialized = false;
    }
}

static bool audio_xaudio2_realize(AudioBackend *abe,
                                  Audiodev *dev,
                                  Error **errp)
{
    AudioXAudio2 *s = AUDIO_XAUDIO2(abe);
    HRESULT hr;

    assert(dev->driver == AUDIODEV_DRIVER_XAUDIO2);

    if (!audio_xaudio2_parent_class->realize(abe, dev, errp)) {
        return false;
    }

    hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        s->winrt_initialized = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        xaudio2_set_error(errp, hr, "could not initialize WinRT");
        return false;
    }

    hr = XAudio2Create(&s->xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        xaudio2_set_error(errp, hr, "could not create XAudio2 instance");
        return false;
    }

    hr = IXAudio2_CreateMasteringVoice(s->xaudio, &s->mastering_voice,
                                       XAUDIO2_DEFAULT_CHANNELS,
                                       XAUDIO2_DEFAULT_SAMPLERATE,
                                       0, NULL, NULL,
                                       AudioCategory_GameEffects);
    if (FAILED(hr)) {
        xaudio2_set_error(errp, hr, "could not create XAudio2 mastering voice");
        return false;
    }

    return true;
}

static void audio_xaudio2_class_init(ObjectClass *klass, const void *data)
{
    AudioBackendClass *b = AUDIO_BACKEND_CLASS(klass);
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    audio_xaudio2_parent_class =
        AUDIO_BACKEND_CLASS(object_class_get_parent(klass));

    b->realize = audio_xaudio2_realize;

    k->max_voices_out = INT_MAX;
    k->max_voices_in = 0;
    k->voice_size_out = sizeof(XAudio2VoiceOut);
    k->voice_size_in = 0;

    k->init_out = xaudio2_init_out;
    k->fini_out = xaudio2_fini_out;
    k->write = xaudio2_write;
    k->buffer_get_free = xaudio2_buffer_get_free;
    k->run_buffer_out = audio_generic_run_buffer_out;
    k->enable_out = xaudio2_enable_out;
    k->volume_out = xaudio2_volume_out;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_XAUDIO2,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioXAudio2),
        .class_init = audio_xaudio2_class_init,
        .instance_finalize = audio_xaudio2_finalize,
    },
};

DEFINE_TYPES(audio_types)
module_obj(TYPE_AUDIO_XAUDIO2);
