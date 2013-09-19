/*
 * data_collection.c

 *
 *  Created on: 13/09/2013
 *      Author: alan
 */


#include "quadrotor.h"
#include "DebugConsole.h"
#include "board.h"
#include "types.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "mavlink.h"
#include "mavlink_bridge.h"

#include "qUART.h"
#include "lpc17xx_gpio.h"

#include "math.h"
#include "bmp085.h"
#include "HMC5883L.h"
#include "eMPL/inv_mpu.h"
#include "eMPL/inv_mpu_dmp_motion_driver.h"
#include "ultrasonic_sensor.h"
#include "qESC.h"

#define PRESCALER_VALUE 5
xSemaphoreHandle mpuSempahore;
uint8_t prescaler = PRESCALER_VALUE;
float z_bias;

#define PI 3.14159265359

#define ATTI_THRESHOLD 3.0
float atti_bias[3];
float control[4]={0.0};

uint8_t MPU6050_dmpGetEuler(float *euler, int32_t q[]) {

	float q1[4];
	uint8_t i;

	for(i = 0; i < 4; i++ ) {
		q1[i] = ((float) (q[i]>>16)) / 16384.0f;
	}

	euler[0] = atan2(2*q1[1]*q1[2] - 2*q1[0]*q1[3], 2*q1[0]*q1[0] + 2*q1[1]*q1[1] - 1);
	euler[1] = -asin(2*q1[1]*q1[3] + 2*q1[0]*q1[2]);
	euler[2] = atan2(2*q1[2]*q1[3] - 2*q1[0]*q1[1], 2*q1[0]*q1[0] + 2*q1[3]*q1[3] - 1);

	return 0;
}


uint8_t MPU6050_dmpGetAccel(float accel_0, float accel_1, float accel_2, int32_t q[]) {
	float g[3];

	float q1[4];
	uint8_t i;

	for(i = 0; i < 4; i++ ) {
		q1[i] = ((float) (q[i]>>16)) / 16384.0f;
	}

	// Este vector esta bien armado como la gravedad.
    g[0] = (2 * (q1[1]*q1[3] - q1[0]*q1[2]));
    g[1]=  -(2 * (q1[0]*q1[1] + q1[2]*q1[3]));
    g[2] = (q1[0]*q1[0] - q1[1]*q1[1] - q1[2]*q1[2] + q1[3]*q1[3]);

//    quadrotor.sv.accel[ROLL] =  accel_0;
//    quadrotor.sv.accel[PITCH] = accel_1;
//    quadrotor.sv.accel[YAW] =   accel_2;

    quadrotor.sv.accel[ROLL] =  accel_0 -  g[0];
    quadrotor.sv.accel[PITCH] = accel_1 -  g[1];
    quadrotor.sv.accel[YAW] =   accel_2 -  g[2];

//    quadrotor.sv.accel[ROLL] = accelRaw0 - g[0];
//    quadrotor.sv.accel[PITCH] = accelRaw1 - g[1];
//    quadrotor.sv.accel[YAW] = accelRaw2 - g[2];


    return 0;
}

