# TASFA

TASFA 是本專案用於檔案上傳與下載的傳輸協定。

它建構在普通 HTTP/XHR 之上，增加了分塊加密、檔案級順序佇列，以及一個**伺服器權威的 6 槽 HTP（Hexagonal Tortoise Problem）恢復格子（recovery lattice）**用於完整性控制。

## 路由

上傳:
- `POST /file/upload/init`
- `POST /file/upload/status`
- `POST /file/upload/renegotiate`
- `POST /file/upload`
- `POST /file/upload/complete`
- `POST /file/upload/cancel`

下載:
- `GET /file/download/:id/handshake`
- `GET /file/download/:id/chunk/:chunk_index`
- `GET /assets/tasfa/img/:filename/handshake`
- `GET /assets/tasfa/img/:filename/chunk/:chunk_index`
- `GET /assets/tasfa/uploads/:filename/handshake`
- `GET /assets/tasfa/uploads/:filename/chunk/:chunk_index`

## 上傳協定

瀏覽器先協商上傳工作階段，然後傳送帶有 TASFA 標頭的**分塊**（預設 `24 MiB`，行動裝置 `12 MiB`）：

- `X-TASFA-Upload-ID`
- `X-TASFA-Upload-Token`
- `X-TASFA-Chunk-Index`
- `X-TASFA-Hash-Tag`
- `X-TASFA-Raw-Scalar`
- `X-TASFA-Magic-Scalar`

伺服器將每個分塊直接寫入預先分配暫存檔案的 `chunk_index * chunk_size` 偏移處。不再有傳輸區塊。

如果普通分塊反覆失敗，瀏覽器會透過帶有 `X-TASFA-Stream-Mode: aes-256-gcm` 的後援要求傳送該失敗分塊。伺服器接受密文加上 GCM 認證標籤，且後援分塊保留在自適應並行視窗內。後援要求仍然攜帶相同的 HTP 雜湊標籤和平衡純量標頭。

### 餘數（最後部分分塊）

如果檔案大小不是分塊大小的倍數，最後一個分塊是**餘數**。它以精確位元組範圍作為單一 blob 傳送；伺服器將其寫入正確偏移。不執行填充或拆分。

### 工作階段初始化回應

`init` 端點傳回包含以下欄位的回應：

- `chunk_size` — 協商後的分塊大小
- `current_parallel_chunks` — 伺服器核准的目前上傳視窗
- `max_parallel_chunks` — 用戶端可同時上傳的分塊數量
- `dispatch_pacing_ms` — 僅在測量到的鏈路較差時套用的小傳送間隔
- `upload_secret` — 用於加密串流驗證的輔助伺服器金鑰
- `stream_key_hex`, `stream_iv_seed_hex`, `stream_mode` — 工作階段加密金鑰 (`aes-256-gcm`)
- `modulus_M` — 本工作階段使用的 HTP 模數
- `group_count` — 完整 6 槽 HTP 組的數量
- `client_stripes` — 用戶端工作排程器使用的固定值 `32`

用戶端將 `chunk_size` 視為工作階段契約，因為它定義了每個分塊的檔案偏移。在活躍上傳期間，它調整並行和重試行為，同時學習到的分塊大小提示將套用於下一個 TASFA 工作階段：良好鏈路逐漸提高提示，降級鏈路以小至 `512 KiB` 的步長降低。

### 上傳分塊回應

當分塊被接受時，伺服器回傳 `204 No Content` 和以下標頭：

- `X-TASFA-Accepted: 1`
- `X-TASFA-Chunk-Complete: 1`

如果分塊索引已在 `state.bin` 中標記為完成，**而且**該索引不在伺服器的 `retry_targets` 中，則伺服器回傳相同的 `204` 標頭，但分塊內文將被捨棄。

## HTP 伺服器權威恢復格子

HTP **不是傳輸協定，也不是密碼學證明**。它是一個伺服器端分塊嫌疑引擎，按損壞可能性對分塊進行排序，使用戶端只需重傳高機率嫌疑分塊，而非整個檔案。

### 分塊分組

分塊被分組為連續的 6 元素頂點：

```
組 g: [ v0 , v1 , v2 , v3 , v4 , v5 ]
       chunk g*6+0  ...  g*6+5
```

最後一組可能是部分的；**不完整組絕不補零，且完全排除在 HTP 驗證之外**。零填充會注入人造拓撲，因此被禁止。

