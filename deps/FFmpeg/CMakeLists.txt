set(FFMPEG_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/repo")
set(FFMPEG_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/repo")
set(FFMPEG_INSTALL_DIR "${CMAKE_BINARY_DIR}/ffmpeg-install")
set(FFMPEG_LINK_OPTIONS_FILE "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-link-options.rsp")
set(FFMPEG_BUILD_STAMP "${FFMPEG_INSTALL_DIR}/ffmpeg-build.stamp")
set(FFMPEG_COMPONENTS avformat avcodec avutil swresample)

find_program(sh_executable NAMES sh REQUIRED)
find_program(make_executable NAMES make REQUIRED)
find_program(pkg_config_executable NAMES pkg-config REQUIRED)
find_program(nproc_executable NAMES nproc REQUIRED)


set(ffmpeg_libraries)
set(ffmpeg_outputs)
foreach(ffmpeg_component IN LISTS FFMPEG_COMPONENTS)
	list(APPEND ffmpeg_libraries
		"${FFMPEG_INSTALL_DIR}/lib/lib${ffmpeg_component}.a"
	)
	list(APPEND ffmpeg_outputs
		"${FFMPEG_INSTALL_DIR}/lib/lib${ffmpeg_component}.a"
		"${FFMPEG_INSTALL_DIR}/lib/pkgconfig/lib${ffmpeg_component}.pc"
	)
endforeach()
list(APPEND ffmpeg_outputs "${FFMPEG_INSTALL_DIR}/include/libavutil/avutil.h")
list(APPEND ffmpeg_outputs "${FFMPEG_LINK_OPTIONS_FILE}")

add_custom_command(
	OUTPUT
		"${FFMPEG_BUILD_STAMP}"
	# BYPRODUCTS
		${ffmpeg_outputs}
	COMMAND
		"${CMAKE_COMMAND}"
		"-DFFMPEG_SOURCE_DIR=${FFMPEG_SOURCE_DIR}"
		"-DFFMPEG_BUILD_DIR=${FFMPEG_BUILD_DIR}"
		"-DFFMPEG_INSTALL_DIR=${FFMPEG_INSTALL_DIR}"
		"-DFFMPEG_COMPONENTS=${FFMPEG_COMPONENTS}"
		"-DFFMPEG_LINK_OPTIONS_FILE=${FFMPEG_LINK_OPTIONS_FILE}"
		"-DFFMPEG_BUILD_STAMP=${FFMPEG_BUILD_STAMP}"
		"-DSH_EXECUTABLE=${sh_executable}"
		"-DMAKE_EXECUTABLE=${make_executable}"
		"-DPKG_CONFIG_EXECUTABLE=${pkg_config_executable}"
		"-DNPROC_EXECUTABLE=${nproc_executable}"
		-P "${CMAKE_CURRENT_LIST_DIR}/Build.cmake"
	DEPENDS
		"${FFMPEG_SOURCE_DIR}/Changelog"
		"${FFMPEG_SOURCE_DIR}/RELEASE"
		"${FFMPEG_SOURCE_DIR}/configure"
		"${CMAKE_CURRENT_LIST_DIR}/Build.cmake"
	COMMENT
		"Building FFmpeg"
	VERBATIM
)
add_custom_target(bik_to_webm_ffmpeg_build DEPENDS "${FFMPEG_BUILD_STAMP}")

file(MAKE_DIRECTORY "${FFMPEG_INSTALL_DIR}/include")

add_library(bik_to_webm_ffmpeg INTERFACE)
target_include_directories(bik_to_webm_ffmpeg INTERFACE "${FFMPEG_INSTALL_DIR}/include")
target_link_libraries(bik_to_webm_ffmpeg INTERFACE "-Wl,@${FFMPEG_LINK_OPTIONS_FILE}")
set_property(TARGET bik_to_webm_ffmpeg PROPERTY INTERFACE_LINK_DEPENDS "${ffmpeg_libraries};${FFMPEG_LINK_OPTIONS_FILE}")
add_dependencies(bik_to_webm_ffmpeg bik_to_webm_ffmpeg_build)
