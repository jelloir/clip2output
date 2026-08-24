cmake_minimum_required(VERSION 3.20)
project(clip2output)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# KWin 6.3's KWinConfig.cmake does not pull ECM in, so ECM's find-modules (which is where
# Findepoxy.cmake lives) are not on the module path and find_package(KWin) fails. 6.7 pulls ECM
# itself, so doing it here explicitly is correct on both.
find_package(ECM REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${ECM_FIND_MODULE_DIR})

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Quick DBus)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6Config REQUIRED)
find_package(KWin REQUIRED)
add_library(clip2output MODULE clip2output.cpp)
set_target_properties(clip2output PROPERTIES PREFIX "")
target_link_libraries(clip2output PRIVATE KWin::kwin Qt6::Core Qt6::Gui Qt6::DBus KF6::ConfigCore KF6::CoreAddons)

# Baked in so the installed binary can be told apart from the one a running KWin still has mapped.
# See buildStamp() in clip2output.cpp for why a feature-probe is not good enough.
if(NOT DEFINED CLIP2OUTPUT_BUILD_STAMP)
  string(TIMESTAMP CLIP2OUTPUT_BUILD_STAMP "%Y%m%d-%H%M%S" UTC)
endif()
target_compile_definitions(clip2output PRIVATE CLIP2OUTPUT_BUILD_STAMP="${CLIP2OUTPUT_BUILD_STAMP}")
