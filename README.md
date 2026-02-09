# HSTX DDS - High-Speed Transceiver Direct Digital Synthesis

## Overview

This is a **Direct Memory Access - Direct Digital Synthesis (DMA DDS)** system for the Raspberry Pi RP2350 ARM Cortex-M33 MCU. It generates arbitrary waveforms at high speeds using:

- **HSTX (High-Speed Transceiver)** for 8-bit parallel data output (GPIO 12-19)
- **DMA (Direct Memory Access)** for zero-CPU-overhead waveform playback
- **SEGGER RTT** (Real-Time Transfer) for command/control over J-Link debugger

## Hardware Requirements

- **RP2350 MCU** (Raspberry Pi Pico 2 or RP2350 Stamp, ARM variant)
- **J-Link Debugger** (for RTT communication and power)
- **Logic Analyzer** (optional, for verification on GPIO 12-19)

### Pin Mapping

| GPIO | Function | Notes |
|------|----------|-------|
| 12-19 | HSTX Data Bus | 8-bit parallel output (GP12=D0...GP19=D7) |
| 3 | PWM Reference | LED/timing pin |
| 23-24 | UART | TX/RX (disabled in RTT mode) |

## Project Structure

```
.
├── hstxDDS.c           # Main firmware entry point
├── CMakeLists.txt      # Build configuration
├── board/
│   └── pins.h          # Hardware pin definitions
├── dma_dds/
│   ├── dma_dds.c       # Core DDS logic (HSTX, DMA, RTT state machine)
│   └── dma_dds.h       # Header and type definitions
├── configurator/
│   ├── config.py       # Main Python RTT configurator (recommended)
│   ├── config3.py      # Alternative implementation
│   ├── config4.py      # Alternative implementation
│   ├── chirp.py        # Waveform generator (sine/chirp)
│   └── bin_output/     # Binary waveform data directory
└── other/
    └── SEGGER_RTT/     # Debug output library
```

## Protocol Specification

### RTT Channel Configuration

- **Channel 0 (Terminal)**: 256 bytes, command input & debug output
- **Channel 1 (DataIn)**: 10248 bytes, binary waveform data transfer

### Header Format (20 bytes, sent to Channel 0)

The firmware expects a 20-byte header in little-endian (LE) format:

```c
struct dds_header_t {
    uint32_t sync;      // Magic: 0xDEADBEEF (synchronization marker)
    uint32_t fstart;    // Start frequency (Hz)
    uint32_t fend;      // End frequency (Hz, for sweep—not yet implemented)
    uint32_t duration;  // Ramp duration (ms, for sweep—not yet implemented)
    uint32_t len;       // Waveform data length in bytes (max 8192)
};
```

**Python Pack Format:**
```python
struct.pack('<IIIII', 0xDEADBEEF, fstart, fend, duration, len)
```

### Data Transfer Sequence

1. **Send header** (20 bytes) to RTT Channel 0
   - Firmware detects 0xEF (LSB of sync marker) and enters READ_HEADER state
   - Reads remaining 19 bytes into internal buffer
   - Validates sync marker (0xDEADBEEF)

2. **Send waveform data** (N bytes) to RTT Channel 1
   - Firmware reads `len` bytes from Channel 1
   - Stores in internal waveform buffer (max 8192 bytes)

3. **Firmware acknowledge** (debug output to Channel 0)
   - Prints success message: `"DDS: Success <N> bytes @ <FREQ> Hz"`

### Waveform Format

**Format:** 8-bit unsigned integers (0-255)

**Notes:**
- Each byte is output as-is on GPIO 12-19
- HSTX cycles through entire buffer at programmable frequency
- Frequencies up to ~15 MHz supported (125 MHz clock / 8 divider minimum)

**Constraints:**
- Max 8192 bytes per waveform
- Must be 8-bit unsigned (0-255), not 16-bit signed
- MSB (D7) appears on GP19, LSB (D0) on GP12

## Firmware Usage

### Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cd build
cmake --build .
```

Or use the VS Code task: **Compile Project**

### Flashing

```bash
# Via picotool
picotool load hstxDDS.uf2 -fx

