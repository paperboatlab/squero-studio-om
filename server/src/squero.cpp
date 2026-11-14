// The transport for squero/0: the only file in engine/modelica that knows sockets.

#include "squero/squero.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define NOMINMAX  // windows.h's min/max macros would break std::max
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>  // before anything pulls in windows.h
#include <ws2tcpip.h>

#include <array>
#include <cstdio>

#include "squero/win32.hpp"
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <nlohmann/json.hpp>

#include "squero/generate.hpp"
#include "squero/import.hpp"

namespace squero {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

#ifdef _WIN32
using pid_t = int;
using socklen_t = int;
void close_socket(SOCKET s) { closesocket(s); }
#else
using SOCKET = int;
void close_socket(int fd) { close(fd); }
#endif

// SIGTERM/SIGINT: a flag the serve loop polls, then stops omc. A handler cannot take locks or
// talk to omc itself, and ixwebsocket's own handlers shouldn't decide when omc goes.
std::atomic<bool> stop_requested{false};
extern "C" void request_stop(int) { stop_requested.store(true, std::memory_order_relaxed); }

std::string status(OmcState state, const std::string& version, const std::string& mode) {
  return json{{"op", "status"}, {"state", name(state)}, {"version", version}, {"mode", mode}}.dump();
}

ix::HttpResponsePtr respond(int code, const std::string& type, std::string body) {
  ix::WebSocketHttpHeaders headers;
  headers["Content-Type"] = type;
  headers["Cache-Control"] = "no-cache";
  return std::make_shared<ix::HttpResponse>(code, code == 200 ? "OK" : "Not Found", ix::HttpErrorCode::Ok, headers,
                                            std::move(body));
}

// The built app's files, only from inside `app`; `/` is index.html.
ix::HttpResponsePtr http(const fs::path& app, const ix::HttpRequestPtr& request) {
  if (app.empty()) return respond(404, "text/plain", "squero/0: connect over WebSocket\n");
  std::string path = request->uri.substr(0, request->uri.find('?'));
  if (path == "/") path = "/index.html";
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(app, ec);
  const fs::path file = fs::weakly_canonical(app / path.substr(1), ec);
  const auto inside = file.lexically_relative(root);
  if (ec || inside.empty() || *inside.begin() == ".." || !fs::is_regular_file(file))
    return respond(404, "text/plain", "not found\n");
  static const std::map<std::string, std::string> types = {
      {".html", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
      {".css", "text/css; charset=utf-8"}, {".svg", "image/svg+xml"}, {".json", "application/json"},
      {".woff2", "font/woff2"}, {".png", "image/png"}};
  const auto type = types.find(file.extension().string());
  std::ifstream in(file, std::ios::binary);
  std::stringstream body;
  body << in.rdbuf();
  return respond(200, type == types.end() ? "application/octet-stream" : type->second, body.str());
}


// --- Studio's own ops ---

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  std::size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  return s.substr(a);
}

std::string text_of(const std::string& omc_answer) { return parse_value(omc_answer).atom; }

bool is_msl(const std::string& model) { return model == "Modelica" || model.starts_with("Modelica."); }

// A user model, never the library: Studio edits only the user's file.
std::string user_model(const json& in) {
  std::string model = in.value("model", "");
  if (model.empty() || is_msl(model) || model.find_first_of(" ;(){}\"") != std::string::npos)
    throw std::runtime_error("not a model Studio may edit: " + model);
  return model;
}

void ensure_library(Omc& omc, const std::string& cls) {
  const std::string top = cls.substr(0, cls.find('.'));
  for (const auto& v : parse_value(omc.call("getClassNames()")).items)
    if (v.atom == top) return;
  if (top == "Modelica") omc.call("loadModel(Modelica)");
}

// A class inside a package needs its package loaded first: omc's loadFile on a file with
// `within Pkg.Sub` fails when Pkg is not there. The outermost package.mo loads the whole package,
// subpackages included when package.order names them; loading a subpackage's package.mo directly
// would be read as a new top-level package ("expected within ;").
void load_packages(Omc& omc, const fs::path& file, bool force = false) {
  fs::path top;
  for (fs::path dir = file.parent_path(); fs::exists(dir / "package.mo"); dir = dir.parent_path()) {
    top = dir / "package.mo";
    if (dir.parent_path() == dir) break;
  }
  if (top.empty()) return;
  static std::mutex mutex;
  static std::set<std::string> loaded;
  const std::string key = fs::absolute(top).string();
  {
    std::lock_guard lock(mutex);
    if (!force && loaded.contains(key)) return;
    if (force) loaded.erase(key);
  }
  if (omc.call("loadFile(" + quote(key) + ")") != "true")
    throw std::runtime_error("cannot load package " + top.parent_path().string() + ": " + omc.errors());
  {
    std::lock_guard lock(mutex);
    loaded.insert(key);
  }
}

// The class in `file`: search the tree of loaded user classes, not just the top level, because a
// class inside a package (`Pkg.Sub.B`) is not in getClassNames().
bool find_class(Omc& omc, const std::string& cls, const fs::path& want, std::string& found, int depth) {
  if (depth > 8) return false;
  std::error_code ec;
  const std::string source = text_of(omc.call("getSourceFile(" + cls + ")"));
  if (!source.empty() && fs::weakly_canonical(source, ec) == want) {
    found = cls;
    return true;
  }
  for (const auto& v : parse_value(omc.call("getClassNames(" + cls + ")")).items)
    if (find_class(omc, cls + "." + v.atom, want, found, depth + 1)) return true;
  return false;
}

// The class `file` defines, once loaded.
std::string load_file(Omc& omc, const fs::path& file) {
  ensure_library(omc, "Modelica");
  load_packages(omc, fs::absolute(file));
  if (omc.call("loadFile(" + quote(fs::absolute(file).string()) + ")") != "true") {
    // omc may have restarted since the package was first loaded; reload it once, then the class.
    load_packages(omc, fs::absolute(file), /*force=*/true);
    if (omc.call("loadFile(" + quote(fs::absolute(file).string()) + ")") != "true")
      throw std::runtime_error("cannot load " + file.string() + ": " + omc.errors());
  }
  std::error_code ec;
  const fs::path want = fs::weakly_canonical(file, ec);
  std::string found;
  for (const auto& v : parse_value(omc.call("getClassNames()")).items) {
    if (v.atom == "Modelica") continue;
    if (find_class(omc, v.atom, want, found, 0)) return found;
  }
  throw std::runtime_error("no class in " + file.string());
}

json op_open(Omc& omc, const json& in, const Session& session) {
  fs::path file;
  if (in.contains("file") && in["file"].is_string() && !in["file"].get<std::string>().empty()) {
    file = in["file"].get<std::string>();
  } else {
    file = session.file.empty() ? fs::path("Untitled.mo") : session.file;
  }
  if (in.contains("content") && in["content"].is_string()) {
    std::ofstream(file) << in["content"].get<std::string>();
  } else if (!fs::exists(file)) {
    const std::string name = file.stem().string();
    std::ofstream(file) << "model " << name << "\nend " << name << ";\n";
  }
  return {{"file", fs::absolute(file).string()}, {"model", load_file(omc, file)}};
}

// The folder the open model lives in, for the Files rail: every package (a folder with a
// package.mo) down to its classes, or, for a model outside a package, the .mo files beside it.
// A single model in a plain folder is a workspace of one, which is the quick path.
// The kind a .mo declares, for the rail's glyph: skip comments, `within` and a leading
// partial/expandable/operator/pure, then take the class keyword.
std::string class_kind(const fs::path& file) {
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) continue;
    line.erase(0, first);
    if (line.starts_with("within") || line.starts_with("//") || line.starts_with("/*") || line.starts_with("*")) continue;
    std::istringstream words(line);
    std::string word;
    words >> word;
    while (word == "partial" || word == "expandable" || word == "operator" || word == "pure" || word == "impure") words >> word;
    if (word == "model" || word == "block" || word == "connector" || word == "record" || word == "function" || word == "package") return word;
    return "class";
  }
  return "class";
}

json op_workspace(const json& in) {
  const std::string target = in.value("file", "");
  if (target.empty()) throw std::runtime_error("workspace needs a file");
  const fs::path file = fs::absolute(target);
  fs::path root = file.parent_path();
  while (fs::exists(root / "package.mo") && root.parent_path() != root) root = root.parent_path();

  const std::set<std::string> skip = {".git", "build", "dist", "node_modules", ".cache", "__pycache__"};
  json entries = json::array();
  std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth) {
    if (depth > 6 || entries.size() >= 500) return;
    std::vector<fs::path> children;
    for (const auto& entry : fs::directory_iterator(dir)) children.push_back(entry.path());
    std::sort(children.begin(), children.end());
    for (const fs::path& path : children) {
      const std::string name = path.filename().string();
      if (skip.contains(name)) continue;
      if (fs::is_directory(path)) {
        if (!fs::exists(path / "package.mo")) continue;  // only packages make the tree
        entries.push_back({{"name", name}, {"path", path.string()}, {"kind", "package"}, {"depth", depth}});
        walk(path, depth + 1);
      } else if (path.extension() == ".mo" && name != "package.mo") {
        entries.push_back({{"name", path.stem().string()}, {"path", path.string()}, {"kind", "class"}, {"classKind", class_kind(path)}, {"depth", depth}});
      }
    }
  };
  walk(root, 0);
  return {{"root", root.string()},
          {"file", file.string()},
          {"package", fs::exists(file.parent_path() / "package.mo")},
          {"entries", entries}};
}

