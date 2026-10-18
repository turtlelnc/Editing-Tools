#include "cli/cli.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>

#include "util/base64.h"

namespace et::cli {

using mcp::json;

namespace {

const char* kVersion = "editing-tools 0.1.0";

// Thrown for mistakes in how the command was written (exit code 2).
struct UsageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Parsed {
  std::optional<std::string> project, args_file, image_file;
  bool pretty = false, help = false, version = false;
  std::vector<std::string> positional;
  std::vector<std::pair<std::string, std::optional<std::string>>> flags;  // tool arguments given as --name [value]
};

std::string normalize(std::string name) {
  for (auto& c : name)
    if (c == '-') c = '_';
  return name;
}

Parsed parse(const std::vector<std::string>& args) {
  Parsed p;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& t = args[i];
    if (t == "-h") { p.help = true; continue; }
    if (t.rfind("--", 0) != 0 || t.size() == 2) { p.positional.push_back(t); continue; }

    std::string name = t.substr(2);
    std::optional<std::string> value;
    if (auto eq = name.find('='); eq != std::string::npos) {
      value = name.substr(eq + 1);
      name = name.substr(0, eq);
    }
    // Reserved options. A tool's own parameters (like `in` / `out` seconds) never collide with these.
    const bool takes_value = name == "project" || name == "args-file" || name == "image-file";
    const bool bare = name == "pretty" || name == "help" || name == "version";
    if (takes_value || bare) {
      if (takes_value && !value) {
        if (i + 1 >= args.size()) throw UsageError("--" + name + " needs a value");
        value = args[++i];
      }
      if (name == "project") p.project = value;
      else if (name == "args-file") p.args_file = value;
      else if (name == "image-file") p.image_file = value;
      else if (name == "pretty") p.pretty = true;
      else if (name == "help") p.help = true;
      else p.version = true;
      continue;
    }
    // Tool argument: take the next token as the value unless it is another option.
    if (!value && i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0) value = args[++i];
    p.flags.emplace_back(normalize(name), value);
  }
  return p;
}

std::string type_of(const json& prop) { return prop.is_object() ? prop.value("type", "") : ""; }

// Converts a command-line string into the JSON type the tool's schema asks for.
json coerce(const std::string& flag, const std::optional<std::string>& raw, const json& prop) {
  const std::string type = type_of(prop);
  auto bad = [&](const std::string& why) { return UsageError("--" + flag + ": " + why); };

  if (type == "boolean") {
    if (!raw) return true;  // bare --box means true
    if (*raw == "true" || *raw == "1" || *raw == "yes") return true;
    if (*raw == "false" || *raw == "0" || *raw == "no") return false;
    throw bad("expected true or false, got '" + *raw + "'");
  }
  if (!raw) throw bad("needs a value");
  if (type == "string") return *raw;  // never reinterpret: a font called "123" stays a string

  const std::string expected = type == "integer" ? "an integer"
                               : type == "number" ? "a number"
                               : type == "array"  ? "a JSON array"
                               : type == "object" ? "a JSON object"
                                                  : type;
  const std::string got = ", got '" + *raw + "'";
  json v = json::parse(*raw, nullptr, false);
  if (v.is_discarded()) {
    if (type.empty()) return *raw;
    throw bad("expected " + expected + got);
  }
  if ((type == "integer" && !v.is_number_integer()) || (type == "number" && !v.is_number()) ||
      (type == "array" && !v.is_array()) || (type == "object" && !v.is_object()))
    throw bad("expected " + expected + got);
  return v;
}

std::string first_sentence(const std::string& s, size_t max = 110) {
  auto dot = s.find(". ");
  std::string out = s.substr(0, dot == std::string::npos ? s.size() : dot + 1);
  if (out.size() > max) out = out.substr(0, max - 1) + "…";
  return out;
}

void print_usage(std::ostream& o) {
  o << kVersion << "\n\n"
    << "AI-driven video editing. Every edit is one command; the project lives in a JSON file.\n\n"
    << "USAGE\n"
    << "  editing-tools [--project FILE] <tool> [--arg value ...]\n"
    << "  editing-tools [--project FILE] <tool> '{\"arg\": value}'\n"
    << "  editing-tools tools               list all tools\n"
    << "  editing-tools help <tool>         arguments of one tool\n"
    << "  editing-tools mcp                 run as an MCP server on stdio (also the default when stdin is a pipe)\n\n"
    << "OPTIONS\n"
    << "  --project FILE     project file to read and update (or set EDITING_TOOLS_PROJECT)\n"
    << "  --args-file FILE   read the JSON arguments from FILE (- for stdin)\n"
    << "  --image-file FILE  where preview_frame saves its JPEG (default: next to the project)\n"
    << "  --pretty           indent the JSON result\n\n"
    << "EXAMPLE\n"
    << "  editing-tools --project p.json create_project --project_path p.json --width 1920 --height 1080\n"
    << "  editing-tools --project p.json add_clip --path a.mp4 --in 0.5 --out 5\n"
    << "  editing-tools --project p.json color_adjust --warmth 0.3 --contrast 1.1\n"
    << "  editing-tools --project p.json preview_frame --time 1.0\n"
    << "  editing-tools --project p.json render_export --output_path out.mp4\n\n"
    << "Results are JSON on stdout. Failures print {\"error\": ...} on stderr; exit code 1 = the tool failed, 2 = bad usage.\n"
    << "Subtitles need an ffmpeg with libass: set EDITING_TOOLS_FFMPEG to it.\n";
}

void print_tool_usage(std::ostream& o, const mcp::Tool& t) {
  const json& props = t.input_schema["properties"];
  std::set<std::string> required;
  for (auto& r : t.input_schema["required"]) required.insert(r.get<std::string>());
  o << t.name << "\n  " << t.description << "\n\nARGUMENTS\n";
  if (props.empty()) o << "  (none)\n";
  for (auto& [k, v] : props.items()) {
    o << "  --" << k << " <" << type_of(v) << ">" << (required.count(k) ? "  (required)" : "");
    if (v.is_object() && v.contains("description")) o << "  " << v["description"].get<std::string>();
    o << "\n";
  }
  o << "\nEXAMPLE\n  editing-tools --project p.json " << t.name;
  for (auto& r : required) o << " --" << r << " <" << type_of(props[r]) << ">";
  o << "\n";
}

// preview_frame returns its JPEG inline; the CLI saves it and reports the path instead.
void save_image(json& payload, const Parsed& p, const std::optional<std::string>& project) {
  if (!payload.is_object() || !payload.contains("__image")) return;
  namespace fs = std::filesystem;
  std::string path;
  if (p.image_file) {
    path = *p.image_file;
  } else {
    char name[64];
    std::snprintf(name, sizeof name, "preview_%.2fs.jpg", payload.value("time", 0.0));
    fs::path dir = project ? fs::absolute(*project).parent_path() : fs::current_path();
    path = (dir / name).string();
  }
  const std::string bytes = util::unbase64(payload["__image"].value("data", ""));
  std::ofstream f(path, std::ios::binary);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!f) throw mcp::ToolError("cannot write image to " + path);
  payload.erase("__image");
  payload["image_path"] = fs::absolute(path).lexically_normal().string();
}

