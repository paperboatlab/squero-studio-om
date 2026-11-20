// engine/modelica (#160): the omc bridge's allow-list and answer parser, the expression
// translator, and generate()'s path choice against a real omc.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_approx.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "squero/generate.hpp"
#include "squero/import.hpp"
#include "squero/omc.hpp"
#include "squero/squero.hpp"
#include <toml++/toml.hpp>

namespace mo = squero;
namespace fs = std::filesystem;
using nlohmann::json;

// A file's text, with the file closed again: Windows will not remove or replace a file a test
// still holds open, which an ifstream left in scope does.
static std::string read_text(const fs::path& file) {
  std::ifstream in(file);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A native omc the tests can drive. Windows has no `command -v`: ask the server's own lookup
// (PATH, then %OPENMODELICAHOME%\bin), which is native-only there.
static bool have_omc() {
#ifdef _WIN32
  return mo::resolve_omc().why.empty();
#else
  return std::system("command -v omc >/dev/null 2>&1") == 0;
#endif
}

#ifndef PAPERBOAT_TEST_DATA
#ifdef SQUERO_TEST_DATA
#define PAPERBOAT_TEST_DATA SQUERO_TEST_DATA
#else
#define PAPERBOAT_TEST_DATA "tests/data"
#endif
#endif


TEST_CASE("omc answers parse into lists, strings and atoms", "[modelica]") {
  const auto v = mo::parse_value(R"({{Real, x, "a \"q\"", {}}, (true, "/tmp/f.xml"), Placement(true, -1)})");
  REQUIRE(v.is_list);
  REQUIRE(v.size() == 3);
  CHECK(v[0][1].atom == "x");
  CHECK(v[0][2].is_string);
  CHECK(v[0][2].atom == "a \"q\"");
  CHECK(v[0][3].is_list);
  CHECK(v[1][0].atom == "true");
  CHECK(v[1][1].atom == "/tmp/f.xml");
  CHECK(v[2].atom == "Placement(true, -1)");
  CHECK_THROWS_AS(mo::parse_value("{a, b"), mo::OmcError);
}

TEST_CASE("squero/0 passes only allow-listed calls", "[modelica]") {
  CHECK(mo::allowed("getClassNames()"));
  CHECK(mo::allowed("getClassNames(recursive=true)"));
  CHECK(mo::allowed(R"(loadFile("/tmp/a b;c.mo"))"));
  CHECK(mo::allowed("addComponent(r1, Modelica.Electrical.Analog.Basic.Resistor, M, "
                    "annotate=Placement(transformation(extent={{-10,-10},{10,10}})))"));
  CHECK_FALSE(mo::allowed(R"(system("ls"))"));
  CHECK_FALSE(mo::allowed(R"(runScript("x.mos"))"));
  CHECK_FALSE(mo::allowed(R"(getClassNames(); system("ls"))"));
  CHECK_FALSE(mo::allowed(R"(loadFile(OpenModelica.Scripting.system("ls")))"));
  CHECK_FALSE(mo::allowed(R"(loadFile(readFile("x")))"));
  CHECK_FALSE(mo::allowed("x := getVersion()"));
  CHECK_FALSE(mo::allowed("getVersion"));
  CHECK_FALSE(mo::allowed(R"(loadFile("unterminated))"));

  mo::Omc omc;  // never started: handle() refuses before calling it
  // outside the macro: MSVC 2022 mis-stringizes a raw string holding \" inside CHECK(...)
  const std::string system_call = R"j({"id": 7, "op": "omc", "expr": "system(\"ls\")"})j";
  CHECK(mo::handle(omc, system_call).find(R"("ok":false)") != std::string::npos);
  CHECK(mo::handle(omc, R"({"id": 7, "op": "nope"})").find("unknown op") != std::string::npos);
}

TEST_CASE("Modelica expressions become C++", "[modelica]") {
  auto name = [](std::string_view n, bool der) { return (der ? "d_" : "") + std::string(n == "load.w" ? "w" : n); };
  CHECK(mo::to_cpp("1/2", name) == "(1.0 / 2.0)");  // Modelica's / is real division
  CHECK(mo::to_cpp("-a^2", name) == "(-std::pow(a, 2.0))");
  CHECK(mo::to_cpp("(-shaft.tau) / load.J", name) == "((-shaft.tau) / load.J)");
  CHECK(mo::to_cpp("der(load.w) + time", name) == "(d_w + t)");
  CHECK(mo::to_cpp("if x > 0.5 then -x elseif x < 0 then 0 else 2.0 * x", name) ==
        "((x > 0.5) ? (-x) : ((x < 0.0) ? 0.0 : (2.0 * x)))");
  CHECK(mo::to_cpp("noEvent(abs(x)) + max(a, 1e-3)", name) == "(std::abs(x) + std::max(a, 1e-3))");
  CHECK(mo::to_cpp("t.y[1] * .5", name) == "(t.y[1] * 0.5)");
  CHECK_THROWS_AS(mo::to_cpp("pre(z) + 1", name), mo::GenerateError);
  CHECK_THROWS_AS(mo::to_cpp("a .+ b", name), mo::GenerateError);
}

TEST_CASE("station names come from the class's short name", "[modelica]") {
  CHECK(mo::station_name("DCMotor") == "dc-motor");
  CHECK(mo::station_name("Modelica.Mechanics.Rotational.Components.Inertia") == "inertia");
  CHECK(mo::station_name("PIDController") == "pid-controller");
  CHECK(mo::station_name("mass_spring") == "mass-spring");
}

TEST_CASE("generate picks the ODE path when it can and says why when it can't", "[modelica][omc]") {
  if (!have_omc()) SKIP("omc not installed");
  const fs::path dir = fs::temp_directory_path() / "paperboat-modelica-test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  {
    std::ofstream(dir / "Decay.mo") << R"(model Decay "First-order decay with a probe"
  parameter Real k(unit = "1/s") = 2 "Rate";
  Modelica.Blocks.Interfaces.RealInput u;
  Modelica.Blocks.Interfaces.RealOutput y(unit = "m");
  Real x(start = 1, fixed = true, unit = "m");
  Real rate = k * x annotation(__paperboat(probe = true));
protected
  parameter Real g = 1 "a private variable: computed inside, not a prop";
equation
  der(x) = u - k * g * x;
  y = x;
end Decay;
)";
    std::ofstream(dir / "Bounce.mo") << R"(model Bounce
  Real h(start = 1, fixed = true); Real v(start = 0, fixed = true);
equation
  der(h) = v; der(v) = -9.81;
  when h < 0 then reinit(v, -0.8 * pre(v)); end when;
