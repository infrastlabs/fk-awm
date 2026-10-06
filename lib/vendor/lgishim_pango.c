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
 *     same strings it passes to the setter. Weight/Style/Stretch/Underline/
 *     Variant/Gravity/GravityHint are the exception and are installed as their
 *     real numeric values, because Awesome and theme code compares them
 *     arithmetically (beautiful: weight > Pango.Weight.BOLD).
 *   - pango_attr_list_insert()/insert_before()/change() on 1.48 store the
 *     attribute pointer without ref'ing it, and change() destroys it outright
 *     when the range is empty, so the shim inserts a copy.
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
    C_ATTRIBUTE,
    C_TABARRAY,
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
    "lgishim_pango.Attribute",
    "lgishim_pango.TabArray",
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

/* pango_attribute_destroy() is the only way out: PangoAttribute is not a
 * GBoxed and has no pango_attribute_free/unref variant. */
static void d_attribute(void *p)
{
    pango_attribute_destroy((PangoAttribute *)p);
}

static void d_tabarray(void *p)
{
    pango_tab_array_free((PangoTabArray *)p);
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

/* chk() only proves the argument is a userdata, so two different object kinds
 * can be cast into each other silently -- an AttrList passed where an
 * Attribute is expected dereferences pango_attr_list's internals as an
 * attribute and corrupts memory. chk_cls() adds the class check. */
static void *
chk_cls(lua_State *L, int idx, int cls)
{
    wrap *w = getud(L, idx);
    if (!w || w->cls != cls)
        luaL_error(L, "lgishim: expected %s at arg %d, got %s",
                   CLS_NAMES[cls], idx, lua_typename(L, idx));
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
    if (s) {
        for (i = 0; i < n; i++)
            if (!strcmp(s, t[i].name))
                return t[i].value;
        /* A missing argument is the documented "use default" path; a supplied
         * name that matches nothing is a typo and must not fall back to defv. */
        return luaL_error(L, "lgishim.Pango: unknown %s value", s);
    }
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

/* Numeric counterpart of enum_from(): the Weight/Style/Stretch/Underline/
 * Variant/Gravity/GravityHint tables are installed with integer values, so
 * their setters have to accept numbers as well as names. A missing argument
 * still means "use the default", exactly as enum_from() does. */
static int
enum_num_from(lua_State *L, int idx, const struct enumpair *t, size_t n,
              int defv)
{
    size_t i;
    if (lua_isnoneornil(L, idx))
        return defv;
    if (lua_isnumber(L, idx))
        return (int)luaL_checkinteger(L, idx);
    for (i = 0; i < n; i++)
        if (!strcmp(luaL_checkstring(L, idx), t[i].name))
            return t[i].value;
    return luaL_error(L, "lgishim.Pango: unknown enum value %q",
                      (const char *)lua_tostring(L, idx));
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

/* Numeric enum tables. install_enum_num() publishes the real integer values,
 * which is what lgi's introspected enums look like from Lua; the pa_* and l_*
 * setters go through enum_num_from() and take either spelling.
 *
 * Values are read from the 1.48.10 headers, never hard-coded -- BOOK is 380,
 * which is what a stale "400" assumption would get wrong. */
static const struct enumpair WEIGHT[] = {
    { "THIN", PANGO_WEIGHT_THIN },
    { "ULTRALIGHT", PANGO_WEIGHT_ULTRALIGHT },
    { "LIGHT", PANGO_WEIGHT_LIGHT },
    { "SEMILIGHT", PANGO_WEIGHT_SEMILIGHT },
    { "BOOK", PANGO_WEIGHT_BOOK },
    { "NORMAL", PANGO_WEIGHT_NORMAL },
    { "MEDIUM", PANGO_WEIGHT_MEDIUM },
    { "SEMIBOLD", PANGO_WEIGHT_SEMIBOLD },
    { "BOLD", PANGO_WEIGHT_BOLD },
    { "ULTRABOLD", PANGO_WEIGHT_ULTRABOLD },
    { "HEAVY", PANGO_WEIGHT_HEAVY },
    { "ULTRAHEAVY", PANGO_WEIGHT_ULTRAHEAVY },
};

static const struct enumpair STYLE[] = {
    { "NORMAL", PANGO_STYLE_NORMAL },
    { "ITALIC", PANGO_STYLE_ITALIC },
    { "OBLIQUE", PANGO_STYLE_OBLIQUE },
};

static const struct enumpair STRETCH[] = {
    { "ULTRA_CONDENSED", PANGO_STRETCH_ULTRA_CONDENSED },
    { "EXTRA_CONDENSED", PANGO_STRETCH_EXTRA_CONDENSED },
    { "CONDENSED", PANGO_STRETCH_CONDENSED },
    { "SEMI_CONDENSED", PANGO_STRETCH_SEMI_CONDENSED },
    { "NORMAL", PANGO_STRETCH_NORMAL },
    { "SEMI_EXPANDED", PANGO_STRETCH_SEMI_EXPANDED },
    { "EXPANDED", PANGO_STRETCH_EXPANDED },
    { "EXTRA_EXPANDED", PANGO_STRETCH_EXTRA_EXPANDED },
    { "ULTRA_EXPANDED", PANGO_STRETCH_ULTRA_EXPANDED },
};

static const struct enumpair UNDERLINE[] = {
    { "NONE", PANGO_UNDERLINE_NONE },
    { "SINGLE", PANGO_UNDERLINE_SINGLE },
    { "DOUBLE", PANGO_UNDERLINE_DOUBLE },
    { "LOW", PANGO_UNDERLINE_LOW },
    { "ERROR", PANGO_UNDERLINE_ERROR },
    { "ERROR_LINE", PANGO_UNDERLINE_ERROR_LINE },
    { "SINGLE_LINE", PANGO_UNDERLINE_SINGLE_LINE },
    { "DOUBLE_LINE", PANGO_UNDERLINE_DOUBLE_LINE },
};

/* 1.48 has no PETITE_CAPS/TITLE_CAPS/UNICASE member; adding them would mean
 * inventing values that pango itself does not understand. */
static const struct enumpair VARIANT[] = {
    { "NORMAL", PANGO_VARIANT_NORMAL },
    { "SMALL_CAPS", PANGO_VARIANT_SMALL_CAPS },
};

static const struct enumpair GRAVITY[] = {
    { "NORTH", PANGO_GRAVITY_NORTH },
    { "WEST", PANGO_GRAVITY_WEST },
    { "SOUTH", PANGO_GRAVITY_SOUTH },
    { "EAST", PANGO_GRAVITY_EAST },
    { "AUTO", PANGO_GRAVITY_AUTO },
};

static const struct enumpair GRAVITYHINT[] = {
    { "NATURAL", PANGO_GRAVITY_HINT_NATURAL },
    { "LINE", PANGO_GRAVITY_HINT_LINE },
    { "STRONG", PANGO_GRAVITY_HINT_STRONG },
};

/* Install one enum table on the module, which must sit at stack slot -1 at
 * call time. Values are pushed as integers -- that is how lgi's introspected
 * enum types cross into Lua, and it is what makes arithmetic comparisons work.
 * The ALIGN/ELLIPSIZE/WrAP tables above stay out of this: they only feed
 * enum_from()/enum_to() and are string-valued by design, so no string-flavoured
 * install_enum() is needed here. Leaves the module table on top. */
static void
install_enum_num(lua_State *L, const char *fieldname,
                 const struct enumpair *t, size_t n)
{
    size_t i;
    lua_newtable(L);
    for (i = 0; i < n; i++) {
        lua_pushinteger(L, t[i].value);
        lua_setfield(L, -2, t[i].name);
    }
    lua_setfield(L, -2, fieldname);
}

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

/*
 * Inter-line spacing in Pango units. lib/awful/widget/layoutlist.lua:344
 * calls layout:set_spacing() straight off the object, which died with
 * "attempt to call a nil value" while this was missing.
 *
 * 1.48's setter takes a double but the getter returns an int, so a fractional
 * value is stored and then truncated on read-back -- pass luaL_checknumber, not
 * luaL_checkinteger, or callers passing e.g. 3.5 die with "number has no
 * integer representation". The int cast would have silently truncated too.
 */
 static int l_set_spacing(lua_State *L)
 {
    pango_layout_set_spacing(chk(L, 1), luaL_checknumber(L, 2));
    return 0;
 }

static int l_get_spacing(lua_State *L)
{
    lua_pushinteger(L, pango_layout_get_spacing(chk(L, 1)));
    return 1;
}

/*
 * 1.48's pango_layout_set_markup() takes no GError: on a parse failure
 * pango_layout_set_markup_with_accel() g_warning()s and returns with the
 * layout's text and attributes left exactly as they were, so this can never
 * raise and can never leave a half-set layout. Callers that need a fallible
 * result use Pango.parse_markup() plus set_text()/set_attributes() -- the
 * path textbox:set_markup_silently() already takes.
 */
static int l_set_markup(lua_State *L)
{
    size_t len = 0;
    pango_layout_set_markup(chk(L, 1), luaL_checklstring(L, 2, &len),
                            (int)len);
    return 0;
}

static int l_set_auto_dir(lua_State *L)
{
    pango_layout_set_auto_dir(chk(L, 1), lua_toboolean(L, 2));
    return 0;
}

/* pango_layout_get_auto_dir() exists in 1.48 (PANGO_AVAILABLE_IN_1_4), so no
 * guard is needed here. */
static int l_get_auto_dir(lua_State *L)
{
    lua_pushboolean(L, pango_layout_get_auto_dir(chk(L, 1)));
    return 1;
}

static int l_set_single_paragraph_mode(lua_State *L)
{
    pango_layout_set_single_paragraph_mode(chk(L, 1), lua_toboolean(L, 2));
    return 0;
}

static int l_get_single_paragraph_mode(lua_State *L)
{
    lua_pushboolean(L, pango_layout_get_single_paragraph_mode(chk(L, 1)));
    return 1;
}

static int l_set_tabs(lua_State *L)
{
    wrap *w = getud(L, 2);
    if (w && w->cls != C_TABARRAY)
        luaL_error(L, "lgishim.Pango: expected Pango.TabArray or nil, got %s",
                   lua_typename(L, 2));
    pango_layout_set_tabs(chk(L, 1), w ? w->p : NULL);
    return 0;
}

static int l_get_tabs(lua_State *L)
{
    PangoTabArray *tabs = pango_layout_get_tabs(chk(L, 1));
    if (!tabs) {
        lua_pushnil(L);
        return 1;
    }
    /* Borrowed: the layout owns the tab array, so the wrapper must not free
     * it -- the same treatment as get_attributes(). */
    return push_obj(L, tabs, d_noop, C_TABARRAY);
}

/*
 * pango_layout_get_log_attrs() is out-param style, so the array has to be
 * g_free()d by us. The PangoLogAttr records themselves are scratch storage
 * for the layout, so each one is copied into a plain table rather than being
 * handed out as a pointer.
 */
static int l_get_log_attrs(lua_State *L)
{
    PangoLogAttr *attrs = NULL;
    int n = 0, i;

    pango_layout_get_log_attrs(chk(L, 1), &attrs, &n);
    lua_newtable(L);
    for (i = 0; i < n; i++) {
        const PangoLogAttr *a = &attrs[i];
        lua_newtable(L);
        lua_pushboolean(L, a->is_line_break);
        lua_setfield(L, -2, "is_line_break");
        lua_pushboolean(L, a->is_mandatory_break);
        lua_setfield(L, -2, "is_mandatory_break");
        lua_pushboolean(L, a->is_char_break);
        lua_setfield(L, -2, "is_char_break");
        lua_pushboolean(L, a->is_white);
        lua_setfield(L, -2, "is_white");
        lua_pushboolean(L, a->is_cursor_position);
        lua_setfield(L, -2, "is_cursor_position");
        lua_pushboolean(L, a->is_word_start);
        lua_setfield(L, -2, "is_word_start");
        lua_pushboolean(L, a->is_word_end);
        lua_setfield(L, -2, "is_word_end");
        lua_pushboolean(L, a->is_sentence_boundary);
        lua_setfield(L, -2, "is_sentence_boundary");
        lua_pushboolean(L, a->is_sentence_start);
        lua_setfield(L, -2, "is_sentence_start");
        lua_pushboolean(L, a->is_sentence_end);
        lua_setfield(L, -2, "is_sentence_end");
        lua_pushboolean(L, a->backspace_deletes_character);
        lua_setfield(L, -2, "backspace_deletes_character");
        lua_pushboolean(L, a->is_expandable_space);
        lua_setfield(L, -2, "is_expandable_space");
        lua_pushboolean(L, a->is_word_boundary);
        lua_setfield(L, -2, "is_word_boundary");
        lua_rawseti(L, -2, i + 1);
    }
    if (attrs)
        g_free(attrs);
    return 1;
}

/*
 * PangoLayoutLine is a public struct here, so the fields can be read
 * directly. The structs are internal to the layout and invalidated by the
 * next layout pass, which is why each line is copied into a table instead of
 * returning a wrapper -- the same reason log attrs above are copied. `runs`
 * is a GSList of PangoGlyphItem and is deliberately not exported.
 */
static int l_get_lines(lua_State *L)
{
    GSList *lines = pango_layout_get_lines(chk(L, 1));
    GSList *it;
    int i = 1;

    lua_newtable(L);
    for (it = lines; it; it = it->next, i++) {
        PangoLayoutLine *line = (PangoLayoutLine *)it->data;
        lua_newtable(L);
        lua_pushinteger(L, line->start_index);
        lua_setfield(L, -2, "start_index");
        lua_pushinteger(L, line->length);
        lua_setfield(L, -2, "length");
        lua_pushboolean(L, line->is_paragraph_start);
        lua_setfield(L, -2, "is_paragraph_start");
        lua_pushinteger(L, line->resolved_dir);
        lua_setfield(L, -2, "resolved_dir");
        lua_rawseti(L, -2, i);
    }
    return 1;
}

static int l_get_character_count(lua_State *L)
{
    lua_pushinteger(L, pango_layout_get_character_count(chk(L, 1)));
    return 1;
}

static int l_get_font_description(lua_State *L)
{
    const PangoFontDescription *d = pango_layout_get_font_description(chk(L, 1));
    if (!d) {
        lua_pushnil(L);
        return 1;
    }
    /* Borrowed: the layout owns the description. */
    return push_obj(L, (void *)d, d_noop, C_FONTDESC);
}

static int l_get_context(lua_State *L)
{
    PangoContext *c = pango_layout_get_context(chk(L, 1));
    if (!c) {
        lua_pushnil(L);
        return 1;
    }
    /* Borrowed: the layout owns its context, so d_noop leaks nothing. */
    return push_obj(L, c, d_noop, C_CONTEXT);
}

static int l_is_ellipsized(lua_State *L)
{
    lua_pushboolean(L, pango_layout_is_ellipsized(chk(L, 1)));
    return 1;
}

static int l_is_wrapped(lua_State *L)
{
    lua_pushboolean(L, pango_layout_is_wrapped(chk(L, 1)));
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
    { "set_markup", l_set_markup },
    { "set_spacing", l_set_spacing },
    { "set_auto_dir", l_set_auto_dir },
    { "set_single_paragraph_mode", l_set_single_paragraph_mode },
    { "set_tabs", l_set_tabs },
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
    { "get_spacing", l_get_spacing },
    { "get_auto_dir", l_get_auto_dir },
    { "get_single_paragraph_mode", l_get_single_paragraph_mode },
    { "get_tabs", l_get_tabs },
    { "get_lines", l_get_lines },
    { "get_log_attrs", l_get_log_attrs },
    { "get_character_count", l_get_character_count },
    { "get_font_description", l_get_font_description },
    { "get_context", l_get_context },
    { "is_ellipsized", l_is_ellipsized },
    { "is_wrapped", l_is_wrapped },
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
    { "markup", l_set_markup },
    { "spacing", l_set_spacing },
    { "auto_dir", l_set_auto_dir },
    { "single_paragraph_mode", l_set_single_paragraph_mode },
    { "tabs", l_set_tabs },
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
    { "spacing", l_get_spacing },
    { "auto_dir", l_get_auto_dir },
    { "single_paragraph_mode", l_get_single_paragraph_mode },
    { "tabs", l_get_tabs },
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
    /* Bind the context to the default font map -- what pango_cairo_create_layout()
     * does -- instead of leaving it font-map-less. A bare context gets a NULL
     * font_map, so every fontset load goes through pango_context_load_fontset()
     * -> the deprecated stub pango_font_map_load_fontset(NULL, ...), which only
     * warns "assertion 'fontmap != NULL' failed" and returns NULL; a layout built
     * on that context can never shape text.
     *
     * Note: pango_cairo_font_map_get_default() is a borrowed, per-thread pango
     * singleton -- see pc_font_map_get_default() for why wrapping *that* in a
     * freeing destructor was the GLib-GObject-CRITICAL flood. Calling it here
     * and passing the pointer straight through is safe: nothing is owned by us.
     * d_unref on the returned context is correct, and the context refs the font
     * map, so the map survives past this call. */
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

/*
 * 1.48's insert()/insert_before()/change() take the attribute exactly as they
 * are given it: they store the pointer in the list's GPtrArray without ref'ing
 * it, and change() calls pango_attribute_destroy() on it whenever the range is
 * empty or it merges into an attribute that already covers the same span.
 * Handing over our own pointer would leave the list pointing at something a
 * later collect() frees -- or at something the list already destroyed. Copying
 * first is what keeps both reference counts balanced: the list owns its copy,
 * the Attribute userdata keeps its own, so neither leaks and neither
 * double-frees.
 *
 * 1.50 widens these signatures with a `before` parameter on insert_before()
 * and an `old_attr` on change(); neither exists in 1.48, so both take a single
 * argument here.
 */
static int al_insert(lua_State *L)
{
    pango_attr_list_insert(
        (PangoAttrList *)chk_cls(L, 1, C_ATTRLIST),
        pango_attribute_copy((PangoAttribute *)chk_cls(L, 2, C_ATTRIBUTE)));
    return 0;
}

static int al_insert_before(lua_State *L)
{
    pango_attr_list_insert_before(
        (PangoAttrList *)chk_cls(L, 1, C_ATTRLIST),
        pango_attribute_copy((PangoAttribute *)chk_cls(L, 2, C_ATTRIBUTE)));
    return 0;
}

static int al_change(lua_State *L)
{
    pango_attr_list_change(
        (PangoAttrList *)chk_cls(L, 1, C_ATTRLIST),
        pango_attribute_copy((PangoAttribute *)chk_cls(L, 2, C_ATTRIBUTE)));
    return 0;
}

static const luaL_Reg ATTRLIST_M[] = {
    { "new", al_new },
    { "insert", al_insert },
    { "insert_before", al_insert_before },
    { "change", al_change },
    { NULL, NULL },
};

/* ---------------------------------------------------------- TabArray */

static int ta_new(lua_State *L)
{
    return push_obj(L, pango_tab_array_new(
                    (gint)luaL_optinteger(L, 1, 0),
                    lua_toboolean(L, 2)),
                    d_tabarray, C_TABARRAY);
}

/* Returns (alignment, location). Out-of-range indexes make pango return
 * without writing either out parameter, so both are zeroed first. */
static int ta_get_tab(lua_State *L)
{
    PangoTabAlign align = PANGO_TAB_LEFT;
    gint location = 0;

    pango_tab_array_get_tab((PangoTabArray *)chk_cls(L, 1, C_TABARRAY),
                            (gint)luaL_checkinteger(L, 2),
                            &align, &location);
    lua_pushinteger(L, align);
    lua_pushinteger(L, location);
    return 2;
}

static int ta_get_size(lua_State *L)
{
    lua_pushinteger(L, pango_tab_array_get_size(
                          (PangoTabArray *)chk_cls(L, 1, C_TABARRAY)));
    return 1;
}

static const luaL_Reg TABARRAY_M[] = {
    { "new", ta_new },
    { "get_tab", ta_get_tab },
    { "get_size", ta_get_size },
    { NULL, NULL },
};

/* --------------------------------------------------------- Rectangle */

/*
 * PangoRectangle is a plain C struct with no GObject type, so there is no
 * userdata to manage. new() returns the same {x,y,width,height} table shape
 * that push_rect() emits from get_extents()/get_pixel_extents(), which makes
 * the two interchangeable -- a rectangle read back off a layout can be fed
 * straight to shape_new() without any conversion.
 */
static int rc_new(lua_State *L)
{
    PangoRectangle r;

    r.x = (int)luaL_optinteger(L, 1, 0);
    r.y = (int)luaL_optinteger(L, 2, 0);
    r.width = (int)luaL_optinteger(L, 3, 0);
    r.height = (int)luaL_optinteger(L, 4, 0);
    push_rect(L, &r);
    return 1;
}

/*
 * Read a rectangle out of a plain table (from Rectangle.new() or from
 * get_extents()). 0 means the argument was absent, in which case the caller
 * pushes nil for the parameter. Field access goes through the absolute
 * argument index so the pushed values never shift it.
 */
static int
rect_arg(lua_State *L, int idx, PangoRectangle *out)
{
    int len = lua_gettop(L);

    luaL_checkstack(L, 6, NULL);
    if (!lua_istable(L, idx))
        return 0;
    lua_getfield(L, idx, "x");
    out->x = (int)luaL_optinteger(L, -1, 0);
    lua_getfield(L, idx, "y");
    out->y = (int)luaL_optinteger(L, -1, 0);
    lua_getfield(L, idx, "width");
    out->width = (int)luaL_optinteger(L, -1, 0);
    lua_getfield(L, idx, "height");
    out->height = (int)luaL_optinteger(L, -1, 0);
    lua_settop(L, len);
    return 1;
}

/*
 * A colour argument is either a Pango.Color userdata (from Color.new() /
 * Color.parse()) or a spec string such as "#ff8800". Returning 0 means "no
 * colour given", in which case the caller pushes nil for the parameter.
 * On 1.48 PangoColor is a public struct, so the components are read directly
 * -- the same access path Color's own getters use.
 */
static int
color_arg(lua_State *L, int idx, guint16 *r, guint16 *g, guint16 *b)
{
    wrap *w = getud(L, idx);

    if (w) {
        if (w->cls != C_COLOR)
            luaL_error(L, "lgishim.Pango: expected Pango.Color or a colour "
                           "string, got %s", lua_typename(L, idx));
        *r = ((PangoColor *)w->p)->red;
        *g = ((PangoColor *)w->p)->green;
        *b = ((PangoColor *)w->p)->blue;
        return 1;
    }
    if (lua_isstring(L, idx)) {
        PangoColor c;
        if (!pango_color_parse(&c, luaL_checkstring(L, idx)))
            luaL_error(L, "lgishim.Pango: cannot parse colour %q",
                       luaL_checkstring(L, idx));
        *r = c.red;
        *g = c.green;
        *b = c.blue;
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------- Attribute */

static int pa_new_attr(lua_State *L, PangoAttribute *a)
{
    return push_obj(L, a, d_attribute, C_ATTRIBUTE);
}

static int pa_size_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_size_new(
                            (int)luaL_checkinteger(L, 1)));
}

static int pa_size_new_absolute(lua_State *L)
{
    return pa_new_attr(L, pango_attr_size_new_absolute(
                                    (int)luaL_checkinteger(L, 1)));
}

static int pa_weight_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_weight_new(
                            (PangoWeight)enum_num_from(L, 1, WEIGHT,
                                sizeof(WEIGHT) / sizeof(WEIGHT[0]),
                                PANGO_WEIGHT_NORMAL)));
}

static int pa_style_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_style_new(
                            (PangoStyle)enum_num_from(L, 1, STYLE,
                                sizeof(STYLE) / sizeof(STYLE[0]),
                                PANGO_STYLE_NORMAL)));
}

