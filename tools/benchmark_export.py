"""Repeatable S03 CPU baseline; numbers are observations, never a promised speedup."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('exe', type=Path)
parser.add_argument('fixture_exe', type=Path)
parser.add_argument('--input', type=Path, help='Optional representative private input; never uploaded')
parser.add_argument('--repeats', type=int, default=3)
args = parser.parse_args()
assert 2 <= args.repeats <= 20
exe, fixture = args.exe.resolve(), args.fixture_exe.resolve()
work = Path(__file__).resolve().parents[1] / 'workspace' / f'export-benchmark-{time.time_ns()}'
work.mkdir(parents=True)
source = args.input.resolve() if args.input else work / 'synthetic-motion.nut'
if not args.input:
    subprocess.run([str(fixture), '--make-benchmark-fixture', str(source)], check=True, capture_output=True, timeout=60)
source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
records, summaries = [], []
baseline = None
for threads in [1, 2]:
    group = []
    for iteration in range(args.repeats + 1):  # 第0次预热，不纳入统计
        output = work / f'threads-{threads}-run-{iteration}.mp4'
        command = [str(exe), '--export', str(source), str(output), '--size', '640', '360',
                   '--fps', '30', '--start', '0', '--duration', '3', '--threads', str(threads)]
        started = time.perf_counter()
        process = subprocess.run(command, capture_output=True, timeout=120)
        wall = time.perf_counter() - started
        stdout = process.stdout.decode('utf-8', errors='replace')
        (work/f'run-{threads}-{iteration}.log').write_bytes(process.stdout + process.stderr)
        assert process.returncode == 0, process.stderr.decode('utf-8', errors='replace')
        metrics = json.loads(re.search(r'METRICS_BEGIN\s*(\{.*?\})\s*METRICS_END', stdout, re.S)[1])
        check = subprocess.run([str(fixture), '--inspect-export', str(output)], capture_output=True, timeout=60, check=True)
        inspected = json.loads(check.stdout)
        assert inspected['video_frames'] == metrics['video_frames'] == 90
        assert abs(inspected['video_end']-3) < .001 and abs(inspected['audio_end']-3) < .001
        if baseline is None: baseline = inspected
        assert abs(inspected['first_y']-baseline['first_y']) < 2 and abs(inspected['last_y']-baseline['last_y']) < 2
        record = {'threads': threads, 'iteration': iteration, 'warmup': iteration == 0,
                  'process_wall_seconds': wall, 'metrics': metrics, 'inspection': inspected}
        records.append(record)
        if iteration: group.append(record)
    summaries.append({'threads': threads, 'runs': len(group),
        'median_process_wall_seconds': statistics.median(x['process_wall_seconds'] for x in group),
        'min_process_wall_seconds': min(x['process_wall_seconds'] for x in group),
        'max_process_wall_seconds': max(x['process_wall_seconds'] for x in group),
        'median_api_seconds': statistics.median(x['metrics']['total_seconds'] for x in group),
        'median_cpu_seconds': statistics.median(x['metrics']['cpu_seconds'] for x in group),
        'median_peak_working_set_bytes': statistics.median(x['metrics']['peak_working_set_bytes'] for x in group),
        'median_decode_seconds': statistics.median(x['metrics']['decode_seconds'] for x in group),
        'median_filter_seconds': statistics.median(x['metrics']['filter_seconds'] for x in group),
        'median_encode_seconds': statistics.median(x['metrics']['encode_seconds'] for x in group)})
assert hashlib.sha256(source.read_bytes()).hexdigest() == source_hash
report = {'status': 'PASS', 'scenario': '640x360/30fps, first 3 seconds, H264 CRF23 medium zerolatency + AAC',
    'input_kind': 'user-provided' if args.input else 'synthetic low-complexity 10-second motion pattern, not representative video',
    'input_sha256': source_hash, 'input_bytes': source.stat().st_size,
    'executable_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
    'platform': platform.platform(), 'cpu': os.environ.get('PROCESSOR_IDENTIFIER', platform.processor()),
    'logical_cpus': os.cpu_count(), 'timing': 'warmup excluded; process wall includes launch/cleanup; API stages measure call wall time',
    'limits': 'thread option changes codec/filter shared setting; no GPU or PSNR/SSIM measurement; not a universal performance claim',
    'summaries': summaries, 'runs': records}
(work/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(summaries,ensure_ascii=False,indent=2))
print('Artifacts:',work)
