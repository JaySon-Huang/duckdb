# Proposal: 基于 PR #23611 框架实现 MIN/MAX(LENGTH/CHAR_LENGTH) 提前聚合与裁剪

**日期**: 2026-08-19
**目的**: 在 PR #23611(Allow partial aggregate precompute with inexact row group stats,分支 `partial-precompute-inexact-stats`,commit `324a2133ec`)重构后的 `TryExecuteAggregates` 框架上,实现 `MIN/MAX(LENGTH/CHAR_LENGTH/strlen/octet_length)` 的统计提前计算与 row group 裁剪,将 TiFlash issue #11049 方案 1(max_len skip)在 DuckDB 上落地。

## Summary

开发基线为 PR #23611 的分支 `partial-precompute-inexact-stats`(commit `324a2133ec`),**直接在该分支上继续实现**,不先 rebase 到主干。该分支已将 `TryExecuteAggregates` 重构为"逐分区分类 + 通用改写"框架:任何分区统计不精确时该分区单独降级为扫描,不再导致整个优化整体失效。本 proposal 在该框架上扩展 length 类聚合的支持:

- 在 `TryGetPartitionAggregateInfo` 中识别 `MIN/MAX(LENGTH/CHAR_LENGTH/strlen/octet_length(col))`(聚合输入是长度函数而非列引用);
- 在 `ClassifyPartition` 中新增 length 值的提取(精确值 / 安全边界值),并引入 `#23611` 三态分类之外的"有界值"中间态;
- 主循环按 `min`/`max` 方向对称裁剪:候选极值 `LB = max(精确值)`(MAX)或 `UB_g = min(精确值)`(MIN),无法刷新全局极值的分区跳过,其余分区经现有 `set_partitions_to_scan` 只扫描需要的部分;
- `ReplaceWithPartialPrecompute` / `ReplaceWithFullPrecompute` 无需修改,直接复用。

目标行为:统计全部精确时 `MIN/MAX(LENGTH)` 零扫描产出常量;`CHAR_LENGTH` 在全 ASCII 分区上同样精确,含 Unicode 分区用字节长度安全边界(`MAX`:上界 `字符数 ≤ 字节数`;`MIN`:下界 `字符数 ≥ ceil(字节数/4)`)裁剪。只依赖优化器与现有统计接口,不修改存储格式,对原生表生效;parquet 暂不支持。

## Context

### Current State

