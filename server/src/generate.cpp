#include "squero/generate.hpp"

#include "squero/import.hpp"
#include "squero/squero.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace squero {
namespace {

namespace fs = std::filesystem;

// --- text ---

std::string trim(std::string_view s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw GenerateError("cannot read " + p.string());
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

std::string short_name(std::string_view model) {
  const auto dot = model.rfind('.');
  return std::string(dot == std::string_view::npos ? model : model.substr(dot + 1));
}

std::string snake(std::string_view kebab) {
  std::string s(kebab);
  std::replace(s.begin(), s.end(), '-', '_');
  return s;
}

// A Modelica name as a C++ identifier: `$DER.rotor.phi` → der_rotor_phi, `t.y[1]` → t_y_1.
std::string ident(std::string_view name) {
  std::string out;
  std::string_view n = name;
  if (n.starts_with("$DER.")) out = "der_", n.remove_prefix(5);
  for (const char c : n) {
    if (std::isalnum(static_cast<unsigned char>(c))) out += c;
    else if (c != '$' && !out.empty() && out.back() != '_') out += '_';
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "v_" + out;
  return out;
}

std::optional<double> number(std::string_view s) {
  const std::string t = trim(s);
  if (t == "true") return 1.0;
  if (t == "false") return 0.0;
  double v = 0;
  const char* end = t.data() + t.size();
  auto [p, ec] = std::from_chars(t.data(), end, v);
  if (ec != std::errc() || p != end) return std::nullopt;
  return v;
}

std::string num(double v) {
  char buf[32];
  const auto end = std::to_chars(buf, buf + sizeof buf, v).ptr;  // shortest that reads back the same
  std::string out(buf, end);
  if (out.find_first_of(".eEn") == std::string::npos) out += ".0";
  return out;
}

std::string cpp_string(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out += c;
  }
  return out + '"';
}

std::string toml_string(std::string_view s) { return cpp_string(s); }  // same escapes suffice here

std::string decode(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '&') {
      static const std::pair<std::string_view, char> ents[] = {
          {"&quot;", '"'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&apos;", '\''}};
      bool hit = false;
      for (const auto& [e, c] : ents)
        if (s.substr(i, e.size()) == e) {
          out += c, i += e.size() - 1, hit = true;
          break;
        }
      if (hit) continue;
    }
    out += s[i];
  }
  return out;
}

std::string unquote(std::string s) {
  s = trim(s);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
  return s;
}

// --- the backend DAE (dumpXMLDAE) ---

struct Var {
  int id = 0;
  std::string name, variability, direction, type, unit, bind, start, comment;
  bool fixed = false;
};

struct Dae {
  std::vector<Var> ordered, known, alias;
  std::map<int, std::string> equations;  // id → `lhs = rhs`
  std::map<int, int> solved;             // variable id → equation id
  std::vector<std::vector<int>> blt;     // equation ids per block, in solving order
  std::vector<std::string> events;       // what makes it hybrid
  std::vector<std::string> unsupported;  // equation kinds an ODE can't hold
  std::vector<std::string> externals;    // external functions / tables
};

struct Tag {
  std::string name;
  bool close = false, self = false;
  std::map<std::string, std::string> attrs;
  std::string text;  // up to the next tag
};

// A flat walk over the tags: dumpXMLDAE's output is regular, so no XML library.
template <class Fn>
void scan(std::string_view x, Fn fn) {
  std::size_t i = 0;
  while ((i = x.find('<', i)) != std::string_view::npos) {
    if (x.substr(i, 4) == "<!--") {
      i = x.find("-->", i);
      if (i == std::string_view::npos) return;
      continue;
    }
    std::size_t j = i + 1;
    for (char q = 0; j < x.size() && (q || x[j] != '>'); ++j)
      if (x[j] == '"' || x[j] == '\'') q = q == x[j] ? 0 : q ? q : x[j];
    if (j >= x.size()) return;
    std::string_view body = x.substr(i + 1, j - i - 1);
    if (!body.empty() && (body[0] == '?' || body[0] == '!')) {
      i = j;
      continue;
    }
    Tag t;
    if (!body.empty() && body[0] == '/') t.close = true, body.remove_prefix(1);
    if (!body.empty() && body.back() == '/') t.self = true, body.remove_suffix(1);
    std::size_t k = 0;
    while (k < body.size() && !std::isspace(static_cast<unsigned char>(body[k]))) ++k;
    t.name = body.substr(0, k);
    while (k < body.size()) {
      while (k < body.size() && std::isspace(static_cast<unsigned char>(body[k]))) ++k;
      const std::size_t eq = body.find('=', k);
      if (eq == std::string_view::npos) break;
      const std::string key = trim(body.substr(k, eq - k));
      const char q = body[eq + 1];
      const std::size_t end = body.find(q, eq + 2);
      if (end == std::string_view::npos) break;
      t.attrs[key] = decode(body.substr(eq + 2, end - eq - 2));
      k = end + 1;
    }
    const std::size_t next = x.find('<', j + 1);
    t.text = decode(x.substr(j + 1, (next == std::string_view::npos ? x.size() : next) - j - 1));
    fn(t);
    i = j + 1;
  }
}

Dae parse_dae(std::string_view xml) {
  Dae d;
  std::vector<Var>* list = nullptr;
  Var* var = nullptr;
  bool in_equations = false, in_externals = false, in_function = false;
  int depth = 0;  // inside <equations>
  std::set<int> blocks_seen;
  int block = -1;
  std::string function;
  scan(xml, [&](const Tag& t) {
    const std::string& n = t.name;
    auto attr = [&](const char* k) {
      auto it = t.attrs.find(k);
      return it == t.attrs.end() ? std::string() : it->second;
    };
    if (t.close) {
      if (n == "orderedVariables" || n == "knownVariables" || n == "aliasVariables") list = nullptr;
      else if (n == "externalVariables") in_externals = false;
      else if (n == "variable") var = nullptr;
      else if (n == "function") in_function = false;
      else if (n == "equations") in_equations = false;
      else if (in_equations) --depth;
      return;
    }
    if (in_equations) {
      if (depth == 0 && n == "equation") d.equations[std::stoi(attr("id"))] = trim(t.text);
      else if (depth == 0 && n == "whenEquation") d.events.push_back("a when-clause");
      else if (depth == 0) d.unsupported.push_back("<" + n + ">");
      if (!t.self) ++depth;
      return;
    }
    if (n == "orderedVariables") list = &d.ordered;
    else if (n == "knownVariables") list = &d.known;
    else if (n == "aliasVariables") list = &d.alias;
    else if (n == "externalVariables") in_externals = !t.self;
    else if (n == "equations" && !t.self) in_equations = true, depth = 0;
    else if (n == "variable") {
      if (in_externals) {
        d.externals.push_back(attr("name"));
        return;
      }
      if (!list) return;
      Var v;
      v.id = attr("id").empty() ? 0 : std::stoi(attr("id"));
      v.name = attr("name");
      v.variability = attr("variability");
      v.direction = attr("direction");
      v.type = attr("type");
      v.comment = attr("comment");
      v.fixed = attr("fixed") == "true";
      list->push_back(std::move(v));
      var = t.self ? nullptr : &list->back();
      if (list == &d.ordered && list->back().variability == "discrete")
        d.events.push_back("discrete variable " + list->back().name);
    } else if (var && n == "bindExpression") var->bind = attr("string");
    else if (var && n == "unit") var->unit = unquote(attr("string"));
    else if (var && n == "initialValue") var->start = attr("string");
    else if (var && n == "fixed") var->fixed = attr("string") == "true";
    else if (n == "zeroCrossingElement") d.events.push_back("a zero crossing (" + attr("string") + ")");
    else if (n == "function") in_function = true, function = attr("id").empty() ? attr("name") : attr("id");
    else if (in_function && n == "ModelicaImplementation" && t.text.find("external") != std::string::npos)
      d.externals.push_back("external function " + function);
    else if (n == "solvedIn") d.solved[std::stoi(attr("variableId"))] = std::stoi(attr("equationId"));
    else if (n == "bltBlock") {
      const int id = std::stoi(attr("id"));
      block = blocks_seen.insert(id).second ? id : -1;  // the dump repeats the BLT; keep one
      if (block >= 0) d.blt.emplace_back();
    } else if (block >= 0 && n == "involvedEquation") d.blt.back().push_back(std::stoi(attr("equationId")));
  });
  return d;
}

// --- Modelica expressions → C++ ---

struct Tok {
  enum Kind { Num, Id, Sym, End } kind = End;
  std::string s;
};

std::vector<Tok> lex(std::string_view e) {
  std::vector<Tok> out;
  for (std::size_t i = 0; i < e.size();) {
    const char c = e[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
    } else if (std::isdigit(static_cast<unsigned char>(c)) ||
               (c == '.' && i + 1 < e.size() && std::isdigit(static_cast<unsigned char>(e[i + 1])))) {
      std::size_t j = i;
      while (j < e.size() && (std::isdigit(static_cast<unsigned char>(e[j])) || e[j] == '.')) ++j;
      if (j < e.size() && (e[j] == 'e' || e[j] == 'E')) {
        ++j;
        if (j < e.size() && (e[j] == '+' || e[j] == '-')) ++j;
        while (j < e.size() && std::isdigit(static_cast<unsigned char>(e[j]))) ++j;
      }
      out.push_back({Tok::Num, std::string(e.substr(i, j - i))});
      i = j;
    } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
      std::size_t j = i;
      while (j < e.size()) {
        const char d = e[j];
        if (std::isalnum(static_cast<unsigned char>(d)) || d == '_' || d == '$') {
          ++j;
        } else if (d == '.' && j + 1 < e.size() &&
                   (std::isalpha(static_cast<unsigned char>(e[j + 1])) || e[j + 1] == '_' || e[j + 1] == '$')) {
          ++j;
        } else if (d == '[') {  // a scalarised element: x[1], m[2, 3]
          const std::size_t close = e.find(']', j);
          if (close == std::string_view::npos) throw GenerateError("unclosed [ in `" + std::string(e) + "`");
          j = close + 1;
        } else {
          break;
        }
      }
      out.push_back({Tok::Id, std::string(e.substr(i, j - i))});
      i = j;
    } else {
      static const std::string_view two[] = {"<=", ">=", "==", "<>"};
      std::string s(1, c);
      for (auto t : two)
        if (e.substr(i, 2) == t) s = std::string(t);
      if (s.size() == 1 && std::string_view("+-*/^()<>,").find(c) == std::string_view::npos)
        throw GenerateError("can't translate `" + std::string(1, c) + "` in `" + std::string(e) + "`");
      out.push_back({Tok::Sym, s});
      i += s.size();
    }
  }
  out.push_back({Tok::End, ""});
  return out;
}

class Translator {
 public:
  Translator(std::string_view src, const std::function<std::string(std::string_view, bool)>& name)
      : src_(src), t_(lex(src)), name_(name) {}

