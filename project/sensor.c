// SENSOR FUSION CODE

#include "sensor.h"
#include "startup.h"
#include "PWM.h"
#include "lapack.h"
#include "blas.h"
#include "MPU9250.h"
#include "MadgwickAHRS.h"
#include "mInv.h"

#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include <wiringPi.h>
#include <wiringPiI2C.h>
#include <wiringSerial.h>
#include <errno.h>
#include <math.h>
#include <errno.h>

// PREEMPT_RT
// #include <time.h>
#include <sched.h>
#include <sys/mman.h>

#define PI 3.141592653589793
#define CALIBRATION 500
#define BUFFER 100

/******************************************************************/
/*******************VARIABLES & PREDECLARATIONS********************/
/******************************************************************/

// Predeclarations
static void *threadReadBeacon (void*);
static void *threadSensorFusion (void*);
static void *threadPWMControl (void*);
static void *threadPipeCommunicationToSensor(void*);

void Qq(double*, double*);
void dQqdq(double*, double*, double*, double*, double*, double*, double*);
//void printmat(double*, int, int);
void getOrientationEulers(double*, double*, double*);
void qNormalize(double*);
void Sq(double*, double*, double);
void Somega(double*, double*);
void q2euler(double*, double*);
void accelerometerUpdate(double*, double*, double*, double*, double*);
void gyroscopeUpdate(double*, double*, double*, double*, double);
void magnetometerUpdate(double*, double*, double*, double*, double*, double);
void sensorCalibration(double*, double*, double*, double*, double*, double*, double*, double*, double*, double*, double*, double*, int);
void ekfCalibration(double*, double*, double*, double*, int);
void ekfCalibration6x6(double*, double*, double*, double*, int);
void ekfCalibration9x9_bias(double*, double*, double*, double*, int);
void ekfCalibration9x9(double*, double*, double*, double*, int);
						
int loadSettings(double*, char*, int);
//void saveSettings(double*, char*, int, FILE**);
void saveSettings(double*, char*, int);
//void saveData(double*, char* , int, FILE**, int);
void saveData(double*, char* , int);
void printBits(size_t const, void const * const);

void EKF(double*, double*, double*, double*, double*, double*, double, int);
void EKF_bias(double*, double*, double*, double*, double*, double*, double, int);
void EKF_no_inertia(double*, double*, double*, double*, double*, double*, double, int);
void EKF_6x6(double*, double*, double*, double*, double*, double*, double);
void EKF_9x9(double*, double*, double*, double*, double*, double*, double, int, double*);
void EKF_9x9_bias(double*, double*, double*, double*, double*, double*, double);
void fx(double*, double*, double*, double);
void Jfx(double*, double*, double*, double);
void fx_bias(double*, double*, double*, double);
void Jfx_bias(double*, double*, double*, double);
void fx_no_inertia(double*, double*, double*, double);
void Jfx_no_inertia(double*, double*, double*, double);
void fx_6x1(double*, double*, double*, double);
void Jfx_6x6(double*, double*, double*, double);
void fx_9x1(double*, double*, double*, double, double*);
void Jfx_9x9(double*, double*, double*, double, double*);
void fx_9x1_bias(double*, double*, double*, double);
void Jfx_9x9_bias(double*, double*, double*, double);


// Static variables for threads
static double sensorRawDataPosition[3]={0,0,0}; // Global variable in sensor.c to communicate between IMU read and angle fusion threads
static double controlData[4]={1,1,1,1}; // Global variable in sensor.c to pass control signal u from controller.c to EKF in sensor fusion
static double keyboardData[14]= { 0, 0, 0, 0, 0, 0, 0, 0 , 0, 0, 0, 0.01, 0.05, 0}; // {ref_x,ref_y,ref_z, switch [0=STOP, 1=FLY], PWM print, Timer print, EKF print, reset ekf/mpc, EKF print 6 states, restart calibration, ramp ref, alpha, beta, mpc position toggle}
static double tuningEkfData[18]={ekf_Q_1,ekf_Q_2,ekf_Q_3,ekf_Q_4,ekf_Q_5,ekf_Q_6,ekf_Q_7,ekf_Q_8,ekf_Q_9,ekf_Q_10,ekf_Q_11,ekf_Q_12,ekf_Q_13,ekf_Q_14,ekf_Q_15,ekf_Q_16,ekf_Q_17,ekf_Q_18};

// Variables
static int beaconConnected=0;

// Mutexes
static pthread_mutex_t mutexPositionSensorData = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutexI2CBusy = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutexControlData = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutexKeyboardData = PTHREAD_MUTEX_INITIALIZER;


/******************************************************************/
/*************************START PROCESS****************************/
/******************************************************************/

// Function to start the sensor process threads
void startSensors(void *arg1, void *arg2){
	// Create pipe array
	pipeArray pipeArrayStruct = {.pipe1 = arg1, .pipe2 = arg2 };
	
	// Create thread
	pthread_t threadSenFus, threadPWMCtrl, threadCommToSens; // threadReadPos,
	int threadPID2, threadPID3, threadPID4; //t hreadPID1, 
	
	//threadPID1=pthread_create(&threadReadPos, NULL, &threadReadBeacon, NULL);
	threadPID2=pthread_create(&threadSenFus, NULL, &threadSensorFusion, &pipeArrayStruct);
	threadPID3=pthread_create(&threadPWMCtrl, NULL, &threadPWMControl, arg1);
	threadPID4=pthread_create(&threadCommToSens, NULL, &threadPipeCommunicationToSensor, arg2);
	
	// Set up thread scheduler priority for real time tasks
	struct sched_param paramThread2, paramThread3, paramThread4; // paramThread1, 
	//paramThread1.sched_priority = PRIORITY_SENSOR_BEACON; // set priorities
	paramThread2.sched_priority = PRIORITY_SENSOR_FUSION;
	paramThread3.sched_priority = PRIORITY_SENSOR_PWM;
	paramThread4.sched_priority = PRIORITY_SENSOR_PIPE_COMMUNICATION;
	//if(sched_setscheduler(threadPID1, SCHED_FIFO, &paramThread1)==-1) {perror("sched_setscheduler failed for threadPID1");exit(-1);}
	if(sched_setscheduler(threadPID2, SCHED_FIFO, &paramThread2)==-1) {perror("sched_setscheduler failed for threadPID2");exit(-1);}
	if(sched_setscheduler(threadPID3, SCHED_FIFO, &paramThread3)==-1) {perror("sched_setscheduler failed for threadPID3");exit(-1);}
	if(sched_setscheduler(threadPID4, SCHED_FIFO, &paramThread4)==-1) {perror("sched_setscheduler failed for threadPID3");exit(-1);}
	
	// If threads created successful, start them
	//if (!threadPID1) pthread_join( threadReadPos, NULL);
	if (!threadPID2) pthread_join( threadSenFus, NULL);
	if (!threadPID3) pthread_join( threadPWMCtrl, NULL);
	if (!threadPID4) pthread_join( threadCommToSens, NULL);
}


/******************************************************************/
/*****************************THREADS******************************/
/******************************************************************/

// Thread - Commnication.c to Sensor.c with keyobard inputs
static void *threadPipeCommunicationToSensor(void *arg){
	// Get pipe and define local variables
	structPipe *ptrPipe = arg;
	double communicationDataBuffer[52];
	double keyboardDataBuffer[14];
	double tuningEkfBuffer[18];
	
	/// Setup timer variables for real time performance check
	struct timespec t_start,t_stop;
	
	/// Average sampling
	int tsAverageCounter=0;
	double tsAverageAccum=0;
	double tsAverage=tsController, tsTrue;
	int timerPrint=0;
	
	/// Lock memory
	if(mlockall(MCL_CURRENT) == -1){
		perror("mlockall failed in threadPWMControl");
	}
	
	// Loop forever reading/waiting for data
	while(1){
		/// Time it
		clock_gettime(CLOCK_MONOTONIC ,&t_start); // start elapsed time clock
		
		// Read data from controller process
		if(read(ptrPipe->child[0], communicationDataBuffer, sizeof(communicationDataBuffer)) == -1) printf("read error in sensor from communication\n");
		//else printf("Sensor ID: %d, Recieved Communication data: %f\n", (int)getpid(), keyboardDataBuffer[0]);
				
		memcpy(keyboardDataBuffer, communicationDataBuffer, sizeof(communicationDataBuffer)*14/52);
		//memcpy(tuningMpcBuffer, communicationDataBuffer+14, sizeof(communicationDataBuffer)*14/52);
		//memcpy(tuningMpcBufferControl, communicationDataBuffer+28, sizeof(communicationDataBuffer)*6/52);
		memcpy(tuningEkfBuffer, communicationDataBuffer+34, sizeof(communicationDataBuffer)*18/52);
		
		// Put new data in to global variable in communication.c
		pthread_mutex_lock(&mutexKeyboardData);
			memcpy(keyboardData, keyboardDataBuffer, sizeof(keyboardDataBuffer));
			memcpy(tuningEkfData, tuningEkfBuffer, sizeof(tuningEkfBuffer));
			timerPrint=(int)keyboardData[5];
		pthread_mutex_unlock(&mutexKeyboardData);
		
		/// Print true sampling rate
		clock_gettime(CLOCK_MONOTONIC, &t_stop);
		tsTrue=(t_stop.tv_sec - t_start.tv_sec) + (t_stop.tv_nsec - t_start.tv_nsec) / NSEC_PER_SEC;
		//printf("Sampling time [s] PWM received: %lf\n",tsTrue);
		
		/// Get average sampling time
		if(tsAverageCounter<50){
			tsAverageAccum+=tsTrue;
			tsAverageCounter++;
		}
		else{
			tsAverageAccum/=50;
			tsAverage=tsAverageAccum;
			if(timerPrint){
				printf("Sensor pipe from Communication Read: tsAverage %lf tsTrue %lf\n", tsAverage, tsTrue);
			}
			tsAverageCounter=0;
			tsAverageAccum=0;
			
		}
	}
	return NULL;
}

// Thread - Read position values
void *threadReadBeacon (void *arg){
	// Define local variables
	double posRaw[3], tsTrue=tsReadBeacon;
	//double beaconTimestamp = 0.0;
	//double beaconTimestampPrev = 0.0;
	uint8_t data8[100];	//this must be number of beacons*14+6+2 at least!
	uint16_t data16;
	uint32_t data32;
	//double beacons[16];	//each beacon needs 4 doubles, address and XYZ
	int n;
	//double hedgehogReading[1] = {0};
	int beaconFlag = 0;
	//int beaconsCoordinates = 0;
	//double dummy = 0.0;
	int fdBeacon;
	
	/// Setup timer variables for real time
	struct timespec t,t_start,t_stop;

	/// Lock memory
	if(mlockall(MCL_CURRENT) == -1){
		perror("mlockall failed in threadSensorFusion");
	}
	
	/// Start after 1 second
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec++;
	
	// Loop forever trying to connect to Beacon sensor via USB
	while(1){
		// Open serial communication
		if ((fdBeacon=serialOpen("/dev/ttyACM0", 115200)) < 0){
			//fprintf(stderr, "Unable to open serial device: %s\n", strerror (errno));
		}
		else{
			// Activate wiringPiSetup
			if (wiringPiSetup() == -1){
				fprintf (stdout, "Unable to start wiringPi: %s\n", strerror (errno));
			}
			else{
				beaconConnected=1; // set flag true for the calibration to start when ready
				// Loop for ever reading data
				while(1){
					/// Time it and wait until next shot
					clock_gettime(CLOCK_MONOTONIC ,&t_start); // start elapsed time clock
					clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL); // sleep for necessary time to reach desired sampling time
								
					//counter++;			 
					// Read serial data when available
					if (serialDataAvail(fdBeacon)){
						if ((int)(data8[0]=serialGetchar(fdBeacon)) == -1){
							fprintf(stderr, "serialGetchar, block for 10s: %s\n", strerror (errno));
						}
						else if (data8[0] == 0xff){
							data8[1] = serialGetchar(fdBeacon);	// Yype of packet: 0x47 for both hedgehog coordinates and every 10s about beacons coordinates
							data8[2] = serialGetchar(fdBeacon); // this and 3 are both code of data in packet
							data8[3] = serialGetchar(fdBeacon);
							data8[4] = serialGetchar(fdBeacon);	// number of bytes of data transmitting
							
							if ((data16=data8[3] << 8| data8[2]) == 0x0011 && data8[1] == 0x47 && data8[4] == 0x16){
								//hedgehogReading[0] = 1.0; //reading mm				
								n = (int)(data8[4]);
								//printf("%d", n);
								for (int i=0;i<n;i++){
									data8[5+i] = serialGetchar(fdBeacon);
								}
								data8[5+n] = serialGetchar(fdBeacon); // This and (6+n) are CRC-16
								data8[6+n] = serialGetchar(fdBeacon);
								
								//beaconTimestamp = (double)(data32 = data8[8] << 24| data8[7] << 16| data8[6] << 8| data8[5]);			
								// Raw position uint data to float
								posRaw[0] = (double)(int32_t)(data32 = data8[12] << 24| data8[11] << 16| data8[10] << 8| data8[9])*0.001;
								posRaw[1] = (double)(int32_t)(data32 = data8[16] << 24| data8[15] << 16| data8[14] << 8| data8[13])*0.001;	
								posRaw[2] = (double)(int32_t)(data32 = data8[20] << 24| data8[19] << 16| data8[18] << 8| data8[17])*0.001;
								
								beaconFlag = (data8[21] >> 0) & 1; //takes the bit number 1 of it!
								//printf("beaconFlag %i\n", beaconFlag);						
								if ( beaconFlag == 0 ) {
									// Copy raw position to global variable for use in sensor fusion thread
									pthread_mutex_lock(&mutexPositionSensorData);
										memcpy(sensorRawDataPosition, posRaw, sizeof(posRaw));
										//memcpy(sensorRawDataPosition+3, hedgehogReading, sizeof(hedgehogReading));
										//memcpy(sensorRawDataPosition+4, &beaconTimestamp, sizeof(beaconTimestamp));
										//memcpy(sensorRawDataPosition+5, &beaconFlag, sizeof(beaconFlag));
									pthread_mutex_unlock(&mutexPositionSensorData);
								}
								else {
									printf("beaconFlag ERROR, flag is %i\n", beaconFlag); //keep this on RPi console for troubleshooting
								}
								
								//printf("INSIDE %i    ", counter);
								//printmat(sensorRawDataPosition, 1, 4);
								
								// beaconFlag=(int)data8[21];
								//printf("%.0f => X=%.3f, Y=%.3f, Z=%.3f at %f with all flags (mm)\n", hedgehogReading[0], posRaw[0], posRaw[1], posRaw[2], 1/(beaconTimestamp-beaconTimestampPrev)*1000);
								// printBits(1, &beaconFlag);
								//beaconTimestampPrev = beaconTimestamp;
								
								//usleep(15000);
							}
							else if ((data16=data8[3] << 8| data8[2]) == 0x0001 && data8[1] == 0x47 && data8[4] == 0x10){
								//hedgehogReading[0] = 2.0; //reading cm				
								n = (int)(data8[4]);
								//printf("%d", n);
								for (int i=0;i<n;i++){
									data8[5+i] = serialGetchar(fdBeacon);
								}
								data8[5+n] = serialGetchar(fdBeacon); // This and (6+n) are CRC-16
								data8[6+n] = serialGetchar(fdBeacon);
								
								//beaconTimestamp = (double)(data32 = data8[8] << 24| data8[7] << 16| data8[6] << 8| data8[5]);			
								// Raw position uint data to float
								posRaw[0] = (double)(int16_t)(data16 = data8[10] << 8| data8[9])*0.01;
								posRaw[1] = (double)(int16_t)(data16 = data8[12] << 8| data8[11])*0.01;	
								posRaw[2] = (double)(int16_t)(data16 = data8[14] << 8| data8[13])*0.01;
								
								beaconFlag = (data8[15] >> 0) & 1; //takes the bit number 1 of it!
								//printf("beaconFlag %i\n", beaconFlag);					
								if ( beaconFlag == 1 ) {
									// Copy raw position to global variable for use in sensor fusion thread
									pthread_mutex_lock(&mutexPositionSensorData);
										memcpy(sensorRawDataPosition, posRaw, sizeof(posRaw));
										//memcpy(sensorRawDataPosition+3, hedgehogReading, sizeof(hedgehogReading));
										//memcpy(sensorRawDataPosition+4, &beaconTimestamp, sizeof(beaconTimestamp));
										//memcpy(sensorRawDataPosition+5, &beaconFlag, sizeof(beaconFlag));
									pthread_mutex_unlock(&mutexPositionSensorData);	
								}
								else {
									printf("beaconFlag ERROR, flag is %i\n", beaconFlag); //keep this on RPi console for troubleshooting
								}
								
								//beaconFlag=(int)data8[15];	
								//printf("%.0f => X=%.3f, Y=%.3f, Z=%.3f at %f with all flags (cm)\n", hedgehogReading, posRaw[0], posRaw[1], posRaw[2], 1/(beaconTimestamp-beaconTimestampPrev)*1000);
								//printBits(1, &beaconFlag);
								//beaconTimestampPrev = beaconTimestamp;
								
								//usleep(15000);
							}
							else if ((data16=data8[3] << 8| data8[2]) == 0x0002 && data8[1] == 0x47){//cm
								data8[5] = serialGetchar(fdBeacon); // number of beacons
								n = (int)(data8[4]);
								//printf("%d", n);
								for (int i=0;i<n;i++){
									data8[6+i] = serialGetchar(fdBeacon);
								}
								data8[6+n] = serialGetchar(fdBeacon); // This and (7+n) are CRC-16
								data8[7+n] = serialGetchar(fdBeacon);
								/*
								beacons[0] = (double)(data8[6]);//address
								beacons[1] = (double)(data16 = data8[8] << 8| data8[7])*0.01;
								beacons[2] = (double)(data16 = data8[10] << 8| data8[9])*0.01;
								beacons[3] = (double)(data16 = data8[12] << 8| data8[11])*0.01;
								beacons[4] = (double)(data8[14]);//address
								beacons[5] = (double)(data16 = data8[16] << 8| data8[15])*0.01;
								beacons[6] = (double)(data16 = data8[18] << 8| data8[17])*0.01;
								beacons[7] = (double)(data16 = data8[20] << 8| data8[19])*0.01;
								beacons[8] = (double)(data8[22]);//address
								beacons[9] = (double)(data16 = data8[24] << 8| data8[23])*0.01;
								beacons[10] = (double)(data16 = data8[26] << 8| data8[25])*0.01;
								beacons[11] = (double)(data16 = data8[28] << 8| data8[27])*0.01;
								beacons[12] = (double)(data8[30]);//address
								beacons[13] = (double)(data16 = data8[32] << 8| data8[31])*0.01;
								beacons[14] = (double)(data16 = data8[34] << 8| data8[33])*0.01;
								beacons[15] = (double)(data16 = data8[36] << 8| data8[35])*0.01;
								*/
								//printf("%i beacons:\nBeacon%i> %.3f %.3f %.3f, Beacon%i> %.3f %.3f %.3f,\nBeacon%i> %.3f %.3f %.3f, Beacon%i> %.3f %.3f %.3f\n", data8[5], (int)beacons[0] ,beacons[1] ,beacons[2], beacons[3], (int)beacons[4], beacons[5], beacons[6], beacons[7], (int)beacons[8], beacons[9], beacons[10], beacons[11], (int)beacons[12], beacons[13], beacons[14], beacons[15]);
								
								//usleep(15000);
							}
							else if ((data16=data8[3] << 8| data8[2]) == 0x0012 && data8[1] == 0x47){//mm
								data8[5] = serialGetchar(fdBeacon); // number of beacons
								n = (int)(data8[4]);
								//printf("%d", n);
								for (int i=0;i<n;i++){
									data8[6+i] = serialGetchar(fdBeacon);
								}
								data8[6+n] = serialGetchar(fdBeacon); // This and (7+n) are CRC-16
								data8[7+n] = serialGetchar(fdBeacon);
								/*
								beacons[0] = (double)(data8[6]);//address
								beacons[1] = (double)(data32 = data8[10] << 24| data8[9] << 16| data8[8] << 8| data8[7])*0.001;
								beacons[2] = (double)(data32 = data8[14] << 24| data8[13] << 16| data8[12] << 8| data8[11])*0.001; //?????????
								beacons[3] = (double)(data32 = data8[18] << 24| data8[17] << 16| data8[16] << 8| data8[15])*0.001;
								beacons[4] = (double)(data8[20]);//address
								beacons[5] = (double)(data32 = data8[24] << 24| data8[23] << 16| data8[22] << 8| data8[21])*0.001;
								beacons[6] = (double)(data32 = data8[28] << 24| data8[27] << 16| data8[26] << 8| data8[25])*0.001;
								beacons[7] = (double)(data32 = data8[32] << 24| data8[31] << 16| data8[30] << 8| data8[29])*0.001;
								beacons[8] = (double)(data8[34]);//address
								beacons[9] = (double)(data32 = data8[38] << 24| data8[37] << 16| data8[36] << 8| data8[35])*0.001;
								beacons[10] = (double)(data32 = data8[42] << 24| data8[41] << 16| data8[40] << 8| data8[39])*0.001;
								beacons[11] = (double)(data32 = data8[46] << 24| data8[45] << 16| data8[44] << 8| data8[43])*0.001;
								beacons[12] = (double)(data8[48]);//address
								beacons[13] = (double)(data32 = data8[52] << 24| data8[51] << 16| data8[50] << 8| data8[49])*0.001;
								beacons[14] = (double)(data32 = data8[56] << 24| data8[55] << 16| data8[54] << 8| data8[53])*0.001;
								beacons[15] = (double)(data32 = data8[60] << 24| data8[59] << 16| data8[58] << 8| data8[57])*0.001;
								*/
								//printf("%i beacons:\nBeacon%i> %.3f %.3f %.3f, Beacon%i> %.3f %.3f %.3f,\nBeacon%i> %.3f %.3f %.3f, Beacon%i> %.3f %.3f %.3f\n", data8[5], (int)beacons[0] ,beacons[1] ,beacons[2], beacons[3], (int)beacons[4], beacons[5], beacons[6], beacons[7], (int)beacons[8], beacons[9], beacons[10], beacons[11], (int)beacons[12], beacons[13], beacons[14], beacons[15]);
								
								//usleep(15000);
							}
							else{
								//printf("data8[1] -> %04x\n", data8[1]);
								printf("Unrecognized code of data in packet -> %04x  and  %02x\n", data16, data8[1]);
								//usleep(20000);
							}
						}
						else{
							//usleep(20000);	// if it is not broadcasting 0xff
						}
					}
					else{// else not avail
						//printf("else %i\n", counter);
						/*hedgehogReading[0] = 0.0; //reading is done!
						posRaw[0] = NAN;
						posRaw[1] = NAN;	
						posRaw[2] = NAN;
						pthread_mutex_lock(&mutexPositionSensorData);
							memcpy(sensorRawDataPosition, posRaw, sizeof(posRaw));
							//memcpy(sensorRawDataPosition+3, hedgehogReading, sizeof(hedgehogReading));
							//memcpy(sensorRawDataPosition+4, &beaconTimestamp, sizeof(beaconTimestamp));
							//memcpy(sensorRawDataPosition+5, &beaconFlag, sizeof(beaconFlag));
						pthread_mutex_unlock(&mutexPositionSensorData);*/
						
						//usleep(15000);	// if Data not avail
					}
					
					/// Calculate next shot
					t.tv_nsec += tsReadBeacon;
					while (t.tv_nsec >= NSEC_PER_SEC) {
						t.tv_nsec -= NSEC_PER_SEC;
						t.tv_sec++;
					}	
					
					/// Print true sampling rate
					clock_gettime(CLOCK_MONOTONIC, &t_stop);
					tsTrue=(t_stop.tv_sec - t_start.tv_sec) + (t_stop.tv_nsec - t_start.tv_nsec) / NSEC_PER_SEC;
					//printf("Sampling time [s] read beacon: %lf\n",tsTrue);
				}
			}
			t.tv_sec+=2; // wiringpi not activated, sleep for 2 and retry
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);

		}
		t.tv_sec+=2; // usb not open, sleep for 2 and retry
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);

	}
	return NULL;
}

