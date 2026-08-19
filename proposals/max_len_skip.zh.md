# Proposal: 基于 Row Group 长度统计的 MIN/MAX(LENGTH/CHAR_LENGTH) 提前聚合与裁剪(max_len skip)

**日期**: 2026-08-19
**目的**: 让 `SELECT MAX(LENGTH(c)), MAX(CHAR_LENGTH(c)) ... FROM t` 这类全表长度统计查询在优化器层面利用 row group 的字符串长度统计(`max_string_length`/`min_string_length`/`has_unicode`)提前计算或裁剪扫描范围。

## Summary

在 `StatisticsPropagator::TryExecuteAggregates` 中扩展对 `MIN/MAX(LENGTH(col))`、`MIN/MAX(CHAR_LENGTH(col))`、`MIN/MAX(strlen(col))`、`MIN/MAX(octet_length(col))` 的识别,利用 `StringStats` 中已持久化的 `max_string_length` / `min_string_length`(segment 级、精确、覆盖存量数据)做计划期全局判定:

- **全部 row group 统计精确时**:`MAX(LENGTH(col))` 直接等于各 row group `max_string_length` 的最大值,`MIN(LENGTH(col))` 直接等于 `min_string_length` 的最小值,零扫描;`CHAR_LENGTH` 在 `has_unicode == false`(全 ASCII)的 row group 上同理精确。
- **部分 row group 统计不精确或含 Unicode 时**:对 MAX 用字节长度作字符数安全上界(`字符数 ≤ 字节数`),对 MIN 用 `ceil(字节数 / 4)` 作字符数安全下界(UTF-8 每字符最多 4 字节);以"已确认的精确边界值"裁剪不可能刷新全局极值的 row group,其余 row group 通过现有 `set_partitions_to_scan` 回调只扫描需要的部分,并沿 #21831 的表达式改写模式合并结果。

该优化只依赖优化器与现有统计接口,不修改存储格式、不修改表函数接口,对原生表(`seq_scan`)生效;parquet 因文件格式无长度统计而暂不支持(见 Non-Goals)。

## Context

### Current State

**问题查询**:`SELECT MAX(CHAR_LENGTH(c1)), ..., MAX(CHAR_LENGTH(c16)) FROM t`(以及求最小长度的对偶场景)在无过滤条件下全表扫描,瓶颈是读取字符串列 payload 本身,而非聚合计算。

**DuckDB 的现状**:

1. **长度统计已存在且精确**:`StringStatsData` 内含 `max_string_length` / `min_string_length` / `total_string_length` / `has_unicode`(`src/include/duckdb/storage/statistics/string_stats.hpp`)。它是写数据时维护的 segment 级统计,随 checkpoint 持久化(`src/storage/checkpoint/table_data_writer.cpp:114`),`StringStats::Merge` 正确合并(`src/storage/statistics/string_stats.cpp:638-658`)。Row group 级通过 `PartitionRowGroup::GetColumnStatistics` 可获取,精确性由 `MinMaxIsExact()` 与 `HasPendingWrites()` 判定(`src/storage/table/row_group.cpp:1961-2008`)。
2. **提前聚合框架已存在**:`StatisticsPropagator::TryExecuteAggregates`(`src/optimizer/statistics/operator/propagate_aggregate.cpp:179`)已支持无 GROUP BY 时的 `count_star` / `min` / `max` 提前计算,支持带过滤谓词的三态判定(`FILTER_ALWAYS_TRUE` / `FILTER_ALWAYS_FALSE` / `NO_PRUNING_POSSIBLE`,`src/planner/filter/expression_filter.cpp:167`),并支持"部分预计算 + `set_partitions_to_scan` 下推 + PROJECTION 改写"的混合路径(第 398-456 行)。
3. **缺口**:`TryGetMinMaxColumnInfo`(`propagate_aggregate.cpp:98-122`)只接受 `BOUND_COLUMN_REF` 或安全 cast。`MAX(LENGTH(col))` 的聚合子节点是 `length` 函数表达式,直接 bail,整个优化不生效,查询退化为全量扫描。4. **运行期已有 CPU 层优化**:`LengthPropagateStats`(`src/function/scalar/string/length.cpp:64-94`)在列统计 `CanContainUnicode() == false` 时把 `length` 的执行函数替换为 `StrLenOperator`(不数 codepoint),但列仍被全量读入内存——只省 CPU,不省 IO。

