/*
 * lgishim_gtk.c - Gtk.IconTheme Lua binding for the LGI shim.
 *
 * The only Gtk surface anything in this repo set actually calls is icon-name
 * -> icon-file-path resolution. awful-dots touches 2 files / 6 symbols
 * (05 文档 二·3.2), and nothing else Gtk at all: no windows, dialogs, widget
 * tree, CSS, fonts, menus or theme variables. So instead of linking GTK, this
 * module implements the XDG icon theme search in pure file I/O + glib.
 *
 * Why not link libgtk-3 (05 文档 二·3.3): the build host has no gtk-3.0
 * headers and no static libgtk-3.a / libgdk-3.a (pkg-config gtk+-3.0 = NO),
 * the closure is 67 shared libraries (atk/atspi/epoxy/wayland/systemd/
 * gcrypt/...), and gtk_icon_theme_get_default() needs a live GdkDisplay --
 * without gtk_init() it hits Gtk-CRITICAL 'GDK_IS_SCREEN (screen)' and
 * returns NULL. gtk_icon_theme_new() survives without gtk_init() but resolves
 * only to Adwaita, not the user's theme, so it is not a substitute.
 *
 * get_default() therefore resolves the theme name the way GTK does, minus the
 * X display:
 *   ~/.config/gtk-3.0/settings.ini            [Settings] gtk-icon-theme-name=
 *   ~/.config/gtk-3.0/books/settings.ini
 *   ~/.gtkrc-2.0, ~/.gtkrc                   gtk-icon-theme-name="..."
 *   fallback                                  "hicolor"
 * Search roots follow GTK's own default order:
 *   $HOME/.icons, $XDG_DATA_HOME/icons (default $HOME/.local/share/icons),
 *   every $XDG_DATA_DIRS entry + /icons (default /usr/local/share:/usr/share)
 *
 * Size selection: the largest "<n>x<n>" directory <= the requested size, else
 * the smallest available one. Extensions tried per context directory:
 * svg, png, xpm. GTK orders this more finely (exact first, then smaller, then
 * larger, across all themes); this matches it for the sizes themes actually
 * ship, which is what the callers use (48 everywhere).
 *
 * A per-instance cache keyed "<name>|<size>" makes this cheap: app_launcher
 * rebuilds its entry list on every inotify event and does three lookups per
 * app. "" is stored for a known miss so misses are not re-scanned; real paths
 * are never empty.
 */

#include <lua.h>
#include <lauxlib.h>

#include <glib.h>
#include <gio/gio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exported to luaa.c, which registers it in package.preload. */
int luaopen_lgishim_gtk(lua_State *L);

/* The first three fields of every other module's wrap; we only read ->p. */
typedef struct {
    void *p;
    void (*destroy)(void *);
    int cls;
} foreign_wrap;

enum {
    C_ICONTHEME = 1,
    C_ICONINFO,
    N_GTK_CLASSES
};

static const char *CLS_NAMES[N_GTK_CLASSES] = {
    NULL,
    "lgishim_gtk.IconTheme",
    "lgishim_gtk.IconInfo",
};

#define REG_CLASSES "lgishim_gtk_classes"

typedef struct {
    void *p;
    void (*destroy)(void *);
    int cls;
} wrap;

/* IconTheme: an icon search root list plus the active theme name, and a
 * "<name>|<size>" -> path cache. IconInfo: just the resolved path. */
typedef struct {
    GSList    *roots;
    char      *theme;
    GHashTable *cache;
} theme_t;

typedef struct {
    char *path;
} info_t;

/* --------------------------------------------------------------- helpers */

static void d_noop(void *p) { (void)p; }

static void
d_info(void *p)
{
    info_t *i = (info_t *)p;
    if (!i)
        return;
    g_free(i->path);
    g_free(i);
}

static void
d_theme(void *p)
{
    theme_t *t = (theme_t *)p;
    if (!t)
        return;
    if (t->cache)
        g_hash_table_destroy(t->cache);
    g_slist_free_full(t->roots, g_free);
    g_free(t->theme);
    g_free(t);
}

static wrap *
getud(lua_State *L, int idx)
{
    return lua_touserdata(L, idx);
}