// `git -C <root> <args>`, run without a shell: its stdout, or nullopt when git is missing, the
// folder is no repository, or the call fails.
std::optional<std::string> run_git(const fs::path& root, const std::vector<std::string>& args) {
  std::vector<std::string> argv = {"git", "-C", root.string()};
  argv.insert(argv.end(), args.begin(), args.end());
  std::string out;
#ifdef _WIN32
  void* pipe = nullptr;
  const int pid = win32::spawn_piped(argv, &pipe);
  if (pid < 0) return std::nullopt;
  std::string chunk;
  while (win32::read_pipe(pipe, chunk)) out += chunk;
  win32::close_pipe(pipe);
  if (win32::wait(pid) != 0) return std::nullopt;
#else
  int fds[2];
  if (::pipe(fds) != 0) return std::nullopt;
  const pid_t pid = fork();
  if (pid < 0) { close(fds[0]); close(fds[1]); return std::nullopt; }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    const int nul = open("/dev/null", O_WRONLY);
    if (nul >= 0) dup2(nul, STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    setenv("GIT_OPTIONAL_LOCKS", "0", 1);  // a status read takes no lock
    std::vector<char*> cargv;
    for (std::string& a : argv) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    execvp("git", cargv.data());
    _exit(127);
  }
  close(fds[1]);
  char buf[8192];
  for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR);)
    if (n > 0) out.append(buf, static_cast<size_t>(n));
  close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
#endif
  return out;
}

std::vector<std::string> nul_split(const std::string& z) {
  std::vector<std::string> out;
  std::string::size_type at = 0;
  for (auto end = z.find('\0'); end != std::string::npos; at = end + 1, end = z.find('\0', at))
    if (end > at) out.push_back(z.substr(at, end - at));
  if (at < z.size()) out.push_back(z.substr(at));
  return out;
}

// The git gutter (the same shape as MX's /__mx/workspace/git): HEAD's copy of each tracked .mo
// under the workspace root (`head`), the .mo files HEAD lacks (`untracked`: untracked, or staged
// but not committed), and the ones that differ from HEAD on disk (`modified`). Paths are absolute,
// as the workspace entries are. null outside a git repository.
json op_git(const json& in) {
  const std::string root_name = in.value("root", "");
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(fs::absolute(root_name), ec);
  if (root_name.empty() || ec || !fs::is_directory(root, ec)) throw std::runtime_error("git needs the workspace root");
  const auto tracked = run_git(root, {"ls-files", "-z", "--", "*.mo"});
  if (!tracked) return nullptr;
  json head = json::object();
  json untracked = json::array();
  const auto others = run_git(root, {"ls-files", "-z", "--others", "--exclude-standard", "--", "*.mo"});
  for (const std::string& rel : nul_split(others.value_or(""))) untracked.push_back((root / fs::path(rel)).lexically_normal().string());
  // ponytail: one `git show` per tracked file; batch through `git cat-file --batch` if packages grow to hundreds of classes.
  for (const std::string& rel : nul_split(*tracked)) {
    const std::string path = (root / fs::path(rel)).lexically_normal().string();
    if (const auto text = run_git(root, {"show", "HEAD:./" + rel})) head[path] = *text;
    else untracked.push_back(path);
  }
  json modified = json::array();
  const auto changed = run_git(root, {"diff", "--relative", "--name-only", "-z", "HEAD", "--", "*.mo"});
  for (const std::string& rel : nul_split(changed.value_or(""))) modified.push_back((root / fs::path(rel)).lexically_normal().string());
  return {{"head", head}, {"untracked", untracked}, {"modified", modified}};
}

// A Modelica identifier, or a dotted qualified name (Pkg.Sub).
bool identifier(std::string_view text) {
  if (text.empty() || !(std::isalpha(static_cast<unsigned char>(text.front())) || text.front() == '_')) return false;
  return std::all_of(text.begin() + 1, text.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

bool qualified_name(std::string_view text) {
  if (text.empty()) return true;
  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t dot = text.find('.', at);
    if (!identifier(text.substr(at, dot == std::string_view::npos ? text.size() - at : dot - at))) return false;
    if (dot == std::string_view::npos) break;
    at = dot + 1;
  }
  return true;
}

// Write a new class beside its package and keep package.order in step. Nothing is loaded here;
// the client opens the file next, so the same path as File > Open reads it back.
json op_create_class(const json& in) {
  const std::string name = in.value("name", "");
  const std::string kind = in.value("kind", "model");
  const std::string within = in.value("within", "");
  const std::string base = in.value("base", "");
  const std::string target = in.value("file", "");
  if (!identifier(name)) throw std::runtime_error("a class name starts with a letter or _ and holds letters, digits and _");
  if (kind != "model" && kind != "block" && kind != "connector" && kind != "record" && kind != "function")
    throw std::runtime_error("kind is model, block, connector, record or function");
  if (!qualified_name(within)) throw std::runtime_error("not a package name: " + within);
  if (!base.empty() && !qualified_name(base)) throw std::runtime_error("not a base class: " + base);
  if (target.empty()) throw std::runtime_error("create_class needs a file");
  const fs::path file = fs::absolute(target);
  if (fs::exists(file)) throw std::runtime_error(file.string() + " already exists");
  if (!fs::is_directory(file.parent_path())) throw std::runtime_error("no folder " + file.parent_path().string());

  std::string text;
  if (!within.empty()) text += "within " + within + ";\n\n";
  text += kind + " " + name + "\n  \"A new " + kind + ".\"\n";
  if (!base.empty()) text += "  extends " + base + ";\n";
  if (kind == "model") text += "  Real x;\nequation\n  der(x) = -x;\n";
  else if (kind == "block") text += "  Modelica.Blocks.Interfaces.RealInput u;\n  Modelica.Blocks.Interfaces.RealOutput y;\nequation\n  y = u;\n";
  else if (kind == "connector") text += "  Real v;\n  flow Real i;\n";
  else if (kind == "record") text += "  Real x;\n";
  else text += "  input Real u;\n  output Real y;\nalgorithm\n  y := u;\n";
  text += "end " + name + ";\n";
  std::ofstream(file) << text;

  // package.order, when the file landed in a package: keep what is there and add the name.
  const fs::path dir = file.parent_path();
  if (fs::exists(dir / "package.mo")) {
    const fs::path order_file = dir / "package.order";
    std::vector<std::string> order;
    if (fs::exists(order_file)) {
      std::ifstream order_in(order_file);
      std::string line;
      while (std::getline(order_in, line)) {
        const std::size_t first = line.find_first_not_of(" \t\r");
        const std::size_t last = line.find_last_not_of(" \t\r");
        if (first != std::string::npos) order.push_back(line.substr(first, last - first + 1));
      }
    } else {
      for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".mo" && entry.path().stem() != "package") order.push_back(entry.path().stem().string());
      }
      std::sort(order.begin(), order.end());
    }
    if (std::find(order.begin(), order.end(), name) == order.end()) order.push_back(name);
    std::ofstream order_out(order_file);
    for (const auto& one : order) order_out << one << '\n';
  }

  return {{"file", file.string()}, {"model", within.empty() ? name : within + "." + name}};
}

// The name in a package's package.order, added or removed without touching the rest.
void order_add(const fs::path& dir, const std::string& name) {
  const fs::path order_file = dir / "package.order";
  std::vector<std::string> lines;
  if (fs::exists(order_file)) {
    std::ifstream in(order_file);
    for (std::string line; std::getline(in, line);) {
      const std::size_t first = line.find_first_not_of(" \t\r");
      if (first != std::string::npos) lines.push_back(line.substr(first, line.find_last_not_of(" \t\r") - first + 1));
    }
  }
  if (std::find(lines.begin(), lines.end(), name) == lines.end()) lines.push_back(name);
  std::ofstream out(order_file);
  for (const auto& line : lines) out << line << '\n';
}

void order_remove(const fs::path& dir, const std::string& name) {
  const fs::path order_file = dir / "package.order";
  if (!fs::exists(order_file)) return;
  std::ifstream in(order_file);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    const std::size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) continue;
    const std::string one = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    if (one != name) lines.push_back(one);
  }
  std::ofstream out(order_file);
  for (const auto& line : lines) out << line << '\n';
}

