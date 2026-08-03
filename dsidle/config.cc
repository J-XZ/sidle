#include "dsidle/config.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace dsidle {
namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open experiment config: " + path);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string StripJsonComments(const std::string& text) {
  std::string result;
  bool quoted = false;
  bool escaped = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quoted) {
      result += c;
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '\"') quoted = false;
      continue;
    }
    if (c == '\"') { quoted = true; result += c; continue; }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') ++i;
      if (i < text.size()) result += '\n';
      continue;
    }
    result += c;
  }
  if (quoted) throw std::runtime_error("unterminated JSON string");
  return result;
}

std::string Section(const std::string& json, const std::string& name) {
  const std::regex start("\\\"" + name + "\\\"\\s*:");
  std::smatch match;
  if (!std::regex_search(json, match, start)) throw std::runtime_error("missing section: " + name);
  const auto open = json.find('{', match.position() + match.length());
  if (open == std::string::npos) throw std::runtime_error("section is not an object: " + name);
  int depth = 0;
  bool quoted = false;
  for (std::size_t i = open; i < json.size(); ++i) {
    if (json[i] == '\"' && (i == 0 || json[i - 1] != '\\')) quoted = !quoted;
    if (quoted) continue;
    if (json[i] == '{') ++depth;
    if (json[i] == '}' && --depth == 0) return json.substr(open, i - open + 1);
  }
  throw std::runtime_error("unterminated section: " + name);
}

std::vector<std::string> ObjectKeys(const std::string& object) {
  std::vector<std::string> keys;
  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (std::size_t i = 0; i < object.size(); ++i) {
    const char c = object[i];
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
      continue;
    }
    if (c == '{') { ++depth; continue; }
    if (c == '}') { --depth; continue; }
    if (c != '"' || depth != 1) continue;
    const std::size_t begin = ++i;
    bool local_escaped = false;
    for (; i < object.size(); ++i) {
      if (local_escaped) { local_escaped = false; continue; }
      if (object[i] == '\\') { local_escaped = true; continue; }
      if (object[i] == '"') break;
    }
    if (i == object.size()) throw std::runtime_error("unterminated object key");
    std::size_t after = i + 1;
    while (after < object.size() && std::isspace(static_cast<unsigned char>(object[after]))) ++after;
    if (after < object.size() && object[after] == ':') keys.emplace_back(object.substr(begin, i - begin));
  }
  return keys;
}

void ValidateObjectKeys(const std::string& object, const char* section,
                        std::initializer_list<const char*> required,
                        std::initializer_list<const char*> optional = {}) {
  std::unordered_set<std::string> allowed;
  for (const char* key : required) allowed.emplace(key);
  for (const char* key : optional) allowed.emplace(key);
  std::unordered_set<std::string> actual;
  for (const auto& key : ObjectKeys(object)) {
    if (!allowed.count(key)) throw std::runtime_error(std::string("unknown key in ") + section + ": " + key);
    if (!actual.emplace(key).second)
      throw std::runtime_error(std::string("duplicate key in ") + section + ": " + key);
  }
  for (const char* key : required)
    if (!actual.count(key)) throw std::runtime_error(std::string("missing key in ") + section + ": " + key);
}

std::uint64_t Integer(const std::string& object, const std::string& name) {
  std::smatch match;
  if (!std::regex_search(
          object, match,
          std::regex("\\\"" + name +
                     "\\\"\\s*:\\s*((?:0|[1-9][0-9]*))\\s*[,}]")))
    throw std::runtime_error("missing integer: " + name);
  return std::stoull(match[1]);
}

std::string String(const std::string& object, const std::string& name) {
  std::smatch match;
  if (!std::regex_search(object, match, std::regex("\\\"" + name + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"")))
    throw std::runtime_error("missing string: " + name);
  return match[1];
}
bool Boolean(const std::string& object, const std::string& name) {
  std::smatch m; if (!std::regex_search(object, m, std::regex("\\\"" + name + "\\\"\\s*:\\s*(true|false)"))) throw std::runtime_error("missing boolean: " + name);
  return m[1] == "true";
}
double Number(const std::string& object, const std::string& name) {
  std::smatch m;
  if (!std::regex_search(
          object, m,
          std::regex("\\\"" + name +
                     "\\\"\\s*:\\s*(-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?"
                     "(?:[eE][+-]?[0-9]+)?)\\s*[,}]")))
    throw std::runtime_error("missing number: " + name);
  return std::stod(m[1]);
}

std::vector<int> NumaNodes(const std::string& object, const std::string& name) {
  std::smatch match;
  // Allow pretty-printed arrays with newlines; [^] does not span newlines reliably
  // across libstdc++ ECMAScript, so use [\s\S] with a non-greedy array match.
  if (!std::regex_search(object, match,
                         std::regex("\\\"" + name + "\\\"\\s*:\\s*(\\[[\\s\\S]*?\\]|[0-9]+)")))
    throw std::runtime_error("missing NUMA node: " + name);
  std::vector<int> nodes;
  const std::regex number("[0-9]+");
  for (std::sregex_iterator it(match[1].first, match[1].second, number), end; it != end; ++it)
    nodes.push_back(std::stoi(it->str()));
  if (nodes.empty()) throw std::runtime_error("empty NUMA node list");
  return nodes;
}

bool PowerOfTwo(std::uint64_t value) { return value && !(value & (value - 1)); }

}  // namespace

std::string DefaultExperimentConfigPath() {
  const char* configured = std::getenv("DSIDLE_EXPERIMENT_CONFIG_JSONC");
  return configured && *configured ? configured : "experiment_config.jsonc";
}

