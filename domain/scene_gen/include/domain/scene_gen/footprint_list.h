#pragma once

// **A list of footprints that owns its polygons' points** (roadmap R144; scene_gen.md, "The shapes
// a tile occupies"): what a tile hands beside its placements and its meshes
// (`Placements::shapes`), and what a host keeps of the entries before the one it is asking
// (`Context::earlier`).
//
// **Why a list of its own.** A `Footprint` that is a polygon names its points by a pointer
// (exclusion.h), and W98's review asked who holds them: a generator's scratch dies when its call
// returns, a generator's state is closed before the next entry of a whole read is opened, and a
// plain `Vector<Footprint>` beside a `Vector<FootprintPoint>` dangles the first time the points
// grow. So the answer is the list: `add` copies a polygon's points into the list's own store and
// points the copy at them, and the store growing, the list being copied or moved, all leave every
// polygon pointing at its own points. A disc and a band are whole in themselves and are copied as
// they are.
//
// **What it costs.** A footprint is 80 bytes and a polygon's point 16; a polygon costs eight more
// for its place in the store. Adding is amortised constant: the polygons are pointed again only
// when the store moved.

#include "domain/scene_gen/exclusion.h"

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <span>
#include <utility>

namespace engine::scene_gen {

class FootprintList {
 public:
  FootprintList() = default;
  FootprintList(const FootprintList& other)
      : shapes_(other.shapes_), polygons_(other.polygons_), points_(other.points_) {
    point();
  }
  FootprintList& operator=(const FootprintList& other) {
    if (this != &other) {
      shapes_ = other.shapes_;
      polygons_ = other.polygons_;
      points_ = other.points_;
      point();
    }
    return *this;
  }
  FootprintList(FootprintList&& other) noexcept
      : shapes_(std::move(other.shapes_)),
        polygons_(std::move(other.polygons_)),
        points_(std::move(other.points_)) {
    point();
    other.clear();
  }
  FootprintList& operator=(FootprintList&& other) noexcept {
    if (this != &other) {
      shapes_ = std::move(other.shapes_);
      polygons_ = std::move(other.polygons_);
      points_ = std::move(other.points_);
      point();
      other.clear();
    }
    return *this;
  }

  void clear() noexcept {
    shapes_.clear();
    polygons_.clear();
    points_.clear();
  }
  bool empty() const noexcept { return shapes_.empty(); }
  u32 size() const noexcept { return shapes_.size(); }
  u32 point_count() const noexcept { return points_.size(); }
  void reserve(u32 shapes) { shapes_.reserve(shapes); }

  // Adds a copy of `shape`; a polygon's points are copied with it. `shape` is never one of this
  // list's own (its points would be read while the store they are in grows).
  void add(const Footprint& shape) {
    Footprint copy = shape;
    if (copy.shape != FootprintShape::polygon || copy.points == nullptr || copy.point_count == 0) {
      copy.points = nullptr;
      if (copy.shape == FootprintShape::polygon) copy.point_count = 0;
      shapes_.push_back(copy);
      return;
    }
    const FootprintPoint* const before = points_.data();
    const u32 first = points_.size();
    points_.reserve(first + copy.point_count);
    for (u32 k = 0; k < copy.point_count; ++k)
      points_.push_back(shape.points[k]);
    polygons_.push_back(Polygon{shapes_.size(), first});
    shapes_.push_back(copy);
    if (points_.data() != before) {
      point();
    } else {
      shapes_[shapes_.size() - 1].points = points_.data() + first;
    }
  }
  void add(std::span<const Footprint> shapes) {
    for (const Footprint& shape : shapes)
      add(shape);
  }
  // A disc, as a host makes one from a placement.
  void add_disc(i64 x_mm, i64 z_mm, f64 radius_m) {
    shapes_.push_back(Footprint{x_mm, z_mm, radius_m});
  }

  // The footprints, in the order they were added: valid until the next `add` or `clear`.
  std::span<const Footprint> span() const noexcept { return {shapes_.data(), shapes_.size()}; }
  const Footprint& operator[](u32 k) const noexcept { return shapes_[k]; }

 private:
  struct Polygon {
    u32 shape = 0;  // its place in `shapes_`
    u32 first = 0;  // its first point in `points_`
  };
  // Points every polygon at its points in this list's store.
  void point() noexcept {
    for (const Polygon& polygon : polygons_)
      shapes_[polygon.shape].points = points_.data() + polygon.first;
  }

  Vector<Footprint> shapes_;
  Vector<Polygon> polygons_;
  Vector<FootprintPoint> points_;
};

}  // namespace engine::scene_gen
