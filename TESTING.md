# RTT Communication Testing Guide

## Quick Start - Verify RTT is Working

### Step 1: Prepare Hardware
1. Connect J-Link debugger to RP2350 via SWD (2-wire: SWDIO + SWCLK)
2. Power the board via J-Link or USB
3. Ensure board LED (GP3) blinks or turns on (indicates firmware running)

### Step 2: Flash Firmware
```bash
cd d:\dev\snakeSnoil\code\playground\HSTX_DDS\hstxDDS
# Option A: Use VS Code task
# Click: Terminal > Run Task > "Run Project"

# Option B: Manual flash
picotool load build/hstxDDS.uf2 -fx
```

### Step 3: Debug RTT Output
Open J-Link RTT terminal to see debug messages:

**Windows (Command Prompt):**
```batch
jlink.exe -SelectInterface SWD
jlink> connect RP2350_M33_0
jlink> rtt
# You should see RTT terminal output here
```

Or use VS Code with Cortex Debug extension:
- Open Run & Debug (Ctrl+Shift+D)
- Select "Debug with OpenOCD" or similar

**Expected output when RTT connects:**
```
SEGGER RTT terminal ready
(RTT terminal running...)
```

### Step 4: Generate Test Waveform
```bash
cd configurator

# Create a simple sine waveform (1024 samples, 8-bit unsigned)
python chirp.py --output test_sine.bin --freq 1000 --length 1024
# Output: test_sine.bin (1KB, values 0-255)

# Verify file exists
dir test_sine.bin
# Should show ~1024 bytes
```

### Step 5: Send Waveform via RTT Configurator
```bash
cd configurator
python config.py
```

**In the Python prompt:**
```
(pico) list_bins
Available profiles:
  - test_sine.bin

(pico) send_dds 0 100000 100000 0
  Waveform OK: 0-255 (8-bit)
Sent test_sine.bin (1024 bytes) @ 100000Hz to target.
  Sweep: 100000Hz -> 100000Hz over 0ms

(pico) exit
```

### Step 6: Verify Output on Logic Analyzer
Connect logic analyzer to GPIO 12-19 (8 channels):

- **Expected waveform:** Repeating 8-bit data pattern (0x00...0xFF cyclically for sine)
- **Frequency:** Data changes at ~100 kHz (as specified in fstart)
- **Duration:** Continuous until power off or new command

**If you see data on GPIO 12-19, RTT communication is working!**

---

## Detailed Test Cases

### Test 1: Protocol Verification
**Objective:** Confirm correct 20-byte header format

```python
# In Python, verify the header is built correctly:
import struct
header = struct.pack('<IIIII', 0xDEADBEEF, 100000, 100000, 0, 1024)
print(f"Header length: {len(header)} bytes")  # Should be 20
print(f"Header hex: {header.hex()}")
# Should start with: efbeadde (0xDEADBEEF in LE)
```

### Test 2: RTT Channel 0 (Header)
**Objective:** Verify header arrives at firmware

**Setup:** Add debug output in `process_mailbox()` READ_HEADER state

**Expected firmware output:**
```
Header OK: 100000->100000 Hz, 0 ms, len=1024
```

### Test 3: RTT Channel 1 (Data)
**Objective:** Verify waveform data transfers correctly

**Setup:** Use `send_dds` command

**Verification:**
1. Check RTT terminal for: `DDS: Success 1024 bytes @ 100000 Hz`
2. Probe GPIO 12-19 with logic analyzer
3. Expect to see 8-bit samples repeating at 100 kHz clock

### Test 4: Waveform Format Validation
**Objective:** Confirm 8-bit unsigned validation works

**Test cases:**
```bash
# Good waveform (8-bit unsigned)
python config.py
(pico) send_dds 0 100000 100000 0  # Should say "Waveform OK"

# Bad waveform (if you had one with values > 255)
# Should warn: "Warning: Waveform values out of range"
```

### Test 5: Timeout Recovery
**Objective:** Test that firmware recovers from stuck transfers

**Setup:** 
1. Start `send_dds` command
2. Kill USB connection before data completes
3. Reconnect J-Link
4. Try another `send_dds` command

**Expected:** Firmware resets to IDLE after 500ms timeout and accepts new header

---

## Troubleshooting

### Issue: "RTT control block not found"
**Cause:** RTT not initialized in firmware  
**Solution:**
- Verify `dds_init_rtt()` is called in `main()`
- Check firmware was recompiled and flashed after changes
- Try rebuild: `cmake --build build`

### Issue: Header received but no "DDS: Success" message
**Cause:** Data not arriving on Channel 1, or length mismatch

**Debug steps:**
1. Verify `wlen` in config.py matches actual file size
2. Check `header_pos == 20` in firmware before entering READ_DATA
3. Add debug: print `data_pos` and `incoming_header.len` in firmware

### Issue: Data on GPIO but wrong frequency
**Cause:** Clock divider calculation incorrect

**Check:**
- System clock: Should be 125 MHz on RP2350
- Divider formula: `div = sys_clk / freq_hz`
- For 100 kHz: `div = 125,000,000 / 100,000 = 1250`

### Issue: Python "No .bin files found"
**Cause:** Working directory or path issue

**Solution:**
```bash
cd configurator  # Must be in this directory
python config.py
```

Or fix bin_dir in config.py:
```python
self.bin_dir = os.path.join(self.base_dir, "bin_output")
```

---

## Manual RTT Test (No Configurator)

If you want to test RTT directly via J-Link terminal:

```bash
# In J-Link RTT terminal:
rtt> write0 efbeaddefa0101001e000003000004000000  
# Sends 20-byte header (little-endian)
# = 0xDEADBEEF + fstart=0x0101fa (65018 Hz) + fend=0x00001e + ... + len=4
```

This is useful to isolate whether the problem is:
- RTT communication itself
- Python configurator
- Waveform generation

---

## Next Steps

Once RTT communication is verified:

1. **Generate real waveforms:** Use `chirp.py` to create sine, chirp, or arbitrary waveforms
2. **Measure output:** Logic analyzer on GPIO 12-19 to confirm waveform quality
3. **Implement sweep:** Add timer-based frequency ramp logic to firmware for fend/duration
4. **Benchmark:** Test max frequency (~15 MHz) and minimum (depends on DMA + HSTX overhead)

---

**Questions?** Check [README.md](README.md) for detailed protocol & hardware info.
