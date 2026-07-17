/*
 * QEMU host embedding display backend
 *
 * Copyright (c) 2026
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#define QEMU_HOST_BUILD
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/qemu-host.h"
#include "ui/console.h"

typedef struct HostDisplay {
    DisplayChangeListener dcl;
    DisplaySurface *surface;
} HostDisplay;

static QemuHostPixelFormat host_display_format(pixman_format_code_t format)
{
    switch (format) {
    case PIXMAN_a8r8g8b8:
        return QEMU_HOST_PIXEL_FORMAT_BGRA8888;
    case PIXMAN_x8r8g8b8:
        return QEMU_HOST_PIXEL_FORMAT_BGRX8888;
    case PIXMAN_a8b8g8r8:
        return QEMU_HOST_PIXEL_FORMAT_RGBA8888;
    case PIXMAN_x8b8g8r8:
        return QEMU_HOST_PIXEL_FORMAT_RGBX8888;
    default:
        return QEMU_HOST_PIXEL_FORMAT_UNKNOWN;
    }
}

static void host_display_emit(HostDisplay *hdpy)
{
    DisplaySurface *surface = hdpy->surface;
    QemuHostPixelFormat format;

    if (!surface || surface_is_placeholder(surface)) {
        return;
    }

    format = host_display_format(surface_format(surface));
    if (format == QEMU_HOST_PIXEL_FORMAT_UNKNOWN) {
        return;
    }

    qemu_host_emit_video_frame(surface_data(surface),
                               surface_width(surface),
                               surface_height(surface),
                               surface_stride(surface),
                               format);
}

static void host_display_refresh(DisplayChangeListener *dcl)
{
    if (qemu_console_is_graphic(dcl->con)) {
        graphic_hw_update(dcl->con);
    }
}

static void host_display_update(DisplayChangeListener *dcl,
                                int x, int y, int w, int h)
{
    HostDisplay *hdpy = container_of(dcl, HostDisplay, dcl);

    host_display_emit(hdpy);
}

static void host_display_switch(DisplayChangeListener *dcl,
                                DisplaySurface *new_surface)
{
    HostDisplay *hdpy = container_of(dcl, HostDisplay, dcl);

    hdpy->surface = new_surface;
    host_display_emit(hdpy);
}

static bool host_display_check_format(DisplayChangeListener *dcl,
                                      pixman_format_code_t format)
{
    return host_display_format(format) != QEMU_HOST_PIXEL_FORMAT_UNKNOWN;
}

static const DisplayChangeListenerOps host_display_ops = {
    .dpy_name             = "host",
    .dpy_refresh          = host_display_refresh,
    .dpy_gfx_update       = host_display_update,
    .dpy_gfx_switch       = host_display_switch,
    .dpy_gfx_check_format = host_display_check_format,
};

static void host_display_register_console(QemuConsole *con)
{
    HostDisplay *hdpy;

    if (!con) {
        return;
    }

    hdpy = g_new0(HostDisplay, 1);
    hdpy->dcl.con = con;
    hdpy->dcl.ops = &host_display_ops;
    register_displaychangelistener(&hdpy->dcl);
}

static void host_display_init(DisplayState *ds, DisplayOptions *opts)
{
    QemuConsole *con;
    int idx;

    host_display_register_console(qemu_console_lookup_by_index(0));

    for (idx = 0;; idx++) {
        con = qemu_console_lookup_by_index(idx);
        if (!con) {
            break;
        }
        if (idx == 0 || !qemu_console_is_graphic(con)) {
            continue;
        }

        host_display_register_console(con);
    }
}

static QemuDisplay qemu_display_host = {
    .type = DISPLAY_TYPE_HOST,
    .init = host_display_init,
};

static void register_host_display(void)
{
    qemu_display_register(&qemu_display_host);
}

type_init(register_host_display);
