import os
import cmd
import struct
import pylink
import time

class PicoController(cmd.Cmd):
    intro = 'Pico 2 DDS Control Interface. Type help or ? to list commands.\n'
    prompt = '(pico) '

    def __init__(self):
        super().__init__()
        self.base_dir = os.path.dirname(os.path.abspath(__file__))
        self.bin_dir = self.base_dir 
        
        try:
            self.jlink = pylink.JLink()
            self.jlink.open()
            self.jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
            self.jlink.connect('RP2350_M33_0')
            self.jlink.rtt_start()
            print(f"Connected to RP2350. Root: {self.base_dir}")
        except Exception as e:
            print(f"Connection Error: {e}")

    def postcmd(self, stop, line):
        """Automatically called after every command to flush MCU status text."""
        try:
            # Poll Channel 0 for any log strings or status updates
            mcu_text = self.jlink.rtt_read(0, 1024)
            if mcu_text:
                print(f"\n[MCU]: {bytes(mcu_text).decode('utf-8', errors='replace')}")
        except Exception:
            pass
        return stop

    def do_led(self, line):
        """Usage: led on | off"""
        args = line.split()
        if args:
            cmd_map = {"on": b'1', "off": b'0'}
            char = cmd_map.get(args[0].lower())
            if char:
                self.jlink.rtt_write(0, char)

    def do_read_wavetable(self, arg):
        """Read the current wavetable from Channel 1."""
        # 1. Trigger the read command on Channel 0
        self.jlink.rtt_write(0, b'r')
        print("Waiting for MCU to dump data...")

        # 2. Drain the buffer
        received_data = bytearray()
        timeout = 0.5  # Seconds to wait for more data
        last_read_time = time.time()

        while time.time() - last_read_time < timeout:
            # Read whatever is there, up to 8KB
            chunk = self.jlink.rtt_read(1, 1000)
            if chunk:
                received_data.extend(chunk)
                last_read_time = time.time()  # Reset timeout on success
            else:
                time.sleep(0.01) # Small nap to prevent CPU pegging

        if received_data:
            filename = "readback.bin"
            with open(filename, "wb") as f:
                f.write(received_data)
            print(f"Success: Received {len(received_data)} bytes. Saved to {filename}")
        else:
            print("Error: Read timeout. No data arrived on Channel 1.")

    def do_send_dds(self, line):
        """
        Usage: send_dds <filename> <fstart> [fend] [options]

                Options:
                    loop          : Run waveform continuously
                    loop=Y|N      : Explicit loop enable/disable
                    repeat        : Run waveform once (alias for repeat=1)
                    repeat=N      : Run waveform N times
                    delay=ms      : Delay between repeats in ms
                    swap          : Swap upper/lower nibbles in each byte
                    swap=Y|N      : Explicit swap enable/disable
                    <ms>          : Bare number treated as delay
        """
        args = line.split()
        if len(args) < 2:
            print("Usage: send_dds <filename> <fstart> [fend] [loop|repeat=N] [delay=ms]")
            return

        filename = os.path.join(self.bin_dir, args[0])

        # Parse Options
        mode = 0
        repeats = 0
        delay = 0
        swap = False

        try:
            fstart = int(args[1])
            fend = fstart

            idx = 2
            if idx < len(args) and args[idx].lstrip("-").isdigit():
                fend = int(args[idx])
                idx += 1

            for x in args[idx:]:
                xl = x.lower()
                if xl == "loop":
                    mode = 1
                elif xl.startswith("loop="):
                    value = xl.split("=", 1)[1].strip()
                    if value in {"1", "true", "y", "yes"}:
                        mode = 1
                    elif value in {"0", "false", "n", "no"}:
                        mode = 0
                    else:
                        raise ValueError(f"invalid loop value: {value}")
                elif xl == "repeat":
                    mode = 2
                    repeats = 1
                elif xl.startswith("repeat="):
                    value = xl.split("=", 1)[1].strip()
                    if not value.lstrip("-").isdigit():
                        raise ValueError(f"invalid repeat value: {value}")
                    mode = 2
                    repeats = int(value)
                elif xl.startswith("delay="):
                    delay = int(xl.split("=", 1)[1])
                elif xl == "swap":
                    swap = True
                elif xl.startswith("swap="):
                    value = xl.split("=", 1)[1].strip()
                    if value in {"1", "true", "y", "yes"}:
                        swap = True
                    elif value in {"0", "false", "n", "no"}:
                        swap = False
                    else:
                        raise ValueError(f"invalid swap value: {value}")
                elif xl.lstrip("-").isdigit() and delay == 0:
                    delay = int(xl)

            with open(filename, "rb") as f:
                bin_data = f.read()
            if swap:
                bin_data = bytes(((b & 0x0F) << 4) | ((b & 0xF0) >> 4) for b in bin_data)
        except Exception as e:
            print(f"File/Arg Error: {e}")
            return

        try:
            # 1. Force a reset on the MCU before sending the new header
            self.jlink.rtt_write(0, b'x')
            time.sleep(0.01)

            # 2. Send 32-byte unified header to Channel 0
            header = struct.pack(
                "<IIIIIIII",
                0xDEADBEEF,
                fstart,
                fend,
                0,
                len(bin_data),
                mode,
                repeats,
                delay,
            )
            self.jlink.rtt_write(0, header)

            # 3. Send raw data to Channel 1
            self.jlink.rtt_write(1, bin_data)
            print(f"Transfer initiated: {len(bin_data)} bytes.")

        except Exception as e:
            print(f"Transmission Error: {e}")

    def do_exit(self, arg):
        self.jlink.close()
        return True

if __name__ == '__main__':
    PicoController().cmdloop()