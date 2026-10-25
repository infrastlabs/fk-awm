/*
 * lgishim_gio.c - Gio Lua binding for the LGI shim.
 *
 * Replaces `lgi.Gio` for the subset awesomewm's lib/ actually uses, so that
 * gears/filesystem.lua (GFile + GFileInfo + GFileEnumerator), awful/spawn.lua
 * (DataInputStream over MemoryInputStream / UnixInputStream), menubar/utils.lua
 * (enumerate_children + get_child + parse_dir) and naughty/dbus.lua (session
 * bus: own_name, register_object, emit_signal, proxy call_sync) all work
 * without the gobject-introspection runtime.
 *
 * Same wrapper object model as lgishim_glib.c / lgishim_gobject.c /
 * lgishim_pixbuf.c: every object is a full userdata {ptr, destroy, cls};
 * `__gc` calls destroy(), `finish` releases early, `_native` returns the raw
 * pointer.
 *
 * Public API (docs/sam/spike4-phase2-05-glib-gio-scope.md, Gio section):
 *   File:             new_for_path, new_for_uri, new_for_commandline_arg,
 *                     get_parent, get_child, get_path, get_uri,
 *                     get_basename, get_display_name,
 *                     query_info, query_file_type, query_exists,
 *                     make_directory, make_directory_with_parents, delete,
 *                     enumerate_children, async_enumerate_children
 *   FileInfo:         get_name, get_display_name, get_file_type,
 *                     get_attribute_boolean, get_attribute_string
 *   FileEnumerator:   next_file, next_files, async_next_files, get_child,
 *                     get_container, has_pending, is_closed, close,
 *                     async_close
 *   DataInputStream:  new, read_line, read_line_async, read_line_finish,
 *                     set_buffer_size, close
 *   MemoryInputStream:new_from_data
 *   UnixInputStream:  new, get_fd
 *   InputStream:      class table only: read, read_all, read_async,
 *                     is_type_of; the same methods are also installed on the
 *                     three concrete stream metatables, since lib/ calls them
 *                     on instances
 *   AppInfo:          get_all, get_id, get_name, get_description,
 *                     get_executable, get_commandline, get_icon,
 *                     should_show, launch
 *   DesktopAppInfo:   new, get_string
 *   Icon:             to_string
 *   DBusConnection:   new_sync, call_sync, emit_signal, register_object,
 *                     close, close_sync, is_closed, flush_sync,
 *                     get_unique_name
 *   DBusProxy:        new_for_bus, new_for_bus_sync, call_sync, get_name,
 *                     get_object_path, get_interface_name
 *   DBusMethodInvocation: get_sender, get_object_path, get_interface_name,
 *                     get_method_name, get_parameters, return_value,
 *                     return_error
 *   DBusVariant:      new, value, type_string, n_children
 *   FakeInfo builders: DBusArgInfo, DBusMethodInfo, DBusSignalInfo,
 *                     DBusInterfaceInfo -- marshalled into a real
 *                     GDBusNodeInfo/GDBusInterfaceInfo pair through
 *                     g_dbus_node_info_new_for_xml()
 *   module functions: bus_get_sync, bus_own_name, Async.start, Async.call
 *   enum constants:   BusType, BusNameOwnerFlags, DBusProxyFlags,
 *                     DBusCallFlags, FileQueryInfoFlags, FileType,
 *                     IOErrorEnum, FILE_ATTRIBUTE_*
 *
 * Enum direction, matching LGI: constants going OUT are the C integers (they
 * are passed straight into glib calls), values coming IN are strings
 * ("DIRECTORY", "EXISTS"), which is what lib/ compares against. flags_arg()
 * accepts an integer, nil, or the empty table that gears/filesystem.lua:96
 * passes in for FileQueryInfoFlags.NONE.
 *
 * Six bug-avoidance rules inherited from the pango/cairo/glib/gobject shims:
 *   1. lua_setfield(REG_CLASSES) pops the table -- re-fetch with lua_getfield
 *      before writing clsinfo (see class_setup).
 *   2. luaL_setmetatable already pops the metatable -- no second
 *      lua_setmetatable.
 *   3. m_index uses lua_replace(L, 3) BEFORE lua_settop(L, 3).
 *   4. Callback refs are luaL_ref()ed at registration and luaL_unref()ed
 *      exactly once, by either the GLib destroy notify or __gc.
 *   5. release() NULLs ptr and swaps destroy to d_noop -- no double-free.
 *   6. lua_pushstring only -- never lua_pushlstring(..., -1).
 *
 * This glib build (2.70) has no g_file_info_free/unref, no
 * g_data_input_stream_set_buffer_size, no g_variant_type_peek_dict_entry, no
 * g_dbus_interface_info_get_name/get_methods, no g_gio_error_quark and no
 * g_variant_dup_variant; the per-function notes say what each is replaced by.
 *
 * GVariant ownership (glib 2.70, verified against the docs and the ABI):
 * call_sync(), call() and emit_signal() do NOT take ownership of a
 * non-floating variant, so a ref'd copy is passed and unref'ed here;
 * g_dbus_method_invocation_return_value() consumes only a *floating* variant
 * and never unrefs a non-floating one, so the same ref'd-copy discipline is
 * balanced in every path, including the since-2.48 no-reply-expectd `out:`
 * path; get_parameters() is transfer none (ref it), the proxy reply is
 * transfer full (wrap as is), and the vtable hands the invocation over with
 * transfer-full ownership, so ref'ing it before wrapping makes
 * return_value()'s unref and __gc's unref balance to zero.
 */

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>

#include <stdlib.h>
#include <string.h>

/* Exported to luaa.c, which registers it in package.preload. */
int luaopen_lgishim_gio(lua_State *L);

/* Exported by lgishim_gobject.c: unwrap a `GObject.Closure` userdata to the
 * GClosure* it holds. We only ever read the callback ref out of it. */
GClosure *lgishim_gobject_check_closure(lua_State *L, int idx);

/*
 * lgishim_gobject.c stores { lua_State *L; int ref; } in GClosure->data
 * (closure_new() installs it, closure_marshal() reads it back as
 * `(cbdata *)closure->data`). Reading it lets us call a `GObject.Closure`
 * callback from our own marshalling instead of routing through its
 * GValue marshaler, which is what lib/ expects (a `GObject.Closure` object is
 * handed to register_object() / bus_own_name() at naughty/dbus.lua:433).
 */
typedef struct {
    lua_State *L;
    int ref;
} gobject_cbdata;

/* The first three fields of every other module's wrap; we only read ->p. */
typedef struct {
    void *p;
    void (*destroy)(void *);
    int cls;
} foreign_wrap;

enum {
    C_FILE = 1,
    C_FILEINFO,
    C_FILEENUM,
    C_DATAIN,
    C_MEMIN,
    C_UNIXIN,
    C_DBUSCONN,
    C_DBUSPROXY,
    C_DBUSINV,
    C_DBUSVAR,
    C_DBUSARG,
    C_DBUSMETHOD,
    C_DBUSSIGNAL,
    C_DBUSIFACE,
    C_INSTREAM,
    C_APPINFO,
    C_DESKTOPAPPINFO,
    C_ICON,
    N_GIO_CLASSES
};

static const char *CLS_NAMES[N_GIO_CLASSES] = {
    NULL,
    "lgishim_gio.File",
    "lgishim_gio.FileInfo",
    "lgishim_gio.FileEnumerator",
    "lgishim_gio.DataInputStream",
    "lgishim_gio.MemoryInputStream",
    "lgishim_gio.UnixInputStream",
    "lgishim_gio.DBusConnection",
    "lgishim_gio.DBusProxy",
    "lgishim_gio.DBusMethodInvocation",
    "lgishim_gio.DBusVariant",
    "lgishim_gio.DBusArgInfo",
    "lgishim_gio.DBusMethodInfo",
    "lgishim_gio.DBusSignalInfo",
    "lgishim_gio.DBusInterfaceInfo",
    "lgishim_gio.InputStream",
    "lgishim_gio.AppInfo",
    "lgishim_gio.DesktopAppInfo",
    "lgishim_gio.Icon",
};

#define REG_CLASSES "lgishim_gio_classes"

typedef struct {
    void *p;
    void (*destroy)(void *);
    int cls;
} wrap;

/* --------------------------------------------------------------- helpers */

static void d_noop(void *p) { (void)p; }
static void d_object(void *p) { g_object_unref((GObject *)p); }
static void d_variant(void *p) { g_variant_unref((GVariant *)p); }

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
        luaL_error(L, "lgishim_gio: expected object at arg %d, got %s",
                   idx, w ? (w->p ? "a freed object" : luaL_typename(L, idx))
                          : luaL_typename(L, idx));
    return w->p;
}

