import numpy as np

# Parameters
FS = 600e6          # Sample Rate (600 MHz)
F_START = 100e6     # Start Frequency
F_STOP = 500e6      # Stop Frequency
DURATION = 10e-6    # 10 microseconds
GAIN_TILT = 0.3     # 30% boost at the high end

def generate_compensated_chirp():
    n_samples = int(FS * DURATION)
    t = np.linspace(0, DURATION, n_samples, endpoint=False)

    # 1. Calculate Phase
    k = (F_STOP - F_START) / DURATION
    phase = 2 * np.pi * (F_START * t + 0.5 * k * t**2)

    # 2. Apply Gain Pre-Emphasis
    amplitude_envelope = 1.0 + (GAIN_TILT * (t / DURATION))

    # 3. Generate Sine Wave
    raw_chirp = np.sin(phase) * amplitude_envelope

    # 4. Manual Tukey Window (alpha=0.1)
    # We do this manually to avoid the scipy version conflict
    alpha = 0.1
    width = int(alpha * n_samples / 2)
    window = np.ones(n_samples)
    # Taper the start and end
    window[:width] = 0.5 * (1 + np.cos(np.pi * (np.linspace(0, 1, width) - 1)))
    window[-width:] = 0.5 * (1 + np.cos(np.pi * np.linspace(0, 1, width)))
    
    windowed_chirp = raw_chirp * window

    # 5. Quantize to 8-bit (0-255)
    # Mapping [-1.3, 1.3] to [0, 255]
    quantized = ((windowed_chirp + 1.3) / 2.6 * 255).astype(np.uint8)

    return quantized

if __name__ == "__main__":
    data = generate_compensated_chirp()
    # Save as raw binary
    with open("chirp.bin", "wb") as f:
        f.write(data.tobytes())
    
    # Fixed print syntax for older python3 versions
    print("Generated {0} bytes of chirp data.".format(len(data)))
