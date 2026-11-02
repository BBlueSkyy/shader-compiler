cmake_minimum_required(VERSION 3.16)
project(surface_atomic_semantics CXX)
set(CMAKE_CXX_STANDARD 20)
if(NOT DEPS)
  message(FATAL_ERROR "Pass -DDEPS=/path/to/app/libraries")
endif()
set(FMT_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/fmt" fmt)
add_subdirectory("${DEPS}/sirit" sirit)
get_filename_component(root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(GLOB boost_inc "${DEPS}/boost/libs/*/include")
file(GLOB ir "${root}/frontend/ir/*.cpp")
file(GLOB spirv "${root}/backend/spirv/*.cpp")
add_executable(surface_atomics test.cpp ${ir} ${spirv}
  "${root}/frontend/maxwell/decode.cpp"
  "${root}/frontend/maxwell/opcodes.cpp"
  "${root}/frontend/maxwell/translate/impl/impl.cpp"
  "${root}/frontend/maxwell/translate/impl/surface_atomic_operations.cpp"
  "${root}/ir_opt/ssa_rewrite_pass.cpp"
  "${root}/ir_opt/texture_pass.cpp"
  "${root}/ir_opt/constant_propagation_pass.cpp"
  "${root}/ir_opt/identity_removal_pass.cpp"
  "${root}/ir_opt/dead_code_elimination_pass.cpp"
  "${root}/ir_opt/collect_shader_info_pass.cpp")
target_include_directories(surface_atomics PRIVATE "${root}/include" ${boost_inc}
  "${DEPS}/range/include" "${DEPS}/robin-map/include" ${EXTRA_INCLUDE})
target_compile_options(surface_atomics PRIVATE -include cstddef -fno-strict-aliasing
  -ffunction-sections -fdata-sections)
target_link_options(surface_atomics PRIVATE -Wl,--gc-sections)
target_link_libraries(surface_atomics PRIVATE fmt::fmt sirit)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
  target_compile_options(surface_atomics PRIVATE -Wno-changes-meaning)
endif()
enable_testing()
add_test(NAME surface_atomics COMMAND surface_atomics "${CMAKE_CURRENT_BINARY_DIR}/spirv")
set_tests_properties(surface_atomics PROPERTIES FIXTURES_SETUP surface_atomic_modules)
find_package(Python3 COMPONENTS Interpreter)
find_program(SPIRV_VAL_EXECUTABLE spirv-val)
if(Python3_FOUND AND SPIRV_VAL_EXECUTABLE)
  add_test(NAME surface_atomics_spirv
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_spirv.py"
      --directory "${CMAKE_CURRENT_BINARY_DIR}/spirv" --validator "${SPIRV_VAL_EXECUTABLE}")
  set_tests_properties(surface_atomics_spirv PROPERTIES FIXTURES_REQUIRED surface_atomic_modules)
endif()
