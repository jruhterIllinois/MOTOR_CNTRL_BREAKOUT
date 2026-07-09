/*
 * scanner_app.c — GTK3 flatbed scanner front-end
 *
 * Buttons:
 *   [Identify Scanner]  — runs "scanimage -L", parses the device string
 *   [Start Scan]        — runs "scanimage -d <device> -o /tmp/scan_out.pnm"
 *   [Stop Scan]         — sends SIGTERM to the scanimage process
 *
 * After a successful scan the PNM image is loaded into a scrollable
 * image widget for preview.
 *
 * Build:
 *   gcc $(pkg-config --cflags gtk+-3.0) \
 *       -o scanner_app scanner_app.c    \
 *       $(pkg-config --libs gtk+-3.0)   \
 *       -Wall -g
 */

#include <gtk/gtk.h>
#include <glib.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
#define SCAN_SUBDIR   "root scans"
#define MAX_PREVIEW_W 800
#define MAX_PREVIEW_H 640

/* Must match Serial.begin() in arduino_ping/arduino_ping.ino */
#define ARDUINO_BAUD      B115200
/* Opening the port pulls DTR, which resets the Nano into its bootloader.
 * It takes ~1.5 s to hand off to the sketch, so re-send PING for a while
 * instead of assuming the first one is heard. */
#define ARDUINO_ATTEMPTS  12
#define ARDUINO_WAIT_MS   300

/* ------------------------------------------------------------------ */
typedef struct {
    /* Widgets */
    GtkWidget *window;
    GtkWidget *identify_btn;
    GtkWidget *scan_btn;
    GtkWidget *stop_btn;
    GtkWidget *clear_calib_btn;
    GtkWidget *arduino_btn;
    GtkWidget *spinner;
    GtkWidget *device_label;
    GtkWidget *arduino_label;
    GtkWidget *status_label;
    GtkWidget *image_widget;

    /* State */
    gchar    *device_id;    /* e.g. "genesys:libusb:003:002" */
    gchar    *output_file;  /* path of the current/last scan */
    GPid      scan_pid;
    gboolean  scanning;

    int       arduino_fd;   /* -1 when closed — NOT 0, that's stdin */
    gchar    *arduino_port;
    gboolean  connecting;   /* TRUE while on_arduino_clicked pumps events */
} AppState;

static AppState app;   /* zero-initialised at startup */

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void set_status(const gchar *msg)
{
    gtk_label_set_text(GTK_LABEL(app.status_label), msg);
}

/*
 * Build ~/Desktop/root scans/<unix_timestamp>.pnm, creating the directory
 * if it doesn't exist.  Returns a newly-allocated string (caller must g_free).
 * Returns NULL on error.
 */
static gchar *make_output_path(void)
{
    gchar *dir = g_build_filename(g_get_home_dir(), "Desktop", SCAN_SUBDIR, NULL);

    if (g_mkdir_with_parents(dir, 0755) != 0) {
        gchar *msg = g_strdup_printf("Could not create output directory: %s", dir);
        set_status(msg);
        g_free(msg);
        g_free(dir);
        return NULL;
    }

    gchar *path = g_strdup_printf("%s/%ld.pnm", dir, (long)time(NULL));
    g_free(dir);
    return path;
}

/*
 * Parse the first backtick-quoted device string from "scanimage -L" output.
 *
 * Example line:
 *   device `genesys:libusb:003:002' is a CANON LiDE 400 flatbed scanner
 */
static gchar *parse_first_device(const gchar *text)
{
    if (!text || *text == '\0')
        return NULL;

    const gchar *tick = strchr(text, '`');
    if (!tick) return NULL;
    tick++;  /* skip the backtick */

    const gchar *end = strchr(tick, '\'');
    if (!end || end == tick) return NULL;

    return g_strndup(tick, (gsize)(end - tick));
}