  std::string run() {
    std::string out = expr();
    if (peek().kind != Tok::End) fail("unexpected `" + peek().s + "`");
    return out;
  }

 private:
  [[noreturn]] void fail(const std::string& what) const {
    throw GenerateError(what + " in `" + std::string(src_) + "`");
  }
  const Tok& peek() const { return t_[i_]; }
  bool at(std::string_view s) const { return (peek().kind == Tok::Sym || peek().kind == Tok::Id) && peek().s == s; }
  bool eat(std::string_view s) {
    if (!at(s)) return false;
    ++i_;
    return true;
  }
  void want(std::string_view s) {
    if (!eat(s)) fail("expected `" + std::string(s) + "`");
  }

  std::string expr() {
    if (!eat("if")) return logical_or();
    std::string out = "(" + expr() + " ? ";
    want("then");
    out += expr() + " : ";
    int open = 1;
    while (eat("elseif")) {
      out += "(" + expr() + " ? ";
      want("then");
      out += expr() + " : ";
      ++open;
    }
    want("else");
    out += expr();
    return out + std::string(static_cast<std::size_t>(open), ')');
  }
  std::string logical_or() {
    std::string a = logical_and();
    while (eat("or")) a = "(" + a + " || " + logical_and() + ")";
    return a;
  }
  std::string logical_and() {
    std::string a = logical_not();
    while (eat("and")) a = "(" + a + " && " + logical_not() + ")";
    return a;
  }
  std::string logical_not() {
    if (eat("not")) return "(!" + logical_not() + ")";
    return relation();
  }
  std::string relation() {
    std::string a = arithmetic();
    for (std::string_view op : {"<=", ">=", "==", "<>", "<", ">"}) {
      if (eat(op)) return "(" + a + " " + (op == "<>" ? "!=" : std::string(op)) + " " + arithmetic() + ")";
    }
    return a;
  }
  std::string arithmetic() {
    std::string a;
    if (eat("-")) a = "(-" + term() + ")";
    else {
      eat("+");
      a = term();
    }
    for (;;) {
      if (eat("+")) a = "(" + a + " + " + term() + ")";
      else if (eat("-")) a = "(" + a + " - " + term() + ")";
      else return a;
    }
  }
  std::string term() {
    std::string a = factor();
    for (;;) {
      if (eat("*")) a = "(" + a + " * " + factor() + ")";
      else if (eat("/")) a = "(" + a + " / " + factor() + ")";
      else return a;
    }
  }
  std::string factor() {
    std::string a = primary();
    if (eat("^")) return "std::pow(" + a + ", " + primary() + ")";
    return a;
  }
  std::vector<std::string> args() {
    want("(");
    std::vector<std::string> out;
    if (eat(")")) return out;
    do out.push_back(expr());
    while (eat(","));
    want(")");
    return out;
  }
  std::string primary() {
    const Tok tok = peek();
    if (tok.kind == Tok::Num) {
      ++i_;
      std::string s = tok.s;
      if (s.find_first_of(".eE") == std::string::npos) s += ".0";  // 1/2 is 0.5 in Modelica
      else if (s.front() == '.') s = "0" + s;
      return s;
    }
    if (eat("(")) {
      std::string a = expr();
      want(")");
      return a;
    }
    if (tok.kind != Tok::Id) fail("unexpected `" + tok.s + "`");
    ++i_;
    const std::string& id = tok.s;
    if (id == "true") return "1.0";
    if (id == "false") return "0.0";
    if (id == "time") return "t";
    if (!at("(")) return name_(id, false);
    if (id == "der") {
      want("(");
      const Tok v = peek();
      if (v.kind != Tok::Id) fail("der() of an expression");
      ++i_;
      want(")");
      return name_(v.s, true);
    }
    static const std::map<std::string_view, std::pair<std::string_view, std::size_t>> fns = {
        {"sin", {"std::sin", 1}},   {"cos", {"std::cos", 1}},     {"tan", {"std::tan", 1}},
        {"asin", {"std::asin", 1}}, {"acos", {"std::acos", 1}},   {"atan", {"std::atan", 1}},
        {"atan2", {"std::atan2", 2}}, {"sinh", {"std::sinh", 1}}, {"cosh", {"std::cosh", 1}},
        {"tanh", {"std::tanh", 1}}, {"exp", {"std::exp", 1}},     {"log", {"std::log", 1}},
        {"log10", {"std::log10", 1}}, {"sqrt", {"std::sqrt", 1}}, {"abs", {"std::abs", 1}},
        {"min", {"std::min", 2}},   {"max", {"std::max", 2}}};
    std::vector<std::string> a = args();
    auto arity = [&](std::size_t n) {
      if (a.size() != n) fail(id + "() with " + std::to_string(a.size()) + " arguments");
    };
    if (const auto it = fns.find(id); it != fns.end()) {
      arity(it->second.second);
      std::string out = std::string(it->second.first) + "(";
      for (std::size_t k = 0; k < a.size(); ++k) out += (k ? ", " : "") + a[k];
      return out + ")";
    }
    if (id == "noEvent") return arity(1), a[0];
    if (id == "smooth") return arity(2), a[1];
    if (id == "homotopy") return arity(2), a[0];  // the actual expression
    if (id == "sign") return arity(1), "((" + a[0] + ") > 0.0 ? 1.0 : ((" + a[0] + ") < 0.0 ? -1.0 : 0.0))";
    fail("calls `" + id + "()`");
  }

