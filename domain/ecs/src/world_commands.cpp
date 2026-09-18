#include <core/base/assert.h>
#include <core/log/log.h>
#include <core/memory/memory.h>
#include <domain/ecs/components.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/world_commands.h>

#include <cstddef>
#include <cstring>

namespace engine::ecs {

ENGINE_LOG_CATEGORY_DECLARE(log_ecs);

WorldCommands::WorldCommands(flecs::world& world) : world_(&world) {
  // Both singletons are forced into existence here, where the world is certainly writable.
  // `apply()` may run inside a tick, and a world flecs has made readonly for the pipeline is one
  // nothing can create a component in — so everything the drain needs has to exist before the
  // first tick rather than on first use.
  components(world);
  identity_map(world);
}

WorldCommands::~WorldCommands() { clear(); }

void WorldCommands::release(Command& command) noexcept {
  if (command.value == nullptr) return;
  if (command.info != nullptr && command.info->ops != nullptr)
    command.info->ops->destroy(command.value);
  mem::deallocate(command.value, command.info != nullptr ? command.info->size : 0,
                  command.info != nullptr ? command.info->align : alignof(max_align_t));
  command.value = nullptr;
}

void WorldCommands::clear() noexcept {
  for (Command& command : commands_)
    release(command);
  commands_.clear();
}

void WorldCommands::create(const Id128& id) {
  Command command;
  command.id = id;
  command.kind = CommandKind::Create;
  commands_.push_back(command);
}

void WorldCommands::destroy(const Id128& id) {
  Command command;
  command.id = id;
  command.kind = CommandKind::Destroy;
  commands_.push_back(command);
}

void* WorldCommands::allocate_value(std::string_view type_name, const schema::TypeInfo*& out_info,
                                    flecs::entity_t& out_component, u32& out_size, bool& out_flat) {
  const ComponentType* type = components(*world_).find(type_name);
  if (type == nullptr || type->info == nullptr) {
    ENGINE_LOG_WARN(log_ecs, "world command names a component this world does not have",
                    log::field("type", type_name));
    return nullptr;
  }
  out_info = type->info;
  out_component = type->id;
  out_size = type->size;
  out_flat = type->flat;
  if (out_info->ops == nullptr || out_info->size == 0) return nullptr;

  void* value = mem::allocate(out_info->size, out_info->align);
  out_info->ops->construct(value);
  return value;
}

bool WorldCommands::set_json(const Id128& id, std::string_view type_name, const JsonValue& value,
                             Vector<schema::Diagnostic>* diagnostics) {
  Command command;
  u32 size = 0;
  bool flat = false;
  command.value = allocate_value(type_name, command.info, command.component, size, flat);
  if (command.value == nullptr) return false;

  // Parsed here rather than in apply(): a caller that sent bad JSON has to hear about it while
  // it is still holding the request, and apply() runs inside a tick where there is nobody left
  // to tell.
  schema::ReadContext context;
  if (!schema::from_json(*command.info, command.value, value, context)) {
    if (diagnostics != nullptr) {
      for (const schema::Diagnostic& diagnostic : context.diagnostics)
        diagnostics->push_back(diagnostic);
    }
    release(command);
    return false;
  }

  command.id = id;
  command.kind = CommandKind::SetComponent;
  commands_.push_back(command);
  return true;
}

bool WorldCommands::set_bytes(const Id128& id, std::string_view type_name,
                              std::span<const u8> bytes) {
  Command command;
  u32 size = 0;
  bool flat = false;
  command.value = allocate_value(type_name, command.info, command.component, size, flat);
  if (command.value == nullptr) return false;
  if (!flat || bytes.size() != size) {
    // A component with a string, a vector or a map in it is not its bytes, and a caller that
    // thinks otherwise is about to write a pointer from another process into the world.
    ENGINE_LOG_WARN(log_ecs, "world command sets bytes for a component that is not flat",
                    log::field("type", type_name), log::field("flat", flat),
                    log::field("size", size), log::field("given", bytes.size()));
    release(command);
    return false;
  }
  std::memcpy(command.value, bytes.data(), bytes.size());

  command.id = id;
  command.kind = CommandKind::SetComponent;
  commands_.push_back(command);
  return true;
}

bool WorldCommands::remove(const Id128& id, std::string_view type_name) {
  const ComponentType* type = components(*world_).find(type_name);
  if (type == nullptr) {
    ENGINE_LOG_WARN(log_ecs, "world command names a component this world does not have",
                    log::field("type", type_name));
    return false;
  }
  Command command;
  command.id = id;
  command.info = type->info;
  command.component = type->id;
  command.kind = CommandKind::RemoveComponent;
  commands_.push_back(command);
  return true;
}

CommandStats WorldCommands::apply() {
  CommandStats stats;
  if (commands_.empty()) return stats;
  flecs::world& world = *world_;

  // Every mutation below goes through flecs the way a system's would — deferred when the world
  // is deferred, immediate when it is not. The alternative, suspending deferring so each command
  // finishes before the next one starts, is not available: `install()` runs this from inside the
  // tick, where `ecs_progress` has made the world *readonly*, and writing a readonly world is an
  // assert rather than a race.
  //
  // The cost of playing by those rules is that an entity created here is not in the world's
  // identity map until the batch merges, because the `Identity` set is itself a command and the
  // observer fires at the merge. So the batch remembers its own creates: `create` followed by
  // `set` in one request is the shape every caller will write, and it has to work.
  const IdentityMap* map = identity_map_if_present(world);
  ENGINE_ASSERT(map != nullptr,
                "ecs::WorldCommands::apply: the world has no identity map; construct the "
                "WorldCommands outside a tick, or call ecs::identity_map(world) at setup");
  HashMap<Id128, flecs::entity_t> created;

  const auto resolve = [&](const Id128& id) -> flecs::entity_t {
    if (const flecs::entity_t* fresh = created.find_value(id)) return *fresh;
    return map != nullptr ? map->find(id) : 0;
  };

  for (Command& command : commands_) {
    switch (command.kind) {
      case CommandKind::Create: {
        // Idempotent: replaying a log that creates the same entity twice must not make two.
        if (command.id.is_null() || resolve(command.id) != 0) {
          ++stats.failed;
          break;
        }
        const flecs::entity entity = world.entity();
        entity.set<Identity>(Identity{command.id});
        created.insert_or_assign(command.id, entity.id());
        ++stats.created;
        break;
      }
      case CommandKind::Destroy: {
        const flecs::entity_t entity = resolve(command.id);
        if (entity == 0) {
          ++stats.failed;
          break;
        }
        ecs_delete(world.c_ptr(), entity);
        created.erase(command.id);
        ++stats.destroyed;
        break;
      }
      case CommandKind::SetComponent: {
        const flecs::entity_t entity = resolve(command.id);
        if (entity == 0 || command.info == nullptr || command.info->ops == nullptr) {
          ++stats.failed;
          break;
        }
        // `ensure` adds the component when the entity does not have it and hands back storage
        // flecs has already constructed — a command buffer while deferred, the table row when
        // not — so assigning into it is the one correct way to write a component whose type has
        // a constructor.
        void* storage = ecs_ensure_id(world.c_ptr(), entity, command.component, command.info->size);
        if (storage == nullptr) {
          ++stats.failed;
          break;
        }
        command.info->ops->copy_assign(storage, command.value);
        ecs_modified_id(world.c_ptr(), entity, command.component);
        ++stats.set;
        break;
      }
      case CommandKind::RemoveComponent: {
        const flecs::entity_t entity = resolve(command.id);
        if (entity == 0) {
          ++stats.failed;
          break;
        }
        ecs_remove_id(world.c_ptr(), entity, command.component);
        ++stats.removed;
        break;
      }
    }
    release(command);
  }
  commands_.clear();
  return stats;
}

flecs::system WorldCommands::install(SimWorld& sim, TickPhase phase) {
  WorldCommands* self = this;
  // A system with no terms: it runs once a tick, at the top of its phase, and its whole body is
  // the drain. It is deliberately *not* registered through `ecs::register_system`, because it
  // declares no component sets — it touches whatever the queue happens to name, which is exactly
  // the property that makes it a phase-boundary drain and not a system the scheduler can plan
  // around.
  return sim.world()
      .system("engine::ecs::world_commands")
      .kind(sim.phase(phase))
      .run([self](flecs::iter& it) {
        while (it.next()) {
        }
        self->apply();
      });
}

}  // namespace engine::ecs
