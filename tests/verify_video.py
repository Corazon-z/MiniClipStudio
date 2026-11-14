"""S02: verify real PNG pixels, frame count/drain and failure cleanup without FFmpeg CLI."""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
import sys
import time
import wave
import zlib

exe, fixture_exe = map(lambda p: Path(p).resolve(), sys.argv[1:3])
work = Path(__file__).resolve().parents[1] / 'workspace' / f'video-check-{time.time_ns()}'
work.mkdir(parents=True)
checks = []

def run(program, args, expected=0):
    result = subprocess.run([str(program), *map(str, args)], capture_output=True, timeout=30)
    text = (result.stdout + result.stderr).decode('utf-8', errors='replace')
    assert result.returncode == expected, (args, result.returncode, text)
    return text

def read_png(path):
    """Validate PNG chunks and reconstruct all five scanline filters; no image dependency."""
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    offset, packed, header, ended = 8, b'', None, False
    while offset < len(data):
        size = struct.unpack_from('>I', data, offset)[0]
        tag = data[offset+4:offset+8]
        payload = data[offset+8:offset+8+size]
        assert len(payload) == size
        crc = struct.unpack_from('>I', data, offset+8+size)[0]
        assert zlib.crc32(tag+payload) & 0xffffffff == crc
        if tag == b'IHDR': header = struct.unpack('>IIBBBBB', payload)
        if tag == b'IDAT': packed += payload
        offset += size + 12
        if tag == b'IEND': ended = True; break
    assert ended and offset == len(data) and header
    width, height, depth, kind, comp, filt, interlace = header
    assert (depth, kind, comp, filt, interlace) == (8, 2, 0, 0, 0)
    raw = zlib.decompress(packed)
    stride = width * 3
    assert len(raw) == height * (stride + 1)
    rows, previous = [], bytes(stride)
    for y in range(height):
        mode = raw[y*(stride+1)]
        current = bytearray(raw[y*(stride+1)+1:(y+1)*(stride+1)])
        assert mode in range(5)
        for x in range(stride):
            left = current[x-3] if x >= 3 else 0
            up = previous[x]
            corner = previous[x-3] if x >= 3 else 0
            estimate = left + up - corner
            distances = [abs(estimate-left), abs(estimate-up), abs(estimate-corner)]
            paeth = [left, up, corner][distances.index(min(distances))]
            prediction = [0, left, up, (left+up)//2, paeth][mode]
            current[x] = (current[x] + prediction) & 255
        rows.append(bytes(current))
        previous = current
    return width, height, rows

def make_bmp(path, top_down):
    # width=18 makes a BGR row 54 active bytes plus 2 padding bytes.
    width, height, stride = 18, 10, 56
    rows = []
    for y in range(height):
        color = bytes((0, 0, 255)) if y < 5 else bytes((255, 0, 0))
        rows.append(bytes((0, 255, 0)) + color * 17 + b'\0\0')
    body = b''.join(rows if top_down else reversed(rows))
    header = struct.pack('<2sIHHI', b'BM', 54+len(body), 0, 0, 54)
    header += struct.pack('<IiiHHIIiiII', 40, width, -height if top_down else height,
                          1, 24, 0, stride*height, 0, 0, 0, 0)
    path.write_bytes(header + body)

for top_down in (True, False):
    source = work / f'中文 彩色{top_down}.bmp'
    output = source.with_suffix('.png')
    make_bmp(source, top_down)
    before = hashlib.sha256(source.read_bytes()).digest()
    text = run(exe, ['--video-frame', source, output])
    assert 'decoded_frames=1' in text
    width, height, rows = read_png(output)
    assert (width, height) == (18, 10)
    assert rows[0][:6] == bytes((0,255,0,255,0,0)), rows[0][:6]
    assert rows[-1][:6] == bytes((0,255,0,0,0,255)), rows[-1][:6]
    assert hashlib.sha256(source.read_bytes()).digest() == before
    assert 'png' in run(exe, ['--probe', output])
checks.append('Chinese paths, RGB channel order, BMP row padding and top/bottom orientation; input unchanged')

multi = work / 'multi.avi'
run(fixture_exe, ['--make-probe-fixture', multi])
png = work / 'yuv.png'
assert 'decoded_frames=2' in run(exe, ['--video-frame', multi, png])
width, height, rows = read_png(png)
assert (width, height) == (16, 16)
assert all(abs(value - 130) <= 3 for row in rows for value in row)
checks.append('YUV420 neutral-color conversion, two decoded frames and skipping two audio tracks')

delayed = work / 'delayed.avi'
run(fixture_exe, ['--make-delayed-video', delayed])
png = work / 'delayed.png'
assert 'decoded_frames=8' in run(exe, ['--video-frame', delayed, png])
assert read_png(png)[:2] == (16, 16)
checks.append('Eight MPEG4 frames including B-frame delay; decode/drain reaches expected count')

preserved = png.read_bytes()
run(exe, ['--video-frame', delayed, png], 1)
assert png.read_bytes() == preserved
broken = work / 'broken.avi'
broken.write_bytes(b'broken media input')
audio = work / 'audio.wav'
with wave.open(str(audio), 'wb') as out:
    out.setparams((1, 2, 8000, 0, 'NONE', 'not compressed'))
    out.writeframes(b'\0\0' * 80)
for source in [broken, audio, work / 'missing.avi', work]:
    output = work / 'must-not-publish.png'
    assert run(exe, ['--video-frame', source, output], 1).strip()
    assert not output.exists()
run(exe, ['--video-frame', delayed, work/'missing-directory'/'out.png'], 1)
run(exe, ['--video-frame', delayed, work/'wrong.jpg'], 1)
run(exe, ['--video-frame', delayed], 2)
run(fixture_exe, [work / 'lifecycle.wav'])
assert not list(work.glob('*.part-*'))
checks.append('Existing output preserved, invalid/audio-only inputs, extension/path errors, pre-cancel and cleanup')

report = {'status': 'PASS', 'checks': checks}
(work/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps(report, ensure_ascii=False, indent=2))
print('Artifacts:', work)
