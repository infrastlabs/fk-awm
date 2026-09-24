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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <cairo.h>

/* For the pango_cairo_* layout rendering bridge (cr:show_layout). Pulls in
 * pango.h as well. Only the two rendering entry points are used here. */
#include <pango/pangocairo.h>

/* Not all libc headers define M_PI under -std=gnu99 without _GNU_SOURCE. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------------------
 * NOTE ON THE CONTAINER'S CAIRO BUILD.
 *
 * cairo-version.h says 1.16.0 but the actual API is pre-1.14. The shim is
 * written against what is really in /usr/include/cairo/cairo.h +
 * /usr/lib/libcairo.a, verified with `nm -D /usr/lib/libcairo.so.2`. Absent:
 *   cairo_set_hairline / cairo_get_hairline     -> reimplemented via line_width
 *   cairo_get_font_size                         -> not implemented
 *   cairo_set/get_font_slant, cairo_set/get_font_weight
 *   cairo_glyph_show, cairo_pattern_finish
 *   cairo_pattern_get_surface_filter            -> cairo_pattern_get_filter
 *   cairo_pattern_get_target_format             -> the target's image format
 *   cairo_surface_get_width/get_height          -> the image surface variants
 *   cairo_surface_get_data                      -> the image surface variant
 *   cairo_region_clone, cairo_region_get_rectangle_list
 * Reimplemented on top of what IS there, one NOTE comment per site:
 *   cairo_pattern_get_target          -> cairo_pattern_get_surface (same object,
 *                                        the pre-1.16 name; borrowed pointer)
 *   cairo_pattern_get_opaque          -> solid alpha / per-stop alpha probe
 *   cairo_surface_get_description     -> composed from get_type + image getters
 *   cairo_push_group_to_image         -> push_group + pop_group + the group
 *                                        pattern's target surface
 *   cairo_surface_get_font_options    -> cairo_font_options_create(): a surface
 *                                        has no font options of its own here
 * Present but with the OLD signatures (the modern ones are commented below):
 *   cairo_get_dash(cr, dashes, *offset)                  [no *num_dashes]
 *   cairo_get_dash_count(cr)                             [count in, none out]
 *   cairo_pattern_get_color_stop_rgba(p, int index, *off, *r, *g, *b, *a)
 *   cairo_pattern_get_color_stop_count(p, int *count)    [count out, status]
 *   cairo_matrix_invert(m) -> void
 *   cairo_surface_set_user_data(s, key, data, void(*)(void*))
 *   cairo_get_font_options(cr, cairo_font_options_t *o)  [in/out buffer, not an
 *                                                          out-pointer; see
 *                                                          ctx_get_font_options]
 * Region is refcounted here (cairo 1.4+): cairo_region_destroy asserts on a
 * second call, so exactly one destroy per create/copy/create_rectangle.
 * ------------------------------------------------------------------------ */

/* Exported to luaa.c, which registers it in package.preload. */
int luaopen_lgishim_cairo(lua_State *L);

/* Provided by lgishim_pango.c; PangoLayout is opaque over there. The
 * pangocairo entry points themselves are named pango_cairo_* in this build
 * (with the second underscore), not pangocairo_* as the lgi typelib spells
 * them. */
PangoLayout *lgishim_pango_check_layout(lua_State *L, int idx);

enum {
    C_CTX = 1,
    C_SURFACE = 2,
    C_PATTERN = 3,
    C_FONT_OPTIONS = 4,
    C_MATRIX = 5,
    C_REGION = 6,
    N_CAIRO_CLASSES
};

