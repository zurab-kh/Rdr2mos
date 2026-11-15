# RDR2Coop.asi - the Script Hook RDR2 plugin (Windows x64).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "RDR2 is 64-bit: configure an x64 build")
endif()

# Import library for ScriptHookRDR2.dll, generated from the .def file (no SDK download needed).
if(MSVC)
  set(SH_DEF ${CMAKE_CURRENT_SOURCE_DIR}/ScriptHookRDR2.def)
  set(SH_IMPLIB ${CMAKE_CURRENT_BINARY_DIR}/ScriptHookRDR2.lib)
  add_custom_command(OUTPUT ${SH_IMPLIB}
    COMMAND lib /nologo /def:${SH_DEF} /out:${SH_IMPLIB} /machine:x64
    DEPENDS ${SH_DEF} VERBATIM)
else()
  if(NOT CMAKE_DLLTOOL)
    find_program(CMAKE_DLLTOOL NAMES ${CMAKE_CXX_COMPILER_TARGET}-dlltool x86_64-w64-mingw32-dlltool dlltool REQUIRED)
  endif()
  set(SH_DEF ${CMAKE_CURRENT_SOURCE_DIR}/ScriptHookRDR2.mingw.def)
  set(SH_IMPLIB ${CMAKE_CURRENT_BINARY_DIR}/libScriptHookRDR2.a)
  add_custom_command(OUTPUT ${SH_IMPLIB}
    COMMAND ${CMAKE_DLLTOOL} -d ${SH_DEF} -l ${SH_IMPLIB} -m i386:x86-64
    DEPENDS ${SH_DEF} VERBATIM)
endif()
add_custom_target(scripthook_implib DEPENDS ${SH_IMPLIB})

add_library(RDR2Coop SHARED main.cpp rdr2_game.cpp saves.cpp)
add_dependencies(RDR2Coop scripthook_implib)
target_link_libraries(RDR2Coop PRIVATE coop_core ${SH_IMPLIB} shell32)
target_compile_definitions(RDR2Coop PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(RDR2Coop PROPERTIES PREFIX "" SUFFIX ".asi")
if(MINGW)
  target_link_options(RDR2Coop PRIVATE -static)
endif()

# Ship the default settings next to the plugin.
add_custom_command(TARGET RDR2Coop POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/RDR2Coop.ini $<TARGET_FILE_DIR:RDR2Coop>)