int run_tool(const Parsed& p, const mcp::Server& server, std::shared_ptr<tools::Session> session, std::istream& in,
             std::ostream& out) {
  size_t next = 0;
  if (p.positional[next] == "call") ++next;
  if (next >= p.positional.size()) throw UsageError("missing tool name; run `editing-tools tools`");
  const std::string name = p.positional[next++];
  const mcp::Tool* tool = server.find(name);
  if (!tool) throw UsageError("unknown tool '" + name + "'; run `editing-tools tools` to list them");
  const json& props = tool->input_schema["properties"];

  // Base arguments: a JSON object (positional or file/stdin), then flags on top.
  json args = json::object();
  std::optional<std::string> raw_json;
  if (next < p.positional.size()) raw_json = p.positional[next++];
  if (p.args_file) {
    if (raw_json) throw UsageError("give the JSON arguments either inline or with --args-file, not both");
    if (*p.args_file == "-") {
      raw_json = std::string(std::istreambuf_iterator<char>(in), {});
    } else {
      std::ifstream f(*p.args_file);
      if (!f) throw UsageError("cannot read " + *p.args_file);
      raw_json = std::string(std::istreambuf_iterator<char>(f), {});
    }
  }
  if (next < p.positional.size()) throw UsageError("unexpected argument '" + p.positional[next] + "'; pass tool arguments as --name value");
  if (raw_json) {
    args = json::parse(*raw_json, nullptr, false);
    if (!args.is_object()) throw UsageError("the JSON arguments must be an object like {\"path\": \"a.mp4\"}");
  }
  for (auto& [flag, value] : p.flags) {
    if (!props.contains(flag)) {
      std::string valid;
      for (auto& [k, _] : props.items()) valid += (valid.empty() ? "--" : ", --") + k;
      throw UsageError("unknown argument --" + flag + " for " + name + (valid.empty() ? " (it takes none)" : "; valid: " + valid));
    }
    args[flag] = coerce(flag, value, props[flag]);
  }

  const char* env_project = std::getenv("EDITING_TOOLS_PROJECT");
  std::optional<std::string> project = p.project;
  if (!project && env_project && *env_project) project = env_project;

  // Each CLI call is a fresh process, so the project is loaded from (and saved back to) its file.
  const bool defines_project = name == "create_project" || name == "open_project";
  if (project) {
    if (defines_project) {
      if (!args.contains("project_path")) args["project_path"] = *project;
    } else if (name != "probe_media") {
      session->open(*project);
    }
  }

  std::vector<std::string> missing;
  for (auto& r : tool->input_schema["required"])
    if (!args.contains(r.get<std::string>())) missing.push_back("--" + r.get<std::string>());
  if (!missing.empty()) {
    std::string list;
    for (auto& m : missing) list += (list.empty() ? "" : ", ") + m;
    throw UsageError("missing required argument(s): " + list + " (see `editing-tools help " + name + "`)");
  }

  json payload;
  try {
    payload = server.call(name, args);
    save_image(payload, p, project);
  } catch (const std::exception& e) {
    std::string msg = e.what();
    if (msg.rfind("no project open", 0) == 0) msg = "no project: pass --project FILE (or set EDITING_TOOLS_PROJECT)";
    throw mcp::ToolError(msg);
  }
  out << (p.pretty ? payload.dump(2) : payload.dump()) << "\n";
  return 0;
}

}  // namespace

