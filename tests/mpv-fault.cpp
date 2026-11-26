// SPDX-License-Identifier: LGPL-3.0-only
#include <cstdlib>
#include <cstring>
#include <mpv/client.h>
#include <mpv/render.h>
#include <unistd.h>

namespace {
bool mode(const char *name) {
    const auto *value = std::getenv("IMV_MPV_FAULT_MODE");
    return value && std::strcmp(value, name) == 0;
}
} // namespace

unsigned long mpv_client_api_version() { return MPV_MAKE_VERSION(2, 0); }
mpv_handle *mpv_create() {
    if (mode("watchdog"))
        for (;;)
            ::pause();
    if (mode("fault"))
        std::abort();
    static int handle;
    return reinterpret_cast<mpv_handle *>(&handle);
}
int mpv_initialize(mpv_handle *) { return mode("initialize") ? MPV_ERROR_UNSUPPORTED : 0; }
void mpv_terminate_destroy(mpv_handle *) {}
int mpv_set_option_string(mpv_handle *, const char *name, const char *) {
    return mode("options") && std::strcmp(name, "access-references") == 0
               ? MPV_ERROR_OPTION_NOT_FOUND
               : 0;
}
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
