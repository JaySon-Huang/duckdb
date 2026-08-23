# Proposal: MIN/MAX(strlen/octet_length) 提前聚合(两阶段推进·阶段一)

**日期**: 2026-08-23
**目的**: 将 `max-len-stats` 分支的 length 聚合预计算拆分为两个独立可评审、可合入 main 的 PR;本文档定义拆分方案,并给出阶段一(字节长度)的完整设计。
**关系**: 取代 `proposals/min_max_length_precompute.zh.md` 中 Incremental Plan 的单 PR 视角(该文档保留为全量分支实现的设计说明与上游调研来源);阶段二(char_length 与 Unicode 边界)仅在本文"Phase 2 展望"中留草案与 TODO,合入前另立文档细化。

## Summary

把分支 `max-len-stats` 的改动按"值的确定性"拆成两个阶段推进主线:

- **阶段一(本提案,单 PR)**:`MIN/MAX(strlen(col))`、`MIN/MAX(octet_length(col))` 的统计预计算。字节长度与 `StringStats` 的 `max_string_length`/`min_string_length` 语义完全一致,**每个分区提取的都是精确值**,不需要边界推导与裁剪机制——统计全部精确时聚合直接替换为常量(零扫描),任一分区统计不可用则整体 bail(与 main 现状一致)。
- **阶段二(后续 PR)**:`length`/`char_length`(码点语义)的识别,以及可能含 Unicode 分区的**安全边界**(MAX 上界 = 字节长度,MIN 下界 = `ceil(字节长度/4)`)与"纯 length 查询跳过不可能刷新极值的分区"。该阶段引入正确性论证负担(边界方向、混合查询策略),独立成 PR 评审。

拆分依据:阶段一是纯精确值的折叠,正确性论证与既有 `min`/`max` 预计算同构,评审面小;阶段二的核心价值(Unicode 裁剪)依赖边界推理,风险与测试矩阵独立。两者共享识别框架与装配重构,阶段一落地后阶段二对 `propagate_aggregate.cpp` 的增量约 ±150 行(分支 commit `660949920a` 实测)。

改动面:仅 `src/optimizer/statistics/operator/propagate_aggregate.cpp` 与新增测试;不改存储格式、不改表函数接口。

## Context

### Current State

**DuckDB main**(merge-base `ce512b864c`,2026-08)已有提前聚合框架:

1. `StatisticsPropagator::TryExecuteAggregates`(`src/optimizer/statistics/operator/propagate_aggregate.cpp:179`)支持无 GROUP BY 的 `count_star` 与普通列 `min`/`max` 预计算:
   - 识别:`TryGetMinMaxColumnInfo`(`:98`)只接受裸列引用或安全 integral cast;
   - 无 filter 分支:遍历全部分区用 `TryGetValueFromStats` 提取极值再折叠,任一分区统计不精确(`MinMaxIsExact`/`HasPendingWrites` 门控,`:133`)→ **整体 bail**;
   - filter 分支(`:285`):逐分区三态判定(整体预计算 / 整体丢弃 / 进 `scan_partition_indices`),`need_to_scan` 时走部分预计算改写(`COALESCE(least/greatest(pre, agg), pre)` + `set_partitions_to_scan`,`:398-456`),合并投影按聚合**函数名**(`min`→`least`,`max`→`greatest`)选择合并方向。
2. **`MIN/MAX(strlen(col))` 在 main 上不优化**:聚合输入是函数表达式,`TryGetMinMaxColumnInfo` 失败即 bail,查询退化为全列扫描。
3. 底层材料齐备:`get_partition_stats` 回调返回各 row group 的 `PartitionStatistics`;`StringStats` 含 `max_string_length`/`min_string_length`/`has_unicode`,精确且随 checkpoint 持久化(V2 格式;见 Risks 5)。
4. 运行期已有 CPU 层优化:`LengthPropagateStats`(`src/function/scalar/string/length.cpp:64`)在列全 ASCII 时把 `length` 执行函数替换为 `StrLenOperator`,但不减少列读取 IO。
5. 上游 filter 侧已有同类统计应用:PR #23873(已合入)用 `max_string_length` 做 `WHERE strlen(col) > N` 的 row group 裁剪;**聚合侧统计折叠上游无任何 issue/PR**(调研见附录)。

