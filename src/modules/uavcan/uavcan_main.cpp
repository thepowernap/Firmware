/****************************************************************************
 *
 *   Copyright (c) 2014 PX4 Development Team. All rights reserved.
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

#include <nuttx/config.h>

#include <cstdlib>
#include <cstring>
#include <systemlib/err.h>
#include <systemlib/systemlib.h>
#include <systemlib/mixer/mixer.h>
#include <arch/board/board.h>
#include <arch/chip/chip.h>

#include <drivers/drv_hrt.h>
#include <drivers/drv_pwm_output.h>

#include "uavcan_main.hpp"

/**
 * @file uavcan_main.cpp
 *
 * Implements basic functinality of UAVCAN node.
 *
 * @author Pavel Kirienko <pavel.kirienko@gmail.com>
 */

/*
 * UavcanNode
 */
UavcanNode *UavcanNode::_instance;

UavcanNode::UavcanNode(uavcan::ICanDriver &can_driver, uavcan::ISystemClock &system_clock) :
	CDev("uavcan", UAVCAN_DEVICE_PATH),
	_task(0),
	_task_should_exit(false),
	_armed_sub(-1),
	_is_armed(false),
	_output_count(0),
	_node(can_driver, system_clock),
	_controls({}),
	_poll_fds({})
{
	_control_topics[0] = ORB_ID(actuator_controls_0);
	_control_topics[1] = ORB_ID(actuator_controls_1);
	_control_topics[2] = ORB_ID(actuator_controls_2);
	_control_topics[3] = ORB_ID(actuator_controls_3);

	// memset(_controls, 0, sizeof(_controls));
	// memset(_poll_fds, 0, sizeof(_poll_fds));
}

UavcanNode::~UavcanNode()
{
	if (_task != -1) {
		/* tell the task we want it to go away */
		_task_should_exit = true;

		unsigned i = 10;

		do {
			/* wait 50ms - it should wake every 100ms or so worst-case */
			usleep(50000);

			/* if we have given up, kill it */
			if (--i == 0) {
				task_delete(_task);
				break;
			}

		} while (_task != -1);
	}

	/* clean up the alternate device node */
		// unregister_driver(PWM_OUTPUT_DEVICE_PATH);

	::close(_armed_sub);

	_instance = nullptr;
}

int UavcanNode::start(uavcan::NodeID node_id, uint32_t bitrate)
{
	if (_instance != nullptr) {
		warnx("Already started");
		return -1;
	}

	/*
	 * GPIO config.
	 * Forced pull up on CAN2 is required for Pixhawk v1 where the second interface lacks a transceiver.
	 * If no transceiver is connected, the RX pin will float, occasionally causing CAN controller to
	 * fail during initialization.
	 */
	stm32_configgpio(GPIO_CAN1_RX);
	stm32_configgpio(GPIO_CAN1_TX);
	stm32_configgpio(GPIO_CAN2_RX | GPIO_PULLUP);
	stm32_configgpio(GPIO_CAN2_TX);

	/*
	 * CAN driver init
	 */
	static CanInitHelper can;
	static bool can_initialized = false;

	if (!can_initialized) {
		const int can_init_res = can.init(bitrate);

		if (can_init_res < 0) {
			warnx("CAN driver init failed %i", can_init_res);
			return can_init_res;
		}

		can_initialized = true;
	}

	/*
	 * Node init
	 */
	_instance = new UavcanNode(can.driver, uavcan_stm32::SystemClock::instance());

	if (_instance == nullptr) {
		warnx("Out of memory");
		return -1;
	}

	const int node_init_res = _instance->init(node_id);

	if (node_init_res < 0) {
		delete _instance;
		_instance = nullptr;
		warnx("Node init failed %i", node_init_res);
		return node_init_res;
	}

	/*
	 * Start the task. Normally it should never exit.
	 */
	static auto run_trampoline = [](int, char *[]) {return UavcanNode::_instance->run();};
	_instance->_task = task_spawn_cmd("uavcan", SCHED_DEFAULT, SCHED_PRIORITY_DEFAULT, StackSize,
			      static_cast<main_t>(run_trampoline), nullptr);

	if (_instance->_task < 0) {
		warnx("start failed: %d", errno);
		return -errno;
	}

	return OK;
}