// Put `to` where `from` stood in a package.order (append it when `from` is not listed).
void order_rename(const fs::path& dir, const std::string& from, const std::string& to) {
  const fs::path order_file = dir / "package.order";
  std::vector<std::string> lines;
  bool found = false;
  if (fs::exists(order_file)) {
    std::ifstream in(order_file);
    for (std::string line; std::getline(in, line);) {
      const std::size_t first = line.find_first_not_of(" \t\r");
      if (first == std::string::npos) continue;
      std::string one = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
      if (one == from) { one = to; found = true; }
      lines.push_back(one);
    }
  }
  if (!found) lines.push_back(to);
  std::ofstream out(order_file);
  for (const auto& line : lines) out << line << '\n';
}

json op_new_package(const json& in) {
  const std::string name = in.value("name", "");
  const std::string parent = in.value("parent", "");
  const std::string within = in.value("within", "");
  if (!identifier(name)) throw std::runtime_error("a package name starts with a letter or _ and holds letters, digits and _");
  if (!qualified_name(within)) throw std::runtime_error("not a package name: " + within);
  if (parent.empty()) throw std::runtime_error("new_package needs a parent folder");
  const fs::path dir = fs::absolute(parent) / name;
  if (fs::exists(dir)) throw std::runtime_error(dir.string() + " already exists");
  fs::create_directories(dir);
  std::ofstream(dir / "package.mo") << (within.empty() ? "" : "within " + within + ";\n\n") << "package " << name
                                   << "\n  \"A new package.\"\nend " << name << ";\n";
  std::ofstream(dir / "package.order");
  return {{"path", dir.string()}, {"name", name}};
}

json op_duplicate_class(const json& in) {
  const std::string file_name = in.value("file", "");
  const std::string name = in.value("name", "");
  const std::string new_name = in.value("newName", "");
  const std::string within = in.value("within", "");
  if (!identifier(name) || !identifier(new_name)) throw std::runtime_error("both names must be Modelica identifiers");
  if (name == new_name) throw std::runtime_error("the duplicate needs a different name");
  if (file_name.empty()) throw std::runtime_error("duplicate_class needs a file");
  const fs::path file = fs::absolute(file_name);
  if (!fs::is_regular_file(file)) throw std::runtime_error("no file " + file.string());
  const fs::path target = file.parent_path() / (new_name + ".mo");
  if (fs::exists(target)) throw std::runtime_error(target.string() + " already exists");
  std::ifstream source(file);
  const std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
  const std::regex word("\\b" + name + "\\b");
  std::ofstream(target) << std::regex_replace(text, word, new_name);
  if (fs::exists(file.parent_path() / "package.mo")) order_add(file.parent_path(), new_name);
  return {{"file", target.string()}, {"model", within.empty() ? new_name : within + "." + new_name}};
}

// Rename a class: the file (Name.mo), the class name inside it (as duplicate does: every whole
// word), and its line in package.order, keeping its place. Other classes that use it are not edited.
json op_rename_class(const json& in) {
  const std::string file_name = in.value("file", "");
  const std::string name = in.value("name", "");
  const std::string new_name = in.value("newName", "");
  const std::string within = in.value("within", "");
  if (!identifier(name) || !identifier(new_name)) throw std::runtime_error("both names must be Modelica identifiers");
  if (name == new_name) throw std::runtime_error("the new name is the same");
  if (file_name.empty()) throw std::runtime_error("rename_class needs a file");
  const fs::path file = fs::absolute(file_name);
  if (!fs::is_regular_file(file) || file.extension() != ".mo") throw std::runtime_error("no .mo file " + file.string());
  const fs::path target = file.parent_path() / (new_name + ".mo");
  if (fs::exists(target)) throw std::runtime_error(target.string() + " already exists");
  std::ifstream source(file);
  const std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
  source.close();
  { std::ofstream(target) << std::regex_replace(text, std::regex("\\b" + name + "\\b"), new_name); }
  fs::remove(file);
  if (fs::exists(file.parent_path() / "package.mo")) order_rename(file.parent_path(), name, new_name);
  return {{"file", target.string()}, {"model", within.empty() ? new_name : within + "." + new_name}};
}

json op_delete_class(const json& in) {
  const std::string file_name = in.value("file", "");
  const std::string name = in.value("name", "");
  if (file_name.empty()) throw std::runtime_error("delete_class needs a file");
  const fs::path file = fs::absolute(file_name);
  if (file.extension() != ".mo") throw std::runtime_error("only .mo files can be deleted");
  if (!fs::exists(file)) throw std::runtime_error("no file " + file.string());
  fs::remove(file);
  if (!name.empty() && fs::exists(file.parent_path() / "package.mo")) order_remove(file.parent_path(), name);
  return json::object();
}

// Delete a package folder and everything under it. `root` is the workspace the UI shows, so a
// stray path (or the root itself) cannot take the whole workspace with it.
json op_delete_package(const json& in) {
  const std::string dir_name = in.value("dir", "");
  const std::string root_name = in.value("root", "");
  if (dir_name.empty()) throw std::runtime_error("delete_package needs a folder");
  std::error_code ec;
  const fs::path dir = fs::weakly_canonical(fs::absolute(dir_name), ec);
  const fs::path root = fs::weakly_canonical(fs::absolute(root_name), ec);
  if (ec) throw std::runtime_error("cannot resolve " + dir_name);
  if (dir == root) throw std::runtime_error("the workspace root cannot be deleted");
  if (!fs::is_directory(dir) || !fs::is_regular_file(dir / "package.mo"))
    throw std::runtime_error(dir.string() + " is not a Modelica package");
  auto a = dir.begin(), b = root.begin();
  for (; b != root.end(); ++a, ++b)
    if (a == dir.end() || *a != *b) throw std::runtime_error(dir.string() + " is outside " + root.string());
  fs::remove_all(dir);
  return json{{"dir", dir.string()}};
}

// The user's config file, squero.toml, shared with Squero Studio MX: $XDG_CONFIG_HOME/paperboat
// (else ~/.config/paperboat) on Linux and macOS, %APPDATA%\paperboat on Windows. The server only
// reads and writes the text; @paperboat/shared/editor parses it and patches single keys.
fs::path config_file() {
  const auto env = [](const char* name) { const char* v = std::getenv(name); return std::string(v ? v : ""); };
#ifdef _WIN32
  fs::path dir = env("APPDATA").empty() ? fs::path(env("USERPROFILE")) / "AppData" / "Roaming" : fs::path(env("APPDATA"));
#else
  fs::path dir = env("XDG_CONFIG_HOME").empty() ? fs::path(env("HOME")) / ".config" : fs::path(env("XDG_CONFIG_HOME"));
#endif
  return dir / "paperboat" / "squero.toml";
}

// Shown to the user, so ~ for the home folder as MX shows it.
std::string config_path() {
  const std::string file = config_file().string();
  const char* home = std::getenv("HOME");
  return home && *home && file.rfind(home, 0) == 0 ? "~" + file.substr(std::strlen(home)) : file;
}

