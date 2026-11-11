/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include <catch2/catch_all.hpp>

#include "Visibility.h"
#include <algorithm>
#include <random>

using Ids = std::vector<uint32>;

TEST_CASE("Visibility: within the distance, the viewer first, then nearest first", "[visibility]")
{
    Visibility visibility;
    visibility.Update({ { 1, 100, 100 }, { 2, 400, 100 }, { 3, 150, 100 }, { 4, 700, 100 } }, 500.0f, 0);

    CHECK(visibility.GetVisible(1) == Ids{ 1, 3, 2 });
    CHECK(visibility.GetVisible(4) == Ids{ 4, 2 });
    // 3 is 250 away, 1 and 4 both 300: the lower id first
    CHECK(visibility.GetVisible(2) == Ids{ 2, 3, 1, 4 });
    CHECK(visibility.GetVisible(9).empty());
}

TEST_CASE("Visibility: who sees a unit is the other way round", "[visibility]")
{
    Visibility visibility;
    visibility.Update({ { 1, 100, 100 }, { 2, 400, 100 }, { 3, 150, 100 }, { 4, 700, 100 } }, 500.0f, 0);

    Ids viewers = visibility.GetViewers(1);
    std::ranges::sort(viewers);
    CHECK(viewers == Ids{ 1, 2, 3 });
    viewers = visibility.GetViewers(4);
    std::ranges::sort(viewers);
    CHECK(viewers == Ids{ 2, 4 });
}

TEST_CASE("Visibility: at most MaxVisible, the nearest kept", "[visibility]")
{
    Visibility visibility;
    std::vector<VisibilityPoint> points;
    for (uint32 i = 0; i < 20; ++i)
        points.push_back({ i + 1, 100.0f + i * 10.0f, 100.0f });
    visibility.Update(points, 1000.0f, 4);

    CHECK(visibility.GetVisible(1) == Ids{ 1, 2, 3, 4 });
    CHECK(visibility.GetVisible(10).size() == 4);
    CHECK(visibility.GetVisible(10)[0] == 10);
}

TEST_CASE("Visibility: across cell edges and negative positions", "[visibility]")
{
    Visibility visibility;
    // Cells -1, 0 and 1 of y: 2 sees both (28 and 195 away), 1 and 3 are 216 apart
    visibility.Update({ { 1, -10, -10 }, { 2, 10, 10 }, { 3, 10, 205 } }, 200.0f, 0);

    CHECK(visibility.GetVisible(1) == Ids{ 1, 2 });
    CHECK(visibility.GetVisible(2) == Ids{ 2, 1, 3 });
    CHECK(visibility.GetVisible(3) == Ids{ 3, 2 });
}

TEST_CASE("Visibility: a later update forgets who left", "[visibility]")
{
    Visibility visibility;
    visibility.Update({ { 1, 0, 0 }, { 2, 10, 0 } }, 100.0f, 0);
    visibility.Update({ { 1, 0, 0 } }, 100.0f, 0);

    CHECK(visibility.GetVisible(1) == Ids{ 1 });
    CHECK(visibility.GetVisible(2).empty());
    CHECK(visibility.GetViewers(2).empty());
}

TEST_CASE("Visibility: the grid agrees with looking at everyone", "[visibility]")
{
    std::mt19937 random(7);
    std::uniform_real_distribution<float> position(-500.0f, 3000.0f);
    std::vector<VisibilityPoint> points;
    for (uint32 i = 1; i <= 300; ++i)
        points.push_back({ i, position(random), position(random) });

    Visibility visibility;
    visibility.Update(points, 400.0f, 0);
    for (VisibilityPoint const& viewer : points)
    {
        Ids expected;
        for (VisibilityPoint const& other : points)
        {
            float dx = other.X - viewer.X, dy = other.Y - viewer.Y;
            if (dx * dx + dy * dy <= 400.0f * 400.0f)
                expected.push_back(other.Id);
        }
        Ids visible = visibility.GetVisible(viewer.Id);
        std::ranges::sort(expected);
        std::ranges::sort(visible);
        REQUIRE(visible == expected);
    }
}
