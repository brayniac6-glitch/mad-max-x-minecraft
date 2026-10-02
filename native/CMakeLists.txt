cmake_minimum_required(VERSION 3.25)

project(MadCraft VERSION 0.2.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL sits in the game folder and shouldn't depend on which VC++ runtime is installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(minhook
	GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
	GIT_TAG        v1.3.4)
FetchContent_MakeAvailable(minhook)

file(GLOB_RECURSE MADCRAFT_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)

# Built as dinput8.dll: Mad Max imports DirectInput 8, so Windows loads this from the game folder
# first. It forwards DirectInput to the real system DLL and wraps the keyboard/mouse devices.
add_library(MadCraft SHARED ${MADCRAFT_SOURCES} exports.def)
set_target_properties(MadCraft PROPERTIES OUTPUT_NAME dinput8)
target_include_directories(MadCraft PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
target_precompile_headers(MadCraft PRIVATE src/PCH.h)
target_link_libraries(MadCraft PRIVATE minhook d3d11.lib d3dcompiler.lib dxgi.lib dxguid.lib ole32.lib oleaut32.lib shell32.lib)
target_compile_definitions(MadCraft PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE DIRECTINPUT_VERSION=0x0800)
if(MSVC)
	target_compile_options(MadCraft PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8 /MP /wd4200)
	target_link_options(MadCraft PRIVATE /DEBUG:FULL)
endif()

# Development: -DMADCRAFT_DEPLOY_DIR="C:/Program Files/GOG Galaxy/Games/Mad Max" copies the DLL there.
if(MADCRAFT_DEPLOY_DIR AND EXISTS "${MADCRAFT_DEPLOY_DIR}")
	add_custom_command(TARGET MadCraft POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:MadCraft>" "${MADCRAFT_DEPLOY_DIR}/"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${MADCRAFT_DEPLOY_DIR}/madcraft"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/MadCraft.ini" "${MADCRAFT_DEPLOY_DIR}/madcraft/"
		VERBATIM)
endif()
