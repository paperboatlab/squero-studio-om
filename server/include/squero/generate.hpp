#pragma once

// A Modelica model as a Paperboat station (#160 contracts §6): `paperboat modelica generate`
// and Studio's "Export station" both call generate().
//
//   <model>-cpp/
//     station.toml descriptor.toml README.md   written once: yours to edit afterwards
//     CMakeLists.txt main.cpp domain/…         rewritten on every generate
//     modelica/<Model>.mo                      the source, for the round trip
//     fmu/<Model>.fmu                          the FMU path only
//
// Path `ode`: the backend DAE (dumpXMLDAE) becomes a paperboat::Model whose equations are
// plain C++, linking only paperboat::core. Path `fmu`: omc's co-simulation FMU behind a
// StationObject adapter. `auto` takes ode unless the model has events, a torn or implicit
// system, or calls a function the C++ can't (externals, tables), and says why.

#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "squero/omc.hpp"

namespace squero {

struct GenerateError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct GenerateOptions {
  std::filesystem::path source;  // the .mo file
  std::string model = {};        // its qualified class; "" = the one class the file has
  std::filesystem::path out = {};  // "" = ./<model>-cpp
  std::string path = "auto";     // "auto", "ode" or "fmu"
  // Studio's preview (squero/0 `generate` with dry): nothing touches the disk. The files that
  // would be written come back in GenerateResult::files; the fmu path builds nothing, so no files.
  bool dry = false;
  // Studio: generate the class as loaded in omc (unsaved edits included), never reloading `source`;
  // a loadFile would build the stale file and wipe the edits from the session.
  bool live = false;
  // The solver inside an FMU (OpenModelica's CS FMUs have two): "cvode" or "euler".
  std::string fmu_solver = "cvode";
  // Called as the stages go by: "check", "flatten", "generate", "build", fraction 0..1.
  std::function<void(std::string_view stage, double fraction)> progress = {};
};

struct GenerateResult {
  std::string model;   // qualified class
  std::string name;    // its station name, e.g. "dc-motor"
  std::filesystem::path out;
  std::string path;    // "ode" or "fmu"
  std::string reason;  // why that path
  std::vector<std::string> notices;  // what was simplified or assumed
  std::vector<std::filesystem::path> written;
  std::map<std::string, std::string> files;  // dry only: relative path → text
  bool dry = false;
};

GenerateResult generate(Omc& omc, const GenerateOptions& options);

struct CheckResult {
  bool ok = false;
  std::string model;
  std::string message;  // omc's checkModel text, plus its errors when not ok
};
CheckResult check(Omc& omc, const std::filesystem::path& source, const std::string& model = {});

// --- the pieces, for tests ---

// "DCMotor" → "dc-motor"; "Modelica.Mechanics.X" → "x".
std::string station_name(std::string_view model);

// A Modelica expression as omc prints it (backend DAE) → C++. `name(var, der)` spells a
// variable (der: its derivative). Throws GenerateError naming what it can't translate.
std::string to_cpp(std::string_view expr, const std::function<std::string(std::string_view name, bool der)>& name);

}  // namespace squero
