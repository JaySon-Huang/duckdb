# max_len_skip 性能基准(服务器端)

在服务器上执行的性能基准,验证 `MIN/MAX(LENGTH-family)` 提前聚合与 row group
裁剪(proposals/max_len_skip_on_23611.zh.md Phase D 的合入门槛)。

## 服务器准备

```bash
git clone <repo> duckdb && cd duckdb
git checkout max-len-skip
make reldebug                      # 或 FORCE_DEBUG=1 make relassert
```

- 磁盘:数据文件 + WAL/checkpoint 临时空间,建议预留 ≥ 1.5× 数据量
- 所有数据库文件以 `storage_compatibility_version='latest'` 创建(`min_string_length`
  持久化的前提;默认 v0.10.2 兼容格式会在 checkpoint 后丢失该统计)

## 数据生成

```bash
benchmark/max_len_skip/generate_data.sh /data/lenbench.duckdb [large_rows] [multi_rows]
# 默认:large_rows=5e8(约 100GB),multi_rows=1e8(8 列,约 100GB)
```

4 张表,长度按 **row group 分块**(非按行混入):

| 表 | 布局 | 预期行为 |
| --- | --- | --- |
| `len_concentrated` | 90% RG 短串(10-50B)+ 10% RG 长串(900-1000B),纯 ASCII | MAX/MIN(strlen/char_length) 全精确 → **零扫描** |
| `len_unicode` | 90% RG 纯 ASCII(10-50B)+ 10% RG Unicode(`'你'*20-50`) | MAX(char_length) **只扫 ~10%**(上界裁剪);MIN(char_length) 零扫描(`ceil(bytes/4)` 下界裁剪) |
| `len_unicode_all` | 全部 RG 含 Unicode | 无精确候选极值 → 全扫,**与基线持平** |
| `len_multi` | 8 个字符串列(concentrated 布局) | issue 场景(`MAX(CHAR_LENGTH)` × 8)零扫描 |

> 注:90%/10% 分块边界通常不与 row group 边界对齐,会产生 1 个混合
> row group(同时含 ASCII 与 Unicode 行)。真实规模(数千 row group)下:
> MAX(char_length) 扫描 = unicode 块 row group 数 + 1(边界);MIN(char_length)
> 扫描 ≈ 仅 1 个边界 row group(纯 unicode row group 的下界 `ceil(60/4)=15 ≥ 10`
> 被裁剪)。小规模冒烟(如 3 个 row group)中边界 row group 占比较高,扫描比例
> 会偏离理论值,属预期。

生成完成后脚本打印每表的 row group 数与总行数,用于与扫描结果对照。

## 运行基准

```bash
benchmark/max_len_skip/run_benchmark.sh /data/lenbench.duckdb [runs=3]
```

- 两种模式:**baseline**(`SET disabled_optimizers='statistics_propagation'`,
  关闭整个统计传播 pass,含既有 count/min/max 优化——无更细粒度开关)与
  **optimized**(默认)
- 每查询每模式:wall time(3 次中位数)+ `EXPLAIN ANALYZE` 的
  `Row Groups Scanned: X / Y` + 结果值(两模式必须一致)
- 输出为管道分隔表格,可重定向保存:`... > result.csv`

## 门槛对照(设计文档 Validation Strategy)

| 门槛 | 命令 | 期望 |
| --- | --- | --- |
| (a) 全精确零扫描 | `max_strlen_concentrated` / `min_strlen_concentrated` / `max_charlen_concentrated` / `min_charlen_concentrated` | `Row Groups Scanned: 0 / N` |
| (b) 裁剪与理论一致 | `max_charlen_unicode` | 扫描 ≈ 10% 的 row group(与生成脚本打印的 `len_unicode` row group 数对照) |
| (c) 分散分布不劣于基线 | `max_charlen_unicode_all` | optimized wall time ≤ baseline |
| (d) 非目标无回归 | `sum_i` / `count_star` | optimized 与 baseline 差异在 ±5% 内 |

结果正确性:所有查询两模式返回值一致(脚本已输出)。

## 注意事项

- 避免与其他 IO 任务并发运行;重复运行取多次结果确认稳定
- `len_multi` 数据量是 8 倍单列,如磁盘紧张可降低 `multi_rows`
- 若服务器内存较小,`range(N)` 生成是流式的,峰值内存为单个 row group 大小,
  不受 N 影响