static void *
chk_cls(lua_State *L, int idx, int cls)
{
    wrap *w = getud(L, idx);

    if (!w || !w->p)
        luaL_error(L, "lgishim_gio: expected %s at arg %d, got %s",
                   CLS_NAMES[cls], idx,
                   w ? (w->p ? CLS_NAMES[w->cls] : luaL_typename(L, idx))
                     : luaL_typename(L, idx));
    if (w->cls != cls)
        luaL_error(L, "lgishim_gio: expected %s at arg %d, got %s",
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
     * for field-style access such as `variant.value` (naughty/dbus.lua reads
     * `.value` without parens, so a method function would not do). */
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
    luaL_error(L, "lgishim_gio: object is read-only");
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
 * m_index's property-getter path auto-invokes the closure with obj as its
 * argument and returns the resulting value, so `obj._native` is the raw
 * G* pointer (lightuserdatum). A metatable field would hand back the
 * closure itself, and lua_touserdata() on a Lua function yields NULL --
 * exactly the trap that black-screened the root wallpaper, because
 * lib/awful/_compat.lua:71 does `pcall(function() return pattern._native end)`
 * and then passes `pattern._native` straight to root._wallpaper()
 * (root.c:503 casts it via lua_touserdata).
 *
 * lgishim_pixbuf.c documents the same reasoning for GdkPixbuf. lgishim_gobject.c
 * carries the identical NATIVE_PG table with a matching comment (lines 619-632).
 * cairo / pango / glib / gio shims used to put _native on the metatable, which
 * broke every lib/ call site of the form `X._native` (wibox shape, snap,
 * client/shape, _compat root.wallpaper). This is the last of them to be fixed.
 *
 * Note: gio's clsinfo has no "g" field -- its property getters live in "p"
 * (auto-invoked with obj as self, see m_index), so NATIVE_PG goes there.
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

    /* The discriminator is metatable IDENTITY, deliberately not w->cls: the
     * other shims define the same wrap layout with their own cls enum, so an
     * index comparison would let a Pango.Layout pass as a Gio.File. Every
     * userdata carries exactly one registered metatable, so comparing the
     * tables is sound across module boundaries. */
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
    /* NOTE: `_native` used to be registered here on the metatable, which made
     * `X._native` return the closure instead of the raw pointer (see NATIVE_PG
     * above). It is now a property getter in the per-class "p" table below so
     * m_index auto-invokes it. */
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
    luaL_setfuncs(L, NATIVE_PG, 0);            /* always: _native (property getter) */
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

/* ------------------------------------------------------------- enum tables */

struct enumpair {
    const char *name;
    int value;
};

static void
install_enum(lua_State *L, const char *field,
             const struct enumpair *t, size_t n)
{
    size_t i;

    lua_newtable(L);
    for (i = 0; i < n; i++) {
        lua_pushinteger(L, (lua_Integer)t[i].value);
        lua_setfield(L, -2, t[i].name);
    }
    lua_setfield(L, -2, field);
}

static const struct enumpair BUS_TYPES[] = {
    { "STARTER", G_BUS_TYPE_STARTER },
    { "NONE",    G_BUS_TYPE_NONE },
    { "SYSTEM",  G_BUS_TYPE_SYSTEM },
    { "SESSION", G_BUS_TYPE_SESSION },
};

static const struct enumpair BUS_NAME_OWNER_FLAGS[] = {
    { "NONE",              G_BUS_NAME_OWNER_FLAGS_NONE },
    { "ALLOW_REPLACEMENT", G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT },
    { "REPLACE",           G_BUS_NAME_OWNER_FLAGS_REPLACE },
    { "DO_NOT_QUEUE",      G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE },
};

static const struct enumpair DBUS_PROXY_FLAGS[] = {
    { "NONE",                              G_DBUS_PROXY_FLAGS_NONE },
    { "DO_NOT_LOAD_PROPERTIES",            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES },
    { "DO_NOT_CONNECT_SIGNALS",            G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS },
    { "DO_NOT_AUTO_START",                 G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START },
    { "GET_INVALIDATED_PROPERTIES",        G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES },
    { "DO_NOT_AUTO_START_AT_CONSTRUCTION", G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START_AT_CONSTRUCTION },
};

static const struct enumpair DBUS_CALL_FLAGS[] = {
    { "NONE",  G_DBUS_CALL_FLAGS_NONE },
    { "NO_AUTO_START", G_DBUS_CALL_FLAGS_NO_AUTO_START },
    { "ALLOW_INTERACTIVE_AUTHORIZATION",
      G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION },
};

static const struct enumpair FILE_QUERY_INFO_FLAGS[] = {
    { "NONE",              G_FILE_QUERY_INFO_NONE },
    { "NOFOLLOW_SYMLINKS", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS },
};

static const struct enumpair FILE_CREATE_FLAGS[] = {
    { "NONE",                 G_FILE_CREATE_NONE },
    { "PRIVATE",              G_FILE_CREATE_PRIVATE },
    { "REPLACE_DESTINATION",  G_FILE_CREATE_REPLACE_DESTINATION },
};

/*
 * Flags come in two shapes: the integer this module installs, or the empty
 * table that gears/filesystem.lua:96 passes for FileQueryInfoFlags.NONE.
 */
static guint
flags_arg(lua_State *L, int idx, guint def)
{
    if (lua_isnoneornil(L, idx))
        return def;
    if (lua_isnumber(L, idx))
        return (guint)luaL_checkinteger(L, idx);
    if (lua_istable(L, idx) && lua_rawlen(L, idx) == 0)
        return 0;
    luaL_error(L, "lgishim_gio: bad flags value at arg %d", idx);
    return def;
}

static const char *
io_error_name(int code)
{
    static const struct enumpair names[] = {
        { "FAILED", G_IO_ERROR_FAILED },
        { "NOT_FOUND", G_IO_ERROR_NOT_FOUND },
        { "EXISTS", G_IO_ERROR_EXISTS },
        { "IS_DIRECTORY", G_IO_ERROR_IS_DIRECTORY },
        { "NOT_DIRECTORY", G_IO_ERROR_NOT_DIRECTORY },
        { "NOT_EMPTY", G_IO_ERROR_NOT_EMPTY },
        { "NOT_REGULAR_FILE", G_IO_ERROR_NOT_REGULAR_FILE },
        { "NOT_SYMBOLIC_LINK", G_IO_ERROR_NOT_SYMBOLIC_LINK },
        { "NOT_MOUNTABLE_FILE", G_IO_ERROR_NOT_MOUNTABLE_FILE },
        { "FILENAME_TOO_LONG", G_IO_ERROR_FILENAME_TOO_LONG },
        { "INVALID_FILENAME", G_IO_ERROR_INVALID_FILENAME },
        { "TOO_MANY_LINKS", G_IO_ERROR_TOO_MANY_LINKS },
        { "NO_SPACE", G_IO_ERROR_NO_SPACE },
        { "INVALID_ARGUMENT", G_IO_ERROR_INVALID_ARGUMENT },
        { "PERMISSION_DENIED", G_IO_ERROR_PERMISSION_DENIED },
        { "NOT_SUPPORTED", G_IO_ERROR_NOT_SUPPORTED },
        { "NOT_MOUNTED", G_IO_ERROR_NOT_MOUNTED },
        { "ALREADY_MOUNTED", G_IO_ERROR_ALREADY_MOUNTED },
        { "CLOSED", G_IO_ERROR_CLOSED },
        { "CANCELLED", G_IO_ERROR_CANCELLED },
        { "PENDING", G_IO_ERROR_PENDING },
        { "READ_ONLY", G_IO_ERROR_READ_ONLY },
        { "CANT_CREATE_BACKUP", G_IO_ERROR_CANT_CREATE_BACKUP },
        { "WRONG_ETAG", G_IO_ERROR_WRONG_ETAG },
        { "TIMED_OUT", G_IO_ERROR_TIMED_OUT },
        { "WOULD_RECURSE", G_IO_ERROR_WOULD_RECURSE },
        { "BUSY", G_IO_ERROR_BUSY },
        { "WOULD_BLOCK", G_IO_ERROR_WOULD_BLOCK },
        { "HOST_NOT_FOUND", G_IO_ERROR_HOST_NOT_FOUND },
        { "WOULD_MERGE", G_IO_ERROR_WOULD_MERGE },
        { "FAILED_HANDLED", G_IO_ERROR_FAILED_HANDLED },
        { "TOO_MANY_OPEN_FILES", G_IO_ERROR_TOO_MANY_OPEN_FILES },
        { "NOT_INITIALIZED", G_IO_ERROR_NOT_INITIALIZED },
        { "ADDRESS_IN_USE", G_IO_ERROR_ADDRESS_IN_USE },
        { "PARTIAL_INPUT", G_IO_ERROR_PARTIAL_INPUT },
        { "INVALID_DATA", G_IO_ERROR_INVALID_DATA },
        { "DBUS_ERROR", G_IO_ERROR_DBUS_ERROR },
        { "HOST_UNREACHABLE", G_IO_ERROR_HOST_UNREACHABLE },
        { "NETWORK_UNREACHABLE", G_IO_ERROR_NETWORK_UNREACHABLE },
        { "CONNECTION_REFUSED", G_IO_ERROR_CONNECTION_REFUSED },
        { "PROXY_FAILED", G_IO_ERROR_PROXY_FAILED },
        { "PROXY_AUTH_FAILED", G_IO_ERROR_PROXY_AUTH_FAILED },
        { "PROXY_NEED_AUTH", G_IO_ERROR_PROXY_NEED_AUTH },
        { "PROXY_NOT_ALLOWED", G_IO_ERROR_PROXY_NOT_ALLOWED },
        { "BROKEN_PIPE", G_IO_ERROR_BROKEN_PIPE },
        { "CONNECTION_CLOSED", G_IO_ERROR_CONNECTION_CLOSED },
        { "NOT_CONNECTED", G_IO_ERROR_NOT_CONNECTED },
        { "MESSAGE_TOO_LARGE", G_IO_ERROR_MESSAGE_TOO_LARGE },
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].value == code)
            return names[i].name;
    return "UNKNOWN";
}

static const char *
file_type_name(GFileType t)
{
    switch (t) {
    case G_FILE_TYPE_UNKNOWN:       return "UNKNOWN";
    case G_FILE_TYPE_REGULAR:       return "REGULAR";
    case G_FILE_TYPE_DIRECTORY:     return "DIRECTORY";
    case G_FILE_TYPE_SYMBOLIC_LINK: return "SYMBOLIC_LINK";
    case G_FILE_TYPE_SPECIAL:       return "SPECIAL";
    case G_FILE_TYPE_SHORTCUT:      return "SHORTCUT";
    case G_FILE_TYPE_MOUNTABLE:     return "MOUNTABLE";
    default:                        return "UNKNOWN";
    }
}

/* --------------------------------------------------------- value marshalling */

/*
 * gears/filesystem.lua:22 matches `err.domain == Gio.IOErrorEnum` and
 * `err.code == "EXISTS"`, so domain is the GType name string and code is the
 * enum name string, exactly as LGI's GError table looks. g_gio_error_quark()
 * does not exist in this build, so use the public g_io_error_quark().
 */
static void
push_error(lua_State *L, GError *err)
{
    if (!err) {
        lua_pushnil(L);
        return;
    }
    lua_newtable(L);
    if (err->domain == g_io_error_quark()) {
        lua_pushstring(L, "Gio.IOErrorEnum");
        lua_setfield(L, -2, "domain");
        lua_pushstring(L, io_error_name(err->code));
        lua_setfield(L, -2, "code");
    } else {
        const char *dom = g_quark_to_string(err->domain);
        lua_pushstring(L, dom ? dom : "");
        lua_setfield(L, -2, "domain");
        lua_pushinteger(L, (lua_Integer)err->code);
        lua_setfield(L, -2, "code");
    }
    lua_pushstring(L, err->message ? err->message : "");
    lua_setfield(L, -2, "message");
}

/*
 * Push `debug.traceback` onto the stack as a pcall message handler and return
 * its stack index. A raised callback error must be captured while the callback
 * is still on the stack: once pcall returns, it is already unwound, so a bare
 * "attempt to call a string value" would never say which lib/ frame raised.
 * Returns 0 (plain message, no handler) when debug.traceback is absent.
 */
static int
push_traceback_h(lua_State *L)
{
    int idx;

    lua_getglobal(L, "debug");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    lua_getfield(L, -1, "traceback");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return 0;
    }
    idx = lua_gettop(L) - 1;
    lua_remove(L, idx);                   /* drop the debug table */
    return idx;
}

/*
 * Call a Lua function that GLib dispatched, catching raises. The tail has to
 * be left empty because the callback shares the lua_State with the event
 * loop (see lgishim_gobject.c's closure_marshal()).
 */
