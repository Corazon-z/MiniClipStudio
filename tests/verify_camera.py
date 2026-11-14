"""S06: 已知素材数值/像素/停止/发布检查，可显式增加真实摄像头短录；核心使用C++ FFmpeg C API。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--exe', required=True, type=Path)
parser.add_argument('--lifecycle', required=True, type=Path)
parser.add_argument('--with-camera', action='store_true')
parser.add_argument('--mode', type=int, default=4)
parser.add_argument('--mic', type=int, default=0)
args = parser.parse_args()
args.exe, args.lifecycle = args.exe.resolve(), args.lifecycle.resolve()
root = Path(__file__).resolve().parents[1]
folder = root / 'workspace' / f'camera-check-{time.time_ns()}'
folder.mkdir(parents=True)
report = {'passed': False, 'hardware_requested': args.with_camera, 'cases': [], 'outputs': []}


def run(exe, arguments, expected=0, timeout=90):
    started = time.perf_counter()
    process = subprocess.run([str(exe), *map(str, arguments)], capture_output=True,
                             encoding='utf-8', errors='replace', timeout=timeout)
    record = {'command': [str(exe), *map(str, arguments)], 'exit': process.returncode,
              'wall_seconds': time.perf_counter() - started, 'stdout': process.stdout, 'stderr': process.stderr}
    report['cases'].append(record)
    assert process.returncode == expected, record
    return process.stdout


def inspect(path):
    value = json.loads(run(args.lifecycle, ['--inspect-export', path]))
    report['outputs'].append({'file': str(path), 'inspection': value})
    return value


def simulate(source, output, *options):
    text = run(args.exe, ['--camera-simulate', source, output, '--fps', 25, *options])
    result = json.loads(re.search(r'CAMERA_RESULT (\{[^\n]+\})', text)[1])
    assert result['simulated'] and result['saved'] and result['preview_frames'] > 0
    return result, inspect(output)


try:
    sources = {}
    hashes = {}
    for kind in ['av', 'delay', 'silent']:
        source = folder / f'{kind} 中文输入.nut'
        run(args.lifecycle, ['--make-preview-fixture', kind, source])
        sources[kind] = source
        hashes[kind] = hashlib.sha256(source.read_bytes()).hexdigest()
        result, data = simulate(source, folder / f'{kind} 输出.mp4')
        assert result['preview_frames'] == 100 and data['video_frames'] == 100
        assert abs(data['video_end'] - 4) < .002 and abs(data['first_y'] - 32) < 4 and data['last_y'] > 220
        if kind == 'silent':
            assert result['audio_samples'] == 0 and data['audio_end'] == 0
        else:
            expected_end = 4.2 if kind == 'delay' else 4
            assert abs(data['audio_end'] - expected_end) < .03
            assert result['audio_samples'] == round(expected_end * 48000)
        if kind == 'delay':
            # 再重解AAC到PCM，实际开头应有约0.2秒静音；不只相信源/输出时间字段。
            raw = folder / 'delay-pcm'
            run(args.lifecycle, ['--decode-playback', folder / f'{kind} 输出.mp4', raw])
            import array
            values = array.array('f')
            values.frombytes(raw.read_bytes())
            leading = values[960:14400]  # 跳过最早10ms，留出AAC边界过渡。
            content = values[24000:36000]
            silence_power = sum(x*x for x in leading) / len(leading)
            content_power = sum(x*x for x in content) / len(content)
            assert silence_power < 1e-5 and content_power > 1e-4
    run(args.lifecycle, ['--test-camera-session', sources['av'], folder / 'session.mp4'])
    result, data = simulate(sources['av'], folder / 'stopped.mp4', '--stop-after-frames', 10)
    assert result['stopped'] and 1 <= data['video_frames'] < 100 and data['video_end'] < 1
    result, data = simulate(sources['av'], folder / 'clip.mp4', '--seconds', 1.2)
    assert data['video_frames'] == 30 and abs(data['video_end'] - 1.2) < .002
    existing = folder / 'existing.mp4'
    existing.write_bytes(b'KEEP EXISTING USER CONTENT')
    before = existing.read_bytes()
    run(args.exe, ['--camera-simulate', sources['av'], existing], expected=1)
    assert existing.read_bytes() == before
    for options in [['--fps', 0], ['--seconds', 'nan'], ['--countdown', 11]]:
        target = folder / f'invalid-{len(report["cases"])}.mp4'
        run(args.exe, ['--camera-simulate', sources['av'], target, *options], expected=1)
        assert not target.exists()
    run(args.exe, ['--camera-simulate', folder / 'missing.nut', folder / 'missing.mp4'], expected=1)
    assert not list(folder.glob('*.part-*'))
    for kind, path in sources.items():
        assert hashlib.sha256(path.read_bytes()).hexdigest() == hashes[kind]
    cameras = run(args.exe, ['--cameras'])
    report['camera_list'] = cameras
    count = int(re.search(r'CAMERA_COUNT=(\d+)', cameras)[1])
    if args.with_camera:
        assert count > 0, 'Hardware capture requested, but no camera found'
        report['modes'] = run(args.exe, ['--camera-modes', 0])
        run(args.exe, ['--camera-options', 0])  # 独立进程也须自行注册FFmpeg dshow后端。
        for audio in [False, True]:
            path = folder / ('hardware-av.mp4' if audio else 'hardware-silent.mp4')
            command = ['--camera-record', 0, args.mode, path, '--seconds', 4, '--fps', 30]
            if audio:
                command += ['--mic', args.mic]
            text = run(args.exe, command)
            result = json.loads(re.search(r'CAMERA_RESULT (\{[^\n]+\})', text)[1])
            data = inspect(path)
            assert not result['simulated'] and result['saved'] and data['video_frames'] > 30
            assert 3.5 < data['video_end'] < 9
            if audio:
                assert abs(data['audio_end'] - data['video_end']) < .5
            else:
                assert data['audio_end'] == 0
    report['passed'] = True
finally:
    (folder / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print('PASS S06 camera:', folder / 'report.json')