json op_config() {
  const fs::path file = config_file();
  std::ifstream source(file);
  if (!source) return {{"path", config_path()}, {"text", nullptr}};
  return {{"path", config_path()}, {"text", std::string((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>())}};
}

json op_config_write(const json& in) {
  if (!in.contains("text") || !in["text"].is_string()) throw std::runtime_error("config_write needs text");
  const fs::path file = config_file();
  fs::create_directories(file.parent_path());
  std::ofstream out(file, std::ios::binary);
  out << in["text"].get<std::string>();
  if (!out) throw std::runtime_error("cannot write " + file.string());
  return {{"path", config_path()}};
}

json op_read_file(const json& in) {
  const std::string file_name = in.value("file", "");
  if (file_name.empty()) throw std::runtime_error("read_file needs a file");
  const fs::path file = fs::absolute(file_name);
  if (file.extension() != ".mo") throw std::runtime_error("only .mo files can be read");
  std::ifstream source(file);
  if (!source) throw std::runtime_error("no file " + file.string());
  return {{"text", std::string((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>())}};
}

// Move a class file into another package, rewriting its `within` and both package.order files.
json op_move_class(const json& in) {
  const std::string file_name = in.value("file", "");
  const std::string target_dir_name = in.value("targetDir", "");
  const std::string within = in.value("within", "");
  if (file_name.empty() || target_dir_name.empty()) throw std::runtime_error("move_class needs a file and a target folder");
  const fs::path file = fs::absolute(file_name);
  const fs::path target_dir = fs::absolute(target_dir_name);
  if (file.extension() != ".mo" || !fs::is_regular_file(file)) throw std::runtime_error("no .mo file " + file.string());
  if (!fs::is_directory(target_dir)) throw std::runtime_error("no folder " + target_dir.string());
  const fs::path target = target_dir / file.filename();
  if (target == file) return {{"file", file.string()}, {"model", within.empty() ? file.stem().string() : within + "." + file.stem().string()}};
  if (fs::exists(target)) throw std::runtime_error(target.string() + " already exists");
  std::ifstream source(file);
  std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
  source.close();  // Windows will not remove a file that is still open (below)
  static const std::regex within_line(R"(^\s*within\s+[^;]+;\s*\n?)");
  text = std::regex_replace(text, within_line, "", std::regex_constants::format_first_only);
  // Drop a blank line left at the very top by the within line.
  while (!text.empty() && text.front() == '\n') text.erase(0, 1);
  if (!within.empty()) text = "within " + within + ";\n\n" + text;
  std::ofstream(target) << text;
  fs::remove(file);
  const std::string name = file.stem().string();
  if (fs::exists(file.parent_path() / "package.mo")) order_remove(file.parent_path(), name);
  if (fs::exists(target_dir / "package.mo")) order_add(target_dir, name);
  return {{"file", target.string()}, {"model", within.empty() ? name : within + "." + name}};
}

json op_save(Omc& omc, const json& in) {
  const std::string model = user_model(in);
  std::string file = in.value("file", "");
  if (file.empty()) file = text_of(omc.call("getSourceFile(" + model + ")"));
  if (file.empty()) throw std::runtime_error(model + " has no file to save to");
  if (in.contains("content") && in["content"].is_string()) {
    std::ofstream(file) << in["content"].get<std::string>();
  } else {
    if (omc.call("save(" + model + ")") != "true") {
      const std::string listed = text_of(omc.call("list(" + model + ")"));
      if (!listed.empty()) {
        std::ofstream(file) << listed << "\n";
      } else {
        throw std::runtime_error("cannot save " + model + ": " + omc.errors());
      }
    }
  }
  return {{"file", file}};
}

json messages_json(const std::vector<OmcMessage>& messages) {
  json out = json::array();
  for (const auto& message : messages) {
    json one{{"level", message.level}, {"text", message.text}};
    if (!message.file.empty()) one["file"] = message.file;
    if (message.line > 0) one["line"] = message.line;
    if (message.column > 0) one["column"] = message.column;
    out.push_back(std::move(one));
  }
  return out;
}

// The balance from checkModel's prose: "Class M has 2 equation(s) and 2 variable(s)."
std::pair<int, int> balance_of(const std::string& text) {
  static const std::regex re(R"((\d+)\s+equation(?:s|\(s\))?\s+and\s+(\d+)\s+(?:variable|unknown)(?:s|\(s\))?)", std::regex::icase);
  std::smatch match;
  if (std::regex_search(text, match, re)) return {std::stoi(match[1]), std::stoi(match[2])};
  return {0, 0};
}

json op_check_model(Omc& omc, const json& in) {
  const std::string model = user_model(in);
  const std::string reply = text_of(omc.call("checkModel(" + model + ")"));
  const auto [equations, unknowns] = balance_of(reply);
  return {{"reply", reply}, {"equations", equations}, {"unknowns", unknowns}, {"messages", messages_json(omc.messages())}};
}

json op_instantiate_model(Omc& omc, const json& in) {
  const std::string model = user_model(in);
  const std::string text = text_of(omc.call("instantiateModel(" + model + ")", std::chrono::minutes(2)));
  return {{"model", model}, {"text", text}, {"messages", messages_json(omc.messages())}};
}

json op_get_messages(Omc& omc) {
  std::vector<OmcMessage> messages = omc.last_messages();
  if (messages.empty()) messages = omc.messages();
  return {{"messages", messages_json(messages)}};
}

json op_icon(Omc& omc, const json& in) {
  const std::string cls = in.value("class", "");
  if (cls.empty() || cls.find_first_of(" ;(){}\"") != std::string::npos) throw std::runtime_error("icon needs a class name");
  static std::mutex cache_mutex;
  static std::map<std::string, json> cache;  // library classes never change under Studio
  if (is_msl(cls)) {
    std::lock_guard lock(cache_mutex);
    if (const auto it = cache.find(cls); it != cache.end()) return it->second;
  }
  ensure_library(omc, cls);
  json out = import_class(omc, cls).to_json();
  if (is_msl(cls)) {
    std::lock_guard lock(cache_mutex);
    cache[cls] = out;
  }
  return out;
}

json op_set_icon(Omc& omc, const json& in) {
  const std::string model = user_model(in);
  if (!in.contains("icon") || !in["icon"].is_object()) throw std::runtime_error("set_icon needs an icon");
  const std::string icon = icon_annotation(in["icon"]);
  if (omc.call("addClassAnnotation(" + model + ", annotate=" + icon + ")") != "true")
    throw std::runtime_error("cannot set the icon: " + omc.errors());
  return json::object();
}

json op_generate(Omc& omc, const json& in, const Session& session) {
  GenerateOptions o{.source = in.value("source", ""), .model = in.value("model", ""), .out = in.value("out", ""),
                    .path = in.value("path", "auto"), .dry = in.value("dry", false), .live = true,
                    .fmu_solver = in.value("fmu_solver", "cvode")};
  if (o.source.empty()) throw std::runtime_error("generate needs a source .mo");
  if (!o.dry && o.out.empty()) throw std::runtime_error("generate needs an out folder");
  const json id = in.value("id", json());
  if (session.push)
    o.progress = [&](std::string_view stage, double fraction) {
      session.push(json{{"op", "progress"}, {"id", id}, {"stage", stage}, {"fraction", fraction}}.dump());
    };
  const GenerateResult r = generate(omc, o);
  json out{{"model", r.model}, {"path", r.path}, {"reason", r.reason}, {"out", r.out.string()}, {"notices", r.notices}};
  if (o.dry) out["files"] = r.files;
  return out;
}

// A free TCP port: bind to 0 and read back what the kernel chose.
int free_port() {
  const SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
  const bool valid = fd != INVALID_SOCKET;
#else
  const bool valid = fd >= 0;
#endif
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t n = sizeof a;
  const bool ok = valid && bind(fd, reinterpret_cast<sockaddr*>(&a), n) == 0 && getsockname(fd, reinterpret_cast<sockaddr*>(&a), &n) == 0;
  if (valid) close_socket(fd);
  if (!ok) throw std::runtime_error("no free port");
  return ntohs(a.sin_port);
}

std::string base64(const std::string& in) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < in.size(); i += 3) {
    const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) << 8 : 0) |
                       (i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0);
    out += t[v >> 18 & 63];
    out += t[v >> 12 & 63];
    out += i + 1 < in.size() ? t[v >> 6 & 63] : '=';
    out += i + 2 < in.size() ? t[v & 63] : '=';
  }
  return out;
}

// The paperboat console: $PAPERBOAT_BIN or the one on PATH.
std::string paperboat_bin() {
  const char* e = std::getenv("PAPERBOAT_BIN");
  if (e && *e) return e;
#ifdef _WIN32
  // the sibling checkout's build: <repo>/paperboat/engine/build/tools/<config>/paperboat.exe
  // (bin/paperboat is a bash script, which CreateProcess cannot run)
  const fs::path exe = win32::exe_path();
  for (fs::path d = exe.parent_path(); d.has_parent_path() && d != d.root_path(); d = d.parent_path()) {
    for (const char* config : {"Release", "RelWithDebInfo", "Debug", "."}) {
      const fs::path c = d.parent_path() / "paperboat" / "engine" / "build" / "tools" / config / "paperboat.exe";
      std::error_code missing;
      if (fs::is_regular_file(c, missing)) return fs::weakly_canonical(c, missing).string();
    }
  }
  return "paperboat";
#else
  // the sibling checkout: <repo>/squero-studio-om/server/build/squero-om -> <repo>/paperboat/bin/paperboat
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  for (fs::path d = exe.parent_path(); !ec && d.has_parent_path() && d != d.root_path(); d = d.parent_path()) {
    const fs::path c = d.parent_path() / "paperboat" / "bin" / "paperboat";
    std::error_code missing;  // not `ec`: a missing candidate sets it, which would end the walk
    if (fs::is_regular_file(c, missing)) return c.string();
  }
  return "paperboat";
#endif
}

// The installed paperboat's version, or "" when it can't be run. Cached: About asks once.
std::string paperboat_version() {
  static const std::string version = [] {
    std::string out;
#ifdef _WIN32
    // _popen runs `cmd /c <cmd>`, which strips one outer pair of quotes: hence the extra pair.
    const std::string cmd = "\"\"" + paperboat_bin() + "\" --version 2>NUL\"";
    if (FILE* p = _popen(cmd.c_str(), "r")) {
      char line[512];
      while (std::fgets(line, sizeof line, p)) out += line;
      _pclose(p);
    }
#else
    const std::string cmd = "\"" + paperboat_bin() + "\" --version 2>/dev/null";
    if (FILE* p = popen(cmd.c_str(), "r")) {
      char line[512];
      while (std::fgets(line, sizeof line, p)) out += line;
      pclose(p);
    }
#endif
    const auto first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const auto end = out.find_first_of("\r\n", first);
    const std::string text = out.substr(first, end == std::string::npos ? std::string::npos : end - first);
    const auto space = text.find(' ');
    return space == std::string::npos ? text : text.substr(space + 1);
  }();
  return version;
}

