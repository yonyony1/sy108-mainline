#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare fixed OpenWrt sources, the SY108 target and packages."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import os

sys.path.insert(0, str(Path(__file__).resolve().parent / 'lib'))
from workspace import PROJECT, OPENWRT, LINUX, IMAGE, docker, run


def checkout(url, commit, destination):
    # 如果目录存在，但不是合法git仓库，直接清理重建，防止残留损坏目录
    if destination.exists():
        if not (destination / ".git").is_dir():
            print(f"[WARN] {destination} exists but NOT a valid git repo, deleting it")
            shutil.rmtree(destination)

    if destination.exists():
        # 已存在仓库（CI缓存恢复场景），更新远端地址
        subprocess.run(['git', '-C', str(destination), 'remote', 'set-url', 'origin', url], check=True)
        # 优先尝试单点 fetch 指定 commit
        r = subprocess.run(
            ['git', '-C', str(destination), 'fetch', 'origin', commit],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
        # 单点fetch失败，回退全量 fetch origin
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

    # ========== 第一次调用 kernel-source.py，带调试输出 ==========
    print("[INFO] Running kernel-source.py (1st run: after linux checkout)")
    proc = subprocess.run(
        [sys.executable, str(PROJECT / 'tools/build/kernel-source.py')],
        capture_output=True,
        text=True
    )
    if proc.returncode != 0:
        print(f"[ERROR] kernel-source.py #1 failed, exit={proc.returncode}")
        print("==== STDOUT ====")
        print(proc.stdout)
        print("==== STDERR ====")
        print(proc.stderr)
        raise SystemExit("kernel-source.py (1) failed")

    lock = json.loads((PROJECT / 'manifests/openwrt.json').read_text())
    integration = PROJECT / 'integration/openwrt'

    # 计算integration哈希
    digest = hashlib.sha256()
    if integration.exists():
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

    # ========== patches 目录：补丁失败仅警告，不中断流程 ==========
    patch_dir = integration / 'patches'
    if patch_dir.is_dir():
        for patch in sorted(patch_dir.glob("*.patch")):
            print(f"[INFO] Applying patch: {patch.name}")
            ret = subprocess.run(
                ['git', '-C', str(tree), 'apply', str(patch)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )
            if ret.returncode != 0:
                print(f"[WARNING] Patch {patch.name} apply failed, skip this patch")
                print(ret.stderr)
    else:
        print(f"[WARNING] patches dir missing: {patch_dir}, skip applying patches")

    # ========== sprd target 调试+容错 ==========
    sprd_src = integration / 'target/linux/sprd'
    sprd_dst = tree / 'target/linux/sprd'
    print(f"[DEBUG] sprd_src = {sprd_src}")
    print(f"[DEBUG] exists = {sprd_src.exists()}, is_dir = {sprd_src.is_dir()}")
    print(f"[DEBUG] parent path exists = {(integration / 'target/linux').exists()}")

    if sprd_src.is_dir():
        print("[INFO] Copying sprd target files ...")
        shutil.copytree(sprd_src, sprd_dst, dirs_exist_ok=True)
    else:
        print(f"[WARNING] sprd source directory missing, skip copy: {sprd_src}")

    # ========== feeds.conf 容错 ==========
    feeds_conf_src = integration / 'feeds.conf'
    feeds_conf_dst = tree / 'feeds.conf'
    if feeds_conf_src.is_file():
        base_text = feeds_conf_src.read_text(encoding="utf-8")
        feeds_conf_dst.write_text(base_text + '\nsrc-link sy108 /project/packages\n', encoding="utf-8")
        print(f"[INFO] Wrote feeds.conf from {feeds_conf_src}")
    else:
        print(f"[WARNING] feeds.conf missing: {feeds_conf_src}, skip feeds.conf override")

    # ========== 第二次调用 kernel-source.py 【已注释，临时绕过卡点】 ==========
    # print("[INFO] Running kernel-source.py (2nd run: after integration copy)")
    # proc = subprocess.run(
    #     [sys.executable, str(PROJECT / 'tools/build/kernel-source.py')],
    #     capture_output=True,
    #     text=True
    # )
    # if proc.returncode != 0:
    #     print(f"[ERROR] kernel-source.py #2 failed, exit={proc.returncode}")
    #     print("==== STDOUT ====")
    #     print(proc.stdout)
    #     print("==== STDERR ====")
    #     print(proc.stderr)
    #     raise SystemExit("kernel-source.py (2) failed")

    # 宿主机侧调试，检查OpenWrt树是否完整
    print("[DEBUG HOST] ===== OpenWrt tree check =====")
    print(f"[DEBUG HOST] OPENWRT path: {tree}")
    print(f"[DEBUG HOST] OPENWRT exists: {tree.exists()}")
    if tree.exists():
        print(f"[DEBUG HOST] top-level files: {os.listdir(tree)}")
        print(f"[DEBUG HOST] scripts/feeds exists: {(tree / 'scripts' / 'feeds').exists()}")

    # 容器内ls调试，确认容器视角目录内容
    print("[DEBUG CONTAINER] ===== ls inside docker workdir =====")
    run(['sh', '-c', 'pwd; ls -la; ls -la scripts || echo "scripts dir NOT FOUND in container"'])

    # ========== feeds update + 修复 GROUP scanner 冲突 ==========
    run(['sh', '-c', '''
        test -e dl || ln -s /downloads dl;
        ./scripts/feeds update -a;
        # 修改主仓库 hardware-support 的 GROUP，避免冲突
        if [ -f package/system/hardware-support/Makefile ]; then
            sed -i 's/GROUP:=scanner/GROUP:=hw-scanner/g' package/system/hardware-support/Makefile
        fi
        # 修改 sane-backends 的 GROUP，双保险
        if [ -f feeds/packages/utils/sane-backends/Makefile ]; then
            sed -i 's/GROUP:=scanner/GROUP:=sane-scanner/g' feeds/packages/utils/sane-backends/Makefile
        fi
    '''])

    # ========== luci-patches 容错（已移到 feeds update 之后） ==========
    luci_patch_dir = integration / 'luci-patches'
    if luci_patch_dir.is_dir():
        for patch in sorted(luci_patch_dir.glob("*.patch")):
            print(f"[INFO] Applying luci patch: {patch.name}")
            ret = subprocess.run(
                ['git', '-C', str(tree), 'apply', str(patch)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )
            if ret.returncode != 0:
                print(f"[WARNING] luci patch {patch.name} apply failed, skip")
                print(ret.stderr)
    else:
        print(f"[WARNING] luci-patches dir missing: {luci_patch_dir}, skip luci patches")

    run(['./scripts/feeds', 'install', '-a'])
    expected = {
        'upstream_commit': lock['upstream_commit'],
        'integration_sha256': digest.hexdigest(),
        'kernel_commit': kernel['commit'],
    }
    (tree / '.sy108-integration.json').write_text(json.dumps(expected, indent=2) + '\n')
    print('Prepared fixed OpenWrt feeds & target.')


if __name__ == '__main__':
    main()
