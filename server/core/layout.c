/*
 * Layout restore by monitor identity.
 *
 * mutter remembers one display configuration per *set of monitors*
 * (~/.config/monitors.xml), but its key includes each monitor's connector
 * name (meta_monitor_spec_equals: connector, vendor, product, serial),
 * looked up exactly. A client's virtual screen keeps its EDID identity
 * (serial derived from the MAC), yet its connector changes whenever the
 * evdi card does (DVI-I-13 today, DVI-I-19 after a mode switch that
 * recreated the cards): mutter then finds no match and falls back to its
 * default layout — position, primary, refresh rate, "only this screen"
 * all forgotten.
 *
 * So once per connection, when the client's connector shows up in
 * mutter's state, the server looks in monitors.xml for a configuration of
 * the same monitors where only this client's connector name differs; if
 * mutter did not already apply an exact match, it re-applies that
 * configuration with the connector renamed, through
 * org.gnome.Mutter.DisplayConfig.ApplyMonitorsConfig (persistent: mutter
 * stores the renamed copy too). The other monitors must match exactly.
 * Several remembered configurations can qualify (one per connector name
 * this screen ever had), and mutter writes them in hash-table order, so
 * the server notes the connector each identity last streamed on
 * ($XDG_STATE_HOME/sremfb/last-connectors): that one's configuration is
 * the most recent, since mutter stored whatever was current under it.
 *
 * SREMFB_LAYOUT=0 turns it off.
 */
#include <gio/gio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

#define DC_NAME  "org.gnome.Mutter.DisplayConfig"
#define DC_PATH  "/org/gnome/Mutter/DisplayConfig"
#define STATE_T  "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})"

/* ------------------------------------------------ tiny XML tree */

typedef struct XNode XNode;
struct XNode {
    char *name;
    GString *text;
    GPtrArray *kids;                   /* XNode* */
    XNode *parent;
};

static XNode *xnode_new(const char *name, XNode *parent)
{
    XNode *n = g_new0(XNode, 1);
    n->name = g_strdup(name);
    n->text = g_string_new(NULL);
    n->kids = g_ptr_array_new();
    n->parent = parent;
    if (parent)
        g_ptr_array_add(parent->kids, n);
    return n;
}

static void xnode_free(XNode *n)
{
    if (!n)
        return;
    for (guint i = 0; i < n->kids->len; i++)
        xnode_free(g_ptr_array_index(n->kids, i));
    g_ptr_array_free(n->kids, TRUE);
    g_string_free(n->text, TRUE);
    g_free(n->name);
    g_free(n);
}

static void x_start(GMarkupParseContext *ctx, const char *name,
                    const char **an, const char **av, gpointer data,
                    GError **err)
{
    XNode **cur = data;
    *cur = xnode_new(name, *cur);
}

static void x_end(GMarkupParseContext *ctx, const char *name, gpointer data,
                  GError **err)
{
    XNode **cur = data;
    if ((*cur)->parent)
        *cur = (*cur)->parent;
}

static void x_text(GMarkupParseContext *ctx, const char *text, gsize len,
                   gpointer data, GError **err)
{
    XNode **cur = data;
    g_string_append_len((*cur)->text, text, (gssize)len);
}

static XNode *xml_load(const char *path)
{
    static const GMarkupParser p = { x_start, x_end, x_text, NULL, NULL };
    gchar *txt = NULL;
    gsize len;
    XNode *root, *cur;
    GMarkupParseContext *ctx;
    gboolean ok;

    if (!g_file_get_contents(path, &txt, &len, NULL))
        return NULL;
    root = cur = xnode_new("#root", NULL);
    ctx = g_markup_parse_context_new(&p, 0, &cur, NULL);
    ok = g_markup_parse_context_parse(ctx, txt, (gssize)len, NULL) &&
         g_markup_parse_context_end_parse(ctx, NULL);
    g_markup_parse_context_free(ctx);
    g_free(txt);
    if (!ok) {
        xnode_free(root);
        return NULL;
    }
    return root;
}

