#include <core/base/assert.h>
#include <foundation/image/deflate.h>
#include <foundation/image/png.h>  // adler32 for the zlib trailer

#include <algorithm>
#include <bit>
#include <cstring>

namespace engine::image {

namespace {

static_assert(std::endian::native == std::endian::little,
              "deflate: the match comparison and the three-byte hash read whole words");

// ---- RFC 1951 constants and tables -------------------------------------------------------

constexpr u32 k_min_match = 3;
constexpr u32 k_max_match = 258;
constexpr u32 k_window_size = 32768;  // the furthest back a match may reach
constexpr u32 k_window_mask = k_window_size - 1;
constexpr u32 k_hash_bits = 15;
constexpr u32 k_hash_size = 1u << k_hash_bits;
constexpr u32 k_no_pos = 0xFFFFFFFFu;  // empty hash slot, and the end of a chain

constexpr u32 k_lit_symbols = 288;  // literal/length alphabet (286 used, 288 in the fixed code)
constexpr u32 k_lit_used = 286;
constexpr u32 k_dist_symbols = 30;        // distance alphabet
constexpr u32 k_clen_symbols = 19;        // code-length alphabet
constexpr u32 k_max_bits = 15;            // longest literal/length or distance code
constexpr u32 k_max_clen_bits = 7;        // longest code-length code
constexpr u32 k_max_unlimited_bits = 32;  // before the length limit is enforced

constexpr u32 k_max_block_bytes = 256u * 1024;  // input per block
constexpr u32 k_max_block_tokens = 65535;       // symbols per block
constexpr u32 k_max_stored = 65535;             // the LEN field of a stored block

constexpr u16 k_length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 k_length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 k_dist_base[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                 33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u8 k_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// The order the code-length code's own lengths are written in (RFC 1951 section 3.2.7).
constexpr u8 k_clen_order[k_clen_symbols] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                             11, 4,  12, 3, 13, 2, 14, 1, 15};

// Match length (3..258) to its length code (0..28).
struct LengthCodeTable {
  u8 code[k_max_match + 1];
  constexpr LengthCodeTable() : code{} {
    u32 c = 0;
    for (u32 len = k_min_match; len <= k_max_match; ++len) {
      while (c + 1 < 29 && k_length_base[c + 1] <= len)
        ++c;
      code[len] = static_cast<u8>(c);
    }
  }
};
constexpr LengthCodeTable k_length_code{};

// Distance to its distance code. Entries 0..255 are the distances 1..256; entries 256..511 are
// 128-wide buckets, which works because every code above 256 spans whole buckets.
struct DistCodeTable {
  u8 code[512];
  constexpr DistCodeTable() : code{} {
    u32 c = 0;
    for (u32 i = 0; i < 256; ++i) {
      while (c + 1 < k_dist_symbols && k_dist_base[c + 1] <= i + 1)
        ++c;
      code[i] = static_cast<u8>(c);
    }
    c = 0;
    for (u32 bucket = 2; bucket < 256; ++bucket) {
      const u32 distance = 128 * bucket + 1;
      while (c + 1 < k_dist_symbols && k_dist_base[c + 1] <= distance)
        ++c;
      code[256 + bucket] = static_cast<u8>(c);
    }
  }
};
constexpr DistCodeTable k_dist_code{};

constexpr u32 distance_code(u32 distance) noexcept {
  return distance <= 256 ? k_dist_code.code[distance - 1]
                         : k_dist_code.code[256 + ((distance - 1) >> 7)];
}

// ---- Huffman codes -----------------------------------------------------------------------

// A canonical code over `N` symbols. `code` is stored bit-reversed, because deflate writes
// Huffman codes most significant bit first inside a least-significant-bit-first stream.
template <u32 N>
struct Huffman {
  u8 len[N];
  u16 code[N];
};

constexpr u16 reverse_bits(u32 value, u32 count) noexcept {
  u32 r = 0;
  for (u32 i = 0; i < count; ++i)
    r = (r << 1) | ((value >> i) & 1u);
  return static_cast<u16>(r);
}

// RFC 1951 section 3.2.2: the codes follow from the lengths alone.
template <u32 N>
constexpr void assign_codes(Huffman<N>& h) noexcept {
  u32 length_count[k_max_bits + 1] = {};
  for (u32 i = 0; i < N; ++i)
    ++length_count[h.len[i]];
  length_count[0] = 0;
  u32 next_code[k_max_bits + 1] = {};
  u32 code = 0;
  for (u32 bits = 1; bits <= k_max_bits; ++bits) {
    code = (code + length_count[bits - 1]) << 1;
    next_code[bits] = code;
  }
  for (u32 i = 0; i < N; ++i) {
    const u32 l = h.len[i];
    h.code[i] = l != 0 ? reverse_bits(next_code[l]++, l) : u16{0};
  }
}

constexpr Huffman<k_lit_symbols> make_fixed_lit() noexcept {
  Huffman<k_lit_symbols> h{};
  for (u32 i = 0; i < 144; ++i)
    h.len[i] = 8;
  for (u32 i = 144; i < 256; ++i)
    h.len[i] = 9;
  for (u32 i = 256; i < 280; ++i)
    h.len[i] = 7;
  for (u32 i = 280; i < 288; ++i)
    h.len[i] = 8;
  assign_codes(h);
  return h;
}
constexpr Huffman<k_lit_symbols> k_fixed_lit = make_fixed_lit();

constexpr Huffman<k_dist_symbols> make_fixed_dist() noexcept {
  Huffman<k_dist_symbols> h{};
  for (u32 i = 0; i < k_dist_symbols; ++i) {
    h.len[i] = 5;
    h.code[i] = reverse_bits(i, 5);
  }
  return h;
}
constexpr Huffman<k_dist_symbols> k_fixed_dist = make_fixed_dist();

// Moffat and Katajainen's in-place minimum-redundancy code: `a` holds the frequencies of `n`
// symbols sorted ascending and comes back holding each symbol's code length. Linear after the
// sort, with no tree nodes to allocate.
void minimum_redundancy(i32* a, i32 n) noexcept {
  if (n < 2) {
    if (n == 1) a[0] = 1;
    return;
  }
  a[0] += a[1];
  i32 root = 0;
  i32 leaf = 2;
  i32 next = 0;
  for (next = 1; next < n - 1; ++next) {
    // Twice: the smaller of the next leaf and the next internal node becomes a child.
    if (leaf >= n || a[root] < a[leaf]) {
      a[next] = a[root];
      a[root++] = next;
    } else {
      a[next] = a[leaf++];
    }
    if (leaf >= n || (root < next && a[root] < a[leaf])) {
      a[next] += a[root];
      a[root++] = next;
    } else {
      a[next] += a[leaf++];
    }
  }
  a[n - 2] = 0;
  for (next = n - 3; next >= 0; --next)
    a[next] = a[a[next]] + 1;
  // Internal-node depths become leaf depths, deepest first.
  i32 available = 1;
  i32 used = 0;
  i32 depth = 0;
  root = n - 2;
  next = n - 1;
  while (available > 0) {
    while (root >= 0 && a[root] == depth) {
      ++used;
      --root;
    }
    while (available > used) {
      a[next--] = depth;
      --available;
    }
    available = 2 * used;
    ++depth;
    used = 0;
  }
}

// Folds every code longer than `max_len` back to the limit, then repairs the Kraft sum by
// lengthening one shorter code per folded code. `histogram` counts symbols per code length.
void enforce_max_length(u32* histogram, u32 max_len) noexcept {
  for (u32 i = max_len + 1; i <= k_max_unlimited_bits; ++i) {
    histogram[max_len] += histogram[i];
    histogram[i] = 0;
  }
  u32 total = 0;
  for (u32 i = max_len; i >= 1; --i)
    total += histogram[i] << (max_len - i);
  const u32 target = 1u << max_len;
  while (total > target) {
    ENGINE_ASSERT(histogram[max_len] > 0, "deflate: no long code left to drop");
    --histogram[max_len];
    for (u32 i = max_len - 1; i >= 1; --i) {
      if (histogram[i] != 0) {
        --histogram[i];
        histogram[i + 1] += 2;
        break;
      }
    }
    --total;
  }
}

struct SymbolFreq {
  u32 freq;
  u16 symbol;
};

// Builds the canonical code for `count` symbols with lengths limited to `max_len`. A code with
// fewer than two used symbols is padded to two one-bit codes: an incomplete code is legal in
// some places and not others, and two bits per block is not worth the argument.
template <u32 N>
void build_tree(const u32* freq, u32 count, u32 max_len, Huffman<N>& h) {
  static_assert(N >= 2, "deflate: a code needs at least two symbols");
  ENGINE_ASSERT(count <= N, "deflate: alphabet larger than the code");
  for (u32 i = 0; i < N; ++i) {
    h.len[i] = 0;
    h.code[i] = 0;
  }

  SymbolFreq sorted[N];
  u32 used = 0;
  for (u32 i = 0; i < count; ++i) {
    if (freq[i] != 0) sorted[used++] = SymbolFreq{freq[i], static_cast<u16>(i)};
  }
  if (used < 2) {
    const u16 first = used == 1 ? sorted[0].symbol : u16{0};
    const u16 second = first == 0 ? u16{1} : u16{0};
    h.len[first] = 1;
    h.len[second] = 1;
    assign_codes(h);
    return;
  }

  // Ascending frequency, the symbol index breaking ties so the output never depends on the sort.
  std::sort(sorted, sorted + used, [](const SymbolFreq& a, const SymbolFreq& b) {
    return a.freq != b.freq ? a.freq < b.freq : a.symbol < b.symbol;
  });

  i32 work[N];
  for (u32 i = 0; i < used; ++i)
    work[i] = static_cast<i32>(sorted[i].freq);
  minimum_redundancy(work, static_cast<i32>(used));

  u32 histogram[k_max_unlimited_bits + 1] = {};
  for (u32 i = 0; i < used; ++i) {
    u32 depth = static_cast<u32>(work[i]);
    if (depth > k_max_unlimited_bits) depth = k_max_unlimited_bits;
    ++histogram[depth];
  }
  enforce_max_length(histogram, max_len);

  // The sort put the rarest symbol first, and the longest codes belong to the rarest symbols.
  u32 at = 0;
  for (u32 len = max_len; len >= 1; --len) {
    for (u32 k = 0; k < histogram[len]; ++k)
      h.len[sorted[at++].symbol] = static_cast<u8>(len);
  }
  ENGINE_ASSERT(at == used, "deflate: the length histogram lost a symbol");
  assign_codes(h);
}

// ---- bit writer --------------------------------------------------------------------------

// Least significant bit first, as RFC 1951 section 3.1.1 requires, into an engine `Vector`.
class BitWriter {
 public:
  explicit BitWriter(Vector<u8>& out) noexcept : out_(out) {}