static int pa_family_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_family_new(
                            luaL_checkstring(L, 1)));
}

static int pa_underline_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_underline_new(
                                (PangoUnderline)enum_num_from(L, 1, UNDERLINE,
                                    sizeof(UNDERLINE) / sizeof(UNDERLINE[0]),
                                    PANGO_UNDERLINE_NONE)));
}

static int pa_strikethrough_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_strikethrough_new(
                                    lua_toboolean(L, 1)));
}

static int pa_foreground_new(lua_State *L)
{
    guint16 r, g, b;

    if (!color_arg(L, 1, &r, &g, &b))
        luaL_error(L, "lgishim.Pango.Attribute:foreground_new: missing colour");
    return pa_new_attr(L, pango_attr_foreground_new(r, g, b));
}

static int pa_background_new(lua_State *L)
{
    guint16 r, g, b;

    if (!color_arg(L, 1, &r, &g, &b))
        luaL_error(L, "lgishim.Pango.Attribute:background_new: missing colour");
    return pa_new_attr(L, pango_attr_background_new(r, g, b));
}

static int pa_letter_spacing_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_letter_spacing_new(
                                    (int)luaL_checkinteger(L, 1)));
}

static int pa_rise_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_rise_new(
                            (int)luaL_checkinteger(L, 1)));
}

