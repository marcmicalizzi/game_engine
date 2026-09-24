// The generated bindings: a script reads a schema object's properties by name with the IDL's
// types, a property that does not exist is an error and not nil, nothing can be assigned, and a
// view never outlives what it shows (docs/subsystems/scripting.md, "The generated bindings").
#include <core/ids/id128.h>
#include <core/json/json.h>
#include <foundation/scripting/script_context.h>

#include <doctest/doctest.h>

#include <schemas/scripting_test.h>
#include <string>
#include <string_view>

using namespace engine;
using scripting::Arg;
using scripting::ScriptContext;
using scripting::ScriptId;
using scripting::Status;

namespace {

std::string render(const JsonValue& value) {
  JsonWriteOptions options;
  options.pretty = false;
  return write_json(value, options);
}

ScriptId must_load(ScriptContext& context, std::string_view name, std::string_view source) {
  ScriptId id;
  const Status status = context.load(name, source, id);
  INFO(context.last_error().message);
  REQUIRE(status == Status::Ok);
  return id;
}

scripting_test::Kinds sample_kinds() {
  scripting_test::Kinds k;
  k.blob = {0x41, 0x42};
  k.id = Id128::from_parts(0x0123456789abcdefull, 0xfedcba9876543210ull);
  k.flat = Vec2{1.0f, 2.0f};
  k.point = Vec3{3.0f, 4.0f, 5.0f};
  k.color = Vec4{0.1f, 0.2f, 0.3f, 1.0f};
  k.turn = Quat{0.0f, 0.0f, 0.0f, 1.0f};
  JsonValue extra = JsonValue::object();
  extra.set("hint", JsonValue("north"));
  k.extra = std::move(extra);
  k.fixed = {1, 2, 3};
  k.maybe.reset();
  k.nothing.reset();
  scripting_test::Attributes a;
  a.agility = 3.5f;
  k.crowd.push_back(a);
  k.by_id.insert_or_assign(7u, std::string("seven"));
  return k;
}

}  // namespace

TEST_CASE("scripting bindings: every schema kind reads with its IDL type") {
  ScriptContext context;
  const ScriptId id = must_load(context, "kinds.luau", R"(
function read(k)
  return {
    flag = k.flag, tiny = k.tiny, small = k.small, whole = k.whole, count = k.count,
    big = k.big, signed_big = k.signed_big, ratio = k.ratio, precise = k.precise,
    text = k.text, blob = k.blob, id = k.id,
    flat = { k.flat.x, k.flat.y, k.flat.z }, point = k.point,
    color_w = k.color.w, turn_w = k.turn.w, hint = k.extra.hint,
    faction = k.faction, fixed = k.fixed, strength = k.attributes.strength,
    maybe = k.maybe == nil, nothing = k.nothing == nil,
    crowd = k.crowd[1].agility, crowd_n = #k.crowd, by_id = k.by_id[7],
    types = { typeof(k.point), type(k.attributes), typeof(k.attributes), tostring(k.attributes) },
  }
end
function huge(k) return k.huge end
)");
  const scripting_test::Kinds kinds = sample_kinds();
  const Arg args[] = {Arg::object(scripting::object_ref(kinds))};
  JsonValue out;
  REQUIRE(context.call(id, "read", args, &out) == Status::Ok);
  CHECK(render(out) ==
        R"({"big":9007199254740992,"blob":"AB","by_id":"seven","color_w":1,"count":42,)"
        R"("crowd":3.5,"crowd_n":1,"faction":"Drifters","fixed":[1,2,3],"flag":true,)"
        R"("flat":[1,2,0],"hint":"north","id":"0123456789abcdeffedcba9876543210","maybe":true,)"
        R"("nothing":true,"point":[3,4,5],"precise":0.1,"ratio":0.25,"signed_big":-5,)"
        R"("small":7,"strength":10,"text":"hello","tiny":-3,"turn_w":1,)"
        R"("types":["vector","userdata","SchemaObject","engine.scripting_test.Attributes"],)"
        R"("whole":-12})");

  // 2^53 + 1 has no exact double: the read is an error, never a silently different number.
  CHECK(context.call(id, "huge", args, &out) == Status::RuntimeError);
  CHECK(context.last_error().message.find("2^53") != std::string::npos);
}

