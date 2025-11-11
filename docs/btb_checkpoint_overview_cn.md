# BTB Checkpoint 总结

## 背景理解
1. **BTB vs. Branch Predictor**
   - Branch predictor 决定分支方向（taken/not-taken）；BTB 记录 “PC→目标地址” 信息，包含 direct predictor、indirect predictor、返回栈等结构。
   - 两者在 ChampSim 中是独立可替换模块，可以组合不同实现。
2. **Checkpoint 现状**
   - 原实现仅序列化 cache/TLB。CPU、ROB、分支预测器及 BTB 在恢复后需要 resume warmup 才能重新收敛。
   - 需求：在 warmup→simulation 或跨进程恢复时维持 BTB 状态，减少恢复成本与偏差。

## 设计与实现方案
1. **统一数据结构**
   - 新增 `champsim::btb_checkpoint_state`（`inc/btb_checkpoint_types.h`），描述：
     - Direct predictor LRU 表条目（set/way/last_used/IP/target/type）。
     - Indirect predictor 4096 项数组 + `conditional_history`。
     - Return address stack 和 `call_size_trackers`。
2. **基础设施改动**
   - `champsim::msl::lru_table` 追加 `checkpoint_contents()/restore_checkpoint()/clear()`，通用于 direct predictor。
   - BTB 模块接口扩展：检测 `checkpoint_contents` 与 `restore_checkpoint`，并在 `O3_CPU` 中暴露 `btb_checkpoint_contents()`/`restore_btb_checkpoint()`。
3. **basic_btb 实现**
   - `checkpoint_contents()`：遍历 direct predictor LRU 表、复制 indirect predictor 数组/历史、转储 RAS 栈与调用大小跟踪器。
   - `restore_checkpoint()`：对输入状态进行几何校验（set/way/数组长度），重建 direct 表、间接表、history、RAS 与 trackers。
4. **Checkpoint 文件格式**
   - `save_cache_checkpoint()` 在 cache 列表后按 CPU 写入：
     ```
     BTB: CPU <id>
       DirectGeometry: Sets <S> Ways <W>
       IndirectSize: <N>
       IndirectHistory: <value>
       CallSizeTrackerSize: <M>
       DirectEntry: ...
       IndirectEntry: ...
       ReturnStackEntry: ...
       CallSizeTracker: ...
     EndBTB
     ```
   - `load_cache_checkpoint()` 解析上述块后通过 `restore_btb_checkpoint()` 重放。缺失 BTB 块的 CPU 仍默认冷启动。
5. **文档**
   - `docs/btb_checkpoint_cn.md` 记录实现细节、兼容性说明及使用步骤。

## 未来测试计划
1. **构建验证**
   - 运行 `./config.sh …` 生成 `_configuration.mk` 后 `make`，确保新增接口编译通过。
2. **功能测试**
   - 单核/多核场景：
     1. Warmup 运行，生成 checkpoint。
     2. 复制 checkpoint，执行 resume warmup + simulation；对比 Branch MPKI/IPC 与单次长跑。
   - 对比 “无 BTB checkpoint” vs “启用 BTB checkpoint” 的恢复收敛速度。
3. **回归脚本**
   - 扩展 `rl_controller/compare_checkpoint.py` 或新增脚本，自动执行：
     - Warmup-only run → 生成 checkpoint。
     - 从 checkpoint 恢复窗口 run。
     - 单次 baseline run。
     - 汇总 core IPC / branch miss delta，以检测 BTB 状态是否成功复现。
4. **模块兼容**
   - 若引入新的 BTB 模块，要求实现相同接口；可在 CI 中通过反射检查缺失的 `checkpoint_contents/restore_checkpoint` 并发出警告。

## 注意事项
- 老格式 checkpoint 不含 BTB 段，加载时自动忽略，保持向后兼容。
- 目前仅 `basic_btb` 支持序列化；对其他 BTB 模块需额外实现同名方法。
- BTB 恢复不代表 ROB/预测器状态同步，仍建议保留少量 resume warmup 以冲洗队列。
