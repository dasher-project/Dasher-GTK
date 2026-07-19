// Unit tests for EngineLogRingBuffer — the bounded log tail attached to crash
// reports (RFC 0009).
#include "Analytics/EngineLogRingBuffer.h"

#include <doctest/doctest.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <fstream>
#include <sstream>
#include <string>

using analytics::EngineLogRingBuffer;

TEST_CASE("keeps only the most recent lines within the line cap") {
    EngineLogRingBuffer buf(/*mirror=*/"", /*max_lines=*/3, /*max_bytes=*/1 << 20);
    for (int i = 0; i < 10; ++i)
        buf.append(1, "line" + std::to_string(i));
    CHECK(buf.snapshot() == "[I] line7\n[I] line8\n[I] line9");
}

TEST_CASE("enforces the byte cap") {
    EngineLogRingBuffer buf(/*mirror=*/"", /*max_lines=*/1000, /*max_bytes=*/40);
    for (int i = 0; i < 20; ++i)
        buf.append(2, "abcdefghij");
    const std::string snap = buf.snapshot();
    CHECK(snap.size() <= 40);
    CHECK(snap.find("abcdefghij") != std::string::npos); // recent lines survive
}

TEST_CASE("maps DasherCore levels to single-char prefixes") {
    EngineLogRingBuffer buf("", 8, 1 << 20);
    buf.append(0, "d");
    buf.append(1, "i");
    buf.append(2, "w");
    buf.append(3, "e");
    buf.append(9, "x");
    CHECK(buf.snapshot() == "[D] d\n[I] i\n[W] w\n[E] e\n[X] x");
}

TEST_CASE("mirrors lines to the on-disk file") {
    char* p = g_build_filename(g_get_tmp_dir(), "dasher_test_engine_log.txt", nullptr);
    const std::string path = p ? p : "";
    g_free(p);

    {
        EngineLogRingBuffer buf(path, 64, 8 * 1024);
        buf.append(1, "hello");
        buf.append(2, "world");
    }

    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string content = ss.str();
    CHECK(content.find("[I] hello") != std::string::npos);
    CHECK(content.find("[W] world") != std::string::npos);

    g_remove(path.c_str());
}
