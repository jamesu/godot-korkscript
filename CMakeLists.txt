cmake_minimum_required(VERSION 3.16)

set(GODOT_CPP_DIR "" CACHE PATH "Path to a godot-cpp checkout for the Godot extension")
set(KS_BUILD_MODE "static" CACHE STRING "korkscript build mode" FORCE)

add_subdirectory(ext/korkscript EXCLUDE_FROM_ALL)

if (TARGET godot-cpp)
  set(KORKSCRIPT_GODOT_CPP_TARGET godot-cpp)
elseif(TARGET godot::cpp)
  set(KORKSCRIPT_GODOT_CPP_TARGET godot::cpp)
elseif(GODOT_CPP_DIR)
  add_subdirectory("${GODOT_CPP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp")
  if (TARGET godot-cpp)
    set(KORKSCRIPT_GODOT_CPP_TARGET godot-cpp)
  elseif(TARGET godot::cpp)
    set(KORKSCRIPT_GODOT_CPP_TARGET godot::cpp)
  endif()
else()
  find_package(godot-cpp CONFIG REQUIRED)
  if (TARGET godot-cpp)
    set(KORKSCRIPT_GODOT_CPP_TARGET godot-cpp)
  elseif(TARGET godot::cpp)
    set(KORKSCRIPT_GODOT_CPP_TARGET godot::cpp)
  endif()
endif()

if (NOT KORKSCRIPT_GODOT_CPP_TARGET)
  message(FATAL_ERROR "Could not locate a godot-cpp target. Set GODOT_CPP_DIR or install a godot-cpp CMake package.")
endif()

set(KORKSCRIPT_GODOT_ENGINE_SRCS)
foreach(KORKSCRIPT_SRC IN LISTS KS_SRCS)
  if (IS_ABSOLUTE "${KORKSCRIPT_SRC}")
    list(APPEND KORKSCRIPT_GODOT_ENGINE_SRCS "${KORKSCRIPT_SRC}")
  else()
    list(APPEND KORKSCRIPT_GODOT_ENGINE_SRCS "${CMAKE_SOURCE_DIR}/${KORKSCRIPT_SRC}")
  endif()
endforeach()

add_library(korkscript_godot SHARED
  KorkScript.cpp
  KorkScriptLanguage.cpp
  KorkScriptVMNode.cpp
  KorkScriptVMHost.cpp
  gdextension_entry.cpp
  register_types.cpp
)

target_link_libraries(korkscript_godot PRIVATE korkscript_ks ${KORKSCRIPT_GODOT_CPP_TARGET})
target_sources(korkscript_godot PRIVATE ${KORKSCRIPT_GODOT_ENGINE_SRCS})
target_include_directories(korkscript_godot PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/engine")

set_target_properties(korkscript_godot PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  PREFIX ""
  OUTPUT_NAME "korkscript_godot"
)

set(KORKSCRIPT_GODOT_ADDON_DIR "${CMAKE_BINARY_DIR}/godot-addon/addons/korkscript" CACHE PATH
  "Staging directory for the built Godot addon")

add_custom_command(
  TARGET korkscript_godot POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${KORKSCRIPT_GODOT_ADDON_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:korkscript_godot>"
          "${KORKSCRIPT_GODOT_ADDON_DIR}/$<TARGET_FILE_NAME:korkscript_godot>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/addons/korkscript/korkscript.gdextension"
          "${KORKSCRIPT_GODOT_ADDON_DIR}/korkscript.gdextension"
  COMMENT "Staging Godot addon in ${KORKSCRIPT_GODOT_ADDON_DIR}"
)

add_custom_target(stage_korkscript_godot_addon
  DEPENDS korkscript_godot
  COMMENT "Build and stage the korkscript Godot addon"
)
