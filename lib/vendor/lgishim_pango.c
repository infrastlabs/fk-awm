/*
 * lgishim_pango.c - Pango / PangoCairo Lua binding for the LGI shim.
 *
 * Replaces `lgi.Pango` / `lgi.PangoCairo` by calling the real Pango C API,
 * so text metrics stay correct without the gobject-introspection runtime.
 *
 * Object model (b16 hard constraints 1-4):
 *   - every Pango object is a full userdata holding { ptr, destroy, cls },
 *     so C code can recover the raw pointer with lua_touserdata();
 *   - `._native` returns lightuserdata (the raw pointer), the only channel
 *     across the C boundary;
 *   - every userdata has `__gc`, calling destroy() -> g_object_unref();
 *   - `finish()` releases explicitly and makes the slot inert.
 *
 * Pango 1.48 here vs the 1.50 API lib/ was written against:
 *   - pango_context_set_resolution() does not exist in 1.48 (resolution used
 *     to live on the font map), so set_resolution() goes through
 *     pango_cairo_font_map_set_resolution() + pango_context_changed();
 *   - enums travel as UPPERCASE variant names ("LEFT", "WORD_CHAR", ...)
 *     because wibox/widget/textbox.lua compares the getter result against the
 *     same strings it passes to the setter.
 *
 * Layout measurement API (get_extents/get_size/get_baseline/get_line_count)
 * added in Phase 2 Part 7. get_extents is the Pango-units counterpart of
 * get_pixel_extents; lib/ uses get_pixel_extents exclusively for text
 * measurement (textbox.lua:45,57,554).
 */

#include <lauxlib.h>
#include <lua.h>

#include <stdlib.h>
#include <string.h>

#include <glib-object.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>

typedef struct lua_State lua_State;

/* Exported to luaa.c, which registers both in package.preload. */
int luaopen_lgishim_pango(lua_State *L);
int luaopen_lgishim_pangocairo(lua_State *L);

/* Exported to lgishim_cairo.c for Context:show_layout()/update_layout(). */
typedef struct _PangoLayout PangoLayout;
PangoLayout *lgishim_pango_check_layout(lua_State *L, int idx);

enum {
    C_ATTRLIST = 1,
    C_LAYOUT,
    C_FONTDESC,
    C_COLOR,
    C_FONTMAP,
    C_CONTEXT,
    C_METRICS,
    N_CLASSES
};

static const char *CLS_NAMES[N_CLASSES] = {
    NULL,
    "lgishim_pango.AttrList",
    "lgishim_pango.Layout",
    "lgishim_pango.FontDescription",
    "lgishim_pango.Color",
    "lgishim_pango.FontMap",
    "lgishim_pango.Context",
    "lgishim_pango.FontMetrics",
};

/* Both this module and lgishim_cairo.c must own their own metatable names and
 * their own REG_CLASSES array. luaL_newmetatable() adopts an already-registered
 * name rather than creating one, so when pango's Context and cairo's Context
 * both claimed "lgishim.Context" -- cairo loads after pango, see lgishim.lua --
 * cairo's class_setup silently took over pango's metatable, overwriting its
 * __index/__newindex/__gc/finish and installing cairo's Context methods on top.
 * A pango_context_t asked for set_resolution through that metatable was looked
 * up as a cairo_t, and the method resolved to nil. m_index also indexes
 * REG_CLASSES by the per-module class number, so the numeric ids collide as
 * well (pango C_CONTEXT=6 vs cairo C_REGION=6); the registry key has to be
 * split for that reason too, not only the metatable names. */
#define REG_CLASSES "lgishim_pango_classes"

typedef struct {
    void *p;
    void (*destroy)(void *);
    int cls;
} wrap;

/* --------------------------------------------------------------- helpers */

static void d_unref(void *p)
{
    g_object_unref((GObject *)p);
}

static void d_gfree(void *p)
{
    g_free(p);
}

static void d_attrlist(void *p)
{
    pango_attr_list_unref((PangoAttrList *)p);
}

/* PangoFontDescription and PangoFontMetrics are GBoxed (independent
 * refcount), NOT GObject. Sending them to g_object_unref trips GLib's
 * `G_IS_OBJECT` assertion. Their own free/unref variants are required. */
static void d_fontdesc(void *p)
{
    pango_font_description_free((PangoFontDescription *)p);
}