TEST_CASE("scripting bindings: an enum value with no name reads as its number") {
  ScriptContext context;
  const ScriptId id = must_load(context, "enum.luau", "function f(k) return k.faction end");
  scripting_test::Kinds kinds = sample_kinds();
  kinds.faction = static_cast<scripting_test::Faction>(9);
  const Arg args[] = {Arg::object(scripting::object_ref(kinds))};
  JsonValue out;
  REQUIRE(context.call(id, "f", args, &out) == Status::Ok);
  CHECK(out == JsonValue(i64{9}));
}

TEST_CASE("scripting bindings: a missing property is an error, not nil") {
  ScriptContext context;
  const ScriptId id = must_load(context, "missing.luau", R"(
function misspelt(npc)
  return npc.helth
end
function nested(npc) return npc.attributes.charisma end
function numeric(npc) return npc[1] end
function built(npc) return npc["hea" .. "lth"] end
)");
  scripting_test::Character npc;
  npc.health = 64.0f;
  const Arg args[] = {Arg::object(scripting::object_ref(npc))};
  CHECK(context.call(id, "misspelt", args) == Status::RuntimeError);
  CHECK(context.last_error().line == 3);
  CHECK(context.last_error().message == "engine.scripting_test.Character has no property 'helth'");
  CHECK(context.call(id, "nested", args) == Status::RuntimeError);
  CHECK(context.last_error().message ==
        "engine.scripting_test.Attributes has no property 'charisma'");
  CHECK(context.call(id, "numeric", args) == Status::RuntimeError);
  CHECK(context.last_error().message.find("must be a string") != std::string::npos);
  // A name built at run time finds its field as readily as a constant does.
  JsonValue out;
  REQUIRE(context.call(id, "built", args, &out) == Status::Ok);
  CHECK(out == JsonValue(i64{64}));
}

TEST_CASE("scripting bindings: read-only, so assignment is refused") {
  ScriptContext context;
  const ScriptId id = must_load(context, "write.luau", R"(
function heal(npc) npc.health = 100 end
function nested(npc) npc.attributes.strength = 99 end
function add_tag(npc) table.insert(npc.tags, "hero") end
function metatable(npc) return getmetatable(npc) end
)");
  scripting_test::Character npc;
  npc.health = 5.0f;
  npc.tags = {"scout"};
  const Arg args[] = {Arg::object(scripting::object_ref(npc))};
  CHECK(context.call(id, "heal", args) == Status::RuntimeError);
  CHECK(context.last_error().message.find(
            "engine.scripting_test.Character is read-only: 'health'") != std::string::npos);
  CHECK(npc.health == 5.0f);
  CHECK(context.call(id, "nested", args) == Status::RuntimeError);
  CHECK(context.last_error().message.find("read-only") != std::string::npos);
  CHECK(context.call(id, "add_tag", args) == Status::RuntimeError);
  CHECK(context.last_error().message.find("readonly") != std::string::npos);
  CHECK(npc.tags.size() == 1u);
  JsonValue out;
  REQUIRE(context.call(id, "metatable", args, &out) == Status::Ok);
  CHECK(out == JsonValue("locked"));
}

