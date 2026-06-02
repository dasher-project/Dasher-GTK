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

set(TTS_WRAPPER_DIR "${CMAKE_CURRENT_LIST_DIR}/../rust-tts-wrapper")
set(TTS_WRAPPER_BUILD_DIR "${CMAKE_BINARY_DIR}/../rust-tts-wrapper")
set(TTS_WRAPPER_VERSION "0.1.0" CACHE STRING "rust-tts-wrapper version to download")

if(EXISTS "${TTS_WRAPPER_DIR}/Cargo.toml")
	set(TTS_WRAPPER_LIB "${TTS_WRAPPER_BUILD_DIR}/release/librust_tts_wrapper.a")
	set(TTS_WRAPPER_HEADER "${TTS_WRAPPER_DIR}/include/tts_wrapper.h")

	if(EXISTS "${TTS_WRAPPER_LIB}")
		add_custom_target(tts_wrapper DEPENDS "${TTS_WRAPPER_LIB}")
	else()
		add_custom_command(
			OUTPUT "${TTS_WRAPPER_LIB}"
			COMMAND cargo build --release --no-default-features --features system --target-dir "${TTS_WRAPPER_BUILD_DIR}"
			WORKING_DIRECTORY "${TTS_WRAPPER_DIR}"
			DEPENDS "${TTS_WRAPPER_DIR}/src/lib.rs"
			COMMENT "Building rust-tts-wrapper static library (system TTS only)"
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
    ${CMAKE_CURRENT_LIST_DIR}/src/**/*.css
)

ADD_Executable(Dasher
	${HEADER_FILES}
	${SOURCE_FILES}
)

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
	target_link_libraries(Dasher PRIVATE "${TTS_WRAPPER_LIB}" dl pthread speechd)
	target_include_directories(Dasher PRIVATE "${TTS_WRAPPER_DIR}/include")
endif()
target_include_directories(Dasher SYSTEM PRIVATE ${GTK_INCLUDE_DIRS} ${GTKMM_INCLUDE_DIRS})
target_include_directories(Dasher PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/)
target_include_directories(Dasher PRIVATE ${CMAKE_CURRENT_LIST_DIR}/DasherCore/src/)
target_include_directories(Dasher PRIVATE ${CMAKE_CURRENT_LIST_DIR}/Thirdparty/SDL/include/)
target_link_directories(Dasher PRIVATE ${GTK_LIBRARY_DIRS} ${GTKMM_LIBRARY_DIRS})

###############################
# Setting up runtime environment
###############################

file(GLOB DATA_FILES
  ${PROJECT_SOURCE_DIR}/DasherCore/Data/alphabets/alphabet*.xml
  ${PROJECT_SOURCE_DIR}/DasherCore/Data/colours/colour*.xml
  ${PROJECT_SOURCE_DIR}/DasherCore/Data/colors/color*.xml
  ${PROJECT_SOURCE_DIR}/DasherCore/Data/control/control*.xml
  ${PROJECT_SOURCE_DIR}/DasherCore/Data/training/training*.txt
)
file(COPY ${DATA_FILES} DESTINATION ${CMAKE_BINARY_DIR}/Dasher/Data)

file(GLOB STRING_FILES
  ${PROJECT_SOURCE_DIR}/DasherCore/Strings/*.json
)
file(COPY ${STRING_FILES} DESTINATION ${CMAKE_BINARY_DIR}/Dasher/Strings)

file(COPY ${PROJECT_SOURCE_DIR}/Resources/ DESTINATION ${CMAKE_BINARY_DIR}/Dasher/Resources)
file(COPY ${PROJECT_SOURCE_DIR}/src/UIStyle.css DESTINATION ${CMAKE_BINARY_DIR}/Dasher/)

file(COPY_FILE ${PROJECT_SOURCE_DIR}/Thirdparty/SDL/LICENSE.txt ${CMAKE_BINARY_DIR}/Dasher/Resources/License/LICENSE_SDL.txt)
file(COPY_FILE ${PROJECT_SOURCE_DIR}/DasherCore/Thirdparty/pugixml/LICENSE.md ${CMAKE_BINARY_DIR}/Dasher/Resources/License/LICENSE_PugiXML.txt)

# Copy the shared library next to the executable
add_custom_command(TARGET Dasher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:dasher>
        ${CMAKE_BINARY_DIR}/Dasher/
)

if(EXISTS "${TTS_WRAPPER_BUILD_DIR}/release/librust_tts_wrapper.so")
	add_custom_command(TARGET Dasher POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${TTS_WRAPPER_BUILD_DIR}/release/librust_tts_wrapper.so"
			${CMAKE_BINARY_DIR}/Dasher/
	)
endif()

###############################
# Setting up deployment
###############################

install(DIRECTORY ${CMAKE_BINARY_DIR}/Dasher/ DESTINATION .)
