#include "dsidle/config.h"

#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

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
  const std::string shared = Section(json, "shared_memory");
  const std::string vm = Section(json, "vm");
  const std::string e2e = Section(json, "e2e");
  const std::string local = Section(json, "dsidle");
  ExperimentConfig config;
  config.shared_size_mb = Integer(shared, "size_mb");
  config.shared_path = String(shared, "path");
  config.device_path = String(shared, "device_path");
  config.shared_numa_nodes = NumaNodes(shared, "numa_node");
  const auto hwcc = Section(shared, "hwcc");
  const auto swcc = Section(shared, "swcc");
  config.hwcc = {Integer(hwcc, "offset_mb"), Integer(hwcc, "size_mb")};
  config.swcc = {Integer(swcc, "offset_mb"), Integer(swcc, "size_mb")};
  config.vm_count = static_cast<std::uint32_t>(Integer(vm, "count"));
  config.core_count_per_vm = static_cast<std::uint32_t>(Integer(vm, "core_count_per_vm"));
  config.foreground_worker_count_per_vm = static_cast<std::uint32_t>(Integer(e2e, "foreground_worker_count_per_vm"));
  config.replica_budget_mb = Integer(local, "replica_budget_mb");
  config.fixed_key_size = static_cast<std::uint32_t>(Integer(local, "fixed_key_size"));
  config.fixed_value_size = static_cast<std::uint32_t>(Integer(local, "fixed_value_size"));
  if (!PowerOfTwo(config.shared_size_mb) || config.hwcc.offset_mb != 0 ||
      config.swcc.offset_mb != config.hwcc.size_mb ||
      config.hwcc.size_mb + config.swcc.size_mb != config.shared_size_mb ||
      !config.vm_count || !config.core_count_per_vm || !config.fixed_key_size || !config.fixed_value_size)
    throw std::runtime_error("invalid D-SIDLE shared-memory layout or topology");
  return config;
}

}  // namespace dsidle