static void
pcall_dispatch(lua_State *L, int nargs)
{
    int errf = push_traceback_h(L);
    if (errf != 0) {
        int fn = lua_gettop(L) - nargs - 1;
        lua_insert(L, fn);
        errf = fn;
    }
    if (lua_pcall(L, nargs, 1, errf) != 0) {
        g_printerr("lgishim_gio callback error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, 0);
}

/*
 * `Gio.DBusVariant` in lgishim_glib.c already exposes a Variant userdata with
 * metatable "lgishim.GLib.Variant"; both are accepted here and the raw
 * GVariant* is lifted out of the wrapper's first field.
 */
static int
is_variant_userdata(lua_State *L, int idx)
{
    const char *names[] = { "lgishim.GLib.Variant", CLS_NAMES[C_DBUSVAR] };
    int i, ok = 0;

    if (!lua_isuserdata(L, idx))
        return 0;
    for (i = 0; i < 2; i++) {
        luaL_getmetatable(L, names[i]);        /* [.., metatable] */
        if (lua_getmetatable(L, idx)) {        /* [.., metatable, obj_mt] */
            ok = lua_rawequal(L, -1, -2);
            lua_pop(L, 1);
        } else {
            lua_pop(L, 1);                     /* getmetatable popped the obj */
        }
        lua_pop(L, 1);                         /* the metatable */
    }
    return ok;
}

/* Returns a NEW reference, or NULL for a nil argument. */
static GVariant *
variant_from_lua(lua_State *L, int idx)
{
    foreign_wrap *w;

    if (lua_isnoneornil(L, idx))
        return NULL;
    if (!is_variant_userdata(L, idx))
        luaL_error(L, "lgishim_gio: expected a Variant at arg %d, got %s",
                   idx, luaL_typename(L, idx));
    w = (foreign_wrap *)lua_touserdata(L, idx);
    if (!w || !w->p)
        return NULL;
    return g_variant_ref((GVariant *)w->p);
}

/*
 * g_variant_to_lua(): a variant -> a Lua value. Tuples and non-dict arrays
 * become 1-based tables (that is what DBusVariant.value is read as), dict
 * entries become string-keyed tables (Notify's a{sv} hints), scalars become
 * the matching Lua type. `v` inside a variant is unwrapped one level, which
 * is the a{sv} case.
 *
 * `g_variant_type_element()` is called ONLY inside the ARRAY branch: it
 * asserts `type_string[0] == 'a' || type_string[0] == 'm'`, and every
 * non-array variant reaches here (Notify's `(susssasa{sv}i)` tuple, a `(u)`
 * pid reply, any `s`/`u`/`i`/`as`/`a{sv}` child).  Fetching the element type
 * unconditionally made the very first `.value` read abort the whole process
 * with `GLib:ERROR:gvarianttype.c:g_variant_type_element`.
 */
static void
variant_to_lua(lua_State *L, GVariant *v)
{
    if (!v) {
        lua_pushnil(L);
        return;
    }
    switch (g_variant_classify(v)) {
    case G_VARIANT_CLASS_BOOLEAN:
        lua_pushboolean(L, g_variant_get_boolean(v) ? TRUE : FALSE);
        break;
    case G_VARIANT_CLASS_BYTE:
        lua_pushinteger(L, (lua_Integer)g_variant_get_byte(v));
        break;
    case G_VARIANT_CLASS_INT16:
        lua_pushinteger(L, (lua_Integer)g_variant_get_int16(v));
        break;
    case G_VARIANT_CLASS_UINT16:
        lua_pushinteger(L, (lua_Integer)g_variant_get_uint16(v));
        break;
    case G_VARIANT_CLASS_INT32:
        lua_pushinteger(L, (lua_Integer)g_variant_get_int32(v));
        break;
    case G_VARIANT_CLASS_UINT32:
        lua_pushinteger(L, (lua_Integer)g_variant_get_uint32(v));
        break;
    case G_VARIANT_CLASS_INT64:
        lua_pushinteger(L, (lua_Integer)g_variant_get_int64(v));
        break;
    case G_VARIANT_CLASS_UINT64:
        lua_pushinteger(L, (lua_Integer)g_variant_get_uint64(v));
        break;
    case G_VARIANT_CLASS_DOUBLE:
        lua_pushnumber(L, (lua_Number)g_variant_get_double(v));
        break;
    case G_VARIANT_CLASS_STRING: {
        const char *s = g_variant_get_string(v, NULL);
        lua_pushstring(L, s ? s : "");
        break;
    }
    case G_VARIANT_CLASS_VARIANT: {
        GVariant *inner = g_variant_ref(g_variant_get_child_value(v, 0));
        variant_to_lua(L, inner);
        g_variant_unref(inner);
        break;
    }
    case G_VARIANT_CLASS_ARRAY: {
        /* Here the type is guaranteed to start with 'a', so the assert
         * inside g_variant_type_element() can not fire. */
        const GVariantType *el = g_variant_type_element(g_variant_get_type(v));
        guint i, n = g_variant_n_children(v);
        lua_newtable(L);
        for (i = 0; i < n; i++) {
            GVariant *child = g_variant_ref(g_variant_get_child_value(v, i));
            if (g_variant_type_is_dict_entry(el)) {
                GVariant *key = g_variant_ref(g_variant_get_child_value(child, 0));
                GVariant *val = g_variant_ref(g_variant_get_child_value(child, 1));
                const char *ks = g_variant_get_string(key, NULL);
                variant_to_lua(L, val);
                lua_pushstring(L, ks ? ks : "");
                lua_insert(L, -2);
                lua_rawset(L, -3);
                g_variant_unref(val);
                g_variant_unref(key);
            } else {
                variant_to_lua(L, child);
                lua_rawseti(L, -2, (lua_Integer)(i + 1));
            }
            g_variant_unref(child);
        }
        break;
    }
    case G_VARIANT_CLASS_TUPLE: {
        guint i, n = g_variant_n_children(v);
        lua_newtable(L);
        for (i = 0; i < n; i++) {
            GVariant *child = g_variant_ref(g_variant_get_child_value(v, i));
            variant_to_lua(L, child);
            lua_rawseti(L, -2, (lua_Integer)(i + 1));
            g_variant_unref(child);
        }
        break;
    }
    default: {
        gchar *s = g_variant_print(v, TRUE);
        lua_pushstring(L, s);
        g_free(s);
        break;
    }
    }
}

static void
xml_esc(GString *out, const char *s)
{
    const char *p;

    if (!s)
        return;
    for (p = s; *p; p++) {
        switch (*p) {
        case '&': g_string_append(out, "&amp;"); break;
        case '<': g_string_append(out, "&lt;"); break;
        case '>': g_string_append(out, "&gt;"); break;
        case '\'': g_string_append(out, "&apos;"); break;
        case '"': g_string_append(out, "&quot;"); break;
        default: g_string_append_c(out, *p); break;
        }
    }
}

/* ============================================================ GFile family */

static int
file_new_for_path(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    return push_obj(L, g_file_new_for_path(path), d_object, C_FILE);
}

static int
file_new_for_commandline_arg(lua_State *L)
{
    const char *arg = luaL_checkstring(L, 1);
    return push_obj(L, g_file_new_for_commandline_arg(arg), d_object, C_FILE);
}

static int
file_new_for_uri(lua_State *L)
{
    const char *uri = luaL_checkstring(L, 1);
    return push_obj(L, g_file_new_for_uri(uri), d_object, C_FILE);
}

static int
file_new_tmp(lua_State *L)
{
    const char *tmpl = NULL;
    GFileIOStream *iostream = NULL;
    GError *err = NULL;
    GFile *f;

    if (!lua_isnoneornil(L, 1))
        tmpl = luaL_checkstring(L, 1);

    f = g_file_new_tmp(tmpl, &iostream, &err);

    if (err) {
        if (f)
            g_object_unref(f);
        if (iostream)
            g_object_unref(iostream);
        lua_pushnil(L);
        push_error(L, err);
        g_clear_error(&err);
        return 2;
    }

    /* iostream is rarely needed from Lua; drop it. */
    if (iostream)
        g_object_unref(iostream);
    return push_obj(L, f, d_object, C_FILE);
}

static int
file_get_path(lua_State *L)
{
    const char *p = g_file_get_path(chk_cls(L, 1, C_FILE));
    if (p)
        lua_pushstring(L, p);
    else
        lua_pushnil(L);                       /* a URI-only GFile */
    return 1;
}

static int
file_get_uri(lua_State *L)
{
    lua_pushstring(L, g_file_get_uri(chk_cls(L, 1, C_FILE)));
    return 1;
}

static int
file_get_basename(lua_State *L)
{
    lua_pushstring(L, g_file_get_basename(chk_cls(L, 1, C_FILE)));
    return 1;
}

/*
 * g_file_get_display_name() does not exist in this glib build. GFile has no
 * display name of its own (that is a GFileInfo attribute), and lib/ only
 * reads it off files enumerated from the icon theme, where basename() is the
 * documented fallback, so hand it back.
 */
static int
file_get_display_name(lua_State *L)
{
    lua_pushstring(L, g_file_get_basename(chk_cls(L, 1, C_FILE)));
    return 1;
}

static int
file_get_parent(lua_State *L)
{
    GFile *f = g_file_get_parent(chk_cls(L, 1, C_FILE));
    if (!f) {
        lua_pushnil(L);
        return 1;
    }
    return push_obj(L, f, d_object, C_FILE);
}

static int
file_get_child(lua_State *L)
{
    GFile *f = g_file_get_child(chk_cls(L, 1, C_FILE), luaL_checkstring(L, 2));
    if (!f) {
        lua_pushnil(L);
        return 1;
    }
    return push_obj(L, f, d_object, C_FILE);
}

static int
file_query_exists(lua_State *L)
{
    lua_pushboolean(L, g_file_query_exists(chk_cls(L, 1, C_FILE), NULL) ? TRUE : FALSE);
    return 1;
}

/* gears/filesystem.lua compares the result with "DIRECTORY" / "REGULAR". */
static int
file_query_file_type(lua_State *L)
{
    lua_pushstring(L, file_type_name(
        g_file_query_file_type(chk_cls(L, 1, C_FILE), flags_arg(L, 2, 0), NULL)));
    return 1;
}

static int
file_query_info(lua_State *L)
{
    GFile *f = chk_cls(L, 1, C_FILE);
    GError *err = NULL;
    GFileInfo *info;

    info = g_file_query_info(f, luaL_checkstring(L, 2),
                             flags_arg(L, 3, 0), NULL, &err);
    if (!info) {
        lua_pushnil(L);
        push_error(L, err);
        g_clear_error(&err);
        return 2;
    }
    return push_obj(L, info, d_object, C_FILEINFO);
}

/*
 * This glib build has no g_file_info_free()/g_file_info_unref(); GFileInfo is
 * a plain GObject here, so g_object_unref() (d_object) releases it.
 */
static int
fileinfo_get_name(lua_State *L)
{
    const char *s = g_file_info_get_name(chk_cls(L, 1, C_FILEINFO));
    /* NULL when "standard::name" was not part of the query -- pushstring()
     * does not accept a NULL (Lua 5.3 asserts). */
    if (s)
        lua_pushstring(L, s);
    else
        lua_pushnil(L);
    return 1;
}

static int
fileinfo_get_display_name(lua_State *L)
{
    const char *s = g_file_info_get_display_name(chk_cls(L, 1, C_FILEINFO));
    if (s)
        lua_pushstring(L, s);
    else
        lua_pushnil(L);
    return 1;
}

static int
fileinfo_get_file_type(lua_State *L)
{
    lua_pushstring(L, file_type_name(
        g_file_info_get_file_type(chk_cls(L, 1, C_FILEINFO))));
    return 1;
}

static int
fileinfo_get_attribute_boolean(lua_State *L)
{
    lua_pushboolean(L, g_file_info_get_attribute_boolean(
        chk_cls(L, 1, C_FILEINFO), luaL_checkstring(L, 2)) ? TRUE : FALSE);
    return 1;
}

static int
fileinfo_get_attribute_string(lua_State *L)
{
    const char *s = g_file_info_get_attribute_string(
        chk_cls(L, 1, C_FILEINFO), luaL_checkstring(L, 2));
    if (s)
        lua_pushstring(L, s);
    else
        lua_pushnil(L);
    return 1;
}

static int
file_make_directory(lua_State *L)
{
    GFile *f = chk_cls(L, 1, C_FILE);
    GError *err = NULL;
    gboolean ok = g_file_make_directory(f, NULL, &err);

    lua_pushboolean(L, ok ? TRUE : FALSE);
    push_error(L, err);
    g_clear_error(&err);
    return 2;
}

static int
file_make_directory_with_parents(lua_State *L)
{
    GFile *f = chk_cls(L, 1, C_FILE);
    GError *err = NULL;
    gboolean ok = g_file_make_directory_with_parents(f, NULL, &err);

    lua_pushboolean(L, ok ? TRUE : FALSE);
    push_error(L, err);
    g_clear_error(&err);
    return 2;
}

static int
file_delete(lua_State *L)
{
    GFile *f = chk_cls(L, 1, C_FILE);
    GError *err = NULL;
    gboolean ok = g_file_delete(f, NULL, &err);

    lua_pushboolean(L, ok ? TRUE : FALSE);
    push_error(L, err);
    g_clear_error(&err);
    return 2;
}

static int
file_make_symbolic_link(lua_State *L)
{
    GFile *f = chk_cls(L, 1, C_FILE);
    size_t len;
    const char *target = luaL_checklstring(L, 2, &len);
    GError *err = NULL;
    gboolean ok;

    if (!f || !target) {
        lua_pushboolean(L, FALSE);
        lua_pushnil(L);
        return 2;
    }

    ok = g_file_make_symbolic_link(f, target, NULL, &err);

    lua_pushboolean(L, ok);
    push_error(L, err);
    g_clear_error(&err);
    return 2;
}

static int
file_create(lua_State *L)
{
    GFile *f = chk_cls(L, 1, C_FILE);
    gint32 flags = (gint32)luaL_checkinteger(L, 2);
    GError *err = NULL;
    GFileIOStream *stream;

    if (!f) {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }

    stream = g_file_create(f, (GFileCreateFlags)flags, NULL, &err);

    if (stream)
        g_object_unref(stream);

    lua_pushboolean(L, stream != NULL);
    push_error(L, err);
    g_clear_error(&err);
    return 2;
}

static int
file_enumerate_children(lua_State *L)
{
    GFile *f = chk_cls(L, 1, C_FILE);
    GError *err = NULL;
    GFileEnumerator *e;

    e = g_file_enumerate_children(f, luaL_checkstring(L, 2),
                                  flags_arg(L, 3, 0), NULL, &err);
    if (!e) {
        lua_pushnil(L);
        push_error(L, err);
        g_clear_error(&err);
        return 2;
    }
    return push_obj(L, e, d_object, C_FILEENUM);
}

static int
fileinfo_new(lua_State *L)
{
    return push_obj(L, g_file_info_new(), d_object, C_FILEINFO);
}

static int
fileenum_next_file(lua_State *L)
{
    GFileEnumerator *e = chk_cls(L, 1, C_FILEENUM);
    GError *err = NULL;
    GFileInfo *info = g_file_enumerator_next_file(e, NULL, &err);

    if (!info) {
        lua_pushnil(L);
        push_error(L, err);
        g_clear_error(&err);
        return 2;
    }
    return push_obj(L, info, d_object, C_FILEINFO);
}

/*
 * Both next_files() and async_next_files(): menubar/utils.lua:378 reads
 * `list, err = enum:async_next_files(n)` in a loop and stops on #list == 0,
 * so the synchronous form is what the loop needs. Returns a table of
 * GFileInfo plus an error table (nil when none).
 */
static int
fileenum_next_files(lua_State *L)
{
    GFileEnumerator *e = chk_cls(L, 1, C_FILEENUM);
    guint n = (guint)luaL_optinteger(L, 2, 1);
    GError *err = NULL;
    GPtrArray *arr = g_ptr_array_new();
    guint i;

    if (n == 0)
        n = 1;
    for (i = 0; i < n; i++) {
        GFileInfo *info = g_file_enumerator_next_file(e, NULL, &err);
        if (!info)
            break;
        g_ptr_array_add(arr, info);
    }
    lua_newtable(L);
    for (i = 0; i < arr->len; i++) {
        push_obj(L, arr->pdata[i], d_object, C_FILEINFO);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    g_ptr_array_free(arr, TRUE);
    push_error(L, err);
    g_clear_error(&err);
    return 2;
}

static int
fileenum_get_child(lua_State *L)
{
    GFile *c = g_file_enumerator_get_child(chk_cls(L, 1, C_FILEENUM),
                                           chk_cls(L, 2, C_FILEINFO));
    if (!c) {
        lua_pushnil(L);
        return 1;
    }
    return push_obj(L, c, d_object, C_FILE);
}

static int
fileenum_get_container(lua_State *L)
{
    lua_pushlightuserdata(L, (void *)g_file_enumerator_get_container(
        chk_cls(L, 1, C_FILEENUM)));
    return 1;
}

static int
fileenum_has_pending(lua_State *L)
{
    lua_pushboolean(L, g_file_enumerator_has_pending(
        chk_cls(L, 1, C_FILEENUM)) ? TRUE : FALSE);
    return 1;
}

static int
fileenum_is_closed(lua_State *L)
{
    lua_pushboolean(L, g_file_enumerator_is_closed(
        chk_cls(L, 1, C_FILEENUM)) ? TRUE : FALSE);
    return 1;
}

static int
fileenum_close(lua_State *L)
{
    g_file_enumerator_close(chk_cls(L, 1, C_FILEENUM), NULL, NULL);
    return 0;
}

static const luaL_Reg FILE_M[] = {
    { "new_for_path",                file_new_for_path },
    { "new_for_commandline_arg",      file_new_for_commandline_arg },
    { "new_for_uri",                 file_new_for_uri },
    { "new_tmp",                     file_new_tmp },
    { "get_parent",                  file_get_parent },
    { "get_child",                   file_get_child },
    { "get_path",                    file_get_path },
    { "get_uri",                     file_get_uri },
    { "get_basename",                file_get_basename },
    { "get_display_name",            file_get_display_name },
    { "query_exists",                file_query_exists },
    { "query_file_type",             file_query_file_type },
    { "query_info",                  file_query_info },
    { "make_directory",              file_make_directory },
    { "make_directory_with_parents", file_make_directory_with_parents },
    { "delete",                      file_delete },
    { "make_symbolic_link",          file_make_symbolic_link },
    { "create",                      file_create },
    { "enumerate_children",          file_enumerate_children },
    { "async_enumerate_children",    file_enumerate_children },
    { NULL, NULL },
};

static const luaL_Reg FILEINFO_M[] = {
    { "new",                   fileinfo_new },
    { "get_name",              fileinfo_get_name },
    { "get_display_name",      fileinfo_get_display_name },
    { "get_file_type",         fileinfo_get_file_type },
    { "get_attribute_boolean", fileinfo_get_attribute_boolean },
    { "get_attribute_string",  fileinfo_get_attribute_string },
    { NULL, NULL },
};

static const luaL_Reg FILEENUM_M[] = {
    { "next_file",          fileenum_next_file },
    { "next_files",         fileenum_next_files },
    { "async_next_files",   fileenum_next_files },
    { "get_child",          fileenum_get_child },
    { "get_container",      fileenum_get_container },
    { "has_pending",        fileenum_has_pending },
    { "is_closed",          fileenum_is_closed },
    { "close",              fileenum_close },
    { "async_close",        fileenum_close },
    { NULL, NULL },
};

/* ============================================================== GIOStreams */

/*
 * The shim has no glib dispatch of its own, so a DataInputStream must carry
 * the pending read_line_async() result itself; read_line_finish() just hands
 * it back. This mirrors the shape lib/awful/spawn.lua:236-262 and :505-545
 * are written against: async read, then finish, no GAsyncReadyInterface.
 */
typedef struct {
    GDataInputStream *stream;
    gchar *pending;
    gsize pending_len;
    gboolean pending_is_error;
} datain_ctx;

static void
d_datain(void *p)
{
    datain_ctx *c = (datain_ctx *)p;
    if (!c)
        return;
    if (c->stream) {
        g_input_stream_close((GInputStream *)c->stream, NULL, NULL);  /* idempotent */
        g_object_unref(c->stream);
    }
    g_free(c->pending);
    g_free(c);
}

static int
datain_new(lua_State *L)
{
    GIOStream *inner = chk(L, 1);
    datain_ctx *c;

    if (g_object_is_floating(inner))
        g_object_ref_sink(inner);
    else
        g_object_ref(inner);

    c = (datain_ctx *)g_malloc0(sizeof(*c));
    c->stream = g_data_input_stream_new((GInputStream *)inner);
    return push_obj(L, c, d_datain, C_DATAIN);
}

static int
datain_read_line(lua_State *L)
{
    datain_ctx *c = chk_cls(L, 1, C_DATAIN);
    gchar *line;
    gsize len = 0;
    GError *err = NULL;

    /* Returns char* (NULL at end of file); there is no success boolean. */
    line = g_data_input_stream_read_line(c->stream, &len, NULL, &err);

    if (err) {
        lua_pushnil(L);
        lua_pushstring(L, err->message);
        g_error_free(err);
        return 2;
    }
    if (line) {
        lua_pushstring(L, line);
        g_free(line);
    } else {
        lua_pushnil(L);                       /* end of file */
    }
    lua_pushinteger(L, (lua_Integer)len);
    return 2;
}

/*
 * read_line_async(priority, cancel, callback) -> reads one line now, stashes
 * it, and dispatches callback(obj, nil) on the next main-loop iteration.
 * lib/awful/spawn.lua:522-545 then calls obj:read_line_finish(res); the nil
 * `res` is ignored, the stashed result is what gets returned.
 *
 * The dispatch MUST be deferred (g_idle_add), not inline: spawn.lua chains
 * start_read -> read_line_async -> callback -> start_read once per line, and
 * an inline call makes that recurse on the C stack for the whole stream --
 * any multi-line output blows the stack ("C stack overflow") and the
 * easy_async user callback never fires.
 */
typedef struct {
    lua_State *L;
    int stream_ref;   /* the DataInputStream userdata (keeps datain_ctx alive) */
    int cb_ref;
} datain_async_cbdata;

static gboolean
datain_async_dispatch(gpointer user_data)
{
    datain_async_cbdata *d = user_data;
    lua_State *L = d->L;
    int errf;

    /* [tb][cb][obj][nil] so lua_pcall's e(top-nargs) lands on the cb */
    errf = push_traceback_h(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, d->cb_ref);      /* cb */
    lua_rawgeti(L, LUA_REGISTRYINDEX, d->stream_ref);  /* obj */
    lua_pushnil(L);                                    /* async result handle */
    if (lua_pcall(L, 2, 0, errf) != 0) {
        g_printerr("lgishim_gio read_line_async error: %s\n",
                   lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, d->cb_ref);
    luaL_unref(L, LUA_REGISTRYINDEX, d->stream_ref);
    lua_settop(L, 0);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static int
datain_read_line_async(lua_State *L)
{
    datain_ctx *c = chk_cls(L, 1, C_DATAIN);
    gchar *line = NULL;
    gsize len = 0;
    GError *err = NULL;
    datain_async_cbdata *d;

    luaL_checktype(L, 4, LUA_TFUNCTION);

    line = g_data_input_stream_read_line(c->stream, &len, NULL, &err);
    g_free(c->pending);
    c->pending = NULL;
    c->pending_len = 0;
    c->pending_is_error = FALSE;
    if (line) {
        c->pending = line;
        c->pending_len = len;
    } else if (err) {
        c->pending = g_strdup(err->message);
        c->pending_is_error = TRUE;
        g_error_free(err);
    }
    g_clear_error(&err);

    /* Defer the callback to the next main-loop iteration (see the comment
     * above datain_async_dispatch). */
    d = g_new0(datain_async_cbdata, 1);
    d->L = L;
    lua_pushvalue(L, 1);
    d->stream_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, 4);
    d->cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_idle_add(datain_async_dispatch, d);
    return 0;
}

static int
datain_read_line_finish(lua_State *L)
{
    datain_ctx *c = chk_cls(L, 1, C_DATAIN);
    gchar *pending = c->pending;

    c->pending = NULL;
    if (!pending) {
        c->pending_len = 0;
        lua_pushnil(L);
        lua_pushinteger(L, 0);
        return 2;
    }
    if (c->pending_is_error) {
        c->pending_len = 0;
        lua_pushnil(L);
        lua_pushstring(L, pending);
        g_free(pending);
        return 2;
    }
    lua_pushstring(L, pending);
    lua_pushinteger(L, (lua_Integer)c->pending_len);
    c->pending_len = 0;              /* reset after the length is pushed out */
    g_free(pending);
    return 2;
}

/* g_data_input_stream_set_buffer_size() is absent from this build. */
static int
datain_set_buffer_size(lua_State *L)
{
    (void)L;
    return 0;
}

static int
datain_close(lua_State *L)
{
    datain_ctx *c = chk_cls(L, 1, C_DATAIN);
    /* c->stream is a GDataInputStream (a plain GInputStream), NOT a
     * GIOStream -- g_io_stream_close() here is an assertion failure. */
    if (c->stream)
        g_input_stream_close((GInputStream *)c->stream, NULL, NULL);
    return 0;
}

static int
memin_new_from_data(lua_State *L)
{
    GInputStream *s;

    if (lua_isstring(L, 1)) {
        size_t len;
        const char *data = luaL_checklstring(L, 1, &len);
        s = g_memory_input_stream_new_from_data(data, (gssize)len, NULL);
    } else {
        /* The pre-2016 lgi form takes a table of characters; concatenate so
         * the pcall fallback at lib/awful/spawn.lua:240-253 also works.
         * Elements may be single-character strings or byte numbers. */
        GString *acc = g_string_new(NULL);
        lua_Integer n = luaL_len(L, 1);
        lua_Integer i;
        for (i = 1; i <= n; i++) {
            lua_rawgeti(L, 1, i);
            if (lua_isnumber(L, -1)) {
                guint b = (guint)luaL_checknumber(L, -1);
                g_string_append_c(acc, (char)(b & 0xffu));
            } else {
                size_t elen = 0;
                const char *e = luaL_checklstring(L, -1, &elen);
                g_string_append_len(acc, e, (gssize)elen);
            }
            lua_pop(L, 1);
        }
        s = g_memory_input_stream_new_from_data(acc->str, (gssize)acc->len, NULL);
        g_string_free(acc, TRUE);
    }
    return push_obj(L, s, d_object, C_MEMIN);
}

static int
memin_new(lua_State *L)
{
    lua_insert(L, 2);
    return memin_new_from_data(L);
}

static int
unixin_new(lua_State *L)
{
    gint fd = (gint)luaL_checkinteger(L, 1);
    gboolean close_fd = lua_isnoneornil(L, 2) ? TRUE : lua_toboolean(L, 2);
    GInputStream *s = g_unix_input_stream_new(fd, close_fd);

    if (!s)
        luaL_error(L, "lgishim_gio: could not open fd %d", fd);
    if (g_object_is_floating(s))
        g_object_ref_sink(s);
    return push_obj(L, s, d_object, C_UNIXIN);
}

static int
unixin_get_fd(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)g_unix_input_stream_get_fd(
        (GUnixInputStream *)chk_cls(L, 1, C_UNIXIN)));
    return 1;
}

/* ====================================================== InputStream (base) */

/*
 * InputStream is the base of DataInputStream / MemoryInputStream /
 * UnixInputStream, but the shim never wraps a bare GInputStream: datain_new(),
 * memin_new_from_data() and unixin_new() always produce one of the three. So
 * the InputStream class table has no metatable and no `new` -- just the methods
 * that apply to any GInputStream plus is_type_of(). luaopen_lgishim_gio()
 * installs those methods on the three concrete metatables as well, because
 * lib/ calls them on instances (`stream:read(n)`), not on the class.
 */

/*
 * chk_cls() accepts a single class, but the InputStream methods have to accept
 * all three concrete stream classes. A DataInputStream wraps a datain_ctx, so
 * its GInputStream sits one level down in ->stream; the other two hold the
 * GInputStream directly.
 */
static GInputStream *
chk_instream(lua_State *L, int idx)
{
    wrap *w = getud(L, idx);
    const char *got = luaL_typename(L, idx);

    if (!w || !w->p)
        luaL_error(L, "lgishim_gio: expected a Gio.InputStream at arg %d, "
                   "got %s", idx, got);
    if (w->cls != C_DATAIN && w->cls != C_MEMIN && w->cls != C_UNIXIN) {
        got = (w->cls >= 1 && w->cls < N_GIO_CLASSES) ? CLS_NAMES[w->cls] : got;
        luaL_error(L, "lgishim_gio: expected a Gio.InputStream at arg %d, "
                   "got %s", idx, got);
    }
    return (GInputStream *)((w->cls == C_DATAIN) ? (((datain_ctx *)w->p)->stream)
                                                 : w->p);
}

static int
instream_read(lua_State *L)
{
    GInputStream *s = chk_instream(L, 1);
    lua_Integer count = luaL_checkinteger(L, 2);
    char *buf;
    gssize n;
    GError *err = NULL;

    luaL_argcheck(L, count > 0, 2, "count must be > 0");

    buf = g_try_malloc((gsize)count);
    if (!buf)
        luaL_error(L, "lgishim_gio: out of memory");

    n = g_input_stream_read(s, buf, (gsize)count, NULL, &err);
    if (n < 0) {
        /* Same shape as datain_read_line(): nil + the message, which is what
         * spawn.lua's `type(length) ~= "number"` test distinguishes. */
        lua_pushnil(L);
        lua_pushstring(L, err ? err->message : "read error");
        g_clear_error(&err);
        g_free(buf);
        return 2;
    }
    lua_pushlstring(L, buf, (size_t)n);
    lua_pushinteger(L, (lua_Integer)n);
    g_free(buf);
    return 2;
}

static int
instream_read_all(lua_State *L)
{
    GInputStream *s = chk_instream(L, 1);
    GString *acc = g_string_new(NULL);
    char buf[4096];
    gssize n;
    GError *err = NULL;

    /* g_input_stream_read() may return short reads, so loop until EOF (0);
     * only n < 0 is an error. Iterative on purpose -- a recursive version
     * would blow the C stack on a large stream. */
    while ((n = g_input_stream_read(s, buf, sizeof(buf), NULL, &err)) > 0)
        g_string_append_len(acc, buf, (gsize)n);

    if (n < 0) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err->message : "read error");
        g_clear_error(&err);
        g_string_free(acc, TRUE);
        return 2;
    }
    lua_pushlstring(L, acc->str, acc->len);
    lua_pushinteger(L, (lua_Integer)acc->len);
    g_string_free(acc, TRUE);
    return 2;
}

