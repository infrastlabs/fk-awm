/*
 * lgishim_glib.c - GLib Lua binding for the LGI shim.
 *
 * Replaces `lgi.GLib` for the subset the awesomewm lib/ actually uses, so
 * timers (gears/timer.lua), D-Bus Variant payloads (naughty/dbus.lua),
 * KeyFile (.desktop parsing in menubar/utils.lua), path utilities
 * (menubar/icon_theme.lua, menubar/utils.lua), DateTime (textclock,
 * screenshot), and small UTF-8/String/Timer helpers all work without the
 * gobject-introspection runtime. Same wrapper object model as
 * lgishim_cairo.c / lgishim_pango.c: every GLib object is a full
 * userdata holding { ptr, destroy, cls }; `__gc` calls destroy();
 * `_native` returns lightuserdata.
 *
 * Scope from docs/sam/spike4-phase2-05-glib-gio-scope.md:
 *   P0  main loop (timeout_add / source_remove + closure guard)
 *   P0  GLib.Variant (formats (u)/(us)/(uu)/(s)/(ssss)/(as)/() etc.)
 *   P1  path utils + GLib.KeyFile (+KeyFileFlags.NONE)
 *   P2  UTF-8 helpers, GLib.String, GLib.Timer, GLib.DateTime, TimeZone
 *
 * Six bug-avoidance rules (lgishim_pango.c / lgishim_cairo.c lessons):
 *   1. lua_setfield(REG_CLASSES) pops the table -- re-fetch with
 *      lua_getfield before writing clsinfo (see class_setup).
 *   2. luaL_setmetatable already pops the metatable -- no second
 *      lua_setmetatable.
 *   3. m_index uses lua_replace(L, 3) BEFORE lua_settop(L, 3).
 *   4. Timer closure guard: luaL_ref() at register, luaL_unref() in the
 *      GSource destroy notify AND in source_remove/__gc (idempotent).
 *   5. release() NULLs ptr and swaps destroy to d_noop -- no double-free.
 *   6. lua_pushstring only -- NEVER lua_pushlstring(..., -1).
 */

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

/* Exported to luaa.c, which registers it in package.preload. */
int luaopen_lgishim_glib(lua_State *L);

enum {
    C_VARIANT = 1,
    C_KEYFILE = 2,
    C_DATETIME = 3,
    C_TIMEZONE = 4,
    C_TIMER = 5,
    C_STRING = 6,
    N_GLIB_CLASSES
};

static const char *CLS_NAMES[N_GLIB_CLASSES] = {
    NULL,
    "lgishim.GLib.Variant",
    "lgishim.GLib.KeyFile",
    "lgishim.GLib.DateTime",
    "lgishim.GLib.TimeZone",
    "lgishim.GLib.Timer",
    "lgishim.GLib.String",
};

#define REG_CLASSES "lgishim_glib_classes"

typedef struct {
    void *p;
    void (*destroy)(void *);
    int cls;
} wrap;

/* --------------------------------------------------------------- helpers */

static void d_variant(void *p) { g_variant_unref((GVariant *)p); }
static void d_keyfile(void *p) { g_key_file_free((GKeyFile *)p); }
static void d_datetime(void *p) { g_date_time_unref((GDateTime *)p); }
static void d_timezone(void *p) { g_time_zone_unref((GTimeZone *)p); }
static void d_timer(void *p) { g_timer_destroy((GTimer *)p); }
static void d_string(void *p) { g_string_free((GString *)p, TRUE); }
static void d_noop(void *p) { (void)p; }

static wrap *
getud(lua_State *L, int idx)
{
    return lua_touserdata(L, idx);
}

static void *
chk(lua_State *L, int idx)
{
    wrap *w = getud(L, idx);
    if (!w || !w->p)
        luaL_error(L, "lgishim.GLib: expected object at arg %d, got %s",
                   idx, w ? (w->p ? "nil" : luaL_typename(L, idx))
                          : luaL_typename(L, idx));
    return w->p;
}

static int
push_obj(lua_State *L, void *p, void (*destroy)(void *), int cls)
{
    wrap *w;

    /* Object must be created BEFORE luaL_setmetatable() -- the macro is
     * luaL_getmetatable + lua_setmetatable(-2) and already pops the
     * metatable, so the userdata has to be underneath it. Adding a second
     * lua_setmetatable would install the userdata itself as someone else's
     * metatable. */
    w = (wrap *)lua_newuserdata(L, sizeof(wrap));
    w->p = p;
    w->destroy = destroy;
    w->cls = cls;
    luaL_setmetatable(L, CLS_NAMES[cls]);
    return 1;
}

struct enumpair {
    const char *name;
    int value;
};

/* enum_from / enum_to were planned helpers for string-based enum conversion,
 * but the current install_enum() only exposes named string constants (e.g.
 * `glib.Priority.HIGH` -> "HIGH"), and the caller (lib/gears/timer.lua) uses
 * those strings directly as integer-like truthiness. Deleting to keep the
 * build warning-free. */

/* PRIORITY_DEFAULT / SOURCE_CONTINUE / SOURCE_REMOVE -- lib/gears/timer.lua
 * uses these as raw integers (glib.PRIORITY_DEFAULT is 0, SOURCE_CONTINUE
 * is truthy). We register them as both: the C integer constants for the
 * timer callback path, and named string aliases on the enum tables. */

static const struct enumpair PRIORITY[] = {
    { "HIGH", G_PRIORITY_HIGH },
    { "HIGH_IDLE", G_PRIORITY_HIGH_IDLE },
    { "DEFAULT", G_PRIORITY_DEFAULT },
    { "DEFAULT_IDLE", G_PRIORITY_DEFAULT_IDLE },
    { "LOW", G_PRIORITY_LOW },
};

static const struct enumpair SOURCE[] = {
    { "CONTINUE", G_SOURCE_CONTINUE },
    { "REMOVE", G_SOURCE_REMOVE },
};

static const struct enumpair KEYFILEFLAGS[] = {
    { "NONE", G_KEY_FILE_NONE },
    { "KEEP_COMMENTS", G_KEY_FILE_KEEP_COMMENTS },
    { "KEEP_TRANSLATIONS", G_KEY_FILE_KEEP_TRANSLATIONS },
};

static void
install_enum(lua_State *L, const char *fieldname,
             const struct enumpair *t, size_t n)
{
    size_t i;
    lua_newtable(L);
    for (i = 0; i < n; i++) {
        lua_pushstring(L, t[i].name);
        lua_setfield(L, -2, t[i].name);
    }
    lua_setfield(L, -2, fieldname);
}

/* Resolve an enum argument that may arrive as the C integer (real lgi
 * exposes GKeyFileFlags.NONE as 0) or as the shim's string form. lib/
 * passes whatever the module table hands it, so accept both and fail loudly
 * only on a genuinely unknown name -- a silent default would let a typo in
 * the flag column through. */