static void d_metrics(void *p)
{
    /* Pango >= 1.50: pango_font_metrics_unref was renamed to
     * pango_font_metrics_free and metrics became copies, not refcounted.
     * On 1.48 the _unref variant is the correct call. */
#if PANGO_VERSION_MAJOR * 100 + PANGO_VERSION_MINOR >= 150
    pango_font_metrics_free((PangoFontMetrics *)p);
#else
    pango_font_metrics_unref((PangoFontMetrics *)p);
#endif
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
        luaL_error(L, "lgishim: expected object at arg %d, got %s",
                   idx, lua_isnil(L, idx) ? "nil" : luaL_typename(L, idx));
    return w->p;
}

/* Exported to lgishim_cairo.c, which implements cairo.Context:show_layout() and
 * update_layout() against a Pango.Layout built here. w->cls alone is not a
 * sound identifier: the per-module class numbers collide (pango C_LAYOUT=2 is
 * cairo C_SURFACE=2) even with the REG_CLASSES split, so a cairo.Surface would
 * otherwise pass as a layout. Compare metatable identity too -- the same idiom
 * as the cairo shim's is_type_of. Returns NULL instead of raising so the caller
 * can say what it was expecting. */
PangoLayout *
lgishim_pango_check_layout(lua_State *L, int idx)
{
    wrap *w = getud(L, idx);

    if (!w || w->cls != C_LAYOUT)
        return NULL;
    luaL_getmetatable(L, CLS_NAMES[C_LAYOUT]);
    lua_pushvalue(L, idx);
    lua_getmetatable(L, -1);                  /* [.., mt1, ud, udmt] */
    if (lua_isnil(L, -1) || !lua_rawequal(L, -1, -3)) {
        lua_settop(L, lua_gettop(L) - 3);
        return NULL;
    }
    lua_settop(L, lua_gettop(L) - 3);
    return (PangoLayout *)w->p;
}

static int
push_obj(lua_State *L, void *p, void (*destroy)(void *), int cls)
{
    wrap *w;

    /* Order matters: lua_setmetatable(L, -2) takes the metatable from the top
     * of the stack and assigns it to the value at -2, popping the top. So the
     * object must be created *before* luaL_setmetatable() pushes the table --
     * the old order pushed the metatable first, then the userdata, so the
     * userdata got popped and the caller was left holding the metatable. */
    w = (wrap *)lua_newuserdata(L, sizeof(wrap));
    w->p = p;
    w->destroy = destroy;
    w->cls = cls;
    /* luaL_setmetatable(l,n) == luaL_getmetatable(l,n), lua_setmetatable(l,-2).
     * The macro already sets the metatable on -2 (our userdata) and pops the
     * metatable, so adding another lua_setmetatable here would try to install
     * the userdata itself as someone else's metatable. */
    luaL_setmetatable(L, CLS_NAMES[cls]);
    return 1;
}

static void
push_rect(lua_State *L, const PangoRectangle *r)
{
    lua_newtable(L);
    lua_pushinteger(L, r->x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, r->y);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, r->width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, r->height);
    lua_setfield(L, -2, "height");
}

struct enumpair {
    const char *name;
    int value;
};

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

static const struct enumpair ALIGN[] = {
    { "LEFT", PANGO_ALIGN_LEFT },
    { "CENTER", PANGO_ALIGN_CENTER },
    { "RIGHT", PANGO_ALIGN_RIGHT },
};

static const struct enumpair ELLIPSIZE[] = {
    { "NONE", PANGO_ELLIPSIZE_NONE },
    { "START", PANGO_ELLIPSIZE_START },
    { "MIDDLE", PANGO_ELLIPSIZE_MIDDLE },
    { "END", PANGO_ELLIPSIZE_END },
};

static const struct enumpair WrAP[] = {
    { "WORD", PANGO_WRAP_WORD },
    { "CHAR", PANGO_WRAP_CHAR },
    { "WORD_CHAR", PANGO_WRAP_WORD_CHAR },
};

/* ------------------------------------------------------------ Layout */

static int l_new(lua_State *L)
{
    return push_obj(L, pango_layout_new(chk(L, 1)), d_unref, C_LAYOUT);
}

static int l_set_text(lua_State *L)
{
    pango_layout_set_text(chk(L, 1), luaL_checkstring(L, 2), -1);
    return 0;
}

static int l_get_text(lua_State *L)
{
    const char *t = pango_layout_get_text(chk(L, 1));
    lua_pushstring(L, t ? t : "");
    return 1;
}

