# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("install_hpn_sdk.sh")
URL = "https://zone-ci.oss-cn-hangzhou.aliyuncs.com/hpn-sdk/test/installer.bin"


class InstallHPNSDK(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.download_args = self.root / "download.args"
        self.install_args = self.root / "install.args"
        wget = self.root / "wget"
        wget.write_text('''#!/bin/sh
set -eu
printf '%s\\n' "$@" > "$HPN_TEST_DOWNLOAD_ARGS"
while [ "$1" != -O ]; do shift; done
case "$HPN_TEST_MODE" in
    download-fail) printf 'partial download' > "$2"; exit 1 ;;
    empty) : > "$2"; exit 0 ;;
esac
cat > "$2" <<'INSTALLER'
set -eu
printf '%s\\n' "$@" > "$HPN_TEST_INSTALL_ARGS"
if [ "$HPN_TEST_MODE" = install-fail ]; then exit 42; fi
INSTALLER
''')
        wget.chmod(0o755)

    def run_installer(self, url=None, mode="pass"):
        env = dict(os.environ, PATH=f"{self.root}:{os.environ['PATH']}", TMPDIR=str(self.root),
                   HPN_TEST_DOWNLOAD_ARGS=str(self.download_args), HPN_TEST_INSTALL_ARGS=str(self.install_args),
                   HPN_TEST_MODE=mode)
        env.pop("HPN_SDK_URL", None)
        if url is not None:
            env["HPN_SDK_URL"] = url
        result = subprocess.run(["bash", str(SCRIPT)], env=env, text=True, capture_output=True, timeout=10)
        self.assertEqual(list(self.root.glob("hpnsdk.*")), [], "temporary installer was not removed")
        return result

    def test_unset_or_empty_url_uses_image_sdk(self):
        for url in (None, ""):
            with self.subTest(url=url):
                result = self.run_installer(url)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("image-builtin", result.stdout)
                self.assertFalse(self.download_args.exists())
                self.assertFalse(self.install_args.exists())

    def test_installs_with_pccl_arguments_and_bounded_download(self):
        result = self.run_installer(URL)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("installation completed", result.stdout)
        args = self.download_args.read_text().splitlines()
        self.assertEqual(args[:6], ["--quiet", "--max-redirect=0", "--tries=3", "--timeout=60", "--waitretry=5", "-O"])
        self.assertEqual(Path(args[6]).parent, self.root)
        self.assertEqual(args[7:], ["--", URL])
        self.assertEqual(self.install_args.read_text().splitlines(), ["--install", "--force"])

    def test_rejects_urls_before_download(self):
        for url in (URL.replace("https:", "http:"), URL.replace("zone-ci.", "other."),
                    URL.replace("://", "://someone:password@"), URL.replace(".com/", ".com:8443/"),
                    URL.removesuffix(".bin"), URL + "?token=secret", URL + "#fragment", "\t" + URL):
            with self.subTest(url=url):
                result = self.run_installer(url)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("credential-free HTTPS", result.stderr)
                self.assertNotIn(url, result.stdout + result.stderr)
                self.assertFalse(self.download_args.exists())
                self.assertFalse(self.install_args.exists())

    def test_does_not_run_missing_or_empty_payload(self):
        for mode, error in (("download-fail", "download failed"), ("empty", "installer is empty")):
            with self.subTest(mode=mode):
                result = self.run_installer(URL, mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)
                self.assertFalse(self.install_args.exists())

    def test_propagates_install_failure(self):
        result = self.run_installer(URL, "install-fail")
        self.assertEqual(result.returncode, 42)
        self.assertNotIn("installation completed", result.stdout)
