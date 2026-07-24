# Rayforce vs DuckDB 性能分析

> 分析基于 DuckDB 源码（`/home/kindred/mywork/projects/cpp/duckdb`）与 Rayforce 源码（当前目录）的对比。
> 核心观察：所有 SQL 查询都比 DuckDB 慢很多，不仅是字符串比较类查询。

---

## 1. 执行粒度：向量化批处理 vs 逐行原子求值（最核心差异）

### DuckDB

数据以 `DataChunk`（`STANDARD_VECTOR_SIZE = 2048` 行）为单位在 pipeline 中流动。每个 operator 一次处理整个 chunk：

```cpp
// physical_filter.cpp — PhysicalFilter::ExecuteInternal
OperatorResultType PhysicalFilter::ExecuteInternal(
    ExecutionContext &context, DataChunk &input, DataChunk &chunk, ...) const
{
    auto &state = state_p.Cast<FilterState>();
    // 一次调用评估整个 chunk（2048 行）的过滤条件
    idx_t result_count = state.executor.SelectExpression(input, state.sel);
    if (result_count == input.size()) {
        chunk.Reference(input);
    } else if (result_count > 0) {
        chunk.Slice(input, state.sel, result_count);
    }
    return OperatorResultType::NEED_MORE_INPUT;
}
```

`ExpressionExecutor::SelectExpression` 内部对整列做 SIMD 友好的循环，编译器可以自动向量化。比较运算符通过 `BinaryExecutor::Select<T, T, OP>` 实现，一次处理一整个向量。

### Rayforce

过滤操作通过 `ray_rowsel_from_pred`（`rowsel.c:131`）实现，**逐行求值**：

```c
// exec.c ~line 2493
ray_t* new_sel = ray_rowsel_from_pred(pred);
```

`ray_rowsel_from_pred` 内部对每一行：
1. 调用 `ray_table_get_col` 提取列值 → 产生一个 `ray_t*` 原子
2. 对每个原子调用谓词函数（如 `ray_eq_fn`）
3. 根据结果设置 SelectionVector 位

**关键差异**：

| | DuckDB | Rayforce |
|---|---|---|
| 执行单元 | DataChunk（2048 行） | 单个原子（ray_t*） |
| 过滤方式 | 1 次 SelectExpressionvectorized 调用 | 2048 次 rowsel 循环 |
| 列访问 | 列向量直接连续内存 | 每行 `ray_table_get_col` + 原子解引用 |
| 比较操作 | `BinaryExecutor::Select` 向量化 | `ray_eq_fn` 逐 atom 调用 |

**这是差距的最大来源**。对于 `WHERE addr_str = '...'`：
- DuckDB：1 次向量化比较处理 2048 行
- Rayforce：2048 次 `ray_eq_fn` → `char_str_cmp` → `ray_str_ptr` → `memcmp`

---

## 2. 并行执行：Morsel-Driven vs 串行 DAG

### DuckDB

`pipeline_executor.cpp` 实现 Morsel-Driven 并行：

```
FetchFromSource(chunk)    ← 多个线程并行读取 CSV 的不同分片
Execute(input, output)    ← 每个线程独立推送数据经过 pipeline
Sink(output, sink_input)  ← 每个线程独立的 local_sink_state
```

关键机制：
- CSV 文件被分成多个 `ScannerBoundary`（`global_csv_state.cpp`）
- 每个 boundary 由独立的 `StringValueScanner` 实例处理
- Pipeline operator 状态是线程局部的（`local_sink_state`, `local_source_state`）
- 通过 `ExecutePushInternal` 循环持续从 source 拉取、经过 operator、送入 sink

```cpp
// pipeline_executor.cpp ~line 280-330
while (!exhausted_pipeline && !done_flushing) {
    source_chunk.Reset();
    source_result = FetchFromSource(source_chunk);
    // ...
    result = ExecutePushInternal(source_chunk, chunk_budget);
}
```

### Rayforce

DAG 执行基本是串行的：

```c
// exec.c ~line 3677
ray_t* ray_execute(ray_graph_t* g, ray_op_t* root) {
    return ray_execute_inner(g, root);
}
```

`ray_execute_inner` 单线程遍历整个 DAG 的 operator 树。CSV 读入阶段确实有并行（`ray_pool_dispatch`），但 DAG 执行阶段**不具备内建的并行能力**。

对于大文件，全部数据读完后再在 DAG 里串行处理，远不如 DuckDB 的"边读边并行处理"。

---

## 3. 数据访问模式：列向量直接访问 vs 逐行原子提取

### DuckDB

列数据以 `Vector` 形式连续存储。filter 操作直接访问列的连续内存：

```cpp
// PhysicalFilter 的 SelectExpression 内部
// 列数据以连续指针 + 步长访问
Vector& col = input.data[col_idx];
auto* col_data = FlatVector::GetData<T>(col);
// 对 col_data[0..size-1] 做向量化操作
```

### Rayforce

对每一行提取独立原子：