### Problem Statement

对"数百 GB 字符串列求最大/最小长度"的查询,DuckDB 当前必须读完整列 payload。而所需的全部材料(segment 级精确长度统计、精确性判定、提前聚合框架、部分扫描下推)都已存在,缺的只是对"聚合输入是长度函数"这一模式的识别与统计折叠逻辑。该缺口使这类高频运维查询(常用于确定下游 `VARCHAR` 长度)无法利用 row group 统计,造成不必要的磁盘 IO 与内存带宽消耗。

### Constraints

- **结果语义必须与朴素执行完全一致**:`MIN`/`MAX` 忽略 NULL;全 NULL 列或空输入返回 NULL;`CHAR_LENGTH` 是 Unicode 码点数,`LENGTH` 是字节数,二者不可混淆。
- **统计不确定即不可用**:任何 row group 统计不精确(未提交变更、删除版本链、`COUNT_APPROXIMATE`)时,该 row group 的统计值既不能作为精确结果,也不能作为裁剪边界,必须扫描。
- **不修改存储格式与表函数接口**:优化只发生在优化器层,复用现有 `get_partition_stats` / `set_partitions_to_scan` 回调;存量数据库文件无需迁移。
- **对 parquet 暂不支持**:parquet 文件元数据不含字符串长度统计,`ParquetPartitionRowGroup` 提供的统计中无 `max_string_length` / `has_unicode`(`extension/parquet/parquet_reader.cpp:1909-1963`),本优化对其自动降级为不优化。

## Goals

