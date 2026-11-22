# 遮挡查询结果的生命周期与采信规则（bgfx）

> 面向场景遮挡剔除开发者。结论基于本仓库源码（API 层 `src/bgfx_p.h`、`src/bgfx.cpp`，
> 后端 `src/renderer_*.cpp`），文中给出的 `文件:行号` 均可直接跳转核对。
> 公共语义见 `include/bgfx/bgfx.h:4048-4078` 与 `examples/26-occlusion/occlusion.cpp`。

---

## 1. 一句话结论

**只有 `bgfx::getResult()` 返回 `Visible` 或 `Invisible` 时，结果才描述一次已经被 GPU
执行完毕的查询；返回 `NoResult` 时必须按"未知"处理（剔除系统通常退化为"视为可见"）。**
结果永远滞后至少一个 `bgfx::frame()`：第 N 帧提交的查询，最早在第 N+1 帧 API 线程上才能读到，
且不同后端在 GPU 尚未完成时会继续返回 `NoResult`，滞后可能超过一帧。

---

## 2. 三个数值与三种判定：结果在内存里到底是什么

### 2.1 存放位置与哨兵值

每个 bgfx `Frame` 对象里有一个按**句柄索引**寻址的数组：

```cpp
int32_t m_occlusion[BGFX_CONFIG_MAX_OCCLUSION_QUERIES]; // bgfx_p.h:4576
```

- `Frame` 构造时整块填 `0xff`，即每个元素为 `-1`（`bgfx_p.h:4234`）。
- 创建查询时，API 层把当前提交帧的槽位写成 `INT32_MIN`（`bgfx_p.h:8758`）。
- 渲染线程解析到 GPU 结果后，写入实际像素/采样数（各后端 `resolve/flush/consumeResults`）。

### 2.2 `getResult` 的三分支判定（`bgfx_p.h:8768-8787`）

```cpp
switch (m_submit->m_occlusion[_handle.idx])
{
case 0:         return OcclusionQueryResult::Invisible;  // 0 个像素通过
case INT32_MIN: return OcclusionQueryResult::NoResult;   // 查询尚未产生结果
default:        /* 非 0、非 INT32_MIN */
                *_result = m_submit->m_occlusion[...];   // 可选输出像素数
                return OcclusionQueryResult::Visible;
}
```

| 槽位值 | 公共返回 | 含义 | 剔除决策可否采信 |
|---|---|---|---|
| `INT32_MIN` | `NoResult` | 查询还没被任何已完成的 GPU 工作解析 | 否，按未知处理 |
| `0` | `Invisible` | 查询区间内没有任何片段/采样通过深度测试 | 可剔除 |
| `>0`（默认分支） | `Visible` | 有片段通过；`_result` 带回通过的数量 | 可保留 |
| `-1`（`0xffffffff`，Frame 初值） | `Visible`（默认分支） | **不是真实结果**，只是"从没被写过"的初值 | 见 2.3，不可当作真实可见性 |

注意 `getResult` 读的是 **API 线程的 `m_submit` 帧**，加的是 `m_resourceApiLock`
（`bgfx_p.h:8770,8774`），与渲染线程之间靠帧交接时的整块内存拷贝同步，不存在逐查询的锁。

### 2.3 `-1` 初值的陷阱

`-1` 走 default 分支会被报成 `Visible`。正常流程下它不会暴露给用户：创建查询时槽位立即被
改成 `INT32_MIN`（`bgfx_p.h:8758`），而帧交接时会用旧提交帧的数组覆盖新提交帧
（见第 3 节）。但这意味着**"Visible"并不区分"真实测到像素"和"槽位从未被查询写过"**——
不要把一个从未提交过查询的句柄的 `Visible` 当成测量结论。判定结果归属时，必须结合
"该句柄上一次带查询的 `submit` 发生在哪一帧"（见第 6 节的软件记账办法）。

### 2.4 像素数的后端差异

