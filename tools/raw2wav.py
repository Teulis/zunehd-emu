"""Wrap raw s16 stereo 44.1 kHz PCM as a WAV file: py -3 -I tools/raw2wav.py in.raw out.wav"""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
h = (b'RIFF' + struct.pack('<I', 36 + len(d)) + b'WAVEfmt '
     + struct.pack('<IHHIIHH', 16, 1, 2, 44100, 44100 * 4, 4, 16) + b'data' + struct.pack('<I', len(d)))
open(sys.argv[2], 'wb').write(h + d)
