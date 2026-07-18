cmake_minimum_required(VERSION 3.20)
project(shadps4_gcn_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(SHADPS4_SOURCE_DIR "C:/Users/user1/Projetos/shadPS4" CACHE PATH
    "Path to the shadPS4 source tree")

set(SHADPS4_GENERATED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SHADPS4_GENERATED_INCLUDE}/shader_recompiler")
file(READ "${SHADPS4_SOURCE_DIR}/src/shader_recompiler/info.h"
     SHADPS4_INFO_HEADER)
string(REPLACE "#pragma once" [=[#pragma once
#include "bridge-memory.h"]=] SHADPS4_INFO_HEADER "${SHADPS4_INFO_HEADER}")
set(SHADPS4_READ_UD_ORIGINAL [=[        T data;
        const u32* base = user_data.data();
        if (ptr_index != IR::NumScalarRegs) {
            std::memcpy(&base, &user_data[ptr_index], sizeof(base));
            base = reinterpret_cast<const u32*>(VAddr(base) & 0xFFFFFFFFFFFFULL);
        }
        std::memcpy(&data, base + dword_offset, sizeof(T));
        return data;]=])
set(SHADPS4_READ_UD_BROKERED [=[        T data{};
        if (ptr_index == IR::NumScalarRegs) {
            std::memcpy(&data, user_data.data() + dword_offset, sizeof(T));
            return data;
        }
        if (ptr_index + 1 >= user_data.size()) {
            return data;
        }
        u64 address = 0;
        std::memcpy(&address, &user_data[ptr_index], sizeof(address));
        address = (address & 0xFFFFFFFFFFFFULL) +
                  static_cast<u64>(dword_offset) * sizeof(u32);
        shadps4_gcn_bridge_read_memory(address, &data, sizeof(T));
        return data;]=])
string(REPLACE "${SHADPS4_READ_UD_ORIGINAL}" "${SHADPS4_READ_UD_BROKERED}"
       SHADPS4_INFO_HEADER "${SHADPS4_INFO_HEADER}")
set(SHADPS4_TESS_READ_ORIGINAL [=[        VAddr tess_constants_addr = buf.base_address;
        memcpy(&tess_constants,
               reinterpret_cast<TessellationDataConstantBuffer*>(tess_constants_addr),
               sizeof(tess_constants));]=])
set(SHADPS4_TESS_READ_BROKERED [=[        VAddr tess_constants_addr = buf.base_address;
        shadps4_gcn_bridge_read_memory(tess_constants_addr, &tess_constants,
                                       sizeof(tess_constants));]=])
string(REPLACE "${SHADPS4_TESS_READ_ORIGINAL}" "${SHADPS4_TESS_READ_BROKERED}"
       SHADPS4_INFO_HEADER "${SHADPS4_INFO_HEADER}")
file(WRITE "${SHADPS4_GENERATED_INCLUDE}/shader_recompiler/info.h"
     "${SHADPS4_INFO_HEADER}")

file(GLOB_RECURSE SHADPS4_SHADER_SOURCES CONFIGURE_DEPENDS
     "${SHADPS4_SOURCE_DIR}/src/shader_recompiler/*.cpp")
list(REMOVE_ITEM SHADPS4_SHADER_SOURCES
    "${SHADPS4_SOURCE_DIR}/src/shader_recompiler/frontend/fetch_shader.cpp"
    "${SHADPS4_SOURCE_DIR}/src/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp"
    "${SHADPS4_SOURCE_DIR}/src/shader_recompiler/ir/program.cpp")
file(GLOB_RECURSE SIRIT_SOURCES CONFIGURE_DEPENDS
     "${SHADPS4_SOURCE_DIR}/externals/sirit/src/*.cpp")

add_library(shadps4_gcn_bridge SHARED
    shadps4-gcn-bridge.cpp
    fetch_shader_uwp.cpp
    flatten_extended_userdata_uwp.cpp
    program_uwp.cpp
    ${SHADPS4_SHADER_SOURCES}
    ${SIRIT_SOURCES}
    "${SHADPS4_SOURCE_DIR}/src/video_core/amdgpu/pixel_format.cpp")
target_sources(shadps4_gcn_bridge PRIVATE
    "${SHADPS4_SOURCE_DIR}/src/video_core/amdgpu/tiling.cpp")

target_compile_definitions(shadps4_gcn_bridge PRIVATE
    FMT_HEADER_ONLY
    SHADPS4_GCN_BRIDGE_BUILD
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

target_include_directories(shadps4_gcn_bridge PRIVATE
    "${SHADPS4_GENERATED_INCLUDE}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${SHADPS4_SOURCE_DIR}"
    "${SHADPS4_SOURCE_DIR}/src"
    "${SHADPS4_SOURCE_DIR}/externals/fmt/include"
    "${SHADPS4_SOURCE_DIR}/externals/magic_enum/include"
    "${SHADPS4_SOURCE_DIR}/externals/json/include"
    "${SHADPS4_SOURCE_DIR}/externals/ext-boost"
    "${SHADPS4_SOURCE_DIR}/externals/sirit/include"
    "${SHADPS4_SOURCE_DIR}/externals/sirit/src"
    "${SHADPS4_SOURCE_DIR}/externals/sirit/externals/SPIRV-Headers/include"
    "${SHADPS4_SOURCE_DIR}/externals/xbyak"
    "${SHADPS4_SOURCE_DIR}/externals/robin-map/include")

if(MINGW)
    target_link_options(shadps4_gcn_bridge PRIVATE
        -static-libgcc
        -static-libstdc++)
    target_link_libraries(shadps4_gcn_bridge PRIVATE
        "C:/msys64/ucrt64/lib/libwinpthread.a")
endif()

set_target_properties(shadps4_gcn_bridge PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "shadps4_gcn_bridge")
