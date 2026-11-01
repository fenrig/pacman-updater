cmake_minimum_required(VERSION 3.16)
project(pacman_updater C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Wstrict-prototypes)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
add_library(updater_core STATIC
	src/core.c
	src/conditions.c
	src/battery.c
	src/idle.c
	src/cpu.c
	src/connectivity.c)
target_include_directories(updater_core PUBLIC src)
target_link_libraries(updater_core PUBLIC PkgConfig::SYSTEMD PkgConfig::CURL)
add_executable(pacman-updater src/main.c)
target_link_libraries(pacman-updater PRIVATE updater_core)
add_executable(pacman-updater-check src/check.c)
target_link_libraries(pacman-updater-check PRIVATE updater_core)
enable_testing()
add_executable(test_core tests/test_core.c)
target_link_libraries(test_core PRIVATE updater_core)
target_compile_options(test_core PRIVATE -UNDEBUG)
add_test(NAME core COMMAND test_core)

