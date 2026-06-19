// Unit tests for DwellClickHandler — the dwell-to-click input helper used for
// hands-light / switch-free access. These cover the deterministic geometry,
// input clamping and state-machine behaviour. The single timing case uses a
// real short wait because the handler reads steady_clock directly.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <glibmm/init.h>

#include <chrono>
#include <thread>

#include "Input/DwellClickHandler.h"

TEST_CASE("duration is clamped to a 100ms floor") {
    DwellClickHandler h;
    CHECK(h.get_duration_ms() == 800); // default
    h.set_duration_ms(50);
    CHECK(h.get_duration_ms() == 100);
    h.set_duration_ms(1000);
    CHECK(h.get_duration_ms() == 1000);
}

TEST_CASE("radius is clamped to a 1.0 floor") {
    DwellClickHandler h;
    h.set_radius(0.0f);
    CHECK(h.get_radius() == doctest::Approx(1.0f));
    h.set_radius(25.0f);
    CHECK(h.get_radius() == doctest::Approx(25.0f));
}

TEST_CASE("a disabled handler never tracks or activates") {
    DwellClickHandler h;
    CHECK_FALSE(h.get_enabled());
    h.on_pointer_move(10.0f, 10.0f);
    CHECK_FALSE(h.is_active());
    CHECK(h.get_progress() == doctest::Approx(0.0f));
}

TEST_CASE("first move after enabling starts a dwell at that point") {
    DwellClickHandler h;
    h.set_enabled(true);
    h.on_pointer_move(10.0f, 20.0f);
    CHECK(h.is_active());
    CHECK(h.get_center_x() == doctest::Approx(10.0f));
    CHECK(h.get_center_y() == doctest::Approx(20.0f));
}

TEST_CASE("movement within the radius keeps the dwell centre") {
    DwellClickHandler h;
    h.set_enabled(true);
    h.set_radius(20.0f);
    h.on_pointer_move(0.0f, 0.0f);   // start dwell at the origin
    h.on_pointer_move(5.0f, 5.0f);   // ~7.07px, inside the 20px radius
    CHECK(h.get_center_x() == doctest::Approx(0.0f));
    CHECK(h.get_center_y() == doctest::Approx(0.0f));
}

TEST_CASE("movement beyond the radius restarts the dwell at the new point") {
    DwellClickHandler h;
    h.set_enabled(true);
    h.set_radius(20.0f);
    h.on_pointer_move(0.0f, 0.0f);
    h.on_pointer_move(100.0f, 0.0f); // 100px, outside the 20px radius
    CHECK(h.get_center_x() == doctest::Approx(100.0f));
    CHECK(h.get_center_y() == doctest::Approx(0.0f));
}

TEST_CASE("movement exactly at the radius does not restart the dwell") {
    // The reset uses a strict '>' comparison, so a point on the boundary holds.
    DwellClickHandler h;
    h.set_enabled(true);
    h.set_radius(10.0f);
    h.on_pointer_move(0.0f, 0.0f);
    h.on_pointer_move(10.0f, 0.0f);  // distance == radius, not greater
    CHECK(h.get_center_x() == doctest::Approx(0.0f));
}

TEST_CASE("disabling cancels an in-progress dwell") {
    DwellClickHandler h;
    h.set_enabled(true);
    h.on_pointer_move(0.0f, 0.0f);
    REQUIRE(h.is_active());
    h.set_enabled(false);
    CHECK_FALSE(h.is_active());
}

TEST_CASE("no click is emitted before the dwell duration elapses") {
    DwellClickHandler h;
    int clicks = 0;
    h.signal_dwell_click().connect([&clicks]() { ++clicks; });
    h.set_enabled(true);
    h.set_duration_ms(100);
    h.on_pointer_move(0.0f, 0.0f);
    h.on_frame();                    // ~0ms elapsed
    CHECK(clicks == 0);
}

TEST_CASE("a click is emitted once the dwell duration elapses") {
    DwellClickHandler h;
    int clicks = 0;
    h.signal_dwell_click().connect([&clicks]() { ++clicks; });
    h.set_enabled(true);
    h.set_duration_ms(100);
    h.on_pointer_move(0.0f, 0.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    h.on_frame();
    CHECK(clicks == 1);
    CHECK_FALSE(h.is_active());       // the handler latches after firing
}

int main(int argc, char** argv) {
    Glib::init(); // on_frame() schedules an unclick via Glib::signal_timeout()
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