- 桌面 GL（`GL_SAMPLES_PASSED`）、D3D11（`D3D11_QUERY_OCCLUSION`，`uint64` 截断为
  `int32`，`renderer_d3d11.cpp:6937-6944`）、D3D12、Vulkan（精确特性开启时）、Metal
  （`VisibilityResultModeCounting`）、WebGPU：返回通过的采样/像素**计数**。
- OpenGL ES 只有布尔查询 `GL_ANY_SAMPLES_PASSED`（`renderer_gl.cpp:8967-8971`），结果非 0
  即 1，**拿不到有意义的像素数**，只能用来区分可见/不可见。
- Vulkan 未支持 `occlusionQueryPrecise` 时，规范只保证 0/非 0 的布尔语义，计数可能被保守化。
- 计数统一经 `int32_t` 中转：超大计数可能截断，但只影响数值、不影响 0/非 0 的可见性判定。

---

## 3. 提交帧与渲染帧的交接：结果为什么天然滞后一帧

bgfx 维持两个 `Frame`：`m_submit`（API 线程填充）和 `m_render`（渲染线程消费），
`Context::frame()` → `frameNoRenderWait()` → `swap()`（`bgfx.cpp:3077-3079`）中的时序如下。

### 3.1 一次 `frame()` 内的严格顺序（`bgfx.cpp:3345-3465`）

```
Context::swap()
 ├─ resolveDynamicBuffers()
 ├─ freeDynamicBuffers()                       // bgfx.cpp:3356
 │    └─ m_occlusionQueryHandle.free(...)      // bgfx.cpp:2898-2902  ① 先回收句柄索引
 ├─ freeAllHandles(m_submit)                   // bgfx.cpp:3443
 ├─ m_submit->finish()
 ├─ bx::swap(m_render, m_submit)               // bgfx.cpp:3448        ② 交换双帧
 ├─ for 新建句柄: m_submit->m_occlusion[idx] = INT32_MIN  // :3450-3454 ③ 新句柄哨兵
 ├─ memCopy(m_render->m_occlusion, m_submit->m_occlusion, ...) // :3456 ④ 结果数组前滚
 └─ 单线程模式下直接 renderFrame()             // :3458-3462
```

### 3.2 第④步拷贝的方向与含义

交换之后，**新的 `m_render` 是 API 线程刚填满的那一帧**（带着本帧的 draw 和查询命令），
**新的 `m_submit` 是渲染线程刚处理完的那一帧**。拷贝 `m_render->m_occlusion ← m_submit->m_occlusion`
做两件事：

1. 把渲染线程已经解析出的旧结果前滚到新的提交帧，供下一帧 API 线程 `getResult` 读取；
2. 把整个结果数组作为"上一帧已知状态"的快照带给渲染帧——渲染线程的条件绘制判定
   （`isVisible`）读的也是 `m_render->m_occlusion`，即**渲染线程同样只能看到上一帧或更早的
   结果，永远看不到本帧正在记录的查询**。

### 3.3 渲染线程内部（`Context::renderFrame`，`bgfx.cpp:3555-3638`）

1. 执行前置资源命令（含 `InvalidateOcclusionQuery`，`bgfx.cpp:4628-4637`）；
2. `m_renderCtx->submit(...)` 遍历本帧 render item：
   - 帧首先做一次非阻塞 `resolve/flush`（GL `renderer_gl.cpp:10111-10114`、
     D3D11 `:7576`、Metal `:6876`、D3D12 `:10788`、Vulkan `:12482`），把已就绪的 GPU
     结果写进 `m_render->m_occlusion`；
   - 条件绘制每次判定前还会再尝试解析（见第 4 节）；
   - 带查询的 draw 包在 `begin/end` 之间，结果要等后续帧才可取。
3. 执行后置命令，`renderSemPost()` 放行 API 线程的下一次 `frame()`。

多线程模式下，API 线程在 `frame()` 开头 `renderSemWait()`（`bgfx.cpp:3035`）等待上一个渲染帧
完成，因此两个线程不会同时触碰同一个 `Frame`；`m_occlusion` 的跨线程传递只发生在交换与
整块拷贝两个点，这是无逐查询锁也能安全读取的原因。