static int l_set_attributes(lua_State *L)
{
    wrap *w = getud(L, 2);
    pango_layout_set_attributes(chk(L, 1),
                                w ? (PangoAttrList *)w->p : NULL);
    return 0;
}

static int l_get_attributes(lua_State *L)
{
    PangoAttrList *a = pango_layout_get_attributes(chk(L, 1));
    if (!a) {
        lua_pushnil(L);
        return 1;
    }
    /* Borrowed reference: the layout owns the list, so the wrapper must not
     * unref it, or a Lua-level collect() would underflow the refcount. */
    return push_obj(L, a, d_noop, C_ATTRLIST);
}

static int l_set_width(lua_State *L)
{
    pango_layout_set_width(chk(L, 1), (int)luaL_checkinteger(L, 2));
    return 0;
}

static int l_get_width(lua_State *L)
{
    lua_pushinteger(L, pango_layout_get_width(chk(L, 1)));
    return 1;
}

static int l_set_height(lua_State *L)
{
    pango_layout_set_height(chk(L, 1), (int)luaL_checkinteger(L, 2));
    return 0;
}

static int l_get_height(lua_State *L)
{
    lua_pushinteger(L, pango_layout_get_height(chk(L, 1)));
    return 1;
}

static int l_set_indent(lua_State *L)
{
    pango_layout_set_indent(chk(L, 1), (int)luaL_checkinteger(L, 2));
    return 0;
}

static int l_get_indent(lua_State *L)
{
    lua_pushinteger(L, pango_layout_get_indent(chk(L, 1)));
    return 1;
}

static int l_set_wrap(lua_State *L)
{
    pango_layout_set_wrap(
        chk(L, 1),
        enum_from(L, 2, WrAP, sizeof(WrAP) / sizeof(WrAP[0]),
                  PANGO_WRAP_WORD_CHAR));
    return 0;
}

static int l_get_wrap(lua_State *L)
{
    return enum_to(L, WrAP, sizeof(WrAP) / sizeof(WrAP[0]),
                   pango_layout_get_wrap(chk(L, 1)));
}

static int l_set_ellipsize(lua_State *L)
{
    pango_layout_set_ellipsize(
        chk(L, 1),
        enum_from(L, 2, ELLIPSIZE, sizeof(ELLIPSIZE) / sizeof(ELLIPSIZE[0]),
                  PANGO_ELLIPSIZE_END));
    return 0;
}

static int l_get_ellipsize(lua_State *L)
{
    return enum_to(L, ELLIPSIZE, sizeof(ELLIPSIZE) / sizeof(ELLIPSIZE[0]),
                   pango_layout_get_ellipsize(chk(L, 1)));
}

static int l_set_justify(lua_State *L)
{
    pango_layout_set_justify(chk(L, 1), lua_toboolean(L, 2));
    return 0;
}

static int l_get_justify(lua_State *L)
{
    lua_pushboolean(L, pango_layout_get_justify(chk(L, 1)));
    return 1;
}

static int l_set_alignment(lua_State *L)
{
    pango_layout_set_alignment(
        chk(L, 1),
        enum_from(L, 2, ALIGN, sizeof(ALIGN) / sizeof(ALIGN[0]),
                  PANGO_ALIGN_LEFT));
    return 0;
}

static int l_get_alignment(lua_State *L)
{
    return enum_to(L, ALIGN, sizeof(ALIGN) / sizeof(ALIGN[0]),
                   pango_layout_get_alignment(chk(L, 1)));
}

static int l_set_line_spacing(lua_State *L)
{
    pango_layout_set_line_spacing(chk(L, 1), (int)luaL_checkinteger(L, 2));
    return 0;
}

static int l_get_line_spacing(lua_State *L)
{
    lua_pushinteger(L, pango_layout_get_line_spacing(chk(L, 1)));
    return 1;
}

static int l_set_font_description(lua_State *L)
{
    pango_layout_set_font_description(chk(L, 1), chk(L, 2));
    return 0;
}

static int l_get_pixel_extents(lua_State *L)
{
    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(chk(L, 1), &ink, &logical);
    push_rect(L, &ink);
    push_rect(L, &logical);
    return 2;
}

/*
 * Pango-units counterpart of get_pixel_extents. Returns (ink, logical)
 * rectangles in Pango units (1/1024 px). On Pango >= 1.50 this delegates to
 * pango_layout_get_preferred_size(), which has the same signature.
 */