TEST_CASE("scripting bindings: views read the host object in place and never outlive it") {
  ScriptContext context;
  const ScriptId id = must_load(context, "keep.luau", R"(
local kept = {}
function keep(o) kept.o = o kept.attributes = o.attributes end
function name() return kept.o.name end
function strength() return kept.attributes.strength end
)");
  scripting_test::Character a;
  a.name = "Ada";
  scripting_test::Character b;
  b.name = "Bo";
  const scripting::BindingId binding = context.bind(scripting::object_ref(a));
  REQUIRE(binding.valid());
  const Arg bound[] = {Arg::binding(binding)};
  REQUIRE(context.call(id, "keep", bound) == Status::Ok);

  JsonValue out;
  REQUIRE(context.call(id, "name", {}, &out) == Status::Ok);
  CHECK(out == JsonValue("Ada"));
  a.name = "Ada Lovelace";  // no copy: the view reads the object as it is now
  REQUIRE(context.call(id, "name", {}, &out) == Status::Ok);
  CHECK(out == JsonValue("Ada Lovelace"));
  REQUIRE(context.call(id, "strength", {}, &out) == Status::Ok);
  CHECK(out == JsonValue(i64{10}));

  // Rebinding moves the binding's own view to the new object; a nested view of the old one is
  // stale, because it pointed into memory the host may since have freed.
  REQUIRE(context.rebind(binding, scripting::object_ref(b)) == Status::Ok);
  REQUIRE(context.call(id, "name", {}, &out) == Status::Ok);
  CHECK(out == JsonValue("Bo"));
  CHECK(context.call(id, "strength", {}, &out) == Status::RuntimeError);
  CHECK(context.last_error().message.find("no longer valid") != std::string::npos);

  context.unbind(binding);
  CHECK(context.call(id, "name", {}, &out) == Status::RuntimeError);
  CHECK(context.last_error().message.find("no longer valid") != std::string::npos);
  CHECK(context.rebind(binding, scripting::object_ref(a)) == Status::NotFound);

  // An object passed for one call cannot be read on the next.
  const Arg once[] = {Arg::object(scripting::object_ref(a))};
  REQUIRE(context.call(id, "keep", once) == Status::Ok);
  CHECK(context.call(id, "name", {}, &out) == Status::RuntimeError);
  CHECK(context.last_error().message.find("no longer valid") != std::string::npos);
}

TEST_CASE("scripting bindings: engine.object follows ids through the host's resolver") {
  struct World {
    scripting_test::Item rope;
    scripting_test::Item torch;
    Id128 rope_id = Id128::from_seed(1, 1);
    Id128 torch_id = Id128::from_seed(1, 2);
  };
  World world;
  world.rope.name = "rope";
  world.rope.weight = 1.5f;
  world.torch.name = "torch";
  world.torch.weight = 0.5f;

  ScriptContext context;
  context.set_resolver(
      scripting::ObjectResolver{&world, [](void* user, const Id128& id, scripting::ObjectRef& out) {
                                  auto* w = static_cast<World*>(user);
                                  if (id == w->rope_id)
                                    out = scripting::object_ref(w->rope);
                                  else if (id == w->torch_id)
                                    out = scripting::object_ref(w->torch);
                                  else
                                    return false;
                                  return true;
                                }});
  const ScriptId id = must_load(context, "carry.luau", R"(
function carried(npc)
  local total = 0
  for _, item_id in npc.inventory do
    local item = engine.object(item_id)
    if item then total += item.weight end
  end
  return total
end
function bad() return engine.object("not an id") end
)");
  scripting_test::Character npc;
  npc.inventory = {world.rope_id, world.torch_id, Id128::from_seed(9, 9)};
  const Arg args[] = {Arg::object(scripting::object_ref(npc))};
  JsonValue out;
  REQUIRE(context.call(id, "carried", args, &out) == Status::Ok);
  CHECK(out == JsonValue(i64{2}));
  CHECK(context.call(id, "bad") == Status::RuntimeError);
  CHECK(context.last_error().message.find("is not an id") != std::string::npos);
}

TEST_CASE("scripting bindings: returning a view returns the object's schema JSON") {
  ScriptContext context;
  const ScriptId id = must_load(context, "ret.luau", "function f(npc) return npc.attributes end");
  scripting_test::Character npc;
  npc.attributes.strength = 12;
  const Arg args[] = {Arg::object(scripting::object_ref(npc))};
  JsonValue out;
  REQUIRE(context.call(id, "f", args, &out) == Status::Ok);
  CHECK(render(out) == R"({"agility":1.5,"strength":12})");
}
