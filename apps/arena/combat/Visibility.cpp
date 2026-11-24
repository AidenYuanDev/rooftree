/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Visibility.h"

#include <algorithm>
#include <cmath>

namespace {
std::vector<uint32> const NoIds;

int32 CellOf(float position, float cellSize) {
  return int32(std::floor(position / cellSize));
}

uint64 CellKey(int32 x, int32 y) {
  return uint64(uint32(x)) << 32 | uint32(y);
}
}  // namespace

void Visibility::Update(
    std::vector<VisibilityPoint> const& points,
    float viewDistance,
    uint32 maxVisible) {
  for (auto& [id, ids] : _visible)
    ids.clear();
  for (auto& [id, ids] : _viewers)
    ids.clear();
  for (auto& [key, cell] : _grid)
    cell.clear();

  viewDistance = std::max(viewDistance, 1.0f);
  for (VisibilityPoint const& point : points)
    _grid[CellKey(CellOf(point.X, viewDistance), CellOf(point.Y, viewDistance))].push_back(point);

  float const range = viewDistance * viewDistance;
  std::vector<std::pair<float, uint32>> near;
  for (VisibilityPoint const& viewer : points) {
    near.clear();
    int32 cellX = CellOf(viewer.X, viewDistance);
    int32 cellY = CellOf(viewer.Y, viewDistance);
    for (int32 dx = -1; dx <= 1; ++dx) {
      for (int32 dy = -1; dy <= 1; ++dy) {
        auto cell = _grid.find(CellKey(cellX + dx, cellY + dy));
        if (cell == _grid.end())
          continue;
        for (VisibilityPoint const& other : cell->second) {
          if (other.Id == viewer.Id)
            continue;
          float distanceX = other.X - viewer.X;
          float distanceY = other.Y - viewer.Y;
          float distance = distanceX * distanceX + distanceY * distanceY;
          if (distance <= range)
            near.emplace_back(distance, other.Id);
        }
      }
    }

    // The nearest ones, the viewer taking one of the places
    std::size_t keep = maxVisible ? std::min<std::size_t>(near.size(), maxVisible - 1)
                                  : near.size();
    std::partial_sort(near.begin(), near.begin() + keep, near.end());

    std::vector<uint32>& visible = _visible[viewer.Id];
    visible.push_back(viewer.Id);
    for (std::size_t i = 0; i < keep; ++i)
      visible.push_back(near[i].second);
    for (uint32 id : visible)
      _viewers[id].push_back(viewer.Id);
  }

  // Gone since the last update: dropped, so the maps don't keep everyone who ever was
  std::erase_if(_visible, [](auto const& entry) { return entry.second.empty(); });
  std::erase_if(_viewers, [](auto const& entry) { return entry.second.empty(); });
  std::erase_if(_grid, [](auto const& entry) { return entry.second.empty(); });
}

std::vector<uint32> const& Visibility::GetVisible(uint32 viewer) const {
  auto itr = _visible.find(viewer);
  return itr != _visible.end() ? itr->second : NoIds;
}

std::vector<uint32> const& Visibility::GetViewers(uint32 id) const {
  auto itr = _viewers.find(id);
  return itr != _viewers.end() ? itr->second : NoIds;
}
