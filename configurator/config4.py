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

    def do_led(self, line):
        """Usage: led on | off | toggle"""
        args = line.split()
        if not args:
            print("Usage: led on | off | toggle")
            return
        cmd_map = {"on": b'1', "off": b'0', "toggle": b't'}
        char = cmd_map.get(args[0].lower())
        if char:
            # Channel 0: Command Pipe
            self.jlink.rtt_write(0, char)
            print(f"Sent {args[0].upper()} to Channel 0.")

    def do_read_wavetable(self, arg):
        """Request the current wavetable from MCU via Channel 1."""
        # 1. Trigger the read on Channel 0
        print("Requesting readback...")
        self.jlink.rtt_write(0, b'r')
        
        # 2. Poll Channel 1 for the binary dump
        # We'll wait a brief moment for the MCU to fill the buffer
        time.sleep(0.2) 
        try:
            # We read from Channel 1 (Binary Data)
            data = self.jlink.rtt_read(1, 8192)
            if data:
                with open("readback.bin", "wb") as f:
                    f.write(bytearray(data))
                print(f"Success: Received {len(data)} bytes. Saved to readback.bin")
            else:
                print("Error: No data received on Channel 1.")
        except Exception as e:
            print(f"Read Error: {e}")

    def do_send_dds(self, line):
        """Send binary wavetable to MCU via Channel 1."""
        args = line.split()
        if len(args) < 2:
            print("Usage: send_dds <filename.bin> <frequency>")
            return

        filename = os.path.join(self.bin_dir, args[0])
        try:
            freq = int(args[1])
            with open(filename, "rb") as f:
                bin_data = f.read()
            
            # 1. Send metadata to Channel 0 (so MCU prepares for data)
            # We use the sync word to tell the MCU "Incoming on Chan 1"
            header = struct.pack("<IIIII", 0xDEADBEEF, freq, freq, 0, len(bin_data))
            self.jlink.rtt_write(0, header)
            
            # 2. Send the binary payload to Channel 1
            # pylink.rtt_write returns the number of bytes successfully written
            bytes_sent = self.jlink.rtt_write(1, bin_data)
            
            if bytes_sent < len(bin_data):
                print(f"Warning: Buffer full! Only sent {bytes_sent}/{len(bin_data)} bytes.")
            else:
                print(f"Successfully pushed {bytes_sent} bytes to Channel 1.")
                
        except Exception as e:
            print(f"Transmission Error: {e}")

    def postcmd(self, stop, line):
        """This runs automatically after every command you type."""
        # Check Channel 0 for any status text from the MCU
        try:
            terminal_output = self.jlink.rtt_read(0, 1024)
            if terminal_output:
                # Convert bytes to string and print it
                print(f"\n[MCU]: {terminal_output.decode('utf-8', errors='replace')}")
        except Exception:
            pass
        return stop


    def do_exit(self, arg):
        self.jlink.close()
        return True

if __name__ == '__main__':
    PicoController().cmdloop()