static int pa_scale_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_scale_new(
                            luaL_checknumber(L, 1)));
}

/* lgi calls this font_desc_new(); desc_new is the short spelling and both are
 * wired up in ATTRIBUTE_M. */
static int pa_font_desc_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_font_desc_new(
                                (const PangoFontDescription *)chk_cls(L, 1,
                                                                     C_FONTDESC)));
}

static int pa_variant_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_variant_new(
                                (PangoVariant)enum_num_from(L, 1, VARIANT,
                                    sizeof(VARIANT) / sizeof(VARIANT[0]),
                                    PANGO_VARIANT_NORMAL)));
}

static int pa_gravity_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_gravity_new(
                               (PangoGravity)enum_num_from(L, 1, GRAVITY,
                                   sizeof(GRAVITY) / sizeof(GRAVITY[0]),
                                   PANGO_GRAVITY_SOUTH)));
}

static int pa_gravity_hint_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_gravity_hint_new(
                                    (PangoGravityHint)enum_num_from(
                                        L, 1, GRAVITYHINT,
                                        sizeof(GRAVITYHINT) /
                                            sizeof(GRAVITYHINT[0]),
                                        PANGO_GRAVITY_HINT_NATURAL)));
}

static int pa_shape_new(lua_State *L)
{
    PangoRectangle ink, logical;

    if (!rect_arg(L, 1, &ink) || !rect_arg(L, 2, &logical)) {
        lua_pushnil(L);
        return 1;
    }
    return pa_new_attr(L, pango_attr_shape_new(&ink, &logical));
}

