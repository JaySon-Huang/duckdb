# Proposal: 字符串长度专用扫描(length-only scan)——TiFlash #11051 在 DuckDB 的落地

**日期**: 2026-08-20
**目的**: 让只需要字符串长度(`LENGTH`/`CHAR_LENGTH`/`strlen`/`octet_length`)而无需 payload 的查询,在存储层只读取每行 4 字节的 offset 数组、跳过字符串 payload,将 TiFlash issue #11051("read only StringSizes for LENGTH / CHAR_LENGTH scans")在 DuckDB 上落地。

## Summary

在 DuckDB 实现"length-only"扫描:当投影/过滤/聚合只使用某字符串列的长度函数时,优化器把"列需要长度而非 payload"的信号经 `projection_expression_pushdown` 回调下推到 `seq_scan`,存储层以专门的扫描路径只读 StringSegment 的 offset 数组(每行 4 字节,长度 = 相邻 offset 差)并直接输出 BIGINT 长度列,跳过 payload 的读取、解压与 `string_t` 构造。

该优化与已完成的统计折叠(`MIN/MAX(LENGTH-family)` 提前计算,见 `max_len_skip_on_23611.zh.md`)**正交互补**:统计折叠在计划期避免扫描(全精确时零扫描);length-only 扫描在执行期最小化逐行读取成本(覆盖统计折叠达不到的场景:带 GROUP BY、`SUM`/`AVG`、过滤、排序、连接、统计不精确时的兜底)。

**收益性质**:受 DuckDB 布局约束(offset 数组与 payload 同 block,block 为最小读取单元),length-only 扫描省的是解压、payload 拷贝、`string_t` 构造与下游处理——**CPU 与内存带宽收益**,长字符串列(日志、URL、文本)上最显著;不改变 block 级磁盘 IO(压缩列不在本 proposal 范围,见 Non-Goals)。

## Context

### Current State

**问题查询**:`SELECT strlen(s), ... FROM t`、`WHERE strlen(s) > 100`、`GROUP BY strlen(s) % 10`、`SUM(strlen(s))` 等,只需长度但必须读整列 payload。

**DuckDB 现状**:

1. **存储布局已支持长度直接计算**:StringSegment 每行固定 4 字节 offset 数组 + 字典 payload(`src/include/duckdb/storage/string_uncompressed.hpp`)。`StringScanPartial` 已按 `string_length = |cur| - |prev|` 计算长度(`src/storage/compression/string_uncompressed.cpp:92-113`),null 行存储为前值拷贝以便长度计算。**长度信息完全蕴含在 offset 数组中**,但当前扫描路径无条件调用 `FetchStringFromDict` 构造 `string_t`(pin payload block、memcpy)。
2. **优化器侧下推机制不完整**:`projection_expression_pushdown` 表函数回调已存在(`src/include/duckdb/function/table_function.hpp:494`);parquet 已注册 `ParquetProjectionExpressionPushdown` 且接受 `strlen`/`octet_length`(`extension/parquet/parquet_multi_file_info.cpp:385-393`),reader 侧有 `ByteArrayLengthColumnReader`(读 4 字节长度、`inc` 跳过 payload,`extension/parquet/reader/byte_array_length_column_reader.cpp`)。**但驱动回调的 TypePushdown pass 只收集 CAST 表达式**(`src/optimizer/type_pushdown.cpp:130-215` 的 `CastCollect` 仅处理 `BOUND_CAST`,`col_to_cast` 只存 cast)——实测 `SELECT strlen(s) FROM read_parquet(...)` 计划仍读 s 列、投影保留 `strlen(s)`,**函数下推链路未打通**。
3. **原生表未注册回调**:`TableScanFunction::GetFunction()`(`src/function/table/table_scan.cpp:1050-1078`)未设置 `projection_expression_pushdown`,且 StringSegment 无 length-only 扫描路径。
4. **统计信息可用作语义判定**:`has_unicode`(全 ASCII 判定的反面)、`max_string_length`/`min_string_length`(精确、持久化,`src/include/duckdb/storage/statistics/string_stats.hpp`)、`MinMaxIsExact`(精确性门控)。CPU 层已有 `LengthPropagateStats` 在列全 ASCII 时把 `length` 执行函数替换为 `StrLenOperator`(`src/function/scalar/string/length.cpp:64-94`),但列仍全量读出。

### Problem Statement

