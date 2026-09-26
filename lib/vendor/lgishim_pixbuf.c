/*
 * lgishim_pixbuf.c - GdkPixbuf Lua binding for the LGI shim.
 *
 * Replaces `lgi.GdkPixbuf` for the small set of APIs actually used:
 * lib/ calls only `GdkPixbuf.Pixbuf.new_from_file(path)`
 * (lib/gears/surface.lua:59), and the module-level
 * `GdkPixbuf.pixbuf_get_from_surface(surface, x, y, w, h)` is called from
 * spec/gears/surface_spec.lua:50,57.  The latter takes a cairo surface, so
 * this module reaches into lgishim_cairo.c for the raw cairo_surface_t *.
 *
 * lib/ reads `pixbuf._native` and hands the
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
 * Compile flags (see CMakeLists.txt:116-124, spike4-phase2-04-c-api-conventions.md):
 *   -Wundef, -Wmissing-prototypes, -Wstrict-prototypes, -Wall, -Wextra,
 *   -Wno-unused-parameter, -Wuninitialized, -Wsign-compare, -Wformat-nonliteral.
 */

#include <lauxlib.h>
#include <lua.h>

#include <png.h>
#include <jpeglib.h>

#include <ctype.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib-object.h>

/* Exported to luaa.c, which registers it in package.preload. */
int luaopen_lgishim_pixbuf(lua_State *L);

/* Provided by lgishim_cairo.c, which owns the wrap layout of a
 * cairo.Surface.  Declared here, mirroring the lgishim_pango_check_layout()
 * split (lgishim_cairo.c:90-94 declares the one pango provides). */
cairo_surface_t *lgishim_cairo_check_surface(lua_State *L, int idx);

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

/* ----------------------------------------------------------- image decoders
 *
 * gdk-pixbuf's normal gdk_pixbuf_new_from_file() dispatches through its
 * loader-plugin mechanism, which needs g_module + a loaders.cache to find
 * libpixbufloader-*.so.  Neither is usable in this fully-static build
 * (docs/sam/2026-09-25-record3.md §10), so we sniff the magic bytes here
 * and decode the three formats the callers actually need -- PNG, JPEG, XPM
 * -- directly against the linked libraries.  The pixbuf we return has the
 * same shape a gdk-pixbuf loader would produce: GDK_COLORSPACE_RGB, 8
 * bits per sample, [R,G,B,A] in memory (or [R,G,B] for no-alpha JPEG),
 * which is what draw.c:85's draw_surface_from_pixbuf() reads.  The pixbuf
 * owns its own copy of the pixel data, so d_pixbuf's g_object_unref
 * releases it exactly once.
 */

static unsigned char *
read_whole_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *buf;
    size_t cap = 4096, len = 0, n;

    if (!fp)
        return NULL;
    buf = malloc(cap);
    if (!buf) { fclose(fp); return NULL; }
    while ((n = fread(buf + len, 1, cap - len, fp)) > 0) {
        len += n;
        if (len == cap) {
            unsigned char *nb;
            cap *= 2;
            nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(fp); return NULL; }
            buf = nb;
        }
    }
    fclose(fp);
    *out_len = len;
    return buf;
}

/* PNG: libpng 1.5+ simplified png_image_* API.  The version field must be
 * set to PNG_IMAGE_VERSION before begin_read, else it fails with
 * "incorrect PNG_IMAGE_VERSION".  We supply our own buffer to
 * png_image_finish_read and it cleans up the png_image on return.  With
 * PNG_FORMAT_RGBA libpng writes [R,G,B,A], matching GdkPixbuf's memory
 * order (see the probe in docs/sam/2026-09-25-record3.md §10.3). */
