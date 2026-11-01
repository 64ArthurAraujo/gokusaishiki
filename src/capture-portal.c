/*
 * capture_portal.c - screen capture on Wayland (GNOME, KDE, ...) using the
 * xdg-desktop-portal ScreenCast interface and PipeWire.
 *
 * Flow:
 *   1. D-Bus (session bus), org.freedesktop.portal.ScreenCast:
 *        CreateSession -> SelectSources -> Start -> OpenPipeWireRemote
 *      Each of the first three answers asynchronously through a
 *      org.freedesktop.portal.Request "Response" signal.
 *   2. The compositor asks the user to share a screen (one-time: a restore
 *      token is saved in ~/.config/kbd_rgb/restore_token so later runs
 *      don't ask again, when the portal supports it).
 *   3. PipeWire: connect to the returned fd and receive video frames. Each
 *      frame (at most every FRAME_INTERVAL_MS) is reduced to a dominant
 *      color, kept as "latest". screen_capture_dominant_color() returns it.
 *
 * NOTE: must run as your normal user (the session D-Bus is not reachable
 * as root). See 99-kbd-rgb.rules for access to the keyboard without sudo.
 */

#define _POSIX_C_SOURCE 200809L

#include "capture-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib/gstdio.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

#define PORTAL_BUS     "org.freedesktop.portal.Desktop"
#define PORTAL_PATH    "/org/freedesktop/portal/desktop"
#define PORTAL_SC_IFACE "org.freedesktop.portal.ScreenCast"

#define FRAME_INTERVAL_MS  100     /* compute at most 10 colors per second */
#define FIRST_FRAME_WAIT_S 10

static bool g_debug = false;

static void dbg(const char *fmt, ...)
{
    if (!g_debug)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[portal] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

typedef struct {
    GDBusConnection *conn;
    char            *session_handle;

    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
    struct pw_stream      *stream;
    struct spa_hook        stream_listener;
    struct spa_video_info_raw vfmt;
    bool                   have_format;
    bool                   pw_inited;

    color_hist_t   *hist;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    rgb_t           latest;
    bool            have_frame;
    bool            failed;
    struct timespec last_compute;
} portal_t;

/* ======================= restore token ======================= */

static char *token_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "kbd_rgb", "restore_token", NULL);
}

static char *load_token(void)
{
    char *path = token_path();
    char *contents = NULL;

    if (!g_file_get_contents(path, &contents, NULL, NULL))
        contents = NULL;
    g_free(path);

    if (contents) {
        g_strstrip(contents);
        if (!*contents) {
            g_free(contents);
            contents = NULL;
        }
    }
    return contents;
}

static void save_token(const char *token)
{
    char *path = token_path();
    char *dir = g_path_get_dirname(path);
    GError *err = NULL;

    g_mkdir_with_parents(dir, 0700);
    if (!g_file_set_contents(path, token, -1, &err)) {
        fprintf(stderr, "Could not save restore token: %s\n", err->message);
        g_error_free(err);
    } else {
        chmod(path, 0600);
        dbg("saved restore token to %s", path);
    }
    g_free(dir);
    g_free(path);
}

/* ======================= portal requests ======================= */

typedef struct {
    bool      done;
    bool      timed_out;
    uint32_t  response;
    GVariant *results;
} req_t;

static void on_response(GDBusConnection *c, const gchar *sender,
                        const gchar *path, const gchar *iface,
                        const gchar *sig, GVariant *params, gpointer user)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)sig;
    req_t *r = user;

    g_variant_get(params, "(u@a{sv})", &r->response, &r->results);
    r->done = true;
}

static gboolean on_timeout(gpointer user)
{
    ((req_t *)user)->timed_out = true;
    return G_SOURCE_REMOVE;
}

static gboolean on_tick(gpointer user)
{
    (void)user;
    return G_SOURCE_CONTINUE;     /* just wakes the main loop periodically */
}

static char *make_token(void)
{
    static unsigned counter = 0;
    return g_strdup_printf("kbd_rgb_%u_%u", (unsigned)getpid(), counter++);
}

