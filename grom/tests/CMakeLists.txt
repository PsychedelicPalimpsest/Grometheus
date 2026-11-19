add_executable(test-kmsg kmsg.c ../src/kmsg.c)
target_include_directories(test-kmsg PRIVATE ../src)
target_compile_options(test-kmsg PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME kmsg-borrowed-fd COMMAND test-kmsg)

add_executable(test-driver-bundle driver_bundle.c ../src/driver_bundle.c stderr_log.c)
target_include_directories(test-driver-bundle PRIVATE ../src)
target_compile_options(test-driver-bundle PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME driver-bundle COMMAND test-driver-bundle)

add_executable(test-touch touch.c ../src/touch.c)
target_include_directories(test-touch PRIVATE ../src)
target_compile_options(test-touch PRIVATE -Wall -Wextra -Werror -Wconversion -Wshadow -UNDEBUG)
target_link_options(test-touch PRIVATE -static -Wl,--wrap=ioctl)
add_test(NAME touch-frames COMMAND test-touch)

add_executable(test-power-key power_key.c ../src/power_key.c ../src/backend_stub.c)
target_include_directories(test-power-key PRIVATE ../src ../include)
target_compile_options(test-power-key PRIVATE -Wall -Wextra -Werror -Wconversion -Wshadow -UNDEBUG)
target_link_options(test-power-key PRIVATE -static)
add_test(NAME power-key COMMAND test-power-key)

add_executable(test-touch-lvgl touch_lvgl.c)
target_link_libraries(test-touch-lvgl PRIVATE lvgl m)
target_compile_options(test-touch-lvgl PRIVATE -UNDEBUG)
target_link_options(test-touch-lvgl PRIVATE -static)
add_test(NAME touch-lvgl-cancel COMMAND test-touch-lvgl)

add_executable(grom-interaction-test interaction_test.c ../ui/outline.c ../ui/ghost.c)
target_include_directories(grom-interaction-test PRIVATE ../include ../ui)
target_link_libraries(grom-interaction-test PRIVATE lvgl m)
target_compile_options(grom-interaction-test PRIVATE -Wall -Wextra -Werror)
add_test(NAME grom-interaction COMMAND grom-interaction-test)

set(MONOCYPHER_SRC ${PROJECT_SOURCE_DIR}/vendor/monocypher/src)
add_executable(test-disk disk_test.c ../src/disk.c stderr_log.c ${MONOCYPHER_SRC}/monocypher.c)
target_include_directories(test-disk PRIVATE ../src ${MONOCYPHER_SRC})
target_compile_features(test-disk PRIVATE c_std_11)
target_compile_options(test-disk PRIVATE -UNDEBUG)
target_link_options(test-disk PRIVATE
    -Wl,--wrap=open -Wl,--wrap=opendir -Wl,--wrap=mknod -Wl,--wrap=crypto_argon2)
add_test(NAME disk COMMAND test-disk)
add_test(NAME disk-format COMMAND test-disk format)
set_tests_properties(disk-format PROPERTIES SKIP_RETURN_CODE 77)

set_tests_properties(kmsg-borrowed-fd driver-bundle touch-frames power-key
    touch-lvgl-cancel grom-interaction disk disk-format PROPERTIES TIMEOUT 10)