### 3.4 对剔除开发者的时间线

```
帧 N API 线程:  submit(view, program, queryQ)   // 开始记录 Q 的查询
帧 N 末 frame(): 命令交给渲染线程
帧 N 渲染线程:  begin/end 查询，结果尚未就绪
帧 N+1 (或更后) 渲染线程: resolve 成功 → m_render->m_occlusion[Q] = 像素数
帧 N+1 API 线程: getResult(Q) 才可能得到 Visible/Invisible
帧 N+1 API 线程: setCondition(Q, true) 影响的是帧 N+1 的 draw，依据的是 ≤N 的旧结果
```

这正是公共头注释 "Retrieve occlusion query result **from previous frame**"
（`include/bgfx/bgfx.h:4057`）和官方示例的用法：同一帧内先 `setCondition` + `submit(query)`
再 `getResult`（`examples/26-occlusion/occlusion.cpp:248-268`），读到的永远是历史帧结论。

---

## 4. 条件绘制（setCondition）的判定链

### 4.1 两条独立路径，共用一个句柄

1. **记录路径**：`Encoder::submit(view, program, occlusionQuery, ...)`。句柄有效时置内部位
   `BGFX_STATE_INTERNAL_OCCLUSION_QUERY`（`bgfx.cpp:1650-1654`，标志定义 `bgfx_p.h:196`），
   渲染线程对该 draw 执行查询 `begin/end`，本身**不做**可见性跳过。
2. **判定路径**：`Encoder::setCondition(handle, visible)`（`bgfx_p.h:4889-4893`）把句柄挂到
   draw 上，并用 `BGFX_SUBMIT_INTERNAL_OCCLUSION_VISIBLE` 记录极性（true=可见才画）。

调试构建下，同一个查询句柄在一帧内只能用于一次记录式 `submit`
（`bgfx.cpp:1565-1573` 的断言）；条件式引用次数不受此限。

### 4.2 渲染线程的跳过条件（以 GL 为例，`renderer_gl.cpp:10427-10437`；D3D11/12、Vulkan、
Metal、WebGPU 同构）

```cpp
const bool hasOcclusionQuery = 0 != (draw.m_stateFlags & BGFX_STATE_INTERNAL_OCCLUSION_QUERY);
const bool occluded =
       isValid(draw.m_occlusionQuery)
    && !hasOcclusionQuery                                  // 记录查询的 draw 自己永不跳过
    && !isVisible(_render, draw.m_occlusionQuery, 极性);
if (occluded || 零面积裁剪) 跳过该 draw;
```

`isVisible`（GL `renderer_gl.cpp:4958-4962`，D3D11 `:3373-3377`，D3D12 `:4617-4621`，
MTL `:3752-3756`，WGPU `:3466-3469`，VK 见 `:5822-5825`）：

```cpp
m_occlusionQuery.resolve(_render);                       // 先尽力非阻塞解析
return _visible == (0 != _render->m_occlusion[handle.idx]);
```

### 4.3 `NoResult` 在条件绘制里的实际效果——**fail-open（保守可见）**

槽位为 `INT32_MIN` 时，`0 != INT32_MIN` 为 true：

- `setCondition(q, true)`（可见才画）：`true == true` → **照常绘制**；
- `setCondition(q, false)`（不可见才画）：`false == true` 不成立 → 不画。

即结果缺失时，正向剔除条件永远放行，不会因为查询没回来就错误地丢掉物体。这与 API 层
`getResult` 返回 `NoResult` 相互呼应：**两条路径都把"未知"当作"可见"处理**。场景剔除侧也应
遵守同一策略——拿到 `NoResult` 时不要剔除。

### 4.4 每帧两次解析机会

帧首统一 `resolve` 一次；每个条件 draw 判定前再 `resolve` 一次。两者都是非阻塞的，GPU 未
完成就 `break` 并保留 `NoResult`。因此同一帧内排在后面的条件 draw 比排在前面的更早受益于
新就绪的结果，但这只是调度细节，不改变"结果来自已完成帧"的语义。

