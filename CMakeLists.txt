cmake_minimum_required(VERSION 3.10)

project(rin VERSION 1.0 LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# set(CMAKE_C_STANDARD 99)
# set(CMAKE_C_STANDARD_REQUIRED True)
set(RIN_PACKAGE_DIR "${CMAKE_SOURCE_DIR}/../rin-windows-x64" CACHE PATH "rin package output directory")

include_directories(extern/haikal/src/meta_arena/gen)
include_directories(extern/haikal/src/runtime)
include_directories(src)

add_subdirectory(extern/haikal)

add_executable(${PROJECT_NAME}
    src/main.c
    src/string8.c
    src/string8slice.c
)
add_dependencies(${PROJECT_NAME} haikal)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/src/std"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/std"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${RIN_PACKAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PROJECT_NAME}>"
        "${RIN_PACKAGE_DIR}/rin.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/src/std"
        "${RIN_PACKAGE_DIR}/std"
)

set(LLVM_ROOT "C:/Program Files/LLVM" CACHE PATH "LLVM install root")
find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h
    PATHS "${LLVM_ROOT}/include"
    NO_DEFAULT_PATH
)
find_library(LIBCLANG_LIBRARY libclang
    PATHS "${LLVM_ROOT}/lib"
    NO_DEFAULT_PATH
)

if (LIBCLANG_INCLUDE_DIR AND LIBCLANG_LIBRARY)
    add_executable(rinbind src/rinbind.c)
    target_include_directories(rinbind PRIVATE ${LIBCLANG_INCLUDE_DIR})
    target_link_libraries(rinbind PRIVATE ${LIBCLANG_LIBRARY})
    if (WIN32)
        add_custom_command(TARGET rinbind POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LLVM_ROOT}/bin/libclang.dll"
                "$<TARGET_FILE_DIR:rinbind>/libclang.dll"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${RIN_PACKAGE_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:rinbind>"
                "${RIN_PACKAGE_DIR}/rinbind.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LLVM_ROOT}/bin/libclang.dll"
                "${RIN_PACKAGE_DIR}/libclang.dll"
        )
    endif()
else()
    message(WARNING "libclang not found; rinbind will not be built")
endif()

# add_subdirectory(extern) # in case of modules...

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
endif()

# add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resource ${CMAKE_CURRENT_BINARY_DIR}/resource)
