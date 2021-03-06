#include "ch.h"
#include "hal.h"

#include "stdlib.h"

#include "telemetry.h"
#include "mavlink_bridge.h" /* Has to be before mavlink.h */
#include "mavlink.h"
#include "chprintf.h"
#include "parameters.h"
#include "parameters_d.h"
#include "rc_input.h"
#include "rpm.h"
#include "pid_rpm.h"
#include "voltage_pid.h"
#include "servo.h"
#include "dps6015a.h"

#define SERIAL_DEVICE SD1

static virtual_timer_t led_vt; //Timer for rx
const Info * _queued_parameter = NULL;
ParamToken _queued_parameter_token;
ap_var_type _queued_parameter_type;
uint16_t _queued_parameter_index;
uint16_t _queued_parameter_count;
float voltage;

uint16_t target_rpm;
uint16_t target_rrpm;

/* If true board has full control over engine, otherwise just pass-through */
bool engine_control = false;


/* Engine state */
uint8_t engine_state = ENGINE_STOPED;

// number of 50Hz ticks until we next send this stream
uint8_t stream_ticks[NUM_STREAMS];

void handle_mavlink_message(mavlink_message_t msg);
void handle_param_request_list(mavlink_message_t *msg);
void handle_param_set(mavlink_message_t *msg);
void handle_param_request_read(mavlink_message_t *msg);
void queued_param_send(void);
bool stream_trigger(enum streams stream_num);
void data_stream_send(void);
void blink(void);
uint8_t mav_var_type(ap_var_type t);

static void led_cb(void *arg) {
    (void) arg;
    palClearPad(GPIOC, GPIOC_LED2);
}

void blink() {
    palSetPad(GPIOC, GPIOC_LED2);
	chVTSet(&led_vt, MS2ST(50), led_cb, NULL);
}

/*
 * Mavlink receive
 */
static THD_WORKING_AREA(waMavlinkThread, 2000);
static THD_FUNCTION(MavlinkThread, arg) {

	(void) arg;
	static mavlink_message_t msg;
	static mavlink_status_t status;
	msg_t charData;

	event_listener_t serialData;
	eventflags_t flags;
	chEvtRegisterMask((event_source_t *) chnGetEventSource(&SERIAL_DEVICE),
			&serialData, EVENT_MASK(1));

	chRegSetThreadName("mavlink");
	while (true) {
		chEvtWaitOneTimeout(EVENT_MASK(1), TIME_INFINITE);
		flags = chEvtGetAndClearFlags(&serialData);
        do {
            charData = chnGetTimeout(&SERIAL_DEVICE, TIME_IMMEDIATE);
            if(charData > 0xFF || charData == Q_TIMEOUT) {
                break;
            }
            if (mavlink_parse_char(MAVLINK_COMM_0, (uint8_t)charData, &msg, &status)) {
                handle_mavlink_message(msg);
                blink(); //Blink led for received packet
            }
        } while(charData != Q_TIMEOUT);
        if(flags & SD_OVERRUN_ERROR) {
            //TODO: debug message
            //blink();
        }
	}
	/* This point may be reached if shut down is requested. */
}

static THD_WORKING_AREA(waMavlinkTx, 1000);
static THD_FUNCTION(MavlinkTx, arg) {
    (void) arg;

    // Define the system type, in this case an airplane
    uint8_t system_type = MAV_TYPE_GIMBAL;
    uint8_t autopilot_type = MAV_AUTOPILOT_ARDUPILOTMEGA;

    uint8_t system_mode = MAV_MODE_AUTO_DISARMED; ///< Booting up
    uint32_t custom_mode = 0;   ///< Custom mode, can be defined by user/adopter
    uint8_t system_state = MAV_STATE_ACTIVE; ///< System ready for flight

    systime_t last_1hz = ST2MS(chVTGetSystemTime());
	while(true) {
	    systime_t now = ST2MS(chVTGetSystemTime());
	    if(abs(now - last_1hz) > 1000) {
	        if(engine_control) system_mode = MAV_MODE_AUTO_ARMED;
	        else system_mode = MAV_MODE_AUTO_DISARMED;
	        if(engine_state == ENGINE_EMERGENCY_SHUTDOWN) system_state = MAV_STATE_EMERGENCY;

	        mavlink_msg_heartbeat_send(MAVLINK_COMM_0, system_type, autopilot_type, system_mode, engine_state, system_state);
	        last_1hz = now;
	    }
	    data_stream_send();
        chThdSleepMilliseconds(20);
	}
}

