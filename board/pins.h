//LED
#define LED_PIN 3

// UART defines
// By default the stdout UART is `uart0`, so we will use the second one
#define UART_ID uart1
#define BAUD_RATE 115200

// Use pins 4 and 5 for UART1
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define UART_TX_PIN 23
#define UART_RX_PIN 24

// SPI Definesif
// We are going to use SPI 0, and allocate it to the following GPIO pins
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define SPI_PORT spi0
#define PIN_MISO 16
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19

// I2C defines
// This example will use I2C0 on GPIO8 (SDA) and GPIO9 (SCL) running at 400KHz.
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define I2C_PORT i2c0
#define I2C_SDA 8
#define I2C_SCL 9

// Default HSTX starting pin for RP2350 (standard 2350A)
#ifndef HSTX_DATA_START_PIN
    #if PICO_RP2350B
        // The 2350B has more GPIOs; update this if your specific 
        // 2350B board maps the HSTX bus to a different bank.
        #define HSTX_DATA_START_PIN 12 
    #else
        // Standard RP2350 (Pico 2)
        #define HSTX_DATA_START_PIN 12
    #endif
#endif