  std::string_view src_;
  std::vector<Tok> t_;
  std::size_t i_ = 0;
  const std::function<std::string(std::string_view, bool)>& name_;
};

// `a = b` split at the equation's `=` (not ==, <=, >=).
std::optional<std::pair<std::string, std::string>> split_equation(std::string_view eq) {
  int depth = 0;
  for (std::size_t i = 0; i < eq.size(); ++i) {
    const char c = eq[i];
    if (c == '(' || c == '[' || c == '{') ++depth;
    else if (c == ')' || c == ']' || c == '}') --depth;
    else if (c == '"') return std::nullopt;
    else if (c == '=' && depth == 0) {
      const char before = i ? eq[i - 1] : ' ', after = i + 1 < eq.size() ? eq[i + 1] : ' ';
      if (before == '<' || before == '>' || before == '=' || before == ':' || after == '=') continue;
      return std::pair{trim(eq.substr(0, i)), trim(eq.substr(i + 1))};
    }
  }
  return std::nullopt;
}

// --- the station's interface, and the ODE ---

struct Prop {
  std::string id, unit;
  double value = 0;
};
struct Port {
  std::string id, unit, modelica;
};

struct Spec {
  std::string model, name, id, cls, label, description;
  std::vector<Prop> props;
  std::vector<Port> inputs, outputs;
  // ODE only
  std::vector<std::string> param_fields, param_lines;  // Params, computed by params_of()
  std::vector<std::string> var_fields, eq_lines;       // Vars, computed by eval()
  std::vector<std::string> start_exprs, der_fields;    // per state
  std::vector<std::string> output_exprs;
  std::vector<std::string> notices;
  std::optional<std::string> ode_blocker;  // why this can't be an ODE
  nlohmann::json icon = nlohmann::json::object();
  std::map<std::string, ConnectorDef> connectors;
};

bool top_level(const Var& v) { return v.name.find('.') == std::string::npos && v.name.find('$') == std::string::npos; }

// `components`: the model's top-level components in declaration order; probes among them;
// `hidden`: the protected ones (private variables: computed inside, never props).
Spec build(const Dae& d, const std::string& model, const std::vector<std::string>& components,
           const std::set<std::string>& probes, const std::set<std::string>& hidden) {
  Spec s;
  s.model = model;
  s.label = short_name(model);
  s.name = station_name(model);
  s.id = snake(s.name);
  s.cls = ident(s.label);

  for (const auto& e : d.events) {
    s.ode_blocker = "it has events (" + e + ")";
    break;
  }
  if (!s.ode_blocker && !d.externals.empty()) s.ode_blocker = "it calls externals (" + d.externals.front() + ")";
  if (!s.ode_blocker && !d.unsupported.empty()) s.ode_blocker = "it has " + d.unsupported.front() + " equations";
  for (const auto& b : d.blt)
    if (!s.ode_blocker && b.size() > 1)
      s.ode_blocker = "equations " + std::to_string(b.front()) + "… form a torn system of " + std::to_string(b.size());

  std::map<std::string, const Var*> ordered, known, alias;
  std::map<int, const Var*> by_id;
  std::map<std::string, std::size_t> state_index;
  for (const auto& v : d.ordered) {
    ordered[v.name] = &v;
    by_id[v.id] = &v;
    if (v.variability == "continuousState") state_index.emplace(v.name, state_index.size());
  }
  for (const auto& v : d.known) known[v.name] = &v;
  for (const auto& v : d.alias) alias[v.name] = &v;

  // C++ names, unique.
  std::map<std::string, std::string> idents;
  // Fields of Vars and Params (v.x, p_.k), so only C++ keywords are off limits.
  std::set<std::string> taken = {"and", "auto", "bool", "break", "case", "catch", "char", "class", "const",
                                 "continue", "default", "delete", "do", "double", "else", "enum", "explicit",
                                 "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int",
                                 "long", "namespace", "new", "not", "operator", "or", "private", "protected",
                                 "public", "register", "return", "short", "signed", "sizeof", "static",
                                 "struct", "switch", "template", "this", "throw", "true", "try", "typedef",
                                 "union", "unsigned", "using", "virtual", "void", "volatile", "while"};
  auto cname = [&](const std::string& n) -> const std::string& {
    auto it = idents.find(n);
    if (it != idents.end()) return it->second;
    std::string c = ident(n), base = c;
    if (taken.contains(c)) base = c += "_";
    for (int k = 2; !taken.insert(c).second; ++k) c = base + std::to_string(k);
    return idents[n] = c;
  };

  // Props: the top-level parameters with a value, then the fixed states' starts.
  std::set<std::string> prop_vars;
  for (const auto& v : d.known) {
    if (!top_level(v) || v.variability != "parameter" || v.direction == "input" || hidden.contains(v.name)) continue;
    if (v.type != "Real" && v.type != "Integer" && v.type != "Boolean") continue;
    const auto value = number(v.bind.empty() ? v.start : v.bind);
    if (!value) {
      s.notices.push_back("parameter " + v.name + " = " + v.bind + " is computed, not a prop");
      continue;
    }
    s.props.push_back({v.name, v.unit, *value});
    prop_vars.insert(v.name);
  }

  std::stable_sort(s.props.begin(), s.props.end(), [&](const Prop& a, const Prop& b) {
    auto rank = [&](const Prop& p) { return std::find(components.begin(), components.end(), p.id) - components.begin(); };
    return rank(a) < rank(b);
  });

  for (const auto& v : d.known)
    if (top_level(v) && v.direction == "input") s.inputs.push_back({v.name, v.unit, v.name});
  for (const auto& v : d.ordered)  // an input the backend kept as an unknown
    if (top_level(v) && v.direction == "input") s.inputs.push_back({v.name, v.unit, v.name});
  auto by_declaration = [&](std::vector<Port>& ports) {
    auto rank = [&](const Port& p) { return std::find(components.begin(), components.end(), p.modelica) - components.begin(); };
    std::stable_sort(ports.begin(), ports.end(), [&](const Port& a, const Port& b) { return rank(a) < rank(b); });
  };
  by_declaration(s.inputs);
  std::set<std::string> input_names;
  for (const auto& p : s.inputs) input_names.insert(p.id);

  for (const auto* list : {&d.ordered, &d.alias, &d.known})
    for (const auto& v : *list)
      if (top_level(v) && (v.direction == "output" || probes.contains(v.name)))
        s.outputs.push_back({v.name, v.unit, v.name});
  by_declaration(s.outputs);

  std::set<std::string> needed;  // known variables the C++ reads
  std::set<std::string>* reads = nullptr;  // and those one binding reads, while emitting it
  int alias_depth = 0;
  std::function<std::string(std::string_view, bool)> spell = [&](std::string_view sv, bool der) -> std::string {
    const std::string n(sv);
    if (der) {
      if (state_index.contains(n)) return "v.der_" + cname(n);
      if (ordered.contains("$DER." + n)) return spell("$DER." + n, false);
      throw GenerateError("der(" + n + ") of a non-state");
    }
    if (auto it = state_index.find(n); it != state_index.end()) return "x[" + std::to_string(it->second) + "]";
    if (ordered.contains(n)) return "v." + cname(n);
    if (input_names.contains(n)) return "v." + cname(n);
    if (known.contains(n)) {
      needed.insert(n);
      if (reads) reads->insert(n);
      return "p_." + cname(n);
    }
    if (auto it = alias.find(n); it != alias.end()) {
      if (++alias_depth > 32) throw GenerateError("alias loop at " + n);
      std::string out = "(" + to_cpp(it->second->bind, spell) + ")";
      --alias_depth;
      return out;
    }
    throw GenerateError("unknown variable " + n);
  };

  try {
    for (const auto& p : s.inputs) s.var_fields.push_back(cname(p.modelica));
    for (const auto& block : d.blt) {
      if (s.ode_blocker) break;
      const int eq_id = block.front();
      int var_id = -1;
      for (const auto& [v, e] : d.solved)
        if (e == eq_id) var_id = v;
      const Var* var = by_id.contains(var_id) ? by_id[var_id] : nullptr;
      const auto text = d.equations.contains(eq_id) ? d.equations.at(eq_id) : std::string();
      const auto sides = split_equation(text);
      if (!var || !sides) {
        s.ode_blocker = "equation " + std::to_string(eq_id) + " isn't an assignment";
        break;
      }
      const bool state = state_index.contains(var->name);
      const std::string target = state ? "der(" + var->name + ")" : var->name;
      std::string rhs;
      if (sides->first == target) rhs = sides->second;
      else if (sides->second == target) rhs = sides->first;
      else {
        s.ode_blocker = "equation `" + text + "` is implicit in " + var->name;
        break;
      }
      const std::string field = state ? "der_" + cname(var->name) : cname(var->name);
      s.var_fields.push_back(field);
      s.eq_lines.push_back("v." + field + " = " + to_cpp(rhs, spell) + ";");
    }
    if (!s.ode_blocker) {
      for (const auto& o : s.outputs) s.output_exprs.push_back(spell(o.modelica, false));
      for (const auto& v : d.ordered) {
        if (!state_index.contains(v.name)) continue;
        s.der_fields.push_back("der_" + cname(v.name));
        const auto value = number(v.start);
        if (v.fixed && value) {
          const std::string id = cname(v.name) + "_start";
          s.props.push_back({id, v.unit, *value});
          s.start_exprs.push_back("get_or(in, " + cpp_string(id) + ")");
        } else {
          s.start_exprs.push_back(v.start.empty() ? "0.0" : to_cpp(v.start, spell));
          if (!v.fixed)
            s.notices.push_back("state " + v.name + " starts at its start value " +
                                (v.start.empty() ? "0" : v.start) + " (initial equations aren't solved)");
        }
      }
      // Parameters in dependency order: each after what its binding reads.
      std::set<std::string> done, visiting;
      std::function<void(const std::string&)> emit = [&](const std::string& n) {
        if (done.contains(n)) return;
        if (!visiting.insert(n).second) throw GenerateError("parameter binding loop at " + n);
        const Var& v = *known.at(n);
        std::string value;
        if (prop_vars.contains(n)) {
          value = "get_or(in, " + cpp_string(n) + ")";
        } else {
          const std::string expr = v.bind.empty() ? v.start : v.bind;
          if (expr.empty()) s.notices.push_back(n + " has no value; using 0");
          std::set<std::string> mine, *outer = reads;
          reads = &mine;
          value = expr.empty() ? "0.0" : to_cpp(expr, spell);
          reads = outer;
          for (const auto& m : mine) emit(m);
        }
        visiting.erase(n);
        done.insert(n);
        s.param_fields.push_back(cname(n));
        s.param_lines.push_back("p_." + cname(n) + " = " + value + ";");
      };
      for (const std::set<std::string> first = needed; const auto& n : first) emit(n);
      for (bool more = true; more;) {  // bindings may have pulled in more
        more = false;
        for (const std::set<std::string> now = needed; const auto& n : now)
          if (!done.contains(n)) emit(n), more = true;
      }
    }
  } catch (const GenerateError& e) {
    s.ode_blocker = e.what();
  }
  if (s.ode_blocker) s.param_lines.clear(), s.eq_lines.clear();
  return s;
}

// --- files ---

void write(GenerateResult& r, const fs::path& rel, const std::string& text, bool once = false) {
  if (r.dry) {
    r.files[rel.generic_string()] = text;
    return;
  }
  const fs::path p = r.out / rel;
  if (once && fs::exists(p)) return;
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out << text;
  if (!out) throw GenerateError("cannot write " + p.string());
  r.written.push_back(rel);
}

std::string header(const Spec& s, std::string_view path) {
  return "// Generated by `paperboat modelica generate` from modelica/" + s.label + ".mo (path: " +
         std::string(path) + ").\n// Regenerate rather than edit: the next generate rewrites this file.\n";
}

std::string props_cpp(const Spec& s) {
  std::string out;
  for (const auto& p : s.props) {
    out += out.empty() ? "                 " : ",\n                 ";
    out += "{.id = " + cpp_string(p.id) + ", .default_value = " + num(p.value) +
           (p.unit.empty() ? "" : ", .unit = " + cpp_string(p.unit)) + "}";
  }
  return out + "},\n";
}

std::string ports_cpp(const std::vector<Port>& ports) {
  std::string out;
  for (std::size_t i = 0; i < ports.size(); ++i)
    out += std::string(i ? ", " : "") + "{" + cpp_string(ports[i].id) + ", \"signal\", " + cpp_string(ports[i].unit) + "}";
  return out;
}

std::string register_cpp(const Spec& s) {
  std::string conns;
  for (const auto& [cname, cdef] : s.connectors) {
    conns += "  registry.add_connector(" + cpp_string(s.name) + ", " + cpp_string(cname) +
             ", nlohmann::json::parse(" + cpp_string(to_json(cdef).dump()) + "));\n";
  }
  const std::string view_line =
      (!s.icon.is_null() && !s.icon.empty())
          ? ",\n                                    .view = {.icon = nlohmann::json::parse(" +
                cpp_string(s.icon.dump()) + ")}"
          : "";
  return "void register_" + s.id + "(paperboat::Registry& registry) {\n" + conns +
         "  registry.add(paperboat::define_object<" + s.cls + ">(\n"
         "      " + cpp_string(s.name) + ",\n"
         "      {.type = " + cpp_string(s.name) + ",\n"
         "       .label = " + cpp_string(s.label) + ",\n"
         "       .role = \"body\",\n"
         "       .props = {\n" + props_cpp(s) + "       .links = {},\n"
         "       .block = paperboat::BlockDef{.type = " + cpp_string(s.name) + ",\n"
         "                                    .label = " + cpp_string(s.label) + ",\n"
         "                                    .kind = paperboat::Kind::Source,\n"
         "                                    .inputs = {" + ports_cpp(s.inputs) + "},\n"
         "                                    .outputs = {" + ports_cpp(s.outputs) + "},\n"
         "                                    .params = {},\n"
         "                                    .feedthrough = false" + view_line + "},\n"
         "       .description = " + cpp_string(s.description.empty() ? "Generated from the Modelica model " + s.model : s.description) +
         "}));\n}\n";
}

std::string fields(const std::vector<std::string>& names) {
  std::string out;
  for (const auto& n : names) out += "  double " + n + " = 0.0;\n";
  return out;
}

std::string ode_cpp(const Spec& s) {
  std::string read = "    return {";
  for (std::size_t i = 0; i < s.outputs.size(); ++i)
    read += std::string(i ? ", " : "") + "{" + cpp_string(s.outputs[i].id) + ", " + s.output_exprs[i] + "}";
  read += "};\n";
  std::string inputs, eqs, params, starts, ders;
  for (const auto& p : s.inputs) inputs += "    v." + ident(p.modelica) + " = get_or(in, " + cpp_string(p.id) + ");\n";
  for (const auto& l : s.eq_lines) eqs += "    " + l + "\n";
  for (const auto& l : s.param_lines) params += "  " + l + "\n";
  for (std::size_t i = 0; i < s.start_exprs.size(); ++i) starts += std::string(i ? ", " : "") + s.start_exprs[i];
  for (std::size_t i = 0; i < s.der_fields.size(); ++i) ders += std::string(i ? ", " : "") + "v." + s.der_fields[i];
  return header(s, "ode") + "\n#include \"" + s.id + ".hpp\"\n\n#include <algorithm>\n#include <cmath>\n\nnamespace " + s.id +
         " {\nnamespace {\n\nusing paperboat::Values;\nusing paperboat::get_or;\nusing paperboat::solve::State;\n\n"
         "// " + s.label + "'s parameters and constants, computed once from the props.\nstruct Params {\n" +
         fields(s.param_fields) + "};\n\n"
         "// One pass over the equations: the inputs, then each unknown in solving order.\nstruct Vars {\n" +
         fields(s.var_fields) + "};\n\n"
         "[[maybe_unused]] Params params_of([[maybe_unused]] const Values& in) {\n  Params p_;\n" + params +
         "  return p_;\n}\n\n"
         "State start([[maybe_unused]] const Values& in, [[maybe_unused]] const Params& p_) { return {" + starts + "}; }\n\n"
         "class " + s.cls + " final : public paperboat::Model {\n public:\n"
         "  explicit " + s.cls + "(const Values& props) : " + s.cls + "(props, params_of(props)) {}\n\n"
         "  State derivatives(double t, const State& x, const Values& in) const override {\n"
         "    const Vars v = eval(t, x, in);\n    return {" + ders + "};\n  }\n\n"
         "  // The inputs hold over the step; read() sees the last ones.\n"
         "  void step(const paperboat::Context& ctx, const Values& in) override {\n"
         "    in_ = in;\n    Model::step(ctx, in);\n    t_ = ctx.t + ctx.dt;\n  }\n\n"
         "  Values read() const override {\n    [[maybe_unused]] const State& x = this->x();\n"
         "    [[maybe_unused]] const Vars v = eval(t_, x, in_);\n" + read + "  }\n\n"
         "  std::vector<paperboat::Entity> entities(const std::string&, double) const override { return {}; }\n\n"
         " private:\n"
         "  " + s.cls + "(const Values& props, Params p) : Model(start(props, p)), p_(p) {}  // the run's solver (Run menu)\n\n"
         "  Vars eval([[maybe_unused]] double t, [[maybe_unused]] const State& x, [[maybe_unused]] const Values& in) const {\n"
         "    Vars v;\n" + inputs + eqs + "    return v;\n  }\n\n"
         "  Params p_;\n  Values in_;\n  double t_ = 0.0;\n};\n\n}  // namespace\n\n" + register_cpp(s) +
         "\n}  // namespace " + s.id + "\n";
}

// The FMU: its interface by name from modelDescription.xml.
struct FmuVar {
  unsigned vr = 0;
  std::string type;
};
struct Fmu {
  std::string guid, model_identifier;
  std::map<std::string, FmuVar> vars;
};

Fmu read_model_description(const std::string& xml) {
  Fmu f;
  std::string current;
  scan(xml, [&](const Tag& t) {
    if (t.close) return;
    auto get = [&](const char* k) { return t.attrs.contains(k) ? t.attrs.at(k) : std::string(); };
    if (t.name == "fmiModelDescription") f.guid = get("guid");
    else if (t.name == "CoSimulation") f.model_identifier = get("modelIdentifier");
    else if (t.name == "ScalarVariable") {
      current = get("name");
      f.vars[current].vr = static_cast<unsigned>(std::stoul(get("valueReference")));
    } else if (!current.empty() && (t.name == "Real" || t.name == "Integer" || t.name == "Boolean" || t.name == "String")) {
      f.vars[current].type = t.name;
      current.clear();
    }
  });
  return f;
}

// The MultiBody shapes (visual_shapes(), crefs already FMU variable names) as the adapter's
// table: each value a constant or a value reference, read through the FMU every frame.
std::string shapes_cpp(const nlohmann::json& shapes, const Fmu& f, std::vector<std::string>& notices) {
  std::string rows;
  std::size_t n = 0;
  for (const auto& sh : shapes) {
    const std::string ident = sh["ident"];
    if (ident.starts_with("world.")) continue;  // the World's arrows and letters
    std::string slots;
    const auto slot = [&](const nlohmann::json& v) {
      std::string text;
      if (v.is_number()) text = "{false, 0u, " + std::to_string(v.get<double>()) + "}";
      else if (const auto it = f.vars.find(v.get<std::string>()); it != f.vars.end()) text = "{true, " + std::to_string(it->second.vr) + "u, 0.0}";
      else {
        notices.push_back("the FMU has no " + v.get<std::string>() + "; its shape uses 0");
        text = "{false, 0u, 0.0}";
      }
      slots += (slots.empty() ? "" : ", ") + text;
    };
    for (const char* k : {"T", "r", "r_shape", "lengthDir", "widthDir"})
      for (std::size_t i = 0; i < (std::string_view(k) == "T" ? 9u : 3u); ++i) slot(i < sh[k].size() ? sh[k][i] : nlohmann::json(0.0));
    for (const char* k : {"length", "width", "height"}) slot(sh[k]);
    char color[8] = "#888888";
    if (sh["color"].size() == 3 && sh["color"][0].is_number() && sh["color"][1].is_number() && sh["color"][2].is_number())
      std::snprintf(color, sizeof color, "#%02x%02x%02x", std::clamp(sh["color"][0].get<int>(), 0, 255), std::clamp(sh["color"][1].get<int>(), 0, 255),
                    std::clamp(sh["color"][2].get<int>(), 0, 255));
    const std::string type = sh["type"];
    const char* kind = type == "sphere" ? "1" : (type == "cylinder" || type == "pipecylinder" || type == "pipe" || type == "gearwheel" || type == "spring" || type == "cone") ? "2" : "0";
    rows += "    Look{" + cpp_string(ident) + ", " + kind + ", " + cpp_string(color) + ", {{" + slots + "}}},\n";
    ++n;
  }
  if (!n) return "";
  return R"(
// Its MultiBody shapes (omc's _visual.xml): T (world → frame, row-major), r, r_shape, lengthDir,
// widthDir, length, width, height; each a constant or an FMU variable.
struct Slot {
  bool var;
  Ref vr;
  double k;
};
struct Look {
  const char* id;
  int kind;  // 0 box, 1 sphere, 2 cylinder (cones, pipes, springs too); ponytail: CAD parts draw as their box
  const char* color;
  std::array<Slot, 24> s;
};
const std::array<Look, )" + std::to_string(n) + R"(> kShapes = {{
)" + rows + "}};\n";
}