static int pa_fallback_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_fallback_new(
                                lua_toboolean(L, 1)));
}

static int pa_underline_color_new(lua_State *L)
{
    guint16 r, g, b;

    if (!color_arg(L, 1, &r, &g, &b)) {
        lua_pushnil(L);
        return 1;
    }
    return pa_new_attr(L, pango_attr_underline_color_new(r, g, b));
}

static int pa_strikethrough_color_new(lua_State *L)
{
    guint16 r, g, b;

    if (!color_arg(L, 1, &r, &g, &b)) {
        lua_pushnil(L);
        return 1;
    }
    return pa_new_attr(L, pango_attr_strikethrough_color_new(r, g, b));
}

/*
 * pango_language_from_string() returns a shared entry out of pango's language
 * tables and there is no pango_language_free(), so the wrapper must not release
 * it either. pango_attr_language_new() refs the language itself and
 * pango_attribute_destroy() puts that ref back, which is balanced.
 */
static int pa_language_new(lua_State *L)
{
    return pa_new_attr(L, pango_attr_language_new(
                                pango_language_from_string(
                                    luaL_checkstring(L, 1))));
}

/*
 * PangoAttribute on 1.48 only exposes start_index and end_index; type and
 * flags live on the (private) attribute class, so they are not addressable and
 * are not exported. Both fields are writable, which is what change()-style
 * range tweaks need.
 */
