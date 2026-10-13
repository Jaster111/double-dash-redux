cmake_minimum_required(VERSION 3.20)
project(doubledash CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB GENERATED_SOURCES ${CMAKE_SOURCE_DIR}/build/recomp/*.cpp)
file(GLOB_RECURSE RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/*.cpp)

# generated code is huge: compile it as its own library with fast settings
add_library(recomp STATIC ${GENERATED_SOURCES})
target_include_directories(recomp PUBLIC ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/build/recomp)
if(MSVC)
  target_compile_options(recomp PRIVATE /MP /O2 /Ob1 /GS- /bigobj /wd4102 /wd4146 /wd4244)
endif()
option(DD_SPCHECK "verify r1 is balanced across every guest call (debugging)" ON)
if(DD_SPCHECK)
  target_compile_definitions(recomp PRIVATE DD_SPCHECK)
endif()

add_executable(doubledash ${RUNTIME_SOURCES})
target_link_libraries(doubledash PRIVATE recomp d3d11 dxgi d3dcompiler xinput winmm ole32 windowscodecs)
target_include_directories(doubledash PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/build/recomp)
if(MSVC)
  target_compile_options(doubledash PRIVATE /MP /W3 /wd4244 /wd4267 /wd4996 /wd4146)
  target_compile_definitions(doubledash PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  set_target_properties(doubledash PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  # PDB for the profiler / crash logs (DD_PROF); does not affect code generation
  target_compile_options(doubledash PRIVATE /Zi)
  target_link_options(doubledash PRIVATE /STACK:16777216 /DEBUG /OPT:REF /OPT:ICF)
endif()
