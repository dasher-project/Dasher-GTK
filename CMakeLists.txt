cmake_minimum_required(VERSION 3.20)
project(Dasher)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY $<1:${CMAKE_BINARY_DIR}/Dasher>)

###############################
# Building Dasher Library
###############################

add_subdirectory("DasherCore")

###############################
# Using/Building GTK
###############################

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED gtk4)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)

###############################
# Using/Building SDL2
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
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
add_subdirectory("Thirdparty/SDL" EXCLUDE_FROM_ALL)

###############################
# Building Dasher-UI
###############################

ADD_Executable(Dasher
	${PROJECT_SOURCE_DIR}/src/DasherController.cpp
	${PROJECT_SOURCE_DIR}/src/DasherController.h
	${PROJECT_SOURCE_DIR}/src/MainWindow.cpp
	${PROJECT_SOURCE_DIR}/src/MainWindow.h
	${PROJECT_SOURCE_DIR}/src/FakeInput.h
	${PROJECT_SOURCE_DIR}/src/Input/Joystick/JoystickInput.h
	${PROJECT_SOURCE_DIR}/src/Input/Joystick/JoystickInput.cpp
	${PROJECT_SOURCE_DIR}/src/Preferences/PreferencesWindow.cpp
	${PROJECT_SOURCE_DIR}/src/Preferences/PreferencesWindow.h
	${PROJECT_SOURCE_DIR}/src/Preferences/SettingsMode.cpp
	${PROJECT_SOURCE_DIR}/src/Preferences/SettingsMode.h
	${PROJECT_SOURCE_DIR}/src/Preferences/SettingsHelp.cpp
	${PROJECT_SOURCE_DIR}/src/Preferences/SettingsHelp.h
	${PROJECT_SOURCE_DIR}/src/Preferences/SettingsPageBase.h
	${PROJECT_SOURCE_DIR}/src/UIComponents/ColorDisplayWidget.h
	${PROJECT_SOURCE_DIR}/src/UIComponents/ImageButton.h
	${PROJECT_SOURCE_DIR}/src/UIComponents/MessageOverlay.h
	${PROJECT_SOURCE_DIR}/src/UIComponents/PopoverBox.h
	${PROJECT_SOURCE_DIR}/src/UIComponents/RenderingCanvas.cpp
	${PROJECT_SOURCE_DIR}/src/UIComponents/RenderingCanvas.h
	${PROJECT_SOURCE_DIR}/src/UIComponents/SyncedColorDropdown.h
	${PROJECT_SOURCE_DIR}/src/UIComponents/SyncedEnumDropdown.h
	${PROJECT_SOURCE_DIR}/src/UIComponents/SyncedSlider.h
	${PROJECT_SOURCE_DIR}/src/UIComponents/SyncedSpinButton.h
	${PROJECT_SOURCE_DIR}/src/UIComponents/SyncedStringDropdown.h
	${PROJECT_SOURCE_DIR}/src/UIComponents/SyncedSwitch.h
	${PROJECT_SOURCE_DIR}/src/UIComponents/SyncedTextbox.h
	${PROJECT_SOURCE_DIR}/src/UIStyle.css
	${PROJECT_SOURCE_DIR}/src/main.cpp
)
add_dependencies(Dasher DasherCore)
target_link_libraries(Dasher PRIVATE DasherCore SDL3::SDL3 ${GTK_LIBRARIES} ${GTKMM_LIBRARIES})
target_include_directories(Dasher PRIVATE ${GTK_INCLUDE_DIRS} ${GTKMM_INCLUDE_DIRS})
target_include_directories(Dasher PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/)
target_include_directories(Dasher PRIVATE ${CMAKE_CURRENT_LIST_DIR}/Thirdparty/SDL/include/)
target_link_directories(Dasher PRIVATE ${GTK_LIBRARY_DIRS} ${GTKMM_LIBRARY_DIRS})

###############################
# Setting up deloyment
###############################

file(GLOB RESOURCE_FILES
  ${PROJECT_SOURCE_DIR}/DasherCore/Data/alphabets/alphabet*.xml
  ${PROJECT_SOURCE_DIR}/DasherCore/Data/colors/color*.xml
  ${PROJECT_SOURCE_DIR}/DasherCore/Data/control/control*.xml
  ${PROJECT_SOURCE_DIR}/DasherCore/Data/training/training*.txt
)
file(COPY ${RESOURCE_FILES} DESTINATION ${CMAKE_BINARY_DIR}/Dasher/Data)

file(GLOB RESOURCE_FILES
  ${PROJECT_SOURCE_DIR}/Thirdparty/imgui/misc/fonts/*.ttf
  ${PROJECT_SOURCE_DIR}/Resources/*.*
)
file(COPY ${RESOURCE_FILES} DESTINATION ${CMAKE_BINARY_DIR}/Dasher/Resources)
file(COPY ${PROJECT_SOURCE_DIR}/src/UIStyle.css DESTINATION ${CMAKE_BINARY_DIR}/Dasher/)