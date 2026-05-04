cmake_minimum_required(VERSION 3.28)
project(ViewEngine)

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

set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER clang)

############################### External Projects ###############################

include(ExternalProject)
set(BOSS_SOURCE_DIR "/mnt/apps/lazy-loading/BOSS" CACHE PATH "Path to local BOSS source code")
ExternalProject_Add(BOSS
  SOURCE_DIR "${BOSS_SOURCE_DIR}"
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  CMAKE_ARGS 
    -DCMAKE_INSTALL_PREFIX=${ViewEngine_BINARY_DIR}/deps 
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    # Force the compiler and standard for the external project
    -DCMAKE_CXX_COMPILER=clang++ 
    -DCMAKE_C_COMPILER=clang
    -DCMAKE_CXX_STANDARD=23        # <--- Explicitly force C++23
    -DCMAKE_CXX_STANDARD_REQUIRED=ON
    -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
)


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

# Ensure we can find the headers that BOSS will install
target_include_directories(ViewEngine SYSTEM PUBLIC ${ViewEngine_BINARY_DIR}/deps/include)
add_dependencies(ViewEngine BOSS)

set_target_properties(ViewEngine PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
install(TARGETS ViewEngine LIBRARY DESTINATION lib)