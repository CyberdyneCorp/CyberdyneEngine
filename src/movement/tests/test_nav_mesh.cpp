// SPDX-License-Identifier: MIT
// THE CONVERTED NAVIGATION MESH OF A FIXED WORLD. Task 6.2, design §9.1.
//
// The conversion keeps the source's polygons in reference order with their adjacency, and every
// query on it decides on exact integer products with an index tie-break: a point exactly on a
// shared edge belongs to the lower polygon, on every peer.

#include <cy/movement/nav_mesh.h>

#include <initializer_list>

#include "movement_fixture.h"

namespace {

using cy::u32;
using cy::movement::FixedNavMesh;
using cy::movement::FixedPolyIndex;
using cy::movement::kNoPoly;
using namespace cy::movement_test;

}  // namespace

CY_TEST_CASE("movement mesh: conversion keeps every polygon, its adjacency and its centre") {
    const cy::navigation::NavMesh source = open_mesh(4);
    FixedNavMesh mesh(allocator());
    const auto report = mesh.convert(source);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report->polys, 16U);
    CY_CHECK_EQ(report->corners, 64U);
    // 24 shared edges, seen from both sides; the 16 on the outline have nothing across them.
    CY_CHECK_EQ(report->internal_edges, 48U);
    CY_CHECK_EQ(report->border_edges, 16U);
    CY_CHECK_EQ(mesh.poly_count(), 16U);

    CY_CHECK(mesh.poly(0).centre == (FixedVec2{fraction(1, 1), fraction(1, 1)}));
    CY_CHECK(mesh.poly(5).centre == (FixedVec2{fraction(3, 1), fraction(3, 1)}));
    CY_CHECK(mesh.poly(0).cost == Fixed::one());
    CY_CHECK(mesh.min() == at(0, 0));
    CY_CHECK(mesh.max() == at(4, 4));

    // Polygon 5 (row 1, column 1) has four neighbours: 4, 6, 1 and 9, in some edge order.
    u32 seen = 0;
    for (const FixedPolyIndex across : mesh.neighbours_of(5)) {
        for (const FixedPolyIndex expected : {1U, 4U, 6U, 9U}) {
            seen += across == expected ? 1U : 0U;
        }
    }
    CY_CHECK_EQ(seen, 4U);
    FixedVec2 a;
    FixedVec2 b;
    CY_REQUIRE(mesh.portal(5, 6, a, b));
    CY_CHECK(a.x == metres(2));
    CY_CHECK(b.x == metres(2));
    CY_CHECK_FALSE(mesh.portal(5, 10, a, b));
}

CY_TEST_CASE("movement mesh: a point on a shared edge is located in the lower polygon") {
    const FixedNavMesh mesh = converted(open_mesh(4));
    CY_CHECK_EQ(mesh.locate(FixedVec2{fraction(1, 1), fraction(1, 1)}), 0U);
    CY_CHECK_EQ(mesh.locate(FixedVec2{fraction(3, 1), fraction(1, 1)}), 1U);
    // On the edge between 0 and 1, and on the corner shared by 0, 1, 4 and 5.
    CY_CHECK_EQ(mesh.locate(FixedVec2{metres(1), fraction(1, 1)}), 0U);
    CY_CHECK_EQ(mesh.locate(at(1, 1)), 0U);
    CY_CHECK_EQ(mesh.locate(at(4, 4)), 15U);
    // One ulp off the mesh is off it.
    CY_CHECK_EQ(mesh.locate(FixedVec2{Fixed::from_raw(-1), fraction(1, 1)}), kNoPoly);
    CY_CHECK_EQ(mesh.locate(at(9, 9)), kNoPoly);
}

CY_TEST_CASE("movement mesh: clamp_move keeps a unit on the surface and tracks its polygon") {
    const FixedNavMesh mesh = converted(open_mesh(4));

    FixedPolyIndex poly = 0;
    // Into the neighbour: allowed, and the polygon follows.
    CY_CHECK(mesh.clamp_move(poly, FixedVec2{fraction(3, 1), fraction(1, 1)}) ==
             (FixedVec2{fraction(3, 1), fraction(1, 1)}));
    CY_CHECK_EQ(poly, 1U);

    // Off the outline: stopped on it.
    poly = 0;
    CY_CHECK(mesh.clamp_move(poly, FixedVec2{metres(-1), fraction(1, 1)}) ==
             (FixedVec2{metres(0), fraction(1, 1)}));
    CY_CHECK_EQ(poly, 0U);

    // Two polygons in one tick: stopped at the far edge of the ring rather than tunnelling.
    poly = 0;
    CY_CHECK(mesh.clamp_move(poly, FixedVec2{fraction(5, 1), fraction(1, 1)}) ==
             (FixedVec2{metres(2), fraction(1, 1)}));
    CY_CHECK_EQ(poly, 1U);

    // A unit with no polygon yet is snapped to the nearest one.
    poly = kNoPoly;
    CY_CHECK(mesh.clamp_move(poly, at(-3, 2)) == at(0, 2));
    CY_CHECK_EQ(poly, 4U);
}

CY_TEST_CASE("movement mesh: nearest snaps a point off the mesh, ties going to the lower index") {
    // A hole at row 1, column 1: polygon (1,1) is missing.
    const FixedNavMesh mesh = converted(
        grid_mesh(4.0F, 4, [](u32 row, u32 column) noexcept { return row != 1 || column != 1; }));
    CY_CHECK_EQ(mesh.poly_count(), 15U);
    FixedVec2 on;
    // The hole's centre is equidistant from four polygons' edges; the lowest index wins.
    const FixedPolyIndex snapped = mesh.nearest(FixedVec2{fraction(3, 1), fraction(3, 1)}, on);
    CY_CHECK_EQ(snapped, 1U);
    CY_CHECK(on == (FixedVec2{fraction(3, 1), metres(1)}));
}

CY_TEST_CASE("movement mesh: a rebuilt source is refused as authoritative input") {
    cy::navigation::NavMesh source = open_mesh(4);
    const FixedNavMesh mesh = converted(source);
    CY_CHECK(mesh.check_source(source).has_value());
    CY_CHECK_EQ(mesh.source_version(), source.version());

    // A runtime rebuild republishes the tile: float work on each peer.
    CY_REQUIRE(source
                   .add_tile(cy::navigation::testing::grid_tile(
                       allocator(), cy::navigation::TileCoord{0, 0, 0}, 4.0F, 4))
                   .has_value());
    const cy::Status refused = mesh.check_source(source);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::PermissionDenied);
}

CY_TEST_CASE("movement mesh: the digest is a function of the converted content") {
    const FixedNavMesh first = converted(open_mesh(4));
    const FixedNavMesh second = converted(open_mesh(4));
    CY_CHECK_EQ(first.digest(), second.digest());
    const FixedNavMesh holed = converted(
        grid_mesh(4.0F, 4, [](u32 row, u32 column) noexcept { return row != 2 || column != 2; }));
    CY_CHECK_NE(first.digest(), holed.digest());

    FixedNavMesh blocked = converted(open_mesh(4));
    CY_CHECK_FALSE(blocked.blocked(3));
    blocked.set_blocked(3, true);
    CY_CHECK(blocked.blocked(3));
    blocked.set_blocked(3, false);
    CY_CHECK_FALSE(blocked.blocked(3));
}