/*
 * Call a ScreenCast method that answers through a Request/Response signal
 * and wait for the answer. 'session' is NULL for CreateSession. For Start,
 * pass is_start=true (adds the parent_window argument).
 * On success returns 0 and *results holds the response dictionary.
 */
static int portal_request(portal_t *p, const char *method, const char *session,
                          bool is_start, GVariantBuilder *opts, int timeout_s,
                          GVariant **results)
{
    char *token = make_token();
    GError *err = NULL;
    int ret = -1;

    g_variant_builder_add(opts, "{sv}", "handle_token", g_variant_new_string(token));

    GVariant *params;
    if (!session)
        params = g_variant_new("(a{sv})", opts);
    else if (is_start)
        params = g_variant_new("(osa{sv})", session, "", opts);
    else
        params = g_variant_new("(oa{sv})", session, opts);

    /* The Request object path is predictable: subscribe before calling. */
    char *sender = g_strdup(g_dbus_connection_get_unique_name(p->conn) + 1);
    for (char *c = sender; *c; c++)
        if (*c == '.')
            *c = '_';
    char *req_path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s",
                                     sender, token);

    req_t req = {0};
    guint sub = g_dbus_connection_signal_subscribe(
        p->conn, PORTAL_BUS, "org.freedesktop.portal.Request", "Response",
        req_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_response, &req, NULL);

    dbg("%s -> waiting on %s", method, req_path);

    GVariant *reply = g_dbus_connection_call_sync(
        p->conn, PORTAL_BUS, PORTAL_PATH, PORTAL_SC_IFACE, method, params,
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);

    if (!reply) {
        fprintf(stderr, "ScreenCast.%s failed: %s\n", method, err->message);
        g_error_free(err);
        goto out;
    }
    g_variant_unref(reply);

    guint tmo  = g_timeout_add_seconds((guint)timeout_s, on_timeout, &req);
    guint tick = g_timeout_add(200, on_tick, NULL);

    while (!req.done && !req.timed_out && !CAPTURE_ABORTED())
        g_main_context_iteration(NULL, TRUE);

    g_source_remove(tick);
    if (!req.timed_out)
        g_source_remove(tmo);

    if (!req.done) {
        fprintf(stderr, req.timed_out ? "ScreenCast.%s: timed out\n"
                                      : "ScreenCast.%s: aborted\n", method);
        goto out;
    }
    if (req.response != 0) {
        fprintf(stderr, "ScreenCast.%s: %s (response %u)\n", method,
                req.response == 1 ? "cancelled by the user" : "failed",
                req.response);
        if (req.results)
            g_variant_unref(req.results);
        goto out;
    }

    *results = req.results;
    ret = 0;

out:
    if (sub)
        g_dbus_connection_signal_unsubscribe(p->conn, sub);
    g_free(req_path);
    g_free(sender);
    g_free(token);
    return ret;
}

static uint32_t portal_version(portal_t *p)
{
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_sync(
        p->conn, PORTAL_BUS, PORTAL_PATH, "org.freedesktop.DBus.Properties",
        "Get", g_variant_new("(ss)", PORTAL_SC_IFACE, "version"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);

    if (!r) {
        fprintf(stderr,
                "ScreenCast portal not available: %s\n"
                "Install xdg-desktop-portal and xdg-desktop-portal-gnome "
                "(or -kde / -wlr for other desktops).\n", err->message);
        g_error_free(err);
        return 0;
    }

    GVariant *v = NULL;
    g_variant_get(r, "(v)", &v);
    uint32_t version = g_variant_get_uint32(v);
    g_variant_unref(v);
    g_variant_unref(r);
    return version;
}

/* The session handle is documented as 'o' but some portals send 's'. */
static char *lookup_session_handle(GVariant *results)
{
    GVariant *v = g_variant_lookup_value(results, "session_handle", NULL);
    if (!v)
        return NULL;

    char *s = NULL;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ||
        g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH))
        s = g_strdup(g_variant_get_string(v, NULL));
    g_variant_unref(v);
    return s;
}

