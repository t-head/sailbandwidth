#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Adapted from the PCCL CI installer at c6fab1fe8ba458595e12c09da15243c6a9f22a7f.
set -euo pipefail

url=${HPN_SDK_URL:-}
if [[ -z $url ]]; then
    echo 'HPN SDK source: image-builtin'
    exit 0
fi

python3 -S -B - "$url" <<'PY'
import sys
from urllib.parse import urlsplit

url = sys.argv[1]
try:
    parsed = urlsplit(url)
    valid = (parsed.scheme == "https"
             and parsed.hostname == "zone-ci.oss-cn-hangzhou.aliyuncs.com"
             and parsed.port in (None, 443)
             and parsed.username is None and parsed.password is None
             and parsed.path.endswith(".bin")
             and not any(char.isspace() or ord(char) < 32 or ord(char) == 127 for char in url)
             and not any(char in url for char in "?#@"))
except ValueError:
    valid = False
if not valid:
    raise SystemExit("HPN_SDK_URL must be a credential-free HTTPS .bin URL on "
                     "zone-ci.oss-cn-hangzhou.aliyuncs.com, without a query or fragment")
print(f"HPN SDK URL: {url}")
PY

installer=$(mktemp "${TMPDIR:-/tmp}/hpnsdk.XXXXXX")
trap 'rm -f -- "$installer"' EXIT
if ! wget --quiet --max-redirect=0 --tries=3 --timeout=60 --waitretry=5 -O "$installer" -- "$url"; then
    echo 'HPN SDK download failed' >&2
    exit 1
fi
if [[ ! -s $installer ]]; then
    echo 'HPN SDK installer is empty' >&2
    exit 1
fi
bash "$installer" --install --force
echo 'HPN SDK installation completed'