  ENGINE_FORCE_INLINE void put(u32 value, u32 count) noexcept {
    ENGINE_ASSERT(count <= 28 && value < (1u << count),
                  "deflate: value does not fit in the bit count");
    acc_ |= static_cast<u64>(value) << bits_;
    bits_ += count;
    while (bits_ >= 8) {
      out_.push_back(static_cast<u8>(acc_));
      acc_ >>= 8;
      bits_ -= 8;
    }
  }

  // Pads the current byte with zeros; a stored block's header has to be byte aligned.
  void align() noexcept {
    if (bits_ != 0) {
      out_.push_back(static_cast<u8>(acc_));
      acc_ = 0;
      bits_ = 0;
    }
  }

  void put_bytes(std::span<const u8> bytes) noexcept {
    ENGINE_ASSERT(bits_ == 0, "deflate: unaligned bulk write");
    // `Vector::append` copies element by element and reserves exactly what it needs, which
    // for a stored stream is a byte-at-a-time copy plus a reallocation per 64 KiB block.
    // Grow by halves and move the bytes with one memcpy: 1.35 ns/byte becomes 0.88.
    if (bytes.empty()) return;
    const u32 at = out_.size();
    const u32 needed = at + static_cast<u32>(bytes.size());
    if (needed > out_.capacity()) out_.reserve(needed + needed / 2);
    out_.resize(needed);
    std::memcpy(out_.data() + at, bytes.data(), bytes.size());
  }