/* Load the scanned file and show it in the image widget. */
static void load_and_show_image(const gchar *path)
{
    GError   *err = NULL;
    GdkPixbuf *pb  = gdk_pixbuf_new_from_file(path, &err);

    if (!pb) {
        gchar *msg = g_strdup_printf(
            "Could not load scanned image: %s",
            err ? err->message : "unknown error");
        set_status(msg);
        g_free(msg);
        if (err) g_error_free(err);
        return;
    }

    int orig_w = gdk_pixbuf_get_width(pb);
    int orig_h = gdk_pixbuf_get_height(pb);

    /* Scale down for preview while keeping aspect ratio */
    if (orig_w > MAX_PREVIEW_W || orig_h > MAX_PREVIEW_H) {
        double scale = MIN((double)MAX_PREVIEW_W / orig_w,
                           (double)MAX_PREVIEW_H / orig_h);
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(
            pb,
            (int)(orig_w * scale),
            (int)(orig_h * scale),
            GDK_INTERP_BILINEAR);
        g_object_unref(pb);
        pb = scaled;
    }

    gtk_image_set_from_pixbuf(GTK_IMAGE(app.image_widget), pb);
    g_object_unref(pb);

    gchar *msg = g_strdup_printf(
        "Scan complete  (%d × %d px original)  —  saved to %s",
        orig_w, orig_h, path);
    set_status(msg);
    g_free(msg);
}

/* ------------------------------------------------------------------ */
/* Child-watch callback — called when scanimage exits                  */
/* ------------------------------------------------------------------ */

static void on_scan_process_done(GPid pid, gint status, gpointer user_data)
{
    (void)user_data;

    g_spawn_close_pid(pid);
    app.scan_pid = 0;
    app.scanning = FALSE;

    gtk_spinner_stop(GTK_SPINNER(app.spinner));

    gtk_widget_set_sensitive(app.identify_btn,    TRUE);
    gtk_widget_set_sensitive(app.scan_btn,        TRUE);
    gtk_widget_set_sensitive(app.stop_btn,        FALSE);
    gtk_widget_set_sensitive(app.clear_calib_btn, TRUE);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        load_and_show_image(app.output_file);
    } else {
        set_status("Scan stopped or failed.");
    }
}

/* ------------------------------------------------------------------ */
/* Arduino serial link                                                  */
/* ------------------------------------------------------------------ */

/*
 * First match of /dev/ttyUSB* then /dev/ttyACM*.  Nano v3 clones carry a
 * CH340 and land on ttyUSB0; genuine boards enumerate as ttyACM0.
 * Override with  SCANNER_APP_ARDUINO_PORT=/dev/ttyWhatever.
 * Caller must g_free the result.  NULL if nothing looks like a serial port.
 */
static gchar *arduino_find_port(void)
{
    const gchar *env = g_getenv("SCANNER_APP_ARDUINO_PORT");
    if (env && *env)
        return g_strdup(env);

    static const char *patterns[] = { "/dev/ttyUSB*", "/dev/ttyACM*", NULL };

    for (int i = 0; patterns[i]; i++) {
        glob_t gl;
        int rc = glob(patterns[i], 0, NULL, &gl);

        if (rc == 0 && gl.gl_pathc > 0) {
            gchar *port = g_strdup(gl.gl_pathv[0]);
            globfree(&gl);
            return port;
        }
        /* glob() may allocate on any return except GLOB_NOSPACE; free it in
         * every case so a NOMATCH/ABORTED path doesn't leak. */
        if (rc != GLOB_NOSPACE)
            globfree(&gl);
    }
    return NULL;
}

/*
 * Open the port in raw mode.  Returns the fd, or -1 with *err_out set to a
 * newly-allocated message.  The fd stays O_NONBLOCK so reads return EAGAIN
 * rather than parking the GTK main loop.
 */
static int arduino_open(const gchar *port, gchar **err_out)
{
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        /* EACCES here almost always means the user isn't in group dialout. */
        *err_out = g_strdup_printf("%s: %s%s", port, g_strerror(errno),
                                   errno == EACCES
                                     ? "  (add yourself to group 'dialout',"
                                       " then log out and back in)"
                                     : "");
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        *err_out = g_strdup_printf("%s is not a serial port: %s",
                                   port, g_strerror(errno));
        close(fd);
        return -1;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, ARDUINO_BAUD);
    cfsetospeed(&tio, ARDUINO_BAUD);

    tio.c_cflag |=  (CLOCAL | CREAD);   /* ignore modem lines, enable receiver */
    tio.c_cflag &= ~CRTSCTS;            /* no hardware flow control            */
    tio.c_cflag &= ~HUPCL;              /* hold DTR on close -> no reset next open */
    tio.c_cc[VMIN]  = 0;                /* poll() owns the timing, not termios */
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        *err_out = g_strdup_printf("Could not configure %s: %s",
                                   port, g_strerror(errno));
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);   /* drop bootloader chatter */
    return fd;
}

/* Pull the PONG line out of whatever the Nano sent.  Caller g_frees. */
static gchar *extract_pong(const gchar *buf)
{
    const gchar *p = strstr(buf, "PONG");
    if (!p) return NULL;

    const gchar *end = strpbrk(p, "\r\n");
    return end ? g_strndup(p, (gsize)(end - p)) : g_strdup(p);
}

