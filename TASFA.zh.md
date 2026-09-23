# TASFA

TASFA 是本项目用于文件上传和下载的传输协议。

它构建在普通 HTTP/XHR 之上，增加了分块加密、文件级顺序队列，以及一个**服务器权威的 6 槽 HTP（Hexagonal Tortoise Problem）恢复格子（recovery lattice）**用于完整性控制。

## 路由

上传:
- `POST /file/upload/init`
- `POST /file/upload/status`
- `POST /file/upload/renegotiate`
- `POST /file/upload`
- `POST /file/upload/complete`
- `POST /file/upload/cancel`

下载:
- `GET /file/download/:id/handshake`
- `GET /file/download/:id/chunk/:chunk_index`
- `GET /file/download/:id`
- `GET /assets/tasfa/img/:filename/handshake`
- `GET /assets/tasfa/img/:filename/chunk/:chunk_index`
- `GET /assets/tasfa/uploads/:filename/handshake`
- `GET /assets/tasfa/uploads/:filename/chunk/:chunk_index`

## 上传协议

浏览器先协商上传会话，然后发送带有 TASFA 标头的**分块**（默认 `16 MiB`，移动端 `8 MiB`）：

- `X-TASFA-Upload-ID`
- `X-TASFA-Upload-Token`
- `X-TASFA-Chunk-Index`
- `X-TASFA-Hash-Tag`
- `X-TASFA-Raw-Scalar`
- `X-TASFA-Magic-Scalar`

服务器将每个分块直接写入预分配临时文件的 `chunk_index * chunk_size` 偏移处。不再有传输块。

如果普通分块反复失败，浏览器会通过带有 `X-TASFA-Stream-Mode: aes-256-gcm` 的回退请求发送该失败分块。服务器接受密文加上 GCM 认证标签，且回退分块保留在自适应并行窗口内。回退请求仍然携带相同的 HTP 哈希标签和平衡标量标头。

### XOR 奇偶校验分块

上传会话为**每最多 6 个数据分块组成的组**额外携带一个奇偶校验分块：`chunk_count = data_chunks + ceil(data_chunks / 6)`。奇偶校验分块保存其组的 `P = C_0 xor ... xor C_n`，并单独存储在会话目录中的 `parity_<k>.bin`，而不是 `chunk_index * chunk_size` 偏移处。奇偶校验分块计入已接收分块位图，但排除在 HTP 线验证之外；它们供下文所述的服务器端剥除恢复使用。

### 余数（最后部分分块）

如果文件大小不是分块大小的倍数，最后一个分块是**余数**。它以精确字节范围作为单个 blob 发送；服务器将其写入正确偏移。不执行填充或拆分。

### 会话初始化响应

`init` 端点返回包含以下字段的响应：

- `chunk_size` — 协商后的分块大小
- `current_parallel_chunks` — 服务器批准的当前上传窗口
- `max_parallel_chunks` — 客户端可同时上传的分块数量
- `dispatch_pacing_ms` — 仅在测量到的链路较差时应用的小发送间隔
- `upload_secret` — 用于加密流验证的辅助服务器密钥
- `stream_key_hex`, `stream_iv_seed_hex`, `stream_mode` — 会话加密密钥 (`aes-256-gcm`)
- `modulus_M` — 本会话使用的 HTP 模数
- `group_count` — 完整 6 槽 HTP 组的数量
- `client_stripes` — 客户端工作调度器使用的固定值 `32`

客户端将 `chunk_size` 视为会话契约，因为它定义了每个分块的文件偏移。在活跃上传期间，它调整并发和重试行为，同时学习到的分块大小提示将应用于下一个 TASFA 会话：在持续快速连续完成（3 个干净分块，EWMA `>= 8 Mbps` 或单个分块时间低于 30 秒）后，下一个提示翻倍；在失败后降低四分之一（`* 0.75`，按 `1 MiB` 步长取整，`8 MiB` 下限）。

### 上传分块响应