static void *
chk_cls(lua_State *L, int idx, int cls)
{
    wrap *w = getud(L, idx);

    if (!w || !w->p)
        luaL_error(L, "lgishim_gtk: expected %s at arg %d, got %s",
                   CLS_NAMES[cls], idx,
                   w ? (w->p ? CLS_NAMES[w->cls] : luaL_typename(L, idx))
                     : luaL_typename(L, idx));
    if (w->cls != cls)
        luaL_error(L, "lgishim_gtk: expected %s at arg %d, got %s",
                   CLS_NAMES[cls], idx, CLS_NAMES[w->cls]);
    return w->p;
}

static int
push_obj(lua_State *L, void *p, void (*destroy)(void *), int cls)
{
    wrap *w;

    /* The object must be created BEFORE luaL_setmetatable(): the macro is
     * luaL_getmetatable + lua_setmetatable(-2) and already pops the
     * metatable, so the userdata has to sit underneath it. */
    w = (wrap *)lua_newuserdata(L, sizeof(wrap));
    memset(w, 0, sizeof(*w));
    w->p = p;
    w->destroy = destroy;
    w->cls = cls;
    luaL_setmetatable(L, CLS_NAMES[cls]);
    return 1;
}

/* --------------------------------------------------------- shared protocol */

static int
m_index(lua_State *L)
{
    wrap *w = getud(L, 1);
    const char *k;
    if (!w)
        return 0;
    k = luaL_checkstring(L, 2);

    /* Stack on entry: [obj, key]. The result has to land at slot 3 so that
     * `return 1` hands it back to __index, so every lookup proceeds as
     * [obj, key, clsinfo, ...] and gets swapped to slot 3 with lua_replace
     * before truncating with lua_settop(L, 3). */
    lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
    lua_rawgeti(L, -1, w->cls);                /* [.., clsinfo] */

    lua_getfield(L, -1, "m");
    lua_getfield(L, -1, k);
    if (lua_isfunction(L, -1)) {
        lua_replace(L, 3);
        lua_settop(L, 3);
        return 1;
    }
    lua_pop(L, 2);

    /* Property getters: a per-class "p" table of functions called as (obj),
     * for field-style access such as `variant.value`. */
    lua_getfield(L, -1, "p");
    lua_getfield(L, -1, k);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        lua_replace(L, 3);
        lua_settop(L, 3);
        return 1;
    }
    lua_pop(L, 2);

    luaL_getmetatable(L, CLS_NAMES[w->cls]);   /* [.., metatable] */
    lua_getfield(L, -1, k);
    if (!lua_isnil(L, -1)) {
        lua_replace(L, 3);
        lua_settop(L, 3);
        return 1;
    }
    return 0;
}

static int
m_newindex(lua_State *L)
{
    luaL_error(L, "lgishim_gtk: object is read-only");
    return 0;
}

static int
m_native(lua_State *L)
{
    wrap *w = getud(L, 1);
    if (!w || !w->p)
        return 0;
    lua_pushlightuserdata(L, w->p);
    return 1;
}

static const luaL_Reg NATIVE_PG[] = {
    { "_native", m_native },
    { NULL, NULL },
};

static void
release(wrap *w)
{
    if (w && w->p) {
        w->destroy(w->p);
        w->p = NULL;
        w->destroy = d_noop;
    }
}

static int
m_finish(lua_State *L)
{
    release(getud(L, 1));
    return 0;
}

static int
m_gc(lua_State *L)
{
    release(getud(L, 1));
    return 0;
}