static const char *CLS_NAMES[N_CAIRO_CLASSES] = {
    NULL,
    "lgishim.Context",
    "lgishim.Surface",
    "lgishim.Pattern",
    "lgishim.FontOptions",
    "lgishim.Matrix",
    "lgishim.Region",
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
static void d_region(void *p) { cairo_region_destroy((cairo_region_t *)p); }
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

/* enum_to that returns the NAME on the stack without pushing anything else;
 * used to build error messages (e.g. "failed for format ARGB32"). Leaves the
 * stack unchanged apart from the pushed string. */
static const char *
enum_to_name(const struct enumpair *t, size_t n, int v)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (t[i].value == v)
            return t[i].name;
    return "UNKNOWN";
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

static const struct enumpair PATTERN_TYPE[] = {
    { "SOLID", CAIRO_PATTERN_TYPE_SOLID },
    { "SURFACE", CAIRO_PATTERN_TYPE_SURFACE },
    { "LINEAR", CAIRO_PATTERN_TYPE_LINEAR },
    { "RADIAL", CAIRO_PATTERN_TYPE_RADIAL },
    { "MESH", CAIRO_PATTERN_TYPE_MESH },
    { "RASTER_SOURCE", CAIRO_PATTERN_TYPE_RASTER_SOURCE },
};

static const struct enumpair FILTER[] = {
    { "FAST", CAIRO_FILTER_FAST }, { "GOOD", CAIRO_FILTER_GOOD },
    { "BEST", CAIRO_FILTER_BEST }, { "NEAREST", CAIRO_FILTER_NEAREST },
    { "BILINEAR", CAIRO_FILTER_BILINEAR }, { "GAUSSIAN", CAIRO_FILTER_GAUSSIAN },
};

static const struct enumpair SURFACETYPE[] = {
    { "IMAGE", CAIRO_SURFACE_TYPE_IMAGE }, { "PDF", CAIRO_SURFACE_TYPE_PDF },
    { "PS", CAIRO_SURFACE_TYPE_PS }, { "XLIB", CAIRO_SURFACE_TYPE_XLIB },
    { "XCB", CAIRO_SURFACE_TYPE_XCB }, { "GLITZ", CAIRO_SURFACE_TYPE_GLITZ },
    { "QUARTZ", CAIRO_SURFACE_TYPE_QUARTZ },
    { "WIN32", CAIRO_SURFACE_TYPE_WIN32 }, { "BEOS", CAIRO_SURFACE_TYPE_BEOS },
    { "DIRECTFB", CAIRO_SURFACE_TYPE_DIRECTFB },
    { "SVG", CAIRO_SURFACE_TYPE_SVG }, { "OS2", CAIRO_SURFACE_TYPE_OS2 },
    { "WIN32_PRINTING", CAIRO_SURFACE_TYPE_WIN32_PRINTING },
    { "QUARTZ_IMAGE", CAIRO_SURFACE_TYPE_QUARTZ_IMAGE },
    { "SCRIPT", CAIRO_SURFACE_TYPE_SCRIPT }, { "QT", CAIRO_SURFACE_TYPE_QT },
    { "RECORDING", CAIRO_SURFACE_TYPE_RECORDING },
    { "VG", CAIRO_SURFACE_TYPE_VG }, { "GL", CAIRO_SURFACE_TYPE_GL },
    { "DRM", CAIRO_SURFACE_TYPE_DRM }, { "TEE", CAIRO_SURFACE_TYPE_TEE },
    { "XML", CAIRO_SURFACE_TYPE_XML }, { "SUBSURFACE", CAIRO_SURFACE_TYPE_SUBSURFACE },
    { "COGL", CAIRO_SURFACE_TYPE_COGL },
};

static const struct enumpair OVERLAP[] = {
    { "IN", CAIRO_REGION_OVERLAP_IN },
    { "OUT", CAIRO_REGION_OVERLAP_OUT },
    { "PART", CAIRO_REGION_OVERLAP_PART },
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
    lua_pop(L, 2);                             /* value, method table */

    /* Property setters, resolved so lib/ can call them in method form
     * exactly as lgi does: wibox/widget/imagebox.lua:328 does
     * cr:get_source():set_filter(...) and nothing writes that name into m.
     * Placed after m, so an explicit method in m always wins. */
    lua_getfield(L, -1, "p");                  /* [.., clsinfo, p] */
    lua_getfield(L, -1, k);                    /* [.., p, value] */
    if (lua_isfunction(L, -1)) {
        lua_replace(L, 3);
        lua_settop(L, 3);                      /* [obj, key, setter] */
        return 1;
    }
    lua_pop(L, 2);                             /* value, setter table */

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
    /* Stack on entry is [self = class table, args...].
     *
     * lua_call(L, nargs, n) calls the value at slot(top-nargs) and treats the
     * nargs values ABOVE it as the arguments, so the constructor has to be
     * moved in front of the arguments. The naive `lua_call(L, top-2, 1)`
     * leaves self in place and therefore calls arg1 as if it were the
     * function -- which is exactly why `cairo.Rectangle{...}` blew up with
     * "attempt to call a table value" while every 0-argument constructor
     * (`cairo.Matrix.new()`) looked fine. */
    lua_getfield(L, 1, "new");                /* [self, args..., new] */
    lua_insert(L, 2);                         /* [self, new, args...] */
    lua_remove(L, 1);                         /* [new, args...] */
    lua_call(L, lua_gettop(L) - 1, 1);        /* new(args...) */
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
 * Build just the Lua-facing class table for `cls` (the metatable and the
 * registry entry must already exist -- see class_setup()). Leaves the class
 * table on top of the stack.
 *
 * Shared by the "primary" names (Context, Surface, Pattern, ...) and the
 * derived ones (ImageSurface, RecordingSurface), which reuse the base
 * class's metatable so a cairo.Surface:is_type_of() check still passes for
 * any surface subtype -- lib/ relies on that (gears/surface.lua:54).
 */
static void
make_class_table(lua_State *L, int cls, const luaL_Reg *m)
{
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

/*
 * Build a class table for a "value" type whose instances are plain Lua tables,
 * not userdata (cairo.Rectangle{x,y,width,height}). No metatable, no registry
 * entry, no is_type_of -- there is nothing to type-check against. Leaves the
 * class table on top.
 */
static void
make_call_table(lua_State *L, const luaL_Reg *m)
{
    lua_newtable(L);
    luaL_setfuncs(L, m, 0);
    lua_newtable(L);
    lua_pushcclosure(L, class_call, 0);
    lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);
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

    /* Attach a static is_type_of on the class table. lua_getmetatable
     * retrieves the table this function just built from the registry; the
     * closure captures it so is_type_of can compare metatable identity.
     * lua_pushstring, NOT lua_pushlstring(..., -1): the third arg is size_t,
     * so -1 becomes SIZE_MAX and luaM_malloc_ dies with "block too big". */
    make_class_table(L, cls, m);
}

/* ================================================================ Part 2
 * Real cairo_* calls. Part 1 shipped the wrap/metatable/enum plumbing; this
 * section fills in Matrix, the Surface + Pattern constructors, and the
 * Context state/drawing/font methods. Property getters/setters and the
 * __newindex interception are Part 4.
 *
 * Return-value conventions are dictated by lib/, not by cairo:
 *   - text_extents() returns a TABLE of doubles, not an object
 *     (wibox/widget/piechart.lua:37 reads ext.width / ext.height)
 *   - get_matrix() returns a Matrix object with readable .xx/.yx/... fields
 *     and an :init() method (gears/matrix.lua:201,209)
 *   - set_dash() takes (table, num_dashes, offset) -- wibox/layout/grid.lua:1452
 *   - get_source() must return a Pattern userdata, since lib/ calls
 *     cr:get_source():set_filter(...) (wibox/widget/imagebox.lua:328)
 */

static void d_matrix(void *p) { free(p); }

static cairo_surface_t *
chk_surface(lua_State *L, int idx)
{
    return (cairo_surface_t *)chk(L, idx);
}

static cairo_pattern_t *
chk_pattern(lua_State *L, int idx)
{
    return (cairo_pattern_t *)chk(L, idx);
}

static cairo_matrix_t *
chk_matrix(lua_State *L, int idx)
{
    return (cairo_matrix_t *)chk(L, idx);
}

static cairo_region_t *
chk_region(lua_State *L, int idx)
{
    return (cairo_region_t *)chk(L, idx);
}

/* cairo_rectangle_int_t is a 4-field POD (placement.lua:818 hands it to
 * cairo.Region.create_rectangle through cairo.RectangleInt{...}). */
static void
read_rectint(lua_State *L, int idx, cairo_rectangle_int_t *r)
{
    r->x = r->y = r->width = r->height = 0;
    if (!lua_istable(L, idx))
        return;
    lua_getfield(L, idx, "x");       r->x = (int)luaL_optnumber(L, -1, 0);     lua_pop(L, 1);
    lua_getfield(L, idx, "y");       r->y = (int)luaL_optnumber(L, -1, 0);     lua_pop(L, 1);
    lua_getfield(L, idx, "width");   r->width   = (int)luaL_optnumber(L, -1, 0); lua_pop(L, 1);
    lua_getfield(L, idx, "height");  r->height  = (int)luaL_optnumber(L, -1, 0); lua_pop(L, 1);
}

static void
push_rectint(lua_State *L, const cairo_rectangle_int_t *r)
{
    lua_newtable(L);
    lua_pushinteger(L, r->x);       lua_setfield(L, -2, "x");
    lua_pushinteger(L, r->y);       lua_setfield(L, -2, "y");
    lua_pushinteger(L, r->width);   lua_setfield(L, -2, "width");
    lua_pushinteger(L, r->height);  lua_setfield(L, -2, "height");
}

/* ---- Matrix (value type exposed as a wrapped object) ------------------- */

/* Every "get a matrix" path must hand the wrapper a HEAP allocation: the
 * wrap owns p and d_matrix() frees it on GC. Pointing it at a stack local
 * gives free() a stack address, which silently corrupts the allocator and
 * makes every later read of the returned matrix garbage. */
static int push_matrix(lua_State *L, const cairo_matrix_t *m)
{
    cairo_matrix_t *copy;

    copy = (cairo_matrix_t *)malloc(sizeof(*copy));
    if (!copy)
        return luaL_error(L, "lgishim: out of memory");
    *copy = *m;
    return push_obj(L, copy, d_matrix, C_MATRIX);
}

static int matrix_new(lua_State *L)
{
    cairo_matrix_t *m;

    m = (cairo_matrix_t *)calloc(1, sizeof(cairo_matrix_t));
    if (!m)
        return luaL_error(L, "lgishim: out of memory");
    m->xx = 1.0;
    m->yy = 1.0;
    return push_obj(L, m, d_matrix, C_MATRIX);
}

static int matrix_init(lua_State *L)
{
    cairo_matrix_t *m = chk_matrix(L, 1);

    m->xx = luaL_checknumber(L, 2);
    m->yx = luaL_checknumber(L, 3);
    m->xy = luaL_checknumber(L, 4);
    m->yy = luaL_checknumber(L, 5);
    m->x0 = luaL_checknumber(L, 6);
    m->y0 = luaL_checknumber(L, 7);
    return 0;
}

static int matrix_mul(lua_State *L)
{
    cairo_matrix_t a = *chk_matrix(L, 1);
    cairo_matrix_t b = *chk_matrix(L, 2);
    cairo_matrix_t out;

    cairo_matrix_multiply(&out, &a, &b);
    return push_matrix(L, &out);
}

static int matrix_invert(lua_State *L)
{
    cairo_matrix_t a = *chk_matrix(L, 1);

    cairo_matrix_invert(&a);
    return push_matrix(L, &a);
}

/* lgi exposes identity/translate/scale as STATIC constructors on the Matrix
 * class (cairo.Matrix.identity()), so each returns a fresh matrix instead of
 * mutating an argument. Args start at slot 1: called on the class table there
 * is no self. An instance that wants to mutate in place still has :init(). */
static int matrix_identity(lua_State *L)
{
    cairo_matrix_t m;

    cairo_matrix_init_identity(&m);
    return push_matrix(L, &m);
}

static int matrix_translate(lua_State *L)
{
    cairo_matrix_t m;

    cairo_matrix_init_identity(&m);
    cairo_matrix_translate(&m, luaL_checknumber(L, 1), luaL_checknumber(L, 2));
    return push_matrix(L, &m);
}

static int matrix_scale(lua_State *L)
{
    cairo_matrix_t m;

    cairo_matrix_init_identity(&m);
    cairo_matrix_scale(&m, luaL_checknumber(L, 1), luaL_checknumber(L, 2));
    return push_matrix(L, &m);
}

/* Matrix field getters, installed through the property table so lib/ can read
 * mat.xx / mat.yx / ... exactly like it does for gears.matrix. */
static int mat_get_xx(lua_State *L) { lua_pushnumber(L, chk_matrix(L, 1)->xx); return 1; }
static int mat_get_yx(lua_State *L) { lua_pushnumber(L, chk_matrix(L, 1)->yx); return 1; }
static int mat_get_xy(lua_State *L) { lua_pushnumber(L, chk_matrix(L, 1)->xy); return 1; }
static int mat_get_yy(lua_State *L) { lua_pushnumber(L, chk_matrix(L, 1)->yy); return 1; }
static int mat_get_x0(lua_State *L) { lua_pushnumber(L, chk_matrix(L, 1)->x0); return 1; }
static int mat_get_y0(lua_State *L) { lua_pushnumber(L, chk_matrix(L, 1)->y0); return 1; }

static const luaL_Reg MATRIX_PG[] = {
    { "xx", mat_get_xx }, { "yx", mat_get_yx }, { "xy", mat_get_xy },
    { "yy", mat_get_yy }, { "x0", mat_get_x0 }, { "y0", mat_get_y0 },
    { NULL, NULL },
};

static const luaL_Reg MATRIX_M[] = {
    { "new", matrix_new },
    { "init", matrix_init },
    { "identity", matrix_identity },
    { "translate", matrix_translate },
    { "scale", matrix_scale },
    { "multiply", matrix_mul },
    { "invert", matrix_invert },
    { NULL, NULL },
};

/* ---- Surface constructors ---------------------------------------------- */

static int surface_wrap(lua_State *L)
{
    /* cairo.Surface(obj, copy) -- the generic converter used by
     * gears/surface.lua:72. nil -> nothing; a Surface userdata -> that
     * surface; a string -> a load failure is not our problem here. */
    if (lua_isnil(L, 1))
        return 0;
    if (lua_isuserdata(L, 1)) {
        lua_pushvalue(L, 1);
        return 1;
    }
    return luaL_error(L, "lgishim: Surface(): unsupported argument type %s",
                      luaL_typename(L, 1));
}

static int image_surface_new(lua_State *L)
{
    int fmt = enum_from(L, 1, FORMAT, sizeof(FORMAT) / sizeof(FORMAT[0]),
                        CAIRO_FORMAT_ARGB32);
    int w = (int)luaL_optnumber(L, 2, 0);
    int h = (int)luaL_optnumber(L, 3, 0);
    cairo_surface_t *s;

    s = cairo_image_surface_create(fmt, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS)
        return luaL_error(L, "lgishim: cairo_image_surface_create failed: %s",
                          enum_to_name(FORMAT, sizeof(FORMAT) / sizeof(FORMAT[0]),
                                        cairo_surface_status(s)));
    return push_obj(L, s, d_surface, C_SURFACE);
}

/* ImageSurface.create_for_data(pixels, fmt, w, h, stride) -- dbus.lua:117.
 * cairo holds the pointer, but the Lua string can be collected at any time,
 * so the bytes must be copied and freed via the surface's user-data hook. */
static cairo_user_data_key_t image_surface_data_key;

static void
free_image_surface_data(void *data)
{
    free(data);
}

static int image_surface_create_for_data(lua_State *L)
{
    size_t len;
    const char *pixels = luaL_checklstring(L, 1, &len);
    int fmt = enum_from(L, 2, FORMAT, sizeof(FORMAT) / sizeof(FORMAT[0]),
                        CAIRO_FORMAT_ARGB32);
    int w = (int)luaL_optnumber(L, 3, 0);
    int h = (int)luaL_optnumber(L, 4, 0);
    int stride = (int)luaL_optnumber(L, 5, 0);
    unsigned char *copy;
    cairo_surface_t *s;

    copy = (unsigned char *)malloc(len + 1);
    if (!copy)
        return luaL_error(L, "lgishim: out of memory");
    memcpy(copy, pixels, len + 1);
    s = cairo_image_surface_create_for_data(copy, fmt, w, h, stride);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        free(copy);
        return luaL_error(L, "lgishim: create_for_data failed: %s",
                          enum_to_name(FORMAT, sizeof(FORMAT) / sizeof(FORMAT[0]),
                                        cairo_surface_status(s)));
    }
    (void)cairo_surface_set_user_data(s, &image_surface_data_key, copy,
                                      free_image_surface_data);
    return push_obj(L, s, d_surface, C_SURFACE);
}

static int recording_surface_new(lua_State *L)
{
    int content = enum_from(L, 1, CONTENT, sizeof(CONTENT) / sizeof(CONTENT[0]),
                            CAIRO_CONTENT_COLOR_ALPHA);
    cairo_rectangle_t r;
    cairo_surface_t *s;

    r.x = r.y = r.width = r.height = 0.0;
    if (lua_istable(L, 2)) {
        /* lib/ passes cairo.Rectangle{x,y,width,height} (grid.lua:1396) or
         * cairo.RectangleInt{...} -- both are plain tables with the same keys. */
        lua_getfield(L, 2, "x");      r.x = luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, 2, "y");      r.y = luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, 2, "width");  r.width = luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, 2, "height"); r.height = luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
    }
    s = cairo_recording_surface_create(content, &r);
    return push_obj(L, s, d_surface, C_SURFACE);
}

