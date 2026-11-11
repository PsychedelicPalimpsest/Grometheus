cmake_minimum_required(VERSION 3.19)
project(grom_design C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

set(archive ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lvgl-9.4.0-core.tar.gz)
file(SHA256 ${archive} hash)
if(NOT hash STREQUAL "669d5093d9ff4e78f38dbcc3ca0cc4af6839b1614eb9b680c6639b3a36778dfc")
    message(FATAL_ERROR "LVGL archive mismatch")
endif()
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/lvgl)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${archive}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMAND_ERROR_IS_FATAL ANY)
endif()

file(GLOB_RECURSE sources ${CMAKE_CURRENT_BINARY_DIR}/lvgl/src/*.c)
add_library(lvgl-preview STATIC ${sources})
target_include_directories(lvgl-preview PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}/lvgl ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lvgl-preview PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_link_libraries(lvgl-preview PUBLIC PkgConfig::SDL2 m)

set(GROM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_library(grom-outline STATIC
    ${GROM_ROOT}/grom/ui/outline.c
    ${GROM_ROOT}/grom/ui/ghost.c)
target_include_directories(grom-outline PUBLIC
    ${GROM_ROOT}/grom/include
    ${GROM_ROOT}/grom/ui)
target_link_libraries(grom-outline PUBLIC lvgl-preview)
target_compile_options(grom-outline PRIVATE -Wall -Wextra -Werror)

add_executable(grom-design preview.c ${GROM_ROOT}/grom/src/backend_stub.c)
target_link_libraries(grom-design PRIVATE grom-outline)
target_compile_options(grom-design PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_executable(grom-interaction-test interaction_test.c)
target_link_libraries(grom-interaction-test PRIVATE grom-outline)
target_compile_options(grom-interaction-test PRIVATE -Wall -Wextra -Werror)
add_test(NAME grom-interaction COMMAND grom-interaction-test)