### 原始純量

對於每個分塊，用戶端計算明文分塊的 SHA-512，取前 8 位元組作為大端序無號 64 位元整數 `H`，並導出：

```
raw_scalar = H % modulus_M
```

### 魔法線不變式

對於完整組，定義三條線：

```
L1 = v0 + v1 + v2   (mod M)
L2 = v2 + v3 + v4   (mod M)
L3 = v4 + v5 + v0   (mod M)
```

不變式要求 `L1 == L2 == L3`。如果原始純量不滿足此條件，用戶端透過**僅調整 `v3` 和 `v5`** 來平衡：

```
delta2 = (L1 - L2) mod M
delta3 = (L1 - L3) mod M

v3_balanced = (v3_raw + delta2) mod M
v5_balanced = (v5_raw + delta3) mod M
```

其餘所有頂點保持其原始純量。客戶端同時發送兩者:

- `X-TASFA-Raw-Scalar` — 未修改的 `raw_scalar`
- `X-TASFA-Magic-Scalar` — 平衡值 (`v3_balanced` 或 `v5_balanced`，其餘與 raw 相同)

伺服器將兩個純量分別儲存在 `htp.bin` 中，從而可以在不受人工平衡約束影響的情況下分析原始拓撲。

### 為什麼只有 v3 和 v5？

六邊形格子有兩個自由度。固定 `v0,v1,v2,v4` 並調整 `v3,v5`，可在保持最小增量且局限於組內的同時唯一滿足三條線方程式。

## 伺服器權威 HTP 恢復

**用戶端是愚笨的重傳代理（dumb retransmission agent）。** 用戶端不計算修復代數，不評估成本閾值，也不導出嫌疑排序。所有這些都僅存於伺服器端。

### 伺服器驗證流程

在 `POST /file/upload/complete` 期間，伺服器執行以下操作：

1. 從 `htp.bin` 載入所有分塊級記錄（雜湊標籤、原始純量和平衡純量）。
2. 僅驗證**完整的 6 槽組**（跳過部分組）。
3. 對每個失敗組，透過分析每個槽位參與的線方程式來計算**嫌疑分數（suspicion scores）**。

### 嫌疑置信度評分（每組）

對於失敗組，伺服器將每個槽位與三條線方程式進行比較評估：

| 槽位 | 方程式 |
|------|------|
| v0   | L1, L3 |
| v1   | L1     |
| v2   | L1, L2 |
| v3   | L2     |
| v4   | L2, L3 |
| v5   | L3     |

每個槽位的嫌疑分數是確定性的，僅由拓撲推導得出：

```
score = in_fail / total_fail
```

其中 `in_fail` 是該槽位參與的失敗線方程數，`total_fail` 是該組的總失敗方程數。不使用任意置信度常數。

若槽位僅出現在通過方程式中，則**清除**出嫌疑列表。

分數在所有失敗組間聚合；若一個分塊出現在多組中，保留其最高分。

### 修復成本閾值

在請求任何修復之前，伺服器評估收縮是否比直接重傳更便宜：

```
repair_worthwhile(嫌疑數, 總分塊數, 分塊大小, rtt_ms):
    if 嫌疑數 < 3                → false  （太少，拓撲無意義）
    retry_cost  = 嫌疑數 * 分塊大小 * rtt_factor(rtt_ms)
    repair_cost = 元資料位元組 + 伺服器CPU開銷 + 額外RTT開銷
    return retry_cost > repair_cost
```

抽象成本模型比較客戶端需重傳的位元組與伺服器端分析開銷。大分塊或高延遲使收縮更有吸引力，而大量小嫌疑分塊使直接重傳更便宜。具體數值是伺服器端實作細節，不是協定常數。

若閾值拒絕修復，伺服器傳回 `needs_retry`，並將**所有**嫌疑分塊作為重傳目標。用戶端透過普通上傳端點重傳它們。

### 伺服器端遞迴收縮

若修復值得，伺服器執行 **組級收縮**: 每個原始完整的 6 槽組被收縮為編碼其 **不變式簽名** 的單個純量。伺服器計算該組的三條線值 `L1, L2, L3`，推導殘差 `r12 = (L1-L2) mod M` 和 `r23 = (L2-L3) mod M`，並將組聚合值設為 `(r12 * r23) mod M`。通過組的 `r12 = r23 = 0`，故聚合值為 `0`；失敗組則獲得一個保留線不一致拓撲的非零確定性簽名。這些組聚合值成為更高級別 HTP 格的頂點。連續的 6 個組聚合值形成 level-1 超級組，並重新評估相同的線不變式：

