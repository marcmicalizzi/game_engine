#pragma once

// Index-based proxy iterator over a container that stores keys and values in parallel arrays.
// Dereferencing yields std::pair<const Key&, Value&>; write `for (auto [k, v] : container)`.
// Shared by FlatMap and HashMap.

#include <core/base/types.h>

#include <compare>
#include <iterator>
#include <type_traits>
#include <utility>

namespace engine::containers::detail {

// Grants iterators access to a container's parallel arrays without making them public.
// A container opts in with `friend struct containers::detail::SoaAccess;` and private
// `keys_ptr()` / `values_ptr()` accessors.
struct SoaAccess {
  template <class C>
  static auto* keys(C* c) noexcept {
    return c->keys_ptr();
  }
  template <class C>
  static auto* values(C* c) noexcept {
    return c->values_ptr();
  }
};

template <class Container, bool IsConst>
class SoaIterator {
  using container_pointer = std::conditional_t<IsConst, const Container*, Container*>;
  using Key = typename Container::key_type;
  using Value = typename Container::mapped_type;
  using size_type = typename Container::size_type;

 public:
  using iterator_concept = std::random_access_iterator_tag;
  using iterator_category = std::input_iterator_tag;  // proxy reference
  using value_type = std::pair<Key, Value>;
  using difference_type = isize;
  using reference = std::conditional_t<IsConst, std::pair<const Key&, const Value&>,
                                       std::pair<const Key&, Value&>>;

  struct pointer {
    reference ref;
    const reference* operator->() const noexcept { return &ref; }
  };

  SoaIterator() noexcept = default;
  SoaIterator(container_pointer c, size_type i) noexcept : c_(c), i_(i) {}

  template <bool WasConst>
    requires(IsConst && !WasConst)
  SoaIterator(const SoaIterator<Container, WasConst>& other) noexcept
      : c_(other.c_), i_(other.i_) {}

  reference operator*() const noexcept {
    return reference{SoaAccess::keys(c_)[i_], SoaAccess::values(c_)[i_]};
  }
  pointer operator->() const noexcept { return pointer{**this}; }
  reference operator[](difference_type n) const noexcept { return *(*this + n); }

  SoaIterator& operator++() noexcept {
    ++i_;
    return *this;
  }
  SoaIterator operator++(int) noexcept {
    SoaIterator tmp = *this;
    ++i_;
    return tmp;
  }
  SoaIterator& operator--() noexcept {
    --i_;
    return *this;
  }
  SoaIterator operator--(int) noexcept {
    SoaIterator tmp = *this;
    --i_;
    return tmp;
  }
  SoaIterator& operator+=(difference_type n) noexcept {
    i_ = static_cast<size_type>(static_cast<difference_type>(i_) + n);
    return *this;
  }
  SoaIterator& operator-=(difference_type n) noexcept { return *this += -n; }
  friend SoaIterator operator+(SoaIterator it, difference_type n) noexcept { return it += n; }
  friend SoaIterator operator+(difference_type n, SoaIterator it) noexcept { return it += n; }
  friend SoaIterator operator-(SoaIterator it, difference_type n) noexcept { return it -= n; }

  template <bool OtherConst>
  difference_type operator-(const SoaIterator<Container, OtherConst>& other) const noexcept {
    return static_cast<difference_type>(i_) - static_cast<difference_type>(other.i_);
  }
  template <bool OtherConst>
  bool operator==(const SoaIterator<Container, OtherConst>& other) const noexcept {
    return i_ == other.i_;
  }
  template <bool OtherConst>
  std::strong_ordering operator<=>(const SoaIterator<Container, OtherConst>& other) const noexcept {
    return i_ <=> other.i_;
  }

  // Position in the underlying arrays; valid until the next insert or erase.
  size_type index() const noexcept { return i_; }

 private:
  template <class, bool>
  friend class SoaIterator;

  container_pointer c_ = nullptr;
  size_type i_ = 0;
};

}  // namespace engine::containers::detail
