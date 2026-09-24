/*
 * lgishim_pixbuf.c - GdkPixbuf Lua binding for the LGI shim.
 *
 * Replaces `lgi.GdkPixbuf` for the small set of APIs actually used by
 * lib/: only `GdkPixbuf.Pixbuf.new_from_file(path)` is called
 * (lib/gears/surface.lua:59).  lib/ reads `pixbuf._native` and hands the
 * lightuserdatum straight to capi.awesome.pixbuf_to_surface (luaa.c:322),
 * which does `(GdkPixbuf *)lua_touserdata(L, 1)` and calls
 * draw_surface_from_pixbuf() (draw.c:85).
 *
 * Object model matches lgishim_pango.c / lgishim_cairo.c: every pixbuf is
 * a full userdata holding { ptr, destroy, cls }; `__gc` calls
 * g_object_unref (GdkPixbuf is a GObject: GDK_TYPE_PIXBUF).  Ownership is
 * transferred to the wrap, so `new_from_file` never returns a borrowed
 * pointer.
 *
 * Deliberate design choice: `._native` is registered in the property-
 * getter table (pg), NOT on the metatable as a C closure.  With m_index
 * the pg path auto-invokes the closure with obj as arg and returns the
 * resulting value (lgishim_pango.c:795-805).  The cairo / pango shims put
 * `_native` on the metatable (falling through to the closure-returning
 * fallback at :817-826), which matches LGI semantics for those types but
 * NOT for pixbuf -- lib/gears/surface.lua:63 uses `pixbuf._native`
 * without parens, and lua_touserdata on a Lua closure returns NULL, which
 * would crash draw_surface_from_pixbuf(NULL).  Registering in pg gives us
 * `pixbuf._native` == lightuserdata, which is what luaa.c:322 needs.
 *
 * Alias convention: both `GdkPixbuf.Pixbuf.new_from_file` and
 * `gdkpixbuf.pixbuf.new_from_file` resolve -- the module table sets both
 * `GdkPixbuf` and `gdkpixbuf` (mirroring lgishim.lua's `M.cairo =
 * M.Cairo` pattern), and the inner table carries `Pixbuf` / `pixbuf`.
 *
 * Compile flags (see CMakeLists.txt:116-124, spike4-c-api-conventions.md):
 *   -Wundef, -Wmissing-prototypes, -Wstrict-prototypes, -Wall, -Wextra,
 *   -Wno-unused-parameter, -Wuninitialized, -Wsign-compare, -Wformat-nonliteral.
 */

#include <lauxlib.h>
#include <lua.h>

#include <stddef.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib-object.h>

/* Exported to luaa.c, which registers it in package.preload. */
int luaopen_lgishim_pixbuf(lua_State *L);

enum {
    C_PIXBUF = 1,
    N_CLASSES
};

/* Namespaced with lgishim_pixbuf. prefix to keep the metatable key distinct
 * from lgishim_pango.* and lgishim.* (Spike 2 Bug 2 / record2.md §4.3.1):
 * luaL_newmetatable() adopts an already-registered name rather than
 * creating one, so a duplicate would silently take over the first module's
 * metatable. */
static const char *CLS_NAMES[N_CLASSES] = {
    NULL,
    "lgishim_pixbuf.Pixbuf",
};

/* Split registry key from lgishim.* and lgishim_pango.* so the per-module
 * class numbers do not collide (cairo C_SURFACE=2 vs pango C_LAYOUT=2 vs
 * pixbuf C_PIXBUF=1).  The registry array is indexed by w->cls, so sharing
 * the key across modules would write one module's clsinfo over another's. */
#define REG_CLASSES "lgishim_pixbuf_classes"

typedef struct {
    void *p;
    void (*destroy)(void *);
    int cls;
} wrap;

/* --------------------------------------------------------------- helpers */

/* GdkPixbuf is a GObject: destroy with g_object_unref, not free().
 * gdk_pixbuf_new_from_file returns a refcount-1 pixbuf (GdkPixbuf docs),
 * so the wrap owns exactly one reference that the __gc callback releases.
 * Contrast: pango's FontDescription / FontMetrics are GBoxed and need
 * pango_font_description_free / pango_font_metrics_unref (record.md §9.4 #4). */
static void d_pixbuf(void *p)
{
    g_object_unref((GObject *)p);
}

static void d_noop(void *p)
{
    (void)p;
}

static wrap *
getud(lua_State *L, int idx)
{
    return lua_touserdata(L, idx);
}

static void *
chk(lua_State *L, int idx)
{
    wrap *w = getud(L, idx);
    if (!w)
        luaL_error(L, "lgishim_pixbuf: expected object at arg %d, got %s",
                   idx, lua_isnil(L, idx) ? "nil" : luaL_typename(L, idx));
    return w->p;
}

