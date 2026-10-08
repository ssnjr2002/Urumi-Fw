/**
 * Mesh contract: bilinear heights with the edge carried outside the grid, a
 * walk's range, slope and slope change, and the Z caps they put on a path.
 */

#include "doctest.h"

#include <planner/mesh.h>
#include <planner/planner.h>

#include <math.h>

using namespace planner;

// 3 × 2 grid at x 0, 10, 20 and y 0, 10, µm:
//   y 0:   0  100  300
//   y 10: 200 300  500
static const int16_t kZ[6] = {0, 100, 300, 200, 300, 500};

static Mesh grid() {
    Mesh m;
    m.set(3, 2, 0, 0, 10, 10, kZ);
    return m;
}

TEST_CASE("mesh: set refuses a grid it cannot interpolate") {
    struct Case { const char* name; int nx, ny; float dx, dy; const int16_t* z; };
    const Case cases[] = {
        {"one column", 1, 2, 10, 10, kZ},
        {"one row", 3, 1, 10, 10, kZ},
        {"zero dx", 3, 2, 0, 10, kZ},
        {"negative dy", 3, 2, 10, -1, kZ},
        {"no heights", 3, 2, 10, 10, nullptr},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Mesh m;
        CHECK_FALSE(m.set(c.nx, c.ny, 0, 0, c.dx, c.dy, c.z));
        CHECK_FALSE(m.valid());
    }
}

TEST_CASE("mesh: heights") {
    struct Case { const char* name; float x, y, z; };
    const Case cases[] = {
        {"a point", 10, 0, 0.1f},
        {"the last point", 20, 10, 0.5f},
        {"between two points", 15, 0, 0.2f},
        {"a cell centre", 5, 5, 0.15f},
        {"left of the grid", -50, 0, 0.0f},
        {"right of the grid", 70, 10, 0.5f},
        {"below and past a corner", 99, -99, 0.3f},
        {"above, between columns", 5, 40, 0.25f},
    };
    const Mesh m = grid();
    for (const Case& c : cases) {
        CAPTURE(c.name);
        CHECK(meshAt(m, c.x, c.y) == doctest::Approx(c.z).epsilon(1e-5));
    }
}

TEST_CASE("mesh: walks") {
    // A plane rising 1 µm per mm in x: slope 0.001, no slope change.
    static const int16_t plane[4] = {0, 100, 0, 100};
    Mesh tilt;
    tilt.set(2, 2, 0, 0, 100, 100, plane);

    struct Case {
        const char* name;
        const Mesh* m;
        Vec2 p0, p1, tip;
        float ref, lo, hi, slope, dslope;
    };
    const Mesh g = grid();
    const Case cases[] = {
        {"along the tilt", &tilt, {0, 0}, {100, 0}, {0, 0}, 0, 0, 0.1f, 0.001f, 0},
        {"across the tilt", &tilt, {50, 0}, {50, 100}, {0, 0}, 0, 0.05f, 0.05f, 0, 0},
        {"backwards", &tilt, {100, 50}, {0, 50}, {0, 0}, 0, 0, 0.1f, 0.001f, 0},
        {"shifted by the tip", &tilt, {0, 0}, {50, 0}, {50, 0}, 0, 0.05f, 0.1f, 0.001f, 0},
        {"less the reference", &tilt, {0, 0}, {100, 0}, {0, 0}, 0.05f, -0.05f, 0.05f, 0.001f, 0},
        {"past the edge stays flat", &tilt, {100, 0}, {200, 0}, {0, 0}, 0, 0.1f, 0.1f, 0, 0},
        // Slope 0.01 then 0.02 at x 10: a crossing.
        {"over a grid line", &g, {0, 0}, {20, 0}, {0, 0}, 0, 0, 0.3f, 0.02f, 0.01f},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const MeshWalk w = meshWalkLine(*c.m, c.p0, c.p1, c.tip, c.ref);
        CHECK(w.lo == doctest::Approx(c.lo).epsilon(1e-4));
        CHECK(w.hi == doctest::Approx(c.hi).epsilon(1e-4));
        CHECK(w.slope == doctest::Approx(c.slope).epsilon(1e-3));
        CHECK(w.dslope == doctest::Approx(c.dslope).epsilon(1e-3));
        CHECK(w.step <= 0.5f * fminf(c.m->dx, c.m->dy) + 1e-6f);
    }
}

TEST_CASE("mesh: a Bézier walks its curve") {
    static const int16_t plane[4] = {0, 100, 0, 100};
    Mesh tilt;
    tilt.set(2, 2, 0, 0, 100, 100, plane);
    Bezier b;
    REQUIRE(analyzeBezier({0, 0}, {30, 40}, {70, 40}, {100, 0}, b) == BezierError::None);
    const MeshWalk w = meshWalkBezier(tilt, b, {0, 0}, 0);
    CHECK(w.lo == doctest::Approx(0).epsilon(1e-4));
    CHECK(w.hi == doctest::Approx(0.1f).epsilon(1e-4));
    // dz/ds = 0.001 · dx/ds: at most 0.001, where the curve runs along x.
    CHECK(w.slope <= 0.001f * 1.001f);
    CHECK(w.slope > 0.0009f);
}

TEST_CASE("mesh: caps") {
    struct Case {
        const char* name;
        MeshWalk w;
        float v_max_sqr, accel;   // expected
    };
    // The uncapped path: 80 mm/s, 100 mm/s²; Z 20 mm/s, 500 mm/s².
    const Case cases[] = {
        {"flat", {0, 0, 0, 0, 5}, 6400, 100},
        {"gentle slope binds nothing", {0, 0, 0.001f, 0, 5}, 6400, 100},
        {"steep slope caps speed and accel", {0, 0, 0.5f, 0, 5}, 1600, 100},
        {"steeper still", {0, 0, 5, 0, 5}, 16, 50},
        {"a sharp crossing caps speed", {0, 0, 0, 1, 5}, 1250, 100},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const PathCap cap = meshCap(c.w, 20, 500);
        CHECK(fminf(6400.0f, cap.v_max_sqr) == doctest::Approx(c.v_max_sqr));
        CHECK(fminf(100.0f, cap.accel) == doctest::Approx(c.accel));
    }
}

TEST_CASE("mesh: a cap lowers the pushed block's limits") {
    struct Case { const char* name; PathCap cap; float v_max_sqr, accel; };
    // The line alone: 80 mm/s, 100 mm/s².
    const Case cases[] = {
        {"no cap", PathCap(), 6400, 100},
        {"speed", {1600, INFINITY}, 1600, 100},
        {"accel", {INFINITY, 25}, 6400, 25},
        {"looser than the axes", {1e6f, 1e6f}, 6400, 100},
    };
    AxisLimits lim;
    for (int i = 0; i < 4; i++) { lim.max_feed[i] = 80; lim.max_accel[i] = 100; }
    for (const Case& c : cases) {
        CAPTURE(c.name);
        static Planner p;
        p.reset(Pos());
        REQUIRE(p.pushLine({100, 0}, 80, lim, 0.02f, c.cap));
        const Block* b = p.claim();
        REQUIRE(b);
        CHECK(b->path.v_max_sqr == doctest::Approx(c.v_max_sqr));
        CHECK(b->path.accel == doctest::Approx(c.accel));
    }
}
