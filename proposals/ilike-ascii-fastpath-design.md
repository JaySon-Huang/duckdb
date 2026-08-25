# Proposal: ILIKE ASCII fast path for constant ASCII patterns

**Date**: 2026-08-25
**Purpose**: 为 ILIKE / NOT ILIKE（含 ESCAPE 变体）的常量 pattern 快路径增加一条"按字符串运行时检测 ASCII"的字节级折叠匹配路径，避免对可能含 Unicode 的列逐行做 UTF-8 大小写折叠与折叠副本物化。

## Summary

常量 pattern 的 ILIKE 当前需要逐行执行 `LowerLength` + `LowerCase`（UTF-8 感知、逐 codepoint 解码）并物化一份小写副本，即使 pattern 本身是纯 ASCII 也是如此。本提案在 `ILikeFunction` / `ILikeEscapeFunction` 的常量 pattern 分支中增加一个快速路径：

- 折叠后的 pattern 为纯 ASCII 时，对每一行先做一次无分配的单遍 ASCII 检测（逐字节 `& 0x80`，与 `src/function/scalar/string/substring.cpp` 中已有的先例一致）；
- 字符串全 ASCII 时，直接在该字符串上做**字节级折叠匹配**（匹配时逐字节查 `ASCII_TO_LOWER_MAP`），跳过 `LowerLength`/`LowerCase`、跳过折叠副本分配、跳过 `Utf8Proc` 调用；
- 字符串含任何非 ASCII 字节时，**回退到现有路径**，行为与现状完全一致。

触发条件比参考的 TiFlash PR（pingcap/tiflash#10400，utf8 CI collation 下 LIKE 优化）更保守：TiFlash 以"pattern 全 ASCII"为入口、在匹配中遇到非 ASCII 再回退；本提案以"单个字符串全 ASCII"为安全域。原因见 Design 1) 的正确性论证——存在会在折叠后进入 ASCII 域的非 ASCII codepoint（如 U+212A KELVIN SIGN → `k`、U+017F LONG S → `s`），仅凭 pattern 是 ASCII 无法安全地省略串侧折叠。

## Context

### Current State

ILIKE / NOT ILIKE 在 `src/function/scalar/string/like.cpp` 中有三条路径：

1. **通用路径**（非常量 pattern）：`ILikeOperator::Operation` → `ILikeOperatorFunction`（like.cpp:419-448），每行执行两次 `LowerLength`（一次算折叠后长度、一次分配）+ `LowerCase`（折叠写入），再由 `TemplatedLikeOperator` 递归匹配。每行两次堆分配，且 `LowerLength`/`LowerCase` 会走 `Utf8Proc::UTF8ToCodepoint`（`src/function/scalar/string/caseconvert.cpp:31-76`，`third_party/utf8proc`）。
2. **常量 pattern 快路径**（commit `1de34a64ae`，已在 main）：`ILikeFunction`（like.cpp:598-632）和 `ILikeEscapeFunction`（like.cpp:526-578）在 pattern 为 `CONSTANT_VECTOR` 时只折叠 pattern 一次，复用同一个 scratch buffer 逐行折叠字符串，然后走 `LikeMatcher::Match`（like.cpp:242-299，按 `%` 拆分字面段，段匹配用 `FindStrInStr` 的 SIMD 实现，`src/function/scalar/string/contains.cpp:79-110`；含 `_` 或 escape 时 `CreateLikeMatcher` 返回 nullptr，回退到递归匹配器）。**这一路径省掉了逐 row 的 pattern 折叠与分配，但字符串侧的逐行 `LowerLength`/`LowerCase` 仍在。**
3. **统计驱动路径**：`ILikePropagateStats`（like.cpp:581-591）在列的 `StringStats::CanContainUnicode` 为 false 时把执行函数替换为 `ILikeOperatorASCII`（like.cpp:487-500）——`ASCIILCaseReader`（like.cpp:174-182）逐字节查 `ASCII_TO_LOWER_MAP`，无分配、无 UTF-8 处理。该回调只安装在 ILIKE / NOT ILIKE（无 ESCAPE）上。

几条相关事实：