/*
 * read_async(nbytes, priority, cancellable, callback, user_data)
 *
 * The synchronous read happens in the idle source, exactly like
 * datain_async_dispatch(): calling the callback from read_async() would make
 * spawn.lua's start_read -> read_line_async -> callback -> start_read chain
 * recurse on the C stack for the whole stream (the C-stack-overflow that
 * 758b5aaa fixed for read_line_async). `priority` is the idle source priority;
 * `cancellable` is accepted but not honoured -- a synchronous read inside an
 * idle source cannot be aborted mid-read.
 */
typedef struct {
    lua_State *L;
    int stream_ref;   /* pins the wrapper (and a DataInputStream's datain_ctx) */
    int cb_ref;
    GInputStream *stream;  /* g_object_ref'ed: the idle source must not dangle */
    char *buffer;
    gsize nbytes;
} instream_async_cbdata;

static gboolean
instream_async_dispatch(gpointer user_data)
{
    instream_async_cbdata *d = (instream_async_cbdata *)user_data;
    lua_State *L = d->L;
    int errf;
    gssize n;
    GError *err = NULL;

    n = g_input_stream_read(d->stream, d->buffer, d->nbytes, NULL, &err);

    /* [tb][cb][obj][result] so lua_pcall's fn = top-nargs lands on the cb, the
     * same layout datain_async_dispatch() uses. */
    errf = push_traceback_h(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, d->cb_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, d->stream_ref);
    lua_newtable(L);                              /* result */
    if (n >= 0) {
        lua_pushlstring(L, d->buffer, (size_t)n);
        lua_setfield(L, -2, "data");
        lua_pushinteger(L, (lua_Integer)n);
        lua_setfield(L, -2, "length");
    } else {
        lua_pushstring(L, err ? err->message : "read error");
        lua_setfield(L, -2, "error");
    }
    if (lua_pcall(L, 2, 0, errf) != 0) {
        g_printerr("lgishim_gio read_async error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    luaL_unref(L, LUA_REGISTRYINDEX, d->cb_ref);
    luaL_unref(L, LUA_REGISTRYINDEX, d->stream_ref);
    g_clear_error(&err);
    g_object_unref(d->stream);
    g_free(d->buffer);
    g_free(d);
    lua_settop(L, 0);
    return G_SOURCE_REMOVE;
}

static int
instream_read_async(lua_State *L)
{
    GInputStream *s = chk_instream(L, 1);
    lua_Integer nbytes = luaL_checkinteger(L, 2);
    int prio = (int)luaL_optinteger(L, 3, G_PRIORITY_DEFAULT);
    instream_async_cbdata *d;

    luaL_argcheck(L, nbytes > 0, 2, "nbytes must be > 0");
    luaL_checktype(L, 5, LUA_TFUNCTION);

    d = g_new0(instream_async_cbdata, 1);
    d->L = L;
    d->stream = s;
    g_object_ref(s);
    d->nbytes = (gsize)nbytes;
    d->buffer = g_try_malloc(d->nbytes);
    if (!d->buffer) {
        g_object_unref(s);
        g_free(d);
        luaL_error(L, "lgishim_gio: out of memory");
    }

    /* The Lua ref keeps the wrapper alive until the idle tick; the G ref
     * above keeps the GInputStream itself alive once it is collected. */
    lua_pushvalue(L, 1);
    d->stream_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, 5);
    d->cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_idle_add_full(prio, instream_async_dispatch, d, NULL);
    return 0;
}

/*
 * InputStream has no metatable of its own, so is_type_of() has to match the
 * three concrete subclasses. Metatable identity is the discriminator, as in
 * m_is_type_of(): a wrap from another shim with a colliding cls value would
 * not match, which keeps e.g. an lgishim_glib Variant from passing as a
 * stream.
 */
static int
m_is_type_of_instream(lua_State *L)
{
    int i, same = 0;

    if (!lua_getmetatable(L, 2)) {
        lua_pushboolean(L, FALSE);
        return 1;
    }
    for (i = 1; i <= 3; i++) {
        lua_pushvalue(L, lua_upvalueindex(i));
        if (lua_rawequal(L, -2, -1)) {
            same = 1;
            break;
        }
        lua_pop(L, 1);
    }
    lua_settop(L, 0);
    lua_pushboolean(L, same ? TRUE : FALSE);
    return 1;
}

static const luaL_Reg DATAIN_M[] = {
    { "new",                datain_new },
    { "read_line",          datain_read_line },
    { "read_line_async",    datain_read_line_async },
    { "read_line_finish",   datain_read_line_finish },
    { "set_buffer_size",    datain_set_buffer_size },
    { "close",              datain_close },
    { NULL, NULL },
};

static const luaL_Reg MEMIN_M[] = {
    { "new",            memin_new },
    { "new_from_data",  memin_new_from_data },
    { NULL, NULL },
};

static const luaL_Reg UNIXIN_M[] = {
    { "new",    unixin_new },
    { "get_fd", unixin_get_fd },
    { NULL, NULL },
};

/* Shared by the InputStream class table and the three concrete metatables. */
static const luaL_Reg INSTREAM_M[] = {
    { "read",       instream_read },
    { "read_all",   instream_read_all },
    { "read_async", instream_read_async },
    { NULL, NULL },
};

/*
 * lib/ calls the InputStream methods on concrete instances (`stream:read(n)`),
 * so they must land on the three stream metatables, not just on the InputStream
 * class table. m_index() finds them through its metatable fallback, so a second
 * entry in the registry clsinfo is unnecessary.
 */
static void
install_instream_methods(lua_State *L)
{
    static const int stream_cls[] = { C_DATAIN, C_MEMIN, C_UNIXIN };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(stream_cls); i++) {
        luaL_getmetatable(L, CLS_NAMES[stream_cls[i]]);
        luaL_setfuncs(L, INSTREAM_M, 0);
        lua_pop(L, 1);
    }
}

