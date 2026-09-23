/*
 * lgishim_cairo.c - Cairo Lua binding for the LGI shim.
 *
 * Replaces `lgi.cairo` by calling the real Cairo C API, so drawing calls
 * work without the gobject-introspection runtime. Mirrors the object
 * model of lgishim_pango.c: every Cairo object is a full userdata
 * holding { ptr, destroy, cls }; `._native` returns lightuserdata (the
 * only channel across the C boundary); every userdata has `__gc`,
 * calling destroy(); `finish()` releases explicitly.
 *
 * Part 1 (this file): foundation layer only.
 *   - wrap struct + destroy callbacks
 *   - metatable machinery (m_index / m_newindex / m_native / m_finish / m_gc)
 *   - enum helper + 12 string-valued enum constant tables
 *   - class_setup + module entry with 5 static constructor stubs
 * Parts 2..N (to come): real method bodies, Pango bridge, property getters.
 *
 * Six bug-avoidance rules (lgishim_pango.c §9.4 lessons):
 *   1. lua_setfield(REG_CLASSES) pops the table -- re-fetch with
 *      lua_getfield before writing clsinfo (see class_setup).
 *   2. luaL_setmetatable already pops the metatable -- no second
 *      lua_setmetatable.
 *   3. m_index uses lua_replace(L, 3) BEFORE lua_settop(L, 3).
 *   4. Borrowed references (surface:get_target() etc.) use d_noop.
 *   5. Matrix is a value type: return a plain Lua table, do not wrap.
 *   6. release() NULLs ptr and swaps destroy to d_noop -- no double-free.
 */

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <stdlib.h>
#include <string.h>

#include <cairo.h>

/* Exported to luaa.c, which registers it in package.preload. */
int luaopen_lgishim_cairo(lua_State *L);

enum {
    C_CTX = 1,
    C_SURFACE = 2,
    C_PATTERN = 3,
    C_FONT_OPTIONS = 4,
    N_CAIRO_CLASSES
};

static const char *CLS_NAMES[N_CAIRO_CLASSES] = {
    NULL,
    "lgishim.Context",
    "lgishim.Surface",
    "lgishim.Pattern",
    "lgishim.FontOptions",
};

#define REG_CLASSES "lgishim_classes"

typedef struct {
    void *p;
    void (*destroy)(void *);
    int cls;
} wrap;

/* --------------------------------------------------------------- helpers */

static void d_ctx(void *p) { cairo_destroy((cairo_t *)p); }
static void d_surface(void *p) { cairo_surface_destroy((cairo_surface_t *)p); }
static void d_pattern(void *p) { cairo_pattern_destroy((cairo_pattern_t *)p); }
static void d_font_options(void *p)
{ cairo_font_options_destroy((cairo_font_options_t *)p); }
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
    if (!w)
        luaL_error(L, "lgishim: expected object at arg %d, got %s",
                   idx, lua_isnil(L, idx) ? "nil" : luaL_typename(L, idx));
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

/* Enum tables are stored on the module table as STRING constants
 * (cairo.Operator.SOURCE == "SOURCE"). This matches lib/: cr.status is
 * compared against the literal "SUCCESS" (wibox/drawable.lua, wibox/
 * container/background.lua), and the same strings are passed straight back
 * to the setters, so enum_from()/enum_to() are string-based in both
 * directions -- identical to lgishim_pango.c. */

static int
enum_from(lua_State *L, int idx, const struct enumpair *t, size_t n, int defv)
{
    const char *s = luaL_optstring(L, idx, NULL);
    size_t i;
    if (s)
        for (i = 0; i < n; i++)
            if (!strcmp(s, t[i].name))
                return t[i].value;
    return defv;
}

static int
enum_to(lua_State *L, const struct enumpair *t, size_t n, int v)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (t[i].value == v) {
            lua_pushstring(L, t[i].name);
            return 1;
        }
    lua_pushnumber(L, v);
    return 1;
}

/* ------------------------------------------------------- enum constant tables */