// Thread - Sensor fusion Orientation and Position
static void *threadSensorFusion (void *arg){
	// Get pipe array and define local variables
	pipeArray *pipeArrayStruct = arg;
	structPipe *ptrPipe1 = pipeArrayStruct->pipe1;
	structPipe *ptrPipe2 = pipeArrayStruct->pipe2;
	
	// Define local variables
	double accRaw[3]={0,0,0}, gyrRaw[3]={0,0,0}, magRaw[3]={0,0,0}, magRawRot[3], tempRaw=0, euler[3]={0,0,0}; // acc0[3]={0,0,0}, gyr0[3]={0,0,0}, mag0[3]={0,0,0}, accCal[3*CALIBRATION], gyrCal[3*CALIBRATION], magCal[3*CALIBRATION], 
	double L=1, normMag=0, sensorDataBuffer[19]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}; // Racc[9]={0,0,0,0,0,0,0,0,0}, Rgyr[9]={0,0,0,0,0,0,0,0,0}, Rmag[9]={0,0,0,0,0,0,0,0,0}, Patt[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}, q[4]={1,0,0,0},
	double posRaw[3]={0,0,0}, posRawPrev[3]={0,0,0}, stateDataBuffer[19]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double tsTrue=tsSensorsFusion; // true sampling time measured using clock_gettime() ,ts_save_buffer
	int  calibrationCounterEKF=0, posRawOldFlag=0, enableMPU9250Flag=-1, enableAK8963Flag=-1; // calibrationCounter=0, calibrationLoaded=0,

	
	// Save to file buffer variable
	double buffer_u1[BUFFER];
	double buffer_u2[BUFFER];
	double buffer_u3[BUFFER];
	double buffer_u4[BUFFER];
	double buffer_omega_x[BUFFER];
	double buffer_omega_y[BUFFER];
	double buffer_omega_z[BUFFER];
	double buffer_angle_x[BUFFER];
	double buffer_angle_y[BUFFER];
	double buffer_angle_z[BUFFER];
	int buffer_counter=0;
	//FILE *fpWrite;
	
	// EKF variables
	double Pekf9x9_bias[81]={1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1};
	double Pekf9x9[81]={1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1};
	double xhat9x9_bias[9]={0,0,0,0,0,0,0,0,0};
	double xhat9x9[9]={0,0,0,0,0,0,0,0,-par_g};
	double uControl[4]={.1,.1,.1,.1};
	
	double Pekf9x9_biasInit[81]={1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1};
	double Pekf9x9Init[81]={1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1};
	double xhat9x9_biasInit[9]={0,0,0,0,0,0,0,0,0};
	double xhat9x9Init[9]={0,0,0,0,0,0,0,0,-par_g};
	double uControlInit[4]={.1,.1,.1,.1};
	
	double Rekf9x9_bias[36]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double Rekf9x9[9]={0,0,0,0,0,0,0,0,0};
	double ymeas9x9_bias[6]; // vector for 9x9 EKF attitude - measurement: angles and gyro
	double ymeas9x9[3]; // vector for 9x9 EKF position - measurement: position

	double ekf09x9_bias[6]={0,0,0,0,0,0}, ekfCal9x9_bias[6*CALIBRATION];
	double ekf09x9[3]={0,0,0}, ekfCal9x9[3*CALIBRATION];
	
	double tuningEkfBuffer9x9_bias[9]={ekf_Q_7,ekf_Q_8,ekf_Q_9,ekf_Q_10,ekf_Q_11,ekf_Q_12,ekf_Q_16,ekf_Q_17,ekf_Q_18}; //{phi, theta, psi, omega_x, omega_y, omega_z,bias_taux, bias_tauy,bias_tauz}
	double tuningEkfBuffer9x9[9]={ekf_Q_1,ekf_Q_2,ekf_Q_3,ekf_Q_4,ekf_Q_5,ekf_Q_6,ekf_Q_13,ekf_Q_14,ekf_Q_15}; //{x, y, z, xdot, ydot, zdot, distx, disty, distz}
	
	
	
	
	// Random variables
	double L_temp;
	double a=0.01;
	int counterCalEuler=0;
	double q_comp[4], q_init[4]; 
	int k=0;
	double euler_comp[3];
	double euler_mean[3];
	int eulerCalFlag=0;
	float beta_keyboard;
	int isnan_flag=0, outofbounds_flag=0;

	// Keyboard control variables
	int timerPrint=0, ekfPrint=0, ekfReset=0, ekfPrint6States=0, sensorCalibrationRestart=0;
	int outlierFlag, outlierFlagPercentage, outlierFlagMem[1000];
	
	/// Setup timer variables for real time
	struct timespec t,t_start,t_stop; // ,t_start_buffer,t_stop_buffer

	/// Average sampling
	int tsAverageCounter=0, tsAverageReadyEKF=0; // tsAverageReadyEKF is used for to give orientation filter som time to converge before calibration of EKF starts collecting data
	double tsAverageAccum=0;
	double tsAverage=tsSensorsFusion;

	/// Lock memory
	if(mlockall(MCL_CURRENT|MCL_FUTURE) == -1){
		perror("mlockall failed in threadSensorFusion");
	}
	
	/// Start after 1 second
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec++;

	printf("Enabling sensors...\n");
	while(1){
		// Try to enable acc, gyr, mag  and bmp sensors
		pthread_mutex_lock(&mutexI2CBusy);
			enableMPU9250Flag=enableMPU9250();
			enableAK8963Flag=enableAK8963();
		pthread_mutex_unlock(&mutexI2CBusy);
		
		// Check that I2C sensors have been enabled
		if(enableMPU9250Flag==-1){
			//printf("MPU9250 failed to be enabled\n");
		}
		else if(enableAK8963Flag==-1){
			//printf("AK8963 failed to be enabled\n");
		}
		else{
			// Loop for ever
			while(1){
				/// Wait until next shot
				clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL); // sleep for necessary time to reach desired sampling time
				
				// Read raw sensor data from I2C bus to local variable
				pthread_mutex_lock(&mutexI2CBusy);	
					readAllSensorData(accRaw, gyrRaw, magRaw, &tempRaw);	
				pthread_mutex_unlock(&mutexI2CBusy);
				
				// Read raw position data from global to local variable
				pthread_mutex_lock(&mutexPositionSensorData);	
					memcpy(posRaw, sensorRawDataPosition, sizeof(sensorRawDataPosition));		
				pthread_mutex_unlock(&mutexPositionSensorData);
				
				// Read latest control signal from globale variable to local variable
				pthread_mutex_lock(&mutexControlData);	
					memcpy(uControl, controlData, sizeof(controlData));		
				pthread_mutex_unlock(&mutexControlData);
				
				// Get keyboard input data
				pthread_mutex_lock(&mutexKeyboardData);
					timerPrint=(int)keyboardData[5];
					ekfPrint=(int)keyboardData[6];
					ekfReset=(int)keyboardData[7];
					ekfPrint6States=(int)keyboardData[8];
					sensorCalibrationRestart=(int)keyboardData[9];
					a=keyboardData[11];
					beta_keyboard=keyboardData[12];
					memcpy(tuningEkfBuffer9x9_bias, tuningEkfData+6, sizeof(tuningEkfData)*6/18); // ekf states 7-12
					memcpy(tuningEkfBuffer9x9_bias+6, tuningEkfData+15, sizeof(tuningEkfData)*3/18); // ekf states 16-18
					memcpy(tuningEkfBuffer9x9, tuningEkfData, sizeof(tuningEkfData)*6/18); // ekf states 1-6
					memcpy(tuningEkfBuffer9x9+6, tuningEkfData+12, sizeof(tuningEkfData)*3/18); // ekf states 13-15
				pthread_mutex_unlock(&mutexKeyboardData);
				
				// Convert sensor data to correct (filter) units:
				// Acc: g -> m/s² Factor: 9.81
				// Gyr: degrees/s -> radians/s Factor: (PI/180)
				// Mag: milli gauss -> micro tesla Factor: 10^-1
				//accRaw[0]*=9.81;
				//accRaw[1]*=9.81;
				//accRaw[2]*=9.81;
				gyrRaw[0]*=(PI/180);
				gyrRaw[1]*=(PI/180);
				gyrRaw[2]*=(PI/180);
				//magRaw[0]/=10;
				//magRaw[1]/=10;
				//magRaw[2]/=10;
				//magRaw[0]*=1000;
				//magRaw[1]*=1000;
				//magRaw[2]*=1000;
				
				
				// Rotate magnetometer data such that the sensor coordinate frames match.
				// Rz(90)=[-1,1,1], magX*(-1), magZ*(-1)
				// Note: For more info check the MPU9250 Product Specification (Chapter 9)
				magRawRot[0]=magRaw[1];
				magRawRot[1]=magRaw[0];
				magRawRot[2]=magRaw[2];
				
				//magRawRot[0]=-magRaw[1]*(-1);
				//magRawRot[1]=magRaw[0];
				//magRawRot[2]=magRaw[2]*(-1);
				
				/// Time it and print true sampling rate
				clock_gettime(CLOCK_MONOTONIC, &t_stop); /// stop elapsed time clock
				tsTrue=(t_stop.tv_sec - t_start.tv_sec) + (t_stop.tv_nsec - t_start.tv_nsec) / NSEC_PER_SEC;
				sampleFreq=(float) 1.0/tsTrue; /// set Madgwick filter sampling frequency equal to true sampling time
				//printf("SampleFre: %f\n", sampleFreq);
				clock_gettime(CLOCK_MONOTONIC ,&t_start); /// start elapsed time clock
								
				/// Get average sampling time
				if(tsAverageCounter<50){
					tsAverageAccum+=tsTrue;
					tsAverageCounter++;
				}
				else{
					//printf("tsAverageAccum: %lf\n", tsAverageAccum);
					tsAverageAccum/=50;
					tsAverage=tsAverageAccum;
					if(timerPrint){
						printf("EKF: tsAverage %lf tsTrue %lf\n", tsAverage, tsTrue);
					}
					
					/// Activate EKF calibration after tsAverage has been within limit for a number of times. (allows orientation filter to converge)
					if(tsAverage>(tsSensorsFusion/NSEC_PER_SEC)*0.9 && tsAverage<(tsSensorsFusion/NSEC_PER_SEC)*1.1 && tsAverageReadyEKF<2){
						tsAverageReadyEKF++;
						printf("tsAverage within limit for EKF to start %i times\n",tsAverageReadyEKF);
					}
				
					//printmat(Patt,4,4);
					tsAverageCounter=0;
					tsAverageAccum=0;
				}
				
				//tsAverageReadyEKF=2;
				
				// Set gain of orientation estimation Madgwick beta after initialization
				if(tsAverageReadyEKF==2){
					if(k==500){
						beta=0.0433;
						//eulerCalFlag=1;
					}
					else{
						printf("SampleFre: %f Sample: %i\n", sampleFreq, k++);
					}
				
				//sensorDataBuffer[6]=magRawRot[0];
				//sensorDataBuffer[7]=magRawRot[1];
				//sensorDataBuffer[8]=magRawRot[2];
					
				if(eulerCalFlag==1){
					beta=beta_keyboard;
				}
					
				// Magnetometer outlier detection
				outlierFlag=0;
				normMag=sqrt(pow(magRawRot[0],2) + pow(magRawRot[1],2) + pow(magRawRot[2],2));
				L_temp=(1-a)*L+a*normMag; // recursive magnetometer compensator
				L=L_temp;
				if ((normMag > L*1 || normMag < L*0.9) && eulerCalFlag==1){
					magRawRot[0]=0.0f;
					magRawRot[1]=0.0f;
					magRawRot[2]=0.0f;
					outlierFlag=1;
					beta*=0.8;
					//printf("Mag outlier\n");
				}
				
				// outlier percentage
				outlierFlagPercentage = 0;
				for (int i=1; i<1000; i++) {
					outlierFlagMem[i-1] = outlierFlagMem[i];
					outlierFlagPercentage += outlierFlagMem[i-1];
				}
				outlierFlagMem[999] = outlierFlag;
				outlierFlagPercentage += outlierFlagMem[999];
								
				// Orientation estimation with Madgwick filter
				MadgwickAHRSupdate((float)gyrRaw[0], (float)gyrRaw[1], (float)gyrRaw[2], (float)accRaw[0], (float)accRaw[1], (float)accRaw[2], (float)magRawRot[0], (float)magRawRot[1], (float)magRawRot[2]);
				
				// Copy out the returned quaternions from the filter
				q_comp[0]=q0;
				q_comp[1]=-q1;
				q_comp[2]=-q2;
				q_comp[3]=-q3;
				
				// Quaternions to eulers (rad)
				q2euler(euler,q_comp);
			
				 //Allignment compensation for initial point of orientation angles
				if(k==500){
					if(counterCalEuler<500){
						// Mean (bias) accelerometer, gyroscope and magnetometer
						euler_mean[0]+=euler[0];
						euler_mean[1]+=euler[1];
						euler_mean[2]+=euler[2];
						counterCalEuler++;
						printf("euler_sum: %1.4f %1.4f %1.4f counter: %i\n", euler_mean[0], euler_mean[1], euler_mean[2], counterCalEuler);
					}
					else if(counterCalEuler==500){
						euler_mean[0]/=500.0f;
						euler_mean[1]/=500.0f;
						euler_mean[2]/=500.0f;
						counterCalEuler++;
						eulerCalFlag=1;
						printf("q_mean: %1.4f %1.4f %1.4f counter: %i\n", euler_mean[0], euler_mean[1], euler_mean[2], counterCalEuler);
					}
					else{
						euler_comp[0]=euler[0]-euler_mean[0];
						euler_comp[1]=euler[1]-euler_mean[1];
						euler_comp[2]=euler[2]-euler_mean[2];
					}
				}
				
				}
				else{
					printf("SampleFre: %f\n", sampleFreq);
				}
								
				// Move over data to communication.c via pipe
				sensorDataBuffer[0]=gyrRaw[0];
				sensorDataBuffer[1]=gyrRaw[1];
				sensorDataBuffer[2]=gyrRaw[2];
				sensorDataBuffer[3]=accRaw[0];
				sensorDataBuffer[4]=accRaw[1];
				sensorDataBuffer[5]=accRaw[2];
				sensorDataBuffer[6]=magRawRot[0];
				sensorDataBuffer[7]=magRawRot[1];
				sensorDataBuffer[8]=magRawRot[2];
				sensorDataBuffer[9]=euler_comp[0];
				sensorDataBuffer[10]=euler_comp[1];
				sensorDataBuffer[11]=euler_comp[2];
				sensorDataBuffer[12]=(double)q_comp[0];
				sensorDataBuffer[13]=(double)q_comp[1];
				sensorDataBuffer[14]=(double)q_comp[2];
				sensorDataBuffer[15]=(double)q_comp[3];
				sensorDataBuffer[16]=posRaw[0];
				sensorDataBuffer[17]=posRaw[1];
				sensorDataBuffer[18]=posRaw[2];
				
				// Write to Communication process
				if (write(ptrPipe2->parent[1], sensorDataBuffer, sizeof(sensorDataBuffer)) != sizeof(sensorDataBuffer)) printf("pipe write error in Sensor ot Communicaiont\n");
				//else printf("Sensor ID: %d, Sent: %f to Communication\n", (int)getpid(), sensorDataBuffer[0]);
						
				beaconConnected=1;
						
				if(beaconConnected==1 && tsAverageReadyEKF==2 && eulerCalFlag==1){
					// Check if raw position data is new or old
					if(posRaw[0]==posRawPrev[0] && posRaw[1]==posRawPrev[1] && posRaw[2]==posRawPrev[2]){
						posRawOldFlag=1;
					}
					else{
						posRawOldFlag=0;
						memcpy(posRawPrev, posRaw, sizeof(posRaw));
					}
					
					// Move data from (euler and posRaw) array to ymeas
					//ymeas[0]=posRaw[0];
					//ymeas[1]=posRaw[1];
					//ymeas[2]=posRaw[2];
					ymeas9x9[0]=0; // position x
					ymeas9x9[1]=0; // position y
					ymeas9x9[2]=0; // position z
					ymeas9x9_bias[0]=euler_comp[2]; // phi (x-axis)
					ymeas9x9_bias[1]=euler_comp[1]; // theta (y-axis)
					ymeas9x9_bias[2]=euler_comp[0]; // psi (z-axis)
					ymeas9x9_bias[3]=gyrRaw[0]; // gyro x
					ymeas9x9_bias[4]=gyrRaw[1]; // gyro y
					ymeas9x9_bias[5]=gyrRaw[2]; // gyro z
					
					// Flip direction of rotation and gyro around y-axis and x-axis to match model
					ymeas9x9_bias[0]*=-1; // flip theta (x-axis)						
					ymeas9x9_bias[1]*=-1; // flip theta (y-axis)	
					ymeas9x9_bias[3]*=-1; // flip gyro (x-axis)						
					//ymeas9x9_bias[4]*=-1; // flip gyro (y-axis)
					ymeas9x9_bias[5]*=-1; // flip gyro (z-axis)

					// Calibration routine for EKF
					if (calibrationCounterEKF==0){
						printf("EKF Calibration started\n");
						ekfCalibration9x9_bias(Rekf9x9_bias, ekf09x9_bias, ekfCal9x9_bias, ymeas9x9_bias, calibrationCounterEKF);
						ekfCalibration9x9(Rekf9x9, ekf09x9, ekfCal9x9, ymeas9x9, calibrationCounterEKF);
						//printf("calibrationCounterEKF\n: %i", calibrationCounterEKF);
						calibrationCounterEKF++;
					}
					else if(calibrationCounterEKF<CALIBRATION){
						ekfCalibration9x9_bias(Rekf9x9_bias, ekf09x9_bias, ekfCal9x9_bias, ymeas9x9_bias, calibrationCounterEKF);
						ekfCalibration9x9(Rekf9x9, ekf09x9, ekfCal9x9, ymeas9x9, calibrationCounterEKF);
						//printf("calibrationCounterEKF\n: %i", calibrationCounterEKF);
						calibrationCounterEKF++;
						
					}
					else if(calibrationCounterEKF==CALIBRATION){
						ekfCalibration9x9_bias(Rekf9x9_bias, ekf09x9_bias, ekfCal9x9_bias, ymeas9x9_bias, calibrationCounterEKF);
						ekfCalibration9x9(Rekf9x9, ekf09x9, ekfCal9x9, ymeas9x9, calibrationCounterEKF);
							
						// Save calibration in 'settings.txt' if it does not exist
						//saveSettings(Rekf,"Rekf",sizeof(Rekf)/sizeof(double), &fpWrite);
						//saveSettings(ekf0,"ekf0",sizeof(ekf0)/sizeof(double), &fpWrite);
						saveSettings(Rekf9x9_bias,"Rekf9x9_bias",sizeof(Rekf9x9_bias)/sizeof(double));
						saveSettings(Rekf9x9,"Rekf9x9",sizeof(Rekf9x9)/sizeof(double));
						saveSettings(ekf09x9_bias,"ekf09x9_bias",sizeof(ekf09x9_bias)/sizeof(double));
						saveSettings(ekf09x9,"ekf09x9",sizeof(ekf09x9)/sizeof(double));
							
						//printf("calibrationCounterEKF: %i\n", calibrationCounterEKF);
						printf("EKF Calibrations finish\n");
						calibrationCounterEKF++;
						
						// Initialize EKF with current available measurement
						printf("Initialize EKF xhat with current measurments for position and orientation");
						xhat9x9_bias[0]=ymeas9x9_bias[0];
						xhat9x9_bias[1]=ymeas9x9_bias[1];
						xhat9x9_bias[2]=ymeas9x9_bias[2];
						xhat9x9_bias[3]=ymeas9x9_bias[3];
						xhat9x9_bias[4]=ymeas9x9_bias[4];
						xhat9x9_bias[5]=ymeas9x9_bias[5];
						
						xhat9x9[0]=ymeas9x9[0];
						xhat9x9[1]=ymeas9x9[1];
						xhat9x9[2]=ymeas9x9[2];
						
						q_init[0]=q0;
						q_init[1]=q1;
						q_init[2]=q2;
						q_init[3]=q3;
					}
					// State Estimator
					else{
						// Run EKF as long as ekfReset keyboard input is false
						if(!ekfReset){
							// Run Extended Kalman Filter (state estimator) using position and orientation data
							//EKF_no_inertia(Pekf,xhat,uControl,ymeas,Qekf,Rekf,tsAverage,posRawOldFlag);
							EKF_9x9_bias(Pekf9x9_bias,xhat9x9_bias,uControl,ymeas9x9_bias,tuningEkfBuffer9x9_bias,Rekf9x9_bias,tsTrue);
							EKF_9x9(Pekf9x9,xhat9x9,uControl,ymeas9x9,tuningEkfBuffer9x9,Rekf9x9,tsTrue,posRawOldFlag,xhat9x9_bias);
								
							stateDataBuffer[15]=1; // ready flag for MPC to start using the initial conditions given by EKF.
						}
						// Reset EKF with initial Phat, xhat and uControl as long as ekfReset keyboard input is true
						else{
							memcpy(Pekf9x9_bias, Pekf9x9_biasInit, sizeof(Pekf9x9_biasInit));	
							memcpy(Pekf9x9, Pekf9x9Init, sizeof(Pekf9x9Init));	
							memcpy(xhat9x9_bias, xhat9x9_biasInit, sizeof(xhat9x9_biasInit));
							memcpy(xhat9x9, xhat9x9Init, sizeof(xhat9x9Init));
							memcpy(uControl, uControlInit, sizeof(uControlInit));
							
							q0=q_init[0];
							q1=q_init[1];
							q2=q_init[2];
							q3=q_init[3];
							
							stateDataBuffer[15]=0; // set ready flag for MPC false during reset
							isnan_flag=0;
							outofbounds_flag=0;
						}
						
						// Override disturbance estimation in x and y direction
						xhat9x9[6]=0;
						xhat9x9[7]=0;
						
						// Check for EKF9x9_bias failure (isnan)
						for (int j=0;j<9;j++){
							if (isnan(xhat9x9_bias[j])!=0){
								isnan_flag=1;
								break;
							}						
						}
						
						// Check for EKF9x9 failure (isnan)
						for (int j=0;j<9;j++){
							if (isnan(xhat9x9[j])!=0){
								isnan_flag=1;
								break;
							}						
						}
						
						// Check for EKF9x9_bias failure (out of bounds)
						for (int j=0;j<9;j++){
							if (xhat9x9_bias[j]>1e6){
								outofbounds_flag=1;
								break;
							}						
						}
						//// Check for EKF9x9_bias failure (out of bounds)
						//if (xhat9x9_bias[6]>=.09 || xhat9x9_bias[7]>=.09 || xhat9x9_bias[8]>=.09){
							//outofbounds_flag=1;
							//break;
						//}						
						
						// Check for EKF9x9 failure (out of bounds)
						for (int j=0;j<9;j++){
							if (xhat9x9[j]>1e6){
								outofbounds_flag=1;
								break;
							}						
						}
						
						if(isnan_flag){
							stateDataBuffer[15]=0;
							printf("EKF xhat=nan\n");
						}
						else if(outofbounds_flag){
							stateDataBuffer[15]=0;
							printf("EKF xhat=out of bounds\n");
						}
						
						// Move over data to controller.c via pipe
						stateDataBuffer[0]=xhat9x9[0]; // position x
						stateDataBuffer[1]=xhat9x9[1]; // position y
						stateDataBuffer[2]=xhat9x9[2]; // position z
						stateDataBuffer[3]=xhat9x9[3]; // velocity x
						stateDataBuffer[4]=xhat9x9[4]; // velocity y
						stateDataBuffer[5]=xhat9x9[5]; // velocity z
						stateDataBuffer[6]=xhat9x9_bias[0]; // phi (x-axis)
						stateDataBuffer[7]=xhat9x9_bias[1]; // theta (y-axis)
						stateDataBuffer[8]=xhat9x9_bias[2]; // psi (z-axis)
						stateDataBuffer[9]=xhat9x9_bias[3]; // omega x
						stateDataBuffer[10]=xhat9x9_bias[4]; // omega y
						stateDataBuffer[11]=xhat9x9_bias[5]; // omega z
						stateDataBuffer[12]=xhat9x9[6]; // disturbance x
						stateDataBuffer[13]=xhat9x9[7]; // disturbance y
						stateDataBuffer[14]=xhat9x9[8]; // disturbance z
						stateDataBuffer[16]=xhat9x9_bias[6]; // bias taux
						stateDataBuffer[17]=xhat9x9_bias[7]; // bias tauy
						stateDataBuffer[18]=xhat9x9_bias[8]; // bias tauz
						//stateDataBuffer[15]=1; // ready flag for MPC to start using the initial conditions given by EKF.

						if(ekfPrint){
							printf("xhat: % 1.4f % 1.4f % 1.4f % 1.4f % 1.4f % 1.4f % 2.4f % 2.4f % 2.4f % 2.4f % 2.4f % 2.4f % 1.4f % 1.4f % 1.4f % 1.4f % 1.4f % 1.4f\n",xhat9x9[0],xhat9x9[1],xhat9x9[2],xhat9x9[3],xhat9x9[4],xhat9x9[5],xhat9x9_bias[0]*(180/PI),xhat9x9_bias[1]*(180/PI),xhat9x9_bias[2]*(180/PI),xhat9x9_bias[3],xhat9x9_bias[4],xhat9x9_bias[5],xhat9x9[6],xhat9x9[7],xhat9x9[8],xhat9x9_bias[6], xhat9x9_bias[7], xhat9x9_bias[8]);
						}
						
						if(ekfPrint6States){
							printf("xhat: % 1.4f % 1.4f % 1.4f % 2.4f % 2.4f % 2.4f (euler_meas) % 2.4f % 2.4f % 2.4f (gyr_meas) % 2.4f % 2.4f % 2.4f (outlier) %i %i (freq) %3.5f u: %3.4f %3.4f %3.4f %3.4f\n",xhat9x9[0],xhat9x9[1],xhat9x9[2],xhat9x9_bias[0]*(180/PI),xhat9x9_bias[1]*(180/PI),xhat9x9_bias[2]*(180/PI), ymeas9x9_bias[0]*(180/PI),ymeas9x9_bias[1]*(180/PI),ymeas9x9_bias[2]*(180/PI), gyrRaw[0], gyrRaw[1], gyrRaw[2], outlierFlag, outlierFlagPercentage, sampleFreq, uControl[0], uControl[1], uControl[2], uControl[3]);
						}
	
						// Write to Controller process
						if (write(ptrPipe1->child[1], stateDataBuffer, sizeof(stateDataBuffer)) != sizeof(stateDataBuffer)) printf("pipe write error in Sensor to Controller\n");
						//else printf("Sensor ID: %d, Sent: %f to Controller\n", (int)getpid(), sensorDataBuffer[0]);
						
						// Restart sensor fusion and EKF calibration
						if(sensorCalibrationRestart){
							// calibrationCounter=0; // forces sensor fusion to restart calibration
							calibrationCounterEKF=0; // forces ekf to restart calibration
						}
						
						//// Save buffered data to file
						//clock_gettime(CLOCK_MONOTONIC ,&t_start_buffer); /// start elapsed time clock for buffering procedure
						if(buffer_counter==BUFFER){ // if buffer is full, save to file
							saveData(buffer_u1,"u1",sizeof(buffer_u1)/sizeof(double));
							saveData(buffer_u2,"u2",sizeof(buffer_u2)/sizeof(double));
							saveData(buffer_u3,"u3",sizeof(buffer_u3)/sizeof(double));
							saveData(buffer_u4,"u4",sizeof(buffer_u4)/sizeof(double));
							saveData(buffer_omega_x,"omega_x",sizeof(buffer_omega_x)/sizeof(double));
							saveData(buffer_omega_y,"omega_y",sizeof(buffer_omega_y)/sizeof(double));
							saveData(buffer_omega_z,"omega_z",sizeof(buffer_omega_z)/sizeof(double));
							saveData(buffer_angle_x,"angle_x",sizeof(buffer_angle_x)/sizeof(double));
							saveData(buffer_angle_y,"angle_y",sizeof(buffer_angle_y)/sizeof(double));
							saveData(buffer_angle_z,"angle_z",sizeof(buffer_angle_z)/sizeof(double));
							buffer_counter=0;
						}
						else{ // else keep saving data to buffer
							buffer_u1[buffer_counter]=uControl[0];
							buffer_u2[buffer_counter]=uControl[1];
							buffer_u3[buffer_counter]=uControl[2];
							buffer_u4[buffer_counter]=uControl[3];
							buffer_omega_x[buffer_counter]=xhat9x9_bias[3];
							buffer_omega_y[buffer_counter]=xhat9x9_bias[4];
							buffer_omega_z[buffer_counter]=xhat9x9_bias[5];
							buffer_angle_x[buffer_counter]=xhat9x9_bias[0];
							buffer_angle_y[buffer_counter]=xhat9x9_bias[1];
							buffer_angle_z[buffer_counter]=xhat9x9_bias[2];
							buffer_counter++;
						}
						
						///// Time it and print true sampling rate
						//clock_gettime(CLOCK_MONOTONIC, &t_stop_buffer); /// stop elapsed time clock
						//ts_save_buffer=(t_stop_buffer.tv_sec - t_start_buffer.tv_sec) + (t_stop_buffer.tv_nsec - t_start_buffer.tv_nsec) / NSEC_PER_SEC;
						//printf("Save buffer time: %f, tsTrue: %f, buffer_counter: %i\n", ts_save_buffer, tsTrue, buffer_counter);	
					}
				}
							
				/// Calculate next shot
				t.tv_nsec += (int)tsSensorsFusion;
				while (t.tv_nsec >= NSEC_PER_SEC) {
					t.tv_nsec -= NSEC_PER_SEC;
					t.tv_sec++;
				}
			}
		}
		t.tv_sec+=2; // I2C sensors not enabled, sleep for 2 and retry
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);

	}
	return NULL;
}


