add_library(dolphin-nogui
  SHARED
  Platform.cpp
  Platform.h
  PlatformHeadless.cpp
  MainNoGUI.cpp
  blip_buf.c
  blip_buf.h
)

if(ENABLE_X11 AND X11_FOUND)
  target_sources(dolphin-nogui PRIVATE PlatformX11.cpp)
endif()

if(WIN32)
  target_sources(dolphin-nogui PRIVATE PlatformWin32.cpp)
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  target_sources(dolphin-nogui PRIVATE PlatformFBDev.cpp)
endif()

set_target_properties(dolphin-nogui PROPERTIES OUTPUT_NAME dolphin-emu-nogui)

target_link_libraries(dolphin-nogui
PRIVATE
  core
  uicommon
  cpp-optparse
)

if(MSVC)
# We need to disable PCH for this one file, because it's C and our PCH is C++
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/blip_buf.c
  PROPERTIES COMPILE_FLAGS "/Y- /I${CMAKE_SOURCE_DIR}/Source/PCH/nopch")
endif()

if(USE_DISCORD_PRESENCE)
  target_compile_definitions(dolphin-nogui PRIVATE -DUSE_DISCORD_PRESENCE)
endif()

set(CPACK_PACKAGE_EXECUTABLES ${CPACK_PACKAGE_EXECUTABLES} dolphin-nogui)
install(TARGETS dolphin-nogui RUNTIME DESTINATION ${bindir})