end Bounce;
)";
  }
  mo::Omc omc;
  const auto r = mo::generate(omc, {.source = dir / "Decay.mo", .out = dir / "decay-cpp"});
  CHECK(r.path == "ode");
  CHECK(r.name == "decay");
  const std::string domain = [&] {
    std::ifstream in(dir / "decay-cpp" / "domain" / "decay.cpp");
    return std::string(std::istreambuf_iterator<char>(in), {});
  }();
  CHECK(domain.find("class Decay final : public paperboat::Model") != std::string::npos);
  CHECK(domain.find(R"({.id = "k", .default_value = 2.0, .unit = "1/s"})") != std::string::npos);
  CHECK(domain.find(R"({.id = "x_start", .default_value = 1.0, .unit = "m"})") != std::string::npos);
  CHECK(domain.find(R"({.id = "g")") == std::string::npos);
  CHECK(domain.find("p_.g = 1.0;") != std::string::npos);
  CHECK(domain.find(R"(.outputs = {{"y", "signal", "m"}, {"rate", "signal", ""}})") != std::string::npos);
  CHECK(domain.find(".view = {.icon = nlohmann::json::parse(") != std::string::npos);
  CHECK(domain.find("registry.add_connector(\"decay\", \"RealInput\"") != std::string::npos);
  CHECK(domain.find("registry.add_connector(\"decay\", \"RealOutput\"") != std::string::npos);
  CHECK(fs::is_regular_file(dir / "decay-cpp" / "descriptor.toml"));
  CHECK(fs::is_regular_file(dir / "decay-cpp" / "modelica" / "Decay.mo"));
  const auto desc = toml::parse_file((dir / "decay-cpp" / "descriptor.toml").string());
  const auto* conns = desc["connectors"].as_table();
  REQUIRE(conns != nullptr);
  CHECK(conns->contains("RealInput"));
  CHECK(conns->contains("RealOutput"));
  CHECK(desc["connectors"]["RealInput"]["handle"].value<std::string>() == "triangle");
  CHECK(desc["connectors"]["RealInput"]["kind"].value<std::string>() == "input");
  CHECK(desc["connectors"]["RealOutput"]["handle"].value<std::string>() == "circle");
  CHECK(desc["connectors"]["RealOutput"]["kind"].value<std::string>() == "output");

  CHECK_THROWS_WITH(mo::generate(omc, {.source = dir / "Bounce.mo", .out = dir / "bounce-cpp", .path = "ode"}),
                    Catch::Matchers::ContainsSubstring("events"));
  CHECK(mo::check(omc, dir / "Bounce.mo", "Bounce").ok);
  fs::remove_all(dir);
}

namespace {

bool on_grid(const json& v) {
  return v.is_number_integer() && v.get<int>() >= 0 && v.get<int>() <= 100 && (v.get<int>() % 5) == 0;
}

void check_contract_tokens(const json& doc) {
  const std::string raw = doc.dump();
  CHECK(raw.find('#') == std::string::npos);
  CHECK(raw.find("rgb(") == std::string::npos);
  CHECK(raw.find("\"d\":") == std::string::npos);

  REQUIRE(doc.contains("icon"));
  REQUIRE(doc.contains("connectors"));
  REQUIRE(doc.contains("notice"));

  const json& icon = doc.at("icon");
  const int w = icon.value("w", 1);
  const int h = icon.value("h", 1);
  CHECK((w == 1 || w == 2));
  CHECK((h == 1 || h == 2));
  CHECK_FALSE((w == 2 && h == 2));

  static const std::set<std::string> kStrokes = {"ink", "muted", "electrical", "translational", "thermal", "fluid", "magnetic", "rotational"};  // never "signal"
  static const std::set<std::string> kWeights = {"thin", "normal", "wire"};
  static const std::set<std::string> kDashes = {"solid", "axis", "dash", "dot", "dashdot"};
  static const std::set<std::string> kFills = {"none", "ink", "raised", "sunken", "electrical", "translational", "thermal", "fluid", "magnetic", "rotational"};
  static const std::set<std::string> kCorners = {"0", "sm", "md"};
  static const std::set<std::string> kFonts = {"sans", "mono"};
  static const std::set<double> kSizes = {12.0, 12.5, 13.0, 15.0, 30.0};
  static const std::set<std::string> kSides = {"left", "right", "top", "bottom"};
  static const std::set<std::string> kVariants = {"hollow", "filled"};
  static const std::set<std::string> kKinds = {"acausal", "input", "output", "expandable"};
  static const std::set<std::string> kHandles = {"square", "diamond", "ring2", "triangle", "circle"};
  static const std::set<int> kConnSizes = {8, 10, 12};

  REQUIRE(icon.at("g").is_array());
  for (const auto& p : icon.at("g")) {
    if (p.contains("stroke")) CHECK(kStrokes.contains(p.at("stroke").get<std::string>()));
    if (p.contains("weight")) CHECK(kWeights.contains(p.at("weight").get<std::string>()));
    if (p.contains("dash")) CHECK(kDashes.contains(p.at("dash").get<std::string>()));
    if (p.contains("fill")) CHECK(kFills.contains(p.at("fill").get<std::string>()));
    const std::string t = p.at("t").get<std::string>();
    if (t == "line" || t == "poly") {
      REQUIRE(p.at("p").is_array());
      CHECK(p.at("p").size() >= 2);
      for (const auto& pt : p.at("p")) {
        REQUIRE(pt.size() == 2);
        CHECK(on_grid(pt[0]));
        CHECK(on_grid(pt[1]));
      }
    } else if (t == "rect") {
      CHECK(on_grid(p.at("x")));
      CHECK(on_grid(p.at("y")));
      CHECK(on_grid(p.at("w")));
      CHECK(on_grid(p.at("h")));
      if (p.contains("corner")) CHECK(kCorners.contains(p.at("corner").get<std::string>()));
    } else if (t == "ellipse" || t == "arc") {
      CHECK(on_grid(p.at("x")));
      CHECK(on_grid(p.at("y")));
      CHECK(on_grid(p.at("w")));
      CHECK(on_grid(p.at("h")));
      if (t == "arc") {
        CHECK(p.at("a0").is_number());
        CHECK(p.at("a1").is_number());
      }
    } else if (t == "text") {
      CHECK(on_grid(p.at("x")));
      CHECK(on_grid(p.at("y")));
      CHECK(p.at("s").is_string());
      if (p.contains("font")) CHECK(kFonts.contains(p.at("font").get<std::string>()));
      if (p.contains("size")) CHECK(kSizes.contains(p.at("size").get<double>()));
    } else {
      FAIL("unexpected primitive type: " + t);
    }
  }

  if (icon.contains("ports")) {
    for (const auto& port : icon.at("ports")) {
      CHECK_FALSE(port.at("id").get<std::string>().empty());
      CHECK(doc.at("connectors").contains(port.at("class").get<std::string>()));
      CHECK(kSides.contains(port.at("side").get<std::string>()));
      CHECK(on_grid(port.at("at")));
      if (port.contains("variant")) CHECK(kVariants.contains(port.at("variant").get<std::string>()));
    }
  }

  for (const auto& [name, c] : doc.at("connectors").items()) {
    CHECK_FALSE(name.empty());
    CHECK(kKinds.contains(c.at("kind").get<std::string>()));
    CHECK(kHandles.contains(c.at("handle").get<std::string>()));
    CHECK(kConnSizes.contains(c.at("size").get<int>()));
    const double wt = c.at("weight").get<double>();
    CHECK((std::abs(wt - 1.2) < 1e-9 || std::abs(wt - 1.8) < 1e-9 || std::abs(wt - 2.0) < 1e-9));
    if (c.contains("alt")) CHECK(kVariants.contains(c.at("alt").get<std::string>()));
    if (c.contains("dash")) CHECK((c.at("dash") == "solid" || c.at("dash") == "dashed"));
  }
}

json read_golden(const std::string& stem) {
  const fs::path p = fs::path(PAPERBOAT_TEST_DATA) / "modelica" / (stem + ".icon.json");
  std::ifstream in(p);
  REQUIRE(in.good());
  json doc;
  in >> doc;
  return doc;
}

}  // namespace