static int surface_create_similar(lua_State *L)
{
    cairo_surface_t *src = chk_surface(L, 1);
    int content = enum_from(L, 2, CONTENT, sizeof(CONTENT) / sizeof(CONTENT[0]),
                            CAIRO_CONTENT_COLOR_ALPHA);
    int w = (int)luaL_optnumber(L, 3, 0);
    int h = (int)luaL_optnumber(L, 4, 0);

    return push_obj(L, cairo_surface_create_similar(src, content, w, h),
                    d_surface, C_SURFACE);
}

static int surface_get_content(lua_State *L)
{
    return enum_to(L, CONTENT, sizeof(CONTENT) / sizeof(CONTENT[0]),
                   cairo_surface_get_content(chk_surface(L, 1)));
}

/* cairo_surface_get_width / _get_height / _get_data do not exist in this
 * build, so the size and pixel-data getters go through the image variants.
 * Surface.width / .height are property getters (img.width), so they must
 * error rather than push nil here -- the smoke reads sim.width straight off a
 * create_similar result. */
static int surface_get_width(lua_State *L)
{
    cairo_surface_t *s = chk_surface(L, 1);

    if (cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE)
        return luaL_error(L, "lgishim: surface has no width (not an image surface)");
    lua_pushnumber(L, cairo_image_surface_get_width(s));
    return 1;
}

static int surface_get_height(lua_State *L)
{
    cairo_surface_t *s = chk_surface(L, 1);

    if (cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE)
        return luaL_error(L, "lgishim: surface has no height (not an image surface)");
    lua_pushnumber(L, cairo_image_surface_get_height(s));
    return 1;
}

static int surface_get_type(lua_State *L)
{
    return enum_to(L, SURFACETYPE, sizeof(SURFACETYPE) / sizeof(SURFACETYPE[0]),
                   cairo_surface_get_type(chk_surface(L, 1)));
}

static int surface_get_data(lua_State *L)
{
    cairo_surface_t *s = chk_surface(L, 1);
    cairo_format_t fmt = CAIRO_FORMAT_INVALID;
    unsigned char *data;
    int stride, w, h;

    if (cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE)
        return luaL_error(L,
            "lgishim: only image surfaces have a pixel buffer (no "
            "cairo_surface_get_data in this cairo)");
    data = cairo_image_surface_get_data(s);
    fmt = cairo_image_surface_get_format(s);
    stride = cairo_image_surface_get_stride(s);
    w = cairo_image_surface_get_width(s);
    h = cairo_image_surface_get_height(s);
    /* NULL before the first flush: cairo_image_surface_get_data hands back a
     * pointer with no length, so nothing but an explicit length tells us how
     * many bytes are live. Never pass -1 -- the size arg is size_t. */
    lua_pushlstring(L, data ? (const char *)data : "",
                    data ? (size_t)stride * (size_t)h : 0);
    lua_pushstring(L, enum_to_name(FORMAT, sizeof(FORMAT) / sizeof(FORMAT[0]), fmt));
    lua_pushinteger(L, stride);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 5;
}

/* cairo_surface_get_description (1.18+) is not in this build. Compose one from
 * the introspection calls that ARE, so a caller gets a real string instead of
 * an error; the image case carries the pixel layout lib/ actually wants. */
static int surface_get_description(lua_State *L)
{
    cairo_surface_t *s = chk_surface(L, 1);
    char buf[160];

    if (cairo_surface_get_type(s) == CAIRO_SURFACE_TYPE_IMAGE)
        snprintf(buf, sizeof(buf), "%s/%s/%dx%d",
            enum_to_name(SURFACETYPE, sizeof(SURFACETYPE) / sizeof(SURFACETYPE[0]),
                cairo_surface_get_type(s)),
            enum_to_name(FORMAT, sizeof(FORMAT) / sizeof(FORMAT[0]),
                cairo_image_surface_get_format(s)),
            cairo_image_surface_get_width(s), cairo_image_surface_get_height(s));
    else
        snprintf(buf, sizeof(buf), "%s",
            enum_to_name(SURFACETYPE, sizeof(SURFACETYPE) / sizeof(SURFACETYPE[0]),
                cairo_surface_get_type(s)));
    lua_pushstring(L, buf);
    return 1;
}

static int surface_mark_dirty(lua_State *L)
{ cairo_surface_mark_dirty(chk_surface(L, 1)); return 0; }

static int surface_mark_dirty_rectangle(lua_State *L)
{
    cairo_surface_mark_dirty_rectangle(chk_surface(L, 1),
        (int)luaL_checknumber(L, 2), (int)luaL_checknumber(L, 3),
        (int)luaL_checknumber(L, 4), (int)luaL_checknumber(L, 5));
    return 0;
}

/* Returns (x_offset, y_offset). Method form only: the __index path keeps just
 * the first value. */
