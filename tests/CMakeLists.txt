# Unit tests for Dasher-GTK. Kept lightweight: each unit under test is compiled
# directly into the test binary, so the suite builds without the full app, its
# git submodules or the Rust TTS wrapper. doctest is fetched at configure time.
include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
)
# doctest v2.4.11 ships cmake_minimum_required(VERSION 3.0); CMake 4.x (the
# macOS/Windows runners) removed compatibility with minimums below 3.5 and
# aborts. This variable supplies the 3.5 floor for that dependency and is
# ignored by the older CMake on the Linux runners.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(doctest)

find_package(Threads REQUIRED)

add_executable(dasher_unit_tests
    test_dwell_click_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/Input/DwellClickHandler.cpp
    test_pii_scrubber.cpp
    test_engine_log_ring_buffer.cpp
    test_crash_file_roundtrip.cpp
    test_analytics_client.cpp
    ${CMAKE_SOURCE_DIR}/src/Analytics/PiiScrubber.cpp
    ${CMAKE_SOURCE_DIR}/src/Analytics/EngineLogRingBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/Analytics/CrashReporter.cpp
    ${CMAKE_SOURCE_DIR}/src/Analytics/AnalyticsClient.cpp
    ${CMAKE_SOURCE_DIR}/src/Analytics/AnalyticsSettings.cpp
)

target_include_directories(dasher_unit_tests SYSTEM PRIVATE ${GTKMM_INCLUDE_DIRS})
target_include_directories(dasher_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The tests compile AnalyticsClient.cpp without HAS_ANALYTICS_CURL (they exercise
# only the pure serialisation helpers), so libcurl is not needed here.
target_link_libraries(dasher_unit_tests PRIVATE doctest::doctest ${GTKMM_LIBRARIES} Threads::Threads)
target_link_directories(dasher_unit_tests PRIVATE ${GTKMM_LIBRARY_DIRS})

# The Windows runner's gtkmm/sigc++ are prebuilt release (/MD,
# _ITERATOR_DEBUG_LEVEL=0) binaries. Building this test in the Debug config with
# the debug runtime (/MDd, _ITERATOR_DEBUG_LEVEL=2) gives sigc++'s internal slot
# containers a different STL ABI than the compiled libsigc, corrupting the slot
# list and crashing on the first signal connect(). The Release job already runs
# this suite cleanly with /MD, so force the test to the release runtime in every
# config to match the gtkmm ABI. CMP0091 is NEW here (cmake_minimum 3.20).
if(MSVC)
    set_property(TARGET dasher_unit_tests PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

add_test(NAME dasher_unit_tests COMMAND dasher_unit_tests)
