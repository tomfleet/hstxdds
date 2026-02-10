import os
import cmd
import struct
import pylink

class PicoController(cmd.Cmd):
    prompt = '(pico) '

    def __init__(self):
        super().__init__()
        # Fix directory: Anchor to where config.py actually lives
        self.base_dir = os.path.dirname(os.path.abspath(__file__))
        self.bin_dir = self.base_dir 
        
        try:
            self.jlink = pylink.JLink()
            self.jlink.open() # Ensure the session is open
            self.jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
            self.jlink.connect('RP2350_M33_0')
            print(f"J-Link Connected. Root: {self.base_dir}")
        except Exception as e:
            print(f"Connection Error: {e}")

    def write_rtt_byte(self, char_byte):
        """Uses the built-in rtt_write for the active session."""
        try:
            # We use the established J-Link session to push to Channel 0
            self.jlink.rtt_write(0, char_byte)
        except Exception as e:
            print(f"RTT Write Error: {e}")

    def do_list_bins(self, arg):
        """List .bin files in the same directory as this script."""
        try:
            bins = [f for f in os.listdir(self.bin_dir) if f.endswith('.bin')]
            if not bins:
                print(f"No .bin files found in: {self.bin_dir}")
                return
            print(f"Available profiles in {self.bin_dir}:")
            for b in bins:
                print(f"  - {b}")
        except Exception as e:
            print(f"Path Error: {e}")

    def do_led(self, line):
        """Usage: led on | off | toggle"""
        args = line.split()
        if not args:
            print("Usage: led on | off | toggle")
            return

        cmd_map = {"on": b'1', "off": b'0', "toggle": b't'}
        char = cmd_map.get(args[0].lower())
        
        if char:
            self.write_rtt_byte(char)
            print(f"Sent {args[0].upper()} to RTT.")
        else:
            print(f"Unknown command: {args[0]}")

    def do_send_dds(self, line):
        """Usage: send_dds <bin_name> <freq_hz>"""
        args = line.split()
        if len(args) < 2:
            print("Usage: send_dds chrip.bin 1000000")
            return

        filename = os.path.join(self.bin_dir, args[0])
        try:
            freq = int(args[1])
            # Pack as Little-Endian: 'B' for 'Binary Start' cmd, 'I' for uint32 freq
            header = struct.pack('<BI', ord('B'), freq)
            
            with open(filename, 'rb') as f:
                blob = f.read()
            
            # Send Header + Binary Data
            self.jlink.rtt_write(0, header + blob)
            print(f"Uploaded {len(blob)} bytes to target at {freq}Hz.")
        except Exception as e:
            print(f"Transfer Error: {e}")