当分块被接受时，服务器返回 `204 No Content` 和以下标头：

- `X-TASFA-Accepted: 1`
- `X-TASFA-Chunk-Complete: 1`

如果分块索引已在 `state.bin` 中标记为完成，**并且**该索引不在服务器的 `retry_targets` 中，则服务器返回相同的 `204` 标头，但分块正文将被丢弃。

## HTP 服务器权威恢复格子

HTP **不是传输协议，也不是密码学证明**。它是一个服务器端分块嫌疑引擎，按损坏可能性对分块进行排序，使客户端只需重传高概率嫌疑分块，而非整个文件。

### 分块分组

分块被分组为连续的 6 元素顶点：

```
组 g: [ v0 , v1 , v2 , v3 , v4 , v5 ]
       chunk g*6+0  ...  g*6+5
```

最后一组可能是部分的；**不完整组绝不补零，且完全排除在 HTP 验证之外**。零填充会注入人造拓扑，因此被禁止。

### 原始标量

对于每个分块，客户端计算明文分块的 SHA-512，取前 8 字节作为大端无符号 64 位整数 `H`，并导出：

```
raw_scalar = H % modulus_M
```

### 魔法线不变式

对于完整组，定义三条线：

```
L1 = v0 + v1 + v2   (mod M)
L2 = v2 + v3 + v4   (mod M)
L3 = v4 + v5 + v0   (mod M)
```

不变式要求 `L1 == L2 == L3`。如果原始标量不满足此条件，客户端通过**仅调整 `v3` 和 `v5`** 来平衡：

```
delta2 = (L1 - L2) mod M
delta3 = (L1 - L3) mod M

v3_balanced = (v3_raw + delta2) mod M
v5_balanced = (v5_raw + delta3) mod M
```

其余所有顶点保持其原始标量。客户端同时发送两者:

- `X-TASFA-Raw-Scalar` — 未修改的 `raw_scalar`
- `X-TASFA-Magic-Scalar` — 平衡值 (`v3_balanced` 或 `v5_balanced`，其余与 raw 相同)

服务器将两个标量分别存储在 `htp.bin` 中，从而可以在不受人工平衡约束影响的情况下分析原始拓扑。

### 为什么只有 v3 和 v5？

六边形格子有两个自由度。固定 `v0,v1,v2,v4` 并调整 `v3,v5`，可在保持最小增量且局限于组内的同时唯一满足三条线方程。

## 服务器权威 HTP 恢复

**客户端是愚笨的重传代理（dumb retransmission agent）。** 客户端不计算修复代数，不评估成本阈值，也不导出嫌疑排序。所有这些都仅存于服务器端。

### 服务器验证流程

在 `POST /file/upload/complete` 期间，服务器执行以下操作：

1. 从 `htp.bin` 加载所有分块级记录（哈希标签、原始标量和平衡标量）。
2. 仅验证**完整的 6 槽组**（跳过部分组）。
3. 若 6 槽组内标量顺序发生置换（out-of-order）导致不变式破损，服务器遍历 720 种全排列，找到能恢复不变式的排列后重新排列 `htp.bin` 记录进行修复，无需重传即通过。
4. 对每个失败组，通过分析每个槽位参与的线方程来计算**嫌疑分数（suspicion scores）**。

### 嫌疑置信度评分（每组）

对于失败组，服务器将每个槽位与三条线方程进行比较评估：

| 槽位 | 方程 |
|------|------|
| v0   | L1, L3 |
| v1   | L1     |
| v2   | L1, L2 |
| v3   | L2     |
| v4   | L2, L3 |
| v5   | L3     |

每个槽位的嫌疑分数由拓扑综合征（syndrome）分析导出：
- 当恰好两条线一致而另一条不同时（$L_a == L_b \neq L_c$），失败的方程分离出两个候选：$L_c$ 上的独占侧槽位（分数 `1.0`，主导综合征）以及被 $L_a, L_b$ 共享的对角槽位（分数 `0.5`）。
- 其余 4 个不触及失败方程的槽位立即被**清除**（分数 `0.0`）。
- 当三条线全部不同（$L_1 \neq L_2 \neq L_3$）时，分数与归一化残差幅值 $|L_i - L_j|$ 中的参与度成比例。