/* Runs the whole D-Bus dance. On success returns 0 and fills the outputs. */
static int portal_setup(portal_t *p, int *pw_fd, uint32_t *node_id)
{
    GError *err = NULL;
    GVariant *results = NULL;
    GVariantBuilder o;

    p->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!p->conn) {
        fprintf(stderr, "Cannot connect to the session D-Bus: %s\n", err->message);
        if (geteuid() == 0)
            fprintf(stderr,
                    "You are running as root. Run kbd_rgb as your normal user "
                    "(see 99-kbd-rgb.rules to allow USB access without sudo).\n");
        g_error_free(err);
        return -1;
    }

    uint32_t version = portal_version(p);
    if (version == 0)
        return -1;
    dbg("ScreenCast portal version %u", version);

    /* 1. CreateSession */
    char *session_token = make_token();
    g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&o, "{sv}", "session_handle_token",
                          g_variant_new_string(session_token));
    g_free(session_token);

    if (portal_request(p, "CreateSession", NULL, false, &o, 10, &results) != 0)
        return -1;
    p->session_handle = lookup_session_handle(results);
    g_variant_unref(results);
    if (!p->session_handle) {
        fprintf(stderr, "CreateSession: no session_handle in the response\n");
        return -1;
    }
    dbg("session %s", p->session_handle);

    /* 2. SelectSources: one monitor, remember the choice if possible */
    char *saved = load_token();
    g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&o, "{sv}", "types", g_variant_new_uint32(1));      /* monitor */
    g_variant_builder_add(&o, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    if (version >= 4) {
        g_variant_builder_add(&o, "{sv}", "persist_mode", g_variant_new_uint32(2));
        if (saved) {
            g_variant_builder_add(&o, "{sv}", "restore_token",
                                  g_variant_new_string(saved));
            dbg("using saved restore token");
        }
    }
    g_free(saved);

    if (portal_request(p, "SelectSources", p->session_handle, false, &o, 10,
                       &results) != 0)
        return -1;
    g_variant_unref(results);

    /* 3. Start: this is where the user may be asked to pick a screen */
    printf("Waiting for screen sharing permission (check for a dialog)...\n");
    fflush(stdout);
    g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
    if (portal_request(p, "Start", p->session_handle, true, &o, 120, &results) != 0)
        return -1;

    const char *new_token = NULL;
    if (g_variant_lookup(results, "restore_token", "&s", &new_token) &&
        new_token && *new_token)
        save_token(new_token);

    GVariantIter *it = NULL;
    GVariant *props = NULL;
    guint32 node = 0;
    if (!g_variant_lookup(results, "streams", "a(ua{sv})", &it) ||
        !g_variant_iter_next(it, "(u@a{sv})", &node, &props)) {
        fprintf(stderr, "Start: the portal returned no streams\n");
        if (it)
            g_variant_iter_free(it);
        g_variant_unref(results);
        return -1;
    }
    g_variant_unref(props);
    g_variant_iter_free(it);
    g_variant_unref(results);
    *node_id = node;
    dbg("PipeWire node %u", node);

    /* 4. OpenPipeWireRemote: gives us a file descriptor (synchronous call) */
    GUnixFDList *out_fds = NULL;
    g_variant_builder_init(&o, G_VARIANT_TYPE_VARDICT);
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(
        p->conn, PORTAL_BUS, PORTAL_PATH, PORTAL_SC_IFACE, "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", p->session_handle, &o), G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &out_fds, NULL, &err);
    if (!reply) {
        fprintf(stderr, "OpenPipeWireRemote failed: %s\n", err->message);
        g_error_free(err);
        return -1;
    }

    gint32 fd_index = -1;
    g_variant_get(reply, "(h)", &fd_index);
    g_variant_unref(reply);

    *pw_fd = g_unix_fd_list_get(out_fds, fd_index, &err);
    g_object_unref(out_fds);
    if (*pw_fd < 0) {
        fprintf(stderr, "Could not get the PipeWire fd: %s\n", err->message);
        g_error_free(err);
        return -1;
    }
    return 0;
}

/* ======================= PipeWire ======================= */