 private:
  Vector<u8>& out_;
  u64 acc_ = 0;
  u32 bits_ = 0;
};

// Raw bytes framed as stored blocks of at most 65535, BFINAL on the last one when `final`.
// An empty range still writes one block, which is what an empty deflate stream is.
void write_stored(BitWriter& writer, const u8* data, u32 start, u32 end, bool final) {
  u32 at = start;
  do {
    const u32 len = end - at < k_max_stored ? end - at : k_max_stored;
    const bool last = at + len == end;
    writer.put(last && final ? 1u : 0u, 1);
    writer.put(0, 2);  // BTYPE 00
    writer.align();
    const u8 header[4] = {static_cast<u8>(len), static_cast<u8>(len >> 8), static_cast<u8>(~len),
                          static_cast<u8>(~len >> 8)};
    writer.put_bytes(std::span<const u8>(header, 4));
    writer.put_bytes(std::span<const u8>(data + at, len));
    at += len;
  } while (at < end);
}

// ---- the compressor ----------------------------------------------------------------------

struct Token {
  u16 value;     // the literal byte, or the match length (3..258) when `distance` is set
  u16 distance;  // 0 for a literal
};

// One run-length item of the code-length stream (RFC 1951 section 3.2.7).
struct ClItem {
  u8 symbol;
  u8 extra_bits;
  u8 extra_value;
};

// Level parameters as types, so every loop is specialized and no level knob is read inside the
// matcher (docs/plan/11-performance-principles.md section 11.4).
struct FastLevel {
  static constexpr u32 max_chain = 4;     // hash-chain probes per position
  static constexpr u32 nice_length = 32;  // stop probing once a match is this long
  static constexpr u32 good_length = 0;   // unused without lazy matching
  static constexpr u32 max_lazy = 0;
  static constexpr u32 insert_limit = 4;  // longer matches do not index their interior
  static constexpr bool lazy = false;
  static constexpr bool dynamic = false;
};

struct DefaultLevel {
  static constexpr u32 max_chain = 32;
  static constexpr u32 nice_length = 128;
  static constexpr u32 good_length = 8;  // a match this long shortens the next chain walk
  static constexpr u32 max_lazy = 16;    // no second look once the held match is this long
  static constexpr u32 insert_limit = k_max_match;
  static constexpr bool lazy = true;
  static constexpr bool dynamic = true;
};

ENGINE_FORCE_INLINE u32 common_prefix(const u8* a, const u8* b, u32 max) noexcept {
  u32 i = 0;
  while (i + 8 <= max) {
    u64 x = 0;
    u64 y = 0;
    std::memcpy(&x, a + i, 8);
    std::memcpy(&y, b + i, 8);
    if (x != y) return i + static_cast<u32>(std::countr_zero(x ^ y)) / 8;
    i += 8;
  }
  while (i < max && a[i] == b[i])
    ++i;
  return i;
}

class Compressor {
 public:
  Compressor(std::span<const u8> input, Vector<u8>& out)
      : in_(input.data()),
        size_(static_cast<u32>(input.size())),
        hash_end_(input.size() >= 4 ? static_cast<u32>(input.size()) - 3 : 0),
        writer_(out) {
    head_.assign(k_hash_size, k_no_pos);
    prev_.resize(k_window_size);
    tokens_.reserve(k_max_block_tokens + 1);
    reset_block();
  }