TEST_CASE("checked-in MSL icon goldens obey contract §3 and §4 token tables", "[modelica]") {
  for (const std::string stem : {"Resistor", "Inductor", "Capacitor", "Ground", "Inertia"}) {
    INFO(stem);
    check_contract_tokens(read_golden(stem));
  }

  const json resistor = read_golden("Resistor");
  const auto& rports = resistor["icon"]["ports"];
  REQUIRE(rports.size() == 3);
  CHECK(rports[0] == json{{"id", "p"}, {"class", "PositivePin"}, {"side", "left"}, {"at", 50}, {"variant", "filled"}});
  CHECK(rports[1] == json{{"id", "n"}, {"class", "NegativePin"}, {"side", "right"}, {"at", 50}, {"variant", "hollow"}});
  CHECK(rports[2] == json{{"id", "heatPort"}, {"class", "HeatPort_a"}, {"side", "bottom"}, {"at", 50},
                          {"variant", "filled"}, {"if", "useHeatPort"}});
  CHECK(resistor["connectors"]["PositivePin"]["potential"][0] == json{{"id", "v"}, {"unit", "V"}});
  CHECK(resistor["connectors"]["PositivePin"]["flow"][0] == json{{"id", "i"}, {"unit", "A"}});
  CHECK(resistor["connectors"]["HeatPort_a"]["dash"] == "dashed");

  const json ground = read_golden("Ground");
  REQUIRE(ground["icon"]["ports"].size() == 1);
  CHECK(ground["icon"]["ports"][0] == json{{"id", "p"}, {"class", "Pin"}, {"side", "top"}, {"at", 50}});

  const json inertia = read_golden("Inertia");
  const auto& iports = inertia["icon"]["ports"];
  REQUIRE(iports.size() == 2);
  CHECK(iports[0] == json{{"id", "flange_a"}, {"class", "Flange_a"}, {"side", "left"}, {"at", 50}, {"variant", "filled"}});
  CHECK(iports[1] == json{{"id", "flange_b"}, {"class", "Flange_b"}, {"side", "right"}, {"at", 50}, {"variant", "hollow"}});
  CHECK(inertia["connectors"]["Flange_a"]["size"] == 10);
  CHECK(inertia["connectors"]["Flange_a"]["weight"] == 1.8);
  CHECK(inertia["notice"]["simplified"] == 3);

  const json inductor = read_golden("Inductor");
  CHECK(inductor["notice"]["simplified"] == 4);
}

TEST_CASE("iconTransformation quantises edge slots, snaps rotation, and spreads collisions", "[modelica]") {
  // Prefer iconTransformation over diagram transformation.
  mo::RawPort placed{.id = "p", .class_name = "Pin"};
  const auto annos = mo::parse_value(
      "{Placement(transformation(extent={{90,-10},{110,10}}), "
      "iconTransformation(origin={0,20}, extent={{-110,-10},{-90,10}}, rotation=12))}");
  REQUIRE(mo::parse_placement(annos, placed));
  CHECK(placed.has_placement);
  CHECK(placed.origin_y == 20.0);
  CHECK(placed.x1 == -110.0);

  // Rotation snaps to 90° (85° -> 90° anticlockwise turns (-100, 0) into (0, -100) -> bottom 50);
  // mirrored extent (x1 > x2) keeps its centre.
  mo::RawPort rot_port{.id = "q", .class_name = "Pin", .has_placement = true,
                       .x1 = -110, .y1 = -10, .x2 = -90, .y2 = 10, .rotation = 85.0};
  mo::RawPort mir_port{.id = "m", .class_name = "Pin", .has_placement = true,
                       .x1 = 110, .y1 = -10, .x2 = 90, .y2 = 10, .rotation = 0.0};

  // Three ports colliding on left @ 50 spread across free 5-unit slots and record moved distances.
  mo::RawPort c1{.id = "a", .class_name = "Pin", .has_placement = true, .x1 = -110, .y1 = -2, .x2 = -90, .y2 = 2};
  mo::RawPort c2{.id = "b", .class_name = "Pin", .has_placement = true, .x1 = -110, .y1 = -1, .x2 = -90, .y2 = 1};
  mo::RawPort c3{.id = "c", .class_name = "Pin", .has_placement = true, .x1 = -110, .y1 = 0, .x2 = -90, .y2 = 2};

  mo::ImportNotice notice;
  const auto ports = mo::quantise_ports({rot_port, mir_port, c1, c2, c3}, notice);
  REQUIRE(ports.size() == 5);
  CHECK(ports[0].side == "bottom");
  CHECK(ports[0].at == 50);
  CHECK(ports[1].side == "right");
  CHECK(ports[1].at == 50);

  std::set<int> left_slots;
  for (std::size_t i = 2; i < 5; ++i) {
    CHECK(ports[i].side == "left");
    CHECK(ports[i].at % 5 == 0);
    CHECK(ports[i].at >= 0);
    CHECK(ports[i].at <= 100);
    CHECK(left_slots.insert(ports[i].at).second);
  }
  CHECK(ports[2].at < ports[3].at);
  CHECK(ports[3].at < ports[4].at);
  CHECK(notice.moved.size() >= 2);
  for (const auto& m : notice.moved) {
    CHECK(m.distance > 0);
    CHECK(m.distance % 5 == 0);
  }
}

