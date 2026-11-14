"""区域录屏与双缓冲验证；--with-desktop只抓测试自己的色块窗口，不录用户桌面内容。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--exe', type=Path, required=True)
parser.add_argument('--lifecycle', type=Path, required=True)
parser.add_argument('--with-desktop', action='store_true')
args = parser.parse_args()
args.exe, args.lifecycle = args.exe.resolve(), args.lifecycle.resolve()
folder = Path(__file__).resolve().parents[1] / 'workspace' / f'screen-check-{time.time_ns()}'
folder.mkdir(parents=True)
report = {'passed': False, 'desktop_requested': args.with_desktop,
          'exe_sha256': hashlib.sha256(args.exe.read_bytes()).hexdigest(), 'cases': []}


def run(exe, arguments, expected=0):
    started = time.perf_counter()
    result = subprocess.run([str(exe), *map(str, arguments)], capture_output=True,
                            encoding='utf-8', errors='replace', timeout=45)
    record = {'command': [str(exe), *map(str, arguments)], 'exit': result.returncode,
              'seconds': time.perf_counter() - started, 'stdout': result.stdout, 'stderr': result.stderr}
    report['cases'].append(record)
    assert result.returncode == expected, record
    return result.stdout


try:
    run(args.lifecycle, ['--test-video-surface'])
    run(args.lifecycle, ['--test-screen-region'])
    run(args.exe, ['--screen-record'], 2)
    for name, coordinates in [('odd', [0,0,127,96]), ('tiny', [0,0,8,8]),
                              ('outside', [999999,999999,128,96]), ('negative-size',[0,0,-4,64])]:
        output = folder / f'{name}.mp4'
        run(args.exe, ['--screen-record', *coordinates, output, '--seconds', 1], 1)
        assert not output.exists()
    existing = folder / 'existing.mp4'
    existing.write_bytes(b'KEEP USER FILE')
    run(args.exe, ['--screen-record', 0,0,128,96,existing,'--seconds',1],1)
    assert existing.read_bytes() == b'KEEP USER FILE'
    if args.with_desktop:
        output = folder / 'known-pattern-region.mp4'
        run(args.lifecycle, ['--test-screen-capture', output])
        inspection = json.loads(run(args.lifecycle, ['--inspect-export', output]))
        assert inspection['video_frames'] >= 10 and 1.5 <= inspection['video_end'] < 3
        assert inspection['audio_end'] == 0
        report['inspection'] = inspection
        info = run(args.exe, ['--probe', output])
        assert '128x96' in info and 'h264' in info
    assert not list(folder.glob('*.part-*'))
    report['passed'] = True
finally:
    (folder / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print('PASS region screen and buffered video:', folder / 'report.json')
