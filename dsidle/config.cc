#include "dsidle/config.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <regex>
#include <sstream>
#include <stdexcept>
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
                        std::initializer_list<const char*> allowed) {
  std::unordered_set<std::string> expected;
  for (const char* key : allowed) expected.emplace(key);
  std::unordered_set<std::string> actual;
  for (const auto& key : ObjectKeys(object)) {
    if (!expected.count(key)) throw std::runtime_error(std::string("unknown key in ") + section + ": " + key);
    actual.emplace(key);
  }
  for (const auto& key : expected)
    if (!actual.count(key)) throw std::runtime_error(std::string("missing key in ") + section + ": " + key);
}

std::uint64_t Integer(const std::string& object, const std::string& name) {
  std::smatch match;
  if (!std::regex_search(object, match, std::regex("\\\"" + name + "\\\"\\s*:\\s*([0-9]+)")))
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
  std::smatch m; if (!std::regex_search(object, m, std::regex("\\\"" + name + "\\\"\\s*:\\s*([0-9]+(?:\\.[0-9]+)?)"))) throw std::runtime_error("missing number: " + name);
  return std::stod(m[1]);
}

std::vector<int> NumaNodes(const std::string& object, const std::string& name) {
  std::smatch match;
  if (!std::regex_search(object, match, std::regex("\\\"" + name + "\\\"\\s*:\\s*(\\[[^]]*\\]|[0-9]+)")))
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
  ValidateObjectKeys(json, "root", {"shared_memory", "vm", "host_cpu", "e2e", "dsidle"});
  const std::string shared = Section(json, "shared_memory");
  const std::string vm = Section(json, "vm");
  const std::string host_cpu = Section(json, "host_cpu");
  const std::string e2e = Section(json, "e2e");
  const std::string local = Section(json, "dsidle");
  ValidateObjectKeys(shared, "shared_memory", {"size_mb", "path", "device_path", "numa_node", "hwcc", "swcc"});
  ValidateObjectKeys(vm, "vm", {"count", "core_count_per_vm", "mem_size_mb_per_vm", "storage_path", "ssh_base_port", "numa_node", "local_ssh_pub_key", "first_ip", "bridge_tap_ip"});
  ValidateObjectKeys(host_cpu, "host_cpu", {"reserved_cores", "ivshmem_server_cores", "vm_cores"});
  ValidateObjectKeys(e2e, "e2e", {"foreground_worker_count_per_vm"});
  ValidateObjectKeys(local, "dsidle", {"replica_budget_mb", "hot_percentage_seed", "fixed_key_size", "fixed_value_size", "trace_dir", "latency_inject"});
  ExperimentConfig config;
  config.shared_size_mb = Integer(shared, "size_mb");
  config.shared_path = String(shared, "path");
  // cxlkv's experiment contract names the shared-memory directory; the
  // ivshmem-plain backing file under it has this fixed interoperable name.
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
  config.fixed_key_size = static_cast<std::uint32_t>(Integer(local, "fixed_key_size"));
  config.fixed_value_size = static_cast<std::uint32_t>(Integer(local, "fixed_value_size"));
  config.trace_dir = String(local, "trace_dir");
  const auto latency = Section(local, "latency_inject");
  ValidateObjectKeys(latency, "dsidle.latency_inject", {"enabled", "foreground_enabled", "merge_enabled", "stats_enabled", "cache_line_bytes", "swcc_read_ns_per_line", "swcc_write_ns_per_line", "swcc_flush_ns_per_line", "hwcc_read_ns_per_line", "hwcc_write_ns_per_line", "hwcc_atomic_load_ns", "hwcc_atomic_store_ns", "hwcc_atomic_rmw_ns", "cache_model", "cache_hits_enabled", "cache_capacity_lines", "cache_associativity", "cache_fixed_hit_rate", "cache_hit_extra_ns"});
  auto& l = config.latency_inject;
  l.enabled=Boolean(latency,"enabled"); l.foreground_enabled=Boolean(latency,"foreground_enabled"); l.merge_enabled=Boolean(latency,"merge_enabled"); l.stats_enabled=Boolean(latency,"stats_enabled"); l.cache_line_bytes=Integer(latency,"cache_line_bytes");
  l.swcc_read_ns_per_line=Number(latency,"swcc_read_ns_per_line"); l.swcc_write_ns_per_line=Number(latency,"swcc_write_ns_per_line"); l.swcc_flush_ns_per_line=Number(latency,"swcc_flush_ns_per_line"); l.hwcc_read_ns_per_line=Number(latency,"hwcc_read_ns_per_line"); l.hwcc_write_ns_per_line=Number(latency,"hwcc_write_ns_per_line"); l.hwcc_atomic_load_ns=Number(latency,"hwcc_atomic_load_ns"); l.hwcc_atomic_store_ns=Number(latency,"hwcc_atomic_store_ns"); l.hwcc_atomic_rmw_ns=Number(latency,"hwcc_atomic_rmw_ns");
  l.cache_model=latency_sim::ParseCacheModel(String(latency,"cache_model")); l.cache_hits_enabled=Boolean(latency,"cache_hits_enabled"); l.cache_capacity_lines=Integer(latency,"cache_capacity_lines"); l.cache_associativity=Integer(latency,"cache_associativity"); l.cache_fixed_hit_rate=Number(latency,"cache_fixed_hit_rate"); l.cache_hit_extra_ns=Number(latency,"cache_hit_extra_ns");
  if (!PowerOfTwo(config.shared_size_mb) || config.hwcc.offset_mb != 0 ||
      config.swcc.offset_mb != config.hwcc.size_mb ||
      config.hwcc.size_mb + config.swcc.size_mb != config.shared_size_mb ||
      !config.vm_count || !config.core_count_per_vm || !config.fixed_key_size || !config.fixed_value_size)
    throw std::runtime_error("invalid D-SIDLE shared-memory layout or topology");
  return config;
}

}  // namespace dsidle