---

## 5. 查询对象销毁、句柄复用与未返回结果的归属

### 5.1 句柄是裸索引，没有世代号

`bx::HandleAllocT::alloc/free` 只维护一个占用位图，返回值就是数组下标
（`bx/include/bx/inline/handlealloc.inl:40-70`）。`destroy` 之后下标可被立刻重新分配，
**句柄本身不携带任何"代"信息**。因此"旧物体的结果 vs 新物体的结果"这个问题，bgfx 完全依赖
下面的失效协议来回答，而不是靠句柄区分。

### 5.2 销毁与重建的完整时序

API 线程侧（均在 `m_resourceApiLock` 下）：

- `destroyOcclusionQuery(h)` 只把句柄放入延迟释放队列 `m_freeOcclusionQueryHandle`
  （`bgfx_p.h:8789-8796`），不立即失效。
- `createOcclusionQuery()`：
  1. `alloc()` 分配下标（可能复用刚释放的下标）；
  2. `m_submit->m_occlusion[idx] = INT32_MIN`（`bgfx_p.h:8758`）——API 侧槽位立刻变成
     "无结果"；
  3. 往命令缓冲写 `InvalidateOcclusionQuery`（`bgfx_p.h:8761-8762`）。
- 下一次 `frame()` 的 `freeDynamicBuffers()`（`bgfx.cpp:2898-2902`）才真正 `free` 旧下标。
  因为创建和销毁都在 API 线程、且 `frame()` 持同一把锁，**同帧"销毁 h 再创建得到 h'"时，
  `alloc()` 必然发生在 `free()` 之后不可能拿到同一索引**——同一帧内不会出现索引复用；复用
  最早发生在后续帧。
- 渲染线程在执行该帧前置命令时处理失效命令（`bgfx.cpp:4628-4637`），调用后端的
  `invalidate(handle)`。

### 5.3 后端 `invalidate` 如何摘除飞行中的结果

所有支持查询的后端做法一致：遍历环形查询池中"已提交未消费"的槽，凡是记录的句柄索引等于
被失效索引的，把该槽的句柄改成 `kInvalidHandle`：

- GL `renderer_gl.cpp:9036-9048`、D3D11 `:6951-6963`、D3D12 `:9819-9831`、
  Vulkan `:8092-8104`、Metal `:6422-6434`、WebGPU `:7461-7473`。

之后 GPU 结果即使回来，解析代码都有 `if (isValid(handle))` 守卫（如 GL `:9014`、
D3D11 `:6935`、D3D12 `:9810`、VK `:8083`、MTL `:6412`、WGPU `:7446`），**不会再写入
`m_occlusion`**。也就是说：

> **查询对象销毁时仍在飞行的结果，不归任何人——它被显式丢弃，绝不可能写到复用该索引的
> 新句柄上。**

### 5.4 新句柄对"旧结果残留"的防护有两层

1. 创建即写 `INT32_MIN`（API 数组，`bgfx_p.h:8758`），并在下一帧 `swap()` 后再次写
   `INT32_MIN`（`bgfx.cpp:3450-3454`，针对新提交帧）；
2. `InvalidateOcclusionQuery` 命令在渲染线程摘掉所有匹配的飞行槽，后端不会产生针对该索引
   的新写入。

但要注意第 3.2 节第④步的整块 `memCopy`：它**按索引前滚整个数组**，并不区分句柄是否已经
易主。考虑下面的序列：

```
帧 N:   物体 A 用索引 5 提交查询，GPU 结果在帧 N+2 才就绪
帧 N+1: 销毁 A 的查询（invalidate 已摘除飞行槽，A 的结果不会再被写入）
帧 N+2: createOcclusionQuery() 复用索引 5 给物体 B
```

- 正常情况下 A 的结果在 N+1 已被拦截，B 创建时槽位是 `INT32_MIN`，B 在自己的查询完成前
  一律读到 `NoResult`——归属清晰。