int UavcanNode::init(uavcan::NodeID node_id)
{

	int ret;

	/* do regular cdev init */
	ret = CDev::init();

	if (ret != OK)
		return ret;

	uavcan::protocol::SoftwareVersion swver;
	swver.major = 12;                        // TODO fill version info
	swver.minor = 34;
	_node.setSoftwareVersion(swver);

	uavcan::protocol::HardwareVersion hwver;
	hwver.major = 42;                        // TODO fill version info
	hwver.minor = 42;
	_node.setHardwareVersion(hwver);

	_node.setName("org.pixhawk"); // Huh?

	_node.setNodeID(node_id);

	return _node.start();
}

int UavcanNode::run()
{
	_node.setStatusOk();

	// XXX figure out the output count
	_output_count = 2;


	_armed_sub = orb_subscribe(ORB_ID(actuator_armed));

	actuator_outputs_s outputs;
	memset(&outputs, 0, sizeof(outputs));

	while (!_task_should_exit) {

		if (_groups_subscribed != _groups_required) {
			subscribe();
			_groups_subscribed = _groups_required;
		}

		int ret = ::poll(_poll_fds, _poll_fds_num, 5/* 5 ms wait time */);

		// this would be bad...
		if (ret < 0) {
			log("poll error %d", errno);
			continue;

		} else if (ret == 0) {
			// timeout: no control data, switch to failsafe values

		} else {

			// get controls for required topics
			unsigned poll_id = 0;
			for (unsigned i = 0; i < NUM_ACTUATOR_CONTROL_GROUPS; i++) {
				if (_control_subs[i] > 0) {
					if (_poll_fds[poll_id].revents & POLLIN) {
						orb_copy(_control_topics[i], _control_subs[i], &_controls[i]);
					}
					poll_id++;
				}
			}

			//can we mix?
			if (_mixers != nullptr) {

				// XXX one output group has 8 outputs max,
				// but this driver could well serve multiple groups.
				unsigned num_outputs_max = 8;

				// Do mixing
				outputs.noutputs = _mixers->mix(&outputs.output[0], num_outputs_max);
				outputs.timestamp = hrt_absolute_time();

				// iterate actuators
				for (unsigned i = 0; i < outputs.noutputs; i++) {
					// last resort: catch NaN, INF and out-of-band errors
					if (!isfinite(outputs.output[i]) ||
						outputs.output[i] < -1.0f ||
						outputs.output[i] > 1.0f) {
						/*
						 * Value is NaN, INF or out of band - set to the minimum value.
						 * This will be clearly visible on the servo status and will limit the risk of accidentally
						 * spinning motors. It would be deadly in flight.
						 */
						outputs.output[i] = -1.0f;
					}
				}

				printf("CAN out: ");
				/* output to the bus */
				for (unsigned i = 0; i < outputs.noutputs; i++) {
					printf("%u: %8.4f ", i, outputs.output[i]);
					// can_send_xxx
				}
				printf("%s\n", (_is_armed) ? "ARMED" : "DISARMED");

			}
		}

		// Check arming state
		bool updated = false;
		orb_check(_armed_sub, &updated);

		if (updated) {
			orb_copy(ORB_ID(actuator_armed), _armed_sub, &_armed);

			// Update the armed status and check that we're not locked down
			bool set_armed = _armed.armed && !_armed.lockdown;

			arm_actuators(set_armed);
		}

		// Output commands and fetch data

		const int res = _node.spin(uavcan::MonotonicDuration::getInfinite());

		if (res < 0) {
			warnx("Spin error %i", res);
			::sleep(1);
		}
	}

	teardown();

	exit(0);
}

int
UavcanNode::control_callback(uintptr_t handle,
			 uint8_t control_group,
			 uint8_t control_index,
			 float &input)
{
	const actuator_controls_s *controls = (actuator_controls_s *)handle;

	input = controls[control_group].control[control_index];
	return 0;
}

int
UavcanNode::teardown()
{
	for (unsigned i = 0; i < NUM_ACTUATOR_CONTROL_GROUPS; i++) {
		if (_control_subs > 0) {
			::close(_control_subs[i]);
			_control_subs[i] = -1;
		}
	}
	::close(_armed_sub);
}

