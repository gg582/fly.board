# PQC（后量子密码）签名分析

## 概述

fly.board 在文章上标注了“量子抗性”签名。本文档说明该功能**实际工作原理**以及**真正的局限性**。

## 如何实现 PQC

### 1. 使用算法：ML-DSA-65

- 根据 `src/crypto/fly_crypto.h` 的注释及 `README` 描述。
- ML-DSA-65 是 **NIST 标准化的基于格（lattice）的签名算法**。
- 传统 RSA/ECDSA 依赖整数分解与离散对数问题，而这些问题可被量子计算机上的秀尔算法（Shor's algorithm）破解；格问题目前被认为在多项式时间内无法被量子算法求解。

### 2. 实现路径：CWIST 框架 → BoringSSL

- `fly_crypto.c` 条件包含 `<cwist/security/pqc/pqc_sig.h>`。
- CWIST 框架内部嵌入了 BoringSSL，BoringSSL 在 `crypto/mldsa/` 中包含 ML-DSA 实现。
- 因此 fly.board 并未自行实现独立的 PQC 算法，而是**调用 CWIST/BoringSSL 提供的标准 ML-DSA API**。

### 3. 签名对象

- 文章创建或修改时，对以下字符串进行签名：
  ```
  title + "\n" + content
  ```
- 签名以 Base64 形式存储在 SQLite `posts` 表的 `pqc_signature` 列中。
- 查看文章详情时，通过 `fly_crypto_verify()` 验证签名，并在 UI 上显示 "verified" 徽章。

### 4. 密钥管理

- `fly_crypto_init()` 生成**单一全局签名密钥**，并保留在内存中。
- 密钥在应用启动时生成一次，退出时由 `fly_crypto_cleanup()` 释放。

## 局限与注意事项

### 1. 并非作者认证，而是“服务器背书”

- 密钥为**每服务器一个**，没有按用户隔离。
- 因此该签名不能证明“哪个用户撰写”，而更接近于**“该服务器认可并担保此文章”**的含义。
- 一旦服务器被攻破，攻击者即可利用服务器私钥为任意文章生成有效签名。

### 2. 条件编译带来的可移植性陷阱

- `fly_crypto.c` 被 `#ifdef HAVE_PQC` 包裹。
- 若构建环境中不存在 `<cwist/security/pqc/pqc_sig.h>`，所有密码函数都会退化为 **no-op（空壳）**：`fly_crypto_sign()` 返回 `false`，`fly_crypto_verify()` 永远失败。
- 换言之，“支持 PQC”并非恒定保证，实际**完全依赖 CWIST PQC 模块是否存在**。

### 3. 签名范围有限

- 仅对 `title` 与 `content` 进行签名。
- 作者 ID、创建时间、板块信息、附件元数据等**未被纳入签名**。
- 攻击者可在保持正文不变的情况下篡改作者信息，而签名仍然有效。

### 4. 传输层仍使用传统加密

- fly.board 使用 HTTP/3（QUIC）与 TLS。
- TLS 握手及会话加密**仍然基于 ECC/RSA**。
- PQC 签名仅适用于“文章正文完整性”，并不意味着**客户端与服务器之间的全部通信都是量子安全的**。

### 5. 缺乏密钥轮换与撤销策略

- 全局密钥一旦生成，在重启前不会轮换。
- 若密钥泄露，所有历史文章签名的可信度将同时崩溃。
- 不存在密钥轮换（rotation）、分布式存储（HSM）、撤销（revocation）机制。

## 结论

fly.board 的 PQC 签名通过 **NIST 标准 ML-DSA-65 保障文章正文完整性**。对于博客引擎而言，这是一种罕见且有意义的尝试，意味着即使在量子计算时代，正文篡改仍可被检测。

然而，由于缺少**按作者的非对称认证、元数据保护、传输层 PQC 以及密钥生命周期管理**，将其营销为“可抵御量子攻击”存在明显夸大。将此功能理解为“PQC 签名演示”更为恰当。
