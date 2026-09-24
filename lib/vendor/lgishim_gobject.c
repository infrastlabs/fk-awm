/*
 * lgishim_gobject.c - GObject Lua binding for the LGI shim.
 *
 * Replaces `lgi.GObject` for the single API lib/ actually uses, so the four
 * D-Bus callbacks in naughty/dbus.lua can be registered without the
 * gobject-introspection runtime. Same wrapper object model as
 * lgishim_cairo.c / lgishim_glib.c: every object is a full userdata holding
 * { ptr, destroy, cls }; `__gc` calls destroy(); `_native` returns
 * lightuserdata, the only channel across the C boundary.
 *
 * Public surface (verified by grep over lib/ excluding vendor/):
 *
 *   GObject.Closure(fn)      naughty/dbus.lua:434, 541, 542, 542
 *
 * Nothing else. The other ~20 `gobject` hits in lib/ are
 * `require("gears.object")` -- awesomeWM's Lua-side signal system, not
 * GObject -- and every C-file hit is a comment.
 *
 * --- GClosure <-> Lua association ------------------------------------------
 * A GClosure can hold only a C function pointer plus one gpointer, never a
 * Lua function. So Closure(fn) parks the function in the Lua registry:
 *
 *   cbdata { lua_State *L; int ref; }          ref == luaL_ref(fn)
 *   g_cclosure_new_swap(sentinel, cbdata, cbdata_destroy)
 *   g_closure_set_marshal(closure, closure_marshal)
 *
 * closure_marshal() is a CUSTOM GClosureMarshal and it does all the work. Two
 * things make that necessary:
 *
 *   1. g_cclosure_new_swap() leaves closure->marshal == NULL. g_closure_invoke
 *      asserts `closure->marshal || real_closure->meta_marshal' and bails, so a
 *      marshaler has to be installed by somebody.
 *   2. A GClosureMarshal is the ONLY hook GLib hands n_param_values to.
 *      g_cclosure_marshal_generic() calls a plain C function, which cannot
 *      count its own arguments, so a real handler could not decide how many
 *      GValues to turn into Lua arguments.
 *
 * Because the custom marshaler replaces the generic one, GLib never calls the
 * C callback slot at all: g_closure_marshal() invokes closure->marshal and
 * returns. sentinel() is therefore dead code, kept only because
 * g_cclosure_new_swap() requires a GCallback. That was verified empirically,
 * not assumed.
 *
 * g_cclosure_new_swap() rather than g_cclosure_new(): in this build (GLib
 * 2.70.5) swap puts user_data in argument position 1 and non-swap puts it
 * last. Either reaches closure->data without knowing the arity, but swap is
 * the one to use if the generic marshaler is ever restored in its place.
 *
 * Note also that this build's g_cclosure_new_swap() takes THREE arguments
 * (callback, user_data, destroy_notify) -- not the four-argument form with a
 * GTypeInterface that older GLib versions and the lgi sources show.
 *
 * The argument order delivered to the Lua function is GLib's documented
 * callback signature MINUS the trailing user_data, which is how lib/ was
 * written:
 *
 *   method_call(conn, sender, object_path, iface, method, parameters, invocation)
 *   on_bus_acquire(conn, nil)
 *   on_name_acquired(conn, name, nil)
 *   on_name_lost(name, nil)
 *
 * dbus.lua:380 / :394 / :532 / :536 spell them out as `(_, sender, ...)`,
 * `(conn, _)`, `(conn, _)`, `(_, _)` -- those `_` placeholders are exactly the
 * slots user_data would have occupied.
 *
 * --- Return value ----------------------------------------------------------
 * GLib passes a pre-initialised GValue as return_value (G_TYPE_BOOLEAN for
 * GDBusMethodCallCallback). A truthy Lua return maps to TRUE; nil and false
 * map to FALSE. That is safe for dbus.lua, whose notif_methods reply through
 * invocation:return_value() (dbus.lua:352, 356, 364, 369, 377) and return
 * nothing: modern GLib only answers G_DBUS_ERROR_METHOD_NOT_IMPLEMENTED when
 * the invocation was not already handled
 * (g_dbus_method_invocation_get_handled()). This matches lgi's mapping.
 *
 * --- Cross-thread assumption -----------------------------------------------
 * ASSUMPTION: every callback above is dispatched on the thread that owns the
 * lua_State. That holds today because the GLib default main context is polled
 * from awesome's own event loop (awesome.c:890 installs a_glib_poll() as the
 * poll func; a_glib_poll is at awesome.c:448) and dbus.lua uses
 * Gio.BusType.SESSION, which is that default context.
 *
 * THE ASSUMPTION BREAKS IF a future Gio shim moves the default context to
 * another thread (g_main_context_push_thread_default) or gives a GDBusConnection
 * its own dedicated context/thread. Then a closure must not touch the
 * lua_State at all -- marshal back to the main thread first, via g_task_new /
 * g_return_source_new or a queue drained by an idle source. A lua_State must
 * never be touched from two threads: there is no lock around it, so a second
 * thread is undefined behaviour rather than a mere race.
 *
 * --- Callback errors -------------------------------------------------------
 * A raising callback is caught with lua_pcall() and the message printed with
 * g_printerr(). lgishim_glib.c's timeout_cb() propagates with a bare
 * lua_call(), but these closures can fire from D-Bus, i.e. from an arbitrary
 * other process, so a callback error must not abort the main loop. The cost is
 * that the error is logged rather than raised at its origin.
 *
 * --- Six bug-avoidance rules (lgishim_pango.c / lgishim_cairo.c lessons) ---
 *   1. lua_setfield(REG_CLASSES) pops the table -- re-fetch with lua_getfield
 *      before writing clsinfo (see class_setup).
 *   2. luaL_setmetatable already pops the metatable -- no second lua_setmetatable.
 *   3. m_index uses lua_replace(L, 3) BEFORE lua_settop(L, 3).
 *   4. Closure ref guard: luaL_ref() at construction, luaL_unref() in
 *      cbdata_destroy AND in __gc -- idempotent via the LUA_NOREF sentinel.
 *   5. release() NULLs ptr and swaps destroy to d_noop -- no double-free.
 *   6. lua_pushstring only -- NEVER lua_pushlstring(..., -1).
 */

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <stdlib.h>
#include <string.h>