void handle_mavlink_message(mavlink_message_t msg) {
	switch (msg.msgid) {
		case MAVLINK_MSG_ID_HEARTBEAT: {
			mavlink_heartbeat_t pack;
			mavlink_msg_heartbeat_decode(&msg, &pack);
			break;
		}
		case MAVLINK_MSG_ID_PARAM_REQUEST_LIST: {
	        // mark the firmware version in the tlog
		    //mavlink_msg_statustext_send(MAVLINK_COMM_0, MAV_SEVERITY_INFO, FIRMWARE_STRING);
	        //send_text(MAV_SEVERITY_INFO, FIRMWARE_STRING);
	        handle_param_request_list(&msg);
			break;
		}
	    case MAVLINK_MSG_ID_PARAM_REQUEST_READ:
	    {
	        handle_param_request_read(&msg);
	        break;
	    }
	    case MAVLINK_MSG_ID_PARAM_SET:
	    {
	        handle_param_set(&msg);
	        break;
	    }
		case MAVLINK_MSG_ID_DATA_STREAM: {
		    mavlink_data_stream_t decode;
		    mavlink_msg_data_stream_decode(&msg, &decode);
		    break;
		}
		case MAVLINK_MSG_ID_COMMAND_LONG: {
            mavlink_command_long_t pack;
            mavlink_msg_command_long_decode(&msg, &pack);
            switch (pack.command) {
                case MAV_CMD_COMPONENT_ARM_DISARM : {
                    if(pack.param1 == 1.0f) engine_control = true;
                    else engine_control = false;
                    break;
                }
            }
            mavlink_msg_command_ack_send(MAVLINK_COMM_0, pack.command,
                                   MAV_RESULT_ACCEPTED);
		}
	}
}

void handle_param_request_list(mavlink_message_t *msg) {
    mavlink_param_request_list_t packet;
    mavlink_msg_param_request_list_decode(msg, &packet);

    _queued_parameter = first_param(&_queued_parameter_token, &_queued_parameter_type);
    _queued_parameter_index = 0;
    _queued_parameter_count = count_parameters();
}

void handle_param_request_read(mavlink_message_t *msg)
{
    mavlink_param_request_read_t packet;
    mavlink_msg_param_request_read_decode(msg, &packet);

    const Info * vp;
    ap_var_type p_type;
    char param_name[AP_MAX_NAME_SIZE+1];

    if (packet.param_index != -1) {
        vp = find_by_index(packet.param_index, &p_type);
        if (vp == NULL) {
            return;
        }
        strncpy(param_name, vp->name, sizeof(param_name));
        param_name[AP_MAX_NAME_SIZE] = 0;
    } else {
        strncpy(param_name, packet.param_id, AP_MAX_NAME_SIZE);
        param_name[AP_MAX_NAME_SIZE] = 0;
        vp = find_using_name(param_name, &p_type);
        if (vp == NULL) {
            return;
        }
    }

    float value = cast_to_float(_queued_parameter_type, vp->ptr);
    mavlink_msg_param_value_send(
        MAVLINK_COMM_0,
        param_name,
        value,
        mav_var_type(p_type),
        count_parameters(),
        packet.param_index);
}


// return a MAVLink variable type given a AP_Param type
uint8_t mav_var_type(ap_var_type t)
{
    if (t == AP_PARAM_INT8) {
        return MAVLINK_TYPE_INT8_T;
    }
    if (t == AP_PARAM_INT16) {
        return MAVLINK_TYPE_INT16_T;
    }
    if (t == AP_PARAM_INT32) {
        return MAVLINK_TYPE_INT32_T;
    }
    // treat any others as float
    return MAVLINK_TYPE_FLOAT;
}

void queued_param_send(void) {
    if(_queued_parameter == NULL) {
        return;
    }
    const Info * vp;
    float value;

    vp = _queued_parameter;
    value = cast_to_float(_queued_parameter_type, vp->ptr);

    char param_name[AP_MAX_NAME_SIZE];

    strncpy(param_name, vp->name, sizeof(param_name));

    mavlink_msg_param_value_send(
        MAVLINK_COMM_0,
        param_name,
        value,
        mav_var_type(_queued_parameter_type),
        _queued_parameter_count,
        _queued_parameter_index);

    _queued_parameter = next_scalar(&_queued_parameter_token, &_queued_parameter_type);
    _queued_parameter_index++;
}

