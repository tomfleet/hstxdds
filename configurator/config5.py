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
        """Metadata to Chan 0, Binary to Chan 1."""
        args = line.split()
        if len(args) < 2: return
        
        filename = os.path.join(self.bin_dir, args[0])
        try:
            freq = int(args[1])
            with open(filename, "rb") as f:
                bin_data = f.read()
            
            # # Send the 20-byte metadata header to Channel 0
            # header = struct.pack("<IIIII", 0xDEADBEEF, freq, freq, 0, len(bin_data))
            # self.jlink.rtt_write(0, header)
            
            # # Send raw binary to Channel 1
            # self.jlink.rtt_write(1, bin_data)
            # print(f"Header + {len(bin_data)} bytes dispatched.")
        except Exception as e:
            print(f"fILE LOAD Error: {e}")

        try:
            # 1. Force a reset on the MCU before sending the new header
            self.jlink.rtt_write(0, b'X')
            time.sleep(0.01) 

            # 2. Send 20-byte header to Channel 0
            header = struct.pack("<IIIII", 0xDEADBEEF, freq, freq, 0, len(bin_data))
            self.jlink.rtt_write(0, header)
            
            # 3. Send raw data to Channel 1
            # We give the MCU a tiny gap to process the header and switch channels
            #time.sleep(0.05)
            self.jlink.rtt_write(1, bin_data)
            print(f"Transfer initiated: {len(bin_data)} bytes.")
            
        except Exception as e:
            print(f"Transmission Error: {e}")

    def do_exit(self, arg):
        self.jlink.close()
        return True

if __name__ == '__main__':
    PicoController().cmdloop()