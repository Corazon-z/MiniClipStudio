"""S01 probe acceptance without microphone or ffmpeg command line."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
import time
import wave

exe = Path(sys.argv[1]).resolve()
fixture_exe = Path(sys.argv[2]).resolve()
work = Path(__file__).resolve().parents[1] / 'workspace' / f'probe-check-{time.time_ns()}'
work.mkdir(parents=True)
checks = []
def run(program, args, code=0):
    result = subprocess.run([str(program), *map(str, args)], capture_output=True, timeout=30)
    report = (result.stdout + result.stderr).decode('utf-8', errors='replace')
    assert result.returncode == code, (args, result.returncode, report)
    return report

audio = work / '中文 音频.wav'
with wave.open(str(audio), 'wb') as out:
    out.setparams((1, 2, 8000, 0, 'NONE', 'not compressed'))
    out.writeframes(b'\0\0' * 8000)
before = hashlib.sha256(audio.read_bytes()).hexdigest()
report = run(exe, ['--probe', audio])
for expected in ['Container (容器): wav', 'Streams (轨道数量): 1', 'Stream #0 (轨道): audio',
                 'pcm_s16le', '8000 Hz', 'Channels (声道数量): 1', '1.000000 s']:
    assert expected in report, (expected, report)
assert hashlib.sha256(audio.read_bytes()).hexdigest() == before
checks.append('Unicode/spaced path, WAV metadata, read-only content')
renamed = work / 'audio-without-media-extension.dat'
renamed.write_bytes(audio.read_bytes())
assert 'pcm_s16le' in run(exe, ['--probe', renamed])
checks.append('Detect by content instead of extension')
video = work / 'three-streams.avi'
run(fixture_exe, ['--make-probe-fixture', video])
report = run(exe, ['--probe', video])
for expected in ['Streams (轨道数量): 3', 'Stream #0 (轨道): video', 'Stream #1 (轨道): audio',
                 'Stream #2 (轨道): audio', '16x16', '2.000 frame/s', 'rawvideo', 'yuv420p']:
    assert expected in report, (expected, report)
checks.append('Video + two audio streams, dimensions, pixel format and frame rate')
corrupt = work / 'broken.wav'
corrupt.write_bytes(b'not a valid media file')
for path in [work / 'missing.wav', work, corrupt]:
    text = run(exe, ['--probe', path], code=1)
    assert text.strip()
run(exe, ['--probe'], code=2)
checks.append('Missing/directory/corrupt input and invalid arguments')
run(fixture_exe, [work / 'lifecycle.wav'])
checks.append('Unknown formatting, pre-cancel, ownership and lifecycle regression')
result = {'status': 'PASS', 'checks': checks}
(work / 'report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps(result, ensure_ascii=False, indent=2))
print('Artifacts:', work)
