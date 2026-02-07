import numpy as np
import struct
import os

def generate_wavetable(name="default", length=1024, type="sine"):
    """Generates a 16-bit PCM wavetable and saves as a parameterized .bin."""
    x = np.linspace(0, 2 * np.pi, length, endpoint=False)
    
    if type == "sine":
        data = (np.sin(x) * 32767).astype(np.int16)
    elif type == "saw":
        data = ((((x / np.pi) + 1) % 2 - 1) * 32767).astype(np.int16)
    # Add square/tri as needed...

    filename = f"wt_{name}_{type}_{length}.bin"
    filepath = os.path.join("bin_output", filename)
    
    os.makedirs("bin_output", exist_ok=True)
    with open(filepath, "wb") as f:
        f.write(data.tobytes())
    
    print(f"Generated: {filepath}")
    return filepath