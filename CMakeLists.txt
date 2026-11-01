cmake_minimum_required(VERSION 3.20)
project(LitePainter CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# ---- GLFW ----
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)

FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)

set(TINYOBJLOADER_BUILD_TEST_LOADER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(tinyobj GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git GIT_TAG v2.0.0rc13)

FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.0)

# stb (untuk simpan PNG) - header only
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG master GIT_SHALLOW TRUE)

set(GLAD_PROFILE "core" CACHE STRING "" FORCE)
set(GLAD_API "gl=3.3" CACHE STRING "" FORCE)
set(GLAD_GENERATOR "c" CACHE STRING "" FORCE)
FetchContent_Declare(glad GIT_REPOSITORY https://github.com/Dav1dde/glad.git GIT_TAG v0.1.36)

FetchContent_MakeAvailable(glfw glm tinyobj imgui glad stb)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC glfw)

add_executable(LitePainter src/main.cpp)
target_include_directories(LitePainter PRIVATE ${tinyobj_SOURCE_DIR} ${stb_SOURCE_DIR})
target_compile_definitions(LitePainter PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(LitePainter PRIVATE glfw glad imgui_lib glm::glm tinyobjloader)
if(WIN32)
  target_link_libraries(LitePainter PRIVATE comdlg32)
endif()