static int
m_is_type_of(lua_State *L)
{
    int same = 0;

    /* The discriminator is metatable IDENTITY, deliberately not w->cls: the
     * other shims define the same wrap layout with their own cls enum, so an
     * index comparison would let a Pango.Layout pass as a Gio.File. */
    lua_pushvalue(L, lua_upvalueindex(1));     /* [.., expected_mt] */
    if (lua_getmetatable(L, 2)) {              /* [.., expected_mt, obj_mt] */
        same = lua_rawequal(L, -2, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_pushboolean(L, same ? TRUE : FALSE);
    return 1;
}

static int
class_call(lua_State *L)
{
    lua_getfield(L, 1, "new");
    lua_insert(L, 2);
    lua_remove(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

static void
make_class_table(lua_State *L, int cls, const luaL_Reg *m, const luaL_Reg *p)
{
    luaL_newmetatable(L, CLS_NAMES[cls]);
    lua_pushcfunction(L, m_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, m_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, m_finish);
    lua_setfield(L, -2, "finish");
    lua_pushcfunction(L, m_gc);
    lua_setfield(L, -2, "__gc");
    luaL_setfuncs(L, m, 0);
    lua_pop(L, 1);                             /* metatable */

    lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
        lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES); /* re-push */
    }
    lua_newtable(L);                           /* clsinfo */
    lua_newtable(L);
    luaL_setfuncs(L, m, 0);
    lua_setfield(L, -2, "m");
    lua_newtable(L);
    luaL_setfuncs(L, NATIVE_PG, 0);
    if (p)
        luaL_setfuncs(L, p, 0);
    lua_setfield(L, -2, "p");
    lua_rawseti(L, -2, cls);
    lua_pop(L, 1);                             /* registry array */

    lua_newtable(L);                           /* class table */
    luaL_setfuncs(L, m, 0);
    lua_newtable(L);
    lua_pushcclosure(L, class_call, 0);
    lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);

    luaL_getmetatable(L, CLS_NAMES[cls]);
    lua_pushcclosure(L, m_is_type_of, 1);
    lua_setfield(L, -2, "is_type_of");
}

static void
class_setup(lua_State *L, int cls, const luaL_Reg *m, const luaL_Reg *p)
{
    make_class_table(L, cls, m, p);
}

/* ----------------------------------------------- icon theme name sources */

#define THEME_GROUP "Icon Theme"

/* gtk-2 style rc file: a bare `gtk-icon-theme-name="X"` line anywhere. */
static char *
gtkrc_icon_theme(const char *path)
{
    static const char key[] = "gtk-icon-theme-name";
    size_t klen = sizeof(key) - 1;
    char *rc = NULL, *v = NULL, *p;
    gsize len = 0;

    if (!g_file_get_contents(path, &rc, &len, NULL))
        return NULL;

    for (p = rc; (p = strstr(p, key)); p += klen) {
        char *q = p + klen;
        while (*q == ' ' || *q == '\t' || *q == '=')
            q++;
        if (*q == '"' || *q == '\'') {
            char *e = strchr(q + 1, *q);
            if (e) {
                v = g_strndup(q + 1, (size_t)(e - q - 1));
                break;
            }
        } else {
            const char *e = q;
            while (*e && *e != ' ' && *e != '\t' && *e != '#'
                   && *e != '\n' && *e != '\r')
                e++;
            if (e > q) {
                v = g_strndup(q, (size_t)(e - q));
                break;
            }
        }
    }
    g_free(rc);
    return v;
}

static char *
resolve_default_theme(void)
{
    const char *home = g_get_home_dir();
    char *ini, *v = NULL;
    GKeyFile *kf;

    for (int i = 0; i < 2 && !v; i++) {
        ini = g_strdup_printf("%s/.config/gtk-3.0%s/settings.ini",
                              home, i ? "/books" : "");
        kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, ini, G_KEY_FILE_NONE, NULL))
            v = g_key_file_get_string(kf, "Settings", "gtk-icon-theme-name",
                                      NULL);
        g_key_file_free(kf);
        g_free(ini);
    }
    if (v)
        return v;

    {
        char *rcs[2] = { g_build_filename(home, ".gtkrc-2.0", NULL),
                         g_build_filename(home, ".gtkrc", NULL) };
        for (size_t i = 0; i < 2 && !v; i++) {
            v = gtkrc_icon_theme(rcs[i]);
            g_free(rcs[i]);
        }
    }
    if (v)
        return v;

    return g_strdup("hicolor");
}

/* ------------------------------------------------------ search root setup */

