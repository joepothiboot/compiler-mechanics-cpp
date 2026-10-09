cmake_minimum_required(VERSION 3.20) # 3.20: first version that knows CXX_STANDARD 23
project(cpp_for_compilers CXX)

# C++17: LLVM's required baseline. Building your study code with the same
# standard the upstream project uses avoids learning habits you'd have to unlearn.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a build where assert() is live — LLVM development is done in
# assertions builds, and file 03's cast<> checks depend on them.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Every .cpp under cpp/ and compiler/ is a standalone study program + test. Drop a
# new file into a group folder and re-run cmake; no list to maintain.
file(GLOB_RECURSE STUDY_SOURCES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
  cpp/*.cpp
  compiler/*.cpp
)

enable_testing()
foreach(src ${STUDY_SOURCES})
  # cpp/techniques/memory_layout/01_bump_allocator.cpp -> memory_layout.01_bump_allocator
  # (group prefix because numbering restarts in each group folder).
  get_filename_component(name ${src} NAME_WE)
  get_filename_component(dir ${src} DIRECTORY)
  get_filename_component(group ${dir} NAME)
  if(src MATCHES "^cpp/techniques/")
    set(target ${group}.${name})
  else()
    set(target ${name})
  endif()
  add_executable(${target} ${src})
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # backend/ is deliberately newer C++ (std::format, std::ranges::contains,
  # designated initializers); everything else stays on LLVM's C++17 baseline.
  if(src MATCHES "^compiler/backend/")
    set_target_properties(${target} PROPERTIES CXX_STANDARD 23)
  endif()
  add_test(NAME ${target} COMMAND ${target})
endforeach()

# Optional: prove the isa<>/dyn_cast<> machinery works without RTTI, the way
# LLVM actually builds. The dynamic_cast comparison block compiles out.
if(NOT MSVC)
  add_executable(03_no_rtti cpp/language/03_rtti_isa_dyncast.cpp)
  target_include_directories(03_no_rtti PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(03_no_rtti PRIVATE -fno-rtti)
  add_test(NAME 03_no_rtti COMMAND 03_no_rtti)
endif()