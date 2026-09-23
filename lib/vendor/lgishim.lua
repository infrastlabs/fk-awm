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
    M = {
        shim = true,
        Pango = Pango,
        PangoCairo = ok2 and PangoCairo or {},
        GLib = {},
        Gio = {},
        cairo = {},
        Rsvg = {},
        GObject = {},
    }
else
    M = require("lgi")
    M.shim = false
    M.version = require("lgi.version")
end

return M
