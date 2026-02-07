import pylink
import struct

def send_dds_config(f_start, f_end, ramp, mode):
    jlink = pylink.JLink()
    jlink.open()
    jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
    jlink.connect('RP2350') # Or 'Cortex-M33' depending on Segger version

    # Pack the data into the exact format the C struct expects:
    # < (Little Endian), I (uint32), I (uint32), H (uint16), B (uint8)
    data = struct.pack('<IIHB', f_start, f_end, ramp, mode)
    
    # Write to RTT Channel 0
    jlink.rtt_write(0, data)
    print(f"Pushed Sweep: {f_start}Hz -> {f_end}Hz")
    
    jlink.close()

# Example usage from your wavetable util:
# send_dds_config(1000, 20000, 500, 2)