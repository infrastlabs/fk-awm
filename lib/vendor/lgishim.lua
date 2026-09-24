-- LGI shim: drop-in replacement for `require("lgi")`.
--
-- Backs Pango / PangoCairo with a statically linked binding (lgishim_pango.c)
-- that calls the real Pango C API, so text metrics stay correct without the
-- gobject-introspection runtime (lgi .so + typelibs) that lgi needs.
--
-- Selection is automatic: `lgishim_pango` is only reachable when it has been
-- registered in package.preload, which luaa.c does for the static build. In a
-- normal build the require misses and real lgi is loaded instead, so this file
-- is behaviour-preserving there.
--
-- Modules left empty are Phase 2 work. They are left empty on purpose so the
-- code paths that touch them (naughty/dbus.lua, gears/filesystem.lua,
-- gears/color.lua, gears/matrix.lua) fail loudly instead of measuring with
-- fake objects.

local M
local ok, Pango = pcall(require, "lgishim_pango")

if ok and type(Pango) == "table" then
    local ok2, PangoCairo = pcall(require, "lgishim_pangocairo")
    local ok3, Cairo = pcall(require, "lgishim_cairo")
    local ok4, GdkPixbuf = pcall(require, "lgishim_pixbuf")
    local ok5, GLib = pcall(require, "lgishim_glib")
    local ok6, GObject = pcall(require, "lgishim_gobject")
    local ok7, Gio = pcall(require, "lgishim_gio")
    M = {
        shim = true,
        Pango = Pango,
        PangoCairo = ok2 and PangoCairo or {},
        Cairo = ok3 and Cairo or {},
        GdkPixbuf = ok4 and GdkPixbuf or {},
        GLib = ok5 and GLib or {},
        Gio = ok7 and Gio or {},
        Rsvg = {},
        GObject = ok6 and GObject or {},
    }
    M.cairo = M.Cairo  -- lowercase alias for the codebase's lgi.cairo usage
    M.gdkpixbuf = M.GdkPixbuf
    M.gio = M.Gio
    -- C-side copy of the class table would be a *distinct* table, so the
    -- lowercase class alias is bound here where the assignment is a real
    -- identity copy (lib/menubar/utils.lua et al. look for `pixbuf`).
    M.GdkPixbuf.pixbuf = M.GdkPixbuf.Pixbuf
else
    M = require("lgi")
    M.shim = false
    M.version = require("lgi.version")
end

return M