// The C++ that turns kShapes into Station entities, in the adapter's entities().
constexpr const char* kShapeEntities = R"(
  std::vector<paperboat::Entity> entities(const std::string& id, double) const override {
    std::vector<paperboat::Entity> out;
    for (const auto& look : kShapes) {
      std::array<double, 24> v{};
      for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = look.s[i].k;
        if (look.s[i].var) get_real_(c_, &look.s[i].vr, 1, &v[i]);  // ponytail: one call a value; batch if it shows
      }
      using V = std::array<double, 3>;
      const auto dot = [](const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
      const auto unit = [](V a) {
        const double n = std::hypot(a[0], a[1], a[2]);
        return n > 1e-12 ? V{a[0] / n, a[1] / n, a[2] / n} : V{0, 0, 0};
      };
      const auto cross = [](const V& a, const V& b) { return V{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
      const double* T = v.data();
      const auto world = [&](const V& u) {  // Tᵀu: a frame vector in world
        return V{T[0] * u[0] + T[3] * u[1] + T[6] * u[2], T[1] * u[0] + T[4] * u[1] + T[7] * u[2], T[2] * u[0] + T[5] * u[1] + T[8] * u[2]};
      };
      const V r{v[9], v[10], v[11]}, rs{v[12], v[13], v[14]}, ld{v[15], v[16], v[17]}, wd{v[18], v[19], v[20]};
      const double L = v[21], W = v[22], H = v[23];
      const V ex = unit(ld);
      const double k = dot(wd, ex);
      V ey = unit(V{wd[0] - k * ex[0], wd[1] - k * ex[1], wd[2] - k * ex[2]});
      if (std::hypot(ey[0], ey[1], ey[2]) == 0) ey = unit(cross(ex, std::abs(ex[0]) < 0.9 ? V{1, 0, 0} : V{0, 1, 0}));
      const V X = world(ex), Y = world(ey), Z = world(cross(ex, ey)), p = world(rs);
      // the shape runs from its origin along the length: its centre is half a length on
      paperboat::Entity e;
      e.id = id + "/" + look.id;
      e.color = look.color;
      e.pos = {r[0] + p[0] + X[0] * L / 2, r[1] + p[1] + X[1] * L / 2, r[2] + p[2] + X[2] * L / 2};
      // a rotation with columns a, b, c (unit) as Euler angles, x then y then z (three.js's XYZ order)
      const auto euler = [](const V& a, const V& b, const V& c) {
        const double s13 = std::clamp(c[0], -1.0, 1.0);
        if (std::abs(s13) < 0.9999999) return V{std::atan2(-c[1], c[2]), std::asin(s13), std::atan2(-b[0], a[0])};
        return V{std::atan2(b[2], b[1]), std::asin(s13), 0.0};
      };
      const auto u = [&](const V& a) { return unit(a); };
      if (look.kind == 1) {
        e.shape = "sphere";
        e.size = {L / 2, L / 2, L / 2};
      } else if (look.kind == 2) {
        // the Station's cylinder runs along its y: length on y, the width the diameter
        e.shape = "cylinder";
        e.size = {W / 2, L, W / 2};
        const V nz = u(Z);
        e.rot = euler(u(Y), u(X), V{-nz[0], -nz[1], -nz[2]});
      } else {
        e.size = {L, W, H};
        e.rot = euler(u(X), u(Y), u(Z));
      }
      out.push_back(e);
    }
    return out;
  }
)";

std::string fmu_cpp(const Spec& s, const Fmu& f, std::vector<std::string>& notices, const nlohmann::json& shapes = nlohmann::json::array()) {
  const std::string table = shapes_cpp(shapes, f, notices);
  auto refs = [&](const std::vector<std::string>& names, const char* what) {
    std::string out;
    for (const auto& n : names) {
      const auto it = f.vars.find(n);
      if (it == f.vars.end()) throw GenerateError(std::string("the FMU has no ") + what + " " + n);
      if (it->second.type != "Real") notices.push_back(n + " is " + it->second.type + "; passed as Real");
      out += (out.empty() ? "" : ", ") + std::to_string(it->second.vr) + "u";
    }
    return "{" + out + "}";
  };
  std::vector<std::string> prop_names, prop_ids, in_names, out_names;
  for (const auto& p : s.props) {
    prop_ids.push_back(p.id);
    // a state's start prop "load_w_start" sets the FMU's "load.w"
    std::string n = p.id;
    if (n.ends_with("_start") && !f.vars.contains(n)) {
      for (const auto& [name, var] : f.vars)
        if (ident(name) + "_start" == n) n = name;
    }
    prop_names.push_back(n);
  }
  for (const auto& p : s.inputs) in_names.push_back(p.modelica);
  for (const auto& p : s.outputs) out_names.push_back(p.modelica);
  auto ids = [](const std::vector<std::string>& v) {
    std::string out;
    for (const auto& n : v) out += (out.empty() ? "" : ", ") + cpp_string(n);
    return "{" + out + "}";
  };
  auto out_ids = std::vector<std::string>();
  for (const auto& p : s.outputs) out_ids.push_back(p.id);
  auto in_ids = std::vector<std::string>();
  for (const auto& p : s.inputs) in_ids.push_back(p.id);

  return header(s, "fmu") + R"(
#include ")" + s.id + R"(.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace )" + s.id + R"( {
namespace {

using paperboat::Values;
using paperboat::get_or;

// FMI 2.0 co-simulation, the calls this adapter makes (the standard's C ABI).
using Component = void*;
using Ref = unsigned int;
struct Callbacks {
  void (*logger)(void*, const char*, int, const char*, const char*, ...);
  void* (*allocate)(std::size_t, std::size_t);
  void (*free)(void*);
  void (*step_finished)(void*, int);
  void* env;
};
void log_nothing(void*, const char*, int, const char*, const char*, ...) {}

constexpr const char* kLibrary = PAPERBOAT_FMU_DIR "/binaries/" PAPERBOAT_FMU_PLATFORM "/)" + f.model_identifier + R"(" PAPERBOAT_FMU_SUFFIX;
constexpr const char* kResources = "file://" PAPERBOAT_FMU_DIR "/resources";
constexpr const char* kGuid = )" + cpp_string(f.guid) + R"(;

constexpr std::array<const char*, )" + std::to_string(prop_ids.size()) + R"(> kPropIds = )" + ids(prop_ids) + R"(;
constexpr std::array<Ref, )" + std::to_string(prop_ids.size()) + R"(> kPropRefs = )" + refs(prop_names, "parameter") + R"(;
constexpr std::array<const char*, )" + std::to_string(in_ids.size()) + R"(> kInputIds = )" + ids(in_ids) + R"(;
constexpr std::array<Ref, )" + std::to_string(in_ids.size()) + R"(> kInputRefs = )" + refs(in_names, "input") + R"(;
constexpr std::array<const char*, )" + std::to_string(out_ids.size()) + R"(> kOutputIds = )" + ids(out_ids) + R"(;
constexpr std::array<Ref, )" + std::to_string(out_ids.size()) + R"(> kOutputRefs = )" + refs(out_names, "output") + R"(;
)" + table + R"(
class )" + s.cls + R"( final : public paperboat::StationObject {
 public:
  explicit )" + s.cls + R"((const Values& props) {
    lib_ = dlopen(kLibrary, RTLD_NOW | RTLD_LOCAL);
    if (!lib_) throw std::runtime_error(std::string("cannot load the FMU: ") + dlerror());
    load(instantiate_, "fmi2Instantiate");
    load(setup_, "fmi2SetupExperiment");
    load(enter_init_, "fmi2EnterInitializationMode");
    load(exit_init_, "fmi2ExitInitializationMode");
    load(set_real_, "fmi2SetReal");
    load(get_real_, "fmi2GetReal");
    load(do_step_, "fmi2DoStep");
    load(terminate_, "fmi2Terminate");
    load(free_, "fmi2FreeInstance");
    callbacks_ = {log_nothing, std::calloc, std::free, nullptr, nullptr};
    c_ = instantiate_(")" + s.name + R"(", 1 /* co-simulation */, kGuid, kResources, &callbacks_, 0, 0);
    if (!c_) throw std::runtime_error("the FMU did not instantiate");
    ok(setup_(c_, 0, 0.0, 0.0, 0, 0.0), "setup");
    std::array<double, kPropRefs.size()> values{};
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = get_or(props, kPropIds[i]);
    ok(set_real_(c_, kPropRefs.data(), values.size(), values.data()), "set its props");
    ok(enter_init_(c_), "initialise");
    ok(exit_init_(c_), "initialise");
  }
  ~)" + s.cls + R"(() override {
    if (c_) terminate_(c_), free_(c_);
    if (lib_) dlclose(lib_);
  }
  )" + s.cls + R"((const )" + s.cls + R"(&) = delete;
  )" + s.cls + R"(& operator=(const )" + s.cls + R"(&) = delete;

  Values read() const override {
    std::array<double, kOutputRefs.size()> y{};
    ok(get_real_(c_, kOutputRefs.data(), y.size(), y.data()), "read its outputs");
    Values out;
    for (std::size_t i = 0; i < y.size(); ++i) out[kOutputIds[i]] = y[i];
    return out;
  }

  void step(const paperboat::Context& ctx, const Values& in) override {
    std::array<double, kInputRefs.size()> u{};
    for (std::size_t i = 0; i < u.size(); ++i) u[i] = get_or(in, kInputIds[i]);
    ok(set_real_(c_, kInputRefs.data(), u.size(), u.data()), "take its inputs");
    ok(do_step_(c_, ctx.t, ctx.dt, 1), "step");
  }

)" + (table.empty() ? std::string("  std::vector<paperboat::Entity> entities(const std::string&, double) const override { return {}; }\n") : std::string(kShapeEntities)) + R"(
 private:
  template <class F>
  void load(F& fn, const char* name) {
    fn = reinterpret_cast<F>(dlsym(lib_, name));
    if (!fn) throw std::runtime_error(std::string("the FMU lacks ") + name);
  }
  static void ok(int status, const char* what) {
    if (status > 1) throw std::runtime_error(std::string("the FMU could not ") + what);  // 0 ok, 1 warning
  }

  void* lib_ = nullptr;
  Component c_ = nullptr;
  Callbacks callbacks_{};
  Component (*instantiate_)(const char*, int, const char*, const char*, const Callbacks*, int, int) = nullptr;
  int (*setup_)(Component, int, double, double, int, double) = nullptr;
  int (*enter_init_)(Component) = nullptr;
  int (*exit_init_)(Component) = nullptr;
  int (*set_real_)(Component, const Ref*, std::size_t, const double*) = nullptr;
  int (*get_real_)(Component, const Ref*, std::size_t, double*) = nullptr;
  int (*do_step_)(Component, double, double, int) = nullptr;
  int (*terminate_)(Component) = nullptr;
  void (*free_)(Component) = nullptr;
};

}  // namespace

)" + register_cpp(s) + "\n}  // namespace " + s.id + "\n";
}