- `StringStats::CreateUnknown` 默认 `has_unicode = true`（`src/storage/statistics/string_stats.cpp:34`）；只有扫描数据后才能置 false。因此**无统计信息或统计不完整**（未 ANALYZE、大分区表、表达式列）的列 `CanContainUnicode` 恒为 true，走第 2 条路径。
- `ASCII_TO_LOWER_MAP` 是 256 项表：0-127 做小写折叠，128-255 为恒等映射（`src/common/string_util.cpp:1038-1051`）。
- DuckDB 的字符边界定义：`IsCharacter(c) = (c & 0xc0) != 0x80`（`src/include/duckdb/function/scalar/string_common.hpp:25`），即并置字节不算字符；`_` 匹配一个"字符"，`StandardCharacterReader::NextCharacter` 跳过并置字节。
- ILIKE / LIKE 的 collation 处理：绑定阶段 `PUSH_COMBINABLE_COLLATIONS` 会把 collator 函数包到参数上（`src/function/function_binder.cpp:742`，`src/planner/collation_binding.cpp:110`），ICU 扩展的 collator 生成归一化/折叠后的排序键后再匹配——这是一条独立路径。

### Problem Statement

无统计信息或统计未知的列上，ILIKE 的常量 pattern 快路径仍然要为每个字符串：

1. 调用 `LowerLength`——一次全串扫描，每字符一次 `Utf8Proc::UTF8ToCodepoint` 解码；
2. `LowerCase`——第二次全串扫描并写入折叠副本；
3. 在折叠副本上再做 matcher 扫描（`FindStrInStr` / memcmp / 递归匹配）。

对于 ASCII 文本占绝大多数的典型列（日志、注释、标识符、账目），这些 UTF-8 解码与副本物化几乎没有意义。TiFlash PR #10400 在同一问题上拿到约 3-6 倍的吞吐提升（`SELECT COUNT(*) FROM orders WHERE o_comment LIKE '%ZZZ%'`，general_ci 4.1s→0.8s）。DuckDB 的统计路径只覆盖"统计已证明纯 ASCII"的列，覆盖不到"统计未知但数据实际是 ASCII"的常见场景。

### Constraints and Decision Drivers

- **结果集语义必须与现有实现完全一致**：大小写折叠、`_` 的字符边界语义、`%` 通配、escape、嵌入 NUL 的最近修复（`169f23eaac` / `70c3bb15ff`）都不能回归。
- **判定失败必须无成本螺旋**：快速路径只应改变性能，不应改变错误路径（invalid UTF-8 的抛错行为）、collation 路径或统计路径。
- **尽量复用现有结构**：已有 `ASCIILCaseReader`、`LikeMatcher`、`substring.cpp` 的 ASCII 检测先例；不引入新的执行层或公共抽象。
- **改动面小、可独立评审**：全部改动集中在 `like.cpp` + 测试，不触碰 planner / optimizer / collation。

## Goals

1. 常量 pattern 折叠后为纯 ASCII 时，ILIKE / NOT ILIKE（无 `_`、无 escape：前缀/后缀/包含/全等形态）在**任意行数据**（含非 ASCII、无统计）上不再物化折叠副本、不调用 `Utf8Proc`、不逐行分配。
2. 每行成本从"两次扫描 + 一次分配 + 一次折叠拷贝 + matcher 扫描"降至"一次扫描（ASCII 检测）+ 匹配（含逐字节查表比较）"。
3. 含 `_` / escape 的 ASCII pattern 也覆盖到（走 `ASCIILCaseReader` 化 `TemplatedLikeOperator`，同一触发条件）。
4. 所有行数据（ASCII / 非 ASCII / 无效 UTF-8 / NUL）在快速路径与现有路径上的结果与错误行为逐例一致。
5. 在 ASCII 占比高的列上无行为回退（最坏情形只多一次无分配扫描）。

## Non-Goals

- 不改变 collation（ICU 等）下的 LIKE / ILIKE 行为——collation 参数被禁用本快速路径（见 Design 5)）。
- 不优化 `LIKE`（大小写敏感 `~~`）——它没有折叠开销，`LikeMatcher` 已走 memcmp/`FindStrInStr`。
- 不改变 `ILikePropagateStats` 安装的统计驱动回调；未来可在 Phase D 考虑合并。
- 不为"字符串含非 ASCII 但仍可能安全匹配"的场景做精细分析（例如只有 `%`、无 `_`、且字符串中的非 ASCII 都可以证明不是 U+212A 一类折叠进 ASCII 域的 codepoint）——留给 Phase D 评估。
- 不做 `TemplatedLikeOperator` 的通用迭代化重写（GLOB 的重写 `3e2a411664` 是另一个话题）。

