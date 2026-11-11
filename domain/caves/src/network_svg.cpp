// A plan as a survey a person reads (network.h): a plan view, a long section along strike, and
// the mouths' holes as the scene's terrain takes them. Text for people: nothing reads it back, and
// nothing is decided from it.
#include <domain/caves/network.h>
#include <domain/gfx/formation.h>

#include <algorithm>
#include <cstdio>

namespace engine::caves {

namespace {

constexpr f64 k_px = 4.0;  // pixels a metre

std::string num(f64 v, int decimals = 1) {
  char text[48];
  std::snprintf(text, sizeof(text), "%.*f", decimals, v);
  return text;
}

const char* edge_color(const PlanEdge& e) {
  switch (e.kind) {
    case StrandKind::trunk:
      return e.level == 0 ? "#1f4e79" : (e.level == 1 ? "#8a4b08" : "#6a1b6a");
    case StrandKind::feeder: return "#2e7d32";
    case StrandKind::connector: return "#7b1fa2";
    case StrandKind::bypass: return "#0277bd";
    case StrandKind::crawl: return "#757575";
    case StrandKind::lead: return "#ef6c00";
    case StrandKind::shaft: return "#c62828";
    case StrandKind::fissure: return "#c62828";
    case StrandKind::sump: return "#00838f";
  }
  return "#000000";
}

f64 metres(i64 mm) { return static_cast<f64>(mm) / 1000.0; }

std::string header(f64 w, f64 h) {
  return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + num(w, 0) + "\" height=\"" +
         num(h, 0) + "\" viewBox=\"0 0 " + num(w, 0) + " " + num(h, 0) +
         "\" font-family=\"sans-serif\" font-size=\"11\">\n"
         "<rect width=\"100%\" height=\"100%\" fill=\"#fbfaf6\"/>\n";
}

std::string text_at(f64 x, f64 y, const std::string& what, const char* fill = "#000") {
  return "<text x=\"" + num(x) + "\" y=\"" + num(y) + "\" fill=\"" + fill + "\">" + what +
         "</text>\n";
}

}  // namespace

std::string plan_svg(const Plan& plan) {
  f64 lo_x = 1e30, hi_x = -1e30, lo_z = 1e30, hi_z = -1e30;
  for (const PlanStation& ps : plan.stations) {
    lo_x = std::min(lo_x, metres(ps.station.x));
    hi_x = std::max(hi_x, metres(ps.station.x));
    lo_z = std::min(lo_z, metres(ps.station.z));
    hi_z = std::max(hi_z, metres(ps.station.z));
  }
  lo_x -= 15.0;
  lo_z -= 15.0;
  hi_x += 15.0;
  hi_z += 25.0;
  const f64 w = std::max(900.0, (hi_x - lo_x) * k_px), h = (hi_z - lo_z) * k_px;
  const auto sx = [&](i64 mm) { return (metres(mm) - lo_x) * k_px; };
  const auto sz = [&](i64 mm) { return (metres(mm) - lo_z) * k_px; };
  std::string out = header(w, h);
  for (const PlanHall& hall : plan.halls) {
    // The ellipse turned to its long axis by the axis itself: no angle is taken.
    const f64 ax = static_cast<f64>(hall.hall.axis_x) / 1048576.0;
    const f64 az = static_cast<f64>(hall.hall.axis_z) / 1048576.0;
    out += "<ellipse rx=\"" + num(metres(hall.hall.ra) * k_px) + "\" ry=\"" +
           num(metres(hall.hall.rb) * k_px) + "\" transform=\"matrix(" + num(ax, 4) + " " +
           num(az, 4) + " " + num(-az, 4) + " " + num(ax, 4) + " " + num(sx(hall.hall.x)) + " " +
           num(sz(hall.hall.z)) + ")\" fill=\"#d7ccc8\" stroke=\"#5d4037\"" +
           (hall.fallen ? " stroke-dasharray=\"4 2\"" : "") + "/>\n";
    out += text_at(sx(hall.hall.x) + 6.0, sz(hall.hall.z) - metres(hall.hall.rb) * k_px - 4.0,
                   "hall, span " + num(static_cast<f64>(hall.span_m), 0) + " m, cover " +
                       num(static_cast<f64>(hall.cover_m)) + " m" +
                       (hall.fallen ? ", fallen in" : ""));
  }
  for (const PlanEdge& e : plan.edges) {
    // The passage's width in plan, a station at a time.
    for (u32 j = 0; j + 1 < e.count; ++j) {
      const CellStation& a = plan.stations[e.first + j].station;
      const CellStation& b = plan.stations[e.first + j + 1].station;
      const f64 wide = std::max(0.6, 2.0 * metres(std::max(a.a, a.slot_hw)));
      out += "<line x1=\"" + num(sx(a.x)) + "\" y1=\"" + num(sz(a.z)) + "\" x2=\"" + num(sx(b.x)) +
             "\" y2=\"" + num(sz(b.z)) + "\" stroke=\"" + edge_color(e) + "\" stroke-width=\"" +
             num(wide * k_px) + "\" stroke-linecap=\"round\"" +
             (e.walkable ? "" : " stroke-opacity=\"0.55\"") + "/>\n";
    }
  }
  for (const PlanMouth& m : plan.mouths) {
    for (u32 pass = 0; pass < 2; ++pass) {
      const Vector<OutlinePoint>& ring = pass == 0 ? m.hole : m.outline;
      if (ring.empty()) continue;
      out += "<polygon points=\"";
      for (const OutlinePoint& p : ring)
        out += num(sx(p.x)) + "," + num(sz(p.z)) + " ";
      out += pass == 0 ? "\" fill=\"none\" stroke=\"#c62828\" stroke-dasharray=\"3 2\"/>\n"
                       : "\" fill=\"#ffcdd2\" stroke=\"#c62828\"/>\n";
    }
    out +=
        text_at(sx(m.x) + 8.0, sz(m.z) + 4.0,
                std::string(m.pit ? "pit " : "mouth ") + num(static_cast<f64>(m.area_m2)) + " m2",
                "#c62828");
  }
  for (const PlanNode& n : plan.nodes) {
    if (n.degree != 1 || n.end == EndKind::mouth) continue;
    out += "<circle cx=\"" + num(sx(n.x)) + "\" cy=\"" + num(sz(n.z)) +
           "\" r=\"3\" fill=\"#212121\"/>\n";
    out += text_at(sx(n.x) + 5.0, sz(n.z) + 4.0, end_name(n.end));
  }
  for (const PlanPortal& p : plan.portals) {
    if (p.cell_b == k_outside) continue;
    out += "<circle cx=\"" + num(sx(p.x)) + "\" cy=\"" + num(sz(p.z)) +
           "\" r=\"2\" fill=\"none\" stroke=\"#000\"/>\n";
  }
  // The scale, the dip, the numbers.
  out += "<line x1=\"20\" y1=\"" + num(h - 50.0) + "\" x2=\"" + num(20.0 + 20.0 * k_px) +
         "\" y2=\"" + num(h - 50.0) + "\" stroke=\"#000\" stroke-width=\"2\"/>\n";
  out += text_at(20.0, h - 55.0, "20 m");
  out += text_at(20.0, h - 30.0,
                 "dip " + num(static_cast<f64>(plan.formation.dip_deg), 0) +
                     " deg toward azimuth " + num(static_cast<f64>(plan.formation.azimuth_deg), 0) +
                     " deg; " + std::to_string(plan.report.levels) + " levels, " +
                     num(static_cast<f64>(plan.report.passage_m), 0) + " m of passage (" +
                     num(static_cast<f64>(plan.report.reachable_m), 0) +
                     " m walked from a mouth), " + std::to_string(plan.report.loops) + " loops, " +
                     std::to_string(plan.report.dead_ends) + " dead ends, " +
                     std::to_string(plan.mouths.size()) + " mouths, " +
                     std::to_string(plan.cells.size()) + " cells");
  out += text_at(20.0, h - 12.0,
                 "blue and brown: trunks by level; green: feeders; purple: connectors; grey: "
                 "crawls; orange: leads; red: mouths (dashed: the terrain's hole); rings: portals");
  out += "</svg>\n";
  return out;
}

std::string section_svg(const Plan& plan, const scene_gen::Ground& ground) {
  gfx::FormationParams params;
  gfx::pack_formation(plan.formation, params, nullptr);
  const gfx::FormationFrame frame = gfx::formation_frame(params);
  const f64 strike_x = -frame.dip[2], strike_z = frame.dip[0];
  const auto along = [&](i64 x, i64 z) {
    return metres(x - plan.anchor[0]) * strike_x + metres(z - plan.anchor[2]) * strike_z;
  };
  f64 lo_u = 1e30, hi_u = -1e30, lo_y = 1e30, hi_y = -1e30;
  for (const PlanStation& ps : plan.stations) {
    const f64 u = along(ps.station.x, ps.station.z);
    lo_u = std::min(lo_u, u);
    hi_u = std::max(hi_u, u);
    lo_y = std::min(
        lo_y, metres(ps.station.floor > ps.station.y - 60000 ? ps.station.floor : ps.station.y));
    hi_y = std::max(hi_y,
                    metres(ps.station.y) + metres(ps.station.b) + metres(std::max(ps.cover_mm, 0)));
  }
  lo_u -= 10.0;
  hi_u += 10.0;
  lo_y -= 8.0;
  hi_y += 8.0;
  const f64 w = std::max(900.0, (hi_u - lo_u) * k_px), h = (hi_y - lo_y) * k_px + 30.0;
  const auto su = [&](f64 u) { return (u - lo_u) * k_px; };
  const auto sy = [&](f64 y) { return (hi_y - y) * k_px; };
  std::string out = header(w, h);
  const f64 water = sy(metres(plan.water_mm));
  out += "<line x1=\"0\" y1=\"" + num(water) + "\" x2=\"" + num(w, 0) + "\" y2=\"" + num(water) +
         "\" stroke=\"#0288d1\" stroke-dasharray=\"6 3\"/>\n";
  out += text_at(6.0, water - 3.0, "base level " + num(metres(plan.water_mm)) + " m", "#0288d1");
  for (const PlanEdge& e : plan.edges) {
    // The ground over a trunk, then every edge's roof and floor.
    for (u32 pass = 0; pass < 3; ++pass) {
      if (pass == 0 && (e.kind != StrandKind::trunk || !ground.present())) continue;
      out += "<polyline fill=\"none\" points=\"";
      for (u32 j = 0; j < e.count; ++j) {
        const CellStation& s = plan.stations[e.first + j].station;
        f64 y = 0.0;
        if (pass == 0) {
          y = ground.surface_mm(s.x, s.z);
        } else if (pass == 1) {
          y = metres(s.y) + metres(std::max(s.b, s.slot_hw > 0 ? s.slot_top : 0));
        } else {
          y = s.frame == SectionFrame::Shaft ? metres(s.y) : metres(s.floor);
        }
        out += num(su(along(s.x, s.z))) + "," + num(sy(y)) + " ";
      }
      out += std::string("\" stroke=\"") + (pass == 0 ? "#6d4c41" : edge_color(e)) +
             "\" stroke-width=\"" + (pass == 0 ? "1" : "1.5") + "\"/>\n";
    }
  }
  for (const PlanHall& hall : plan.halls)
    out += "<ellipse cx=\"" + num(su(along(hall.hall.x, hall.hall.z))) + "\" cy=\"" +
           num(sy(metres(hall.hall.y))) + "\" rx=\"" + num(metres(hall.hall.ra) * k_px) +
           "\" ry=\"" + num(metres(hall.hall.rh) * k_px) +
           "\" fill=\"none\" stroke=\"#5d4037\"/>\n";
  out += text_at(6.0, h - 8.0,
                 "long section along strike, no vertical exaggeration; brown: the ground over each "
                 "trunk; the member is " +
                     num(plan.report.member_base_m) + " to " + num(plan.report.member_top_m) +
                     " m of bed coordinate, the least rock from a passage to its edge " +
                     num(plan.report.least_margin_m, 2) + " m");
  out += "</svg>\n";
  return out;
}

std::string survey_json(const Plan& plan) {
  const auto point = [](i64 x, i64 y, i64 z) {
    return "[" + num(metres(x), 3) + ", " + num(metres(y), 3) + ", " + num(metres(z), 3) + "]";
  };
  std::string out = "{\n  \"format\": \"engine.caves.survey\",\n  \"base_level_m\": " +
                    num(metres(plan.water_mm), 3) + ",\n  \"mouths\": [";
  for (u32 m = 0; m < plan.mouths.size(); ++m) {
    const PlanMouth& mouth = plan.mouths[m];
    const PlanEdge& edge = plan.edges[mouth.edge];
    const CellStation& tip = plan.stations[edge.first + edge.count - 1].station;
    out += std::string(m == 0 ? "" : ",") + "\n    {\"at\": " + point(mouth.x, mouth.y, mouth.z) +
           ", \"outward\": [" + num(tip.tx / 1048576.0, 4) + ", " + num(tip.ty / 1048576.0, 4) +
           ", " + num(tip.tz / 1048576.0, 4) +
           "], \"area_m2\": " + num(static_cast<f64>(mouth.area_m2)) +
           ", \"pit\": " + (mouth.pit ? "true" : "false") + "}";
  }
  out += "\n  ],\n  \"halls\": [";
  for (u32 h = 0; h < plan.halls.size(); ++h) {
    const PlanHall& hall = plan.halls[h];
    out += std::string(h == 0 ? "" : ",") +
           "\n    {\"floor\": " + point(hall.hall.x, hall.hall.y, hall.hall.z) +
           ", \"radii_m\": [" + num(metres(hall.hall.ra), 2) + ", " + num(metres(hall.hall.rb), 2) +
           ", " + num(metres(hall.hall.rh), 2) +
           "], \"fallen\": " + (hall.fallen ? "true" : "false") + "}";
  }
  out += "\n  ],\n  \"edges\": [";
  for (u32 e = 0; e < plan.edges.size(); ++e) {
    const PlanEdge& edge = plan.edges[e];
    out += std::string(e == 0 ? "" : ",") + "\n    {\"kind\": \"" + strand_name(edge.kind) +
           "\", \"level\": " + std::to_string(edge.level) +
           ", \"walkable\": " + (edge.walkable ? "true" : "false") +
           ", \"length_m\": " + num(static_cast<f64>(edge.length_m)) + ", \"floor\": [";
    // The floor under the centreline every fourth station, and the last: where a walker stands.
    bool first = true;
    for (u32 j = 0; j < edge.count; ++j) {
      if (j % 4 != 0 && j + 1 != edge.count) continue;
      const CellStation& s = plan.stations[edge.first + j].station;
      out += std::string(first ? "" : ", ") + point(s.x, s.floor, s.z);
      first = false;
    }
    out += "]}";
  }
  out += "\n  ]\n}\n";
  return out;
}

std::string holes_json(const Plan& plan) {
  std::string out = "[";
  u32 n = 0;
  for (const PlanMouth& m : plan.mouths) {
    if (m.hole.size() < 3) continue;
    out += std::string(n == 0 ? "" : ",") + "\n  {\"name\": \"cave-mouth-" + std::to_string(n) +
           "\", \"polygon\": [";
    for (u32 k = 0; k < m.hole.size(); ++k)
      out += std::string(k == 0 ? "" : ", ") + "[" + num(metres(m.hole[k].x), 3) + ", " +
             num(metres(m.hole[k].y), 3) + ", " + num(metres(m.hole[k].z), 3) + "]";
    out += "]}";
    ++n;
  }
  out += "\n]\n";
  return out;
}

}  // namespace engine::caves