static uint32_t
enum_arg(lua_State *L, int idx, const struct enumpair *t, size_t n,
         uint32_t def)
{
    size_t i;
    if (lua_isnoneornil(L, idx))
        return def;
    if (lua_isnumber(L, idx))
        return (uint32_t)luaL_checkinteger(L, idx);
    {
        const char *s = lua_tostring(L, idx);
        if (s)
            for (i = 0; i < n; i++)
               if (!strcmp(s, t[i].name))
                   return (uint32_t)t[i].value;
    }
    return luaL_error(L, "lgishim.GLib: unknown %s value", lua_tostring(L, idx));
}

/* ---------------------------------------------------- shared protocol */

static int
m_index(lua_State *L)
{
    wrap *w = lua_touserdata(L, 1);
    const char *k;
    if (!w)
        return 0;
    k = luaL_checkstring(L, 2);

    lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
    lua_rawgeti(L, -1, w->cls);                /* [.., clsinfo] */

    lua_getfield(L, -1, "g");                  /* [.., clsinfo, g] */
    lua_getfield(L, -1, k);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        lua_replace(L, 3);
        lua_settop(L, 3);
        return 1;
    }
    lua_pop(L, 2);

    lua_getfield(L, -1, "m");
    lua_getfield(L, -1, k);
    if (lua_isfunction(L, -1)) {
        lua_replace(L, 3);
        lua_settop(L, 3);
        return 1;
    }
    lua_pop(L, 2);

    lua_getfield(L, -1, "p");
    lua_getfield(L, -1, k);
    if (lua_isfunction(L, -1)) {
        lua_replace(L, 3);
        lua_settop(L, 3);
        return 1;
    }
    lua_pop(L, 2);

    luaL_getmetatable(L, CLS_NAMES[w->cls]);
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
    wrap *w = lua_touserdata(L, 1);
    const char *k = luaL_checkstring(L, 2);
    if (!w)
        return 0;
    lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
    lua_rawgeti(L, -1, w->cls);
    lua_getfield(L, -1, "p");
    lua_getfield(L, -1, k);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        lua_pushvalue(L, 3);
        lua_call(L, 2, 0);
        return 0;
    }
    lua_pop(L, 4);
    luaL_error(L, "lgishim.GLib: no such property '%s'", k);
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

