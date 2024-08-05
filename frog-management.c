#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/spi.h"
#include <math.h>

#define DEBUG true

// This defines how many Max7219 modules we have cascaded together, in this case, just the one.
#define NUM_MODULES 1

const uint8_t CMD_NOOP = 0;
const uint8_t CMD_DIGIT0 = 1; // Goes up to 8, for each digit
const uint8_t CMD_DECODEMODE = 9;
const uint8_t CMD_BRIGHTNESS = 10;
const uint8_t CMD_SCANLIMIT = 11;
const uint8_t CMD_SHUTDOWN = 12;
const uint8_t CMD_DISPLAYTEST = 15;

#define DHT_PIN 0
#define MAX_TIMINGS 100

#define LOW_TEMP_LED 1
#define HIGH_TEMP_LED 2
#define LOW_HUM_LED 3

#define HEAT_MAT_PIN 4

#define LOW_TEMP_RANGE 22.0f
#define HIGH_TEMP_RANGE 28.0f
#define LOW_HUM_RANGE 75.0f

#define INTERVAL 1000 // in milliseconds, interval of time between measurements

typedef struct
{
    float humidity;
    float temp_celsius;
} dht_reading;

#ifdef PICO_DEFAULT_SPI_CSN_PIN
static inline void cs_select()
{
    asm volatile("nop \n nop \n nop");
    gpio_put(PICO_DEFAULT_SPI_CSN_PIN, 0); // Active low
    asm volatile("nop \n nop \n nop");
}

static inline void cs_deselect()
{
    asm volatile("nop \n nop \n nop");
    gpio_put(PICO_DEFAULT_SPI_CSN_PIN, 1);
    asm volatile("nop \n nop \n nop");
}
#endif

#if defined(spi_default) && defined(PICO_DEFAULT_SPI_CSN_PIN)
static void write_register(uint8_t reg, uint8_t data)
{
    uint8_t buf[2];
    buf[0] = reg;
    buf[1] = data;
    cs_select();
    spi_write_blocking(spi_default, buf, 2);
    cs_deselect();
    sleep_ms(1);
}

static void write_register_all(uint8_t reg, uint8_t data)
{
    uint8_t buf[2];
    buf[0] = reg;
    buf[1] = data;
    cs_select();
    for (int i = 0; i < NUM_MODULES; i++)
    {
        spi_write_blocking(spi_default, buf, 2);
    }
    cs_deselect();
}
#endif

void display_nums(dht_reading measures)
{
    int measure_temp = measures.temp_celsius;
    int measure_hum = measures.humidity;
    int digit = 0;
    int numbers[8] = {0};
    for (; digit < 3; digit++)
    {
        numbers[digit] = measure_hum % 10;
        measure_hum /= 10;
    }
    numbers[0] += 128; // Adds the dot for the fraction
    numbers[3] = 15;   // Represents " "
    for (digit += 2; digit < 8; digit++)
    {
        numbers[digit] = measure_temp % 10;
        measure_temp /= 10;
    }
    numbers[5] += 128; // Adds the dot for the fraction
    numbers[7] = 15;   // Represents " "
    for (size_t i = 0; i < 8; i++)
    {
        // displaying in the opposite direction
        int num_index = 7 - i;
        write_register_all(CMD_DIGIT0 + i, numbers[num_index]);
    }
}

void clear()
{
    for (int i = 0; i < 8; i++)
    {
        write_register_all(CMD_DIGIT0 + i, 0);
    }
}