- 若 level-1 超級組通過，則清除其底層 level-0 組中的嫌疑分塊（失敗模式在組級一致）。
- 若 level-1 超級組失敗，則保留其底層 level-0 組中的嫌疑分塊。
- 若收縮縮小了嫌疑集（更少分塊），伺服器儲存縮小後的目標並傳回 `needs_retry` 及縮減列表。
- 若收縮未能縮小集合，伺服器回退到原始嫌疑分塊的直接重傳。
- 收縮級別在工作階段中繼資料中遞增，以便用戶端報告診斷資訊。

用戶端從不看見或計算收縮組。它只接收 `retry_targets`。

### 重傳接受

當用戶端重傳已標記為接收的分塊時，普通上傳端點**僅當該分塊索引目前在伺服器的 `retry_targets` 列表中時才接受重傳**。重傳分塊儲存後，伺服器將其從 `retry_targets` 中移除。

### 協定可見修復回應

當 HTP 失敗且伺服器決定修復或重傳時，`complete` 端點傳回 `409` 並附帶：

- `htp_status`: `"needs_retry"`
- `retry_targets`: 待重傳的分塊索引陣列（按嫌疑分數降序排列）
- `suspicion_scores`: `{chunk_index, score}` 物件陣列
- `contraction_level`: 已應用的伺服器端收縮遍數
- `retry_reason`: 人類可讀的說明（例如 `"htp group inconsistency detected"`）

若成本閾值判定直接重傳更便宜，`retry_targets` 包含完整嫌疑列表且 `contraction_level` 保持為 `0`。

若伺服器透過收縮成功縮小嫌疑範圍，`retry_targets` 包含縮減列表且 `contraction_level` 遞增。

所有嫌疑分塊重傳並成功驗證後，下一次 `complete` 呼叫繼續 SHA-256 最終化。

## 檔案級順序佇列

**一次僅上傳一個檔案。** 選擇多個檔案時：

1. 每個檔案獲得自己的資源、預覽卡和 HTP 工作階段。
2. 檔案進入 `FileUploadQueue` 排隊。
3. 目前檔案完成（成功或失敗）後，佇列自動前進到下一個檔案。
4. 批次「上傳排隊檔案」按鈕始終啟用；點擊後將所有待處理檔案入隊並啟動泵。

這防止瀏覽器連線池耗盡，並保持停滯檢測可靠。

## 執行階段設定

- 上傳分塊大小：桌面 `24 MiB`，行動裝置 `12 MiB`
- 自適應上傳分塊大小提示：最小 `4 MiB`，最大桌面 `48 MiB` / 行動裝置 `24 MiB`
- 下載分塊大小：桌面 `8 MiB`，行動裝置 `4 MiB`，用戶端提示更大工作階段時最大 `32 MiB`
- 預設瀏覽器上傳並行度：`16`
- 最大瀏覽器上傳並行度：`blog.settings` 中的 `max_upload_parallel_chunks`，上限 `40`
- 最大並發上傳工作階段：`blog.settings` 中的 `max_total_parallel_uploads`，上限 `64`
- 最大上傳大小：`blog.settings` 中的 `max_upload_size`
- 最大瀏覽器下載工作階段：伺服器定義，目前每工作階段最多 `48` 個分塊請求
- 下載合併（span 組大小）：良好鏈路上最多 `16` 個分塊
- 上傳 xhr 逾時：至少 `180 s`
- 上傳工作階段 fetch 逾時：`30 s`

瀏覽器每來源 HTTP 連線限制由工作池自然遵守。

### 客戶端自適應

上傳客戶端測量區塊完成時間、重試、逾時，以及可用時的 Network Information API 提示。它將這些輸入傳送到 `/file/upload/init` 和 `/file/upload/renegotiate`；伺服器傳回目前並行視窗和最大視窗。乾淨的完成會迅速將活動視窗提升到協商的最大值，而暫時性失敗不再崩潰到很小的並行度下限以下。AES-GCM 後援也在自適應視窗內執行。看門狗會中止停滯的上傳並從伺服器位元圖中恢復，而不是讓傳輸卡住。