int run(const std::vector<std::string>& args, std::istream& in, std::ostream& out, std::ostream& err,
        const mcp::Server& server, std::shared_ptr<tools::Session> session) {
  try {
    Parsed p = parse(args);
    if (p.version) { out << kVersion << "\n"; return 0; }
    if (p.positional.empty()) {
      if (p.help) { print_usage(out); return 0; }
      print_usage(err);
      return 2;
    }
    const std::string& cmd = p.positional[0];

    if (cmd == "tools") {
      for (auto* t : server.tools()) out << t->name << "  " << first_sentence(t->description) << "\n";
      return 0;
    }
    if (cmd == "help" || p.help) {
      std::string target = cmd == "help" ? (p.positional.size() > 1 ? p.positional[1] : "") : cmd;
      if (target.empty()) { print_usage(out); return 0; }
      if (target == "call" && p.positional.size() > 1) target = p.positional[1];
      const mcp::Tool* t = server.find(target);
      if (!t) throw UsageError("unknown tool '" + target + "'; run `editing-tools tools` to list them");
      print_tool_usage(out, *t);
      return 0;
    }
    return run_tool(p, server, session, in, out);
  } catch (const UsageError& e) {
    err << json{{"error", e.what()}}.dump() << "\n";
    return 2;
  } catch (const std::exception& e) {
    err << json{{"error", e.what()}}.dump() << "\n";
    return 1;
  }
}

}  // namespace et::cli