std::string cmake(const Spec& s, bool fmu) {
  std::string out = R"(# Generated by `paperboat modelica generate`; rewritten on every generate.
# A Paperboat station as a project of its own: it uses the engine, the engine doesn't know it.
cmake_minimum_required(VERSION 3.25)
project(paperboat_)" + s.id + R"( LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The engine: the sources of a Paperboat checkout (-DPAPERBOAT_ENGINE_DIR=…, or the checkout this
# sits in), else an installed SDK (find_package(Paperboat), e.g. -DPaperboat_DIR=…).
if(NOT PAPERBOAT_ENGINE_DIR AND NOT Paperboat_DIR)
  foreach(dir "${CMAKE_CURRENT_LIST_DIR}/../../engine" "${CMAKE_CURRENT_LIST_DIR}/../../../engine"
              "${CMAKE_CURRENT_LIST_DIR}/../../paperboat/engine" "${CMAKE_CURRENT_LIST_DIR}/../../../paperboat/engine")
    if(EXISTS "${dir}/CMakeLists.txt")
      set(PAPERBOAT_ENGINE_DIR "${dir}")
      break()
    endif()
  endforeach()
endif()
if(PAPERBOAT_ENGINE_DIR)
  set(PAPERBOAT_ENGINE_DIR "${PAPERBOAT_ENGINE_DIR}" CACHE PATH "Paperboat engine sources")
  add_subdirectory(${PAPERBOAT_ENGINE_DIR} paperboat EXCLUDE_FROM_ALL)
else()
  find_package(Paperboat 0.1 CONFIG REQUIRED)
endif()
# The program lands in build/ under every generator (station.toml's run), also multi-config ones.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}")

add_executable(paperboat-)" + s.name + " main.cpp domain/" + s.id + R"(.cpp)
target_link_libraries(paperboat-)" + s.name + R"( PRIVATE paperboat::core paperboat::warnings)
target_compile_definitions(paperboat-)" + s.name + R"( PRIVATE
  PAPERBOAT_DESCRIPTOR="${CMAKE_CURRENT_SOURCE_DIR}/descriptor.toml")
)";
  if (fmu)
    out += R"(
# The FMU, unpacked next to the build; the adapter loads its binary at run time.
set(fmu_dir "${CMAKE_CURRENT_BINARY_DIR}/fmu/)" + s.label + R"(")
file(ARCHIVE_EXTRACT INPUT "${CMAKE_CURRENT_SOURCE_DIR}/fmu/)" + s.label + R"(.fmu" DESTINATION "${fmu_dir}")
if(APPLE)
  set(fmu_platform darwin64)
  set(fmu_suffix .dylib)
elseif(WIN32)
  set(fmu_platform win64)
  set(fmu_suffix .dll)
else()
  set(fmu_platform linux64)
  set(fmu_suffix .so)
endif()
target_compile_definitions(paperboat-)" + s.name + R"( PRIVATE PAPERBOAT_FMU_DIR="${fmu_dir}"
  PAPERBOAT_FMU_PLATFORM="${fmu_platform}" PAPERBOAT_FMU_SUFFIX="${fmu_suffix}")
target_link_libraries(paperboat-)" + s.name + R"( PRIVATE ${CMAKE_DL_LIBS})
)";
  return out;
}