static GdkPixbuf *
decode_png(const unsigned char *data, size_t len, char *err, size_t errsz)
{
    png_image img;
    unsigned char *buf, *dst;
    GdkPixbuf *pixbuf;
    size_t rs, ds;
    png_uint_32 y;
    int has_alpha, bpp;

    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_memory(&img, data, len) == 0) {
        snprintf(err, errsz, "png: cannot read header");
        return NULL;
    }
    has_alpha = (img.format & PNG_FORMAT_FLAG_ALPHA) != 0;
    bpp = has_alpha ? 4 : 3;
    img.format = has_alpha ? PNG_FORMAT_RGBA : PNG_FORMAT_RGB;

    buf = malloc((size_t)img.width * (size_t)img.height * bpp);
    if (!buf) {
        snprintf(err, errsz, "png: out of memory (%lux%lu)",
                 (unsigned long)img.width, (unsigned long)img.height);
        png_image_free(&img);
        return NULL;
    }
    if (png_image_finish_read(&img, NULL, buf,
                              img.width * bpp, NULL) == 0) {
        snprintf(err, errsz, "png: cannot decode pixel data");
        free(buf);
        return NULL;
    }
    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, has_alpha, 8,
                            img.width, img.height);
    if (!pixbuf) {
        snprintf(err, errsz, "png: pixbuf alloc failed (%lux%lu)",
                 (unsigned long)img.width, (unsigned long)img.height);
        free(buf);
        return NULL;
    }
    dst = gdk_pixbuf_get_pixels(pixbuf);
    ds = (size_t)gdk_pixbuf_get_rowstride(pixbuf);
    rs = (size_t)img.width * bpp;
    for (y = 0; y < img.height; y++)
        memcpy(dst + (size_t)y * ds, buf + (size_t)y * rs, rs);
    free(buf);
    return pixbuf;
}

/* JPEG: libjpeg's default error_exit() calls exit(), so we override it via
 * setjmp/longjmp with a stack jmp_buf.  The jmp_buf, cinfo and any pointer
 * we still read after the longjmp are declared volatile-free because C
 * allows non-volatile stack locals to be inspected across a longjmp to
 * their previous value, which is exactly what we want.  jpeg_mem_src()
 * reads from our already-loaded buffer, so no FILE* juggling. */
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
    char msg[256];
} jmp_err_mgr;

static void
jpeg_error_exit(j_common_ptr cinfo)
{
    jmp_err_mgr *st = (jmp_err_mgr *)cinfo->err;
    (*cinfo->err->format_message)(cinfo, st->msg);
    longjmp(st->jb, 1);
}

static GdkPixbuf *
decode_jpeg(const unsigned char *data, size_t len, char *err, size_t errsz)
{
    struct jpeg_decompress_struct cinfo;
    jmp_err_mgr jerr;
    unsigned char *scanbuf, *dst;
    GdkPixbuf *pixbuf;
    JDIMENSION y;
    JSAMPROW rowp;
    size_t rs, ds;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    cinfo.err = (struct jpeg_error_mgr *)&jerr;

    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        snprintf(err, errsz, "jpeg: %s", jerr.msg);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, (unsigned long)len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        snprintf(err, errsz, "jpeg: bad header");
        return NULL;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
                            (int)cinfo.output_width,
                            (int)cinfo.output_height);
    if (!pixbuf) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        snprintf(err, errsz, "jpeg: pixbuf alloc failed");
        return NULL;
    }
    scanbuf = malloc((size_t)cinfo.output_width * 3);
    if (!scanbuf) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        g_object_unref(pixbuf);
        snprintf(err, errsz, "jpeg: out of memory");
        return NULL;
    }
    dst = gdk_pixbuf_get_pixels(pixbuf);
    ds = (size_t)gdk_pixbuf_get_rowstride(pixbuf);
    rs = (size_t)cinfo.output_width * 3;
    for (y = 0; y < cinfo.output_height; y++) {
        rowp = scanbuf;
        if (jpeg_read_scanlines(&cinfo, &rowp, 1) != 1) {
            jpeg_abort_decompress(&cinfo);
            jpeg_destroy_decompress(&cinfo);
            g_object_unref(pixbuf);
            free(scanbuf);
            snprintf(err, errsz, "jpeg: read failure at row %lu",
                     (unsigned long)y);
            return NULL;
        }
        memcpy(dst + (size_t)y * ds, scanbuf, rs);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(scanbuf);
    return pixbuf;
}

