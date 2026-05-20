# PQC (Post-Quantum Cryptography) Signature Analysis

## Overview

fly.board advertises "quantum-resistant" signatures on posts. This document explains **how the feature actually works** and what its **real limitations** are.

## How PQC Is Achieved

### 1. Algorithm: ML-DSA-65

- As noted in `src/crypto/fly_crypto.h` and the `README`.
- ML-DSA-65 is a **NIST-standardized lattice-based signature scheme**.
- Unlike RSA/ECDSA, which rely on integer factorization and discrete logarithms (broken by Shor's algorithm on a quantum computer), lattice problems are currently believed to resist polynomial-time quantum attacks.

### 2. Implementation Path: CWIST Framework → BoringSSL

- `fly_crypto.c` conditionally includes `<cwist/security/pqc/pqc_sig.h>`.
- The CWIST framework embeds BoringSSL, which contains an ML-DSA implementation under `crypto/mldsa/`.
- fly.board does **not** implement its own PQC algorithm; it simply **invokes the standard ML-DSA API provided by CWIST/BoringSSL**.

### 3. Signed Payload

- When a post is created or updated, the following string is signed:
  ```
  title + "\n" + content
  ```
- The signature is stored as Base64 in the SQLite `posts` table, column `pqc_signature`.
- On post detail view, `fly_crypto_verify()` validates it and a "verified" badge is rendered.

### 4. Key Management

- `fly_crypto_init()` generates **a single global signing key** and keeps it in memory.
- The key lives for the lifetime of the application and is freed on shutdown by `fly_crypto_cleanup()`.

## Limitations and Caveats

### 1. Server Attestation, Not Author Authentication

- There is **one key per server**, with no per-user key separation.
- The signature therefore does not prove *which* user wrote the post; it only proves that **the server attests to this post**.
- If the server is compromised, an attacker can use the server's private key to forge valid signatures for arbitrary posts.

### 2. Conditional Compilation Trap

- All crypto functions in `fly_crypto.c` are wrapped in `#ifdef HAVE_PQC`.
- If the build environment lacks `<cwist/security/pqc/pqc_sig.h>`, every function becomes a **no-op dummy**: `fly_crypto_sign()` returns `false`, and `fly_crypto_verify()` always fails.
- PQC support is **not guaranteed**; it depends entirely on the CWIST PQC module being present.

### 3. Narrow Signature Scope

- Only `title` and `content` are signed.
- Author ID, timestamp, board metadata, and attachment info are **not included in the signed payload**.
- An attacker could swap the author field while leaving the body intact, and the signature would still verify.

### 4. Transport Layer Still Uses Classical Crypto

- fly.board uses HTTP/3 (QUIC) over TLS.
- The TLS handshake and session encryption are **still ECC/RSA-based**.
- The PQC signature applies only to the *post body integrity*; it does **not** make the entire client-server communication quantum-safe.

### 5. No Key Rotation or Revocation

- The global key is generated once and never rotated until restart.
- A single key compromise collapses the trustworthiness of every post signature simultaneously.
- There is no HSM integration, key rotation schedule, or revocation mechanism.

## Conclusion

fly.board's PQC signature uses **NIST-standard ML-DSA-65 to protect post body integrity**. For a blog engine this is an unusual and noteworthy experiment: it means post tampering remains detectable even in a quantum-computing era.

However, the absence of **per-author asymmetric authentication, metadata protection, transport-layer PQC, and key lifecycle management** makes the marketing claim of being "quantum attack resistant" an overstatement. Treat this feature as a **PQC signature demo** rather than a fully hardened post-quantum security layer.