  template <class L>
  void run() {
    if constexpr (L::lazy) {
      tokenize_lazy<L>();
    } else {
      tokenize_greedy<L>();
    }
  }

 private:
  // --- matching ---------------------------------------------------------------------------

  ENGINE_FORCE_INLINE u32 hash_at(u32 pos) const noexcept {
    u32 v = 0;
    std::memcpy(&v, in_ + pos, 4);
    return ((v & 0x00FFFFFFu) * 2654435761u) >> (32 - k_hash_bits);
  }

  // Puts `pos` at the head of its chain and returns what the head held before.
  ENGINE_FORCE_INLINE u32 insert(u32 pos, u32 hash) noexcept {
    const u32 previous = head_[hash];
    prev_[pos & k_window_mask] = previous;
    head_[hash] = pos;
    return previous;
  }

  // Indexes the interior of a match so later positions can reach into it.
  ENGINE_FORCE_INLINE void insert_range(u32 first, u32 last) noexcept {
    for (u32 p = first; p < last; ++p) {
      if (p >= hash_end_) return;
      (void)insert(p, hash_at(p));
    }
  }

  // Walks the chain from `chain_head` for the longest match at `pos` that beats `floor`.
  // Returns 0 when there is none, otherwise the length, with the distance in `out_distance`.
  // Every candidate is verified byte for byte, so a chain that aliases at the window edge
  // costs a probe and never a wrong match.
  template <class L>
  u32 find_match(u32 pos, u32 chain_head, u32 floor, u32& out_distance) const noexcept {
    u32 max_len = size_ - pos;
    if (max_len > k_max_match) max_len = k_max_match;
    if (max_len < k_min_match || floor >= max_len) return 0;

    u32 best_len = floor >= k_min_match - 1 ? floor : k_min_match - 1;
    u32 best_distance = 0;
    const u32 oldest = pos > k_window_size ? pos - k_window_size : 0;
    u32 chain = L::max_chain;
    if constexpr (L::good_length != 0) {
      if (best_len >= L::good_length) chain = chain / 4 + 1;
    }
    u32 candidate = chain_head;
    while (candidate != k_no_pos && candidate >= oldest) {
      // Cheap rejection first: a candidate that does not already reach past the best match,
      // or that differs in its first byte, cannot win.
      if (in_[candidate + best_len] == in_[pos + best_len] &&
          in_[candidate + best_len - 1] == in_[pos + best_len - 1] && in_[candidate] == in_[pos]) {
        const u32 len = common_prefix(in_ + candidate, in_ + pos, max_len);
        if (len > best_len) {
          best_len = len;
          best_distance = pos - candidate;
          if (len >= L::nice_length || len >= max_len) break;
        }
      }
      if (--chain == 0) break;
      candidate = prev_[candidate & k_window_mask];
    }
    out_distance = best_distance;
    return best_distance != 0 ? best_len : 0;
  }