std::string main_cpp(const Spec& s, int port, const nlohmann::json& view = nullptr) {
  return header(s, "main") + R"(
// The model is domain/)" + s.id + R"(.cpp: a station object with a linked block. The descriptor,
// next to this file, is yours: imports, starters, variables.

#include <paperboat/sdk.hpp>

#include "domain/)" + s.id + R"(.hpp"

int main(int argc, char** argv) {
  static const paperboat::Registry registry = [] {
    paperboat::Registry r = paperboat::builtins();
    )" + s.id + "::register_" + s.id + R"((r);
    return r;
  }();
  static const paperboat::Catalog catalog = paperboat::load_descriptor_file(PAPERBOAT_DESCRIPTOR, registry);
  return paperboat::main({.engine = "cpp-)" + s.name + R"(", .catalog = &catalog)" +
         (view.is_null() ? "" : ", .view = nlohmann::json::parse(R\"(" + view.dump() + ")\")") + R"(}, argc, argv, )" + std::to_string(port) + R"();
}
)";
}

std::string hpp(const Spec& s) {
  return header(s, "header") + "\n#pragma once\n\n#include <paperboat/sdk.hpp>\n\nnamespace " + s.id +
         " {\n\n// Adds the " + s.label + " station object (and its linked block) to `registry`.\nvoid register_" + s.id +
         "(paperboat::Registry& registry);\n\n}  // namespace " + s.id + "\n";
}

std::string connectors_toml(const Spec& s) {
  if (s.connectors.empty()) return {};
  std::string out;
  for (const auto& [cname, cdef] : s.connectors) {
    const nlohmann::json cj = to_json(cdef);
    out += "[connectors." + cname + "]\n";
    out += "kind = " + toml_string(cdef.kind) + "\n";
    out += "handle = " + toml_string(cdef.handle) + "\n";
    if (!cdef.alt.empty()) out += "alt = " + toml_string(cdef.alt) + "\n";
    out += "size = " + std::to_string(cdef.size) + "\n";
    out += "weight = " + cj.at("weight").dump() + "\n";
    if (!cdef.dash.empty() && cdef.dash != "solid") out += "dash = " + toml_string(cdef.dash) + "\n";
    auto vars_line = [&](const char* key, const std::vector<VarUnit>& list) {
      if (list.empty()) return;
      out += std::string(key) + " = [";
      for (std::size_t i = 0; i < list.size(); ++i) {
        out += std::string(i ? ", " : "") + "{ id = " + toml_string(list[i].id) +
               ", unit = " + toml_string(list[i].unit) + " }";
      }
      out += "]\n";
    };
    vars_line("potential", cdef.potential);
    vars_line("flow", cdef.flow);
    vars_line("stream", cdef.stream);
    out += "\n";
  }
  return out;
}

std::string descriptor(const Spec& s, bool scene = false) {
  std::string nodes = "  { id = " + toml_string(s.name + "-1") + ", type = " + toml_string(s.name) + ", pos = [260, 0] },\n";
  std::string edges;
  int y = 0;
  for (const auto& p : s.inputs) {
    nodes += "  { id = " + toml_string("step-" + p.id) + ", type = \"step\", pos = [0, " + std::to_string(y) + "] },\n";
    edges += "  { from = [" + toml_string("step-" + p.id) + ", \"y\"], to = [" + toml_string(s.name + "-1") + ", " +
             toml_string(p.id) + "] },\n";
    y += 120;
  }
  y = 0;
  for (const auto& p : s.outputs) {
    nodes += "  { id = " + toml_string("graph-" + p.id) + ", type = \"graph\", pos = [520, " + std::to_string(y) + "] },\n";
    edges += "  { from = [" + toml_string(s.name + "-1") + ", " + toml_string(p.id) + "], to = [" +
             toml_string("graph-" + p.id) + ", \"in\"] },\n";
    y += 120;
  }
  return "# " + s.label + " as a Paperboat station, generated by `paperboat modelica generate` from\n"
         "# modelica/" + s.label + ".mo. Written once: edit it freely, generate leaves it alone.\n"
         "# The " + s.label + " station object is domain/" + s.id + ".cpp (its props: the model's\n"
         "# parameters and fixed starts); each one spawns a linked block on the Flow page.\n"
         "domain = " + toml_string(s.name) + "\nimport = [\"signal\", \"math\", \"control\", \"board\"]\n\n" +
         connectors_toml(s) +
         (scene ? "# Its MultiBody shapes draw in the Station's 3D scene.\n[station]\nview = \"scene\"\nobjects = ["
                : "# A model has no geometry of its own: the Station shows its objects as a graph.\n"
                  "[station]\nview = \"graph\"\nobjects = [") + "\n  { id = " + toml_string(s.name + "-1") + ", type = " + toml_string(s.name) + " },\n]\n\n"
         "# Starter: a step on each input, a graph on each output.\n[graph]\nnodes = [\n" + nodes + "]\nedges = [\n" +
         edges + "]\n";
}