void handle_param_set(mavlink_message_t *msg)
{
    mavlink_param_set_t packet;
    mavlink_msg_param_set_decode(msg, &packet);
    ap_var_type var_type;

    // set parameter
    const Info *vp;
    char key[AP_MAX_NAME_SIZE+1];
    strncpy(key, (char *)packet.param_id, AP_MAX_NAME_SIZE);
    key[AP_MAX_NAME_SIZE] = 0;

    // find existing param so we can get the old value
    vp = find_using_name(key, &var_type);
    if (vp == NULL) {
        return;
    }
    float old_value = cast_to_float(var_type, vp->ptr);

    // set the value
    set_value(var_type, vp->ptr, packet.param_value);

    /*
      we force the save if the value is not equal to the old
      value. This copes with the use of override values in
      constructors, such as PID elements. Otherwise a set to the
      default value which differs from the constructor value doesn't
      save the change
     */
    bool force_save = !(packet.param_value == old_value);

    // save the change
    save_parameter(vp->ptr, force_save);

}

void send_parameter_value_all(const char *param_name, ap_var_type param_type,
        float param_value) {
    mavlink_msg_param_value_send(
                        MAVLINK_COMM_0,
                        param_name,
                        param_value,
                        mav_var_type(param_type),
                        count_parameters(),
                        -1);
}

void data_stream_send(void) {

    if (_queued_parameter != NULL) {
        if (stream_rates[STREAM_PARAMS] <= 0) {
            stream_rates[STREAM_PARAMS] = 10;
        }
        if (stream_trigger(STREAM_PARAMS)) {
            queued_param_send();
        }
    }

    if (stream_trigger(STREAM_RAW_SENSORS)) {
        mavlink_msg_rpm_send(MAVLINK_COMM_0, (float)no_filt_rpm(), (float)target_rrpm);
        dps6015a_state psu_state = get_psu_state();
        uint16_t volt_mil = psu_state.voltage_out * 1000; //= voltage * 1000;
        uint16_t cur_mil = psu_state.current_out * 100;
        uint32_t cur_set_mil = psu_state.current_set * 100;
        mavlink_msg_sys_status_send(MAVLINK_COMM_0, 0, 0, cur_set_mil, psu_state.switch_state, volt_mil, cur_mil, 0, 0,
                0, 0, 0, 0, 0);
        mavlink_msg_battery2_send(MAVLINK_COMM_0, voltage * 1000, -1);
    }

    if (stream_trigger(STREAM_RAW_CONTROLLER)) {
        if(pid_report == 1) {
        mavlink_msg_pid_tuning_send(MAVLINK_COMM_0, PID_TUNING_ROLL, (float)target_rrpm,
                    (float)get_rpm(), 0.0f, p_term, i_term, d_term_lpf);
        } else if(pid_report == 0) {
            mavlink_msg_pid_tuning_send(MAVLINK_COMM_0, PID_TUNING_ROLL, (float)target_voltage,
                    voltage, 0.0f, volt_p_term, volt_i_term, volt_d_term);
        }


    }

    if (stream_trigger(STREAM_RC_CHANNELS)) {
        mavlink_msg_rc_channels_raw_send(MAVLINK_COMM_0, ST2MS(chVTGetSystemTime()),
                0, get_rc_pwm(), 0, 0, 0, 0, 0, 0, 0, 0);
        mavlink_msg_servo_output_raw_send(MAVLINK_COMM_0, ST2MS(chVTGetSystemTime()),
                0, get_thr_pwm(), 0, 0, 0, 0, 0, 0, 0);
    }
}

// see if we should send a stream now. Called at 50Hz
bool stream_trigger(enum streams stream_num)
{
    if (stream_num >= NUM_STREAMS) {
        return false;
    }
    float rate = (uint8_t)stream_rates[stream_num];

    if (stream_ticks[stream_num] == 0) {
        // we're triggering now, setup the next trigger point
        if (rate > 50) {
            rate = 50;
        }
        stream_ticks[stream_num] = (50 / rate) - 1;
        return true;
    }

    // count down at 50Hz
    stream_ticks[stream_num]--;
    return false;
}

void init_telemetry() {
/*	mavlink_system.sysid = 3;
	mavlink_system.compid = MAV_COMP_ID_CAMERA; */

    palClearPad(GPIOC, GPIOC_LED2);
	chVTObjectInit(&led_vt);

	chThdCreateStatic(waMavlinkThread, sizeof(waMavlinkThread), NORMALPRIO,
			MavlinkThread, NULL);
	chThdCreateStatic(waMavlinkTx, sizeof(waMavlinkTx), NORMALPRIO,
	            MavlinkTx, NULL);
}