## Design

### 1) 正确性论证：为什么安全域是"字符串全 ASCII"，而不是"pattern 全 ASCII"

记折叠操作 `fold`（字符串侧 = `UTF8Proc::CodepointToLower`，或 ASCII 域内的 `ASCII_TO_LOWER_MAP`），记字节级匹配 `M_b`（pattern 字节与字符串字节查表比较），字符级匹配 `M_c`（先 `fold` 整个字符串，再在折叠结果上做 LIKE 字符匹配）。

**事实 A**：`ASCII_TO_LOWER_MAP[128..255]` 是恒等映射（string_util.cpp:1044-1051）。

**事实 B**：合法 UTF-8 中，任何非 ASCII codepoint 的编码字节全部 ≥ 0x80（首字节 0xC2-0xF4，并置字节 0x80-0xBF）。因此对非 ASCII codepoint，`fold`（无论 UTF-8 解码后的 codepoint 折叠还是字节恒等）**都不会产生任何 ASCII 字节**——除非该 codepoint 本身折叠为一个值 < 0x80 的 codepoint。这类 codepoint 确实存在：`U+212A KELVIN SIGN → U+006B 'k'`、`U+017F LATIN SMALL LETTER LONG S → U+0073 's'` 等（utf8proc case-folding 数据）。

**推论 1（pattern-ASCII 不充分）**：如果 pattern 折叠后就是 `'k'`，字符串 `"K"`（Kelvin）在字符级语义下应匹配（`LowerCase("K") == "k"`）；若以"pattern 全 ASCII"为入口做字节恒等比较，会漏掉这个匹配。TiFlash 的方案通过"匹配中遇到非 ASCII 字符即回退"规避了这一点。

**推论 2（字符串全 ASCII 充分）**：当字符串全部字节 < 0x80 时，每个字节就是一个 codepoint，此时：

- 字节级 `ASCII_TO_LOWER_MAP` 折叠 = codepoint 级折叠（UTF-8 与 ASCII 编码一致，utf8proc 对 0x00-0x7F 的折叠映射与之相同，可加 debug 断言对拍确认）；
- `_` 匹配 1 字节 = 1 字符，`IsCharacter` 的字节边界定义自动退化为每字节一字符；
- `%` 在字节序列上与字符序列上等价（pattern 字面段是 ASCII 字节序列，段出现位置与字节位置一一对应）。

因此在"字符串全 ASCII"域内 `M_b(fold 逐字节) ≡ M_c(先 fold) `，且 `fold` 不需要物化——逐字节查表即可。

**推论 3（回退保证语义不变）**：字符串含 ≥ 0x80 字节时直接走现有路径（`LowerLength` + `LowerCase` + matcher）。现有路径对无效 UTF-8 会通过 `Utf8Proc::UTF8ToCodepoint` 抛异常，本快速路径因永远不见非法字节而不会改变该行为。**快速路径只会命中合法 ASCII 数据，其它一切输入走原路径——语义等价是构造保证，不是测试碰巧。**

### 2) 当前路径示意

```
ILIKE(constant pattern)
  ├─ column 统计 CanContainUnicode == false ──► ILikeOperatorASCII（现有统计路径，无分配）
  └─ 否则（默认，无统计/统计未知）
       ├─ 非常量 pattern ──► 每行: LowerLength + 分配 + LowerCase + 递归匹配
       └─ 常量 pattern（现状）──► fold pattern 一次
                                    └─► 每行: LowerLength + scratch 拷贝 LowerCase
                                          └─► LikeMatcher::Match（分段 memcmp/FindStrInStr）
                                               └─ create-matcher 失败(含 '_'/escape) ──► 递归匹配
```

### 3) 建议路径