static int
push_obj(lua_State *L, void *p, void (*destroy)(void *), int cls)
{
    wrap *w;

    /* Order matters: luaL_setmetatable() is luaL_getmetatable() +
     * lua_setmetatable(l,-2) and already pops the metatable, so the
     * userdata must be on the stack *before* the call.  A second
     * lua_setmetatable would install the userdata itself as someone else's
     * metatable (Spike 2 Bug 2, lgishim_pango.c:187-192). */
    w = (wrap *)lua_newuserdata(L, sizeof(wrap));
    w->p = p;
    w->destroy = destroy;
    w->cls = cls;
    luaL_setmetatable(L, CLS_NAMES[cls]);
    return 1;
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

    /* Stack on entry: [obj, key].  Result goes to slot 3 (return 1 gives
     * it back to __index), so every lookup is [obj, key, REG_CLASSES, ...]
     * and the found value is swapped into slot 3 with lua_replace before
     * truncating with lua_settop(L, 3). */
    lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
    lua_rawgeti(L, -1, w->cls);                /* [.., clsinfo] */

    /* Property getters: pixbuf._native (VALUE, not closure -- see file
     * header).  The closure is called with obj as self and its return is
     * what the caller sees. */
    lua_getfield(L, -1, "g");                  /* [.., clsinfo, g] */
    lua_getfield(L, -1, k);                    /* [.., clsinfo, g, getter] */
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);                   /* [.., g, getter, obj] */
        lua_call(L, 1, 1);                     /* [.., g, result] */
        lua_replace(L, 3);                     /* result -> slot 3 */
        lua_settop(L, 3);                      /* [obj, key, result] */
        return 1;
    }
    lua_pop(L, 2);                             /* value, getter table */

    /* Methods: returned as a plain function. */
    lua_getfield(L, -1, "m");
    lua_getfield(L, -1, k);
    if (lua_isfunction(L, -1)) {
        lua_replace(L, 3);
        lua_settop(L, 3);                      /* [obj, key, method] */
        return 1;
    }
    lua_pop(L, 1);                             /* nil */

    return 0;
}

/* m_newindex is not currently needed (no lib/ site assigns pixbuf properties)
 * but is included for completeness / future growth; returning 0 here means
 * "no assignment intercepted", which is what we want for a read-only class. */
static int
m_newindex(lua_State *L)
{
    (void)L;
    return 0;
}

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
m_gc(lua_State *L)
{
    release(getud(L, 1));
    return 0;
}

/* Type check.  Registered as a closure over the class's OWN metatable
 * (upvalue 1) and attached to the class table, so lib/ can call it in
 * colon form `Pixbuf:is_type_of(x)` -- the class table lands at slot 1
 * and the object under test at slot 2.
 *
 * Discriminator is metatable *identity*, deliberately not w->cls: cairo,
 * pango and pixbuf each have their own cls enumeration (cairo C_CTX=1,
 * pango C_ATTRLIST=1, pixbuf C_PIXBUF=1), so an index/strcmp test lets a
 * Pango.AttrList pass as a cairo.Context.  Each module registers its
 * metatables under its own prefix (lgishim_pixbuf.Pixbuf), and every
 * userdata carries exactly one of them, so comparing the two tables is
 * sound across module boundaries. */
static int
m_is_type_of(lua_State *L)
{
    int same = 0;

    lua_pushvalue(L, lua_upvalueindex(1));     /* [.., expected_mt] */
    if (lua_getmetatable(L, 2)) {              /* [.., expected_mt, obj_mt] */
        same = lua_rawequal(L, -2, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);                             /* [..] */
    lua_pushboolean(L, same);
    return 1;
}

/* Constructor: class table + `new` method + is_type_of.  The pixbuf class
 * has no static constructor alias (`Pixbuf(path)` is not called anywhere
 * in lib/, only `Pixbuf.new_from_file(path)`), so `__call` is deliberately
 * NOT installed -- that means Pixbuf is not callable, which matches what
 * lib/ actually uses. */
static void
make_class_table(lua_State *L, int cls, const luaL_Reg *m)
{
    lua_newtable(L);                           /* class table */
    luaL_setfuncs(L, m, 0);

    /* Attach a static is_type_of on the class table.  lua_getmetatable
     * retrieves the metatable table this function just built from the
     * registry; the closure captures it so is_type_of can compare
     * metatable identity. */
    luaL_getmetatable(L, CLS_NAMES[cls]);
    lua_pushcclosure(L, m_is_type_of, 1);
    lua_setfield(L, -2, "is_type_of");
}

/*
 * Build the metatable, the per-class registry entry and the Lua-facing
 * class table.  Leaves the class table on top of the stack.
 */
static void
class_setup(lua_State *L, int cls, const luaL_Reg *m,
            const luaL_Reg *pg, const luaL_Reg *p)
{
    luaL_newmetatable(L, CLS_NAMES[cls]);
    lua_pushcfunction(L, m_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, m_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, m_gc);
    lua_setfield(L, -2, "__gc");
    luaL_setfuncs(L, m, 0);
    if (p)
        luaL_setfuncs(L, p, 0);
    lua_pop(L, 1);                             /* metatable */

    lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
        lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES); /* re-push; setfield
                  * above consumed the newly created table, so without this
                  * line rawseti(-2) below would write clsinfo into the
                  * module table and lua_pop(1) would drop the module. */
    }
    lua_newtable(L);                           /* clsinfo */
    lua_newtable(L);
    luaL_setfuncs(L, m, 0);
    lua_setfield(L, -2, "m");
    lua_newtable(L);
    if (pg)
        luaL_setfuncs(L, pg, 0);
    lua_setfield(L, -2, "g");
    lua_newtable(L);
    if (p)
        luaL_setfuncs(L, p, 0);
    lua_setfield(L, -2, "p");
    lua_rawseti(L, -2, cls);
    lua_pop(L, 1);                             /* registry array */

    make_class_table(L, cls, m);
}

