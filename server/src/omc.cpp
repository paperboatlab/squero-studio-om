#include "squero/omc.hpp"

#ifdef _WIN32
#include "squero/win32.hpp"
#else
#include <dlfcn.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <type_traits>
#include <thread>
#include <utility>

namespace squero {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
#ifdef _WIN32
using pid_t = int;
#endif

// The few libzmq calls the bridge needs, resolved from libzmq.so.5 at run time. The ABI is
// stable across libzmq 4.x: zmq_msg_t is 64 opaque bytes, the constants are fixed.
struct ZmqMsg {
  alignas(8) unsigned char bytes[64];
};
constexpr int kReq = 3, kLinger = 17, kRcvTimeo = 27, kSndTimeo = 28;

struct Zmq {
  void* (*ctx_new)() = nullptr;
  int (*ctx_term)(void*) = nullptr;
  void* (*socket)(void*, int) = nullptr;
  int (*close)(void*) = nullptr;
  int (*connect)(void*, const char*) = nullptr;
  int (*setsockopt)(void*, int, const void*, std::size_t) = nullptr;
  int (*send)(void*, const void*, std::size_t, int) = nullptr;
  int (*msg_init)(ZmqMsg*) = nullptr;
  int (*msg_recv)(ZmqMsg*, void*, int) = nullptr;
  void* (*msg_data)(ZmqMsg*) = nullptr;
  std::size_t (*msg_size)(const ZmqMsg*) = nullptr;
  int (*msg_close)(ZmqMsg*) = nullptr;

  static const Zmq& get() {
    static const Zmq zmq = [] {
      Zmq z;
      void* lib = nullptr;
#ifdef _WIN32
      // OpenModelica ships it next to omc.exe (libzmq.dll, or a versioned libzmq-*.dll).
      std::vector<fs::path> dirs;
      if (const std::string omc = resolve_omc().exe; fs::path(omc).has_parent_path()) dirs.push_back(fs::path(omc).parent_path());
      if (const char* home = std::getenv("OPENMODELICAHOME"); home && *home) dirs.push_back(fs::path(home) / "bin");
      for (const fs::path& dir : dirs) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
          const std::string n = e.path().filename().string();
          if (n.starts_with("libzmq") && e.path().extension() == ".dll" && (lib = win32::load_library(e.path()))) break;
        }
        if (lib) break;
      }
      if (!lib) return z;
      auto sym = [&](auto& fn, const char* n) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(win32::symbol(lib, n)); };
#else
      for (const char* name : {"libzmq.so.5", "libzmq.so", "libzmq.5.dylib", "libzmq.dylib"})
        if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
      if (!lib) return z;
      auto sym = [&](auto& fn, const char* n) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, n)); };
#endif
      sym(z.ctx_new, "zmq_ctx_new");
      sym(z.ctx_term, "zmq_ctx_term");
      sym(z.socket, "zmq_socket");
      sym(z.close, "zmq_close");
      sym(z.connect, "zmq_connect");
      sym(z.setsockopt, "zmq_setsockopt");
      sym(z.send, "zmq_send");
      sym(z.msg_init, "zmq_msg_init");
      sym(z.msg_recv, "zmq_msg_recv");
      sym(z.msg_data, "zmq_msg_data");
      sym(z.msg_size, "zmq_msg_size");
      sym(z.msg_close, "zmq_msg_close");
      return z;
    }();
#ifdef _WIN32
    if (!zmq.msg_close) throw OmcError("libzmq*.dll not found next to omc.exe or in %OPENMODELICAHOME%\\bin (it comes with OpenModelica)");
#else
    if (!zmq.msg_close) throw OmcError("libzmq.so.5 not found (it comes with OpenModelica)");
#endif
    return zmq;
  }
};

std::atomic<int> sessions{0};

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  std::size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return s.substr(i);
}

