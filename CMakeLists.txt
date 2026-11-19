cmake_minimum_required(VERSION 3.19)
project(grom LANGUAGES C)

include(CTest)

set(GROMUI_LVGL ON)
add_subdirectory(gromui)

if(BUILD_TESTING)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME python-tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -v
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_subdirectory(grom/tests)
endif()
