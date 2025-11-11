
/*
 * This file implements a basic Branch Target Buffer (BTB) structure.
 * It uses a set-associative BTB to predict the targets of non-return branches,
 * and it uses a small Return Address Stack (RAS) to predict the target of
 * returns.
 */

#include "basic_btb.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include "instruction.h"

namespace
{
direct_predictor::branch_info to_branch_info(uint8_t value)
{
  switch (value) {
  case static_cast<uint8_t>(direct_predictor::branch_info::INDIRECT):
    return direct_predictor::branch_info::INDIRECT;
  case static_cast<uint8_t>(direct_predictor::branch_info::RETURN):
    return direct_predictor::branch_info::RETURN;
  case static_cast<uint8_t>(direct_predictor::branch_info::CONDITIONAL):
    return direct_predictor::branch_info::CONDITIONAL;
  case static_cast<uint8_t>(direct_predictor::branch_info::ALWAYS_TAKEN):
  default:
    return direct_predictor::branch_info::ALWAYS_TAKEN;
  }
}
} // namespace

std::pair<champsim::address, bool> basic_btb::btb_prediction(champsim::address ip)
{
  // use BTB for all other branches + direct calls
  auto btb_entry = direct.check_hit(ip);

  // no prediction for this IP
  if (!btb_entry.has_value())
    return {champsim::address{}, false};

  if (btb_entry->type == direct_predictor::branch_info::RETURN)
    return ras.prediction();

  if (btb_entry->type == direct_predictor::branch_info::INDIRECT)
    return indirect.prediction(ip);

  return {btb_entry->target, btb_entry->type != direct_predictor::branch_info::CONDITIONAL};
}

void basic_btb::update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type)
{
  // add something to the RAS
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL)
    ras.push(ip);

  // updates for indirect branches
  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL))
    indirect.update_target(ip, branch_target);

  if (branch_type == BRANCH_CONDITIONAL)
    indirect.update_direction(taken);

  if (branch_type == BRANCH_RETURN)
    ras.calibrate_call_size(branch_target);

  direct.update(ip, branch_target, branch_type);
}

champsim::btb_checkpoint_state basic_btb::checkpoint_contents() const
{
  champsim::btb_checkpoint_state state;
  state.direct_sets = static_cast<long>(direct_predictor::sets);
  state.direct_ways = static_cast<long>(direct_predictor::ways);

  auto lru_entries = direct.BTB.checkpoint_contents();
  state.direct_entries.reserve(lru_entries.size());
  for (const auto& entry : lru_entries) {
    champsim::btb_checkpoint_state::direct_entry direct_entry{};
    direct_entry.set = entry.set;
    direct_entry.way = entry.way;
    direct_entry.last_used = entry.last_used;
    direct_entry.ip_tag = entry.data.ip_tag;
    direct_entry.target = entry.data.target;
    direct_entry.branch_type = static_cast<uint8_t>(entry.data.type);
    state.direct_entries.push_back(direct_entry);
  }

  state.indirect_table_size = std::size(indirect.predictor);
  state.indirect_targets.assign(std::begin(indirect.predictor), std::end(indirect.predictor));
  state.indirect_history = indirect.conditional_history.to_ullong();

  state.return_stack.assign(std::begin(ras.stack), std::end(ras.stack));

  state.call_size_tracker_size = std::size(ras.call_size_trackers);
  state.call_size_trackers.assign(std::begin(ras.call_size_trackers), std::end(ras.call_size_trackers));

  return state;
}

void basic_btb::restore_checkpoint(const champsim::btb_checkpoint_state& state)
{
  if (state.direct_sets != 0 && state.direct_sets != static_cast<long>(direct_predictor::sets)) {
    throw std::out_of_range("BTB checkpoint direct set count mismatch");
  }
  if (state.direct_ways != 0 && state.direct_ways != static_cast<long>(direct_predictor::ways)) {
    throw std::out_of_range("BTB checkpoint direct way count mismatch");
  }

  std::vector<decltype(direct.BTB)::checkpoint_entry> lru_entries;
  lru_entries.reserve(state.direct_entries.size());
  for (const auto& entry : state.direct_entries) {
    typename decltype(direct.BTB)::checkpoint_entry lru_entry{};
    lru_entry.set = entry.set;
    lru_entry.way = entry.way;
    lru_entry.last_used = entry.last_used;
    lru_entry.data.ip_tag = entry.ip_tag;
    lru_entry.data.target = entry.target;
    lru_entry.data.type = to_branch_info(entry.branch_type);
    lru_entries.push_back(lru_entry);
  }
  direct.BTB.restore_checkpoint(lru_entries);

  (void)state.indirect_table_size;
  if (!state.indirect_targets.empty() && state.indirect_targets.size() != std::size(indirect.predictor)) {
    throw std::out_of_range("BTB checkpoint indirect table size mismatch");
  }

  for (auto& target : indirect.predictor) {
    target = champsim::address{};
  }
  if (!state.indirect_targets.empty()) {
    std::copy(std::begin(state.indirect_targets), std::end(state.indirect_targets), std::begin(indirect.predictor));
  }
  indirect.conditional_history = decltype(indirect.conditional_history){state.indirect_history};

  ras.stack.clear();
  for (const auto& addr : state.return_stack) {
    ras.stack.push_back(addr);
    if (std::size(ras.stack) > return_stack::max_size) {
      ras.stack.pop_front();
    }
  }

  if (!state.call_size_trackers.empty() && state.call_size_trackers.size() != std::size(ras.call_size_trackers)) {
    throw std::out_of_range("BTB checkpoint call size tracker size mismatch");
  }
  (void)state.call_size_tracker_size;

  if (state.call_size_trackers.empty()) {
    std::fill(std::begin(ras.call_size_trackers), std::end(ras.call_size_trackers), typename champsim::address::difference_type{4});
  } else {
    std::copy(std::begin(state.call_size_trackers), std::end(state.call_size_trackers), std::begin(ras.call_size_trackers));
  }
}
