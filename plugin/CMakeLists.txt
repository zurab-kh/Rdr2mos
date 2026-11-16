# RDR2Coop.asi - the Script Hook RDR2 plugin (Windows x64).
# ScriptHookRDR2.dll is bound at load time (see scripthook.cpp): no SDK or import library needed.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "RDR2 is 64-bit: configure an x64 build")
endif()

add_library(RDR2Coop SHARED main.cpp rdr2_game.cpp saves.cpp scripthook.cpp)
target_link_libraries(RDR2Coop PRIVATE coop_core shell32)
target_compile_definitions(RDR2Coop PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(RDR2Coop PROPERTIES PREFIX "" SUFFIX ".asi")
if(MINGW)
  target_link_options(RDR2Coop PRIVATE -static)
endif()

# Ship the default settings next to the plugin.
add_custom_command(TARGET RDR2Coop POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/RDR2Coop.ini $<TARGET_FILE_DIR:RDR2Coop>)
