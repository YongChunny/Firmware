/****************************************************************************
 *
 *   Copyright (c) 2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "QMC5883L.hpp"

using namespace time_literals;

static constexpr int16_t combine(uint8_t msb, uint8_t lsb)
{
	return (msb << 8u) | lsb;
}

QMC5883L::QMC5883L(I2CSPIBusOption bus_option, int bus, int bus_frequency, enum Rotation rotation) :
	I2C(DRV_MAG_DEVTYPE_QMC5883L, MODULE_NAME, bus, I2C_ADDRESS_DEFAULT, bus_frequency),
	I2CSPIDriver(MODULE_NAME, px4::device_bus_to_wq(get_device_id()), bus_option, bus),
	_px4_mag(get_device_id(), external() ? ORB_PRIO_VERY_HIGH : ORB_PRIO_DEFAULT, rotation)
{
	_px4_mag.set_external(external());
}

QMC5883L::~QMC5883L()
{
	perf_free(_transfer_perf);
	perf_free(_bad_register_perf);
	perf_free(_bad_transfer_perf);
}

int QMC5883L::init()
{
	int ret = I2C::init();

	if (ret != PX4_OK) {
		DEVICE_DEBUG("I2C::init failed (%i)", ret);
		return ret;
	}

	return Reset() ? 0 : -1;
}

bool QMC5883L::Reset()
{
	_state = STATE::RESET;
	ScheduleClear();
	ScheduleNow();
	return true;
}

void QMC5883L::print_status()
{
	I2CSPIDriverBase::print_status();

	perf_print_counter(_transfer_perf);
	perf_print_counter(_bad_register_perf);
	perf_print_counter(_bad_transfer_perf);

	_px4_mag.print_status();
}

int QMC5883L::probe()
{
	// no identifier, read X LSB register once
	const uint8_t cmd = static_cast<uint8_t>(Register::X_LSB);
	uint8_t buffer{};
	return transfer(&cmd, 1, &buffer, 1);
}

void QMC5883L::RunImpl()
{
	switch (_state) {
	case STATE::RESET:
		// CNTL2: Software Reset
		RegisterWrite(Register::CNTL2, CNTL2_BIT::SOFT_RST);
		_reset_timestamp = hrt_absolute_time();
		_consecutive_failures = 0;
		_state = STATE::WAIT_FOR_RESET;
		ScheduleDelayed(100_ms); // POR Completion Time
		break;

	case STATE::WAIT_FOR_RESET:

		// SOFT_RST: This bit is automatically reset to zero after POR routine
		if ((RegisterRead(Register::CNTL2) & CNTL2_BIT::SOFT_RST) == 0) {
			// if reset succeeded then configure
			_state = STATE::CONFIGURE;
			ScheduleDelayed(1_ms);

		} else {
			// RESET not complete
			if (hrt_elapsed_time(&_reset_timestamp) > 1000_ms) {
				PX4_DEBUG("Reset failed, retrying");
				_state = STATE::RESET;
				ScheduleDelayed(100_ms);

			} else {
				PX4_DEBUG("Reset not complete, check again in 100 ms");
				ScheduleDelayed(100_ms);
			}
		}

		break;

	case STATE::CONFIGURE:
		if (Configure()) {
			// if configure succeeded then start reading every 20 ms (50 Hz)
			_state = STATE::READ;
			ScheduleOnInterval(20_ms, 20_ms); // 50 Hz

		} else {
			// CONFIGURE not complete
			if (hrt_elapsed_time(&_reset_timestamp) > 1000_ms) {
				PX4_DEBUG("Configure failed, resetting");
				_state = STATE::RESET;

			} else {
				PX4_DEBUG("Configure failed, retrying");
			}

			ScheduleDelayed(100_ms);
		}

		break;

	case STATE::READ: {
			struct TransferBuffer {
				uint8_t X_LSB;
				uint8_t X_MSB;
				uint8_t Y_LSB;
				uint8_t Y_MSB;
				uint8_t Z_LSB;
				uint8_t Z_MSB;
				uint8_t STATUS;
			} buffer{};

			perf_begin(_transfer_perf);

			bool failure = false;
			const hrt_abstime timestamp_sample = hrt_absolute_time();
			uint8_t cmd = static_cast<uint8_t>(Register::X_LSB);

			if (transfer(&cmd, 1, (uint8_t *)&buffer, sizeof(buffer)) != PX4_OK) {
				perf_count(_bad_transfer_perf);
				failure = true;
			}

			perf_end(_transfer_perf);

			// process data if successful transfer, data ready, no overflow
			if (!failure && (buffer.STATUS && STATUS_BIT::DRDY) && (buffer.STATUS && STATUS_BIT::OVL == 0)) {
				int16_t x = combine(buffer.X_MSB, buffer.X_LSB);
				int16_t y = combine(buffer.Y_MSB, buffer.Y_LSB);
				int16_t z = combine(buffer.Z_MSB, buffer.Z_LSB);

				// Sensor orientation
				//  Forward X := -X
				//  Right   Y := +Y
				//  Down    Z := -Z
				x = (x == INT16_MIN) ? INT16_MAX : -x; // -x
				z = (z == INT16_MIN) ? INT16_MAX : -z; // -z

				_px4_mag.update(timestamp_sample, x, y, z);

				_consecutive_failures = 0;
			}

			if (failure || hrt_elapsed_time(&_last_config_check_timestamp) > 100_ms) {
				// check configuration registers periodically or immediately following any failure
				if (RegisterCheck(_register_cfg[_checked_register])) {
					_last_config_check_timestamp = timestamp_sample;
					_checked_register = (_checked_register + 1) % size_register_cfg;

				} else {
					// register check failed, force reset
					perf_count(_bad_register_perf);
					Reset();
					return;
				}

			} else if (hrt_elapsed_time(&_temperature_update_timestamp) > 1_s) {
				// limit temperature updates to 1 Hz
				_temperature_update_timestamp = timestamp_sample;

				const uint8_t cmd_temperature = static_cast<uint8_t>(Register::TEMP_LSB);

				struct TransferBufferTemperature {
					uint8_t TOUT_LSB;
					uint8_t TOUT_MSB;
				} buffer_temperature{};

				if (transfer(&cmd_temperature, 1, (uint8_t *)&buffer_temperature, sizeof(buffer_temperature)) == PX4_OK) {
					const int16_t temperature_raw = combine(buffer_temperature.TOUT_MSB, buffer_temperature.TOUT_LSB);

					// The temperature coefficient is about 100 LSB/°C
					const float temperature_C = temperature_raw / 100.f;
					_px4_mag.set_temperature(temperature_C);
				}
			}

			if (_consecutive_failures > 10) {
				Reset();
			}
		}

		break;
	}
}

bool QMC5883L::Configure()
{
	// first set and clear all configured register bits
	for (const auto &reg_cfg : _register_cfg) {
		RegisterSetAndClearBits(reg_cfg.reg, reg_cfg.set_bits, reg_cfg.clear_bits);
	}

	// now check that all are configured
	bool success = true;

	for (const auto &reg_cfg : _register_cfg) {
		if (!RegisterCheck(reg_cfg)) {
			success = false;
		}
	}

	_px4_mag.set_scale(1.f / 12000.f); // 12000 LSB/Gauss (Field Range = ±2G)

	return success;
}

bool QMC5883L::RegisterCheck(const register_config_t &reg_cfg)
{
	bool success = true;

	const uint8_t reg_value = RegisterRead(reg_cfg.reg);

	if (reg_cfg.set_bits && ((reg_value & reg_cfg.set_bits) != reg_cfg.set_bits)) {
		PX4_DEBUG("0x%02hhX: 0x%02hhX (0x%02hhX not set)", (uint8_t)reg_cfg.reg, reg_value, reg_cfg.set_bits);
		success = false;
	}

	if (reg_cfg.clear_bits && ((reg_value & reg_cfg.clear_bits) != 0)) {
		PX4_DEBUG("0x%02hhX: 0x%02hhX (0x%02hhX not cleared)", (uint8_t)reg_cfg.reg, reg_value, reg_cfg.clear_bits);
		success = false;
	}

	return success;
}

uint8_t QMC5883L::RegisterRead(Register reg)
{
	const uint8_t cmd = static_cast<uint8_t>(reg);
	uint8_t buffer{};
	transfer(&cmd, 1, &buffer, 1);
	return buffer;
}

void QMC5883L::RegisterWrite(Register reg, uint8_t value)
{
	uint8_t buffer[2] { (uint8_t)reg, value };
	transfer(buffer, sizeof(buffer), nullptr, 0);
}

void QMC5883L::RegisterSetAndClearBits(Register reg, uint8_t setbits, uint8_t clearbits)
{
	const uint8_t orig_val = RegisterRead(reg);
	uint8_t val = (orig_val & ~clearbits) | setbits;

	if (orig_val != val) {
		RegisterWrite(reg, val);
	}
}