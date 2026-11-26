// SPDX-License-Identifier: LGPL-3.0-only
#include <cstdlib>
#include <mpv/client.h>
#include <mpv/render.h>
#include <unistd.h>

unsigned long mpv_client_api_version() { return MPV_MAKE_VERSION(2, 0); }
mpv_handle *mpv_create() {
    if (std::getenv("IMV_MPV_FAULT_HANG"))
        for (;;)
            ::pause();
    std::abort();
}
int mpv_initialize(mpv_handle *) { return MPV_ERROR_GENERIC; }
void mpv_terminate_destroy(mpv_handle *) {}
int mpv_set_option_string(mpv_handle *, const char *, const char *) { return MPV_ERROR_GENERIC; }
int mpv_command_async(mpv_handle *, uint64_t, const char **) { return MPV_ERROR_GENERIC; }
mpv_event *mpv_wait_event(mpv_handle *, double) { return nullptr; }
int mpv_get_property(mpv_handle *, const char *, mpv_format, void *) { return MPV_ERROR_GENERIC; }
int mpv_render_context_create(mpv_render_context **, mpv_handle *, mpv_render_param *) {
    return MPV_ERROR_GENERIC;
}
void mpv_render_context_free(mpv_render_context *) {}
uint64_t mpv_render_context_update(mpv_render_context *) { return 0; }
int mpv_render_context_render(mpv_render_context *, mpv_render_param *) {
    return MPV_ERROR_GENERIC;
}