static int pa_get_start_index(lua_State *L)
{
    PangoAttribute *a = chk_cls(L, 1, C_ATTRIBUTE);

    lua_pushinteger(L, a->start_index);
    return 1;
}

static int pa_set_start_index(lua_State *L)
{
    ((PangoAttribute *)chk_cls(L, 1, C_ATTRIBUTE))->start_index =
        (guint)luaL_checkinteger(L, 2);
    return 0;
}

static int pa_get_end_index(lua_State *L)
{
    PangoAttribute *a = chk_cls(L, 1, C_ATTRIBUTE);

    lua_pushinteger(L, a->end_index);
    return 1;
}

static int pa_set_end_index(lua_State *L)
{
    ((PangoAttribute *)chk_cls(L, 1, C_ATTRIBUTE))->end_index =
        (guint)luaL_checkinteger(L, 2);
    return 0;
}

static const luaL_Reg ATTRIBUTE_M[] = {
    { "size_new", pa_size_new },
    { "size_new_absolute", pa_size_new_absolute },
    { "weight_new", pa_weight_new },
    { "style_new", pa_style_new },
    { "family_new", pa_family_new },
    { "underline_new", pa_underline_new },
    { "strikethrough_new", pa_strikethrough_new },
    { "foreground_new", pa_foreground_new },
    { "background_new", pa_background_new },
    { "letter_spacing_new", pa_letter_spacing_new },
    { "rise_new", pa_rise_new },
    { "scale_new", pa_scale_new },
    { "desc_new", pa_font_desc_new },
    { "font_desc_new", pa_font_desc_new },
    { "variant_new", pa_variant_new },
    { "gravity_new", pa_gravity_new },
    { "gravity_hint_new", pa_gravity_hint_new },
    { "shape_new", pa_shape_new },
    { "fallback_new", pa_fallback_new },
    { "underline_color_new", pa_underline_color_new },
    { "strikethrough_color_new", pa_strikethrough_color_new },
    { "language_new", pa_language_new },
    { "get_start_index", pa_get_start_index },
    { "get_end_index", pa_get_end_index },
    { NULL, NULL },
};