static const struct enumpair OPERATOR[] = {
    { "CLEAR", CAIRO_OPERATOR_CLEAR }, { "SOURCE", CAIRO_OPERATOR_SOURCE },
    { "OVER", CAIRO_OPERATOR_OVER }, { "IN", CAIRO_OPERATOR_IN },
    { "OUT", CAIRO_OPERATOR_OUT }, { "DEST", CAIRO_OPERATOR_DEST },
    { "ATOP", CAIRO_OPERATOR_ATOP },
    { "DEST_OVER", CAIRO_OPERATOR_DEST_OVER },
    { "DEST_IN", CAIRO_OPERATOR_DEST_IN },
    { "DEST_OUT", CAIRO_OPERATOR_DEST_OUT },
    { "DEST_ATOP", CAIRO_OPERATOR_DEST_ATOP },
    { "XOR", CAIRO_OPERATOR_XOR },
    { "ADD", CAIRO_OPERATOR_ADD },
    { "SATURATE", CAIRO_OPERATOR_SATURATE },
    { "MULTIPLY", CAIRO_OPERATOR_MULTIPLY },
    { "SCREEN", CAIRO_OPERATOR_SCREEN },
    { "OVERLAY", CAIRO_OPERATOR_OVERLAY },
    { "DARKEN", CAIRO_OPERATOR_DARKEN },
    { "LIGHTEN", CAIRO_OPERATOR_LIGHTEN },
    { "COLOR_DODGE", CAIRO_OPERATOR_COLOR_DODGE },
    { "COLOR_BURN", CAIRO_OPERATOR_COLOR_BURN },
    { "HARD_LIGHT", CAIRO_OPERATOR_HARD_LIGHT },
    { "SOFT_LIGHT", CAIRO_OPERATOR_SOFT_LIGHT },
    { "DIFFERENCE", CAIRO_OPERATOR_DIFFERENCE },
    { "EXCLUSION", CAIRO_OPERATOR_EXCLUSION },
};

static const struct enumpair FORMAT[] = {
    { "INVALID", CAIRO_FORMAT_INVALID }, { "ARGB32", CAIRO_FORMAT_ARGB32 },
    { "RGB24", CAIRO_FORMAT_RGB24 }, { "A8", CAIRO_FORMAT_A8 },
    { "A1", CAIRO_FORMAT_A1 },
};

static const struct enumpair CONTENT[] = {
    { "COLOR", CAIRO_CONTENT_COLOR },
    { "ALPHA", CAIRO_CONTENT_ALPHA },
    { "COLOR_ALPHA", CAIRO_CONTENT_COLOR_ALPHA },
};

static const struct enumpair EXTEND[] = {
    { "NONE", CAIRO_EXTEND_NONE }, { "REPEAT", CAIRO_EXTEND_REPEAT },
    { "REFLECT", CAIRO_EXTEND_REFLECT }, { "PAD", CAIRO_EXTEND_PAD },
};

static const struct enumpair STATUS[] = {
    { "SUCCESS", CAIRO_STATUS_SUCCESS },
    { "NO_MEMORY", CAIRO_STATUS_NO_MEMORY },
    { "INVALID_RESTORE", CAIRO_STATUS_INVALID_RESTORE },
    { "INVALID_POP_GROUP", CAIRO_STATUS_INVALID_POP_GROUP },
    { "NO_CURRENT_POINT", CAIRO_STATUS_NO_CURRENT_POINT },
    { "INVALID_MATRIX", CAIRO_STATUS_INVALID_MATRIX },
    { "INVALID_PATH_DATA", CAIRO_STATUS_INVALID_PATH_DATA },
    { "READ_ERROR", CAIRO_STATUS_READ_ERROR },
    { "WRITE_ERROR", CAIRO_STATUS_WRITE_ERROR },
    { "SURFACE_FINISHED", CAIRO_STATUS_SURFACE_FINISHED },
    { "SURFACE_TYPE_MISMATCH", CAIRO_STATUS_SURFACE_TYPE_MISMATCH },
    { "PATTERN_TYPE_MISMATCH", CAIRO_STATUS_PATTERN_TYPE_MISMATCH },
    { "INVALID_CONTENT", CAIRO_STATUS_INVALID_CONTENT },
    { "INVALID_FORMAT", CAIRO_STATUS_INVALID_FORMAT },
    { "FILE_NOT_FOUND", CAIRO_STATUS_FILE_NOT_FOUND },
    { "INVALID_DASH", CAIRO_STATUS_INVALID_DASH },
    { "CLIP_NOT_REPRESENTABLE", CAIRO_STATUS_CLIP_NOT_REPRESENTABLE },
    { "INVALID_STRIDE", CAIRO_STATUS_INVALID_STRIDE },
    { "FONT_TYPE_MISMATCH", CAIRO_STATUS_FONT_TYPE_MISMATCH },
    { "DEVICE_ERROR", CAIRO_STATUS_DEVICE_ERROR },
};

static const struct enumpair ANTIALIAS[] = {
    { "DEFAULT", CAIRO_ANTIALIAS_DEFAULT }, { "NONE", CAIRO_ANTIALIAS_NONE },
    { "GRAY", CAIRO_ANTIALIAS_GRAY }, { "SUBPIXEL", CAIRO_ANTIALIAS_SUBPIXEL },
    { "GOOD", CAIRO_ANTIALIAS_GOOD }, { "BEST", CAIRO_ANTIALIAS_BEST },
};

