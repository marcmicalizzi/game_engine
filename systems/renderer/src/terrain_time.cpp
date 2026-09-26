#include <core/time/time.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/terrain_time.h>

#include <cmath>

namespace engine::renderer {

namespace {

// Read when a time-lapse starts (docs/subsystems/renderer.md, "The dunes in time-lapse").
tunables::Float time_step_s{"renderer.terrain.time_step_s", 86'400.0, 60.0, 3.2e7,
                            "Game seconds between re-evaluations of the dune field in a "
                            "time-lapse (a game day by default)"};
tunables::Int time_min_frames{"renderer.terrain.time_min_frames", 30, 1, 1'000'000,
                              "Frames at least between the starts of two re-evaluations of the "
                              "dune field in a time-lapse"};

// Blocks a job takes: enough that a 4,097 grid's 4,225 blocks are a few hundred jobs, few enough
// that a 257 grid's 25 still spread over a pool.
constexpr u32 k_blocks_per_job = 16;

}  // namespace

TimeLapseConfig time_lapse_config_from_tunables(f64 rate) noexcept {
  TimeLapseConfig c;
  c.rate = rate;
  c.step_s = time_step_s.get();
  c.min_frames = static_cast<u32>(time_min_frames.get());
  return c;
}

i64 time_lapse_due(i64 evaluated_step, f64 game_s, f64 step_s, u32 frames_since, u32 min_frames,
                   bool in_flight) noexcept {
  if (in_flight || frames_since < min_frames || !(step_s > 0.0)) return -1;
  const i64 crossed = static_cast<i64>(std::floor(game_s / step_s));
  return crossed > evaluated_step ? crossed : -1;
}

TerrainTimeLapse::~TerrainTimeLapse() {
  if (in_flight_ && jobs_ != nullptr) jobs_->wait(counter_);
}

bool TerrainTimeLapse::start(const TerrainDesc& desc, const TimeLapseConfig& config,
                             jobs::JobSystem* jobs, std::string* error) {
  if (in_flight_ && jobs_ != nullptr) jobs_->wait(counter_);
  in_flight_ = false;
  sampler_.reset();
  if (!(config.rate > 0.0) || !(config.step_s > 0.0)) {
    if (error != nullptr) *error = "time-lapse: the rate and the step must be positive";
    return false;
  }
  if (!desc.enabled || desc.generator != TerrainGenerator::dunes) {
    if (error != nullptr)
      *error =
          "time-lapse: the scene's terrain does not name the dune generator, and the waves "
          "have no time to run";
    return false;
  }
  if (!terrain_generator_available()) {
    if (error != nullptr) *error = "time-lapse: this build has no terrain capability";
    return false;
  }
  desc_ = desc;
  config_ = config;
  jobs_ = jobs;
  sampler_ = std::make_unique<TerrainSampler>(desc_);
  heights_.resize(desc_.size * desc_.size);
  const u32 blocks = terrain_height_blocks(desc_);
  blocks_.clear();
  jobs_list_.clear();
  for (u32 b = 0; b < blocks; b += k_blocks_per_job)
    blocks_.push_back(Block{this, b, std::min(blocks, b + k_blocks_per_job)});
  for (Block& block : blocks_) {
    jobs_list_.push_back(jobs::Job{[](void* p) {
                                     const Block& b = *static_cast<const Block*>(p);
                                     TerrainTimeLapse& t = *b.owner;
                                     evaluate_terrain_heights(
                                         *t.sampler_, t.eval_time_s_, b.begin, b.end,
                                         std::span<f32>(t.heights_.data(), t.heights_.size()));
                                   },
                                   &block, nullptr});
  }
  game_s_ = 0.0;
  evaluated_step_ = 0;
  frames_since_ = config.min_frames;  // the first boundary need not wait for frames
  started_ = 0;
  delivered_ = 0;
  last_eval_ms_ = 0.0;
  total_eval_ms_ = 0.0;
  return true;
}

void TerrainTimeLapse::deliver(TerrainHeightSink& sink) {
  in_flight_ = false;
  last_eval_ms_ = static_cast<f64>(time::monotonic_ns() - eval_start_ns_) / 1e6;
  total_eval_ms_ += last_eval_ms_;
  ++delivered_;
  sink.heights(eval_time_s_, std::span<const f32>(heights_.data(), heights_.size()));
}

bool TerrainTimeLapse::tick(f64 real_dt_s, TerrainHeightSink& sink) {
  if (!active()) return false;
  if (in_flight_ && counter_.done()) deliver(sink);
  game_s_ += real_dt_s * config_.rate;
  ++frames_since_;
  const i64 due = time_lapse_due(evaluated_step_, game_s_, config_.step_s, frames_since_,
                                 config_.min_frames, in_flight_);
  if (due < 0) return false;
  evaluated_step_ = due;
  eval_time_s_ = desc_.time_s + static_cast<f64>(due) * config_.step_s;
  frames_since_ = 0;
  ++started_;
  eval_start_ns_ = time::monotonic_ns();
  in_flight_ = true;
  if (jobs_ == nullptr) {
    evaluate_terrain_heights(*sampler_, eval_time_s_, 0, terrain_height_blocks(desc_),
                             std::span<f32>(heights_.data(), heights_.size()));
    deliver(sink);
    return true;
  }
  jobs_->schedule(jobs::Pool::Performance,
                  std::span<const jobs::Job>(jobs_list_.data(), jobs_list_.size()), counter_);
  return true;
}

void TerrainTimeLapse::finish(TerrainHeightSink& sink) {
  if (!in_flight_) return;
  if (jobs_ != nullptr) jobs_->wait(counter_);
  deliver(sink);
}

}  // namespace engine::renderer
