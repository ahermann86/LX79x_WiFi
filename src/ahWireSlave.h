#ifndef TwoWireSlave_h
#define TwoWireSlave_h
#ifdef ARDUINO_ARCH_ESP32

#include <stdint.h>
#include <driver/i2c.h>

#define I2C_AH_BUFFER_LENGTH 128

class TwoWireSlave
{
  public:
    TwoWireSlave(uint8_t bus_num);
    ~TwoWireSlave();

    bool begin(int sda, int scl, int address);
    int write_buff(uint8_t *data, size_t size);
    int read_buff(uint8_t *data, size_t size);
    void flush(void);

  private:
    uint8_t num;
    i2c_port_t portNum;
    int8_t sda;
    int8_t scl;
};


extern TwoWireSlave WireSlave;
extern TwoWireSlave WireSlave1;

#endif      // ifdef ARDUINO_ARCH_ESP32
#endif      // ifndef TwoWireSlave_h