分数在所有失败组间聚合；若一个分块出现在多组中，保留其最高分。

### 完整 HTP-XOR 剥除解码器（Peeling Decoder）

为了在不进行昂贵的伽罗瓦域 $GF(2^8)$ 矩阵求逆的情况下解决分块损坏或丢失问题，TASFA 将拓扑综合征定位与迭代剥除式 XOR 解码器相结合：
- **XOR 奇偶校验生成**：每最多 6 个数据分块组成的组都有一个随会话上传的奇偶校验分块 $P = C_0 \oplus C_1 \oplus C_2 \oplus C_3 \oplus C_4 \oplus C_5$。
- **零成本嫌疑定位**：当某个组完整性校验失败时，HTP 的线和能以 $O(1)$ 整数运算定位单个损坏分块索引 $i$。
- **有界迭代剥除恢复**：服务器使用 SIMD 位元 XOR 重构 $C_i = P \oplus \bigoplus_{j \neq i} C_j$。当某组恰好只有一个嫌疑，或有两个嫌疑且最高分为 `1.0`（主导单错误综合征）时，该组即被恢复；已恢复组的所有嫌疑随即被清除。扫描重复进行直到没有组取得进展，最多 6 遍。
- **缺失分块重构**：独立于嫌疑分析，如果完成位图显示某组恰好缺失一个数据分块且该组的奇偶校验分块已接收，服务器会在验证 HTP 线之前以同样方式重构缺失分块。
- **多重丢失回退**：若同一组中有两个或更多独立分块无法被隔离，服务器将回退到通过 `retry_targets` 进行的 ARQ 重传。

### 修复成本阈值

在请求任何修复之前，服务器评估收缩是否比直接重传更便宜：

```
repair_worthwhile(嫌疑数, 总分块数, 分块大小, rtt_ms):
    if 嫌疑数 < 12               → false  （太少，拓扑无意义）
    retry_cost  = 嫌疑数 * 分块大小 * rtt_factor(rtt_ms)
    repair_cost = 元数据字节 + 服务器CPU开销 + 额外RTT开销
    return retry_cost > repair_cost
```

抽象成本模型比较客户端需重传的字节数与服务器端分析开销。大分块或高延迟使收缩更有吸引力，而大量小嫌疑分块使直接重传更便宜。具体数值是服务器端实现细节，不是协议常量。

若阈值拒绝修复，服务器返回 `needs_retry`，并将**所有**嫌疑分块作为重传目标。客户端通过普通上传端点重传它们。

### 服务器端递归收缩

若修复值得，服务器执行 **组级收缩**: 每个原始完整的 6 槽组被收缩为编码其 **不变式签名** 的单个标量。服务器计算该组的三条线索值 `L1, L2, L3`，推导残差 `r12 = (L1-L2) mod M` 和 `r23 = (L2-L3) mod M`，并将组聚合值设为 `(r12 * r23) mod M`。通过组的 `r12 = r23 = 0`，故聚合值为 `0`；失败组则获得一个保留线不一致拓扑的非零确定性签名。这些组聚合值成为更高级别 HTP 格的顶点。连续的 6 个组聚合值形成 level-1 超级组，并重新评估相同的线不变式：

- 若 level-1 超级组通过，则清除其底层 level-0 组中的嫌疑分块（失败模式在组级一致）。
- 若 level-1 超级组失败，则保留其底层 level-0 组中的嫌疑分块。
- 若收缩缩小了嫌疑集（更少分块），服务器存储缩小后的目标并返回 `needs_retry` 及缩减列表。
- 若收缩未能缩小集合，服务器回退到原始嫌疑分块的直接重传。
- 收缩级别在会话元数据中递增，以便客户端报告诊断信息。