/* XPM: parse the file (already in memory) into a NULL-terminated char**
 * array of C-string-literal contents, then hand that to
 * gdk_pixbuf_new_from_xpm_data.  That function is compiled into core
 * libgdk_pixbuf (nm shows T gdk_pixbuf_new_from_xpm_data), NOT a loader
 * plugin, so it works in the static build.  The parser handles the C
 * escapes XPM actually uses: \n \t \r \a \b \f \v \\ \" \' \xNN \NNN,
 * and \<newline> line continuations for long pixel rows. */
static const char **
parse_xpm_strings(const char *src, size_t len, char *err, size_t errsz)
{
    const char **out = NULL;
    char **strings;
    char *cur = NULL;
    size_t cap = 128, nstr = 0;
    size_t cur_len = 0, cur_cap = 0;
    size_t i = 0, k;

    strings = malloc(cap * sizeof(*strings));
    if (!strings) {
        snprintf(err, errsz, "xpm: out of memory");
        return NULL;
    }

#define PUSH_STR() do { \
    if (nstr >= cap) { \
        size_t ncap = cap * 2; \
        char **ns = realloc(strings, ncap * sizeof(*ns)); \
        if (!ns) { snprintf(err, errsz, "xpm: out of memory"); goto fail; } \
        strings = ns; cap = ncap; \
    } \
    if (cur_len > 0) { \
        char *s = malloc(cur_len + 1); \
        if (!s) { snprintf(err, errsz, "xpm: out of memory"); goto fail; } \
        memcpy(s, cur, cur_len); s[cur_len] = '\0'; \
        strings[nstr++] = s; \
        cur_len = 0; \
    } \
} while (0)

#define PUSH_CHAR(c) do { \
    if (cur_len + 2 >= cur_cap) { \
        size_t ncap = cur_cap * 2 + 16; \
        char *nc = realloc(cur, ncap); \
        if (!nc) { snprintf(err, errsz, "xpm: out of memory"); goto fail; } \
        cur = nc; cur_cap = ncap; \
    } \
    cur[cur_len++] = (char)(c); \
} while (0)

    while (i < len) {
        if (src[i] != '"') { i++; continue; }
        i++;
        while (i < len && src[i] != '"') {
            char c = src[i++];
            if (c != '\\') { PUSH_CHAR(c); continue; }
            if (i >= len) {
                snprintf(err, errsz, "xpm: trailing backslash");
                goto fail;
            }
            c = src[i++];
            switch (c) {
            case 'n': PUSH_CHAR('\n'); break;
            case 't': PUSH_CHAR('\t'); break;
            case 'r': PUSH_CHAR('\r'); break;
            case 'a': PUSH_CHAR('\a'); break;
            case 'b': PUSH_CHAR('\b'); break;
            case 'f': PUSH_CHAR('\f'); break;
            case 'v': PUSH_CHAR('\v'); break;
            case '\\': PUSH_CHAR('\\'); break;
            case '"': PUSH_CHAR('"'); break;
            case '\'': PUSH_CHAR('\''); break;
            case '\n': case '\r': break;  /* line continuation */
            case 'x': case 'X': {
                char hb[8]; int nd = 0;
                while (nd < 2 && i < len && isxdigit((unsigned char)src[i]))
                    hb[nd++] = src[i++];
                hb[nd] = '\0';
                if (nd == 0) {
                    snprintf(err, errsz, "xpm: bad hex escape");
                    goto fail;
                }
                PUSH_CHAR(strtoul(hb, NULL, 16));
                break;
            }
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                char ob[8]; int nd = 0;
                ob[nd++] = c;
                while (nd < 3 && i < len &&
                       src[i] >= '0' && src[i] <= '7')
                    ob[nd++] = src[i++];
                ob[nd] = '\0';
                PUSH_CHAR(strtoul(ob, NULL, 8));
                break;
            }
            default:
                PUSH_CHAR('\\'); PUSH_CHAR(c);
                break;
            }
        }
        if (i < len) i++;  /* skip closing quote */
        PUSH_STR();
    }
    PUSH_STR();

    out = malloc((nstr + 1) * sizeof(*out));
    if (!out) { snprintf(err, errsz, "xpm: out of memory"); goto fail; }
    for (k = 0; k < nstr; k++)
        out[k] = strings[k];
    out[nstr] = NULL;
