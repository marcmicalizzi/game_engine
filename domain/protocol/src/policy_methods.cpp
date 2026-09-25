// Plan 06 §6.5's methods (docs/subsystems/protocol.md, "Roles, leases and proposals"):
// `lease.*`, `doc.propose_layer`, `doc.promote`, `doc.reject`, and `engine.roles`. The checks
// themselves live in the session (session.cpp's Guard) and the policy (policy.h); these are the
// wire.
#include <core/time/time.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>

#include <algorithm>
#include <cstring>

namespace engine::protocol {

namespace {

// The session and the caller every method below starts from.
Session* begin(Context& ctx, const std::string& session, const doc::Attribution* attribution,
               Caller& caller, RpcError& error) {
  Session* s = ctx.sessions->require(session, error);
  if (s == nullptr) return nullptr;
  if (!resolve_caller(ctx.policy, attribution, caller, error)) return nullptr;
  return s;
}

// ---- lease.* -----------------------------------------------------------------------------------

bool lease_acquire(Context& ctx, const LeaseAcquireParams& params, LeaseInfo& out,
                   RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, &params.attribution, caller, error);
  return s != nullptr && s->acquire_lease(params, caller, time::wall_unix_ms(), out, error);
}

bool lease_renew(Context& ctx, const LeaseRenewParams& params, LeaseInfo& out, RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, &params.attribution, caller, error);
  return s != nullptr &&
         s->renew_lease(params.lease, params.ttl_seconds, caller, time::wall_unix_ms(), out, error);
}

bool lease_release(Context& ctx, const LeaseReleaseParams& params, LeaseInfo& out,
                   RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, &params.attribution, caller, error);
  return s != nullptr && s->release_lease(params.lease, caller, time::wall_unix_ms(), out, error);
}

bool lease_list(Context& ctx, const LeaseListParams& params, LeasesResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  return s != nullptr &&
         s->list_leases(params.layer, params.actor, time::wall_unix_ms(), out, error);
}

bool lease_require(Context& ctx, const LeaseRequireParams& params, LeasesResult& out,
                   RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, &params.attribution, caller, error);
  return s != nullptr && s->require_leases(params.required, time::wall_unix_ms(), out, error);
}

// ---- proposals ---------------------------------------------------------------------------------

bool doc_propose_layer(Context& ctx, const ProposeLayerParams& params, LayersResult& out,
                       RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, &params.attribution, caller, error);
  if (s == nullptr || !s->propose_layer(params, caller, error)) return false;
  s->layers(out.layers);
  return true;
}

bool doc_promote(Context& ctx, const PromoteParams& params, PromoteResult& out, RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, &params.attribution, caller, error);
  return s != nullptr && s->promote(params, caller, out, error);
}

bool doc_reject(Context& ctx, const RejectParams& params, LayersResult& out, RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, &params.attribution, caller, error);
  if (s == nullptr || !s->reject(params, caller, error)) return false;
  s->layers(out.layers);
  return true;
}

// ---- engine.roles ------------------------------------------------------------------------------

bool engine_roles(Context& ctx, RolesResult& out, RpcError&) {
  const Policy* policy = ctx.policy;
  if (policy == nullptr) return true;
  out.loaded = policy->roles_loaded();
  out.path = policy->roles_path();
  out.default_role = policy->roles().default_role;
  out.actor = policy->identity.actor;
  out.role = policy->identity.role;
  out.task = policy->identity.task;
  for (const Role& role : policy->roles().roles) {
    RoleInfo info;
    info.role = role;
    if (ctx.dispatcher != nullptr) {
      // The dispatcher keeps its methods in name order, so this list is too.
      for (const MethodDesc& m : ctx.dispatcher->methods()) {
        if (m.read_only || any_glob_match(role.methods, m.name))
          info.allowed_methods.push_back(m.name);
      }
    }
    out.roles.push_back(std::move(info));
  }
  return true;
}

}  // namespace

void add_policy_methods(Dispatcher& d) {
  d.add(method<LeaseAcquireParams, LeaseInfo, &lease_acquire>(
      "lease.acquire",
      "Claim tiles or object types of a layer for a time: refused (1009) when another actor's "
      "live lease overlaps, so conflicts show at acquisition rather than at merge."));
  d.add(method<LeaseRenewParams, LeaseInfo, &lease_renew>(
      "lease.renew", "Extend a lease the caller holds by a new time to live from now."));
  d.add(method<LeaseReleaseParams, LeaseInfo, &lease_release>("lease.release",
                                                              "Give up a lease the caller holds."));
  d.add(read_only(method<LeaseListParams, LeasesResult, &lease_list>(
      "lease.list",
      "Every live lease of the document, visible to everyone, and whether edits require one.")));
  d.add(method<LeaseRequireParams, LeasesResult, &lease_require>(
      "lease.require",
      "Turn on (or off) the rule that every writing doc.* call holds a lease covering the "
      "records it touches."));

  d.add(method<ProposeLayerParams, LayersResult, &doc_propose_layer>(
      "doc.propose_layer",
      "Open a proposal layer over a target layer, owned by the caller: the caller's own work, "
      "promoted into the target on acceptance."));
  d.add(method<PromoteParams, PromoteResult, &doc_promote>(
      "doc.promote",
      "Merge a proposal into its target structurally, validate the merged world first, and "
      "commit it as one journaled promotion; refused with the conflicts and the new validation "
      "problems unless `prefer` settles the conflicts."));
  d.add(method<RejectParams, LayersResult, &doc_reject>(
      "doc.reject", "Empty an open proposal without merging it, and close it."));

  d.add(read_only(method_no_params<RolesResult, &engine_roles>(
      "engine.roles",
      "The host's role configurations with the methods each may call, and its default "
      "identity.")));
}

}  // namespace engine::protocol
