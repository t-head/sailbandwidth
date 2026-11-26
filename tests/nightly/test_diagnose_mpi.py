# SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import os
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

from build_cache import ARCHIVE, BINARIES, MPI_PROBE, digest, publish
from cases import generate, write_json
from diagnose_mpi import DiagnosticError, OPERATION, WORKER_PROBE, diagnose, run_step, setup_devices
from smoke_ci import record_nas_probe, smoke_matrix, write_nas_challenge


MOCK_LAUNCHER = '''
import os
import sys

args = sys.argv[1:]
ranks = int(args[args.index('-np') + 1])
assert args[args.index('-npernode') + 1] == str(ranks // 2)
assert os.environ['OMPI_MCA_plm_rsh_agent'] == 'cluster-ssh'
assert os.environ['OMPI_MCA_oob_tcp_if_include'] == 'eth0'
assert os.environ['OMPI_MCA_orte_launch_agent'] == 'env HWLOC_COMPONENTS=-gl orted'
operation = 'multinode_device_to_device_memcpy_write_ce'
if args[-1] == 'hostname':
    assert '-x' not in args
    assert os.environ['OMPI_MCA_orte_base_help_aggregate'] == '0'
    assert os.environ['OMPI_MCA_plm_base_verbose'] == '10'
    assert os.environ['OMPI_MCA_oob_base_verbose'] == '10'
    from pathlib import Path
    logs = Path(os.environ['DIAGNOSTIC_TEST_LOGS'])
    assert (logs / '00-cluster-ssh-0.stdout.log').read_text() == 'worker-a\\n'
    assert (logs / '00-cluster-ssh-1.stdout.log').read_text() == 'worker-b\\n'
    print('worker-a\\nworker-b')
elif args[-1].endswith('/mpi_probe'):
    assert os.path.isfile(args[-1])
    print('probe rank=1 host=worker-a')
    print('probe rank=0 host=worker-b')
else:
    assert args[args.index('-x', args.index('UMD_PLATFORM_TYPE')) + 1] == 'SAILBANDWIDTH_MPI_PROGRESS'
    assert args[-2:] == ['-F', 'perf']
    assert args[args.index('-t') + 1] == operation
    if ranks == 2:
        assert args[args.index('--pair') + 1:args.index('--pair')+3] == [os.environ['UUID_B'], os.environ['UUID_A']]
        assert args[args.index('-b') + 1] == '16' and args[args.index('--loopCount') + 1] == '1'
    else:
        assert '--pair' not in args and '-b' not in args and '--loopCount' not in args and '--displayUR' in args
    for source in range(ranks):
        for peer in range(ranks):
            if source != peer and not (os.getenv('DROP_PAIR') and ranks == 2 and source == 0):
                print(f'&&&& PERF sailbandwidth/{operation}/PPU{source}/PPU{peer} 10 +GB/s')
    print('&&&& sailbandwidth test PASSED')
    print('[mpi-progress] test marker', file=sys.stderr, flush=True)
'''


class MpiDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = root = Path(temporary.name)
        for name in ('logs', 'scratch', 'cache', 'nas-probes', 'bin'):
            (root / name).mkdir()
        generate(Path(__file__).with_name('families.json'), root / 'selection', 'smoke')
        self.group, = [group['id'] for group in smoke_matrix(root / 'selection')['include'] if group['nodes'] == 2]
        write_nas_challenge(root)
        self.uuids = [f'GPU-01234567-0123-4567-890a-{value:012x}' for value in (1, 2)]
        for rank, (host, uuid) in enumerate(zip(('worker-a', 'worker-b'), self.uuids)):
            record_nas_probe(root, self.group, rank)
            (root / 'logs' / f'{self.group}-{rank}.log').write_text(
                f'Diagnostic host: {host}\nPPU 0: ZW-M890P (UUID: {uuid})\n')
        self.hostfile = root / 'hosts'
        self.hostfile.write_text('worker-a slots=8\nworker-b slots=8\n')
        fixture = root / 'binary'
        fixture.write_bytes(b'benchmark fixture')
        archive = root / ARCHIVE
        with tarfile.open(archive, 'w:gz') as package:
            for member in {*BINARIES, MPI_PROBE}:
                package.add(fixture, arcname=member)
        self.sha = digest(archive)
        build = dict(revision='a' * 40, package='ubuntu24-hggcrt13', sdk='image-' + 'b' * 64, sha256=self.sha)
        write_json(root / 'build.json', build)
        publish(archive, root / 'cache' / self.sha, build)
        launcher = root / 'bin' / 'mpirun'
        launcher.write_text('#!/bin/sh\nexec "$DIAGNOSTIC_TEST_PYTHON" -S -B "$DIAGNOSTIC_TEST_LAUNCHER" "$@"\n')
        launcher.chmod(0o755)
        (root / 'launcher.py').write_text(MOCK_LAUNCHER)
        self.env = dict(PATH=f'{root / "bin"}:{os.environ["PATH"]}', DIAGNOSTIC_TEST_PYTHON=sys.executable,
                        DIAGNOSTIC_TEST_LAUNCHER=str(root / 'launcher.py'), DIAGNOSTIC_WORKER_PROBE=WORKER_PROBE,
                        UUID_A=self.uuids[0], UUID_B=self.uuids[1])
        self.logs = root / 'logs' / f'{self.group}-diagnostics'
        self.env['DIAGNOSTIC_TEST_LOGS'] = str(self.logs)
        ssh = root / 'bin' / 'cluster-ssh'
        ssh.write_text('''#!/bin/sh
set -eu
[ "$#" = 2 ] && [ "$2" = "$DIAGNOSTIC_WORKER_PROBE" ] || exit 64
if IFS= read -r unexpected; then exit 65; fi
if [ "${FAILED_WORKER:-}" = "$1" ]; then
    printf '%s\\n' 'remote command channel unavailable' >&2
    exit 9
fi
printf '%s\\n' "$1"
printf '%s\\n' 'SSH worker HWLOC_COMPONENTS=<unset>' '/usr/bin/mpirun' >&2
''')
        ssh.chmod(0o755)
        lookup = root / 'bin' / 'getent'
        lookup.write_text('#!/bin/sh\nexit 2\n')
        lookup.chmod(0o755)

    def test_runs_through_original_and_separates_stderr(self):
        with patch.dict(os.environ, self.env):
            diagnose(self.root, self.group, self.sha, self.hostfile, 'eth0')
        data = (self.logs / '04-original-16-rank.stdout.log').read_text()
        self.assertEqual(data.count('&&&& PERF sailbandwidth/' + OPERATION), 240)
        self.assertNotIn('mpi-progress', data)
        self.assertIn('mpi-progress', (self.logs / '04-original-16-rank.stderr.log').read_text())
        self.assertFalse(list((self.root / 'scratch').iterdir()))
        context = (self.logs / '00-launch-context.log').read_text()
        self.assertIn('Master MPI interface: eth0', context)
        self.assertIn('Hostfile worker: worker-b', context)
        self.assertEqual(context.count('Worker lookup exit code: 2'), 2)
        self.assertIn('SSH worker HWLOC_COMPONENTS=<unset>',
                      (self.logs / '00-cluster-ssh-1.stderr.log').read_text())

    def test_stops_before_mpi_when_direct_worker_command_fails(self):
        with patch.dict(os.environ, {**self.env, 'FAILED_WORKER': 'worker-b'}):
            with self.assertRaisesRegex(DiagnosticError, '00-cluster-ssh-1 exited 9'):
                diagnose(self.root, self.group, self.sha, self.hostfile, 'eth0')
        self.assertEqual((self.logs / '00-cluster-ssh-0.stdout.log').read_text(), 'worker-a\n')
        self.assertIn('channel unavailable', (self.logs / '00-cluster-ssh-1.stderr.log').read_text())
        self.assertFalse((self.logs / '01-hostname.stdout.log').exists())

    def test_rejects_non_hostname_hostfile_target(self):
        self.hostfile.write_text('worker-a slots=8\n--bad-option slots=8\n')
        with patch.dict(os.environ, self.env):
            with self.assertRaisesRegex(DiagnosticError, 'distinct worker names'):
                diagnose(self.root, self.group, self.sha, self.hostfile, 'eth0')
        self.assertFalse((self.logs / '00-cluster-ssh-0.stdout.log').exists())

    def test_rejects_partial_small_case_and_does_not_run_original(self):
        with patch.dict(os.environ, {**self.env, 'DROP_PAIR': '1'}):
            with self.assertRaisesRegex(DiagnosticError, 'all 2 directed pairs'):
                diagnose(self.root, self.group, self.sha, self.hostfile, 'eth0')
        self.assertFalse((self.logs / '04-original-16-rank.stdout.log').exists())

    def test_rejects_two_logs_from_same_worker(self):
        log0 = self.root / 'logs' / f'{self.group}-0.log'
        shutil.copyfile(log0, log0.with_name(f'{self.group}-1.log'))
        with self.assertRaisesRegex(DiagnosticError, 'two distinct'):
            setup_devices(self.root / 'logs', self.group)

    def test_timeout_preserves_stderr_and_stops(self):
        argv = [sys.executable, '-S', '-B', '-c', 'import sys,time; print("started",file=sys.stderr,flush=True); time.sleep(60)']
        with self.assertRaisesRegex(DiagnosticError, 'timed out; remote rank exit was not verified'):
            run_step('timed', argv, dict(os.environ), self.root / 'logs', self.root, 1)
        self.assertEqual((self.root / 'logs' / 'timed.stderr.log').read_text(), 'started\n')


if __name__ == '__main__':
    unittest.main()
