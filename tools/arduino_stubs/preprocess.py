#!/usr/bin/env python3
"""Approximate the Arduino builder's .ino preprocessing for a host type-check.

The Arduino builder concatenates the sketch and inserts a forward declaration
for every function, which is why a .ino may call a function defined further
down. Plain g++ does not, so the prototypes are generated here.

Prototypes are extracted from REAL PREPROCESSOR OUTPUT rather than from the
raw text. The sketches select ESP-NOW callback signatures with #if on the core
version, so the raw text contains two definitions of each callback and its
brace depth does not balance. Running cpp first resolves the branch actually
taken and makes both the prototype set and the depth tracking correct.

Usage:  preprocess.py <sketch.ino> <stub_dir> [extra -D flags...]
Writes the compilable translation unit to stdout.
"""
import os
import re
import subprocess
import sys
import tempfile

DROP_ANGLE = re.compile(
    r'^\s*#include\s*<(LilyGoWatch|esp_now|esp_system|esp_wifi|EEPROM|Wire|WiFi|'
    r'WiFiMulti|HTTPClient|WiFiClientSecure|WebSocketsClient|ESPmDNS|esp_timer|'
    r'Arduino|esp_adc_cal)\.h>')
DROP_QUOTE = re.compile(
    r'^\s*#include\s*"(ICM_20948|Arduino|WiFi|ESPAsyncWebServer|AsyncTCP|esp_adc_cal)\.h"')
DROP_DEFINE = re.compile(r'^\s*#define\s+LILYGO_WATCH_2019_WITH_TOUCH')

FUNC = re.compile(
    r'^((?:[A-Za-z_][A-Za-z0-9_:<>]*\s+)+[\*&]?\s*)([A-Za-z_][A-Za-z0-9_]*)\s*'
    r'\(([^;{)]*)\)\s*\{\s*$')

SKIP = {'if', 'for', 'while', 'switch', 'catch', 'else', 'do', 'return', 'setup', 'loop'}


def strip_includes(path):
    out = []
    with open(path, encoding='utf-8', errors='replace') as fh:
        for ln in fh:
            ln = ln.rstrip('\n')
            if DROP_ANGLE.match(ln) or DROP_QUOTE.match(ln) or DROP_DEFINE.match(ln):
                continue
            out.append(ln)
    return out


def prototypes(body, stub_dir, sketch_dir, defines):
    """Extract prototypes from cpp output so #if branches are already resolved."""
    with tempfile.TemporaryDirectory() as td:
        stage = os.path.join(td, 'stage1.cpp')
        with open(stage, 'w') as fh:
            fh.write('#include "mesq_stubs.h"\n')
            fh.write('\n'.join(body))
        # No -P: the line markers are needed to tell which regions came from
        # the sketch itself. Functions defined in the included headers must
        # NOT be prototyped again -- they already have declarations, and
        # re-declaring one that carries a default argument is an error.
        cmd = ['g++', '-std=c++17', '-E', '-DMESQ_STUB_BUILD',
               '-I' + stub_dir, '-I' + sketch_dir] + defines + [stage]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            # Preprocessing failed; let the real compile report it properly.
            return [], res.stderr
        text = res.stdout

    protos, seen, depth = [], set(), 0
    in_sketch = False
    marker = re.compile(r'^#\s+\d+\s+"([^"]*)"')
    for ln in text.split('\n'):
        m = marker.match(ln)
        if m:
            in_sketch = m.group(1).endswith('stage1.cpp')
            continue
        if in_sketch and depth == 0:
            m = FUNC.match(ln)
            if m:
                ret, name, args = m.group(1).strip(), m.group(2), m.group(3)
                if name not in SKIP and name not in seen:
                    seen.add(name)
                    # A default argument may appear only once. The definition
                    # further down carries it, so the prototype must not.
                    clean = ','.join(a.split('=')[0].rstrip() for a in args.split(','))
                    protos.append('%s %s(%s);' % (ret, name, clean))
        depth += ln.count('{') - ln.count('}')
        if depth < 0:
            depth = 0
    return protos, None


def main():
    sketch, stub_dir = sys.argv[1], sys.argv[2]
    defines = sys.argv[3:]
    sketch_dir = os.path.dirname(os.path.abspath(sketch))

    body = strip_includes(sketch)
    protos, err = prototypes(body, stub_dir, sketch_dir, defines)
    if err:
        sys.stderr.write(err)

    sys.stdout.write('#include "mesq_stubs.h"\n')
    sys.stdout.write('// --- auto-generated prototypes (Arduino builder behaviour) ---\n')
    sys.stdout.write('\n'.join(protos))
    sys.stdout.write('\n// --- sketch ---\n')
    sys.stdout.write('\n'.join(body))
    sys.stdout.write('\n')


if __name__ == '__main__':
    main()