// Spawns paperboat in its own session, stdout and stderr to `log`; returns its pid.
pid_t spawn_paperboat(const std::vector<std::string>& args, const fs::path& log) {
  const std::string bin = paperboat_bin();
#ifdef _WIN32
  // its own job (setsid's session): serve_stop ends it and what it started
  std::vector<std::string> argv{bin};
  argv.insert(argv.end(), args.begin(), args.end());
  const int pid = win32::spawn(argv, {}, log, win32::Err::Same, false);
  if (pid < 0) throw std::runtime_error("cannot run " + bin + " (writing " + log.string() + ")");
  return pid;
#else
  std::vector<char*> argv{const_cast<char*>(bin.c_str())};
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) throw std::runtime_error("cannot write " + log.string());
  const pid_t pid = fork();
  if (pid == 0) {
    setsid();
    const int null = open("/dev/null", O_RDONLY);
    dup2(null, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    // nothing else of the server's: a backend that kept its listening socket would hold the
    // server's port after the server is gone
    close_range(3, ~0U, 0);
    execvp(bin.c_str(), argv.data());
    dprintf(2, "cannot run %s: %s\n", bin.c_str(), std::strerror(errno));
    _exit(127);
  }
  close(fd);
  if (pid < 0) throw std::runtime_error("fork failed");
  return pid;
#endif
}

// Runs paperboat to the end; its exit code (127: it could not be started).
int run_paperboat(const std::vector<std::string>& args, const fs::path& log) {
  const pid_t pid = spawn_paperboat(args, log);
#ifdef _WIN32
  return win32::wait(pid);
#else
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

// Runs a program with its output going to `log` and to `on_line` as it arrives, so a slow build
// shows why it is slow while it runs (a packed archive reports its progress too).
int run_process_streaming(const std::string& bin, const std::vector<std::string>& args, const fs::path& log,
                          const std::function<void(const std::string&)>& on_line) {
#ifdef _WIN32
  std::vector<std::string> command{bin};
  command.insert(command.end(), args.begin(), args.end());
  void* pipe = nullptr;
  const int pid = win32::spawn_piped(command, &pipe);
  if (pid < 0) return 127;  // could not be started, as exec failing in the child
  std::ofstream out(log, std::ios::binary | std::ios::trunc);
  std::string pending;
  for (std::string chunk; win32::read_pipe(pipe, chunk);) {
    out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    out.flush();
    pending += chunk;
    for (std::size_t at; (at = pending.find('\n')) != std::string::npos;) {
      std::string line = pending.substr(0, at);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      on_line(line);
      pending.erase(0, at + 1);
    }
  }
  win32::close_pipe(pipe);
  if (!pending.empty()) on_line(pending);
  return win32::wait(pid);
#else
  std::vector<char*> argv{const_cast<char*>(bin.c_str())};
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) throw std::runtime_error("cannot pipe paperboat's output");
  const pid_t pid = fork();
  if (pid == 0) {
    setsid();
    const int null = open("/dev/null", O_RDONLY);
    dup2(null, 0);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close_range(3, ~0U, 0);
    execvp(bin.c_str(), argv.data());
    dprintf(2, "cannot run %s: %s\n", bin.c_str(), std::strerror(errno));
    _exit(127);
  }
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    throw std::runtime_error("fork failed");
  }
  std::ofstream out(log, std::ios::binary | std::ios::trunc);
  std::string pending;
  std::array<char, 8192> buf{};
  for (ssize_t n; (n = read(fds[0], buf.data(), buf.size())) > 0;) {
    out.write(buf.data(), n);
    out.flush();
    pending.append(buf.data(), static_cast<std::size_t>(n));
    for (std::size_t at; (at = pending.find('\n')) != std::string::npos;) {
      on_line(pending.substr(0, at));
      pending.erase(0, at + 1);
    }
  }
  close(fds[0]);
  if (!pending.empty()) on_line(pending);
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

int run_paperboat_streaming(const std::vector<std::string>& args, const fs::path& log,
                            const std::function<void(const std::string&)>& on_line) {
  return run_process_streaming(paperboat_bin(), args, log, on_line);
}

// The prebuilt SDK for the checkout the paperboat binary came from, installing it once if it is
// missing or older than the engine library. Without it every backend rebuilds the engine and all
// its dependencies in its own build folder (minutes, and gigabytes).
void ensure_engine_sdk(const Session& session) {
  if (const char* given = std::getenv("PAPERBOAT_SDK_DIR"); given && *given) return;
  std::error_code ec;
  const fs::path bin = fs::canonical(paperboat_bin(), ec);
  if (ec || !fs::is_regular_file(bin)) return;
#ifdef _WIN32
  // <repo>/paperboat/engine/build/tools/Release/paperboat.exe -> <repo>/paperboat/engine/build
  fs::path engine;
  for (fs::path d = bin.parent_path(); d.has_parent_path() && d != d.root_path(); d = d.parent_path())
    if (d.filename() == "engine" && fs::is_regular_file(d / "CMakeLists.txt", ec)) {
      engine = d;
      break;
    }
  if (engine.empty()) return;
  const fs::path build = engine / "build";
  fs::path core;
  for (const char* config : {"Release", "RelWithDebInfo", "Debug"})
    if (fs::is_regular_file(build / "core" / config / "paperboat_core.lib", ec)) {
      core = build / "core" / config / "paperboat_core.lib";
      break;
    }
  if (core.empty()) return;
  const std::string config_name = core.parent_path().filename().string();
#else
  // <repo>/paperboat/bin/paperboat -> <repo>/paperboat/engine/build/dev
  const fs::path engine = bin.parent_path().parent_path() / "engine";
  fs::path build;
  for (const fs::path candidate : {engine / "build" / "dev", engine / "build"}) {
    if (fs::is_regular_file(candidate / "core" / "libpaperboat_core.a", ec) ||
        fs::is_regular_file(candidate / "libpaperboat_core.a", ec)) {
      build = candidate;
      break;
    }
  }
  if (build.empty()) return;
#endif
  const fs::path sdk = engine / "build" / "sdk";
  fs::path config;
  const auto find_config = [&] {
    for (const fs::path lib : {sdk / "lib64" / "cmake" / "Paperboat", sdk / "lib" / "cmake" / "Paperboat"})
      if (fs::is_regular_file(lib / "PaperboatConfig.cmake", ec)) return lib;
    return fs::path{};
  };
  config = find_config();
#ifndef _WIN32
  const fs::path core = fs::is_regular_file(build / "core" / "libpaperboat_core.a", ec) ? build / "core" / "libpaperboat_core.a"
                                                                                        : build / "libpaperboat_core.a";
#endif
  if (config.empty() || fs::last_write_time(config / "PaperboatConfig.cmake", ec) < fs::last_write_time(core, ec)) {
    const auto say = [&](const std::string& line) {
      if (session.push && !line.empty()) session.push(json{{"op", "log"}, {"level", "info"}, {"text", line}}.dump());
    };
    say(config.empty() ? "installing the engine SDK once (every backend build reuses it)…" : "refreshing the engine SDK…");
    const fs::path log = fs::temp_directory_path() / "ssom_sdk.log";
#ifdef _WIN32
    // a multi-config build: install the configuration the library was built in
    const int code = run_process_streaming(
        "cmake", {"--install", build.string(), "--config", config_name, "--prefix", sdk.string(), "--component", "dev"}, log, say);
#else
    const int code = run_process_streaming(
        "cmake", {"--install", build.string(), "--prefix", sdk.string(), "--component", "dev"}, log, say);
#endif
    if (code != 0) {
      say("engine SDK install failed; the backend will build the engine from source (slow)");
      return;
    }
    config = find_config();
    if (config.empty()) return;
  }
#ifdef _WIN32
  win32::set_env("PAPERBOAT_SDK_DIR", config.string());
#else
  setenv("PAPERBOAT_SDK_DIR", config.c_str(), 1);
#endif
}

// The last lines of a log, for an error the user can act on.
std::string log_tail(const fs::path& log, std::size_t lines = 12) {
  std::ifstream f(log);
  std::vector<std::string> all;
  for (std::string l; std::getline(f, l);) all.push_back(l);
  std::string out;
  for (std::size_t i = all.size() > lines ? all.size() - lines : 0; i < all.size(); ++i) out += all[i] + "\n";
  return out;
}

std::runtime_error paperboat_failed(const std::string& what, int code, const fs::path& log) {
  std::string msg = "paperboat " + what + " failed (exit " + std::to_string(code) + ")";
  if (code == 127) msg += "; is paperboat on PATH or PAPERBOAT_BIN set?";
  const std::string tail = log_tail(log);
  return std::runtime_error(msg + (tail.empty() ? "" : ":\n" + tail) + "full log: " + log.string());
}