TEST_CASE("Icon(graphics) quantiser maps primitives, drops Bitmap/DynamicSelect, and bounds plates", "[modelica]") {
  // Wide aspect (>= 1.75:1) -> 2x1; 2x2 is never emitted even for large square extents.
  const auto wide = mo::parse_value(
      "{-175.0,-100.0,175.0,100.0,true,-,-,,{"
      "DynamicSelect(Rectangle(true,{0,0},0,{0,0,0},{255,255,255},LinePattern.Dash,FillPattern.HorizontalCylinder,0.5,"
      "BorderPattern.None,{{-100,50},{100,-50}},18.0), Rectangle(extent={{0,0},{10,10}})),"
      "Ellipse(true,{0,0},0,{160,160,160},{0,0,0},LinePattern.Solid,FillPattern.None,0.1,{{-40,40},{40,-40}},30.0,210.0),"
      "Polygon(true,{0,0},0,{0,0,0},{0,0,0},LinePattern.Solid,FillPattern.Solid,0.25,{{-20,10},{20,0},{-20,-10}},Smooth.None),"
      "Bitmap(extent={{-10,-10},{10,10}}, fileName=\"modelica://Pkg/img.png\"),"
      "Text(true,{0,0},0,{0,0,0},{0,0,0},LinePattern.Solid,FillPattern.None,0.25,{{-100,-60},{100,-90}},\"k=%k\",0.0,{-1,-1,-1},\"\",{},TextAlignment.Center)"
      "}}");
  mo::ImportNotice notice;
  const json icon = mo::quantise_icon({wide}, {}, notice);
  CHECK(icon.value("w", 1) == 2);
  CHECK(icon.value("h", 1) == 1);
  REQUIRE(icon["g"].size() == 4);
  CHECK(icon["g"][0]["t"] == "rect");
  CHECK(icon["g"][0]["dash"] == "dash");
  CHECK(icon["g"][0]["weight"] == "wire");
  CHECK(icon["g"][0]["fill"] == "raised");
  CHECK(icon["g"][0]["corner"] == "md");
  CHECK(icon["g"][1]["t"] == "arc");
  CHECK(icon["g"][1]["stroke"] == "muted");
  CHECK(icon["g"][1]["weight"] == "thin");
  CHECK(icon["g"][2]["t"] == "poly");
  CHECK(icon["g"][2]["fill"] == "ink");
  CHECK(icon["g"][3]["t"] == "text");
  CHECK(icon["g"][3]["s"] == "k={k}");
  CHECK(icon["g"][3]["font"] == "mono");

  CHECK(std::ranges::find(notice.dropped, "DynamicSelect") != notice.dropped.end());
  CHECK(std::ranges::find(notice.dropped, "Bitmap") != notice.dropped.end());
  CHECK(notice.simplified >= 3);

  // Large square box stays 1x1, never 2x2.
  mo::ImportNotice sq_notice;
  const json sq = mo::quantise_icon({mo::parse_value("{-400.0,-400.0,400.0,400.0,true,-,-,,{}}")}, {}, sq_notice);
  CHECK_FALSE(sq.contains("w"));
  CHECK_FALSE(sq.contains("h"));
}

TEST_CASE("live omc importer matches MSL goldens and resolves array, if, and fallback connectors", "[modelica][omc]") {
  if (!have_omc()) SKIP("omc not installed");
  mo::Omc omc;

  const std::pair<const char*, const char*> msl_cases[] = {
      {"Resistor", "Modelica.Electrical.Analog.Basic.Resistor"},
      {"Inductor", "Modelica.Electrical.Analog.Basic.Inductor"},
      {"Capacitor", "Modelica.Electrical.Analog.Basic.Capacitor"},
      {"Ground", "Modelica.Electrical.Analog.Basic.Ground"},
      {"Inertia", "Modelica.Mechanics.Rotational.Components.Inertia"},
  };
  for (const auto& [stem, cls] : msl_cases) {
    INFO(cls);
    const json live = mo::import_class(omc, cls).to_json();
    CHECK(live == read_golden(stem));
  }

  // Local fixture with Pin p[n] and `if useHeatPort`.
  const fs::path bus_mo = fs::path(PAPERBOAT_TEST_DATA) / "modelica" / "BusNode.mo";
  const auto bus = mo::import_model(omc, bus_mo);
  check_contract_tokens(bus.to_json());
  REQUIRE(bus.icon["ports"].size() == 3);
  CHECK(bus.icon["ports"][0]["id"] == "p");
  CHECK(bus.icon["ports"][0]["array"] == "n");
  CHECK(bus.icon["ports"][2]["id"] == "heatPort");
  CHECK(bus.icon["ports"][2]["if"] == "useHeatPort");

  // Unknown user-defined connector falls back to nearest domain handle and increments simplified.
  const fs::path tmp = fs::temp_directory_path() / "paperboat-modelica-custom-conn";
  fs::remove_all(tmp);
  fs::create_directories(tmp);
  std::ofstream(tmp / "CustomThermal.mo") << R"(package CustomThermal
  connector MyHeatNode
    Real T(unit = "K");
    flow Real Q_flow(unit = "W");
  end MyHeatNode;

  partial model BaseShell
    MyHeatNode port_a annotation(Placement(iconTransformation(extent = {{-110, -10}, {-90, 10}})));
  end BaseShell;

  model Wall
    extends BaseShell;
    MyHeatNode port_b annotation(Placement(iconTransformation(extent = {{90, -10}, {110, 10}})));
  end Wall;
end CustomThermal;
)";
  const auto wall = mo::import_model(omc, tmp / "CustomThermal.mo", "CustomThermal.Wall");
  check_contract_tokens(wall.to_json());
  REQUIRE(wall.connectors.contains("MyHeatNode"));
  CHECK(wall.connectors.at("MyHeatNode").handle == "diamond");
  CHECK(wall.connectors.at("MyHeatNode").dash == "dashed");
  CHECK(wall.notice.simplified >= 1);
  REQUIRE(wall.icon["ports"].size() == 2);
  CHECK(wall.icon["ports"][0]["id"] == "port_a");
  CHECK(wall.icon["ports"][1]["id"] == "port_b");
  fs::remove_all(tmp);
}

// --- squero/0 ops (#160 Wave 3) ---

namespace {

json op(mo::Omc& omc, json request, const mo::Session& session = {}) {
  return json::parse(mo::handle(omc, request.dump(), session));
}

}  // namespace

TEST_CASE("set_icon writes tokens as fixed Modelica values and refuses what it can't", "[squero]") {
  const json icon = {{"w", 2},
                     {"g", {{{"t", "rect"}, {"x", 15}, {"y", 35}, {"w", 70}, {"h", 30}, {"fill", "raised"}, {"corner", "md"}},
                            {{"t", "line"}, {"p", {{5, 50}, {15, 50}}}, {"weight", "wire"}, {"dash", "axis"}},
                            {{"t", "text"}, {"x", 50}, {"y", 50}, {"s", "R={R}"}, {"font", "mono"}, {"stroke", "muted"}}}}};
  const std::string text = mo::icon_annotation(icon);
  CHECK_THAT(text, Catch::Matchers::StartsWith("Icon(coordinateSystem(extent={{-200, -100}, {200, 100}})"));
  CHECK_THAT(text, Catch::Matchers::ContainsSubstring("fillPattern=FillPattern.Solid"));
  CHECK_THAT(text, Catch::Matchers::ContainsSubstring("pattern=LinePattern.Dash"));
  CHECK_THAT(text, Catch::Matchers::ContainsSubstring("textString=\"R=%R\""));
  // y runs down in an icon and up in Modelica: y=50 is the middle, y=0 the top.
  CHECK_THAT(mo::icon_annotation({{"g", {{{"t", "line"}, {"p", {{0, 0}, {100, 100}}}}}}}),
             Catch::Matchers::ContainsSubstring("points={{-100, 100}, {100, -100}}"));

  CHECK_THROWS_AS(mo::icon_annotation({{"g", {{{"t", "bezier"}}}}}), mo::OmcError);
  CHECK_THROWS_AS(mo::icon_annotation({{"g", {{{"t", "line"}, {"p", {{0, 0}, {5, 5}}}, {"stroke", "pink"}}}}}), mo::OmcError);
  CHECK_THROWS_AS(mo::icon_annotation({{"w", 2}, {"h", 2}, {"g", json::array()}}), mo::OmcError);

  mo::Omc omc;  // never started: both refuse before calling it
  CHECK(op(omc, {{"id", 1}, {"op", "set_icon"}, {"model", "Modelica.Electrical.Analog.Basic.Resistor"}, {"icon", icon}})["ok"] == false);
  CHECK(op(omc, {{"id", 2}, {"op", "save"}, {"model", "Modelica"}})["error"].get<std::string>().find("may edit") != std::string::npos);
  CHECK(op(omc, {{"id", 3}, {"op", "simulate"}, {"model", "M"}, {"stop", -1}})["ok"] == false);
}