static int surface_get_device_offset(lua_State *L)
{
    cairo_surface_t *s = chk_surface(L, 1);
    double x = 0, y = 0;

    cairo_surface_get_device_offset(s, &x, &y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

/* cairo_surface_get_font_options (1.16+) is not in this build, and a surface
 * has no font options of its own here -- they belong to the context (see
 * ctx_get_font_options). Hand back a fresh default set so callers get a real
 * FontOptions instead of an error. */
static int surface_get_font_options(lua_State *L)
{
    (void)chk_surface(L, 1);
    return push_obj(L, cairo_font_options_create(), d_font_options, C_FONT_OPTIONS);
}

static int surface_flush(lua_State *L) { cairo_surface_flush(chk_surface(L, 1)); return 0; }

static int surface_finish(lua_State *L) { cairo_surface_finish(chk_surface(L, 1)); return 0; }

static int surface_show_page(lua_State *L)
{ cairo_surface_show_page(chk_surface(L, 1)); return 0; }

static const luaL_Reg SURF_PG[] = {
    { "width",   surface_get_width },
    { "height",  surface_get_height },
    { "content", surface_get_content },
    { "type",    surface_get_type },
    { NULL, NULL },
};

static const luaL_Reg SURF_M[] = {
    { "new", surface_wrap },
    { "create_similar", surface_create_similar },
    { "get_width", surface_get_width },
    { "get_height", surface_get_height },
    { "get_content", surface_get_content },
    { "get_type", surface_get_type },
    { "get_data", surface_get_data },
    { "get_description", surface_get_description },
    { "get_device_offset", surface_get_device_offset },
    { "get_font_options", surface_get_font_options },
    { "flush", surface_flush },
    { "finish", surface_finish },
    { "mark_dirty", surface_mark_dirty },
    { "mark_dirty_rectangle", surface_mark_dirty_rectangle },
    { "show_page", surface_show_page },
    { NULL, NULL },
};

static const luaL_Reg IMAGE_M[] = {
    { "new", image_surface_new },
    { "create_for_data", image_surface_create_for_data },
    { NULL, NULL },
};

static const luaL_Reg RECORDING_M[] = {
    { "new", recording_surface_new },
    { NULL, NULL },
};

/* ---- Pattern constructors ---------------------------------------------- */

static int pattern_new(lua_State *L)
{
    /* Wrap an existing pattern without taking ownership: get_source() hands us
     * a ref we own, but create_* paths already own theirs. lib/ never wraps a
     * foreign pattern by hand, so this is a passthrough. */
    if (lua_isuserdata(L, 1)) {
        lua_pushvalue(L, 1);
        return 1;
    }
    return luaL_error(L, "lgishim: Pattern(): expected a pattern userdata");
}

static int pattern_create_solid(lua_State *L)
{
    return push_obj(L, cairo_pattern_create_rgba(
        luaL_optnumber(L, 1, 1), luaL_optnumber(L, 2, 1),
        luaL_optnumber(L, 3, 1), luaL_optnumber(L, 4, 1)),
        d_pattern, C_PATTERN);
}

static int pattern_create_for_surface(lua_State *L)
{
    return push_obj(L, cairo_pattern_create_for_surface(chk_surface(L, 1)),
                    d_pattern, C_PATTERN);
}

static int pattern_create_linear(lua_State *L)
{
    return push_obj(L, cairo_pattern_create_linear(
        luaL_checknumber(L, 1), luaL_checknumber(L, 2),
        luaL_checknumber(L, 3), luaL_checknumber(L, 4)),
        d_pattern, C_PATTERN);
}

static int pattern_create_radial(lua_State *L)
{
    return push_obj(L, cairo_pattern_create_radial(
        luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_optnumber(L, 3, 0),
        luaL_checknumber(L, 4), luaL_checknumber(L, 5), luaL_checknumber(L, 6)),
        d_pattern, C_PATTERN);
}

static int pattern_get_type(lua_State *L)
{
    return enum_to(L, PATTERN_TYPE, sizeof(PATTERN_TYPE) / sizeof(PATTERN_TYPE[0]),
                   cairo_pattern_get_type(chk_pattern(L, 1)));
}

static int pattern_set_filter(lua_State *L)
{
    cairo_pattern_set_filter(chk_pattern(L, 1),
        enum_from(L, 2, FILTER, sizeof(FILTER) / sizeof(FILTER[0]), CAIRO_FILTER_GOOD));
    return 0;
}

static int pattern_get_filter(lua_State *L)
{
    return enum_to(L, FILTER, sizeof(FILTER) / sizeof(FILTER[0]),
                   cairo_pattern_get_filter(chk_pattern(L, 1)));
}

static int pattern_set_extend(lua_State *L)
{
    cairo_pattern_set_extend(chk_pattern(L, 1),
        enum_from(L, 2, EXTEND, sizeof(EXTEND) / sizeof(EXTEND[0]), CAIRO_EXTEND_NONE));
    return 0;
}

static int pattern_get_extend(lua_State *L)
{
    return enum_to(L, EXTEND, sizeof(EXTEND) / sizeof(EXTEND[0]),
                   cairo_pattern_get_extend(chk_pattern(L, 1)));
}

static int pattern_set_matrix(lua_State *L)
{
    cairo_pattern_set_matrix(chk_pattern(L, 1), chk_matrix(L, 2));
    return 0;
}

static int pattern_get_matrix(lua_State *L)
{
    cairo_matrix_t m;

    cairo_pattern_get_matrix(chk_pattern(L, 1), &m);
    return push_matrix(L, &m);
}
static int pattern_get_rgba(lua_State *L)
{
    double r, g, b, a;
    cairo_pattern_t *p = chk_pattern(L, 1);

    if (cairo_pattern_get_rgba(p, &r, &g, &b, &a) != CAIRO_STATUS_SUCCESS)
        return luaL_error(L, "lgishim: pattern is not a solid color");
    lua_pushnumber(L, r); lua_pushnumber(L, g); lua_pushnumber(L, b); lua_pushnumber(L, a);
    return 4;
}

static int pattern_add_color_stop_rgba(lua_State *L)
{
    /* color.lua:163,170 call add_color_stop_rgba(point, color.parse_color(..))
     * -- the 4 return values of parse_color spread into slots 3..6. */
    cairo_pattern_add_color_stop_rgba(chk_pattern(L, 1),
        luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4),
        luaL_checknumber(L, 5), luaL_checknumber(L, 6));
    return 0;
}

static int pattern_set_color_stop_rgba(lua_State *L)
{
    return pattern_add_color_stop_rgba(L);
}

/* lgi puts the cairo status first, so these return it as return value 1 and
 * never raise -- gears/color.lua:334 and wibox/container/background.lua:36
 * both read them as `local _, _, _, _, _, alpha = pattern:get_color_stop_rgba(i)`
 * and expect a failure to surface as a bad status plus nils, not a thrown error.
 * This cairo uses the pre-1.14 form: (pattern, int index, *offset, ...);
 * modern cairo passes a double offset and no index. */
static int pattern_get_color_stop_rgba(lua_State *L)
{
    double off = 0, r = 0, g = 0, b = 0, a = 0;
    cairo_pattern_t *p = chk_pattern(L, 1);
    cairo_status_t st = cairo_pattern_get_color_stop_rgba(p,
            (int)luaL_optnumber(L, 2, 0), &off, &r, &g, &b, &a);

    enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]), st);
    lua_pushnumber(L, off);
    lua_pushnumber(L, r); lua_pushnumber(L, g);
    lua_pushnumber(L, b); lua_pushnumber(L, a);
    return 6;
}

/* Pre-1.14: the count is an out-param and the call returns a status. Keep the
 * lgi shape (status, count): gears/color.lua:325 reads `local _, stops = ..`
 * and wibox/container/background.lua:33 reads `local err, count = ..` and bails
 * on `err ~= "SUCCESS"`. */
static int pattern_get_color_stop_count(lua_State *L)
{
    cairo_pattern_t *p = chk_pattern(L, 1);
    int n = 0;
    cairo_status_t st = cairo_pattern_get_color_stop_count(p, &n);

    enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]), st);
    lua_pushinteger(L, n);
    return 2;
}

/* Push the pattern's target surface with no leading status. This cairo returns
 * the pattern's own surface pointer without adding a reference (probe:
 * cairo_probe_p3.c, "destroy result" aborts on
 * CAIRO_REFERENCE_COUNT_HAS_REFERENCE), so the wrapper must not destroy it --
 * the same trap as ctx_get_source / ctx_get_target. */
static int push_pattern_surface(lua_State *L, cairo_pattern_t *p)
{
    cairo_surface_t *s = NULL;

    if (cairo_pattern_get_surface(p, &s) != CAIRO_STATUS_SUCCESS || !s)
        return luaL_error(L, "lgishim: pattern is not a surface pattern");
    push_obj(L, s, d_noop, C_SURFACE);
    return 1;
}

/* Same surface, lgi's own method name. */
static int pattern_get_surface_pattern_surface(lua_State *L)
{
    return push_pattern_surface(L, chk_pattern(L, 1));
}

/* lgi returns only the surface from get_surface(), but this cairo reports a
 * status and lib/ unpacks one first (gears/color.lua:311,
 * wibox/container/background.lua:239). Keep that shape here and expose the
 * surface-only form under the names lgi gives it. */
static int pattern_get_surface(lua_State *L)
{
    cairo_pattern_t *p = chk_pattern(L, 1);
    cairo_surface_t *s = NULL;

    enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]),
            cairo_pattern_get_surface(p, &s));
    if (!s) {
        lua_pushnil(L);
        return 2;
    }
    push_obj(L, s, d_noop, C_SURFACE);
    return 2;
}

/* get_target_format has no direct entry point in this build; go through the
 * surface, which is what a surface pattern's target is. */
static int pattern_get_target_format(lua_State *L)
{
    cairo_pattern_t *p = chk_pattern(L, 1);
    cairo_surface_t *s = NULL;

    if (cairo_pattern_get_surface(p, &s) != CAIRO_STATUS_SUCCESS)
        return luaL_error(L, "lgishim: pattern is not a surface pattern");
    if (cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE)
        return luaL_error(L, "lgishim: pattern target is not an image surface");
    return enum_to(L, FORMAT, sizeof(FORMAT) / sizeof(FORMAT[0]),
                   cairo_image_surface_get_format(s));
}

/* cairo_pattern_get_target (1.16+) is not in this build. It returns the same
 * object as cairo_pattern_get_surface, so delegate to the shared helper. */
static int pattern_get_target(lua_State *L)
{
    return push_pattern_surface(L, chk_pattern(L, 1));
}

/* cairo_pattern_get_opaque (1.16+) is not in this build. Probe what can be
 * probed: a solid pattern is opaque iff its alpha is 1.0, a gradient iff every
 * stop is. A surface pattern's pixels are not inspected cheaply, so it is
 * reported as not opaque -- the conservative answer. */
static int pattern_get_opaque(lua_State *L)
{
    cairo_pattern_t *p = chk_pattern(L, 1);
    int type = cairo_pattern_get_type(p);
    double off, r, g, b, a;
    int n = 0;
    int i;

    switch (type) {
    case CAIRO_PATTERN_TYPE_SOLID:
        cairo_pattern_get_rgba(p, &r, &g, &b, &a);
        lua_pushboolean(L, a >= 1.0);
        return 1;
    case CAIRO_PATTERN_TYPE_LINEAR:
    case CAIRO_PATTERN_TYPE_RADIAL:
        if (cairo_pattern_get_color_stop_count(p, &n) != CAIRO_STATUS_SUCCESS)
            n = 0;
        for (i = 0; i < n; i++) {
            if (cairo_pattern_get_color_stop_rgba(p, i, &off, &r, &g, &b, &a)
                    != CAIRO_STATUS_SUCCESS || a < 1.0) {
                lua_pushboolean(L, 0);
                return 1;
            }
        }
        lua_pushboolean(L, n > 0);
        return 1;
    default:
        lua_pushboolean(L, 0);
        return 1;
    }
}

static const luaL_Reg PAT_PG[] = {
    { "type",  pattern_get_type },
    { "filter", pattern_get_filter },
    { "extend", pattern_get_extend },
    { "matrix", pattern_get_matrix },
    { NULL, NULL },
};

static const luaL_Reg PAT_P[] = {
    { "filter", pattern_set_filter },
    { "extend", pattern_set_extend },
    { "matrix", pattern_set_matrix },
    { NULL, NULL },
};

