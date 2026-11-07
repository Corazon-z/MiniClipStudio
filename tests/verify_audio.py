"""Exercise the C API recording/conversion executable without invoking ffmpeg CLI.
Run: python tests/verify_audio.py build/verified-debug/AudioCapture.exe
Artifacts stay under ignored workspace/.
"""
import array
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import time
import wave

exe = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
work = root / 'workspace' / ('audio-check-' + str(time.time_ns()))
work.mkdir(parents=True)
results = []

def run(args, expected=0):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, timeout=30)
    text = (result.stdout + result.stderr).decode('utf-8', errors='replace')
    if result.returncode != expected:
        raise AssertionError(f'{args}: exit={result.returncode}\n{text}')
    return text

def check_wav(path, samples=None, stereo=False):
    # Parse RIFF chunks, including WAVE_FORMAT_EXTENSIBLE; do not assume a 44-byte header.
    data = path.read_bytes()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
    assert struct.unpack_from('<I', data, 4)[0] + 8 == len(data)
    offset, fmt, pcm = 12, None, None
    while offset + 8 <= len(data):
        tag, size = struct.unpack_from('<4sI', data, offset)
        payload = data[offset + 8:offset + 8 + size]
        assert len(payload) == size
        if tag == b'fmt ': fmt = payload
        if tag == b'data': pcm = payload
        offset += 8 + size + size % 2
    assert fmt and pcm is not None
    code, channels, rate, byte_rate, align, bits = struct.unpack_from('<HHIIHH', fmt)
    assert (channels, rate, byte_rate, align, bits) == (2, 48000, 192000, 4, 16)
    if code == 65534: assert struct.unpack_from('<I', fmt, 24)[0] == 1
    else: assert code == 1
    count = len(pcm) // align
    if samples is not None: assert count == samples, (count, samples)
    assert len(pcm) % align == 0
    values = array.array('h', pcm)
    assert any(values), 'Unexpected silent output'
    # All fixtures are mono, so left and right should match after upmix.
    if stereo:
        assert values[::2] != values[1::2]
        # Count positive zero crossings to detect missing, duplicated or swapped planes.
        for channel, frequency in ((0, 440), (1, 880)):
            plane = values[channel::2]
            crossings = sum(a <= 0 < b for a, b in zip(plane, plane[1:]))
            assert abs(crossings - frequency) <= 2, (channel, crossings)
    else:
        assert values[::2] == values[1::2]
    results.append({'file': path.name, 'samples': count, 'seconds': count / 48000})
    return count

# 44100 Hz float planar frames exercise the actual resampler + PCM encoder + muxer.
planar = work / '浮点平面样本.wav'
run(['--synthetic', planar])
check_wav(planar, 48000)
stereo = work / 'stereo-planar.wav'
run(['--synthetic-stereo', stereo])
check_wav(stereo, 48000, stereo=True)

# End-to-end demux/decode/resample, 16-bit and 24-bit packed input.
for width in (2, 3):
    source = work / f'mono{width * 8}.wav'
    raw = b''.join(int(math.sin(2 * math.pi * 440 * i / 44100) * (2 ** (width * 8 - 3))).to_bytes(width, 'little', signed=True)
                   for i in range(44100))
    with wave.open(str(source), 'wb') as out:
        out.setparams((1, width, 44100, 0, 'NONE', 'not compressed'))
        out.writeframes(raw)
    output = work / f'converted{width * 8}.wav'
    run(['--convert', source, output])
    check_wav(output, 48000)

# Existing file protection and cleanup after publication failure.
before = hashlib.sha256(planar.read_bytes()).digest()
run(['--synthetic', planar], expected=1)
assert hashlib.sha256(planar.read_bytes()).digest() == before
assert not list(work.glob('*.part-*'))
run(['--convert', work / 'missing.wav', work / 'bad.wav'], expected=1)
assert not (work / 'bad.wav').exists()
corrupt = work / 'corrupt.wav'
corrupt.write_bytes(b'This is not a media file')
run(['--convert', corrupt, work / 'corrupt-output.wav'], expected=1)
assert not (work / 'corrupt-output.wav').exists()
run(['--synthetic', work / 'missing-directory' / 'bad.wav'], expected=1)
run(['--cancel-before-start', work / 'cancelled.wav'])
assert not (work / 'cancelled.wav').exists()
assert not list(work.glob('*.part-*'))
report = {'status': 'PASS', 'checks': results,
          'other_checks': ['existing output preserved', 'missing input', 'corrupt input', 'invalid output path', 'pre-start cancellation', 'partial file cleanup']}
(work / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps(report, ensure_ascii=False, indent=2))
print('Artifacts:', work)