下載也使用相同的高吞吐量偏向：握手攜帶客戶端的首選區塊大小提示，活動下載在成功的區塊群組之後增加 `span` 和並行度。短回應、逾時和網路錯誤按區塊索引重新排隊；在相同區塊耗盡高重試預算之前，整個下載不會失敗。

### 後半段 RTT 預測（拉格朗日外插）

對於大檔案（區塊數較多的工作階段），伺服器會累積客戶端回報的每個區塊 RTT 樣本，最多 8 個。當收集到 3 個或更多樣本時，使用拉格朗日多項式外插估算最後一個區塊索引處的 RTT，並乘以剩餘區塊數來計算預計剩餘時間。

- 上傳：每個區塊傳輸完成後，用戶端可以在下一個區塊請求中附加 `X-TASFA-Chunk-RTT` 標頭（毫秒）。
- 下載：用戶端可以透過 `chunk_rtt_ms` 查詢參數單獨回報前一個區塊的 RTT。

伺服器在狀態回應 JSON 中添加 `predicted_remaining_ms`，下載區塊回應攜帶 `X-TASFA-Predicted-Remaining-Ms` 標頭。數學運算有意保持輕量：樣本上限 8 個，簡單 O(n²) 拉格朗日，預測值以 30 秒為上限截斷。

### Wynn 加速預測防禦

TASFA 不會預測明文字節，也不會信任預測的區塊內容。預測僅限於客戶端觀測到的傳輸品質序列：完成時間、吞吐量、重試、逾時和短回應。客戶端對最新品質樣本套用 Wynn epsilon 變換的 Aitken `Delta^2` 形式：

```
S_hat = S0 - ((S1 - S0)^2 / (S2 - 2*S1 + S0))
```

`S_hat` 被限制在 `[0, 1]` 範圍內，僅用作下一個區塊的防禦信號。預測品質高時保持正常路徑，並讓自適應視窗增長。中等偏低時標記下一個區塊為 `guarded`，在分發前將活動並行視窗減少一格。非常低時將下一個上傳區塊標記為並行處理 AES-GCM 後援，或在發出下一個範圍群組請求前減少下載的 `span` 和並行度。這是前向的擁塞/失敗防禦，而非密碼學預言機。

## 儲存模型

每個上傳工作階段預先分配一個暫存檔案：

- 暫存檔案：`data/tasfa/uploads/<upload_id>/upload.bin.part`
- 中繼資料：`data/tasfa/uploads/<upload_id>/meta.json`
- 快速二進位中繼資料：`data/tasfa/uploads/<upload_id>/meta.bin`
- 狀態：`data/tasfa/uploads/<upload_id>/state.json`，`data/tasfa/uploads/<upload_id>/state.bin`
- HTP 0 級：`data/tasfa/uploads/<upload_id>/htp.bin`
- 工作階段鎖：`data/tasfa/uploads/<upload_id>/session.lock`

伺服器不再維護 `blocks.bin` 或 `chunk_counts.bin`。分塊完成是單次點陣圖寫入。

工作階段中繼資料還儲存逐頂點陣列：

- `hash_tags` — 每個分塊的 SHA-512 十六進位字串陣列
- `magic_scalars` — 每個分塊的平衡純量陣列
- `htp_retry_targets` — 目前伺服器發出的重試目標列表
- `htp_suspicion_scores` — 目前嫌疑排序
- `htp_contraction_level` — 已應用的伺服器端收縮遍數

這些在每次分塊上傳時更新，並在完成時驗證。

## 刪除 PIN

已完成的上傳獲得一次性刪除 PIN。明文 PIN 是 12 字元十六進位字串（6 個隨機位元組）；僅傳回一次，僅儲存其雜湊。

## 下載協定

1. 用戶端透過 `GET /.../handshake` 請求交握。
2. 伺服器回傳 `session_id`、`session_token`、`chunk_size`、`chunk_count`、並行提示以及工作階段加密金鑰（`stream_key_hex`、`stream_iv_seed_hex`）。
3. 用戶端使用自適應 `span=...` 獲取分塊組。存在工作階段金鑰時，所有分塊均使用 **AES-256-GCM** 加密。
4. 用戶端在瀏覽器中使用 **Web Crypto API** 和工作階段金鑰解密分塊。
5. 瀏覽器將回應組裝為單個連續緩衝區。

