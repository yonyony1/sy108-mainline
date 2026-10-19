# SPDX-License-Identifier: GPL-2.0-only
"""Public build paths and container invocation."""
from pathlib import Path
import os
import subprocess

PROJECT = Path(__file__).resolve().parents[2]
OUT = Path(os.environ.get('SY108_OUT', PROJECT / 'out')).expanduser().resolve()
OPENWRT = OUT / 'openwrt-build'
DOWNLOADS = Path(os.environ.get('SY108_DOWNLOADS', OUT / 'downloads')).expanduser().resolve()
_SIBLING_LINUX = PROJECT.parent / 'linux-udx710'
LINUX = Path(
    os.environ.get(
        'SY108_LINUX',
        _SIBLING_LINUX if (_SIBLING_LINUX / '.git').exists() else OUT / 'linux',
    )
).expanduser().resolve()
IMAGE = os.environ.get('SY108_BUILD_IMAGE', 'sy108-build:25.12.5')


def docker():
    command = [os.environ.get('SY108_DOCKER', 'docker')]
    if os.environ.get('SY108_DOCKER_CONTEXT'):
        command += ['--context', os.environ['SY108_DOCKER_CONTEXT']]
    return command


def container(arguments, offline=False, root=False):
    OUT.mkdir(parents=True, exist_ok=True)
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    (OUT / 'ccache').mkdir(exist_ok=True)
    return docker() + [
        'run',
        '--rm',
        '--platform',
        'linux/arm64',
        '--user',
        '0:0' if root else f'{os.getuid()}:{os.getgid()}',
        '--network',
        'none' if offline else 'bridge',
        '--cpus',
        os.environ.get('SY108_JOBS', '4'),
        '--memory',
        os.environ.get('SY108_MEMORY', '16g'),
        '--env',
        'HOME=/tmp',
        '--env',
        'CCACHE_DIR=/output/ccache',
        '--env',
        'KBUILD_BUILD_USER=builder',
        '--env',
        'KBUILD_BUILD_HOST=sy108',
        '--tmpfs',
        '/tmp:rw,exec,nosuid,size=1g',
        '--mount',
        f'type=bind,source={PROJECT},target=/project,readonly',
        '--mount',
        f'type=bind,source={OUT},target=/output',
        '--mount',
        f'type=bind,source={DOWNLOADS},target=/downloads',
        '--workdir',
        '/output/openwrt-build',
        IMAGE,
        *map(str, arguments),
    ]


def run(arguments, **kwargs):
    return subprocess.run(container(arguments), check=True, **kwargs)
