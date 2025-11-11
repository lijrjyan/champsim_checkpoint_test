#include "cache_checkpoint.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include <fmt/core.h>
#include <fmt/ostream.h>

#include "cache.h"
#include "environment.h"
#include "ooo_cpu.h"

namespace
{
std::string trim_copy(std::string_view text)
{
  auto begin = text.begin();
  auto end = text.end();

  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };

  while (begin != end && is_space(static_cast<unsigned char>(*begin))) {
    ++begin;
  }

  while (begin != end && is_space(static_cast<unsigned char>(*(end - 1)))) {
    --end;
  }

  return std::string(begin, end);
}

champsim::address parse_address_token(const std::string& token)
{
  std::size_t consumed = 0;
  unsigned long long value = 0;

  try {
    value = std::stoull(token, &consumed, 0);
  } catch (const std::exception& ex) {
    throw std::runtime_error(fmt::format("Failed to parse address token '{}' ({})", token, ex.what()));
  }

  if (consumed != token.size()) {
    throw std::runtime_error(fmt::format("Failed to parse address token '{}' ({} characters consumed)", token, consumed));
  }

  return champsim::address{value};
}
} // namespace

namespace champsim
{
void save_cache_checkpoint(environment& env, const std::filesystem::path& file_path)
{
  std::ofstream out_file{file_path};
  if (!out_file.is_open()) {
    throw std::runtime_error(fmt::format("Unable to open '{}' for writing cache checkpoint", file_path.string()));
  }

  for (const CACHE& cache : env.cache_view()) {
    fmt::print(out_file, "Cache: {}\n", cache.NAME);
    for (const auto& entry : cache.checkpoint_contents()) {
      fmt::print(out_file, "  Set: {} Way: {} Address: {}\n", entry.set, entry.way, entry.block.address);
    }
    fmt::print(out_file, "EndCache\n");
  }

  for (O3_CPU& cpu : env.cpu_view()) {
    auto state = cpu.btb_checkpoint_contents();
    if (!state) {
      continue;
    }

    fmt::print(out_file, "BTB: CPU {}\n", cpu.cpu);
    fmt::print(out_file, "  DirectGeometry: Sets {} Ways {}\n", state->direct_sets, state->direct_ways);
    fmt::print(out_file, "  IndirectSize: {}\n", state->indirect_table_size);
    fmt::print(out_file, "  IndirectHistory: {}\n", state->indirect_history);
    fmt::print(out_file, "  CallSizeTrackerSize: {}\n", state->call_size_tracker_size);

    for (const auto& entry : state->direct_entries) {
      fmt::print(out_file, "  DirectEntry: Set {} Way {} LastUsed {} IP: {} Target: {} Type: {}\n", entry.set, entry.way, entry.last_used, entry.ip_tag,
                 entry.target, entry.branch_type);
    }

    for (std::size_t index = 0; index < state->indirect_targets.size(); ++index) {
      fmt::print(out_file, "  IndirectEntry: Index {} Target: {}\n", index, state->indirect_targets.at(index));
    }

    for (const auto& addr : state->return_stack) {
      fmt::print(out_file, "  ReturnStackEntry: {}\n", addr);
    }

    for (std::size_t index = 0; index < state->call_size_trackers.size(); ++index) {
      fmt::print(out_file, "  CallSizeTracker: Index {} Size {}\n", index, state->call_size_trackers.at(index));
    }

    fmt::print(out_file, "EndBTB\n");
  }
}

void load_cache_checkpoint(environment& env, const std::filesystem::path& file_path)
{
  std::ifstream in_file{file_path};
  if (!in_file.is_open()) {
    throw std::runtime_error(fmt::format("Unable to open '{}' for reading cache checkpoint", file_path.string()));
  }

  std::unordered_map<std::string, std::vector<CACHE::checkpoint_entry>> checkpoints;
  std::unordered_map<long, champsim::btb_checkpoint_state> btb_checkpoints;
  std::string current_cache;
  long current_btb_cpu = -1;
  std::string line;
  long line_number = 0;

  while (std::getline(in_file, line)) {
    ++line_number;
    auto trimmed_line = trim_copy(line);
    if (trimmed_line.empty()) {
      continue;
    }

    std::istringstream iss(trimmed_line);
    std::string token;
    iss >> token;

    if (token.empty()) {
      continue;
    }

    if (token == "Cache:") {
      std::string remainder;
      std::getline(iss >> std::ws, remainder);
      current_cache = trim_copy(remainder);
      checkpoints[current_cache]; // Ensure entry exists
      continue;
    }

    if (token == "EndCache") {
      current_cache.clear();
      continue;
    }

    if (token == "#") {
      continue;
    }

    if (token == "BTB:") {
      std::string cpu_label;
      if (!(iss >> cpu_label) || cpu_label != "CPU") {
        throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'CPU' after 'BTB:'", line_number));
      }

      long cpu_id = -1;
      if (!(iss >> cpu_id)) {
        throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing CPU id for BTB section", line_number));
      }

      current_btb_cpu = cpu_id;
      btb_checkpoints[current_btb_cpu];
      continue;
    }

    if (token == "EndBTB") {
      if (current_btb_cpu < 0) {
        throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: 'EndBTB' without active BTB section", line_number));
      }
      current_btb_cpu = -1;
      continue;
    }

    if (current_btb_cpu >= 0) {
      auto& state = btb_checkpoints[current_btb_cpu];

      if (token == "DirectGeometry:") {
        std::string sets_label;
        if (!(iss >> sets_label) || sets_label != "Sets") {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'Sets' token in DirectGeometry", line_number));
        }
        if (!(iss >> state.direct_sets)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing direct set count", line_number));
        }

        std::string ways_label;
        if (!(iss >> ways_label) || ways_label != "Ways") {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'Ways' token in DirectGeometry", line_number));
        }
        if (!(iss >> state.direct_ways)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing direct way count", line_number));
        }
        continue;
      }

      if (token == "DirectEntry:") {
        champsim::btb_checkpoint_state::direct_entry entry{};
        std::string label;

        if (!(iss >> label) || label != "Set") {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'Set' token for DirectEntry", line_number));
        }
        if (!(iss >> entry.set)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing direct set value", line_number));
        }

        if (!(iss >> label) || label != "Way") {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'Way' token for DirectEntry", line_number));
        }
        if (!(iss >> entry.way)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing direct way value", line_number));
        }

        if (!(iss >> label) || label != "LastUsed") {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'LastUsed' token for DirectEntry", line_number));
        }
        if (!(iss >> entry.last_used)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing last_used value for DirectEntry", line_number));
        }

        if (!(iss >> label) || label != "IP:") {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'IP:' token for DirectEntry", line_number));
        }
        std::string ip_token;
        if (!(iss >> ip_token)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing IP value for DirectEntry", line_number));
        }
        entry.ip_tag = parse_address_token(ip_token);

        if (!(iss >> label) || label != "Target:") {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'Target:' token for DirectEntry", line_number));
        }
        std::string target_token;
        if (!(iss >> target_token)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing target value for DirectEntry", line_number));
        }
        entry.target = parse_address_token(target_token);

        if (!(iss >> label) || label != "Type:") {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'Type:' token for DirectEntry", line_number));
        }
        long branch_type = 0;
        if (!(iss >> branch_type)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing type value for DirectEntry", line_number));
        }
        entry.branch_type = static_cast<uint8_t>(branch_type);

        state.direct_entries.push_back(entry);
        continue;
      }

      if (token == "IndirectSize:") {
        if (!(iss >> state.indirect_table_size)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing IndirectSize value", line_number));
        }
        state.indirect_targets.resize(state.indirect_table_size);
        continue;
      }

      if (token == "IndirectHistory:") {
        if (!(iss >> state.indirect_history)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing IndirectHistory value", line_number));
        }
        continue;
      }

      if (token == "IndirectEntry:") {
        std::string label;
        if (!(iss >> label) || label != "Index") {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'Index' token for IndirectEntry", line_number));
        }
        std::size_t index = 0;
        if (!(iss >> index)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing index value for IndirectEntry", line_number));
        }

        if (!(iss >> label) || label != "Target:") {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'Target:' token for IndirectEntry", line_number));
        }
        std::string addr_token;
        if (!(iss >> addr_token)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing target value for IndirectEntry", line_number));
        }

        if (state.indirect_targets.size() <= index) {
          state.indirect_targets.resize(index + 1);
        }
        state.indirect_targets.at(index) = parse_address_token(addr_token);
        continue;
      }

      if (token == "ReturnStackEntry:") {
        std::string addr_token;
        if (!(iss >> addr_token)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing address for ReturnStackEntry", line_number));
        }
        state.return_stack.push_back(parse_address_token(addr_token));
        continue;
      }

      if (token == "CallSizeTrackerSize:") {
        if (!(iss >> state.call_size_tracker_size)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing CallSizeTrackerSize value", line_number));
        }
        state.call_size_trackers.resize(state.call_size_tracker_size);
        continue;
      }

      if (token == "CallSizeTracker:") {
        std::string label;
        if (!(iss >> label) || label != "Index") {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'Index' token for CallSizeTracker", line_number));
        }
        std::size_t index = 0;
        if (!(iss >> index)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing index for CallSizeTracker", line_number));
        }

        if (!(iss >> label) || label != "Size") {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'Size' token for CallSizeTracker", line_number));
        }
        long long size_value = 0;
        if (!(iss >> size_value)) {
          throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing size value for CallSizeTracker", line_number));
        }

        if (state.call_size_trackers.size() <= index) {
          state.call_size_trackers.resize(index + 1);
        }
        state.call_size_trackers.at(index) = static_cast<typename champsim::address::difference_type>(size_value);
        continue;
      }

      throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: unexpected BTB token '{}'", line_number, token));
    }

    if (token == "Set:") {
      if (current_cache.empty()) {
        throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: 'Set' entry without active cache", line_number));
      }

      long set = -1;
      if (!(iss >> set)) {
        throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing set value", line_number));
      }

      std::string way_label;
      if (!(iss >> way_label) || way_label != "Way:") {
        throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'Way:' token", line_number));
      }

      long way = -1;
      if (!(iss >> way)) {
        throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing way value", line_number));
      }

      std::string addr_label;
      if (!(iss >> addr_label) || addr_label != "Address:") {
        throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: expected 'Address:' token", line_number));
      }

      std::string addr_token;
      if (!(iss >> addr_token)) {
        throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: missing address token", line_number));
      }

      CACHE::checkpoint_entry entry{};
      entry.set = set;
      entry.way = way;
      entry.block.valid = true;
      entry.block.address = parse_address_token(addr_token);
      entry.block.v_address = entry.block.address;

      checkpoints[current_cache].push_back(entry);
      continue;
    }

    throw std::runtime_error(fmt::format("Checkpoint parse error on line {}: unexpected token '{}'", line_number, token));
  }

  for (CACHE& cache : env.cache_view()) {
    auto it = checkpoints.find(cache.NAME);
    if (it != std::end(checkpoints)) {
      cache.restore_checkpoint(it->second);
    } else {
      cache.restore_checkpoint({});
    }
  }

  for (O3_CPU& cpu : env.cpu_view()) {
    auto it = btb_checkpoints.find(static_cast<long>(cpu.cpu));
    if (it != std::end(btb_checkpoints)) {
      cpu.restore_btb_checkpoint(it->second);
    }
  }
}
} // namespace champsim
