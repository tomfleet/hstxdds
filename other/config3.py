import os
import cmd
import struct
import pylink

class PicoController(cmd.Cmd):
    intro = 'Pico 2 DDS Control Interface. Type help or ? to list commands.\n'
    prompt = '(pico) '

    def __init__(self):
        super().__init__()
        # Anchor to script directory
        self.base_dir = os.path.dirname(os.path.abspath(__file__))
        self.bin_dir = self.base_dir 
        
        try:
            self.jlink = pylink.JLink()
            self.jlink.open()
            self.jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
            # Specific target for the QFN56 RP2350
            self.jlink.connect('RP2350_M33_0')
            
            # Start RTT polling - crucial for transport!
            self.jlink.rtt_start()
            print(f"Connected to RP2350. Root: {self.base_dir}")
        except Exception as e:
            print(f"Connection Error: {e}")

    def write_rtt(self, data):
        """Standardized RTT write to Channel 0."""
        try:
            self.jlink.rtt_write(0, data)
        except Exception as e:
            print(f"RTT Write Error: {e}")

    def do_led(self, line):
        """Usage: led on | off | toggle"""
        args = line.split()
        if not args:
            print("Usage: led on | off | toggle")
            return
        cmd_map = {"on": b'1', "off": b'0', "toggle": b't'}
        char = cmd_map.get(args[0].lower())
        if char:
            self.write_rtt(char)
            print(f"Sent {args[0].upper()} to RTT.")

    def do_send_dds(self, line):
        """Usage: send_dds <bin_name> <freq_hz>
        Packs: Sync(0xDEADBEEF) + Fstart + Fend + Duration + Len + Data
        """
        args = line.split()
        if len(args) < 2:
            print("Usage: send_dds <filename.bin> <frequency>")
            return

        filename = os.path.join(self.bin_dir, args[0])
        if not os.path.exists(filename):
            print(f"File not found: {filename}")
            return

        try:
            freq = int(args[1])
            with open(filename, "rb") as f:
                bin_data = f.read()
            
            # Pack the 20-byte header: Sync(4), Fstart(4), Fend(4), Duration(4), Len(4)
            header = struct.pack("<IIIII", 0xDEADBEEF, freq, freq, 0, len(bin_data))
            self.write_rtt(header + bin_data)
            print(f"Sent {len(bin_data)} bytes at {freq}Hz via RTT.")
        except Exception as e:
            print(f"Transmission Error: {e}")

    def do_exit(self, arg):
        self.jlink.close()
        return True

if __name__ == '__main__':
    PicoController().cmdloop()