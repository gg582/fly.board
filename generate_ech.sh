#!/bin/bash
set -euo pipefail

PUBLIC_NAME="${1:-localhost}"
OUTPUT_FILE="${2:-ech/server.ech}"

mkdir -p "$(dirname "$OUTPUT_FILE")"

python3 - "$PUBLIC_NAME" "$OUTPUT_FILE" << 'PYEOF'
import sys
import subprocess
import struct
import base64
import secrets

public_name = sys.argv[1].encode('ascii')
output_path = sys.argv[2]

# 1. Generate X25519 private key (DER format)
priv_der = subprocess.run(
    ["openssl", "genpkey", "-algorithm", "X25519", "-outform", "DER"],
    capture_output=True, check=True
).stdout

# 2. Extract raw public key (last 32 bytes of DER SubjectPublicKeyInfo)
pub_der = subprocess.run(
    ["openssl", "pkey", "-inform", "DER", "-in", "/dev/stdin",
     "-pubout", "-outform", "DER"],
    input=priv_der, capture_output=True, check=True
).stdout
pub_raw = pub_der[-32:]

# 3. ECH parameters
config_id = secrets.randbelow(256)          # uint8
kem_id = 0x0020                              # X25519 (RFC 9180)
kdf_id = 0x0001                              # HKDF-SHA256
aead_id = 0x0001                             # AES-128-GCM
max_name_len = min(255, max(32, len(public_name) + 16))

# 4. Build ECHConfigContents (draft-13 / RFC 9614, version 0xfe0d)
contents = (
    bytes([config_id])                       # uint8  config_id
    + struct.pack(">H", kem_id)              # uint16 kem_id
    + struct.pack(">H", len(pub_raw)) + pub_raw  # opaque public_key
    + struct.pack(">H", 4)                   # cipher_suites length
    + struct.pack(">H", kdf_id)              # CipherSuite.kdf_id
    + struct.pack(">H", aead_id)             # CipherSuite.aead_id
    + bytes([max_name_len])                  # uint8 maximum_name_length
    + bytes([len(public_name)]) + public_name # opaque public_name
    + struct.pack(">H", 0)                   # extensions length
)

# 5. Build ECHConfig (version + length + contents)
ech_config = b'\xfe\x0d' + struct.pack(">H", len(contents)) + contents

# 6. Build ECHConfigList (length-prefixed list of ECHConfigs)
ech_config_list = struct.pack(">H", len(ech_config)) + ech_config

# 7. Base64 PEM encoding
priv_b64 = base64.b64encode(priv_der).decode('ascii')
ech_b64 = base64.b64encode(ech_config_list).decode('ascii')

def wrap64(s):
    return '\n'.join(s[i:i+64] for i in range(0, len(s), 64))

with open(output_path, 'w') as f:
    f.write("-----BEGIN PRIVATE KEY-----\n")
    f.write(wrap64(priv_b64) + "\n")
    f.write("-----END PRIVATE KEY-----\n")
    f.write("-----BEGIN ECHCONFIG-----\n")
    f.write(wrap64(ech_b64) + "\n")
    f.write("-----END ECHCONFIG-----\n")

print(f"Generated ECH key: {output_path}")
print(f"  Public name : {public_name.decode()}")
print(f"  Config ID   : {config_id}")
print(f"  KEM         : X25519 (0x{kem_id:04x})")
print(f"  CipherSuite : HKDF-SHA256 / AES-128-GCM")
PYEOF
