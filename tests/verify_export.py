"""S03 independent content/timestamp/cancellation verification; no FFmpeg command line."""
from pathlib import Path
import hashlib
import json
import math
import re
import struct
import subprocess
import sys
import time
import wave

exe, fixture = [Path(x).resolve() for x in sys.argv[1:3]]
work = Path(__file__).resolve().parents[1] / 'workspace' / f'export-check-{time.time_ns()}'
work.mkdir(parents=True)
checks, outputs = [], []

def run(program, args, code=0):
    process = subprocess.run([str(program), *map(str, args)], capture_output=True, timeout=60)
    text = (process.stdout + process.stderr).decode('utf-8', errors='replace')
    assert process.returncode == code, (args, process.returncode, text)
    return text

def export(source, name, options=()):
    path = work / name
    text = run(exe, ['--export', source, path, *options])
    metrics = json.loads(re.search(r'METRICS_BEGIN\s*(\{.*?\})\s*METRICS_END', text, re.S)[1])
    assert metrics['total_seconds'] > 0 and metrics['output_bytes'] == path.stat().st_size
    assert metrics['peak_working_set_bytes'] > 0
    inspect = json.loads(run(fixture, ['--inspect-export', path]))
    assert inspect['video_frames'] == metrics['video_frames']
    outputs.append({'file': name, 'metrics': metrics, 'inspection': inspect})
    return path, metrics, inspect

def read_pcm(path):
    # WAV output may be extensible and contain extra chunks; walk RIFF, don't assume 44 bytes.
    data = path.read_bytes()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
    offset, pcm = 12, None
    while offset + 8 <= len(data):
        tag, size = struct.unpack_from('<4sI', data, offset)
        body = data[offset+8:offset+8+size]
        if tag == b'fmt ':
            _, channels, rate, _, _, bits = struct.unpack_from('<HHIIHH', body)
            assert (channels, rate, bits) == (2, 48000, 16)
        if tag == b'data': pcm = body
        offset += 8 + size + size % 2
    assert pcm is not None
    return [x[0] for x in struct.iter_unpack('<hh', pcm)]

def frequency(samples):
    crossings = sum(a <= 0 < b for a, b in zip(samples, samples[1:]))
    return crossings * 48000 / len(samples)

source = work / '中文 双音轨.nut'
run(fixture, ['--make-export-fixture', source])
original_hash = hashlib.sha256(source.read_bytes()).hexdigest()
full, metrics, inspection = export(source, '完整.mp4')
assert (metrics['video_frames'], metrics['decoded_video_frames'], metrics['audio_samples']) == (60, 50, 96000)
assert metrics['audio_tracks'] == 1
assert abs(inspection['video_end']-2) < .001 and abs(inspection['audio_end']-2) < .001
assert abs(inspection['first_y']-48) < 4 and abs(inspection['last_y']-146) < 4
probe = run(exe, ['--probe', full])
for value in ['h264', 'aac', 'Streams (轨道数量): 2', '48000 Hz', 'Channels (声道数量): 2']:
    assert value in probe, (value, probe)
decoded = work/'decoded.wav'
run(exe, ['--convert', full, decoded])
samples = read_pcm(decoded)
assert abs(frequency(samples[4800:24000])-440) < 6
assert abs(frequency(samples[60000:85000])-880) < 6  # 第一轨先440后880，不是第二轨1760Hz
checks.append('H264/AAC output, first audio selection, real video luminance, audio frequencies, timestamps and EOF')

