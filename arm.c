/*
 * arm.c
 *
 *  Created on: Jun 22, 2013
 *      Author: theo
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdbool.h>
#include <libusb-1.0/libusb.h>
#include "net_addrs.h"
#include "arm.h"
#include "fcfutils.h"
#include "utils_libusb-1.0.h"
#include "utils_sockets.h"

#define COMPARE_BUFFER_TO_CMD(a, b, len)\
	!strncmp((char*)a, b, sizeof(b) > len? len: sizeof(b))

#define ROCKET_READY_PIN 8
#define ROCKET_READY_PORT 0

libusb_device_handle * aps;
int sd;

bool slock_enable;
bool GPS_locked;

#define ACCEL_NOISE_BOUND 0.1
static int upright = 0;

int about(double a, double b){
	return abs(a-b) < ACCEL_NOISE_BOUND;
}

void arm_receive_imu(ADISMessage * data){
	// does the acceleration vector == -1g over the last 100 samples?
	// 3.3mg per bit
	double x = data->data.adis_xaccl_out * 0.00333;
	double y = data->data.adis_yaccl_out * 0.00333;
	double z = data->data.adis_zaccl_out * 0.00333;
	if(!about(x, -1) || !about(y, 0) || !about(z, 0)){
		upright = 0;
	}
	else if(upright < 100){
		++upright;
	}
}

void arm_receive_gps(GPSMessage * data){
	// check for GPS lock
	if (data->ID[3] == '1') {
		switch(data->gps1.nav_mode) {
		case 2:   // 3D fix
		case 4:   // 3D + diff
		case 6:	  // 3D + diff + rtk
			GPS_locked = true; break;
		default:
			GPS_locked = false; break;
		}
	}
	return;
}

static void set_aps_gpio(int port, uint32_t val){
    int setport = 0x80;
    unsigned char data[64];
    int usb_err;
    data[0] = (val & 0xFF<<0)>>0;
    data[1] = (val & 0xFF<<8)>>8;
    data[2] = (val & 0xFF<<16)>>16;
    data[3] = (val & 0xFF<<24)>>24;
    usb_err = libusb_control_transfer(aps,
            LIBUSB_RECIPIENT_OTHER | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
            setport | port, 0, 0, data, 4, 2000);
    if(usb_err < 0){
//        print_libusb_error(usb_err, "set_port");
    }
    if(usb_err != 4){
        printf("set_port: Didn't send correct number of bytes");
    }
}

static void clear_aps_gpio(int port, uint32_t val){
    int clearport = 0x40;
    unsigned char data[64];
    int usb_err;
    data[0] = (val & 0xFF<<0)>>0;
    data[1] = (val & 0xFF<<8)>>8;
    data[2] = (val & 0xFF<<16)>>16;
    data[3] = (val & 0xFF<<24)>>24;
    usb_err = libusb_control_transfer(aps,
            LIBUSB_RECIPIENT_OTHER | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
            clearport | port, 0, 0, data, 4, 2000);
    if(usb_err < 0){
//        print_libusb_error(usb_err, "set_port");
    }
    if(usb_err != 4){
        printf("set_port: Didn't send correct number of bytes");
    }
}

static void send_arm_response(const char * message){
	sendto_socket(sd, message, strlen(message), ARM_IP, ARM_PORT);
}

void arm_raw_in(unsigned char *buffer, int len, unsigned char * timestamp){
	/* Commands:
	 * #YOLO - (You Only Launch Once) Arm the rocket for launch
	 * #SAFE - Disarm the rocket (default)
	 * EN_SLOCK - Enable sensors lock required to arm (default)
	 * DI_SLOCK - Disable sensors lock required to arm
	 */

	char ARM[] = "#YOLO";
	char ARM_response[] = "successful ARM";
	char ARM_response_no_aps[] = "successful ARM but RR not sent due to bad APS";
	char ARM_decline_sensors[] = "ARM failed due to sensor lock";
	char SAFE[] ="#SAFE";
	char SAFE_response[] = "successful SAFE";
	char SAFE_response_no_aps[] = "successful SAFE but RR not unsent due to bad APS";
	char EN_SLOCK []= "EN_SLOCK";
	char EN_SLOCK_response[] = "successful EN_SLOCK";
	char DI_SLOCK []= "DI_SLOCK";
	char DI_SLOCK_response[] = "Sensor Lock Overridden";
	char UNKNOWN_COMMAND[] = "UNKNOWN COMMAND";

	if(COMPARE_BUFFER_TO_CMD(buffer, ARM, len)){
		//send arm
		bool accel_locked = upright == 100;
		bool sensors_allow_launch = (GPS_locked && accel_locked) || !slock_enable;
		if(sensors_allow_launch){
			if(aps){
				set_aps_gpio(ROCKET_READY_PIN, (1<<ROCKET_READY_PORT));
				arm_send_signal("ARM");
				send_arm_response(ARM_response);
			}
			else{
				arm_send_signal("ARM");
				send_arm_response(ARM_response_no_aps);
			}
		}
		else{
			send_arm_response(ARM_decline_sensors);
		}
	}
	else if(COMPARE_BUFFER_TO_CMD(buffer, SAFE, len)){
		//send safe
		if(aps){
			clear_aps_gpio(ROCKET_READY_PIN, (1<<ROCKET_READY_PORT));
			arm_send_signal("SAFE");
			send_arm_response(SAFE_response);
		}
		else{
			arm_send_signal("SAFE");
			send_arm_response(SAFE_response_no_aps);
		}

	}
	else if(COMPARE_BUFFER_TO_CMD(buffer, EN_SLOCK, len)){
		//enable slock
		slock_enable = true;
		send_arm_response(EN_SLOCK_response);
	}
	else if(COMPARE_BUFFER_TO_CMD(buffer, DI_SLOCK, len)){
		//disable slock
		slock_enable = false;
		send_arm_response(DI_SLOCK_response);
	}
	else{
		//unknown command
		send_arm_response(UNKNOWN_COMMAND);
	}
}

void arm_init(void){
	slock_enable = true;
	char aps_name[] = "aps";
	init_libusb(aps_name);
	aps = open_device (aps_name, 0xFFFF, 0x0006);
	sd = get_send_socket();
}

void arm_final(void){
	close(sd);
	close_device(aps);
}