```
ILIKE(constant pattern)
  ├─ column 统计 CanContainUnicode == false ──► ILikeOperatorASCII（不变）
  ├─ 参数带 collation ──► 现状（collator 包裹；不变）
  └─ 常量 pattern（新）
       ├─ fold pattern 一次
       │    ├─ fold 后含非 ASCII 字节 ──► 现状（matcher / 递归）
       │    └─ fold 后全 ASCII：
       │         每行:
       │           ├─ IsAllASCII(str)（单遍 & 0x80，可选 SIMD/64-bit 加速）
       │           │    ├─ true  ──► ASCIIFastMatcher（字节级折叠匹配，无分配/无 UTF8Proc）
       │           │    └─ false ──► 现状（LowerLength + scratch LowerCase + matcher/递归）
       │              （含 '_'/escape 时快速匹配器/递归= ASCIILCaseReader 化的 TemplatedLikeOperator）
```

### 4) 实现要点

**触发条件判定**（两处入口共用）：

- `ILikeFunction`（like.cpp:598）：pattern 为常量时先 `fold pattern`（现有代码 603-606 行已做），同时在折叠过程中或折叠后记录 `pattern_folded_is_ascii`（`bool`，任意字节 `& 0x80` 判定，单遍即可，无须额外扫描）。
- `ILikeEscapeFunction`（like.cpp:526）：同上；escape 字符本身若 ≥ 0x80，则它出现在 pattern 中的位置折叠后仍非 ASCII，天然不触发。
- 附加激活条件：两个参数（str / pattern / escape）的 `StringType::GetCollation` 均为空。带 collation 时 pattern 可能已被 collator 包裹为常量向量，不能假定字节语义；此检查与 `LikeBindFunction`（like.cpp:355-370）对 collation 的处理一致。

**快速匹配器**（新增，`like.cpp` 内部）：

- 无 `_`、无 escape：`LikeMatcher` 的 ASCII 折叠变体。segment 定位不能用裸 `FindStrInStr`（字符串未折叠，`memchr(0x61)` 找不到 `'A'`），改为"候选定位 + 逐字节折叠比较"：段首字节按折叠表展开候选（对 `'a'` 是 `'A'`/`'a'` 两个字节，可用两次 `memchr` 或一次折叠后的首字节命中评测），候选处按 `fold(str[i]) == segment[i]` 逐字节确认；前缀/后缀段用同样的折叠比较而非直接 `memcmp`。
- 含 `_` / escape：新增模板实例 `TemplatedLikeOperator<'%', '_', true/false, ASCIILCaseReader>`——pattern 在外部已折叠（幂等，重复折叠无害），`ASCIILCaseReader` 对字符串字节查 `ASCII_TO_LOWER_MAP`；`_` 的字符边界由 `ASCIILCaseReader::NextCharacter` 处理。注意：与现有 `ILikeOperatorASCII`（like.cpp:487）的区别是现有版本直接从原始 pattern 逐行读表，新实例可用于"pattern 已折叠一次"的场景，二者可共存。
- `IsAllASCII(const char *data, idx_t len)`：单遍 `data[i] & 0x80`；实现上可先尝试按 8 字节/64-bit 归约加速（与 `substring.cpp:210-221` 的现有循环先例相同，那里是逐字节）。放 `string_common.hpp` 或 `like.cpp` 内部，取决于复用面。

**NOT ILIKE / ESCAPE 变体**：`ILikeFunction<OP, INVERT>` 模板已统一处理 `INVERT`（like.cpp:597）；`ILikeEscapeFunction<true>` 同理。快速路径内做一次 `INVERT ? !match : match`，与现有代码一致。

**统计路径的取舍**：`ILikePropagateStats` 保持不动。新路径在运行时判定，统计路径在计划期判定；二者的交集（列统计证明纯 ASCII 且 pattern 也 ASCII）以统计路径优先（它不需每行检测）。将来可评估：统计路径回调用"折叠后 pattern 全 ASCII"且字符串全 ASCII 的联合判定来避免重复——Phase D。

### 5) 兼容性与失效行为

