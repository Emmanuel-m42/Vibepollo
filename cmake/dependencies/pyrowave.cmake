# PyroWave encoder library (Windows).
# Built as its own DLL from a pinned upstream commit and loaded at runtime by
# src/platform/windows/pyrowave_loader.cpp, so a missing Vulkan driver only disables PyroWave.
include(ExternalProject)

set(PYROWAVE_GIT_TAG 89f7e47d4abbf650c91fae766728af866c5e32a0)
set(PYROWAVE_PREFIX "${CMAKE_BINARY_DIR}/third-party/pyrowave")
set(PYROWAVE_SOURCE_DIR "${PYROWAVE_PREFIX}/src/pyrowave_ext")
set(PYROWAVE_BINARY_DIR "${PYROWAVE_PREFIX}/src/pyrowave_ext-build")
set(PYROWAVE_INSTALL_DIR "${PYROWAVE_PREFIX}/install")
set(PYROWAVE_DLL "${PYROWAVE_INSTALL_DIR}/bin/libpyrowave-shared-0.dll")

# checkout_granite.sh fetches the pinned Granite subset (volk + Vulkan headers) PyroWave builds against.
find_program(PYROWAVE_BASH bash REQUIRED)

ExternalProject_Add(pyrowave_ext
        PREFIX "${PYROWAVE_PREFIX}"
        SOURCE_DIR "${PYROWAVE_SOURCE_DIR}"
        BINARY_DIR "${PYROWAVE_BINARY_DIR}"
        GIT_REPOSITORY https://github.com/Themaister/pyrowave.git
        GIT_TAG ${PYROWAVE_GIT_TAG}
        GIT_SUBMODULES ""
        UPDATE_COMMAND ""
        PATCH_COMMAND "${PYROWAVE_BASH}" checkout_granite.sh
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            # Self-contained DLL: no libstdc++/libgcc/winpthread runtime DLLs to ship.
            "-DCMAKE_SHARED_LINKER_FLAGS=-static -static-libgcc -static-libstdc++"
        BUILD_COMMAND ${CMAKE_COMMAND} --build "${PYROWAVE_BINARY_DIR}" --target pyrowave-shared
        INSTALL_COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
                "${PYROWAVE_BINARY_DIR}/libpyrowave-shared-0.dll" "${PYROWAVE_DLL}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PYROWAVE_SOURCE_DIR}/pyrowave.h" "${PYROWAVE_INSTALL_DIR}/include/pyrowave/pyrowave.h"
        BUILD_BYPRODUCTS "${PYROWAVE_DLL}"
)

set(PYROWAVE_INCLUDE_DIRS
        "${PYROWAVE_INSTALL_DIR}/include"
        "${PYROWAVE_SOURCE_DIR}/Granite/third_party/khronos/vulkan-headers/include")