// Thread - PWM Control
static void *threadPWMControl(void *arg){
	// Get pipe and define local variables
	structPipe *ptrPipe = arg;
	double pwmValueBuffer[4], tsTrue;

	// Initialize I2C connection to the PWM board and define PWM frequency
	pthread_mutex_lock(&mutexI2CBusy);
		int fdPWM=wiringPiI2CSetup(PWM_ADDRESS);
	pthread_mutex_unlock(&mutexI2CBusy);

	if(fdPWM==-1){
	 printf("Error setup the I2C PWM connection\n");
	}
	
	/// Setup timer variables for real time performance check
	struct timespec t_start,t_stop;
	
	/// Average sampling
	int tsAverageCounter=0;
	double tsAverageAccum=0;
	double tsAverage=tsController;
	int timerPrint=0;
	int killPWM=0; // switch [0=STOP, 1=FLY]
	
	/// Lock memory
	if(mlockall(MCL_CURRENT) == -1){
		perror("mlockall failed in threadPWMControl");
	}
	
	else{

		// Initialize PWM board
		pthread_mutex_lock(&mutexI2CBusy);
			enablePWM(fdPWM,500);
		pthread_mutex_unlock(&mutexI2CBusy);
		printf("PWM initialization complete\n");
		
		// Run forever and set PWM when controller computes new values
		while(1){
			/// Time it
			clock_gettime(CLOCK_MONOTONIC ,&t_start); // start elapsed time clock

			// Get keyboard input data
			pthread_mutex_lock(&mutexKeyboardData);
				timerPrint=(int)keyboardData[5];
				killPWM=(int)keyboardData[3];
			pthread_mutex_unlock(&mutexKeyboardData);
			
			// Read data from controller process
			if(read(ptrPipe->parent[0], pwmValueBuffer, sizeof(pwmValueBuffer)) == -1) printf("read error in sensor from controller\n");
			//printf("Data received: %f\n", pwmValueBuffer[0]);
			
			// killPWM is linked to keyboard start flying switch. Forces PWM to zero if stop signal is given
			if(!killPWM){
				pwmValueBuffer[0]=0;
				pwmValueBuffer[1]=0;
				pwmValueBuffer[2]=0;
				pwmValueBuffer[3]=0;
			}
			
			// Saturation pwm 0-100%
			for(int i=0;i<4;i++){
				if(pwmValueBuffer[i]>100){
					pwmValueBuffer[i]=100;
				}
				else if(pwmValueBuffer[i]<0){
					pwmValueBuffer[i]=0;
				}
			}
			
			// Copy control signal over to global memory for EKF to use during next state estimation
			pthread_mutex_lock(&mutexControlData);	
				memcpy(controlData, pwmValueBuffer, sizeof(pwmValueBuffer));		
			pthread_mutex_unlock(&mutexControlData);
			
			//// Adjust PWM for 
			//pwmValueBuffer[0]*=0.7;
			//pwmValueBuffer[1]*=0.7;
			//pwmValueBuffer[2]*=0.7;
			//pwmValueBuffer[3]*=0.7;
			
			// Set PWM
			pthread_mutex_lock(&mutexI2CBusy);
				setPWM(fdPWM, pwmValueBuffer);
			pthread_mutex_unlock(&mutexI2CBusy);
			
			/// Print true sampling rate
			clock_gettime(CLOCK_MONOTONIC, &t_stop);
			tsTrue=(t_stop.tv_sec - t_start.tv_sec) + (t_stop.tv_nsec - t_start.tv_nsec) / NSEC_PER_SEC;
			//printf("Sampling time [s] PWM received: %lf\n",tsTrue);
			
			/// Get average sampling time
			if(tsAverageCounter<50){
				tsAverageAccum+=tsTrue;
				tsAverageCounter++;
			}
			else{
				tsAverageAccum/=50;
				tsAverage=tsAverageAccum;
				if(timerPrint){
					printf("PWM: tsAverage %lf tsTrue %lf\n", tsAverage, tsTrue);
					printf("PWM received: %3.4f %3.4f %3.4f %3.4f\n", pwmValueBuffer[0], pwmValueBuffer[1], pwmValueBuffer[2], pwmValueBuffer[3]);
				}
				tsAverageCounter=0;
				tsAverageAccum=0;
				
			}
			
		}
	}
	return NULL;
}


/******************************************************************/
/****************************FUNCTIONS*****************************/
/******************************************************************/

// Accelerometer part: mu_g
void accelerometerUpdate(double *q, double *P, double *yacc, double *g0, double *Ra){
	// local variables
	int ione=1, n=3, k=3, m=3;
	double fone=1, fzero=0, yka[3], yka2[9], Q[9], h1[9], h2[9], h3[9], h4[9], hd[12], Sacc[9], P_temp[16], S_temp[12], K_temp[12], Sacc_inv[9], yacc_diff[3], state_temp[4];
	double fka[3]={0,0,0}, K[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
	// check if acc is valid (isnan and all!=0)
	// outlier detection
	if (sqrt(pow(yacc[0],2) + pow(yacc[1],2) + pow(yacc[2],2)) > 1.1* sqrtf(pow(g0[0],2) + pow(g0[1],2) + pow(g0[2],2))){
		// dont use measurement
		//printf("Accelerometer Outlier\n");
	}
	else{
		// continue measurement
		// mu_g
		// yka=Qq(x)'*yka2=Qq(x)'*(g0+fka); accelerometer and quaternion model relation
		Qq(Q, q);
		yka2[0]=g0[0]+fka[0];
		yka2[1]=g0[1]+fka[1];
		yka2[2]=g0[2]+fka[2];

		F77_CALL(dgemv)("t",&m,&n,&fone,Q,&m,yka2,&ione,&fzero,yka,&ione);
		
		// [h1 h2 h3 h4]=dQqdq(x); jacobian
		// hd=[h1'*g0 h2'*g0 h3'*g0 h4'*g0];
		dQqdq(h1, h2, h3, h4, hd, q, g0);	
		
		// S=hd*P*hd'+Ra; innovation covariance
		n=4; k=4; m=3;
		F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,hd,&m,P,&k,&fzero,S_temp,&m);
		n=3; k=4; m=3;
		F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,S_temp,&m,hd,&n,&fzero,Sacc,&m);
		Sacc[0]+=Ra[0];
		Sacc[1]+=Ra[1];
		Sacc[2]+=Ra[2];
		Sacc[3]+=Ra[3];
		Sacc[4]+=Ra[4];
		Sacc[5]+=Ra[5];
		Sacc[6]+=Ra[6];
		Sacc[7]+=Ra[7];
		Sacc[8]+=Ra[8];
		
		// K=P*hd'/S; kalman gain
		n=3; k=4; m=4;
		F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,P,&m,hd,&n,&fzero,K_temp,&m);
		mInverse(Sacc, Sacc_inv);
		n=3; k=3; m=4;
		F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K_temp,&m,Sacc_inv,&k,&fzero,K,&m);
				
		// x=x+K*yacc_diff=x+K*(yacc-yka); state update
		yacc_diff[0]=yacc[0]-yka[0];
		yacc_diff[1]=yacc[1]-yka[1];
		yacc_diff[2]=yacc[2]-yka[2];
		n=3, k=4, m=4;
		F77_CALL(dgemv)("n",&m,&n,&fone,K,&m,yacc_diff,&ione,&fzero,state_temp,&ione);
		q[0]+=state_temp[0];
		q[1]+=state_temp[1];
		q[2]+=state_temp[2];
		q[3]+=state_temp[3];

		// P=P-K*S*K'; covariance update
		n=3; k=3; m=4;
		F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K,&m,Sacc,&k,&fzero,K_temp,&m);
		n=4; k=3; m=4;
		F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,K_temp,&m,K,&n,&fzero,P_temp,&m);
		P[0]-=P_temp[0];
		P[1]-=P_temp[1];
		P[2]-=P_temp[2];
		P[3]-=P_temp[3];
		P[4]-=P_temp[4];
		P[5]-=P_temp[5];
		P[6]-=P_temp[6];
		P[7]-=P_temp[7];
		P[8]-=P_temp[8];
		P[9]-=P_temp[9];
		P[10]-=P_temp[10];
		P[11]-=P_temp[11];
		P[12]-=P_temp[12];
		P[13]-=P_temp[13];
		P[14]-=P_temp[14];
		P[15]-=P_temp[15];
	}
}

// Gyroscope part: tu_qw
void gyroscopeUpdate(double *q, double *P, double *ygyr, double *Rw, double Ts){
	// local variables
	int ione=1, n=4, k=4, m=4;
	double fone=1, fzero=0, Gm[12], Sm[16], F[16], q_temp[4], F_temp[16], P_temp[16], Gm_temp12[12], Gm_temp16[16];
	
	// check if gyr is valid (isnan and all!=0)
	// outlier detection
	if (ygyr[0]==0 && ygyr[1]==0 && ygyr[2]==0){
		// dont use measurement
		//printf("Gyroscope Outlier\n");
	}
	else{
		// continue measurement
		// tu_qw
		// Gm=Sq(x)*0.5*T, ;
		Sq(Gm, q, Ts);
		
		// F=eye(4)+Somega(omega)*0.5*T;
		Somega(Sm,ygyr);
		F[0]=1+Sm[0]*0.5*Ts;
		F[1]=0+Sm[1]*0.5*Ts;
		F[2]=0+Sm[2]*0.5*Ts;
		F[3]=0+Sm[3]*0.5*Ts;
		F[4]=0+Sm[4]*0.5*Ts;
		F[5]=1+Sm[5]*0.5*Ts;
		F[6]=0+Sm[6]*0.5*Ts;
		F[7]=0+Sm[7]*0.5*Ts;
		F[8]=0+Sm[8]*0.5*Ts;
		F[9]=0+Sm[9]*0.5*Ts;
		F[10]=1+Sm[10]*0.5*Ts;
		F[11]=0+Sm[11]*0.5*Ts;
		F[12]=0+Sm[12]*0.5*Ts;
		F[13]=0+Sm[13]*0.5*Ts;
		F[14]=0+Sm[14]*0.5*Ts;
		F[15]=1+Sm[15]*0.5*Ts;
		
		// x=F*x; predicted state estimate
		F77_CALL(dgemv)("n",&m,&n,&fone,F,&m,q,&ione,&fzero,q_temp,&ione);	
		memcpy(q, q_temp, sizeof(q_temp));
		
		// P=F_temp*F'+G_temp*G'=F*P*F'+G*Rw*G'=P_temp+G_temp2; predicted covariance estimate
		n=4; k=4; m=4;
		F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,F,&m,P,&k,&fzero,F_temp,&m);
		F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,F_temp,&m,F,&n,&fzero,P_temp,&m);
		n=3; k=3; m=4;
		F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,Gm,&m,Rw,&k,&fzero,Gm_temp12,&m);
		n=4; k=3; m=4;
		F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Gm_temp12,&m,Gm,&n,&fzero,Gm_temp16,&m);	
		P[0]=P_temp[0]+Gm_temp16[0];
		P[1]=P_temp[1]+Gm_temp16[1];
		P[2]=P_temp[2]+Gm_temp16[2];
		P[3]=P_temp[3]+Gm_temp16[3];
		P[4]=P_temp[4]+Gm_temp16[4];
		P[5]=P_temp[5]+Gm_temp16[5];
		P[6]=P_temp[6]+Gm_temp16[6];
		P[7]=P_temp[7]+Gm_temp16[7];
		P[8]=P_temp[8]+Gm_temp16[8];
		P[9]=P_temp[9]+Gm_temp16[9];
		P[10]=P_temp[10]+Gm_temp16[10];
		P[11]=P_temp[11]+Gm_temp16[11];
		P[12]=P_temp[12]+Gm_temp16[12];
		P[13]=P_temp[13]+Gm_temp16[13];
		P[14]=P_temp[14]+Gm_temp16[14];
		P[15]=P_temp[15]+Gm_temp16[15];
	}
}