static XNode *xchild(const XNode *n, const char *name)
{
    for (guint i = 0; n && i < n->kids->len; i++) {
        XNode *k = g_ptr_array_index(n->kids, i);
        if (strcmp(k->name, name) == 0)
            return k;
    }
    return NULL;
}

static const char *xtext(const XNode *n, const char *name)
{
    XNode *k = xchild(n, name);
    return k ? g_strstrip(k->text->str) : NULL;
}

/* ------------------------------------------------ current state */

typedef struct {
    char *connector, *vendor, *product, *serial;
    GVariant *modes;                   /* a(siiddada{sv}) */
} Mon;

typedef struct {
    guint serial;
    guint layout_mode;
    GPtrArray *mons;                   /* Mon* */
} State;

static void mon_free(gpointer p)
{
    Mon *m = p;
    g_free(m->connector);
    g_free(m->vendor);
    g_free(m->product);
    g_free(m->serial);
    g_variant_unref(m->modes);
    g_free(m);
}

static void state_parse(GVariant *res, State *st)
{
    GVariant *mons, *logs, *props;
    GVariantIter it;
    GVariant *v;

    st->layout_mode = 1;
    st->mons = g_ptr_array_new_with_free_func(mon_free);
    g_variant_get(res, "(u@a((ssss)a(siiddada{sv})a{sv})"
                  "@a(iiduba(ssss)a{sv})@a{sv})",
                  &st->serial, &mons, &logs, &props);
    g_variant_lookup(props, "layout-mode", "u", &st->layout_mode);
    g_variant_iter_init(&it, mons);
    while ((v = g_variant_iter_next_value(&it))) {
        Mon *m = g_new0(Mon, 1);
        g_variant_get(v, "((ssss)@a(siiddada{sv})@a{sv})", &m->connector,
                      &m->vendor, &m->product, &m->serial, &m->modes, NULL);
        g_ptr_array_add(st->mons, m);
        g_variant_unref(v);
    }
    g_variant_unref(mons);
    g_variant_unref(logs);
    g_variant_unref(props);
}

/* ------------------------------------------------ matching */

static gboolean spec_is(const XNode *spec, const Mon *m, gboolean connector)
{
    return g_strcmp0(xtext(spec, "vendor"), m->vendor) == 0 &&
           g_strcmp0(xtext(spec, "product"), m->product) == 0 &&
           g_strcmp0(xtext(spec, "serial"), m->serial) == 0 &&
           (!connector || g_strcmp0(xtext(spec, "connector"),
                                    m->connector) == 0);
}

/* Every monitorspec of a <configuration> (enabled and disabled). */
static void config_specs(const XNode *cfg, GPtrArray *out)
{
    for (guint i = 0; i < cfg->kids->len; i++) {
        XNode *k = g_ptr_array_index(cfg->kids, i);
        if (strcmp(k->name, "logicalmonitor") == 0) {
            for (guint j = 0; j < k->kids->len; j++) {
                XNode *mon = g_ptr_array_index(k->kids, j);
                if (strcmp(mon->name, "monitor") == 0 &&
                    xchild(mon, "monitorspec"))
                    g_ptr_array_add(out, xchild(mon, "monitorspec"));
            }
        } else if (strcmp(k->name, "disabled") == 0) {
            for (guint j = 0; j < k->kids->len; j++) {
                XNode *s = g_ptr_array_index(k->kids, j);
                if (strcmp(s->name, "monitorspec") == 0)
                    g_ptr_array_add(out, s);
            }
        }
    }
}

/* 2 = exact match, 1 = match but for `ours`' connector, 0 = no. */
static int config_match(const XNode *cfg, const State *st, const Mon *ours)
{
    const char *lm = xtext(cfg, "layoutmode");
    guint mode = lm && strcmp(lm, "physical") == 0 ? 2 : 1;
    GPtrArray *specs = g_ptr_array_new();
    int result = 0;

    if (mode != st->layout_mode)
        goto out;
    config_specs(cfg, specs);
    if (specs->len != st->mons->len)
        goto out;
    result = 2;
    for (guint i = 0; i < st->mons->len && result; i++) {
        const Mon *m = g_ptr_array_index(st->mons, i);
        int found = 0;
        for (guint j = 0; j < specs->len && found < 2; j++) {
            const XNode *s = g_ptr_array_index(specs, j);
            if (spec_is(s, m, TRUE))
                found = 2;
            else if (m == ours && spec_is(s, m, FALSE))
                found = 1;
        }
        result = MIN(result, found);
    }
out:
    g_ptr_array_free(specs, TRUE);
    return result;
}