int
UavcanNode::arm_actuators(bool arm)
{
	bool changed = (_is_armed != arm);

	_is_armed = arm;

	if (changed) {
		// Propagate immediately to CAN bus
	}

	return OK;
}

void
UavcanNode::subscribe()
{
	// Subscribe/unsubscribe to required actuator control groups
	uint32_t sub_groups = _groups_required & ~_groups_subscribed;
	uint32_t unsub_groups = _groups_subscribed & ~_groups_required;
	_poll_fds_num = 0;
	for (unsigned i = 0; i < NUM_ACTUATOR_CONTROL_GROUPS; i++) {
		if (sub_groups & (1 << i)) {
			warnx("subscribe to actuator_controls_%d", i);
			_control_subs[i] = orb_subscribe(_control_topics[i]);
		}
		if (unsub_groups & (1 << i)) {
			warnx("unsubscribe from actuator_controls_%d", i);
			::close(_control_subs[i]);
			_control_subs[i] = -1;
		}

		if (_control_subs[i] > 0) {
			_poll_fds[_poll_fds_num].fd = _control_subs[i];
			_poll_fds[_poll_fds_num].events = POLLIN;
			_poll_fds_num++;
		}
	}
}

int
UavcanNode::pwm_ioctl(file *filp, int cmd, unsigned long arg)
{
	int ret = OK;

	lock();

	switch (cmd) {
	case PWM_SERVO_ARM:
		arm_actuators(true);
		break;

	case PWM_SERVO_SET_ARM_OK:
	case PWM_SERVO_CLEAR_ARM_OK:
	case PWM_SERVO_SET_FORCE_SAFETY_OFF:
		// these are no-ops, as no safety switch
		break;

	case PWM_SERVO_DISARM:
		arm_actuators(false);
		break;

	case MIXERIOCGETOUTPUTCOUNT:
		*(unsigned *)arg = _output_count;
		break;

	case MIXERIOCRESET:
		if (_mixers != nullptr) {
			delete _mixers;
			_mixers = nullptr;
			_groups_required = 0;
		}

		break;

	case MIXERIOCLOADBUF: {
			const char *buf = (const char *)arg;
			unsigned buflen = strnlen(buf, 1024);

			if (_mixers == nullptr)
				_mixers = new MixerGroup(control_callback, (uintptr_t)_controls);

			if (_mixers == nullptr) {
				_groups_required = 0;
				ret = -ENOMEM;

			} else {

				ret = _mixers->load_from_buf(buf, buflen);

				if (ret != 0) {
					debug("mixer load failed with %d", ret);
					delete _mixers;
					_mixers = nullptr;
					_groups_required = 0;
					ret = -EINVAL;
				} else {

					_mixers->groups_required(_groups_required);
				}
			}

			break;
		}

	default:
		ret = -ENOTTY;
		break;
	}

	unlock();

	return ret;
}

/*
 * App entry point
 */
static void print_usage()
{
	warnx("usage: uavcan start <node_id> [can_bitrate]");
}

extern "C" __EXPORT int uavcan_main(int argc, char *argv[]);

int uavcan_main(int argc, char *argv[])
{
	constexpr unsigned DEFAULT_CAN_BITRATE = 1000000;

	if (argc < 2) {
		print_usage();
		::exit(1);
	}

	if (!std::strcmp(argv[1], "start")) {
		if (argc < 3) {
			print_usage();
			::exit(1);
		}

		/*
		 * Node ID
		 */
		const int node_id = atoi(argv[2]);

		if (node_id < 0 || node_id > uavcan::NodeID::Max || !uavcan::NodeID(node_id).isUnicast()) {
			warnx("Invalid Node ID %i", node_id);
			::exit(1);
		}

		/*
		 * CAN bitrate
		 */
		unsigned bitrate = 0;

		if (argc > 3) {
			bitrate = atol(argv[3]);
		}

		if (bitrate <= 0) {
			bitrate = DEFAULT_CAN_BITRATE;
		}

		/*
		 * Start
		 */
		warnx("Node ID %u, bitrate %u", node_id, bitrate);
		return UavcanNode::start(node_id, bitrate);

	} else {
		print_usage();
		::exit(1);
	}

	return 0;
}