-- This script is given to Busted via the --helper argument. Modules loaded here
-- won't be cleared and reloaded by Busted. This is needed for lgi because lgi
-- is not safe to reload and yet Busted manages to do this.
--
-- Backend selection. lib/ asks for the bindings through
-- require("vendor.lgishim"); the *_spec.lua files ask for them through
-- require("lgi"). vendor.lgishim is the shim's Lua entry point: when the shim's
-- C part is resolvable on package.cpath it builds the shim and sets
-- M.shim = true, otherwise it degrades to the distribution's lua-lgi. Requiring
-- vendor.lgishim first and then aliasing "lgi" onto it makes both spellings
-- return the very same table, so the shim's is_type_of() metatable checks stay
-- true across the two spellings.
--
-- Setting LGI_SHIM_CDIR to a directory holding lgishim_*.so points package.cpath
-- at the shim (see docs/sam/scripts/12-spec-shim-loader.sh). Without it the
-- require misses and the suite runs on lua-lgi, which is the historic behaviour.
local cdir = os.getenv("LGI_SHIM_CDIR")
if cdir and #cdir > 0 then
    package.cpath = cdir .. "/?.so;" .. package.cpath
end

local lgi = require("vendor.lgishim")
if lgi.shim then
    package.preload["lgi"] = function() return lgi end
end
require "lgi"

-- Always show deprecated messages
_G.awesome = {
    version = "v9999",
    api_level = 9999,
}

-- "fix" some intentional beautiful breakage done by .travis.yml
require("beautiful").init { a_key = "a_value" }

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