static GSList *
default_roots(void)
{
    GSList *roots = NULL;
    char *home = g_strdup(g_get_home_dir());
    char *xh, *xd;
    gchar **dirs;

    roots = g_slist_append(roots, g_build_filename(home, ".icons", NULL));

    xh = g_strdup(g_getenv("XDG_DATA_HOME"));
    if (!xh || !*xh) {
        g_free(xh);
        xh = g_build_filename(home, ".local", "share", NULL);
    }
    roots = g_slist_append(roots, g_build_filename(xh, "icons", NULL));
    g_free(xh);

    xd = g_strdup(g_getenv("XDG_DATA_DIRS"));
    if (!xd || !*xd) {
        g_free(xd);
        xd = g_strdup("/usr/local/share:/usr/share");
    }
    dirs = g_strsplit(xd, ":", -1);
    for (int i = 0; dirs[i]; i++)
        roots = g_slist_append(roots,
                               g_build_filename(dirs[i], "icons", NULL));
    g_free(dirs);
    g_free(xd);
    g_free(home);

    return roots;
}

/*
 * Inherits chain for one search root: the theme itself first, then the
 * parents listed in <root>/<theme>/index.theme, in order. The theme is always
 * included even when index.theme is missing, which is how "hicolor" and
 * directory-only themes still resolve.
 */
static GSList *
theme_chain(const char *root, const char *theme)
{
    GSList *chain = NULL;
    char *idx = g_build_filename(root, theme, "index.theme", NULL);
    char *inher;
    GKeyFile *kf = g_key_file_new();

    chain = g_slist_prepend(chain, g_strdup(theme));

    if (g_key_file_load_from_file(kf, idx, G_KEY_FILE_NONE, NULL)) {
        inher = g_key_file_get_string(kf, THEME_GROUP, "Inherits", NULL);
        if (inher) {
            gchar **names = g_strsplit(inher, ",", -1);
            GSList *parents = NULL;
            for (int i = 0; names[i]; i++) {
                char *n = g_strstrip(names[i]);
                if (*n)
                    parents = g_slist_prepend(parents, g_strdup(n));
            }
            /* prepend reverses Inherits, so re-order before splicing it onto
             * the list after the theme itself: [theme, p1, p2, ...]. */
            chain = g_slist_concat(chain, g_slist_reverse(parents));
            g_free(names);
            g_free(inher);
        }
    }
    g_key_file_free(kf);
    g_free(idx);

    return chain;
}

/* ----------------------------------------------------------- file matching */

static int
cmp_size(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* The "<n>x<n>" subdirectories of an icon theme base dir, ascending. */
static int *
collect_sizes(const char *base, int *n_out)
{
    GDir *d = g_dir_open(base, 0, NULL);
    const char *name;
    int *sizes = NULL;
    int n = 0, cap = 0;

    if (!d) {
        *n_out = 0;
        return NULL;
    }
    while ((name = g_dir_read_name(d))) {
        int a, b;
        if (sscanf(name, "%dx%d", &a, &b) == 2 && a == b && a > 0) {
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                sizes = (int *)g_realloc(sizes, (gsize)cap * sizeof(int));
            }
            sizes[n++] = a;
        }
    }
    g_dir_close(d);
    if (n > 1)
        qsort(sizes, (gsize)n, sizeof(int), cmp_size);
    *n_out = n;
    return sizes;
}

static const char *ICON_EXTS[] = { "svg", "png", "xpm", NULL };

/*
 * Look up <name> in one <root>/<theme> base dir. Returns the full path, or
 * NULL. A miss is not an error: gtk_icon_theme_lookup_icon() returns NULL too.
 *
 * Sizes are tried in GTK's own order -- exact, then smaller sizes largest
 * first, then larger sizes smallest first. Without that, an icon that only
 * ships at 128x128 (hicolor's xfce-display-*) would report missing for a
 * 48 lookup even though GTK would resolve it.
 */