static const struct enumpair LINECAP[] = {
    { "BUTT", CAIRO_LINE_CAP_BUTT }, { "ROUND", CAIRO_LINE_CAP_ROUND },
    { "SQUARE", CAIRO_LINE_CAP_SQUARE },
};

static const struct enumpair LINEJOIN[] = {
    { "MITER", CAIRO_LINE_JOIN_MITER }, { "ROUND", CAIRO_LINE_JOIN_ROUND },
    { "BEVEL", CAIRO_LINE_JOIN_BEVEL },
};

static const struct enumpair FILLRULE[] = {
    { "WINDING", CAIRO_FILL_RULE_WINDING }, { "EVEN_ODD", CAIRO_FILL_RULE_EVEN_ODD },
};

static const struct enumpair PATH[] = {
    { "MOVE_TO", CAIRO_PATH_MOVE_TO }, { "LINE_TO", CAIRO_PATH_LINE_TO },
    { "CURVE_TO", CAIRO_PATH_CURVE_TO }, { "CLOSE_PATH", CAIRO_PATH_CLOSE_PATH },
};

/* cairo.Rectangle / cairo.RectangleInt are *constructors* in lib/
 * (cairo.Rectangle{x,y,width,height}, cairo.RectangleInt{...}), not enums.
 * They return plain tables -- see Part 3. Deliberately NOT installed here: an
 * enum table under that name would shadow the constructor. */

static const struct enumpair FONTSLANT[] = {
    { "NORMAL", CAIRO_FONT_SLANT_NORMAL }, { "ITALIC", CAIRO_FONT_SLANT_ITALIC },
    { "OBLIQUE", CAIRO_FONT_SLANT_OBLIQUE },
};

static const struct enumpair FONTWEIGHT[] = {
    { "NORMAL", CAIRO_FONT_WEIGHT_NORMAL }, { "BOLD", CAIRO_FONT_WEIGHT_BOLD },
};

/* Install one enum table on the module (which must be at stack slot -1 at
 * call time). Leaves the module table on top. */
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

/* ---------------------------------------------------- shared protocol */

static int
m_index(lua_State *L)
{
    wrap *w = lua_touserdata(L, 1);
    const char *k;
    if (!w)
        return 0;
    k = luaL_checkstring(L, 2);

    /* Stack on entry: [obj, key]. After lookup, we want the return value at
     * slot 3 (return 1 gives it back to __index), so every lookup goes
     * [obj, key, REG_CLASSES, ...] and result gets swapped into slot 3 with
     * lua_replace before truncating with lua_settop(L, 3). */
    lua_getfield(L, LUA_REGISTRYINDEX, REG_CLASSES);
    lua_rawgeti(L, -1, w->cls);                /* [.., clsinfo] */

    /* Property getters: layout.text, color.red. */
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

    /* Helpers living on the metatable (_native, finish). __index is a
     * function, so a plain field there is never found otherwise. */
    luaL_getmetatable(L, CLS_NAMES[w->cls]);
    lua_getfield(L, -1, k);
    if (!lua_isnil(L, -1)) {
        lua_replace(L, 3);
        lua_settop(L, 3);                      /* [obj, key, value] */
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
    luaL_error(L, "lgishim: no such property '%s'", k);
    return 0;
}

static int
m_native(lua_State *L)
{
    wrap *w = getud(L, 1);
    if (!w)
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
class_call(lua_State *L)
{
    /* Stack on entry is [self = class table, args...]. */
    lua_getfield(L, 1, "new");                /* [self, args..., new] */
    lua_call(L, lua_gettop(L) - 2, 1);        /* new(args...) */
    return 1;
}

/* Type check. Registered as a closure over the class's OWN metatable table
 * (upvalue 1) and attached to the class table, so lib/ calls it in colon form:
 * `cairo.Surface:is_type_of(image)` -- the class table lands at slot 1 and the
 * object under test at slot 2.
 *
 * The discriminator is metatable *identity*, deliberately not w->cls: cairo and
 * pango each define the same `wrap` layout with their OWN cls enumeration
 * (cairo C_FONT_OPTIONS == 4, pango C_COLOR == 4), so an index/strcmp test lets
 * a Pango.Color pass as a cairo FontOptions. Each module registers its
 * metatables under its own name, and every userdata carries exactly one of them,
 * so comparing the two tables is sound across module boundaries.
 *
 * lib/ call sites: gears/color.lua:263,288, gears/surface.lua:54,67,
 * gears/wallpaper.lua:104,110, wibox/widget/imagebox.lua:427. */
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

/*
 * Build the metatable, the per-class registry entry and the Lua-facing class
 * table. Leaves the class table on top of the stack.
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

    lua_newtable(L);                           /* class table */
    luaL_setfuncs(L, m, 0);
    lua_newtable(L);                           /* class table metatable */
    lua_pushcclosure(L, class_call, 0);
    lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);                   /* assigns + pops the metatable */

    /* Attach a static is_type_of on the class table. lua_getmetatable
     * retrieves the table this function just built from the registry; the
     * closure captures it so is_type_of can compare metatable identity.
     * lua_pushstring, NOT lua_pushlstring(..., -1): the third arg is size_t,
     * so -1 becomes SIZE_MAX and luaM_malloc_ dies with "block too big". */
    luaL_getmetatable(L, CLS_NAMES[cls]);
    lua_pushcclosure(L, m_is_type_of, 1);
    lua_setfield(L, -2, "is_type_of");
}