// `getErrorString()` as separate messages. omc writes one per line, a source span in brackets
// when it has one: `[/tmp/M.mo:18:5-18:10:writable] Error: …`.
std::vector<OmcMessage> parse_messages(std::string_view raw) {
  std::vector<OmcMessage> out;
  std::size_t at = 0;
  while (at < raw.size()) {
    const std::size_t end = raw.find('\n', at);
    std::string line = trim(std::string(raw.substr(at, end == std::string_view::npos ? raw.size() - at : end - at)));
    at = end == std::string_view::npos ? raw.size() : end + 1;
    if (line.empty()) continue;
    OmcMessage message;
    std::string text = line;
    if (line.front() == '[') {
      if (const std::size_t close = line.find(']'); close != std::string::npos) {
        const std::string inside = line.substr(1, close - 1);
        // A Windows path starts with its drive, `C:/Users/…/M.mo:18:5-…`: the file ends at the
        // first colon after it.
        const bool drive = inside.size() > 2 && std::isalpha(static_cast<unsigned char>(inside[0])) && inside[1] == ':' &&
                           (inside[2] == '/' || inside[2] == '\\');
        if (const std::size_t colon = inside.find(':', drive ? 2 : 0); colon != std::string::npos) {
          message.file = inside.substr(0, colon);
          std::size_t p = colon + 1;
          const auto number = [&](int& value) {
            const std::size_t start = p;
            while (p < inside.size() && std::isdigit(static_cast<unsigned char>(inside[p]))) ++p;
            if (p == start) return false;
            value = std::stoi(inside.substr(start, p - start));
            return true;
          };
          if (number(message.line) && p < inside.size() && inside[p] == ':') {
            ++p;
            number(message.column);
          }
        }
        text = trim(line.substr(close + 1));
      }
    }
    for (const auto& [prefix, level] : {std::pair{"Error:", "error"}, std::pair{"Warning:", "warning"},
                                        std::pair{"Notification:", "info"}, std::pair{"Info:", "info"}}) {
      if (text.rfind(prefix, 0) == 0) {
        message.level = level;
        text = trim(text.substr(std::string_view(prefix).size()));
        break;
      }
    }
    if (message.level.empty()) message.level = "info";
    if (text.empty()) continue;
    message.text = std::move(text);
    out.push_back(std::move(message));
  }
  return out;
}

}  // namespace

std::string_view name(OmcState state) {
  switch (state) {
    case OmcState::Ready: return "ready";
    case OmcState::Checking: return "checking";
    case OmcState::Failed: return "failed";
  }
  return "failed";
}

struct Omc::Impl {
  Options options;
  std::mutex mutex;  // the queue: one request in flight
  pid_t pid = -1;
  void* ctx = nullptr;
  void* sock = nullptr;
  std::string session;
  std::string container_name;  // a podman/docker run's name, for `rm -f` on stop
  pid_t cleanup_pid = -1;      // the child that removes the container when we go, however we go
  int cleanup_fd = -1;         // its pipe: closing it (even by SIGKILL) wakes it
  fs::path workdir;
  std::string version;
  OmcRuntime runtime;
  OmcState state = OmcState::Ready;
  int restarts = 0;  // in a row
  bool started_once = false;
  bool shutting_down = false;  // a stop was asked for; never start again
  std::vector<std::string> replay;  // loadModel / loadFile calls that succeeded
  StatusFn status_fn;
  LogFn log_fn;
  mutable std::mutex state_mutex;
  std::string last_raw;             // getErrorString() as the last drain saw it
  std::vector<OmcMessage> last_msgs;

  void set_state(OmcState s) {
    {
      std::lock_guard lock(state_mutex);
      if (state == s) return;
      state = s;
    }
    if (status_fn) status_fn(s, version);
  }