#include <glib-object.h>

/* Exported to luaa.c, which registers it in package.preload. */
int luaopen_lgishim_gobject(lua_State *L);

/* Exported to the Gio shim: unwrap a `GObject.Closure` userdata and hand back
 * the real GClosure*. Mirrors lgishim_pango_check_layout(). */
GClosure *lgishim_gobject_check_closure(lua_State *L, int idx);

enum {
    C_CLOSURE = 1,
    C_OBJECT = 2,
    N_GOBJECT_CLASSES
};

static const char *CLS_NAMES[N_GOBJECT_CLASSES] = {
    NULL,
    "lgishim.GObject.Closure",
    "lgishim.GObject.Object",
};

#define REG_CLASSES "lgishim_gobject_classes"

typedef struct {
    void *p;
    void (*destroy)(void *);
    int cls;
} wrap;

/* --------------------------------------------------------------- helpers */

static void d_noop(void *p) { (void)p; }
static void d_closure(void *p) { g_closure_unref((GClosure *)p); }
static void d_object(void *p) { g_object_unref((GObject *)p); }

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
        luaL_error(L, "lgishim.GObject: expected object at arg %d, got %s",
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
     * metatable, so the userdata has to sit underneath it. A second
     * lua_setmetatable would install the userdata itself as somebody else's
     * metatable. */
    w = (wrap *)lua_newuserdata(L, sizeof(wrap));
    w->p = p;
    w->destroy = destroy;
    w->cls = cls;
    luaL_setmetatable(L, CLS_NAMES[cls]);
    return 1;
}