/*
 * Send PING until a PONG comes back or we run out of attempts.
 * Pumps the GTK event queue while waiting so the window keeps redrawing —
 * same trick on_identify_clicked() uses around its blocking g_spawn_sync().
 *
 * Returns the PONG payload (caller g_frees) or NULL on timeout/error.
 */
static gchar *arduino_ping(int fd)
{
    GString *rx = g_string_new(NULL);
    gchar   *pong = NULL;

    for (int attempt = 0; attempt < ARDUINO_ATTEMPTS && !pong; attempt++) {
        if (write(fd, "PING\n", 5) != 5)
            break;

        gint64 deadline = g_get_monotonic_time() + ARDUINO_WAIT_MS * 1000;

        while (!pong) {
            gint64 left_us = deadline - g_get_monotonic_time();
            if (left_us <= 0)
                break;

            struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
            /* Cap the slice at 50 ms so the UI stays responsive. */
            int pr = poll(&pfd, 1, MIN((int)(left_us / 1000), 50));

            while (gtk_events_pending())
                gtk_main_iteration();

            if (pr < 0) {
                if (errno == EINTR) continue;
                goto done;
            }
            if (pr == 0)
                continue;   /* slice expired, still inside this attempt */

            char chunk[128];
            ssize_t n = read(fd, chunk, sizeof chunk);
            if (n > 0) {
                g_string_append_len(rx, chunk, n);
                pong = extract_pong(rx->str);
            } else if (n == 0) {
                goto done;  /* port vanished — cable pulled */
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                goto done;
            }
        }
    }

done:
    g_string_free(rx, TRUE);
    return pong;
}

static void arduino_disconnect(void)
{
    if (app.arduino_fd >= 0) {
        close(app.arduino_fd);
        app.arduino_fd = -1;
    }
    g_clear_pointer(&app.arduino_port, g_free);
}

/* ------------------------------------------------------------------ */
/* Button callbacks                                                     */
/* ------------------------------------------------------------------ */

static void on_arduino_clicked(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;

    /* Already up?  Then this click means disconnect. */
    if (app.arduino_fd >= 0) {
        arduino_disconnect();
        gtk_button_set_label(GTK_BUTTON(app.arduino_btn), "Connect Arduino");
        gtk_label_set_text(GTK_LABEL(app.arduino_label), "Arduino:  (not connected)");
        set_status("Arduino disconnected.");
        return;
    }

    gchar *port = arduino_find_port();
    if (!port) {
        set_status("No serial port found.  Is the Nano plugged in?  "
                   "(looked for /dev/ttyUSB* and /dev/ttyACM*)");
        return;
    }

    gchar *msg = g_strdup_printf("Opening %s …", port);
    set_status(msg);
    g_free(msg);

    /* From here on we pump the event queue by hand; block window-close so the
     * widgets we touch afterwards can't be destroyed underneath us. */
    app.connecting = TRUE;
    gtk_widget_set_sensitive(app.arduino_btn, FALSE);
    while (gtk_events_pending())
        gtk_main_iteration();

    gchar *oerr = NULL;
    int fd = arduino_open(port, &oerr);
    if (fd < 0) {
        set_status(oerr);
        g_free(oerr);
        g_free(port);
        gtk_widget_set_sensitive(app.arduino_btn, TRUE);
        app.connecting = FALSE;
        return;
    }

    set_status("Waiting for the Nano to finish resetting, then pinging …");
    while (gtk_events_pending())
        gtk_main_iteration();

    gchar *pong = arduino_ping(fd);

    /* No more event pumping past this point, so the window is safe to close. */
    app.connecting = FALSE;
    gtk_widget_set_sensitive(app.arduino_btn, TRUE);

    if (!pong) {
        close(fd);
        msg = g_strdup_printf(
            "Opened %s but got no PONG.  Check the baud rate matches the "
            "sketch (115200), and that arduino_ping.ino is actually flashed.",
            port);
        set_status(msg);
        g_free(msg);
        g_free(port);
        return;
    }

    app.arduino_fd   = fd;
    app.arduino_port = port;   /* ownership moves into app state */

    gchar *alabel = g_strdup_printf("Arduino:  %s  —  %s", port, pong);
    gtk_label_set_text(GTK_LABEL(app.arduino_label), alabel);
    g_free(alabel);

    gtk_button_set_label(GTK_BUTTON(app.arduino_btn), "Disconnect Arduino");
    set_status("Arduino is connected and alive.");
    g_free(pong);
}