static const luaL_Reg PAT_M[] = {
    { "new", pattern_new },
    { "CreateSolid", pattern_create_solid },
    { "create_solid", pattern_create_solid },
    { "CreateRgba", pattern_create_solid },
    { "create_rgba", pattern_create_solid },
    { "CreateForColor", pattern_create_solid },
    { "CreateForSurface", pattern_create_for_surface },
    { "create_for_surface", pattern_create_for_surface },
    { "CreateLinear", pattern_create_linear },
    { "create_linear", pattern_create_linear },
    { "CreateRadial", pattern_create_radial },
    { "create_radial", pattern_create_radial },
    { "get_rgba", pattern_get_rgba },
    { "get_type", pattern_get_type },
    { "get_filter", pattern_get_filter },
    { "get_extend", pattern_get_extend },
    { "get_matrix", pattern_get_matrix },
    { "get_surface", pattern_get_surface },
    { "get_target", pattern_get_target },
    { "get_target_format", pattern_get_target_format },
    { "get_opaque", pattern_get_opaque },
    /* lgi keeps the surface-pattern accessors under their own names; on a
     * surface pattern cairo_pattern_get_filter already is the surface filter. */
    { "get_surface_pattern_surface", pattern_get_surface_pattern_surface },
    { "get_surface_pattern_filter", pattern_get_filter },
    { "set_filter", pattern_set_filter },
    { "set_extend", pattern_set_extend },
    { "set_matrix", pattern_set_matrix },
    { "set_color_stop_rgba", pattern_set_color_stop_rgba },
    { "add_color_stop_rgba", pattern_add_color_stop_rgba },
    { "get_color_stop_rgba", pattern_get_color_stop_rgba },
    { "get_color_stop", pattern_get_color_stop_rgba },
    { "get_color_stop_count", pattern_get_color_stop_count },
    { NULL, NULL },
};

/* ---- Context: state ---------------------------------------------------- */

static int ctx_new(lua_State *L)
{
    return push_obj(L, cairo_create(chk_surface(L, 1)), d_ctx, C_CTX);
}

static int ctx_save(lua_State *L) { cairo_save(chk(L, 1)); return 0; }
static int ctx_restore(lua_State *L) { cairo_restore(chk(L, 1)); return 0; }
static int ctx_translate(lua_State *L)
{ cairo_translate(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)); return 0; }
static int ctx_scale(lua_State *L)
{ cairo_scale(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)); return 0; }
static int ctx_rotate(lua_State *L)
{ cairo_rotate(chk(L, 1), luaL_checknumber(L, 2)); return 0; }

static int ctx_transform(lua_State *L)
{ cairo_transform(chk(L, 1), chk_matrix(L, 2)); return 0; }

static int ctx_set_matrix(lua_State *L)
{ cairo_set_matrix(chk(L, 1), chk_matrix(L, 2)); return 0; }

static int ctx_get_matrix(lua_State *L)
{
    cairo_matrix_t m;

    cairo_get_matrix(chk(L, 1), &m);
    return push_matrix(L, &m);
}

/* NOT cairo_set_matrix(cr, NULL): this cairo copies the matrix straight off
 * the pointer (cairo_matrix_copy has no NULL check), so NULL segfaults.
 * An explicit identity is correct in every cairo version. */
static int ctx_reset_matrix(lua_State *L)
{
    cairo_matrix_t m = { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 };

    cairo_set_matrix(chk(L, 1), &m);
    return 0;
}

static int ctx_set_operator(lua_State *L)
{
    cairo_set_operator(chk(L, 1),
        enum_from(L, 2, OPERATOR, sizeof(OPERATOR) / sizeof(OPERATOR[0]),
                  CAIRO_OPERATOR_OVER));
    return 0;
}

static int ctx_set_source(lua_State *L)
{
    cairo_set_source(chk(L, 1), chk_pattern(L, 2));
    return 0;
}

static int ctx_set_source_surface(lua_State *L)
{
    cairo_set_source_surface(chk(L, 1), chk_surface(L, 2),
                             luaL_optnumber(L, 3, 0), luaL_optnumber(L, 4, 0));
    return 0;
}

static int ctx_set_source_rgb(lua_State *L)
{
    cairo_set_source_rgb(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                         luaL_checknumber(L, 4));
    return 0;
}

static int ctx_set_source_rgba(lua_State *L)
{
    cairo_set_source_rgba(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                          luaL_checknumber(L, 4), luaL_checknumber(L, 5));
    return 0;
}

/* BORROWED pointer: this cairo build returns cr->source.pattern without adding
 * a reference, so d_pattern here would drop the refcount the context still
 * owns and abort in cairo_pattern_destroy. d_noop leaks nothing -- the context
 * releases the ref it took in set_source. (cairo_pop_group, by contrast, does
 * return a ref the caller must free.) */
static int ctx_get_source(lua_State *L)
{
    return push_obj(L, cairo_get_source(chk(L, 1)), d_noop, C_PATTERN);
}

static int ctx_set_line_width(lua_State *L)
{ cairo_set_line_width(chk(L, 1), luaL_checknumber(L, 2)); return 0; }

static int ctx_set_line_cap(lua_State *L)
{
    cairo_set_line_cap(chk(L, 1),
        enum_from(L, 2, LINECAP, sizeof(LINECAP) / sizeof(LINECAP[0]),
                  CAIRO_LINE_CAP_BUTT));
    return 0;
}

static int ctx_set_line_join(lua_State *L)
{
    cairo_set_line_join(chk(L, 1),
        enum_from(L, 2, LINEJOIN, sizeof(LINEJOIN) / sizeof(LINEJOIN[0]),
                  CAIRO_LINE_JOIN_MITER));
    return 0;
}

static int ctx_set_miter_limit(lua_State *L)
{ cairo_set_miter_limit(chk(L, 1), luaL_checknumber(L, 2)); return 0; }

/* lib/ calls this as cr:set_dash(table, #table, offset) -- grid.lua:1452. */
static int ctx_set_dash(lua_State *L)
{
    cairo_t *cr = chk(L, 1);
    double *dashes;
    int n, i;

    luaL_checktype(L, 2, LUA_TTABLE);
    n = lua_rawlen(L, 2);
    if (!lua_isnil(L, 3))
        n = (int)luaL_optinteger(L, 3, n);
    if (n <= 0) {
        cairo_set_dash(cr, NULL, 0, luaL_optnumber(L, 4, 0.0));
        return 0;
    }
    dashes = (double *)malloc(sizeof(double) * n);
    if (!dashes)
        return luaL_error(L, "lgishim: out of memory");
    for (i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        dashes[i] = luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }
    cairo_set_dash(cr, dashes, n, luaL_optnumber(L, 4, 0.0));
    free(dashes);
    return 0;
}

static int ctx_set_antialias(lua_State *L)
{
    cairo_set_antialias(chk(L, 1),
        enum_from(L, 2, ANTIALIAS, sizeof(ANTIALIAS) / sizeof(ANTIALIAS[0]),
                  CAIRO_ANTIALIAS_DEFAULT));
    return 0;
}

static int ctx_set_fill_rule(lua_State *L)
{
    cairo_set_fill_rule(chk(L, 1),
        enum_from(L, 2, FILLRULE, sizeof(FILLRULE) / sizeof(FILLRULE[0]),
                  CAIRO_FILL_RULE_WINDING));
    return 0;
}

static int ctx_set_hairline(lua_State *L)
{
    /* This cairo build lacks cairo_set_hairline; line_width == 0 is the
     * documented equivalent. */
    cairo_set_line_width(chk(L, 1), 0.0);
    return 0;
}

static int ctx_set_tolerance(lua_State *L)
{ cairo_set_tolerance(chk(L, 1), luaL_checknumber(L, 2)); return 0; }

static int ctx_get_current_point(lua_State *L)
{
    double x = 0, y = 0;

    cairo_get_current_point(chk(L, 1), &x, &y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

static int ctx_get_target(lua_State *L)
{
    /* BORROWED: this build returns cr->target with no added reference, so
     * d_surface would over-release and abort in cairo_surface_destroy. The
     * context holds the only ref it created, so d_noop is correct. */
    return push_obj(L, cairo_get_target(chk(L, 1)), d_noop, C_SURFACE);
}

/* Pre-1.14 signature: the second argument is an IN/OUT buffer, not an
 * out-pointer. The disassembly calls cairo_font_options_status(options) on the
 * argument, so it must be a live cairo_font_options_t that we then own. Handing
 * cairo our own object avoids taking ownership of one the font face keeps. */
static int ctx_get_font_options(lua_State *L)
{
    cairo_font_options_t *fo = cairo_font_options_create();

    cairo_get_font_options(chk(L, 1), fo);
    return push_obj(L, fo, d_font_options, C_FONT_OPTIONS);
}

static int ctx_clip(lua_State *L) { cairo_clip(chk(L, 1)); return 0; }

static int ctx_clip_preserve(lua_State *L) { cairo_clip_preserve(chk(L, 1)); return 0; }

static int ctx_reset_clip(lua_State *L) { cairo_reset_clip(chk(L, 1)); return 0; }

static int ctx_clip_extents(lua_State *L)
{
    double x0, y0, x1, y1;

    cairo_clip_extents(chk(L, 1), &x0, &y0, &x1, &y1);
    lua_pushnumber(L, x0); lua_pushnumber(L, y0);
    lua_pushnumber(L, x1); lua_pushnumber(L, y1);
    return 4;
}

static int ctx_push_group(lua_State *L) { cairo_push_group(chk(L, 1)); return 0; }

/* cairo_push_group_to_image (1.14+) is not in this build. Reconstruct its
 * result: push and immediately pop a real group, then hand back the surface the
 * group was rendered into -- the group pattern's target, which is a genuine
 * image surface of the target's dimensions (probe: cairo_probe_p3b.c case A).
 *
 * The group pattern owns the only reference to that surface -- destroying it
 * first makes the surface dangle (probe case B aborts in
 * cairo_surface_destroy) -- so take a reference BEFORE cairo_pattern_destroy.
 * Note cairo_get_target() is no help while a group is pushed: this cairo
 * returns the ORIGINAL target (probe case A, "distinct from base: 0").
 *
 * Approximation, not push semantics: the group is consumed, so drawing done on
 * the context AFTER this call does not land in the returned surface.
 *
 * Note: pattern_get_surface reports NULL for patterns that have no target,
 * e.g. solid and gradient patterns. */
static int ctx_push_group_to_image(lua_State *L)
{
    cairo_t *cr = chk(L, 1);
    cairo_pattern_t *p;
    cairo_surface_t *s = NULL;

    cairo_push_group(cr);
    p = cairo_pop_group(cr);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS || !p)
        return luaL_error(L, "lgishim: no group surface available in this cairo");
    if (cairo_pattern_get_surface(p, &s) != CAIRO_STATUS_SUCCESS || !s) {
        cairo_pattern_destroy(p);
        return luaL_error(L, "lgishim: group surface not reachable in this cairo");
    }
    cairo_surface_reference(s);
    cairo_pattern_destroy(p);
    return push_obj(L, s, d_surface, C_SURFACE);
}

static int ctx_push_group_with_content(lua_State *L)
{
    cairo_push_group_with_content(chk(L, 1),
        enum_from(L, 2, CONTENT, sizeof(CONTENT) / sizeof(CONTENT[0]),
                  CAIRO_CONTENT_COLOR_ALPHA));
    return 0;
}

static int ctx_pop_group(lua_State *L)
{
    return push_obj(L, cairo_pop_group(chk(L, 1)), d_pattern, C_PATTERN);
}

static int ctx_pop_group_to_source(lua_State *L)
{ cairo_pop_group_to_source(chk(L, 1)); return 0; }

static int ctx_new_path(lua_State *L) { cairo_new_path(chk(L, 1)); return 0; }

static int ctx_get_status(lua_State *L)
{
    /* lib/ compares this against the literal "SUCCESS" (drawable.lua:166,
     * background.lua:240), so it must round-trip as an enum NAME. */
    return enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]),
                   cairo_status(chk(L, 1)));
}