**问题查询**(TiFlash issue #11049):`SELECT MAX(CHAR_LENGTH(c1)), ..., MAX(CHAR_LENGTH(c16)) FROM t` 无过滤全表扫描,瓶颈是读取字符串列 payload,而非聚合计算。

**PR #23611 重构后的框架**(分支 `partial-precompute-inexact-stats`,`src/optimizer/statistics/operator/propagate_aggregate.cpp`):

```
TryExecuteAggregates(379-443)
 ├─ TryGetPartitionAggregateInfo(162)    收集 requirements(per-partition 判定所需的全部信息)
 │    · filters: map<StorageIndex, TableFilter>
 │    · min_max_columns + comparators: 普通 min/max 聚合的列与比较器
 │    · needs_exact_count: 是否存在 count(*)
 ├─ get_partition_stats 获取全部分区统计
 ├─ 主循环(394-419):逐分区 ClassifyPartition →
 │    PRECOMPUTE: 值折叠进预计算常量(precompute_count++, total_count += count)
 │    SCAN:       partition_idx 进 scan_partition_indices
 │    PRUNE:      丢弃(filter 恒假)
 ├─ precompute_count == 0 → return(唯一整体 bail 条件)
 └─ ReplaceWithPartialPrecompute(311) / ReplaceWithFullPrecompute(360)
      · 按 aggr.expressions 顺序组装常量,与聚合类型无关
      · 合并表达式:count_star → pre + agg;min/max → COALESCE(least/greatest(pre, agg), pre)
```

该框架的关键语义:`ClassifyPartition`(256)中 `TryGetValueFromStats`(88)失败——统计不精确、无 min/max、有 pending writes——一律返回 `SCAN`,精确分区照常预计算。**不精确处理已内建**,且 `test/optimizer/partial_aggregate_precomputation.test` 已覆盖 UPDATE/DELETE/未 checkpoint 场景。

**DuckDB 字符串统计**(与 #23611 无关,已存在):`StringStatsData` 含 `max_string_length` / `min_string_length` / `total_string_length` / `has_unicode`(`src/include/duckdb/storage/statistics/string_stats.hpp`),segment 级精确、随 checkpoint 持久化、`StringStats::Merge` 正确合并;row group 级经 `PartitionRowGroup::GetColumnStatistics` 获取,精确性由 `MinMaxIsExact()` / `HasPendingWrites()` 判定(`src/storage/table/row_group.cpp:1961-2008`)。

**开发基线与当前主干的差异**(合入上游时需处理,开发期不受影响):分支 `partial-precompute-inexact-stats` 基于 2026-07 主干,其 `TryGetPartitionAggregateInfo` 的 min/max 分支只接受 `BOUND_COLUMN_REF` 聚合输入(列 binding 列表);当前主干已演进为 `MinMaxColumnInfo`(带 `input_type`/`result_type`、支持安全 cast 与 projection 穿越,`propagate_aggregate.cpp:37-41`、`98-122`)。直接基于分支开发意味着开发期不获得主干的该演进;待 #23611 合入主干(或本 proposal 提交时),再做 rebase 适配(见 Risks 第 4 条)。

### Problem Statement

对"数百 GB 字符串列求最大/最小长度"的查询,即便 #23611 合入,`TryGetPartitionAggregateInfo` 仍只接受列引用形态的 min/max;`MIN/MAX(LENGTH(col))` 的聚合输入是 `length` 函数,识别失败 → 整体 bail → 全量扫描。所需的全部材料(长度统计、逐分区降级框架、通用改写路径)在 #23611 合入后齐备,缺的只是 length 聚合的识别、值提取与边界裁剪逻辑。

### Constraints

- **结果语义与朴素执行一致**:`MIN`/`MAX` 忽略 NULL;全 NULL 列或空输入返回 NULL;`CHAR_LENGTH` 是码点数,`LENGTH` 是字节数。
- **统计不确定即不可用**:不精确分区(未提交变更、删除、`COUNT_APPROXIMATE`)的统计值既不能当精确结果,也不能当裁剪边界,必须扫描——该语义由 #23611 的 `ClassifyPartition` 内建,length 扩展必须保持一致。
- **不修改存储格式与表函数接口**:只改优化器,复用 `get_partition_stats` / `set_partitions_to_scan`。
- **parquet 暂不支持**:文件元数据无字符串长度统计,自动降级为不优化。

## Goals

1. `MIN/MAX(LENGTH(col))` 在统计全部精确时零扫描(常量结果);不精确分区被 #23611 框架自动降级为扫描,结果仍正确。
2. `CHAR_LENGTH` 在全 ASCII 分区上零扫描;含 Unicode 分区用安全边界(`MAX` 上界 / `MIN` 下界)裁剪,只扫描可能刷新全局极值的分区。
3. 与 `count_star`、普通列 `min`/`max` 聚合混合时共用同一分类、合并与改写路径。
4. 全部扩展点落在 `#23611` 的四个组件内;`ReplaceWithPartialPrecompute` / `ReplaceWithFullPrecompute` 零改动。
5. 达到性能合入门槛(见 Validation Strategy)。

## Non-Goals

- **不实现 sizes-only 扫描**(TiFlash 方案 2):需存储层布局改动,独立 proposal。
- **不实现 `SUM(LENGTH)` / `AVG(LENGTH)` 的统计折叠**:`total_string_length` 已存在,列为后续跟进。
- **不修改 `#23611` 已合入语义**:如 `PRUNE` 的行为、`precompute_count == 0` 的整体放弃条件等保持原样;如实现中发现必须改动,需在该 PR 中说明并单独评审。
- **暂不支持 parquet 端**。
- **不修改 `LengthPropagateStats` 运行期函数替换**。

## Design

### 1) 目标架构(在 #23611 组件上的扩展)

```
TryGetPartitionAggregateInfo(扩展)
 ├─ 现有:min_max_bindings(仅 BOUND_COLUMN_REF)/ comparators / needs_exact_count / filters
 └─ 新增:length_columns — 每个 length 聚合:
      · binding(storage index)、聚合方向(min/max)、函数类别(字节/字符语义)、结果类型(BIGINT)

ClassifyPartition(扩展)
 └─ 现有:filter 三态、count、普通 min/max 提取(TryGetValueFromStats)
    └─ 新增:length 值提取,结果为"值 + 类别":
         · EXACT(精确值):字节语义;或字符语义且 !CanContainUnicode
         · BOUNDED(边界值):字符语义且可能含 Unicode → MAX 方向记上界 UB,
           MIN 方向记下界 LB_p = ceil(min_string_length / 4)
         · NO_VALUE:无字符串数据(全 NULL 分区)→ 不贡献
         · 统计不精确 / 无长度统计 → 沿用 #23611 规则返回 SCAN

主循环(扩展为两遍)
 ├─ 第一遍:收集各分区 length 候选值(EXACT / BOUNDED),计算
 │    · MAX 方向:LB = max(所有 EXACT 值)
 │    · MIN 方向:UB_g = min(所有 EXACT 值)
 ├─ 第二遍:对 BOUNDED 分区做裁剪决策
 │    · MAX:UB <= LB → 跳过(SKIP,不扫描、不进预计算)
 │    · MIN:LB_p >= UB_g → 跳过
 │    · 否则 → SCAN
 └─ 折叠:EXACT 分区进预计算常量;决策与 #23611 的 PRECOMPUTE/SCAN/PRUNE 对齐
    (SKIP 可复用 PRUNE 语义,不产生任何扫描与常量贡献)

ReplaceWithPartialPrecompute / ReplaceWithFullPrecompute(零改动)
 └─ 按 aggr.expressions 索引组装常量;MAX 合并用 greatest,MIN 用 least
```

### 2) 识别(扩展 `TryGetPartitionAggregateInfo`)

在 min/max 分支内,当聚合子节点不是 `BOUND_COLUMN_REF`(分支上的现有检查)时,进一步匹配 length 聚合(基于分支结构,不依赖主干的 `MinMaxColumnInfo`):

- 聚合方向 `min` 或 `max`;
- 聚合子节点是 `BoundFunctionExpression`,函数名 ∈ `{strlen, octet_length}`(Phase B,字节语义;`length`/`char_length` 为码点语义,Phase C 再支持,按函数名识别,不受 `LengthPropagateStats` 运行期函数替换影响);
- 该函数唯一子节点是 `BOUND_COLUMN_REF`,结果类型 `BIGINT`;length 聚合所需的类型信息(`BIGINT` 结果、VARCHAR/BLOB 输入)由本 proposal 自己记录,不依赖主干结构;
- 穿越 PROJECTION 时在投影表达式中查找同构 `length(colref)` 子表达式回填 binding;失败则该聚合 bail(`TryGetPartitionAggregateInfo` 返回 false,与现有逻辑一致);
- 记录到 `requirements.length_columns`(新增结构):storage index、方向、字节/字符语义类别。

### 3) 值提取与分类(扩展 `ClassifyPartition`)

对 `requirements.length_columns` 中的每一列(统计精确时才进入本段;不精确由现有 `MinMaxIsExact` / `HasPendingWrites` 门控先行判定):

| 情形 | 结果 |
| --- | --- |
| 字节语义(`strlen`/`octet_length`),有 `HasMaxStringLength` / `MinStringLength` 有效 | `EXACT`:`V = max_string_length`(MAX 方向)或 `V = min_string_length`(MIN 方向) |
| 字符语义,`!CanContainUnicode(stats)`(全 ASCII) | `EXACT`:同上(字符数 == 字节数) |
| 字符语义,可能含 Unicode | `BOUNDED`:MAX 方向 `UB = max_string_length`(`字符数 ≤ 字节数`);MIN 方向 `LB_p = ceil(min_string_length / 4)`(`字符数 ≥ ceil(字节数/4)`,UTF-8 每字符最多 4 字节;`min_string_length == 0` 时 `LB_p = 0`) |
| 无字符串数据(`HasMaxStringLength`/`HasMinStringLength` 均不可得,如全 NULL 分区) | `NO_VALUE`:不贡献值、不强制扫描 |
| 统计不精确 / 长度统计缺失但列有数据 | 沿用 #23611:返回 `SCAN` |

`AggregateValues`(156)扩展 length 候选值数组;`PartitionAction` 保持三态,`BOUNDED`/`NO_VALUE`/`EXACT` 是**值类别**而非动作,动作由主循环决策。

### 4) 主循环裁剪(min/max 对称)

- 第一遍收集后:
  - MAX 方向:候选下界 `LB = max(EXACT 值)`;`LB` 存在且 `UB <= LB` 的 BOUNDED 分区**不可能刷新全局最大值** → SKIP;否则 SCAN。
  - MIN 方向:候选上界 `UB_g = min(EXACT 值)`;`UB_g` 存在且 `LB_p >= UB_g` 的 BOUNDED 分区 → SKIP;否则 SCAN。
- 无 EXACT 值时(全 NO_VALUE,即全 NULL/空数据)→ 不裁剪,所有分区照常判定;若最终 SCAN 为空且无 EXACT 值 → 常量 `NULL`(`MIN`/`MAX` 对全 NULL/空输入语义)。
- 出口与 #23611 一致:`precompute_count == 0` → return;SCAN 非空 → `ReplaceWithPartialPrecompute`;SCAN 为空 → `ReplaceWithFullPrecompute`。
- 常量组装:`MAX` 预计算值 `pre = LB`,`MIN` 预计算值 `pre = UB_g`;合并表达式 `COALESCE(greatest(pre, agg), pre)` / `COALESCE(least(pre, agg), pre)` 由 `ReplaceWithPartialPrecompute` 按函数名自动选择,零改动。

**正确性论证**(MAX 方向):被跳过分区真实最大值 ≤ `UB ≤ LB`;扫描分区真实最大值由聚合算出为 `S`;全局最大值 = `max(S, 被跳过区真实最大值) ≤ max(S, LB)`,且 `LB` 来自某分区精确值(≤ 全局最大值),故 `max(S, LB)` 精确等于全局最大值。MIN 方向对偶(用 `UB_g` 与 `least`)。

### 5) 失败/降级路径

- 任一聚合无法识别(如 `MAX(LENGTH(col) + 1)`、函数参数非列引用)→ `TryGetPartitionAggregateInfo` 返回 false,沿用现状(整树不优化)。
- 统计不精确 → `ClassifyPartition` 返回 SCAN(#23611 语义,零新增代码)。
- `set_partitions_to_scan` 回调缺失且 SCAN 非空 → 现有保护路径 bail。
- 全 NULL 分区 `NO_VALUE` 的次优退化:若实现时发现"无字符串数据"与"长度统计缺失"难以区分,可保守地降级为 SCAN(正确性不变,仅全 NULL 列多扫)。此为实现取舍,测试锁定行为。

### 6) 可观测性

- `EXPLAIN` 零扫描路径显示 `EXPRESSION_GET`;部分路径显示 `GET(partitions_to_scan)` 与 PROJECTION 改写。
- 复用 #23611 测试中的 `EXPLAIN ANALYZE` 断言(如 `Row Groups Scanned: 1 / 4`)验证裁剪效果。

## Incremental Plan

### Phase A: 基线确认与开发环境准备

- 开发基线:分支 `partial-precompute-inexact-stats`(commit `324a2133ec`),直接在此分支上新建特性分支开发;不 rebase 到当前主干。
- 基线验收:分支上现有测试全绿,重点确认 `test/optimizer/partial_aggregate_precomputation.test` 中无 filter 分支的 UPDATE/DELETE 场景行为(整体预期:`UPDATE` 后 `count(*)` 仍全预计算、`min/max` 部分预计算、`DELETE` 后只扫受影响 row group)。
- 与 #23611 上游同步:跟踪作者 Damon07 的后续更新(updatedAt 2026-08-08,仍在维护);#23611 分支更新或合入主干时,将本特性分支同步到其最新状态。

### Phase B: 字节语义 length 聚合(`MIN/MAX(strlen/octet_length)`)

- 实现 Design 2) 识别与 3) 值提取的字节语义分支;主循环两遍裁剪对字节语义退化为"全精确 → 零扫描,有不精确 → 部分预计算"。
- 注意:`length(varchar)` 在 DuckDB 中是码点数(仅全 ASCII 数据上等于字节数),不属字节语义——`length` / `char_length` 归入 Phase C(需 `CanContainUnicode` 判定);本 Phase 只覆盖 `strlen`(VARCHAR)与 `octet_length`(BLOB/VARCHAR),其值直接等于 `max_string_length` / `min_string_length`。
- **实现形态**:识别与投影穿越分离——先收集所有 min/max 候选(列引用或 length 函数),穿越投影后按最底层表达式分类。该形态使**公共子表达式提升(CSE)与视图列场景**同样可优化(聚合输入是"指向 length 函数结果的计算列"时,沿投影链下推仍能匹配到 `strlen`/`octet_length`)。
- **存储格式依赖(实现发现)**:`min_string_length` / `total_string_length` 仅在 V2_0_0+ 存储格式中持久化;默认 `storage_compatibility_version`(v0.10.2 兼容格式)下 checkpoint 后二者丢失,reopen 后 `MIN(strlen)` 退化为扫描(结果正确,仅次优)。`max_string_length` 在两种格式中都持久化,`MAX(strlen)` 不受影响。测试需在 ATTACH 前设置 `storage_compatibility_version='latest'`。
- **HasChanges 行为(实现发现)**:row group 移入 collection(`MoveToCollection`)会永久置 `has_changes = true` 且无清除路径,故同进程内新建表(含 CREATE TABLE AS + CHECKPOINT)的 row group `MinMaxIsExact` 恒为 false,优化不生效;从磁盘重新加载后 `has_changes = false`,优化生效。这是 DuckDB 既有行为(主干 #21831 的测试同样依赖 reopen 流程),测试需沿用 reopen 模式。
- 验收:`MAX(strlen(col))` / `MIN(strlen(col))` EXPLAIN 显示零扫描或部分扫描,结果与关闭优化一致;`MAX(length(col))`(码点语义)在本 Phase 不触发优化,仍走正常聚合;与 `count_star` 混合场景正确;CSE(同一列多个 length 聚合)与视图场景零扫描。

### Phase C: 字符语义(`MIN/MAX(CHAR_LENGTH/LENGTH)`)+ 边界裁剪

- 实现 `CanContainUnicode` 判定、`BOUNDED` 值类别与两遍裁剪决策(MAX 上界 / MIN 下界)。
- 覆盖 `char_length` 与 `length`(DuckDB 中二者均为码点语义;`char_length` 是 `length` 的别名但绑定后保留独立函数名,识别需同时处理两个名字)。`length` 还有 BIT/LIST 变体,识别时校验输入类型为 VARCHAR,排除 `length(bit)`/`length(list)`。
- **实现形态**:`ClassifyPartition` 对字符语义且 `CanContainUnicode == true` 的分区提取**边界值**(MAX 方向:`UB = max_string_length`,`字符数 ≤ 字节数`;MIN 方向:`LB_p = ceil(min_string_length / 4)`,UTF-8 每字符最多 4 字节),否则提取精确值。主循环改为**两遍**:第一遍折叠精确值得到候选极值(`LB = max(精确值)` / `UB_g = min(精确值)`),第二遍对含边界值的分区判定——**仅当查询是纯 length 聚合**(无普通 min/max、无 count)时,边界值无法刷新全局极值的分区可整体跳过(SKIP);混合查询下边界值分区一律扫描(分区需要为其他聚合贡献精确值)。
- 验收:多字节 UTF-8 数据(4 字节 emoji、截断边界、空字符串)下结果与朴素执行一致;边界裁剪只减少扫描量、不改变结果。
- **测试**:`test/optimizer/max_len_skip_char.test`(46 断言),覆盖全 ASCII 零扫描、Unicode 分区上界可/不可裁剪、MIN 下界 `ceil(bytes/4)` 裁剪、混合查询、BIT 列不误识别、空字符串。

### Phase D: 测试矩阵扩展(性能基准待后续执行)

- **已完成**:在 `partial_aggregate_precomputation.test` 基础上扩展 length 断言(该文件已覆盖 UPDATE/DELETE/未 checkpoint 不精确场景,length 测试直接复用其数据构造与 reopen 流程):
  - `MAX/MIN(strlen)`、`MAX(char_length)` 全精确零扫描;与 `count_star`/普通 `min` 混合全预计算;
  - UPDATE 后部分预计算(`COALESCE(greatest/least)` + `Row Groups Scanned: 1 / 4`),count 与 length 共享扫描集合并合并回写;
  - DELETE 后部分预计算;经 checkpoint 后删除标记仍使 row group 统计不精确,部分预计算保持正确;
  - 单值列(`min == max`)、溢出大字符串(overflow block,`max_string_length` 正确反映 300000 字节)、空表(NULL 结果)、GROUP BY 不误伤(`HASH_GROUP_BY`);
  - 与 `set_scan_order`(ORDER BY + LIMIT 的 Top N)在同一表上共存互不干扰;
  - 测试文件需 `storage_compatibility_version='latest'`(`min_string_length` 持久化)与 `profiling_renderer_settings` upper casing(`TOP_N` 断言大小写)。
- **待办(另行执行)**:性能基准与合入门槛(百 GB 级字符串列,集中/分散分布两组,对比扫描 row group 数、出站字节与 wall time;零扫描断言、理论一致性、无回归 ±5%)。

## Validation Strategy

- **正确性(核心)**:每个场景用 `PRAGMA disable_optimizer` 对比结果。重点:多字节 UTF-8(`MIN` 方向验证 `ceil(字节数/4)` 下界不产生错误裁剪)、全 NULL 列(NULL 结果)、空表、`DELETE` 后统计不精确、未提交事务中查询、`UPDATE` 后 count 仍可全预计算(复用 #23611 测试语义)。
- **计划形态**:`EXPLAIN` 断言零扫描(`EXPRESSION_GET`)与部分路径;`EXPLAIN ANALYZE` 断言扫描 row group 数(参考 #23611 测试的 `Row Groups Scanned: 1 / 4` 模式)。
- **边界**:空字符串(长度 0)、单值列(min == max)、overflow 大字符串、`has_max_string_length == false` 统计缺失。
- **回归**:`make allunit`;新增/扩展测试置于 `test/optimizer/`。
- **性能基准(合入门槛)**:
  - 数据:两组百 GB 级字符串列(长度集中分布 / 分散分布),各含一个 Unicode 列。
  - 指标:优化前后扫描 row group 数、TableScan 出站字节数、wall time。
  - 门槛:(a) 统计全部精确时 `MIN/MAX(LENGTH)` 扫描 row group 数为 0(字节语义不依赖 ASCII),全 ASCII 数据上 `MIN/MAX(CHAR_LENGTH)` 同理;(b) 集中分布数据扫描量下降与候选极值裁剪理论预测一致;(c) 分散分布数据不劣于现状;(d) 非目标查询 wall time 无回归(±5%)。
  - 门槛不满足不得合入,回退 Phase 或调整设计。

## Risks and Mitigations

1. **#23611 未合并 / 分支基于旧主干** — 直接基于分支开发,开发期不获得主干后续演进(如 `MinMaxColumnInfo`/cast 支持);最终向上游提交时需将特性分支 rebase 到最新主干并同时推动 #23611 合入(作者 Damon07 仍在活跃维护,updatedAt 2026-08-08);开发期间跟踪 #23611 分支更新并及时同步。
2. **`BOUNDED` 值类别的语义边界**(`ceil(bytes/4)` 下界对纯 ASCII 数据低估,但 ASCII 数据走 `EXACT` 分支,不会误入)— 误用由 `CanContainUnicode` 门控兜底;对确含多字节字符的分区,过松边界只减少裁剪收益、不影响正确性。
3. **全 NULL 分区处理** — `NO_VALUE` 与"统计缺失"的区分若不可靠,保守降级为 SCAN(正确性不变);测试锁定。
4. **未来与主干 `MinMaxColumnInfo`(cast 支持)的适配** — 分支上的普通 min/max 不支持 cast 形态(`MAX(CAST(x AS ...))`),本 proposal 不引入该能力;待 #23611 合入主干后 rebase 时,将 length 识别适配到主干的 `MinMaxColumnInfo` 结构(作为其失败后的回退分支),并验证 cast 形态普通 min/max 行为不回归。
5. **与 `LengthPropagateStats` 运行期替换交互** — 识别按函数名,替换不改变函数名。
6. **与下游 pass 交互**(`MultiStageAggregateRewriter`、`PartitionedExecution`) — 聚合替换为常量后无可重写输入,安全;Phase D 组合测试。
7. **partition index 语义漂移** — `set_partitions_to_scan` index 与 `get_partition_stats` 顺序一致;`set_scan_order` 只改顺序不改映射;Phase D 验证。
8. **性能基准不达标** — 按 Validation 门槛执行;不达标回退 Phase 或调整设计。
9. **`min_string_length` 持久化依赖存储格式** — 默认 v0.10.2 兼容格式下 reopen 后 `MIN(strlen)` 统计缺失,优化退化为扫描;该限制源于存储格式(非本优化引入),测试以 `storage_compatibility_version='latest'` 覆盖;若需默认格式下也生效,需推动默认存储版本升级或接受次优行为。

## Open Questions

1. **性能基准量化指标确认** — 推荐按 Validation Strategy 门槛执行(零扫描断言、集中分布理论一致性、分散分布不劣于现状、±5% 无回归);若百 GB 级构造成本过高,降级为 TPC-H SF100 字符串列 + 理论一致性断言,合入评审时确认。
2. **#23611 的上游推进方式** — 本 proposal 直接基于其分支开发,最终向上游提交时需将特性分支(含 #23611 改动)一并提交,或等 #23611 先合入主干后再 rebase;推荐在 GitHub 上对 #23611 发起 review 并说明本 proposal 的依赖关系,由作者或社区决定合并顺序。
3. **`SUM(LENGTH)` / `AVG(LENGTH)` 跟进安排** — 已决策不纳入;`total_string_length` 已存在,建议 Phase D 后独立小 proposal。

## Alternatives Considered

- **独立实现不依赖 #23611(原 proposal `max_len_skip.zh.md`)**:Phase A 自行实现"无 filter 分支不精确降级"与框架拆分,与 #23611 工作重叠,且上游合入后需迁移。本版本选择直接以 #23611 分支为开发基线,实现面更小、测试基础更厚,省去 rebase 前置工作。
- **运行时 running max(TiFlash 方案 1 形态)**:优化器计划期即可拿到全部 row group 统计,静态全局判定更优(全精确零扫描),不采用。
- **sizes-only 扫描 / 持久化长度生成列**:见 Non-Goals,与本 proposal 正交。