static void on_identify_clicked(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;

    set_status("Running  scanimage -L  …");
    gtk_widget_set_sensitive(app.identify_btn, FALSE);

    /* Flush UI before the blocking call */
    while (gtk_events_pending())
        gtk_main_iteration();

    gchar   *out_buf = NULL, *err_buf = NULL;
    gint     exit_st = 0;
    GError  *err     = NULL;
    gchar   *argv[]  = { "scanimage", "-L", NULL };

    gboolean ok = g_spawn_sync(
        NULL, argv, NULL,
        G_SPAWN_SEARCH_PATH,
        NULL, NULL,
        &out_buf, &err_buf,
        &exit_st, &err);

    gtk_widget_set_sensitive(app.identify_btn, TRUE);

    if (!ok) {
        gchar *msg = g_strdup_printf("Failed to run scanimage: %s",
                                     err ? err->message : "not found in PATH");
        set_status(msg);
        g_free(msg);
        if (err) g_error_free(err);
        g_free(out_buf); g_free(err_buf);
        return;
    }
    if (err) g_error_free(err);

    /* Try stdout first; some sane-backends versions print to stderr */
    const gchar *search_in =
        (out_buf && *out_buf) ? out_buf : (err_buf ? err_buf : "");

    gchar *dev = parse_first_device(search_in);
    g_free(out_buf);
    g_free(err_buf);

    if (!dev) {
        set_status("No scanner detected.  Check USB cable / sane-backends.");
        gtk_label_set_text(GTK_LABEL(app.device_label), "Device: (none)");
        gtk_widget_set_sensitive(app.scan_btn, FALSE);
        return;
    }

    g_free(app.device_id);
    app.device_id = dev;

    gchar *dlabel = g_strdup_printf("Device:  %s", dev);
    gtk_label_set_text(GTK_LABEL(app.device_label), dlabel);
    g_free(dlabel);

    set_status("Scanner identified.  Press  [Start Scan].");
    gtk_widget_set_sensitive(app.scan_btn,        TRUE);
    gtk_widget_set_sensitive(app.clear_calib_btn, TRUE);
}

static void on_start_scan_clicked(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;

    if (!app.device_id) {
        set_status("No device selected — click  [Identify Scanner]  first.");
        return;
    }

    g_free(app.output_file);
    app.output_file = make_output_path();
    if (!app.output_file)
        return;

    GError *err    = NULL;
    /*
     * scanimage -d <device> -o <file>
     * The output format is inferred from the extension; .pnm → PNM/PPM.
     * Add --format=pnm explicitly for newer sane-backends versions
     * that require it when writing to a file.
     */
    gchar *argv[] = {
        "scanimage",
        "-d", app.device_id,
        "--format=pnm",
        "-o", app.output_file,
        NULL
    };

    gboolean ok = g_spawn_async(
        NULL, argv, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL,
        &app.scan_pid, &err);

    if (!ok) {
        gchar *msg = g_strdup_printf("Could not start scanimage: %s",
                                     err ? err->message : "unknown");
        set_status(msg);
        g_free(msg);
        if (err) g_error_free(err);
        return;
    }
    if (err) g_error_free(err);

    app.scanning = TRUE;
    set_status("Scanning …  please wait.");
    gtk_spinner_start(GTK_SPINNER(app.spinner));

    gtk_widget_set_sensitive(app.identify_btn,    FALSE);
    gtk_widget_set_sensitive(app.scan_btn,        FALSE);
    gtk_widget_set_sensitive(app.stop_btn,        TRUE);
    gtk_widget_set_sensitive(app.clear_calib_btn, FALSE);

    g_child_watch_add(app.scan_pid, on_scan_process_done, NULL);
}

static void on_clear_calibration_clicked(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;

    if (!app.device_id) {
        set_status("No device selected — click  [Identify Scanner]  first.");
        return;
    }

    gchar *argv[] = {
        "scanimage",
        "-d", app.device_id,
        "--clear-calibration",
        NULL
    };

    GError *err = NULL;
    gint exit_st = 0;
    gboolean ok = g_spawn_sync(
        NULL, argv, NULL,
        G_SPAWN_SEARCH_PATH,
        NULL, NULL,
        NULL, NULL,
        &exit_st, &err);

    if (!ok) {
        gchar *msg = g_strdup_printf("Failed to clear calibration: %s",
                                     err ? err->message : "unknown");
        set_status(msg);
        g_free(msg);
        if (err) g_error_free(err);
        return;
    }
    if (err) g_error_free(err);

    set_status("Calibration cache cleared.  Next scan will recalibrate.");
}

