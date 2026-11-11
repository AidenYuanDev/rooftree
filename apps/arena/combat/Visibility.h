/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Who sees whom, the area of interest: each viewer sees the units within ViewDistance of it, nearest
 * first, at most MaxVisible of them counting itself, which comes first. The world gives each client only
 * what its player sees (its State, its combat log), so what one client gets no longer grows with
 * everyone online.
 *
 * Update rebuilds it all from the positions, once a world update after the units moved: a uniform
 * grid of ViewDistance cells, so a viewer only looks through the 3x3 cells round it.
 */

#ifndef ARENA_VISIBILITY_H
#define ARENA_VISIBILITY_H

#include "Define.h"
#include <unordered_map>
#include <vector>

struct VisibilityPoint
{
    uint32 Id;
    float X;
    float Y;
};

class Visibility
{
public:
    // Every point is a unit and a viewer. maxVisible counts the viewer itself; 0 for no limit
    void Update(std::vector<VisibilityPoint> const& points, float viewDistance, uint32 maxVisible);

    // The viewer itself first, then nearest first. Empty for an id the last Update didn't have
    std::vector<uint32> const& GetVisible(uint32 viewer) const;
    // The viewers that see the unit, itself among them
    std::vector<uint32> const& GetViewers(uint32 id) const;

private:
    std::unordered_map<uint32, std::vector<uint32>> _visible;
    std::unordered_map<uint32, std::vector<uint32>> _viewers;
    // Points by cell; a cell stays while someone is in it, its vector reused
    std::unordered_map<uint64, std::vector<VisibilityPoint>> _grid;
};

#endif
