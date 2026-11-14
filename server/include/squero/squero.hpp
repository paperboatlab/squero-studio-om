#pragma once

// Squero Studio OM's server (#160 contracts §5): protocol squero/0 over WebSocket, separate
// from paperboat/0. One omc for the process, shared by every client:
//
//   client → {"id": 1, "op": "omc", "expr": "getClassNames()"}
//   server → {"id": 1, "ok": true, "result": "{Modelica}"} | {"id": 1, "ok": false, "error": "…"}
//   server → {"op": "status", "state": "ready" | "checking" | "failed", "version": "…"}  (on
//            connect and on every change)
//
// Expressions pass only through allowed() (no system(), no runScript). Plain HTTP on the
// same port serves the built Studio app from `app` when given.
//
// The other ops are Studio's own, written here in C++ (Wave 3). Each is {id, op, …} →
// {id, ok: true, result: {…}} | {id, ok: false, error}:
//
//   open      {}                                → {file, model}      the model the server was started on
//   workspace {file}                            → {root, file, package, entries:[{name, path, kind, depth}]}
//                                                  the model's folder as a tree of .mo classes and packages
//   create_class {file, name, kind, within?, base?}
//                                              → {file, model}      write a class file; append package.order
//                                                  when it lands in a package
//   new_package {parent, name, within?}        → {path, name}      make a package folder with package.mo/order
//   duplicate_class {file, name, newName, within?}
//                                              → {file, model}      copy a class file under a new name
//   delete_class {file, name}                  → {}                 remove the file and its package.order entry
//   delete_package {dir, root}                 → {dir}              remove a package folder inside the workspace
//   move_class {file, targetDir, within?}      → {file, model}      move a class to another package, rewrite `within`
//   read_file {file}                           → {text}             the .mo file's own text, for the editor
//   config    {}                                → {path, text|null}  the user's squero.toml (shared with MX), null when absent
//   config_write {text}                        → {path}             write it, making the folder
//   save      {model}                           → {file}             `save(model)`: the model's own .mo
//   icon      {class}                           → {icon, connectors, notice}
//                                                  the imported look of a class (`paperboat modelica icon`)
//   set_icon  {model, icon}                     → {}                 IconDef → Icon(graphics) annotation;
//                                                  the inverse of the importer, `g` and size only
//   generate  {source, model, out, dry?, path?, fmu_solver?}
//                                              → {model, path, reason, out, notices, files?}
//                                                  pushes {op: "progress", id, stage, fraction} first, stage
//                                                  check|flatten|generate|build; `files` (relative path →
//                                                  text) only when `dry`, which writes nothing
//   simulate  {model, stop, vars?, method?, tolerance?, intervals?, start?}
//                                               → {t, series, names, shapes?}  a local check run with omc,
//                                                  method dassl, tolerance 1e-6, 500 intervals from 0 unless given; series for `vars` (all when omitted),
//                                                  `names` every variable in the result; `shapes` (no
//                                                  `vars`) the MultiBody shapes, see visual_shapes()
//
// set_icon and save refuse the Modelica Standard Library: Studio edits only the user's file.

#include <filesystem>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "squero/omc.hpp"

namespace squero {

// What a handler may do besides answer: push a message to its client (progress), and which .mo
// the server was started on.
struct Session {
  std::function<void(const std::string&)> push = {};
  std::filesystem::path file = {};
};

// One squero/0 message handled; returns the reply (empty for a message that wants none).
std::string handle(Omc& omc, const std::string& message, const Session& session = {});

// omc's <model>_visual.xml (-d=visxml) as [{ident, type, T[9], r[3], r_shape[3], lengthDir[3],
// widthDir[3], length, width, height, extra, color[3]}]. Each value is a number, or the result
// variable that drives it as `resolve` says (a cref → its series name, or a number when constant).
nlohmann::json visual_shapes(const std::string& xml, const std::function<nlohmann::json(const std::string&)>& resolve);

// The Studio's own folder: $SQUERO_OM_WORKSPACE, else ~/Documents/paperboat/squero/om.
std::filesystem::path default_workspace();

// What opens when no model is named: the first .mo in `dir` (not package.mo), else dir/Untitled.mo.
// Creates `dir`; the model itself is created by `open` when it doesn't exist.
std::filesystem::path default_file(const std::filesystem::path& dir = default_workspace());

// Serves until killed. Returns 1 when the port can't be taken. port 0 = first free of 59201..59299. `file` is the .mo to open
// (created as a stub model when it doesn't exist); empty = default_file().
int serve_squero(int port, const std::filesystem::path& app = {}, Omc::Options options = {},
                 const std::filesystem::path& file = {});

}  // namespace squero
