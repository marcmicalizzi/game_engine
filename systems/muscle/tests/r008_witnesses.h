#pragma once

// The authoring track's r008 Arm A supervisor witnesses, as data (supervisor_tests.cpp reads them
// with core/json). Rows copied, every number as its source token, from two files of the
// `06-muscle-activation` worktree (read as text; nothing there was run):
//
//   r008-20261011/results/a001/preflight.json, 16359 bytes,
//     SHA-256 9c9380af294363c2bbcf3b5e9362345cb718ce6065a4b28d55f78ca05e23de00 (the run's
//     MANIFEST.json records the same): `witnesses.failure_eight_restart_nine`,
//     `witnesses.rigid_recovery`, `witnesses.moving_freeze_resume` (its `resume_work` cut to the
//     five channels and the residual), `witnesses.undefined_and_inert`;
//   r008-20261011/execution/a001/PREFLIGHT-INPUTS.json, 4764 bytes,
//     SHA-256 57898277ef54828447666c5a879a13a1659888c44f12ae7262d530e36c75a110:
//     `initial_state` (cut to the mechanics and the cache), `initial_inputs.parameter_sha256`,
//     `initial_inputs.path`, and `freeze_path_delta_m`.
//
// What produced them (preflight.py, SHA-256
// ecf14746937c6cd7698fbd92a222beafcdc1311495aee6ce523af34d112ec29d) is a double of the supervisor,
// not one: `failure_update` and `success_rigid` move the counters from synthetic success flags,
// `projection_at_nine` computes the projection's geometry only ("no tier-1 force integration"), and
// the moving-path resume is r008's own tick. So the engine is held to the doubles' *transitions*
// and to the law's *numbers* (the projection, the jump, the ledger), and to r008's resumed state
// within a stated tolerance, since r008's scheme is not r010's. Two fields record the doubles'
// shortcuts rather than a state machine: `last_attempt_tick` stays 8 through the synthetic rigid
// ticks, and the `reset` witness was built from the `scheduled` one with the streak set to 59, so
// it still carries `ToCompliant`.

namespace engine::muscle::test_witnesses {

inline constexpr const char* k_r008 = R"json({
  "initial_state": {
    "last_commit_tick": 0,
    "activation": 0.0,
    "fiber_length_m": 0.09988618207614752,
    "path_length_m": 0.14800665778412417,
    "tendon_force_N": 17.728927531024326,
    "active_fiber_force_N": 0.0,
    "passive_fiber_force_N": 18.09036097965944,
    "damping_fiber_force_N": -2.220446049250313E-13,
    "passive_fiber_energy_J": 0.2642906954665156,
    "tendon_energy_J": 0.0010149084070475156
  },
  "parameter_sha256": "9665d9f3c53dfae5861a5318536d6625b973eaed432fd67c533d4627cd5c1ed6",
  "path": {"kind": "constant", "length_m": 0.14800665778412417},
  "freeze_path_delta_m": 1E-05,
  "failure_eight_restart_nine": {
    "supervisor": {
      "last_attempt_tick": 8,
      "consecutive_frozen_ticks": 8,
      "tier": 0,
      "pending_transition": "ToRigid",
      "consecutive_rigid_success_ticks": 0
    },
    "decisions": [
      {"kind": "TierDegraded", "tick": 8, "tier_before": 0, "tier_after": 0, "requested_tier": 1}
    ],
    "projection": {
      "tick": 9,
      "tier": 1,
      "pending_transition": "None",
      "fiber_length_m": 0.1,
      "transition_elastic_jump_J": 0.0010519687328129912
    }
  },
  "rigid_recovery": {
    "scheduled": {
      "last_attempt_tick": 8,
      "consecutive_frozen_ticks": 8,
      "tier": 1,
      "pending_transition": "ToCompliant",
      "consecutive_rigid_success_ticks": 60
    },
    "reset": {
      "last_attempt_tick": 8,
      "consecutive_frozen_ticks": 8,
      "tier": 1,
      "pending_transition": "ToCompliant",
      "consecutive_rigid_success_ticks": 0
    }
  },
  "moving_freeze_resume": {
    "delta_path_m": 1e-05,
    "freeze": {
      "status": "Valid",
      "external_input_J": 0.00017728927531042056,
      "external_input_valid": true,
      "elastic_change_J": 0.00018527030848986513,
      "approximation_error_J": -7.981033179444574e-06,
      "resume_initial_elastic_J": 0.265490874182053
    },
    "resume_work": {
      "external_work_J": 0.0001785791240519235,
      "active_output_work_J": 0.0,
      "dissipation_J": 1.0832879316330495e-05,
      "delta_elastic_energy_J": 0.00016955105919846947,
      "signed_residual_J": -1.8048144628764718e-06
    },
    "final_state": {
      "last_commit_tick": 2,
      "activation": 0.0,
      "fiber_length_m": 0.09990528994199423,
      "tendon_force_N": 17.80898257945053,
      "passive_fiber_energy_J": 0.26463658499353687,
      "tendon_energy_J": 0.0010238402477146143
    }
  },
  "undefined_and_inert": {
    "undefined": {
      "status": "UndefinedPath",
      "external_input_J": null,
      "external_input_valid": false,
      "elastic_change_J": null,
      "approximation_error_J": null,
      "resume_initial_elastic_J": null
    },
    "inert": {
      "status": "Inert",
      "external_input_J": 0.0,
      "external_input_valid": true,
      "elastic_change_J": null,
      "approximation_error_J": null,
      "resume_initial_elastic_J": null
    }
  }
})json";

}  // namespace engine::muscle::test_witnesses