- 唯一能让索引 5 在 B 创建时仍带非哨兵值的路径，是 A 的结果**已在 A 存活期被解析进数组**
  （此时它本来就是 A 的合法结果），而 B 的创建写 `INT32_MIN` 会立刻清掉它。因此 B 不会读到
  A 的像素数。
- 反过来，如果 B 先创建并开始记录查询，而软件层仍持有 A 的陈旧句柄值来调用 `getResult`：
  由于句柄无世代号，`BGFX_CHECK_HANDLE` 只查占用位图，调用会**合法地命中 B 的槽位**，读到
  B 的 `NoResult`/结果。这不是 bgfx 能拦截的——**软件层绝不能在 `destroy` 之后继续使用旧
  句柄**，需要自行保证生命周期（见第 6 节）。

### 5.5 环形池耗尽时的强制回收（后端兜底，可能阻塞）

各后端为飞行查询维护容量 `BGFX_CONFIG_MAX_OCCLUSION_QUERIES`（默认 256，`config.h:519-521`）
的环形控制（`bx::RingBufferControl`，N 槽实际可用 N-1）。`begin` 预留不到空槽时，会强制
解析最旧的查询来腾位：

- GL/D3D11/Metal：`while (reserve==0) resolve(_render, true)`（GL `:8992-8995`、
  D3D11 `:6908-6911`、MTL `:6377-6380`），`_wait=true` 表示阻塞直到 GPU 出结果——
  **可能造成渲染线程停顿**；
- D3D12：在 `begin` 内同步读回最旧槽（`:9762-9770`），同样是强制回收；
- Vulkan：`reserve` 后在帧回收点 `reset()` 里按 frame-in-flight 取结果（`:8036-8071`），
  每帧延迟有独立 query pool；
- WebGPU：池满时 `reserve` 失败则本次 `begin/end` 直接跳过（`:7348-7365`），该次查询静默
  丢失，槽位保持 `NoResult`。

实践含义：**同帧在飞行中的查询数量应显著低于 256**，否则桌面后端可能出现 GPU/CPU 同步
卡顿，WebGPU 还会丢查询。

---

## 6. 不依赖图形设备判断"结果归谁"的软件办法

句柄无世代号、结果数组只按索引寻址，所以归属判定必须在场景层自己记账。以下办法全部只需要
CPU 侧数据结构，不碰任何 GPU 调用。

### 6.1 给每个查询槽绑定"代 + 帧号 + 物体 ID"

封装一层查询对象，而不是直接裸用 bgfx 句柄：

```cpp
struct OcclusionToken {
    bgfx::OcclusionQueryHandle handle = BGFX_INVALID_HANDLE;
    uint32_t generation = 0;   // 每次（重新）创建递增
    uint32_t submitFrame = 0;  // 调用带查询 submit 的 frame() 序号
    uint32_t objectId = 0;     // 该代查询服务的物体
};
```

- `create`/复用时 `++generation`，记录 `bgfx::frame()` 返回的帧号到 `submitFrame`；
- `destroy` 时把 token 标记为失效并从活动表摘除；即使 bgfx 稍后把同一索引分给别的物体，
  旧 token 的 `generation` 也对不上，自行丢弃即可；
- 读结果时校验 `generation` 与 `objectId` 未变，再调用 `bgfx::getResult`。

### 6.2 用帧号判定结果是否"够新"，无需问设备

维护最近一次 `bgfx::frame()` 的计数 `F`。对任何 token：

- 若 `F < token.submitFrame + 1`：结果在时间上不可能就绪，直接按 `NoResult` 处理，连
  `getResult` 都不用调；
- 若 `F >= submitFrame + 1` 且 `getResult` 仍返回 `NoResult`：说明 GPU 还没跑完（后端非阻塞
  解析未就绪），继续保守可见，下一帧再试；
- 一旦拿到 `Visible/Invisible`，连同当时的 `F` 一起缓存。该结论描述的是
  `submitFrame` 帧的场景状态，剔除决策时要意识到它的时效性（相机/物体快速移动时可配合
  "最后已知可见帧龄"做滞后容忍或强制可见）。

### 6.3 销毁即摘除、重建即换 token 的纪律

