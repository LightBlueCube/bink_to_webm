file(MAKE_DIRECTORY "${FFMPEG_BUILD_DIR}" "${FFMPEG_INSTALL_DIR}")

if(NOT EXISTS "${FFMPEG_BUILD_DIR}/ffbuild/config.mak")
	message(STATUS "Configuring FFmpeg (this may take a few minutes)")
	execute_process(
		COMMAND
			"${SH_EXECUTABLE}" "${FFMPEG_SOURCE_DIR}/configure"
			"--prefix=${FFMPEG_INSTALL_DIR}"
			--target-os=mingw64
			--arch=x86_64
			--disable-autodetect
			--disable-programs
			--disable-doc
			--disable-debug
			--disable-network
			--disable-everything
			--disable-avdevice
			--disable-avfilter
			--disable-swscale
			--enable-small
			--enable-static
			--disable-shared
			--enable-protocol=file
			--enable-demuxer=matroska
			--enable-decoder=vp8,vp9,opus
			--enable-swresample
		WORKING_DIRECTORY "${FFMPEG_BUILD_DIR}"
		RESULT_VARIABLE configure_result
	)
	if(NOT configure_result EQUAL 0)
		message(FATAL_ERROR "FFmpeg configure failed (code:${configure_result})")
	endif()
else()
	message(STATUS "FFmpeg already configured")
endif()


execute_process(
	COMMAND ${NPROC_EXECUTABLE}
	RESULT_VARIABLE nproc_result
	OUTPUT_VARIABLE nproc_output
	ERROR_VARIABLE nproc_error
	OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT nproc_result EQUAL 0)
	message(FATAL_ERROR "Failed while getting nproc (code:${nproc_result}):\n${nproc_output}\n${nproc_error}")
endif()

set(build_jobs ${nproc_output})
message(STATUS "Building FFmpeg with ${build_jobs} parallel jobs")
execute_process(
	COMMAND "${MAKE_EXECUTABLE}" "-j${build_jobs}" "install"
	WORKING_DIRECTORY "${FFMPEG_BUILD_DIR}"
	RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
	message(FATAL_ERROR "FFmpeg build failed (code:${build_result})")
endif()

set(ffmpeg_pkg_config_modules)
foreach(ffmpeg_component IN LISTS FFMPEG_COMPONENTS)
	if(NOT EXISTS "${FFMPEG_INSTALL_DIR}/lib/lib${ffmpeg_component}.a"
		OR NOT EXISTS "${FFMPEG_INSTALL_DIR}/lib/pkgconfig/lib${ffmpeg_component}.pc")
		message(FATAL_ERROR "FFmpeg component not found: ${ffmpeg_component}")
	endif()
	list(APPEND ffmpeg_pkg_config_modules "lib${ffmpeg_component}")
endforeach()

message(STATUS "Generating FFmpeg link options with pkg-config")
set(ENV{PKG_CONFIG_PATH} "${FFMPEG_INSTALL_DIR}/lib/pkgconfig")
execute_process(
	COMMAND
		"${PKG_CONFIG_EXECUTABLE}"
		--static
		--libs
		${ffmpeg_pkg_config_modules}
	RESULT_VARIABLE pkg_config_result
	OUTPUT_VARIABLE pkg_config_output
	ERROR_VARIABLE pkg_config_error
	OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT pkg_config_result EQUAL 0)
	message(FATAL_ERROR "Failed to get FFmpeg link options from pkg-config (code:${pkg_config_result}):\n${pkg_config_output}\n${pkg_config_error}")
endif()

file(WRITE "${FFMPEG_LINK_OPTIONS_FILE}" "${pkg_config_output}\n")
file(TOUCH "${FFMPEG_BUILD_STAMP}")