std::string station_toml(const Spec& s, int port) {
  return "[station]\nformat = 0\nname = " + toml_string(s.name) + "\nversion = \"0.1.0\"\nengine = \">=0.1\"\n"
         "language = \"cpp\"\nrun = \"build/paperboat-" + s.name + "\"\nport = " + std::to_string(port) +
         "\ndescriptor = \"descriptor.toml\"\n";
}

std::string readme(const Spec& s, const GenerateResult& r) {
  std::string ins, outs;
  for (const auto& p : s.inputs) ins += (ins.empty() ? "" : ", ") + p.id;
  for (const auto& p : s.outputs) outs += (outs.empty() ? "" : ", ") + p.id;
  return "# " + s.name + "-cpp\n\n" + s.model + " (Modelica) as a Paperboat station, generated by\n"
         "`paperboat modelica generate modelica/" + s.label + ".mo --out .` (path: " + r.path + ").\n\n"
         "- Station: one `" + s.name + "` object; its props are the model's parameters and fixed starts.\n"
         "- Flow: its linked block takes " + (ins.empty() ? "nothing" : ins) + " and gives " + (outs.empty() ? "nothing" : outs) +
         ".\n\nRegenerate after editing the `.mo`; `descriptor.toml`, `station.toml` and this file are\n"
         "written once and kept.\n";
}

// --- omc ---

// A MultiBody model's shapes (visual_shapes(), crefs left as names) and the Station's view hint
// fitted to its start (camera on its box, the floor under its reach): a 0-length simulate with
// -d=visxml writes <prefix>_visual.xml and a result to read the start from. Empty for a model
// without MultiBody.
struct Multibody {
  nlohmann::json shapes = nlohmann::json::array();
  nlohmann::json view = nullptr;
};
Multibody multibody_shapes(Omc& omc, const std::string& model, const std::string& label) {
  Multibody m;
  if (parse_value(omc.call("list(" + model + ")")).atom.find("MultiBody") == std::string::npos) return m;
  const std::string prefix = label + "_visual";
  omc.call("setCommandLineOptions(\"-d=visxml\")");
  const std::string answer = omc.call("simulate(" + model + ", stopTime=0.0, numberOfIntervals=1, fileNamePrefix=\"" + prefix + "\")", std::chrono::minutes(5));
  const auto at = answer.find("resultFile = \"");
  const std::string mat = at == std::string::npos ? "" : answer.substr(at + 14, answer.find('"', at + 14) - at - 14);
  const std::string xml = read_file(fs::path(omc.workdir()) / (prefix + "_visual.xml"));
  m.shapes = visual_shapes(xml, [](const std::string& c) { return nlohmann::json(c); });
  if (mat.empty()) return m;
  // the start: each shape's two ends (its origin, then a length on), padded by its width
  const nlohmann::json start = visual_shapes(xml, [&](const std::string& c) {
    return nlohmann::json(std::strtod(parse_value(omc.call("val(" + c + ", 0.0, \"" + mat + "\")")).atom.c_str(), nullptr));
  });
  std::array<double, 3> lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
  for (const auto& sh : start) {
    if (sh["ident"].get<std::string>().starts_with("world.")) continue;
    const auto n = [&](const char* k, std::size_t i) { return sh[k].size() > i && sh[k][i].is_number() ? sh[k][i].get<double>() : 0.0; };
    const auto world = [&](double x, double y, double z) {  // Tᵀu
      return std::array<double, 3>{n("T", 0) * x + n("T", 3) * y + n("T", 6) * z, n("T", 1) * x + n("T", 4) * y + n("T", 7) * z,
                                   n("T", 2) * x + n("T", 5) * y + n("T", 8) * z};
    };
    const double len = std::hypot(n("lengthDir", 0), n("lengthDir", 1), n("lengthDir", 2));
    const double L = sh["length"].is_number() ? sh["length"].get<double>() : 0.0;
    const double pad = std::max(sh["width"].is_number() ? sh["width"].get<double>() : 0.0, sh["height"].is_number() ? sh["height"].get<double>() : 0.0) / 2;
    const auto rs = world(n("r_shape", 0), n("r_shape", 1), n("r_shape", 2));
    const auto ld = len > 0 ? world(n("lengthDir", 0) / len, n("lengthDir", 1) / len, n("lengthDir", 2) / len) : std::array<double, 3>{};
    for (const double f : {0.0, 1.0})
      for (int i = 0; i < 3; ++i) {
        const double p = n("r", i) + rs[i] + ld[i] * L * f;
        lo[i] = std::min(lo[i], p - pad);
        hi[i] = std::max(hi[i], p + pad);
      }
  }
  if (lo[0] > hi[0]) return m;
  const std::array<double, 3> c{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
  // ponytail: the reach is the start's box diagonal, from its centre; a model that travels further leaves it
  const double reach = std::max(0.5, std::hypot(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]));
  m.view = {{"mode", "3d"},
            {"floor", c[1] - reach},
            {"camera", {{"target", c}, {"eye", {c[0] + 0.6 * reach * 1.6, c[1] + 0.5 * reach * 1.6, c[2] + reach * 1.6}}}}};
  return m;
}

std::string ok_call(Omc& omc, const std::string& expr) {
  std::string out = omc.call(expr);
  if (out == "false" || out.empty()) {
    const std::string e = omc.errors();
    throw GenerateError(expr.substr(0, expr.find('(')) + " failed" + (e.empty() ? "" : ": " + e));
  }
  return out;
}

std::set<std::string> class_names(Omc& omc) {
  std::set<std::string> out;
  for (const auto& v : parse_value(omc.call("getClassNames()")).items) out.insert(v.atom);
  return out;
}

// Loads the MSL and `source`; returns the model to use.
std::string load(Omc& omc, const fs::path& source, const std::string& model) {
  if (!fs::is_regular_file(source)) throw GenerateError("no such file: " + source.string());
  const std::set<std::string> before = class_names(omc);
  if (!before.contains("Modelica")) ok_call(omc, "loadModel(Modelica)");
  const std::set<std::string> with_msl = class_names(omc);
  ok_call(omc, "loadFile(" + quote(fs::absolute(source).string()) + ")");
  if (!model.empty()) return model;
  std::vector<std::string> fresh;
  for (const auto& n : class_names(omc))
    if (!with_msl.contains(n)) fresh.push_back(n);
  if (fresh.size() == 1) return fresh.front();
  std::string list;
  for (const auto& n : fresh) list += " " + n;
  throw GenerateError(fresh.empty() ? "no new class in " + source.string() + " (already loaded? pass --model)"
                                    : "pick one with --model:" + list);
}

// The top-level components in order (the protected ones also in `hidden`), and those marked
// annotation(__paperboat(probe = true)).
std::set<std::string> probes(Omc& omc, const std::string& model, std::vector<std::string>& components,
                             std::set<std::string>& hidden) {
  std::set<std::string> out;
  const OmValue comps = parse_value(omc.call("getComponents(" + model + ")"));
  const OmValue notes = parse_value(omc.call("getComponentAnnotations(" + model + ")"));
  for (const auto& c : comps.items) {
    if (c.size() > 1) components.push_back(c[1].atom);
    if (c.size() > 3 && c[3].atom == "protected") hidden.insert(c[1].atom);
  }
  for (std::size_t i = 0; i < comps.size() && i < notes.size(); ++i) {
    std::string text;
    for (const auto& a : notes[i].items) text += a.atom;
    std::string compact;
    for (const char c : text)
      if (!std::isspace(static_cast<unsigned char>(c))) compact += c;
    if (compact.find("__paperboat") != std::string::npos && compact.find("probe=true") != std::string::npos &&
        comps[i].size() > 1)
      out.insert(comps[i][1].atom);
  }
  return out;
}

std::string run_tool(const std::string& command) {
  if (std::system(command.c_str()) != 0) throw GenerateError("failed: " + command);
  return {};
}

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (const char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

}  // namespace