  fs::path port_file() const {
    // omc writes <tmp>/openmodelica.<user>.port.<session>; the user part varies, so look.
    const std::string suffix = ".port." + session;
    for (const fs::path& dir : {fs::temp_directory_path(), fs::path("/tmp")}) {
      std::error_code ec;
      for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.starts_with("openmodelica.") && n.ends_with(suffix)) return e.path();
      }
    }
    return {};
  }

  // A podman/docker run's client can die with us and leave its container running (`--init`
  // omc as PID 1). This child waits for our end of a pipe to close (a normal stop, SIGKILL,
  // or a crash all close it) and then removes the container by name.
  void start_cleaner() {
    if (runtime.mode == "native" || container_name.empty()) return;
#ifdef _WIN32
    // Windows runs omc natively only (resolve_omc); no container to clean up after.
#else
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return;
    cleanup_pid = fork();
    if (cleanup_pid == 0) {
#ifdef __linux__
      prctl(PR_SET_PDEATHSIG, 0);  // we must outlive the server to clean up after it
#endif
      // A terminal sends Ctrl-C (SIGINT) to the whole process group, and `kill` often does
      // the same: ignore it here, or the cleaner would die before it removed the container.
      struct sigaction ignore {};
      ignore.sa_handler = SIG_IGN;
      sigemptyset(&ignore.sa_mask);
      sigaction(SIGINT, &ignore, nullptr);
      sigaction(SIGTERM, &ignore, nullptr);
      sigaction(SIGHUP, &ignore, nullptr);
      close(fds[1]);
      char buf[1];
      while (read(fds[0], buf, 1) > 0) {
      }
      close(fds[0]);
      execlp(runtime.exe.c_str(), runtime.exe.c_str(), "rm", "-f", "-t", "1", container_name.c_str(),
             static_cast<char*>(nullptr));
      _exit(127);
    }
    close(fds[0]);
    if (cleanup_pid < 0) {
      close(fds[1]);
      return;
    }
    cleanup_fd = fds[1];
#endif
  }

  void remove_container() {
    if (container_name.empty()) return;
#ifdef _WIN32
    container_name.clear();
#else
    if (cleanup_fd >= 0) {
      close(cleanup_fd);  // wakes the cleaner: it runs `rm -f`
      cleanup_fd = -1;
    }
    if (cleanup_pid > 0) {
      // `podman rm -f -t 1` stops the container in a second or two; do not hang on a broken engine.
      for (int i = 0; i < 300 && waitpid(cleanup_pid, nullptr, WNOHANG) == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (waitpid(cleanup_pid, nullptr, WNOHANG) == 0) {
        kill(cleanup_pid, SIGKILL);
        waitpid(cleanup_pid, nullptr, 0);
      }
      cleanup_pid = -1;
      container_name.clear();
      return;
    }
    // No cleaner (it could not be forked): remove the container here.
    const std::string engine = runtime.exe;
    const std::string name = std::exchange(container_name, {});
    const pid_t child = fork();
    if (child == 0) {
      execlp(engine.c_str(), engine.c_str(), "rm", "-f", "-t", "1", name.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    if (child > 0) waitpid(child, nullptr, 0);
#endif
  }

  void stop() {
    const Zmq* z = nullptr;
    try {
      z = &Zmq::get();
    } catch (const OmcError&) {
    }
    if (z && sock) z->close(sock);
    if (z && ctx) z->ctx_term(ctx);
    sock = ctx = nullptr;
    if (pid > 0) {
#ifdef _WIN32
      // no SIGTERM to ask with: the ZeroMQ socket is closed, so end omc and what it started
      win32::kill_tree(pid);
      win32::wait(pid);
#else
      kill(pid, SIGTERM);
      for (int i = 0; i < 20 && waitpid(pid, nullptr, WNOHANG) == 0; ++i) std::this_thread::sleep_for(25ms);
      if (waitpid(pid, nullptr, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
      }
#endif
      pid = -1;
    }
    // A container's podman client can go while its container keeps running (`--init` omc as
    // PID 1): remove it by name, so a stopped Studio leaves no omc behind.
    remove_container();
    if (const fs::path f = port_file(); !f.empty()) fs::remove(f);
  }

  void start() {
    const Zmq& z = Zmq::get();
#ifdef _WIN32
    session = "paperboat-" + std::to_string(win32::current_pid()) + "-" + std::to_string(sessions++);
#else
    session = "paperboat-" + std::to_string(getpid()) + "-" + std::to_string(sessions++);
#endif
    workdir = fs::temp_directory_path() / ("paperboat-omc-" + session);
    fs::create_directories(workdir);
    const std::string zarg = "-z=" + session;
    if (runtime.mode.empty()) runtime = options.omc.empty() ? resolve_omc() : OmcRuntime{"native", options.omc, ""};
    if (runtime.mode != "native") container_name = "paperboat-omc-" + session;
    start_cleaner();
    std::vector<std::string> command = omc_command(runtime, workdir.string(), {"--interactive=zmq", zarg}, container_name);
#ifdef _WIN32
    // In a job that ends with this process, however it ends (PR_SET_PDEATHSIG on Linux).
    pid = win32::spawn(command, workdir, {}, win32::Err::Inherit, true);
    if (pid < 0) throw OmcError("cannot start omc (`" + runtime.exe + "`" + (runtime.why.empty() ? "" : "; " + runtime.why) + ")");
#else
    pid = fork();
    if (pid < 0) throw OmcError("cannot start omc: fork failed");
    if (pid == 0) {
#ifdef __linux__
      // A native omc can ignore SIGTERM; kill it outright when the server goes away, even if
      // the server was stopped with SIGTERM and never ran its destructor. A container gets
      // SIGTERM so podman can stop and --rm it.
      prctl(PR_SET_PDEATHSIG, runtime.mode == "native" ? SIGKILL : SIGTERM);
      if (getppid() == 1) _exit(127);  // the parent died before prctl took effect
#endif
      if (chdir(workdir.c_str()) != 0) _exit(127);
      const int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        // the engine's own chatter (a stop signal racing the container's exit) isn't omc's
        if (runtime.mode != "native") dup2(devnull, STDERR_FILENO);
      }
      close_range(3, ~0U, 0);  // not the server's sockets
      std::vector<char*> argv;
      for (auto& a : command) argv.push_back(a.data());
      argv.push_back(nullptr);
      execvp(argv[0], argv.data());
      _exit(127);
    }
#endif
    fs::path file;
    // up to 20 s; a container's first start pulls the image, so minutes
    const int tries = runtime.mode == "native" ? 400 : 20 * 60 * 10;
    for (int i = 0; i < tries && (file = port_file()).empty(); ++i) {
#ifdef _WIN32
      if (!win32::running(pid)) {
        win32::wait(pid);
#else
      if (waitpid(pid, nullptr, WNOHANG) == pid) {
#endif
        pid = -1;
        throw OmcError("omc exited at start (" + runtime.mode + ": `" + runtime.exe + "`" + (runtime.why.empty() ? "" : "; " + runtime.why) + ")");
      }
      std::this_thread::sleep_for(50ms);
    }
    std::string endpoint;
    for (int i = 0; i < 40 && endpoint.empty(); ++i) {  // the file can appear before it's written
      std::ifstream in(file);
      std::getline(in, endpoint);
      endpoint = trim(endpoint);
      if (endpoint.empty()) std::this_thread::sleep_for(25ms);
    }
    if (endpoint.empty()) {
      stop();
      throw OmcError("omc did not report its port");
    }
    ctx = z.ctx_new();
    sock = z.socket(ctx, kReq);
    const int linger = 0;
    z.setsockopt(sock, kLinger, &linger, sizeof linger);
    if (z.connect(sock, endpoint.c_str()) != 0) {
      stop();
      throw OmcError("cannot connect to omc at " + endpoint);
    }
    started_once = true;
  }

  // One round trip; nullopt on timeout or a dead omc.
  std::optional<std::string> exchange(std::string_view expr, std::chrono::milliseconds timeout) {
    const Zmq& z = Zmq::get();
    const int ms = static_cast<int>(timeout.count());
    z.setsockopt(sock, kSndTimeo, &ms, sizeof ms);
    z.setsockopt(sock, kRcvTimeo, &ms, sizeof ms);
    if (z.send(sock, expr.data(), expr.size(), 0) < 0) return std::nullopt;
    ZmqMsg msg;
    z.msg_init(&msg);
    if (z.msg_recv(&msg, sock, 0) < 0) {
      z.msg_close(&msg);
      return std::nullopt;
    }
    std::string out(static_cast<const char*>(z.msg_data(&msg)), z.msg_size(&msg));
    z.msg_close(&msg);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
  }

  std::string call(std::string_view expr, std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex);
    if (shutting_down) throw OmcError("omc is shutting down");
    if (state == OmcState::Failed) throw OmcError("omc failed: restarted " + std::to_string(restarts) + " times");
    set_state(OmcState::Checking);
    for (;;) {
      try {
        if (!sock) {
          const bool again = started_once;
          start();
          if (auto v = exchange("getVersion()", timeout)) version = trim(*v), version = version.size() >= 2 ? version.substr(1, version.size() - 2) : version;
          if (again)  // a restart: load what the old omc had loaded
            for (const auto& r : replay) exchange(r, timeout);
        }
        if (auto out = exchange(expr, timeout)) {
          restarts = 0;
          if ((expr.starts_with("loadModel(") || expr.starts_with("loadFile(")) && *out == "true")
            if (std::find(replay.begin(), replay.end(), expr) == replay.end()) replay.emplace_back(expr);
          set_state(OmcState::Ready);
          return *out;
        }
      } catch (const OmcError&) {
        stop();
        set_state(OmcState::Failed);
        throw;
      }
      stop();  // timed out or died: restart and retry, up to max_restarts in a row
      if (++restarts > options.max_restarts) {
        set_state(OmcState::Failed);
        throw OmcError("omc did not answer `" + std::string(expr.substr(0, 60)) + "` within " +
                       std::to_string(timeout.count() / 1000) + " s; gave up after " +
                       std::to_string(options.max_restarts) + " restarts");
      }
    }
  }

  // Drain omc's message buffer into the last-drain cache and tell the log listener. One extra
  // round trip; callers decide when it is worth it.
  std::pair<std::string, std::vector<OmcMessage>> drain() {
    const std::string raw = trim(parse_value(call("getErrorString()", options.timeout)).atom);
    std::vector<OmcMessage> messages = parse_messages(raw);
    {
      std::lock_guard lock(state_mutex);
      last_raw = raw;
      last_msgs = messages;
    }
    if (log_fn)
      for (const auto& message : messages) log_fn(message);
    return {raw, std::move(messages)};
  }
};

Omc::Omc() : Omc(Options{}) {}
Omc::Omc(Options options) : impl_(std::make_unique<Impl>()) { impl_->options = std::move(options); }

Omc::~Omc() {
  if (impl_->sock) {
    try {
      impl_->exchange("quit()", 1s);
    } catch (...) {
    }
  }
  impl_->stop();
  std::error_code ec;
  if (!impl_->workdir.empty()) fs::remove_all(impl_->workdir, ec);
}

std::string Omc::call(std::string_view expr) { return impl_->call(expr, impl_->options.timeout); }
std::string Omc::call(std::string_view expr, std::chrono::milliseconds timeout) { return impl_->call(expr, timeout); }

void Omc::shutdown() {
  std::lock_guard lock(impl_->mutex);
  impl_->shutting_down = true;
  impl_->stop();
  std::error_code ec;
  if (!impl_->workdir.empty()) fs::remove_all(impl_->workdir, ec);
}

std::string Omc::errors() { return impl_->drain().first; }

std::vector<OmcMessage> Omc::messages() { return impl_->drain().second; }

std::vector<OmcMessage> Omc::last_messages() const {
  std::lock_guard lock(impl_->state_mutex);
  return impl_->last_msgs;
}

std::string Omc::take_errors() {
  std::lock_guard lock(impl_->state_mutex);
  std::string raw = std::move(impl_->last_raw);
  impl_->last_raw.clear();
  impl_->last_msgs.clear();
  return raw;
}

void Omc::clear_messages() {
  std::lock_guard lock(impl_->state_mutex);
  impl_->last_raw.clear();
  impl_->last_msgs.clear();
}

void Omc::on_log(LogFn fn) { impl_->log_fn = std::move(fn); }

OmcState Omc::state() const {
  std::lock_guard lock(impl_->state_mutex);
  return impl_->state;
}
std::string Omc::version() const { return impl_->version; }
void Omc::on_status(StatusFn fn) { impl_->status_fn = std::move(fn); }
std::string Omc::workdir() const { return impl_->workdir.string(); }
std::string Omc::mode() const { return impl_->runtime.mode; }

namespace {
// `name` on PATH (or a path that exists), else "".
std::string which(const std::string& name) {
#ifdef _WIN32
  if (name.find_first_of("/\\") != std::string::npos) return fs::exists(name) ? name : "";
  return win32::which(name);
#else
  if (name.find('/') != std::string::npos) return fs::exists(name) ? name : "";
  const char* path = std::getenv("PATH");
  std::string_view rest = path ? path : "";
  while (!rest.empty()) {
    const auto colon = rest.find(':');
    const fs::path f = fs::path(std::string(rest.substr(0, colon))) / name;
    if (access(f.c_str(), X_OK) == 0) return f.string();
    rest = colon == std::string_view::npos ? "" : rest.substr(colon + 1);
  }
  return "";
#endif
}

// Why a native omc can't build FMUs, or "": every file of its FMU build project must be
// readable (OpenModelica nightlies have shipped config.guess and config.sub root-only).
std::string unhealthy(const std::string& omc) {
#ifdef _WIN32
  (void)omc;  // the root-only files are a Linux nightly's packaging; the Windows installer's are readable
  return "";
#else
  std::error_code ec;
  const fs::path project = fs::canonical(omc, ec).parent_path().parent_path() / "share/omc/runtime/c/fmi/buildproject";
  if (ec || !fs::is_directory(project, ec)) return "";
  for (const auto& e : fs::directory_iterator(project, ec))
    if (e.is_regular_file() && access(e.path().c_str(), R_OK) != 0)
      return e.path().string() + " is not readable, so omc can't build FMUs";
  return "";
#endif
}
}  // namespace

OmcRuntime resolve_omc() {
  const auto env = [](const char* k) { const char* v = std::getenv(k); return std::string(v ? v : ""); };
  for (const char* k : {"SQUERO_OMC_BIN", "PAPERBOAT_OMC_BIN"})
    if (!env(k).empty()) return {"native", env(k), ""};
#ifdef _WIN32
  // Native only: the container modes mount the host's /tmp and $HOME at the same paths and run as
  // its uid, which a Windows host has none of. The OpenModelica installer sets OPENMODELICAHOME.
  if (const std::string native = which("omc"); !native.empty()) return {"native", native, ""};
  if (const std::string home = env("OPENMODELICAHOME"); !home.empty()) {
    const fs::path omc = fs::path(home) / "bin" / "omc.exe";
    if (fs::is_regular_file(omc)) return {"native", omc.string(), ""};
  }
  return {"native", "omc", "no omc on PATH and no OPENMODELICAHOME: install OpenModelica (podman and docker are not used on Windows)"};
#else
  const std::string force = env("SQUERO_OMC");
  const std::string native = which("omc");
  const std::string engine = force == "docker" ? which("docker") : force == "podman" ? which("podman")
                             : !which("podman").empty() ? which("podman") : which("docker");
  const std::string engine_mode = engine.empty() ? "" : fs::path(engine).filename().string();
  if (force == "native") return {"native", native.empty() ? "omc" : native, ""};
  if ((force == "podman" || force == "docker") && !engine.empty()) return {engine_mode, engine, ""};
  const std::string sick = native.empty() ? "no omc on PATH" : unhealthy(native);
  if (!native.empty() && sick.empty()) return {"native", native, ""};
  if (!engine.empty()) return {engine_mode, engine, sick};
  return {"native", native.empty() ? "omc" : native, sick + (native.empty() ? ", and no podman or docker to run it in" : "")};
#endif
}

std::vector<std::string> omc_command(const OmcRuntime& rt, const std::string& workdir, const std::vector<std::string>& args,
                                     const std::string& name) {
  std::vector<std::string> out;
  if (rt.mode == "native") {
    out.push_back(rt.exe);
  } else {
    const char* home = std::getenv("HOME");
    const std::string h = home ? home : "/root";
    const std::string tmp = fs::temp_directory_path().string();
    out = {rt.exe, "run", "--rm", "-i", "--init", "--network=host", "--security-opt", "label=disable", "-e", "HOME=" + h,
           "-e", "USER=" + std::string(std::getenv("USER") ? std::getenv("USER") : "omc"), "-v", tmp + ":" + tmp, "-v", h + ":" + h, "-w", workdir};
    // A stable name lets a stopping Studio remove the container even when podman's client is
    // gone (omc is the init process inside, so SIGTERM to the client may not reach it).
    if (!name.empty()) out.insert(out.begin() + 2, {"--name", name, "--label", "paperboat.omc=1"});
    // --init: omc as PID 1 ignores SIGTERM, so the container outlived us
    // files omc writes are the user's: podman maps the user in, docker runs as them
    if (rt.mode == "podman") out.insert(out.begin() + 2, "--userns=keep-id");
#ifndef _WIN32  // never reached on Windows: resolve_omc is native there
    else out.insert(out.begin() + 2, {"--user", std::to_string(getuid()) + ":" + std::to_string(getgid())});
#endif
    if (tmp != "/tmp") out.insert(out.end() - 2, {"-v", "/tmp:/tmp"});
    out.push_back(kImage);
    out.push_back("omc");
  }
  out.insert(out.end(), args.begin(), args.end());
  return out;
}

// --- allow-list ---

const std::vector<std::string>& allowed_calls() {
  static const std::vector<std::string> calls = {
      "loadModel", "loadFile", "getClassNames", "getComponents", "getComponentAnnotations",
      "getNthComponentCondition", "getInheritedClasses", "getClassRestriction", "isConnector",
      "getComponentModifierValue", "getDerivedClassModifierValue", "getIconAnnotation", "getDiagramAnnotation",
      "getConnectionCount", "getNthConnection", "checkModel", "instantiateModel", "addComponent", "deleteComponent", "addConnection",
      "deleteConnection", "setComponentModifierValue", "list", "dumpXMLDAE", "buildModelFMU", "getVersion",
      "getErrorString",  // read-only, how a client words a failed call
      "isPartial", "getSourceFile", "getClassComment", "getDocumentationAnnotation",  // read-only: the library tree, open and Help
      "getComponentModifierNames", "getNthConnectionAnnotation", "getParameterNames", "getParameterValue",  // read-only: the canvas
      "setParameterValue", "setComponentProperties", "updateComponent", "updateConnection"};  // edits, like the rest, of the class named
  return calls;
}

bool allowed(std::string_view expr) {
  const auto& calls = allowed_calls();
  auto listed = [&](std::string_view n) { return std::find(calls.begin(), calls.end(), n) != calls.end(); };
  bool first = true;
  for (std::size_t i = 0; i < expr.size();) {
    const char c = expr[i];
    if (c == '"') {  // a string: skip it, escapes included
      for (++i; i < expr.size() && expr[i] != '"'; ++i)
        if (expr[i] == '\\') ++i;
      if (i >= expr.size()) return false;
      ++i;
      continue;
    }
    if (c == ';' || (c == ':' && i + 1 < expr.size() && expr[i + 1] == '=')) return false;
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      const std::size_t start = i;
      while (i < expr.size() && (std::isalnum(static_cast<unsigned char>(expr[i])) || expr[i] == '_' || expr[i] == '.')) ++i;
      const std::string_view ident = expr.substr(start, i - start);
      std::size_t j = i;
      while (j < expr.size() && std::isspace(static_cast<unsigned char>(expr[j]))) ++j;
      const bool is_call = j < expr.size() && expr[j] == '(';
      if (first) {  // the expression is one listed call, unqualified
        if (!is_call || !listed(ident) || start != expr.find_first_not_of(" \t\r\n")) return false;
        first = false;
      } else if (is_call) {
        const std::string_view last = ident.substr(ident.rfind('.') == std::string_view::npos ? 0 : ident.rfind('.') + 1);
        const bool record = !last.empty() && std::isupper(static_cast<unsigned char>(last[0]));
        if (!listed(last) && !record && last != "transformation" && last != "iconTransformation") return false;
        if (!record && ident.find('.') != std::string_view::npos) return false;  // OpenModelica.Scripting.x(…)
      }
      continue;
    }
    ++i;
  }
  return !first;
}

// --- values ---

namespace {

struct Parser {
  std::string_view s;
  std::size_t i = 0;

  void ws() {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  }
  OmValue value() {
    ws();
    OmValue v;
    if (i >= s.size()) throw OmcError("omc answer ended early");
    if (s[i] == '{' || s[i] == '(') {  // lists and tuples alike
      const char close = s[i] == '{' ? '}' : ')';
      v.is_list = true;
      ++i;
      ws();
      if (i < s.size() && s[i] == close) {
        ++i;
        return v;
      }
      for (;;) {
        v.items.push_back(value());
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == close) {
          ++i;
          return v;
        }
        throw OmcError("malformed omc answer near: " + std::string(s.substr(i, 40)));
      }
    }
    if (s[i] == '"') {
      v.is_string = true;
      for (++i; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
          const char e = s[++i];
          v.atom += e == 'n' ? '\n' : e == 't' ? '\t' : e;
        } else {
          v.atom += s[i];
        }
      }
      if (i >= s.size()) throw OmcError("unterminated string in omc answer");
      ++i;
      return v;
    }
    // An atom runs to the next separator at this depth; it may hold parentheses itself
    // (a record value such as Placement(…)), kept as written.
    int depth = 0;
    const std::size_t start = i;
    for (; i < s.size(); ++i) {
      const char c = s[i];
      if (c == '"') {
        for (++i; i < s.size() && s[i] != '"'; ++i)
          if (s[i] == '\\') ++i;
      } else if (c == '(' || c == '{') {
        ++depth;
      } else if (c == ')' || c == '}') {
        if (depth == 0) break;
        --depth;
      } else if (c == ',' && depth == 0) {
        break;
      }
    }
    v.atom = trim(std::string(s.substr(start, i - start)));
    return v;
  }
};

}  // namespace

OmValue parse_value(std::string_view text) {
  Parser p{text};
  OmValue v = p.value();
  p.ws();
  if (p.i != text.size()) throw OmcError("trailing text in omc answer: " + std::string(text.substr(p.i, 40)));
  return v;
}

std::string quote(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + '"';
}

}  // namespace squero
