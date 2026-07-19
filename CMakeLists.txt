cmake_minimum_required(VERSION 3.20)
project(DasherGTK)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Dasher)

###############################
# Building Dasher CAPI Library
###############################

set(BUILD_CAPI ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("DasherCore")

###############################
# Using/Building GTK
###############################

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED gtk4)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)

###############################
# Using/Building SDL3
###############################

set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)

set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
set(SDL_HAPTIC ON CACHE BOOL "" FORCE)
set(SDL_HIDAPI ON CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)

IF(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	set(SDL_VIDEO ON CACHE BOOL "" FORCE)
ENDIF()

add_subdirectory("Thirdparty/SDL" EXCLUDE_FROM_ALL)

###############################
# rust-tts-wrapper
###############################

set(TTS_WRAPPER_DIR "${CMAKE_CURRENT_LIST_DIR}/rust-tts-wrapper")
set(TTS_WRAPPER_BUILD_DIR "${CMAKE_BINARY_DIR}/rust-tts-wrapper")
set(TTS_WRAPPER_VERSION "0.1.0" CACHE STRING "rust-tts-wrapper version to download")

if(EXISTS "${TTS_WRAPPER_DIR}/Cargo.toml")
	# cargo names the staticlib per target: MSVC produces rust_tts_wrapper.lib,
	# GNU/Unix toolchains produce librust_tts_wrapper.a.
	if(WIN32)
		set(TTS_WRAPPER_LIB "${TTS_WRAPPER_BUILD_DIR}/release/rust_tts_wrapper.lib")
	else()
		set(TTS_WRAPPER_LIB "${TTS_WRAPPER_BUILD_DIR}/release/librust_tts_wrapper.a")
	endif()
	set(TTS_WRAPPER_HEADER "${TTS_WRAPPER_DIR}/include/tts_wrapper.h")

	if(APPLE)
		set(TTS_WRAPPER_FEATURES_DEFAULT "avsynth,cloud")
	elseif(WIN32)
		set(TTS_WRAPPER_FEATURES_DEFAULT "sapi,cloud")
	else()
		set(TTS_WRAPPER_FEATURES_DEFAULT "system,cloud")
	endif()
	if(NOT TTS_WRAPPER_FEATURES)
		set(TTS_WRAPPER_FEATURES "${TTS_WRAPPER_FEATURES_DEFAULT}")
	endif()

	if(EXISTS "${TTS_WRAPPER_LIB}")
		add_custom_target(tts_wrapper DEPENDS "${TTS_WRAPPER_LIB}")
	else()
		add_custom_command(
			OUTPUT "${TTS_WRAPPER_LIB}"
			COMMAND cargo build --release --no-default-features --features ${TTS_WRAPPER_FEATURES} --target-dir "${TTS_WRAPPER_BUILD_DIR}"
			WORKING_DIRECTORY "${TTS_WRAPPER_DIR}"
			DEPENDS "${TTS_WRAPPER_DIR}/src/lib.rs"
			COMMENT "Building rust-tts-wrapper static library (features: ${TTS_WRAPPER_FEATURES})"
		)
		add_custom_target(tts_wrapper DEPENDS "${TTS_WRAPPER_LIB}")
	endif()
endif()

###############################
# Building Dasher-UI
###############################

file(GLOB_RECURSE HEADER_FILES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/src/*.h
    ${CMAKE_CURRENT_LIST_DIR}/src/**/*.h
)
file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/**/*.cpp
)

###############################
# Embedded resources (GResource)
###############################

# Compile UIStyle.css into the binary as a GResource so the stylesheet loads
# regardless of the current working directory or install layout. Flatpak and
# AppImage place the css in different locations relative to the executable, so
# any file-path lookup was fragile; embedding removes the dependency entirely.
find_program(GLIB_COMPILE_RESOURCES NAMES glib-compile-resources REQUIRED)
set(GRESOURCE_XML ${CMAKE_CURRENT_LIST_DIR}/src/dasher.gresource.xml)
set(GRESOURCE_C ${CMAKE_BINARY_DIR}/dasher_resources.c)
add_custom_command(
    OUTPUT ${GRESOURCE_C}
    COMMAND ${GLIB_COMPILE_RESOURCES}
            --target=${GRESOURCE_C}
            --sourcedir=${CMAKE_CURRENT_LIST_DIR}/src
            --generate-source
            ${GRESOURCE_XML}
    DEPENDS ${GRESOURCE_XML} ${CMAKE_CURRENT_LIST_DIR}/src/UIStyle.css
    COMMENT "Compiling GResource bundle (UIStyle.css)"
    VERBATIM
)

ADD_Executable(Dasher
	${HEADER_FILES}
	${SOURCE_FILES}
	${GRESOURCE_C}
)

# Install as lowercase 'dasher' on Linux for desktop integration
if(UNIX AND NOT APPLE)
	# Lowercase binary, and resolve the co-located libdasher.so via $ORIGIN.
	# The app deploys as a portable directory (binary + libdasher.so together),
	# so without an $ORIGIN rpath the runtime can't find libdasher.so.
	set_target_properties(Dasher PROPERTIES
		OUTPUT_NAME "dasher"
		BUILD_WITH_INSTALL_RPATH TRUE
		INSTALL_RPATH "$ORIGIN")
endif()

if(WIN32)
	set_target_properties(Dasher PROPERTIES
		LINK_FLAGS /SUBSYSTEM:WINDOWS
	)
endif()

