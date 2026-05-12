# evmone: Fast Ethereum Virtual Machine implementation
# Copyright 2019 The evmone Authors.
# SPDX-License-Identifier: Apache-2.0

include(LibraryTools)

add_library(evmone
    ${include_dir}/evmone/evmone.h
    advanced_analysis.cpp
    advanced_analysis.hpp
    advanced_execution.cpp
    advanced_execution.hpp
    advanced_instructions.cpp
    baseline.hpp
    baseline_analysis.cpp
    baseline_execution.cpp
    baseline_instruction_table.cpp
    baseline_instruction_table.hpp
    constants.hpp
    delegation.cpp
    delegation.hpp
    instructions.hpp
    instructions_calls.cpp
    instructions_opcodes.hpp
    instructions_storage.cpp
    instructions_traits.hpp
    instructions_xmacro.hpp
    lru_cache.hpp
    tracing.cpp
    tracing.hpp
    vm.cpp
    vm.hpp
)
target_compile_features(evmone PUBLIC cxx_std_20)
target_link_libraries(evmone PUBLIC evmc::evmc intx::intx PRIVATE evmone::precompiles)
target_include_directories(evmone PUBLIC
    $<BUILD_INTERFACE:${include_dir}>$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(EVMONE_X86_64_ARCH_LEVEL GREATER_EQUAL 2)
    # Add CPU architecture runtime check. The EVMONE_X86_64_ARCH_LEVEL has a valid value.
    target_sources(evmone PRIVATE cpu_check.cpp)
    set_source_files_properties(cpu_check.cpp PROPERTIES COMPILE_DEFINITIONS EVMONE_X86_64_ARCH_LEVEL=${EVMONE_X86_64_ARCH_LEVEL})
endif()

if(CABLE_COMPILER_GNULIKE)
    target_compile_options(
        evmone PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wstack-usage=2900>
    )
    if(NOT SANITIZE MATCHES undefined)
        # RTTI can be disabled except for UBSan which checks vptr integrity.
        target_compile_options(evmone PRIVATE -fno-rtti)
    endif()
endif()
set_target_properties(
    evmone
    PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_SOVERSION}
)

if(NOT SANITIZE)
    # On Linux, check if all symbols in evmone are resolved during linking.
    target_link_options(evmone PRIVATE $<$<PLATFORM_ID:Linux>:LINKER:--no-undefined>)
endif()

set_source_files_properties(vm.cpp PROPERTIES COMPILE_DEFINITIONS PROJECT_VERSION="${PROJECT_VERSION}")

add_standalone_library(evmone)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion
)

set(CONFIGURE_TEMPLATE_FILE_PATH "${CMAKE_CURRENT_BINARY_DIR}/Config.cmake.in")
set(CONFIGURE_TEMPLATE_FILE_CONTENT
    "@PACKAGE_INIT@\ninclude(CMakeFindDependencyMacro)\nfind_dependency(evmc CONFIG)\ncheck_required_components(\"@PROJECT_NAME@\")\n"
)
file(WRITE "${CONFIGURE_TEMPLATE_FILE_PATH}" "${CONFIGURE_TEMPLATE_FILE_CONTENT}")

configure_package_config_file(
    "${CONFIGURE_TEMPLATE_FILE_PATH}"
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
    PATH_VARS CMAKE_INSTALL_LIBDIR
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)
