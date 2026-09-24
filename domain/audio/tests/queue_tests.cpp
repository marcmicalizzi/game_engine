// The single-producer single-consumer ring that carries every command to the audio thread
// (spsc_queue.h). The property the mixer rests on: nothing lost, nothing reordered, nothing
// duplicated, and the consumer never waits — `try_pop` on an empty ring returns at once.
#include <domain/audio/commands.h>
#include <domain/audio/spsc_queue.h>

#include <doctest/doctest.h>

#include <atomic>
#include <thread>

using namespace engine;
using namespace engine::audio;

TEST_CASE("the ring rounds its capacity up to a power of two and refuses past it") {
  SpscQueue<u32> queue(5);
  CHECK(queue.capacity() == 8u);
  for (u32 i = 0; i < 8; ++i)
    CHECK(queue.try_push(i));
  CHECK_FALSE(queue.try_push(99));
  CHECK(queue.size_approx() == 8u);
  u32 value = 1234;
  for (u32 i = 0; i < 8; ++i) {
    CHECK(queue.try_pop(value));
    CHECK(value == i);
  }
  value = 1234;
  CHECK_FALSE(queue.try_pop(value));
  CHECK(value == 1234u);  // an empty pop leaves the output alone
  // Wrapping the indices past the capacity many times over changes nothing.
  for (u32 round = 0; round < 1000; ++round) {
    CHECK(queue.try_push(round));
    CHECK(queue.try_pop(value));
    CHECK(value == round);
  }
}

namespace {

// A producer thread and a consumer thread over a ring of `capacity` commands. The consumer only
// ever calls `try_pop`; an empty ring is counted and polled again, never waited on.
void run_two_threads(u32 capacity, u32 count) {
  SpscQueue<Command> queue(capacity);
  std::atomic<bool> start{false};
  u64 producer_full = 0;

  std::thread producer([&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (u32 i = 0; i < count; ++i) {
      Command c;
      c.kind = static_cast<CommandKind>(i % 6u);
      c.slot = i;
      c.generation = i * 2654435761u;
      c.start_frame = ~i;
      c.payload.params = ParamsPayload{static_cast<f32>(i), 1.0f, -1.0f};
      while (!queue.try_push(c)) {
        ++producer_full;
        std::this_thread::yield();
      }
    }
  });

  u32 received = 0;
  u64 empty_polls = 0;
  bool in_order = true;
  bool intact = true;
  start.store(true, std::memory_order_release);
  Command c;
  while (received < count) {
    if (!queue.try_pop(c)) {
      ++empty_polls;
      continue;
    }
    in_order = in_order && c.slot == received;
    intact = intact && c.generation == received * 2654435761u && c.start_frame == ~received &&
             c.kind == static_cast<CommandKind>(received % 6u) &&
             c.payload.params.gain == static_cast<f32>(received);
    ++received;
  }
  producer.join();

  CHECK(received == count);
  CHECK(in_order);
  CHECK(intact);
  CHECK(queue.size_approx() == 0u);
  CHECK_FALSE(queue.try_pop(c));
  MESSAGE("capacity " << capacity << ": producer found it full " << producer_full
                      << " times, consumer found it empty " << empty_polls << " times");
}

}  // namespace

TEST_CASE("commands cross threads in order, whole, and exactly once") {
  // A tiny ring forces the full and empty edges thousands of times; a large one runs the common
  // case, where each side reads the other's index only when its cached copy says it must.
  run_two_threads(8, 200'000);
  run_two_threads(1024, 1'000'000);
}