- **collation**：带 collation 的参数激活条件排除（见上）。行为与现状完全一致，无需额外测试之外的动作。
- **无效 UTF-8**：快速路径只见全 ASCII 字符串；非法字节一律回退，抛错行为不变。原始 pattern 无效 UTF-8 时，fold pattern 阶段（`LowerLength`，like.cpp:603/539）在进入快速路径前已按现状抛错——行为不变。
- **NUL**：所有比较均携带显式长度（`FindStrInStr`、`memcmp`、`string_t` 长度），不依赖 NUL 结尾；与近期 NUL 修复一致，快速路径继承该约束。新增测试覆盖嵌入 NUL。
- **回滚**：无配置开关、无持久化状态、无接口变化。回滚 = 还原提交；`ILikePropagateStats` 和通用路径不受影响。
- **观察性**：无新指标。性能验证依赖基准（见 Validation）。

### 6) 边界情况清单（测试用例直接映射）

| 输入 | 期望 | 快速路径行为 |
| --- | --- | --- |
| 字符串全 ASCII、pattern `%abc%` | 与现状一致 | 命中 |
| 字符串含中文、pattern `%abc%` | 与现状一致 | 检测后回退 |
| 字符串含 `K`、pattern `%k%` | true（折叠后 `k`） | 回退后正确 |
| pattern `'A_B'`、字符串 `'AαB'` | true（`_` 匹配一个字符） | 回退（含非 ASCII） |
| pattern `'A_B'`、字符串 `'AXB'` | true（字节级 `_` 匹配一字节） | 命中 |
| 字符串含孤立 0x80 字节 | 现状抛错 | 回退后抛错 |
| 嵌入 NUL | 与现状一致 | 携带长度比较 |
| pattern 含非 ASCII（如 `%café%`） | 现状 | 不触发（fold 后非 ASCII） |
| escape 常量且 pattern 中出现 escape | 现状语义 | `ASCIILCaseReader` 化模板实例 |
| collation 修饰的列 / pattern | 现状 | 不触发 |

## Compatibility and Invariants

- ILIKE（`~~*`）与 NOT ILIKE（`!~~*`）对全部合法输入的布尔结果与错误行为不变——由"全 ASCII 才快速、其余回退"的构造保证。
- `ILIKE ... ESCAPE` 的 escape 语义、尾部 escape 的 `SyntaxException`（like.cpp:192-195）不变。
- 大小写折叠只使用 `ASCII_TO_LOWER_MAP` 的 0-127 段（128+ 恒等），与 UTF-8 路径在 ASCII 域一致。
- `_` 的字符语义 = `IsCharacter` 定义（并置字节不算字符），与 `length`/`substring` 等字符串函数使用同一判定。
- collation 处理（`PUSH_COMBINABLE_COLLATIONS`、ICU collator）不因本提案变化。
- 统计传播回调（`ILikePropagateStats`）及其决策条件不因本提案变化。

## Incremental Plan

- **Phase A — 无 `_` / 无 escape 的快速匹配器**：`IsAllASCII` + `ASCIIFastMatcher`（LikeMatcher 的折叠变体）；`ILikeFunction` 常量分支接入；扩展 sqllogictest（ASCII/非 ASCII 混合、大小写、NUL、无效 UTF-8 回退、`%` 前缀/后缀/包含/全等）。体积小、可独立评审。
- **Phase B — `_` / escape 覆盖**：`TemplatedLikeOperator<'%','_',{true,false},ASCIILCaseReader>` 实例 + `ILikeEscapeFunction` 接入 + 对应测试（`_` 跨多字节、escape 常量与非 ASCII escape、尾部 escape 报错）。
- **Phase C — 验证与性能**：随机对拍（见 Validation）+ 基准 + `make allunit` + format。
- **Phase D — 可选后续**：合并扫描（检测与匹配一体）、SIMD 折叠比较、与 `ILikePropagateStats` 的统一、针对"仅含 `%` 且无 `_`"场景的带回退细化（需论证 U+212A 类 codepoint）。

## Validation Strategy