下載區塊回應包括：

- `X-TASFA-Chunk-Index` 和 `X-TASFA-Chunk-Count`
- RTT 樣本可用時 `X-TASFA-Predicted-Remaining-Ms`
- 加密啟動時 `X-TASFA-Stream-Mode: aes-256-gcm` 和加密承載
- 預先計算的 HTP 中繼資料存在時 `X-TASFA-Hash-Tag` 和 `X-TASFA-Magic-Scalar`

## 媒體處理整合

伺服器產生的媒體（縮圖、音訊預覽）是 TASFA 的一等資產：
- **HTP 元資料預計算**：當伺服器產生縮圖或預覽時，它會立即計算其分塊的 HTP 純量和 SHA-256 標籤，並將其儲存在 `data/tasfa/media_htp` 中。注意：伺服器端媒體產生器為此目的使用 **SHA-256**，而上傳客戶端對使用者上傳的分塊仍使用 **SHA-512**。
- **可靠的媒體傳輸**：媒體透過 `/assets/tasfa/...` 路由提供，支援完整的 TASFA 協定，包括使用預先計算的 HTP 元資料進行分塊級完整性驗證。
- **並行控制**：媒體產生（ffmpeg）限制為 4 個並行程序，以保護伺服器資源。

## 透過點陣圖的 DoS 緩解

上傳和下載狀態均透過**稠密二進位點陣圖**追蹤（每個分塊 1 位元組，`'0'` / `'1'`）。

### 上傳側

- 伺服器拒絕 `state.bin` 中已標記為 `'1'` 的分塊索引重傳，**除非該分塊顯式位於伺服器的重試目標列表中**。攻擊者無法重放任意分塊來消耗磁碟 I/O。
- `state.bin` 透過 `pwrite(..., 1, chunk_index)` 實現 O(1) 原子寫入，熱路徑上不存在 JSON 剖析。
- `complete` 處理常式重新開啟點陣圖，計數已設定位元，並拒絕最終化直到 `received_chunks == chunk_count`。這防止截斷檔案攻擊。

### 工作階段強化

- 上傳 ID 和 Token 分別為 16 位元組和 24 位元組隨機十六進位字串。
- 工作階段鎖（`session.lock` 上的 `flock`）防止並行要求間的點陣圖更新競爭。

## 工作排程器

伺服器使用固定輪詢工作排程器。工作器數量等於 CPU 核心數（最小 2，最大 64）。屬於同一 `upload_id` 的分塊始終分派到同一工作器以保持快取局部性和順序。作業透過每個工作器的佇列提交，並透過條件變數發出訊號。

## 非同步最終化

`POST /file/upload/complete` 是非同步處理的。首次呼叫傳回 `202 Accepted` 和 `{"processing": true}` 並啟動背景最終化工作器。後續呼叫輪詢最終化快取；當工作器完成時，快取的狀態和內文立即傳回。這防止長時間執行的 HTP 驗證和媒體產生阻塞 HTTP 連線。

## 自查清單

| 問題 | 回答 |
|------|------|
| Q1: 用戶端是否計算任何修復代數？ | **否。** 用戶端是愚笨的重傳代理。所有嫌疑推導、置信度評分、成本閾值和收縮邏輯均為伺服器端專用。 |
| Q2: 是否在收縮前顯式評估修復成本閾值？ | **是。** `htp_repair_worthwhile` 比較 `retry_cost_estimate`（位元組 × RTT）與 `repair_cost_estimate`（伺服器分析開銷）。當 `嫌疑數 < 3` 或重傳更便宜時傳回 false，回退到直接重傳。 |
| Q3: 部分組是否補零？ | **否。** 僅驗證完整的 6 槽組（`chunk_count / 6`）。不完整的最後一組完全排除。 |
| Q4: 回應是否包含嫌疑分數，而非僅二進位旗標？ | **是。** 每個 `needs_retry` 回應都包含 `suspicion_scores` 作為 `{chunk_index, score}` 物件。 |
| Q5: 收縮是否保留原始組拓撲？ | **是。** `htp_contract_groups` 將每個原始完整組視為單個高級頂點，不在組間重新打亂嫌疑分塊。 |
| Q6: 成功重傳時是否清除重試目標？ | **是。** `handler_file_upload` 在接受重試重傳後從 `htp_retry_targets` 中移除該分塊。 |