/* =========================================================== AppInfo family */

/*
 * Theme-side app-launcher surface (awful-dots), the last Gio gap in the
 * launcher chain:
 *   bling/widget/app_launcher/init.lua:545-637 generate_apps()
 *     local app_info = Gio.AppInfo
 *     local apps = app_info.get_all()                  -- Lua array table
 *     app_info.get_name(a) / app.should_show(app)      -- class-table shape
 *     app_info.get_commandline(app) / .get_executable(app) / .get_icon(app)
 *     Gio.DesktopAppInfo.new(app_info.get_id(app))
 *     Gio.DesktopAppInfo.get_string(dai, "Terminal") / (dai, "GenericName")
 *   popups/launcher/launcher2.lua:173-190, 292, 365
 *     for _, entry in ipairs(Gio.AppInfo.get_all()) do
 *     entry:should_show() / :get_name() / :get_icon() / :get_description()
 *     icon:to_string() ; entry.appinfo:launch()
 *   bling/helpers/icon_theme.lua:71-79
 *     Gio.AppInfo.get_all()
 *
 * Both call shapes reach the same C function with the object at arg 1: the
 * same luaL_Reg table is installed on the class table AND on the instance
 * metatable (the clsinfo "m" table), so Gio.AppInfo.get_name(a) and
 * a:get_name() are both fine.
 *
 * gdesktopappinfo.h is absent from this glib's installed headers (see the
 * missing-header list at the top of this file), so GDesktopAppInfo and its
 * two functions are forward-declared with the shapes libgi generates.  Both
 * symbols are present in libgio-2.0.
 */
typedef struct _GDesktopAppInfo GDesktopAppInfo;

GAppInfo *g_desktop_app_info_new(const gchar *id, GError **error);
gchar *g_desktop_app_info_get_string(GDesktopAppInfo *info, const gchar *key);

/* push_str_or_nil(): most getters below return const char * and must yield
 * Lua nil rather than a NULL push (Lua 5.3 asserts on that). */
static int
push_str_or_nil(lua_State *L, const char *s)
{
    if (s)
        lua_pushstring(L, s);
    else
        lua_pushnil(L);
    return 1;
}

/*
 * g_app_info_get_all() is CONTAINER transfer: the elements stay owned by the
 * live app-info monitor and the returned GList only holds the pointers, so
 * each element has to be ref'd here before wrapping.  Handing the list's
 * pointers to the wrappers as-is would let __gc() free objects the monitor
 * still has live.  g_list_free() frees the nodes only.
 *
 * Probed on a host with 52 .desktop apps: ref one element, g_list_free(),
 * unref it, and the object survives -- so the list never owned it (a FULL
 * transfer list would have dropped its refcount to zero).
 */
static int
appinfo_get_all(lua_State *L)
{
    GList *l = g_app_info_get_all();
    GList *it;
    lua_Integer i = 0;

    lua_createtable(L, (int)g_list_length(l), 0);
    for (it = l; it != NULL; it = it->next) {
        g_object_ref(it->data);
        i++;
        push_obj(L, it->data, d_object, C_APPINFO);
        lua_rawseti(L, -2, i);
    }
    g_list_free(l);
    return 1;
}

static int
appinfo_get_id(lua_State *L)
{
    return push_str_or_nil(L, g_app_info_get_id(chk_cls(L, 1, C_APPINFO)));
}

static int
appinfo_get_name(lua_State *L)
{
    return push_str_or_nil(L, g_app_info_get_name(chk_cls(L, 1, C_APPINFO)));
}

static int
appinfo_get_description(lua_State *L)
{
    return push_str_or_nil(L,
                           g_app_info_get_description(chk_cls(L, 1, C_APPINFO)));
}

static int
appinfo_get_executable(lua_State *L)
{
    return push_str_or_nil(L,
                           g_app_info_get_executable(chk_cls(L, 1, C_APPINFO)));
}

static int
appinfo_get_commandline(lua_State *L)
{
    /* const char * in this glib 2.70 build -- a single string, not char **. */
    return push_str_or_nil(L,
        g_app_info_get_commandline(chk_cls(L, 1, C_APPINFO)));
}

static int
appinfo_should_show(lua_State *L)
{
    lua_pushboolean(L, g_app_info_should_show(chk_cls(L, 1, C_APPINFO))
                      ? TRUE : FALSE);
    return 1;
}

/*
 * g_app_info_get_icon() is transfer none: the AppInfo owns the icon for its
 * whole lifetime, so the wrapper takes a ref of its own, or __gc() would
 * unref an icon the AppInfo still holds.  NULL is possible (no Icon= key),
 * and launcher2.lua:178-181 / icon_theme:get_gicon_path() both treat nil as
 * "no icon", so a nil is pushed rather than erroring out.
 */
static int
appinfo_get_icon(lua_State *L)
{
    GIcon *icon = g_app_info_get_icon(chk_cls(L, 1, C_APPINFO));

    if (!icon) {
        lua_pushnil(L);
        return 1;
    }
    g_object_ref(icon);
    return push_obj(L, icon, d_object, C_ICON);
}

/*
 * launcher2.lua:292 and :365 call entry.appinfo:launch() with no arguments.
 * NULL files and a NULL context mean "no files, non-interactively".
 */
static int
appinfo_launch(lua_State *L)
{
    GAppInfo *info = chk_cls(L, 1, C_APPINFO);
    GError *err = NULL;

    if (!lua_isnoneornil(L, 2))
        luaL_error(L, "lgishim_gio: AppInfo:launch(files) is not supported");

    if (g_app_info_launch(info, NULL, NULL, &err))
        lua_pushboolean(L, TRUE);
    else {
        lua_pushnil(L);
        push_error(L, err);
        g_clear_error(&err);
    }
    return 2;
}

static const luaL_Reg APPINFO_M[] = {
    { "get_all",         appinfo_get_all },
    { "get_id",          appinfo_get_id },
    { "get_name",        appinfo_get_name },
    { "get_description", appinfo_get_description },
    { "get_executable",  appinfo_get_executable },
    { "get_commandline", appinfo_get_commandline },
    { "get_icon",        appinfo_get_icon },
    { "should_show",     appinfo_should_show },
    { "launch",          appinfo_launch },
    { NULL, NULL },
};

/* ---- DesktopAppInfo ---- */

static int
desktopappinfo_new(lua_State *L)
{
    GError *err = NULL;
    GAppInfo *info = g_desktop_app_info_new(luaL_checkstring(L, 1), &err);

    if (!info) {
        lua_pushnil(L);
        push_error(L, err);
        g_clear_error(&err);
        return 2;
    }
    return push_obj(L, info, d_object, C_DESKTOPAPPINFO);
}

/*
 * Unlike the const char * getters on GAppInfo, get_string() returns a freshly
 * allocated string (it goes through g_key_file_get_string).  NULL means "key
 * absent", which is exactly how app_launcher decides generic_name is nil and
 * Terminal is false.
 */
