# ChampSim BTB Checkpoint 实现说明

## 背景
ChampSim 现有的 checkpoint 机制只序列化 cache/TLB。为了在跨阶段仿真时保持分支目标预测一致性，需要把 BTB（含直接表、间接表、返回栈等）也纳入 checkpoint。

## 核心改动
1. **统一状态表示**：在 `inc/btb_checkpoint_types.h` 定义 `champsim::btb_checkpoint_state`，描述 BTB 中直接表条目、间接目标表、返回栈和调用大小跟踪器。
2. **通用 LRU 快照**：`inc/msl/lru_table.h` 为任意 `lru_table` 提供 `checkpoint_contents()` / `restore_checkpoint()` / `clear()`，可提取 set/way、last_used 及数据实体，便于 BTB 直接套用。
3. **basic_btb 实现**：  
   - `btb/basic_btb/basic_btb.cc` 中 `checkpoint_contents()` 会序列化 direct predictor LRU 表、间接表、`conditional_history`、RAS 栈以及 `call_size_trackers`。  
   - `restore_checkpoint()` 反序列化并做几项健壮性检查（set/way/数组大小），然后重放到内部结构。
4. **模块接口扩展**：  
   - `inc/modules.h` 为 BTB 模块检测 `checkpoint_contents/restore_checkpoint`。  
   - `inc/ooo_cpu.h` / `src/ooo_cpu.cc` 将这些接口暴露给 `O3_CPU`，提供 `btb_checkpoint_contents()` 与 `restore_btb_checkpoint()`。
5. **Checkpoint 文件格式**：  
   - `src/cache_checkpoint.cc` 写入 cache 状态后，会为每个 CPU 写一段 `BTB: CPU <id>` 块，内含 direct 几何、所有 entry、间接表、返回栈与调用大小跟踪器。  
   - 读文件时按同样格式解析并调用 `restore_btb_checkpoint()`。  
   - 按 CPU 编号存储，未提供 BTB section 的 CPU 仍默认冷启动。

## 使用流程
1. **生成 checkpoint**：运行 ChampSim 时指定 `--cache-checkpoint <file>`，warmup 结束后文件中将同时写入 cache + BTB 状态。
2. **恢复 checkpoint**：后续 phase 或独立进程读取该文件，`load_cache_checkpoint()` 会自动恢复相应 CPU 的 BTB。
3. **兼容性**：  
   - 老文件没有 `BTB` 区块，解析器会直接跳过，缓存恢复逻辑不受影响。  
   - 仅 `basic_btb` 实现了序列化接口；若配置使用其他 BTB 模块，只要补上同名方法即可自动纳入。

## 进一步计划
* 在 CI 或脚本中补充“保存→恢复→对比 branch miss/IPC”的回归测试，验证 BTB checkpoint 与连续仿真一致性。  
* 若需要支持自定义 BTB，实现 `checkpoint_contents()` / `restore_checkpoint()` 即可复用当前 IO 流程。
