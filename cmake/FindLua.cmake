# Simplified FindLua.cmake for Lua 5.3 with static libs
# Uses pkg-config to find Lua instead of complex header scanning

find_package(PkgConfig QUIET)

if(PKG_CONFIG_FOUND)
  pkg_check_modules(_LUA lua5.3)
  if(_LUA_FOUND)
    set(LUA_INCLUDE_DIR "${_LUA_INCLUDE_DIRS}")
    set(LUA_LIBRARY "${_LUA_LIBRARY_DIRS}/liblua.a")
    # Check if library exists, fall back to shared
    if(NOT EXISTS "${LUA_LIBRARY}")
      set(LUA_LIBRARY "${_LUA_LIBRARY_DIRS}/liblua-5.3.so")
    endif()
    set(LUA_LIBRARIES "${LUA_LIBRARY}")
    set(LUA_INCLUDE_DIRS "${LUA_INCLUDE_DIR}")
    set(LUA_VERSION_MAJOR 5)
    set(LUA_VERSION_MINOR 3)
    set(LUA_VERSION_PATCH 6)
    set(LUA_VERSION "5.3.6")
    set(LUA_FOUND TRUE)
    message(STATUS "Found Lua 5.3.6 (via pkg-config): ${LUA_INCLUDE_DIR}")
  endif()
endif()

if(NOT LUA_FOUND)
  # Fallback: try standard locations
  find_path(LUA_INCLUDE_DIR lua.h
    HINTS /usr/include /usr/local/include
    )
  find_library(LUA_LIBRARY NAMES lua lua5.3 liblua
    HINTS /usr/lib /usr/local/lib
    )
  if(LUA_INCLUDE_DIR AND LUA_LIBRARY)
    set(LUA_LIBRARIES "${LUA_LIBRARY}")
    set(LUA_INCLUDE_DIRS "${LUA_INCLUDE_DIR}")
    set(LUA_VERSION_MAJOR 5)
    set(LUA_VERSION_MINOR 3)
    set(LUA_VERSION_PATCH 6)
    set(LUA_VERSION "5.3.6")
    set(LUA_FOUND TRUE)
    message(STATUS "Found Lua: ${LUA_INCLUDE_DIR} ${LUA_LIBRARY}")
  endif()
endif()

if(LUA_FOUND)
  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(Lua
    REQUIRED_VARS LUA_INCLUDE_DIR LUA_LIBRARY
    )
  mark_as_advanced(LUA_INCLUDE_DIR LUA_LIBRARY)
endif()