TEST_CASE("set_icon round-trips the Wave 2 icon goldens through omc", "[squero][omc]") {
  if (!have_omc()) SKIP("omc not installed");
  mo::Omc omc;
  for (const char* stem : {"Resistor", "Inductor", "Capacitor", "Ground", "Inertia"}) {
    INFO(stem);
    const json golden = read_golden(stem)["icon"];
    const std::string model = std::string("RT") + stem;
    const fs::path file = fs::temp_directory_path() / (model + ".mo");
    std::ofstream(file) << "model " << model << "\nend " << model << ";\n";
    CHECK(op(omc, {{"id", 1}, {"op", "open"}}, {.file = file})["ok"] == true);
    const json reply = op(omc, {{"id", 2}, {"op", "set_icon"}, {"model", model}, {"icon", golden}});
    INFO(reply.dump());
    REQUIRE(reply["ok"] == true);
    const json back = mo::import_class(omc, model).icon;
    CHECK(back["g"] == golden["g"]);
    CHECK(back.value("w", 1) == golden.value("w", 1));
    CHECK(back.value("h", 1) == golden.value("h", 1));
    CHECK(op(omc, {{"id", 3}, {"op", "save"}, {"model", model}})["ok"] == true);
    CHECK_THAT(read_text(file), Catch::Matchers::ContainsSubstring("Icon("));
    fs::remove(file);
  }
}

TEST_CASE("icon, generate (dry, with progress) and simulate answer over handle", "[squero][omc]") {
  if (!have_omc()) SKIP("omc not installed");
  const fs::path dir = fs::temp_directory_path() / "paperboat-squero-test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "Decay.mo";
  std::ofstream(file) << R"(model Decay
  parameter Real k = 2;
  Real x(start = 1, fixed = true);
equation
  der(x) = -k * x;
end Decay;
)";
  mo::Omc omc;
  std::vector<json> pushed;
  const mo::Session session{[&](const std::string& text) { pushed.push_back(json::parse(text)); }, file};

  const json opened = op(omc, {{"id", 1}, {"op", "open"}}, session);
  REQUIRE(opened["ok"] == true);
  CHECK(opened["result"]["model"] == "Decay");

  const json icon = op(omc, {{"id", 2}, {"op", "icon"}, {"class", "Modelica.Electrical.Analog.Basic.Resistor"}}, session);
  REQUIRE(icon["ok"] == true);
  CHECK(icon["result"]["icon"]["ports"].size() == 3);  // p, n and the conditional heatPort
  CHECK(icon["result"]["connectors"].contains("PositivePin"));
  CHECK(op(omc, {{"id", 3}, {"op", "icon"}, {"class", "Modelica.Electrical.Analog.Basic.Resistor"}}, session) ==
        json{{"id", 3}, {"ok", true}, {"result", icon["result"]}});  // the cached answer is the same

  const json dry = op(omc, {{"id", 4}, {"op", "generate"}, {"source", file.string()}, {"model", "Decay"}, {"dry", true}}, session);
  INFO(dry.dump());
  REQUIRE(dry["ok"] == true);
  CHECK(dry["result"]["path"] == "ode");
  CHECK(dry["result"]["files"].contains("domain/decay.cpp"));
  CHECK_FALSE(fs::exists("decay-cpp"));
  CHECK_FALSE(fs::exists(dir / "decay-cpp"));
  REQUIRE_FALSE(pushed.empty());
  CHECK(pushed.front()["op"] == "progress");
  CHECK(pushed.front()["id"] == 4);
  CHECK(pushed.back()["stage"] == "generate");
  CHECK(pushed.back()["fraction"] == 1);

  const json sim = op(omc, {{"id", 5}, {"op", "simulate"}, {"model", "Decay"}, {"stop", 1.0}, {"vars", {"x"}}}, session);
  INFO(sim.dump());
  REQUIRE(sim["ok"] == true);
  const auto& t = sim["result"]["t"];
  const auto& x = sim["result"]["series"]["x"];
  REQUIRE(t.size() == x.size());
  CHECK(t.size() > 100);
  CHECK(x.front().get<double>() == Catch::Approx(1.0));
  CHECK(x.back().get<double>() == Catch::Approx(std::exp(-2.0)).epsilon(0.01));  // x' = -2x
  CHECK(sim["result"]["series"].size() == 1);

  // A top-level input is driven by its stimulus (0 unless given); names stay the model's own.
  std::ofstream(dir / "Lag.mo") << R"(model Lag
  Modelica.Blocks.Interfaces.RealInput u;
  Modelica.Blocks.Interfaces.RealOutput y;
  Real x(start = 0, fixed = true);
equation
  der(x) = u - x;
  y = x;