  // --- tokenizing -------------------------------------------------------------------------

  ENGINE_FORCE_INLINE void push_literal(u8 byte) {
    tokens_.push_back(Token{byte, 0});
    ++lit_freq_[byte];
  }

  ENGINE_FORCE_INLINE void push_match(u32 length, u32 distance) {
    tokens_.push_back(Token{static_cast<u16>(length), static_cast<u16>(distance)});
    ++lit_freq_[257u + k_length_code.code[length]];
    ++dist_freq_[distance_code(distance)];
  }

  bool block_full(u32 block_bytes) const noexcept {
    return tokens_.size() >= k_max_block_tokens || block_bytes >= k_max_block_bytes;
  }

  // Greedy: take the first match found at each position, emit a literal otherwise.
  template <class L>
  void tokenize_greedy() {
    u32 pos = 0;
    u32 block_start = 0;
    while (pos < size_) {
      u32 length = 0;
      u32 distance = 0;
      if (pos < hash_end_) {
        const u32 chain_head = insert(pos, hash_at(pos));
        if (chain_head != k_no_pos) length = find_match<L>(pos, chain_head, 0, distance);
      }
      if (length >= k_min_match) {
        push_match(length, distance);
        if (length <= L::insert_limit) insert_range(pos + 1, pos + length);
        pos += length;
      } else {
        push_literal(in_[pos]);
        ++pos;
      }
      if (pos < size_ && block_full(pos - block_start)) {
        flush_block<L>(block_start, pos, false);
        block_start = pos;
      }
    }
    flush_block<L>(block_start, size_, true);
  }