1. **sqllogictest**：扩展 `test/sql/function/string/test_ilike_constant_pattern.test` 与 `test_ilike_escape_constant_pattern.test`，或新增 `test_ilike_ascii_fastpath.test`，覆盖 Design 6) 表格全部行；保留"非 ASCII pattern 不触发"的回归用例。
2. **结果对拍**：debug 构建中让快速路径与现有路径在随机生成的 pattern / 字符串（合法 UTF-8 + 无效 UTF-8 + NUL）上双跑并 `D_ASSERT` 结果一致，作为 sqllogictest 之外的正确性兜底（可放在测试 helper 或单测中，如 `test/api` 下的 C++ 单测）。
3. **性能基准**：一个大表（如 10M 行），列含"ASCII 为主、少量非 ASCII"的数据且不做 ANALYZE（保证走常量 pattern 快路径而非统计路径），比较 `ILIKE '%term%'` 构建前后耗时与内存分配（`SET enable_profiling` / perf）。参考量级：TiFlash 同型优化约 3-6 倍。
4. **工程检查**：`make allunit`、`make format-fix`；收集 benchmark 数据附在 PR 描述。

## Risks and Mitigations

1. **字符边界语义回归（`_`）** —— 安全域论证（推论 2/3）+ `_` 用例集合测试 + 全 ASCII 域内 `IsCharacter` 自动退化；非 ASCII 一律回退。
2. **折叠表与 utf8proc 在 ASCII 域不一致** —— 二者对 0x00-0x7F 的映射一致（可加 debug 断言对拍 256 项）；现有 `ILikeOperatorASCII` 已在生产路径使用同一张表，风险与现状等同。
3. **回退一致性随代码演化被破坏**（未来修改通用路径）—— 快速路径激活条件显式集中、回退调用点唯一（直接调用现有折叠逻辑，不复制实现）；对拍断言兜底。
4. **最坏情形性能恶化**（ASCII 检测扫描浪费）—— 检测是单遍无分配线性扫描（逐字节 `& 0x80`，可 64-bit 归约），显著低于它替代的 `LowerLength` 扫描 + 分配 + `LowerCase` 扫描；基准验证。
5. **collation 交互误判断**（pattern 被 collator 包裹仍显示为常量）—— 激活条件显式检查 `GetCollation` 为空；附带回归测试（ICU 扩展下 `ILIKE ... COLLATE`）。
6. **无效 UTF-8 行为漂移** —— 回退设计已消除：快速路径不见 ≥ 0x80 字节；pattern 侧折叠先于判定，抛错行为不变。

## Alternatives Considered

1. **TiFlash #10400 方案（pattern-ASCII 触发 + 匹配中回退）**：入口更宽（一行字符串含非 ASCII 也能走一部分字节级匹配），但需要匹配器支持"中途放弃并重入完整路径"，且必须处理 U+212A 一类折叠入 ASCII 域的 codepoint（找到时若已消费部分 `%` 通配段，回退需要恢复状态或从行头重做）。本提案选择"行内先检测、全部 ASCII 才快速"，实现与正确性论证都更简单，安全域是构造性的。
2. **仅依赖现有统计路径**：覆盖不到无统计/统计未知的列——正是本提案的目标场景。
3. **把整个字符串折叠改为 SIMD 单遍折叠 + 匹配**：改动大、正确性风险高（UTF-8 变长、无效字节），收益不确定；列为 Phase D 研究方向。
4. **复用 `Utf8Proc::Analyze` 做 ASCII 检测**：该函数返回 UnicodeType 并做完整校验，开销高于单遍 `& 0x80` 检测；仅在需要"无效 UTF-8 报错"语义时考虑（当前不需要——快速路径不见非法字节）。

## Open Questions

1. **`IsAllASCII` 放哪、是否复用**：`substring.cpp` 有相同的内联循环先例；本提案建议提取到 `string_common.hpp` 供两处复用（若 reviewer 不赞成则各自持有一份，避免公共 API 膨胀）。实现 Phase A 时定，不影响设计。
2. **快速匹配器的 SIMD 化**（`%` 段候选定位与折叠比较）达到什么程度：建议先实现"双候选 memchr + 逐字节折叠比较"，基准后再决定是否上 SIMD；默认不做。
3. **是否合并统计路径**：Phase D 候选，不阻塞本提案；默认保持两条路径并存。
4. **性能基准的验收基线**：以 TiFlash 数据（3-6 倍）为参考还是以 DuckDB 自身的绝对数字为准——建议以 DuckDB 自身构建前后对比为准，PR 描述里引用 TiFlash 量级仅作背景。
