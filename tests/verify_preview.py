"""S05：已知B帧/VFR素材 -> 实际解码数值 -> GDI像素 -> 可选真实设备时钟/播放验证。"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--exe', type=Path, required=True)
parser.add_argument('--lifecycle', type=Path, required=True)
parser.add_argument('--with-device', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
folder = root / 'workspace' / f'preview-check-{time.time_ns()}'
folder.mkdir(parents=True)
report = {'passed': False, 'device_requested': args.with_device, 'cases': [], 'measurements': {}}


def run(program, arguments, expected=0, timeout=60):
    command = [str(program), *map(str, arguments)]
    started = time.perf_counter()
    result = subprocess.run(command, capture_output=True, encoding='utf-8', errors='replace', timeout=timeout)
    record = {'command': command, 'exit': result.returncode, 'seconds': time.perf_counter() - started,
              'stdout': result.stdout, 'stderr': result.stderr}
    report['cases'].append(record)
    assert result.returncode == expected, record
    return result.stdout


def tone_strength(values, frequency):
    # 中间0.25秒排除转换启动；双声道都从真实转换输出读取，不以文件扩展名猜格式。
    channel = values[2400 * 2:14400 * 2:2]
    real = sum(x * math.cos(2 * math.pi * frequency * i / 48000) for i, x in enumerate(channel))
    imag = sum(x * math.sin(2 * math.pi * frequency * i / 48000) for i, x in enumerate(channel))
    return math.hypot(real, imag) / len(channel)


try:
    run(args.lifecycle, ['--test-video-surface'])
    sources = {}
    for kind in ['av', 'delay', 'silent', 'vfr', 'short']:
        source = folder / f'{kind} 中文 预览.nut'
        run(args.lifecycle, ['--make-preview-fixture', kind, source])
        sources[kind] = source
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        for target in [0, 1.1, 2.3]:
            prefix = folder / f'{kind}-{target}'
            run(args.lifecycle, ['--inspect-preview', source, target, prefix])
            data = json.loads(prefix.with_suffix(prefix.suffix + '.json').read_text())
            assert data['origin_us'] == 500000, data
            times = [(i // 2 * 4 + i % 2) * 20000 if kind == 'vfr' and i < 98 else i * 40000 for i in range(100)]
            expected_index = max(i for i, pts in enumerate(times) if pts <= round(target * 1000000))
            pictures = data['pictures']
            assert len(pictures) == 100 - expected_index
            assert [p['pts_us'] for p in pictures] == times[expected_index:]
            assert pictures[0]['pts_us'] <= target * 1000000 < pictures[0]['pts_us'] + pictures[0]['duration_us']
            assert data['video_end_us'] == 4000000
            for i, picture in enumerate(pictures, expected_index):
                expected_blue = (32 + 2 * i - 16) * 255 / 219
                assert abs(picture['blue'] - expected_blue) <= 3, (kind, target, i, picture)
                end = times[i + 1] if i < 99 else 4000000
                assert picture['duration_us'] == end - times[i]
            pcm_bytes = prefix.with_suffix(prefix.suffix + '.f32').read_bytes()
            values = struct.unpack(f'<{len(pcm_bytes) // 4}f', pcm_bytes)
            assert all(math.isfinite(x) and abs(x) <= 1 for x in values)
            if kind == 'silent':
                assert data['audio_stream'] == -1 and not values
            else:
                audio_start = 9600 if kind == 'delay' else 0
                audio_end = (201600 if kind == 'delay' else 96000 if kind == 'short' else 192000)
                first_sample = max(audio_start, (round(target * 1000000) * 48000 + 999999) // 1000000)
                expected_count = max(0, audio_end - first_sample)
                assert abs(data['audio_frames'] - expected_count) <= 1, (kind, target, data['audio_frames'], expected_count)
                assert len(values) == data['audio_frames'] * 2
                if expected_count:
                    assert data['first_sample'] == first_sample
                    frequency = 880 if target >= (2.2 if kind == 'delay' else 2.0) else 440
                    assert tone_strength(values, frequency) > 0.04
                    assert tone_strength(values, 1760) < 0.005
        assert hashlib.sha256(source.read_bytes()).hexdigest() == before
    # 同样拒绝音频文件、损坏输入；已有输出不被数值检查辅助命令覆盖。
    broken = folder / 'broken.nut'
    broken.write_bytes(b'not a media file')
    run(args.lifecycle, ['--inspect-preview', broken, 0, folder / 'bad'], 1)
    run(args.exe, ['--preview', 'one', 'two'], 2)
    if args.with_device:
        run(args.lifecycle, ['--test-preview-clock'])
        for kind in ['av', 'delay', 'silent', 'vfr', 'short']:
            run(args.lifecycle, ['--test-video-playback', sources[kind]])
            text = run(args.lifecycle, ['--measure-video-playback', sources[kind]])
            measured = json.loads(text.split('PREVIEW_MEASURE_BEGIN\n', 1)[1].split('\nPREVIEW_MEASURE_END', 1)[0])
            report['measurements'][kind] = measured
            end = 4200000 if kind == 'delay' else 4000000
            assert measured['duration_us'] == end
            assert end - 1000 <= measured['position_us'] <= end + 25000
            assert measured['prepared_frames'] == 100
            assert measured['scheduled_frames'] + measured['dropped_frames'] == 100
            assert measured['observations'] > 10
            assert measured['schedule_lateness_us'] < 100000 and measured['mailbox_lateness_us'] < 100000
            assert measured['audio_master'] == (kind != 'silent')
            if kind != 'silent':
                assert abs(measured['submitted_audio_frames'] - round(end * 48000 / 1000000)) <= 1
                expected_silence = 9600 if kind == 'delay' else 96000 if kind == 'short' else 0
                assert abs(measured['silence_frames'] - expected_silence) <= 1, (kind, measured)
                assert measured['late_audio_frames'] == 0
        # 10秒原始视频检查持续消费与自然尾部，不把4秒素材称为长时稳定性验收。
        longer = folder / 'ten-seconds.nut'
        run(args.lifecycle, ['--make-benchmark-fixture', longer])
        text = run(args.lifecycle, ['--measure-video-playback', longer])
        report['measurements']['ten_seconds'] = json.loads(text.split('PREVIEW_MEASURE_BEGIN\n', 1)[1].split('\nPREVIEW_MEASURE_END', 1)[0])
        assert report['measurements']['ten_seconds']['duration_us'] == 10000000
    report['passed'] = True
finally:
    (folder / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print('PASS S05 preview:', folder / 'report.json')