bool listening(int port) {
  const SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
  const bool valid = fd != INVALID_SOCKET;
#else
  const bool valid = fd >= 0;
#endif
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  const bool ok = valid && connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
  if (valid) close_socket(fd);
  return ok;
}

// Backends started by "serve": port -> (`paperboat start` pid, its log).
std::mutex served_mutex;
std::map<int, std::pair<pid_t, fs::path>> served;

// "status": building (not listening yet), running, or exited (with the log's tail). `offset`
// asks for the log from that byte on: the console streams a long build instead of re-reading it.
json serve_status(int port, std::size_t offset) {
  std::lock_guard lock(served_mutex);
  const auto it = served.find(port);
  if (it == served.end()) throw std::runtime_error("no backend started on port " + std::to_string(port));
  const auto [pid, log] = it->second;
  std::string state = listening(port) ? "running" : "building";
#ifdef _WIN32
  if (!win32::running(pid)) {
    win32::wait(pid);
#else
  int st = 0;
  if (waitpid(pid, &st, WNOHANG) == pid) {
#endif
    state = "exited";
    served.erase(it);
  }
  std::error_code ec;
  const auto size = fs::file_size(log, ec);
  std::string chunk;
  std::size_t at = offset;
  if (!ec) {
    // the first call (offset 0) shows the last 64 KiB, not a whole old log
    if (at == 0 && size > 64 * 1024) at = size - 64 * 1024;
    if (at > size) at = 0;
    if (at < size) {
      std::ifstream f(log, std::ios::binary);
      f.seekg(static_cast<std::streamoff>(at));
      chunk.resize(static_cast<std::size_t>(size - at));
      f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      chunk.resize(static_cast<std::size_t>(f.gcount()));
      at += chunk.size();
    }
  }
  return json{{"port", port}, {"name", log.stem().string()}, {"state", state}, {"log", chunk}, {"offset", at}, {"tail", log_tail(log)}};
}

// "stop": `paperboat stop <port>` once it listens, then the rest of its session (a build in flight).
json serve_stop(int port) {
  const fs::path log = fs::temp_directory_path() / "ssom_stop.log";
  if (listening(port))
    if (const int code = run_paperboat({"stop", std::to_string(port)}, log); code != 0)
      throw paperboat_failed("stop " + std::to_string(port), code, log);
  std::lock_guard lock(served_mutex);
  if (const auto it = served.find(port); it != served.end()) {
#ifdef _WIN32
    win32::kill_tree(it->second.first);  // its job: the build or backend it started too
    win32::wait(it->second.first);
#else
    kill(-it->second.first, SIGTERM);
    waitpid(it->second.first, nullptr, 0);
#endif
    served.erase(it);
  }
  return json{{"port", port}, {"state", "stopped"}};
}

// Export without a folder of the user's, into <tmp>/ssom_<model>: mode "ppk" packs it and returns
// the archive (base64); "serve" starts it on the given port if free, else a free one and returns at once (poll "status": the
// first start builds the engine, minutes); "stop" ends one.
json op_export(Omc& omc, const json& in, const Session& session) {
  const std::string mode = in.value("mode", "ppk");
  if (mode == "status") return serve_status(in.value("port", 0), in.value("offset", 0));
  if (mode == "stop") return serve_stop(in.value("port", 0));
  if (mode != "ppk" && mode != "serve") throw std::runtime_error("export mode is ppk, serve, status or stop");
  const std::string model = user_model(in);
  const std::string restriction = text_of(omc.call("getClassRestriction(" + model + ")"));
  if (restriction != "model" && restriction != "block")
    throw std::runtime_error("only a model or a block can be exported to a Paperboat backend");
  std::string stem = model;
  stem = stem.substr(stem.rfind('.') == std::string::npos ? 0 : stem.rfind('.') + 1);
  for (auto& c : stem) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  const fs::path dir = fs::temp_directory_path() / ("ssom_" + stem);
  if (mode == "serve") ensure_engine_sdk(session);
  json g = in;
  g["out"] = dir.string();
  g["dry"] = false;
  json out = op_generate(omc, g, session);
  if (session.push) session.push(json{{"op", "progress"}, {"id", in.value("id", json())}, {"stage", "build"}, {"fraction", 0.8}}.dump());
  const fs::path log = dir.string() + ".log";
  if (mode == "ppk") {
    const fs::path ppk = dir.string() + ".ppk";
    // pack verifies first, and the catalog check builds a C++ backend: minutes on a cold build/
    const auto say = [&](const std::string& line) {
      if (session.push && !line.empty()) session.push(json{{"op", "log"}, {"level", "info"}, {"text", line}}.dump());
    };
    const int code = run_paperboat_streaming({"pack", dir.string(), "--out", ppk.string(), "--json-stream"}, log, [&](const std::string& line) {
      // its JSON-stream lines are progress, not prose: show the readable milestones only
      try {
        const json e = json::parse(line);
        if (e.value("event", "") == "progress") {
          const int files = e.value("files", 0), total = e.value("total", 0);
          if (total > 0 && (files == total || files % std::max(1, total / 10) == 0)) say("packing " + std::to_string(files) + "/" + std::to_string(total) + " files…");
        } else if (e.value("event", "") == "result") {
          if (e.value("ok", false))
            say("packed " + e.value("name", std::string("backend")) + " · " + std::to_string(e.value("files", 0)) + " files");
        } else {
          say(line);
        }
      } catch (const std::exception&) {
        say(line);
      }
    });
    if (code != 0) throw paperboat_failed("pack", code, log);
    std::ifstream f(ppk, std::ios::binary);
    out["file"] = "ssom_" + stem + ".ppk";
    out["ppk"] = base64(std::string(std::istreambuf_iterator<char>(f), {}));
  } else {
    // a restart asks for the port it just left, so a paperboat tab on it reconnects
    const int want = in.value("port", 0);
    const int port = want > 0 && !listening(want) ? want : free_port();
    const pid_t pid = spawn_paperboat({"start", dir.string(), "--port", std::to_string(port), "--verbose", "--time"}, log);
    {
      std::lock_guard lock(served_mutex);
      served[port] = {pid, log};
    }
    out["name"] = "ssom_" + stem;
    out["state"] = "building";
    out["port"] = port;
    out["log"] = log.string();
  }
  return out;
}

// A CSV header's cells: quoted ones may hold commas ("R.T[1,1]").
std::vector<std::string> csv_cells(const std::string& line) {
  std::vector<std::string> out(1);
  bool quoted = false;
  for (const char c : line) {
    if (c == '"') quoted = !quoted;
    else if (c == ',' && !quoted) out.emplace_back();
    else if (c != '\r') out.back() += c;
  }
  return out;
}

// One CSV row of numbers; NaN and infinity become null (JSON has neither).
std::vector<json> numbers(const std::string& line) {
  std::vector<json> out;
  std::stringstream row(line);
  for (std::string cell; std::getline(row, cell, ',');) {
    const double v = std::strtod(cell.c_str(), nullptr);
    out.push_back(std::isfinite(v) ? json(v) : json());
  }
  return out;
}

// A test stimulus for a top-level input, as Modelica: {kind: constant|step|ramp|sine, k, t0, f}.
std::string stimulus(const json& s, bool boolean) {
  const auto num = [&](const char* key, double fallback) {
    const double v = s.contains(key) && s[key].is_number() ? s[key].get<double>() : fallback;
    if (!std::isfinite(v)) throw std::runtime_error(std::string("stimulus ") + key + " must be a number");
    std::ostringstream out;
    out.precision(17);
    out << v;
    return "(" + out.str() + ")";
  };
  const std::string kind = s.value("kind", "constant");
  const std::string k = num("k", 0), t0 = num("t0", 0);
  if (boolean) return kind == "constant" ? (s.value("k", 0.0) != 0 ? "true" : "false") : "time >= " + t0;
  if (kind == "constant") return k;
  if (kind == "step") return "if time >= " + t0 + " then " + k + " else 0";
  if (kind == "ramp") return "if time >= " + t0 + " then " + k + " * (time - " + t0 + ") else 0";
  if (kind == "sine") return k + " * sin(2 * 3.141592653589793 * " + num("f", 1) + " * time)";
  throw std::runtime_error("stimulus is constant, step, ramp or sine, not " + kind);
}

// The model's top-level inputs (a RealInput connector, or `input Real u`) and whether each is Boolean.
std::map<std::string, bool> top_inputs(Omc& omc, const std::string& model) {
  std::map<std::string, bool> out;
  for (const auto& c : parse_value(omc.call("getComponents(" + model + ")")).items) {
    if (c.size() < 11) continue;
    const std::string cls = c[0].atom, name = c[1].atom;
    const std::string shortcls = cls.substr(cls.rfind('.') + 1);
    const bool causal = shortcls == "RealInput" || shortcls == "IntegerInput" || shortcls == "BooleanInput";
    if (c[3].atom != "protected" && (causal || c[10].atom == "input"))
      out[name] = shortcls == "BooleanInput" || cls == "Boolean";
  }
  return out;
}

json op_simulate(Omc& omc, const json& in) {
  const std::string model = user_model(in);
  const double stop = in.value("stop", 1.0);
  if (!(stop > 0.0) || !std::isfinite(stop)) throw std::runtime_error("stop time must be above 0");
  // The solver settings (Studio's Simulate ▸ solver): omc's method, its tolerance, the output
  // intervals and the start time.
  static const std::set<std::string> methods = {"dassl", "ida", "cvode", "euler", "heun", "rungekutta", "impeuler", "trapezoid", "gbode"};
  const std::string method = in.value("method", "dassl");
  if (!methods.contains(method)) throw std::runtime_error("no solver " + method);
  const double tolerance = in.value("tolerance", 1e-6), start = in.value("start", 0.0);
  const int intervals = in.value("intervals", 500);
  if (!(tolerance > 0.0) || !(tolerance < 1.0)) throw std::runtime_error("the tolerance is between 0 and 1");
  if (intervals < 1 || intervals > 1000000) throw std::runtime_error("intervals are 1 to 1000000");
  if (!(start < stop) || !std::isfinite(start)) throw std::runtime_error("the start time is before the stop time");

  // Top-level inputs have nothing driving them on their own: simulate a harness that holds the
  // model as `m` and feeds each input its stimulus (0 unless given), then name things as the model does.
  const auto inputs = top_inputs(omc, model);
  const json given = in.value("inputs", json::object());
  const std::string harness = "SqueroSimHarness";
  const std::string prefix = inputs.empty() ? "" : "m.";
  if (!inputs.empty()) {
    std::string text = "model " + harness + "\n  " + model + " m;\nequation\n";
    for (const auto& [name, boolean] : inputs)
      text += "  m." + name + " = " + stimulus(given.contains(name) ? given[name] : json::object(), boolean) + ";\n";
    omc.call("deleteClass(" + harness + ")");
    if (omc.call("loadString(" + quote(text + "end " + harness + ";\n") + ")") != "true")
      throw std::runtime_error("cannot build the input harness: " + omc.errors());
  }
  struct Drop {
    Omc& omc;
    std::string cls;
    ~Drop() {
      if (!cls.empty()) omc.call("deleteClass(" + cls + ")");
    }
  } drop{omc, inputs.empty() ? "" : harness};

  std::string filter;
  for (const auto& v : in.value("vars", json::array())) {
    const std::string name = prefix + v.get<std::string>();
    if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.[](),") != std::string::npos)
      throw std::runtime_error("not a variable name: " + name);
    std::string re;
    for (const char c : name) re += (std::string_view(".[]()").find(c) != std::string_view::npos ? "\\" : "") + std::string(1, c);
    filter += (filter.empty() ? "" : "|") + re;
  }
  // .mat, as it alone keeps parameters: a shape's sizes are often parameters, read back with val().
  // -d=visxml writes <model>_visual.xml next to it: the MultiBody shapes and what drives them.
  omc.call("setCommandLineOptions(\"-d=visxml\")");
  const auto num = [](double v) {
    std::ostringstream o;
    o.precision(17);
    o << v;
    return o.str();
  };
  std::string expr = "simulate(" + (inputs.empty() ? model : harness) + ", startTime=" + num(start) + ", stopTime=" + num(stop) +
                     ", numberOfIntervals=" + std::to_string(intervals) + ", method=\"" + method + "\", tolerance=" + num(tolerance);
  // a filtered run is plain CSV: just those variables, no shapes
  if (!filter.empty()) expr += ", outputFormat=\"csv\", variableFilter=" + quote(filter);
  const std::string answer = omc.call(expr + ")", std::chrono::minutes(5));
  const auto at = answer.find("resultFile = \"");
  std::string mat;
  if (at != std::string::npos) mat = answer.substr(at + 14, answer.find('"', at + 14) - at - 14);
  // the scopes read CSV: the run's variables (parameters can't be CSV columns)
  const std::string file = mat.empty() || !filter.empty() ? mat : mat.substr(0, mat.size() - 4) + ".csv";
  if (!mat.empty() && filter.empty() && omc.call("filterSimulationResults(" + quote(mat) + ", " + quote(file) + ", readSimulationResultVars(" + quote(mat) +
                               ", readParameters=false, openmodelicaStyle=true))", std::chrono::minutes(1)) != "true")
    throw std::runtime_error("cannot read the result: " + omc.errors());
  std::ifstream csv(file);
  std::string header;
  if (file.empty() || !std::getline(csv, header)) {
    const auto m = answer.find("messages = \"");
    std::string why = m == std::string::npos ? "" : answer.substr(m + 12, answer.find("\",\n", m) - m - 12);
    // "Failed to build model: X" says nothing; omc's first Error line says why
    const std::string errors = omc.errors();
    const auto e = errors.find("Error: ");
    const std::string cause = e == std::string::npos ? "" : errors.substr(e + 7, errors.find('\n', e) - e - 7);
    throw std::runtime_error("simulation failed: " + trim(!cause.empty() ? cause : why.empty() ? errors : why));
  }
  // the harness's m.x is the model's x, der(m.x) its der(x)
  const auto unprefix = [&](std::string cell) {
    for (const std::string from : {prefix, "der(" + prefix})
      if (!prefix.empty() && cell.starts_with(from)) return (from.size() > prefix.size() ? "der(" : "") + cell.substr(from.size());
    return cell;
  };
  json names = json::array();
  std::vector<std::vector<json>> columns;
  for (const auto& cell : csv_cells(header)) names.push_back(unprefix(cell));
  columns.resize(names.size());
  for (std::string line; std::getline(csv, line);) {
    const auto row = numbers(line);
    for (std::size_t i = 0; i < columns.size() && i < row.size(); ++i) columns[i].push_back(row[i]);
  }
  json series = json::object();
  json all = json::array();
  for (std::size_t i = 1; i < names.size(); ++i) {
    series[names[i].get<std::string>()] = columns[i];
    all.push_back(names[i]);
  }
  json out{{"t", columns.empty() ? std::vector<json>{} : columns[0]}, {"series", series}, {"names", all}};
  // A filtered run lacks what the shapes read: shapes only for a full one.
  if (filter.empty()) {
    std::ifstream xml(mat.substr(0, mat.size() - 8) + "_visual.xml");
    std::stringstream text;
    text << xml.rdbuf();
    out["shapes"] = visual_shapes(text.str(), [&](const std::string& cref) -> json {
      const std::string name = unprefix(cref);
      if (series.contains(name)) return name;
      const double v = std::strtod(text_of(omc.call("val(" + cref + ", 0.0, " + quote(mat) + ")")).c_str(), nullptr);
      return std::isfinite(v) ? json(v) : json(0.0);
    });
    // the harness's m.world is the model's world
    for (auto& sh : out["shapes"]) sh["ident"] = unprefix(sh["ident"].get<std::string>());
  }
  return out;
}

}  // namespace