end Lag;
)";
  REQUIRE(op(omc, {{"id", 6}, {"op", "omc"}, {"expr", "loadFile(" + mo::quote((dir / "Lag.mo").string()) + ")"}}, session)["ok"] == true);
  const json lag = op(omc, {{"id", 7}, {"op", "simulate"}, {"model", "Lag"}, {"stop", 2.0}, {"vars", {"u", "y"}},
                            {"inputs", {{"u", {{"kind", "step"}, {"k", 3.0}, {"t0", 1.0}}}}}}, session);
  INFO(lag.dump());
  REQUIRE(lag["ok"] == true);
  const auto& u = lag["result"]["series"]["u"];
  REQUIRE_FALSE(u.empty());
  CHECK(u.front().get<double>() == 0.0);
  CHECK(u.back().get<double>() == Catch::Approx(3.0));
  CHECK(lag["result"]["series"]["y"].back().get<double>() == Catch::Approx(3.0 * (1 - std::exp(-1.0))).epsilon(0.01));
  CHECK(op(omc, {{"id", 8}, {"op", "omc"}, {"expr", "getClassNames()"}}, session)["result"].get<std::string>().find("SqueroSimHarness") ==
        std::string::npos);  // the harness is gone
  CHECK(op(omc, {{"id", 9}, {"op", "simulate"}, {"model", "Lag"}, {"stop", 1.0}, {"inputs", {{"u", {{"kind", "noise"}}}}}}, session)["error"]
            .get<std::string>()
            .find("constant, step, ramp or sine") != std::string::npos);


  // Generate builds what is on screen: an unsaved edit is exported and survives, the file is not reloaded.
  REQUIRE(op(omc, {{"id", 10}, {"op", "omc"}, {"expr", "setParameterValue(Decay, k, 5)"}}, session)["ok"] == true);
  const json live = op(omc, {{"id", 11}, {"op", "generate"}, {"source", file.string()}, {"model", "Decay"}, {"dry", true}}, session);
  INFO(live.dump());
  REQUIRE(live["ok"] == true);
  CHECK(live["result"]["files"]["modelica/Decay.mo"].get<std::string>().find("k = 5") != std::string::npos);
  CHECK(op(omc, {{"id", 12}, {"op", "omc"}, {"expr", "list(Decay)"}}, session)["result"].get<std::string>().find("k = 5") != std::string::npos);

  fs::remove_all(dir);
}

TEST_CASE("workspace lists the model folder's classes and packages", "[squero]") {
  const fs::path dir = fs::temp_directory_path() / "paperboat-squero-workspace";
  fs::remove_all(dir);
  fs::create_directories(dir / "Pkg" / "Sub");
  std::ofstream(dir / "Pkg" / "package.mo") << "package Pkg\nend Pkg;\n";
  std::ofstream(dir / "Pkg" / "A.mo") << "model A\nend A;\n";
  std::ofstream(dir / "Pkg" / "Sub" / "package.mo") << "package Sub\nend Sub;\n";
  std::ofstream(dir / "Pkg" / "Sub" / "B.mo") << "model B\nend B;\n";
  std::ofstream(dir / "Pkg" / "Port.mo") << "within Pkg;\nconnector Port\n  Real p;\nend Port;\n";
  std::ofstream(dir / "Pkg" / "F.mo") << "within Pkg;\nfunction F\n  input Real u;\n  output Real y;\nalgorithm\n  y := u;\nend F;\n";
  std::ofstream(dir / "Quick.mo") << "model Quick\nend Quick;\n";
  std::ofstream(dir / "notes.txt") << "not Modelica\n";

  mo::Omc omc;  // never started: workspace reads the disk
  const json quick = op(omc, {{"op", "workspace"}, {"file", (dir / "Quick.mo").string()}});
  INFO(quick.dump());
  REQUIRE(quick["ok"] == true);
  CHECK(quick["result"]["root"] == dir.string());
  CHECK(quick["result"]["package"] == false);
  bool saw_quick = false;
  for (const auto& entry : quick["result"]["entries"]) {
    if (entry["name"] == "Quick") saw_quick = entry["kind"] == "class" && entry["depth"] == 0;
  }
  CHECK(saw_quick);

  const json nested = op(omc, {{"op", "workspace"}, {"file", (dir / "Pkg" / "Sub" / "B.mo").string()}});
  REQUIRE(nested["ok"] == true);
  INFO(nested.dump());
  CHECK(nested["result"]["root"] == dir.string());
  CHECK(nested["result"]["package"] == true);
  std::vector<std::string> seen;
  for (const auto& entry : nested["result"]["entries"]) seen.push_back(entry["kind"].get<std::string>() + ":" + entry["name"].get<std::string>() + ":" + std::to_string(entry["depth"].get<int>()));
  CHECK(seen == std::vector<std::string>{"package:Pkg:0", "class:A:1", "class:F:1", "class:Port:1", "package:Sub:1", "class:B:2", "class:Quick:0"});
  for (const auto& entry : nested["result"]["entries"]) {
    if (entry["name"] == "A") CHECK(entry["classKind"] == "model");
    if (entry["name"] == "Port") CHECK(entry["classKind"] == "connector");
    if (entry["name"] == "F") CHECK(entry["classKind"] == "function");
  }

  fs::remove_all(dir);
}

TEST_CASE("create_class writes a class and keeps package.order in step", "[squero]") {
  const fs::path dir = fs::temp_directory_path() / "paperboat-squero-new-class";
  fs::remove_all(dir);
  fs::create_directories(dir / "Pkg");
  std::ofstream(dir / "Pkg" / "package.mo") << "package Pkg\nend Pkg;\n";
  std::ofstream(dir / "Pkg" / "package.order") << "A\n";
  std::ofstream(dir / "Pkg" / "A.mo") << "within Pkg;\nmodel A\nend A;\n";

  mo::Omc omc;  // never started: create_class writes files
  const json made = op(omc, {{"op", "create_class"},
                             {"file", (dir / "Pkg" / "B.mo").string()},
                             {"name", "B"},
                             {"kind", "block"},
                             {"within", "Pkg"},
                             {"base", "Modelica.Blocks.Icons.Block"}});
  INFO(made.dump());
  REQUIRE(made["ok"] == true);
  CHECK(made["result"]["model"] == "Pkg.B");
  const std::string text = read_text(dir / "Pkg" / "B.mo");
  CHECK_THAT(text, Catch::Matchers::ContainsSubstring("within Pkg;"));
  CHECK_THAT(text, Catch::Matchers::ContainsSubstring("block B"));
  CHECK_THAT(text, Catch::Matchers::ContainsSubstring("extends Modelica.Blocks.Icons.Block;"));
  CHECK(read_text(dir / "Pkg" / "package.order") == "A\nB\n");

  // A name that is taken, a bad name, and a bad kind are refused without touching the disk.
  CHECK(op(omc, {{"op", "create_class"}, {"file", (dir / "Pkg" / "A.mo").string()}, {"name", "A"}, {"kind", "model"}, {"within", "Pkg"}})["ok"] == false);
  CHECK(op(omc, {{"op", "create_class"}, {"file", (dir / "Pkg" / "1B.mo").string()}, {"name", "1B"}, {"kind", "model"}})["ok"] == false);
  CHECK(op(omc, {{"op", "create_class"}, {"file", (dir / "Pkg" / "C.mo").string()}, {"name", "C"}, {"kind", "package"}})["ok"] == false);

  // A quick class at the root has no within and no package.order.
  const json quick = op(omc, {{"op", "create_class"}, {"file", (dir / "Q.mo").string()}, {"name", "Q"}, {"kind", "function"}});
  REQUIRE(quick["ok"] == true);
  CHECK(quick["result"]["model"] == "Q");
  CHECK_THAT(read_text(dir / "Q.mo"), Catch::Matchers::StartsWith("function Q"));
  CHECK_FALSE(fs::exists(dir / "package.order"));

  fs::remove_all(dir);
}