/*
 * `_native` is registered as a PROPERTY GETTER, not as a metatable field.
 * m_index's pg path auto-invokes the closure with obj as its argument and
 * returns the resulting value, so `obj._native` is the raw Glib* pointer
 * (lightuserdatum). A metatable field would hand back the closure itself, and
 * lua_touserdata() on a Lua function yields NULL -- exactly the trap that
 * black-screened the root wallpaper, because lib/awful/_compat.lua:71 does
 * `pcall(function() return pattern._native end)` and then passes `pattern._native`
 * straight to root._wallpaper() (root.c:503 casts it via lua_touserdata).
 *
 * lgishim_pixbuf.c documents the same reasoning for GdkPixbuf. lgishim_gobject.c
 * carries the identical NATIVE_PG table with a matching comment (lines 619-632).
 * cairo / pango / glib / gio shims used to put _native on the metatable, which
 * broke every lib/ call site of the form `X._native` (wibox shape, snap,
 * client/shape, _compat root.wallpaper). This is the last of them to be fixed.
 */
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
    lua_pushvalue(L, lua_upvalueindex(1));
    if (lua_getmetatable(L, 2)) {
        same = lua_rawequal(L, -2, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_pushboolean(L, same);
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
make_class_table(lua_State *L, int cls, const luaL_Reg *m)
{
    lua_newtable(L);
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
class_setup(lua_State *L, int cls, const luaL_Reg *m,
            const luaL_Reg *pg, const luaL_Reg *p)
{
    luaL_newmetatable(L, CLS_NAMES[cls]);
    lua_pushcfunction(L, m_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, m_newindex);
    lua_setfield(L, -2, "__newindex");
    /* NOTE: `_native` used to be registered here on the metatable, which made
     * `X._native` return the closure instead of the raw pointer (see NATIVE_PG
     * above). It is now a property getter in the per-class "g" table below so
     * m_index auto-invokes it. */
    lua_pushcfunction(L, m_finish);
    lua_setfield(L, -2, "finish");
    lua_pushcfunction(L, m_gc);
    lua_setfield(L, -2, "__gc");
    luaL_setfuncs(L, m, 0);
    if (p)
        luaL_setfuncs(L, p, 0);
    lua_pop(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
        lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
    }
    lua_newtable(L);
    lua_newtable(L);
    luaL_setfuncs(L, m, 0);
    lua_setfield(L, -2, "m");
    lua_newtable(L);
    luaL_setfuncs(L, NATIVE_PG, 0);            /* always: _native (property getter) */
    if (pg)
        luaL_setfuncs(L, pg, 0);
    lua_setfield(L, -2, "g");
    lua_newtable(L);
    if (p)
        luaL_setfuncs(L, p, 0);
    lua_setfield(L, -2, "p");
    lua_rawseti(L, -2, cls);
    lua_pop(L, 1);
    make_class_table(L, cls, m);
}


/* ================================================================
 * P0: Main loop -- glib.timeout_add / glib.source_remove
 *
 * Closure guard lifecycle:
 *
 *   timeout_add(priority, interval, fn):
 *     1. luaL_ref() stores `fn` in the Lua registry, so the closure is
 *        never collected while the GSource is alive.
 *     2. The ref id goes into `cbdata` alongside the lua_State, and
 *        `cbdata` is registered as the GSource's destroy notify.
 *     3. When `g_source_remove(id)` fires (user call, source returning
 *        G_SOURCE_REMOVE, main loop exit) GLib invokes the destroy
 *        notify exactly once; we luaL_unref() and free(cbdata).
 *     4. `source_remove` and `__gc` are defensive: they clear cbdata->ref
 *        to LUA_NOREF before unref, so a double path cannot double-free.
 *
 *   The Lua callback return value is interpreted as a boolean:
 *     G_SOURCE_CONTINUE (1 / truthy) -> keep the source, return TRUE.
 *     G_SOURCE_REMOVE  (0 / falsy)   -> return FALSE, GLib destroys.
 *
 * lib/gears/timer.lua:102 registers with `PRIORITY_DEFAULT` and :104
 * returns `glib.SOURCE_CONTINUE` unconditionally, so the source is only
 * ever removed via timer:stop() -> glib.source_remove(id) at :120.
 */

typedef struct {
    lua_State *L;
    int ref;
} cbdata;

static void
cbdata_destroy(gpointer user_data)
{
    cbdata *d = (cbdata *)user_data;
    if (!d)
        return;
    if (d->ref != LUA_NOREF) {
        if (d->ref >= 0)
            luaL_unref(d->L, LUA_REGISTRYINDEX, d->ref);
        d->ref = LUA_NOREF;
    }
    free(d);
}

static gboolean
timeout_cb(gpointer user_data)
{
    cbdata *d = (cbdata *)user_data;
    if (!d || d->ref == LUA_NOREF)
        return G_SOURCE_REMOVE;

    /* The callback returns one value (a boolean-ish truthiness). We push
     * nothing else so lua_settop(L, 0) below is safe -- it just discards
     * the call results and leaves the stack clean for the next dispatch. */
    lua_rawgeti(d->L, LUA_REGISTRYINDEX, d->ref);
    if (!lua_isfunction(d->L, -1))
        lua_pushboolean(d->L, 0);
    else {
        /* Protected call with traceback: a redraw/timer-chain error here
         * must not unwind through the GLib source callback (the wibox
         * drawable routes every redraw through timer.delayed_call). */
        lua_getglobal(d->L, "debug");
        lua_getfield(d->L, -1, "traceback");
        lua_remove(d->L, -2);                 /* [fn][tb] */
        if (lua_isfunction(d->L, -1)) {
            int errf = lua_gettop(d->L);      /* tb */
            lua_pushvalue(d->L, -2);          /* [fn][tb][fn] */
            if (lua_pcall(d->L, 0, 1, errf) != 0) {
                g_printerr("lgishim_glib timeout error: %s\n",
                           lua_tostring(d->L, -1));
                lua_pop(d->L, 1);
                lua_pushboolean(d->L, 1);     /* keep the source alive */
            }
        } else {
            lua_pop(d->L, 1);                 /* no debug.traceback: fallback */
            lua_call(d->L, 0, 1);
        }
    }

    int ret = 0;
    if (lua_isboolean(d->L, -1))
        ret = lua_toboolean(d->L, -1) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    else if (lua_isnumber(d->L, -1))
        ret = lua_tointeger(d->L, -1) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    else if (lua_isstring(d->L, -1)) {
        const char *s = lua_tostring(d->L, -1);
        if (s && !strcmp(s, "CONTINUE"))
            ret = G_SOURCE_CONTINUE;
        else
            ret = G_SOURCE_REMOVE;
    } else
        ret = G_SOURCE_CONTINUE;

    lua_settop(d->L, 0);
    return ret ? TRUE : FALSE;
}

static int
glib_timeout_add(lua_State *L)
{
    /* priority is normally the integer constant 0 (glib.PRIORITY_DEFAULT);
     * we accept either. interval is milliseconds. fn is the callback. */
    gint priority = (gint)luaL_optnumber(L, 1, G_PRIORITY_DEFAULT);
    guint interval = (guint)luaL_optnumber(L, 2, 0);
    cbdata *d;
    guint id;

    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (interval == 0)
        return luaL_error(L, "lgishim.GLib.timeout_add: interval must be > 0");

    d = (cbdata *)malloc(sizeof(cbdata));
    if (!d)
        return luaL_error(L, "lgishim.GLib.timeout_add: out of memory");
    d->L = L;
    d->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (d->ref == LUA_NOREF) {
        free(d);
        return luaL_error(L, "lgishim.GLib.timeout_add: luaL_ref failed");
    }

    /* g_timeout_add_full returns the guint source id directly (NOT a
     * GSource*). On failure it returns 0, which is also G_SOURCE_INVALID
     * in every documented call site. */
    id = g_timeout_add_full(priority, interval, timeout_cb, d, cbdata_destroy);
    if (!id) {
        luaL_unref(L, LUA_REGISTRYINDEX, d->ref);
        d->ref = LUA_NOREF;
        free(d);
        return luaL_error(L, "lgishim.GLib.timeout_add: g_timeout_add_full failed");
    }

    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int
glib_source_remove(lua_State *L)
{
    guint id = (guint)luaL_optnumber(L, 1, 0);
    if (!id)
        return 0;
    g_source_remove(id);
    return 0;
}

/* ================================================================
 * P1: Path utilities
 *   GLib.build_filenamev({a, b, ...}) -> "a/b/..."
 *   GLib.get_home_dir() -> string
 *   GLib.get_user_data_dir() -> string
 *   GLib.get_system_data_dirs() -> { dir1, dir2, ... }
 */

static int
glib_build_filenamev(lua_State *L)
{
    int argc = (int)lua_gettop(L);
    int n = 0, i;
    gchar **args = NULL;
    gchar *result = NULL;

    if (argc == 0)
        return luaL_error(L, "lgishim.GLib.build_filenamev: no segments");

    if (lua_istable(L, 1)) {
        /* lib/menubar/utils.lua:159-163 passes `{path, "icons"}` — count the
         * contiguous integer range, skipping nil holes. */
        for (i = 1; ; i++) {
            lua_rawgeti(L, 1, i);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            lua_pop(L, 1);
            n++;
        }
        if (n == 0)
            return luaL_error(L, "lgishim.GLib.build_filenamev: empty table");
        args = (gchar **)malloc((n + 1) * sizeof(gchar *));
        if (!args)
            return luaL_error(L, "lgishim.GLib.build_filenamev: out of memory");
        for (i = 0; i < n; i++) {
            lua_rawgeti(L, 1, i + 1);
            const char *s = luaL_checkstring(L, -1);
            args[i] = g_strdup(s);
            lua_pop(L, 1);
        }
    } else {
        /* Real GLib takes a GStrv, but accept plain string varargs too so a
         * `{...}` vs `...` slip does not turn into a missing-argument error. */
        n = argc;
        args = (gchar **)malloc((n + 1) * sizeof(gchar *));
        if (!args)
            return luaL_error(L, "lgishim.GLib.build_filenamev: out of memory");
        for (i = 0; i < n; i++) {
            const char *s = luaL_checkstring(L, i + 1);
            args[i] = g_strdup(s);
        }
    }
    args[n] = NULL;

    if (!args)
        return luaL_error(L, "lgishim.GLib.build_filenamev: no segments");

    result = g_build_filenamev(args);
    if (!result) {
        for (i = 0; args && args[i]; i++)
            g_free(args[i]);
        free(args);
        return luaL_error(L, "lgishim.GLib.build_filenamev: failed");
    }

    lua_pushstring(L, result);
    g_free(result);
    for (i = 0; args && args[i]; i++)
        g_free(args[i]);
    free(args);
    return 1;
}

static int
glib_get_home_dir(lua_State *L)
{
    const gchar *s = g_get_home_dir();
    if (s)
        lua_pushstring(L, s);
    else
        lua_pushnil(L);
    return 1;
}

static int
glib_get_user_data_dir(lua_State *L)
{
    const gchar *s = g_get_user_data_dir();
    if (s)
        lua_pushstring(L, s);
    else
        lua_pushnil(L);
    return 1;
}

static int
glib_get_system_data_dirs(lua_State *L)
{
    const gchar *const *dirs = g_get_system_data_dirs();
    lua_newtable(L);
    if (dirs) {
        int i = 0;
        while (dirs[i]) {
            lua_pushstring(L, dirs[i]);
            lua_rawseti(L, -2, ++i);
        }
    }
    return 1;
}

/* ================================================================
 * P1.5: Environment
 *   GLib.getenv(name) -> string or nil
 *   GLib.setenv(name, value, overwrite)
 *   GLib.find_program_in_path(filename) -> string or nil
 */

static int
glib_getenv(lua_State *L)
{
    const gchar *name = luaL_checkstring(L, 1);
    const gchar *s = g_getenv(name);
    if (s)
        lua_pushstring(L, s);
    else
        lua_pushnil(L);
    return 1;
}

static int
glib_setenv(lua_State *L)
{
    const gchar *name = luaL_checkstring(L, 1);
    const gchar *value = luaL_checkstring(L, 2);
    gboolean overwrite = lua_toboolean(L, 3);
    g_setenv(name, value, overwrite);
    return 0;
}

static int
glib_find_program_in_path(lua_State *L)
{
    const gchar *filename = luaL_checkstring(L, 1);
    gchar *result = g_find_program_in_path(filename);
    if (result) {
        lua_pushstring(L, result);
        g_free(result);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/* ================================================================
 * P2: UTF-8 helpers
 *   GLib.utf8_strlen(s, len) -> int      (len may be -1 for null-terminated)
 *   GLib.utf8_substring(s, offset, nchars) -> string
 *
 * awful/keygrabber.lua:206 calls glib.utf8_strlen(self.sequence, -1).
 */

static int
glib_utf8_strlen(lua_State *L)
{
    size_t slen;
    const char *s = luaL_checklstring(L, 1, &slen);
    glong len = (glong)luaL_optinteger(L, 2, -1);
    glong n = g_utf8_strlen(s, len);
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int
glib_utf8_substring(lua_State *L)
{
    size_t slen;
    const char *s = luaL_checklstring(L, 1, &slen);
    glong offset = (glong)luaL_optinteger(L, 2, 0);
    glong nchars = (glong)luaL_optinteger(L, 3, -1);
    /* g_utf8_substring(str, start_pos, end_pos): start_pos is inclusive,
     * end_pos is exclusive (or -1 for "to end"). We translate our
     * (offset, nchars) spec to (start_pos, end_pos) at the C level. */
    glong end_pos = (nchars < 0) ? -1 : (offset + nchars);
    gchar *r = g_utf8_substring(s, offset, end_pos);
    if (!r)
        return luaL_error(L, "lgishim.GLib.utf8_substring: failed");
    lua_pushstring(L, r);
    g_free(r);
    return 1;
}

/* ================================================================
 * P2: GLib.String
 *
 * awful/spawn.lua:305-307:
 *   local glibstr = GLib.String(cmd)
 *   return string.format('%x', math.ceil(GLib.String.hash(glibstr)))
 *
 * hash is a STATIC method on the String class table (it accepts a GString
 * userdata and returns its hash), so we attach it as a field on the class
 * table directly, not through a metatable method lookup.
 */

static int
string_new(lua_State *L)
{
    const char *s = luaL_optstring(L, 1, "");
    GString *gs = g_string_new(s);
    if (!gs)
        return luaL_error(L, "lgishim.GLib.String: out of memory");
    return push_obj(L, gs, d_string, C_STRING);
}

static int
string_hash(lua_State *L)
{
    wrap *w = getud(L, 1);
    guint h;
    if (!w || !w->p)
        return luaL_error(L, "lgishim.GLib.String.hash: expected GString userdata");
    h = g_string_hash((GString *)w->p);
    lua_pushinteger(L, (lua_Integer)h);
    return 1;
}

static const luaL_Reg STRING_M[] = {
    { "new", string_new },
    { "hash", string_hash },
    { NULL, NULL },
};

/* ================================================================
 * P2: GLib.Timer
 *
 * wibox/container/scroll.lua:425: ret._private.timer = GLib.Timer()
 * wibox/container/scroll.lua:131: timer:elapsed()
 * wibox/container/scroll.lua:232,257: timer:stop()       (scroll:pause)
 * wibox/container/scroll.lua:244:     timer:continue()   (scroll:continue)
 * wibox/container/scroll.lua:255:     timer:start()      (scroll:reset_scrolling)
 *
 * GLib.Timer here is a GTimer (a wall-clock timer, NOT a main-loop timer
 * source). Its constructor takes no arguments; :elapsed() returns
 * seconds as a double.
 *
 * :stop() / :continue() / :start() are the pause/resume/reset trio the
 * scroll container uses. scroll.lua:232,244,255,257 all call them in
 * statement position, so no return value is ever consumed -- but the
 * signatures still mirror real LGI rather than being flattened, so the
 * binding stays drop-in if a future caller wants the result.
 *
 * Return value is glib-version dependent: g_timer_stop() /
 * g_timer_continue() return void up to glib-2.79, then gboolean from
 * 2.80 on (the 2.80 change exists precisely so the caller can tell
 * whether the timer was running). We follow whichever ABI the build
 * links against, which is what real LGI exposes for that toolchain.
 * g_timer_start() is void in every version and resets the counter to
 * zero as well as arming it.
 */

static int
timer_new(lua_State *L)
{
    GTimer *t = g_timer_new();
    if (!t)
        return luaL_error(L, "lgishim.GLib.Timer: out of memory");
    return push_obj(L, t, d_timer, C_TIMER);
}

static int
timer_elapsed(lua_State *L)
{
    GTimer *t = (GTimer *)chk(L, 1);
    gulong micros = 0;
    gdouble seconds = g_timer_elapsed(t, &micros);
    lua_pushnumber(L, seconds);
    return 1;
}

static int
timer_stop(lua_State *L)
{
    GTimer *t = (GTimer *)chk(L, 1);
#if GLIB_CHECK_VERSION(2, 80, 0)
    lua_pushboolean(L, g_timer_stop(t));
    return 1;
#else
    g_timer_stop(t);
    return 0;
#endif
}

static int
timer_continue(lua_State *L)
{
    GTimer *t = (GTimer *)chk(L, 1);
#if GLIB_CHECK_VERSION(2, 80, 0)
    lua_pushboolean(L, g_timer_continue(t));
    return 1;
#else
    g_timer_continue(t);
    return 0;
#endif
}

static int
timer_start(lua_State *L)
{
    GTimer *t = (GTimer *)chk(L, 1);
    g_timer_start(t);
    return 0;
}

static const luaL_Reg TIMER_M[] = {
    { "new",      timer_new },
    { "elapsed",  timer_elapsed },
    { "stop",     timer_stop },
    { "continue", timer_continue },
    { "start",    timer_start },
    { NULL, NULL },
};

/* ================================================================
 * P2: GLib.DateTime / GLib.TimeZone
 *
 * textclock.lua:52-53, 57, 64, 93, 148 and screenshot.lua:44-65:
 *   local DateTime = glib.DateTime
 *   local TimeZone = glib.TimeZone
 *   DateTime.new_now()
 *   DateTime.new_now(TimeZone.new_local())
 *   DateTime.new_from_unix_utc(epoch)
 *   DateTime.new(tz, year, month, day, hour, minute, second)
 *   dt:format("%Y-%m-%d")
 *   TimeZone.new("Europe/Amsterdam")
 *   TimeZone.new_local()
 *
 * Note g_time_zone_new is deprecated in 2.68 in favour of
 * g_time_zone_new_identifier; we use the non-deprecated form when
 * available (glib-2.70 in the container) and fall back on older builds.
 */

#if GLIB_CHECK_VERSION(2, 68, 0)
#define lgishim_tz_new(id) g_time_zone_new_identifier(id)
#else
#define lgishim_tz_new(id) g_time_zone_new(id)
#endif

static int
timezone_new(lua_State *L)
{
    const char *id = luaL_checkstring(L, 1);
    GTimeZone *tz = lgishim_tz_new(id);
    if (!tz)
        return luaL_error(L, "lgishim.GLib.TimeZone.new: unknown identifier '%s'", id);
    return push_obj(L, tz, d_timezone, C_TIMEZONE);
}

static int
timezone_new_local(lua_State *L)
{
    GTimeZone *tz = g_time_zone_new_local();
    return push_obj(L, tz, d_timezone, C_TIMEZONE);
}

static int
timezone_new_utc(lua_State *L)
{
    GTimeZone *tz = g_time_zone_new_utc();
    return push_obj(L, tz, d_timezone, C_TIMEZONE);
}

static const luaL_Reg TIMEZONE_M[] = {
    { "new",       timezone_new },
    { "new_local", timezone_new_local },
    { "new_utc",   timezone_new_utc },
    { NULL, NULL },
};

static int
datetime_new(lua_State *L)
{
    /* GLib.DateTime.new(tz, year, month, day, hour, minute, second)
     * tz is a GTimeZone userdata (may be nil for local). */
    GTimeZone *tz = NULL;
    wrap *w = getud(L, 1);
    if (w && w->p)
        tz = (GTimeZone *)w->p;
    gint year   = (gint)luaL_optinteger(L, 2, 1970);
    gint month  = (gint)luaL_optinteger(L, 3, 1);
    gint day    = (gint)luaL_optinteger(L, 4, 1);
    gint hour   = (gint)luaL_optinteger(L, 5, 0);
    gint minute = (gint)luaL_optinteger(L, 6, 0);
    gint second = (gint)luaL_optinteger(L, 7, 0);
    GDateTime *dt = g_date_time_new(tz, year, month, day, hour, minute, second);
    if (!dt)
        return luaL_error(L, "lgishim.GLib.DateTime.new: failed");
    return push_obj(L, dt, d_datetime, C_DATETIME);
}

static int
datetime_new_now(lua_State *L)
{
    /* DateTime.new_now(tz) -- tz is optional; defaults to local. */
    GTimeZone *tz = NULL;
    wrap *w = getud(L, 1);
    if (w && w->p)
        tz = (GTimeZone *)w->p;
    else
        tz = g_time_zone_new_local();
    GDateTime *dt = g_date_time_new_now(tz);
    if (tz && !w)
        g_time_zone_unref(tz);
    if (!dt)
        return luaL_error(L, "lgishim.GLib.DateTime.new_now: failed");
    return push_obj(L, dt, d_datetime, C_DATETIME);
}

static int
datetime_new_from_unix_utc(lua_State *L)
{
    gint64 t = (gint64)luaL_optnumber(L, 1, 0);
    GDateTime *dt = g_date_time_new_from_unix_utc(t);
    if (!dt)
        return luaL_error(L, "lgishim.GLib.DateTime.new_from_unix_utc: failed");
    return push_obj(L, dt, d_datetime, C_DATETIME);
}

static int
datetime_new_from_unix_local(lua_State *L)
{
    gint64 t = (gint64)luaL_optnumber(L, 1, 0);
    GDateTime *dt = g_date_time_new_from_unix_local(t);
    if (!dt)
        return luaL_error(L, "lgishim.GLib.DateTime.new_from_unix_local: failed");
    return push_obj(L, dt, d_datetime, C_DATETIME);
}

static int
datetime_format(lua_State *L)
{
    GDateTime *dt = (GDateTime *)chk(L, 1);
    const char *fmt = luaL_checkstring(L, 2);
    gchar *r = g_date_time_format(dt, fmt);
    if (!r)
        return luaL_error(L, "lgishim.GLib.DateTime:format: failed for '%s'", fmt);
    lua_pushstring(L, r);
    g_free(r);
    return 1;
}

static const luaL_Reg DATETIME_M[] = {
    { "new",                 datetime_new },
    { "new_now",             datetime_new_now },
    { "new_from_unix_utc",   datetime_new_from_unix_utc },
    { "new_from_unix_local", datetime_new_from_unix_local },
    { "format",              datetime_format },
    { NULL, NULL },
};

/* ================================================================
 * P0: GLib.Variant
 *
 * naughty/dbus.lua constructs these formats across 8 sites:
 *   GLib.Variant("(us)", { notificationId, action })
 *   GLib.Variant("(uu)", { notificationId, reason })
 *   GLib.Variant("(u)",  { notification.id })
 *   GLib.Variant("()",   {})                   -- empty tuple
 *   GLib.Variant("(ssss)", { str1, str2, str3, str4 })
 *   GLib.Variant("(as)", { capabilities })
 *   GLib.Variant("(s)",  { sender })
 *
 * The dbus.lua D-Bus interface definitions also refer to `(ay)`,
 * `a{sv}` and `(i)` as variant types; we support them in the format
 * parser for future-proofing.
 *
 * Construction semantics (per spike4-phase2-05-glib-gio-scope.md §Variant):
 *   format "()"       -> empty tuple
 *   format "(<list>)" -> tuple with <list> type children, one Lua arg per child
 *   format "as"       -> array of strings, one Lua arg = table of strings
 *   format "ay"       -> array of bytes,  one Lua arg = table of ints
 *   format "a{sv}"    -> dictionary of string->variant, one Lua arg = table
 *   format "<one>"    -> single primitive, one Lua arg = primitive
 *
 * Ownership: children passed to `g_variant_new_tuple` / `g_variant_new_array`
 * are claimed (floating refs consumed), so we must NOT unref them separately.
 * `g_variant_type_new()` returns a full ref that the variant borrows; the
 * caller must `g_variant_type_free()` it after building.
 */

/* Convert a single Lua value to a GVariant of the given single-character
 * type spec. Returns NULL on error (with a lua_error raised).
 * The GVariant returned is a floating reference, ready to be adopted by
 * a container constructor or ref'd by the caller. */
static GVariant *
variant_from_lua(lua_State *L, int idx, char type)
{
    switch (type) {
    case 'b':
        return g_variant_new_boolean(lua_toboolean(L, idx));
    case 'y':
        return g_variant_new_byte((guint8)luaL_checknumber(L, idx));
    case 'n':
        return g_variant_new_int16((gint16)luaL_checknumber(L, idx));
    case 'q':
        return g_variant_new_uint16((guint16)luaL_checknumber(L, idx));
    case 'i':
        return g_variant_new_int32((gint32)luaL_checkinteger(L, idx));
    case 'u':
        return g_variant_new_uint32((guint32)luaL_checkinteger(L, idx));
    case 'x':
        return g_variant_new_int64((gint64)luaL_checknumber(L, idx));
    case 't':
        return g_variant_new_uint64((guint64)luaL_checknumber(L, idx));
    case 'd':
        return g_variant_new_double(luaL_checknumber(L, idx));
    case 's': {
        const char *s = luaL_checkstring(L, idx);
        return g_variant_new_string(s ? s : "");
    }
    case 'o': {
        const char *s = luaL_checkstring(L, idx);
        return g_variant_new_object_path(s ? s : "");
    }
    default:
        /* Unknown char: return a string variant so callers at least get
         * something coherent rather than a segfault on the NULL. */
        return g_variant_new_string("");
    }
}

/* Build a GVariant from a Lua table at stack position `tbl_idx`, using
 * the format string `fmt` (no outer parens -- caller strips those).
 * Returns a floating reference. The Lua stack position may be shifted
 * but the value at `tbl_idx` is expected to be the source.
 *
 * Supported recursive forms:
 *   single char      -> primitive from tbl_idx (must be non-table)
 *   "a<c>"           -> array of c; tbl_idx is a Lua array table
 *   "a{sv}"          -> dict of string->variant; tbl_idx is a Lua key/value table
 *   "(c1c2...)"      -> tuple; tbl_idx is a Lua array table
 *   anything else    -> falls back to string, but raises an error instead
 */
static GVariant *
variant_from_table(lua_State *L, int tbl_idx, const char *fmt)
{
    size_t flen = strlen(fmt);

    if (flen == 0) {
        /* "" -> empty tuple (matches GLib.Variant("()", {}) semantics
         * when the caller has already stripped the outer parens). */
        return g_variant_new_tuple(NULL, 0);
    }

    if (fmt[0] == 'a' && flen > 1 && fmt[1] == '{') {
        /* Dictionary a{sv}: Lua table with string keys, values converted
         * via g_variant_parse to accept any GVariant text form. */
        int n = 0, i = 0;
        GVariant **entries = NULL;
        lua_pushnil(L);
        while (lua_next(L, tbl_idx)) {
            if (!lua_isstring(L, -2))
                lua_pop(L, 2);
            else
                n++;
        }
        if (n == 0)
            return g_variant_new_array(g_variant_type_new("s"), NULL, 0);
        entries = (GVariant **)g_malloc0(n * sizeof(GVariant *));
        lua_pushnil(L);
        while (lua_next(L, tbl_idx)) {
            const char *k = luaL_checkstring(L, -2);
            const char *v = lua_tostring(L, -1);
            GVariant *kvar = g_variant_new_string(k);
            /* For the value, use `g_variant_new_variant()` around a variant
             * parsed from the string. Fall back to a string variant if
             * the value isn't parseable as a GVariant. */
            GError *err = NULL;
            const gchar *endptr = NULL;
            GVariant *parsed = g_variant_parse(NULL, v ? v : "", NULL,
                                               &endptr, &err);
            if (err || !parsed) {
                if (err) g_error_free(err);
                if (parsed) g_variant_unref(parsed);
                parsed = g_variant_new_string(v ? v : "");
            }
            GVariant *vvar = g_variant_new_variant(parsed);
            g_variant_unref(parsed);
            entries[i++] = g_variant_new_dict_entry(kvar, vvar);
        }
        GVariant *v = g_variant_new_array(g_variant_type_new("{sv}"), entries, n);
        g_free(entries);
        return v;
    }

    if (fmt[0] == 'a' && flen > 1) {
        /* Array: tbl_idx is a Lua array table. Element type is fmt[1]. */
        char elem_fmt[2] = { fmt[1], 0 };
        int count = (int)luaL_len(L, tbl_idx);
        GVariant **elems;
        int i;

        if (count == 0) {
            GVariantType *et = g_variant_type_new(elem_fmt);
            GVariant *v = g_variant_new_array(et, NULL, 0);
            g_variant_type_free(et);
            return v;
        }
        elems = (GVariant **)g_malloc0(count * sizeof(GVariant *));
        for (i = 1; i <= count; i++) {
            lua_rawgeti(L, tbl_idx, i);
            elems[i - 1] = variant_from_lua(L, -1, elem_fmt[0]);
            lua_pop(L, 1);
        }
        GVariantType *et = g_variant_type_new(elem_fmt);
        GVariant *v = g_variant_new_array(et, elems, count);
        g_free(elems);
        g_variant_type_free(et);
        return v;
    }

    /* Single primitive: tbl_idx is the primitive value itself. */
    return variant_from_lua(L, tbl_idx, fmt[0]);
}

/* GLib.Variant.new(format, values): build a variant from a format string
 * and a Lua argument table. Values is a table matching the shape the
 * format implies:
 *   "()"       values can be {} or omitted
 *   "(t1..tn)" values is an array table of length n
 *   "a<t>"     values is an array table
 *   "a{sv}"    values is a key/value table
 *   "<t>"      values can be a single element (or omitted, we use idx 2)
 *
 * We always read Lua argument 2 as the values table, matching how LGI's
 * `GLib.Variant("...", {...})` is called from dbus.lua. */
static int
variant_new(lua_State *L)
{
    const char *fmt = luaL_checkstring(L, 1);
    GVariant *v = NULL;

    if (fmt[0] == '(' && fmt[strlen(fmt) - 1] == ')') {
        /* Tuple: strip outer parens, then interpret the body as
         * a comma-free sequence of type specifiers. */
        size_t flen = strlen(fmt);
        GVariant **children = NULL;
        GVariant *tup = NULL;
        size_t i = 0;
        size_t n = 0;

        /* Skip past '(' */
        i = 1;
        if (flen > 2 && fmt[1] != ')') {
            /* Non-empty tuple body. */
            /* First pass: count children. */
            size_t j = 1;
            while (j < flen - 1) {
                if (fmt[j] == 'a' && j + 2 < flen - 1 && fmt[j + 1] == '{') {
                    /* Skip a{sv} */
                    n++;
                    j += 4;
                } else {
                    n++;
                    j++;
                }
            }
            children = (GVariant **)g_malloc0(n * sizeof(GVariant *));
            /* Second pass: build children from Lua arg 2 (a Lua table). */
            j = 1;
            for (i = 0; i < n && j < flen - 1; i++, j++) {
                const char *child_fmt;
                size_t child_len;
                lua_rawgeti(L, 2, (lua_Integer)(i + 1));
                if (fmt[j] == 'a' && j + 2 < flen - 1 && fmt[j + 1] == '{') {
                    child_fmt = "a{sv}";
                    child_len = 4;
                    j += child_len;
                } else {
                    child_fmt = &fmt[j];
                    child_len = 1;
                }
                if (child_len > 1)
                    j += child_len - 1;
                children[i] = variant_from_table(L, -1, child_fmt);
                lua_pop(L, 1);
            }
            /* g_variant_new_tuple() assembles the tuple type itself; there is
             * nothing to build with g_variant_type_new() beforehand. */
            tup = g_variant_new_tuple(children, (gint)n);
            g_free(children);
            v = tup;
        } else {
            /* Empty tuple "()" */
            v = g_variant_new_tuple(NULL, 0);
        }
    } else {
        /* Non-tuple: could be a single primitive, or an array (fmt[0]=='a'). */
        if (!lua_istable(L, 2) && fmt[0] != 'a') {
            /* Single primitive value at Lua arg 2. */
            v = variant_from_lua(L, 2, fmt[0]);
        } else {
            /* Array or a{sv} -- Lua arg 2 is the table. */
            v = variant_from_table(L, 2, fmt);
        }
    }

    if (!v)
        return luaL_error(L, "lgishim.GLib.Variant.new: failed to build '%s'", fmt);
    return push_obj(L, v, d_variant, C_VARIANT);
}

static int
variant_is_type_of(lua_State *L)
{
    GVariant *v = (GVariant *)chk(L, 1);
    const char *type = luaL_checkstring(L, 2);
    gboolean r = g_variant_is_of_type(v, g_variant_type_new(type));
    lua_pushboolean(L, r);
    return 1;
}

static int
variant_n_children(lua_State *L)
{
    GVariant *v = (GVariant *)chk(L, 1);
    lua_pushinteger(L, (lua_Integer)g_variant_n_children(v));
    return 1;
}

static int
variant_get_child_value(lua_State *L)
{
    GVariant *v = (GVariant *)chk(L, 1);
    gint idx = (gint)luaL_checkinteger(L, 2);
    GVariant *child;

    /* Range-check first: g_variant_get_child_value() only asserts on bad
     * indexes, it does not report an error. */
    if (idx < 0 || (gsize)idx > (gsize)g_variant_n_children(v))
        return luaL_error(L, "lgishim.GLib.Variant.get_child_value: index %d out of range", idx);
    child = g_variant_get_child_value(v, idx);
    if (!child)
        return luaL_error(L, "lgishim.GLib.Variant.get_child_value: index %d out of range", idx);
    return push_obj(L, child, d_variant, C_VARIANT);
}

static int
variant_print(lua_State *L)
{
    GVariant *v = (GVariant *)chk(L, 1);
    GError *err = NULL;
    gchar *s = g_variant_print(v, FALSE);
    if (!s) {
        if (err) g_error_free(err);
        return luaL_error(L, "lgishim.GLib.Variant.print: failed");
    }
    lua_pushstring(L, s);
    g_free(s);
    return 1;
}

static int
variant_new_from_parsed(lua_State *L)
{
    const char *s = luaL_checkstring(L, 1);
    GError *err = NULL;
    const gchar *endptr = NULL;
    GVariant *v = g_variant_parse(NULL, s, NULL, &endptr, &err);
    if (!v) {
        const char *msg = err ? err->message : "unknown";
        if (err) g_error_free(err);
        return luaL_error(L, "lgishim.GLib.Variant.new_from_parsed: %s", msg);
    }
    return push_obj(L, v, d_variant, C_VARIANT);
}

static const luaL_Reg VARIANT_M[] = {
    { "new",               variant_new },
    { "is_type_of",        variant_is_type_of },
    { "n_children",        variant_n_children },
    { "get_child_value",   variant_get_child_value },
    { "print",             variant_print },
    { "new_from_parsed",   variant_new_from_parsed },
    { NULL, NULL },
};

/* ================================================================
 * P1: GLib.KeyFile
 *
 * menubar/utils.lua:269-281 parses .desktop files:
 *   local keyfile = glib.KeyFile()
 *   if not keyfile:load_from_file(file, glib.KeyFileFlags.NONE) then ...
 *   if not keyfile:has_group("Desktop Entry") then ...
 *   for _, key in pairs(keyfile:get_keys("Desktop Entry")) do
 *       kf:get_string("Desktop Entry", key)
 *       kf:get_string_list("Desktop Entry", key, nil)
 *       kf:get_locale_string("Desktop Entry", key, nil)
 *       kf:get_locale_string_list("Desktop Entry", key, nil, nil)
 *       kf:get_boolean("Desktop Entry", key)
 *
 * The `nil` third arg to get_string_list / get_locale_string /
 * get_locale_string_list is the "lang" or "listsep" parameter, which
 * we pass through as NULL to GLib.
 */

static int
keyfile_new(lua_State *L)
{
    GKeyFile *kf = g_key_file_new();
    if (!kf)
        return luaL_error(L, "lgishim.GLib.KeyFile: out of memory");
    return push_obj(L, kf, d_keyfile, C_KEYFILE);
}

static int
keyfile_load_from_file(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    const char *path = luaL_checkstring(L, 2);
    guint32 flags = enum_arg(L, 3, KEYFILEFLAGS,
                             sizeof(KEYFILEFLAGS) / sizeof(KEYFILEFLAGS[0]),
                             G_KEY_FILE_NONE);
    GError *err = NULL;
    gboolean ok = g_key_file_load_from_file(kf, path, flags, &err);
    if (!ok) {
        if (err) g_error_free(err);
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int
keyfile_load_from_data(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    size_t slen;
    const char *data = luaL_checklstring(L, 2, &slen);
    guint32 flags = enum_arg(L, 3, KEYFILEFLAGS,
                             sizeof(KEYFILEFLAGS) / sizeof(KEYFILEFLAGS[0]),
                             G_KEY_FILE_NONE);
    GError *err = NULL;
    gboolean ok = g_key_file_load_from_data(kf, data, (gsize)slen, flags, &err);
    if (!ok) {
        if (err) g_error_free(err);
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int
keyfile_has_group(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    const char *group = luaL_checkstring(L, 2);
    lua_pushboolean(L, g_key_file_has_group(kf, group));
    return 1;
}

static int
keyfile_has_key(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    const char *group = luaL_checkstring(L, 2);
    const char *key = luaL_checkstring(L, 3);
    lua_pushboolean(L, g_key_file_has_key(kf, group, key, NULL));
    return 1;
}

static int
keyfile_get_groups(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    gsize n = 0;
    gchar **groups = g_key_file_get_groups(kf, &n);
    if (!groups) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    for (gsize i = 0; i < n; i++) {
        lua_pushstring(L, groups[i]);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    g_strfreev(groups);
    return 1;
}

static int
keyfile_get_keys(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    const char *group = luaL_checkstring(L, 2);
    GError *err = NULL;
    gsize n = 0;
    gchar **keys = g_key_file_get_keys(kf, group, &n, &err);
    if (!keys) {
        if (err) g_error_free(err);
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    for (gsize i = 0; i < n; i++) {
        lua_pushstring(L, keys[i]);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    g_strfreev(keys);
    return 1;
}

static int
keyfile_get_string(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    const char *group = luaL_checkstring(L, 2);
    const char *key = luaL_checkstring(L, 3);
    GError *err = NULL;
    gchar *s = g_key_file_get_string(kf, group, key, &err);
    if (!s) {
        if (err) g_error_free(err);
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, s);
    g_free(s);
    return 1;
}

static int
keyfile_get_boolean(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    const char *group = luaL_checkstring(L, 2);
    const char *key = luaL_checkstring(L, 3);
    GError *err = NULL;
    gboolean ok = g_key_file_get_boolean(kf, group, key, &err);
    if (ok < 0) {
        if (err) g_error_free(err);
        lua_pushnil(L);
        return 1;
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int
keyfile_get_int(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    const char *group = luaL_checkstring(L, 2);
    const char *key = luaL_checkstring(L, 3);
    GError *err = NULL;
    gint v = g_key_file_get_integer(kf, group, key, &err);
    if (err) {
        g_error_free(err);
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

static int
keyfile_get_double(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    const char *group = luaL_checkstring(L, 2);
    const char *key = luaL_checkstring(L, 3);
    GError *err = NULL;
    gdouble v = g_key_file_get_double(kf, group, key, &err);
    if (err) {
        g_error_free(err);
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, v);
    return 1;
}

static int
keyfile_get_string_list(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    const char *group = luaL_checkstring(L, 2);
    const char *key = luaL_checkstring(L, 3);
    GError *err = NULL;
    gsize n = 0;
    gchar **list = g_key_file_get_string_list(kf, group, key, &n, &err);
    if (!list) {
        if (err) g_error_free(err);
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    for (gsize i = 0; i < n; i++) {
        lua_pushstring(L, list[i]);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    g_strfreev(list);
    return 1;
}

static int
keyfile_get_locale_string(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    const char *group = luaL_checkstring(L, 2);
    const char *key = luaL_checkstring(L, 3);
    const char *lang = lua_isnil(L, 4) ? NULL : luaL_optstring(L, 4, NULL);
    GError *err = NULL;
    gchar *s = g_key_file_get_locale_string(kf, group, key, lang, &err);
    if (!s) {
        if (err) g_error_free(err);
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, s);
    g_free(s);
    return 1;
}

static int
keyfile_get_locale_string_list(lua_State *L)
{
    GKeyFile *kf = (GKeyFile *)chk(L, 1);
    const char *group = luaL_checkstring(L, 2);
    const char *key = luaL_checkstring(L, 3);
    /* lang (L,4) can be nil or a locale string. Caller may also pass a
     * 5th arg (listsep) which GLib 2.70 ignores -- we accept it too so
     * menubar/utils.lua's `kf:get_locale_string_list(g, k, nil, nil)`
     * works. */
    const char *lang = lua_isnil(L, 4) ? NULL : luaL_optstring(L, 4, NULL);
    GError *err = NULL;
    gsize n = 0;
    gchar **list = g_key_file_get_locale_string_list(kf, group, key, lang,
                                                     &n, &err);
    if (!list) {
        if (err) g_error_free(err);
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    for (gsize i = 0; i < n; i++) {
        lua_pushstring(L, list[i]);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    g_strfreev(list);
    return 1;
}

static const luaL_Reg KEYFILE_M[] = {
    { "new",                     keyfile_new },
    { "load_from_file",          keyfile_load_from_file },
    { "load_from_data",          keyfile_load_from_data },
    { "has_group",               keyfile_has_group },
    { "has_key",                 keyfile_has_key },
    { "get_groups",              keyfile_get_groups },
    { "get_keys",                keyfile_get_keys },
    { "get_string",              keyfile_get_string },
    { "get_boolean",             keyfile_get_boolean },
    { "get_int",                 keyfile_get_int },
    { "get_double",              keyfile_get_double },
    { "get_string_list",         keyfile_get_string_list },
    { "get_locale_string",       keyfile_get_locale_string },
    { "get_locale_string_list",  keyfile_get_locale_string_list },
    { NULL, NULL },
};

/* ================================================================
 * Module initializer: luaopen_lgishim_glib
 *
 * Registration order matches lgishim_cairo.c / lgishim_pango.c:
 *   1. luaopen_lgishim_glib creates the module table (returned to Lua).
 *   2. For each class, class_setup() installs the metatable (for
 *      userdata) and a clsinfo table in REG_CLASSES[cls] (for m_index
 *      method/property lookups), and pushes a class table on the stack.
 *   3. lua_setfield stores the class table under the module key.
 *   4. install_enum stores enum tables under their enum class name.
 *   5. Individual utility functions (timeout_add, source_remove,
 *      path helpers, utf8 helpers) are installed on the module table
 *      via luaL_setfuncs.
 *   6. Return 1 to leave the module table on top.
 */

int
luaopen_lgishim_glib(lua_State *L)
{
    lua_newtable(L);                                  /* module */

    /* Ensure REG_CLASSES exists (class_setup handles it lazily, but we
     * touch it first to make sure the initial entry is populated). */
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_CLASSES);

    /* -- Classes (order matches the C_* enum above) -- */

    class_setup(L, C_VARIANT,   VARIANT_M,   NULL, NULL);
    lua_setfield(L, -2, "Variant");

    class_setup(L, C_KEYFILE,   KEYFILE_M,   NULL, NULL);
    lua_setfield(L, -2, "KeyFile");

    class_setup(L, C_DATETIME,  DATETIME_M,  NULL, NULL);
    lua_setfield(L, -2, "DateTime");

    class_setup(L, C_TIMEZONE,  TIMEZONE_M,  NULL, NULL);
    lua_setfield(L, -2, "TimeZone");

    class_setup(L, C_TIMER,     TIMER_M,     NULL, NULL);
    lua_setfield(L, -2, "Timer");

    class_setup(L, C_STRING,    STRING_M,    NULL, NULL);
    lua_setfield(L, -2, "String");

    /* -- Enum tables: exposed as `glib.Priority.HIGH`,
     * `glib.Source.CONTINUE`, `glib.KeyFileFlags.NONE`. -- */
    install_enum(L, "Priority",     PRIORITY,     sizeof(PRIORITY) / sizeof(PRIORITY[0]));
    install_enum(L, "Source",       SOURCE,       sizeof(SOURCE) / sizeof(SOURCE[0]));
    install_enum(L, "KeyFileFlags", KEYFILEFLAGS, sizeof(KEYFILEFLAGS) / sizeof(KEYFILEFLAGS[0]));

    /* -- Flat integer constants --
     *
     * lib/gears/timer.lua reads these unqualified: `glib.PRIORITY_DEFAULT`
     * at :102, `glib.SOURCE_CONTINUE` at :104, `glib.SOURCE_REMOVE` at :118.
     * Real lgi puts them straight on the module table as plain integers, so
     * mirror that. The nested Priority/Source tables above stay for
     * completeness; these flat values are the ones the codebase actually
     * reads. timeout_add() defaults a missing priority to G_PRIORITY_DEFAULT
     * and timeout_cb() treats a nil return as G_SOURCE_CONTINUE, so the
     * timers survive even if a caller forgets to reference them. */
    lua_pushinteger(L, G_PRIORITY_DEFAULT);
    lua_setfield(L, -2, "PRIORITY_DEFAULT");
    lua_pushinteger(L, G_SOURCE_CONTINUE);
    lua_setfield(L, -2, "SOURCE_CONTINUE");
    lua_pushinteger(L, G_SOURCE_REMOVE);
    lua_setfield(L, -2, "SOURCE_REMOVE");

    /* -- Utility functions (no `self`; called as glib.foo(...)) -- */
    static const luaL_Reg MODULE_F[] = {
        /* P0 main loop */
        { "timeout_add",      glib_timeout_add },
        { "source_remove",    glib_source_remove },
        /* P1 paths */
        { "build_filenamev",     glib_build_filenamev },
        { "get_home_dir",        glib_get_home_dir },
        { "get_user_data_dir",   glib_get_user_data_dir },
        { "get_system_data_dirs", glib_get_system_data_dirs },
        /* P1.5 env */
        { "getenv",               glib_getenv },
        { "setenv",               glib_setenv },
        { "find_program_in_path", glib_find_program_in_path },
        /* P2 UTF-8 */
        { "utf8_strlen",     glib_utf8_strlen },
        { "utf8_substring",  glib_utf8_substring },
        { NULL, NULL },
    };
    luaL_setfuncs(L, MODULE_F, 0);

    return 1;
}