- 不要在物体之间"借用"查询句柄；物体与 token 一一对应，物体销毁 → token 失效 →
  `bgfx::destroy`，新物体 → 新 token（新 generation）。
- 这样即使发生第 5.4 节的索引复用，两代数据在软件表里就是两条记录，归属不可能混淆。
- 预算控制：活动 token 数远低于 `BGFX_CONFIG_MAX_OCCLUSION_QUERIES`（256），避免环形池
  强制回收带来的卡顿或 WebGPU 丢查询。

### 6.4 判定真值表（设备无关的决策层）

| 条件 | 设备无关的结论 | 剔除动作 |
|---|---|---|
| token 已失效 / generation 不符 | 结果不属于当前物体 | 不使用 |
| `F < submitFrame + 1` | 时间上未到可读帧 | 视为可见（fail-open） |
| `getResult == NoResult` | GPU 未完成 | 视为可见 |
| `getResult == Invisible`（0 像素） | 提交帧确无片段通过深度测试 | 可剔除（结论滞后 ≥1 帧） |
| `getResult == Visible`（>0） | 提交帧有片段通过 | 保留；ES/非精确 Vulkan 下只有布尔意义 |

---

## 7. 各后端可见性保证与适用条件

| 后端 | 查询机制（源码位置） | 就绪判定 | 结果语义 | 阻塞/退化行为 | 适用条件与限制 |
|---|---|---|---|---|---|
| **OpenGL 桌面** | `GL_SAMPLES_PASSED`，环形 query id 池 `renderer_gl.cpp:8967-9048` | `GL_QUERY_RESULT_AVAILABLE` 轮询，未就绪 break（`:9018-9026`） | 采样计数；0=不可见 | 池满 `resolve(_,true)` 强制等待 | 需要 `glGenQueries/glBeginQuery/...` 全部函数指针非空（`:3095-3101`），否则 `m_occlusionQuerySupport=false`，查询不创建（`:3271-3273`） |
| **OpenGL ES** | `GL_ANY_SAMPLES_PASSED` 布尔查询（`:8968-8969`） | 同上 | 仅 0/1，**无像素数** | 同上 | ES 扩展/版本支持布尔 occlusion query |
| **D3D11** | `D3D11_QUERY_OCCLUSION`，环形 ID3D11Query 池 `:6883-6963` | `GetData` 返回 `S_FALSE` 即未就绪（`:6938-6942`）；非等待用 `D3D11_ASYNC_GETDATA_DONOTFLUSH` | `uint64` 采样数截断为 int32 | 池满强制 `GetData` 等待 | 设备丢失/reset 时查询经 `preReset/postReset` 重建（`:6897-6904, 2384-2400`），reset 期间在飞行的结果不保证可用 |
| **D3D12** | 单一 query heap + readback 常驻映射，查询索引直接用 `handle.idx`，fence 跟踪 `:9729-9831` | `m_fence[idx] <= completedFence` 才读（`:9804-9807`） | `uint64` 截断 int32 | `begin` 池满时同步驱逐最旧槽（`:9762-9770`） | 结果可用性由 command queue fence 严格保证；同一索引在 fence 完成前不会被新查询覆盖 |
| **Vulkan** | 每个 frame-in-flight 一个 `VkQueryPool`（`:7977-7990`），环形句柄表 + `m_ready` 标记 `:8007-8104` | 帧回收时 `vkGetQueryPoolResults`（flags=0，该帧已完成才调用，`:8062`），`flush` 只消费 ready 槽（`:8077-8078`） | 支持 `occlusionQueryPrecise` 时为精确计数（带 `VK_QUERY_CONTROL_PRECISE_BIT`，`:8021`），否则仅 0/非 0 保证 | 池满依赖 frame-in-flight 回收；查询池随延迟帧数创建 | 依赖 `m_maxFrameLatency` 个池隔离飞行帧；`reset` 在该帧命令缓冲重放时执行（`:11639`） |
| **Metal** | visibility result buffer（每查询 8 字节，偏移 `handle.idx*8`），`VisibilityResultModeCounting` `:6365-6434` | 比较 `m_completedFrames` 与记录时的 `m_submittedFrames`（`:6399-6410`），未完成且非等待则 break | `uint64` 计数截断 int32 | 池满 `resolve(_,true)` 等待对应帧完成 | buffer 在 reset 时经 `preReset/postReset` 重建（`:6365-6373`）；查询依赖 render command encoder 的 visibility 模式，encoder 切换会结束查询区间 |
| **WebGPU** | query set + resolve buffer + map-read buffer，异步 map 回调 `:7292-7473` | `mapAsync` 回调 `consumeResults` 里写帧数组（`:7431-7459`）；未 map 完一直 `NoResult` | `uint64` 截断 int32（规范 occlusion 为 0/非 0 语义） | **池满时本次 begin/end 静默跳过**（`:7348-7365`），查询丢失且无报错 | 结果延迟可能多帧（异步 map）；不得依赖每帧必出结果 |
| **Noop / 无查询能力的 GL 配置** | `invalidateOcclusionQuery` 空实现（`renderer_noop.cpp:205-207`）；GL 不支持时不建池、不 resolve | 数组永不被后端写入 | 槽位停留在 `INT32_MIN` → 恒 `NoResult`（GL 帧首 resolve 被 support 标志跳过，`:10111`） | 无 | 条件绘制恒 fail-open 放行；剔除系统必须能容忍"永远 NoResult" |
| **AGC / GNM / NVN（主机后端）** | 本仓库这些文件中无任何 occlusion 相关代码（grep 计数为 0） | — | — | — | **视为不支持**，按 Noop 同款策略处理 |