// Magnetometer part: mu_m
void magnetometerUpdate(double *q, double *P, double *ymag, double *m0, double *Rm, double L){
	// local variables
	int ione=1, n=3, k=3, m=3;
	double fone=1, fzero=0, ykm[3], ykm2[9], Q[9], h1[9], h2[9], h3[9], h4[9], hd[12], Smag[9], P_temp[16], S_temp[12], K_temp[12], Smag_inv[9], ymag_diff[3], state_temp[4];
	double fkm[3]={0,0,0}, K[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}, a=0.05;
	
	// check if acc is valid (isnan and all!=0)
	// outlier detection
	L=(1-a)*L+a*sqrt(pow(ymag[0],2) + pow(ymag[1],2) + pow(ymag[2],2)); // recursive magnetometer compensator
	if (sqrt(pow(ymag[0],2) + pow(ymag[1],2) + pow(ymag[2],2)) > L){
		// dont use measurement
		//printf("Magnetometer Outlier\n");
	}
	else{
		// continue measurement
		// mu_m
		// ykm=Qq(x)'*ykm2=Qq(x)'*(m0+fkm); magnetometer and quaternion model relation
		Qq(Q, q);
		ykm2[0]=m0[0]+fkm[0];
		ykm2[1]=m0[1]+fkm[1];
		ykm2[2]=m0[2]+fkm[2];
		F77_CALL(dgemv)("t",&m,&n,&fone,Q,&m,ykm2,&ione,&fzero,ykm,&ione);
		
		// [h1 h2 h3 h4]=dQqdq(x); jacobian
		// hd=[h1'*m0 h2'*m0 h3'*m0 h4'*m0];
		dQqdq(h1, h2, h3, h4, hd, q, m0);	
		
		// Smag=hd*P*hd'+Rm; innovation covariance
		n=4; k=4; m=3;
		F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,hd,&m,P,&k,&fzero,S_temp,&m);
		n=3; k=4; m=3;
		F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,S_temp,&m,hd,&n,&fzero,Smag,&m);
		Smag[0]+=Rm[0];
		Smag[1]+=Rm[1];
		Smag[2]+=Rm[2];
		Smag[3]+=Rm[3];
		Smag[4]+=Rm[4];
		Smag[5]+=Rm[5];
		Smag[6]+=Rm[6];
		Smag[7]+=Rm[7];
		Smag[8]+=Rm[8];

		// K=P*hd'/Smag; kalman gain
		n=3; k=4; m=4;
		F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,P,&m,hd,&n,&fzero,K_temp,&m);	
		mInverse(Smag, Smag_inv);
		n=3; k=3; m=4;
		F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K_temp,&m,Smag_inv,&k,&fzero,K,&m);		
				
		// x=x+K*ymag_diff=x+K*(ymag-ykm); state update
		ymag_diff[0]=ymag[0]-ykm[0];
		ymag_diff[1]=ymag[1]-ykm[1];
		ymag_diff[2]=ymag[2]-ykm[2];
		n=3, k=4, m=4;
		F77_CALL(dgemv)("n",&m,&n,&fone,K,&m,ymag_diff,&ione,&fzero,state_temp,&ione);
		q[0]+=state_temp[0];
		q[1]+=state_temp[1];
		q[2]+=state_temp[2];
		q[3]+=state_temp[3];
		
		// P=P-K*S*K'; covariance update
		n=3; k=3; m=4;
		F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K,&m,Smag,&k,&fzero,K_temp,&m);
		n=4; k=3; m=4;
		F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,K_temp,&m,K,&n,&fzero,P_temp,&m);
		P[0]-=P_temp[0];
		P[1]-=P_temp[1];
		P[2]-=P_temp[2];
		P[3]-=P_temp[3];
		P[4]-=P_temp[4];
		P[5]-=P_temp[5];
		P[6]-=P_temp[6];
		P[7]-=P_temp[7];
		P[8]-=P_temp[8];
		P[9]-=P_temp[9];
		P[10]-=P_temp[10];
		P[11]-=P_temp[11];
		P[12]-=P_temp[12];
		P[13]-=P_temp[13];
		P[14]-=P_temp[14];
		P[15]-=P_temp[15];
	}

}

// Sensor calibration
void sensorCalibration(double *Racc, double *Rgyr, double *Rmag, double *acc0, double *gyr0, double *mag0, double *accCal, double *gyrCal, double *magCal, double *yacc, double *ygyr, double *ymag, int counterCal){
	// Calibration routine to get mean, variance and std_deviation
	if(counterCal==CALIBRATION){
		// Mean (bias) accelerometer, gyroscope and magnetometer
		for (int i=0;i<CALIBRATION;i++){
			acc0[0]+=accCal[i*3];
			acc0[1]+=accCal[i*3+1];
			acc0[2]+=accCal[i*3+2];
			gyr0[0]+=gyrCal[i*3];
			gyr0[1]+=gyrCal[i*3+1];
			gyr0[2]+=gyrCal[i*3+2];
			mag0[0]+=magCal[i*3];
			mag0[1]+=magCal[i*3+1];
			mag0[2]+=magCal[i*3+2];
		}
		acc0[0]/=CALIBRATION;
		acc0[1]/=CALIBRATION;
		acc0[2]/=CALIBRATION;
		gyr0[0]/=CALIBRATION;
		gyr0[1]/=CALIBRATION;
		gyr0[2]/=CALIBRATION;
		mag0[0]/=CALIBRATION;
		mag0[1]/=CALIBRATION;
		mag0[2]/=CALIBRATION;
		
		// Sum up for variance calculation
		for (int i=0;i<CALIBRATION;i++){
			Racc[0]+=pow((accCal[i*3] - acc0[0]), 2);
			Racc[4]+=pow((accCal[i*3+1] - acc0[1]), 2);
			Racc[8]+=pow((accCal[i*3+2] - acc0[2]), 2);
			Rgyr[0]+=pow((gyrCal[i*3] - gyr0[0]), 2);
			Rgyr[4]+=pow((gyrCal[i*3+1] - gyr0[1]), 2);
			Rgyr[8]+=pow((gyrCal[i*3+2] - gyr0[2]), 2);
			Rmag[0]+=pow((magCal[i*3] - mag0[0]), 2);
			Rmag[4]+=pow((magCal[i*3+1] - mag0[1]), 2);
			Rmag[8]+=pow((magCal[i*3+2] - mag0[2]), 2);
		}
		// Variance (sigma)
		Racc[0]/=CALIBRATION;
		Racc[4]/=CALIBRATION;
		Racc[8]/=CALIBRATION;
		Rgyr[0]/=CALIBRATION;
		Rgyr[4]/=CALIBRATION;
		Rgyr[8]/=CALIBRATION;
		Rmag[0]/=CALIBRATION;
		Rmag[4]/=CALIBRATION;
		Rmag[8]/=CALIBRATION;
		
		// Print results
		printf("Mean (bias) accelerometer\n");
		printmat(acc0,3,1);
		printf("Mean (bias) gyroscope\n");
		printmat(gyr0,3,1);
		printf("Mean (bias) magnetometer\n");
		printmat(mag0,3,1);
		printf("Covariance matrix (sigma) accelerometer\n");
		printmat(Racc,3,3);
		printf("Covariance (sigma) gyroscope\n");
		printmat(Rgyr,3,3);
		printf("Covariance (sigma) magnetometer\n");
		printmat(Rmag,3,3);
	}
	// Default i save calibrartion data
	else{
		accCal[counterCal*3]=yacc[0];
		accCal[counterCal*3+1]=yacc[1];
		accCal[counterCal*3+2]=yacc[2];
		gyrCal[counterCal*3]=ygyr[0];
		gyrCal[counterCal*3+1]=ygyr[1];
		gyrCal[counterCal*3+2]=ygyr[2];
		magCal[counterCal*3]=ymag[0];
		magCal[counterCal*3+1]=ymag[1];
		magCal[counterCal*3+2]=ymag[2];
	}		
}

// EKF calibration
void ekfCalibration(double *Rekf, double *ekf0, double *ekfCal, double *ymeas, int counterCal){
	// Calibration routine to get mean, variance and std_deviation
	if(counterCal==CALIBRATION){
		// Mean (bias) accelerometer, gyroscope and magnetometer
		for (int i=0;i<CALIBRATION;i++){
			ekf0[0]+=ekfCal[i*6];
			ekf0[1]+=ekfCal[i*6+1];
			ekf0[2]+=ekfCal[i*6+2];
			ekf0[3]+=ekfCal[i*6+3];
			ekf0[4]+=ekfCal[i*6+4];
			ekf0[5]+=ekfCal[i*6+5];
		}
		ekf0[0]/=CALIBRATION;
		ekf0[1]/=CALIBRATION;
		ekf0[2]/=CALIBRATION;
		ekf0[3]/=CALIBRATION;
		ekf0[4]/=CALIBRATION;
		ekf0[5]/=CALIBRATION;
		
		// Sum up for variance calculation
		for (int i=0;i<CALIBRATION;i++){
			Rekf[0]+=pow((ekfCal[i*6] - ekf0[0]), 2);
			Rekf[7]+=pow((ekfCal[i*6+1] - ekf0[1]), 2);
			Rekf[14]+=pow((ekfCal[i*6+2] - ekf0[2]), 2);
			Rekf[21]+=pow((ekfCal[i*6+3] - ekf0[3]), 2);
			Rekf[28]+=pow((ekfCal[i*6+4] - ekf0[4]), 2);
			Rekf[35]+=pow((ekfCal[i*6+5] - ekf0[5]), 2);
		}
		// Variance (sigma)
		Rekf[0]/=CALIBRATION;
		Rekf[7]/=CALIBRATION;
		Rekf[14]/=CALIBRATION;
		Rekf[21]/=CALIBRATION;
		Rekf[28]/=CALIBRATION;
		Rekf[35]/=CALIBRATION;
		
		// Overide calibration when position measurements are gone
		Rekf[0]=1;
		Rekf[7]=1;
		Rekf[14]=1;
	
		// Print results
		printf("Mean (bias) EKF\n");
		printmat(ekf0,6,1);
		printf("Covariance matrix (sigma) EKF\n");
		printmat(Rekf,6,6);
	}
	// Default i save calibrartion data
	else{
		ekfCal[counterCal*6]=ymeas[0];
		ekfCal[counterCal*6+1]=ymeas[1];
		ekfCal[counterCal*6+2]=ymeas[2];
		ekfCal[counterCal*6+3]=ymeas[3];
		ekfCal[counterCal*6+4]=ymeas[4];
		ekfCal[counterCal*6+5]=ymeas[5];
	}		
}

// EKF calibration
void ekfCalibration6x6(double *Rekf, double *ekf0, double *ekfCal, double *ymeas, int counterCal){
	// Calibration routine to get mean, variance and std_deviation
	if(counterCal==CALIBRATION){
		// Mean (bias) accelerometer, gyroscope and magnetometer
		for (int i=0;i<CALIBRATION;i++){
			ekf0[0]+=ekfCal[i*6];
			ekf0[1]+=ekfCal[i*6+1];
			ekf0[2]+=ekfCal[i*6+2];
			ekf0[3]+=ekfCal[i*6+3];
			ekf0[4]+=ekfCal[i*6+4];
			ekf0[5]+=ekfCal[i*6+5];
		}
		ekf0[0]/=CALIBRATION;
		ekf0[1]/=CALIBRATION;
		ekf0[2]/=CALIBRATION;
		ekf0[3]/=CALIBRATION;
		ekf0[4]/=CALIBRATION;
		ekf0[5]/=CALIBRATION;
		
		// Sum up for variance calculation
		for (int i=0;i<CALIBRATION;i++){
			Rekf[0]+=pow((ekfCal[i*6] - ekf0[0]), 2);
			Rekf[7]+=pow((ekfCal[i*6+1] - ekf0[1]), 2);
			Rekf[14]+=pow((ekfCal[i*6+2] - ekf0[2]), 2);
			Rekf[21]+=pow((ekfCal[i*6+3] - ekf0[3]), 2);
			Rekf[28]+=pow((ekfCal[i*6+4] - ekf0[4]), 2);
			Rekf[35]+=pow((ekfCal[i*6+5] - ekf0[5]), 2);
		}
		// Variance (sigma)
		Rekf[0]/=CALIBRATION;
		Rekf[7]/=CALIBRATION;
		Rekf[14]/=CALIBRATION;
		Rekf[21]/=CALIBRATION;
		Rekf[28]/=CALIBRATION;
		Rekf[35]/=CALIBRATION;
	
		// Print results
		printf("Mean (bias) EKF 6x6\n");
		printmat(ekf0,6,1);
		printf("Covariance matrix (sigma) EKF 6x6\n");
		printmat(Rekf,6,6);
	}
	// Default i save calibrartion data
	else{
		ekfCal[counterCal*6]=ymeas[0];
		ekfCal[counterCal*6+1]=ymeas[1];
		ekfCal[counterCal*6+2]=ymeas[2];
		ekfCal[counterCal*6+3]=ymeas[3];
		ekfCal[counterCal*6+4]=ymeas[4];
		ekfCal[counterCal*6+5]=ymeas[5];
	}		
}

// EKF calibration
void ekfCalibration9x9_bias(double *Rekf, double *ekf0, double *ekfCal, double *ymeas, int counterCal){
	// Calibration routine to get mean, variance and std_deviation
	if(counterCal==CALIBRATION){
		// Mean (bias) accelerometer, gyroscope and magnetometer
		for (int i=0;i<CALIBRATION;i++){
			ekf0[0]+=ekfCal[i*6];
			ekf0[1]+=ekfCal[i*6+1];
			ekf0[2]+=ekfCal[i*6+2];
			ekf0[3]+=ekfCal[i*6+3];
			ekf0[4]+=ekfCal[i*6+4];
			ekf0[5]+=ekfCal[i*6+5];
		}
		ekf0[0]/=CALIBRATION;
		ekf0[1]/=CALIBRATION;
		ekf0[2]/=CALIBRATION;
		ekf0[3]/=CALIBRATION;
		ekf0[4]/=CALIBRATION;
		ekf0[5]/=CALIBRATION;
		
		// Sum up for variance calculation
		for (int i=0;i<CALIBRATION;i++){
			Rekf[0]+=pow((ekfCal[i*6] - ekf0[0]), 2);
			Rekf[7]+=pow((ekfCal[i*6+1] - ekf0[1]), 2);
			Rekf[14]+=pow((ekfCal[i*6+2] - ekf0[2]), 2);
			Rekf[21]+=pow((ekfCal[i*6+3] - ekf0[3]), 2);
			Rekf[28]+=pow((ekfCal[i*6+4] - ekf0[4]), 2);
			Rekf[35]+=pow((ekfCal[i*6+5] - ekf0[5]), 2);
		}
		// Variance (sigma)
		Rekf[0]/=CALIBRATION;
		Rekf[7]/=CALIBRATION;
		Rekf[14]/=CALIBRATION;
		Rekf[21]/=CALIBRATION;
		Rekf[28]/=CALIBRATION;
		Rekf[35]/=CALIBRATION;
	
		// Print results
		printf("Mean (bias) EKF 6x6\n");
		printmat(ekf0,6,1);
		printf("Covariance matrix (sigma) EKF 6x6\n");
		printmat(Rekf,6,6);
	}
	// Default i save calibrartion data
	else{
		ekfCal[counterCal*6]=ymeas[0];
		ekfCal[counterCal*6+1]=ymeas[1];
		ekfCal[counterCal*6+2]=ymeas[2];
		ekfCal[counterCal*6+3]=ymeas[3];
		ekfCal[counterCal*6+4]=ymeas[4];
		ekfCal[counterCal*6+5]=ymeas[5];
	}		
}

// EKF calibration
void ekfCalibration9x9(double *Rekf, double *ekf0, double *ekfCal, double *ymeas, int counterCal){
	// Calibration routine to get mean, variance and std_deviation
	if(counterCal==CALIBRATION){
		// Mean (bias) accelerometer, gyroscope and magnetometer
		for (int i=0;i<CALIBRATION;i++){
			ekf0[0]+=ekfCal[i*3];
			ekf0[1]+=ekfCal[i*3+1];
			ekf0[2]+=ekfCal[i*3+2];
		}
		ekf0[0]/=CALIBRATION;
		ekf0[1]/=CALIBRATION;
		ekf0[2]/=CALIBRATION;
		
		// Sum up for variance calculation
		for (int i=0;i<CALIBRATION;i++){
			Rekf[0]+=pow((ekfCal[i*3] - ekf0[0]), 2);
			Rekf[4]+=pow((ekfCal[i*3+1] - ekf0[1]), 2);
			Rekf[8]+=pow((ekfCal[i*3+2] - ekf0[2]), 2);
		}
		// Variance (sigma)
		Rekf[0]/=CALIBRATION;
		Rekf[4]/=CALIBRATION;
		Rekf[8]/=CALIBRATION;
		
		// Overide calibration when position measurements are gone
		Rekf[0]=1;
		Rekf[4]=1;
		Rekf[8]=1;
	
		// Print results
		printf("Mean (bias) EKF 9x9\n");
		printmat(ekf0,3,1);
		printf("Covariance matrix (sigma) EKF 9x9\n");
		printmat(Rekf,3,3);
	}
	// Default i save calibrartion data
	else{
		ekfCal[counterCal*3]=ymeas[0];
		ekfCal[counterCal*3+1]=ymeas[1];
		ekfCal[counterCal*3+2]=ymeas[2];
	}		
}

// S(q) matrix
void Sq(double *Gm, double *q, double T){
	Gm[0]=-q[1]*0.5*T;
	Gm[1]=q[0]*0.5*T;
	Gm[2]=q[3]*0.5*T;
	Gm[3]=-q[2]*0.5*T;
	Gm[4]=-q[2]*0.5*T;
	Gm[5]=-q[3]*0.5*T;
	Gm[6]=q[0]*0.5*T;
	Gm[7]=q[1]*0.5*T;
	Gm[8]=-q[3]*0.5*T;
	Gm[9]=q[2]*0.5*T;
	Gm[10]=-q[1]*0.5*T;
	Gm[11]=q[0]*0.5*T;	
}

// S(omega) matrix
void Somega(double *Sm, double *omega){
	Sm[0]=0;
	Sm[1]=omega[0];
	Sm[2]=omega[1];
	Sm[3]=omega[2];
	Sm[4]=-omega[0];
	Sm[5]=0;
	Sm[6]=-omega[2];
	Sm[7]=omega[1];
	Sm[8]=-omega[1];
	Sm[9]=omega[2];
	Sm[10]=0;
	Sm[11]=-omega[0];
	Sm[12]=-omega[2];
	Sm[13]=-omega[1];
	Sm[14]=omega[0];
	Sm[15]=0;
}

// Quaternions matrix
void Qq(double *Q, double *q){
	// input q0->q3
	//float q0, q1, q2, q3;
	Q[0] = 2*(pow(q[0],2)+pow(q[1],2))-1;
	Q[3] = 2*(q[1]*q[2]-q[0]*q[3]);
	Q[6] = 2*(q[1]*q[3]+q[0]*q[2]);
	Q[1] = 2*(q[1]*q[2]+q[0]*q[3]);
	Q[4] = 2*(pow(q[0],2)+pow(q[2],2))-1;
	Q[7] = 2*(q[2]*q[3]-q[0]*q[1]);
	Q[2] = 2*(q[1]*q[3]-q[0]*q[2]);
	Q[5] = 2*(q[2]*q[3]+q[0]*q[1]);
	Q[8] = 2*(pow(q[0],2)+pow(q[3],2))-1;
}

// Quaternions Jacobian
void dQqdq(double *h1, double *h2, double *h3, double *h4, double *hd, double *q, double *biasvec){
	// Q=dQqdq(q) Jacobian
	// input q0->q3
	// [h1 h2 h3 h4]=dQqdq(x);
	h1[0] = 4*q[0];
	h1[1] = 2*q[3];
	h1[2] = -2*q[2];
	h1[3] = -2*q[3];
	h1[4] = 4*q[0];
	h1[5] = 2*q[1];
	h1[6] = 2*q[2];
	h1[7] = -2*q[1];
	h1[8] = 4*q[0];
	
	//printmat(h1, 3, 3);
	
	h2[0] = 4*q[1];
	h2[1] = 2*q[2];
	h2[2] = 2*q[3];
	h2[3] = 2*q[2];
	h2[4] = 0;
	h2[5] = 2*q[0]; 
	h2[6] = 2*q[3];
	h2[7] = -2*q[0];
	h2[8] = 0;
	
	//printmat(h2, 3, 3);
	
	h3[0] = 0;
	h3[1] = 2*q[1];
	h3[2] = -2*q[0];
	h3[3] = 2*q[1];
	h3[4] = 4*q[2];
	h3[5] = 2*q[3];
	h3[6] = 2*q[0];
	h3[7] = 2*q[3];
	h3[8] = 0;
	
	//printmat(h3, 3, 3);
	
	h4[0] = 0;
	h4[1] = 2*q[0];
	h4[2] = 2*q[1];
	h4[3] = -2*q[0];
	h4[4] = 0;
	h4[5] = 2*q[2];
	h4[6] = 2*q[1];
	h4[7] = 2*q[2];
	h4[8] = 4*q[3];
	
	//printmat(h4, 3, 3);
	
	// hd=[h1'*biasvec h2'*biasvec h3'*biasvec h4'*biasvec];
	double hd_temp[3];
	int n=3, m=3, ione=1;
	double fone=1, fzero=0;
	F77_CALL(dgemv)("t",&m,&n,&fone,h1,&m,biasvec,&ione,&fzero,hd_temp,&ione);
	memcpy(hd, hd_temp, sizeof(hd_temp));
	F77_CALL(dgemv)("t",&m,&n,&fone,h2,&m,biasvec,&ione,&fzero,hd_temp,&ione);
	memcpy(hd+3, hd_temp, sizeof(hd_temp));
	F77_CALL(dgemv)("t",&m,&n,&fone,h3,&m,biasvec,&ione,&fzero,hd_temp,&ione);
	memcpy(hd+6, hd_temp, sizeof(hd_temp));
	F77_CALL(dgemv)("t",&m,&n,&fone,h4,&m,biasvec,&ione,&fzero,hd_temp,&ione);
	memcpy(hd+9, hd_temp, sizeof(hd_temp));
}

// Quaternions normalization
void qNormalize(double *q){
	double qnorm=sqrt(pow(q[0],2)+pow(q[1],2)+pow(q[2],2)+pow(q[3],2));
	q[0]/=qnorm;
	q[1]/=qnorm;
	q[2]/=qnorm;
	q[3]/=qnorm;
	if(q[0]<0){
		q[0]*=-1;
		q[1]*=-1;
		q[2]*=-1;
		q[3]*=-1;
	}
}

// Quaternions to Eulers (avoids gimbal lock)
void q2euler(double *result, double *q){
	// Handle north pole case
	if (q[1]*q[3]+q[0]*q[2] > 0.5){
		result[0]=2*atan2(q[1],q[0]);
		result[2]=0;
	}
	else{
		result[0]=atan2(-2*(q[0]*q[2]-q[0]*q[3]),1-2*(pow(q[2],2)+pow(q[3],2))); //heading
		result[2]=atan2(2*(q[2]*q[3]-q[0]*q[1]),1-2*(pow(q[1],2)+pow(q[2],2))); // bank
	}
	
	// Handle south pole case
	if (q[1]*q[3]+q[0]*q[2] < -0.5){
		result[0]=-2*atan2(q[1],q[0]);
		result[2]=0;
	}
	else{
		result[0]=atan2(-2*(q[0]*q[2]-q[0]*q[3]),1-2*(pow(q[2],2)+pow(q[3],2))); //heading
		result[2]=atan2(2*(q[2]*q[3]-q[0]*q[1]),1-2*(pow(q[1],2)+pow(q[2],2))); // bank
	}
	
	result[1]=asin(2*(q[1]*q[3]+q[0]*q[2])); // attitude
}