nlohmann::json visual_shapes(const std::string& xml, const std::function<json(const std::string&)>& resolve) {
  // ponytail: a scan for omc's fixed layout, not an XML parser; fine for what omc writes
  const auto inner = [](const std::string& text, const std::string& tag, std::size_t from = 0) -> std::string {
    const auto a = text.find("<" + tag + ">", from);
    if (a == std::string::npos) return "";
    const auto b = text.find("</" + tag + ">", a);
    return text.substr(a + tag.size() + 2, b - a - tag.size() - 2);
  };
  // a field's values in order: <exp>1.0</exp> a number, <cref>x</cref> what resolve says
  const auto values = [&](const std::string& field) {
    json out = json::array();
    for (std::size_t i = 0; (i = field.find('<', i)) != std::string::npos;) {
      const bool cref = field.compare(i, 6, "<cref>") == 0;
      if (!cref && field.compare(i, 5, "<exp>") != 0) {
        ++i;
        continue;
      }
      const auto a = i + (cref ? 6 : 5), b = field.find('<', a);
      const std::string v = trim(field.substr(a, b - a));
      out.push_back(cref ? resolve(v) : json(std::strtod(v.c_str(), nullptr)));
      i = b + 1;
    }
    return out;
  };
  json shapes = json::array();
  for (std::size_t at = 0; (at = xml.find("<shape>", at)) != std::string::npos;) {
    const auto end = xml.find("</shape>", at);
    const std::string s = xml.substr(at, end - at);
    json shape{{"ident", trim(inner(s, "ident"))}, {"type", trim(inner(s, "type"))}};
    for (const char* v : {"T", "r", "r_shape", "lengthDir", "widthDir", "color"}) shape[v] = values(inner(s, v));
    for (const char* v : {"length", "width", "height", "extra"}) {
      const json one = values(inner(s, v));
      shape[v] = one.empty() ? json(0.0) : one[0];
    }
    shapes.push_back(shape);
    at = end;
  }
  return shapes;
}