static void on_stop_scan_clicked(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;

    if (app.scan_pid > 0) {
        kill(app.scan_pid, SIGTERM);
        set_status("Stopping scan …");
    }
}

/*
 * Veto the window close while on_arduino_clicked() is pumping the event queue
 * by hand.  Without this, closing the window mid-connect destroys the widgets,
 * and the set_sensitive()/set_text() calls after arduino_ping() returns would
 * touch freed memory.  The connect finishes in a few seconds and the user can
 * close normally after that.
 */
static gboolean on_delete_event(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    if (app.connecting) {
        set_status("Finishing Arduino connection — please wait a moment, "
                   "then close the window.");
        return TRUE;   /* stop the default handler from destroying the window */
    }
    return FALSE;      /* allow the normal close */
}

/* ------------------------------------------------------------------ */
/* UI construction                                                      */
/* ------------------------------------------------------------------ */

static void build_ui(void)
{
    /* ---- Window ---- */
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Scanner App");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 980, 780);
    g_signal_connect(app.window, "delete-event",
                     G_CALLBACK(on_delete_event), NULL);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* ---- Root container ---- */
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(root), 10);
    gtk_container_add(GTK_CONTAINER(app.window), root);

    /* ---- Header / button bar ---- */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);

    app.identify_btn    = gtk_button_new_with_label("Identify Scanner");
    app.scan_btn        = gtk_button_new_with_label("Start Scan");
    app.stop_btn        = gtk_button_new_with_label("Stop Scan");
    app.clear_calib_btn = gtk_button_new_with_label("Reset Calibration");
    app.arduino_btn     = gtk_button_new_with_label("Connect Arduino");
    app.spinner         = gtk_spinner_new();

    gtk_widget_set_sensitive(app.scan_btn,        FALSE);
    gtk_widget_set_sensitive(app.stop_btn,        FALSE);
    gtk_widget_set_sensitive(app.clear_calib_btn, FALSE);
    /* The Arduino link is independent of the scanner — no need to identify first. */

    gtk_box_pack_start(GTK_BOX(header), app.identify_btn,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), app.scan_btn,        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), app.stop_btn,        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), app.clear_calib_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), app.arduino_btn,     FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), app.spinner,         FALSE, FALSE, 8);

    g_signal_connect(app.identify_btn,    "clicked",
                     G_CALLBACK(on_identify_clicked),          NULL);
    g_signal_connect(app.scan_btn,        "clicked",
                     G_CALLBACK(on_start_scan_clicked),        NULL);
    g_signal_connect(app.stop_btn,        "clicked",
                     G_CALLBACK(on_stop_scan_clicked),         NULL);
    g_signal_connect(app.clear_calib_btn, "clicked",
                     G_CALLBACK(on_clear_calibration_clicked), NULL);
    g_signal_connect(app.arduino_btn,     "clicked",
                     G_CALLBACK(on_arduino_clicked),           NULL);

    /* ---- Info labels ---- */
    app.device_label = gtk_label_new("Device:  (none)");
    gtk_label_set_xalign(GTK_LABEL(app.device_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app.device_label, FALSE, FALSE, 0);

    app.arduino_label = gtk_label_new("Arduino:  (not connected)");
    gtk_label_set_xalign(GTK_LABEL(app.arduino_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app.arduino_label, FALSE, FALSE, 0);

    app.status_label = gtk_label_new(
        "Click  [Identify Scanner]  to detect your USB scanner.");
    gtk_label_set_xalign(GTK_LABEL(app.status_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(app.status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(root), app.status_label, FALSE, FALSE, 0);

    /* ---- Divider ---- */
    gtk_box_pack_start(GTK_BOX(root),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 4);

    /* ---- Scrollable image area ---- */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    /*
     * GtkImage doesn't scroll itself; wrap it in a GtkViewport so the
     * scrolled-window can manage it properly.
     */
    GtkWidget *vp = gtk_viewport_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), vp);

    app.image_widget = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(vp), app.image_widget);
}

/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);
    memset(&app, 0, sizeof(app));
    app.arduino_fd = -1;   /* memset left it at 0, which is a valid fd (stdin) */

    build_ui();
    gtk_widget_show_all(app.window);
    gtk_main();

    arduino_disconnect();
    g_free(app.device_id);
    g_free(app.output_file);
    return 0;
}
