#ifdef ARDUINO_ARCH_ESP32
#include <Arduino.h>
#include <driver/i2c.h>

#include "ahWireSlave.h"

TwoWireSlave::TwoWireSlave(uint8_t bus_num)
  :num(bus_num & 1)
  ,portNum(i2c_port_t(bus_num & 1))
  ,sda(-1)
  ,scl(-1)
{

}

TwoWireSlave::~TwoWireSlave()
{
  flush();
  i2c_driver_delete(portNum);
}

bool TwoWireSlave::begin(int sda, int scl, int address)
{
  i2c_config_t config;
  esp_err_t res = ESP_OK;
  
  config.sda_io_num = gpio_num_t(sda);
  config.sda_pullup_en = GPIO_PULLUP_ENABLE;
  config.scl_io_num = gpio_num_t(scl);
  config.scl_pullup_en = GPIO_PULLUP_ENABLE;
  config.mode = I2C_MODE_SLAVE;
  config.slave.addr_10bit_en = 0;
  config.slave.slave_addr = address & 0x7F;

  res = i2c_param_config(portNum, &config);

  if (res != ESP_OK) 
  {
    log_e("invalid I2C parameters");
    return false;
  }

  res = i2c_driver_install(
        portNum,
        config.mode,
        2 * I2C_AH_BUFFER_LENGTH,  // rx buffer length
        2 * I2C_AH_BUFFER_LENGTH,  // tx buffer length
        0);

  if (res != ESP_OK) 
  {
    log_e("failed to install I2C driver");
  }

  return res == ESP_OK;
}

int TwoWireSlave::write_buff(uint8_t *data, size_t size)
{  
  return i2c_slave_write_buffer(portNum, data, (int)size, 0);//21/portTICK_RATE_MS);
}

int TwoWireSlave::read_buff(uint8_t *data, size_t size)
{  
  return i2c_slave_read_buffer(portNum, data, size, 0);
}

void TwoWireSlave::flush(void)
{
  i2c_reset_rx_fifo(portNum);
  i2c_reset_tx_fifo(portNum);
}

#endif      // ifdef ARDUINO_ARCH_ESP32