static const luaL_Reg ATTRIBUTE_PG[] = {
    { "start_index", pa_get_start_index },
    { "end_index", pa_get_end_index },
    { NULL, NULL },
};

static const luaL_Reg ATTRIBUTE_P[] = {
    { "start_index", pa_set_start_index },
    { "end_index", pa_set_end_index },
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

/*
 * `_native` is registered as a PROPERTY GETTER, not as a metatable field.
 * m_index's pg path auto-invokes the closure with obj as its argument and
 * returns the resulting value, so `layout._native` is the raw Pango* pointer
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
    luaL_setfuncs(L, NATIVE_PG, 0);            /* always: _native (property getter) */
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
    class_setup(L, C_ATTRIBUTE, ATTRIBUTE_M, ATTRIBUTE_PG, ATTRIBUTE_P);
    lua_setfield(L, -2, "Attribute");
    class_setup(L, C_TABARRAY, TABARRAY_M, NULL, NULL);
    lua_setfield(L, -2, "TabArray");

    /* Rectangle is a plain table, not a userdata class: PangoRectangle has no
     * GObject type and nothing to destroy, so class_setup() would only install
     * an __index chain that has no object behind it. */
    lua_newtable(L);
    lua_pushcfunction(L, rc_new);
    lua_setfield(L, -2, "new");
    lua_setfield(L, -2, "Rectangle");

    lua_pushinteger(L, PANGO_SCALE);
    lua_setfield(L, -2, "SCALE");
    lua_pushcfunction(L, p_units_from_double);
    lua_setfield(L, -2, "units_from_double");
    lua_pushcfunction(L, p_parse_markup);
    lua_setfield(L, -2, "parse_markup");

    /* Numeric enum tables. These are deliberately NOT registered as
     * Alignment/EllipsizeMode/WrapMode: those three are string-valued through
     * enum_from()/enum_to() and changing them would break every existing caller.
     */
    install_enum_num(L, "Weight", WEIGHT,
                     sizeof(WEIGHT) / sizeof(WEIGHT[0]));
    install_enum_num(L, "Style", STYLE,
                     sizeof(STYLE) / sizeof(STYLE[0]));
    install_enum_num(L, "Stretch", STRETCH,
                     sizeof(STRETCH) / sizeof(STRETCH[0]));
    install_enum_num(L, "Underline", UNDERLINE,
                     sizeof(UNDERLINE) / sizeof(UNDERLINE[0]));
    install_enum_num(L, "Variant", VARIANT,
                     sizeof(VARIANT) / sizeof(VARIANT[0]));
    install_enum_num(L, "Gravity", GRAVITY,
                     sizeof(GRAVITY) / sizeof(GRAVITY[0]));
    install_enum_num(L, "GravityHint", GRAVITYHINT,
                     sizeof(GRAVITYHINT) / sizeof(GRAVITYHINT[0]));
    return 1;
}

static int
pc_font_map_get_default(lua_State *L)
{
    /* BORROWED, never freed by us. Pango 1.48 caches this in a per-thread
     * g_private and documents it as "(transfer none): owned by Pango and must
     * not be freed". Handing it to d_unref (g_object_unref) was the runtime
     * flood: PangoFontMap is a real GObject here (G_DEFINE_ABSTRACT_TYPE_WITH_CODE
     * ... G_TYPE_OBJECT), so the wrap dropped the singleton's only ref the
     * moment the Lua temporary was collected. Pango's g_private then kept a
     * dangling pointer, pango_cairo_font_map_get_default() handed the freed
     * object back out forever, pango_font_map_create_context() warned and
     * stored the garbage in context->font_map, and pango/itemize.c
     * get_font_cache() spammed `g_object_get_qdata` / `g_object_replace_qdata`
     * `G_IS_OBJECT (object)' criticals in its `goto retry' loop at ~11 MB/s.
     * A context or layout built from it stays alive because it refs the font
     * map itself, so d_noop leaks nothing. */
    return push_obj(L, pango_cairo_font_map_get_default(), d_noop,
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