static char *
find_in_base(const char *base, const char *name, int want)
{
    int *sizes = NULL, *order = NULL;
    int n = 0, k = 0, c, i;
    char *hit = NULL, *sdir = NULL, *fn = NULL, *try = NULL;

    sizes = collect_sizes(base, &n);
    if (n == 0) {
        g_free(sizes);
        return NULL;
    }

    /* Sizes come back ascending, so: equal first, then descending-below,
     * then ascending-above. */
    order = (int *)g_new(int, (gsize)n);
    for (i = 0; i < n; i++)
        if (sizes[i] == want)
            order[k++] = sizes[i];
    for (i = n - 1; i >= 0; i--)
        if (sizes[i] < want)
            order[k++] = sizes[i];
    for (i = 0; i < n; i++)
        if (sizes[i] > want)
            order[k++] = sizes[i];

    for (c = 0; c < k && !hit; c++) {
        char *sname = g_strdup_printf("%dx%d", order[c], order[c]);
        GDir *d;

        sdir = g_build_filename(base, sname, NULL);
        g_free(sname);
        d = g_dir_open(sdir, 0, NULL);
        if (d) {
            const char *ctx;
            while ((ctx = g_dir_read_name(d))) {
                for (i = 0; !hit && ICON_EXTS[i]; i++) {
                    fn = g_strdup_printf("%s.%s", name, ICON_EXTS[i]);
                    try = g_build_filename(sdir, ctx, fn, NULL);
                    if (g_file_test(try, G_FILE_TEST_IS_REGULAR))
                        hit = g_strdup(try);
                    g_free(try);
                    g_free(fn);
                }
            }
            g_dir_close(d);
        }
        g_free(sdir);
    }
    if (!hit) {                                 /* themes without a context dir */
        for (c = 0; c < k && !hit; c++) {
            char *sname = g_strdup_printf("%dx%d", order[c], order[c]);

            sdir = g_build_filename(base, sname, NULL);
            g_free(sname);
            for (i = 0; !hit && ICON_EXTS[i]; i++) {
                fn = g_strdup_printf("%s.%s", name, ICON_EXTS[i]);
                try = g_build_filename(sdir, fn, NULL);
                if (g_file_test(try, G_FILE_TEST_IS_REGULAR))
                    hit = g_strdup(try);
                g_free(try);
                g_free(fn);
            }
            g_free(sdir);
        }
    }
    g_free(order);
    g_free(sizes);
    return hit;
}

/*
 * "<name>|<size>" -> path, or "" for a known miss. Real icon paths are never
 * empty, so "" is a safe negative sentinel.
 */
static char *
lookup_cached(theme_t *t, const char *name, int size)
{
    char *key = g_strdup_printf("%s|%d", name, size);
    gpointer cached;
    char *hit = NULL;

    if (t->cache && (cached = g_hash_table_lookup(t->cache, key))) {
        hit = ((const char *)cached)[0] ? g_strdup(cached) : NULL;
        g_free(key);
        return hit;
    }

    for (GSList *r = t->roots; r && !hit; r = r->next) {
        GSList *chain = theme_chain(r->data, t->theme), *c;
        for (c = chain; c && !hit; c = c->next) {
            char *base = g_build_filename(r->data, c->data, NULL);
            hit = find_in_base(base, name, size);
            g_free(base);
        }
        g_slist_free_full(chain, g_free);
    }

    /* The cache keeps its OWN copy: the caller (and the IconInfo wrapping the
     * result) takes ownership of what is returned, so the two must be freed
     * independently. Returning the cached pointer directly would be a
     * double-free once the theme and the IconInfo are collected in some order.
     * The miss path stores "" so misses are not re-scanned. */
    if (t->cache)
        g_hash_table_insert(t->cache, key, hit ? g_strdup(hit) : g_strdup(""));
    else
        g_free(key);

    return hit;
}

/* -------------------------------------------------------------- C API map */

static int
theme_new(lua_State *L)
{
    theme_t *t = (theme_t *)g_new0(theme_t, 1);

    t->roots = default_roots();
    t->theme = resolve_default_theme();
    t->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    return push_obj(L, t, d_theme, C_ICONTHEME);
}

/*
 * icon_theme.lua:132 and launcher2.lua:13 both reach for the default theme
 * when no theme name was given, so this is the path that matters; both are
 * thin wrappers over theme_new(), and Gtk treats them as interchangeable.
 */
static int
theme_get_default(lua_State *L)
{
    return theme_new(L);
}

