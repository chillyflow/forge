#include "internal.h"
#include "forge/watch.h"

/* Only linked into forge_fallback_fixture, never the installed CLI/library.
 * Returning UNSUPPORTED selects the actual bounded filesystem-snapshot monitor.
 * This isolates scripted action ordering from asynchronous native delivery; it
 * does not suppress filesystem changes or bypass final validation. */
forge_watch_limits forge_default_watch_limits(void) {
    return (forge_watch_limits){1024, 1024u * 1024u, 4095, 4096, 64, 1000000, 8192};
}

forge_watch *forge_watch_create(const char *root, const forge_watch_limits *limits,
                                forge_cancel_fn cancelled, void *user, uint64_t timeout_ms,
                                forge_error *error) {
    (void)root;
    (void)limits;
    (void)cancelled;
    (void)user;
    (void)timeout_ms;
    fg_error(error, FORGE_ERR_UNSUPPORTED,
             "Native watcher intentionally unavailable in fallback test fixture");
    return NULL;
}

char *forge_watch_poll(forge_watch *watch, uint64_t timeout_ms, forge_cancel_fn cancelled,
                       void *user, forge_error *error) {
    (void)watch;
    (void)timeout_ms;
    (void)cancelled;
    (void)user;
    fg_error(error, FORGE_ERR_UNSUPPORTED, "Fallback test fixture has no native watcher");
    return NULL;
}

void forge_watch_invalidate(forge_watch *watch) {
    (void)watch;
}

void forge_watch_destroy(forge_watch *watch) {
    (void)watch;
}