/* ---- Context: state getters (Part 4 territory, pulled in here because the
 * property tables below need them) ---------------------------------------- */

static int ctx_get_operator(lua_State *L)
{
    return enum_to(L, OPERATOR, sizeof(OPERATOR) / sizeof(OPERATOR[0]),
                   cairo_get_operator(chk(L, 1)));
}

static int ctx_get_dash(lua_State *L)
{
    /* Return (dashes_table, num_dashes, offset) so cr.dashes yields the same
     * shape cairo_get_dash produces. This cairo has the pre-1.14 split:
     * cairo_get_dash_count(cr) for the count, then cairo_get_dash with no
     * num_dashes out-param. */
    cairo_t *cr = chk(L, 1);
    double offset = 0.0;
    int n = cairo_get_dash_count(cr);
    double *dashes = NULL;
    int i;

    if (n > 0) {
        dashes = (double *)malloc(sizeof(double) * n);
        if (!dashes)
            return luaL_error(L, "lgishim: out of memory");
        cairo_get_dash(cr, dashes, &offset);
    }
    lua_newtable(L);
    for (i = 0; i < n; i++) {
        lua_pushnumber(L, dashes[i]);
        lua_rawseti(L, -2, i + 1);
    }
    if (dashes)
        free(dashes);
    lua_pushinteger(L, n);
    lua_pushnumber(L, offset);
    return 3;
}

static int ctx_get_line_width(lua_State *L)
{ lua_pushnumber(L, cairo_get_line_width(chk(L, 1))); return 1; }

static int ctx_get_line_cap(lua_State *L)
{
    return enum_to(L, LINECAP, sizeof(LINECAP) / sizeof(LINECAP[0]),
                   cairo_get_line_cap(chk(L, 1)));
}

static int ctx_get_line_join(lua_State *L)
{
    return enum_to(L, LINEJOIN, sizeof(LINEJOIN) / sizeof(LINEJOIN[0]),
                   cairo_get_line_join(chk(L, 1)));
}

static int ctx_get_antialias(lua_State *L)
{
    return enum_to(L, ANTIALIAS, sizeof(ANTIALIAS) / sizeof(ANTIALIAS[0]),
                   cairo_get_antialias(chk(L, 1)));
}

static int ctx_get_fill_rule(lua_State *L)
{
    return enum_to(L, FILLRULE, sizeof(FILLRULE) / sizeof(FILLRULE[0]),
                   cairo_get_fill_rule(chk(L, 1)));
}

static int ctx_get_hairline(lua_State *L)
{ lua_pushboolean(L, cairo_get_line_width(chk(L, 1)) == 0.0); return 1; }

static int ctx_get_tolerance(lua_State *L)
{ lua_pushnumber(L, cairo_get_tolerance(chk(L, 1))); return 1; }

/* No cairo_get_font_size / cairo_get_font_slant / cairo_get_font_weight in
 * this build, so those properties are not exposed (lib/ never reads them). */

/* ---- Context: drawing -------------------------------------------------- */

static int ctx_move_to(lua_State *L)
{ cairo_move_to(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)); return 0; }

static int ctx_rel_move_to(lua_State *L)
{ cairo_rel_move_to(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)); return 0; }

static int ctx_line_to(lua_State *L)
{ cairo_line_to(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)); return 0; }

static int ctx_rel_line_to(lua_State *L)
{ cairo_rel_line_to(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)); return 0; }

static int ctx_curve_to(lua_State *L)
{
    cairo_curve_to(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                    luaL_checknumber(L, 4), luaL_checknumber(L, 5),
                    luaL_checknumber(L, 6), luaL_checknumber(L, 7));
    return 0;
}

static int ctx_rel_curve_to(lua_State *L)
{
    cairo_rel_curve_to(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                       luaL_checknumber(L, 4), luaL_checknumber(L, 5),
                       luaL_checknumber(L, 6), luaL_checknumber(L, 7));
    return 0;
}

static int ctx_arc(lua_State *L)
{
    cairo_arc(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3),
              luaL_checknumber(L, 4), luaL_checknumber(L, 5), luaL_checknumber(L, 6));
    return 0;
}

static int ctx_arc_negative(lua_State *L)
{
    cairo_arc_negative(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                       luaL_checknumber(L, 4), luaL_checknumber(L, 5), luaL_checknumber(L, 6));
    return 0;
}

static int ctx_rectangle(lua_State *L)
{
    cairo_rectangle(chk(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                    luaL_checknumber(L, 4), luaL_checknumber(L, 5));
    return 0;
}

static int ctx_fill_rectangle(lua_State *L)
{
    cairo_t *cr = chk(L, 1);

    cairo_rectangle(cr, luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                    luaL_checknumber(L, 4), luaL_checknumber(L, 5));
    cairo_fill(cr);
    return 0;
}

static int ctx_stroke_rectangle(lua_State *L)
{
    cairo_t *cr = chk(L, 1);

    cairo_rectangle(cr, luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                    luaL_checknumber(L, 4), luaL_checknumber(L, 5));
    cairo_stroke(cr);
    return 0;
}

static int ctx_circle(lua_State *L)
{
    cairo_t *cr = chk(L, 1);

    cairo_new_sub_path(cr);
    cairo_arc(cr, luaL_checknumber(L, 2), luaL_checknumber(L, 3),
              luaL_checknumber(L, 4), 0.0, 2.0 * M_PI);
    cairo_close_path(cr);
    return 0;
}

static int ctx_close_path(lua_State *L) { cairo_close_path(chk(L, 1)); return 0; }

/* lib/ spells it `close_path` (surface.lua:187+); accept both. */
static int ctx_path_close(lua_State *L) { cairo_close_path(chk(L, 1)); return 0; }

static int ctx_fill(lua_State *L) { cairo_fill(chk(L, 1)); return 0; }

static int ctx_fill_preserve(lua_State *L) { cairo_fill_preserve(chk(L, 1)); return 0; }

static int ctx_stroke(lua_State *L) { cairo_stroke(chk(L, 1)); return 0; }

static int ctx_stroke_preserve(lua_State *L) { cairo_stroke_preserve(chk(L, 1)); return 0; }

static int ctx_paint(lua_State *L) { cairo_paint(chk(L, 1)); return 0; }

static int ctx_paint_with_alpha(lua_State *L)
{ cairo_paint_with_alpha(chk(L, 1), luaL_checknumber(L, 2)); return 0; }

static int ctx_mask(lua_State *L)
{ cairo_mask(chk(L, 1), chk_pattern(L, 2)); return 0; }

static int ctx_mask_surface(lua_State *L)
{
    cairo_mask_surface(chk(L, 1), chk_surface(L, 2),
                       luaL_optnumber(L, 3, 0), luaL_optnumber(L, 4, 0));
    return 0;
}

/* ---- Context: toy font API --------------------------------------------- */

static int ctx_select_font_face(lua_State *L)
{
    cairo_select_font_face(chk(L, 1), luaL_optstring(L, 2, "sans"),
        enum_from(L, 3, FONTSLANT, sizeof(FONTSLANT) / sizeof(FONTSLANT[0]),
                  CAIRO_FONT_SLANT_NORMAL),
        enum_from(L, 4, FONTWEIGHT, sizeof(FONTWEIGHT) / sizeof(FONTWEIGHT[0]),
                  CAIRO_FONT_WEIGHT_NORMAL));
    return 0;
}

static int ctx_set_font_size(lua_State *L)
{ cairo_set_font_size(chk(L, 1), luaL_checknumber(L, 2)); return 0; }

static int ctx_show_text(lua_State *L)
{ cairo_show_text(chk(L, 1), luaL_checkstring(L, 2)); return 0; }

/* ---- Pango bridge ------------------------------------------------------ */

/* textbox.lua:44/53 calls cr:update_layout(layout) then cr:show_layout(layout).
 * pango_cairo_show_layout() calls pango_cairo_update_layout() itself first, so
 * the lib/ pair is redundant but harmless -- keep both to match lgi. */
static int ctx_show_layout(lua_State *L)
{
    PangoLayout *layout = lgishim_pango_check_layout(L, 2);

    if (!layout)
        return luaL_error(L, "lgishim: show_layout: expected a Pango.Layout");
    pango_cairo_show_layout(chk(L, 1), layout);
    return 0;
}

static int ctx_update_layout(lua_State *L)
{
    PangoLayout *layout = lgishim_pango_check_layout(L, 2);

    if (!layout)
        return luaL_error(L, "lgishim: update_layout: expected a Pango.Layout");
    pango_cairo_update_layout(chk(L, 1), layout);
    return 0;
}

static int ctx_glyph_extents(lua_State *L)
{
    cairo_t *cr = chk(L, 1);
    cairo_glyph_t g = { luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4) };
    cairo_text_extents_t te;

    /* This build's cairo_glyph_extents returns void and fills a
     * cairo_text_extents_t (not a cairo_glyph_extents_t). */
    cairo_glyph_extents(cr, &g, 1, &te);
    lua_newtable(L);
    lua_pushnumber(L, te.x_bearing); lua_setfield(L, -2, "x_bearing");
    lua_pushnumber(L, te.y_bearing); lua_setfield(L, -2, "y_bearing");
    lua_pushnumber(L, te.width);     lua_setfield(L, -2, "width");
    lua_pushnumber(L, te.height);    lua_setfield(L, -2, "height");
    lua_pushnumber(L, te.x_advance); lua_setfield(L, -2, "x_advance");
    lua_pushnumber(L, te.y_advance); lua_setfield(L, -2, "y_advance");
    return 1;
}

