#include "wm_include.h"
#include "wm_i2c.h"
#include "wm_gpio_afsel.h"
#include "wm_type_def.h"

#define PIN_SCL WM_IO_PA_01
#define PIN_SDA WM_IO_PA_04

#define I2C_FREQ (100000)

void UserMain(void)
{
	printf("I2C and GPIO configuration\n");

	wm_i2c_scl_config(PIN_SCL);
	wm_i2c_sda_config(PIN_SDA);
	tls_i2c_init(I2C_FREQ);

	tls_gpio_cfg(WM_IO_PB_05, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);

	printf("I2C Address scan started...\n\n");

	for (uint8_t addr = 0x08; addr < 0xf0; addr += 0x1)
	{
		tls_i2c_write_byte(addr << 1, 1);
		tls_i2c_stop();

		if (!(tls_reg_read32(HR_I2C_CR_SR) & I2C_SR_NAK))
		{
			printf("0x%.2x addr found\n", addr);
		}
		tls_os_time_delay(HZ * 0.0001);
	}
	printf("\nI2C Address scan finished \n");

	for (;;)
	{
		tls_gpio_write(WM_IO_PB_05, 1);
		tls_os_time_delay(HZ * 1);
		tls_gpio_write(WM_IO_PB_05, 0);
	}
}