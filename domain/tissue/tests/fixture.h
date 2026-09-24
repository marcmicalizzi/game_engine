#pragma once

// The tests' view of the synthetic tissue definition (domain/tissue/synthetic.h): the generator is
// the module's, so `engine-content tissue example` and the end-to-end test build the same one;
// what lives here are the helpers the tests break it with.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/tissue/synthetic.h>
#include <domain/tissue/tissue_file.h>

#include <cstring>
#include <string>

namespace engine::tissue::fixture {

using namespace engine::tissue::synthetic;
using Built = SyntheticTissue;

inline Built make() { return make_synthetic_tissue(); }

template <class T>
void add(TissueFile& file, const std::string& name, BlockKind kind, const Vector<T>& data,
         u32 per_element = 1) {
  add_block(file, name, kind, std::span<const T>(data.data(), data.size()), per_element);
}

// Replaces a block's bytes (and reseals its table entry): how the tests break things.
template <class T>
void replace(TissueFile& file, const std::string& name, const Vector<T>& data,
             u32 per_element = 1) {
  for (TissueBlock& block : file.blocks)
    if (block.name == name) {
      block.count = data.size() / per_element;
      block.bytes.resize(static_cast<u32>(data.size() * sizeof(T)));
      if (!data.empty()) std::memcpy(block.bytes.data(), data.data(), block.bytes.size());
      seal_block(file, block);
    }
}

template <class T>
Vector<T> read(const TissueFile& file, const std::string& name) {
  Vector<T> out;
  const TissueBlock* block = file.find(name);
  if (block == nullptr) return out;
  out.resize(static_cast<u32>(block->bytes.size() / sizeof(T)));
  std::memcpy(out.data(), block->bytes.data(), block->bytes.size());
  return out;
}

}  // namespace engine::tissue::fixture