GClosure *
lgishim_gobject_check_closure(lua_State *L, int idx)
{
    return (GClosure *)chk(L, idx);
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

    /* Stack on entry: [obj, key]. The result has to end up at slot 3 so that
     * `return 1` hands it back to __index, so every lookup proceeds as
     * [obj, key, REG_CLASSES, ...] and gets swapped into slot 3 with
     * lua_replace before truncating via lua_settop(L, 3). */
    lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
    lua_rawgeti(L, -1, w->cls);                /* [.., clsinfo] */

    /* Property getters. */
    lua_getfield(L, -1, "g");
    lua_getfield(L, -1, k);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        lua_replace(L, 3);
        lua_settop(L, 3);
        return 1;
    }
    lua_pop(L, 2);

    /* Methods: handed back as a plain function. */
    lua_getfield(L, -1, "m");
    lua_getfield(L, -1, k);
    if (lua_isfunction(L, -1)) {
        lua_replace(L, 3);
        lua_settop(L, 3);
        return 1;
    }
    lua_pop(L, 2);

    /* Property setters, resolved so lib/ can call them in method form. */
    lua_getfield(L, -1, "p");
    lua_getfield(L, -1, k);
    if (lua_isfunction(L, -1)) {
        lua_replace(L, 3);
        lua_settop(L, 3);
        return 1;
    }
    lua_pop(L, 2);

    /* Helpers living on the metatable (_native, finish). __index is a
     * function, so a plain field there is never found any other way. */
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
        /* Pass (self, value); the assignment target is stack slot 3. */
        lua_pushvalue(L, 1);
        lua_pushvalue(L, 3);
        lua_call(L, 2, 0);
        return 0;
    }
    lua_pop(L, 4);
    luaL_error(L, "lgishim.GObject: no such property '%s'", k);
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

    /* The discriminator is metatable IDENTITY, deliberately not w->cls: cairo,
     * glib and this module each define the same `wrap` layout with their own
     * cls enumeration, so an index comparison lets a Pango.Color pass as a
     * cairo FontOptions. Each module registers its metatables under its own
     * name and every userdata carries exactly one of them, so comparing the
     * tables is sound across module boundaries. */

    lua_pushvalue(L, lua_upvalueindex(1));     /* [.., expected_mt] */
    if (lua_getmetatable(L, 2)) {              /* [.., expected_mt, obj_mt] */
        same = lua_rawequal(L, -2, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);                             /* [..] */
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
    /*
     * After lua_setmetatable the stack is [class table]. Push the
     * userdata's registered metatable (CLS_NAMES[cls]) — that is the table
     * m_is_type_of compares against, not the class table's own __call
     * metatable. lua_pushcclosure() pops it as the upvalue and pushes the
     * closure, so the stack becomes [class table, closure] and -2 is the
     * class table.
     */
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
    lua_pushcfunction(L, m_native);
    lua_setfield(L, -2, "_native");
    lua_pushcfunction(L, m_finish);
    lua_setfield(L, -2, "finish");
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
        lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES); /* re-push: setfield
                   above consumed the table, so without this line rawseti(-2)
                   would write clsinfo into the module table and lua_pop(1)
                   would drop the module. */
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

/* ================================================================
 * Closure -- the whole public API of this module.
 *
 *   GObject.Closure(fn) -> userdata wrapping a real GClosure*
 *
 * Lifecycle of the Lua function ref, matching lgishim_glib.c's timer closure
 * guard exactly:
 *
 *   1. closure_new() luaL_ref()s `fn` and hands the id to cbdata.
 *   2. cbdata is user_data, so GLib owns it and calls cbdata_destroy() once
 *      when the closure's refcount reaches zero. That is the only path that
 *      sees a GClosure dying under us, so it is the primary releaser.
 *   3. __gc runs when the WRAPPER userdata is collected, which can happen first
 *      (d_closure() drops our ref) or after cbdata_destroy() already ran.
 *      Setting cbdata->ref = LUA_NOREF before unref makes both paths
 *      idempotent, and cbdata_destroy() checks the same sentinel, so the ref
 *      is released exactly once and cbdata exactly once.
 *
 * Refcount accounting: g_cclosure_new_swap() returns a closure with refcount 1.
 * That ref belongs to this userdata (d_closure releases it). Callers that keep
 * the GClosure* beyond the userdata -- the Gio shim does -- must g_closure_ref()
 * it themselves, which is the normal GClosure discipline.
 */

typedef struct {
    lua_State *L;
    int ref;
} cbdata;

