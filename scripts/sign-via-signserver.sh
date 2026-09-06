#!/usr/bin/env bash
#
# Sign the unsigned application (blinky.bin) via the local SignServer and
# replace it with the signed image in place.
#
# Runs on the self-hosted runner next to SignServer, from the workspace root
# where the "firmware-unsigned" artifact was downloaded (blinky.bin,
# bootloader/, partition_table/, flasher_args.json).
#
# The FULL image is sent to SignServer. The PlainSigner worker is configured to
# hash internally (RSA-PSS/SHA-256/salt 32 = ESP32 Secure Boot v2)
set -euo pipefail

export PATH="$HOME/.local/bin:$PATH"

# Runner defaults; override when testing outside the runner (e.g. devcontainer):
#   KEYS_DIR=/workspace/keys SIGN_URL=https://localhost:8444/...
KEYS_DIR="${KEYS_DIR:-/home/runner/keys}"
SIGN_URL="${SIGN_URL:-https://signserver:8443/signserver/rest/v1/workers/PlainSigner/process}"

# upload-artifact uses the least common ancestor of the paths as the artifact
# root, so the download restores the files at the workspace root.
ls -la
ls -la bootloader partition_table

[[ -f blinky.bin ]] || {
  echo "error: blinky.bin not found - run the build/upload job first" >&2
  exit 1
}

# 1. Build the REST v1 JSON request containing the FULL unsigned app (base64).
jq -n --rawfile b64 <(base64 -w0 blinky.bin) \
  '{data:$b64, encoding:"BASE64"}' > signserver-request.json

# 2. Ask the local SignServer for the signature (mTLS). 
#    The response carries the 384-byte
#    signature ("data") and the signer certificate ("signerCertificate").
curl --fail-with-body --silent --show-error \
  --cacert "$KEYS_DIR/ManagementCA.crt" \
  --cert "$KEYS_DIR/client.crt" \
  --key "$KEYS_DIR/client.key" \
  -H 'X-Keyfactor-Requested-With: REST' \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  --data @signserver-request.json \
  "$SIGN_URL" \
  > signserver-response.json

# 3. Extract the signer certificate -> public key, and the signature
jq -r .signerCertificate signserver-response.json \
  | base64 -d > signer-certificate.der
openssl x509 -inform DER -in signer-certificate.der \
  -pubkey -noout > signing_public.pem
jq -r .data signserver-response.json \
  | base64 -d > signature.bin

# 4. Assemble the Secure Boot v2 signature block over the app
espsecure sign-data --version 2 \
  --pub-key signing_public.pem \
  --signature signature.bin \
  --output blinky.signed.bin \
  blinky.bin

# 5. The published/flashed app is the signed one (flasher_args.json points at
#    blinky.bin, so keep that name)
mv blinky.signed.bin blinky.bin

# 6. Verify before publishing
espsecure verify-signature --version 2 \
  --keyfile signing_public.pem blinky.bin