add_dependencies(Dasher dasher)
if(TARGET tts_wrapper)
	add_dependencies(Dasher tts_wrapper)
	target_compile_definitions(Dasher PRIVATE HAS_TTS)
endif()
target_link_libraries(Dasher PRIVATE dasher SDL3::SDL3 ${GTK_LIBRARIES} ${GTKMM_LIBRARIES})
if(TARGET tts_wrapper)
	target_link_libraries(Dasher PRIVATE "${TTS_WRAPPER_LIB}")
	target_include_directories(Dasher PRIVATE "${TTS_WRAPPER_DIR}/include")
	if(APPLE)
		target_link_libraries(Dasher PRIVATE "-framework AVFAudio" "-framework Foundation" "-lobjc")
	elseif(WIN32)
		# The rust-tts-wrapper staticlib pulls in Rust std, getrandom and rustls,
		# whose native dependencies are not propagated when the archive is linked
		# by MSVC instead of by cargo. Mirror `rustc --print native-static-libs`.
		target_link_libraries(Dasher PRIVATE
			ntdll userenv bcrypt ws2_32 advapi32 crypt32 secur32 ncrypt ole32 oleaut32)
	else()
		target_link_libraries(Dasher PRIVATE dl pthread)
		# speech-dispatcher (libspeechd) is only pulled in by the `system`
		# TTS engine. Cloud-only builds have no speechd symbols, so linking
		# it there just fails (e.g. the Flatpak SDK has no libspeechd).
		if(TTS_WRAPPER_FEATURES MATCHES "system")
			target_link_libraries(Dasher PRIVATE speechd)
		endif()
	endif()
endif()
target_include_directories(Dasher SYSTEM PRIVATE ${GTK_INCLUDE_DIRS} ${GTKMM_INCLUDE_DIRS})
target_include_directories(Dasher PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/)
target_include_directories(Dasher PRIVATE ${CMAKE_CURRENT_LIST_DIR}/DasherCore/src/)
target_include_directories(Dasher PRIVATE ${CMAKE_CURRENT_LIST_DIR}/Thirdparty/SDL/include/)
target_link_directories(Dasher PRIVATE ${GTK_LIBRARY_DIRS} ${GTKMM_LIBRARY_DIRS})

###############################
# Setting up runtime environment
###############################

# Data files are copied as a POST_BUILD step so they stay fresh on every build
# rather than only on cmake reconfigure.
set(DATA_DIR ${CMAKE_SOURCE_DIR}/DasherCore/Data)

add_custom_command(TARGET Dasher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Dasher/Data
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${DATA_DIR}/alphabets ${CMAKE_BINARY_DIR}/Dasher/Data
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${DATA_DIR}/colors ${CMAKE_BINARY_DIR}/Dasher/Data
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${DATA_DIR}/colours ${CMAKE_BINARY_DIR}/Dasher/Data
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${DATA_DIR}/training ${CMAKE_BINARY_DIR}/Dasher/Data
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Dasher/Strings
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/DasherCore/Strings ${CMAKE_BINARY_DIR}/Dasher/Strings
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/Dasher/Resources/License
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/Resources ${CMAKE_BINARY_DIR}/Dasher/Resources
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/Thirdparty/SDL/LICENSE.txt ${CMAKE_BINARY_DIR}/Dasher/Resources/License/LICENSE_SDL.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/DasherCore/Thirdparty/pugixml/LICENSE.md ${CMAKE_BINARY_DIR}/Dasher/Resources/License/LICENSE_PugiXML.txt
    COMMENT "Copying runtime data files"
)

# Copy the shared library next to the executable
add_custom_command(TARGET Dasher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:dasher>
        ${CMAKE_BINARY_DIR}/Dasher/
)

if(APPLE)
	set(TTS_WRAPPER_LIB_EXT "dylib")
else()
	set(TTS_WRAPPER_LIB_EXT "so")
endif()

if(EXISTS "${TTS_WRAPPER_BUILD_DIR}/release/librust_tts_wrapper.${TTS_WRAPPER_LIB_EXT}")
	add_custom_command(TARGET Dasher POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${TTS_WRAPPER_BUILD_DIR}/release/librust_tts_wrapper.${TTS_WRAPPER_LIB_EXT}"
			${CMAKE_BINARY_DIR}/Dasher/
	)
endif()

###############################
# Setting up deployment
###############################

# USE_SOURCE_PERMISSIONS keeps the executable bit on the binary; without it
# install(DIRECTORY) forces 644 and /app/dasher becomes non-executable.
install(DIRECTORY ${CMAKE_BINARY_DIR}/Dasher/ DESTINATION . USE_SOURCE_PERMISSIONS)

###############################
# Linux desktop integration
###############################

# Additive freedesktop metadata so the app shows up in menus / software centres
# and can be packaged (Flatpak, distro packages). Does not alter the portable
# install(DIRECTORY) above; uses standard share/ locations via GNUInstallDirs.
if(UNIX AND NOT APPLE)
	include(GNUInstallDirs)
	set(DASHER_APP_ID "org.alternativeinterface.dasher")
	install(FILES packaging/${DASHER_APP_ID}.desktop
		DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
		COMPONENT dasher-metadata)
	install(FILES packaging/${DASHER_APP_ID}.metainfo.xml
		DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
		COMPONENT dasher-metadata)
	install(FILES Resources/icon.png
		DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/48x48/apps
		RENAME ${DASHER_APP_ID}.png
		COMPONENT dasher-metadata)
endif()

###############################
# Tests
###############################

option(BUILD_GTK_TESTS "Build Dasher-GTK unit tests" ON)
if(BUILD_GTK_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