ExperimentConfig LoadExperimentConfig(const std::string& path) {
  const std::string json = StripJsonComments(ReadFile(path));
  ValidateObjectKeys(json, "root",
                     {"shared_memory", "vm", "host_cpu", "e2e", "dsidle"},
                     {"network", "sync"});
  const std::string shared = Section(json, "shared_memory");
  const std::string vm = Section(json, "vm");
  const std::string host_cpu = Section(json, "host_cpu");
  const std::string e2e = Section(json, "e2e");
  const std::string local = Section(json, "dsidle");
  ValidateObjectKeys(shared, "shared_memory", {"size_mb", "path", "device_path", "numa_node", "hwcc", "swcc"});
  ValidateObjectKeys(vm, "vm", {"count", "core_count_per_vm", "mem_size_mb_per_vm", "storage_path", "ssh_base_port", "numa_node", "local_ssh_pub_key", "first_ip", "bridge_tap_ip"},
                     {"copy_root_img", "use_ivshmem_doorbell"});
  ValidateObjectKeys(host_cpu, "host_cpu", {"reserved_cores", "ivshmem_server_cores", "vm_cores"});
  ValidateObjectKeys(e2e, "e2e", {"foreground_worker_count_per_vm"});
  ValidateObjectKeys(local, "dsidle", {"replica_budget_mb", "hot_percentage_seed", "fixed_key_size", "fixed_value_size", "trace_dir", "verbose", "extra_check", "latency_inject"});
  ExperimentConfig config;
  config.shared_size_mb = Integer(shared, "size_mb");
  config.shared_path = String(shared, "path");
  // The configured path is a host directory; ivshmem-plain uses the stable
  // backing-file name below when the directory already exists.
  if (std::filesystem::is_directory(config.shared_path))
    config.shared_path = (std::filesystem::path(config.shared_path) / "ivshmem_shared_mem").string();
  config.device_path = String(shared, "device_path");
  config.shared_numa_nodes = NumaNodes(shared, "numa_node");
  const auto hwcc = Section(shared, "hwcc");
  const auto swcc = Section(shared, "swcc");
  ValidateObjectKeys(hwcc, "shared_memory.hwcc", {"offset_mb", "size_mb"});
  ValidateObjectKeys(swcc, "shared_memory.swcc", {"offset_mb", "size_mb"});
  config.hwcc = {Integer(hwcc, "offset_mb"), Integer(hwcc, "size_mb")};
  config.swcc = {Integer(swcc, "offset_mb"), Integer(swcc, "size_mb")};
  config.vm_count = static_cast<std::uint32_t>(Integer(vm, "count"));
  config.core_count_per_vm = static_cast<std::uint32_t>(Integer(vm, "core_count_per_vm"));
  config.foreground_worker_count_per_vm = static_cast<std::uint32_t>(Integer(e2e, "foreground_worker_count_per_vm"));
  config.replica_budget_mb = Integer(local, "replica_budget_mb");
  config.hot_percentage_seed =
      static_cast<std::uint32_t>(Integer(local, "hot_percentage_seed"));
  config.fixed_key_size = static_cast<std::uint32_t>(Integer(local, "fixed_key_size"));
  config.fixed_value_size = static_cast<std::uint32_t>(Integer(local, "fixed_value_size"));
  config.trace_dir = String(local, "trace_dir");
  config.verbose = Boolean(local, "verbose");
  config.extra_check = Boolean(local, "extra_check");
  const auto latency = Section(local, "latency_inject");
  auto& l = config.latency_inject;
  ValidateObjectKeys(latency, "dsidle.latency_inject", {"fixed_latency"});
  const auto fixed = Section(latency, "fixed_latency");
  ValidateObjectKeys(fixed, "dsidle.latency_inject.fixed_latency",
                     {"enabled", "cache_line_bytes", "swcc_fixed_ns_per_line",
                      "hwcc_fixed_ns_per_line", "foreground_enabled",
                      "background_enabled"});

  auto& fixed_config = l.fixed_latency;
  fixed_config.enabled = Boolean(fixed, "enabled");
  fixed_config.cache_line_bytes = Integer(fixed, "cache_line_bytes");
  fixed_config.swcc_fixed_ns_per_line =
      Number(fixed, "swcc_fixed_ns_per_line");
  fixed_config.hwcc_fixed_ns_per_line =
      Number(fixed, "hwcc_fixed_ns_per_line");
  fixed_config.foreground_enabled = Boolean(fixed, "foreground_enabled");
  fixed_config.background_enabled = Boolean(fixed, "background_enabled");

  const bool hardware_enabled = fixed_config.enabled;
  if (hardware_enabled && (config.verbose || config.extra_check))
    throw std::runtime_error(
        "latency injection requires dsidle.verbose=false and "
        "dsidle.extra_check=false");
  if (hardware_enabled &&
      std::string_view(DSIDLE_CMAKE_BUILD_TYPE) != "RelWithDebInfo")
    throw std::runtime_error(
        "latency injection requires an exact RelWithDebInfo build");
  if (!PowerOfTwo(config.shared_size_mb) || config.hwcc.offset_mb != 0 ||
      config.swcc.offset_mb != config.hwcc.size_mb ||
      config.hwcc.size_mb + config.swcc.size_mb != config.shared_size_mb ||
      !config.vm_count || !config.core_count_per_vm ||
      config.hot_percentage_seed > 100 ||
      !config.fixed_key_size || !config.fixed_value_size)
    throw std::runtime_error("invalid D-SIDLE shared-memory layout or topology");
  try {
    latency_sim::LatencySimulator validator(l);
    (void)validator;
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::string("invalid dsidle.latency_inject: ") + error.what());
  }
  return config;
}

}  // namespace dsidle
