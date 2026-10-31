# Third-party code in DFORM

Everything the module ships is MIT-licensed. Nothing copyleft is compiled in or
linked (`dsp.so` needs only `libc` and `libm`; `scripts/check_abi.py` checks the
build), so there is no conflict with Schwung, whose source is MIT.

The sound engine (`dsp/bk_engine.c`: oscillators, FM, ladder filter,
envelopes) is original code written for this module; no code from any
synthesizer or its firmware is used.

## The phrase engine and source MIDI files — the author's own work

`dsp/phrase/` (the MIDI loader, analyser, generator, scale quantizer and
sequencer) and `patterns/` (the six source MIDI files) are the author's own
work (Hiro Ikezawa), under this repository's MIT licence. One change in
`pg_sequencer.c`, marked in the source, makes a queued pattern swap on the
longer of the old and new lengths. DFORM uses only the loader, analyser
and generator, for the RANDOM pad (`dsp/bk_phrase.c`); the sequencer is
compiled but unused.

## Schwung — MIT

`third_party/schwung/host/plugin_api_v1.h` is Schwung's plugin API header
(Copyright 2025-2026 Charles Vestal, MIT), vendored unmodified. Its licence (also in
`third_party/schwung/LICENSE`):

```
MIT License

Copyright (c) 2025-2026 Charles Vestal

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