static const Mon *mon_by_spec(const State *st, const XNode *spec,
                              const Mon *ours)
{
    for (guint i = 0; i < st->mons->len; i++) {
        const Mon *m = g_ptr_array_index(st->mons, i);
        if (spec_is(spec, m, m != ours))
            return m;
    }
    return NULL;
}

/* The mode id of m for a stored <mode>: same size, closest rate. */
static char *mode_id(const Mon *m, const XNode *mode)
{
    const char *w = xtext(mode, "width"), *h = xtext(mode, "height");
    const char *r = xtext(mode, "rate");
    double rate = r ? g_ascii_strtod(r, NULL) : 60.0, best = 1e9;
    char *id = NULL;
    GVariantIter it;
    const char *mid;
    int mw, mh;
    double mr;

    if (!w || !h)
        return NULL;
    g_variant_iter_init(&it, m->modes);
    while (g_variant_iter_next(&it, "(&siid@d@ad@a{sv})", &mid, &mw, &mh,
                               &mr, NULL, NULL, NULL)) {
        if (mw == atoi(w) && mh == atoi(h) && fabs(mr - rate) < best) {
            best = fabs(mr - rate);
            g_free(id);
            id = g_strdup(mid);
        }
    }
    return id;
}

static guint transform_of(const XNode *lm)
{
    XNode *t = xchild(lm, "transform");
    const char *rot = t ? xtext(t, "rotation") : NULL;
    const char *fl = t ? xtext(t, "flipped") : NULL;
    guint v = 0;

    if (rot && strcmp(rot, "left") == 0)
        v = 1;
    else if (rot && strcmp(rot, "upside_down") == 0)
        v = 2;
    else if (rot && strcmp(rot, "right") == 0)
        v = 3;
    if (fl && strcmp(fl, "yes") == 0)
        v += 4;
    return v;
}

/* The ApplyMonitorsConfig logical monitors for cfg on the current
 * monitors, NULL when a monitor or mode cannot be resolved. */
static GVariant *build_logical(const XNode *cfg, const State *st,
                               const Mon *ours)
{
    GVariantBuilder lms;

    g_variant_builder_init(&lms, G_VARIANT_TYPE("a(iiduba(ssa{sv}))"));
    for (guint i = 0; i < cfg->kids->len; i++) {
        XNode *lm = g_ptr_array_index(cfg->kids, i);
        GVariantBuilder mons;
        const char *x, *y, *sc, *pr;

        if (strcmp(lm->name, "logicalmonitor") != 0)
            continue;
        x = xtext(lm, "x");
        y = xtext(lm, "y");
        sc = xtext(lm, "scale");
        pr = xtext(lm, "primary");
        g_variant_builder_init(&mons, G_VARIANT_TYPE("a(ssa{sv})"));
        for (guint j = 0; j < lm->kids->len; j++) {
            XNode *mon = g_ptr_array_index(lm->kids, j);
            const Mon *m;
            char *id;

            if (strcmp(mon->name, "monitor") != 0)
                continue;
            m = mon_by_spec(st, xchild(mon, "monitorspec"), ours);
            id = m ? mode_id(m, xchild(mon, "mode")) : NULL;
            if (!id) {
                g_variant_builder_clear(&mons);
                g_variant_builder_clear(&lms);
                return NULL;
            }
            g_variant_builder_add(&mons, "(ss@a{sv})", m->connector, id,
                                  g_variant_new_array(G_VARIANT_TYPE(
                                      "{sv}"), NULL, 0));
            g_free(id);
        }
        g_variant_builder_add(&lms, "(iiduba(ssa{sv}))",
                              x ? atoi(x) : 0, y ? atoi(y) : 0,
                              sc ? g_ascii_strtod(sc, NULL) : 1.0,
                              transform_of(lm),
                              pr && strcmp(pr, "yes") == 0, &mons);
    }
    return g_variant_builder_end(&lms);
}

