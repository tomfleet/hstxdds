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
        try:
            mcu_text = self.jlink.rtt_read(0, 1024)
            if mcu_text:
                print(f"\n[MCU]: {bytes(mcu_text).decode('utf-8', errors='replace')}")
        except Exception:
            pass
        return stop

    def do_led(self, line):
        args = line.split()
        if args:
            cmd_map = {"on": b'1', "off": b'0'}
            char = cmd_map.get(args[0].lower())
            if char:
                self.jlink.rtt_write(0, char)

    def do_load(self, arg):
        """
        Usage: load <filename> <freq> [options]
        
        Options:
          loop          : Run waveform continuously
          repeat=N      : Run waveform N times
          delay=ms      : Delay between repeats in ms
          
        Example: load sine.bin 1000
        Example: load sine.bin 1000 loop
        Example: load sine.bin 1000 repeat=5 delay=500
        """
        args = arg.split()
        if len(args) < 2:
            print("Usage: load <filename> <freq> [loop|repeat=N] [delay=ms]")
            return

        filename = os.path.join(self.bin_dir, args[0])
        
        # Parse Options
        mode = 0      # 0=Single (Default)
        repeats = 0
        delay = 0
        
        try:
            freq = int(args[1])
            
            # Check for extra flags
            for x in args[2:]:
                if x.lower() == "loop":
                    mode = 1
                elif x.lower().startswith("repeat="):
                    mode = 2
                    repeats = int(x.split("=")[1])
                elif x.lower().startswith("delay="):
                    delay = int(x.split("=")[1])

            with open(filename, "rb") as f:
                bin_data = f.read()

        except Exception as e:
            print(f"File/Arg Error: {e}")
            return

        try:
            # 1. Force a reset on the MCU before sending the new header
            self.jlink.rtt_write(0, b'x') 
            time.sleep(0.01)

            # 2. Pack Unified Header (32 Bytes)
            # Struct: sync(DEADBEEF), fstart, fend, duration, len, mode, repeats, delay
            print(f"Sending Config (Mode={mode}, Rep={repeats}, Dly={delay})...")
            
            # header = struct.pack("<IIIIIIII", 
            #                      0xDEADBEEF, 
            #                      freq, 
            #                      freq, # fend 
            #                      0,    # duration
            #                      len(bin_data), 
            #                      mode, 
            #                      repeats, 
            #                      delay)


            header = struct.pack("<IIIIIIII", 
                                0xDEADBEEF, 
                                freq, freq, 0, len(bin_data), 
                                mode, repeats, delay) # Always 32 bytes
            self.jlink.rtt_write(0, header)
            self.jlink.rtt_write(1, bin_data)

            
            print(f"Dispatch complete. {len(bin_data)} bytes.")

        except Exception as e:
            print(f"RTT Write Error: {e}")

    def do_exit(self, line):
        return True