  // Lazy: a match is held for one position to see whether the next position starts a longer
  // one, in which case the held position becomes a literal instead (zlib's deflate_slow).
  template <class L>
  void tokenize_lazy() {
    u32 pos = 0;
    u32 block_start = 0;
    u32 held_len = 0;
    u32 held_distance = 0;
    bool literal_pending = false;
    while (pos < size_) {
      const u32 previous_len = held_len;
      const u32 previous_distance = held_distance;
      held_len = 0;
      held_distance = 0;
      if (pos < hash_end_) {
        const u32 chain_head = insert(pos, hash_at(pos));
        if (chain_head != k_no_pos && previous_len < L::max_lazy)
          held_len = find_match<L>(pos, chain_head, previous_len, held_distance);
      }

      if (previous_len >= k_min_match && held_len <= previous_len) {
        // The match that began one position back wins: emit it and skip past it. Positions
        // `start` and `start + 1` are already indexed; the rest of its interior is not.
        const u32 start = pos - 1;
        push_match(previous_len, previous_distance);
        insert_range(start + 2, start + previous_len);
        pos = start + previous_len;
        held_len = 0;
        held_distance = 0;
        literal_pending = false;
      } else if (literal_pending) {
        push_literal(in_[pos - 1]);
        ++pos;
      } else {
        literal_pending = true;
        ++pos;
      }

      // Everything below `done` is in `tokens_`; a pending literal is not.
      const u32 done = literal_pending ? pos - 1 : pos;
      if (pos < size_ && block_full(done - block_start)) {
        flush_block<L>(block_start, done, false);
        block_start = done;
      }
    }
    if (literal_pending) push_literal(in_[size_ - 1]);
    flush_block<L>(block_start, size_, true);
  }

  // --- block costs and emission -----------------------------------------------------------

  void reset_block() noexcept {
    tokens_.clear();
    std::memset(lit_freq_, 0, sizeof(lit_freq_));
    std::memset(dist_freq_, 0, sizeof(dist_freq_));
  }

  u64 extra_bit_cost() const noexcept {
    u64 bits = 0;
    for (u32 i = 0; i < 29; ++i)
      bits += u64{lit_freq_[257 + i]} * k_length_extra[i];
    for (u32 i = 0; i < k_dist_symbols; ++i)
      bits += u64{dist_freq_[i]} * k_dist_extra[i];
    return bits;
  }

  u64 fixed_cost() const noexcept {
    u64 bits = 3;
    for (u32 i = 0; i < k_lit_symbols; ++i)
      bits += u64{lit_freq_[i]} * k_fixed_lit.len[i];
    for (u32 i = 0; i < k_dist_symbols; ++i)
      bits += u64{dist_freq_[i]} * 5;
    return bits + extra_bit_cost();
  }

  void add_item(u32 symbol, u32 extra_bits, u32 extra_value, u32* clen_freq) noexcept {
    ENGINE_ASSERT(item_count_ < k_lit_used + k_dist_symbols, "deflate: code-length overflow");
    items_[item_count_++] =
        ClItem{static_cast<u8>(symbol), static_cast<u8>(extra_bits), static_cast<u8>(extra_value)};
    ++clen_freq[symbol];
  }