/* ---------------------------------------------------- static constructor stubs */

/* All stub bodies are `return 0` placeholders. Parts 2..N fill in the
 * real cairo_*_create() calls. Kept here so class_setup has something
 * concrete to install and callers can already exercise the plumbing. */

static int stub_ctx_create(lua_State *L) { (void)L; return 0; }
static int stub_surface_create_image(lua_State *L) { (void)L; return 0; }
static int stub_matrix_create(lua_State *L) {
    /* Matrix is a value type. lib/ never wraps it, so we return a plain
     * table with default identity values. */
    (void)L;
    return 0;
}
static int stub_paint_create(lua_State *L) { (void)L; return 0; }
static int stub_pattern_create_linear(lua_State *L) { (void)L; return 0; }

/* Class method tables. In Part 1 these only carry the `new` stub + a
 * handful of `*create*` aliases so class_setup() has something to install
 * and `type(ctx) ~= nil` etc. already work. Real method bodies come later. */

static const luaL_Reg CTX_M[] = {
    { "new", stub_ctx_create },
    { "Create", stub_ctx_create },
    { NULL, NULL },
};

static const luaL_Reg SURF_M[] = {
    { "new", stub_surface_create_image },
    { "CreateImage", stub_surface_create_image },
    { NULL, NULL },
};

static const luaL_Reg PAT_M[] = {
    { "new", stub_pattern_create_linear },
    { "CreateLinear", stub_pattern_create_linear },
    { "CreateSolid", stub_paint_create },
    { NULL, NULL },
};

/* FontOptions has no public constructor from lib/; the class exists so
 * font_options_from_* bridges can return typed userdata. */
static const luaL_Reg FONTOPT_M[] = {
    { NULL, NULL },
};

/* ---------------------------------------------------- module initialiser */

int
luaopen_lgishim_cairo(lua_State *L)
{
    lua_newtable(L);

    class_setup(L, C_CTX,          CTX_M,     NULL, NULL);
    lua_setfield(L, -2, "Context");
    class_setup(L, C_SURFACE,      SURF_M,    NULL, NULL);
    lua_setfield(L, -2, "Surface");
    class_setup(L, C_PATTERN,      PAT_M,     NULL, NULL);
    lua_setfield(L, -2, "Pattern");
    class_setup(L, C_FONT_OPTIONS, FONTOPT_M, NULL, NULL);
    lua_setfield(L, -2, "FontOptions");

    install_enum(L, "Operator",   OPERATOR,  sizeof(OPERATOR)  / sizeof(OPERATOR[0]));
    install_enum(L, "Format",     FORMAT,    sizeof(FORMAT)    / sizeof(FORMAT[0]));
    install_enum(L, "Content",    CONTENT,   sizeof(CONTENT)   / sizeof(CONTENT[0]));
    install_enum(L, "Status",     STATUS,    sizeof(STATUS)    / sizeof(STATUS[0]));
    install_enum(L, "Antialias",  ANTIALIAS, sizeof(ANTIALIAS) / sizeof(ANTIALIAS[0]));
    install_enum(L, "LineCap",    LINECAP,   sizeof(LINECAP)   / sizeof(LINECAP[0]));
    install_enum(L, "LineJoin",   LINEJOIN,  sizeof(LINEJOIN)  / sizeof(LINEJOIN[0]));
    install_enum(L, "FillRule",   FILLRULE,  sizeof(FILLRULE)  / sizeof(FILLRULE[0]));
    install_enum(L, "Path",       PATH,      sizeof(PATH)      / sizeof(PATH[0]));
    install_enum(L, "Extend",     EXTEND,    sizeof(EXTEND)    / sizeof(EXTEND[0]));
    install_enum(L, "FontSlant",  FONTSLANT, sizeof(FONTSLANT) / sizeof(FONTSLANT[0]));
    install_enum(L, "FontWeight", FONTWEIGHT, sizeof(FONTWEIGHT) / sizeof(FONTWEIGHT[0]));

    return 1;
}