static void
cbdata_destroy(gpointer user_data, GClosure *closure)
{
    cbdata *d = (cbdata *)user_data;
    (void)closure;

    if (!d)
        return;
    if (d->ref != LUA_NOREF) {
        if (d->ref >= 0)
            luaL_unref(d->L, LUA_REGISTRYINDEX, d->ref);
        d->ref = LUA_NOREF;
    }
    free(d);
}

/*
 * GValue -> Lua value. Objects are g_object_ref()ed and wrapped, so the GLib
 * side reference is released by d_object() when the userdata is collected.
 * Boxed payloads arrive as lightuserdata -- the GLib shim owns GLib.Variant
 * wrapping and can install its own marshaler to upgrade that.
 *
 * G_TYPE_POINTER is not registered in this build (g_type_from_name("gpointer")
 * returns G_TYPE_INVALID), so pointer-shaped values must come in as G_TYPE_INT
 * or G_TYPE_INT64, which is what GLib does internally.
 */
static void
push_gvalue(lua_State *L, const GValue *v)
{
    GType t = G_VALUE_TYPE(v);

    if (G_VALUE_HOLDS_OBJECT(v)) {
        gpointer o = g_value_get_object(v);
        if (!o) {
            lua_pushnil(L);
            return;
        }
        g_object_ref((GObject *)o);
        push_obj(L, o, d_object, C_OBJECT);
        return;
    }

    switch (t) {
    case G_TYPE_STRING: {
        const char *s = g_value_get_string(v);
        lua_pushstring(L, s ? s : "");
        return;
    }
    case G_TYPE_BOOLEAN:
        lua_pushboolean(L, g_value_get_boolean(v));
        return;
    case G_TYPE_CHAR:   lua_pushinteger(L, (lua_Integer)g_value_get_schar(v)); return;
    case G_TYPE_UCHAR:  lua_pushinteger(L, (lua_Integer)g_value_get_uchar(v)); return;
    case G_TYPE_INT:    lua_pushinteger(L, (lua_Integer)g_value_get_int(v)); return;
    case G_TYPE_UINT:   lua_pushinteger(L, (lua_Integer)g_value_get_uint(v)); return;
    case G_TYPE_LONG:   lua_pushinteger(L, (lua_Integer)g_value_get_long(v)); return;
    case G_TYPE_ULONG:  lua_pushinteger(L, (lua_Integer)g_value_get_ulong(v)); return;
    case G_TYPE_INT64:  lua_pushinteger(L, (lua_Integer)g_value_get_int64(v)); return;
    case G_TYPE_UINT64: lua_pushinteger(L, (lua_Integer)g_value_get_uint64(v)); return;
    case G_TYPE_FLOAT:  lua_pushnumber(L, (lua_Number)g_value_get_float(v)); return;
    case G_TYPE_DOUBLE: lua_pushnumber(L, g_value_get_double(v)); return;
    default:            break;
    }

    if (g_type_is_a(t, G_TYPE_BOXED)) {
        lua_pushlightuserdata(L, g_value_get_boxed(v));
        return;
    }
    lua_pushnil(L);
}

/*
 * The custom marshaler. See the header for why this is a GClosureMarshal and
 * not a C callback: only a marshaler receives n_param_values.
 *
 * n_param_values is the number of arguments EXCLUDING user_data -- GLib puts
 * user_data in closure->data and never appends it to param_values. So the
 * Lua function sees exactly the documented callback arguments, which is the
 * shape lib/ was written against.
 */
