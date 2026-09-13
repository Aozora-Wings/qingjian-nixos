cmake_minimum_required(VERSION 3.16)
project(qingjian-fcitx5 LANGUAGES CXX)

find_package(Fcitx5Core REQUIRED)
find_package(Fcitx5Utils REQUIRED)

# fcitx5 5.1.20+ 的头文件使用 C++20（std::span、string_view::ends_with）。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_library(qingjian MODULE
    src/qingjian.cpp
    src/ipc.cpp
    src/json.cpp
)
target_compile_definitions(qingjian PRIVATE FCITX_GETTEXT_DOMAIN=\"qingjian\")
target_include_directories(qingjian PRIVATE src)
target_link_libraries(qingjian PRIVATE Fcitx5::Core Fcitx5::Utils)

set_target_properties(qingjian PROPERTIES PREFIX "")

install(TARGETS qingjian LIBRARY DESTINATION lib/fcitx5)
install(FILES qingjian.conf DESTINATION share/fcitx5/addon)