/* ------------------------------------------------ last connectors */

static gchar *last_path(void)
{
    return g_build_filename(g_get_user_state_dir(), "sremfb",
                            "last-connectors", NULL);
}

/* "serial product connector" lines; returns the connector or NULL. */
static char *last_get(const Mon *m)
{
    gchar *path = last_path(), *txt = NULL, *found = NULL;

    if (g_file_get_contents(path, &txt, NULL, NULL)) {
        gchar **lines = g_strsplit(txt, "\n", -1);
        for (gchar **l = lines; *l; l++) {
            gchar **f = g_strsplit(*l, "\t", 3);
            if (g_strv_length(f) == 3 && strcmp(f[0], m->serial) == 0 &&
                strcmp(f[1], m->product) == 0) {
                g_free(found);
                found = g_strdup(f[2]);
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    g_free(txt);
    g_free(path);
    return found;
}

static void last_set(const Mon *m)
{
    gchar *path = last_path(), *txt = NULL, *dir;
    GString *out = g_string_new(NULL);

    if (g_file_get_contents(path, &txt, NULL, NULL)) {
        gchar **lines = g_strsplit(txt, "\n", -1);
        for (gchar **l = lines; *l; l++) {
            gchar **f = g_strsplit(*l, "\t", 3);
            if (g_strv_length(f) == 3 && !(strcmp(f[0], m->serial) == 0 &&
                                           strcmp(f[1], m->product) == 0))
                g_string_append_printf(out, "%s\n", *l);
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    g_string_append_printf(out, "%s\t%s\t%s\n", m->serial, m->product,
                           m->connector);
    dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents(path, out->str, (gssize)out->len, NULL);
    g_free(dir);
    g_string_free(out, TRUE);
    g_free(txt);
    g_free(path);
}

/* The connector our monitor had in a matching configuration. */
static const char *config_connector(const XNode *cfg, const Mon *ours)
{
    GPtrArray *specs = g_ptr_array_new();
    const char *conn = NULL;

    config_specs(cfg, specs);
    for (guint j = 0; j < specs->len && !conn; j++)
        if (spec_is(g_ptr_array_index(specs, j), ours, FALSE))
            conn = xtext(g_ptr_array_index(specs, j), "connector");
    g_ptr_array_free(specs, TRUE);
    return conn;
}

/* ------------------------------------------------ driver */

static struct {
    GDBusConnection *bus;
    SremfbServer *srv;
    gboolean pending, again;
} Y;

static void refresh(void);

static void on_applied(GObject *src, GAsyncResult *ar, gpointer data)
{
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), ar,
                                                &err);
    char *who = data;

    if (r) {
        g_message("[%s] layout: remembered configuration applied", who);
        g_variant_unref(r);
    } else {
        g_warning("[%s] layout: ApplyMonitorsConfig failed: %s", who,
                  err->message);
        g_error_free(err);
    }
    g_free(who);
}

static void restore_for(SremfbClient *c, const State *st)
{
    const Mon *ours = NULL;
    gchar *path;
    XNode *root, *mons, *best = NULL;
    char *last;
    gboolean best_is_last = FALSE;

    for (guint i = 0; i < st->mons->len; i++) {
        const Mon *m = g_ptr_array_index(st->mons, i);
        if (strcmp(m->connector, c->connector) == 0)
            ours = m;
    }
    if (!ours)
        return;                        /* not in the state yet: wait */
    c->layout_pending = FALSE;         /* one shot per connection */

    path = g_build_filename(g_get_user_config_dir(), "monitors.xml", NULL);
    root = xml_load(path);
    g_free(path);
    last = last_get(ours);
    last_set(ours);                    /* whatever ends up current is
                                          stored under this connector */
    mons = xchild(root, "monitors");
    for (guint i = 0; mons && i < mons->kids->len; i++) {
        XNode *cfg = g_ptr_array_index(mons->kids, i);
        int mt;
        if (strcmp(cfg->name, "configuration") != 0)
            continue;
        mt = config_match(cfg, st, ours);
        if (mt == 2) {                 /* mutter found it by itself */
            best = NULL;
            break;
        }
        if (mt == 1 && !best_is_last) {
            /* the configuration of the connector used last time wins,
             * else the last one in the file */
            best = cfg;
            best_is_last = last && g_strcmp0(config_connector(cfg, ours),
                                             last) == 0;
        }
    }
    g_free(last);
    if (best) {
        GVariant *lms = build_logical(best, st, ours);
        if (lms) {
            g_message("[%s] layout: mutter lost this screen's configuration "
                      "(new connector %s), re-applying it", c->macstr,
                      c->connector);
            g_dbus_connection_call(Y.bus, DC_NAME, DC_PATH, DC_NAME,
                                   "ApplyMonitorsConfig",
                                   g_variant_new("(uu@a(iiduba(ssa{sv}))"
                                                 "@a{sv})", st->serial, 2u,
                                                 lms,
                                                 g_variant_new_array(
                                                     G_VARIANT_TYPE("{sv}"),
                                                     NULL, 0)),
                                   NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                                   on_applied, g_strdup(c->macstr));
        } else {
            g_message("[%s] layout: remembered configuration does not fit "
                      "the current modes, left to mutter", c->macstr);
        }
    }
    xnode_free(root);
}

static void on_state(GObject *src, GAsyncResult *ar, gpointer data)
{
    GError *err = NULL;
    GVariant *res = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src),
                                                  ar, &err);
    Y.pending = FALSE;
    if (!res) {
        g_warning("layout: GetCurrentState failed: %s", err->message);
        g_error_free(err);
    } else {
        State st = {0};
        state_parse(res, &st);
        for (guint i = 0; i < Y.srv->clients->len; i++) {
            SremfbClient *c = g_ptr_array_index(Y.srv->clients, i);
            if (c->layout_pending && c->connector[0] &&
                c->state == SREMFB_CLIENT_STREAMING)
                restore_for(c, &st);
        }
        g_ptr_array_free(st.mons, TRUE);
        g_variant_unref(res);
    }
    if (Y.again) {
        Y.again = FALSE;
        refresh();
    }
}

static void refresh(void)
{
    gboolean want = FALSE;

    for (guint i = 0; Y.srv && i < Y.srv->clients->len; i++)
        want |= ((SremfbClient *)g_ptr_array_index(Y.srv->clients,
                                                   i))->layout_pending;
    if (!Y.bus || !want)
        return;
    if (Y.pending) {
        Y.again = TRUE;
        return;
    }
    Y.pending = TRUE;
    g_dbus_connection_call(Y.bus, DC_NAME, DC_PATH, DC_NAME,
                           "GetCurrentState", NULL, G_VARIANT_TYPE(STATE_T),
                           G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_state,
                           NULL);
}

static void on_monitors_changed(GDBusConnection *bus, const gchar *sender,
                                const gchar *path, const gchar *iface,
                                const gchar *signal, GVariant *params,
                                gpointer data)
{
    refresh();
}

void sremfb_layout_init(SremfbServer *srv)
{
    const char *env = getenv("SREMFB_LAYOUT");
    GError *err = NULL;

    if (env && strcmp(env, "0") == 0) {
        g_message("layout restore disabled (SREMFB_LAYOUT=0)");
        return;
    }
    Y.srv = srv;
    Y.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!Y.bus) {
        g_warning("layout restore off: no session bus (%s)", err->message);
        g_error_free(err);
        return;
    }
    g_dbus_connection_signal_subscribe(Y.bus, DC_NAME, DC_NAME,
                                       "MonitorsChanged", DC_PATH, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_monitors_changed, NULL, NULL);
}

void sremfb_layout_client(SremfbClient *c)
{
    c->layout_pending = TRUE;
    refresh();
}