1. `MAX(LENGTH(col))` 在全部 row group 统计精确时零扫描;`MIN(LENGTH(col))` 同理(精确值直接来自 `max_string_length` / `min_string_length`)。
2. `CHAR_LENGTH` 在全 ASCII 列上零扫描;含 Unicode 列上用安全上界(MAX)或安全下界(MIN)裁剪,只扫描可能刷新全局极值的 row group。
3. 统计不精确的 row group 被安全降级为扫描,不再导致整个优化整体失效(即 #23611 在无过滤分支的落地)。
4. 与现有 `count_star` / 普通列 `min` / `max` 提前聚合可混合使用,共用同一部分预计算与表达式改写路径。
5. 结果正确性由测试矩阵锁定:覆盖 Unicode、NULL、空表、删除、未提交写入等边界。
6. 达到性能合入门槛(见 Validation Strategy):百 GB 级字符串列查询的扫描量与 wall time 显著下降,与设计预测一致,且对非目标查询无回归。

## Non-Goals

- **不实现 sizes-only 扫描**:原生表 `StringSegment` 的 offset 数组与 payload 同 block,最小 IO 单位为 block,无法只读长度;该方向需要存储层改动,属独立 proposal。
- **不实现 `SUM(LENGTH)` / `AVG(LENGTH)` 的统计折叠**:`total_string_length` 已存在,统计精确时可直接提前计算,实现成本低,但超出本 proposal 范围,列为后续跟进。
- **暂不支持 parquet 端**:parquet 文件格式无长度元数据,需要写入端自定义扩展(如写入时附加长度 min/max 到自定义 metadata)配合;后续按需评估,不在本 proposal 内。
- **不修改 `LengthPropagateStats` 或任何存储布局**:运行期函数替换保持现状。
- **不做存储层 "fold CHAR_LENGTH into HashAgg" 之外的改动**:DuckDB 聚合为 push-based 流式,不存在 TiFlash 的中间结果物化问题,方案 3 无需专门优化。

## Design

### 1) 现状架构快照

```
SELECT MAX(CHAR_LENGTH(c1)), MAX(CHAR_LENGTH(c2)) FROM t;

优化器(现状):
  AGGREGATE(MAX x 2)
    └─ PROJECTION(char_length(c1), char_length(c2))
         └─ GET(t, 全部分区)          ← 全量扫描,读全部字符串 payload

TryExecuteAggregates 判定流程(现状):
  child 是 length 函数 → TryGetMinMaxColumnInfo 失败 → bail → 无优化
```

### 2) 目标结构

```
优化后(全部统计精确 + 全 ASCII):
  EXPRESSION_GET(常量: max_string_length 的最大值 / min_string_length 的最小值)   ← 零扫描

优化后(部分 row group 不精确 / 含 Unicode):
  PROJECTION(COALESCE(greatest(pre_max, agg_max), pre_max))   -- MAX 路径
  PROJECTION(COALESCE(least(pre_min, agg_min), pre_min))      -- MIN 路径
    └─ AGGREGATE(MIN/MAX x 2)
         └─ GET(t, 仅 scan_partition_indices)        ← set_partitions_to_scan 下推
```

### 3) 聚合输入识别(长度聚合)

在 `TryExecuteAggregates` 中,对 `fun_name == "min" || fun_name == "max"` 的分支,若 `TryGetMinMaxColumnInfo` 失败,进一步尝试识别长度聚合:

- 聚合函数限定为 `min` 与 `max`(`min` 使用 `min_string_length`,裁剪方向与 `max` 对称)。
- 聚合子节点是 `BoundFunctionExpression`,函数名 ∈ `{length, char_length, strlen, octet_length}`(按函数名识别,不受 `LengthPropagateStats` 运行期函数替换影响——替换只改执行回调,不改函数名)。
- 该函数的唯一子节点是 `BOUND_COLUMN_REF`;结果类型为 `BIGINT`。
- 穿越 PROJECTION 时(`propagate_aggregate.cpp:228-245`),在投影表达式中查找同构的 `length(colref)` 子表达式并回填 binding;若投影表达式不是长度函数形态,则该聚合 bail(与现有 `TryGetMinMaxColumnInfo` 的穿越逻辑一致)。

记录每个长度聚合:列 binding、storage index、聚合方向(`min`/`max`)、函数类别(字节语义 `length`/`strlen`/`octet_length` 或字符语义 `char_length`)、结果类型。

### 4) 分区分类与预计算(min/max 对称语义)

对每个 partition(`get_partition_stats` 返回的顺序),取该列的 `StringStats`。以下以 `max` 为例,`min` 为对偶(`min` 相关说明在括号内):

1. **精确性门控**:`MinMaxIsExact(storage_index) && !HasPendingWrites()` 且 `count_type == COUNT_EXACT`。任一不满足 → 该 partition 归入 **SCAN**(强制扫描,统计不可信)。
2. **值提取**(统计精确时):
   - 字节语义(`length`/`strlen`/`octet_length`):精确值 `V = max_string_length`(对 `min`:`V = min_string_length`,若 `HasMinStringLength`)。
   - 字符语义(`char_length`):
     - `!CanContainUnicode(stats)`(全 ASCII)→ 精确值 `V = max_string_length` / `min_string_length`(字符数 == 字节数);
     - 否则(可能含多字节字符):
       - `max` 方向 → 上界 `UB = max_string_length`(UTF-8 每字符 ≥ 1 字节,`字符数 ≤ 字节数`,上界安全);
       - `min` 方向 → 下界 `LB_p = ceil(min_string_length / 4)`(UTF-8 每字符最多 4 字节,`字符数 ≥ ceil(字节数 / 4)`,下界安全;`min_string_length == 0` 时 `LB_p = 0`)。
     - 两种方向都不产生精确值。
   - 无字符串数据(`HasMaxStringLength`/`HasMinStringLength` 均不可得,如全 NULL 分区)→ 不贡献任何值。
3. **汇总与裁剪**:
   - `max` 方向:候选下界 `LB = max(所有精确值 V)`;对每个上界分区,若 `UB <= LB`,该分区**不可能刷新全局最大值** → 跳过(SKIP);否则 → SCAN。
   - `min` 方向:候选上界 `UB_g = min(所有精确值 V)`;对每个下界分区,若 `LB_p >= UB_g`,该分区**不可能刷新全局最小值** → 跳过(SKIP);否则 → SCAN。
   - 不精确分区恒为 SCAN。
4. **出口**:
   - SCAN 为空且存在精确值 → 常量结果(`max` 方向为 `LB`,`min` 方向为 `UB_g`),替换为 `LogicalExpressionGet`(#19906 的零扫描路径)。注:被裁剪分区的真实极值必然不优于候选极值,常量结果精确。
   - SCAN 为空且无任何精确值(所有分区无字符串数据)→ 常量 `NULL`(与 `MIN`/`MAX` 对全 NULL/空输入语义一致)。
   - SCAN 非空 → 部分预计算路径(见下)。
   - partition 列表为空(空表)→ 保持现有 bail 行为。

### 5) 部分预计算路径(SCAN 非空)

复用 `propagate_aggregate.cpp:398-456` 的现有混合路径:

- `get.SetPartitionsToScan(scan_partition_indices)` 经 `set_partitions_to_scan` 回调下推,存储层按 `ParallelCollectionScanState::ShouldScanPartition`(`src/include/duckdb/storage/table/scan_state.hpp:398-400`)只扫这些 row group。
- 聚合上方插入 `LogicalProjection`,合并表达式沿用现有模式(`propagate_aggregate.cpp:423-435` 的 `min`/`max` 分支):
  - `max` 方向:`COALESCE(greatest(pre_max, agg_max), pre_max)`(`pre_max` 为候选下界;`COALESCE` 兜底扫描侧无结果);
  - `min` 方向:`COALESCE(least(pre_min, agg_min), pre_min)`(`pre_min` 为候选上界)。
- `count_star` 的 `pre_count + count_star_from_scan` 合并保持不变,可与长度聚合共存。
- `ColumnBindingReplacer` 修复绑定,与现有实现一致。

**正确性论证**(`max` 方向):被跳过分区(上界分区)的真实最大值 ≤ `UB ≤ LB`;扫描分区的真实最大值由聚合算出为 `S`;全局最大值 = `max(S, 被跳过分区真实最大值) ≤ max(S, LB)`,且 `LB` 本身来自某个分区的精确值(≤ 全局最大值),故 `max(S, LB)` 精确等于全局最大值。
**正确性论证**(`min` 方向,对偶):被跳过分区(下界分区)的真实最小值 ≥ `LB_p ≥ UB_g`;扫描分区的真实最小值由聚合算出为 `S'`;全局最小值 = `min(S', 被跳过分区真实最小值) ≥ min(S', UB_g)`,且 `UB_g` 本身来自某个分区的精确值(≥ 全局最小值),故 `min(S', UB_g)` 精确等于全局最小值。

### 6) 失败/降级路径

- 任一聚合无法识别(如 `MAX(LENGTH(col) + 1)`、`LENGTH` 参数非列引用)→ 该聚合 bail,整棵聚合树沿用现有行为(与现状一致,不冒险部分优化)。
- 统计不精确 → 强制 SCAN(见第 4 节),结果仍正确,只是优化退化为部分/无裁剪。
- `set_partitions_to_scan` 回调缺失 → bail(现有第 401-404 行的保护)。
- 字符串统计类型异常(`NumericStats` 误判等)→ 按"无长度统计"处理,不猜测。
- `min` 方向下界 `ceil(min_string_length / 4)` 为空字符串(长度 0)分区时为 0,与全局最小可能一致,该分区不被裁剪、照常参与判定,语义安全。

### 7) 可观测性

- 计划形态可观测:`EXPLAIN` 中零扫描路径显示 `EXPRESSION_GET`,部分路径显示 `GET(partitions_to_scan)` 与 PROJECTION 改写。
- 聚合输出统计可观测:`MIN/MAX(LENGTH(col))` 的输出统计经 `LengthPropagateStats` 已有 `[min_length, max_length]` 区间,本优化不改变该传播。

## Incremental Plan

### Phase A: 框架重构 + 不精确分区降级(#23611 落地)

- 重构 `TryExecuteAggregates`:将"从分区统计提取聚合候选值"抽成可扩展路径,使无过滤分支遇到统计不精确分区时不再整体 bail,而是归入 SCAN(与现有过滤分支的 `NO_PRUNING_POSSIBLE` 处理对齐)。
- 行为等价性:现有 `count_star`/`min`/`max`(列引用形态)结果不变。
- 验收:现有优化器测试全绿;新增"delete 后 / 未提交写入后结果仍正确"测试。

### Phase B: 字节语义长度聚合(`MIN/MAX(LENGTH/strlen/octet_length)`)

- 实现第 3 节识别逻辑与第 4 节字节语义分支(`max_string_length` / `min_string_length` 直接作为精确值)。
- 全精确 → 零扫描;混合 → 部分预计算。
- 验收:TPC-H 风格字符串列的 `MAX(LENGTH(col))` / `MIN(LENGTH(col))` EXPLAIN 显示零扫描或部分扫描,结果与关闭优化一致。

### Phase C: 字符语义(`MIN/MAX(CHAR_LENGTH)`)+ 边界裁剪

- 实现 `CanContainUnicode` 判定:`max` 方向 `UB <= LB` 裁剪,`min` 方向 `LB_p >= UB_g` 裁剪(含 `ceil(min_string_length / 4)` 下界);含 Unicode 分区的降级扫描。
- 验收:多字节 UTF-8 数据(含 4 字节 emoji、截断边界、空字符串)下结果与朴素执行一致。

### Phase D: 测试矩阵与性能基准(合入门槛)

- 补齐边界测试(全 NULL、空表、单值列、空字符串、溢出大字符串、混合 `count_star`、与 `set_scan_order`/分区执行优化器组合)。
- 执行性能基准并对照合入门槛(见 Validation Strategy):构造长度分布集中(利于裁剪)与分散(不利裁剪)两组百 GB 级字符串列数据,对比优化前后扫描行数、IO 与 wall time;验证裁剪效果与设计预测一致,且对非目标查询(如 `SELECT LENGTH(col) FROM t` 全投影)无回归。

## Validation Strategy

- **正确性(核心)**:对每个测试场景,用 `PRAGMA disable_optimizer` 或等价手段关闭优化,对比结果。重点覆盖:多字节 UTF-8(字符数 ≠ 字节数,`MIN` 方向验证 `ceil(字节数 / 4)` 下界不产生错误裁剪)、全 NULL 列(结果 NULL)、空表、`DELETE` 后统计不精确、未提交事务中查询。
- **计划形态**:`EXPLAIN` 断言零扫描路径(`EXPRESSION_GET`)与部分路径(scan partition 数量)。
- **边界**:空字符串(长度 0,`min` 方向下界为 0)、单值列(min == max)、超过 block 限制的大字符串(overflow block)、`has_max_string_length == false` 的统计缺失场景。
- **回归**:`make allunit` 全量 sqllogictest;新增测试置于 `test/sql/optimizer/`。
- **性能基准(合入门槛)**:
  - 数据:两组百 GB 级字符串列(长度分布集中,利于裁剪;长度分布分散,不利裁剪),各含一个含 Unicode 列。
  - 指标:优化前后对比实际扫描 row group 数、TableScan 出站字节数与 wall time。
  - 门槛:(a) 统计全部精确的数据上 `MIN/MAX(LENGTH)` 扫描 row group 数为 0(字节语义不依赖 ASCII);全 ASCII 数据上 `MIN/MAX(CHAR_LENGTH)` 同理;(b) 集中分布数据上扫描量下降与"候选极值裁剪"的理论预测一致(误差来自统计分段粒度);(c) 分散分布数据上优化不劣于现状;(d) 非目标查询(全列投影、其他聚合)wall time 无回归(±5% 内)。
  - 门槛不满足时,该功能不得合入,需先回退 Phase 或调整设计。

## Risks and Mitigations

1. **统计精确性判定失误导致错误结果** — 严格门控:任何不确定(未提交变更、删除、近似计数)一律归入 SCAN;`MinMaxIsExact`/`HasPendingWrites`/`COUNT_EXACT` 三条件缺一不可;测试覆盖 delete 与未提交写入场景。
2. **字节/字符语义混淆**(`CHAR_LENGTH` 用字节数当字符数) — 只有 `CanContainUnicode == false` 时才产生精确字符数;含 Unicode 分区仅使用字节数作安全上界(`字符数 ≤ 字节数`,MAX)或 `ceil(字节数 / 4)` 作安全下界(`字符数 ≥ ceil(字节数 / 4)`,MIN),绝不直接当结果;测试含多字节字符。
3. **MIN 方向下界过松导致裁剪不足**(`ceil(bytes/4)` 对纯 ASCII 数据会低估字符数下界,但 ASCII 数据 `CanContainUnicode == false` 走精确分支,不会误入该路径)——误用场景由第 2 条门控兜底;对确含多字节字符的分区,过松下界只减少裁剪收益、不影响正确性。
4. **全 NULL / 无字符串数据分区的 MIN/MAX 语义**(结果应为 NULL 而非 0) — 按第 4 节"不贡献值 + 无精确值时输出 NULL"处理;测试锁定。
5. **与 `LengthPropagateStats` 运行期替换的交互** — 识别按函数名进行,替换不改变函数名;验证替换后路径仍正确。
6. **与下游优化器 pass 的交互**(`MultiStageAggregateRewriter`、`PartitionedExecution`) — 聚合被替换为常量后这些 pass 无可重写输入,安全;Phase D 增加组合测试。
7. **partition index 语义漂移** — `set_partitions_to_scan` 的 index 与 `get_partition_stats` 顺序一致;`set_scan_order` 只改变扫描顺序不改变 index 映射;Phase D 验证组合场景。
8. **性能基准不达标(合入门槛未满足)** — 按 Validation Strategy 门槛执行;不达标时该功能不得合入,回退 Phase 或调整设计(如仅保留零扫描路径、裁剪逻辑后置)。

## Open Questions

1. **性能基准的量化指标确认** — 推荐按 Validation Strategy 中的门槛执行:零扫描断言、集中分布数据的扫描量理论一致性、分散分布不劣于现状、非目标查询 ±5% 无回归。若实现时发现百 GB 级数据构造成本过高,可降级为 TPC-H SF100 的字符串列 + 理论一致性断言,需在合入评审时确认。
2. **`SUM(LENGTH)` / `AVG(LENGTH)` 的跟进安排** — 已决策不纳入本 proposal;`total_string_length` 统计已存在,建议在 Phase D 后作为独立小 proposal 跟进,由同一框架自然扩展。

## Alternatives Considered

- **运行时 running max(TiFlash 方案 1 的实现形态)**:扫描过程中维护 running max 跳过后续 pack。DuckDB 优化器在计划期即可通过 `get_partition_stats` 拿到全部 row group 统计,静态全局判定更优(全精确时零扫描、无需"先读几个 pack 建立下界"),故不采用运行形态。
- **sizes-only 扫描(TiFlash 方案 2)**:省 IO 效果最好,但原生表 offset 数组与 payload 同 block,需要存储层布局改动;与本 proposal 正交,独立推进(见 Non-Goals)。
- **持久化长度生成列(TiFlash 方案 4)**:`max_string_length` 已是持久化的长度元数据,方案 1 免费复用;生成列需用户改 SQL 且优化器不会自动改写,不采用。
- **仅依赖运行期函数替换(现状)**:只省 CPU 不省 IO,不解决磁盘带宽瓶颈,本 proposal 是其自然延伸而非替代。
