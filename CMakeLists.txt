cmake_minimum_required(VERSION 3.28)
project(ViewEngine)

# Automatically generate compile_commands.json for your language server (clangd)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

############################## Custom build modes ###############################

set(CMAKE_CXX_FLAGS_SANITIZE "-fno-omit-frame-pointer -fsanitize=address,signed-integer-overflow,null,alignment,bounds,function,return,vla-bound -O0 -g"
  CACHE STRING
  "Flags used by the C++ compiler during Sanitize builds."
  FORCE)
set(CMAKE_C_FLAGS_SANITIZE "-fno-omit-frame-pointer -fsanitize=address,signed-integer-overflow,null,alignment,bounds,function,return,vla-bound -O0 -g"
  CACHE STRING
  "Flags used by the C compiler during Sanitize builds."
  FORCE)
set(CMAKE_EXE_LINKER_FLAGS_SANITIZE
  ${CMAKE_EXE_LINKER_FLAGS_DEBUG} CACHE STRING
  "Flags used for linking binaries during Sanitize builds."
  FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_SANITIZE
  ${CMAKE_SHARED_LINKER_FLAGS_DEBUG} CACHE STRING
  "Flags used by the shared libraries linker during Sanitize builds."
  FORCE)
mark_as_advanced(
  CMAKE_CXX_FLAGS_SANITIZE        CMAKE_EXE_LINKER_FLAGS_SANITIZE
  CMAKE_C_FLAGS_SANITIZE          CMAKE_SHARED_LINKER_FLAGS_SANITIZE
  )

set(CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE STRING
  "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel Sanitize."
  FORCE)

# REMOVED: Hardcoded CMAKE_CXX_COMPILER and CMAKE_C_COMPILER lines here 
# so they don't overwrite your command-line choices.

############################### External Projects ###############################

set(BOSS_SOURCE_DIR "/mnt/apps/lazy-loading/BOSS" CACHE PATH "Path to local BOSS source code")
set(BOSS_BUILD_DIR "${BOSS_SOURCE_DIR}/build" CACHE PATH "Path to BOSS build directory")

option(BOSS_USE_PREBUILT "Use pre-built BOSS instead of building from source" OFF)

if(BOSS_USE_PREBUILT)
  message(STATUS "Using pre-built BOSS from ${BOSS_BUILD_DIR}")
else()
  message(STATUS "Building BOSS from source at ${BOSS_SOURCE_DIR}")
  include(ExternalProject)
  ExternalProject_Add(BOSS
    SOURCE_DIR "${BOSS_SOURCE_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=${ViewEngine_BINARY_DIR}/deps
      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}  # FIXED: Dynamically matches the host toolchain
      -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}      # FIXED: Dynamically matches the host toolchain
      -DCMAKE_CXX_STANDARD=23
      -DCMAKE_CXX_STANDARD_REQUIRED=ON
      -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
  )
  set(BOSS_BUILD_DIR ${ViewEngine_BINARY_DIR}/deps)
endif()

#################################### Targets ####################################

if(MSVC)
  set(LibraryType SHARED)
else()
  set(LibraryType MODULE)
endif(MSVC)

add_library(ViewEngine ${LibraryType} Source/ViewEngine.cpp)

set_property(TARGET ViewEngine PROPERTY CXX_STANDARD 23)
if(MSVC)
  target_compile_options(ViewEngine PUBLIC "/Zc:__cplusplus")
  target_compile_options(ViewEngine PUBLIC "/EHsc")
endif(MSVC)

if(BOSS_USE_PREBUILT)
  target_include_directories(ViewEngine SYSTEM PUBLIC ${BOSS_SOURCE_DIR}/Source)
  target_link_libraries(ViewEngine PUBLIC ${BOSS_BUILD_DIR}/libBOSS.so)
else()
  target_include_directories(ViewEngine SYSTEM PUBLIC ${BOSS_BUILD_DIR}/include)
  target_link_libraries(ViewEngine PUBLIC ${BOSS_BUILD_DIR}/lib/libBOSS.so)
  add_dependencies(ViewEngine BOSS)
endif()

set_target_properties(ViewEngine PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
install(TARGETS ViewEngine LIBRARY DESTINATION lib)