static void
closure_marshal(GClosure *closure, GValue *return_value,
                guint n_param_values, const GValue *param_values,
                gpointer invocation_hint, gpointer marshal_data)
{
    cbdata *d;
    lua_State *L;
    guint i;

    (void)invocation_hint;
    (void)marshal_data;

    if (!closure || closure->is_invalid)
        return;

    d = (cbdata *)closure->data;
    if (!d || d->ref == LUA_NOREF)
        return;
    L = d->L;

    lua_rawgeti(L, LUA_REGISTRYINDEX, d->ref);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    for (i = 0; i < n_param_values; i++)
        push_gvalue(L, &param_values[i]);

    /* [fn, arg1, ..., argN] -- call fn with the GValue args.
     * lua_pcall catches Lua errors from user callbacks so a raise inside
     * the callback does not abort the GLib dispatch loop. */
    if (lua_pcall(L, n_param_values, 1, 0) != 0) {
        g_printerr("lgishim.GObject.Closure callback error: %s\n",
                   lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    if (return_value) {
        /* G_TYPE_INVALID is 0, which is also what an uninitialised GValue
         * carries, so an uninitialised return_value falls through here. */
        GType rt = G_VALUE_TYPE(return_value);
        if (rt == G_TYPE_BOOLEAN)
            g_value_set_boolean(return_value,
                                lua_toboolean(L, -1) ? TRUE : FALSE);
        else if (rt == G_TYPE_INT)
            g_value_set_int(return_value, (gint)luaL_optinteger(L, -1, 0));
        else if (rt == G_TYPE_INT64)
            g_value_set_int64(return_value, (gint64)luaL_optinteger(L, -1, 0));
    }

    /* The callback context and user code share the same lua_State, so the
     * stack has to be left empty for whatever dispatches next. */
    lua_settop(L, 0);
}

/*
 * Required by g_cclosure_new_swap() but never executed: closure_marshal()
 * replaces g_cclosure_marshal_generic(), and g_closure_marshal() calls
 * closure->marshal directly, so the callback slot is bypassed. Retained so the
 * construction call has a well-typed GCallback to store.
 */
static void
sentinel(void)
{
}

static int
closure_new(lua_State *L)
{
    cbdata *d;
    GClosure *cl;

    luaL_checktype(L, 1, LUA_TFUNCTION);

    d = (cbdata *)malloc(sizeof(cbdata));
    if (!d)
        return luaL_error(L, "lgishim.GObject.Closure: out of memory");
    d->L = L;
    d->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (d->ref == LUA_NOREF) {
        free(d);
        return luaL_error(L, "lgishim.GObject.Closure: luaL_ref failed");
    }

    cl = g_cclosure_new_swap(G_CALLBACK(sentinel), d, cbdata_destroy);
    if (!cl) {
        luaL_unref(L, LUA_REGISTRYINDEX, d->ref);
        d->ref = LUA_NOREF;
        free(d);
        return luaL_error(L, "lgishim.GObject.Closure: g_cclosure_new_swap failed");
    }

    /* Mandatory: the closure comes back with closure->marshal == NULL and
     * g_closure_invoke() aborts on that. */
    g_closure_set_marshal(cl, closure_marshal);

    return push_obj(L, cl, d_closure, C_CLOSURE);
}

/*
 * `_native` is registered as a PROPERTY GETTER, not as a metatable field.
 * m_index's getter path auto-invokes the closure with obj as its argument and
 * returns the resulting value, so `closure._native` is the raw GClosure*
 * (lightuserdata). A metatable field would hand back the closure itself, and
 * lua_touserdata() on a Lua function yields NULL.
 *
 * lgishim_pixbuf.c documents the same reasoning for GdkPixbuf. The cairo /
 * pango / glib shims put _native on the metatable instead.
 */
static const luaL_Reg NATIVE_PG[] = {
    { "_native", m_native },
    { NULL, NULL },
};

static const luaL_Reg CLOSURE_M[] = {
    { "new", closure_new },
    { "finish", m_finish },
    { NULL, NULL },
};

/* C_OBJECT instances are the GObjects arriving as callback arguments. The
 * class exists only so m_index / is_type_of have a clsinfo entry to look up;
 * it is deliberately NOT published on the module table, because lib/ has no
 * call site for it. */
static const luaL_Reg OBJECT_M[] = {
    { "finish", m_finish },
    { NULL, NULL },
};

/* ---------------------------------------------------- module initialiser */

int
luaopen_lgishim_gobject(lua_State *L)
{
    lua_newtable(L);                                  /* module */

    /* Pre-create REG_CLASSES so class_setup() takes the non-nil branch. */
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, REG_CLASSES);

    class_setup(L, C_OBJECT, OBJECT_M, NATIVE_PG, NULL);
    lua_pop(L, 1);                                    /* not exported */

    class_setup(L, C_CLOSURE, CLOSURE_M, NATIVE_PG, NULL);
    lua_setfield(L, -2, "Closure");

    return 1;
}