static int l_get_extents(lua_State *L)
{
    PangoRectangle ink, logical;
#if PANGO_VERSION_MAJOR * 100 + PANGO_VERSION_MINOR >= 150
    pango_layout_get_preferred_size(chk(L, 1), &ink, &logical);
#else
    pango_layout_get_extents(chk(L, 1), &ink, &logical);
#endif
    push_rect(L, &ink);
    push_rect(L, &logical);
    return 2;
}

/* Returns (width, height) in Pango units. */
static int l_get_size(lua_State *L)
{
    int width = 0, height = 0;
    pango_layout_get_size(chk(L, 1), &width, &height);
    lua_pushinteger(L, width);
    lua_pushinteger(L, height);
    return 2;
}

/* Returns the baseline from the top in Pango units, or 0 if no baseline. */
static int l_get_baseline(lua_State *L)
{
    lua_pushinteger(L, pango_layout_get_baseline(chk(L, 1)));
    return 1;
}

/* Returns the number of lines (paragraphs) in the layout. */
static int l_get_line_count(lua_State *L)
{
    lua_pushinteger(L, pango_layout_get_line_count(chk(L, 1)));
    return 1;
}

static int l_context_changed(lua_State *L)
{
    pango_layout_context_changed(chk(L, 1));
    return 0;
}

static const luaL_Reg LAYOUT_M[] = {
    { "new", l_new },
    { "set_text", l_set_text },
    { "set_attributes", l_set_attributes },
    { "set_width", l_set_width },
    { "set_height", l_set_height },
    { "set_indent", l_set_indent },
    { "set_wrap", l_set_wrap },
    { "set_ellipsize", l_set_ellipsize },
    { "set_justify", l_set_justify },
    { "set_alignment", l_set_alignment },
    { "set_line_spacing", l_set_line_spacing },
    { "set_font_description", l_set_font_description },
    { "get_text", l_get_text },
    { "get_attributes", l_get_attributes },
    { "get_width", l_get_width },
    { "get_height", l_get_height },
    { "get_indent", l_get_indent },
    { "get_wrap", l_get_wrap },
    { "get_ellipsize", l_get_ellipsize },
    { "get_justify", l_get_justify },
    { "get_alignment", l_get_alignment },
    { "get_line_spacing", l_get_line_spacing },
    { "get_pixel_extents", l_get_pixel_extents },
    { "get_extents", l_get_extents },
    { "get_size", l_get_size },
    { "get_baseline", l_get_baseline },
    { "get_line_count", l_get_line_count },
    { "context_changed", l_context_changed },
    { NULL, NULL },
};

static const luaL_Reg LAYOUT_P[] = {
    { "text", l_set_text },
    { "attributes", l_set_attributes },
    { "width", l_set_width },
    { "height", l_set_height },
    { NULL, NULL },
};

/*
 * Layout properties are read-write: textbox.lua assigns layout.text and then
 * reads it back (see set_text/get_text in that file), and compares
 * layout.attributes == nil. m_index tries the getter table first, so the
 * setter-only table above only handles assignments.
 */
static const luaL_Reg LAYOUT_PG[] = {
    { "text", l_get_text },
    { "attributes", l_get_attributes },
    { "width", l_get_width },
    { "height", l_get_height },
    { NULL, NULL },
};

/* ------------------------------------------------------ FontDescription */

static int fd_new(lua_State *L)
{
    return push_obj(L, pango_font_description_new(), d_fontdesc, C_FONTDESC);
}

static int fd_from_string(lua_State *L)
{
    return push_obj(L, pango_font_description_from_string(
                   luaL_checkstring(L, 1)), d_fontdesc, C_FONTDESC);
}

static int fd_merge(lua_State *L)
{
    pango_font_description_merge(chk(L, 1), chk(L, 2), lua_toboolean(L, 3));
    return 0;
}

static int fd_get_size(lua_State *L)
{
    lua_pushinteger(L, pango_font_description_get_size(chk(L, 1)));
    return 1;
}

static int fd_copy_static(lua_State *L)
{
    return push_obj(L, pango_font_description_copy_static(chk(L, 1)),
                     d_fontdesc, C_FONTDESC);
}

static int fd_to_string(lua_State *L)
{
    lua_pushstring(L, pango_font_description_to_string(chk(L, 1)));
    return 1;
}