TEST_CASE("rename_class renames the file and the class and keeps its package.order place", "[squero]") {
  const fs::path dir = fs::temp_directory_path() / "paperboat-squero-rename";
  fs::remove_all(dir);
  fs::create_directories(dir / "Pkg");
  std::ofstream(dir / "Pkg" / "package.mo") << "package Pkg\nend Pkg;\n";
  std::ofstream(dir / "Pkg" / "package.order") << "A\nB\nC\n";
  std::ofstream(dir / "Pkg" / "B.mo") << "within Pkg;\nmodel B\n  Real x;\nend B;\n";
  std::ofstream(dir / "Pkg" / "C.mo") << "within Pkg;\nmodel C\nend C;\n";

  mo::Omc omc;  // never started: rename_class writes files
  const json made = op(omc, {{"op", "rename_class"}, {"file", (dir / "Pkg" / "B.mo").string()}, {"name", "B"}, {"newName", "Bee"}, {"within", "Pkg"}});
  INFO(made.dump());
  REQUIRE(made["ok"] == true);
  CHECK(made["result"]["model"] == "Pkg.Bee");
  CHECK_FALSE(fs::exists(dir / "Pkg" / "B.mo"));
  CHECK(read_text(dir / "Pkg" / "Bee.mo") == "within Pkg;\nmodel Bee\n  Real x;\nend Bee;\n");
  CHECK(read_text(dir / "Pkg" / "package.order") == "A\nBee\nC\n");

  // A taken name is refused without touching the disk.
  CHECK(op(omc, {{"op", "rename_class"}, {"file", (dir / "Pkg" / "Bee.mo").string()}, {"name", "Bee"}, {"newName", "C"}, {"within", "Pkg"}})["ok"] == false);
  CHECK(fs::exists(dir / "Pkg" / "Bee.mo"));
  fs::remove_all(dir);
}

TEST_CASE("packages and class files can be created, duplicated and deleted", "[squero]") {
  const fs::path dir = fs::temp_directory_path() / "paperboat-squero-package-ops";
  fs::remove_all(dir);
  fs::create_directories(dir / "Pkg");
  std::ofstream(dir / "Pkg" / "package.mo") << "package Pkg\nend Pkg;\n";
  std::ofstream(dir / "Pkg" / "package.order") << "A\n";
  std::ofstream(dir / "Pkg" / "A.mo") << "within Pkg;\nmodel A\n  Real x;\nend A;\n";

  mo::Omc omc;  // never started: these ops only touch the disk
  const json pkg = op(omc, {{"op", "new_package"}, {"parent", dir.string()}, {"name", "Sub"}, {"within", "Pkg"}});
  INFO(pkg.dump());
  REQUIRE(pkg["ok"] == true);
  CHECK(fs::exists(dir / "Sub" / "package.mo"));
  CHECK(fs::exists(dir / "Sub" / "package.order"));

  const json dup = op(omc, {{"op", "duplicate_class"}, {"file", (dir / "Pkg" / "A.mo").string()}, {"name", "A"}, {"newName", "B"}, {"within", "Pkg"}});
  INFO(dup.dump());
  REQUIRE(dup["ok"] == true);
  CHECK(dup["result"]["model"] == "Pkg.B");
  const std::string text = read_text(dir / "Pkg" / "B.mo");
  CHECK_THAT(text, Catch::Matchers::ContainsSubstring("model B"));
  CHECK_THAT(text, Catch::Matchers::ContainsSubstring("end B;"));
  CHECK(read_text(dir / "Pkg" / "package.order") == "A\nB\n");

  // Read the file's own text, then move it to another package and rewrite its within.
  const json read = op(omc, {{"op", "read_file"}, {"file", (dir / "Pkg" / "A.mo").string()}});
  REQUIRE(read["ok"] == true);
  CHECK_THAT(read["result"]["text"].get<std::string>(), Catch::Matchers::ContainsSubstring("model A"));
  const json moved = op(omc, {{"op", "move_class"},
                              {"file", (dir / "Pkg" / "A.mo").string()},
                              {"targetDir", (dir / "Sub").string()},
                              {"within", "Pkg.Sub"}});
  INFO(moved.dump());
  REQUIRE(moved["ok"] == true);
  CHECK(moved["result"]["model"] == "Pkg.Sub.A");
  CHECK_FALSE(fs::exists(dir / "Pkg" / "A.mo"));
  CHECK_THAT(read_text(dir / "Sub" / "A.mo"), Catch::Matchers::ContainsSubstring("within Pkg.Sub;"));
  CHECK(read_text(dir / "Sub" / "package.order") == "A\n");
  CHECK(read_text(dir / "Pkg" / "package.order") == "B\n");

  CHECK(op(omc, {{"op", "delete_class"}, {"file", (dir / "Pkg" / "B.mo").string()}, {"name", "B"}})["ok"] == true);
  CHECK_FALSE(fs::exists(dir / "Pkg" / "B.mo"));
  CHECK(read_text(dir / "Pkg" / "package.order") == "");

  // A package goes with everything under it, but never the workspace root or a folder outside it.
  CHECK(op(omc, {{"op", "delete_package"}, {"dir", dir.string()}, {"root", dir.string()}})["ok"] == false);
  const json removed_pkg = op(omc, {{"op", "delete_package"}, {"dir", (dir / "Sub").string()}, {"root", dir.string()}});
  INFO(removed_pkg.dump());
  REQUIRE(removed_pkg["ok"] == true);
  CHECK_FALSE(fs::exists(dir / "Sub"));
  CHECK(fs::exists(dir / "Pkg" / "B.mo") == false);

  fs::remove_all(dir);
}

TEST_CASE("opening a class inside a package loads the package chain", "[squero][omc]") {
  if (!have_omc()) SKIP("omc not installed");
  const fs::path dir = fs::temp_directory_path() / "paperboat-squero-package-open";
  fs::remove_all(dir);
  fs::create_directories(dir / "Pkg" / "Sub");
  std::ofstream(dir / "Pkg" / "package.mo") << "package Pkg\nend Pkg;\n";
  std::ofstream(dir / "Pkg" / "package.order") << "Sub\n";
  std::ofstream(dir / "Pkg" / "Sub" / "package.mo") << "within Pkg;\npackage Sub\nend Sub;\n";
  std::ofstream(dir / "Pkg" / "Sub" / "package.order") << "B\n";
  std::ofstream(dir / "Pkg" / "Sub" / "B.mo") << "within Pkg.Sub;\nmodel B\n  Real x(start = 1, fixed = true);\nequation\n  der(x) = -x;\nend B;\n";

  mo::Omc omc;
  const json opened = op(omc, {{"id", 1}, {"op", "open"}, {"file", (dir / "Pkg" / "Sub" / "B.mo").string()}});
  INFO(opened.dump());
  REQUIRE(opened["ok"] == true);
  CHECK(opened["result"]["model"] == "Pkg.Sub.B");

  fs::remove_all(dir);
}

