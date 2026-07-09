/* Drives the real arduino_* functions from scanner_app.c against a fake Nano.
 * Renames main() so we can link the translation unit under test. */
#define main scanner_app_main_unused
#include "scanner_app.c"
#undef main

int main(void)
{
    gchar *port = arduino_find_port();
    printf("find_port     -> %s\n", port ? port : "(none)");
    if (!port) return 2;

    gchar *err = NULL;
    int fd = arduino_open(port, &err);
    if (fd < 0) {
        printf("open          -> FAILED: %s\n", err);
        return 3;
    }
    printf("open          -> ok (fd=%d)\n", fd);

    gint64 t0 = g_get_monotonic_time();
    gchar *pong = arduino_ping(fd);
    long ms = (long)((g_get_monotonic_time() - t0) / 1000);

    printf("ping          -> %s   [%ld ms]\n", pong ? pong : "(TIMEOUT)", ms);

    int rc = pong ? 0 : 1;
    g_free(pong);
    close(fd);
    g_free(port);
    return rc;
}
