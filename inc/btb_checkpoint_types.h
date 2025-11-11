/*
 *    Copyright 2024 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef BTB_CHECKPOINT_TYPES_H
#define BTB_CHECKPOINT_TYPES_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "address.h"

namespace champsim
{
struct btb_checkpoint_state {
  struct direct_entry {
    long set = 0;
    long way = 0;
    uint64_t last_used = 0;
    champsim::address ip_tag{};
    champsim::address target{};
    uint8_t branch_type = 0;
  };

  long direct_sets = 0;
  long direct_ways = 0;
  std::vector<direct_entry> direct_entries;

  std::size_t indirect_table_size = 0;
  std::vector<champsim::address> indirect_targets;
  uint64_t indirect_history = 0;

  std::vector<champsim::address> return_stack;

  std::size_t call_size_tracker_size = 0;
  std::vector<typename champsim::address::difference_type> call_size_trackers;
};
} // namespace champsim

#endif