static int
theme_set_custom_theme(lua_State *L)
{
    theme_t *t = (theme_t *)chk_cls(L, 1, C_ICONTHEME);
    const char *name = luaL_checkstring(L, 2);

    g_free(t->theme);
    t->theme = g_strdup(name);
    if (t->cache) {
        g_hash_table_destroy(t->cache);
        t->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         g_free);
    }
    return 0;
}

static int
info_get_filename(lua_State *L)
{
    info_t *i = (info_t *)chk_cls(L, 1, C_ICONINFO);

    if (i->path)
        lua_pushstring(L, i->path);
    else
        lua_pushnil(L);
    return 1;
}

static int
theme_lookup_icon(lua_State *L)
{
    theme_t *t = (theme_t *)chk_cls(L, 1, C_ICONTHEME);
    const char *name = luaL_checkstring(L, 2);
    int size = (int)luaL_optinteger(L, 3, 48);
    char *hit = lookup_cached(t, name, size);

    if (!hit) {
        lua_pushnil(L);
        return 1;
    }
    info_t *i = (info_t *)g_new0(info_t, 1);
    i->path = hit;
    return push_obj(L, i, d_info, C_ICONINFO);
}

static int
theme_choose_icon(lua_State *L)
{
    theme_t *t = (theme_t *)chk_cls(L, 1, C_ICONTHEME);
    int size = (int)luaL_optinteger(L, 3, 48);
    char *hit = NULL;

    /* GTK_icon_theme_choose_icon(names, ...) is just lookup_icon() over the
     * list, first hit wins; icon_theme.lua:82 passes
     * {"application-all","application","application-default-icon","app"} and
     * expects nil when none of them exists. */
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        if (lua_isstring(L, -1)) {
            hit = lookup_cached(t, lua_tostring(L, -1), size);
            if (hit)
                break;
        }
        lua_pop(L, 1);                          /* discard value, keep key */
    }
    lua_settop(L, 1);

    if (!hit) {
        lua_pushnil(L);
        return 1;
    }
    info_t *i = (info_t *)g_new0(info_t, 1);
    i->path = hit;
    return push_obj(L, i, d_info, C_ICONINFO);
}

/*
 * The gicon arrives from lgishim_gio's AppInfo:get_icon() (icon_theme.lua:98
 * routes both iconless and named icons through here), so its wrap layout is
 * gio's. Reading ->p and calling g_icon_to_string() is enough -- that string
 * is exactly the icon name the theme search takes.
 */
static int
theme_lookup_by_gicon(lua_State *L)
{
    theme_t *t = (theme_t *)chk_cls(L, 1, C_ICONTHEME);
    int size = (int)luaL_optinteger(L, 3, 48);
    foreign_wrap *w = lua_touserdata(L, 2);
    char *hit = NULL;

    if (w && w->p) {
        char *s = g_icon_to_string((GIcon *)w->p);
        if (s) {
            hit = lookup_cached(t, s, size);
            g_free(s);
        }
    }
    if (!hit) {
        lua_pushnil(L);
        return 1;
    }
    info_t *i = (info_t *)g_new0(info_t, 1);
    i->path = hit;
    return push_obj(L, i, d_info, C_ICONINFO);
}

static const luaL_Reg ICONTHEME_M[] = {
    { "new",              theme_new },
    { "get_default",      theme_get_default },
    { "set_custom_theme", theme_set_custom_theme },
    { "lookup_icon",      theme_lookup_icon },
    { "lookup_by_gicon",  theme_lookup_by_gicon },
    { "choose_icon",      theme_choose_icon },
    { NULL, NULL },
};

static const luaL_Reg ICONINFO_M[] = {
    { "get_filename", info_get_filename },
    { NULL, NULL },
};

/* --- module entry point */

int
luaopen_lgishim_gtk(lua_State *L)
{
    lua_newtable(L);                           /* module table */

    /* make_class_table() leaves the class table on top of the stack, so each
     * class_setup() must be followed by a setfield that pops it into the
     * module table. */
    class_setup(L, C_ICONTHEME, ICONTHEME_M, NULL);
    lua_setfield(L, -2, "IconTheme");
    class_setup(L, C_ICONINFO, ICONINFO_M, NULL);
    lua_setfield(L, -2, "IconInfo");

    return 1;
}
