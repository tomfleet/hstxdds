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
        self.jlink = pylink.JLink()
        self.jlink.open()
        self.jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
        self.jlink.connect('RP2350')

    # --- DDS MENU ---
    def do_list_bins(self, arg):
        """List available wavetables in the default output folder."""
        bins = [f for f in os.listdir(self.bin_dir) if f.endswith('.bin')]
        for i, b in enumerate(bins):
            print(f"[{i}] {b}")

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

    # --- DEBUG MENU ---
    def do_led(self, arg):
        """Toggle onboard LED: led <on|off>"""
        val = 1 if arg.lower() == "on" else 0
        # Command 0x01 = LED control
        self.jlink.rtt_write(0, struct.pack('<BB', 0x01, val))

    def do_pin(self, arg):
        """Get/Set pin state: pin <num> <val|get>"""
        # Command 0x02 = GPIO control
        args = arg.split()
        pin = int(args[0])
        state = 2 if args[1] == "get" else int(args[1])
        self.jlink.rtt_write(0, struct.pack('<BBB', 0x02, pin, state))

    def do_exit(self, arg):
        self.jlink.close()
        return True

if __name__ == '__main__':
    PicoController().cmdloop()