// Load settings file
int loadSettings(double *data, char* name, int size){
	// Create file pointer
	FILE *fp;
	
	float value;
	char string[30];
	int finish=0;
	//int lines=0;
	int i;
	
	
	// Open file and prepare for read
	fp=fopen("settings.txt", "r");
	// Check to see that file has opened
	if(fp==NULL){
		printf("File could not be opened for read\n");
	}
	else{
		while((fgets(string,30,fp)) != NULL && !finish){
			// Search for variable
			if(strstr(string,name) != NULL){
				//printf("%s\n",name);
				// Get variable data
				for(i=0;i<size;i++){
					if((fgets(string,30,fp)) != NULL && string[0]!='\n'){
						sscanf(string, "%f\n", &value);
						data[i]=(double)value;
						//printf("%f\n",value);
					}
					else{
						printf("Bad format or missing data on reading settings file\n");
					}
				}
				finish=1;
				printf("%s settings loaded\n",name);
			}
		}
		if(!finish){
			printf("%s not loaded (does not exist in settings.txt)\n",name);
		}
	}
	
	// Close file
	fclose(fp);
	return finish;
}

// Save settings file
//void saveSettings(double *data, char* name, int size, FILE **fp){
void saveSettings(double *data, char* name, int size){
	// Create file pointer
	FILE *fpWrite;
	int i;
	int finish=0;

	// Open file and prepare for write
	fpWrite=fopen("settings.txt", "a"); // "a" means append
	if(fpWrite==NULL){
		printf("File could not be opened for write\n");
	}
	else{
		if(!finish){	// if variable does not exist
			fprintf(fpWrite, "%s\n", name); // append variable name
			for(i=0;i<size;i++){
				fprintf(fpWrite, "%3.18f\n", data[i]); // append content
			}
			fprintf(fpWrite, "\n"); // newline
			printf("%s settings saved\n",name);
		}
		else{	// if variable does exist
			printf("%s not saved (lready exists in settings.txt)\n", name);
		}
	}
	
	// Close file
	fclose(fpWrite);
}

// Save data to file
//void saveData(double *data, char* name, int size, FILE **fp, int action){
void saveData(double *data, char* name, int size){
	// int action: 0=close, 1=open, NULL=nothing
	// Create file pointer
	FILE *fpWrite;
	int i;

	// Open file and prepare for write (append)
		fpWrite=fopen("data.txt", "a"); // "a" means append
	
	// Write to file
	if(fpWrite==NULL){
		printf("File could not be opened for write\n");
	}
	else{
		fprintf(fpWrite, "%s\n", name); // append variable name
		for(i=0;i<size;i++){
			fprintf(fpWrite, "%5.18f\n", data[i]); // append content
		}
		fprintf(fpWrite, "\n"); // newline
		//printf("%s settings saved\n",name);
	}
	
	// Close file
		fclose(fpWrite);
}

// Used to print the bits in a data type
void printBits(size_t const size, void const * const ptr){
    unsigned char *b = (unsigned char*) ptr;
    unsigned char byte;
    int i, j;

    for (i=size-1;i>=0;i--)
    {
        for (j=7;j>=0;j--)
        {
            byte = (b[i] >> j) & 1;
            printf("%u", byte);
        }
    }
    puts("");
}

// State Observer - Extended Kalman Filter (18 states)
void EKF(double *Phat, double *xhat, double *u, double *ymeas, double *Q, double *R, double Ts, int flag){
	// Local variables
	double xhat_pred[18]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double C[108]={1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double eye18[324]={1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
	//double S_inv[36]={0.25,0,0,0,0,0,0,0.25,0,0,0,0,0,0,0.25,0,0,0,0,0,0,0.25,0,0,0,0,0,0,0.25,0,0,0,0,0,0,0.25};
	double S_inv[36];
	double A[324], S[36], C_temp[108], Jfx_temp[324], Phat_pred[324], K_temp[108], K[108], V[6], xhat_temp[6], x_temp[18], fone=1, fzero=0;
	int n=18, k=18, m=18, ione=1;
	
	// Prediction step
	fx(xhat_pred, xhat, u, Ts); // state 
	Jfx(xhat, A, u, Ts); // update Jacobian A matrix
	
	// A*Phat_prev*A' + Q
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,A,&m,Phat,&k,&fzero,Jfx_temp,&m);
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Jfx_temp,&m,A,&n,&fzero,Phat_pred,&m);
	Phat_pred[0]+=Q[0];
	Phat_pred[19]+=Q[1];
	Phat_pred[38]+=Q[2];
	Phat_pred[57]+=Q[3];
	Phat_pred[76]+=Q[4];
	Phat_pred[95]+=Q[5];
	Phat_pred[114]+=Q[6];
	Phat_pred[133]+=Q[7];
	Phat_pred[152]+=Q[8];
	Phat_pred[171]+=Q[9];
	Phat_pred[190]+=Q[10];
	Phat_pred[209]+=Q[11];
	Phat_pred[228]+=Q[12];
	Phat_pred[247]+=Q[13];
	Phat_pred[266]+=Q[14];
	Phat_pred[285]+=Q[15];
	Phat_pred[304]+=Q[16];
	Phat_pred[323]+=Q[17];

	// Update step
	// S=C*P*C'+R; Innovation covariance
	n=18, k=18, m=6;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,C,&m,Phat_pred,&k,&fzero,C_temp,&m);
	n=6, k=18, m=6;
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,C_temp,&m,C,&n,&fzero,S,&m);
	S[0]+=R[0];
	S[7]+=R[1];
	S[14]+=R[2];
	S[21]+=R[3];
	S[28]+=R[4];
	S[35]+=R[5];

	// K=P*C'*S^-1; Kalman gain
	n=6, k=18, m=18; // 18x18 * 18*6 = 18x6
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Phat_pred,&m,C,&n,&fzero,K_temp,&m);
	mInverse6x6(S,S_inv);
	n=6, k=6, m=18; // 18x6 * 6*6 = 18x6
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K_temp,&m,S_inv,&k,&fzero,K,&m);

	// V=y_meas-C*x_hat; Innovation
	n=18, m=6; 
	F77_CALL(dgemv)("n",&m,&n,&fone,C,&m,xhat_pred,&ione,&fzero,xhat_temp,&ione);
	// Check if ymeas is new data
	if(flag==1){
		V[0]=0; // Old data, kill innovation
		V[1]=0; // Old data, kill innovation
		V[2]=0; // Old data, kill innovation
		V[3]=ymeas[3]-xhat_temp[3];
		V[4]=ymeas[4]-xhat_temp[4];
		V[5]=ymeas[5]-xhat_temp[5];
		//printf("Old data\n");
	}
	else{
		V[0]=ymeas[0]-xhat_temp[0];
		V[1]=ymeas[1]-xhat_temp[1];
		V[2]=ymeas[2]-xhat_temp[2];
		V[3]=ymeas[3]-xhat_temp[3];
		V[4]=ymeas[4]-xhat_temp[4];
		V[5]=ymeas[5]-xhat_temp[5];
	}

	// x=x+K*v; State update

	n=6, m=18;
	F77_CALL(dgemv)("n",&m,&n,&fone,K,&m,V,&ione,&fzero,x_temp,&ione);
	xhat[0]=xhat_pred[0]+x_temp[0];
	xhat[1]=xhat_pred[1]+x_temp[1];
	xhat[2]=xhat_pred[2]+x_temp[2];
	xhat[3]=xhat_pred[3]+x_temp[3];
	xhat[4]=xhat_pred[4]+x_temp[4];
	xhat[5]=xhat_pred[5]+x_temp[5];
	xhat[6]=xhat_pred[6]+x_temp[6];
	xhat[7]=xhat_pred[7]+x_temp[7];
	xhat[8]=xhat_pred[8]+x_temp[8];
	xhat[9]=xhat_pred[9]+x_temp[9];
	xhat[10]=xhat_pred[10]+x_temp[10];
	xhat[11]=xhat_pred[11]+x_temp[11];
	xhat[12]=xhat_pred[12]+x_temp[12];
	xhat[13]=xhat_pred[13]+x_temp[13];
	xhat[14]=xhat_pred[14]+x_temp[14];
	xhat[15]=xhat_pred[15]+x_temp[15];
	xhat[16]=xhat_pred[16]+x_temp[16];
	xhat[17]=xhat_pred[17]+x_temp[17];
	
	//printf("\nxhat\n");
	//printmat(xhat,18,1);
	
	// P=P-K*S*K'; Covariance update
	n=6, k=6, m=18; // 18x6 * 6x6 = 18x6
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K,&m,S,&k,&fzero,K_temp,&m); // K*S
	n=18, k=6, m=18; // 18x6 * 6x18 = 18x18
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,K_temp,&m,K,&n,&fzero,Phat,&m); // K_temp*K'
	n=18, k=18, m=18; // 18x18 * 18x18 = 18x18
	fzero=-1;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,eye18,&m,Phat_pred,&k,&fzero,Phat,&m); // P=P-K*S*K'
	
	//printf("\nPhat\n");
	//printmat(Phat,18,18);
}

// State Observer - Extended Kalman Filter with bias estimation for angles (eulers) (21 states)
void EKF_bias(double *Phat, double *xhat, double *u, double *ymeas, double *Q, double *R, double Ts, int flag){
	// Local variables
	double xhat_pred[21]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double C[126]={1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double eye21[441]={1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
	//double S_inv[36]={0.25,0,0,0,0,0,0,0.25,0,0,0,0,0,0,0.25,0,0,0,0,0,0,0.25,0,0,0,0,0,0,0.25,0,0,0,0,0,0,0.25};
	double S_inv[36];
	double A[441], S[36], C_temp[126], Jfx_temp[441], Phat_pred[441], K_temp[126], K[126], V[6], xhat_temp[6], x_temp[21], fone=1, fzero=0;
	int n=21, k=21, m=21, ione=1;
	
	// Prediction step
	fx_bias(xhat_pred, xhat, u, Ts); // state 
	Jfx_bias(xhat, A, u, Ts); // update Jacobian A matrix
	
	// A*Phat_prev*A' + Q
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,A,&m,Phat,&k,&fzero,Jfx_temp,&m);
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Jfx_temp,&m,A,&n,&fzero,Phat_pred,&m);
	Phat_pred[0]+=Q[0];
	Phat_pred[22]+=Q[1];
	Phat_pred[44]+=Q[2];
	Phat_pred[66]+=Q[3];
	Phat_pred[88]+=Q[4];
	Phat_pred[110]+=Q[5];
	Phat_pred[132]+=Q[6];
	Phat_pred[154]+=Q[7];
	Phat_pred[176]+=Q[8];
	Phat_pred[198]+=Q[9];
	Phat_pred[220]+=Q[10];
	Phat_pred[242]+=Q[11];
	Phat_pred[264]+=Q[12];
	Phat_pred[286]+=Q[13];
	Phat_pred[308]+=Q[14];
	Phat_pred[330]+=Q[15];
	Phat_pred[352]+=Q[16];
	Phat_pred[374]+=Q[17];
	Phat_pred[296]+=Q[18];
	Phat_pred[418]+=Q[19];
	Phat_pred[440]+=Q[20];


	// Update step
	// S=C*P*C'+R; Innovation covariance
	n=21, k=21, m=6;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,C,&m,Phat_pred,&k,&fzero,C_temp,&m);
	n=6, k=21, m=6;
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,C_temp,&m,C,&n,&fzero,S,&m);
	S[0]+=R[0];
	S[7]+=R[1];
	S[14]+=R[2];
	S[21]+=R[3];
	S[28]+=R[4];
	S[35]+=R[5];

	// K=P*C'*S^-1; Kalman gain
	n=6, k=21, m=21; // 21x21 * 21*6 = 21x6
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Phat_pred,&m,C,&n,&fzero,K_temp,&m);
	mInverse6x6(S,S_inv);
	n=6, k=6, m=21; // 21x6 * 6*6 = 21x6
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K_temp,&m,S_inv,&k,&fzero,K,&m);

	// V=y_meas-C*x_hat; Innovation
	n=21, m=6; 
	F77_CALL(dgemv)("n",&m,&n,&fone,C,&m,xhat_pred,&ione,&fzero,xhat_temp,&ione);
	// Check if ymeas is new data
	if(flag==1){
		V[0]=0; // Old data, kill innovation
		V[1]=0; // Old data, kill innovation
		V[2]=0; // Old data, kill innovation
		V[3]=ymeas[3]-xhat_temp[3];
		V[4]=ymeas[4]-xhat_temp[4];
		V[5]=ymeas[5]-xhat_temp[5];
		//printf("Old data\n");
	}
	else{
		V[0]=ymeas[0]-xhat_temp[0];
		V[1]=ymeas[1]-xhat_temp[1];
		V[2]=ymeas[2]-xhat_temp[2];
		V[3]=ymeas[3]-xhat_temp[3];
		V[4]=ymeas[4]-xhat_temp[4];
		V[5]=ymeas[5]-xhat_temp[5];
	}

	// x=x+K*v; State update

	n=6, m=21;
	F77_CALL(dgemv)("n",&m,&n,&fone,K,&m,V,&ione,&fzero,x_temp,&ione);
	xhat[0]=xhat_pred[0]+x_temp[0];
	xhat[1]=xhat_pred[1]+x_temp[1];
	xhat[2]=xhat_pred[2]+x_temp[2];
	xhat[3]=xhat_pred[3]+x_temp[3];
	xhat[4]=xhat_pred[4]+x_temp[4];
	xhat[5]=xhat_pred[5]+x_temp[5];
	xhat[6]=xhat_pred[6]+x_temp[6];
	xhat[7]=xhat_pred[7]+x_temp[7];
	xhat[8]=xhat_pred[8]+x_temp[8];
	xhat[9]=xhat_pred[9]+x_temp[9];
	xhat[10]=xhat_pred[10]+x_temp[10];
	xhat[11]=xhat_pred[11]+x_temp[11];
	xhat[12]=xhat_pred[12]+x_temp[12];
	xhat[13]=xhat_pred[13]+x_temp[13];
	xhat[14]=xhat_pred[14]+x_temp[14];
	xhat[15]=xhat_pred[15]+x_temp[15];
	xhat[16]=xhat_pred[16]+x_temp[16];
	xhat[17]=xhat_pred[17]+x_temp[17];
	xhat[18]=xhat_pred[18]+x_temp[18];
	xhat[19]=xhat_pred[19]+x_temp[19];
	xhat[20]=xhat_pred[20]+x_temp[20];
	
	//printf("\nxhat\n");
	//printmat(xhat,21,1);
	
	// P=P-K*S*K'; Covariance update
	n=6, k=6, m=21; // 21x6 * 6x6 = 21x6
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K,&m,S,&k,&fzero,K_temp,&m); // K*S
	n=21, k=6, m=21; // 21x6 * 6x21 = 21x21
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,K_temp,&m,K,&n,&fzero,Phat,&m); // K_temp*K'
	n=21, k=21, m=21; // 21x21 * 21x21 = 21x21
	fzero=-1;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,eye21,&m,Phat_pred,&k,&fzero,Phat,&m); // P=P-K*S*K'
	
	//printf("\nPhat\n");
	//printmat(Phat,21,21);
}

// State Observer - Extended Kalman Filter without inertia estimation (15 states)
void EKF_no_inertia(double *Phat, double *xhat, double *u, double *ymeas, double *Q, double *R, double Ts, int flag){
	// Local variables
	double xhat_pred[15]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double C[90]={1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double eye15[225]={1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
	double S_inv[36];
	double A[225], S[36], C_temp[90], Jfx_temp[225], Phat_pred[225], K_temp[90], K[90], V[6], xhat_temp[6], x_temp[15], fone=1, fzero=0;
	int n=15, k=15, m=15, ione=1;
	
	// Prediction step
	fx_no_inertia(xhat_pred, xhat, u, Ts); // state 
	Jfx_no_inertia(xhat, A, u, Ts); // update Jacobian A matrix
	
	// A*Phat_prev*A' + Q
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,A,&m,Phat,&k,&fzero,Jfx_temp,&m);
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Jfx_temp,&m,A,&n,&fzero,Phat_pred,&m);
	Phat_pred[0]+=Q[0];
	Phat_pred[16]+=Q[1];
	Phat_pred[32]+=Q[2];
	Phat_pred[48]+=Q[3];
	Phat_pred[64]+=Q[4];
	Phat_pred[80]+=Q[5];
	Phat_pred[96]+=Q[6];
	Phat_pred[112]+=Q[7];
	Phat_pred[128]+=Q[8];
	Phat_pred[144]+=Q[9];
	Phat_pred[160]+=Q[10];
	Phat_pred[176]+=Q[11];
	Phat_pred[192]+=Q[12];
	Phat_pred[208]+=Q[13];
	Phat_pred[224]+=Q[14];

	// Update step
	// S=C*P*C'+R; Innovation covariance
	n=15, k=15, m=6;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,C,&m,Phat_pred,&k,&fzero,C_temp,&m);
	n=6, k=15, m=6;
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,C_temp,&m,C,&n,&fzero,S,&m);
	S[0]+=R[0];
	S[7]+=R[7];
	S[14]+=R[14];
	S[21]+=R[21];
	S[28]+=R[28];
	S[35]+=R[35];

	// K=P*C'*S^-1; Kalman gain
	n=6, k=15, m=15; // 15x15 * 15x6 = 15x6
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Phat_pred,&m,C,&n,&fzero,K_temp,&m);
	mInverse6x6(S,S_inv);
	n=6, k=6, m=15; // 15x6 * 6*6 = 15x6
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K_temp,&m,S_inv,&k,&fzero,K,&m);

	// V=y_meas-C*x_hat; Innovation
	n=15, m=6; 
	F77_CALL(dgemv)("n",&m,&n,&fone,C,&m,xhat_pred,&ione,&fzero,xhat_temp,&ione);
	// Check if ymeas is new data
	if(flag==1){
		V[0]=0; // Old data, kill innovation
		V[1]=0; // Old data, kill innovation
		V[2]=0; // Old data, kill innovation
		V[3]=ymeas[3]-xhat_temp[3];
		V[4]=ymeas[4]-xhat_temp[4];
		V[5]=ymeas[5]-xhat_temp[5];
		//printf("Old data\n");
	}
	else{
		V[0]=ymeas[0]-xhat_temp[0];
		V[1]=ymeas[1]-xhat_temp[1];
		V[2]=ymeas[2]-xhat_temp[2];
		V[3]=ymeas[3]-xhat_temp[3];
		V[4]=ymeas[4]-xhat_temp[4];
		V[5]=ymeas[5]-xhat_temp[5];
	}
	
	//printf("\nymeas:\n");
	//printmat(ymeas,1,6);
	
	//printf("\nxhat_temp:\n");
	//printmat(xhat_temp,1,6);
		
	//printf("\nV:\n");
	//printmat(V,1,6);
	
	//printf("\nK:\n");
	//printmat(K,15,6);

	// x=x+K*v; State update
	n=6, m=15;
	F77_CALL(dgemv)("n",&m,&n,&fone,K,&m,V,&ione,&fzero,x_temp,&ione);
	xhat[0]=xhat_pred[0]+x_temp[0];
	xhat[1]=xhat_pred[1]+x_temp[1];
	xhat[2]=xhat_pred[2]+x_temp[2];
	xhat[3]=xhat_pred[3]+x_temp[3];
	xhat[4]=xhat_pred[4]+x_temp[4];
	xhat[5]=xhat_pred[5]+x_temp[5];
	xhat[6]=xhat_pred[6]+x_temp[6];
	xhat[7]=xhat_pred[7]+x_temp[7];
	xhat[8]=xhat_pred[8]+x_temp[8];
	xhat[9]=xhat_pred[9]+x_temp[9];
	xhat[10]=xhat_pred[10]+x_temp[10];
	xhat[11]=xhat_pred[11]+x_temp[11];
	xhat[12]=xhat_pred[12]+x_temp[12];
	xhat[13]=xhat_pred[13]+x_temp[13];
	xhat[14]=xhat_pred[14]+x_temp[14];
	
	//printf("\nxhat\n");
	//printmat(xhat,15,1);
	
	// P=P-K*S*K'; Covariance update
	n=6, k=6, m=15; // 15x6 * 6x6 = 15x6
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K,&m,S,&k,&fzero,K_temp,&m); // K*S
	n=15, k=6, m=15; // 15x6 * 6x15 = 15x15
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,K_temp,&m,K,&n,&fzero,Phat,&m); // K_temp*K'
	n=15, k=15, m=15; // 15x15 * 15x15 = 15x15
	fzero=-1;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,eye15,&m,Phat_pred,&k,&fzero,Phat,&m); // P=P-K*S*K'
	
	//printf("\nPhat\n");
	//printmat(Phat,15,15);
}