```c
// rowsel.c 内部
for (int64_t i = 0; i < nrows; i++) {
    ray_t* cell = ray_table_get_col(tbl, col_idx);
    // cell 是一个 ray_t* 原子，需要解引用 type 等字段
    ray_t* result = ray_eq_fn(cell, literal); // 或 ray_lt_fn 等
    // 检查 result，设置 selection 位
}
```

`ray_table_get_col` 内部还要处理：
- slice parent 重定向
- 类型转换
- 符号域解析

这每行都要做一次。

---

## 4. Filter Pushdown：扫描时过滤 vs 全量加载后过滤

### DuckDB

优化器把 WHERE 条件推入数据扫描阶段（`pushdown_get.cpp`）：

```cpp
// pushdown_get.cpp
// WHERE 条件 → ConstantFilter / ColumnComparisonFilter
get.table_filters = combiner.GenerateTableScanFilters(column_ids, pushdown_results);
```

CSV 扫描器 `TableFilter` 在解析时直接跳过不匹配的行：

```
CSV 行 → 状态机解析 → 类型转换 → filter 检查 → 通过则输出到 Chunk
                                          ↓ 不通过 → 跳过
```

### Rayforce

所有行先完整解析（CSV read）→ 类型转换 → 符号表插入 → 内存中构建完整表格 → DAG 执行阶段才应用 WHERE 过滤。

**每行都经过完整的数据链路，无论它是否会被过滤掉**。对于高过滤率的查询（如 `WHERE id < 3`），这是巨大的浪费。

---

## 5. SYM 列的比较路径

对于 `addr_str = 'The 4291, streat 1772277249'`（`addr_str` 是 RAY_SYM 列）：

### DuckDB

列值是 `string_t` 结构体（直接存储或前缀+指针）。比较通过 `BinaryExecutor::Select<string_t, string_t, Equals>` 向量化完成：
- 加载前 8 字节为 uint64 → 快速拒绝
- 需要时才做 memcmp

### Rayforce

列值是 SYM id（int64）+ 符号域指针。逐行比较的路径：

```c
// ray_eq_fn(a, b) 对每行调用
// a 是 SYM atom, b 是字面量 STR atom
char_str_cmp(a, b, &c)     ← SYM 不是 STR → 跳过
a->type == -RAY_SYM && 
b->type == -RAY_SYM         ← b 是 STR → 跳过
// 如果没有特殊路径 → 落到 error
```

由于查询实际能工作，说明 DAG 编译阶段**转换了比较路径**（可能是把字面量预注册到符号域，统一为 SYM-vs-SYM 比较），这个转换本身有额外开销。

---

## 优化建议（按优先级排列）

| 优先级 | 优化方向 | 预期提升 | 说明 |
|--------|----------|----------|------|
| ★★★★★ | **批量化谓词求值** | **10x-100x** | `ray_rowsel_from_pred` 从逐行改为按列批量求值。一列数据一次处理，避免每行 `ray_table_get_col` + 函数调用 |
| ★★★★★ | **DAG 并行执行** | **2x-8x** | `ray_execute_inner` 支持多线程并行执行不同数据分区 |
| ★★★★ | **Filter Pushdown** | **2x-10x** | 在 CSV 解析阶段应用 WHERE 过滤，跳过不匹配行的解析和类型转换 |
| ★★★ | **消除 SYM-vs-STR 转换开销** | **1.2x-2x** | DAG 编译阶段预先把字面量注册到符号域，避免逐行转换 |
| ★★ | **向量化原子访问** | **1.5x-3x** | `ray_table_get_col` 对批量行返回连续值数组，而非逐行返回单原子 |

### 第一优先级详细说明：批量化谓词求值

当前 `ray_rowsel_from_pred` 的伪代码：

```c
for i in 0..nrows:
    pred_atoms[0] = ray_table_get_col(table, lhs_col, i)  // 提取原子
    pred_atoms[1] = rhs_literal_atom
    result = eval_pred(pred_atoms)                        // 逐原子调用
    sel_bitmap[i] = result
```

改为批量化的伪代码：

```c
// 步骤 1：提取整列数据（一列就是一块连续内存）
void* col_data = ray_table_col_data(table, lhs_col);
int8_t col_type = ray_table_col_type(table, lhs_col);

// 步骤 2：批量求值（根据类型分发到不同函数）
if (col_type == RAY_I64) {
    int64_t* vals = (int64_t*)col_data;
    for i in 0..nrows:
        sel_bitmap[i] = (vals[i] < 10);
} else if (col_type == RAY_SYM || col_type == RAY_STR) {
    // 对字符串列专用批量比较路径
    str_batch_equals(col_data, col_type, nrows, literal_str, sel_bitmap);
}
```

核心思路：**对同一列的所有行，一次获取数据指针，然后在紧凑循环里做比较**，避免每行都经过 `ray_table_get_col` → 产生 atom → 函数派发的链路。

目前字符串比较的优化（`char_str_cmp`、`ray_str_cmp`）本身没什么问题，只是被执行链路中更上层的开销淹没了——你把 memcmp 从 100ns 优化到 10ns，但每次调用的函数派发 + atom 提取 + 类型分派花了 500ns，整体收益只有不到 2%。

真正的加速要从**减少每行的函数调用次数**入手。