/* Returns a TABLE, not an object: piechart.lua:37 reads ext.width / ext.height. */
static int ctx_text_extents(lua_State *L)
{
    cairo_t *cr = chk(L, 1);
    const char *s = luaL_optstring(L, 2, "");
    cairo_text_extents_t te;

    /* Returns void in this build, so no status check. */
    cairo_text_extents(cr, s, &te);
    lua_newtable(L);
    lua_pushnumber(L, te.x_bearing); lua_setfield(L, -2, "x_bearing");
    lua_pushnumber(L, te.y_bearing); lua_setfield(L, -2, "y_bearing");
    lua_pushnumber(L, te.width);     lua_setfield(L, -2, "width");
    lua_pushnumber(L, te.height);    lua_setfield(L, -2, "height");
    lua_pushnumber(L, te.x_advance); lua_setfield(L, -2, "x_advance");
    lua_pushnumber(L, te.y_advance); lua_setfield(L, -2, "y_advance");
    return 1;
}

static int ctx_font_extents(lua_State *L)
{
    cairo_font_extents_t fe;

    /* Returns void here. This build uses the PRE-1.8 struct
     * {ascent, descent, height, max_x_advance, max_y_advance} instead of the
     * modern {x_size, y_size, x_bearing, y_bearing, ascent, descent}. Both
     * names are exposed so lib/ code written against either API works. */
    cairo_font_extents(chk(L, 1), &fe);
    lua_newtable(L);
    lua_pushnumber(L, fe.ascent);        lua_setfield(L, -2, "ascent");
    lua_pushnumber(L, fe.descent);       lua_setfield(L, -2, "descent");
    lua_pushnumber(L, fe.height);        lua_setfield(L, -2, "height");
    lua_pushnumber(L, fe.max_x_advance); lua_setfield(L, -2, "max_x_advance");
    lua_pushnumber(L, fe.max_y_advance); lua_setfield(L, -2, "max_y_advance");
    lua_pushnumber(L, fe.max_x_advance); lua_setfield(L, -2, "x_size");
    lua_pushnumber(L, fe.height);        lua_setfield(L, -2, "y_size");
    lua_pushnumber(L, 0.0);              lua_setfield(L, -2, "x_bearing");
    lua_pushnumber(L, 0.0);              lua_setfield(L, -2, "y_bearing");
    return 1;
}

static const luaL_Reg CTX_PG[] = {
    { "matrix", ctx_get_matrix },
    { "operator", ctx_get_operator },
    { "status", ctx_get_status },
    { "dashes", ctx_get_dash },
    { "line_width", ctx_get_line_width },
    { "line_cap", ctx_get_line_cap },
    { "line_join", ctx_get_line_join },
    { "antialias", ctx_get_antialias },
    { "fill_rule", ctx_get_fill_rule },
    { "hairline", ctx_get_hairline },
    { "tolerance", ctx_get_tolerance },
    { NULL, NULL },
};

static const luaL_Reg CTX_P[] = {
    { "operator", ctx_set_operator },
    { "fill_rule", ctx_set_fill_rule },
    { "antialias", ctx_set_antialias },
    { "line_width", ctx_set_line_width },
    { "line_cap", ctx_set_line_cap },
    { "line_join", ctx_set_line_join },
    { "miter_limit", ctx_set_miter_limit },
    { "tolerance", ctx_set_tolerance },
    { "dashes", ctx_set_dash },
    { "font_size", ctx_set_font_size },
    { "matrix", ctx_set_matrix },
    { "source", ctx_set_source },
    { NULL, NULL },
};

static const luaL_Reg CTX_M[] = {
    { "new", ctx_new },
    { "create", ctx_new },
    /* state */
    { "save", ctx_save },
    { "restore", ctx_restore },
    { "translate", ctx_translate },
    { "scale", ctx_scale },
    { "rotate", ctx_rotate },
    { "transform", ctx_transform },
    { "set_matrix", ctx_set_matrix },
    { "get_matrix", ctx_get_matrix },
    { "reset_matrix", ctx_reset_matrix },
    { "set_operator", ctx_set_operator },
    { "get_operator", ctx_get_operator },
    { "set_source", ctx_set_source },
    { "set_source_surface", ctx_set_source_surface },
    { "set_source_rgb", ctx_set_source_rgb },
    { "set_source_rgba", ctx_set_source_rgba },
    { "get_source", ctx_get_source },
    { "get_target", ctx_get_target },
    { "get_font_options", ctx_get_font_options },
    { "set_line_width", ctx_set_line_width },
    { "set_line_cap", ctx_set_line_cap },
    { "set_line_join", ctx_set_line_join },
    { "set_miter_limit", ctx_set_miter_limit },
    { "get_dash", ctx_get_dash },
    { "set_dash", ctx_set_dash },
    { "set_antialias", ctx_set_antialias },
    { "set_fill_rule", ctx_set_fill_rule },
    { "set_hairline", ctx_set_hairline },
    { "set_tolerance", ctx_set_tolerance },
    { "clip", ctx_clip },
    { "clip_preserve", ctx_clip_preserve },
    { "reset_clip", ctx_reset_clip },
    { "clip_extents", ctx_clip_extents },
    { "new_path", ctx_new_path },
    { "push_group", ctx_push_group },
    { "push_group_with_content", ctx_push_group_with_content },
    { "push_group_to_image", ctx_push_group_to_image },
    { "pop_group", ctx_pop_group },
    { "pop_group_to_source", ctx_pop_group_to_source },
    { "get_current_point", ctx_get_current_point },
    { "get_status", ctx_get_status },
    /* drawing */
    { "move_to", ctx_move_to },
    { "rel_move_to", ctx_rel_move_to },
    { "line_to", ctx_line_to },
    { "rel_line_to", ctx_rel_line_to },
    { "curve_to", ctx_curve_to },
    { "rel_curve_to", ctx_rel_curve_to },
    { "arc", ctx_arc },
    { "arc_negative", ctx_arc_negative },
    { "rectangle", ctx_rectangle },
    { "fill_rectangle", ctx_fill_rectangle },
    { "stroke_rectangle", ctx_stroke_rectangle },
    { "circle", ctx_circle },
    { "close_path", ctx_close_path },
    { "path_close", ctx_path_close },
    { "fill", ctx_fill },
    { "fill_preserve", ctx_fill_preserve },
    { "stroke", ctx_stroke },
    { "stroke_preserve", ctx_stroke_preserve },
    { "paint", ctx_paint },
    { "paint_with_alpha", ctx_paint_with_alpha },
    { "mask", ctx_mask },
    { "mask_surface", ctx_mask_surface },
    /* toy font */
    { "select_font_face", ctx_select_font_face },
    { "set_font_size", ctx_set_font_size },
    { "show_text", ctx_show_text },
    { "glyph_extents", ctx_glyph_extents },
    { "text_extents", ctx_text_extents },
    { "get_text_extents", ctx_text_extents },
    { "font_extents", ctx_font_extents },
    { "get_font_extents", ctx_font_extents },
    { "show_layout", ctx_show_layout },
    { "update_layout", ctx_update_layout },
    { NULL, NULL },
};

/* ---- Rectangle / RectangleInt: plain tables, not userdata -------------- */

/* lib/ calls cairo.Rectangle{x,y,width,height} and cairo.RectangleInt{...} as
 * constructors (theme_assets.lua:203, drawable.lua:104, placement.lua:832).
 * They take a single table argument and return it unchanged: the fields are
 * already in the right shape for cairo_recording_surface_create /
 * cairo_region_union_rectangle. NOT an enum table -- an enum table under this
 * name would shadow the constructor. */
static int rectangle_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushvalue(L, 1);
    return 1;
}

static const luaL_Reg RECT_M[] = {
    { "new", rectangle_new },
    { NULL, NULL },
};

/* ---- Region ------------------------------------------------------------ */

/* drawable.lua:118/384 and hierarchy.lua:229 call cairo.Region.create();
 * placement.lua:832/835/838 call create_rectangle(RectangleInt), :intersect()
 * and :get_rectangle(0). Region is refcounted in this build, so exactly one
 * cairo_region_destroy per create/copy/create_rectangle -- a second destroy
 * asserts (probe: cairo_probe_p3.c). */
