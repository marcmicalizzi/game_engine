#include <core/platform/address_space.h>

#include <doctest/doctest.h>

#include <cstring>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

using namespace engine;
using namespace engine::platform;

// The walk has to find what a process committed and did not touch, because that is the question it
// exists to answer (platform.md, "Where the commit is"): a 256 MiB block committed and left alone
// must appear among the largest private allocations at its own address with its own size and next
// to nothing resident, and must move the private total and the OS's commit charge by its size. The
// bounds are the block's size, give or take what the test framework itself allocates meanwhile.

namespace {

constexpr u64 k_mib = 1024ull * 1024ull;
constexpr u64 k_block = 256 * k_mib;
constexpr u64 k_slack = 16 * k_mib;

// Commits `bytes` of private memory and touches none of it. On Linux the block sits between two
// inaccessible pages so the kernel cannot merge it into a neighbouring anonymous mapping, which
// would move its base.
u8* commit_untouched(u64 bytes) {
#if ENGINE_PLATFORM_WINDOWS
  return static_cast<u8*>(::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
  const u64 page = 4096;
  void* p = ::mmap(nullptr, bytes + 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  u8* inner = static_cast<u8*>(p) + page;
  if (::mprotect(inner, bytes, PROT_READ | PROT_WRITE) != 0) return nullptr;
  return inner;
#endif
}

void release(u8* p, u64 bytes) {
#if ENGINE_PLATFORM_WINDOWS
  (void)bytes;
  ::VirtualFree(p, 0, MEM_RELEASE);
#else
  ::munmap(p - 4096, bytes + 2 * 4096);
#endif
}

u8* reserve_only(u64 bytes) {
#if ENGINE_PLATFORM_WINDOWS
  return static_cast<u8*>(::VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS));
#else
  void* p = ::mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? nullptr : static_cast<u8*>(p);
#endif
}

void unreserve(u8* p, u64 bytes) {
#if ENGINE_PLATFORM_WINDOWS
  (void)bytes;
  ::VirtualFree(p, 0, MEM_RELEASE);
#else
  ::munmap(p, bytes);
#endif
}

const AddressRegion* find(const AddressSpaceReport& r, const u8* base) {
  for (u32 i = 0; i < r.largest_count; ++i) {
    if (r.largest[i].base == reinterpret_cast<u64>(base)) return &r.largest[i];
  }
  return nullptr;
}

}  // namespace

TEST_CASE("address space: a block committed and untouched is found, by kind and by address") {
  AddressSpaceReport before;
  REQUIRE(walk_address_space(before));
  CHECK(before.page_bytes > 0);
  CHECK(before.kinds[static_cast<u32>(RegionKind::image)].committed > 0);  // this executable
  CHECK(before.sample_stride_pages == 1);  // a test process is far under the sample budget

  u8* block = commit_untouched(k_block);
  REQUIRE(block != nullptr);
  AddressSpaceReport after;
  REQUIRE(walk_address_space(after));

  const u64 private_before = before.kinds[static_cast<u32>(RegionKind::private_memory)].committed;
  const u64 private_after = after.kinds[static_cast<u32>(RegionKind::private_memory)].committed;
  CHECK(private_after >= private_before + k_block - k_slack);
  CHECK(private_after <= private_before + k_block + k_slack);
  CHECK(after.process_commit >= before.process_commit + k_block - k_slack);

  const AddressRegion* found = find(after, block);
  REQUIRE_MESSAGE(found != nullptr, "the block is not among the largest private allocations");
  CHECK(found->committed == k_block);
  CHECK(found->kind == RegionKind::private_memory);
  CHECK(found->resident <= k_slack);  // committed, never touched
  CHECK(after.largest[0].committed >= found->committed);
  CHECK(resident_estimate(block, k_block, 512) == 0);
  // Ranked by what is committed and not resident, too, and counted in its size's bucket
  // (256 MiB to 1 GiB, the sixth).
  bool in_untouched = false;
  for (u32 i = 0; i < after.untouched_count; ++i)
    in_untouched = in_untouched || after.untouched[i].base == reinterpret_cast<u64>(block);
  CHECK(in_untouched);
  CHECK(after.private_sizes[5].allocations == before.private_sizes[5].allocations + 1);
  CHECK(after.private_sizes[5].committed >= before.private_sizes[5].committed + k_block);

  // Touch half of it: the half becomes resident and nothing else moves.
  std::memset(block, 1, k_block / 2);
  AddressSpaceReport touched;
  REQUIRE(walk_address_space(touched));
  const AddressRegion* half = find(touched, block);
  REQUIRE(half != nullptr);
  CHECK(half->committed == k_block);
  CHECK(half->resident + k_slack >= k_block / 2);
  CHECK(half->resident <= k_block / 2 + k_slack);
  CHECK(touched.working_set >= after.working_set + k_block / 2 - k_slack);
  // The sampled estimate of one block: 512 samples over 65,536 pages, the first half touched.
  const u64 estimate = resident_estimate(block, k_block, 512);
  CHECK(estimate + k_slack >= k_block / 2);
  CHECK(estimate <= k_block / 2 + k_slack);

  release(block, k_block);
  AddressSpaceReport freed;
  REQUIRE(walk_address_space(freed));
  CHECK(find(freed, block) == nullptr);
}

TEST_CASE("address space: reserved address space costs no commit and is counted apart") {
  constexpr u64 k_reserve = 512 * k_mib;
  AddressSpaceReport before;
  REQUIRE(walk_address_space(before));
  u8* p = reserve_only(k_reserve);
  REQUIRE(p != nullptr);
  AddressSpaceReport after;
  REQUIRE(walk_address_space(after));
  const RegionKindTotals& b = before.kinds[static_cast<u32>(RegionKind::private_memory)];
  const RegionKindTotals& a = after.kinds[static_cast<u32>(RegionKind::private_memory)];
  CHECK(a.reserved_only >= b.reserved_only + k_reserve - k_slack);
  CHECK(a.committed <= b.committed + k_slack);
  CHECK(find(after, p) == nullptr);  // nothing committed: not among the largest by commit
  unreserve(p, k_reserve);
}

TEST_CASE("address space: the C runtime's heaps report what they committed and handed out") {
  HeapTotals heaps;
  if (!heap_totals(heaps)) {
    MESSAGE("no heap summary on this C library");
    return;
  }
  CHECK(heaps.heaps >= 1);
  CHECK(heaps.committed > 0);
  CHECK(heaps.allocated <= heaps.committed);
}

TEST_CASE("address space: the kinds have names") {
  CHECK(std::strcmp(region_kind_name(RegionKind::private_memory), "private") == 0);
  CHECK(std::strcmp(region_kind_name(RegionKind::mapped_file), "mapped") == 0);
  CHECK(std::strcmp(region_kind_name(RegionKind::image), "image") == 0);
}
