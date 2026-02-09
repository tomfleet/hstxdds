# Quick Reference - RP2350 DDS Control

## Command Cheat Sheet

### Compile & Flash
```bash
# Build firmware
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Flash to device
picotool load build/hstxDDS.uf2 -fx

# Or use VS Code: Terminal > Run Task > "Run Project"
```

### Generate Test Waveform
```bash
cd configurator
python chirp.py --output sine_1k.bin --freq 1000 --length 1024
# Creates 1KB sine waveform at 1 kHz tone
```

### Run Python Configurator
```bash
cd configurator
python config.py

# In the shell:
(pico) list_bins                           # List available waveforms
(pico) send_dds 0 100000 100000 0          # Send waveform at 100 kHz
(pico) led on                              # Control LED
(pico) exit                                # Disconnect
```

### RTT Debug Terminal
```bash
jlink.exe -SelectInterface SWD
jlink> connect RP2350_M33_0
jlink> rtt                  # Open RTT terminal
# You'll see firmware debug messages here
```

---

## Header Format (20 bytes)

```python
import struct

# Build header for 1024-byte waveform at 100 kHz
header = struct.pack(
    '<IIIII',
    0xDEADBEEF,      # Sync marker (required!)
    100000,          # fstart (Hz)
    100000,          # fend (Hz, for sweep—not yet used)
    0,               # duration (ms, for sweep—not yet used)
    1024             # waveform length in bytes
)
# Result: 20 bytes, sent to RTT Channel 0
```

---

## Expected Success Messages

**Terminal output (Python configurator):**
```
(pico) send_dds 0 100000 100000 0
  Waveform OK: 0-255 (8-bit)
Sent sine_1k.bin (1024 bytes) @ 100000Hz to target.
  Sweep: 100000Hz -> 100000Hz over 0ms
```

**RTT debug output (J-Link):**
```
Header OK: 100000->100000 Hz, 0 ms, len=1024
DDS: Success 1024 bytes @ 100000 Hz
```

**GPIO output (Logic Analyzer):**
- Pins GP12-GP19: 8-bit data cycling at 100 kHz
- Pattern: Repeating sine wave samples (0x00...0xFF)

---

## Frequency Range

| Parameter | Min | Max | Notes |
|-----------|-----|-----|-------|
| fstart | ~30 kHz | ~15 MHz | Clock divider 4095→8 |
| Sample Rate | ~30 kHz | ~125 MHz | System clock dependent |
| Waveform Length | 1 byte | 8192 bytes | Fixed 8KB buffer |

**Tone frequency example:**
```
Tone_freq = Sample_rate / Waveform_length
1 kHz tone = 1,024,000 Hz / 1,024 samples
```

---

## Pinout Quick Ref

```
GPIO 12-19 : HSTX 8-bit data out (D0-D7)
GPIO  3    : PWM reference / LED
GPIO 23-24 : UART (disabled in RTT mode)
```

---

## Troubleshooting Quick Ref

| Symptom | Check First |
|---------|------------|
| "No .bin files found" | Run from `configurator/` directory |
| RTT connection fails | J-Link must be plugged in, try re-flashing |
| No output on GPIO 12-19 | Verify RTT shows "Success" message first |
| Wrong frequency | Check fstart value in send_dds command |
| "Bad sync" error | Ensure config.py was rebuilt after fixes |

---

## File Locations

```
hstxDDS/
├── README.md              ← Full documentation
├── TESTING.md             ← Detailed test procedures  
├── RECOVERY_SUMMARY.md    ← What was fixed
├── hstxDDS.c              ← Main firmware
├── dma_dds/
│   ├── dma_dds.c          ← DDS & RTT logic
│   └── dma_dds.h          ← Structs & prototypes
└── configurator/
    ├── config.py          ← Fixed! ← Use this one
    ├── config3.py         ← Alternative (working)
    ├── config4.py         ← Alternative (working)
    ├── chirp.py           ← Waveform generator
    └── bin_output/        ← Put .bin files here
```

---

## Did It Work?

✅ **RTT Communication:**
- [ ] Python sends header without error
- [ ] RTT shows "Header OK" message
- [ ] RTT shows "DDS: Success" message

✅ **HSTX Output:**
- [ ] Logic analyzer shows data on GP12-19
- [ ] Data cycles at expected frequency
- [ ] Pattern matches waveform input

If all checks pass → **You're good!** 🎉

---

## Next: Frequency Sweep (Future Work)

Current state: `fend` and `duration` are accepted but ignored.

To enable sweep:
1. Implement timer interrupt in `apply_dds_config()`
2. Update HSTX divider every 1-10ms
3. Linear ramp: `freq(t) = fstart + (fend - fstart) * t/duration`

Example: 100 kHz → 200 kHz over 1 second will show frequency slide on spectrum analyzer.

---

*Quick ref v1.0 — Feb 2026*