static const luaL_Reg FONTSPEC_M[] = {
    { "new", fd_new },
    { "from_string", fd_from_string },
    { "merge", fd_merge },
    { "get_size", fd_get_size },
    { "copy_static", fd_copy_static },
    { "to_string", fd_to_string },
    { NULL, NULL },
};

/* --------------------------------------------------------------- Color */

static int co_new(lua_State *L)
{
    return push_obj(L, g_new(PangoColor, 1), d_gfree, C_COLOR);
}

static int co_parse(lua_State *L)
{
    lua_pushboolean(L, pango_color_parse(chk(L, 1), luaL_checkstring(L, 2)));
    return 1;
}

static const luaL_Reg COLOR_M[] = {
    { "new", co_new },
    { "parse", co_parse },
    { NULL, NULL },
};

/*
 * lib/gears/color.lua reads c.red / c.green / c.blue as *fields* (not as
 * :get_red() calls), so they have to resolve through m_index's property path
 * rather than the method table.
 *
 * PangoColor is a public struct on 1.48 (direct field access); on 1.50+ it
 * is opaque and pango_color_get_red() etc. are the only access path. The
 * #if branch picks the right one at compile time. `c.alpha` is intentionally
 * left unimplemented -- it reads as nil, which is what any missing property
 * does.
 */
static int
co_get_red(lua_State *L)
{
#if PANGO_VERSION_MAJOR * 100 + PANGO_VERSION_MINOR >= 150
    lua_pushinteger(L, pango_color_get_red((const PangoColor *)chk(L, 1)));
#else
    lua_pushinteger(L, ((PangoColor *)chk(L, 1))->red);
#endif
    return 1;
}

static int
co_get_green(lua_State *L)
{
#if PANGO_VERSION_MAJOR * 100 + PANGO_VERSION_MINOR >= 150
    lua_pushinteger(L, pango_color_get_green((const PangoColor *)chk(L, 1)));
#else
    lua_pushinteger(L, ((PangoColor *)chk(L, 1))->green);
#endif
    return 1;
}

static int
co_get_blue(lua_State *L)
{
#if PANGO_VERSION_MAJOR * 100 + PANGO_VERSION_MINOR >= 150
    lua_pushinteger(L, pango_color_get_blue((const PangoColor *)chk(L, 1)));
#else
    lua_pushinteger(L, ((PangoColor *)chk(L, 1))->blue);
#endif
    return 1;
}

static const luaL_Reg COLOR_PG[] = {
    { "red", co_get_red },
    { "green", co_get_green },
    { "blue", co_get_blue },
    { NULL, NULL },
};

/* -------------------------------------------------------------- FontMap */

static int fm_create_context(lua_State *L)
{
    return push_obj(L, pango_font_map_create_context(
                   (PangoFontMap *)chk(L, 1)), d_unref, C_CONTEXT);
}

static const luaL_Reg FONTMAP_M[] = {
    { "create_context", fm_create_context },
    { NULL, NULL },
};

/* -------------------------------------------------------------- Context */

static int cx_new(lua_State *L)
{
    /* pango_context_new() returns a context with no font map and no font
     * backend on this Pango 1.48 build. Any fontset load then goes through
     * pango_context_load_fontset() -> the deprecated stub
     * pango_font_map_load_fontset(NULL, ...), which only warns
     * "assertion 'fontmap != NULL' failed" and returns NULL, so a layout pass
     * retries it forever: a hot loop that also spews g_object_get_qdata
     * criticals and never returns. Tie the context to the default font map
     * instead -- what pango_cairo_create_layout() does -- and the same call
     * gives ink=37x12 for "Hello" with zero criticals. */
    return push_obj(L, pango_font_map_create_context(
                   (PangoFontMap *)pango_cairo_font_map_get_default()),
                    d_unref, C_CONTEXT);
}

static int cx_set_resolution(lua_State *L)
{
    PangoContext *c = chk(L, 1);
    /* Pango 1.50 API; on 1.48 resolution lives on the font map. */
    pango_cairo_font_map_set_resolution(
        (PangoCairoFontMap *)pango_cairo_font_map_get_default(),
        luaL_checknumber(L, 2));
    pango_context_changed(c);
    return 0;
}

static int cx_get_font_description(lua_State *L)
{
    /* The context owns the font description; the pointer is borrowed, so the
     * wrap must not free it. */
    PangoFontDescription *d = pango_context_get_font_description(chk(L, 1));
    return push_obj(L, d, d_noop, C_FONTDESC);
}