# Or use VS Code task: Run Project
```

### Debug Output

Open SEGGER RTT terminal via J-Link:
```bash
jlink -SelectInterface SWD
jlink> connect RP2350_M33_0
jlink> rtt
```

## Python Configurator Usage

### Installation

```bash
pip install pylink-square
```

### Running the Configurator

```bash
cd configurator
python config.py
```

### Commands

#### List Available Waveforms
```
(pico) list_bins
```

#### Send Waveform with DDS Parameters
```
(pico) send_dds <bin_idx> <fstart> <fend> <ramp_ms>
```

**Example:** Send bin_output/sine_1k.bin at 100 kHz
```
(pico) send_dds 0 100000 100000 0
```

**Example:** Send chirp from 1 MHz to 2 MHz over 1 second
```
(pico) send_dds 1 1000000 2000000 1000
```

#### LED Control
```
(pico) led on|off|toggle
```

#### Exit
```
(pico) exit
```

## Frequency Calculation

The output frequency is determined by the waveform and playback rate:

$$f_{out} = \frac{f_{sample}}{N_{samples\_per\_cycle}}$$

Where:
- $f_{sample}$ = `fstart` (Hz, from header)
- $N_{samples\_per\_cycle}$ = length of one complete waveform period in buffer

**Example:** 1024-byte sine wave at 102.4 kHz sample rate → 100 Hz output tone

## Known Limitations

1. **Frequency Sweep:** `fend` and `duration` fields are accepted but not yet implemented
2. **Single Waveform:** Only one waveform can be active at a time (DMA one-shot transfer)
3. **No Stop Command:** Once started, waveform plays indefinitely until new command received
4. **8-bit Resolution:** HSTX outputs 8 bits only (not 16-bit)

## Troubleshooting

### RTT Communication Fails
- Verify J-Link is connected and recognized: `jlink -ShowVID`
- Ensure firmware is running: check LED pin (GP3) blinking
- Try rebuilding and re-flashing
- Check firmware debug output: `jlink> rtt`

### Waveform Not Outputting Data
- Verify GPIO 12-19 are probed on logic analyzer
- Check clock divider: expected ~125 MHz / fstart
- Ensure waveform binary is correct format (8-bit unsigned)

### Python "pylink" Module Conflict
If you see "The wrong 'pylink' package is installed":
```bash
pip uninstall pylink
pip install pylink-square
```

## Waveform Generation

Use the provided Python script to generate test waveforms:

```bash
python configurator/chirp.py --output sine_1k.bin --freq 1000 --length 1024
```

See `configurator/chirp.py` for more options (sine, chirp, sawtooth, etc.)

## Implementation Notes

### State Machine (Firmware)

```
IDLE
  ↓ (detect 0xEF from Channel 0)
READ_HEADER (read 19 more bytes, verify 0xDEADBEEF)
  ↓
READ_DATA (read `len` bytes from Channel 1)
  ↓ (success)
IDLE + apply_dds_config()
```

### Timeout Handling

- If header/data transfer stalls for > 500 ms, state machine resets to IDLE
- Useful for recovering from partial/corrupted transfers

## Future Enhancements

1. **Frequency Sweep Implementation:** Linear ramp from fstart→fend over duration_ms
2. **DMA Chaining:** Support multiple waveforms in sequence
3. **Streaming Mode:** Continuous waveform updates without stops
4. **Amplitude Modulation:** PWM-based amplitude control
5. **16-bit Support:** Enable wider dynamic range (requires GPIO expansion)

## References

- [RP2350 Datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
- [Pico SDK Documentation](https://raspberrypi.github.io/pico-sdk-doxygen/)
- [SEGGER RTT Documentation](https://wiki.segger.com/RTT)
- [J-Link Software and Documentation Pack](https://www.segger.com/downloads/jlink/)

## License

See repository LICENSE file (if present) or contact project owner.

---

**Last Updated:** February 2026  
**Status:** RTT communication fixed, frequency sweep pending