客户端从不看见或计算收缩组。它只接收 `retry_targets`。

### 重传接受

当客户端重传已标记为接收的分块时，普通上传端点**仅当该分块索引当前在服务器的 `retry_targets` 列表中时才接受重传**。重传分块存储后，服务器将其从 `retry_targets` 中移除。

### 协议可见修复响应

当 HTP 失败且服务器决定修复或重传时，`complete` 端点返回 `409` 并附带：

- `htp_status`: `"needs_retry"`
- `retry_targets`: 待重传的分块索引数组（按嫌疑分数降序排列）
- `suspicion_scores`: `{chunk_index, score}` 对象数组
- `contraction_level`: 已应用的服务器端收缩遍数
- `retry_reason`: 人类可读的说明（例如 `"htp group inconsistency detected"`）

若成本阈值判定直接重传更便宜，`retry_targets` 包含完整嫌疑列表且 `contraction_level` 保持为 `0`。

若服务器通过收缩成功缩小嫌疑范围，`retry_targets` 包含缩减列表且 `contraction_level` 递增。

所有嫌疑分块重传并成功验证后，下一次 `complete` 调用继续 SHA-256 最终化。

## 文件级顺序队列

**一次仅上传一个文件。** 选择多个文件时：

1. 每个文件获得自己的资源、预览卡和 HTP 会话。
2. 文件进入 `FileUploadQueue` 排队。
3. 当前文件完成（成功或失败）后，队列自动前进到下一个文件。
4. 批量“上传排队文件”按钮始终启用；点击后将所有待处理文件入队并启动泵。

这防止浏览器连接池耗尽，并保持停滞检测可靠。

## 运行时设置

- 上传分块大小：桌面 `16 MiB`，移动端 `8 MiB`
- 自适应上传分块大小提示：服务器协商最小 `8 MiB`，最大桌面 `32 MiB` / 移动端 `16 MiB`（客户端偏好可达 `64 MiB` / `32 MiB`）；快速连续成功后将提示翻倍，失败按 `* 0.75` 以 `1 MiB` 步长降低
- 下载分块大小：桌面 `8 MiB`，移动端 `6 MiB`，客户端提示更大会话时最大 `32 MiB`
- 默认浏览器上传并行度：`8`，成功 `*1.15`（快速恢复窗口内 `*1.3`），失败 `*0.85`
- 最大浏览器上传并行度：`blog.settings` 中的 `max_upload_parallel_chunks`，上限 `40`
- 最大并发上传会话：`blog.settings` 中的 `max_total_parallel_uploads`，上限 `512`
- 最大上传大小：`blog.settings` 中的 `max_upload_size`
- 最大浏览器下载会话：服务器定义，当前每会话最多 `48` 个分块请求
- 下载合并（span 组大小）：成功 `*1.2`，失败 `*0.8`；服务器配置最多提示 `16` 个分块，客户端 span 可增长至 `64` 个分块，并受每请求 `128 MiB` 上限约束
- 图片预览：请求 DPR 感知的 128px 尺寸桶（最小 `256px`，最大 `1920px`），使图片传输在渲染尺寸较小时不会膨胀为桌面级预览
- 视频/音频播放：仅在指针悬停/焦点意图时预热 TASFA 会话握手，随后保持原生 range-request 播放；不提前获取任何媒体负载，渐进式调度器保持不变
- 上传 xhr 超时：按传输规模缩放自适应空闲看门狗，`30-120 s`
- 上传会话 fetch 超时：`120 s`

TASFA 针对一般高带宽服务器部署进行调优，而非嵌入式/航空航天低带宽配置文件。浏览器的每源 HTTP 连接限制由工作池自然遵守。

### 客户端自适应

上传客户端测量块完成时间、重试、超时，以及可用时的 Network Information API 提示。它将这些输入发送到 `/file/upload/init` 和 `/file/upload/renegotiate`；服务器返回当前并行窗口和最大窗口。干净的完成会将活动窗口以 `*1.15` 提升到协商的最大值（在掉线后 5 秒快速恢复窗口内为 `*1.3`），而反复的、可预测的失败模式以 `*0.85` 降低，在良好链路上最低降至 `4`（移动端）/ `8`（桌面）。AES-GCM 回退也在自适应窗口内运行。

