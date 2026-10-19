#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare fixed OpenWrt sources, the SY108 target and standard feeds."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / 'lib'))
from workspace import PROJECT, OPENWRT, LINUX, IMAGE, docker, run


def checkout(url, commit, destination):
    if destination.exists():
        # 已存在仓库（CI缓存恢复场景），更新远端地址
        subprocess.run(['git', '-C', str(destination), 'remote', 'set-url', 'origin', url], check=True)
        # 优先尝试单点 fetch 指定 commit
        r = subprocess.run(
            ['git', '-C', str(destination), 'fetch', 'origin', commit],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
        # 单点fetch失败（git版本不支持按sha拉取），回退全量 fetch origin
        if r.returncode != 0:
            subprocess.run(['git', '-C', str(destination), 'fetch', 'origin'], check=True)
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'init', str(destination)], check=True)
        subprocess.run(['git', '-C', str(destination), 'remote', 'add', 'origin', url], check=True)
        subprocess.run(['git', '-C', str(destination), 'fetch', '--no-tags', 'origin'], check=True)

    # 校验commit对象存在，然后detach切到目标commit
    subprocess.run(['git', '-C', str(destination), 'cat-file', '-e', commit + '^{commit}'], check=True)
    subprocess.run(['git', '-C', str(destination), 'checkout', '--detach', commit], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--linux-url', help='Optional Linux repository URL override')
    p.add_argument('--openwrt-url', help='Optional OpenWrt mirror URL override')
    p.add_argument('--skip-container', action='store_true')
    a = p.parse_args()
    kernel = json.loads((PROJECT / 'manifests/kernel.json').read_text())
    linux_url = a.linux_url or kernel.get('repo')
    if not LINUX.exists() and not linux_url:
        p.error('Provide --linux-url or set SY108_LINUX to the Linux checkout')
    checkout(linux_url, kernel['commit'], LINUX)
    subprocess.run([sys.executable, str(PROJECT / 'tools/build/kernel-source.py')], check=True)
    lock = json.loads((PROJECT / 'manifests/openwrt.json').read_text())
    integration = PROJECT / 'integration/openwrt'
    digest = hashlib.sha256()
    for path in sorted(integration.rglob('*')):
        if path.is_file():
            digest.update(str(path.relative_to(integration)).encode() + b'\0' + path.read_bytes())
    if not a.skip_container:
        subprocess.run(
            docker() + [
                'build', '--platform', 'linux/arm64', '-t', IMAGE,
                '-f', str(PROJECT / 'containers/Dockerfile'), str(PROJECT / 'containers'),
            ],
            check=True,
        )
    tree = OPENWRT
    checkout(a.openwrt_url or lock['upstream'], lock['upstream_commit'], tree)
    head = subprocess.check_output(['git', '-C', str(tree), 'rev-parse', 'HEAD'], text=True).strip()
    if head != lock['upstream_commit']:
        raise SystemExit(f'Expected OpenWrt {lock["upstream_commit"]}, found {head}: {tree}')

    # ========== 替换：自动遍历 patches 目录下所有 .patch ==========
    patch_dir = integration / 'patches'
    for patch in sorted(patch_dir.glob("*.patch")):
        applied = subprocess.run(
            ['git', '-C', str(tree), 'apply', '--reverse', '--check', str(patch)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode == 0
        if not applied:
            subprocess.run(['git', '-C', str(tree), 'apply', str(patch)], check=True)

    shutil.copytree(integration / 'target/linux/sprd', tree / 'target/linux/sprd', dirs_exist_ok=True)
    (tree / 'feeds.conf').write_text((integration / 'feeds.conf').read_text() + '\nsrc-link sy108 /project/packages\n')
    subprocess.run([sys.executable, str(PROJECT / 'tools/build/kernel-source.py')], check=True)
    run(['sh', '-c', 'test -e dl || ln -s /downloads dl; ./scripts/feeds update -a'])
    for patch in sorted((integration / 'luci-patches').glob('*.patch')):
        feed = tree / 'feeds/luci'
        applied = subprocess.run(
            ['git', '-C', str(feed), 'apply', '--reverse', '--check', str(patch)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode == 0
        if not applied:
            subprocess.run(['git', '-C', str(feed), 'apply', str(patch)], check=True)
    run(['./scripts/feeds', 'install', '-a'])
    expected = {
        'upstream_commit': lock['upstream_commit'],
        'integration_sha256': digest.hexdigest(),
        'kernel_commit': kernel['commit'],
    }
    (tree / '.sy108-integration.json').write_text(json.dumps(expected, indent=2) + '\n')
    print('Prepared fixed sources and feeds.')


if __name__ == '__main__':
    main()