static int
desktopappinfo_get_string(lua_State *L)
{
    gchar *s = g_desktop_app_info_get_string(
        (GDesktopAppInfo *)chk_cls(L, 1, C_DESKTOPAPPINFO),
        luaL_checkstring(L, 2));

    if (s) {
        lua_pushstring(L, s);
        g_free(s);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static const luaL_Reg DESKTOPAPPINFO_M[] = {
    { "new",        desktopappinfo_new },
    { "get_string", desktopappinfo_get_string },
    { NULL, NULL },
};

/* ---- Icon ---- */

/*
 * This build's gio/gicon.h only declares g_icon_to_string() and
 * g_icon_new_for_string() -- no g_themed_icon_new -- so Icon stays thin.  The
 * Gtk.IconTheme lookup a real launcher does next (launcher2.lua:181) is a
 * separate shim module that is still out of scope.
 */
static int
icon_to_string(lua_State *L)
{
    gchar *s = g_icon_to_string(chk_cls(L, 1, C_ICON));

    if (s) {
        lua_pushstring(L, s);
        g_free(s);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static const luaL_Reg ICON_M[] = {
    { "to_string", icon_to_string },
    { NULL, NULL },
};

/* ============================================================ D-Bus */

/*
 * Classes and enums for lib/naughty/dbus.lua (session bus: own_name,
 * register_object, emit_signal, proxy call_sync) and the menubar/gears
 * async enumerator surface.
 *
 * Objects arrive from GLib callbacks, never from a constructor (except the
 * FakeInfo builder classes), so the wrappers mostly re-wrap ref'd pointers
 * handed to vtable handlers. register_object uses the VTABLE variant of
 * g_dbus_connection_register_object() rather than the *_with_closures()
 * GClosure path: gobject's closure_marshal() turns a boxed GValue into a
 * plain lightuserdata and every GObject into its own generic wrapper, which
 * would make parameters.value and invocation:return_value() unusable. The
 * vtable callback builds the exact Lua argument list lib/ expects and
 * dispatches it against the GClosure's cbdata (lua_State + registry ref),
 * which lgishim_gobject.c stores at closure->data.
 */

/* --- FakeInfo builders (naughty/dbus.lua:396-432 table-init constructors) */

typedef struct {
    char *name;
    char *signature;
} farg;

typedef struct {
    char *name;
    farg **in;
    size_t nin;
    farg **out;
    size_t nout;
} fmethod;

typedef struct {
    char *name;
    farg **args;
    size_t nargs;
} fsignal;

typedef struct {
    GDBusNodeInfo *node;
    GDBusInterfaceInfo *iface;
} fiface;

static void
d_farg(void *p)
{
    farg *a = (farg *)p;
    g_free(a->name);
    g_free(a->signature);
    g_free(a);
}

static void
d_fmethod(void *p)
{
    fmethod *m = (fmethod *)p;
    g_free(m->name);
    g_free(m);          /* ->in / ->out are borrowed from arg wrappers */
}

static void
d_fsignal(void *p)
{
    fsignal *s = (fsignal *)p;
    g_free(s->name);
    g_free(s);          /* ->args are borrowed from arg wrappers */
}

static void
d_fiface(void *p)
{
    fiface *f = (fiface *)p;
    if (f->iface)
        g_dbus_interface_info_unref(f->iface);
    if (f->node)
        g_dbus_node_info_unref(f->node);
    g_free(f);
}

static char *
field_string(lua_State *L, int idx, const char *key)
{
    const char *s;

    lua_getfield(L, idx, key);
    s = lua_tostring(L, -1);
    lua_pop(L, 1);
    return g_strdup(s ? s : "");
}

/* Parse an array of C_DBUSARG wrappers into a newly allocated farg** array
 * of BORROWED pointers (the wrappers own their farg structs). */
static farg **
arg_array(lua_State *L, int idx, size_t *n)
{
    farg **arr = NULL;
    size_t i, len = 0;

    *n = 0;
    if (!lua_istable(L, idx))
        return NULL;
    len = (size_t)lua_rawlen(L, idx);
    if (len == 0)
        return NULL;
    arr = g_new0(farg *, len);
    for (i = 0; i < len; i++) {
        lua_rawgeti(L, idx, (lua_Integer)(i + 1));
        arr[i] = (farg *)chk_cls(L, -1, C_DBUSARG);
        lua_pop(L, 1);
    }
    *n = len;
    return arr;
}

static int
dbusarg_new(lua_State *L)
{
    farg *a;

    luaL_checktype(L, 1, LUA_TTABLE);
    a = g_new0(farg, 1);
    a->name = field_string(L, 1, "name");
    a->signature = field_string(L, 1, "signature");
    return push_obj(L, a, d_farg, C_DBUSARG);
}

static int
dbusmethod_new(lua_State *L)
{
    fmethod *m;

    luaL_checktype(L, 1, LUA_TTABLE);
    m = g_new0(fmethod, 1);
    m->name = field_string(L, 1, "name");
    lua_getfield(L, 1, "in_args");
    m->in = arg_array(L, -1, &m->nin);
    lua_pop(L, 1);
    lua_getfield(L, 1, "out_args");
    m->out = arg_array(L, -1, &m->nout);
    lua_pop(L, 1);
    return push_obj(L, m, d_fmethod, C_DBUSMETHOD);
}

static int
dbussignal_new(lua_State *L)
{
    fsignal *s;

    luaL_checktype(L, 1, LUA_TTABLE);
    s = g_new0(fsignal, 1);
    s->name = field_string(L, 1, "name");
    lua_getfield(L, 1, "args");
    s->args = arg_array(L, -1, &s->nargs);
    lua_pop(L, 1);
    return push_obj(L, s, d_fsignal, C_DBUSSIGNAL);
}

/* Render one arg as <arg direction=".." type=".." name=".."/> */
static void
xml_arg(GString *x, const char *dir, farg *a)
{
    /* Signals carry no direction attribute; D-Bus XML rejects direction=''. */
    g_string_append(x, "<arg type='");
    xml_esc(x, a->signature);
    g_string_append(x, "'");
    if (dir && *dir) {
        g_string_append(x, " direction='");
        xml_esc(x, dir);
        g_string_append(x, "'");
    }
    g_string_append(x, " name='");
    xml_esc(x, a->name);
    g_string_append(x, "'/>");
}

/*
 * DBusInterfaceInfo{name=,methods={...},signals={...}} -> parse the table
 * into D-Bus XML, hand it to g_dbus_node_info_new_for_xml() (2-param in this
 * build), ref the matching interface and keep BOTH the node (anchor) and the
 * ref'd interface alive until GC.
 */
static int
dbusiface_new(lua_State *L)
{
    fiface *f;
    GString *x;
    GError *err = NULL;
    char *iname;
    size_t i;

    luaL_checktype(L, 1, LUA_TTABLE);
    iname = field_string(L, 1, "name");
    x = g_string_new("<node><interface name='");
    xml_esc(x, iname);
    g_string_append(x, "'>");

    lua_getfield(L, 1, "methods");
    if (lua_istable(L, -1)) {
        for (i = 0; i < (size_t)lua_rawlen(L, -1); i++) {
            fmethod *m;
            size_t j;
            lua_rawgeti(L, -1, (lua_Integer)(i + 1));
            m = (fmethod *)chk_cls(L, -1, C_DBUSMETHOD);
            lua_pop(L, 1);
            g_string_append(x, "<method name='");
            xml_esc(x, m->name);
            g_string_append(x, "'>");
            for (j = 0; j < m->nin; j++)
                xml_arg(x, "in", m->in[j]);
            for (j = 0; j < m->nout; j++)
                xml_arg(x, "out", m->out[j]);
            g_string_append(x, "</method>");
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "signals");
    if (lua_istable(L, -1)) {
        for (i = 0; i < (size_t)lua_rawlen(L, -1); i++) {
            fsignal *s;
            size_t j;
            lua_rawgeti(L, -1, (lua_Integer)(i + 1));
            s = (fsignal *)chk_cls(L, -1, C_DBUSSIGNAL);
            lua_pop(L, 1);
            g_string_append(x, "<signal name='");
            xml_esc(x, s->name);
            g_string_append(x, "'>");
            for (j = 0; j < s->nargs; j++)
                xml_arg(x, "", s->args[j]);
            g_string_append(x, "</signal>");
        }
    }
    lua_pop(L, 1);

    g_string_append(x, "</interface></node>");

    f = g_new0(fiface, 1);
    f->node = g_dbus_node_info_new_for_xml(x->str, &err);
    g_string_free(x, TRUE);
    if (!f->node) {
        g_free(f);
        if (err)
            g_printerr("lgishim_gio: g_dbus_node_info_new_for_xml: %s\n",
                       err->message);
        g_clear_error(&err);
        g_free(iname);
        lua_pushnil(L);
        return 1;
    }
    f->iface = g_dbus_interface_info_ref(
        g_dbus_node_info_lookup_interface(f->node, iname));
    g_free(iname);
    return push_obj(L, f, d_fiface, C_DBUSIFACE);
}

/* --- DBusVariant: wraps a GVariant*, exposing `.value` as a Lua table
 * (field-style access via the "p" property-getter table). */

static int
dbusvar_new(lua_State *L)
{
    luaL_error(L, "lgishim_gio: DBusVariant is only produced by callbacks");
    return 0;
}

/*
 * `.value`: tuples and arrays keep their shape (Notify unpacks
 * `parameters.value`; hints a{sv} is a dict table inside it); a scalar
 * variant (e.g. the uint32 reply of GetConnectionUnixProcessID, which
 * dbus.lua reads as `pid.value[1]`) is wrapped as { [1] = scalar }.
 */
static int
dbusvar_get_value(lua_State *L)
{
    GVariant *v = (GVariant *)chk_cls(L, 1, C_DBUSVAR);

    if (!v) {
        lua_pushnil(L);
        return 1;
    }
    switch (g_variant_classify(v)) {
    case G_VARIANT_CLASS_TUPLE:
    case G_VARIANT_CLASS_ARRAY:
        variant_to_lua(L, v);
        break;
    default:
        lua_newtable(L);
        variant_to_lua(L, v);
        lua_rawseti(L, -2, 1);
        break;
    }
    return 1;
}

static int
dbusvar_type_string(lua_State *L)
{
    GVariant *v = (GVariant *)chk_cls(L, 1, C_DBUSVAR);

    lua_pushstring(L, v ? g_variant_get_type_string(v) : "");
    return 1;
}

static int
dbusvar_n_children(lua_State *L)
{
    GVariant *v = (GVariant *)chk_cls(L, 1, C_DBUSVAR);

    lua_pushinteger(L, (lua_Integer)(v ? g_variant_n_children(v) : 0));
    return 1;
}

/* --- DBusMethodInvocation */

static int
dbusinv_get_sender(lua_State *L)
{
    const char *s = g_dbus_method_invocation_get_sender(
        (GDBusMethodInvocation *)chk_cls(L, 1, C_DBUSINV));
    if (s)
        lua_pushstring(L, s);
    else
        lua_pushnil(L);
    return 1;
}

static int
dbusinv_get_object_path(lua_State *L)
{
    const char *s = g_dbus_method_invocation_get_object_path(
        (GDBusMethodInvocation *)chk_cls(L, 1, C_DBUSINV));
    lua_pushstring(L, s ? s : "");
    return 1;
}

static int
dbusinv_get_interface_name(lua_State *L)
{
    const char *s = g_dbus_method_invocation_get_interface_name(
        (GDBusMethodInvocation *)chk_cls(L, 1, C_DBUSINV));
    lua_pushstring(L, s ? s : "");
    return 1;
}

static int
dbusinv_get_method_name(lua_State *L)
{
    const char *s = g_dbus_method_invocation_get_method_name(
        (GDBusMethodInvocation *)chk_cls(L, 1, C_DBUSINV));
    lua_pushstring(L, s ? s : "");
    return 1;
}

static int
dbusinv_get_parameters(lua_State *L)
{
    GVariant *v = g_dbus_method_invocation_get_parameters(
        (GDBusMethodInvocation *)chk_cls(L, 1, C_DBUSINV));

    if (v)
        return push_obj(L, g_variant_ref(v), d_variant, C_DBUSVAR);
    lua_pushnil(L);
    return 1;
}

/* return_value() does NOT take ownership of a non-floating variant, so the
 * ref'd copy from variant_from_lua() is unref'd here in every path. */
static int
dbusinv_return_value(lua_State *L)
{
    GDBusMethodInvocation *inv = (GDBusMethodInvocation *)chk_cls(L, 1, C_DBUSINV);
    GVariant *v = variant_from_lua(L, 2);

    g_dbus_method_invocation_return_value(inv, v);
    if (v)
        g_variant_unref(v);
    return 0;
}

static int
dbusinv_return_error(lua_State *L)
{
    GDBusMethodInvocation *inv = (GDBusMethodInvocation *)chk_cls(L, 1, C_DBUSINV);
    const char *domain = luaL_optstring(L, 2, "g-io-error-quark");
    const char *message = luaL_optstring(L, 4, "");

    g_dbus_method_invocation_return_error(inv, g_quark_from_string(domain),
                                          (gint)luaL_optinteger(L, 3, 0),
                                          "%s", message);
    return 0;
}

/* --- DBusConnection (mostly produced by bus_own_name / bus_get callbacks) */

static int
dbusconn_call_sync(lua_State *L)
{
    GDBusConnection *c = (GDBusConnection *)chk_cls(L, 1, C_DBUSCONN);
    GVariant *params = variant_from_lua(L, 6);
    GVariantType *rt = NULL;
    GVariant *reply = NULL;
    GError *err = NULL;

    if (lua_isstring(L, 7)) {
        rt = g_variant_type_new(lua_tostring(L, 7));
        lua_pop(L, 1);
    }
    reply = g_dbus_connection_call_sync(c,
        lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2),
        luaL_checkstring(L, 3), luaL_checkstring(L, 4),
        luaL_checkstring(L, 5), params, rt,
        (GDBusCallFlags)flags_arg(L, 8, G_DBUS_CALL_FLAGS_NONE),
        (gint)luaL_optinteger(L, 9, -1), NULL, &err);
    if (params)
        g_variant_unref(params);
    if (rt)
        g_variant_type_free(rt);
    if (reply)
        return push_obj(L, reply, d_variant, C_DBUSVAR);
    lua_pushnil(L);
    push_error(L, err);
    g_clear_error(&err);
    return 2;
}

static int
dbusconn_emit_signal(lua_State *L)
{
    GDBusConnection *c = (GDBusConnection *)chk_cls(L, 1, C_DBUSCONN);
    GVariant *params = variant_from_lua(L, 6);
    GError *err = NULL;
    gboolean ok = g_dbus_connection_emit_signal(c,
        lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2),
        luaL_checkstring(L, 3), luaL_checkstring(L, 4),
        luaL_checkstring(L, 5), params, &err);

    if (params)
        g_variant_unref(params);
    lua_pushboolean(L, ok ? TRUE : FALSE);
    push_error(L, err);
    g_clear_error(&err);
    return 2;
}

/* --- vtable method-call dispatch (see module comment at the top of the
 * D-Bus section: the GClosure is used only as a cbdata container). */

static void
gio_dbus_method_call(GDBusConnection *connection, const gchar *sender,
                     const gchar *object_path, const gchar *interface_name,
                     const gchar *method_name, GVariant *parameters,
                     GDBusMethodInvocation *invocation, gpointer user_data)
{
    GClosure *cl = (GClosure *)user_data;
    gobject_cbdata *cb;
    lua_State *L;

    if (!cl || cl->is_invalid)
        return;
    cb = (gobject_cbdata *)cl->data;
    if (!cb || cb->ref == LUA_NOREF)
        return;
    L = cb->L;

    lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);   /* fn */
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    if (connection)
        push_obj(L, g_object_ref(connection), d_object, C_DBUSCONN);
    else
        lua_pushnil(L);
    lua_pushstring(L, sender ? sender : "");
    lua_pushstring(L, object_path ? object_path : "");
    lua_pushstring(L, interface_name ? interface_name : "");
    lua_pushstring(L, method_name ? method_name : "");
    if (parameters) {
        GVariant *p = g_variant_ref(parameters);
        push_obj(L, p, d_variant, C_DBUSVAR);
    } else {
        lua_pushnil(L);
    }
    /* transfer-full ownership: rebind our ref, drop the passed-in one so
     * that __gc unref + this unref balance to exactly zero. */
    /* GDBusMethodInvocation is a GObject: ref/unref spellings were removed
     * from newer GLib headers, use the object pair. */
    push_obj(L, g_object_ref(invocation), d_object, C_DBUSINV);
    g_object_unref(invocation);

    /* [fn, conn, sender, path, iface, method, parameters, invocation] */
    pcall_dispatch(L, 7);
}

static const GDBusInterfaceVTable dbus_vtable = {
    .method_call = gio_dbus_method_call,
    .get_property = NULL,
    .set_property = NULL
};

static int
dbusconn_register_object(lua_State *L)
{
    GDBusConnection *c = (GDBusConnection *)chk_cls(L, 1, C_DBUSCONN);
    fiface *f = (fiface *)chk_cls(L, 3, C_DBUSIFACE);
    GClosure *cl = g_closure_ref(lgishim_gobject_check_closure(L, 4));
    GError *err = NULL;
    guint id;

    if (!f->iface) {
        lua_pushboolean(L, FALSE);
        lua_pushstring(L, "lgishim_gio: interface_info has no GDBusInterfaceInfo");
        g_closure_unref(cl);
        return 2;
    }
    id = g_dbus_connection_register_object(c, luaL_checkstring(L, 2),
        f->iface, &dbus_vtable, (gpointer)cl,
        (GDestroyNotify)g_closure_unref, &err);
    if (id == 0) {
        lua_pushboolean(L, FALSE);
        push_error(L, err);
        g_clear_error(&err);
        g_closure_unref(cl);
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int
dbusconn_close(lua_State *L)
{
    GDBusConnection *c = (GDBusConnection *)chk_cls(L, 1, C_DBUSCONN);

    g_dbus_connection_close(c, NULL, NULL, NULL);
    lua_pushboolean(L, TRUE);
    return 1;
}

static int
dbusconn_close_sync(lua_State *L)
{
    GDBusConnection *c = (GDBusConnection *)chk_cls(L, 1, C_DBUSCONN);
    GError *err = NULL;
    gboolean ok = g_dbus_connection_close_sync(c, NULL, &err);

    lua_pushboolean(L, ok ? TRUE : FALSE);
    push_error(L, err);
    g_clear_error(&err);
    return 2;
}

static int
dbusconn_is_closed(lua_State *L)
{
    lua_pushboolean(L, g_dbus_connection_is_closed(
        (GDBusConnection *)chk_cls(L, 1, C_DBUSCONN)) ? TRUE : FALSE);
    return 1;
}

static int
dbusconn_flush_sync(lua_State *L)
{
    GDBusConnection *c = (GDBusConnection *)chk_cls(L, 1, C_DBUSCONN);
    GError *err = NULL;
    gboolean ok = g_dbus_connection_flush_sync(c, NULL, &err);

    lua_pushboolean(L, ok ? TRUE : FALSE);
    push_error(L, err);
    g_clear_error(&err);
    return 2;
}

static int
dbusconn_get_unique_name(lua_State *L)
{
    const char *s = g_dbus_connection_get_unique_name(
        (GDBusConnection *)chk_cls(L, 1, C_DBUSCONN));

    if (s)
        lua_pushstring(L, s);
    else
        lua_pushnil(L);
    return 1;
}

/* --- DBusProxy */

static int
dbusproxy_new_for_bus_sync(lua_State *L)
{
    GDBusProxy *p;
    GError *err = NULL;
    fiface *f = NULL;
    lua_Integer bus = luaL_optinteger(L, 1, G_BUS_TYPE_SESSION);
    GDBusProxyFlags flags = (GDBusProxyFlags)flags_arg(L, 2, G_DBUS_PROXY_FLAGS_NONE);
    const char *name = luaL_checkstring(L, 4);
    const char *path = luaL_checkstring(L, 5);
    const char *iface = luaL_checkstring(L, 6);

    if (!lua_isnoneornil(L, 3))
        f = (fiface *)chk_cls(L, 3, C_DBUSIFACE);
    p = g_dbus_proxy_new_for_bus_sync((GBusType)bus, flags,
        f ? f->iface : NULL, name, path, iface, NULL, &err);
    if (!p) {
        lua_pushnil(L);
        push_error(L, err);
        g_clear_error(&err);
        return 2;
    }
    return push_obj(L, p, d_object, C_DBUSPROXY);
}

/* Async entry: lib/naughty/dbus.lua:439 calls it with a callback; without an
 * event loop there is nothing to defer to, so resolve immediately through
 * the sync path and hand the result (or nil, err) to the Lua callback. */
static int
dbusproxy_new_for_bus(lua_State *L)
{
    GDBusProxy *p;
    GError *err = NULL;
    fiface *f = NULL;
    lua_Integer bus = luaL_optinteger(L, 1, G_BUS_TYPE_SESSION);
    GDBusProxyFlags flags = (GDBusProxyFlags)flags_arg(L, 2, G_DBUS_PROXY_FLAGS_NONE);
    const char *name = luaL_checkstring(L, 4);
    const char *path = luaL_checkstring(L, 5);
    const char *iface = luaL_checkstring(L, 6);

    if (!lua_isnoneornil(L, 3))
        f = (fiface *)chk_cls(L, 3, C_DBUSIFACE);
    p = g_dbus_proxy_new_for_bus_sync((GBusType)bus, flags,
        f ? f->iface : NULL, name, path, iface, NULL, &err);
    lua_settop(L, 0);
    lua_pushvalue(L, 8);                        /* callback */
    if (!lua_isfunction(L, -1)) {
        /* No callback: mirror the sync path's result pair. */
        lua_pop(L, 1);
        if (!p) {
            lua_pushnil(L);
            push_error(L, err);
            g_clear_error(&err);
            return 2;
        }
        return push_obj(L, p, d_object, C_DBUSPROXY);
    }
    if (!p) {
        lua_pushnil(L);
        push_error(L, err);
        g_clear_error(&err);
        pcall_dispatch(L, 2);
        return 0;
    }
    push_obj(L, p, d_object, C_DBUSPROXY);
    pcall_dispatch(L, 1);
    return 0;
}

static int
dbusproxy_call_sync(lua_State *L)
{
    GDBusProxy *p = (GDBusProxy *)chk_cls(L, 1, C_DBUSPROXY);
    GVariant *params = variant_from_lua(L, 3);
    GVariant *reply;
    GError *err = NULL;

    reply = g_dbus_proxy_call_sync(p, luaL_checkstring(L, 2), params,
        (GDBusCallFlags)flags_arg(L, 4, G_DBUS_CALL_FLAGS_NONE),
        (gint)luaL_optinteger(L, 5, -1), NULL, &err);
    if (params)
        g_variant_unref(params);
    if (reply)
        return push_obj(L, reply, d_variant, C_DBUSVAR);
    lua_pushnil(L);
    push_error(L, err);
    g_clear_error(&err);
    return 2;
}

static int
dbusproxy_get_name(lua_State *L)
{
    const char *s = g_dbus_proxy_get_name((GDBusProxy *)chk_cls(L, 1, C_DBUSPROXY));
    lua_pushstring(L, s ? s : "");
    return 1;
}

static int
dbusproxy_get_object_path(lua_State *L)
{
    const char *s = g_dbus_proxy_get_object_path((GDBusProxy *)chk_cls(L, 1, C_DBUSPROXY));
    lua_pushstring(L, s ? s : "");
    return 1;
}

static int
dbusproxy_get_interface_name(lua_State *L)
{
    const char *s = g_dbus_proxy_get_interface_name((GDBusProxy *)chk_cls(L, 1, C_DBUSPROXY));
    lua_pushstring(L, s ? s : "");
    return 1;
}

/* --- module-level bus functions */

typedef struct {
    GClosure *acquired;
    GClosure *name_acquired;
    GClosure *name_lost;
} bown;

static void
d_bown(void *p)
{
    bown *b = (bown *)p;

    if (b->acquired)
        g_closure_unref(b->acquired);
    if (b->name_acquired)
        g_closure_unref(b->name_acquired);
    if (b->name_lost)
        g_closure_unref(b->name_lost);
    g_free(b);
}

/* Build (conn-or-nil, name) and dispatch against closure's cbdata. */
static void
dispatch_bus_cb(GClosure *cl, GDBusConnection *conn, const gchar *name)
{
    gobject_cbdata *cb;
    lua_State *L;

    if (!cl || cl->is_invalid)
        return;
    cb = (gobject_cbdata *)cl->data;
    if (!cb || cb->ref == LUA_NOREF)
        return;
    L = cb->L;

    lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);   /* fn */
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    if (conn)
        push_obj(L, g_object_ref(conn), d_object, C_DBUSCONN);
    else
        lua_pushnil(L);
    lua_pushstring(L, name ? name : "");
    /* Do NOT normalize with lua_insert(L, 1): the top here is `name`, so it
     * would move the name into slot 1 and pcall_dispatch would treat the
     * bus name string as the callback.  Any residue below is fine --
     * pcall_dispatch locates the callback as gettop - nargs - 1. */
    pcall_dispatch(L, 2);
}

static void
bus_acquired_cb(GDBusConnection *connection, const gchar *name, gpointer ud)
{
    dispatch_bus_cb(((bown *)ud)->acquired, connection, name);
}

static void
bus_name_acquired_cb(GDBusConnection *connection, const gchar *name, gpointer ud)
{
    dispatch_bus_cb(((bown *)ud)->name_acquired, connection, name);
}

static void
bus_name_lost_cb(GDBusConnection *connection, const gchar *name, gpointer ud)
{
    dispatch_bus_cb(((bown *)ud)->name_lost, connection, name);
}

static int
gio_bus_own_name(lua_State *L)
{
    bown *b = g_new0(bown, 1);
    guint id;

    b->acquired = g_closure_ref(lgishim_gobject_check_closure(L, 4));
    b->name_acquired = g_closure_ref(lgishim_gobject_check_closure(L, 5));
    b->name_lost = g_closure_ref(lgishim_gobject_check_closure(L, 6));
    id = g_bus_own_name((GBusType)luaL_optinteger(L, 1, G_BUS_TYPE_SESSION),
        luaL_checkstring(L, 2),
        (GBusNameOwnerFlags)flags_arg(L, 3, G_BUS_NAME_OWNER_FLAGS_NONE),
        bus_acquired_cb, bus_name_acquired_cb, bus_name_lost_cb,
        (gpointer)b, (GDestroyNotify)d_bown);
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int
gio_bus_get_sync(lua_State *L)
{
    GDBusConnection *c;
    GError *err = NULL;

    c = g_bus_get_sync((GBusType)luaL_optinteger(L, 1, G_BUS_TYPE_SESSION),
        NULL, &err);
    if (!c) {
        lua_pushnil(L);
        push_error(L, err);
        g_clear_error(&err);
        return 2;
    }
    return push_obj(L, c, d_object, C_DBUSCONN);
}

static int
gio_bus_get(lua_State *L)
{
    GDBusConnection *c;
    GError *err = NULL;

    c = g_bus_get_sync((GBusType)luaL_optinteger(L, 1, G_BUS_TYPE_SESSION),
        NULL, &err);
    lua_settop(L, 0);
    lua_pushvalue(L, 2);                        /* callback */
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        if (!c) {
            lua_pushnil(L);
            push_error(L, err);
            g_clear_error(&err);
            return 2;
        }
        return push_obj(L, c, d_object, C_DBUSCONN);
    }
    if (!c) {
        lua_pushnil(L);
        push_error(L, err);
        g_clear_error(&err);
        pcall_dispatch(L, 2);
        return 0;
    }
    push_obj(L, c, d_object, C_DBUSCONN);
    pcall_dispatch(L, 1);
    return 0;
}

/* --- Gio.Async: menubar/utils.lua:407 does
 *   gio.Async.start(do_protected_call)(function() ... end)
 * The real LGI version schedules a GTask; with no main loop to defer to,
 * running the task immediately inside do_protected_call() is equivalent
 * from the caller's point of view (results arrive before call_callback). */

static int
gio_async_run(lua_State *L)
{
    lua_pushvalue(L, lua_upvalueindex(1));      /* task (do_protected_call) */
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 0);          /* task(body) */
    return 0;
}

static int
gio_async_start(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, gio_async_run, 1);
    return 1;
}

/* --- Gio.Async.call: `Gio.Async.call(func, cancellable, io_priority)`
 * returns a starter; calling the starter runs func(...) and returns its
 * results. Faithful to lgi's override: the body runs in a coroutine spawned
 * from an idle source inside a nested GMainLoop, and the nested loop is what
 * gives it lgi's modality -- work scheduled from inside the body still reaches
 * the main loop while the starter is blocked, and the results come back through
 * the same call. Async.start() above is a different beast and is untouched.
 *
 * `cancellable` is accepted but not forwarded (the shim has no GCancellable);
 * `io_priority` becomes the idle source priority, like the async-context
 * priority in the real override.
 */

typedef struct {
    lua_State *L;
    lua_State *t;      /* the body's coroutine, pinned by this ctx */
    GMainLoop *loop;
    int nargs;
} async_call_ctx;

static gboolean
async_call_idle(gpointer user_data)
{
    async_call_ctx *c = (async_call_ctx *)user_data;

    /* The coroutine already holds the body, so move the starter's args on top
     * of it first; lua_resume() then hands them to the body. */
    if (c->nargs > 0)
        lua_xmove(c->L, c->t, c->nargs);

    switch (lua_resume(c->t, c->L, c->nargs)) {
    case LUA_OK: {
        /* The results sit on the coroutine's stack; move them onto the outer
         * one so the starter can return them from its own top. */
        int n = lua_gettop(c->t);
        if (n > 0)
            lua_xmove(c->t, c->L, n);
        break;
    }
    case LUA_YIELD:
        /* lgi leaves the coroutine suspended until a resumed async callback
         * finishes it. This shim has no GAsyncReady dispatch that would
         * resume it, so the loop is quit instead: the caller gets no results
         * rather than an infinite hang. */
        g_printerr("lgishim_gio: Async.call body yielded; nothing in this shim "
                   "would resume it, so no results are returned\n");
        break;
    default:
        g_printerr("lgishim_gio: Async.call body error: %s\n",
                   lua_tostring(c->t, -1));
        break;
    }

    g_main_loop_quit(c->loop);
    g_free(c);
    return G_SOURCE_REMOVE;
}

static int
async_call_start(lua_State *L)
{
    lua_State *t;
    int prio = (int)luaL_checkinteger(L, lua_upvalueindex(2));
    int nargs;
    GMainLoop *loop;
    async_call_ctx *c;

    luaL_checkstack(L, LUA_EXTRASPACE, "lgishim_gio: Async.call nested too deep");

    /* A fresh coroutine per starter call: a coroutine may only be resumed once,
     * and lgi's starter is reusable. The body goes on the coroutine's stack
     * first, and the thread is dropped again so lua_gettop() below counts only
     * the caller's arguments. */
    t = lua_newthread(L);
    lua_pushvalue(L, lua_upvalueindex(1));      /* the body */
    lua_xmove(L, t, 1);
    lua_pop(L, 1);                              /* the thread */
    /* lua_gettop() excludes this closure, so no -1: in Lua 5.2+ the function
     * being called sits above the top, and index 1 is the first argument. */
    nargs = lua_gettop(L);

    c = g_new0(async_call_ctx, 1);
    c->L = L;
    c->t = t;
    c->nargs = nargs;
    c->loop = loop = g_main_loop_new(NULL, FALSE);

    /* Spawn the body inside an idle source, not inline: if the coroutine
     * finishes during its first resume, an inline resume would quit the loop
     * before the loop had a chance to enter at all. */
    g_idle_add_full(prio, async_call_idle, c, NULL);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    /* The idle source moved the results onto the top and freed the ctx. */
    return lua_gettop(L);
}

static int
gio_async_call(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);

    /* The body is captured by the starter, which builds the coroutine; the
     * thread is pinned by the ctx while the nested loop runs. */
    lua_pushvalue(L, 1);
    lua_pushinteger(L, (lua_Integer)luaL_optinteger(L, 3, G_PRIORITY_DEFAULT));
    lua_pushcclosure(L, async_call_start, 2);
    return 1;
}