void read_from_dht(dht_reading *result)
{
    int data[5] = {0, 0, 0, 0, 0};
    uint last = 1;
    uint j = 0;

    gpio_set_dir(DHT_PIN, GPIO_OUT);
    gpio_put(DHT_PIN, 0);
    sleep_ms(20);
    gpio_set_dir(DHT_PIN, GPIO_IN);

    for (uint i = 0; i < MAX_TIMINGS; i++)
    {
        uint count = 0;
        while (gpio_get(DHT_PIN) == last)
        {
            count++;
            sleep_us(1);
            if (count == 255)
                break;
        }
        last = gpio_get(DHT_PIN);
        if (count == 255)
            break;

        if ((i >= 4) && (i % 2 == 0))
        {
            data[j / 8] <<= 1;
            if (count > 16)
                data[j / 8] |= 1;
            j++;
        }
    }

    if ((j >= 40) && (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)))
    {
        result->humidity = (float)((data[0] << 8) + data[1]) / 10;
        if (result->humidity > 100)
        {
            result->humidity = data[0];
        }
        result->temp_celsius = (float)(((data[2] & 0x7F) << 8) + data[3]) / 10;
        if (result->temp_celsius > 125)
        {
            result->temp_celsius = data[2];
        }
        if (data[2] & 0x80)
        {
            result->temp_celsius = -result->temp_celsius;
        }
    }
    else
    {
        if (DEBUG)
            printf("Bad data\n");
    }
}

int main()
{
    stdio_init_all();
    gpio_init(DHT_PIN);
    gpio_init(LOW_TEMP_LED);
    gpio_init(HIGH_TEMP_LED);
    gpio_init(LOW_HUM_LED);
    gpio_set_dir(LOW_TEMP_LED, GPIO_OUT);
    gpio_set_dir(HIGH_TEMP_LED, GPIO_OUT);
    gpio_set_dir(LOW_HUM_LED, GPIO_OUT);

    // This example will use SPI0 at 1MHz.
    spi_init(spi_default, 1000 * 1000);
    gpio_set_function(PICO_DEFAULT_SPI_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(PICO_DEFAULT_SPI_TX_PIN, GPIO_FUNC_SPI);

    // Make the SPI pins available to picotool
    bi_decl(bi_2pins_with_func(PICO_DEFAULT_SPI_TX_PIN, PICO_DEFAULT_SPI_SCK_PIN, GPIO_FUNC_SPI));

    // Chip select is active-low, so we'll initialise it to a driven-high state
    gpio_init(PICO_DEFAULT_SPI_CSN_PIN);
    gpio_set_dir(PICO_DEFAULT_SPI_CSN_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_SPI_CSN_PIN, 1);

    // Make the CS pin available to picotool
    bi_decl(bi_1pin_with_name(PICO_DEFAULT_SPI_CSN_PIN, "SPI CS"));

    // Send init sequence to device

    write_register_all(CMD_SHUTDOWN, 0);
    write_register_all(CMD_DISPLAYTEST, 0);
    write_register_all(CMD_SCANLIMIT, 7);
    write_register_all(CMD_DECODEMODE, 255);
    write_register_all(CMD_SHUTDOWN, 1);
    write_register_all(CMD_BRIGHTNESS, 4);

    clear();

    while (true)
    {
        /*
        Logic for the project

        1. measure temperature and humidity
        2. display both
        3. if humidity is low, turn on warning LED
        4. if temperature is high, turn on warning LED
        5. if temperature is low:
            5a. turn on warning LED
            5b. turn on relay to heating mat
        */

        sleep_ms(INTERVAL);

        dht_reading reading;
        read_from_dht(&reading);

        if (DEBUG)
            printf("Humidity = %.1f%%, Temperature = %.1fC\n",
                   reading.humidity, reading.temp_celsius);
        display_nums(reading);

        if (reading.temp_celsius >= HIGH_TEMP_RANGE)
        {
            gpio_put(HIGH_TEMP_LED, 1);
        }
        else if (reading.temp_celsius <= LOW_TEMP_RANGE)
        {
            gpio_put(LOW_TEMP_LED, 1);
            gpio_put(HEAT_MAT_PIN, 1);
        }
        else
        {
            gpio_put(HIGH_TEMP_LED, 0);
            gpio_put(LOW_TEMP_LED, 0);
            gpio_put(HEAT_MAT_PIN, 0);
        }

        if (reading.humidity <= LOW_HUM_RANGE)
        {
            gpio_put(LOW_HUM_LED, 1);
        }
        else
        {
            gpio_put(LOW_HUM_LED, 0);
        }
    }
}