cmake_minimum_required(VERSION 3.16)
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

set(STUDY_SOURCES
  01_memory_ownership.cpp
  02_polymorphism.cpp
  03_rtti_isa_dyncast.cpp
  04_ir_data_structures.cpp
  05_ssa_construction.cpp
  06_dataflow_analysis.cpp
  07_register_allocation.cpp
)

enable_testing()
foreach(src ${STUDY_SOURCES})
  get_filename_component(target ${src} NAME_WE)
  add_executable(${target} ${src})
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${target} COMMAND ${target})
endforeach()

# Optional: prove the isa<>/dyn_cast<> machinery works without RTTI, the way
# LLVM actually builds. The dynamic_cast comparison block compiles out.
if(NOT MSVC)
  add_executable(03_no_rtti 03_rtti_isa_dyncast.cpp)
  target_include_directories(03_no_rtti PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(03_no_rtti PRIVATE -fno-rtti)
  add_test(NAME 03_no_rtti COMMAND 03_no_rtti)
endif()