std::string handle(Omc& omc, const std::string& message, const Session& session) {
  json in;
  try {
    in = json::parse(message);
  } catch (const json::exception&) {
    return json{{"ok", false}, {"error", "not JSON"}}.dump();
  }
  json out{{"id", in.value("id", json())}};
  const std::string op = in.value("op", "");
  // A client asking for getErrorString wants the messages from the previous operation, which
  // the log drain already read. Everything else starts a fresh read.
  const bool errors_read = op == "omc" && in.contains("expr") && in["expr"].is_string() &&
                           in["expr"].get<std::string>() == "getErrorString()";
  if (!errors_read) omc.clear_messages();
  try {
    if (errors_read) {
      out["result"] = omc.take_errors();
    } else if (op == "omc" && in.contains("expr") && in["expr"].is_string()) {
      const std::string expr = in["expr"];
      if (!allowed(expr)) {
        out["ok"] = false;
        out["error"] = "not allowed: " + expr.substr(0, expr.find('(')) + " (Studio may call only the squero/0 allow-list)";
        return out.dump();
      }
      out["result"] = omc.call(expr);
    } else if (op == "open") {
      out["result"] = op_open(omc, in, session);
    } else if (op == "workspace") {
      out["result"] = op_workspace(in);
    } else if (op == "git") {
      out["result"] = op_git(in);
    } else if (op == "config") {
      out["result"] = op_config();
    } else if (op == "config_write") {
      out["result"] = op_config_write(in);
    } else if (op == "about") {
      const std::string version = paperboat_version();
      out["result"] = json{{"paperboat", version.empty() ? json(nullptr) : json(version)}};
    } else if (op == "create_class") {
      out["result"] = op_create_class(in);
    } else if (op == "new_package") {
      out["result"] = op_new_package(in);
    } else if (op == "duplicate_class") {
      out["result"] = op_duplicate_class(in);
    } else if (op == "rename_class") {
      out["result"] = op_rename_class(in);
    } else if (op == "delete_class") {
      out["result"] = op_delete_class(in);
    } else if (op == "move_class") {
      out["result"] = op_move_class(in);
    } else if (op == "delete_package") {
      out["result"] = op_delete_package(in);
    } else if (op == "read_file") {
      out["result"] = op_read_file(in);
    } else if (op == "save") {
      out["result"] = op_save(omc, in);
    } else if (op == "check_model") {
      out["result"] = op_check_model(omc, in);
    } else if (op == "instantiate_model") {
      out["result"] = op_instantiate_model(omc, in);
    } else if (op == "get_messages") {
      out["result"] = op_get_messages(omc);
    } else if (op == "icon") {
      out["result"] = op_icon(omc, in);
    } else if (op == "set_icon") {
      out["result"] = op_set_icon(omc, in);
    } else if (op == "generate") {
      out["result"] = op_generate(omc, in, session);
    } else if (op == "export") {
      out["result"] = op_export(omc, in, session);
    } else if (op == "simulate") {
      out["result"] = op_simulate(omc, in);
    } else {
      out["ok"] = false;
      out["error"] = "unknown op; squero/0 has omc, open, workspace, config, config_write, about, create_class, new_package, duplicate_class, delete_class, delete_package, move_class, read_file, save, check_model, instantiate_model, get_messages, icon, set_icon, generate, export, simulate";
      return out.dump();
    }
    out["ok"] = true;
  } catch (const std::exception& e) {
    out["ok"] = false;
    out["error"] = e.what();
  }
  // Drain once per operation so the Console hears omc, and a later getErrorString can read it.
  // If an op already read the messages (errors() or its own drain), keep that read.
  if (!errors_read) {
    try {
      if (omc.last_messages().empty()) omc.messages();
    } catch (...) {
      // omc failed: the reply already says so
    }
  }
  return out.dump();
}

fs::path default_workspace() {
  if (const char* given = std::getenv("SQUERO_OM_WORKSPACE"); given && *given) return given;
  const char* home = std::getenv("HOME");
  if (!home || !*home) home = std::getenv("USERPROFILE");
  return fs::path(home && *home ? home : ".") / "Documents" / "paperboat" / "squero" / "om";
}

fs::path default_file(const fs::path& dir) {
  fs::create_directories(dir);
  std::vector<fs::path> models;
  for (const auto& entry : fs::directory_iterator(dir))
    if (entry.is_regular_file() && entry.path().extension() == ".mo" && entry.path().filename() != "package.mo")
      models.push_back(entry.path());
  std::sort(models.begin(), models.end());
  return models.empty() ? dir / "Untitled.mo" : models.front();
}

int serve_squero(int port, const fs::path& app, Omc::Options options, const fs::path& given) {
  ix::initNetSystem();
  // No model named: open the Studio's own folder, as Squero MX does, never the launch directory.
  const fs::path file = given.empty() ? default_file() : given;
  // SIGTERM (a closed terminal, `paperboat stop`, systemd) and SIGINT (Ctrl-C) both mean
  // "stop": wake the loop below so omc is stopped on purpose, not just orphaned.
  stop_requested.store(false, std::memory_order_relaxed);
#ifdef _WIN32
  // Ctrl-C and Ctrl-Break arrive as SIGINT/SIGBREAK. `paperboat stop` ends the process outright,
  // and omc's job (kill on close) takes omc with it.
  std::signal(SIGINT, request_stop);
  std::signal(SIGBREAK, request_stop);
  std::signal(SIGTERM, request_stop);
#else
  struct sigaction sa {};
  sa.sa_handler = request_stop;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // no SA_RESTART: a polled sleep returns early
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
#endif
  Omc omc(std::move(options));
  // port 0: the first free one of SQR01..SQR99 (59201..59299)
  for (int p = 59201; port == 0 && p <= 59299; ++p)
    if (!listening(p)) port = p;
  if (port == 0) {
    std::cerr << "no free port in 59201..59299\n";
    return 1;
  }
  ix::HttpServer server(port, "127.0.0.1");
  // Recursive: sending to a client that has just gone away runs its Close handler on this same
  // thread, which takes the lock again. The send loop walks a copy for the same reason.
  std::recursive_mutex mutex;
  std::set<ix::WebSocket*> clients;

  omc.on_status([&](OmcState state, const std::string& version) {
    const std::string text = status(state, version, omc.mode());
    std::lock_guard lock(mutex);
    for (auto* ws : std::set<ix::WebSocket*>(clients)) ws->sendText(text);
  });
  // omc's own messages, as they are drained: the Console's live log in every client.
  omc.on_log([&](const OmcMessage& message) {
    json one{{"op", "log"}, {"level", message.level}, {"text", message.text}};
    if (!message.file.empty()) one["file"] = message.file;
    if (message.line > 0) one["line"] = message.line;
    if (message.column > 0) one["column"] = message.column;
    const std::string text = one.dump();
    std::lock_guard lock(mutex);
    for (auto* ws : std::set<ix::WebSocket*>(clients)) ws->sendText(text);
  });
  server.setOnConnectionCallback(
      [&app](const ix::HttpRequestPtr& request, const std::shared_ptr<ix::ConnectionState>&) { return http(app, request); });
  server.setOnClientMessageCallback(
      [&](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
          {
            std::lock_guard lock(mutex);
            clients.insert(&ws);
          }
          ws.sendText(status(omc.state(), omc.version(), omc.mode()));
        } else if (msg->type == ix::WebSocketMessageType::Close) {
          std::lock_guard lock(mutex);
          clients.erase(&ws);
        } else if (msg->type == ix::WebSocketMessageType::Message) {
          // Each connection has its own callback thread; omc serialises the calls.
          const Session session{[&ws](const std::string& text) { ws.sendText(text); }, file};
          if (std::string reply = handle(omc, msg->str, session); !reply.empty()) ws.sendText(reply);
        }
      });
  if (auto [ok, error] = server.listen(); !ok) {
    std::cerr << "cannot listen on port " << port << ": " << error << '\n';
    return 1;
  }
  server.start();
  std::cout << "squero studio OM server (squero/0) on ws://localhost:" << port
            << (app.empty() ? std::string() : "  app: http://localhost:" + std::to_string(port) + "/") << '\n'
            << "opens " << fs::absolute(file).string() << std::endl;
  // Start omc now so the first status a client sees is real.
  try {
    omc.call("getVersion()");
  } catch (const std::exception& e) {
    std::cerr << "omc: " << e.what() << '\n';
  }
  // Wait for a stop signal (or forever), polling so the handler's flag is seen.
  while (!stop_requested.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  omc.shutdown();
  server.stop();
  return 0;
}

}  // namespace squero