float map(long x, long in_min, long in_max, float out_min, float out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// Hardware IRQ for the MPU6050 DMP algorithm.
void EINT3_IRQHandler(void)
{
	static signed portBASE_TYPE xHigherPriorityTaskWoken;
    xHigherPriorityTaskWoken = pdFALSE;

	if(GPIO_GetIntStatus(0, 4, 1))
	{
		GPIO_ClearInt(0,(1<<4));
		if (mpuSempahore!=NULL){
			xSemaphoreGiveFromISR(mpuSempahore,&xHigherPriorityTaskWoken);
		}
	}
	portEND_SWITCHING_ISR( xHigherPriorityTaskWoken );
}

float current_alt_sp = 0.0;

void DataCollection(void *p){

	int16_t gyro[3];
	int16_t accel[3];
	int32_t quat[4];
	float scale_gyro;
	uint16_t scale_accel;
	uint8_t more;
	float atti_buffer[3];
	int16_t sensors;
	float bmp_temp, pressure, alt=0.0, c;
	uint16_t i;
	float second_derivate,alt_1,alt_2;

	mpu_get_gyro_sens(&scale_gyro);
	mpu_get_accel_sens(&scale_accel);
	vSemaphoreCreateBinary(mpuSempahore);
	xSemaphoreTake(mpuSempahore,0);
	NVIC_EnableIRQ(EINT3_IRQn);

	// ---- Barometer config ------------
	c = 0.1;

	//===========================================
	prescaler = PRESCALER_VALUE;
	while(1){

		// Wait here for MPU DMP interrupt at 200Hz
		xSemaphoreTake(mpuSempahore,portMAX_DELAY); //FIXME: instead of portMAX it would be nice to have a time out for errors

		//-----------------------------------------------------------------------
		// Data input stage
		//-----------------------------------------------------------------------

		portENTER_CRITICAL();
		dmp_read_fifo(gyro, accel, quat, NULL, &sensors, &more);
		portEXIT_CRITICAL();

		MPU6050_dmpGetEuler(atti_buffer,quat);

		atti_buffer[0] = atti_buffer[0]*180.0/PI;
		atti_buffer[1] = atti_buffer[1]*180.0/PI;
		atti_buffer[2] = atti_buffer[2]*180.0/PI;


#if USE_BAROMETER
		//quadrotor.sv.temperature = BMP085_GetTemperature();
		//quadrotor.sv.current_pressure = BMP085_GetPressure();
		//quadrotor.sv.altitude =  c*BMP085_CalculateAltitude(quadrotor.sv.floor_pressure, quadrotor.sv.current_pressure) + (1-c)*quadrotor.sv.altitude;
#endif

		// --------------------- Biasing ---------------------

		if ((quadrotor.mavlink_control.buttons & BTN_START) != 0){
			atti_bias[ROLL] = atti_buffer[2];
			atti_bias[PITCH] = -atti_buffer[1];
			atti_bias[YAW] = atti_buffer[0];


			uint8_t i;
			for (i=0;i<3;i++){
				qPID_Init(&quadrotor.rateController[i]);
			}

			for (i=0;i<3;i++){
				qPID_Init(&quadrotor.attiController[i]);
			}
			qPID_Init(&quadrotor.altitudeController);

		}

		quadrotor.sv.attitude[0] = atti_buffer[2] - atti_bias[ROLL];
		quadrotor.sv.attitude[1] = -atti_buffer[1] - atti_bias[PITCH];
		quadrotor.sv.attitude[2] = atti_buffer[0] - atti_bias[YAW];

		quadrotor.sv.rate[ROLL] = (float)-gyro[0]/scale_gyro;
		quadrotor.sv.rate[PITCH] = (float) gyro[1]/scale_gyro;
		quadrotor.sv.rate[YAW] =  (float)-gyro[2]/scale_gyro;

//		quadrotor.sv.accel[0] = (float) accel[0]/scale_accel;    // Xb
//		quadrotor.sv.accel[1] = - (float)  accel[1]/scale_accel; // Yb
//		quadrotor.sv.accel[2] = (float) accel[2]/scale_accel; 	 // Zb

		MPU6050_dmpGetAccel((float)accel[0]/scale_accel,(float)-accel[1]/scale_accel,(float)accel[2]/scale_accel,quat);

		if (quadrotor.mavlink_system.nav_mode == NAV_TAKEOFF){
			if (current_alt_sp < quadrotor.sv.setpoint[ALTITUDE]){
				current_alt_sp  += quadrotor.sv.setpoint[ALTITUDE] / (200.0*5.0); // 2 seconds manouver in 200 steps
			}else{
				//quadrotor.mavlink_system.nav_mode == NAV_ALTHOLD;
			}
		}else if (quadrotor.mavlink_system.nav_mode == NAV_LANDING){
			if (current_alt_sp > 0){
				current_alt_sp  -= quadrotor.sv.setpoint[ALTITUDE] / (200.0*5.0); // 5 seconds manouver in 200 steps
			}else{
				//quadrotor.mavlink_system.nav_mode == NAV_ATTI;
			}
		}

		quadrotor.sv.setpoint[ROLL] = map(quadrotor.mavlink_control.y,-1000,1000,-40.0,40.0);
		quadrotor.sv.setpoint[PITCH] = map(quadrotor.mavlink_control.x,-1000,1000,-40.0,40.0);
		quadrotor.sv.setpoint[YAW] = map(quadrotor.mavlink_control.r,-1000,1000,-180.0,180.0);

		//-----------------------------------------------------------------------
		// Controller processing stage
		//-----------------------------------------------------------------------

		// CAS
		quadrotor.sv.attiCtrlOutput[ROLL] = qPID_Procees(&quadrotor.attiController[ROLL],quadrotor.sv.setpoint[ROLL],quadrotor.sv.attitude[ROLL]);
		quadrotor.sv.attiCtrlOutput[PITCH] = qPID_Procees(&quadrotor.attiController[PITCH],quadrotor.sv.setpoint[PITCH],quadrotor.sv.attitude[PITCH]);

		// SAS
		quadrotor.sv.rateCtrlOutput[ROLL] = qPID_Procees(&quadrotor.rateController[ROLL],quadrotor.sv.attiCtrlOutput[ROLL],quadrotor.sv.rate[ROLL]);
		quadrotor.sv.rateCtrlOutput[PITCH] = qPID_Procees(&quadrotor.rateController[PITCH],quadrotor.sv.attiCtrlOutput[PITCH],quadrotor.sv.rate[PITCH]);
		quadrotor.sv.rateCtrlOutput[YAW] = qPID_Procees(&quadrotor.rateController[YAW],quadrotor.sv.setpoint[YAW],quadrotor.sv.rate[YAW]);

		if (prescaler-- == 0){
			RangeFinder_getDistance();

			// Outlayer filter
			second_derivate = (quadrotor.sv.altitude-2*alt_1+alt_2)*40; //200/5 Hz
			alt_2 = alt_1;
			alt_1 = quadrotor.sv.altitude;
			if (fabs(second_derivate)<5.0){
				quadrotor.sv.altitudeCtrlOutput = qPID_Procees(&quadrotor.altitudeController,current_alt_sp,quadrotor.sv.altitude);
			}

			prescaler = PRESCALER_VALUE;


		}

		//-----------------------------------------------------------------------
		// Control Output stage
		//-----------------------------------------------------------------------

		control[ROLL] = quadrotor.sv.rateCtrlOutput[ROLL];
		control[PITCH] =  quadrotor.sv.rateCtrlOutput[PITCH];
		control[YAW] = -quadrotor.sv.rateCtrlOutput[YAW]; //FIXME: there is a problem with the sign (maybe in the Mq)
		control[ALTITUDE] = quadrotor.sv.altitudeCtrlOutput;

		quadrotor.sv.motorOutput[0] = (	control[ALTITUDE]*K_Z - control[ROLL]*K_PHI - control[PITCH]*K_THETA - control[YAW]*K_PSI	);
		quadrotor.sv.motorOutput[1] = (	control[ALTITUDE]*K_Z - control[ROLL]*K_PHI + control[PITCH]*K_THETA + control[YAW]*K_PSI	);
		quadrotor.sv.motorOutput[2] = (	control[ALTITUDE]*K_Z + control[ROLL]*K_PHI + control[PITCH]*K_THETA - control[YAW]*K_PSI	);
		quadrotor.sv.motorOutput[3] = (	control[ALTITUDE]*K_Z + control[ROLL]*K_PHI - control[PITCH]*K_THETA + control[YAW]*K_PSI	);

		if (quadrotor.mode == ESC_STANDBY){
			qESC_SetOutput(MOTOR1,0);
			qESC_SetOutput(MOTOR2,0);
			qESC_SetOutput(MOTOR3,0);
			qESC_SetOutput(MOTOR4,0);

			if ((fabsf(quadrotor.sv.attitude[ROLL])<=ATTI_THRESHOLD) && (fabsf(quadrotor.sv.attitude[PITCH])<=ATTI_THRESHOLD) && (fabsf(quadrotor.sv.attitude[YAW])<=ATTI_THRESHOLD)){
				for (i=1;i<5;i++) qLed_TurnOn(leds[i]);
			}else{
				for (i=1;i<5;i++) qLed_TurnOff(leds[i]);
			}
		}else{
			// Motor command
			qESC_SetOutput(MOTOR1,quadrotor.sv.motorOutput[0]);
			qESC_SetOutput(MOTOR2,quadrotor.sv.motorOutput[1]);
			qESC_SetOutput(MOTOR3,quadrotor.sv.motorOutput[2]);
			qESC_SetOutput(MOTOR4,quadrotor.sv.motorOutput[3]);
		}


	}

}

