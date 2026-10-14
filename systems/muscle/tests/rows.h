#pragma once

// Four of the corpus's rows at t = 0, as r010's CASES.json archives them: the series parameters,
// the path at t = 0, the archived static state and the reference's outputs there (the fiber
// parameters are the defaults on every row). The whole corpus is replayed from content/muscle/ by
// corpus_tests.cpp; these keep the law's and the lifecycle's tests independent of the data files.
#include <systems/muscle/law.h>

namespace engine::muscle::test_rows {

struct Row {
  const char* name;
  double slack, e1, alpha0, beta, min_cos;
  double length, rate;  // the path at t = 0
  double a, l;          // the archived static state
  double tendon_force, active_force, passive_force, damping_force, passive_energy, tendon_energy;
  double residual;
};

inline constexpr Row k_rows[] = {
    {"step", 0.05, 0.049, 0.2, 0.1, 0.2, 0.14800665778412417, 0.0, 0.0, 0.09988618207614752,
     17.728927531024326, 0.0, 18.09036097965944, -2.220446049250313e-13, 0.2642906954665156,
     0.0010149084070475156, -3.552713678800501e-15},
    {"active_wave", 0.05, 0.049, 0.2, 0.1, 0.2, 0.14800665778412417, 0.07539822368615504, 0.4,
     0.09851372441671338, 406.51372104756865, 398.54018733297215, 16.500877828644875,
     -1.2814930104593597e-13, 0.24056982123183568, 0.25246007163959644, -1.1368683772161603e-13},
    {"corner_04_passive", 0.05, 0.02, 0.0, 0.2, 0.2, 0.15000000000000002, 0.25132741228718347, 0.0,
     0.09995146913743873, 18.1696560963044, 0.0, 18.16965609630584, -1.4210854715202004e-12,
     0.2654743485674369, 0.00043450563587089106, 1.7763568394002505e-14},
    {"corner_06_passive", 0.05, 0.02, 0.4, 0.2, 0.2, 0.1421060994002885, 0.25132741228718347, 0.0,
     0.09995867606936222, 16.74220566778924, 0.0, 18.178430525241136, 6.1950444774083735e-12,
     0.26560532765782047, 0.0003705564212297017, -1.4210854715202004e-14},
};

inline SeriesParameters series_of(const Row& r) {
  SeriesParameters s{};
  s.tendon_slack_length = r.slack;
  s.tendon_strain_at_unit_force = r.e1;
  s.pennation_at_optimal = r.alpha0;
  s.normalized_damping = r.beta;
  s.minimum_cosine = r.min_cos;
  return s;
}

inline LawRow law_of(const Row& r) { return make_law_row(FiberParameters{}, series_of(r)); }

}  // namespace engine::muscle::test_rows
