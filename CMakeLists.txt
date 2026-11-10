cmake_minimum_required(VERSION 3.19)
project(miniui VERSION 0.1.0 LANGUAGES C)

option(MUI_STATIC_EXECUTABLES "Link example executables statically" OFF)
option(MUI_LVGL "Build the LVGL adapter and demo" OFF)
option(MUI_BENCHMARKS "Build CPU/RAM copy benchmarks" OFF)

if(MUI_BENCHMARKS)
    add_executable(bench-fb-copy benchmarks/fb_copy.c)
    target_include_directories(bench-fb-copy PRIVATE src third_party/drm)
    target_link_libraries(bench-fb-copy PRIVATE miniui)
endif()

add_library(miniui STATIC
    src/display.c
    src/drm.c
    src/fbdev.c
)
target_include_directories(miniui
    PUBLIC include
    PRIVATE src third_party/drm
)
target_compile_features(miniui PUBLIC c_std_11)
target_compile_definitions(miniui PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(miniui PRIVATE -Wall -Wextra -Werror -Wconversion -Wshadow)

add_executable(miniui-init examples/init.c)
target_link_libraries(miniui-init PRIVATE miniui)
if(MUI_STATIC_EXECUTABLES)
    target_link_options(miniui-init PRIVATE -static)
endif()

if(MUI_LVGL)
    set(LVGL_ARCHIVE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl-9.4.0-core.tar.gz)
    set(LVGL_SHA256 669d5093d9ff4e78f38dbcc3ca0cc4af6839b1614eb9b680c6639b3a36778dfc)
    set(LVGL_VENDOR_DIR ${CMAKE_CURRENT_BINARY_DIR}/vendor)
    set(LVGL_DIR ${LVGL_VENDOR_DIR}/lvgl)

    file(SHA256 ${LVGL_ARCHIVE} archive_hash)
    if(NOT archive_hash STREQUAL LVGL_SHA256)
        message(FATAL_ERROR "Vendored LVGL archive checksum mismatch")
    endif()

    # Extract once per archive version. The source archive is pinned and offline.
    if(NOT EXISTS ${LVGL_VENDOR_DIR}/${archive_hash})
        file(MAKE_DIRECTORY ${LVGL_VENDOR_DIR})
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xzf ${LVGL_ARCHIVE}
            WORKING_DIRECTORY ${LVGL_VENDOR_DIR}
            COMMAND_ERROR_IS_FATAL ANY
        )
        file(WRITE ${LVGL_VENDOR_DIR}/${archive_hash} "extracted")
    endif()

    file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS ${LVGL_DIR}/src/*.c)
    add_library(lvgl STATIC ${LVGL_SOURCES})
    target_include_directories(lvgl PUBLIC ${LVGL_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
    target_compile_options(lvgl PRIVATE -ffunction-sections -fdata-sections)

    add_library(miniui-lvgl STATIC src/lvgl.c)
    target_link_libraries(miniui-lvgl PUBLIC miniui lvgl m)
    target_compile_options(miniui-lvgl PRIVATE -Wall -Wextra -Werror)

    add_executable(miniui-lvgl-init examples/init.c examples/lvgl_demo.c)
    target_compile_definitions(miniui-lvgl-init PRIVATE MUI_USE_LVGL)
    target_link_libraries(miniui-lvgl-init PRIVATE miniui-lvgl)
    target_link_options(miniui-lvgl-init PRIVATE -Wl,--gc-sections)
    if(MUI_STATIC_EXECUTABLES)
        target_link_options(miniui-lvgl-init PRIVATE -static)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(test-formats tests/formats.c)
    target_include_directories(test-formats PRIVATE src third_party/drm)
    target_link_libraries(test-formats PRIVATE miniui)
    target_compile_options(test-formats PRIVATE -UNDEBUG)
    add_test(NAME formats COMMAND test-formats)

    add_executable(test-failures tests/failures.c)
    target_include_directories(test-failures PRIVATE src third_party/drm)
    target_link_libraries(test-failures PRIVATE miniui)
    target_compile_options(test-failures PRIVATE -UNDEBUG)
    target_link_options(test-failures PRIVATE
        -Wl,--wrap=open
        -Wl,--wrap=open64
        -Wl,--wrap=close
        -Wl,--wrap=ioctl
        -Wl,--wrap=mmap
        -Wl,--wrap=mmap64
        -Wl,--wrap=munmap
    )
    add_test(NAME failures COMMAND test-failures)
    set_tests_properties(formats failures PROPERTIES TIMEOUT 10)

    if(MUI_LVGL)
        add_executable(test-lvgl tests/lvgl_adapter.c)
        target_link_libraries(test-lvgl PRIVATE miniui-lvgl)
        target_compile_options(test-lvgl PRIVATE -UNDEBUG)
        add_test(NAME lvgl-adapter COMMAND test-lvgl)
        set_tests_properties(lvgl-adapter PROPERTIES TIMEOUT 10)
    endif()
endif()