  // Builds the dynamic code and its header and returns the block's exact size in bits.
  u64 build_dynamic() {
    build_tree(lit_freq_, k_lit_used, k_max_bits, lit_);
    build_tree(dist_freq_, k_dist_symbols, k_max_bits, dist_);

    hlit_ = 257;
    for (u32 i = k_lit_used; i > 257; --i) {
      if (lit_.len[i - 1] != 0) {
        hlit_ = i;
        break;
      }
    }
    hdist_ = 1;
    for (u32 i = k_dist_symbols; i > 1; --i) {
      if (dist_.len[i - 1] != 0) {
        hdist_ = i;
        break;
      }
    }

    u8 lengths[k_lit_used + k_dist_symbols];
    for (u32 i = 0; i < hlit_; ++i)
      lengths[i] = lit_.len[i];
    for (u32 i = 0; i < hdist_; ++i)
      lengths[hlit_ + i] = dist_.len[i];

    // Run-length encode the lengths with symbol 16 (repeat the last) and 17 and 18 (zeros).
    const u32 total = hlit_ + hdist_;
    u32 clen_freq[k_clen_symbols] = {};
    item_count_ = 0;
    u32 i = 0;
    while (i < total) {
      const u8 value = lengths[i];
      u32 run = 1;
      while (i + run < total && lengths[i + run] == value)
        ++run;
      if (value == 0) {
        while (run >= 3) {
          u32 take = run < 138 ? run : 138;
          if (run > take && run - take < 3) take = run - 3;
          if (take <= 10) {
            add_item(17, 3, take - 3, clen_freq);
          } else {
            add_item(18, 7, take - 11, clen_freq);
          }
          run -= take;
          i += take;
        }
      } else {
        add_item(value, 0, 0, clen_freq);  // symbol 16 repeats what came before it
        ++i;
        --run;
        while (run >= 3) {
          u32 take = run < 6 ? run : 6;
          if (run > take && run - take < 3) take = run - 3;
          add_item(16, 2, take - 3, clen_freq);
          run -= take;
          i += take;
        }
      }
      while (run > 0) {
        add_item(value, 0, 0, clen_freq);
        --run;
        ++i;
      }
    }

    build_tree(clen_freq, k_clen_symbols, k_max_clen_bits, clen_);
    hclen_ = 4;
    for (u32 j = k_clen_symbols; j > 4; --j) {
      if (clen_.len[k_clen_order[j - 1]] != 0) {
        hclen_ = j;
        break;
      }
    }

    u64 bits = 3 + 5 + 5 + 4 + u64{hclen_} * 3;
    for (u32 k = 0; k < item_count_; ++k)
      bits += u64{clen_.len[items_[k].symbol]} + items_[k].extra_bits;
    for (u32 s = 0; s < k_lit_used; ++s)
      bits += u64{lit_freq_[s]} * lit_.len[s];
    for (u32 s = 0; s < k_dist_symbols; ++s)
      bits += u64{dist_freq_[s]} * dist_.len[s];
    return bits + extra_bit_cost();
  }

  template <u32 N1, u32 N2>
  void emit_tokens(const Huffman<N1>& lit, const Huffman<N2>& dist) {
    for (const Token& token : tokens_) {
      if (token.distance == 0) {
        writer_.put(lit.code[token.value], lit.len[token.value]);
      } else {
        const u32 length = token.value;
        const u32 lc = k_length_code.code[length];
        writer_.put(lit.code[257 + lc], lit.len[257 + lc]);
        if (k_length_extra[lc] != 0) writer_.put(length - k_length_base[lc], k_length_extra[lc]);
        const u32 distance = token.distance;
        const u32 dc = distance_code(distance);
        writer_.put(dist.code[dc], dist.len[dc]);
        if (k_dist_extra[dc] != 0) writer_.put(distance - k_dist_base[dc], k_dist_extra[dc]);
      }
    }
    writer_.put(lit.code[256], lit.len[256]);  // end of block
  }

