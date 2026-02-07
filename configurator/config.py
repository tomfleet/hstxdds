import cmd
import os
import struct
import pylink

class PicoController(cmd.Cmd):
    intro = 'Pico 2 DDS Control Interface. Type help or ? to list commands.\n'
    prompt = '(pico) '
    bin_dir = "bin_output"

    def __init__(self):
        super().__init__()
        # This finds the absolute path of the folder containing config.py
        self.base_dir = os.path.dirname(os.path.abspath(__file__))
        
        # Set bin_dir to current folder, or a subfolder if you prefer
        # Use os.path.join for Windows/Linux compatibility
        self.bin_dir = self.base_dir 
        
        # Verify it exists immediately
        if not os.path.exists(self.bin_dir):
            print(f"Warning: Directory not found: {self.bin_dir}")
        try:
            self.jlink = pylink.JLink()
        except AttributeError:
            # This catches when the 'wrong' pylink is installed globally
            print("--- Environment Conflict Detected ---")
            print("The wrong 'pylink' package is installed.")
            print("Please run:")
            print("  pip uninstall pylink")
            print("  pip install pylink-square")
            exit(1)
        self.jlink.open()
        self.jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
        self.jlink.connect('RP2350_M33_0')

    def do_list_bins(self, arg):
        """List all .bin files in the configurator directory."""
        try:
            bins = [f for f in os.listdir(self.bin_dir) if f.endswith('.bin')]
            if not bins:
                print(f"No .bin files found in: {self.bin_dir}")
                return
            print("Available profiles:")
            for b in bins:
                print(f"  - {b}")
        except FileNotFoundError:
            print(f"Error: Path not found: {self.bin_dir}")

    def do_pin(self, line):
        """Usage: pin <number> [on|off|get]"""
        args = line.split()
        if not args:
            print("Error: Missing pin number. Usage: pin 25 on")
            return

        # 1. Validate Pin
        try:
            pin_num = int(args[0])
        except ValueError:
            print(f"Error: '{args[0]}' is not a valid pin integer.")
            return

        # 2. Validate/Default State
        # If user typed 'pin 7', default to 'get'
        state = args[1].lower() if len(args) > 1 else "get"
        
        if state == "get":
            print(f"Reading Pin {pin_num} state via J-Link...")
        elif state in ["on", "1", "high"]:
            print(f"Setting Pin {pin_num} HIGH...")
        elif state in ["off", "0", "low"]:
            print(f"Setting Pin {pin_num} LOW...")
        else:
            print(f"Error: Unknown state '{state}'. Use on, off, or get.")

    def do_send_dds(self, arg):
        """Usage: send_dds <bin_index> <fstart> <fend> <ramp_ms> <mode>"""
        args = arg.split()
        if len(args) < 5:
            print("Error: Missing parameters.")
            return

        idx, fs, fe, ramp, mode = args
        bins = [f for f in os.listdir(self.bin_dir) if f.endswith('.bin')]
        selected_bin = os.path.join(self.bin_dir, bins[int(idx)])

        # 1. Send Parameters via RTT (Header)
        # Struct: <IIHB (Start, End, Ramp, Mode)
        header = struct.pack('<IIHB', int(fs), int(fe), int(ramp), int(mode))
        self.jlink.rtt_write(0, header)

        # 2. Send Wavetable Data
        with open(selected_bin, "rb") as f:
            blob = f.read()
            self.jlink.rtt_write(1, blob) # Use Channel 1 for bulk data
        print(f"Sent {selected_bin} and config to target.")

    def write_rtt(self, data):
        """Finds the RTT Control Block and writes data to Channel 0."""
        try:
            # Look for the SEGGER RTT ID string in RAM
            # RP2350 SRAM starts at 0x20000000
            addr = self.jlink.search_mem(0x20000000, 0x80000, b"SEGGER RTT")
            if addr > 0:
                # 24 bytes is the typical offset to the Up-Buffer write pointer
                # This is a bit 'brute force'; for production, parse the structure
                self.jlink.rtt_write(0, data)
            else:
                print("Error: RTT Control Block not found in RAM.")
        except Exception as e:
            print(f"J-Link Write Error: {e}")

    def do_led(self, line):
        """Usage: led [on|off|toggle]"""
        cmd_map = {"on": b'1', "off": b'0', "toggle": b't'}
        char = cmd_map.get(line.strip().lower())
        
        if char:
            self.write_rtt(char)
            print(f"Sent {line.strip().upper()} command to RTT.")
        else:
            print("Usage: led on | off | toggle")



    # def do_pin(self, line):
    #     args = line.split()
    #     if not args:
    #         print("Error: Please specify a pin number (e.g., 'pin 25')")
    #         return
        
    #     try:
    #         pin = int(args[0])
    #         print(f"Setting target pin to: {pin}")
    #         """Toggle onboard LED: led <on|off>"""
    #         val = 1 if arg.lower() == "on" else 0
    #         # Command 0x01 = LED control
    #         self.jlink.rtt_write(0, struct.pack('<BB', 0x01, val))
    #             # Your J-Link memory write logic goes here
    #     except ValueError:
    #         print(f"Error: '{args[0]}' is not a valid integer.")

    def do_exit(self, arg):
        self.jlink.close()
        return True

if __name__ == '__main__':
    PicoController().cmdloop()