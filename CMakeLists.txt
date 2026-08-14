cmake_minimum_required(VERSION 3.28)

project(bik_to_webm LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_file(MINHOOK_LIBRARY NAMES libMinHook.a libminhook.a MinHook.a minhook.a PATH_SUFFIXES lib REQUIRED)
find_file(WINPTHREAD_LIBRARY NAMES libwinpthread.a PATH_SUFFIXES lib REQUIRED)

add_subdirectory(deps/fxc2)
add_subdirectory(deps/FFmpeg)

# this is not necessary, just makes parallel build logs in a readable order
add_dependencies(bik_to_webm_fxc2_build bik_to_webm_ffmpeg_build)

set(BIK_TO_WEBM_SOURCES
	lib/hook.cpp
	lib/plugin.cpp
	lib/plugin_logging.cpp
	playvideoex.cpp
	webm_assets.cpp
	webm_audio.cpp
	webm_decoder.cpp
	webm_hooks.cpp
	webm_renderer.cpp
)

add_library(bik_to_webm SHARED ${BIK_TO_WEBM_SOURCES})
set_target_properties(bik_to_webm PROPERTIES
	PREFIX ""
	OUTPUT_NAME "bik_to_webm"
)

target_compile_features(bik_to_webm PRIVATE cxx_std_20)
target_compile_options(bik_to_webm PRIVATE
	-Wall
	-Wextra
	-Wunused
	-Wpedantic
	-Wshadow
	-Werror

	-fanalyzer
	-O2 -g
)

target_include_directories(bik_to_webm PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_BINARY_DIR}"
)

target_link_options(bik_to_webm PRIVATE -static)

target_link_libraries(bik_to_webm PRIVATE
	bik_to_webm_ffmpeg
	"${MINHOOK_LIBRARY}"
	"${WINPTHREAD_LIBRARY}"
)

set(BIK_TO_WEBM_SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/webm_shader.hlsl")
set(BIK_TO_WEBM_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/webm_shader.h")
add_custom_command(
	OUTPUT
		"${BIK_TO_WEBM_SHADER_HEADER}"
	COMMAND
		$<TARGET_FILE:bik_to_webm_fxc2>
		-T ps_4_0
		-E PSMain
		-VnPSMain
		"-Fh${BIK_TO_WEBM_SHADER_HEADER}"
		"${BIK_TO_WEBM_SHADER_SOURCE}"
	DEPENDS
		bik_to_webm_fxc2
		"${BIK_TO_WEBM_SHADER_SOURCE}"
	COMMENT
		"Compiling ${BIK_TO_WEBM_SHADER_SOURCE}"
	VERBATIM
)

add_custom_target(bik_to_webm_shader DEPENDS "${BIK_TO_WEBM_SHADER_HEADER}")
add_dependencies(bik_to_webm bik_to_webm_shader)
target_sources(bik_to_webm PRIVATE "${BIK_TO_WEBM_SHADER_HEADER}")