每个分块设置绝对超时，以防止连接接收部分数据但永不关闭导致的停滞。看门狗每 2 秒采样一次上传活动：在软停滞（`30 s` 无分块进展）时执行**软恢复**——重新协商、提升并行度并开启 5 秒快速恢复窗口——且不拆除活动连接；在硬停滞（`90 s`）时执行**激进重启（aggressive restart）**：中止在途 XHR，并从服务器位图恢复上传，从完整的 `maxParallel` 窗口重新协商（高性能传输配置还会将 `targetParallel` 重置为 `maxParallel`，将 `dispatchPacingMs` 设为 `0`）。学习到的分块大小提示仅在**会话本地**保持，不会立即持久化到 `localStorage`；仅应用于下一个 TASFA 会话。

下载也使用相同的高吞吐偏向：握手携带客户端的首选块大小提示，活动下载在成功的块组之后将 `span` 和并行度增长 `*1.2`。失败时两者均按 `*0.8` 降低。短响应、超时和网络错误按块索引重新排队；在相同块耗尽高重试预算之前，整个下载不会失败。

对于渐进式视频/音频播放，TASFA 更为严格：客户端按顺序组装并喂入分块，而有界调度器将自适应并行度限制在从播放器所需的下一个分块开始的向前窗口内。失败或超时的分块记录重试时间并释放网络槽位，使调度器能够继续填充同一窗口内的其他合格分块。播放可能在缓冲区边缘停滞，但它从不跳过字节范围，不留下未解决的缺失分块，也不提前关闭媒体流。

### 后半段 RTT 预测（拉格朗日外插）

对于大文件（块数较多的会话），服务器会累积客户端报告的每个块的 RTT 样本，最多 8 个。当收集到 3 个或更多样本时，使用拉格朗日多项式外插估算最后一个块索引处的 RTT，并乘以剩余块数来计算预计剩余时间。

- 上传：每个块传输完成后，客户端可以在下一个块请求中附加 `X-TASFA-Chunk-RTT` 头部（毫秒）。
- 下载：客户端可以通过 `chunk_rtt_ms` 查询参数单独报告前一个块的 RTT。

服务器在状态响应 JSON 中添加 `predicted_remaining_ms`，下载块响应携带 `X-TASFA-Predicted-Remaining-Ms` 头部。数学运算有意保持轻量：样本上限 8 个，简单 O(n²) 拉格朗日，预测值以 30 秒为上限截断。

### Wynn 加速预测防御

TASFA 不会预测明文字节，也不会信任预测的块内容。预测仅限于客户端观测到的传输质量序列：完成时间、吞吐量、重试、超时和短响应。客户端对最新质量样本应用 Wynn epsilon 变换的 Aitken `Delta^2` 形式：

```
S_hat = S0 - ((S1 - S0)^2 / (S2 - 2*S1 + S0))
```

`S_hat` 被限制在 `[0, 1]` 范围内，仅用作下一个块的防御信号。预测质量高时保持正常路径，并让自适应窗口增长。中等偏低时标记下一个块为 `guarded`，在分发前将活动并行窗口减少一格。非常低时将下一个上传块标记为并行处理 AES-GCM 回退，或在发出下一个范围组请求前减少下载的 `span` 和并行度。这是前向的拥塞/失败防御，而非密码学预言机。

## 存储模型

每个上传会话预先分配一个临时文件：

- 临时文件：`data/tasfa/uploads/<upload_id>/upload.bin.part`
- 元数据：`data/tasfa/uploads/<upload_id>/meta.json`
- 快速二进制元数据：`data/tasfa/uploads/<upload_id>/meta.bin`
- 状态：`data/tasfa/uploads/<upload_id>/state.json`，`data/tasfa/uploads/<upload_id>/state.bin`
- HTP 0 级：`data/tasfa/uploads/<upload_id>/htp.bin`
- 会话锁：`data/tasfa/uploads/<upload_id>/session.lock`