统计折叠(Phase B/C)只覆盖无 GROUP BY 的 `MIN/MAX`;`SUM`/`AVG`、带分组、过滤、排序、连接的查询仍需逐行处理字符串。对这些查询,当前必须读入整列 payload 并构造 `string_t`,尽管长度信息已存在于 offset 数组。优化器到存储层之间缺少"列只需要长度"的信号传递,存储层缺少对应的扫描路径。

### Constraints

- **不改变存储格式与表函数接口签名**:复用现有 `projection_expression_pushdown` 回调;存量文件无需迁移。
- **语义正确**:`length`/`char_length` 是码点数,`strlen`/`octet_length` 是字节数;`length(bit)`/`length(list)` 不是字符串长度。`has_unicode == true` 的分区上码点长度不能直接由 offset 数组得出,必须回退正常扫描。
- **null 语义**:`length(NULL) = NULL`;length-only 扫描必须同时输出 validity,不能把 null 行的长度(存储为前值拷贝)当结果。
- **收益定位**:offset 数组与 payload 同 block,length-only 扫描不省 block 级 IO(见 Non-Goals)。

## Goals

1. `seq_scan` 注册 `projection_expression_pushdown`,支持字节语义函数(`strlen`/`octet_length`)的 length-only 下推。
2. 优化器 TypePushdown pass 扩展:收集"投影中只被 length 函数使用的列",与既有 CAST 下推共用冲突分析机制。
3. StringSegment 增加 length-only 扫描路径:只读 offset 数组(含 validity),输出 BIGINT 长度列,不 pin payload、不构造 `string_t`。
4. 码点语义函数(`length`/`char_length`)结合 `has_unicode` 统计判定:全 ASCII 分区走 length-only,否则回退。
5. 与统计折叠互补:带 GROUP BY、`SUM`/`AVG`、过滤、排序、连接场景受益;统计不精确时作为兜底。
6. 正确性由测试矩阵锁定(null、Unicode、overflow 字符串、与过滤/分组/连接组合)。

## Non-Goals

- **不分离 offset 与 payload 的物理存储**:offset 数组与 payload 同 block 是现有布局;将其拆为独立流(可省 block IO)需要存储格式变更与迁移,属独立 proposal。
- **不新增文件级 `all_ascii` 标志**(TiFlash 方案):DuckDB 用 `has_unicode` 精确统计判定,天然覆盖存量数据,无需写时记录与旧文件回退。
- **不覆盖字符串内容类查询**:`LIKE`/`prefix`/`contains`/正则等需要 payload,length-only 无收益。
- **不覆盖压缩列(dictionary/fsst)**:压缩列的 offset 数组是字典索引、payload 在字典中,length-only 需要"每值长度"信息;本 proposal 仅覆盖 uncompressed 列,压缩列回退正常扫描。字典列的长度缓存(短字典场景收益显著)列为后续独立项。
- **不将 TypePushdown 通用化为任意函数下推**:本 proposal 只收集 length 家族函数;`upper(col)`/`substr(col, ...)` 等纯函数下推需另行评估收益与冲突分析复杂度,不在本 proposal 内。
- **不扩展 parquet**:`BYTE_LENGTH` 已存在但受 TypePushdown pass 限制;本 proposal 打通 pass 后 parquet 自动受益,`length`/`char_length` 的 parquet 扩展列为后续。

## Design

### 1) 现状架构快照

```
SELECT strlen(s) FROM t;                       SELECT strlen(s) FROM read_parquet('...');

PROJECTION(strlen(s))                          PROJECTION(strlen(s))
  └─ SEQ_SCAN(s)          ← 读整列 payload       └─ READ_PARQUET(s)  ← 同样读整列
                                                            (BYTE_LENGTH 回调存在但 pass 不驱动)
```

StringSegment 布局(每行 4B offset + payload 同 block):
```
block: [offset 数组: int32 × 行数] [payload/字典区]
长度 = |offset[i]| - |offset[i-1]|;null 行 offset = 前值拷贝;大字符串 offset 为负(overflow marker)
```

### 2) 优化器:TypePushdown pass 扩展(前置)

当前 `CastCollect` 只收集 `BOUND_CAST`(`type_pushdown.cpp:130-215`)。扩展方案:

- 新增 `BoundFunctionExpression` 收集:投影表达式为 `length-family(colref)` 形态(`strlen`/`octet_length`/`length`/`char_length`,按函数名识别;`length` 需校验输入为 VARCHAR,排除 BIT/LIST 变体)时,把 `(column_index → expr)` 记入 `col_to_cast`(改名 `col_to_expr` 或复用)。
- 冲突分析复用现有机制:列同时被非 length 使用(裸列引用、其他函数)→ 注册冲突(`nullptr`)→ 不推。
- 下推成功后复用 `CastReplace` 的替换路径:投影中的 `strlen(col)` 替换为对 GET 输出列的引用,`returned_types[storage_index] = BIGINT`。
- `ReachesPushdownGet`/`FindGetsAndProjections` 的"clean projection"判定扩展:接受 `BOUND_FUNCTION`(length 家族)。

**注**:parquet 的 `ParquetProjectionExpressionPushdown` 已按此接口实现(接受 `strlen`/`octet_length`),pass 打通后其自动生效;原生表 `seq_scan` 需注册等价回调。

### 3) 接口:seq_scan 注册

`TableScanFunction::GetFunction()` 增加:

```
scan_function.projection_expression_pushdown = TableScanProjectionExpressionPushdown;
```

回调校验:表达式是 `strlen`/`octet_length`(Phase A/B)或 `length`/`char_length` + VARCHAR 输入(Phase C),返回 true;其他返回 false。成功后把"该 storage index 需要长度输出"记录到 `TableScanBindData`(如 `unordered_set<StorageIndex> length_only_columns`)。

### 4) 存储:StringSegment length-only 扫描

新增 `UncompressedStringStorage::StringScanLengthPartial`(与 `StringScanPartial` 并列):

- 只访问 offset 数组(`base_data[start + i]`)与 validity;不 pin payload 区、不调用 `FetchStringFromDict`。
- 输出:BIGINT 向量;长度 = `|cur| - |prev|`(与 `StringScanPartial` 同算术,overflow 字符串的负 offset 用 `abs` 同样成立);null 行写 null。
- 大字符串(overflow block):长度计算不需要访问 overflow 内容(offset 差已含长度),天然支持。
- 列压缩为 dictionary/fsst 时:字典压缩列的 offset 数组是字典索引,payload 在字典中——length-only 需读取字典(所有不同值的 payload)或另存长度信息;本 proposal 仅覆盖 uncompressed 列,压缩列回退正常扫描(见 Non-Goals)。

### 5) 语义判定:字节 vs 码点

- 字节语义(`strlen`/`octet_length`):offset 差即精确长度,无条件 length-only。
- 码点语义(`length`/`char_length`):`has_unicode == false` 的分区(全 ASCII)码点数 == 字节数,length-only;`has_unicode == true` 的分区必须回退正常扫描(码点数需逐字符数)。
  - 实现:scan 初始化时检查该列统计(与 `MinMaxIsExact` 相同门控);判定失败回退 `StringScanPartial`。
  - 分区粒度:同一列的 row group 统计可能不同(部分含 Unicode),按 row group 决定扫描路径。

### 6) 与现有优化的交互

- **统计折叠**(已完成):`MIN/MAX(LENGTH-family)` 全精确零扫描时无扫描发生,length-only 不介入;部分预计算扫描的分区、带 GROUP BY、`SUM`/`AVG` 由 length-only 兜底。
- **列裁剪/投影下推**:length-only 下推后 GET 输出 BIGINT 长度列,下游投影/过滤引用长度列;`filter_prune`/列裁剪自然生效。
- **LengthPropagateStats**:运行期函数替换(全 ASCII 列)不改变函数名,识别按函数名稳定;length-only 使其从"省 CPU 不省 IO"升级为"不读 payload"。
- **过滤场景**:`WHERE strlen(s) > 100`——表达式过滤下推时,filter 表达式中的 `strlen(s)` 需随下推替换为长度列引用(与投影替换同机制);过滤列从 GET 输出长度列,scan 内过滤在长度上执行。

### 7) 失败/降级路径

- pass 未识别(列被混用、函数不匹配)→ 不推,现状扫描,语义安全。
- 存储层不支持(压缩列、判定失败)→ 回退正常扫描。
- 回调缺失 → 不推(bail,与现状一致)。

## Incremental Plan

### Phase A: 优化器 pass 扩展 + parquet 验证