static int cx_get_metrics(lua_State *L)
{
    PangoFontMetrics *m = pango_context_get_metrics(
        chk(L, 1), chk(L, 2), NULL);
    return push_obj(L, m, d_metrics, C_METRICS);
}

static const luaL_Reg CONTEXT_M[] = {
    { "new", cx_new },
    { "set_resolution", cx_set_resolution },
    { "get_font_description", cx_get_font_description },
    { "get_metrics", cx_get_metrics },
    { NULL, NULL },
};

/* ---------------------------------------------------------- FontMetrics */

static int mt_get_ascent(lua_State *L)
{
    lua_pushinteger(L, pango_font_metrics_get_ascent(chk(L, 1)));
    return 1;
}

static int mt_get_descent(lua_State *L)
{
    lua_pushinteger(L, pango_font_metrics_get_descent(chk(L, 1)));
    return 1;
}

static const luaL_Reg METRICS_M[] = {
    { "get_ascent", mt_get_ascent },
    { "get_descent", mt_get_descent },
    { NULL, NULL },
};

/* ----------------------------------------------------------- AttrList */

static int al_new(lua_State *L)
{
    return push_obj(L, pango_attr_list_new(), d_attrlist, C_ATTRLIST);
}

static const luaL_Reg ATTRLIST_M[] = {
    { "new", al_new },
    { NULL, NULL },
};

/* ------------------------------------------------- Pango module statics */

static int
p_units_from_double(lua_State *L)
{
    lua_pushinteger(L, pango_units_from_double(luaL_checknumber(L, 1)));
    return 1;
}

/*
 * Returns (attrlist, text). On a parse error attrlist is nil and text holds
 * the error message - wibox/widget/textbox.lua reads `parsed.message` or
 * falls back to tostring(parsed), so a bare string works either way.
 */
static int
p_parse_markup(lua_State *L)
{
    const char *markup = luaL_checkstring(L, 1);
    int len = luaL_optinteger(L, 2, -1);
    PangoAttrList *list = NULL;
    char *text = NULL;
    gunichar accel = 0;
    GError *err = NULL;

    pango_parse_markup(markup, len, 0, &list, &text, &accel, &err);
    (void)accel;

    if (list)
        push_obj(L, list, d_attrlist, C_ATTRLIST);
    else
        lua_pushnil(L);

    if (err)
        lua_pushstring(L, err->message);
    else
        lua_pushstring(L, text ? text : "");
    if (text)
        g_free(text);
    if (err)
        g_error_free(err);
    return 2;
}

/* ----------------------------------------------------- shared protocol */

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
}

/* ---------------------------------------------------- module initialisers */

int
luaopen_lgishim_pango(lua_State *L)
{
    lua_newtable(L);
    class_setup(L, C_ATTRLIST, ATTRLIST_M, NULL, NULL);
    lua_setfield(L, -2, "AttrList");
    class_setup(L, C_LAYOUT, LAYOUT_M, LAYOUT_PG, LAYOUT_P);
    lua_setfield(L, -2, "Layout");
    class_setup(L, C_FONTDESC, FONTSPEC_M, NULL, NULL);
    lua_setfield(L, -2, "FontDescription");
    class_setup(L, C_COLOR, COLOR_M, COLOR_PG, NULL);
    lua_setfield(L, -2, "Color");
    class_setup(L, C_FONTMAP, FONTMAP_M, NULL, NULL);
    lua_setfield(L, -2, "FontMap");
    class_setup(L, C_CONTEXT, CONTEXT_M, NULL, NULL);
    lua_setfield(L, -2, "Context");
    class_setup(L, C_METRICS, METRICS_M, NULL, NULL);
    lua_setfield(L, -2, "FontMetrics");

    lua_pushinteger(L, PANGO_SCALE);
    lua_setfield(L, -2, "SCALE");
    lua_pushcfunction(L, p_units_from_double);
    lua_setfield(L, -2, "units_from_double");
    lua_pushcfunction(L, p_parse_markup);
    lua_setfield(L, -2, "parse_markup");
    return 1;
}

static int
pc_font_map_get_default(lua_State *L)
{
    return push_obj(L, pango_cairo_font_map_get_default(), d_unref,
                    C_FONTMAP);
}

int
luaopen_lgishim_pangocairo(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, pc_font_map_get_default);
    lua_setfield(L, -2, "font_map_get_default");
    return 1;
}
