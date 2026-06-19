# Unit tests for Dasher-GTK. Kept lightweight: each unit under test is compiled
# directly into the test binary, so the suite builds without the full app, its
# git submodules or the Rust TTS wrapper. doctest is fetched at configure time.
include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
)
FetchContent_MakeAvailable(doctest)

add_executable(dasher_unit_tests
    test_dwell_click_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/Input/DwellClickHandler.cpp
)

target_include_directories(dasher_unit_tests SYSTEM PRIVATE ${GTKMM_INCLUDE_DIRS})
target_include_directories(dasher_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dasher_unit_tests PRIVATE doctest::doctest ${GTKMM_LIBRARIES})
target_link_directories(dasher_unit_tests PRIVATE ${GTKMM_LIBRARY_DIRS})

add_test(NAME dasher_unit_tests COMMAND dasher_unit_tests)