TEST_CASE("check_model, messages and the flat model answer over handle", "[squero][omc]") {
  if (!have_omc()) SKIP("omc not installed");
  const fs::path dir = fs::temp_directory_path() / "paperboat-squero-messages";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path good = dir / "Good.mo";
  std::ofstream(good) << R"(model Good
  parameter Real k = 2;
  Real x(start = 1, fixed = true);
equation
  der(x) = -k * x;
end Good;
)";
  const fs::path bad = dir / "Bad.mo";
  std::ofstream(bad) << R"(model Bad
  Real x;
equation
  x = "not a Real";
end Bad;
)";

  mo::Omc omc;
  std::vector<mo::OmcMessage> logged;
  omc.on_log([&](const mo::OmcMessage& message) { logged.push_back(message); });
  const mo::Session session{nullptr, good};
  REQUIRE(op(omc, {{"id", 1}, {"op", "open"}}, session)["ok"] == true);

  logged.clear();
  const json checked = op(omc, {{"id", 2}, {"op", "check_model"}, {"model", "Good"}});
  INFO(checked.dump());
  REQUIRE(checked["ok"] == true);
  CHECK(checked["result"]["equations"] == 1);
  CHECK(checked["result"]["unknowns"] == 1);
  CHECK(checked["result"]["messages"].empty());

  const json flat = op(omc, {{"id", 3}, {"op", "instantiate_model"}, {"model", "Good"}});
  INFO(flat.dump());
  REQUIRE(flat["ok"] == true);
  CHECK(flat["result"]["text"].get<std::string>().find("class Good") != std::string::npos);
  CHECK(flat["result"]["text"].get<std::string>().find("der(x)") != std::string::npos);

  // A model omc rejects still answers, with its message in the result and on the log.
  REQUIRE(op(omc, {{"id", 4}, {"op", "omc"}, {"expr", "loadFile(" + mo::quote(bad.string()) + ")"}})["ok"] == true);
  logged.clear();
  const json broken = op(omc, {{"id", 5}, {"op", "check_model"}, {"model", "Bad"}});
  INFO(broken.dump());
  REQUIRE(broken["ok"] == true);
  CHECK(broken["result"]["equations"] == 0);
  REQUIRE_FALSE(broken["result"]["messages"].empty());
  CHECK(broken["result"]["messages"][0]["level"] == "error");
  CHECK(broken["result"]["messages"][0]["file"].get<std::string>().find("Bad.mo") != std::string::npos);
  CHECK(broken["result"]["messages"][0]["line"] == 4);
  REQUIRE_FALSE(logged.empty());
  CHECK(logged.front().level == "error");
  CHECK(logged.front().file.find("Bad.mo") != std::string::npos);

  // The client's read is the messages the last operation left, and it clears them.
  const json again = op(omc, {{"id", 6}, {"op", "omc"}, {"expr", "getErrorString()"}});
  CHECK(again["result"].get<std::string>().find("Bad.mo") != std::string::npos);
  CHECK(op(omc, {{"id", 7}, {"op", "omc"}, {"expr", "getErrorString()"}})["result"] == "");

  fs::remove_all(dir);
}

TEST_CASE("the allow-list has what the canvas reads and edits, and nothing that writes elsewhere", "[squero]") {
  CHECK(mo::allowed(R"(getComponentModifierNames(M, "R1"))"));
  CHECK(mo::allowed("getNthConnectionAnnotation(M, 1)"));
  CHECK(mo::allowed("getClassRestriction(Modelica.Electrical.Analog.Basic.Resistor)"));
  CHECK(mo::allowed("setComponentModifierValue(M, R1.R, $Code(=10))"));
  CHECK(mo::allowed("updateComponent(R1, A.B, M, annotate=Placement(visible=true, transformation=transformation(origin={1, 2}, "
                    "extent={{-10, -10}, {10, 10}}, rotation=90)))"));
  CHECK(mo::allowed("setParameterValue(M, k, 3)"));
  // saving, simulating and the icon annotation are squero/0 ops of their own, confined to the user's model
  CHECK_FALSE(mo::allowed("save(M)"));
  CHECK_FALSE(mo::allowed("simulate(M, stopTime=1)"));
  CHECK_FALSE(mo::allowed("addClassAnnotation(M, annotate=Icon())"));
  CHECK_FALSE(mo::allowed("writeFile(\"/tmp/x\", \"y\")"));
}

TEST_CASE("visual_shapes reads omc's _visual.xml", "[squero]") {
  const std::string xml = R"(<visualization>
  <shape>
      <ident>body.shape</ident>
      <type>box</type>
      <T><exp>1.0</exp><exp>0.0</exp><exp>0.0</exp><exp>0.0</exp><exp>1.0</exp><exp>0.0</exp><exp>0.0</exp><exp>0.0</exp><cref>body.R.T[3,3]</cref></T>
      <r><cref>body.r[1]</cref><exp>0.0</exp><exp>-1.5</exp></r>
      <r_shape><exp>0.0</exp><exp>0.0</exp><exp>0.0</exp></r_shape>
      <lengthDir><exp>1.0</exp><exp>0.0</exp><exp>0.0</exp></lengthDir>
      <widthDir><exp>0.0</exp><exp>1.0</exp><exp>0.0</exp></widthDir>
      <length><cref>body.length</cref></length>
      <width><exp>0.1</exp></width>
      <height><exp>0.1</exp></height>
      <extra><exp>0.0</exp></extra>
      <color><exp>255.0</exp><exp>0.0</exp><exp>0.0</exp></color>
  </shape>
</visualization>)";
  // a variable resolves to its series name, a parameter to its value
  const json shapes = squero::visual_shapes(xml, [](const std::string& c) { return c == "body.length" ? json(0.5) : json(c); });
  REQUIRE(shapes.size() == 1);
  CHECK(shapes[0]["ident"] == "body.shape");
  CHECK(shapes[0]["type"] == "box");
  CHECK(shapes[0]["T"].size() == 9);
  CHECK(shapes[0]["T"][8] == "body.R.T[3,3]");
  CHECK(shapes[0]["r"] == json({"body.r[1]", 0.0, -1.5}));
  CHECK(shapes[0]["length"] == 0.5);
  CHECK(shapes[0]["color"][0] == 255.0);
}

TEST_CASE("with no model named, the Studio opens its own folder", "[squero]") {
  const auto dir = std::filesystem::temp_directory_path() / "squero-default-workspace-test";
  std::filesystem::remove_all(dir);
  // A fresh folder is created and opens a new Untitled model.
  CHECK(squero::default_file(dir) == dir / "Untitled.mo");
  CHECK(std::filesystem::is_directory(dir));
  // Otherwise the first model in it, never its package.mo.
  std::ofstream(dir / "package.mo") << "package P\nend P;\n";
  std::ofstream(dir / "b.mo") << "model b\nend b;\n";
  std::ofstream(dir / "a.mo") << "model a\nend a;\n";
  CHECK(squero::default_file(dir) == dir / "a.mo");
  std::filesystem::remove_all(dir);
  CHECK(squero::default_workspace().filename() == (std::getenv("SQUERO_OM_WORKSPACE") ? squero::default_workspace().filename() : "om"));
}
