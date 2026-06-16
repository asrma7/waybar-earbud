cmake_minimum_required(VERSION 3.20)
project(waybar-earbud VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(waybar-earbud
    src/audio.cpp
    src/main.cpp
    src/devices/airpods/airpods.cpp
    src/devices/sony/sony.cpp
)

find_library(BLUETOOTH_LIBRARY bluetooth REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED gio-2.0 gio-unix-2.0 glib-2.0)

target_include_directories(waybar-earbud PRIVATE ${GIO_INCLUDE_DIRS})
target_link_directories(waybar-earbud PRIVATE ${GIO_LIBRARY_DIRS})
target_link_libraries(waybar-earbud PRIVATE ${BLUETOOTH_LIBRARY} ${GIO_LIBRARIES})
target_compile_definitions(waybar-earbud PRIVATE WAYBAR_EARBUD_VERSION="${PROJECT_VERSION}")
target_compile_options(waybar-earbud PRIVATE -Wall -Wextra -pedantic)
target_compile_options(waybar-earbud PRIVATE ${GIO_CFLAGS_OTHER})

install(TARGETS waybar-earbud RUNTIME DESTINATION bin)

if (BUILD_TESTING)
    add_executable(test_audio
        tests/test_audio.cpp
        src/audio.cpp
    )
    target_compile_options(test_audio PRIVATE -Wall -Wextra -pedantic)
    add_test(NAME audio_parsing COMMAND test_audio)

    add_executable(test_common
        tests/test_common.cpp
    )
    target_compile_options(test_common PRIVATE -Wall -Wextra -pedantic)
    add_test(NAME common_json COMMAND test_common)

    add_test(NAME cli_no_device COMMAND waybar-earbud)
    set_tests_properties(cli_no_device PROPERTIES
        ENVIRONMENT "EARBUD_BATTERY_DISABLE_AUDIO_DISCOVERY=1"
        PASS_REGULAR_EXPRESSION "\"class\":\"disconnected\""
    )

    add_test(NAME cli_version COMMAND waybar-earbud --version)
    set_tests_properties(cli_version PROPERTIES
        PASS_REGULAR_EXPRESSION "^waybar-earbud ${PROJECT_VERSION}"
    )

    add_test(NAME cli_version_short COMMAND waybar-earbud -v)
    set_tests_properties(cli_version_short PROPERTIES
        PASS_REGULAR_EXPRESSION "^waybar-earbud ${PROJECT_VERSION}"
    )

    add_test(NAME cli_help COMMAND waybar-earbud --help)
    set_tests_properties(cli_help PROPERTIES
        PASS_REGULAR_EXPRESSION "^Usage: waybar-earbud"
    )

    add_test(NAME cli_unknown_option
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_cli_failure.sh
            $<TARGET_FILE:waybar-earbud> 2 "unknown option: --hel" --hel
    )

    add_test(NAME cli_invalid_mac
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_cli_failure.sh
            $<TARGET_FILE:waybar-earbud> 2 "invalid value for --mac" --mac not-a-mac
    )

endif()
