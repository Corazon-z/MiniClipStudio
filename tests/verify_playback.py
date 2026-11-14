"""S04验证：已知PCM -> 真实FFmpeg解码/转换 -> 数值检查；可选真实WASAPI静音测试。
不调用ffmpeg命令行，不用sleep假装设备已播放。输出全部证据到独立workspace目录。
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import signal
import struct
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--exe', type=Path, required=True)
parser.add_argument('--lifecycle', type=Path, required=True)
parser.add_argument('--with-device', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
folder = root / 'workspace' / f'playback-check-{time.time_ns()}'
folder.mkdir(parents=True)
report = {'device_requested': args.with_device, 'cases': [], 'passed': False}


def run(command, expected=0):
    started = time.perf_counter()
    result = subprocess.run([str(x) for x in command], capture_output=True, encoding='utf-8', errors='replace', timeout=45)
    case = {'command': [str(x) for x in command], 'exit': result.returncode,
            'seconds': time.perf_counter() - started, 'stdout': result.stdout, 'stderr': result.stderr}
    report['cases'].append(case)
    assert result.returncode == expected, case
    return result


def make_wav(path, rate, channels, bits, tag=1, duration=1.0):
    count = round(rate * duration)
    samples = bytearray()
    for i in range(count):
        for channel in range(channels):
            value = 0.2 * math.sin(2 * math.pi * (440 if channel == 0 else 880) * i / rate)
            if tag == 3:
                samples += struct.pack('<f', value)
            elif bits == 8:
                samples += bytes([round(value * 127 + 128)])
            else:
                integer = round(value * ((1 << (bits - 1)) - 1))
                samples += integer.to_bytes(bits // 8, 'little', signed=True)
    fmt = struct.pack('<HHIIHH', tag, channels, rate, rate * channels * bits // 8, channels * bits // 8, bits)
    payload = b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(samples)) + samples
    if len(samples) % 2:
        payload += b'\0'
    path.write_bytes(b'RIFF' + struct.pack('<I', len(payload) + 4) + b'WAVE' + payload)
    return count


def frequency_strength(samples, channel, frequency):
    # 取中间整0.5秒，排除重采样启动/收尾；左右不同已知频率可发现漏平面/串声道。
    values = samples[4800 * 2 + channel:28800 * 2:2]
    real = sum(x * math.cos(2 * math.pi * frequency * i / 48000) for i, x in enumerate(values))
    imag = sum(x * math.sin(2 * math.pi * frequency * i / 48000) for i, x in enumerate(values))
    return math.hypot(real, imag) / len(values)


try:
    for name, rate, channels, bits, tag, duration in [
        ('stereo16', 48000, 2, 16, 1, 1.0), ('mono44', 44100, 1, 16, 1, 1.0),
        ('mono24', 32000, 1, 24, 1, 1.0), ('float32', 44100, 2, 32, 3, 1.0),
        ('mono8', 8000, 1, 8, 1, 1.0), ('short-tail', 44100, 1, 16, 1, 10001 / 44100),
    ]:
        source = folder / f'{name} 中文 音频.wav'
        count = make_wav(source, rate, channels, bits, tag, duration)
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        target = folder / f'{name}.f32'
        run([args.lifecycle, '--decode-playback', source, target])
        values = struct.unpack(f'<{target.stat().st_size // 4}f', target.read_bytes())
        assert all(math.isfinite(x) and abs(x) < 1 for x in values)
        expected = round(count * 48000 / rate)
        assert abs(len(values) // 2 - expected) <= 1, (name, len(values) // 2, expected)
        if duration >= 1:
            for channel in range(2):
                freq = 440 if channels == 1 or channel == 0 else 880
                assert frequency_strength(values, channel, freq) > 0.05, (name, channel, freq)
                assert frequency_strength(values, channel, 1760) < 0.005
        assert hashlib.sha256(source.read_bytes()).hexdigest() == before
    # AAC通常返回float planar；通过S03 C API生成，验证真实解码而非仅识别文件头。
    fixture = folder / 'audio-source.nut'
    compressed = folder / 'audio-aac.mp4'
    run([args.lifecycle, '--make-export-fixture', fixture])
    run([args.exe, '--export', fixture, compressed])
    target = folder / 'aac.f32'
    run([args.lifecycle, '--decode-playback', compressed, target])
    values = struct.unpack(f'<{target.stat().st_size // 4}f', target.read_bytes())
    assert frequency_strength(values, 0, 440) > 0.03
    assert frequency_strength(values, 1, 440) > 0.03
    assert frequency_strength(values, 0, 1760) < 0.005  # 没有错选第二条音轨。
    video = folder / 'no-audio.nut'
    run([args.lifecycle, '--make-delayed-video', video])
    corrupt = folder / 'broken.bin'
    corrupt.write_bytes(b'this is not audio')
    run([args.lifecycle, '--decode-playback', video, folder / 'no-audio.f32'], 1)
    run([args.lifecycle, '--decode-playback', corrupt, folder / 'broken.f32'], 1)
    run([args.exe, '--play'], 2)
    run([args.exe, '--play', compressed, '--volume', '101'], 2)
    if args.with_device:
        # 用2秒文件留足操作窗口，API测试静音：暂停/继续/自然结束/停止/重复打开真实设备。
        device_source = folder / 'device-test.wav'
        make_wav(device_source, 44100, 2, 16, duration=2.0)
        run([args.lifecycle, '--test-playback-device', device_source])
        run([args.exe, '--play', compressed, '--volume', '0'])
        run([args.exe, '--play', folder / 'missing.wav'], 1)
        # 定向向自己创建的CLI进程组发送Ctrl+Break，验证真实控制台取消回调。
        command = [str(args.exe), '--play', str(device_source), '--volume', '0']
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        try:
            time.sleep(1.0)
            started = time.perf_counter()
            child.send_signal(signal.CTRL_BREAK_EVENT)
            stdout, stderr = child.communicate(timeout=15)
            case = {'command': command, 'exit': child.returncode, 'cancel_to_exit_seconds': time.perf_counter() - started,
                    'stdout': stdout.decode('utf-8', errors='replace'), 'stderr': stderr.decode('utf-8', errors='replace')}
            report['cases'].append(case)
            assert child.returncode == 3, case
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
    report['passed'] = True
finally:
    (folder / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print('PASS S04 playback:', folder / 'report.json')