/* Byte offsets of R, G, B inside a pixel, for the formats we accept. */
static bool format_offsets(uint32_t fmt, int *r, int *g, int *b)
{
    switch (fmt) {
    case SPA_VIDEO_FORMAT_BGRx:
    case SPA_VIDEO_FORMAT_BGRA:
        *r = 2; *g = 1; *b = 0;
        return true;
    case SPA_VIDEO_FORMAT_RGBx:
    case SPA_VIDEO_FORMAT_RGBA:
        *r = 0; *g = 1; *b = 2;
        return true;
    default:
        return false;
    }
}

static long ms_since(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000L + (b->tv_nsec - a->tv_nsec) / 1000000L;
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    portal_t *p = data;
    struct spa_video_info info;

    if (!param || id != SPA_PARAM_Format)
        return;
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;
    if (info.media_type != SPA_MEDIA_TYPE_video ||
        info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0)
        return;

    p->vfmt = info.info.raw;
    p->have_format = true;
    dbg("video format %u, %ux%u", p->vfmt.format,
        p->vfmt.size.width, p->vfmt.size.height);
}

static void on_state_changed(void *data, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error)
{
    portal_t *p = data;

    dbg("stream state: %s -> %s%s%s", pw_stream_state_as_string(old),
        pw_stream_state_as_string(state), error ? " : " : "", error ? error : "");

    if (state == PW_STREAM_STATE_ERROR ||
        (state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_UNCONNECTED)) {
        pthread_mutex_lock(&p->lock);
        p->failed = true;
        pthread_cond_broadcast(&p->cond);
        pthread_mutex_unlock(&p->lock);
    }
}