// State Observer - Extended Kalman Filter for 6 attitude states
void EKF_6x6(double *Phat, double *xhat, double *u, double *ymeas, double *Q, double *R, double Ts){
	// Local variables
	double xhat_pred[6]={0,0,0,0,0,0};
	double C[36]={1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1};
	double eye6[36]={1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1};
	double S_inv[36];
	double A[36], S[36], C_temp[36], Jfx_temp[36], Phat_pred[36], K_temp[36], K[36], V[6], xhat_temp[6], x_temp[6], fone=1, fzero=0;
	int n=6, k=6, m=6, ione=1;
	
	// Prediction step
	fx_6x1(xhat_pred, xhat, u, Ts); // state 
	Jfx_6x6(xhat, A, u, Ts); // update Jacobian A matrix
	
	// A*Phat_prev*A' + Q
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,A,&m,Phat,&k,&fzero,Jfx_temp,&m);
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Jfx_temp,&m,A,&n,&fzero,Phat_pred,&m);
	Phat_pred[0]+=Q[0];
	Phat_pred[7]+=Q[1];
	Phat_pred[14]+=Q[2];
	Phat_pred[21]+=Q[3];
	Phat_pred[28]+=Q[4];
	Phat_pred[35]+=Q[5];

	// Update step
	// S=C*P*C'+R; Innovation covariance
	n=6, k=6, m=6;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,C,&m,Phat_pred,&k,&fzero,C_temp,&m);
	n=6, k=6, m=6;
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,C_temp,&m,C,&n,&fzero,S,&m);
	S[0]+=R[0];
	S[7]+=R[7];
	S[14]+=R[14];
	S[21]+=R[21];
	S[28]+=R[28];
	S[35]+=R[35];

	// K=P*C'*S^-1; Kalman gain
	n=6, k=6, m=6; // 6x6 * 6x6 = 6x6
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Phat_pred,&m,C,&n,&fzero,K_temp,&m);
	mInverse6x6(S,S_inv);
	n=6, k=6, m=6; // 6x6 * 6*6 = 6x6
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K_temp,&m,S_inv,&k,&fzero,K,&m);

	// V=y_meas-C*x_hat; Innovation
	n=6, m=6; 
	F77_CALL(dgemv)("n",&m,&n,&fone,C,&m,xhat_pred,&ione,&fzero,xhat_temp,&ione);
	V[0]=ymeas[0]-xhat_temp[0];
	V[1]=ymeas[1]-xhat_temp[1];
	V[2]=ymeas[2]-xhat_temp[2];
	V[3]=ymeas[3]-xhat_temp[3];
	V[4]=ymeas[4]-xhat_temp[4];
	V[5]=ymeas[5]-xhat_temp[5];
	
	//printf("\nymeas:\n");
	//printmat(ymeas,1,6);
	
	//printf("\nxhat_temp:\n");
	//printmat(xhat_temp,1,6);
		
	//printf("\nV:\n");
	//printmat(V,1,6);
	
	//printf("\nK:\n");
	//printmat(K,15,6);

	// x=x+K*v; State update
	n=6, m=6;
	F77_CALL(dgemv)("n",&m,&n,&fone,K,&m,V,&ione,&fzero,x_temp,&ione);
	xhat[0]=xhat_pred[0]+x_temp[0];
	xhat[1]=xhat_pred[1]+x_temp[1];
	xhat[2]=xhat_pred[2]+x_temp[2];
	xhat[3]=xhat_pred[3]+x_temp[3];
	xhat[4]=xhat_pred[4]+x_temp[4];
	xhat[5]=xhat_pred[5]+x_temp[5];
	
	//printf("\nxhat\n");
	//printmat(xhat,15,1);
	
	// P=P-K*S*K'; Covariance update
	n=6, k=6, m=6; // 6x6 * 6x6 = 6x6
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K,&m,S,&k,&fzero,K_temp,&m); // K*S
	n=6, k=6, m=6; // 6x6 * 6x6 = 6x6
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,K_temp,&m,K,&n,&fzero,Phat,&m); // K_temp*K'
	n=6, k=6, m=6; // 6x6 * 6x6 = 6x6
	fzero=-1;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,eye6,&m,Phat_pred,&k,&fzero,Phat,&m); // P=P-K*S*K'
	
	//printf("\nPhat\n");
	//printmat(Phat,15,15);
}

// State Observer - Extended Kalman Filter for 9 attitude states (including bias estimation)
void EKF_9x9_bias(double *Phat, double *xhat, double *u, double *ymeas, double *Q, double *R, double Ts){
	// Local variables
	double xhat_pred[9]={0,0,0,0,0,0};
	double C[54]={1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double eye9[81]={1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1};
	double S_inv[36];
	double A[81], S[36], C_temp[81], Jfx_temp[81], Phat_pred[81], K_temp[54], K[54], V[6], xhat_temp[6], x_temp[6], fone=1, fzero=0;
	int n=9, k=9, m=9, ione=1;
	
	// Prediction step
	fx_9x1_bias(xhat_pred, xhat, u, Ts); // state 
	Jfx_9x9_bias(xhat, A, u, Ts); // update Jacobian A matrix
	
	// A*Phat_prev*A' + Q
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,A,&m,Phat,&k,&fzero,Jfx_temp,&m);
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Jfx_temp,&m,A,&n,&fzero,Phat_pred,&m);
	Phat_pred[0]+=Q[0];
	Phat_pred[10]+=Q[1];
	Phat_pred[20]+=Q[2];
	Phat_pred[30]+=Q[3];
	Phat_pred[40]+=Q[4];
	Phat_pred[50]+=Q[5];
	Phat_pred[60]+=Q[6];
	Phat_pred[70]+=Q[7];
	Phat_pred[80]+=Q[8];

	// Update step
	// S=C*P*C'+R; Innovation covariance
	n=9, k=9, m=6;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,C,&m,Phat_pred,&k,&fzero,C_temp,&m);
	n=6, k=9, m=6;
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,C_temp,&m,C,&n,&fzero,S,&m);
	S[0]+=R[0];
	S[7]+=R[7];
	S[14]+=R[14];
	S[21]+=R[21];
	S[28]+=R[28];
	S[35]+=R[35];

	// K=P*C'*S^-1; Kalman gain
	n=6, k=9, m=9; // 9x9 * 9x6 = 9x6
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Phat_pred,&m,C,&n,&fzero,K_temp,&m);
	mInverse6x6(S,S_inv);
	n=6, k=6, m=9; // 9x6 * 6*6 = 9x6
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K_temp,&m,S_inv,&k,&fzero,K,&m);

	// V=y_meas-C*x_hat; Innovation
	n=9, m=6; 
	F77_CALL(dgemv)("n",&m,&n,&fone,C,&m,xhat_pred,&ione,&fzero,xhat_temp,&ione);
	V[0]=ymeas[0]-xhat_temp[0];
	V[1]=ymeas[1]-xhat_temp[1];
	V[2]=ymeas[2]-xhat_temp[2];
	V[3]=ymeas[3]-xhat_temp[3];
	V[4]=ymeas[4]-xhat_temp[4];
	V[5]=ymeas[5]-xhat_temp[5];
	
	//printf("\nymeas:\n");
	//printmat(ymeas,1,6);
	
	//printf("\nxhat_temp:\n");
	//printmat(xhat_temp,1,6);
		
	//printf("\nV:\n");
	//printmat(V,1,6);
	
	//printf("\nK:\n");
	//printmat(K,15,6);

	// x=x+K*v; State update
	n=6, m=9;
	F77_CALL(dgemv)("n",&m,&n,&fone,K,&m,V,&ione,&fzero,x_temp,&ione);
	xhat[0]=xhat_pred[0]+x_temp[0];
	xhat[1]=xhat_pred[1]+x_temp[1];
	xhat[2]=xhat_pred[2]+x_temp[2];
	xhat[3]=xhat_pred[3]+x_temp[3];
	xhat[4]=xhat_pred[4]+x_temp[4];
	xhat[5]=xhat_pred[5]+x_temp[5];
	xhat[6]=xhat_pred[6]+x_temp[6];
	xhat[7]=xhat_pred[7]+x_temp[7];
	xhat[8]=xhat_pred[8]+x_temp[8];
	
	//printf("\nxhat\n");
	//printmat(xhat,15,1);
	
	// P=P-K*S*K'; Covariance update
	n=6, k=6, m=9; // 9x6 * 6x6 = 9x6
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K,&m,S,&k,&fzero,K_temp,&m); // K*S
	n=9, k=6, m=9; // 9x6 * 6x9 = 9x9
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,K_temp,&m,K,&n,&fzero,Phat,&m); // K_temp*K'
	n=9, k=9, m=9; // 9x9 * 9x9 = 9x9
	fzero=-1;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,eye9,&m,Phat_pred,&k,&fzero,Phat,&m); // P=P-K*S*K'
	
	//printf("\nPhat\n");
	//printmat(Phat,15,15);
}

// State Observer - Extended Kalman Filter for 9 position states
void EKF_9x9(double *Phat, double *xhat, double *u, double *ymeas, double *Q, double *R, double Ts, int flag, double *par_att){
	// Local variables
	double xhat_pred[9]={0,0,0,0,0,0,0,0,0};
	double C[27]={1,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double eye9[81]={1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1};
	double S_inv[9];
	double A[81], S[9], C_temp[27], Jfx_temp[81], Phat_pred[81], K_temp[27], K[27], V[3], xhat_temp[3], x_temp[9], fone=1, fzero=0;
	int n=9, k=9, m=9, ione=1;
	
	// Prediction step
	fx_9x1(xhat_pred, xhat, u, Ts, par_att); // state 
	Jfx_9x9(xhat, A, u, Ts, par_att); // update Jacobian A matrix
	
	// A*Phat_prev*A' + Q
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,A,&m,Phat,&k,&fzero,Jfx_temp,&m);
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Jfx_temp,&m,A,&n,&fzero,Phat_pred,&m);
	Phat_pred[0]+=Q[0];
	Phat_pred[10]+=Q[1];
	Phat_pred[20]+=Q[2];
	Phat_pred[30]+=Q[3];
	Phat_pred[40]+=Q[4];
	Phat_pred[50]+=Q[5];
	Phat_pred[60]+=Q[6];
	Phat_pred[70]+=Q[7];
	Phat_pred[80]+=Q[8];

	// Update step
	// S=C*P*C'+R; Innovation covariance
	n=9, k=9, m=3;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,C,&m,Phat_pred,&k,&fzero,C_temp,&m);
	n=3, k=9, m=3;
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,C_temp,&m,C,&n,&fzero,S,&m);
	S[0]+=R[0];
	S[4]+=R[4];
	S[8]+=R[8];

	// K=P*C'*S^-1; Kalman gain
	n=3, k=9, m=9; // 9x9 * 9x3 = 9x3
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Phat_pred,&m,C,&n,&fzero,K_temp,&m);
	mInverse(S,S_inv);
	n=3, k=3, m=9; // 9x3 * 3*3 = 9x3
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K_temp,&m,S_inv,&k,&fzero,K,&m);

	// V=y_meas-C*x_hat; Innovation
	n=9, m=3; 
	F77_CALL(dgemv)("n",&m,&n,&fone,C,&m,xhat_pred,&ione,&fzero,xhat_temp,&ione);
	// Check if ymeas is new data
	if(flag==1){
		V[0]=0; // Old data, kill innovation
		V[1]=0; // Old data, kill innovation
		V[2]=0; // Old data, kill innovation
		//printf("Old data\n");
	}
	else{
		V[0]=ymeas[0]-xhat_temp[0];
		V[1]=ymeas[1]-xhat_temp[1];
		V[2]=ymeas[2]-xhat_temp[2];
	}
	
	//printf("\nymeas:\n");
	//printmat(ymeas,1,6);
	
	//printf("\nxhat_temp:\n");
	//printmat(xhat_temp,1,6);
		
	//printf("\nV:\n");
	//printmat(V,1,6);
	
	//printf("\nK:\n");
	//printmat(K,15,6);

	// x=x+K*v; State update
	n=3, m=9;
	F77_CALL(dgemv)("n",&m,&n,&fone,K,&m,V,&ione,&fzero,x_temp,&ione);
	xhat[0]=xhat_pred[0]+x_temp[0];
	xhat[1]=xhat_pred[1]+x_temp[1];
	xhat[2]=xhat_pred[2]+x_temp[2];
	xhat[3]=xhat_pred[3]+x_temp[3];
	xhat[4]=xhat_pred[4]+x_temp[4];
	xhat[5]=xhat_pred[5]+x_temp[5];
	xhat[6]=xhat_pred[6]+x_temp[6];
	xhat[7]=xhat_pred[7]+x_temp[7];
	xhat[8]=xhat_pred[8]+x_temp[8];
	
	//printf("\nxhat\n");
	//printmat(xhat,15,1);
	
	// P=P-K*S*K'; Covariance update
	n=3, k=3, m=9; // 9x3 * 3x3 = 9x3
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K,&m,S,&k,&fzero,K_temp,&m); // K*S
	n=9, k=3, m=9; // 9x3 * 3x9 = 9x9
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,K_temp,&m,K,&n,&fzero,Phat,&m); // K_temp*K'
	n=9, k=9, m=9; // 9x9 * 9x9 = 9x9
	fzero=-1;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,eye9,&m,Phat_pred,&k,&fzero,Phat,&m); // P=P-K*S*K'
	
	//printf("\nPhat\n");
	//printmat(Phat,15,15);
}

// Nonlinear Model (18x1)
void fx(double *xhat, double *xhat_prev, double *u, double Ts){
	xhat[0]=xhat_prev[0]+Ts*xhat_prev[3]; // position x
    xhat[1]=xhat_prev[1]+Ts*xhat_prev[4]; // position y
    xhat[2]=xhat_prev[2]+Ts*xhat_prev[5]; // position z
    xhat[3]=xhat_prev[3]+Ts*(-par_k_d/par_mass*xhat_prev[3] + (par_k*par_c_m)/par_mass*(sin(xhat_prev[8])*sin(xhat_prev[6]) + cos(xhat_prev[8])*cos(xhat_prev[6])*sin(xhat_prev[7]))*(pow(u[0],2)+pow(u[1],2)+pow(u[2],2)+pow(u[3],2)) + xhat_prev[15]);
    xhat[4]=xhat_prev[4]+Ts*(-par_k_d/par_mass*xhat_prev[4] + (par_k*par_c_m)/par_mass*(cos(xhat_prev[6])*sin(xhat_prev[8])*sin(xhat_prev[7]) - cos(xhat_prev[8])*sin(xhat_prev[6]))*(pow(u[0],2)+pow(u[1],2)+pow(u[2],2)+pow(u[3],2)) + xhat_prev[16]);
    xhat[5]=xhat_prev[5]+Ts*(-par_k_d/par_mass*xhat_prev[5] + (par_k*par_c_m)/par_mass*(cos(xhat_prev[7])*cos(xhat_prev[6]))*(pow(u[0],2)+pow(u[1],2)+pow(u[2],2)+pow(u[3],2)) + xhat_prev[17]);
    xhat[6]=xhat_prev[6]+Ts*(xhat_prev[9] + xhat_prev[10]*sin(xhat_prev[6])*tan(xhat_prev[7])+xhat_prev[11]*cos(xhat_prev[6])*tan(xhat_prev[7]));
    xhat[7]=xhat_prev[7]+Ts*(xhat_prev[10]*cos(xhat_prev[6]) - xhat_prev[11]*sin(xhat_prev[6]));
    xhat[8]=xhat_prev[8]+Ts*(sin(xhat_prev[6])/cos(xhat_prev[7])*xhat_prev[10] + cos(xhat_prev[6])/cos(xhat_prev[7])*xhat_prev[11]);
    xhat[9]=xhat_prev[9]+Ts*((par_L*par_k*par_c_m)/xhat_prev[12]*(pow(u[0],2)-pow(u[2],2))-((xhat_prev[13]-xhat_prev[14])/xhat_prev[12])*xhat_prev[10]*xhat_prev[11]);
    xhat[10]=xhat_prev[10]+Ts*((par_L*par_k*par_c_m)/xhat_prev[13]*(pow(u[1],2)-pow(u[3],2))-((xhat_prev[14]-xhat_prev[12])/xhat_prev[13])*xhat_prev[9]*xhat_prev[11]);
    xhat[11]=xhat_prev[11]+Ts*(par_b*par_c_m/xhat_prev[14]*(pow(u[0],2)-pow(u[1],2)+pow(u[2],2)-pow(u[3],2))-((xhat_prev[12]-xhat_prev[13])/xhat_prev[14])*xhat_prev[9]*xhat_prev[10]);
    xhat[12]=xhat_prev[12]; // inertia i_xx
    xhat[13]=xhat_prev[13]; // inertia i_yy
    xhat[14]=xhat_prev[14]; // inertia i_zz
    xhat[15]=xhat_prev[15]; // disturbance x
    xhat[16]=xhat_prev[16]; // disturbance y
    xhat[17]=xhat_prev[17]; // disturbance z (including gravity)
}