/* --- class/enum tables */

static const luaL_Reg DBUSVAR_M[] = {
    { "new",         dbusvar_new },
    { "type_string", dbusvar_type_string },
    { "n_children",  dbusvar_n_children },
    { NULL, NULL },
};

static const luaL_Reg DBUSVAR_P[] = {
    { "value", dbusvar_get_value },
    { NULL, NULL },
};

static const luaL_Reg DBUSINV_M[] = {
    { "get_sender",         dbusinv_get_sender },
    { "get_object_path",    dbusinv_get_object_path },
    { "get_interface_name", dbusinv_get_interface_name },
    { "get_method_name",    dbusinv_get_method_name },
    { "get_parameters",     dbusinv_get_parameters },
    { "return_value",       dbusinv_return_value },
    { "return_error",       dbusinv_return_error },
    { NULL, NULL },
};

static const luaL_Reg DBUSCONN_M[] = {
    { "call_sync",      dbusconn_call_sync },
    { "emit_signal",    dbusconn_emit_signal },
    { "register_object", dbusconn_register_object },
    { "close",          dbusconn_close },
    { "close_sync",     dbusconn_close_sync },
    { "is_closed",      dbusconn_is_closed },
    { "flush_sync",     dbusconn_flush_sync },
    { "get_unique_name", dbusconn_get_unique_name },
    { NULL, NULL },
};

