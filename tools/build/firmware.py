#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build one OpenWrt profile and collect its firmware and source metadata."""
from pathlib import Path
import argparse
import json
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'lib'))
from workspace import PROJECT, OUT, OPENWRT, run

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('profile', choices=('main', 'recovery'))
p.add_argument('--jobs', type=int, default=6)
p.add_argument('--configure-only', action='store_true')
a = p.parse_args()
tree = OPENWRT
lock = json.loads((PROJECT / 'manifests/kernel.json').read_text())
archive_path = Path(
    subprocess.check_output([sys.executable, str(PROJECT / 'tools/build/kernel-source.py')], text=True).strip()
)
revision = subprocess.check_output(['git', '-C', str(PROJECT), 'rev-parse', 'HEAD'], text=True).strip()
build_id = subprocess.check_output(['git', '-C', str(PROJECT), 'describe', '--always', '--dirty'], text=True).strip()
common = [
    f'LINUX_SOURCE={archive_path.name}',
    'LINUX_SITE=file:///downloads',
    'BUILD_KEY=',
    f'SY108_BUILD_ID={build_id}',
]
shutil.copyfile(PROJECT / f'configs/openwrt-{a.profile}.config', tree / '.config')

# ========== 修复：先 update/install，再 sed 修复冲突，最后清缓存 ==========
# 注意顺序：update 会恢复 feeds 源文件，所以 sed 必须放在 install 之后，
# 且同时修改 feeds/ 源和 package/feeds/ 构建路径，确保最终扫描到的是修复后的文件。
run([
    'sh', '-c',
    '''
    rm -rf tmp;
    ./scripts/feeds update -a;
    ./scripts/feeds install -a;

    # 1) 修复 sane-backends：feeds 源 + package/feeds 构建目录 两处都改
    for f in \
        feeds/packages/utils/sane-backends/Makefile \
        package/feeds/packages/sane-backends/Makefile ; do
        if [ -f "$f" ]; then
            echo "[FIX] patching $f"
            sed -i 's/GROUP:=scanner/GROUP:=sane-scanner/g' "$f"
        fi
    done

    # 2) 修复 hardware-support（若存在）
    if [ -f package/system/hardware-support/Makefile ]; then
        echo "[FIX] patching package/system/hardware-support/Makefile"
        sed -i 's/GROUP:=scanner/GROUP:=hw-scanner/g' package/system/hardware-support/Makefile
    fi

    # 3) 清理包元数据缓存，强制 make defconfig 重新生成
    rm -f tmp/.config-package.in
    echo "[FIX] scanner group conflict patched, cache cleared"
    '''
])

# ========== 诊断：打印 scanner 在关键 Makefile 中的实际写法 ==========
# 用于确认上面的 sed 正则是否匹配；若 make defconfig 仍报 group 冲突，
# 查看这一步输出即可定位实际写法（如 GROUP := scanner、变量引用等）。
run([
    'sh', '-c',
    'grep -n "scanner" '
    'feeds/packages/utils/sane-backends/Makefile '
    'package/feeds/packages/sane-backends/Makefile '
    'package/system/hardware-support/Makefile '
    '2>/dev/null || true'
])

run(['make', 'defconfig', *common])
if a.configure_only:
    raise SystemExit(0)
# Build the feed host tool before the target image rule consumes it.
run(['make', f'-j{a.jobs}', 'V=s', 'tools/install', *common])
run(['make', f'-j{a.jobs}', 'V=s', 'package/feeds/sy108/sy108-image/host/compile', *common])
bin_dir = tree / 'bin/targets/sprd/udx710'
# Image outputs are profile-specific. Keep package/toolchain caches, but do not
# let an earlier profile's firmware appear in this profile's checksums/listing.
if bin_dir.is_dir():
    for source in bin_dir.iterdir():
        if source.is_file():
            source.unlink()
run(['make', f'-j{a.jobs}', 'V=s', *common])
output = OUT / 'images' / a.profile
output.mkdir(parents=True, exist_ok=True)
for previous in output.iterdir():
    if previous.is_file():
        previous.unlink()
for source in bin_dir.iterdir():
    if source.is_file():
        shutil.copy2(source, output / source.name)
kernel = tree / 'build_dir/target-aarch64_cortex-a55_musl/linux-sprd_udx710/linux-7.3.0-rc7'
# OpenWrt derives the target triplet from the selected profile.
if not kernel.is_dir():
    candidates = list((tree / 'build_dir').glob('target-*/linux-sprd_udx710/linux-7.3.0-rc7'))
    if len(candidates) != 1:
        raise RuntimeError('Missing or ambiguous built kernel')
    kernel = candidates[0]
shutil.copyfile(kernel / 'arch/arm64/boot/Image', output / 'Image')
shutil.copyfile(kernel / '.config', output / 'kernel.config')
dtb = 'udx710-sy108' + ('-recovery' if a.profile == 'recovery' else '') + '.dtb'
shutil.copyfile(kernel / 'arch/arm64/boot/dts/sprd' / dtb, output / 'board.dtb')
if a.profile == 'main':
    shutil.copyfile(bin_dir / 'openwrt-sprd-udx710-sy108-kernel.bin', output / 'boot.bin')
    roots = list(bin_dir.glob('*rootfs.squashfs'))
    if len(roots) != 1:
        raise RuntimeError('Missing or ambiguous SquashFS')
    shutil.copyfile(roots[0], output / 'rootfs.squashfs')
    upgrades = list(bin_dir.glob('*sysupgrade.bin'))
    if len(upgrades) != 1:
        raise RuntimeError('Missing or ambiguous sysupgrade image')
    shutil.copyfile(upgrades[0], output / 'sysupgrade.bin')
else:
    roots = list(bin_dir.glob('*.cpio.gz'))
    if len(roots) != 1:
        raise RuntimeError('Missing or ambiguous Recovery root archive')
    shutil.copyfile(roots[0], output / 'rootfs.cpio.gz')
    base = '/output/images/recovery'
    host = '/output/openwrt-build/staging_dir/host/share/sy108-image/tools/image/capsule.py'
    run([
        'python3', host, '--image', base + '/Image', '--dtb', base + '/board.dtb',
        '--ramdisk', base + '/rootfs.cpio.gz', '--target', 'recovery', '--output', base + '/boot.bin',
    ])
shutil.copyfile(tree / '.config', output / 'openwrt.config')
metadata = {
    'profile': a.profile,
    'kernel': lock,
    'component_commit': revision,
    'integration_commit': revision,
    'openwrt_commit': json.loads((PROJECT / 'manifests/openwrt.json').read_text())['upstream_commit'],
    'build_id': build_id,
    'local_sources': True,
    'source_dirty': '-dirty' in build_id,
}
(output / 'build.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(output)