static int region_new(lua_State *L)
{
    return push_obj(L, cairo_region_create(), d_region, C_REGION);
}

static int region_create_rectangle(lua_State *L)
{
    cairo_rectangle_int_t r;

    read_rectint(L, 1, &r);
    return push_obj(L, cairo_region_create_rectangle(&r), d_region, C_REGION);
}

static int region_copy(lua_State *L)
{
    return push_obj(L, cairo_region_copy(chk_region(L, 1)), d_region, C_REGION);
}

static int region_intersect(lua_State *L)
{
    return enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]),
                   cairo_region_intersect(chk_region(L, 1), chk_region(L, 2)));
}

static int region_union(lua_State *L)
{
    return enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]),
                   cairo_region_union(chk_region(L, 1), chk_region(L, 2)));
}

static int region_subtract(lua_State *L)
{
    return enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]),
                   cairo_region_subtract(chk_region(L, 1), chk_region(L, 2)));
}

static int region_xor(lua_State *L)
{
    return enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]),
                   cairo_region_xor(chk_region(L, 1), chk_region(L, 2)));
}

static int region_intersect_rectangle(lua_State *L)
{
    cairo_rectangle_int_t r;

    read_rectint(L, 2, &r);
    return enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]),
                   cairo_region_intersect_rectangle(chk_region(L, 1), &r));
}

static int region_union_rectangle(lua_State *L)
{
    cairo_rectangle_int_t r;

    read_rectint(L, 2, &r);
    return enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]),
                   cairo_region_union_rectangle(chk_region(L, 1), &r));
}

static int region_subtract_rectangle(lua_State *L)
{
    cairo_rectangle_int_t r;

    read_rectint(L, 2, &r);
    return enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]),
                   cairo_region_subtract_rectangle(chk_region(L, 1), &r));
}

static int region_xor_rectangle(lua_State *L)
{
    cairo_rectangle_int_t r;

    read_rectint(L, 2, &r);
    return enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]),
                   cairo_region_xor_rectangle(chk_region(L, 1), &r));
}

/* Mutates in place; no return value (cairo_region_translate returns void).
 * Also exposed as a property so region.translate works like region:translate(). */
static int region_translate(lua_State *L)
{
    cairo_region_translate(chk_region(L, 1),
                           (int)luaL_optnumber(L, 2, 0),
                           (int)luaL_optnumber(L, 3, 0));
    return 0;
}

static int region_contains_point(lua_State *L)
{
    lua_pushboolean(L, cairo_region_contains_point(chk_region(L, 1),
        (int)luaL_checknumber(L, 2), (int)luaL_checknumber(L, 3)));
    return 1;
}

static int region_contains_rectangle(lua_State *L)
{
    cairo_rectangle_int_t r;

    read_rectint(L, 2, &r);
    return enum_to(L, OVERLAP, sizeof(OVERLAP) / sizeof(OVERLAP[0]),
                   cairo_region_contains_rectangle(chk_region(L, 1), &r));
}

static int region_is_empty(lua_State *L)
{
    lua_pushboolean(L, cairo_region_is_empty(chk_region(L, 1)));
    return 1;
}

static int region_num_rectangles(lua_State *L)
{
    lua_pushinteger(L, cairo_region_num_rectangles(chk_region(L, 1)));
    return 1;
}

/* placement.lua:838 reads region:get_rectangle(0).index. Returns a plain
 * table, matching how cairo.RectangleInt{...} is consumed everywhere else. */
static int region_get_rectangle(lua_State *L)
{
    cairo_rectangle_int_t r;

    cairo_region_get_rectangle(chk_region(L, 1),
                               (int)luaL_optnumber(L, 2, 0), &r);
    push_rectint(L, &r);
    return 1;
}

static int region_get_extents(lua_State *L)
{
    cairo_rectangle_int_t r;

    cairo_region_get_extents(chk_region(L, 1), &r);
    push_rectint(L, &r);
    return 1;
}

static int region_equal(lua_State *L)
{
    lua_pushboolean(L, cairo_region_equal(chk_region(L, 1), chk_region(L, 2)));
    return 1;
}

static int region_status(lua_State *L)
{
    return enum_to(L, STATUS, sizeof(STATUS) / sizeof(STATUS[0]),
                   cairo_region_status(chk_region(L, 1)));
}

 /* Names that exist in this table are resolved as properties BEFORE methods
  * (m_index checks g, then m), and a property getter is *called*, so a name in
  * here returns its value and cannot be used in method form:
  * region:num_rectangles() would read the integer and then call it, giving
  * "attempt to call a number value". wibox/drawable.lua:114 calls it as a
  * method, so it stays in REGION_M only. Same reason "is_empty" is absent --
  * wibox/drawable.lua:112 calls region:is_empty(). */
static const luaL_Reg REGION_PG[] = {
    { "empty", region_is_empty },
    { NULL, NULL },
};

static const luaL_Reg REGION_M[] = {
    { "new",                    region_new },
    { "create",                 region_new },
    { "create_rectangle",       region_create_rectangle },
    { "copy",                   region_copy },
    { "intersect",              region_intersect },
    { "union",                  region_union },
    { "subtract",               region_subtract },
    { "xor",                    region_xor },
    { "intersect_rectangle",    region_intersect_rectangle },
    { "union_rectangle",        region_union_rectangle },
    { "subtract_rectangle",     region_subtract_rectangle },
    { "xor_rectangle",          region_xor_rectangle },
    { "translate",              region_translate },
    { "contains_point",         region_contains_point },
    { "contains_rectangle",     region_contains_rectangle },
    { "is_empty",               region_is_empty },
    { "num_rectangles",         region_num_rectangles },
    { "get_rectangle",          region_get_rectangle },
    { "get_extents",            region_get_extents },
    { "equal",                  region_equal },
    { "status",                 region_status },
    { NULL, NULL },
};

/* ---- Format.stride_for_width: a function living on an enum table ------- */

/* naughty/dbus.lua:94 calls cairo.Format.stride_for_width(fmt, w). The Format
 * table is otherwise a bag of string constants, so this helper is attached
 * after install_enum(). */
static int format_stride_for_width(lua_State *L)
{
    int fmt = enum_from(L, 1, FORMAT, sizeof(FORMAT) / sizeof(FORMAT[0]),
                        CAIRO_FORMAT_ARGB32);
    int w = (int)luaL_optnumber(L, 2, 0);
    int stride = cairo_format_stride_for_width(fmt, w);

    if (stride < 0)
        return luaL_error(L, "lgishim: cairo_format_stride_for_width failed for format %s",
                          enum_to_name(FORMAT, sizeof(FORMAT) / sizeof(FORMAT[0]), fmt));
    lua_pushinteger(L, stride);
    return 1;
}

/* FontOptions.new() -> cairo_font_options_create(). lib/ never constructs one
 * itself; the constructor exists so the class is callable and so FontOptions
 * userdata coming back out of ctx_get_font_options() has a documented home. */
static int font_options_new(lua_State *L)
{
    return push_obj(L, cairo_font_options_create(), d_font_options, C_FONT_OPTIONS);
}

static const luaL_Reg FONTOPT_M[] = {
    { "new", font_options_new },
    { NULL, NULL },
};

/* ---------------------------------------------------- module initialiser */

int
luaopen_lgishim_cairo(lua_State *L)
{
    lua_newtable(L);

    class_setup(L, C_CTX,          CTX_M,     CTX_PG, CTX_P);
    lua_setfield(L, -2, "Context");
    class_setup(L, C_SURFACE,      SURF_M,    SURF_PG, NULL);
    lua_setfield(L, -2, "Surface");
    /* Derived surface names reuse C_SURFACE's metatable, so
     * cairo.Surface:is_type_of() accepts them. */
    make_class_table(L, C_SURFACE, IMAGE_M);
    lua_setfield(L, -2, "ImageSurface");
    make_class_table(L, C_SURFACE, RECORDING_M);
    lua_setfield(L, -2, "RecordingSurface");
    class_setup(L, C_PATTERN,      PAT_M,     PAT_PG, PAT_P);
    lua_setfield(L, -2, "Pattern");
    class_setup(L, C_FONT_OPTIONS, FONTOPT_M, NULL, NULL);
    lua_setfield(L, -2, "FontOptions");
    class_setup(L, C_MATRIX,       MATRIX_M,  MATRIX_PG, NULL);
    lua_setfield(L, -2, "Matrix");
    class_setup(L, C_REGION,       REGION_M,  REGION_PG, NULL);
    lua_setfield(L, -2, "Region");
    /* Rectangle / RectangleInt are constructors for PLAIN TABLES, not
     * userdata, so no metatable/registry entry -- just a __call table. */
    make_call_table(L, RECT_M);
    lua_setfield(L, -2, "Rectangle");
    make_call_table(L, RECT_M);
    lua_setfield(L, -2, "RectangleInt");

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
    install_enum(L, "PatternType", PATTERN_TYPE, sizeof(PATTERN_TYPE) / sizeof(PATTERN_TYPE[0]));
    install_enum(L, "Filter",   FILTER,    sizeof(FILTER)    / sizeof(FILTER[0]));
    install_enum(L, "SurfaceType", SURFACETYPE, sizeof(SURFACETYPE) / sizeof(SURFACETYPE[0]));
    install_enum(L, "Overlap",  OVERLAP,   sizeof(OVERLAP)   / sizeof(OVERLAP[0]));

    /* Format is an enum table, but lib/ also calls a function on it
     * (naughty/dbus.lua:94), so attach the helper after install_enum(). */
    lua_getfield(L, -1, "Format");
    lua_pushcfunction(L, format_stride_for_width);
    lua_setfield(L, -2, "stride_for_width");
    lua_pop(L, 1);

    return 1;
}