服务器不再维护 `blocks.bin` 或 `chunk_counts.bin`。分块完成是单次位图写入。

### 可选 S3 兼容对象存储

已完成的文件还可以额外存储在通过可选的 `s3.settings` 文件配置的 S3 兼容桶中（全部为空表示禁用，本地磁盘存储保持不变）。在 `mode=mirror`（默认）下，保留本地文件并同时 PUT 到桶中；下载继续从磁盘提供服务。在 `mode=offload` 下，字节在上传后移至 S3，文件行被标记为 `s3://<key>`，下载以 `302` 重定向到 1 小时预签名 GET URL 应答。S3 PUT 失败时回退到本地存储，所有删除路径都会清理两份副本。完整键参考见 `S3.md`。

会话元数据还存储逐顶点数组：

- `hash_tags` — 每个分块的 SHA-512 十六进制字符串数组
- `magic_scalars` — 每个分块的平衡标量数组
- `htp_retry_targets` — 当前服务器发出的重试目标列表
- `htp_suspicion_scores` — 当前嫌疑排序
- `htp_contraction_level` — 已应用的服务器端收缩遍数

这些在每次分块上传时更新，并在完成时验证。

## 删除 PIN

已完成的上传获得一次性删除 PIN。明文 PIN 是 12 字符十六进制字符串（6 个随机字节）；仅返回一次，仅存储其哈希。

## 下载协议

1. 客户端通过 `GET /.../handshake` 请求握手。
2. 服务器返回 `session_id`、`session_token`、`chunk_size`、`chunk_count`、并发提示以及会话加密密钥（`stream_key_hex`、`stream_iv_seed_hex`）。
3. 客户端使用自适应 `span=...` 获取分块组。存在会话密钥时，所有分块均使用 **AES-256-GCM** 加密。
4. 客户端在浏览器中使用 **Web Crypto API** 和会话密钥解密分块。
5. 浏览器将响应组装成一个连续的缓冲区。
6. **负载压缩（Zstd/Brotli/Gzip 回退）**：
   在下传过程中，客户端使用 `X-TASFA-Accept-Encoding` 标头宣告支持的解压算法（例如 `br, zstd, gzip`；TASFA 标头缺失时服务器回退到 `Accept-Encoding`）。该标头是能力列表而非优先级顺序：服务器在 GCM 加密**之前**应用压缩，使用其最偏好的受支持编解码器做且仅做一次全缓冲区处理，偏好顺序为 **Zstd（level 1）→ Brotli（quality 1）→ gzip（best speed）**；这是偏好顺序而非回退链——如果选定的编解码器未能节省足够字节，则直接提供原始负载，不再尝试其他编解码器。这避免了双重压缩，并确保客户端只收到可原生解压（使用 `DecompressionStream`）的负载。如果压缩收益不足（`TASFA_COMPRESS_MIN_GAIN_BYTES`，1,024 字节；要求严格大于该值），服务器跳过压缩并直接提供原始二进制负载。压缩后的分块响应携带 `X-TASFA-Content-Encoding` 和 `X-TASFA-Uncompressed-Length`，以便客户端在解密前解压。

## 可选 WASM 沙箱块加密

块级 AES-256-GCM（分块加密/解密）通常经由原生 OpenSSL EVP 路径运行。当 `TASFA_CRYPTO_WASM` 环境变量指向一个已构建的 WASI 模块时（`make wasm-tasfa-crypto` 生成 `build-wasm/tasfa_crypto.wasm`），服务器改为在沙箱中运行每个块操作：它生成 `wasmtime run <module>` 子进程，并通过 stdin/stdout 交换一次带帧的请求/响应（协议见 `wasm/tasfa_crypto_protocol.h`）。该模块执行与原生路径相同的 IV 派生和 AAD 布局，并通过 `wasm/test_parity.py` 强制与 OpenSSL EVP 保持字节级一致。此后端严格按需启用：如果 `TASFA_CRYPTO_WASM` 未设置或模块不可读，`tasfa_wasm_crypto_available()` 返回 false，使用原生路径。`wasmtime` 二进制文件可通过 `TASFA_WASMTIME` 覆盖。这仅改变分块加密的执行位置，不改变线上格式。

