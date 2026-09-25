// Proposal layers' own properties and merge bases (proposals.h).
#include <domain/protocol/proposals.h>

namespace engine::protocol {

ProposalTable::Entry* ProposalTable::find(std::string_view layer) noexcept {
  for (Entry& e : entries_) {
    if (e.info.layer == layer) return &e;
  }
  return nullptr;
}

const ProposalTable::Entry* ProposalTable::find(std::string_view layer) const noexcept {
  for (const Entry& e : entries_) {
    if (e.info.layer == layer) return &e;
  }
  return nullptr;
}

ProposalTable::Entry& ProposalTable::add(ProposalInfo info) {
  Entry entry;
  entry.info = std::move(info);
  entries_.push_back(std::move(entry));
  return entries_.back();
}

bool ProposalTable::note_base(Entry& entry, doc::ObjectId id,
                              const doc::ObjectRecord* target_record) {
  if (entry.base_index.contains(id)) return false;
  ProposalBaseEntry base;
  base.id = id;
  if (target_record != nullptr) base.record = *target_record;
  entry.base_index.insert(id, entry.base.size());
  entry.base.push_back(std::move(base));
  return true;
}

const ProposalBaseEntry* ProposalTable::base_of(const Entry& entry, doc::ObjectId id) noexcept {
  const u32* i = entry.base_index.find_value(id);
  return i != nullptr ? &entry.base[*i] : nullptr;
}

void ProposalTable::clear_base(Entry& entry) noexcept {
  entry.base.clear();
  entry.base_index.clear();
}

ProposalsFile ProposalTable::to_file() const {
  ProposalsFile file;
  for (const Entry& e : entries_) {
    ProposalInfo info = e.info;
    info.touched = 0;  // derived from the base when reported, never stored
    file.proposals.push_back(std::move(info));
  }
  return file;
}

void ProposalTable::assign(ProposalsFile file) {
  entries_.clear();
  for (ProposalInfo& info : file.proposals)
    add(std::move(info));
}

doc::Layer compose_onto(const doc::Layer& over, const doc::Layer& proposal) {
  doc::Layer out = over;
  for (auto [id, opinion] : proposal.records()) {
    const doc::ObjectRecord* existing = over.find(id);
    doc::ObjectRecord r;
    if (existing != nullptr) {
      r = *existing;
    } else {
      r.id = id;
    }
    if (!opinion.type.empty()) r.type = opinion.type;
    if (opinion.parent.has_value()) r.parent = opinion.parent;
    for (auto [name, value] : opinion.properties)
      r.properties.insert_or_assign(name, value);
    if (opinion.deleted) {
      if (!r.type.empty()) {
        // `over` (or the proposal) defines the object here: deleting it drops the definition, as
        // DeleteObject does in the defining layer.
        out.remove(id);
        continue;
      }
      r.deleted = true;
    }
    if (r.type.empty() && !r.parent.has_value() && !r.deleted && r.properties.empty()) {
      out.remove(id);  // an override that says nothing is not kept (ClearProperty's rule)
      continue;
    }
    out.set(std::move(r));
  }
  return out;
}

}  // namespace engine::protocol
