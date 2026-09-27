cmake_minimum_required(VERSION 3.20)
project(wiz LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# WIN32 -> GUI subsystem (no console window), entry point is wWinMain.
add_executable(wiz WIN32 wiz.c)

target_compile_definitions(wiz PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(wiz PRIVATE user32 gdi32 shell32 dwmapi advapi32)

if(MSVC)
    target_compile_options(wiz PRIVATE /W4)
else()
    # MinGW / Clang on Windows
    target_compile_options(wiz PRIVATE -Wall -Wextra)
    target_link_options(wiz PRIVATE -municode)
endif()
