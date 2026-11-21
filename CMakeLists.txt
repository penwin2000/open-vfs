cmake_minimum_required(VERSION 3.25)

project(open_vfs
    VERSION 0.1.0
    DESCRIPTION "Clean-room C++20 / DirectX 12 standalone virtual filesystem and runtime"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Shared compile options for isolated subsystem targets
add_library(open_vfs_common INTERFACE)
target_include_directories(open_vfs_common INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(MSVC)
    target_compile_options(open_vfs_common INTERFACE /W4 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(open_vfs_common INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ----------------------------------------------------------------------------
# Partitioned Subsystem Libraries (8 isolated modules)
# ----------------------------------------------------------------------------

add_library(open_vfs_core STATIC
    src/vfs_core/vfs_core.cpp
)
target_link_libraries(open_vfs_core PUBLIC open_vfs_common)

add_library(open_vfs_rpf9 STATIC
    src/rpf9/rpf9.cpp
)
target_link_libraries(open_vfs_rpf9 PUBLIC open_vfs_core)

add_library(open_vfs_rds5 STATIC
    src/rds5/rds5.cpp
)
target_link_libraries(open_vfs_rds5 PUBLIC open_vfs_core)

add_library(open_vfs_xvd STATIC
    src/xvd/xvd.cpp
)
target_link_libraries(open_vfs_xvd PUBLIC open_vfs_core)

add_library(open_vfs_render STATIC
    src/render/render.cpp
)
target_link_libraries(open_vfs_render PUBLIC open_vfs_common)
if(WIN32)
    target_link_libraries(open_vfs_render PUBLIC user32 d3d12 dxgi)
endif()

add_library(open_vfs_audio STATIC
    src/audio/audio.cpp
)
target_link_libraries(open_vfs_audio PUBLIC open_vfs_common)
if(WIN32)
    target_link_libraries(open_vfs_audio PUBLIC ole32)
endif()

add_library(open_vfs_input STATIC
    src/input/input.cpp
)
target_link_libraries(open_vfs_input PUBLIC open_vfs_common)
if(WIN32)
    target_link_libraries(open_vfs_input PUBLIC user32)
endif()

add_library(open_vfs_world_vm STATIC
    src/world_vm/world_vm.cpp
)
target_link_libraries(open_vfs_world_vm PUBLIC open_vfs_core)

# ----------------------------------------------------------------------------
# Standalone Desktop Executable (open-vfs.exe)
# ----------------------------------------------------------------------------

add_executable(open-vfs
    app/main.cpp
)
target_link_libraries(open-vfs PRIVATE
    open_vfs_core
    open_vfs_rpf9
    open_vfs_rds5
    open_vfs_xvd
    open_vfs_render
    open_vfs_audio
    open_vfs_input
    open_vfs_world_vm
)

# ----------------------------------------------------------------------------
# Synthetic-Buffer CTest Unit Test Suite (one isolated binary per module)
# ----------------------------------------------------------------------------

include(CTest)

if(BUILD_TESTING)
    function(open_vfs_add_module_test test_name source_file target_lib)
        add_executable(${test_name} ${source_file})
        target_link_libraries(${test_name} PRIVATE ${target_lib})
        add_test(NAME ${test_name} COMMAND ${test_name})
    endfunction()

    open_vfs_add_module_test(test_vfs_core tests/test_vfs_core.cpp open_vfs_core)
    open_vfs_add_module_test(test_rpf9     tests/test_rpf9.cpp     open_vfs_rpf9)
    open_vfs_add_module_test(test_rds5     tests/test_rds5.cpp     open_vfs_rds5)
    open_vfs_add_module_test(test_xvd      tests/test_xvd.cpp      open_vfs_xvd)
    open_vfs_add_module_test(test_render   tests/test_render.cpp   open_vfs_render)
    open_vfs_add_module_test(test_audio    tests/test_audio.cpp    open_vfs_audio)
    open_vfs_add_module_test(test_input    tests/test_input.cpp    open_vfs_input)
    open_vfs_add_module_test(test_world_vm tests/test_world_vm.cpp open_vfs_world_vm)
endif()