std::string station_name(std::string_view model) {
  const std::string n = short_name(model);
  std::string out;
  for (std::size_t i = 0; i < n.size(); ++i) {
    const char c = n[i];
    if (!std::isalnum(static_cast<unsigned char>(c))) {
      if (!out.empty() && out.back() != '-') out += '-';
      continue;
    }
    const bool upper = std::isupper(static_cast<unsigned char>(c));
    const bool after_lower = i && std::islower(static_cast<unsigned char>(n[i - 1]));
    const bool before_lower = i + 1 < n.size() && std::islower(static_cast<unsigned char>(n[i + 1]));
    const bool after_upper = i && std::isupper(static_cast<unsigned char>(n[i - 1]));
    if (upper && !out.empty() && out.back() != '-' && (after_lower || (after_upper && before_lower))) out += '-';
    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "model" : out;
}

std::string to_cpp(std::string_view expr, const std::function<std::string(std::string_view, bool)>& name) {
  return Translator(expr, name).run();
}

CheckResult check(Omc& omc, const fs::path& source, const std::string& model) {
  CheckResult r;
  try {
    r.model = load(omc, source, model);
    r.message = trim(parse_value(omc.call("checkModel(" + r.model + ")")).atom);
    r.ok = r.message.find("completed successfully") != std::string::npos;
    if (const std::string e = omc.errors(); !e.empty()) r.message += "\n" + e;
  } catch (const std::exception& e) {
    r.message = e.what();
  }
  return r;
}

GenerateResult generate(Omc& omc, const GenerateOptions& o) {
  if (o.path != "auto" && o.path != "ode" && o.path != "fmu")
    throw GenerateError("--path is auto, ode or fmu, not " + o.path);
  GenerateResult r;
  r.dry = o.dry;
  const auto progress = [&](std::string_view stage, double fraction) {
    if (o.progress) o.progress(stage, fraction);
  };
  progress("check", 0);
  const bool live = o.live && !o.model.empty() &&
                    !trim(parse_value(omc.call("getClassRestriction(" + o.model + ")")).atom).empty();
  if (live) {
    r.model = o.model;
    const std::string said = trim(parse_value(omc.call("checkModel(" + r.model + ")")).atom);
    if (said.find("completed successfully") == std::string::npos) throw GenerateError(said + "\n" + omc.errors());
  } else {
    r.model = load(omc, o.source, o.model);
    if (const CheckResult c = check(omc, o.source, r.model); !c.ok) throw GenerateError(c.message);
  }

  progress("flatten", 0.25);
  const OmValue dump = parse_value(ok_call(
      omc, "dumpXMLDAE(" + r.model + ", translationLevel=\"backEnd\", addMathMLCode=false, addSolvingInfo=true, "
           "fileNamePrefix=\"" + station_name(r.model) + "_dae\")"));
  if (!dump.is_list || dump.size() < 2 || dump[0].atom != "true") throw GenerateError("dumpXMLDAE failed: " + omc.errors());
  const Dae dae = parse_dae(read_file(dump[1].atom));
  std::vector<std::string> components;
  std::set<std::string> hidden;
  const std::set<std::string> probed = probes(omc, r.model, components, hidden);
  Spec spec = build(dae, r.model, components, probed, hidden);
  const OmValue description = parse_value(omc.call("getClassComment(" + r.model + ")"));
  spec.description = trim(description.atom);

  // Import the top-level model's Icon and causal connector ports (§3, §4, §6).
  {
    const ImportedModel imp = import_class(omc, r.model);
    std::map<std::string, const RawPort*> by_id;
    for (const auto& rp : imp.raw_ports) by_id[rp.id] = &rp;

    std::vector<RawPort> causal_raw;
    for (const auto& p : spec.inputs) {
      if (const auto it = by_id.find(p.id); it != by_id.end() && it->second->kind == "input") {
        causal_raw.push_back(*it->second);
        if (const auto cit = imp.connectors.find(it->second->class_name); cit != imp.connectors.end())
          spec.connectors[cit->first] = cit->second;
      } else {
        causal_raw.push_back(
            {.id = p.id, .class_name = "RealInput", .full_class = "Modelica.Blocks.Interfaces.RealInput", .kind = "input"});
        spec.connectors.try_emplace("RealInput",
                                    ConnectorDef{.kind = "input", .handle = "triangle", .size = 8, .weight = 1.2});
      }
    }
    for (const auto& p : spec.outputs) {
      if (const auto it = by_id.find(p.id); it != by_id.end() && it->second->kind == "output") {
        causal_raw.push_back(*it->second);
        if (const auto cit = imp.connectors.find(it->second->class_name); cit != imp.connectors.end())
          spec.connectors[cit->first] = cit->second;
      } else {
        causal_raw.push_back({.id = p.id,
                              .class_name = "RealOutput",
                              .full_class = "Modelica.Blocks.Interfaces.RealOutput",
                              .kind = "output"});
        spec.connectors.try_emplace(
            "RealOutput", ConnectorDef{.kind = "output", .handle = "circle", .alt = "filled", .size = 8, .weight = 1.2});
      }
    }

    ImportNotice port_notice;
    const std::vector<IconPort> causal_ports = quantise_ports(causal_raw, port_notice, imp.extent);
    spec.icon = imp.icon;
    spec.icon.erase("ports");
    if (!causal_ports.empty()) {
      nlohmann::json p_arr = nlohmann::json::array();
      for (const auto& p : causal_ports) p_arr.push_back(to_json(p));
      spec.icon["ports"] = std::move(p_arr);
    }

    if (imp.notice.simplified > 0 || !imp.notice.dropped.empty() || !port_notice.moved.empty()) {
      std::string note = "icon:";
      if (imp.notice.simplified > 0) note += " simplified " + std::to_string(imp.notice.simplified);
      if (!imp.notice.dropped.empty()) note += " dropped " + std::to_string(imp.notice.dropped.size());
      if (!port_notice.moved.empty()) note += " spread " + std::to_string(port_notice.moved.size()) + " colliding port(s)";
      spec.notices.push_back(std::move(note));
    }
  }

  r.name = spec.name;
  r.out = o.out.empty() ? fs::path(spec.name + "-cpp") : o.out;
  r.notices = spec.notices;
  if (o.path == "ode" && spec.ode_blocker) throw GenerateError("can't be an ODE: " + *spec.ode_blocker);
  const bool fmu = o.path == "fmu" || (o.path == "auto" && spec.ode_blocker);
  r.path = fmu ? "fmu" : "ode";
  r.reason = o.path != "auto" ? "asked for --path " + o.path
             : fmu            ? "FMU: " + *spec.ode_blocker
                              : "ODE: no events, no torn systems, no external functions";

  constexpr int port = 8780;
  progress("generate", 0.5);
  if (!o.dry) fs::create_directories(r.out);
  std::string domain;
  bool scene = false;
  nlohmann::json view = nullptr;
  if (fmu && o.dry) {
    r.notices.push_back("dry run: the FMU path builds with omc, so no files are previewed");
    progress("generate", 1);
    return r;
  }
  if (fmu) {
    progress("build", 0.6);
    // a MultiBody model's shapes read protected variables: the FMU exposes them too, for this build only
    const Multibody mb = multibody_shapes(omc, r.model, spec.label);
    const nlohmann::json& shapes = mb.shapes;
    view = mb.view;
    omc.call(std::string("setCommandLineOptions(\"--fmiFilter=") + (shapes.empty() ? "protected" : "internal") + "\")");
    // CVODE inside the FMU unless asked: explicit Euler at Paperboat's step gains energy (a
    // pendulum spins up from rest)
    if (o.fmu_solver != "cvode" && o.fmu_solver != "euler") throw GenerateError("the FMU solver is cvode or euler, not " + o.fmu_solver);
    omc.call("setCommandLineOptions(\"--fmiFlags=s:" + o.fmu_solver + "\")");
    const OmValue built = parse_value(ok_call(
        omc, "buildModelFMU(" + r.model + ", version=\"2.0\", fmuType=\"cs\", fileNamePrefix=\"" + spec.label +
                 "\", platforms={\"static\"})"));
    const fs::path fmu_file = built.atom;
    omc.call("setCommandLineOptions(\"--fmiFilter=protected\")");
    if (!fs::is_regular_file(fmu_file)) throw GenerateError("buildModelFMU failed: " + omc.errors());
    fs::create_directories(r.out / "fmu");
    fs::copy_file(fmu_file, r.out / "fmu" / (spec.label + ".fmu"), fs::copy_options::overwrite_existing);
    r.written.push_back(fs::path("fmu") / (spec.label + ".fmu"));
    const fs::path tmp = fs::path(omc.workdir()) / "fmu-description";
    fs::create_directories(tmp);
    run_tool("cd " + shell_quote(tmp.string()) + " && cmake -E tar xf " + shell_quote(fs::absolute(fmu_file).string()) +
             " modelDescription.xml");
    domain = fmu_cpp(spec, read_model_description(read_file(tmp / "modelDescription.xml")), r.notices, shapes);
    scene = domain.find("kShapes") != std::string::npos;
  } else {
    domain = ode_cpp(spec);
    std::error_code ec;
    if (!o.dry) fs::remove(r.out / "fmu" / (spec.label + ".fmu"), ec);
  }

  write(r, "CMakeLists.txt", cmake(spec, fmu));
  write(r, "main.cpp", main_cpp(spec, port, view));
  write(r, fs::path("domain") / (spec.id + ".hpp"), hpp(spec));
  write(r, fs::path("domain") / (spec.id + ".cpp"), domain);
  const fs::path mo = fs::path("modelica") / (spec.label + ".mo");
  if (live) write(r, mo, parse_value(omc.call("list(" + r.model + ")")).atom + "\n");
  else if (o.dry || !fs::exists(r.out / mo) || !fs::equivalent(r.out / mo, o.source)) write(r, mo, read_file(o.source));
  write(r, "station.toml", station_toml(spec, port), true);
  write(r, "descriptor.toml", descriptor(spec, scene), true);
  // A kept descriptor still follows the model gaining or losing shapes: swap the generated
  // [station] lines, leaving the user's own edits alone.
  if (!o.dry && fs::exists(r.out / "descriptor.toml")) {
    const std::string graph = "# A model has no geometry of its own: the Station shows its objects as a graph.\n[station]\nview = \"graph\"\n";
    const std::string shown = "# Its MultiBody shapes draw in the Station's 3D scene.\n[station]\nview = \"scene\"\n";
    std::string text = read_file(r.out / "descriptor.toml");
    const std::string& from = scene ? graph : shown;
    if (const auto at = text.find(from); at != std::string::npos) {
      text.replace(at, from.size(), scene ? shown : graph);
      std::ofstream(r.out / "descriptor.toml", std::ios::binary) << text;
      r.notices.push_back(std::string("descriptor.toml: the Station view is now ") + (scene ? "the 3D scene" : "a graph"));
    }
  }
  write(r, "README.md", readme(spec, r), true);
  progress("generate", 1);
  return r;
}

}  // namespace squero
