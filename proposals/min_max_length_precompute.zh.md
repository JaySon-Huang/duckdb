# Proposal: MIN/MAX(LENGTH/CHAR_LENGTH) 提前聚合与裁剪(main 分支实现方案)

**日期**: 2026-08-20
**目的**: 在 main 分支(commit `ce512b864c`)上直接实现 `MIN/MAX(LENGTH/CHAR_LENGTH/strlen/octet_length)` 的统计提前聚合与 row group 裁剪,不依赖 PR #23611(未合入)的逐分区分类框架。

## Summary

在 `StatisticsPropagator::TryExecuteAggregates` 的现有单块结构上嵌入 length 聚合支持:识别 `MIN/MAX` 聚合输入为 length 家族函数(`strlen`/`octet_length`/`length`/`char_length`)的模式,利用 `StringStats` 的 `max_string_length`/`min_string_length`/`has_unicode`(segment 级、精确、持久化)做计划期判定:

- **统计全部精确时零扫描**:`MIN/MAX(strlen/octet_length)` 直接等于各 row group 长度统计的极值;`MIN/MAX(length/char_length)` 在 `has_unicode == false`(全 ASCII)的 row group 上同理精确。
- **含 Unicode 的 row group(码点语义)提取安全边界**:MAX 方向 `UB = max_string_length`(`字符数 ≤ 字节数`),MIN 方向 `LB_p = ceil(min_string_length / 4)`(UTF-8 每字符最多 4 字节);纯 length 查询时用"候选极值"裁剪不可能刷新全局极值的分区(复用 main 现有的 `need_to_scan` 部分预计算出口)。
- **统计不精确的分区 → 整体 bail**(与 main 现状一致,不引入 #23611 的逐分区降级能力)。

只修改 `src/optimizer/statistics/operator/propagate_aggregate.cpp`,不改变存储格式、不修改表函数接口;不包含 length-only 扫描(#11051,独立 proposal)。

## Context

### Current State

**DuckDB main 现状**(2026-08,commit `ce512b864c`):

1. **提前聚合框架存在但为单块结构**:`TryExecuteAggregates`(`src/optimizer/statistics/operator/propagate_aggregate.cpp:179`)支持无 GROUP BY 的 `count_star`/`min`/`max` 提前计算:
   - 识别:`TryGetMinMaxColumnInfo`(`:98`)只接受 `BOUND_COLUMN_REF` 或安全 cast,带 `input_type`/`result_type`(支持 cast 形态);
   - **无 filter 分支**:遍历全部分区提取值,任一分区统计不精确(`MinMaxIsExact`/`HasPendingWrites` 门控,`:133`)或 `COUNT_APPROXIMATE` → **整体 bail**;
   - **filter 分支**(`:285`):逐分区三态判定(`FILTER_ALWAYS_TRUE` → 预计算集合,`FILTER_ALWAYS_FALSE` → 丢弃,`NO_PRUNING_POSSIBLE` → `scan_partition_indices`),`need_to_scan` 时走部分预计算改写(`COALESCE(least/greatest(pre, agg), pre)` + `set_partitions_to_scan`,`:398-456`)。
2. **底层机制齐备**:`get_partition_stats` / `set_partitions_to_scan` 回调、`PartitionRowGroup`(`MinMaxIsExact(storage_index)` 单参数 + 独立 `HasPendingWrites`,`src/include/duckdb/function/partition_stats.hpp:29-35`)、`StringStats`(`max_string_length`/`min_string_length`/`has_unicode`,精确且随 checkpoint 持久化)。
3. **#23611 未合入**:无 filter 分支没有"不精确分区降级为扫描"的能力;该能力属于 #23611(逐分区分类重构),本 proposal 不引入。
4. **运行期已有 CPU 层优化**:`LengthPropagateStats`(`src/function/scalar/string/length.cpp:64`)在列全 ASCII 时把 `length` 执行函数替换为 `StrLenOperator`,但不改变列读取。

### Problem Statement

对"数百 GB 字符串列求最大/最小长度"的查询(如 TiFlash issue #11049 场景 `SELECT MAX(CHAR_LENGTH(c1)), ... FROM t`),DuckDB 必须读完整列 payload。所需材料(精确长度统计、提前聚合框架、部分扫描下推)在 main 上齐备,缺的只是"聚合输入是 length 函数"的识别与统计折叠逻辑。

### Constraints

- **结果语义与朴素执行一致**:`MIN`/`MAX` 忽略 NULL;全 NULL 列返回 NULL;`length`/`char_length` 是码点数,`strlen`/`octet_length` 是字节数。
- **统计不确定即不可用**:不精确分区(`MinMaxIsExact`/`HasPendingWrites`/`COUNT_APPROXIMATE`)→ 整体 bail(沿用 main 现状,不做 #23611 的逐分区降级)。
- **不修改存储格式与表函数接口**:只改优化器;存量数据无需迁移。
- **不包含 length-only 扫描**:#11051 的存储层下推为独立 proposal。

## Goals

1. `MIN/MAX(strlen/octet_length(col))` 在统计全部精确时零扫描(常量结果)。
2. `MIN/MAX(length/char_length(col))` 在全 ASCII row group 上零扫描;含 Unicode row group 用安全边界裁剪,纯 length 查询时只扫描可能刷新全局极值的分区。
3. 与 `count_star`/普通列 `min`/`max` 聚合可混合(全精确时全部预计算;含边界值分区时按分区整体处理)。
4. 正确性由测试矩阵锁定(Unicode、NULL、空表、overflow 字符串、混合聚合、CSE 场景不误优化)。

## Non-Goals

- **不移植 #23611 的逐分区分类框架与不精确降级**:统计不精确时整体 bail(与 main 现状一致);#23611 若未来合入,本实现与其天然兼容(不冲突)。
- **不实现 `SUM(LENGTH)`/`AVG(LENGTH)` 的统计折叠**:`total_string_length` 已存在,列为后续。
- **不实现 length-only 扫描(#11051)**:见 `length_only_scan.zh.md`。
- **不支持公共子表达式(CSE)提升场景**:聚合输入被提升为计算列引用时,length 识别不生效(与分支实现不同,main 单块结构下该场景退化为现状,结果正确)。
- **不修改 `LengthPropagateStats` 运行期函数替换**。

## Design

### 1) 现状架构快照(main)

```
SELECT MAX(CHAR_LENGTH(c1)), MAX(CHAR_LENGTH(c2)) FROM t;

AGGREGATE(MAX x 2)
  └─ PROJECTION(char_length(c1), char_length(c2))
       └─ GET(t, 全部分区)        ← 当前:全量扫描,读全部 payload

TryExecuteAggregates(现状):
  识别:child 是 length 函数 → TryGetMinMaxColumnInfo 失败 → bail → 无优化
```

### 2) 识别(嵌入 min/max 分支)

`TryGetMinMaxColumnInfo` 失败后,尝试 length 家族识别(与分支 Phase B/C 同源):

- 聚合方向 `min`/`max`;聚合输入是 `BoundFunctionExpression`,函数名 ∈ `{strlen, octet_length, length, char_length}`(按函数名识别,不受 `LengthPropagateStats` 运行期替换影响);
- 唯一子节点是 `BOUND_COLUMN_REF`;`length`/`char_length` 校验输入类型为 VARCHAR(排除 `length(bit)`/`length(list)`);`strlen`/`octet_length` 接受 VARCHAR/BLOB;
- 记录:`binding`、`input_type`、`result_type`(BIGINT)、方向、字节/码点语义类别(`length_is_char`)、聚合在 `aggr.expressions` 的位置;
- 穿越 PROJECTION 时在投影表达式中查找同构 `length(colref)`(与 `TryGetMinMaxColumnInfo` 的穿越逻辑并列);CSE 提升的计算列引用场景不识别(见 Non-Goals)。

### 3) 值提取与分类(嵌入预计算段)

新增 `TryGetLengthValueFromStats`(main 版,门控与 `TryGetValueFromStats` 对齐:`MinMaxIsExact(storage_index)` + `HasPendingWrites()` + `STRING_STATS` + `CanHaveNoNull()`),输出"值 + 类别":

| 情形 | 类别 | 值 |
| --- | --- | --- |
| 字节语义(`strlen`/`octet_length`) | EXACT | `max_string_length`(MAX)/`min_string_length`(MIN) |
| 码点语义且 `!CanContainUnicode`(全 ASCII) | EXACT | 同上(字符数 == 字节数) |
| 码点语义且可能含 Unicode | BOUNDED | MAX:`UB = max_string_length`;MIN:`LB_p = ceil(min_string_length / 4)` |
| 统计不精确 / 无长度统计 | bail(整体) | — |

### 4) 折叠与裁剪(单块内两遍)

- **第一遍(折叠)**:遍历分区提取值;EXACT 值折叠进候选极值(`LB = max(EXACT)` / `UB_g = min(EXACT)`);BOUNDED 值暂存(值 + 分区索引);任一提取失败 → 整体 bail。
- **第二遍(裁剪,仅纯 length 查询)**:当查询只含 length 聚合(无普通 `min`/`max`/`count_star`)时,对 BOUNDED 分区判定:
  - MAX 方向:`UB <= LB` → 该分区不可能刷新全局最大值 → 跳过(不扫描、不进预计算);
  - MIN 方向:`LB_p >= UB_g` → 跳过;
  - 否则 → 收集进 `scan_partition_indices`,复用 main 的 `need_to_scan` 部分预计算出口。
- **混合查询**:含普通 `min`/`max`/`count_star` 时,BOUNDED 分区一律进 `scan_partition_indices`(分区需要为其他聚合贡献精确值,不能跳过);EXACT 分区照常预计算。

**正确性论证**(MAX 方向):被跳过分区真实最大值 ≤ `UB ≤ LB`;扫描分区真实最大值由聚合算出 `S`;全局最大值 = `max(S, 被跳过区真实值) ≤ max(S, LB)`,且 `LB` 来自某分区精确值(≤ 全局最大值),故 `max(S, LB)` 精确。MIN 方向对偶。

### 5) 出口

- SCAN 列表为空 → 常量替换(`LogicalExpressionGet`,main 现有路径)。
- SCAN 列表非空 → 部分预计算(main 现有 `need_to_scan` 改写:PROJECTION + `COALESCE(least/greatest(pre, agg), pre)` + `set_partitions_to_scan`;length 聚合的方向经 `is_min` 向量传给改写逻辑——main 当前按函数名 `min`/`max` 判定,需改为按位置的方向向量,与分支 Phase B 的改动相同)。
- 任一提取失败/不精确 → 整体 bail(与 main 现状一致)。

### 6) 失败/降级路径

- 无法识别(非 length 函数、`length(bit)`、CSE 提升)→ 走现状路径(普通 min/max 或 bail),语义安全。
- 统计不精确 → 整体 bail(无 filter);filter 分支的逐分区机制不受影响(可与其他聚合共存)。
- `set_partitions_to_scan` 缺失且 SCAN 非空 → bail(现有保护)。

## Incremental Plan

### Phase A: 字节语义(`MIN/MAX(strlen/octet_length)`)

- 识别 + `TryGetLengthValueFromStats`(EXACT 路径)+ 折叠 + 常量替换;不精确 → bail。
- 验收:移植分支 `max_len_skip.test` 的字节语义断言(全精确零扫描、混合 count、不精确场景改为"不优化但结果正确")。

### Phase B: 码点语义(`MIN/MAX(length/char_length)`)+ BOUNDED 裁剪

- `CanContainUnicode` 判定 + BOUNDED 值;第二遍裁剪(纯 length 查询 SKIP;混合查询 SCAN);`need_to_scan` 出口的 `is_min` 向量化。
- 验收:移植分支 `max_len_skip_char.test`(全 ASCII 零扫描、Unicode 上界可/不可裁剪、MIN 下界裁剪、BIT 不误识别);无 filter 的 Unicode 裁剪场景与分支实现结果一致(408/4070 级)。

### Phase C: 测试矩阵与基准

- 边界:全 NULL、空表、单值列、空字符串、overflow 大字符串、混合聚合、CSE 不误优化、`storage_compatibility_version='latest'` 持久化前提。
- 基准:复用 `benchmark/max_len_skip/`(服务器数据仍在),对照"main 基线(无优化)"与"main + 本优化"的行组扫描数与 wall time;门槛:零扫描断言、Unicode 裁剪 ≈ 10%、分散分布不劣于基线、非目标 ±5%。

## Validation Strategy

- **正确性**:每场景与 `PRAGMA disable_optimizer` 对照;重点:多字节 UTF-8(码点 ≠ 字节)、全 NULL(NULL 结果)、空表、删除后不精确(结果正确但退化为全扫)、overflow 字符串、`length(bit)`/`length(list)` 不误识别。
- **计划形态**:`EXPLAIN` 断言零扫描(`EXPRESSION_GET`/无 `UNGROUPED_AGGREGATE`)与部分路径(`COALESCE` + `UNGROUPED_AGGREGATE`);`EXPLAIN ANALYZE` 断言 `Row Groups Scanned`。
- **回归**:`make allunit`;测试置于 `test/optimizer/`(`max_len_skip.test`/`max_len_skip_char.test` 从分支移植并调整不精确断言)。
- **基准门槛**(服务器):全精确表扫描数 0;`len_unicode` 的 `MAX(char_length)` ≈ 10%(理论:unicode 块 + 1 边界);`unicode_all` 不劣于基线;`sum(i)`/`count(*)` ±5% 无回归。

## Risks and Mitigations

1. **码点/字节语义混淆** — 仅 `CanContainUnicode == false` 产生精确码点值;含 Unicode 分区只用安全边界;测试含多字节字符。
2. **BOUNDED 裁剪在混合查询下的正确性** — 混合查询 BOUNDED 分区一律扫描(分区级一致性);纯 length 查询的 SKIP 有第 4 节正确性论证;组合测试覆盖。
3. **不精确统计整体 bail(接受)** — 与 main 现状一致,结果正确仅次优;#23611 未来合入后自动获得降级能力,本实现不冲突。
4. **`need_to_scan` 出口的方向判定** — main 按函数名 `min`/`max` 选择 `least`/`greatest`,length 聚合函数名不是 `min`/`max`,需改为按位置的方向向量;回归 `partial_aggregate_precomputation.test` 的既有断言。
5. **CSE 场景不优化(接受)** — 聚合输入被公共子表达式提升为计算列时识别不生效,退化为现状;测试断言"不优化但结果正确"。
6. **与 `LengthPropagateStats` 交互** — 识别按函数名,运行期替换不改变函数名。
7. **`min_string_length` 持久化依赖存储格式** — 默认 v0.10.2 兼容格式 checkpoint 后 `min_string_length` 丢失(MIN 优化 reopen 后失效);测试与基准需 `storage_compatibility_version='latest'`。

## Open Questions

1. **`SUM(LENGTH)`/`AVG(LENGTH)` 的统计折叠** — 推荐:本 proposal 完成后跟进;`total_string_length` 精确时可直接提前计算,与 MAX/MIN 同框架扩展。
2. **与 #23611 未来合入的适配** — 本实现嵌入单块结构;#23611 合入后需将 length 逻辑迁移到其逐分区分类框架(迁移面:识别 + 值提取 + 裁剪,预计可控),或等待其合入后直接在其框架上重做。推荐:先在本 proposal 内完成单块实现,合入时再迁移。

## Alternatives Considered

- **先移植 #23611 再实现**:获得不精确降级与逐分区框架,但引入其他作者的 PR 能力,超出本 proposal 范围;且 #23611 两年未合入,挂靠风险高。
- **依赖运行时函数替换(现状)**:只省 CPU 不省 IO,不解决磁盘带宽瓶颈。
- **length-only 扫描(#11051)**:与统计折叠正交(计划期避免扫描 vs 执行期最小化读取),独立 proposal,不在此实现。

## Appendix: Upstream duckdb 相关 issue/discussion 调研(2026-08-20)

### 直接相关(已合入 upstream):#23872 → PR #23873

- **#23872** "`strlen` doesn't leverage string length stats for row group pruning"
  (2026-07-16 创建):`WHERE strlen(v) > 30` 应只扫 1 个 row group,实际扫全部。
- **PR #23873** "Prune row group for `strlen` function via string length stats":
  已 MERGED(2026-07-20),改动 `src/function/scalar/string/length.cpp` 与
  `test/optimizer/statistics/statistics_string_length.test`。

**方向互补,不重叠**:#23873 做 **filter 场景**(`WHERE strlen(col) > N` 用
`max_string_length` 裁剪 row group);本 proposal 做 **聚合场景**
(`MIN/MAX(strlen/char_length(col))` 统计折叠)。**聚合场景在 upstream 没有任何
对应 issue/PR**——本 proposal 填补的是 upstream 空白方向。

main 上组合验证(本分支基于 main,含 #23873):

```
WHERE strlen(v) > 30                     → Row Groups Scanned: 1 / 2  (#23873 生效)
SELECT max(strlen(v)) WHERE strlen(v)>30 → Row Groups Scanned: 1 / 2  (组合,裁剪叠加)
```

### 弱相关

- **#23633** "Total string length records incorrectly"(已关闭):dictionary 压缩下
  `total_string_length` 统计错误(报告 5 vs 实际 5000)——本 proposal Open Question
  的 `SUM(LENGTH)` 跟进需注意 `total_string_length` 的统计可靠性。
- **discussion #18657** "retain varchar(length) information":ETL 场景希望数据字典
  保留 varchar 长度信息——元数据保留诉求,与优化无关。

### 无对应

- **length-only 扫描**(TiFlash #11051 对应):upstream 无任何 issue/discussion;
  `projection_expression_pushdown` + `ParquetProjectionExpressionPushdown` 是
  parquet 扩展的内部实现,没有对应的功能请求。
- **`char_length` 的统计裁剪**:#23873 只覆盖 `strlen`(字节语义),`char_length`
  的 ASCII 判定裁剪(本 proposal Phase B)upstream 也没有。

### 启示

1. 聚合统计折叠是 upstream 空白方向,有提交价值;提交 PR 时可引用 #23873 作为
   "同一统计信息的不同应用方向"的上下文。
2. `SUM(LENGTH)` 跟进需先确认 `total_string_length` 统计在压缩列上的可靠性(#23633)。
3. 可补充 `char_length` 的 ASCII 判定裁剪(upstream 未覆盖,本 proposal 已实现)。