// Jacobian of model (18x18)
void Jfx(double *xhat, double *A, double *u, double Ts){
	A[0]=1;		A[18]=0;	A[36]=0;	A[54]=Ts;            			A[72]=0;                		A[90]=0;                		A[108]=0;                                                                                                      														A[126]=0;                                                                                                 							A[144]=0;                                         																													A[162]=0;                                         		A[180]=0;                                         		A[198]=0;                                                 A[216]=0;                                        						                                                   					A[234]=0;                                              							                                                       		A[252]=0; 	 																																						A[270]=0;  	A[288]=0;  	A[306]=0;
    A[1]=0;		A[19]=1; 	A[37]=0;	A[55]=0;   				        A[73]=Ts;               		A[91]=0;                		A[109]=0;                                                                                                      														A[127]=0;                                                                                                 							A[145]=0;                                         																													A[163]=0;                                         		A[181]=0;                                         		A[199]=0;                                                 A[217]=0;                                                             					                              					A[235]=0;                                                                       						                              		A[253]=0;  																																							A[271]=0;  	A[289]=0;  	A[307]=0;
    A[2]=0;		A[20]=0; 	A[38]=1;	A[56]=0;               			A[74]=0;                		A[92]=Ts;               		A[110]=0;                                                                                                      														A[128]=0;                                                                                                 							A[146]=0;                                         																													A[164]=0;                                         		A[182]=0;                                         		A[200]=0;                                                 A[218]=0;                                                                                 					          					A[236]=0;                                                                                               						      		A[254]=0;  																																							A[272]=0;  	A[290]=0;  	A[308]=0;
    A[3]=0; 	A[21]=0; 	A[39]=0; 	A[57]=1-(Ts*par_k_d)/par_mass;	A[75]=0;                		A[93]=0;  						A[111]=(Ts*par_c_m*par_k*(cos(xhat[6])*sin(xhat[8]) - cos(xhat[8])*sin(xhat[6])*sin(xhat[7]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;   	A[129]=(Ts*par_c_m*par_k*cos(xhat[6])*cos(xhat[7])*cos(xhat[8])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;	A[147]=(Ts*par_c_m*par_k*(cos(xhat[8])*sin(xhat[6]) - cos(xhat[6])*sin(xhat[7])*sin(xhat[8]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;   	A[165]=0;                                         		A[183]=0;                                         		A[201]=0;                                                 A[219]=0;                                	                                                        										A[237]=0;                                             						                                                        		A[255]=0; 																																							A[273]=Ts;	A[291]=0;  	A[309]=0;
    A[4]=0; 	A[22]=0; 	A[40]=0;	A[58]=0; 						A[76]=1-(Ts*par_k_d)/par_mass;  A[94]=0; 						A[112]=-(Ts*par_c_m*par_k*(cos(xhat[6])*cos(xhat[8]) + sin(xhat[6])*sin(xhat[7])*sin(xhat[8]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;  	A[130]=(Ts*par_c_m*par_k*cos(xhat[6])*cos(xhat[7])*sin(xhat[8])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass; 	A[148]=(Ts*par_c_m*par_k*(sin(xhat[6])*sin(xhat[8]) + cos(xhat[6])*cos(xhat[8])*sin(xhat[7]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;   	A[166]=0;          	            	                   	A[184]=0;                                         		A[202]=0;                                                 A[220]=0;                               					                                                            					A[238]=0;                                                                   						                                  		A[256]=0;  																																							A[274]=0; 	A[292]=Ts;  A[310]=0;
    A[5]=0; 	A[23]=0; 	A[41]=0;	A[59]=0;     		            A[77]=0; 						A[95]=1-(Ts*par_k_d)/par_mass;  A[113]=-(Ts*par_c_m*par_k*cos(xhat[7])*sin(xhat[6])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;                                   			A[131]=-(Ts*par_c_m*par_k*cos(xhat[6])*sin(xhat[7])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;              A[149]=0;                                         																													A[167]=0;           	                              	A[185]=0;                                         		A[203]=0;                                                 A[221]=0;                                                 					                                          					A[239]=0;                                                                                           						          		A[257]=0;  																																							A[275]=0;  	A[293]=0; 	A[311]=Ts;
    A[6]=0; 	A[24]=0; 	A[42]=0;	A[60]=0;            		    A[78]=0;                		A[96]=0;                		A[114]=Ts*(xhat[10]*cos(xhat[6])*tan(xhat[7]) - xhat[11]*sin(xhat[6])*tan(xhat[7])) + 1;                 															A[132]=Ts*(xhat[11]*cos(xhat[6])*(pow(tan(xhat[7]),2) + 1) + xhat[10]*sin(xhat[6])*(pow(tan(xhat[7]),2) + 1));						A[150]=0;                                        																													A[168]=Ts;              	  							A[186]=Ts*sin(xhat[6])*tan(xhat[7]);                	A[204]=Ts*cos(xhat[6])*tan(xhat[7]);					  A[222]=0;                                                                     					                      					A[240]=0;                                         						                                                            		A[258]=0;  																																							A[276]=0;  	A[294]=0;  	A[312]=0;
    A[7]=0; 	A[25]=0; 	A[43]=0;	A[61]=0; 		                A[79]=0;                		A[97]=0;                		A[115]=-Ts*(xhat[11]*cos(xhat[6]) + xhat[10]*sin(xhat[6]));                                                                        									A[133]=1;                                                                                                     						A[151]=0;                                         																													A[169]=0;                   	         				A[187]=Ts*cos(xhat[6]);                           		A[205]=-Ts*sin(xhat[6]);                                  A[223]=0;                                                                                         					  					A[241]=0;                                                               						                                      		A[259]=0;  																																							A[277]=0;  	A[295]=0;  	A[313]=0;
    A[8]=0; 	A[26]=0; 	A[44]=0;	A[62]=0;        		        A[80]=0;                		A[98]=0;                		A[116]=Ts*((xhat[10]*cos(xhat[6]))/cos(xhat[7]) - (xhat[11]*sin(xhat[6]))/cos(xhat[7]));																			A[134]=Ts*((xhat[11]*cos(xhat[6])*sin(xhat[7]))/pow(cos(xhat[7]),2) + (xhat[10]*sin(xhat[6])*sin(xhat[7]))/pow(cos(xhat[7]),2));  	A[152]=1;                                         																													A[170]=0;              									A[188]=(Ts*sin(xhat[6]))/cos(xhat[7]);					A[206]=(Ts*cos(xhat[6]))/cos(xhat[7]);                    A[224]=0;                                                                                           										A[242]=0;                                                                                       						              		A[260]=0;  																																							A[278]=0;  	A[296]=0;  	A[314]=0;
    A[9]=0; 	A[27]=0; 	A[45]=0;	A[63]=0; 		                A[81]=0;                		A[99]=0;                		A[117]=0;                                                                                                      														A[135]=0;                                                                                                           				A[153]=0;                                         																													A[171]=1; 												A[189]=-(Ts*xhat[11]*(xhat[13] - xhat[14]))/xhat[12]; 	A[207]=-(Ts*xhat[10]*(xhat[13] - xhat[14]))/xhat[12]; 	  A[225]=-Ts*((par_c_m*par_k*(pow(u[0],2) - pow(u[2],2)))/(8*pow(xhat[12],2)) - (xhat[10]*xhat[11]*(xhat[13] - xhat[14]))/pow(xhat[12],2));	A[243]=-(Ts*xhat[10]*xhat[11])/xhat[12];                                                                         							A[261]=(Ts*xhat[10]*xhat[11])/xhat[12];  																															A[279]=0;  	A[297]=0;  	A[315]=0;
    A[10]=0; 	A[28]=0; 	A[46]=0;	A[64]=0;        		        A[82]=0;                		A[100]=0;               		A[118]=0;                                                                                                      														A[136]=0;                                                                                                           				A[154]=0;  																																							A[172]=(Ts*xhat[11]*(xhat[12] - xhat[14]))/xhat[13];	A[190]=1;												A[208]=(Ts*xhat[9]*(xhat[12] - xhat[14]))/xhat[13];  	  A[226]=(Ts*xhat[9]*xhat[11])/xhat[13]; 																									A[244]=-Ts*((par_c_m*par_k*(pow(u[1],2) - pow(u[3],2)))/(8*pow(xhat[13],2)) + (xhat[9]*xhat[11]*(xhat[12] - xhat[14]))/pow(xhat[13],2)); 	A[262]=-(Ts*xhat[9]*xhat[11])/xhat[13];  																															A[280]=0;  	A[298]=0;  	A[316]=0;
    A[11]=0; 	A[29]=0; 	A[47]=0;	A[65]=0;                		A[83]=0;                		A[101]=0;               		A[119]=0;                                                                                                      														A[137]=0;                                                                                                           				A[155]=0; 																																							A[173]=-(Ts*xhat[10]*(xhat[12] - xhat[13]))/xhat[14];	A[191]=-(Ts*xhat[9]*(xhat[12] - xhat[13]))/xhat[14];    A[209]=1;												  A[227]=-(Ts*xhat[9]*xhat[10])/xhat[14];                                                                									A[245]=(Ts*xhat[9]*xhat[10])/xhat[14];																										A[263]=-Ts*((par_b*par_c_m*(pow(u[0],2) - pow(u[1],2) + pow(u[2],2) - pow(u[3],2)))/pow(xhat[14],2) - (xhat[9]*xhat[10]*(xhat[12] - xhat[13]))/pow(xhat[14],2));	A[281]=0;	A[299]=0;	A[317]=0;
    A[12]=0; 	A[30]=0; 	A[48]=0;	A[66]=0;                		A[84]=0;                		A[102]=0;               		A[120]=0;                                                                                                      														A[138]=0;                                                                                                           				A[156]=0;                                         																													A[174]=0;          		                               	A[192]=0;                                         		A[210]=0;                					              A[228]=1;                                                                                           										A[246]=0;                                                                       						                              		A[264]=0;  																																							A[282]=0;  	A[300]=0;  	A[318]=0;
    A[13]=0; 	A[31]=0; 	A[49]=0;	A[67]=0;                		A[85]=0;                		A[103]=0;               		A[121]=0;                                                                                                      														A[139]=0;                                                                                                           				A[157]=0;                                         																													A[175]=0;               	                          	A[193]=0;                                         		A[211]=0;                                                 A[229]=0;                                                                                           										A[247]=1;                                               						                                                      		A[265]=0;  																																							A[283]=0;  	A[301]=0;  	A[319]=0;
    A[14]=0; 	A[32]=0; 	A[50]=0;	A[68]=0;                		A[86]=0;                		A[104]=0;               		A[122]=0;                                                                                                      														A[140]=0;                                                                                                           				A[158]=0;                                         																													A[176]=0;                   	                      	A[194]=0;                                         		A[212]=0;                                                 A[230]=0;                                                                                           										A[248]=0;                                                                                               						      		A[266]=1;  																																							A[284]=0;  	A[302]=0;  	A[320]=0;
    A[15]=0; 	A[33]=0; 	A[51]=0;	A[69]=0;                		A[87]=0;                		A[105]=0;               		A[123]=0;                                                                                                      														A[141]=0;                                                                                                           				A[159]=0;                                         																													A[177]=0;                       	                  	A[195]=0;                                         		A[213]=0;                                                 A[231]=0;                                                                                           										A[249]=0;                                                          							                                           		A[267]=0;  																																							A[285]=1;  	A[303]=0;  	A[321]=0;
    A[16]=0; 	A[34]=0; 	A[52]=0;	A[70]=0;                		A[88]=0;                		A[106]=0;               		A[124]=0;                                                                                                      														A[142]=0;                                                                                                           				A[160]=0;                                         																													A[178]=0;                           	              	A[196]=0;                                         		A[214]=0;                                                 A[232]=0;                                                                                           										A[250]=0;                                                                                   						                  		A[268]=0;  																																							A[286]=0;  	A[304]=1;  	A[322]=0;
    A[17]=0; 	A[35]=0; 	A[53]=0;	A[71]=0;                		A[89]=0;                		A[107]=0;               		A[125]=0;                                                                                                      														A[143]=0;                                                                                                           				A[161]=0;                                         																													A[179]=0;                               	          	A[197]=0;                                         		A[215]=0;                                                 A[233]=0;                                                                                           										A[251]=0;                                                                                                     								A[269]=0;  																																							A[287]=0;	A[305]=0;  	A[323]=1;	
}

// Nonlinear Model with bias estimation for angles (21x1)
void fx_bias(double *xhat, double *xhat_prev, double *u, double Ts){
	xhat[0]=xhat_prev[0]+Ts*xhat_prev[3]; // position x
    xhat[1]=xhat_prev[1]+Ts*xhat_prev[4]; // position y
    xhat[2]=xhat_prev[2]+Ts*xhat_prev[5]; // position z
    xhat[3]=xhat_prev[3]+Ts*(-par_k_d/par_mass*xhat_prev[3] + (par_k*par_c_m)/par_mass*(sin(xhat_prev[8])*sin(xhat_prev[6]) + cos(xhat_prev[8])*cos(xhat_prev[6])*sin(xhat_prev[7]))*(pow(u[0],2)+pow(u[1],2)+pow(u[2],2)+pow(u[3],2)) + xhat_prev[15]);
    xhat[4]=xhat_prev[4]+Ts*(-par_k_d/par_mass*xhat_prev[4] + (par_k*par_c_m)/par_mass*(cos(xhat_prev[6])*sin(xhat_prev[8])*sin(xhat_prev[7]) - cos(xhat_prev[8])*sin(xhat_prev[6]))*(pow(u[0],2)+pow(u[1],2)+pow(u[2],2)+pow(u[3],2)) + xhat_prev[16]);
    xhat[5]=xhat_prev[5]+Ts*(-par_k_d/par_mass*xhat_prev[5] + (par_k*par_c_m)/par_mass*(cos(xhat_prev[7])*cos(xhat_prev[6]))*(pow(u[0],2)+pow(u[1],2)+pow(u[2],2)+pow(u[3],2)) + xhat_prev[17]);
    xhat[6]=xhat_prev[6]+	xhat_prev[18]	+Ts*(xhat_prev[9] + xhat_prev[10]*sin(xhat_prev[6])*tan(xhat_prev[7])+xhat_prev[11]*cos(xhat_prev[6])*tan(xhat_prev[7]));
    xhat[7]=xhat_prev[7]+	xhat_prev[19]	+Ts*(xhat_prev[10]*cos(xhat_prev[6]) - xhat_prev[11]*sin(xhat_prev[6]));
    xhat[8]=xhat_prev[8]+	xhat_prev[20]	+Ts*(sin(xhat_prev[6])/cos(xhat_prev[7])*xhat_prev[10] + cos(xhat_prev[6])/cos(xhat_prev[7])*xhat_prev[11]);
    xhat[9]=xhat_prev[9]+Ts*((par_L*par_k*par_c_m)/xhat_prev[12]*(pow(u[0],2)-pow(u[2],2))-((xhat_prev[13]-xhat_prev[14])/xhat_prev[12])*xhat_prev[10]*xhat_prev[11]);
    xhat[10]=xhat_prev[10]+Ts*((par_L*par_k*par_c_m)/xhat_prev[13]*(pow(u[1],2)-pow(u[3],2))-((xhat_prev[14]-xhat_prev[12])/xhat_prev[13])*xhat_prev[9]*xhat_prev[11]);
    xhat[11]=xhat_prev[11]+Ts*(par_b*par_c_m/xhat_prev[14]*(pow(u[0],2)-pow(u[1],2)+pow(u[2],2)-pow(u[3],2))-((xhat_prev[12]-xhat_prev[13])/xhat_prev[14])*xhat_prev[9]*xhat_prev[10]);
    xhat[12]=xhat_prev[12]; // inertia i_xx
    xhat[13]=xhat_prev[13]; // inertia i_yy
    xhat[14]=xhat_prev[14]; // inertia i_zz
    xhat[15]=xhat_prev[15]; // disturbance x
    xhat[16]=xhat_prev[16]; // disturbance y
    xhat[17]=xhat_prev[17]; // disturbance z (including gravity)
	xhat[18]=xhat_prev[18]; // bias phi
    xhat[19]=xhat_prev[19]; // bias theta
    xhat[20]=xhat_prev[20]; // bias psi
}

// Jacobian of model with bias estimation for angles (21x21)
void Jfx_bias(double *xhat, double *A, double *u, double Ts){
	A[0]=1;		A[21]=0;	A[42]=0;	A[63]=Ts;            			A[84]=0;                		A[105]=0;                		A[126]=0;                                                                                                      														A[147]=0;                                                                                                 							A[168]=0;                                         																													A[189]=0;                                         		A[210]=0;                                         		A[231]=0;                                                 A[252]=0;                                        						                                                   					A[273]=0;                                              							                                                       		A[294]=0; 	 																																						A[315]=0;  	A[336]=0;  	A[357]=0;	A[378]=0;  	A[399]=0;  	A[420]=0;
    A[1]=0;		A[22]=1; 	A[43]=0;	A[64]=0;   				        A[85]=Ts;               		A[106]=0;                		A[127]=0;                                                                                                      														A[148]=0;                                                                                                 							A[169]=0;                                         																													A[190]=0;                                         		A[211]=0;                                         		A[232]=0;                                                 A[253]=0;                                                             					                              					A[274]=0;                                                                       						                              		A[295]=0;  																																							A[316]=0;  	A[337]=0;  	A[358]=0;	A[379]=0;  	A[400]=0;  	A[421]=0;
    A[2]=0;		A[23]=0; 	A[44]=1;	A[65]=0;               			A[86]=0;                		A[107]=Ts;               		A[128]=0;                                                                                                      														A[149]=0;                                                                                                 							A[170]=0;                                         																													A[191]=0;                                         		A[212]=0;                                         		A[233]=0;                                                 A[254]=0;                                                                                 					          					A[275]=0;                                                                                               						      		A[296]=0;  																																							A[317]=0;  	A[338]=0;  	A[359]=0;	A[380]=0;  	A[401]=0;  	A[422]=0;
    A[3]=0; 	A[24]=0; 	A[45]=0; 	A[66]=1-(Ts*par_k_d)/par_mass;	A[87]=0;                		A[108]=0;  						A[129]=(Ts*par_c_m*par_k*(cos(xhat[6])*sin(xhat[8]) - cos(xhat[8])*sin(xhat[6])*sin(xhat[7]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;   	A[150]=(Ts*par_c_m*par_k*cos(xhat[6])*cos(xhat[7])*cos(xhat[8])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;	A[171]=(Ts*par_c_m*par_k*(cos(xhat[8])*sin(xhat[6]) - cos(xhat[6])*sin(xhat[7])*sin(xhat[8]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;   	A[192]=0;                                         		A[213]=0;                                         		A[234]=0;                                                 A[255]=0;                                	                                                        										A[276]=0;                                             						                                                        		A[297]=0; 																																							A[318]=Ts;	A[339]=0;  	A[360]=0;	A[381]=0;  	A[402]=0;  	A[423]=0;
    A[4]=0; 	A[25]=0; 	A[46]=0;	A[67]=0; 						A[88]=1-(Ts*par_k_d)/par_mass;  A[109]=0; 						A[130]=-(Ts*par_c_m*par_k*(cos(xhat[6])*cos(xhat[8]) + sin(xhat[6])*sin(xhat[7])*sin(xhat[8]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;  	A[151]=(Ts*par_c_m*par_k*cos(xhat[6])*cos(xhat[7])*sin(xhat[8])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass; 	A[172]=(Ts*par_c_m*par_k*(sin(xhat[6])*sin(xhat[8]) + cos(xhat[6])*cos(xhat[8])*sin(xhat[7]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;   	A[193]=0;          	            	                   	A[214]=0;                                         		A[235]=0;                                                 A[256]=0;                               					                                                            					A[277]=0;                                                                   						                                  		A[298]=0;  																																							A[319]=0; 	A[340]=Ts;  A[361]=0;	A[382]=0;  	A[403]=0;  	A[424]=0;
    A[5]=0; 	A[26]=0; 	A[47]=0;	A[68]=0;     		            A[89]=0; 						A[110]=1-(Ts*par_k_d)/par_mass;  A[131]=-(Ts*par_c_m*par_k*cos(xhat[7])*sin(xhat[6])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;                                   			A[152]=-(Ts*par_c_m*par_k*cos(xhat[6])*sin(xhat[7])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;              A[173]=0;                                         																													A[194]=0;           	                              	A[215]=0;                                         		A[236]=0;                                                 A[257]=0;                                                 					                                          					A[278]=0;                                                                                           						          		A[299]=0;  																																							A[320]=0;  	A[341]=0; 	A[362]=Ts;	A[383]=0;  	A[404]=0;  	A[425]=0;
    A[6]=0; 	A[27]=0; 	A[48]=0;	A[69]=0;            		    A[90]=0;                		A[111]=0;                		A[132]=Ts*(xhat[10]*cos(xhat[6])*tan(xhat[7]) - xhat[11]*sin(xhat[6])*tan(xhat[7])) + 1;                 															A[153]=Ts*(xhat[11]*cos(xhat[6])*(pow(tan(xhat[7]),2) + 1) + xhat[10]*sin(xhat[6])*(pow(tan(xhat[7]),2) + 1));						A[174]=0;                                        																													A[195]=Ts;              	  							A[216]=Ts*sin(xhat[6])*tan(xhat[7]);                	A[237]=Ts*cos(xhat[6])*tan(xhat[7]);					  A[258]=0;                                                                     					                      					A[279]=0;                                         						                                                            		A[300]=0;  																																							A[321]=0;  	A[342]=0;  	A[363]=0;	A[384]=1;  	A[405]=0;  	A[426]=0;
    A[7]=0; 	A[28]=0; 	A[49]=0;	A[70]=0; 		                A[91]=0;                		A[112]=0;                		A[133]=-Ts*(xhat[11]*cos(xhat[6]) + xhat[10]*sin(xhat[6]));                                                                        									A[154]=1;                                                                                                     						A[175]=0;                                         																													A[196]=0;                   	         				A[217]=Ts*cos(xhat[6]);                           		A[238]=-Ts*sin(xhat[6]);                                  A[259]=0;                                                                                         					  					A[280]=0;                                                               						                                      		A[301]=0;  																																							A[322]=0;  	A[343]=0;  	A[364]=0;	A[385]=0;  	A[406]=1;  	A[427]=0;
    A[8]=0; 	A[29]=0; 	A[50]=0;	A[71]=0;        		        A[92]=0;                		A[113]=0;                		A[134]=Ts*((xhat[10]*cos(xhat[6]))/cos(xhat[7]) - (xhat[11]*sin(xhat[6]))/cos(xhat[7]));																			A[155]=Ts*((xhat[11]*cos(xhat[6])*sin(xhat[7]))/pow(cos(xhat[7]),2) + (xhat[10]*sin(xhat[6])*sin(xhat[7]))/pow(cos(xhat[7]),2));  	A[176]=1;                                         																													A[197]=0;              									A[218]=(Ts*sin(xhat[6]))/cos(xhat[7]);					A[239]=(Ts*cos(xhat[6]))/cos(xhat[7]);                    A[260]=0;                                                                                           										A[281]=0;                                                                                       						              		A[302]=0;  																																							A[323]=0;  	A[344]=0;  	A[365]=0;	A[386]=0;  	A[407]=0;  	A[428]=1;
    A[9]=0; 	A[30]=0; 	A[51]=0;	A[72]=0; 		                A[93]=0;                		A[114]=0;                		A[135]=0;                                                                                                      														A[156]=0;                                                                                                           				A[177]=0;                                         																													A[198]=1; 												A[219]=-(Ts*xhat[11]*(xhat[13] - xhat[14]))/xhat[12]; 	A[240]=-(Ts*xhat[10]*(xhat[13] - xhat[14]))/xhat[12]; 	  A[261]=-Ts*((par_c_m*par_k*(pow(u[0],2) - pow(u[2],2)))/(8*pow(xhat[12],2)) - (xhat[10]*xhat[11]*(xhat[13] - xhat[14]))/pow(xhat[12],2));	A[282]=-(Ts*xhat[10]*xhat[11])/xhat[12];                                                                         							A[303]=(Ts*xhat[10]*xhat[11])/xhat[12];  																															A[324]=0;  	A[345]=0;  	A[366]=0;	A[387]=0;  	A[408]=0;  	A[429]=0;
    A[10]=0; 	A[31]=0; 	A[52]=0;	A[73]=0;        		        A[94]=0;                		A[115]=0;               		A[136]=0;                                                                                                      														A[157]=0;                                                                                                           				A[178]=0;  																																							A[199]=(Ts*xhat[11]*(xhat[12] - xhat[14]))/xhat[13];	A[220]=1;												A[241]=(Ts*xhat[9]*(xhat[12] - xhat[14]))/xhat[13];  	  A[262]=(Ts*xhat[9]*xhat[11])/xhat[13]; 																									A[283]=-Ts*((par_c_m*par_k*(pow(u[1],2) - pow(u[3],2)))/(8*pow(xhat[13],2)) + (xhat[9]*xhat[11]*(xhat[12] - xhat[14]))/pow(xhat[13],2)); 	A[304]=-(Ts*xhat[9]*xhat[11])/xhat[13];  																															A[325]=0;  	A[346]=0;  	A[367]=0;	A[388]=0;  	A[409]=0;  	A[430]=0;
    A[11]=0; 	A[32]=0; 	A[53]=0;	A[74]=0;                		A[95]=0;                		A[116]=0;               		A[137]=0;                                                                                                      														A[158]=0;                                                                                                           				A[179]=0; 																																							A[200]=-(Ts*xhat[10]*(xhat[12] - xhat[13]))/xhat[14];	A[221]=-(Ts*xhat[9]*(xhat[12] - xhat[13]))/xhat[14];    A[242]=1;												  A[263]=-(Ts*xhat[9]*xhat[10])/xhat[14];                                                                									A[284]=(Ts*xhat[9]*xhat[10])/xhat[14];																										A[305]=-Ts*((par_b*par_c_m*(pow(u[0],2) - pow(u[1],2) + pow(u[2],2) - pow(u[3],2)))/pow(xhat[14],2) - (xhat[9]*xhat[10]*(xhat[12] - xhat[13]))/pow(xhat[14],2));	A[326]=0;	A[347]=0;	A[368]=0;	A[389]=0;  	A[410]=0;  	A[431]=0;
    A[12]=0; 	A[33]=0; 	A[54]=0;	A[75]=0;                		A[96]=0;                		A[117]=0;               		A[138]=0;                                                                                                      														A[159]=0;                                                                                                           				A[180]=0;                                         																													A[201]=0;          		                               	A[222]=0;                                         		A[243]=0;                					              A[264]=1;                                                                                           										A[285]=0;                                                                       						                              		A[306]=0;  																																							A[327]=0;  	A[348]=0;  	A[369]=0;	A[390]=0;  	A[411]=0;  	A[432]=0;
    A[13]=0; 	A[34]=0; 	A[55]=0;	A[76]=0;                		A[97]=0;                		A[118]=0;               		A[139]=0;                                                                                                      														A[160]=0;                                                                                                           				A[181]=0;                                         																													A[202]=0;               	                          	A[223]=0;                                         		A[244]=0;                                                 A[265]=0;                                                                                           										A[286]=1;                                               						                                                      		A[307]=0;  																																							A[328]=0;  	A[349]=0;  	A[370]=0;	A[391]=0;  	A[412]=0;  	A[433]=0;
    A[14]=0; 	A[35]=0; 	A[56]=0;	A[77]=0;                		A[98]=0;                		A[119]=0;               		A[140]=0;                                                                                                      														A[161]=0;                                                                                                           				A[182]=0;                                         																													A[203]=0;                   	                      	A[224]=0;                                         		A[245]=0;                                                 A[266]=0;                                                                                           										A[287]=0;                                                                                               						      		A[308]=1;  																																							A[329]=0;  	A[350]=0;  	A[371]=0;	A[392]=0;  	A[413]=0;  	A[434]=0;
    A[15]=0; 	A[36]=0; 	A[57]=0;	A[78]=0;                		A[99]=0;                		A[120]=0;               		A[141]=0;                                                                                                      														A[162]=0;                                                                                                           				A[183]=0;                                         																													A[204]=0;                       	                  	A[225]=0;                                         		A[246]=0;                                                 A[267]=0;                                                                                           										A[288]=0;                                                          							                                           		A[309]=0;  																																							A[330]=1;  	A[351]=0;  	A[372]=0;	A[393]=0;  	A[414]=0;  	A[435]=0;
    A[16]=0; 	A[37]=0; 	A[58]=0;	A[79]=0;                		A[100]=0;                		A[121]=0;               		A[142]=0;                                                                                                      														A[163]=0;                                                                                                           				A[184]=0;                                         																													A[205]=0;                           	              	A[226]=0;                                         		A[247]=0;                                                 A[268]=0;                                                                                           										A[289]=0;                                                                                   						                  		A[310]=0;  																																							A[331]=0;  	A[352]=1;  	A[373]=0;	A[394]=0;  	A[415]=0;  	A[436]=0;
    A[17]=0; 	A[38]=0; 	A[59]=0;	A[80]=0;                		A[101]=0;                		A[122]=0;               		A[143]=0;                                                                                                      														A[164]=0;                                                                                                           				A[185]=0;                                         																													A[206]=0;                               	          	A[227]=0;                                         		A[248]=0;                                                 A[269]=0;                                                                                           										A[290]=0;                                                                                                     								A[311]=0;  																																							A[332]=0;	A[353]=0;  	A[374]=1;	A[395]=0;  	A[416]=0;  	A[437]=0;
    A[18]=0; 	A[39]=0; 	A[60]=0;	A[81]=0;                		A[102]=0;                		A[123]=0;               		A[144]=0;                                                                                                      														A[165]=0;                                                                                                           				A[186]=0;                                         																													A[207]=0;                       	                  	A[228]=0;                                         		A[249]=0;                                                 A[270]=0;                                                                                           										A[291]=0;                                                          							                                           		A[312]=0;  																																							A[333]=0;  	A[354]=0;  	A[375]=0;	A[396]=1;  	A[417]=0;  	A[438]=0;
    A[19]=0; 	A[40]=0; 	A[61]=0;	A[82]=0;                		A[103]=0;                		A[124]=0;               		A[145]=0;                                                                                                      														A[166]=0;                                                                                                           				A[187]=0;                                         																													A[208]=0;                           	              	A[229]=0;                                         		A[250]=0;                                                 A[271]=0;                                                                                           										A[292]=0;                                                                                   						                  		A[313]=0;  																																							A[334]=0;  	A[355]=0;  	A[376]=0;	A[397]=0;  	A[418]=1;  	A[439]=0;
    A[20]=0; 	A[41]=0; 	A[62]=0;	A[83]=0;                		A[104]=0;                		A[125]=0;               		A[146]=0;                                                                                                      														A[167]=0;                                                                                                           				A[188]=0;                                         																													A[209]=0;                               	          	A[230]=0;                                         		A[251]=0;                                                 A[272]=0;                                                                                           										A[293]=0;                                                                                                     								A[314]=0;  																																							A[335]=0;	A[356]=0;  	A[377]=0;	A[398]=0;  	A[419]=0;  	A[440]=1;
}

// Nonlinear Model without inertia estimation (15x1)
void fx_no_inertia(double *xhat, double *xhat_prev, double *u, double Ts){
	xhat[0]=xhat_prev[0] + Ts*xhat_prev[3];
	xhat[1]=xhat_prev[1] + Ts*xhat_prev[4];
	xhat[2]=xhat_prev[2] + Ts*xhat_prev[5];
	xhat[3]=xhat_prev[3] + Ts*(xhat_prev[12] - (par_k_d*xhat_prev[3])/par_mass + (par_c_m*par_k*(sin(xhat_prev[6])*sin(xhat_prev[8]) + cos(xhat_prev[6])*cos(xhat_prev[8])*sin(xhat_prev[7]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[4]=xhat_prev[4] - Ts*((par_k_d*xhat_prev[4])/par_mass - xhat_prev[13] + (par_c_m*par_k*(cos(xhat_prev[8])*sin(xhat_prev[6]) - cos(xhat_prev[6])*sin(xhat_prev[7])*sin(xhat_prev[8]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[5]=xhat_prev[5] + Ts*(xhat_prev[14] - (par_k_d*xhat_prev[5])/par_mass + (par_c_m*par_k*cos(xhat_prev[6])*cos(xhat_prev[7])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[6]=xhat_prev[6] + Ts*(xhat_prev[9] + xhat_prev[11]*cos(xhat_prev[6])*tan(xhat_prev[7]) + xhat_prev[10]*sin(xhat_prev[6])*tan(xhat_prev[7]));
	xhat[7]=xhat_prev[7] + Ts*(xhat_prev[10]*cos(xhat_prev[6]) - xhat_prev[11]*sin(xhat_prev[6]));
	xhat[8]=xhat_prev[8] + Ts*((xhat_prev[11]*cos(xhat_prev[6]))/cos(xhat_prev[7]) + (xhat_prev[10]*sin(xhat_prev[6]))/cos(xhat_prev[7]));
	xhat[9]=xhat_prev[9] - Ts*((xhat_prev[10]*xhat_prev[11]*(par_i_yy - par_i_zz))/par_i_xx - (par_L*par_c_m*par_k*(pow(u[0],2) - pow(u[2],2)))/par_i_xx);
	xhat[10]=xhat_prev[10] + Ts*((xhat_prev[9]*xhat_prev[11]*(par_i_xx - par_i_zz))/par_i_yy + (par_L*par_c_m*par_k*(pow(u[1],2) - pow(u[3],2)))/par_i_yy);
	xhat[11]=xhat_prev[11] + Ts*((par_b*par_c_m*(pow(u[0],2) - pow(u[1],2) + pow(u[2],2) - pow(u[3],2)))/par_i_zz - (xhat_prev[9]*xhat_prev[10]*(par_i_xx - par_i_yy))/par_i_zz);
	xhat[12]=xhat_prev[12];
	xhat[13]=xhat_prev[13];
	xhat[14]=xhat_prev[14];
}

// Jacobian of model without inertia estimation (15x15)
void Jfx_no_inertia(double *xhat, double *A, double *u, double Ts){
	A[0]=1;A[1]=0;A[2]=0;A[3]=0;A[4]=0;A[5]=0;A[6]=0;A[7]=0;A[8]=0;A[9]=0;A[10]=0;A[11]=0;A[12]=0;A[13]=0;A[14]=0;A[15]=0;A[16]=1;A[17]=0;A[18]=0;A[19]=0;A[20]=0;A[21]=0;A[22]=0;A[23]=0;A[24]=0;A[25]=0;A[26]=0;A[27]=0;A[28]=0;A[29]=0;A[30]=0;A[31]=0;A[32]=1;A[33]=0;A[34]=0;A[35]=0;A[36]=0;A[37]=0;A[38]=0;A[39]=0;A[40]=0;A[41]=0;A[42]=0;A[43]=0;A[44]=0;A[45]=Ts;A[46]=0;A[47]=0;A[48]=1 - (Ts*par_k_d)/par_mass;A[49]=0;A[50]=0;A[51]=0;A[52]=0;A[53]=0;A[54]=0;A[55]=0;A[56]=0;A[57]=0;A[58]=0;A[59]=0;A[60]=0;A[61]=Ts;A[62]=0;A[63]=0;A[64]=1 - (Ts*par_k_d)/par_mass;A[65]=0;A[66]=0;A[67]=0;A[68]=0;A[69]=0;A[70]=0;A[71]=0;A[72]=0;A[73]=0;A[74]=0;A[75]=0;A[76]=0;A[77]=Ts;A[78]=0;A[79]=0;A[80]=1 - (Ts*par_k_d)/par_mass;A[81]=0;A[82]=0;A[83]=0;A[84]=0;A[85]=0;A[86]=0;A[87]=0;A[88]=0;A[89]=0;A[90]=0;A[91]=0;A[92]=0;A[93]=(Ts*par_c_m*par_k*(cos(xhat[6])*sin(xhat[8]) - cos(xhat[8])*sin(xhat[6])*sin(xhat[7]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[94]=-(Ts*par_c_m*par_k*(cos(xhat[6])*cos(xhat[8]) + sin(xhat[6])*sin(xhat[7])*sin(xhat[8]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[95]=-(Ts*par_c_m*par_k*cos(xhat[7])*sin(xhat[6])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[96]=Ts*(xhat[10]*cos(xhat[6])*tan(xhat[7]) - xhat[11]*sin(xhat[6])*tan(xhat[7])) + 1;A[97]=-Ts*(xhat[11]*cos(xhat[6]) + xhat[10]*sin(xhat[6]));A[98]=Ts*((xhat[10]*cos(xhat[6]))/cos(xhat[7]) - (xhat[11]*sin(xhat[6]))/cos(xhat[7]));A[99]=0;A[100]=0;A[101]=0;A[102]=0;A[103]=0;A[104]=0;A[105]=0;A[106]=0;A[107]=0;A[108]=(Ts*par_c_m*par_k*cos(xhat[6])*cos(xhat[7])*cos(xhat[8])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[109]=(Ts*par_c_m*par_k*cos(xhat[6])*cos(xhat[7])*sin(xhat[8])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[110]=-(Ts*par_c_m*par_k*cos(xhat[6])*sin(xhat[7])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[111]=Ts*(xhat[11]*cos(xhat[6])*(pow(tan(xhat[7]),2) + 1) + xhat[10]*sin(xhat[6])*(pow(tan(xhat[7]),2) + 1));A[112]=1;A[113]=Ts*((xhat[11]*cos(xhat[6])*sin(xhat[7]))/pow(cos(xhat[7]),2) + (xhat[10]*sin(xhat[6])*sin(xhat[7]))/pow(cos(xhat[7]),2));A[114]=0;A[115]=0;A[116]=0;A[117]=0;A[118]=0;A[119]=0;A[120]=0;A[121]=0;A[122]=0;A[123]=(Ts*par_c_m*par_k*(cos(xhat[8])*sin(xhat[6]) - cos(xhat[6])*sin(xhat[7])*sin(xhat[8]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[124]=(Ts*par_c_m*par_k*(sin(xhat[6])*sin(xhat[8]) + cos(xhat[6])*cos(xhat[8])*sin(xhat[7]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[125]=0;A[126]=0;A[127]=0;A[128]=1;A[129]=0;A[130]=0;A[131]=0;A[132]=0;A[133]=0;A[134]=0;A[135]=0;A[136]=0;A[137]=0;A[138]=0;A[139]=0;A[140]=0;A[141]=Ts;A[142]=0;A[143]=0;A[144]=1;A[145]=(Ts*xhat[11]*(par_i_xx - par_i_zz))/par_i_yy;A[146]=-(Ts*xhat[10]*(par_i_xx - par_i_yy))/par_i_zz;A[147]=0;A[148]=0;A[149]=0;A[150]=0;A[151]=0;A[152]=0;A[153]=0;A[154]=0;A[155]=0;A[156]=Ts*sin(xhat[6])*tan(xhat[7]);A[157]=Ts*cos(xhat[6]);A[158]=(Ts*sin(xhat[6]))/cos(xhat[7]);A[159]=-(Ts*xhat[11]*(par_i_yy - par_i_zz))/par_i_xx;A[160]=1;A[161]=-(Ts*xhat[9]*(par_i_xx - par_i_yy))/par_i_zz;A[162]=0;A[163]=0;A[164]=0;A[165]=0;A[166]=0;A[167]=0;A[168]=0;A[169]=0;A[170]=0;A[171]=Ts*cos(xhat[6])*tan(xhat[7]);A[172]=-Ts*sin(xhat[6]);A[173]=(Ts*cos(xhat[6]))/cos(xhat[7]);A[174]=-(Ts*xhat[10]*(par_i_yy - par_i_zz))/par_i_xx;A[175]=(Ts*xhat[9]*(par_i_xx - par_i_zz))/par_i_yy;A[176]=1;A[177]=0;A[178]=0;A[179]=0;A[180]=0;A[181]=0;A[182]=0;A[183]=Ts;A[184]=0;A[185]=0;A[186]=0;A[187]=0;A[188]=0;A[189]=0;A[190]=0;A[191]=0;A[192]=1;A[193]=0;A[194]=0;A[195]=0;A[196]=0;A[197]=0;A[198]=0;A[199]=Ts;A[200]=0;A[201]=0;A[202]=0;A[203]=0;A[204]=0;A[205]=0;A[206]=0;A[207]=0;A[208]=1;A[209]=0;A[210]=0;A[211]=0;A[212]=0;A[213]=0;A[214]=0;A[215]=Ts;A[216]=0;A[217]=0;A[218]=0;A[219]=0;A[220]=0;A[221]=0;A[222]=0;A[223]=0;A[224]=1;
}

// Nonlinear Model for attitude states (6x1)
void fx_6x1(double *xhat, double *xhat_prev, double *u, double Ts){
	xhat[0]=xhat_prev[0] + Ts*(xhat_prev[3] + xhat_prev[5]*cos(xhat_prev[0])*tan(xhat_prev[1]) + xhat_prev[4]*sin(xhat_prev[0])*tan(xhat_prev[1]));
	xhat[1]=xhat_prev[1] + Ts*(xhat_prev[4]*cos(xhat_prev[0]) - xhat_prev[5]*sin(xhat_prev[0]));
	xhat[2]=xhat_prev[2] + Ts*((xhat_prev[5]*cos(xhat_prev[0]))/cos(xhat_prev[1]) + (xhat_prev[4]*sin(xhat_prev[0]))/cos(xhat_prev[1]));
	xhat[3]=xhat_prev[3] - Ts*((xhat_prev[4]*xhat_prev[5]*(par_i_yy - par_i_zz))/par_i_xx - (par_L*par_c_m*par_k*(pow(u[0],2) - pow(u[2],2)))/par_i_xx);
	xhat[4]=xhat_prev[4] + Ts*((xhat_prev[3]*xhat_prev[5]*(par_i_xx - par_i_zz))/par_i_yy + (par_L*par_c_m*par_k*(pow(u[1],2) - pow(u[3],2)))/par_i_yy);
	xhat[5]=xhat_prev[5] + Ts*((par_b*par_c_m*(pow(u[0],2) - pow(u[1],2) + pow(u[2],2) - pow(u[3],2)))/par_i_zz - (xhat_prev[3]*xhat_prev[4]*(par_i_xx - par_i_yy))/par_i_zz);
}

// Jacobian of model for attitude states (6x6)
void Jfx_6x6(double *xhat, double *A, double *u, double Ts){
	A[0]=Ts*(xhat[4]*cos(xhat[0])*tan(xhat[1]) - xhat[5]*sin(xhat[0])*tan(xhat[1])) + 1;A[1]=-Ts*(xhat[5]*cos(xhat[0]) + xhat[4]*sin(xhat[0]));A[2]=Ts*((xhat[4]*cos(xhat[0]))/cos(xhat[1]) - (xhat[5]*sin(xhat[0]))/cos(xhat[1]));A[3]=0;A[4]=0;A[5]=0;A[6]=Ts*(xhat[5]*cos(xhat[0])*(pow(tan(xhat[1]),2) + 1) + xhat[4]*sin(xhat[0])*(pow(tan(xhat[1]),2) + 1));A[7]=1;A[8]=Ts*((xhat[5]*cos(xhat[0])*sin(xhat[1]))/pow(cos(xhat[1]),2) + (xhat[4]*sin(xhat[0])*sin(xhat[1]))/pow(cos(xhat[1]),2));A[9]=0;A[10]=0;A[11]=0;A[12]=0;A[13]=0;A[14]=1;A[15]=0;A[16]=0;A[17]=0;A[18]=Ts;A[19]=0;A[20]=0;A[21]=1;A[22]=(Ts*xhat[5]*(par_i_xx - par_i_zz))/par_i_yy;A[23]=-(Ts*xhat[4]*(par_i_xx - par_i_yy))/par_i_zz;A[24]=Ts*sin(xhat[0])*tan(xhat[1]);A[25]=Ts*cos(xhat[0]);A[26]=(Ts*sin(xhat[0]))/cos(xhat[1]);A[27]=-(Ts*xhat[5]*(par_i_yy - par_i_zz))/par_i_xx;A[28]=1;A[29]=-(Ts*xhat[3]*(par_i_xx - par_i_yy))/par_i_zz;A[30]=Ts*cos(xhat[0])*tan(xhat[1]);A[31]=-Ts*sin(xhat[0]);A[32]=(Ts*cos(xhat[0]))/cos(xhat[1]);A[33]=-(Ts*xhat[4]*(par_i_yy - par_i_zz))/par_i_xx;A[34]=(Ts*xhat[3]*(par_i_xx - par_i_zz))/par_i_yy;A[35]=1;
}

// Nonlinear Model for position states (9x1)
void fx_9x1(double *xhat, double *xhat_prev, double *u, double Ts, double *par_att){
	xhat[0]=xhat_prev[0] + Ts*xhat_prev[3];
	xhat[1]=xhat_prev[1] + Ts*xhat_prev[4];
	xhat[2]=xhat_prev[2] + Ts*xhat_prev[5];
	xhat[3]=xhat_prev[3] + Ts*(xhat_prev[6] - (par_k_d*xhat_prev[3])/par_mass + (par_c_m*par_k*(sin(par_att[0])*sin(par_att[2]) + cos(par_att[0])*cos(par_att[2])*sin(par_att[1]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[4]=xhat_prev[4] - Ts*((par_k_d*xhat_prev[4])/par_mass - xhat_prev[7] + (par_c_m*par_k*(cos(par_att[2])*sin(par_att[0]) - cos(par_att[0])*sin(par_att[1])*sin(par_att[2]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[5]=xhat_prev[5] + Ts*(xhat_prev[8] - (par_k_d*xhat_prev[5])/par_mass + (par_c_m*par_k*cos(par_att[0])*cos(par_att[1])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[6]=xhat_prev[6];
	xhat[7]=xhat_prev[7];
	xhat[8]=xhat_prev[8];
}

// Jacobian of model for position states (9x9)
void Jfx_9x9(double *xhat, double *A, double *u, double Ts, double *par_att){
	A[0]=1;A[1]=0;A[2]=0;A[3]=0;A[4]=0;A[5]=0;A[6]=0;A[7]=0;A[8]=0;A[9]=0;A[10]=1;A[11]=0;A[12]=0;A[13]=0;A[14]=0;A[15]=0;A[16]=0;A[17]=0;A[18]=0;A[19]=0;A[20]=1;A[21]=0;A[22]=0;A[23]=0;A[24]=0;A[25]=0;A[26]=0;A[27]=Ts;A[28]=0;A[29]=0;A[30]=1 - (Ts*par_k_d)/par_mass;A[31]=0;A[32]=0;A[33]=0;A[34]=0;A[35]=0;A[36]=0;A[37]=Ts;A[38]=0;A[39]=0;A[40]=1 - (Ts*par_k_d)/par_mass;A[41]=0;A[42]=0;A[43]=0;A[44]=0;A[45]=0;A[46]=0;A[47]=Ts;A[48]=0;A[49]=0;A[50]=1 - (Ts*par_k_d)/par_mass;A[51]=0;A[52]=0;A[53]=0;A[54]=0;A[55]=0;A[56]=0;A[57]=Ts;A[58]=0;A[59]=0;A[60]=1;A[61]=0;A[62]=0;A[63]=0;A[64]=0;A[65]=0;A[66]=0;A[67]=Ts;A[68]=0;A[69]=0;A[70]=1;A[71]=0;A[72]=0;A[73]=0;A[74]=0;A[75]=0;A[76]=0;A[77]=Ts;A[78]=0;A[79]=0;A[80]=1;
}

// Nonlinear Model for attitude states including bias estimation (9x1)
void fx_9x1_bias(double *xhat, double *xhat_prev, double *u, double Ts){
	xhat[0]=xhat_prev[0] + Ts*(xhat_prev[3] + xhat_prev[5]*cos(xhat_prev[0])*tan(xhat_prev[1]) + xhat_prev[4]*sin(xhat_prev[0])*tan(xhat_prev[1]));
	xhat[1]=xhat_prev[1] + Ts*(xhat_prev[4]*cos(xhat_prev[0]) - xhat_prev[5]*sin(xhat_prev[0]));
	xhat[2]=xhat_prev[2] + Ts*((xhat_prev[5]*cos(xhat_prev[0]))/cos(xhat_prev[1]) + (xhat_prev[4]*sin(xhat_prev[0]))/cos(xhat_prev[1]));
	xhat[3]=xhat_prev[3] + Ts*(xhat_prev[6] - (xhat_prev[4]*xhat_prev[5]*(par_i_yy - par_i_zz))/par_i_xx + (par_L*par_c_m*par_k*(pow(u[0],2) - pow(u[2],2)))/par_i_xx);
	xhat[4]=xhat_prev[4] + Ts*(xhat_prev[7] + (xhat_prev[3]*xhat_prev[5]*(par_i_xx - par_i_zz))/par_i_yy + (par_L*par_c_m*par_k*(pow(u[1],2) - pow(u[3],2)))/par_i_yy);
	xhat[5]=xhat_prev[5] + Ts*(xhat_prev[8] + (par_b*par_c_m*(pow(u[0],2) - pow(u[1],2) + pow(u[2],2) - pow(u[3],2)))/par_i_zz - (xhat_prev[3]*xhat_prev[4]*(par_i_xx - par_i_yy))/par_i_zz);
	xhat[6]=xhat_prev[6];
	xhat[7]=xhat_prev[7];
	xhat[8]=xhat_prev[8];
}

// Jacobian of model for attitude states including bias estimation (9x9)
void Jfx_9x9_bias(double *xhat, double *A, double *u, double Ts){
	A[0]=Ts*(xhat[4]*cos(xhat[0])*tan(xhat[1]) - xhat[5]*sin(xhat[0])*tan(xhat[1])) + 1;A[1]=-Ts*(xhat[5]*cos(xhat[0]) + xhat[4]*sin(xhat[0]));A[2]=Ts*((xhat[4]*cos(xhat[0]))/cos(xhat[1]) - (xhat[5]*sin(xhat[0]))/cos(xhat[1]));A[3]=0;A[4]=0;A[5]=0;A[6]=0;A[7]=0;A[8]=0;A[9]=Ts*(xhat[5]*cos(xhat[0])*(pow(tan(xhat[1]),2) + 1) + xhat[4]*sin(xhat[0])*(pow(tan(xhat[1]),2) + 1));A[10]=1;A[11]=Ts*((xhat[5]*cos(xhat[0])*sin(xhat[1]))/pow(cos(xhat[1]),2) + (xhat[4]*sin(xhat[0])*sin(xhat[1]))/pow(cos(xhat[1]),2));A[12]=0;A[13]=0;A[14]=0;A[15]=0;A[16]=0;A[17]=0;A[18]=0;A[19]=0;A[20]=1;A[21]=0;A[22]=0;A[23]=0;A[24]=0;A[25]=0;A[26]=0;A[27]=Ts;A[28]=0;A[29]=0;A[30]=1;A[31]=(Ts*xhat[5]*(par_i_xx - par_i_zz))/par_i_yy;A[32]=-(Ts*xhat[4]*(par_i_xx - par_i_yy))/par_i_zz;A[33]=0;A[34]=0;A[35]=0;A[36]=Ts*sin(xhat[0])*tan(xhat[1]);A[37]=Ts*cos(xhat[0]);A[38]=(Ts*sin(xhat[0]))/cos(xhat[1]);A[39]=-(Ts*xhat[5]*(par_i_yy - par_i_zz))/par_i_xx;A[40]=1;A[41]=-(Ts*xhat[3]*(par_i_xx - par_i_yy))/par_i_zz;A[42]=0;A[43]=0;A[44]=0;A[45]=Ts*cos(xhat[0])*tan(xhat[1]);A[46]=-Ts*sin(xhat[0]);A[47]=(Ts*cos(xhat[0]))/cos(xhat[1]);A[48]=-(Ts*xhat[4]*(par_i_yy - par_i_zz))/par_i_xx;A[49]=(Ts*xhat[3]*(par_i_xx - par_i_zz))/par_i_yy;A[50]=1;A[51]=0;A[52]=0;A[53]=0;A[54]=0;A[55]=0;A[56]=0;A[57]=Ts;A[58]=0;A[59]=0;A[60]=1;A[61]=0;A[62]=0;A[63]=0;A[64]=0;A[65]=0;A[66]=0;A[67]=Ts;A[68]=0;A[69]=0;A[70]=1;A[71]=0;A[72]=0;A[73]=0;A[74]=0;A[75]=0;A[76]=0;A[77]=Ts;A[78]=0;A[79]=0;A[80]=1;
}