static const luaL_Reg DBUSPROXY_M[] = {
    { "new_for_bus",         dbusproxy_new_for_bus },
    { "new_for_bus_sync",    dbusproxy_new_for_bus_sync },
    { "call_sync",           dbusproxy_call_sync },
    { "get_name",            dbusproxy_get_name },
    { "get_object_path",     dbusproxy_get_object_path },
    { "get_interface_name",  dbusproxy_get_interface_name },
    { NULL, NULL },
};

static const luaL_Reg DBUSARG_M[] = {
    { "new", dbusarg_new },
    { NULL, NULL },
};

static const luaL_Reg DBUSMETHOD_M[] = {
    { "new", dbusmethod_new },
    { NULL, NULL },
};

static const luaL_Reg DBUSSIGNAL_M[] = {
    { "new", dbussignal_new },
    { NULL, NULL },
};

static const luaL_Reg DBUSIFACE_M[] = {
    { "new", dbusiface_new },
    { NULL, NULL },
};

static const struct enumpair FILE_TYPES[] = {
    { "UNKNOWN",       G_FILE_TYPE_UNKNOWN },
    { "REGULAR",       G_FILE_TYPE_REGULAR },
    { "DIRECTORY",     G_FILE_TYPE_DIRECTORY },
    { "SYMBOLIC_LINK", G_FILE_TYPE_SYMBOLIC_LINK },
    { "SPECIAL",       G_FILE_TYPE_SPECIAL },
    { "SHORTCUT",      G_FILE_TYPE_SHORTCUT },
    { "MOUNTABLE",     G_FILE_TYPE_MOUNTABLE },
};

static const struct enumpair FILE_ATTRIBUTE_TYPES[] = {
    { "INVALID",    G_FILE_ATTRIBUTE_TYPE_INVALID },
    { "STRING",     G_FILE_ATTRIBUTE_TYPE_STRING },
    { "BYTE_STRING", G_FILE_ATTRIBUTE_TYPE_BYTE_STRING },
    { "BOOLEAN",    G_FILE_ATTRIBUTE_TYPE_BOOLEAN },
    { "UINT32",     G_FILE_ATTRIBUTE_TYPE_UINT32 },
    { "INT32",      G_FILE_ATTRIBUTE_TYPE_INT32 },
    { "UINT64",     G_FILE_ATTRIBUTE_TYPE_UINT64 },
    { "INT64",      G_FILE_ATTRIBUTE_TYPE_INT64 },
    { "STRINGV",    G_FILE_ATTRIBUTE_TYPE_STRINGV },
    { "OBJECT",     G_FILE_ATTRIBUTE_TYPE_OBJECT },
};

/* --- module entry point */

int
luaopen_lgishim_gio(lua_State *L)
{
    lua_newtable(L);                           /* module table */

    /* make_class_table() leaves the class table on top of the stack, so each
     * class_setup() must be followed by a setfield that pops it into the module
     * table -- exactly the lgishim_gobject luaopen pattern.  Without the
     * setfield the module table is never populated and `return 1` hands back
     * the last class table instead, leaving Gio.DBusProxy & co. nil. */
    class_setup(L, C_FILE,        FILE_M, NULL);
    lua_setfield(L, -2, "File");
    class_setup(L, C_FILEINFO,    FILEINFO_M, NULL);
    lua_setfield(L, -2, "FileInfo");
    class_setup(L, C_FILEENUM,    FILEENUM_M, NULL);
    lua_setfield(L, -2, "FileEnumerator");
    class_setup(L, C_DATAIN,      DATAIN_M, NULL);
    lua_setfield(L, -2, "DataInputStream");
    class_setup(L, C_MEMIN,       MEMIN_M, NULL);
    lua_setfield(L, -2, "MemoryInputStream");
    class_setup(L, C_UNIXIN,      UNIXIN_M, NULL);
    lua_setfield(L, -2, "UnixInputStream");

    /* InputStream is a base class only: the three stream metatables get the
     * shared methods, and the class table carries them plus is_type_of(),
     * which matches any of the three subclass metatables (see
     * m_is_type_of_instream()). No metatable and no `new`: the shim never
     * wraps a bare GInputStream. */
    install_instream_methods(L);
    lua_newtable(L);                           /* InputStream class table */
    luaL_setfuncs(L, INSTREAM_M, 0);
    luaL_getmetatable(L, CLS_NAMES[C_DATAIN]);
    luaL_getmetatable(L, CLS_NAMES[C_MEMIN]);
    luaL_getmetatable(L, CLS_NAMES[C_UNIXIN]);
    lua_pushcclosure(L, m_is_type_of_instream, 3);
    lua_setfield(L, -2, "is_type_of");
    lua_setfield(L, -2, "InputStream");

    class_setup(L, C_DBUSCONN,    DBUSCONN_M, NULL);
    lua_setfield(L, -2, "DBusConnection");
    class_setup(L, C_DBUSPROXY,   DBUSPROXY_M, NULL);
    lua_setfield(L, -2, "DBusProxy");
    class_setup(L, C_DBUSINV,     DBUSINV_M, NULL);
    lua_setfield(L, -2, "DBusMethodInvocation");
    class_setup(L, C_DBUSVAR,     DBUSVAR_M, DBUSVAR_P);
    lua_setfield(L, -2, "DBusVariant");
    class_setup(L, C_DBUSARG,     DBUSARG_M, NULL);
    lua_setfield(L, -2, "DBusArgInfo");
    class_setup(L, C_DBUSMETHOD,  DBUSMETHOD_M, NULL);
    lua_setfield(L, -2, "DBusMethodInfo");
    class_setup(L, C_DBUSSIGNAL,  DBUSSIGNAL_M, NULL);
    lua_setfield(L, -2, "DBusSignalInfo");
    class_setup(L, C_DBUSIFACE,   DBUSIFACE_M, NULL);
    lua_setfield(L, -2, "DBusInterfaceInfo");
    class_setup(L, C_APPINFO,      APPINFO_M, NULL);
    lua_setfield(L, -2, "AppInfo");
    class_setup(L, C_DESKTOPAPPINFO, DESKTOPAPPINFO_M, NULL);
    lua_setfield(L, -2, "DesktopAppInfo");
    class_setup(L, C_ICON,         ICON_M, NULL);
    lua_setfield(L, -2, "Icon");

    install_enum(L, "BusType", BUS_TYPES, G_N_ELEMENTS(BUS_TYPES));
    install_enum(L, "BusNameOwnerFlags", BUS_NAME_OWNER_FLAGS,
                 G_N_ELEMENTS(BUS_NAME_OWNER_FLAGS));
    install_enum(L, "DBusProxyFlags", DBUS_PROXY_FLAGS,
                 G_N_ELEMENTS(DBUS_PROXY_FLAGS));
    install_enum(L, "DBusCallFlags", DBUS_CALL_FLAGS,
                 G_N_ELEMENTS(DBUS_CALL_FLAGS));
    install_enum(L, "FileQueryInfoFlags", FILE_QUERY_INFO_FLAGS,
                 G_N_ELEMENTS(FILE_QUERY_INFO_FLAGS));
    install_enum(L, "FileCreateFlags", FILE_CREATE_FLAGS,
                 G_N_ELEMENTS(FILE_CREATE_FLAGS));
    install_enum(L, "FileType", FILE_TYPES,
                 G_N_ELEMENTS(FILE_TYPES));
    install_enum(L, "FileAttributeType", FILE_ATTRIBUTE_TYPES,
                 G_N_ELEMENTS(FILE_ATTRIBUTE_TYPES));

    /* IOErrorEnum is the domain NAME string: gears/filesystem.lua:22 tests
     * `err.domain == Gio.IOErrorEnum`, i.e. the string "Gio.IOErrorEnum"
     * against the string push_error() emits. An install_enum() table would
     * never equal it. */
    lua_pushstring(L, "Gio.IOErrorEnum");
    lua_setfield(L, -2, "IOErrorEnum");

    /* Module-level constants menubar/utils.lua:370-371 reads directly. */
    lua_pushstring(L, "standard::name");
    lua_setfield(L, -2, "FILE_ATTRIBUTE_STANDARD_NAME");
    lua_pushstring(L, "standard::type");
    lua_setfield(L, -2, "FILE_ATTRIBUTE_STANDARD_TYPE");

    lua_pushcfunction(L, gio_bus_get);
    lua_setfield(L, -2, "bus_get");
    lua_pushcfunction(L, gio_bus_get_sync);
    lua_setfield(L, -2, "bus_get_sync");
    lua_pushcfunction(L, gio_bus_own_name);
    lua_setfield(L, -2, "bus_own_name");

    lua_newtable(L);
    lua_pushcfunction(L, gio_async_start);
    lua_setfield(L, -2, "start");
    lua_pushcfunction(L, gio_async_call);
    lua_setfield(L, -2, "call");
    lua_setfield(L, -2, "Async");

    return 1;
}