silent, metrics, inspection = export(source, 'silent.mp4', ['--no-audio','--size',32,24,'--fps',20,'--start',1,'--duration',.6])
assert (metrics['video_frames'], metrics['audio_tracks'], metrics['width'], metrics['height']) == (12,0,32,24)
assert abs(inspection['video_end']-.6) < .001 and abs(inspection['first_y']-98) < 5
assert 'Streams (轨道数量): 1' in run(exe, ['--probe', silent])
clip, metrics, inspection = export(source, 'clip.mp4', ['--size',32,24,'--fps',20,'--start',1.1,'--duration',.55])
assert (metrics['video_frames'], metrics['audio_samples']) == (11,26400)
assert abs(inspection['video_end']-.55) < .001 and abs(inspection['audio_end']-.55) < .001
assert abs(inspection['first_y']-104) < 5
clip_wav = work/'clip.wav'; run(exe, ['--convert',clip,clip_wav]); pcm=read_pcm(clip_wav)
assert abs(frequency(pcm[2400:22000])-880) < 6
# AAC解码尾部可能带最多一个编码块的padding；内容样本数以过滤器统计/MP4轨道时长分别验证。
assert 26400 <= len(pcm) <= 26400+1024
checks.append('Resize/CFR, silent export, non-keyframe re-encode trim, exact audio content count and clipped packet ends')

custom, metrics, _ = export(source, 'mono44100.mp4', ['--sample-rate',44100,'--channels',1,'--fps',25])
assert metrics['audio_samples'] == 88200 and metrics['video_frames'] == 50
assert '44100 Hz' in run(exe, ['--probe', custom])
offset = work/'offset.nut'; run(fixture,['--make-export-offset-fixture',offset])
offset_out, metrics, _ = export(offset,'offset.mp4',['--duration',1])
assert metrics['audio_samples'] == 48000
offset_wav = work/'offset.wav';run(exe,['--convert',offset_out,offset_wav]);pcm=read_pcm(offset_wav)
assert math.sqrt(sum(x*x for x in pcm[:6000])/6000) < 30
assert math.sqrt(sum(x*x for x in pcm[14000:20000])/6000) > 1000
checks.append('Configurable sample rate/channels and preservation of delayed-audio offset with leading silence')

delayed = work/'bframes.nut';run(fixture,['--make-delayed-video',delayed])
_, metrics, inspection = export(delayed,'bframes.mp4',['--fps',4])
# 此NUT样本因编码重排序有0.25秒呈现偏移；输出按声明策略在零点保持首帧，故8帧输入得到9帧CFR。
assert metrics['decoded_video_frames'] == 8
assert metrics['video_frames'] == 9 and abs(inspection['video_end']-2.25) < .001
assert abs(inspection['last_y']-172) < 4
legacy = work/'legacy-missing-pts.avi';run(fixture,['--make-delayed-video',legacy])
assert 'timestamp' in run(exe,['--export',legacy,work/'must-not-publish.mp4','--fps',4],1)
assert not (work/'must-not-publish.mp4').exists()
run(fixture,['--test-export-cancel',source,work/'cancel.mp4'])
before=full.read_bytes();run(exe,['--export',source,full],1);assert full.read_bytes()==before
for options in [['--size',31,24],['--fps',0],['--start','nan'],['--start',10],['--duration',-1],['--sample-rate',12345],['--channels',6]]:
    target=work/'bad.mp4';run(exe,['--export',source,target,*options],1);assert not target.exists()
bad=work/'broken.nut';bad.write_bytes(b'broken')
for path in [bad,work/'missing.nut',work]:
    run(exe,['--export',path,work/'bad.mp4'],1)
audio=work/'audio.wav'
with wave.open(str(audio),'wb') as out:
    out.setparams((1,2,8000,0,'NONE','not compressed'));out.writeframes(b'\0\0'*80)
run(exe,['--export',audio,work/'bad.mp4'],1)
run(exe,['--export',source,work/'missing-folder'/'out.mp4'],1)
run(exe,['--export',source,work/'wrong.avi'],1)
run(exe,['--export'],2)
assert hashlib.sha256(source.read_bytes()).hexdigest()==original_hash
assert not list(work.glob('*.part-*'))
checks.append('B-frame input drain, pre/in-flight cancellation, no overwrite, invalid inputs/options and cleanup')
report={'status':'PASS','checks':checks,'outputs':outputs}
(work/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({'status':'PASS','checks':checks},ensure_ascii=False,indent=2))
print('Artifacts:',work)