### 7.1 跨后端的共同保证（可以写进引擎契约的部分）

1. **已返回的 `Visible/Invisible` 一定对应一次已完成 GPU 工作的真实查询**，且因为
   `invalidate` 摘除 + 创建哨兵，不会属于另一个复用了同索引的对象——前提是软件层不使用
   已销毁的旧句柄。
2. **`NoResult` 与条件绘制 fail-open 是全后端一致行为**：未知即保守可见，不会错误剔除。
3. **结果至少滞后一个 `frame()`**；WebGPU（异步 map）、GPU 高负载时的 D3D11/GL（非阻塞轮询
   未就绪）可能滞后多帧。
4. **0 像素判定（`Invisible`）在所有支持后端都是可靠的剔除信号**；非 0 的具体数值只在
   桌面 GL/D3D11/D3D12/精确 Vulkan/Metal 上有计数意义，ES、非精确 Vulkan、WebGPU 只有布尔意义。
5. 设备 reset（D3D11、Metal）期间飞行结果不保证，恢复后需要重新积累——软件上表现为再次
   收到一段时间的 `NoResult`。

---

## 8. 给场景剔除实现的落地清单

1. 用第 6.1 节的 token 表（generation + submitFrame + objectId）包住 bgfx 句柄，禁止裸句柄
   跨物体复用、禁止销毁后引用。
2. 每帧顺序：先 `getResult` 读上一代结论做剔除决策 → 再对存活物体 `setCondition` +
   带查询 `submit` → 最后 `bgfx::frame()`；与 `examples/26-occlusion` 保持同一节奏。
3. 决策函数严格三分支：`NoResult`/帧龄不足 → 可见；`Invisible` → 可剔除；`Visible` → 保留。
   不要把 `-1` 初值或"从未提交过查询"的句柄当成测量结论。
4. 活动查询数控制在远低于 256，避免环形池强制同步（桌面）或静默丢查询（WebGPU）。
5. 对结论滞后做防护：被剔除物体保留宽限期（例如连续 2 次 `Invisible` 才真正剔除，或相机
   剧烈移动时强制可见），避免上一帧滞后结果误杀当前帧物体。
6. 引擎初始化与后端能力探测失败时（GL 无扩展、主机后端、noop），整条遮挡剔除路径要能退化为
   "全可见"，因为这些环境下 `getResult` 永远是 `NoResult`。