- TypePushdown 支持 length-family 函数收集与冲突分析;复用 `CastReplace` 替换路径。
- 验证:`EXPLAIN SELECT strlen(s) FROM read_parquet(...)` 投影消失(parquet BYTE_LENGTH 生效)。
- 验收:既有 cast 下推测试全绿(`test/sql/copy/csv/` 等);新增函数下推的冲突/替换测试。

### Phase B: seq_scan 注册 + StringSegment length-only 扫描(字节语义)

- `TableScanProjectionExpressionPushdown` 注册,接受 `strlen`/`octet_length`。
- `StringScanLengthPartial` 实现(offset 数组 + validity,输出 BIGINT)。
- 验收:原生表 `SELECT strlen(s) FROM t` 的 EXPLAIN 显示 GET 输出长度列;结果与朴素执行一致;null/overflow 字符串正确。

### Phase C: 码点语义(`length`/`char_length`)+ has_unicode 判定

- pass/回调接受 `length`/`char_length`(VARCHAR);scan 按 row group 统计判定 ASCII,回退正常扫描。
- 验收:多字节 UTF-8 数据(4 字节 emoji、截断边界)下结果正确;含 Unicode 分区回退、全 ASCII 分区 length-only。

### Phase D: 测试矩阵与基准

- 测试:过滤(`WHERE strlen(s) > N`)、GROUP BY 长度、`SUM`/`AVG`、排序/TopN、连接、与统计折叠组合(不精确统计兜底)。
- 基准:复用 `benchmark/max_len_skip/` 脚本,新增 length-only 场景(长度过滤、SUM、GROUP BY),对照行组扫描数与 wall time。

## Validation Strategy

- **正确性**:每个场景与 `disabled_optimizers` 关闭下对比;重点:null(长度输出 null 而非前值)、Unicode(码点 vs 字节)、overflow 大字符串、空字符串、`length(bit)`/`length(list)` 不误识别。
- **计划形态**:`EXPLAIN` 断言投影中 `strlen(s)` 消失、GET 输出 BIGINT 长度列;过滤场景断言 filter 引用长度列。
- **回归**:`make allunit`;测试置于 `test/sql/optimizer/` 与 `test/sql/function/string/`。
- **性能**:集中/分散分布数据(复用 lenbench 数据),对比 length-only 查询与基线的 wall time;长字符串列(1000B)与短列(30B)对照。

## Risks and Mitigations

1. **码点/字节语义混淆**(`length` 在含 Unicode 分区上被当字节长度) — `has_unicode == true` 强制回退;测试含多字节字符。
2. **null 行长度误输出**(null 存储为前值拷贝) — length-only 路径显式输出 validity;测试全 NULL 列与混合 NULL。
3. **TypePushdown 冲突分析误判**(列被混用时错误下推) — 复用现有冲突机制(混用 → `nullptr` → 不推);测试列同时用于 `strlen` 与内容匹配的查询。
4. **压缩列不支持** — dictionary/fsst 压缩列的 offset 语义不同,本 proposal 回退正常扫描;收益场景为 uncompressed 长字符串列,后续可扩展压缩列的长度缓存。
5. **block 布局限制 IO 收益** — 收益定位为 CPU/内存;如需 IO 收益需存储格式变更(独立 proposal,见 Non-Goals)。
6. **与统计折叠交互** — 两者独立触发路径;组合测试(统计不精确时的 length-only 兜底)验证不互相干扰。
7. **过滤表达式替换遗漏** — `WHERE strlen(s) > N` 的 filter 表达式需与投影同机制替换;测试覆盖过滤+投影组合。

## Open Questions

1. **parquet `length`/`char_length` 扩展** — 推荐:Phase C 后评估;parquet 的 `all_ascii` 判定需从列统计(parquet min/max 无 unicode 标志)或文件写入时记录,与原生表路径不同。

## Alternatives Considered

- **只做 CPU 层函数替换(现状)**:`LengthPropagateStats` 已把全 ASCII 列的 `length` 换成 `StrLenOperator`,但列仍全量读出;本 proposal 是同一思路向存储层的延伸。
- **存储格式变更(offset 独立成流)**:可省 block IO(接近 TiFlash 形态),但需要格式版本迁移与 checkpoint/压缩框架改动,风险高;本 proposal 先取 CPU/内存收益,格式变更为后续独立项。
- **生成列持久化长度**:需用户改 SQL,且与 offset 数组已有信息重复,不采用。
