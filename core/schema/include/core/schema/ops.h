#pragma once

// Type-erased operation tables referenced from generated TypeRef/TypeInfo constants. Each is a
// constexpr static so `&XxxOpsFor<T>::ops` is a constant expression and the generated tables
// are constant-initialized.

#include <core/base/types.h>
#include <core/containers/flat_map.h>
#include <core/containers/vector.h>
#include <core/schema/type_info.h>

#include <array>
#include <memory>
#include <optional>

namespace engine::schema {

template <class T>
struct StructOpsFor {
  static constexpr StructOps ops{
      [](void* p) { std::construct_at(static_cast<T*>(p)); },
      [](void* p) { std::destroy_at(static_cast<T*>(p)); },
      [](void* d, const void* s) { *static_cast<T*>(d) = *static_cast<const T*>(s); },
      [](const void* a, const void* b) { return *static_cast<const T*>(a) == *static_cast<const T*>(b); },
  };
};

template <class Opt>
struct OptionalOpsFor {
  static constexpr OptionalOps ops{
      [](const void* p) { return static_cast<const Opt*>(p)->has_value(); },
      [](const void* p) -> const void* { return &**static_cast<const Opt*>(p); },
      [](void* p) -> void* { return &static_cast<Opt*>(p)->emplace(); },
      [](void* p) { static_cast<Opt*>(p)->reset(); },
  };
};

template <class Vec>
struct ArrayOpsFor {
  using size_type = typename Vec::size_type;
  static constexpr ArrayOps ops{
      [](const void* p) -> usize { return static_cast<const Vec*>(p)->size(); },
      [](const void* p, usize i) -> const void* { return &(*static_cast<const Vec*>(p))[static_cast<size_type>(i)]; },
      [](void* p, usize i) -> void* { return &(*static_cast<Vec*>(p))[static_cast<size_type>(i)]; },
      [](void* p) -> void* { return &static_cast<Vec*>(p)->emplace_back(); },
      [](void* p) { static_cast<Vec*>(p)->clear(); },
  };
};

template <class Arr>
struct FixedArrayOpsFor {
  static constexpr ArrayOps ops{
      [](const void* p) -> usize { return static_cast<const Arr*>(p)->size(); },
      [](const void* p, usize i) -> const void* { return &(*static_cast<const Arr*>(p))[i]; },
      [](void* p, usize i) -> void* { return &(*static_cast<Arr*>(p))[i]; },
      nullptr,
      nullptr,
  };
};

template <class Map>
struct MapOpsFor {
  using key_type = typename Map::key_type;
  using size_type = typename Map::size_type;
  static constexpr MapOps ops{
      [](const void* p) -> usize { return static_cast<const Map*>(p)->size(); },
      [](const void* p, usize i) -> const void* { return &static_cast<const Map*>(p)->key_at(static_cast<size_type>(i)); },
      [](const void* p, usize i) -> const void* { return &static_cast<const Map*>(p)->value_at(static_cast<size_type>(i)); },
      [](void* p, const void* key) -> void* {
        return &static_cast<Map*>(p)->try_emplace(*static_cast<const key_type*>(key)).first->second;
      },
      [](void* p) { static_cast<Map*>(p)->clear(); },
  };
};

}  // namespace engine::schema