**分支 `max-len-stats`** 已实现两阶段全部内容并验证(测试 `test/optimizer/max_len_skip.test`/`max_len_skip_char.test`,服务器基准 `benchmark/max_len_skip/`):字节长度查询 302ms→101ms、4070 row groups→0 扫描。本提案将该实现拆分后分批推进主线。

### Problem Statement

对"大表字符串列求最大/最小长度"的查询(典型场景:TiFlash issue #11049 的 `SELECT MAX(CHAR_LENGTH(c1)), ... FROM t`,数据质检中的极值长度探查),DuckDB 必须读完整列 payload。所需材料(精确长度统计、提前聚合框架、部分扫描出口)在 main 上全部存在,缺的只是"聚合输入是字节长度函数"的识别与统计折叠——阶段一补上这块,且不引入任何边界推理。

### Constraints

- **结果语义与朴素执行一致**:`MIN`/`MAX` 忽略 NULL;全 NULL 输入返回 NULL;`strlen`/`octet_length` 返回字节数(BIGINT)。
- **统计不确定即不可用**:不精确分区 → 整体 bail(沿用 main 现状,不引入 #23611 的逐分区降级)。
- **不修改存储格式与表函数接口**:存量数据无需迁移;优化器内部改动。
- **识别按函数名**,不受 `LengthPropagateStats` 运行期函数替换影响(替换不改函数名)。

## Goals

1. 统计全部精确时,`MIN/MAX(strlen/octet_length(col))` 零扫描完成:`EXPLAIN` 无 `UNGROUPED_AGGREGATE`,`EXPLAIN ANALYZE` 显示 `Row Groups Scanned: 0`。
2. 字节长度聚合可与 `count_star`、普通列 `min`/`max` 在同一查询混合,全部按各自路径预计算。
3. 任一分区统计不可用(不精确 / pending writes / 非 `STRING_STATS` / 可能全 NULL / 无长度值)→ 整体 bail,结果正确,行为与 main 现状一致。
4. `length`/`char_length` 聚合在阶段一**不被优化**(识别不命中即 bail),结果正确,为阶段二留位。
5. 既有 `partial_aggregate_precomputation` 行为与断言不回归。

## Non-Goals

- **不识别 `length`/`char_length`(码点语义)**:阶段一 `IsByteLengthFunction` 只匹配 `strlen`/`octet_length`;含 char-length 聚合的查询整体 bail(保持 main 行为)。全 ASCII 分区下 char-length 本可精确折叠,刻意推迟到阶段二,避免阶段一触碰 `has_unicode` 语义。
- **不实现 Unicode 安全边界与分区裁剪**(阶段二,见 Phase 2 展望)。
- **不移植 #23611 的逐分区分类框架与不精确降级**;#23611 若未来合入,本实现与其天然兼容。
- **不实现 `SUM(LENGTH)`/`AVG(LENGTH)` 折叠**(`total_string_length` 依赖,且 #23633 报告其压缩列可靠性问题)。
- **不实现 length-only 扫描**(#11051,独立方向)。
- **不支持 CSE 提升场景**:`max(strlen(s))` 与 `min(strlen(s))` 共存时公共子表达式被提升为投影列,聚合输入变为列引用,进入普通 min/max 路径后在投影穿越处 bail——不优化但结果正确(测试锁定该行为)。
- **不修改 `LengthPropagateStats` 运行期替换**。

## Design

### 1) 现状与目标计划形态

```
SELECT max(strlen(s)) FROM t;          -- 统计精确时

main / 阶段一未命中:                    阶段一命中后:
UGROUPED_AGGREGATE(max)                EXPRESSION_GET(常量 100)
  └─ PROJECTION(strlen)                  └─ DUMMY_SCAN
       └─ GET(t)  ← 全列扫描
```

### 2) 识别(嵌入既有 min/max 分支)

`min`/`max` 聚合的子表达式过不了 `TryGetMinMaxColumnInfo` 时,不再直接 bail,先尝试字节长度识别:

- `IsByteLengthFunction`:函数名 ∈ {`strlen`, `octet_length`}(按函数名,精确匹配 bind 后的函数);
- 子表达式是 `BoundFunctionExpression`,唯一子节点为 `BOUND_COLUMN_REF`;接受 VARCHAR 列(BLOB 列统计同为 `STRING_STATS`,`octet_length(BLOB)` 同样折叠,测试覆盖);
- 命中则记入 `length_columns`:`binding`、`result_type`(BIGINT)、`is_min`、该聚合在 `aggr.expressions` 中的位置 `aggr_idx`;同时为普通 min/max 补记 `min_max_aggr_idxs`/`min_max_is_min`(供出口装配与合并投影使用);
- 未命中(char-length 函数、`length(bit)`、CSE 提升后的列引用等)→ bail,现状行为。

穿越 PROJECTION:与普通 min/max 并列,对 `length_columns` 在投影表达式中查找同构 `strlen/octet_length(colref)`,逐层下推绑定;失败即 bail。

### 3) 统计门控与值提取

新增 `TryGetLengthValueFromStats(stats, storage_index, is_min, result)`,门控与既有 `TryGetValueFromStats` 对齐:

- 分区有底层 row group(`partition_row_group` 非空)、列统计存在;
- `MinMaxIsExact(storage_index)` 且无 `HasPendingWrites()`;
- 列统计为 `STRING_STATS`(字节长度只对字符串/BLOB 统计存在);
- `CanHaveNoNull()`——分区可能全 NULL 时,`MIN/MAX` 的 SQL 结果是 NULL 而非数值,不可提取(交由正常执行返回 NULL)。

通过后取值:**MAX → `MaxStringLength`(需 `HasMaxStringLength`);MIN → `MinStringLength`(需有效值)**,产出 BIGINT 常量。任何一步失败 → 整体 bail。

### 4) 装配重构:按聚合位置直接赋值

main 的 `types`/`agg_results` 用 push_back(min/max 顺序)加按位置 insert(count_star)拼装,隐含依赖各类聚合的相对顺序。阶段一改为:数组预开 `aggr.expressions.size()`,三类聚合各算各的、**直接写到自己的聚合下标上**(length 用 `aggr_idx`,普通 min/max 用 `min_max_aggr_idxs`,count 用 `count_star_idx`)。这是三类聚合混合共存的必要前置,也消除 insert 的 O(n²) 拼装。

### 5) 部分预计算出口与方向向量

字节长度全部是精确值,阶段一**不新增任何扫描来源**——`need_to_scan` 仍只由 filter 分支置位。filter 场景下 length 聚合与其他聚合一起走既有部分预计算改写;合并投影的方向判定从"按聚合函数名"改为**按聚合位置的显式 `is_min` 向量**(`min_max_aggr_idxs`/`min_max_is_min` 与 `length_columns` 共同填充),使出口不再依赖函数名分派,阶段二扩展无需再触碰此处。

### 6) 失败与降级路径

| 情形 | 行为 |
| --- | --- |
| 非字节长度函数 / 多参数 / 非 colref 子节点 | bail,现状执行 |
| 穿越投影时不同构 | bail |
| 统计不精确 / pending writes / 非 STRING_STATS / 可能全 NULL / 无长度值 | bail(任一分区失败即整体) |
| filter 下部分分区不可判定 | 进 `scan_partition_indices`,部分预计算合并(既有路径) |
| CSE 提升(聚合输入为列引用) | 进入普通 min/max 路径,投影穿越失败 bail |

所有 bail 路径的结果与朴素执行一致,仅退化为扫描。

## Rollout Plan

**阶段一 = 单个 PR**,建议两个 commit(均以 main 为基):

1. **commit 1(功能主体)**:`propagate_aggregate.cpp` 的识别、门控提取、按位置装配、`is_min` 向量;装配重构与功能耦合紧密(重构本身无独立合入价值),不单独拆 PR。
2. **commit 2(测试)**:移植 `test/optimizer/max_len_skip.test`(289 行,见 Validation),断言按"统计精确才优化"校准。

**阶段二触发条件**:阶段一合入后启动,草案与 TODO 见下节;基准脚本 `benchmark/max_len_skip/` 随对应阶段移植(其 README 门槛表覆盖两阶段)。

## Phase 2 展望:char_length 与 Unicode 边界(草案 + TODO)

### 大致想法

`length`/`char_length` 数的是码点,而统计记录的是字节长度,仅在全 ASCII(`CanContainUnicode == false`)时两者相等。因此把提取函数从"仅精确"泛化为"值 + 类别":

- 全 ASCII 分区 → EXACT,与阶段一同;
- 可能含 Unicode 分区 → BOUNDED 安全边界:**MAX 方向 `UB = max_string_length`(每码点 ≥ 1 字节,字符数 ≤ 字节数);MIN 方向 `LB = ceil(min_string_length / 4)`(UTF-8 每码点 ≤ 4 字节)**;
- 折叠分两遍:第一遍把 EXACT 值折叠成候选极值、按分区收集 BOUNDED 值(无任何 EXACT 候选则 bail——合并出口需要精确锚点);第二遍对 BOUNDED 分区判定能否刷新全局极值,不能则**整个分区跳过**,能则进 `scan_partition_indices` 走部分预计算合并。

正确性直觉(以 MAX 为例):被跳过分区的真实最大值 ≤ `UB` ≤ 候选值,候选值本身 ≤ 全局最大值,故 `max(扫描结果, 候选)` 精确;MIN 对偶。分支 `max-len-stats`(commit `660949920a` 起)已有完整实现、测试与基准(unicode 表 MAX 只扫 ~10% row groups、MIN 只扫 ~1 个边界 row group),阶段二 PR 以其为移植底稿。

### TODO 清单(阶段二 PR 逐项跟进)

1. `TryGetLengthValueFromStats` 增加 `is_char`/`is_exact` 语义:char + `CanContainUnicode` → BOUNDED(上/下界公式如上),否则 EXACT。
2. 识别扩展 `length`/`char_length`,校验唯一子节点为 VARCHAR 列引用(排除 `length(bit)`/`length(list)` 重载)。
3. 第一遍折叠 + `length_has_exact` 锚点检查(无 EXACT 候选 → bail)。
4. 第二遍裁剪:仅"纯 length 查询"(无普通 min/max、无 count_star)允许跳过;混合查询 BOUNDED 分区一律扫描。
5. filter 与裁剪共存:引入 `precomputed_partition_indices` 把筛选后分区位置映射回原始分区索引(`SetPartitionsToScan` 需要原始索引)。
6. `need_to_scan` 新增来源:裁剪判定产生的扫描分区。
7. 移植 `max_len_skip_char.test`;基准门槛:`len_unicode` 的 MAX(char_length) 扫描 ≈ 10%、MIN ≈ 1 个边界 row group、`len_unicode_all` 不劣于基线。
8. (开放设计点)混合查询裁剪策略可否放宽:当其他聚合对该分区的值已全部精确预计算时,是否允许跳过 BOUNDED 分区——分支实现保守取"一律扫描",阶段二设计文档中论证后定夺。

## Validation Strategy

**测试矩阵**(移植分支 `test/optimizer/max_len_skip.test`,sqllogictest,置于 `test/optimizer/`):

| 场景 | 断言 |
| --- | --- |
| `max/min(strlen(s))`,统计精确 | `EXPLAIN` 无 `UNGROUPED_AGGREGATE`;结果 100/1 |
| 与 `count(*)`、普通列 `min(i)` 混合 | 全部预计算,单常量行 |
| filter 整体裁剪若干 row group | 剩余分区全预计算(零扫描) |
| `octet_length(BLOB 列)` | 同样预计算 |
| `length(s)`(码点语义) | **不优化**(`UNGROUPED_AGGREGATE` 仍在),结果正确 |
| CSE 提升(`max`/`min` 共享 `strlen(s)`) | 不优化,结果正确 |
| 全 NULL 列 | 不优化,返回 NULL |
| 空表 | 不优化 |
| update 后统计不精确 | 不优化,结果正确 |
| delete 后 count 近似 | 不优化,结果正确 |
| 单值列 / overflow 大字符串 | 均正常折叠 |

前提:测试库须 `SET storage_compatibility_version='latest'` 创建(`min_string_length` 仅 V2+ 格式持久化,见 Risks 5)。

**回归**:`make allunit`;重点回归 `test/optimizer/partial_aggregate_precomputation.test`(出口重构触及)与 `test/optimizer/statistics/`(既有 #23873 断言)。

**基准门槛**(服务器,复用 `benchmark/max_len_skip/`;分支实测数据,字节查询仅经过阶段一代码路径):

| 查询 | baseline → optimized | 门槛 |
| --- | --- | --- |
| `max_strlen_concentrated`(5e8 行,~100GB) | 302ms → 101ms,4070 → 0 row groups | 零扫描 |
| `min_strlen_concentrated` | 234ms → 97ms,4070 → 0 | 零扫描 |
| `sum_i` / `count_star`(非目标) | 146→131ms / 110→71ms | ±5% 内无回归 |

两模式(关闭 `statistics_propagation` 的 baseline vs 默认)结果值必须一致。

## Risks and Mitigations

1. **字节/码点语义混淆** — 阶段一识别白名单只含字节语义函数,`length`/`char_length` 一律不命中;测试锁定 `length(s)` 不优化。
2. **统计门控遗漏导致错误结果** — 门控逐项对齐既有 `TryGetValueFromStats`(不精确/pending writes/全 NULL),任一失败整体 bail;矩阵覆盖 update/delete/空表/全 NULL。
3. **装配重构引入既有路径回归** — 普通min/max、count_star 改为按位置赋值,语义等价;回归 `partial_aggregate_precomputation.test` 与全量 `make allunit`。
4. **CSE 场景不优化(接受)** — 提升后进入普通 min/max 路径在投影穿越处 bail,结果正确;可作为后续独立增强(识别投影中的 length 列)。
5. **`min_string_length` 持久化依赖存储格式** — 默认 v0.10.2 兼容格式 checkpoint 后丢失该统计,MIN 优化 reopen 后退化为不优化(MAX 的 `max_string_length` 持久化不受影响);测试与基准统一 `storage_compatibility_version='latest'`,文档注明。
6. **与 #23611 未来合入的适配** — 本实现嵌入单块结构;#23611 合入后需将 length 逻辑迁移到逐分区框架(迁移面:识别 + 提取,预计可控)。先合阶段一不等 #23611。

## Alternatives Considered

- **单 PR 全量合入(原 proposal 的单 PR 视角)**:评审面含 Unicode 边界推理与混合查询策略,争议会阻塞本无争议的字节长度部分;拆分后阶段一可与阶段二并行评审。
- **阶段一同时识别 char-length 的全 ASCII 精确路径**:实现增量小,但把 `has_unicode` 语义与"EXACT/BOUNDED 分类"引入阶段一,模糊了两阶段的拆分依据(确定性);推迟到阶段二一次性引入。
- **先移植 #23611 再实现**:获得逐分区降级能力,但引入他人未合入 PR 的依赖,且两年未合入,挂靠风险高。
- **仅依赖运行期函数替换(现状)**:只省 CPU 不省 IO,不解决磁盘带宽瓶颈。

## Appendix: 上游调研摘要(2026-08-20,详版见 `min_max_length_precompute.zh.md` 附录)

- **#23872 / PR #23873**(已合入,2026-07):`WHERE strlen(v) > N` 用 `max_string_length` 做 row group 裁剪——filter 侧应用,与本提案(聚合侧折叠)互补不重叠,PR 提交时可引为同源统计的上下文。
- **聚合侧统计折叠上游无 issue/PR**:本方向为上游空白,具备提交价值。
- **#23633**(已关闭):dictionary 压缩下 `total_string_length` 统计错误——`SUM(LENGTH)` 跟进前需先确认该统计可靠性。
- 组合验证(main 含 #23873):`SELECT max(strlen(v)) WHERE strlen(v) > 30` 两种裁剪叠加生效。