## 通过位图的 DoS 缓解

上传和下载状态均通过**稠密二进制位图**跟踪（每个分块 1 字节，`'0'` / `'1'`）。

### 上传侧

- 服务器拒绝 `state.bin` 中已标记为 `'1'` 的分块索引重传，**除非该分块显式位于服务器的重试目标列表中**。攻击者无法重放任意分块来消耗磁盘 I/O。
- `state.bin` 通过 `pwrite(..., 1, chunk_index)` 实现 O(1) 原子写入，热路径上不存在 JSON 解析。
- `complete` 处理程序重新打开位图，计数已设置位，并拒绝最终化直到 `received_chunks == chunk_count`。这防止截断文件攻击。

### 会话加固

- 上传 ID 和 Token 分别为 16 字节和 24 字节随机十六进制字符串。
- 会话锁（`session.lock` 上的 `flock`）防止并发请求间的位图更新竞争。

## 工作调度器

服务器使用固定轮询工作调度器。工作器数量等于 CPU 核心数（最小 2，最大 64）。属于同一 `upload_id` 的分块始终分派到同一工作器以保持缓存局部性和顺序。作业通过每个工作器的队列提交，并通过条件变量发出信号。

## 异步最终化

`POST /file/upload/complete` 是异步处理的。首次调用返回 `202 Accepted` 和 `{"processing": true}`，并在引擎池上调度一个后台最终化工作器，处于隔离的 `TTAK_TASK_DOMAIN_THREAD` 调度域中，使最终化工作（媒体转码、SHA-256 校验和、数据库写入）从不与 HTTP 服务循环共享工作器。如果池拒绝该作业（例如正在关闭），最终化记录被标记为 `503`，客户端重试而不是占用 HTTP 工作器。后续调用轮询最终化缓存；当工作器完成时，缓存的状态和正文立即返回。这防止长时间运行的最终化工作阻塞 HTTP 连接。

## 自查清单

| 问题 | 回答 |
|------|------|
| Q1: 客户端是否计算任何修复代数？ | **否。** 客户端是愚笨的重传代理。所有嫌疑推导、置信度评分、成本阈值和收缩逻辑均为服务器端专用。 |
| Q2: 是否在收缩前显式评估修复成本阈值？ | **是。** `htp_repair_worthwhile` 比较 `retry_cost_estimate`（字节 × RTT）与 `repair_cost_estimate`（服务器分析开销）。当 `suspect_count < 12` 或直接重传更便宜时返回 false，回退到直接重传。 |
| Q3: 部分组是否补零？ | **否。** 仅验证完整的 6 槽组（`data_chunks / 6`）。不完整的最后一组完全排除。 |
| Q4: 响应是否包含嫌疑分数，而非仅二进制标志？ | **是。** 每个 `needs_retry` 响应都包含 `suspicion_scores` 作为 `{chunk_index, score}` 对象。 |
| Q5: 收缩是否保留原始组拓扑？ | **是。** `htp_contract_groups` 将每个原始完整组视为单个高级顶点，不在组间重新打乱嫌疑分块。 |
| Q6: 成功重传时是否清除重试目标？ | **是。** `handler_file_upload` 在接受重试重传后从 `htp_retry_targets` 中移除该分块。 |
| Q7: 服务器是否通过交换修复乱序标量？ | **是。** `htp_try_swap_repair` 暴力遍历 6 槽组的全部 720 种排列；若任一排列恢复不变式，`htp.bin` 记录立即被重新排序，该组无需重传即通过。 |
