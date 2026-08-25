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
    test_direct_mode_service.cpp
    ${CMAKE_SOURCE_DIR}/src/Output/DirectModeService.cpp
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

# Register the #42 gate with ctest only where it can actually run: it maps a
# real window, so it needs a display. Linux CI has xvfb-run; a Linux dev box
# with a session can run it directly. Left unregistered on macOS/Windows and
# on headless boxes with no xvfb, so `ctest` there stays green instead of
# failing for want of a display. (Registration per @owenpkent's review; the
# WORKING_DIRECTORY keeps the engine's relative "Data" path resolving.)
if(UNIX AND NOT APPLE)
    find_program(XVFB_RUN_EXECUTABLE xvfb-run)
    if(XVFB_RUN_EXECUTABLE)
        add_test(NAME prefs_rebuild_selftest
                 COMMAND "${XVFB_RUN_EXECUTABLE}" -a "$<TARGET_FILE:prefs_rebuild_selftest>"
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    elseif(DEFINED ENV{DISPLAY})
        add_test(NAME prefs_rebuild_selftest
                 COMMAND "$<TARGET_FILE:prefs_rebuild_selftest>"
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
    if(TEST prefs_rebuild_selftest)
        set_tests_properties(prefs_rebuild_selftest PROPERTIES TIMEOUT 180)
    endif()
endif()
list(FILTER SOURCE_FILES EXCLUDE REGEX ".*/src/main\\.cpp$")
# Engine-contract tests (GTK counterpart of Dasher-Windows' EngineCApiTests):
# drive the real DasherCore through DasherBridge — fresh-user-dir creation
# (DasherCore #59), probe-then-fetch permitted values (#58), text-size cache
# (steady-state frames issue zero callbacks, #56/v0.2.4) and the frame-delta
# clamp. Links DasherBridge + engine only (no GTK widgets), so no xvfb.
add_executable(engine_contract_tests
    test_engine_contract.cpp
    ${CMAKE_SOURCE_DIR}/src/Engine/DasherBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/Analytics/EngineLogRingBuffer.cpp
    ${CMAKE_BINARY_DIR}/dasher_resources.c
)
target_include_directories(engine_contract_tests SYSTEM PRIVATE ${GTK_INCLUDE_DIRS} ${GTKMM_INCLUDE_DIRS})
target_include_directories(engine_contract_tests SYSTEM PRIVATE ${doctest_SOURCE_DIR}/doctest)
target_include_directories(engine_contract_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/ ${CMAKE_SOURCE_DIR}/DasherCore/src/)
add_dependencies(engine_contract_tests dasher dasher_gresource)
target_link_libraries(engine_contract_tests PRIVATE doctest::doctest dasher ${GTK_LIBRARIES} ${GTKMM_LIBRARIES} Threads::Threads)
target_link_directories(engine_contract_tests PRIVATE ${GTK_LIBRARY_DIRS} ${GTKMM_LIBRARY_DIRS})
# Same MSVC ABI guard as dasher_unit_tests (prebuilt gtkmm/sigc++ use the
# release runtime).
if(MSVC)
    set_property(TARGET engine_contract_tests PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()
if(CURL_FOUND)
    target_link_libraries(engine_contract_tests PRIVATE CURL::libcurl)
    target_compile_definitions(engine_contract_tests PRIVATE HAS_ANALYTICS_CURL)
endif()
# Runs from the source root so the engine's relative "Data" path resolves —
# same WORKING_DIRECTORY trick as prefs_rebuild_selftest.
if(NOT MSVC)
    add_test(NAME engine_contract_tests
             COMMAND "$<TARGET_FILE:engine_contract_tests>"
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # A hung engine call must fail the suite after a bound, not stall CI forever
    # (the Windows leg sat in-progress for an hour on the first revision).
    set_tests_properties(engine_contract_tests PROPERTIES TIMEOUT 120)
endif()

add_executable(prefs_rebuild_selftest
    test_prefs_rebuild_selftest.cpp
    ${SOURCE_FILES}
    ${CMAKE_BINARY_DIR}/dasher_resources.c
)
target_compile_definitions(prefs_rebuild_selftest PRIVATE DASHER_GTK_VERSION="selftest")
target_include_directories(prefs_rebuild_selftest SYSTEM PRIVATE ${GTK_INCLUDE_DIRS} ${GTKMM_INCLUDE_DIRS})
target_include_directories(prefs_rebuild_selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/src/ ${CMAKE_SOURCE_DIR}/DasherCore/src/ ${CMAKE_SOURCE_DIR}/Thirdparty/SDL/include/)
add_dependencies(prefs_rebuild_selftest dasher dasher_gresource)
if(TARGET tts_wrapper)
    add_dependencies(prefs_rebuild_selftest tts_wrapper)
    target_compile_definitions(prefs_rebuild_selftest PRIVATE HAS_TTS)
endif()
target_link_libraries(prefs_rebuild_selftest PRIVATE dasher SDL3::SDL3 ${GTK_LIBRARIES} ${GTKMM_LIBRARIES})
# Windows GTK lives in a custom prefix, so the bare -l names above need the
# pkg-config link dirs to resolve (matches Dasher and dasher_unit_tests).
target_link_directories(prefs_rebuild_selftest PRIVATE ${GTK_LIBRARY_DIRS} ${GTKMM_LIBRARY_DIRS})
target_include_directories(prefs_rebuild_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(CURL_FOUND)
    target_link_libraries(prefs_rebuild_selftest PRIVATE CURL::libcurl)
    target_compile_definitions(prefs_rebuild_selftest PRIVATE HAS_ANALYTICS_CURL)
endif()
if(TARGET tts_wrapper)
    target_link_libraries(prefs_rebuild_selftest PRIVATE "${TTS_WRAPPER_LIB}")
    target_include_directories(prefs_rebuild_selftest PRIVATE "${TTS_WRAPPER_DIR}/include")
    if(APPLE)
        target_link_libraries(prefs_rebuild_selftest PRIVATE "-framework AVFAudio" "-framework Foundation" "-lobjc")
    elseif(WIN32)
        target_link_libraries(prefs_rebuild_selftest PRIVATE
            ntdll userenv bcrypt ws2_32 advapi32 crypt32 secur32 ncrypt ole32 oleaut32)
    else()
        target_link_libraries(prefs_rebuild_selftest PRIVATE dl pthread)
        # speech-dispatcher (libspeechd) is only pulled in by the `system`
        # TTS engine (the default feature set on Linux CI). Cloud-only builds
        # have no speechd symbols, so linking it there just fails — mirrors
        # the Dasher target's conditional.
        if(TTS_WRAPPER_FEATURES MATCHES "system")
            target_link_libraries(prefs_rebuild_selftest PRIVATE speechd)
        endif()
    endif()
endif()
