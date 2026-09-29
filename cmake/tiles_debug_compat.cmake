# tiles_debug_compat.cmake — run at install time via install(SCRIPT) (P13).
#
# vcpkg's imported targets reference per-config DEBUG library paths, but this
# SDK installs release third-party libs only. Create compatibility symlinks so
# the DEBUG imported locations resolve to the release archives:
#   1. <prefix>/debug -> .            (so <prefix>/debug/lib/X == <prefix>/lib/X)
#   2. <prefix>/lib/libfoo[d|-d].a -> libfoo.a   (vcpkg debug-suffixed names)
#
# Debug-info fidelity is NOT claimed: consumers building in Debug link the
# release archives. This is documented in the SDK packaging notes.

set(_tiles_prefix "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")

# 1. debug/ -> .
if(NOT EXISTS "${_tiles_prefix}/debug")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E create_symlink "." "${_tiles_prefix}/debug"
    RESULT_VARIABLE _tiles_rc)
  if(NOT _tiles_rc EQUAL 0)
    message(WARNING "[P13] could not create <prefix>/debug symlink (rc=${_tiles_rc})")
  endif()
endif()

# 2. debug-suffixed archive names: scan installed *-targets.cmake files for
#    ${_IMPORT_PREFIX}/debug/lib/<name>.a references and link each missing
#    name to its release counterpart (strip trailing 'd' / '-d').
file(GLOB_RECURSE _tiles_cfgs
  "${_tiles_prefix}/share/*targets*.cmake"
  "${_tiles_prefix}/share/*Target*.cmake")
set(_tiles_names "")
foreach(_c IN LISTS _tiles_cfgs)
  file(STRINGS "${_c}" _tiles_refs
    REGEX "\\\$\\{_IMPORT_PREFIX\\}/debug/lib/[A-Za-z0-9_+.-]+\\.a")
  foreach(_r IN LISTS _tiles_refs)
    string(REGEX MATCH "debug/lib/([A-Za-z0-9_+.-]+\\.a)" _tiles_m "${_r}")
    if(_tiles_m)
      list(APPEND _tiles_names "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endforeach()
list(REMOVE_DUPLICATES _tiles_names)

foreach(_n IN LISTS _tiles_names)
  if(NOT EXISTS "${_tiles_prefix}/lib/${_n}")
    string(REGEX REPLACE "(d|-d)\\.a$" ".a" _tiles_cand "${_n}")
    if(EXISTS "${_tiles_prefix}/lib/${_tiles_cand}")
      execute_process(
        COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "${_tiles_cand}" "${_tiles_prefix}/lib/${_n}"
        RESULT_VARIABLE _tiles_rc)
      if(NOT _tiles_rc EQUAL 0)
        message(WARNING "[P13] could not link ${_n} -> ${_tiles_cand} (rc=${_tiles_rc})")
      endif()
    else()
      message(WARNING "[P13] no release counterpart for debug lib '${_n}'; "
        "consumers may hit a missing-file error in find_package")
    endif()
  endif()
endforeach()

unset(_tiles_prefix)
unset(_tiles_cfgs)
unset(_tiles_names)
unset(_tiles_rc)
unset(_tiles_cand)
unset(_tiles_m)