#undef PUSH_STR
#undef PUSH_CHAR
    return out;
fail:
    for (k = 0; k < nstr; k++)
        free(strings[k]);
    free(strings);
    free(cur);
#undef PUSH_STR
#undef PUSH_CHAR
    return NULL;
}
static GdkPixbuf *
decode_xpm(const char *data, size_t len, char *err, size_t errsz)
{
    const char **strs = parse_xpm_strings(data, len, err, errsz);
    GdkPixbuf *pixbuf = NULL;
    unsigned char *pixels = NULL;
    int width, height, ncolors, cpp, has_alpha = 0;
    unsigned char *palette;
    int row, col, si, k;
    size_t ds;
    const char *s;

    if (!strs)
        return NULL;

    /* Header: "width height ncolors cpp" */
    if (sscanf(strs[0], "%d %d %d %d", &width, &height, &ncolors, &cpp)
            != 4 || width <= 0 || height <= 0 || ncolors <= 0 ||
            cpp < 1 || cpp > 4) {
        snprintf(err, errsz, "xpm: bad header");
        goto cleanup;
    }

    palette = calloc((size_t)ncolors, 4);
    if (!palette) {
        snprintf(err, errsz, "xpm: out of memory");
        goto cleanup;
    }

    /* Colors: strs[1..ncolors] are "key c color_string" where key is
     * exactly cpp characters at the start of the string.  A space key
     * (e.g. " 	c None") means transparent.  We skip past the key, any
     * whitespace, the 'c', more whitespace, and land on the color. */
    for (si = 1; si <= ncolors && si < (int)height + ncolors + 1; si++) {
        unsigned char r = 0, g = 0, b = 0, a = 255;
        if (!strs[si]) {
            snprintf(err, errsz, "xpm: truncated color table");
            goto cleanup;
        }
        s = strs[si];
        if (strlen(s) < (size_t)cpp + 2)
            continue;
        s += cpp;  /* skip key */
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s != 'c' && *s != 'C')
            continue;
        s++;
        while (*s == ' ' || *s == '\t')
            s++;

        if (strcmp(s, "None") == 0) {
            a = 0;
            has_alpha = 1;
        } else if (s[0] == '#') {
            const char *h = s + 1;
            size_t hl = strlen(h);
            unsigned int u[4] = {0, 0, 0, 255};
            if (hl == 6) {
                sscanf(h, "%2x%2x%2x", &u[0], &u[1], &u[2]);
                r = (unsigned char)u[0];
                g = (unsigned char)u[1];
                b = (unsigned char)u[2];
            } else if (hl == 8) {
                sscanf(h, "%2x%2x%2x%2x", &u[0], &u[1], &u[2], &u[3]);
                a = (unsigned char)u[0];
                r = (unsigned char)u[1];
                g = (unsigned char)u[2];
                b = (unsigned char)u[3];
                has_alpha = 1;
            } else if (hl == 12) {
                sscanf(h, "%4x%4x%4x", &u[0], &u[1], &u[2]);
                r = (unsigned char)(u[0] >> 8);
                g = (unsigned char)(u[1] >> 8);
                b = (unsigned char)(u[2] >> 8);
            } else if (hl == 16) {
                sscanf(h, "%4x%4x%4x%4x", &u[0], &u[1], &u[2], &u[3]);
                a = (unsigned char)(u[0] >> 8);
                r = (unsigned char)(u[1] >> 8);
                g = (unsigned char)(u[2] >> 8);
                b = (unsigned char)(u[3] >> 8);
                has_alpha = 1;
            }
        } else if (strncmp(s, "rgb:", 4) == 0 ||
                   strncmp(s, "rgba:", 5) == 0) {
            const char *p = strchr(s, ':') + 1;
            char tok[64]; int ti, nv = 0;
            unsigned int vals[4] = {0};
            while (*p && nv < 4) {
                ti = 0;
                while (*p && *p != '/' && ti < 63)
                    tok[ti++] = *p++;
                tok[ti] = '\0';
                if (ti > 0)
                    vals[nv++] = strtoul(tok, NULL, 16);
                if (*p == '/') p++;
            }
            {
                int scale = (vals[0] > 255 || vals[1] > 255 ||
                             vals[2] > 255) ? 257 : 1;
                r = (unsigned char)(vals[0] / scale);
                g = (unsigned char)(vals[1] / scale);
                b = (unsigned char)(vals[2] / scale);
                if (nv >= 4) {
                    a = (unsigned char)(vals[3] / scale);
                    has_alpha = 1;
                }
            }
        }
        palette[(si - 1) * 4 + 0] = r;
        palette[(si - 1) * 4 + 1] = g;
        palette[(si - 1) * 4 + 2] = b;
        palette[(si - 1) * 4 + 3] = a;
    }

    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, has_alpha, 8,
                            width, height);
    if (!pixbuf) {
        snprintf(err, errsz, "xpm: pixbuf alloc failed (%dx%d)",
                 width, height);
        goto cleanup;
    }
    pixels = gdk_pixbuf_get_pixels(pixbuf);
    ds = (size_t)gdk_pixbuf_get_rowstride(pixbuf);

    /* Pixel rows: strs[ncolors+1 .. ncolors+height].  Each row has
     * width*cpp characters; every cpp chars is a key into the palette. */
    k = ncolors + 1;
    for (row = 0; row < height && k < (int)height + ncolors + 1; row++) {
        if (!strs[k]) break;
        s = strs[k];
        if ((int)strlen(s) < width * cpp) break;
        for (col = 0; col < width; col++) {
            unsigned char r = 0, g = 0, b = 0, a = 0;
            int found = 0;
            for (si = 0; si < ncolors && !found; si++) {
                const char *ck = strs[si + 1];
                int ok = 1, jj;
                if (!ck) { ok = 0; }
                else {
                    for (jj = 0; jj < cpp; jj++)
                        if (ck[jj] != s[col * cpp + jj]) { ok = 0; break; }
                    if (ok && jj == cpp) {
                        r = palette[si * 4];
                        g = palette[si * 4 + 1];
                        b = palette[si * 4 + 2];
                        a = palette[si * 4 + 3];
                        found = 1;
                    }
                }
            }
            if (has_alpha) {
                unsigned char *q = pixels + (size_t)row * ds +
                                    (size_t)col * 4;
                q[0] = r; q[1] = g; q[2] = b; q[3] = a;
            } else {
                unsigned char *q = pixels + (size_t)row * ds +
                                    (size_t)col * 3;
                q[0] = r; q[1] = g; q[2] = b;
            }
        }
        k++;
    }
    if (k - ncolors - 1 < height) {
        snprintf(err, errsz, "xpm: only %d of %d pixel rows",
                 k - ncolors - 1, height);
        g_object_unref(pixbuf);
        pixbuf = NULL;
    }