static void on_process(void *data)
{
    portal_t *p = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(p->stream);

    if (!b)
        return;

    struct spa_buffer *buf = b->buffer;
    struct spa_data *d = &buf->datas[0];
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    bool ready = p->have_format && buf->n_datas > 0 && d->data && d->chunk &&
                 d->chunk->size > 0 &&
                 !(d->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED);

    if (ready && (!p->have_frame || ms_since(&p->last_compute, &now) >= FRAME_INTERVAL_MS)) {
        int w = (int)p->vfmt.size.width;
        int h = (int)p->vfmt.size.height;
        int stride = d->chunk->stride > 0 ? d->chunk->stride : w * 4;
        int ro, go, bo;

        if (w > 0 && h > 0 && format_offsets(p->vfmt.format, &ro, &go, &bo) &&
            d->chunk->size >= (uint32_t)(stride * (h - 1) + w * 4)) {
            const uint8_t *base = (const uint8_t *)d->data + d->chunk->offset;
            rgb_t c;

            hist_reset(p->hist);
            hist_add_frame32(p->hist, base, w, h, stride, ro, go, bo);

            if (hist_result(p->hist, &c) == 0) {
                pthread_mutex_lock(&p->lock);
                p->latest = c;
                p->have_frame = true;
                p->last_compute = now;
                pthread_cond_broadcast(&p->cond);
                pthread_mutex_unlock(&p->lock);
            }
        }
    }

    pw_stream_queue_buffer(p->stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process       = on_process,
};

static int pw_start(portal_t *p, int fd, uint32_t node_id)
{
    p->pw_inited = true;
    pw_init(NULL, NULL);

    p->loop = pw_thread_loop_new("kbd_rgb-pw", NULL);
    if (!p->loop)
        return -1;
    p->context = pw_context_new(pw_thread_loop_get_loop(p->loop), NULL, 0);
    if (!p->context)
        return -1;

    if (pw_thread_loop_start(p->loop) < 0)
        return -1;

    pw_thread_loop_lock(p->loop);

    p->core = pw_context_connect_fd(p->context, fd, NULL, 0);   /* owns fd */
    if (!p->core) {
        fprintf(stderr, "pw_context_connect_fd failed\n");
        pw_thread_loop_unlock(p->loop);
        return -1;
    }

    p->stream = pw_stream_new(p->core, "kbd_rgb",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                          PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Screen", NULL));
    if (!p->stream) {
        pw_thread_loop_unlock(p->loop);
        return -1;
    }
    pw_stream_add_listener(p->stream, &p->stream_listener, &stream_events, p);

    /* Ask for plain 32-bit RGB frames (no DMA-BUF modifiers: memory buffers). */
    uint8_t pod_buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
    const struct spa_pod *params[1];

    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
            SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(1920, 1080), &SPA_RECTANGLE(1, 1),
            &SPA_RECTANGLE(8192, 8192)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(30, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(60, 1)));

    int rc = pw_stream_connect(p->stream, PW_DIRECTION_INPUT, node_id,
                               PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                               params, 1);
    pw_thread_loop_unlock(p->loop);

    if (rc < 0) {
        fprintf(stderr, "pw_stream_connect failed: %s\n", strerror(-rc));
        return -1;
    }
    return 0;
}

/* ======================= backend interface ======================= */

static void portal_close(void *impl)
{
    portal_t *p = impl;
    if (!p)
        return;

    if (p->loop) {
        pw_thread_loop_lock(p->loop);
        if (p->stream)
            pw_stream_destroy(p->stream);
        if (p->core)
            pw_core_disconnect(p->core);
        if (p->context)
            pw_context_destroy(p->context);
        pw_thread_loop_unlock(p->loop);
        pw_thread_loop_stop(p->loop);
        pw_thread_loop_destroy(p->loop);
    } else if (p->context) {
        pw_context_destroy(p->context);
    }
    if (p->pw_inited)
        pw_deinit();

    if (p->conn) {
        if (p->session_handle)
            g_dbus_connection_call_sync(p->conn, PORTAL_BUS, p->session_handle,
                                        "org.freedesktop.portal.Session", "Close",
                                        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 1000,
                                        NULL, NULL);
        g_object_unref(p->conn);
    }

    g_free(p->session_handle);
    hist_free(p->hist);
    pthread_cond_destroy(&p->cond);
    pthread_mutex_destroy(&p->lock);
    free(p);
}

static void *portal_open(void)
{
    g_debug = getenv("KBD_RGB_DEBUG") != NULL;

    portal_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cond, &ca);
    pthread_condattr_destroy(&ca);

    p->hist = hist_new();
    if (!p->hist) {
        portal_close(p);
        return NULL;
    }

    int fd = -1;
    uint32_t node = 0;
    if (portal_setup(p, &fd, &node) != 0 || pw_start(p, fd, node) != 0) {
        portal_close(p);
        return NULL;
    }

    /* Wait for the first frame so dominant() has something to return. */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += FIRST_FRAME_WAIT_S;

    pthread_mutex_lock(&p->lock);
    while (!p->have_frame && !p->failed && !CAPTURE_ABORTED()) {
        struct timespec slice;
        clock_gettime(CLOCK_MONOTONIC, &slice);
        if (slice.tv_sec > deadline.tv_sec ||
            (slice.tv_sec == deadline.tv_sec && slice.tv_nsec >= deadline.tv_nsec))
            break;
        slice.tv_nsec += 200 * 1000000L;      /* wake up regularly to check abort */
        if (slice.tv_nsec >= 1000000000L) {
            slice.tv_sec++;
            slice.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&p->cond, &p->lock, &slice);
    }
    bool ok = p->have_frame;
    pthread_mutex_unlock(&p->lock);

    if (!ok) {
        fprintf(stderr, "No video frames received from the compositor "
                        "(try KBD_RGB_DEBUG=1 for details)\n");
        portal_close(p);
        return NULL;
    }
    return p;
}

static int portal_dominant(void *impl, rgb_t *out)
{
    portal_t *p = impl;
    int rc = -1;

    pthread_mutex_lock(&p->lock);
    if (p->have_frame && !p->failed) {
        *out = p->latest;
        rc = 0;
    } else if (p->failed) {
        fprintf(stderr, "Screen capture stream ended (sharing stopped?)\n");
    }
    pthread_mutex_unlock(&p->lock);
    return rc;
}

const capture_backend_t capture_backend_portal = {
    .name     = "portal",
    .open     = portal_open,
    .close    = portal_close,
    .dominant = portal_dominant,
};
