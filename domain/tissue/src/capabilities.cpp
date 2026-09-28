#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <domain/geometry/surface_binding.h>
#include <domain/tissue/capabilities.h>
#include <domain/tissue/tissue_file.h>

#include <algorithm>
#include <string>

namespace engine::tissue {

namespace {

using enum RowStatus;

// Every row, grouped as a report emits them. A row the table calls evaluated can fail; an
// info-only row reports and never gates, naming what it cannot evaluate and the authority for it;
// a row that is not implemented is never emitted, and a file that requires it fails the gate.
constexpr RowCapability k_rows[] = {
    {"file.read", evaluated},
    {"definition.format", evaluated},
    {"blocks.resolve", evaluated},
    {"observation.topology", evaluated},
    {"observation.outside_bitwise", evaluated},
    {"observation.deviation", info_only},
    {"observation.landmarks", evaluated},
    {"observation.load", evaluated},
    {"observation.acceptance", info_only},
    {"region.cage_size", evaluated},
    {"region.cell_quality", evaluated},
    {"region.cell_edge_nodes", evaluated},
    {"region.cell_orientation", evaluated},
    {"region.materials", evaluated},
    {"region.sheets", evaluated},
    {"state.inverse", evaluated},
    {"state.inverse_solver", evaluated},
    {"region.shell_closed", evaluated},
    {"attachments.resolve", evaluated},
    {"region.through_thickness", info_only},
    {"region.affine_patch", info_only},
    {"region.volumetric_strain", info_only},
    {"state.equilibrium_gap", info_only},
    {"frame.closed_manifold", evaluated},
    {"frame.cover", info_only},
    {"frame.layering", evaluated},
    {"frame.containment", evaluated},
    {"frame.intersections", evaluated},
    {"region.skin_containment", evaluated},
    {"depth.layer_order", evaluated},
    {"depth.clearance", evaluated},
    {"cover.range", evaluated},
    {"cover.report", info_only},
    {"binding.seam_identity", evaluated},
    {"binding.domain", evaluated},
    {"binding.written_in_observation_domain", evaluated},
    {"binding.records", evaluated},
    {"binding.reference_identity", evaluated},
    {"binding.image_orientation", evaluated},
    {"binding.footpoint_normals", info_only},
    {"binding.transfer_agreement", evaluated},
    {"binding.mode_conformance", info_only},
    {"volume.report", info_only},
    {"volume.visible_vs_top", evaluated},
    {"volume.omitted_volume", evaluated},
    // The layered model (TissueDefinition version 2).
    {"parameters.domain", evaluated},
    {"state.parameters", evaluated},
    {"muscle.rest_identity", evaluated},
    {"muscle.target_volume", evaluated},
    {"muscle.passive_reference", evaluated},
    {"muscle.objectivity", evaluated},
    {"frame.rigidity", evaluated},
    {"contact.frame_trajectory", info_only},
    {"attachment.enforcement", evaluated},
    {"attachment.pose_binding", evaluated},
    {"attachment.reaction_balance", not_implemented},
    {"surface.material_boundary", evaluated},
    {"surface.orientation", evaluated},
    {"surface.embedding", evaluated},
    {"fat.thickness_positive", evaluated},
    {"fat.field_jacobian", evaluated},
    {"fat.mass_ledger", evaluated},
    {"fat.depot_partition", evaluated},
    {"skin.material_binding", evaluated},
    {"skin.rest_metric", evaluated},
    {"skin.energy_transfer", info_only},
    {"contact.pairs", evaluated},
    {"contact.exclusions", evaluated},
    {"attachment.contact_compatibility", evaluated},
    {"contact.curved_clearance", evaluated},
    {"contact.constraint_compatibility", evaluated},
    {"contact.trajectory", info_only},
    {"reference.certificate_provenance", info_only},
    {"definition.requirements", evaluated},
};

// The laws this build evaluates, by what they are the law of.
struct LawFamily {
  const char* family;
  const char* laws[4];
};
constexpr LawFamily k_laws[] = {
    {"rest_driver", {"fusiform-arch-v1", nullptr, nullptr, nullptr}},
    {"thickness", {"uniform-v1", "cosine-modulation-v1", nullptr, nullptr}},
    {"surface_evaluation", {"p2-six-node-v1", nullptr, nullptr, nullptr}},
    {"normal_mode",
     {"triangle", "interpolated-vertex-area-weighted", "limit-interpolated", nullptr}},
    {"refinement_rule", {"loop-hoppe-1994-v1", nullptr, nullptr, nullptr}},
    {"energy", {"bulk-edge-v0", nullptr, nullptr, nullptr}},
};

void walk(const schema::TypeRef& ref, Vector<std::string>& names);

void walk_type(const schema::TypeInfo& info, Vector<std::string>& names) {
  const std::string name = info.name;
  for (const std::string& n : names)
    if (n == name) return;
  names.push_back(name);
  if (info.kind == schema::Kind::Enum) {
    for (const schema::EnumValueInfo& v : info.values)
      names.push_back(name + "." + v.name);
    return;
  }
  for (const schema::FieldInfo& f : info.fields) {
    names.push_back(name + "." + f.name);
    walk(f.type, names);
  }
}

void walk(const schema::TypeRef& ref, Vector<std::string>& names) {
  if ((ref.kind == schema::Kind::Struct || ref.kind == schema::Kind::Enum) && ref.type != nullptr)
    walk_type(*ref.type, names);
  if (ref.element != nullptr) walk(*ref.element, names);
  if (ref.key != nullptr) walk(*ref.key, names);
}

Capabilities make_capabilities() {
  Capabilities out;
  out.schema_version = TissueDefinition::k_schema_version;
  out.container_version = k_tissue_file_version;
  walk_type(schema::type_of<TissueDefinition>(), out.records);
  // The laws' coefficient records, read from a `json` field by the law that owns them.
  walk_type(schema::type_of<FusiformArchCoefficients>(), out.records);
  walk_type(schema::type_of<CosineModulation>(), out.records);
  std::sort(out.records.begin(), out.records.end());
  out.records.erase(std::unique(out.records.begin(), out.records.end()), out.records.end());
  for (u32 kind = 10; kind < 256; ++kind)
    if (block_element_size(static_cast<BlockKind>(kind)) != 0)
      out.blocks.push_back(block_kind_name(kind));
  for (const RowCapability& r : k_rows) {
    out.row_ids.push_back(r.id);
    out.row_status.push_back(r.status);
  }
  for (const LawFamily& family : k_laws) {
    out.law_families.push_back(family.family);
    Vector<std::string> laws;
    for (const char* law : family.laws)
      if (law != nullptr) laws.push_back(law);
    out.laws.push_back(std::move(laws));
  }
  return out;
}

bool contains(const Vector<std::string>& list, std::string_view name) noexcept {
  for (const std::string& s : list)
    if (s == name) return true;
  return false;
}

bool remove(Vector<std::string>& list, std::string_view name) {
  for (u32 i = 0; i < list.size(); ++i)
    if (list[i] == name) {
      list.erase(list.begin() + i);
      return true;
    }
  return false;
}

void sentence(CapabilityFailure& out) {
  if (!out.failed()) return;
  std::string s =
      "a capability failure, not a validation result: the file requires what this build does not "
      "have (";
  for (u32 i = 0; i < out.missing.size(); ++i)
    s += (i == 0 ? "" : "; ") + out.missing[i];
  s +=
      "), so it cannot be read with its meaning; read it with a build whose `engine-content tissue "
      "capabilities` lists them";
  out.sentence = std::move(s);
}

void check_one(std::string_view kind, std::string_view name, const Capabilities& c,
               CapabilityFailure& out) {
  if (kind == "records") {
    if (!c.has_record(name)) out.missing.push_back("record " + std::string(name));
  } else if (kind == "blocks") {
    if (!c.has_block(name)) out.missing.push_back("block " + std::string(name));
  } else if (kind == "rows") {
    const RowStatus status = c.row(name);
    if (status == RowStatus::not_implemented) {
      const bool known = contains(c.row_ids, name);
      out.missing.push_back(
          "row " + std::string(name) +
          (known ? " (known to this build and not implemented)" : " (unknown to this build)"));
    }
  } else if (kind == "laws") {
    if (!c.has_law(name)) out.missing.push_back("law " + std::string(name));
  }
}

}  // namespace

const char* row_status_name(RowStatus status) noexcept {
  switch (status) {
    case RowStatus::evaluated: return "evaluated";
    case RowStatus::info_only: return "info-only";
    case RowStatus::not_implemented: return "not-implemented";
  }
  return "unknown";
}

std::span<const RowCapability> row_capabilities() noexcept { return k_rows; }

bool Capabilities::has_record(std::string_view name) const noexcept {
  return std::binary_search(records.begin(), records.end(), name, [](const auto& a, const auto& b) {
    return std::string_view(a) < std::string_view(b);
  });
}

bool Capabilities::has_block(std::string_view name) const noexcept {
  return contains(blocks, name);
}

bool Capabilities::has_law(std::string_view name) const noexcept {
  for (const Vector<std::string>& family : laws)
    if (contains(family, name)) return true;
  return false;
}

RowStatus Capabilities::row(std::string_view id) const noexcept {
  for (u32 i = 0; i < row_ids.size(); ++i)
    if (row_ids[i] == id) return row_status[i];
  return RowStatus::not_implemented;
}

bool Capabilities::strip(std::string_view name) {
  bool removed = remove(records, name) || remove(blocks, name);
  for (u32 i = 0; !removed && i < row_ids.size(); ++i)
    if (row_ids[i] == name) {
      row_ids.erase(row_ids.begin() + i);
      row_status.erase(row_status.begin() + i);
      removed = true;
    }
  for (Vector<std::string>& family : laws)
    removed = removed || remove(family, name);
  return removed;
}

const Capabilities& build_capabilities() {
  static const Capabilities capabilities = make_capabilities();
  return capabilities;
}

bool check_requirements_json(const JsonValue& definition, const Capabilities& capabilities,
                             CapabilityFailure& out) {
  out = CapabilityFailure{};
  const JsonValue* requirements =
      definition.is_object() ? definition.find("requirements") : nullptr;
  if (requirements == nullptr || requirements->is_null()) return true;
  if (!requirements->is_object()) {
    out.missing.push_back("a readable requirements object (the file's is not an object)");
    sentence(out);
    return false;
  }
  for (const auto& [kind, list] : requirements->as_object()) {
    const bool known = kind == "records" || kind == "blocks" || kind == "rows" || kind == "laws";
    if (!known) {
      out.missing.push_back("requirements of kind '" + kind + "', which this build does not know");
      continue;
    }
    if (list.is_null()) continue;
    if (!list.is_array()) {
      out.missing.push_back("a readable requirements." + kind + " (it is not an array)");
      continue;
    }
    for (usize i = 0; i < list.size(); ++i) {
      std::string_view name;
      if (!list[i].get_string(name)) {
        out.missing.push_back("a readable requirements." + kind + " entry (" + std::to_string(i) +
                              " is not a string)");
        continue;
      }
      check_one(kind, name, capabilities, out);
    }
  }
  sentence(out);
  return !out.failed();
}

bool check_requirements(const Requirements& requirements, const Capabilities& capabilities,
                        CapabilityFailure& out) {
  return check_requirements_json(
      [&] {
        JsonValue definition = JsonValue::object();
        definition.set("requirements", schema::to_json(requirements));
        return definition;
      }(),
      capabilities, out);
}

JsonValue capabilities_json(const Capabilities& c) {
  JsonValue out = JsonValue::object();
  out.set("format", JsonValue(k_tissue_format));
  out.set("schema_version", JsonValue(c.schema_version));
  out.set("container_version", JsonValue(c.container_version));
  JsonValue records = JsonValue::object();
  JsonValue enums = JsonValue::object();
  JsonValue fields = JsonValue::array();
  for (const std::string& name : c.records) {
    const usize dot = name.find('.');
    if (dot != std::string::npos) {
      const std::string owner = name.substr(0, dot);
      const schema::TypeInfo* info = schema::Registry::global().find("engine.tissue." + owner);
      if (info != nullptr && info->kind == schema::Kind::Enum) {
        JsonValue& values = enums[owner];
        if (!values.is_array()) values = JsonValue::array();
        values.push_back(JsonValue(name.substr(dot + 1)));
      } else {
        fields.push_back(JsonValue(name));
      }
      continue;
    }
    const schema::TypeInfo* info = schema::Registry::global().find("engine.tissue." + name);
    if (info != nullptr && info->kind == schema::Kind::Enum) {
      JsonValue& values = enums[name];
      if (!values.is_array()) values = JsonValue::array();
      continue;
    }
    records.set(name, JsonValue(static_cast<u64>(info != nullptr ? info->version : 0)));
  }
  out.set("records", std::move(records));
  out.set("fields", std::move(fields));
  out.set("enums", std::move(enums));
  JsonValue blocks = JsonValue::array();
  for (const std::string& b : c.blocks)
    blocks.push_back(JsonValue(b));
  out.set("blocks", std::move(blocks));
  JsonValue rows = JsonValue::object();
  for (u32 i = 0; i < c.row_ids.size(); ++i)
    rows.set(c.row_ids[i], JsonValue(row_status_name(c.row_status[i])));
  out.set("rows", std::move(rows));
  JsonValue laws = JsonValue::object();
  for (u32 i = 0; i < c.law_families.size(); ++i) {
    JsonValue list = JsonValue::array();
    for (const std::string& law : c.laws[i])
      list.push_back(JsonValue(law));
    laws.set(c.law_families[i], std::move(list));
  }
  out.set("laws", std::move(laws));
  JsonValue kinds = JsonValue::array();
  for (const char* kind : {"records", "blocks", "rows", "laws"})
    kinds.push_back(JsonValue(kind));
  out.set("requirements", std::move(kinds));
  return out;
}

}  // namespace engine::tissue