cleanup:
    for (k = 0; strs[k]; k++)
        free((char *)strs[k]);
    free((void *)strs);
    free(palette);
    return pixbuf;
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
    GdkPixbuf *pixbuf = NULL;
    GError *error = NULL;
    unsigned char *data = NULL;
    char errbuf[512];
    size_t len = 0;
    int attempted = 0;

    /* luaL_checkstring raises on non-string; lib/ always passes a string
     * (surface.lua:59 only reaches here inside `type(_surface)=="string"`). */
    path = luaL_checkstring(L, 1);

    /* Sniff magic bytes ourselves so the three formats gdk-pixbuf would
     * normally load via plugins are decodable in the static build.  We
     * read the whole file once -- small for icons, and we'd otherwise
     * open it twice (sniff + decode).  The decoders copy out of `data`
     * into the pixbuf, so we free it before handing back. */
    data = read_whole_file(path, &len);
    if (data) {
        if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
            pixbuf = decode_png(data, len, errbuf, sizeof(errbuf));
            attempted = 1;
        } else if (len >= 3 && data[0] == 0xFF &&
                   data[1] == 0xD8 && data[2] == 0xFF) {
            pixbuf = decode_jpeg(data, len, errbuf, sizeof(errbuf));
            attempted = 1;
        } else if (len >= 7 && memcmp(data, "/* XPM */", 7) == 0) {
            pixbuf = decode_xpm((const char *)data, len, errbuf,
                                sizeof(errbuf));
            attempted = 1;
        }
        free(data);
        data = NULL;
    }

    /* For anything else -- TIFF, BMP, SVG, a missing file -- let gdk-pixbuf
     * produce its own error message (e.g. "could not find an origin for
     * this loader" for a format we didn't sniff).  Falling through here
     * preserves the pre-existing "unknown format" error the caller gets. */
    if (!pixbuf && !attempted)
        pixbuf = gdk_pixbuf_new_from_file(path, &error);

    if (!pixbuf) {
        if (attempted) {
            /* our decoder failed: errbuf holds the message */
            lua_pushnil(L);
            lua_pushstring(L, errbuf);
            return 2;
        }
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

#if !defined(CAIRO_HAS_GDKPIXBUF_FUNCTIONS)
/* Render the rect into our own ARGB32 image surface and copy its bytes into a
 * fresh pixbuf.  gdk-pixbuf's cairo glue (gdk_pixbuf_get_from_surface,
 * declared by <cairo.h> only under CAIRO_HAS_GDKPIXBUF_FUNCTIONS) is absent
 * from this toolchain: cairo-features.h defines no
 * CAIRO_HAS_GDKPIXBUF_FUNCTIONS and libcairo.a exports no gdk_pixbuf_* symbol
 * at all, so the direct call would compile but fail to link.  draw.c:85 hit
 * the same gap going the other way and hand-rolled the copy; this is the
 * mirror image.  Returns NULL on failure, so the caller raises. */
static GdkPixbuf *
surface_to_pixbuf(cairo_surface_t *surface, int sx, int sy, int w, int h)
{
    cairo_surface_t *tmp;
    cairo_t *cr;
    cairo_status_t status;
    const unsigned char *data;
    unsigned char *rowsrc;
    unsigned char *dest;
    size_t stride;
    size_t dststride;
    int row;
    GdkPixbuf *pixbuf;

    status = cairo_surface_status(surface);
    if (status != CAIRO_STATUS_SUCCESS)
        return NULL;

    /* create_similar() is cheaper for a backend surface, but it can hand back a
     * surface with no in-process pixel buffer (Xlib, script, png, ...), and
     * cairo_image_surface_get_data() asserts on those.  Fall back to a plain
     * image surface whenever the result is not an image surface -- the same
     * guard lgishim_cairo.c's surface_get_data() applies.  Note create_similar
     * takes a cairo_content_t (0x1000+), NOT the cairo_format_t used by
     * cairo_image_surface_create below; ARGB32 == COLOR_ALPHA in value but the
     * two enums are unrelated, and content 0 means "invalid content". */
    tmp = cairo_surface_create_similar(surface, CAIRO_CONTENT_COLOR_ALPHA, w, h);
    status = cairo_surface_status(tmp);
    if (status != CAIRO_STATUS_SUCCESS ||
        cairo_surface_get_type(tmp) != CAIRO_SURFACE_TYPE_IMAGE) {
        cairo_surface_destroy(tmp);
        tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        status = cairo_surface_status(tmp);
    }
    if (status != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(tmp);
        return NULL;
    }

    /* Painting the source at (-sx, -sy) shifts the window, so only the rect is
     * sampled; cairo clips reads outside the source, so a negative offset
     * cannot overread.  Check the paint result: a failed context would leave
     * `tmp` zero-filled and we would hand back an all-transparent pixbuf
     * instead of raising. */
    cairo_surface_flush(surface);
    cr = cairo_create(tmp);
    cairo_set_source_surface(cr, surface, -sx, -sy);
    cairo_paint(cr);
    status = cairo_status(cr);
    cairo_destroy(cr);
    cairo_surface_flush(tmp);
    if (status != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(tmp);
        return NULL;
    }

    data = cairo_image_surface_get_data(tmp);
    if (!data) {
        cairo_surface_destroy(tmp);
        return NULL;
    }

    /* RGBA, 4 channels, has_alpha -- the shape
     * gdk_pixbuf_get_from_surface() promises.  Note the 3rd argument is
     * bits_per_sample, NOT depth: depth is derived as bits_per_sample *
     * n_channels, so 8 with has_alpha gives depth 32 / n_channels 4 /
     * bits_per_sample 8.  Passing 24 (the depth) makes gdk-pixbuf compute
     * bits_per_sample 6 and trips gdk_pixbuf_calculate_rowstride()'s
     * 'bits_per_sample == 8' assertion, so the allocation returns an unusable
     * pixbuf.  The bytes are copied verbatim, so cairo's ARGB32 premultiplied
     * alpha stays premultiplied, exactly as upstream's glue does; no
     * un-premultiplication is applied here.  The pixbuf owns its own copy of
     * the bytes, so the wrap's d_pixbuf releases it exactly once and no
     * borrowed pointer escapes. */
    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    if (!pixbuf) {
        cairo_surface_destroy(tmp);
        return NULL;
    }

    stride = (size_t)cairo_image_surface_get_stride(tmp);
    rowsrc = (unsigned char *)data;
    dest = gdk_pixbuf_get_pixels(pixbuf);
    dststride = (size_t)gdk_pixbuf_get_rowstride(pixbuf);
    for (row = 0; row < h; row++)
        memcpy(dest + (size_t)row * dststride,
               rowsrc + (size_t)row * stride, (size_t)w * 4);
    cairo_surface_destroy(tmp);

    return pixbuf;
}
#endif

static int
pixbuf_get_from_surface(lua_State *L)
{
    cairo_surface_t *surface;
    int sx, sy, w, h;
    GdkPixbuf *pixbuf;

    /* Module-level function, NOT a Pixbuf: method: the first argument is the
     * cairo surface, not self (spec/gears/surface_spec.lua:50,57 -- the only
     * callers in tree, and both pass a wrapped cairo.Surface).  check_surface
     * also validates w->cls, so a wrapped GdkPixbuf handed in here raises
     * instead of being reinterpreted as a surface. */
    surface = lgishim_cairo_check_surface(L, 1);
    sx = (int)luaL_checknumber(L, 2);
    sy = (int)luaL_checknumber(L, 3);
    w = (int)luaL_checknumber(L, 4);
    h = (int)luaL_checknumber(L, 5);

    if (w <= 0 || h <= 0)
        return luaL_error(L, "lgishim_pixbuf: pixbuf_get_from_surface: "
                              "width and height must be positive, got %dx%d",
                          w, h);

#if defined(CAIRO_HAS_GDKPIXBUF_FUNCTIONS)
    pixbuf = gdk_pixbuf_get_from_surface(surface, sx, sy, w, h);
    if (!pixbuf)
        return luaL_error(L, "lgishim_pixbuf: pixbuf_get_from_surface: "
                              "cairo refused the rectangle");
#else
    pixbuf = surface_to_pixbuf(surface, sx, sy, w, h);
    if (!pixbuf)
        return luaL_error(L, "lgishim_pixbuf: pixbuf_get_from_surface: "
                              "failed to read the surface");
#endif
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

    /* pixbuf_get_from_surface is a MODULE-LEVEL function, not a Pixbuf:
     * method (the cairo surface is arg 1, not self), so it goes on the module
     * table and NOT in PIXBUF_M.  Pushing it after lua_setfield() above is
     * fine: that call already popped the class table, so the module table is
     * on top again and the cfunction binds to it. */
    lua_pushcfunction(L, pixbuf_get_from_surface);
    lua_setfield(L, -2, "pixbuf_get_from_surface");

    /* The module-level aliases (GdkPixbuf/gdkpixbuf/Gdk and the lowercase
     * `pixbuf` class alias) are not installed here: lua_setfield() pops the
     * class table, so anything pushed afterwards points at the module table
     * itself, and a lua_pushvalue() copy is a distinct table. lgishim.lua owns
     * all of them (M.GdkPixbuf / M.gdkpixbuf / M.Gdk / M.GdkPixbuf.pixbuf),
     * where a plain assignment is a genuine identity copy, which is where lib/
     * actually reaches for them. */
    return 1;
}
