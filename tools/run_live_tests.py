#!/usr/bin/env python3
"""Run explicit virtual-device-only tone/capture tests; never records a physical mic.
Requires the installed driver and build/hal_probe. Stop any app that is
currently feeding the private writer before running.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--seconds', type=int, default=8)
parser.add_argument('--output', type=Path, default=root / 'artifacts' / 'live-rerun')
args = parser.parse_args()
if not 3 <= args.seconds <= 300:
    parser.error('seconds must be 3..300')
probe = root / 'build' / 'hal_probe'
args.output.mkdir(parents=True, exist_ok=True)
subprocess.run([str(probe), 'inspect'], check=True)
cases = [('writer', ['tone', str(args.seconds + 12), '1024']),
         ('ioproc512', ['record', str(args.seconds), '512']),
         ('ioproc256', ['record', str(args.seconds), '256']),
         ('au48000', ['record-au', str(args.seconds), '48000'])]
cases += [(f'queue{rate}', ['record-queue', str(args.seconds), str(rate)])
          for rate in [8000, 16000, 32000, 44100, 48000]]
processes = []
results = {}
try:
    for name, command in cases:
        output = (args.output / (name + '.log')).open('w')
        process = subprocess.Popen([str(probe)] + command, stdout=output, stderr=subprocess.STDOUT)
        processes.append((name, process, output))
        time.sleep(0.7)
    for name, process, output in processes:
        try:
            code = process.wait(timeout=args.seconds + 35)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            code = 124
        output.close()
        results[name] = code
        print(name, 'PASS' if code == 0 else f'FAIL ({code})', flush=True)
finally:
    for _, process, output in processes:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        output.close()
(args.output / 'exit-codes.json').write_text(json.dumps(results, indent=2) + '\n')
raise SystemExit(0 if len(results) == len(cases) and all(v == 0 for v in results.values()) else 1)