  // Emits [start, end) as whichever of stored, fixed, and dynamic is smallest.
  template <class L>
  void flush_block(u32 start, u32 end, bool final) {
    ++lit_freq_[256];  // the end-of-block symbol

    const u32 length = end - start;
    const u32 parts = length == 0 ? 1 : (length + k_max_stored - 1) / k_max_stored;
    const u64 stored_bits = u64{parts} * 42 + u64{length} * 8;
    const u64 fixed_bits = fixed_cost();
    u64 dynamic_bits = ~u64{0};
    if constexpr (L::dynamic) {
      dynamic_bits = build_dynamic();
    }

    if (stored_bits <= fixed_bits && stored_bits <= dynamic_bits) {
      write_stored(writer_, in_, start, end, final);
    } else if (fixed_bits <= dynamic_bits) {
      writer_.put(final ? 1u : 0u, 1);
      writer_.put(1, 2);  // BTYPE 01
      emit_tokens(k_fixed_lit, k_fixed_dist);
    } else {
      writer_.put(final ? 1u : 0u, 1);
      writer_.put(2, 2);  // BTYPE 10
      writer_.put(hlit_ - 257, 5);
      writer_.put(hdist_ - 1, 5);
      writer_.put(hclen_ - 4, 4);
      for (u32 j = 0; j < hclen_; ++j)
        writer_.put(clen_.len[k_clen_order[j]], 3);
      for (u32 k = 0; k < item_count_; ++k) {
        const ClItem& item = items_[k];
        writer_.put(clen_.code[item.symbol], clen_.len[item.symbol]);
        if (item.extra_bits != 0) writer_.put(item.extra_value, item.extra_bits);
      }
      emit_tokens(lit_, dist_);
    }
    if (final) writer_.align();
    reset_block();
  }

  const u8* in_;
  u32 size_;
  u32 hash_end_;  // below this a position can read the four bytes the hash wants
  BitWriter writer_;
  Vector<u32> head_;
  Vector<u32> prev_;
  Vector<Token> tokens_;
  u32 lit_freq_[k_lit_symbols];
  u32 dist_freq_[k_dist_symbols];
  Huffman<k_lit_symbols> lit_{};
  Huffman<k_dist_symbols> dist_{};
  Huffman<k_clen_symbols> clen_{};
  ClItem items_[k_lit_used + k_dist_symbols];
  u32 item_count_ = 0;
  u32 hlit_ = 257;
  u32 hdist_ = 1;
  u32 hclen_ = 4;
};

// The output a level is expected to need, so the buffer is sized once rather than grown into.
u32 reserve_hint(u32 size, Compression level) noexcept {
  if (level == Compression::Stored) {
    const u32 parts = size == 0 ? 1 : (size + k_max_stored - 1) / k_max_stored;
    return size + parts * 5 + 8;  // exactly what stored framing costs
  }
  return size / 8 + 64;  // a guess; the writer grows by halves from here
}

void deflate_into(std::span<const u8> input, Compression level, Vector<u8>& out) {
  ENGINE_ASSERT(input.size() <= 0xFFFFFFFFu, "deflate: input must be smaller than 4 GiB");
  const u32 size = static_cast<u32>(input.size());
  out.reserve(out.size() + reserve_hint(size, level));
  if (level == Compression::Stored) {
    BitWriter writer(out);
    write_stored(writer, input.data(), 0, size, true);
    return;
  }
  Compressor compressor(input, out);
  if (level == Compression::Fast) {
    compressor.run<FastLevel>();
  } else {
    compressor.run<DefaultLevel>();
  }
}

}  // namespace

void deflate(std::span<const u8> input, Compression level, Vector<u8>& out) {
  out.clear();
  deflate_into(input, level, out);
}

void zlib_compress(std::span<const u8> input, Compression level, Vector<u8>& out) {
  out.clear();
  out.reserve(reserve_hint(static_cast<u32>(input.size()), level) + 6);
  out.push_back(0x78);  // deflate, 32 KiB window
  // FLEVEL, with check bits that make the two header bytes a multiple of 31.
  out.push_back(level == Compression::Default ? u8{0x9C} : u8{0x01});
  deflate_into(input, level, out);
  const u32 check = adler32(input);
  out.push_back(static_cast<u8>(check >> 24));
  out.push_back(static_cast<u8>(check >> 16));
  out.push_back(static_cast<u8>(check >> 8));
  out.push_back(static_cast<u8>(check));
}

}  // namespace engine::image
