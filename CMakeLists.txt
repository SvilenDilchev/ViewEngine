cmake_minimum_required(VERSION 3.28)
project(ViewEngine)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

############################## Custom build modes ###############################

set(CMAKE_CXX_FLAGS_SANITIZE "-fno-omit-frame-pointer -fsanitize=address,signed-integer-overflow,null,alignment,bounds,function,return,vla-bound -O0 -g"
  CACHE STRING "Flags used by the C++ compiler during Sanitize builds." FORCE)
set(CMAKE_C_FLAGS_SANITIZE "-fno-omit-frame-pointer -fsanitize=address,signed-integer-overflow,null,alignment,bounds,function,return,vla-bound -O0 -g"
  CACHE STRING "Flags used by the C compiler during Sanitize builds." FORCE)
set(CMAKE_EXE_LINKER_FLAGS_SANITIZE
  ${CMAKE_EXE_LINKER_FLAGS_DEBUG} CACHE STRING
  "Flags used for linking binaries during Sanitize builds." FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_SANITIZE
  ${CMAKE_SHARED_LINKER_FLAGS_DEBUG} CACHE STRING
  "Flags used by the shared libraries linker during Sanitize builds." FORCE)
mark_as_advanced(
  CMAKE_CXX_FLAGS_SANITIZE        CMAKE_EXE_LINKER_FLAGS_SANITIZE
  CMAKE_C_FLAGS_SANITIZE          CMAKE_SHARED_LINKER_FLAGS_SANITIZE)

set(CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE STRING
  "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel Sanitize."
  FORCE)

############################### External Projects ###############################

set(CMAKE_POSITION_INDEPENDENT_CODE TRUE)

if(MSVC)
  set(BOSS_LINK_LIBRARY_PREFIX ${CMAKE_LINK_LIBRARY_PREFIX})
  set(BOSS_LINK_LIBRARY_SUFFIX ${CMAKE_LINK_LIBRARY_SUFFIX})
else()
  set(BOSS_LINK_LIBRARY_PREFIX ${CMAKE_SHARED_LIBRARY_PREFIX})
  set(BOSS_LINK_LIBRARY_SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX})
endif()

include(ExternalProject)

# Configuration for BOSS
set(BOSS_SOURCE_BRANCH main CACHE STRING "If BOSS core is built from source, which branch should be fetched")
set(BOSS_SOURCE_REPOSITORY https://github.com/symbol-store/BOSS.git CACHE STRING "URL for the BOSS repository")

ExternalProject_Add(BOSS
  GIT_REPOSITORY ${BOSS_SOURCE_REPOSITORY}
  DOWNLOAD_DIR $ENV{HOME}/.cmake-downloads/${CMAKE_PROJECT_NAME}
  GIT_TAG ${BOSS_SOURCE_BRANCH}
  GIT_SHALLOW ON
  # Force update from remote on every build
  UPDATE_COMMAND git fetch origin ${BOSS_SOURCE_BRANCH} && git reset --hard origin/${BOSS_SOURCE_BRANCH}
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${ViewEngine_BINARY_DIR}/deps
    $<IF:$<CONFIG:>,,-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}>
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
    -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
    -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
  BUILD_BYPRODUCTS
    ${ViewEngine_BINARY_DIR}/deps/lib/${BOSS_LINK_LIBRARY_PREFIX}BOSS${BOSS_LINK_LIBRARY_SUFFIX}
)

#################################### Targets ####################################

if(MSVC)
  set(LibraryType SHARED)
else()
  set(LibraryType MODULE)
endif()

add_library(ViewEngine ${LibraryType}
  Source/ViewEngine.cpp
  Source/ViewRegistry.cpp
  Source/QueryRewriter.cpp
  Source/MetadataRegistry.cpp
  Source/Cache.cpp
)

set_property(TARGET ViewEngine PROPERTY CXX_STANDARD 23)
if(MSVC)
  target_compile_options(ViewEngine PUBLIC "/Zc:__cplusplus")
  target_compile_options(ViewEngine PUBLIC "/EHsc")
endif()

target_include_directories(ViewEngine SYSTEM PUBLIC ${ViewEngine_BINARY_DIR}/deps/include)
target_link_libraries(ViewEngine PUBLIC
  ${ViewEngine_BINARY_DIR}/deps/lib/${BOSS_LINK_LIBRARY_PREFIX}BOSS${BOSS_LINK_LIBRARY_SUFFIX})

add_dependencies(ViewEngine BOSS)

set_target_properties(ViewEngine PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
install(TARGETS ViewEngine LIBRARY DESTINATION lib)