/* ================================================================ API
 *
 * Return-value conventions dictated by lib/:
 *   - new_from_file returns (pixbuf, err) -- surface.lua:59-62 destructures
 *     as `local pixbuf, err = ...; if not pixbuf then ... tostring(err)`.
 *     On failure we return exactly (nil, "msg") so tostring(err) works.
 *   - `pixbuf._native` returns lightuserdata (the raw GdkPixbuf*), NOT a
 *     closure.  luaa.c:322 does `lua_touserdata(L, 1)`, and
 *     lua_touserdata on a Lua function value returns NULL; if we wrapped
 *     _native in a closure, draw_surface_from_pixbuf(NULL) would deref.
 */

static int
pixbuf_new_from_file(lua_State *L)
{
    const char *path;
    GdkPixbuf *pixbuf;
    GError *error = NULL;

    /* luaL_checkstring raises on non-string; lib/ always passes a string
     * (surface.lua:59 only reaches here inside `type(_surface)=="string"`). */
    path = luaL_checkstring(L, 1);

    pixbuf = gdk_pixbuf_new_from_file(path, &error);
    if (!pixbuf) {
        if (!error)
            error = g_error_new_literal(GDK_PIXBUF_ERROR,
                                        GDK_PIXBUF_ERROR_FAILED,
                                        "unknown error");
        /* (nil, msg) -- the `not pixbuf` branch in surface.lua:60 then calls
         * tostring(err) and hands it to the caller as the load error. */
        lua_pushnil(L);
        lua_pushstring(L, error->message);
        g_error_free(error);
        return 2;
    }
    return push_obj(L, pixbuf, d_pixbuf, C_PIXBUF);
}

static int
pixbuf_finish(lua_State *L)
{
    release(getud(L, 1));
    return 0;
}

/* Property getter -- the closure in the g table is auto-invoked by
 * m_index with obj as self (see m_index pg path above).  It returns
 * lightuserdata (NOT the wrap userdata): luaa.c:322 does
 * `(GdkPixbuf *)lua_touserdata(L, 1)`, which needs a lightuserdata or a
 * full userdata whose pointer *is* GdkPixbuf*.  A wrap userdata would
 * give lua_touserdata the wrap address, not the pixbuf pointer; the wrap
 * would also be a wrong type for draw_surface_from_pixbuf. */
static int
pixbuf_get_native(lua_State *L)
{
    GdkPixbuf *p = (GdkPixbuf *)chk(L, 1);
    if (!p)
        return luaL_error(L, "lgishim_pixbuf: pixbuf already released");
    lua_pushlightuserdata(L, p);
    return 1;
}

static const luaL_Reg PIXBUF_M[] = {
    { "new",       pixbuf_new_from_file },
    { "new_from_file", pixbuf_new_from_file },
    { "finish",    pixbuf_finish },
    { NULL, NULL },
};

static const luaL_Reg PIXBUF_PG[] = {
    { "_native", pixbuf_get_native },
    { NULL, NULL },
};

/* ---------------------------------------------------- module initialiser */

int
luaopen_lgishim_pixbuf(lua_State *L)
{
    lua_newtable(L);                           /* module table */

    class_setup(L, C_PIXBUF, PIXBUF_M, PIXBUF_PG, NULL);
    lua_setfield(L, -2, "Pixbuf");             /* lgi.GdkPixbuf.Pixbuf */

    /* The module-level aliases (GdkPixbuf/gdkpixbuf and the lowercase
     * `pixbuf` class alias) are not installed here: lua_setfield() pops the
     * class table, so anything pushed afterwards points at the module table
     * itself, and a lua_pushvalue() copy is a distinct table. lgishim.lua owns
     * all of them (M.GdkPixbuf / M.gdkpixbuf / M.GdkPixbuf.pixbuf), where a
     * plain assignment is a genuine identity copy, which is where lib/ actually
     * reaches for them. */
    return 1;
}
