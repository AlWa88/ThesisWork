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
#define CALIBRATION 1000
#define MAG_CALIBRATION 1000
#define BUFFER 25

/******************************************************************/
/*******************VARIABLES & PREDECLARATIONS********************/
/******************************************************************/

// Predeclarations
static void *threadSensorFusion (void*);
static void *threadPWMControl (void*);
static void *threadPipeCommunicationToSensor(void*);

void qNormalize(double*);
void q2euler(double*, double*);
void q2euler_zyx(double *, double *);
void magnetometerUpdate(double*, double*, double*, double*, double*, double*, double, int*);
void Qq(double*, double*);
void dQqdq(double*, double*, double*, double*, double*, double*, double*);
void sensorCalibration(double *, double *, double *, double *, int );

void ekfCalibration6x6(double*, double*, double*, double*, int);
void ekfCalibration9x9_bias(double*, double*, double*, double*, int);
void ekfCalibration9x9(double*, double*, double*, double*, int);
void ekfCalibration7x7(double*, double*, double*, double*, int);
	
void lowPassFilter(double *, double*, double *, double* , double* , double* );
void lowPassFilterGyroZ(double* gyrRaw, double* gyrRawMem, double* b_gyr);

void MadgwickQuaternionUpdate(float, float, float, float, float, float, float, float, float, float *, float);

int loadSettings(double*, char*, int);
//void saveSettings(double*, char*, int, FILE**);
void saveSettings(double*, char*, int);
//void saveData(double*, char* , int, FILE**, int);
void saveData(double*, char* , int);
void printBits(size_t const, void const * const);


void EKF_6x6(double*, double*, double*, double*, double*, double*, double);
void EKF_9x9(double*, double*, double*, double*, double*, double*, double, int, double*);
void EKF_8x8(double*, double*, double*, double*, double*, double*, double, int, double*);
void EKF_9x9_bias(double*, double*, double*, double*, double*, double*, double);
void EKF_7x7(double *, double *, double *, double *, double *, double *, double , int , double *);


void fx_6x1(double*, double*, double*, double);
void Jfx_6x6(double*, double*, double*, double);
void fx_9x1(double*, double*, double*, double, double*);
void Jfx_9x9(double*, double*, double*, double, double*);
void fx_8x1(double*, double*, double*, double, double*);
void Jfx_8x8(double*, double*, double*, double, double*);
void fx_9x1_bias(double*, double*, double*, double);
void Jfx_9x9_bias(double*, double*, double*, double);
void fx_7x1(double*, double*, double*, double, double*);
void Jfx_7x7(double*, double*, double*, double, double*);
void saturation(double*, int, double, double);

// Static variables for threads
static double sensorRawDataPosition[3]={0,0,0}; // Global variable in sensor.c to communicate between IMU read and angle fusion threads
static double controlData[19]={.1,.1,.1,.1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}; // Global variable in sensor.c to pass control signal u from controller.c to EKF in sensor fusion {pwm0,pwm1,pwm2,pwm3,thrust,taux,tauy,tauz};
static double keyboardData[18]= {0,0,0,0,0,0,0,0,0,0,1,0.01,0.05,1,0,0,0,0}; // {ref_x,ref_y,ref_z, switch [0=STOP, 1=FLY], PWM print, Timer print, EKF print, reset ekf/mpc, EKF print 6 states, restart calibration, ramp ref, alpha, beta, mpc position toggle, ff toggle mpAtt, save data, PID trigger, PWM range setting}
static double tuningEkfData[18]={ekf_Q_1,ekf_Q_2,ekf_Q_3,ekf_Q_4,ekf_Q_5,ekf_Q_6,ekf_Q_7,ekf_Q_8,ekf_Q_9,ekf_Q_10,ekf_Q_11,ekf_Q_12,ekf_Q_13,ekf_Q_14,ekf_Q_15,ekf_Q_16,ekf_Q_17,ekf_Q_18};
static double tuningMpcData[14]={mpcPos_Q_1,mpcPos_Q_2,mpcPos_Q_3,mpcPos_Q_4,mpcPos_Q_5,mpcPos_Q_6,mpcAtt_Q_1,mpcAtt_Q_2,mpcAtt_Q_3,mpcAtt_Q_4,mpcAtt_Q_5,mpcAtt_Q_6,mpcAlt_Q_1,mpcAlt_Q_2}; // Q and Qf mpc {x,xdot,y,ydot,xform,yform,phi,phidot,theta,thetadot,psi,psidot,z,zdot}
static double tuningMpcQfData[9]={mpcAtt_Qf_1,mpcAtt_Qf_2,mpcAtt_Qf_3,mpcAtt_Qf_4,mpcAtt_Qf_5,mpcAtt_Qf_6,mpcAtt_Qf_1_2,mpcAtt_Qf_3_4,mpcAtt_Qf_5_6};
static double tuningMpcDataControl[6]={mpcPos_R_1,mpcPos_R_2,mpcAtt_R_1,mpcAtt_R_2,mpcAtt_R_3,mpcAlt_R_1}; // R mpc {pos,pos,taux,tauy,tauz,alt}
static double tuningPidData[6]={pid_pos_x_kp_def,pid_pos_x_ki_def,mpcAtt_ki_def,pid_pos_y_kp_def,pid_pos_y_ki_def,mpcPos_ki_def}; // PID gains

static double positionsData[9]={0};
static double positions_timeoutData[3]={0};


// Variables
static int beaconConnected=0;
static const int ione = 1;
static const int itwo = 2;
static const int ithree = 3;
static const int iseven = 7;
static const double fone = 1;
static const double ftwo = 2;
static const double fzero = 0;
static const double fmone = -1;

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
	pthread_t threadSenFus, threadPWMCtrl, threadCommToSens;
	int threadPID2, threadPID3, threadPID4; 
	
	threadPID2=pthread_create(&threadSenFus, NULL, &threadSensorFusion, &pipeArrayStruct);
	threadPID3=pthread_create(&threadPWMCtrl, NULL, &threadPWMControl, arg1);
	threadPID4=pthread_create(&threadCommToSens, NULL, &threadPipeCommunicationToSensor, arg2);
	
	// Set up thread scheduler priority for real time tasks
	struct sched_param paramThread2, paramThread3, paramThread4; // paramThread1, 
	paramThread2.sched_priority = PRIORITY_SENSOR_FUSION;
	paramThread3.sched_priority = PRIORITY_SENSOR_PWM;
	paramThread4.sched_priority = PRIORITY_SENSOR_PIPE_COMMUNICATION;
	if(sched_setscheduler(threadPID2, SCHED_FIFO, &paramThread2)==-1) {perror("sched_setscheduler failed for threadPID2");exit(-1);}
	if(sched_setscheduler(threadPID3, SCHED_FIFO, &paramThread3)==-1) {perror("sched_setscheduler failed for threadPID3");exit(-1);}
	if(sched_setscheduler(threadPID4, SCHED_FIFO, &paramThread4)==-1) {perror("sched_setscheduler failed for threadPID3");exit(-1);}
	
	// If threads created successful, start them
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
	double communicationDataBuffer[84];
	double keyboardDataBuffer[18];
	double tuningEkfBuffer[18];
	double positionsBuffer[9];
	double positions_timeoutBuffer[3];
	
		//double communicationDataBuffer[84];
	//double keyboardDataBuffer[18];
	double tuningMpcBuffer[14];
	double tuningMpcQfBuffer[9];
	double tuningMpcBufferControl[6];
	double tuningPidBuffer[6];
	//double manualThrustBuffer[1];
	//double positionsBuffer[9];
	//double positions_timeoutBuffer[3];
	
	
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
				
		memcpy(keyboardDataBuffer, communicationDataBuffer, sizeof(communicationDataBuffer)*18/84);
		memcpy(tuningEkfBuffer, communicationDataBuffer+38, sizeof(communicationDataBuffer)*18/84);
		memcpy(positionsBuffer, communicationDataBuffer+72, sizeof(communicationDataBuffer)*9/84);
		memcpy(positions_timeoutBuffer, communicationDataBuffer+81, sizeof(communicationDataBuffer)*3/84);

		//memcpy(keyboardDataBuffer, communicationDataBuffer, sizeof(communicationDataBuffer)*18/84);
		memcpy(tuningMpcBuffer, communicationDataBuffer+18, sizeof(communicationDataBuffer)*14/84);
		memcpy(tuningMpcBufferControl, communicationDataBuffer+32, sizeof(communicationDataBuffer)*6/84);
		memcpy(tuningPidBuffer, communicationDataBuffer+56, sizeof(communicationDataBuffer)*6/84);
		memcpy(tuningMpcQfBuffer, communicationDataBuffer+62, sizeof(communicationDataBuffer)*9/84);	
		//memcpy(manualThrustBuffer, communicationDataBuffer+71, sizeof(communicationDataBuffer)*1/84);
		//memcpy(tuningMpcQfBuffer, communicationDataBuffer+62, sizeof(communicationDataBuffer)*9/84);	
		//memcpy(manualThrustBuffer, communicationDataBuffer+71, sizeof(communicationDataBuffer)*1/84);
		//memcpy(positionsBuffer, communicationDataBuffer+72, sizeof(communicationDataBuffer)*9/84);	
		//memcpy(positions_timeoutBuffer, communicationDataBuffer+81, sizeof(communicationDataBuffer)*3/84);
	

		// Put new data in to global variable in communication.c
		pthread_mutex_lock(&mutexKeyboardData);

			//memcpy(references, keyboardDataBuffer, sizeof(keyboardDataBuffer)*3/18); // {ref_x,ref_y,ref_z}
			//memcpy(keyboardData, keyboardDataBuffer, sizeof(keyboardDataBuffer)); // {ref_x,ref_y,ref_z, switch[0=STOP, 1=FLY], pwm_print, timer_print,ekf_print,reset ekf/mpc, EKF print 6 states, reset calibration sensor.c, ramp ref}
			memcpy(tuningMpcData, tuningMpcBuffer, sizeof(tuningMpcBuffer));
			memcpy(tuningMpcQfData, tuningMpcQfBuffer, sizeof(tuningMpcQfBuffer));
			memcpy(tuningMpcDataControl, tuningMpcBufferControl, sizeof(tuningMpcBufferControl));
			memcpy(tuningPidData, tuningPidBuffer, sizeof(tuningPidBuffer));
			//memcpy(manualThrustData, manualThrustBuffer, sizeof(manualThrustBuffer));
			//memcpy(positionsData, positionsBuffer, sizeof(positionsBuffer));
			//memcpy(positions_timeoutData, positions_timeoutBuffer, sizeof(positions_timeoutBuffer));

			memcpy(keyboardData, keyboardDataBuffer, sizeof(keyboardDataBuffer));
			memcpy(tuningEkfData, tuningEkfBuffer, sizeof(tuningEkfBuffer));
			timerPrint=(int)keyboardData[5];
			memcpy(positionsData, positionsBuffer, sizeof(positionsBuffer));
			memcpy(positions_timeoutData, positions_timeoutBuffer, sizeof(positions_timeoutBuffer));
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
				//printf("Sensor pipe from Communication Read: tsAverage %lf tsTrue %lf\n", tsAverage, tsTrue);
			}
			tsAverageCounter=0;
			tsAverageAccum=0;
			
		}
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
	double accRaw[3]={0,0,0}, gyrRaw[3]={0,0,0}, magRaw[3]={0,0,0}, mag0[3]={0.0}, magRawRot[3]={0}, tempRaw=0, euler[3]={0.0}, magCal[3*CALIBRATION];
	double Lmag[1]={1}, normMag=0, Rmag[9]={0.0}, sensorDataBuffer[41]={0};
	double Pmag[16]={1.0f,0.0f,0.0f,0.0f,0.0f,1.0f,0.0f,0.0f,0.0f,0.0f,1.0f,0.0f,0.0f,0.0f,0.0f,1.0f};
	double posRaw[3]={0,0,0}, posRawPrev[3]={0,0,0}, stateDataBuffer[19]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double tsTrue=tsSensorsFusion;
	int  calibrationCounterEKF=0, posRawOldFlag=0, enableMPU9250Flag=-1, enableAK8963Flag=-1, calibrationCounterM0=0, calibrationCounterP=0;
	double positionsBuffer[9];
	double positions_timeoutBuffer[3];
	
	double headingMean[1]={0.0}, heading[1]={0.0}, heading_pred, headingMem[1000]={0.0}, headingAvg=0;
	double headingGyro=0, gyroAvg=0, gyroMem=0;
	double magRawRot_flat[3]={0.0};
	float q_mwk_AHRS[4] = {1,0,0,0}, q_mwk_UPDATE[4] = {1,0,0,0};
	double q_double[4] = {1,0,0,0};
	
	
	// Save to file buffer variable
	double data_tick=0;
	double ts_sensor_t=0;
	double ts_controller_t=0;
	
	double buffer_acc_x[BUFFER];
	double buffer_acc_y[BUFFER];
	double buffer_acc_z[BUFFER];
	double buffer_gyr_x[BUFFER];
	double buffer_gyr_y[BUFFER];
	double buffer_gyr_z[BUFFER];
	double buffer_mag_x[BUFFER];
	double buffer_mag_y[BUFFER];
	double buffer_mag_z[BUFFER];
	
	double buffer_angle_x[BUFFER];
	double buffer_angle_y[BUFFER];
	double buffer_angle_z[BUFFER];
	
	double buffer_pos_x[BUFFER];
	double buffer_pos_y[BUFFER];
	double buffer_pos_z[BUFFER];
	
	double buffer_omega_x[BUFFER];
	double buffer_omega_y[BUFFER];
	double buffer_omega_z[BUFFER];
	
	double buffer_ekf_x[BUFFER];
	double buffer_ekf_y[BUFFER];
	double buffer_ekf_z[BUFFER];
	double buffer_ekf_vx[BUFFER];
	double buffer_ekf_vy[BUFFER];
	double buffer_ekf_vz[BUFFER];
	double buffer_ekf_phi[BUFFER];
	double buffer_ekf_theta[BUFFER];
	double buffer_ekf_psi[BUFFER];
	double buffer_ekf_omega_phi[BUFFER];
	double buffer_ekf_omega_theta[BUFFER];
	double buffer_ekf_omega_psi[BUFFER];
	double buffer_ekf_dist_x[BUFFER];
	double buffer_ekf_dist_y[BUFFER];
	double buffer_ekf_dist_z[BUFFER];
	
	double buffer_mpc_alt_G[BUFFER];
	double buffer_mpc_alt_thrust[BUFFER];
	double buffer_mpc_pos_theta[BUFFER];
	double buffer_mpc_pos_phi[BUFFER];
	double buffer_mpc_pos_theta_comp[BUFFER];
	double buffer_mpc_pos_phi_comp[BUFFER];
	double buffer_mpc_att_tau_x_int[BUFFER];
	double buffer_mpc_att_tau_y_int[BUFFER];
	double buffer_mpc_att_tau_x[BUFFER];
	double buffer_mpc_att_tau_y[BUFFER];
	double buffer_mpc_att_tau_z[BUFFER];
	
	double buffer_ts_sensor[BUFFER];
	double buffer_ts_controller[BUFFER];
	
	double buffer_ref_x[BUFFER];
	double buffer_ref_y[BUFFER];
	
	//double buffer_u1[BUFFER];
	//double buffer_u2[BUFFER];
	//double buffer_u3[BUFFER];
	//double buffer_u4[BUFFER];

	int buffer_counter=0;
	//FILE *fpWrite;
	
	// EKF variables
	//double par_att[4]; // par_att[0]=phi, par_att[1]=theta, par_att[2]=omega_y, par_att[3]=omega_z 
	
	//double Pekf9x9_bias[81]={1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1};
	double Pekf6x6[36]={1e-5,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,1e-5};
	double Pekf7x7[49]={1e-5,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,1e-5};
	//double Pekf9x9[81]={1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1};
	//double Pekf8x8[64]={1e-5,0,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,0,1e-5};
	//double xhat9x9_bias[9]={0,0,0,0,0,0,0,0,0};
	double xhat6x6[6]={0,0,0,0,0,0};
	double xhat7x7[7]={0,0,0,0,0,0,-par_g};
	//double xhat9x9[9]={0,0,0,0,0,0,0,0,-par_g};
	//double xhat8x8[8]={0,0,0,0,0,0,0,-par_g};
	double uControl[4]={.1,.1,.1,.1};
	
	//double Pekf9x9_biasInit[81]={1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1};
	double Pekf6x6Init[36]={1e-5,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,1e-5};
	double Pekf7x7Init[49]={1e-5,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,1e-5};
	//double Pekf9x9Init[81]={1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1};
	//double Pekf8x8Init[64]={1e-5,0,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,0,1e-5,0,0,0,0,0,0,0,0,1e-5};
	//double xhat9x9_biasInit[9]={0,0,0,0,0,0,0,0,0};
	double xhat6x6Init[6]={0,0,0,0,0,0};
	double xhat7x7Init[7]={0,0,0,0,0,0,-par_g};
	//double xhat9x9Init[9]={0,0,0,0,0,0,0,0,-par_g};
	//double xhat8x8Init[8]={0,0,0,0,0,0,0,-par_g};
	double uControlInit[4]={.1,.1,.1,.1};
	double uControlThrustTorques[15]={0};
	
	//double Rekf9x9_bias[36]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double Rekf6x6[36]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double Rekf7x7[9]={0,0,0,0,0,0,0,0,0};
	//double Rekf9x9[9]={0,0,0,0,0,0,0,0,0};
	//double Rekf8x8[9]={0,0,0,0,0,0,0,0,0};
	//double ymeas9x9_bias[6]; // vector for 9x9 EKF attitude - measurement: angles and gyro
	double ymeas6x6[6];
	double ymeas7x7[3]; 
	//double ymeas9x9[3]; // vector for 9x9 EKF position - measurement: position
	//double ymeas8x8[3]; // vector for 8x8 EKF position - measurement: position

	//double ekf09x9_bias[6]={0,0,0,0,0,0}, ekfCal9x9_bias[6*CALIBRATION];
	double ekf06x6[6]={0,0,0,0,0,0}, ekfCal6x6[6*CALIBRATION];
	double ekf07x7[3]={0,0,0}, ekfCal7x7[3*CALIBRATION];
	//double ekf09x9[3]={0,0,0}, ekfCal9x9[3*CALIBRATION];
	//double ekf08x8[3]={0,0,0}, ekfCal8x8[3*CALIBRATION];
	
	//double tuningEkfBuffer9x9_bias[9]={ekf_Q_7,ekf_Q_8,ekf_Q_9,ekf_Q_10,ekf_Q_11,ekf_Q_12,ekf_Q_16,ekf_Q_17,ekf_Q_18}; //{phi, theta, psi, omega_x, omega_y, omega_z,bias_taux, bias_tauy,bias_tauz}
	double tuningEkfBuffer6x6[6]={ekf_Q_7,ekf_Q_8,ekf_Q_9,ekf_Q_10,ekf_Q_11,ekf_Q_12}; //{phi, theta, psi, omega_x, omega_y, omega_z,bias_taux, bias_tauy,bias_tauz}
	double tuningEkfBuffer7x7[7]={ekf_Q_1,ekf_Q_2,ekf_Q_3,ekf_Q_4,ekf_Q_5,ekf_Q_6,ekf_Q_15}; //{x, y, z, xdot, ydot, zdot, distz}
	//double tuningEkfBuffer9x9[9]={ekf_Q_1,ekf_Q_2,ekf_Q_3,ekf_Q_4,ekf_Q_5,ekf_Q_6,ekf_Q_13,ekf_Q_14,ekf_Q_15}; //{x, y, z, xdot, ydot, zdot, distx, disty, distz}
	//double tuningEkfBuffer8x8[8]={ekf_Q_1,ekf_Q_2,ekf_Q_3,ekf_Q_4,ekf_Q_5,ekf_Q_6,ekf_Q_13,ekf_Q_15}; //{x, y, z, xdot, ydot, zdot, yaw, distz}
	
	// Low Pass filter variables(20Hz and 30 for gyr)
	//double b_acc[25]={1.54791392878543e-06,2.07185488006612e-05,-5.19703972529415e-20,-0.000462086226027071,-0.00215334176418159,-0.00523098435850049,-0.00693485312903388,2.82650148861993e-18,0.0249007529841827,0.0717988692685668,0.131922751556644,0.183873196192904,0.204526858025432,0.183873196192904,0.131922751556644,0.0717988692685668,0.0249007529841827,2.82650148861993e-18,-0.00693485312903388,-0.0052309843585005,-0.00215334176418159,-0.000462086226027072,-5.1970397252942e-20,2.07185488006612e-05,1.54791392878543e-06};
	//double b_gyr[25]={-1.51379393190905e-06,-2.78880561210566e-05,7.62372544145295e-20,0.000621987897327971,0.00210587658352694,0.00166218520266596,-0.00678199116255076,-0.0225713907176518,-0.0243518764618903,0.0228146386774076,0.129014835433318,0.247501215646055,0.300027841503688,0.247501215646055,0.129014835433318,0.0228146386774076,-0.0243518764618903,-0.0225713907176518,-0.00678199116255077,0.00166218520266596,0.00210587658352694,0.000621987897327972,7.62372544145302e-20,-2.78880561210566e-05,-1.51379393190905e-06};
	
	// 20Hz cut-off
	double b_gyr[25]={1.54791392878543e-06,2.07185488006612e-05,-5.19703972529415e-20,-0.000462086226027071,-0.00215334176418159,-0.00523098435850049,-0.00693485312903388,2.82650148861993e-18,0.0249007529841827,0.0717988692685668,0.131922751556644,0.183873196192904,0.204526858025432,0.183873196192904,0.131922751556644,0.0717988692685668,0.0249007529841827,2.82650148861993e-18,-0.00693485312903388,-0.0052309843585005,-0.00215334176418159,-0.000462086226027072,-5.1970397252942e-20,2.07185488006612e-05,1.54791392878543e-06};

	//5hz b values
	//double b_acc[25]={3.67701752843937e-06,8.27007534721863e-05,0.000504038930132126,0.0018444766295764,0.00511519100925013,0.0116414444963577,0.0226738421555306,0.0387678986038862,0.0591509020569521,0.0814154703145087,0.101822705283757,0.116246817848376,0.121461669801344,0.116246817848376,0.101822705283757,0.0814154703145088,0.0591509020569522,0.0387678986038862,0.0226738421555306,0.0116414444963577,0.00511519100925013,0.0018444766295764,0.00050403893013213,8.27007534721863e-05,3.67701752843937e-06};
	//double b_gyr[25]={3.67701752843937e-06,8.27007534721863e-05,0.000504038930132126,0.0018444766295764,0.00511519100925013,0.0116414444963577,0.0226738421555306,0.0387678986038862,0.0591509020569521,0.0814154703145087,0.101822705283757,0.116246817848376,0.121461669801344,0.116246817848376,0.101822705283757,0.0814154703145088,0.0591509020569522,0.0387678986038862,0.0226738421555306,0.0116414444963577,0.00511519100925013,0.0018444766295764,0.00050403893013213,8.27007534721863e-05,3.67701752843937e-06};
	
	//double accRawMem[75]={0}; // memory buffer where elements 0-24=x-axis, 25-49=y-axis and 50-74=z-axis
	double gyrRawMem[75]={0};
	
	double tuningMpcBuffer[14];		//Q - 14 states
	double tuningMpcQfBuffer[9];		//Qf
	double tuningMpcBufferControl[6];	//R - 1 for alt, 2 for pos,  3 for att
	//double controllerBuffer[19]; // {PWM, thrust, torques} to be sent over to sensor.c
	double tuningPidBuffer[6];
	//double manualThrustBuffer[1]={manualThrust};
	//double positionsBuffer[9]; // positions of all directly from 'gps'
	//double positions_timeoutBuffer[3]; // timeout data
	//double positions_agents[6]; // position of other agents used for formation flying
	//double positions_self_prev[3]; // previous self position used in case positioning system has time out
	
	// Random variables
	//double L_temp;
	double alpha_mag=0.01;
	int counterCalEuler=0;
	double q_comp[4], q_comp2[4], q_init[4];
	float q[4]={1,0,0,0}; 
	int k=0;
	double euler_comp[3];
	double euler2[3]={0.0};
	double euler_mean[3]={0.0};
	int eulerCalFlag=0;
	int betaCalFlag=0;
	float beta_keyboard;
	int isnan_flag=0, outofbounds_flag=0;
	int triggerFly=0;
	double ref_x_keyboard=0;
	double ref_y_keyboard=0;

	// Keyboard control variables
	int timerPrint=0, ekfPrint=0, ekfReset=0, ekfPrint6States=0, sensorCalibrationRestart=0,saveDataTrigger=0,flag_save_weights=0;
	int outlierFlag[1], ioutlierFlagPercentage, outlierFlagMem[1000];
	int tSensorFusionCounter=0;
	
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
			//enableAK8963Flag=enableAK8963();
		pthread_mutex_unlock(&mutexI2CBusy);
		
		// Check that I2C sensors have been enabled
		if(enableMPU9250Flag==-1){
			printf("MPU9250 failed to be enabled\n");
		}
		//else if(enableAK8963Flag==-1){
			//printf("AK8963 failed to be enabled\n");
		//}
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
				//pthread_mutex_lock(&mutexPositionSensorData);	
					//memcpy(posRaw, sensorRawDataPosition, sizeof(sensorRawDataPosition));		
				//pthread_mutex_unlock(&mutexPositionSensorData);
				
				// Read latest control signal from globale variable to local variable
				pthread_mutex_lock(&mutexControlData);	
					memcpy(uControl, controlData, sizeof(uControl));		
					memcpy(uControlThrustTorques, controlData+4, sizeof(uControlThrustTorques));	
				pthread_mutex_unlock(&mutexControlData);
				
				// Get keyboard input data
				pthread_mutex_lock(&mutexKeyboardData);
					ref_x_keyboard=keyboardData[0];
					ref_y_keyboard=keyboardData[1];
					triggerFly=(int)keyboardData[3];
					timerPrint=(int)keyboardData[5];
					ekfPrint=(int)keyboardData[6];
					ekfReset=(int)keyboardData[7]; // key n
					ekfPrint6States=(int)keyboardData[8];
					sensorCalibrationRestart=(int)keyboardData[9];
					alpha_mag=keyboardData[11];
					beta_keyboard=keyboardData[12];
					saveDataTrigger=(int)keyboardData[15];
					 //memcpy(tuningEkfBuffer9x9_bias, tuningEkfData+6, sizeof(tuningEkfData)*6/18); // ekf states 7-12
					 //memcpy(tuningEkfBuffer9x9_bias+6, tuningEkfData+15, sizeof(tuningEkfData)*3/18); // ekf states 16-18
					memcpy(tuningEkfBuffer6x6, tuningEkfData+6, sizeof(tuningEkfData)*6/18); // ekf states 7-12
					memcpy(tuningEkfBuffer7x7, tuningEkfData, sizeof(tuningEkfData)*6/18); // ekf states 1-6
					memcpy(tuningEkfBuffer7x7+6, tuningEkfData+14, sizeof(tuningEkfData)*1/18); // ekf state 15
					//memcpy(tuningEkfBuffer9x9, tuningEkfData, sizeof(tuningEkfData)*6/18); // ekf states 1-6
					//memcpy(tuningEkfBuffer9x9+6, tuningEkfData+12, sizeof(tuningEkfData)*3/18); // ekf states 13-15
					//memcpy(tuningEkfBuffer8x8, tuningEkfData, sizeof(tuningEkfData)*6/18); // ekf states 1-6
					//memcpy(tuningEkfBuffer8x8+6, tuningEkfData+12, sizeof(tuningEkfData)*1/18); // ekf states 13-15
					//memcpy(tuningEkfBuffer8x8+7, tuningEkfData+14, sizeof(tuningEkfData)*1/18); // ekf states 13-15
					memcpy(positionsBuffer, positionsData, sizeof(positionsData));
					memcpy(positions_timeoutBuffer, positions_timeoutData, sizeof(positions_timeoutData));
					
					memcpy(tuningMpcBuffer, tuningMpcData, sizeof(tuningMpcData));
					memcpy(tuningMpcQfBuffer, tuningMpcQfData, sizeof(tuningMpcQfData));
					memcpy(tuningMpcBufferControl, tuningMpcDataControl, sizeof(tuningMpcDataControl));
					memcpy(tuningPidBuffer, tuningPidData, sizeof(tuningPidData));
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
				// Note: For more info check the MPU9250 Product Specification (Chapter 9)
				magRawRot[0]=magRaw[1];
				magRawRot[1]=magRaw[0];
				magRawRot[2]=magRaw[2];
				
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
				
				// Set gain of orientation estimation Madgwick beta and activate Low Pass filtering of raw accelerometer and gyroscope after sampling frequency has stabilized
				if(tsAverageReadyEKF==2){
					//if(saveDataTrigger){ // only save data when activated from keyboard
						////clock_gettime(CLOCK_MONOTONIC ,&t_start_buffer); /// start elapsed time clock for buffering procedure
						//// Saving data before low-pass filtering
						//if(buffer_counter==BUFFER){ // if buffer is full, save to file
							//saveData(buffer_acc_x,"acc_x",sizeof(buffer_acc_x)/sizeof(double));
							//saveData(buffer_acc_y,"acc_y",sizeof(buffer_acc_y)/sizeof(double));
							//saveData(buffer_acc_z,"acc_z",sizeof(buffer_acc_z)/sizeof(double));
							//saveData(buffer_gyr_x,"gyr_x",sizeof(buffer_gyr_x)/sizeof(double));
							//saveData(buffer_gyr_y,"gyr_y",sizeof(buffer_gyr_y)/sizeof(double));
							//saveData(buffer_gyr_z,"gyr_z",sizeof(buffer_gyr_z)/sizeof(double));
						//}
						//else{ // else keep saving data to buffer
							//buffer_acc_x[buffer_counter]=accRaw[0];
							//buffer_acc_y[buffer_counter]=accRaw[1];
							//buffer_acc_z[buffer_counter]=accRaw[2];
							//buffer_gyr_x[buffer_counter]=gyrRaw[0];
							//buffer_gyr_y[buffer_counter]=gyrRaw[1];
							//buffer_gyr_z[buffer_counter]=gyrRaw[2];

						//}
					//}
					//// Low Pass Filter using Blackman Harris window
					//// Order of 24 = 12 sample delay = 0.012s
					//// Cut-off frequencies: accelerometer = 20Hz and gyroscope = 30Hz 
					//// The delay is approx half of controller frequency
					//// Leaving enough time for Madgwick and EKF to converge after Low Pass filtering and before the controller need new fresh measurements
					// lowPassFilter(accRaw, gyrRaw, accRawMem, gyrRawMem, b_acc, b_gyr);
					// lowPassFilterGyroZ(gyrRaw, gyrRawMem, b_gyr);
					
					// Set gain of orientation estimation Madgwick beta after initial filter learn
					if(k==1000){
						beta=0.05;
						betaCalFlag=1;
						gyroAvg = gyroMem;
						gyroAvg /= 1000;
					}
					else{
						printf("SampleFre: %f Sample: %i\n", sampleFreq, k++);
						gyroMem += gyrRaw[2];
					}
						
					if(betaCalFlag==1){
						beta = beta_keyboard;
					}
						
					// Magnetometer outlier detection
					//outlierFlag=0;
					//normMag=sqrt(pow(magRawRot[0],2) + pow(magRawRot[1],2) + pow(magRawRot[2],2));
					//L_temp=(1-a)*L+a*normMag; // recursive magnetometer compensator
					//L=L_temp;
					//if ((normMag > L*1.05 || normMag < L*0.95) && eulerCalFlag==1){
						//magRawRot[0]=0.0f;
						//magRawRot[1]=0.0f;
						//magRawRot[2]=0.0f;
						//outlierFlag=1;
						//beta*=0.8;
						////printf("Mag outlier\n");
					//}
					
					//// outlier percentage
					//ioutlierFlagPercentage = 0;
					//for (int i=1; i<1000; i++) {
						//outlierFlagMem[i-1] = outlierFlagMem[i];
						//ioutlierFlagPercentage += outlierFlagMem[i-1];
					//}
					//outlierFlagMem[999] = outlierFlag;
					//ioutlierFlagPercentage += outlierFlagMem[999];
									
					// Orientation estimation with Madgwick filter
					//MadgwickAHRSupdate((float)gyrRaw[0], (float)gyrRaw[1], (float)gyrRaw[2], (float)accRaw[0], (float)accRaw[1], (float)accRaw[2], (float)magRawRot[0], (float)magRawRot[1], (float)magRawRot[2]);
					//if ( tsTrue < 10*tsSensorsFusion/NSEC_PER_SEC ) {
						MadgwickAHRSupdateIMU((float)gyrRaw[0], (float)gyrRaw[1], (float)gyrRaw[2]-(float)gyroAvg, (float)accRaw[0], (float)accRaw[1], (float)accRaw[2]);
						//MadgwickQuaternionUpdate((float)accRaw[0], (float)accRaw[1], (float)accRaw[2], (float)gyrRaw[0], (float)gyrRaw[1], (float)gyrRaw[2], (float)magRawRot[0], (float)magRawRot[1], (float)magRawRot[2], q, (float)tsTrue);	
					//}

					//q_comp[0]=(double)q[0];
					//q_comp[1]=-(double)q[1];
					//q_comp[2]=-(double)q[2];
					//q_comp[3]=-(double)q[3];
					
					q_comp[0]=q0;
					q_comp[1]=-q1;
					q_comp[2]=-q2;
					q_comp[3]=-q3;
					
					normMag=sqrt(pow(magRawRot[0],2) + pow(magRawRot[1],2) + pow(magRawRot[2],2));
					//magRawRot[0] /= normMag;
					//magRawRot[1] /= normMag;
					//magRawRot[2] /= normMag;
					
					/*
					//normMag=sqrt(pow(magRawRot[0],2) + pow(magRawRot[1],2) + pow(magRawRot[2],2));
					//// Calibration routine
					//if ( calibrationCounterM0 >  CALIBRATION ) {
						//// Measurement update of EKF with mag
						////double nom_mag[3]={0,sqrt(pow(mag0[0],2)+pow(mag0[1],2)),mag0[2]};
						////magnetometerUpdate(q_comp, Pmag, magRawRot, nom_mag, Rmag, Lmag, alpha_mag, outlierFlag);
						
						////// outlier percentage
						////ioutlierFlagPercentage = 0;
						////for (int i=1; i<1000; i++) {
							////outlierFlagMem[i-1] = outlierFlagMem[i];
							////ioutlierFlagPercentage += outlierFlagMem[i-1];
						////}
						////outlierFlagMem[999] = outlierFlag[0];
						////ioutlierFlagPercentage += outlierFlagMem[999];
						
						////// calibrationCounterP routine
						////if ( calibrationCounterP <= MAG_CALIBRATION ) {
							////calibrationCounterP++;
							////if ( calibrationCounterP == 1) {
								////printf("Eulers: % 1.3f, % 1.3f, % 1.3f | counter: %i\n", euler[0]*180/PI, euler[1]*180/PI, euler[2]*180/PI, calibrationCounterP);	
								////printf("Magnometer calibration STARTED\n");
							////}
							////else if ( calibrationCounterP == MAG_CALIBRATION ) {
								////printf("Eulers: % 1.3f, % 1.3f, % 1.3f | counter: %i\n", euler[0]*180/PI, euler[1]*180/PI, euler[2]*180/PI, calibrationCounterP);
								////printf("Pmag = \n");
								////printmat(Pmag,4,4);
								////printf("Magnometer calibration FINISHED\n5 seconds to leave it for alignment!\n");
							////}
							////else {
								////printf("Eulers: % 1.3f, % 1.3f, % 1.3f | counter: %i\n", euler[0]*180/PI, euler[1]*180/PI, euler[2]*180/PI, calibrationCounterP);	
							////}							
						////}
						////calibrationCounterP = MAG_CALIBRATION+1;
					//}
					//else {
						//if ( calibrationCounterM0 == 0 ){
							//printf("Calibration mag0 started\n");
							//sensorCalibration( Rmag, mag0, magCal, magRawRot, calibrationCounterM0 );
							////printf("calibrationCounterM0: %i\n", calibrationCounterM0);
							//calibrationCounterM0++;
						//}
						//else if( calibrationCounterM0 < CALIBRATION ){
							//sensorCalibration( Rmag, mag0, magCal, magRawRot, calibrationCounterM0 );
							////printf("calibrationCounterM0: %i\n", calibrationCounterM0);
							//calibrationCounterM0++;
						//}
						//else if( calibrationCounterM0 == CALIBRATION ){
							//sensorCalibration( Rmag, mag0, magCal, magRawRot, calibrationCounterM0 );
							//Lmag[0] = sqrt(pow(magRawRot[0],2) + pow(magRawRot[1],2) + pow(magRawRot[2],2));
							////// Save calibration in 'settings.txt' if it does not exist
							////saveSettings(Rmag,"Rmag",sizeof(Rmag)/sizeof(double));
							////saveSettings(mag0,"mag0",sizeof(mag0)/sizeof(double));
							////printf("calibrationCounterM0: %i\n", calibrationCounterM0);
							//printf("Calibration mag0 finished\n");
							//calibrationCounterM0++;
						//}
					//}									
					*/
					
					// Quaternions to eulers (rad)
					q2euler_zyx(euler,q_comp);
					
					//Allignment compensation for initial point of orientation angles
					////~ if ( eulerCalFlag != 1 && calibrationCounterP >  MAG_CALIBRATION ) {
					if ( betaCalFlag==1 && eulerCalFlag != 1 ) {
						if( counterCalEuler < 1000 ) {
							// Mean (bias) accelerometer, gyroscope and magnetometer
							if ( counterCalEuler == 0 ) { euler_mean[0]=0.0; euler_mean[1]=0.0; euler_mean[2]=0.0; printf("all euler_mean = %f\n", euler_mean[0]+euler_mean[1]+euler_mean[2]);}
							euler_mean[0]+=euler[0];
							euler_mean[1]+=euler[1];
							euler_mean[2]+=euler[2];												
							counterCalEuler++;
							//printf("euler_sum: %1.4f %1.4f %1.4f counter: %i\n", euler_mean[0]*180/PI, euler_mean[1]*180/PI, euler_mean[2]*180/PI, counterCalEuler);
						}
						else if(counterCalEuler==1000){
							euler_mean[0]/=1000.0f;
							euler_mean[1]/=1000.0f;
							euler_mean[2]/=1000.0f;
							counterCalEuler++;
							eulerCalFlag=1;
							printf("euler_mean: %1.4f %1.4f %1.4f counter: %i\n", euler_mean[0]*180/PI, euler_mean[1]*180/PI, euler_mean[2]*180/PI, counterCalEuler);
						}
					}
					euler_comp[0]=euler[0]-euler_mean[0];
					euler_comp[1]=euler[1]-euler_mean[1];
					euler_comp[2]=euler[2]-euler_mean[2];
					// euler_comp[0]=euler[0];
					// euler_comp[1]=euler[1];
					// euler_comp[2]=euler[2];
					
					//// Heading
					//// this is taking the euler_mean for now instead of euler_comp
					//magRawRot_flat[0] = magRawRot[0]*cos(euler_mean[1]) + magRawRot[2]*sin(euler_mean[0]);
					//magRawRot_flat[1] = magRawRot[0]*sin(euler_mean[2])*sin(euler_mean[1]) + magRawRot[1]*cos(euler_mean[2]) - magRawRot[2]*sin(euler_mean[2])*cos(euler_mean[1]);
					
					//heading[0] = atan2(magRawRot_flat[1],magRawRot_flat[0]) - headingMean[0];
					//heading_pred = heading[0];
					//// Moving average filter on heading
					//headingAvg = 0;
					//for (int i=1; i<1000; i++) {
						//headingMem[i-1] = headingMem[i];
						//headingAvg += headingMem[i-1];
					//}
					//headingMem[999] = heading_pred;
					//headingAvg += headingMem[999];
					//headingAvg /= 1000;
					//// Moving average of gyro bias
					//gyroAvg = 0;
					//for (int i=1; i<1000; i++) {
						//gyroMem[i-1] = gyroMem[i];
						//gyroAvg += gyroMem[i-1];
					//}
					//gyroMem[999] = gyrRaw[2];
					//gyroAvg += gyroMem[999];
					//gyroAvg /= 1000;
					//// Making heading using only gyro
					//headingGyro += tsTrue*gyrRaw[2];		
				}
				else{
					printf("SampleFre: %f\n", sampleFreq);
				}
						
				// Put position data in to posRaw
				switch (MYSELF){
					case AGENT1:
						memcpy(posRaw, positionsBuffer, sizeof(positionsBuffer)*3/9); // agents 1
						break;
					case AGENT2:
						memcpy(posRaw, positionsBuffer+3, sizeof(positionsBuffer)*3/9); // agent 2
						break;
					case AGENT3:
						memcpy(posRaw, positionsBuffer+6, sizeof(positionsBuffer)*3/9); // agents 3
						break;
				}
													
						
				// Check that positioning system is connected
				if (!positions_timeoutBuffer[0])
					beaconConnected=1;
				else
					beaconConnected=0;
					//beaconConnected=1;
						
				if(tsAverageReadyEKF==2 && eulerCalFlag==1){
					// Check if raw position data is new or old
					if(posRaw[0]==posRawPrev[0] && posRaw[1]==posRawPrev[1] && posRaw[2]==posRawPrev[2]){
						posRawOldFlag=1;
						//posRawOldFlag=0;
					}
					else{
						posRawOldFlag=0;
						memcpy(posRawPrev, posRaw, sizeof(posRaw));
					}	
					
					// Move data from (euler and posRaw) array to ymeas in to EKF's
					//ymeas9x9[0]=posRaw[0]; // position x
					//ymeas9x9[1]=posRaw[1]; // position y
					//ymeas9x9[2]=posRaw[2]; // position z
					ymeas7x7[0]=posRaw[0]; // position x
					ymeas7x7[1]=posRaw[1]; // position y
					ymeas7x7[2]=posRaw[2]; // position z
					
					//ymeas8x8[0]=posRaw[0]; // position x
					//ymeas8x8[1]=posRaw[1]; // position y
					//ymeas8x8[2]=posRaw[2]; // position z
					
					ymeas6x6[0]=euler_comp[2]; // phi (x-axis)
					ymeas6x6[1]=euler_comp[1]; // theta (y-axis)
					ymeas6x6[2]=euler_comp[0]; // psi (z-axis)
					//ymeas6x6[0]=0; // phi (x-axis)
					//ymeas6x6[1]=0; // theta (y-axis)
					//ymeas6x6[2]=0; // psi (z-axis)
					ymeas6x6[3]=gyrRaw[0]; // gyro x
					ymeas6x6[4]=gyrRaw[1]; // gyro y
					ymeas6x6[5]=gyrRaw[2]; // gyro z

					// Calibration routine for EKF
					if (calibrationCounterEKF==0 && beaconConnected && !posRawOldFlag){
						printf("EKF Calibration started\n");
						//ekfCalibration9x9_bias(Rekf9x9_bias, ekf09x9_bias, ekfCal9x9_bias, ymeas9x9_bias, calibrationCounterEKF);
						ekfCalibration6x6(Rekf6x6, ekf06x6, ekfCal6x6, ymeas6x6, calibrationCounterEKF);
						ekfCalibration7x7(Rekf7x7, ekf07x7, ekfCal7x7, ymeas7x7, calibrationCounterEKF);
						//ekfCalibration9x9(Rekf9x9, ekf09x9, ekfCal9x9, ymeas9x9, calibrationCounterEKF);
						//ekfCalibration9x9(Rekf8x8, ekf08x8, ekfCal8x8, ymeas8x8, calibrationCounterEKF);
						//printf("calibrationCounterEKF\n: %i", calibrationCounterEKF);
						printf("EKF calibration counter: %i\n", calibrationCounterEKF++);
					}
					else if(calibrationCounterEKF<CALIBRATION && beaconConnected && !posRawOldFlag){
						 //ekfCalibration9x9_bias(Rekf9x9_bias, ekf09x9_bias, ekfCal9x9_bias, ymeas9x9_bias, calibrationCounterEKF);
						ekfCalibration6x6(Rekf6x6, ekf06x6, ekfCal6x6, ymeas6x6, calibrationCounterEKF);
						ekfCalibration7x7(Rekf7x7, ekf07x7, ekfCal7x7, ymeas7x7, calibrationCounterEKF);
						//ekfCalibration9x9(Rekf9x9, ekf09x9, ekfCal9x9, ymeas9x9, calibrationCounterEKF);
						//ekfCalibration9x9(Rekf8x8, ekf08x8, ekfCal8x8, ymeas8x8, calibrationCounterEKF);
						//printf("calibrationCounterEKF\n: %i", calibrationCounterEKF);
						printf("EKF calibration counter: %i\n", calibrationCounterEKF++);
						
					}
					else if(calibrationCounterEKF==CALIBRATION && beaconConnected && !posRawOldFlag){
						//ekfCalibration9x9_bias(Rekf9x9_bias, ekf09x9_bias, ekfCal9x9_bias, ymeas9x9_bias, calibrationCounterEKF);
						ekfCalibration6x6(Rekf6x6, ekf06x6, ekfCal6x6, ymeas6x6, calibrationCounterEKF);
						ekfCalibration7x7(Rekf7x7, ekf07x7, ekfCal7x7, ymeas7x7, calibrationCounterEKF);
						//ekfCalibration9x9(Rekf9x9, ekf09x9, ekfCal9x9, ymeas9x9, calibrationCounterEKF);
						//ekfCalibration9x9(Rekf8x8, ekf08x8, ekfCal8x8, ymeas8x8, calibrationCounterEKF);
						
						//// Save calibration in 'settings.txt' if it does not exist
						////saveSettings(Rekf,"Rekf",sizeof(Rekf)/sizeof(double), &fpWrite);
						////saveSettings(ekf0,"ekf0",sizeof(ekf0)/sizeof(double), &fpWrite);
						 ////saveSettings(Rekf9x9_bias,"Rekf9x9_bias",sizeof(Rekf9x9_bias)/sizeof(double));
						//saveSettings(Rekf6x6,"Rekf6x6",sizeof(Rekf6x6)/sizeof(double));
						//saveSettings(Rekf9x9,"Rekf9x9",sizeof(Rekf9x9)/sizeof(double));
						////saveSettings(ekf09x9_bias,"ekf09x9_bias",sizeof(ekf09x9_bias)/sizeof(double));
						//saveSettings(ekf06x6,"ekf06x6",sizeof(ekf06x6)/sizeof(double));
						//saveSettings(ekf09x9,"ekf09x9",sizeof(ekf09x9)/sizeof(double));
							
						//printf("calibrationCounterEKF: %i\n", calibrationCounterEKF);
						printf("EKF Calibrations finish\n");
						calibrationCounterEKF++;
						
						// Initialize EKF with current available measurement
						//printf("Initialize EKF xhat with current measurments for position and orientation");
						
						// Attitude states initial measurments
						xhat6x6[0]=0;
						xhat6x6[1]=0;
						xhat6x6[2]=0;
						xhat6x6[3]=0;
						xhat6x6[4]=0;
						xhat6x6[5]=0;
						
						// Position states initial measurments
						//xhat9x9[0]=ymeas9x9[0];
						//xhat9x9[1]=ymeas9x9[1];
						//xhat9x9[2]=ymeas9x9[2];
						xhat7x7[0]=ymeas7x7[0];
						xhat7x7[1]=ymeas7x7[1];
						xhat7x7[2]=ymeas7x7[2];
						
						//xhat8x8[0]=ymeas8x8[0];
						//xhat8x8[1]=ymeas8x8[1];
						//xhat8x8[2]=ymeas8x8[2];
						
						// Quaternions initial measurments
						//memcpy(q_init, q, sizeof(q));
						q_init[0]=q0;
						q_init[1]=q1;
						q_init[2]=q2;
						q_init[3]=q3;
					}
					// State Estimator
					else if(calibrationCounterEKF>CALIBRATION){
						//printf("idiot\n");
						// Run EKF as long as ekfReset keyboard input is false
						if(!ekfReset){
							// Run Extended Kalman Filter (state estimator) using position and orientation data
							EKF_6x6(Pekf6x6,xhat6x6,uControl,ymeas6x6,tuningEkfBuffer6x6,Rekf6x6,tsTrue); // Attitude state estimation
							
							// par_att[0]=phi, par_att[1]=theta, par_att[2]=omega_y, par_att[3]=omega_z
							//par_att[0]=xhat6x6[0];
							//par_att[1]=xhat6x6[1];
							//par_att[2]=xhat6x6[2];
							//par_att[3]=xhat6x6[5]; 
							//EKF_8x8(Pekf8x8,xhat8x8,uControl,ymeas8x8,tuningEkfBuffer8x8,Rekf8x8,tsTrue,posRawOldFlag,par_att); // Position state estimation
							
							//EKF_9x9(Pekf9x9,xhat9x9,uCotrol,ymeas9x9,tuningEkfBuffer9x9,Rekf9x9,tsTrue,posRawOldFlag,xhat6x6); // Position state estimation
							//printf("EKF 7x7 Q: %f %f %f %f %f %f %f\n", tuningEkfBuffer7x7[0], tuningEkfBuffer7x7[1], tuningEkfBuffer7x7[2], tuningEkfBuffer7x7[3], tuningEkfBuffer7x7[4], tuningEkfBuffer7x7[5], tuningEkfBuffer7x7[6]);
							EKF_7x7(Pekf7x7,xhat7x7,uControl,ymeas7x7,tuningEkfBuffer7x7,Rekf7x7,tsTrue,posRawOldFlag,xhat6x6); // Position state estimation
							stateDataBuffer[15]=1; // ready flag for MPC to start using the initial conditions given by EKF.
							
							if(!triggerFly){ // forcing it estimation of disturbance g not go off when not flying
								xhat7x7[6]=-par_g;
								//xhat9x9[6]=0;
								//xhat9x9[7]=0;
								xhat7x7[2]=0;
								xhat7x7[5]=0;
								//xhat8x8[7]=-par_g;
								//xhat8x8[2]=0;
								//xhat8x8[5]=0;
								flag_save_weights=0;
							}
							else{
								if(flag_save_weights==0){						
									//// Save buffered data to file
									if(saveDataTrigger){ // only save data when activated from keyboard
										saveData(tuningMpcBuffer,"mpc_Q",sizeof(tuningMpcBuffer)/sizeof(double));
										saveData(tuningMpcQfBuffer,"mpc_Qf",sizeof(tuningMpcQfBuffer)/sizeof(double));
										saveData(tuningMpcBufferControl,"mpc_R",sizeof(tuningMpcBufferControl)/sizeof(double));
										saveData(tuningPidBuffer,"pid_k",sizeof(tuningPidBuffer)/sizeof(double));
									}
									flag_save_weights=1;
								}
							}
							
						}
						// Reset EKF with initial Phat, xhat and uControl as long as ekfReset keyboard input is true
						else{
							//memcpy(Pekf9x9_bias, Pekf9x9_biasInit, sizeof(Pekf9x9_biasInit));
							//memcpy(Pekf6x6, Pekf6x6, sizeof(Pekf6x6Init));	
							//memcpy(Pekf7x7, Pekf7x7Init, sizeof(Pekf7x7Init));	
							//memcpy(Pekf9x9, Pekf9x9Init, sizeof(Pekf9x9Init));	
							//memcpy(Pekf8x8, Pekf8x8Init, sizeof(Pekf8x8Init));	
							//memcpy(xhat9x9_bias, xhat9x9_biasInit, sizeof(xhat9x9_biasInit));
							memcpy(xhat6x6, xhat6x6Init, sizeof(xhat6x6Init));
							memcpy(xhat7x7, xhat7x7Init, sizeof(xhat7x7Init));
							//memcpy(xhat9x9, xhat9x9Init, sizeof(xhat9x9Init));
							//memcpy(xhat8x8, xhat8x8Init, sizeof(xhat8x8Init));
							memcpy(uControl, uControlInit, sizeof(uControlInit));
							
							//memcpy(q, q_init, sizeof(q_init));
							
							q0=q_init[0];
							q1=q_init[1];
							q2=q_init[2];
							q3=q_init[3];
							
							stateDataBuffer[15]=0; // set ready flag for MPC false during reset
							isnan_flag=0;
							outofbounds_flag=0;
						}
						
						// Override disturbance estimation in x and y direction
						//xhat9x9[6]=0;
						//xhat9x9[7]=0;
						
						// Torque disturbance saturation
						//saturation(xhat9x9_bias,6,-0.01,0.01);
						//saturation(xhat9x9_bias,7,-0.01,0.01);
						// saturation(xhat9x9_bias,6,0.0,0.0);
						//saturation(xhat9x9,6,-0.25,0.25);
						//saturation(xhat9x9,7,-0.25,0.25);
						//saturation(xhat9x9,8,-12,-6);
						saturation(xhat7x7,6,-12,-6);
				
						//Check for EKF6x6 failure (isnan)
						 for (int j=0;j<6;j++){
							 if (isnan(xhat6x6[j])!=0){
								 isnan_flag=1;
								 break;
							 }						
						 }
						
						// Check for EKF7x7 failure (isnan)
						for (int j=0;j<7;j++){
							if (isnan(xhat7x7[j])!=0){
								isnan_flag=1;
								break;
							}						
						}

						// Check for EKF6x6 failure (out of bounds)
						for (int j=0;j<6;j++){
							if (xhat6x6[j]>1e6){
								 outofbounds_flag=1;
								 break;
							}						
						}
						
						// Check for EKF7x7 failure (out of bounds)
						for (int j=0;j<7;j++){
							if (xhat7x7[j]>1e6){
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
						
						//double yaw_bias;
						//yaw_bias=xhat8x8[6]-xhat6x6[2];

						 //Move over data to controller.c via pipe
						//stateDataBuffer[0]=xhat9x9[0]; // position x
						//stateDataBuffer[1]=xhat9x9[1]; // position y
						//stateDataBuffer[2]=xhat9x9[2]; // position z
						//stateDataBuffer[3]=xhat9x9[3]; // velocity x
						//stateDataBuffer[4]=xhat9x9[4]; // velocity y
						//stateDataBuffer[5]=xhat9x9[5]; // velocity z
						
						stateDataBuffer[0]=xhat7x7[0]; // position x
						stateDataBuffer[1]=xhat7x7[1]; // position y
						stateDataBuffer[2]=xhat7x7[2]; // position z
						stateDataBuffer[3]=xhat7x7[3]; // velocity x
						stateDataBuffer[4]=xhat7x7[4]; // velocity y
						stateDataBuffer[5]=xhat7x7[5]; // velocity z
						
						//stateDataBuffer[0]=xhat8x8[0]; // position x
						//stateDataBuffer[1]=xhat8x8[1]; // position y
						//stateDataBuffer[2]=xhat8x8[2]; // position z
						//stateDataBuffer[3]=xhat8x8[3]; // velocity x
						//stateDataBuffer[4]=xhat8x8[4]; // velocity y
						//stateDataBuffer[5]=xhat8x8[5]; // velocity z
						
						stateDataBuffer[6]=xhat6x6[0]; // phi (x-axis)
						stateDataBuffer[7]=xhat6x6[1]; // theta (y-axis)
						stateDataBuffer[8]=xhat6x6[2]; // psi (z-axis)
						stateDataBuffer[9]=xhat6x6[3]; // omega x (gyro)
						stateDataBuffer[10]=xhat6x6[4]; // omega y (gyro)
						stateDataBuffer[11]=xhat6x6[5]; // omega z (gyro)
						
						//stateDataBuffer[12]=xhat9x9[6]; // disturbance x
						//stateDataBuffer[13]=xhat9x9[7]; // disturbance y
						//stateDataBuffer[14]=xhat9x9[8]; // disturbance z
						stateDataBuffer[14]=xhat7x7[6]; // disturbance z
						
						//stateDataBuffer[14]=xhat8x8[7]; // disturbance z
						
						//stateDataBuffer[16]=0; // bias taux
						//stateDataBuffer[17]=0; // bias tauy
						//stateDataBuffer[18]=0; // bias tauz

						tSensorFusionCounter++;
						
						if(ekfPrint && tSensorFusionCounter % 10 == 0){
							//double norm_mag = 1/sqrt(magRawRot[0] * magRawRot[0] + magRawRot[1] * magRawRot[1] + magRawRot[2] * magRawRot[2]);
							printf("xhat: (pos) % 1.4f % 1.4f % 1.4f (vel) % 1.4f % 1.4f % 1.4f (dist_z) % 1.4f (ang_e) % 2.4f % 2.4f % 2.4f (omeg_e) % 2.4f % 2.4f % 2.4f ref: % 2.1f % 2.1f\n",xhat7x7[0],xhat7x7[1],xhat7x7[2],xhat7x7[3],xhat7x7[4],xhat7x7[5],xhat7x7[6],xhat6x6[0]*(180/PI),xhat6x6[1]*(180/PI),xhat6x6[2]*(180/PI),xhat6x6[3]*(180/PI),xhat6x6[4]*(180/PI),xhat6x6[5]*(180/PI),ref_x_keyboard,ref_y_keyboard);
							//printf("(mag) % 1.4f % 1.4f % 1.4f (atan2(y/x)) % 1.4f\n", magRawRot[0]*norm_mag, magRawRot[1]*norm_mag, magRawRot[2]*norm_mag, atan2(magRawRot[1]*norm_mag, magRawRot[0]*norm_mag)*(180/PI));
						}
						
						if(ekfPrint6States && tSensorFusionCounter % 10 == 0){
							//printf("xhat: % 1.4f % 1.4f % 1.4f % 2.4f % 2.4f % 2.4f (euler_meas) % 2.4f % 2.4f % 2.4f (gyr_meas) % 2.4f % 2.4f % 2.4f (outlier) %i %i (freq) %3.5f u: %3.4f %3.4f %3.4f %3.4f\n",xhat9x9[0],xhat9x9[1],xhat9x9[2],xhat9x9_bias[0]*(180/PI),xhat9x9_bias[1]*(180/PI),xhat9x9_bias[2]*(180/PI), ymeas9x9_bias[0]*(180/PI),ymeas9x9_bias[1]*(180/PI),ymeas9x9_bias[2]*(180/PI), gyrRaw[0], gyrRaw[1], gyrRaw[2], outlierFlag, outlierFlagPercentage, sampleFreq, uControl[0], uControl[1], uControl[2], uControl[3]);
							//printf("(ang(xhat)) % 2.4f % 2.4f % 2.4f (pos(xhat)) % 2.4f % 2.4f % 2.4f (pwm) % 3.4f % 3.4f % 3.4f % 3.4f (thrust) % 1.3f (torque) % 1.5f % 1.5f % 1.5f \n",xhat6x6[0]*(180/PI),xhat6x6[1]*(180/PI),xhat6x6[2]*(180/PI), xhat8x8[0],xhat8x8[1],xhat8x8[2], uControl[0], uControl[1], uControl[2], uControl[3], uControlThrustTorques[0], uControlThrustTorques[1], uControlThrustTorques[2], uControlThrustTorques[3]);
							//printf("(P8x8) % 1.6f % 1.6f % 1.6f % 1.6f % 1.6f % 1.6f % 1.6f % 1.6f (yaw est) % 1.4f (yaw drift) % 1.4f (yaw comp) % 1.4f\n", Pekf8x8[0], Pekf8x8[9], Pekf8x8[18], Pekf8x8[27], Pekf8x8[36], Pekf8x8[45], Pekf8x8[54], Pekf8x8[63], xhat8x8[6]*(180/PI), xhat6x6[2]*(180/PI), stateDataBuffer[8]*(180/PI));
							
							//printf("(ang(m)) % 2.4f % 2.4f % 2.4f (ang(xhat)) % 2.4f % 2.4f % 2.4f (OLP) %i %i (L) %f (normMag) %f (rawMag) %f %f %f (omeg(m)) % 2.4f % 2.4f % 2.4f (omeg(xhat)) % 2.4f % 2.4f % 2.4f \n",ymeas6x6[0]*(180/PI),ymeas6x6[1]*(180/PI),ymeas6x6[2]*(180/PI), 	xhat6x6[0]*(180/PI),xhat6x6[1]*(180/PI),xhat6x6[2]*(180/PI), 	ioutlierFlagPercentage, outlierFlag[0],		Lmag[0],normMag,	magRawRot[0],magRawRot[1],magRawRot[2],		ymeas6x6[3]*(180/PI),ymeas6x6[4]*(180/PI),ymeas6x6[5]*(180/PI), 	xhat6x6[3]*(180/PI),xhat6x6[4]*(180/PI),xhat6x6[5]*(180/PI));
							//printf("(ang(conj)) % 2.4f % 2.4f % 2.4f (ang) % 2.4f % 2.4f % 2.4f \n",euler[2]*(180/PI),euler[1]*(180/PI),euler[0]*(180/PI),euler2[2]*(180/PI),euler2[1]*(180/PI),euler2[0]*(180/PI));
							
							printf("(ang(m)) % 2.4f % 2.4f % 2.4f (ang(xhat)) % 2.4f % 2.4f % 2.4f (omeg(m)) % 2.4f % 2.4f % 2.4f (omeg(xhat)) % 2.4f % 2.4f % 2.4f (pwm) % 3.4f % 3.4f % 3.4f % 3.4f (thrust) % 1.3f (torque) % 1.5f % 1.5f % 1.5f \n",ymeas6x6[0]*(180/PI),ymeas6x6[1]*(180/PI),ymeas6x6[2]*(180/PI), xhat6x6[0]*(180/PI),xhat6x6[1]*(180/PI),xhat6x6[2]*(180/PI), ymeas6x6[3]*(180/PI),ymeas6x6[4]*(180/PI),ymeas6x6[5]*(180/PI), xhat6x6[3]*(180/PI),xhat6x6[4]*(180/PI),xhat6x6[5]*(180/PI), uControl[0], uControl[1], uControl[2], uControl[3], uControlThrustTorques[0], uControlThrustTorques[1], uControlThrustTorques[2], uControlThrustTorques[3]);
							//printf("(ang(m)) % 2.4f % 2.4f % 2.4f (hdngTrue,gyroAvg,hdngGyro) % 2.4f % 2.4f % 2.4f (OLP) %i %i (L) %2.4f (normMag) %3.2f (hding, hding_pred, predAvg) % 2.4f % 2.4f % 2.4f (rawMag) %f %f %f (omeg(m)) % 2.4f % 2.4f % 2.4f \n",ymeas6x6[0]*(180/PI),ymeas6x6[1]*(180/PI),ymeas6x6[2]*(180/PI), 	(ymeas6x6[2]-headingGyro)*(180/PI),gyroAvg*(180/PI),headingGyro*(180/PI), 	ioutlierFlagPercentage, outlierFlag[0],		Lmag[0],normMag,	heading[0]*180/PI,heading_pred*180/PI,headingAvg*180/PI,		magRawRot[0],magRawRot[1],magRawRot[2],		ymeas6x6[3]*(180/PI),ymeas6x6[4]*(180/PI),ymeas6x6[5]*(180/PI));
						}
	
						// Write to Controller process
						if (write(ptrPipe1->child[1], stateDataBuffer, sizeof(stateDataBuffer)) != sizeof(stateDataBuffer)) printf("pipe write error in Sensor to Controller\n");
						//else printf("Sensor ID: %d, Sent: %f to Controller\n", (int)getpid(), stateDataBuffer[15]);
						
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
						sensorDataBuffer[12]=posRaw[0];
						sensorDataBuffer[13]=posRaw[1];
						sensorDataBuffer[14]=posRaw[2];
						sensorDataBuffer[15]=stateDataBuffer[0]; // x
						sensorDataBuffer[16]=stateDataBuffer[1]; // y
						sensorDataBuffer[17]=stateDataBuffer[2]; // z
						sensorDataBuffer[18]=stateDataBuffer[3]; // vx
						sensorDataBuffer[19]=stateDataBuffer[4]; // vy
						sensorDataBuffer[20]=stateDataBuffer[5]; // vz
						sensorDataBuffer[21]=stateDataBuffer[6]; // phi
						sensorDataBuffer[22]=stateDataBuffer[7]; // theta
						sensorDataBuffer[23]=stateDataBuffer[8]; // psi
						sensorDataBuffer[24]=stateDataBuffer[9]; // omega_phi
						sensorDataBuffer[25]=stateDataBuffer[10]; // omega_theta
						sensorDataBuffer[26]=stateDataBuffer[11]; // omega_psi
						sensorDataBuffer[27]=stateDataBuffer[14]; // tilde_dz
						sensorDataBuffer[28]=uControlThrustTorques[0]; // thrust
						sensorDataBuffer[29]=uControlThrustTorques[1]; // tau_phi with integrator
						sensorDataBuffer[30]=uControlThrustTorques[2]; // tau_theta with integrator
						sensorDataBuffer[31]=uControlThrustTorques[3]; // tau_z
						sensorDataBuffer[32]=uControlThrustTorques[4]; // G
						sensorDataBuffer[33]=uControlThrustTorques[5]; // mpcPos_U_theta
						sensorDataBuffer[34]=uControlThrustTorques[6]; // mpcPos_U_phi
						sensorDataBuffer[35]=uControlThrustTorques[7]; // mpcPos_U_theta_comp
						sensorDataBuffer[36]=uControlThrustTorques[8]; // mpcPos_U_phi_comp
						sensorDataBuffer[37]=uControlThrustTorques[9]; // attU_all_taux
						sensorDataBuffer[38]=uControlThrustTorques[10]; // attU_all_tauy
						sensorDataBuffer[39]=uControlThrustTorques[14]; // tsTrue controller
						//sensorDataBuffer[28]=uControlThrustTorques[12];
						//sensorDataBuffer[29]=uControlThrustTorques[13];
						//sensorDataBuffer[30]=uControlThrustTorques[14];
						sensorDataBuffer[40]=tsTrue; // tsTrue sensor
								
						// Write to Communication process
						if (write(ptrPipe2->parent[1], sensorDataBuffer, sizeof(sensorDataBuffer)) != sizeof(sensorDataBuffer)) printf("pipe write error in Sensor ot Communicaiont\n");
						//else printf("Sensor ID: %d, Sent: %f to Communication\n", (int)getpid(), sensorDataBuffer[0]);
																						
						
						// Restart sensor fusion and EKF calibration
						if(sensorCalibrationRestart){
							// calibrationCounter=0; // forces sensor fusion to restart calibration
							calibrationCounterEKF=0; // forces ekf to restart calibration
							calibrationCounterP=0;
							calibrationCounterM0=0;
							eulerCalFlag=0;
							counterCalEuler=0;
							printf("eulerCalFlag %i\n", eulerCalFlag);
						}
						
						//// Save buffered data to file
						if(saveDataTrigger){ // only save data when activated from keyboard
							//clock_gettime(CLOCK_MONOTONIC ,&t_start_buffer); /// start elapsed time clock for buffering procedure
							if(data_tick==4){
								data_tick=0;
								if(buffer_counter==BUFFER){ // if buffer is full, save to file
									saveData(buffer_acc_x,"acc_x",sizeof(buffer_acc_x)/sizeof(double));
									saveData(buffer_acc_y,"acc_y",sizeof(buffer_acc_y)/sizeof(double));
									saveData(buffer_acc_z,"acc_z",sizeof(buffer_acc_z)/sizeof(double));
									saveData(buffer_gyr_x,"gyr_x",sizeof(buffer_gyr_x)/sizeof(double));
									saveData(buffer_gyr_y,"gyr_y",sizeof(buffer_gyr_y)/sizeof(double));
									saveData(buffer_gyr_z,"gyr_z",sizeof(buffer_gyr_z)/sizeof(double));
									saveData(buffer_mag_x,"mag_x",sizeof(buffer_mag_x)/sizeof(double));
									saveData(buffer_mag_y,"mag_y",sizeof(buffer_mag_y)/sizeof(double));
									saveData(buffer_mag_z,"mag_z",sizeof(buffer_mag_z)/sizeof(double));	
									
									saveData(buffer_angle_x,"angle_x",sizeof(buffer_angle_x)/sizeof(double));
									saveData(buffer_angle_y,"angle_y",sizeof(buffer_angle_y)/sizeof(double));
									saveData(buffer_angle_z,"angle_z",sizeof(buffer_angle_z)/sizeof(double));
									saveData(buffer_pos_x,"pos_x",sizeof(buffer_pos_x)/sizeof(double));
									saveData(buffer_pos_y,"pos_y",sizeof(buffer_pos_y)/sizeof(double));
									saveData(buffer_pos_z,"pos_z",sizeof(buffer_pos_z)/sizeof(double));
									saveData(buffer_omega_x,"omega_x",sizeof(buffer_omega_x)/sizeof(double));
									saveData(buffer_omega_y,"omega_y",sizeof(buffer_omega_y)/sizeof(double));
									saveData(buffer_omega_z,"omega_z",sizeof(buffer_omega_z)/sizeof(double));
									
									saveData(buffer_ekf_x,"ekf_x",sizeof(buffer_ekf_x)/sizeof(double));
									saveData(buffer_ekf_y,"ekf_y",sizeof(buffer_ekf_y)/sizeof(double));
									saveData(buffer_ekf_z,"ekf_z",sizeof(buffer_ekf_z)/sizeof(double));
									saveData(buffer_ekf_vx,"ekf_vx",sizeof(buffer_ekf_vx)/sizeof(double));
									saveData(buffer_ekf_vy,"ekf_vy",sizeof(buffer_ekf_vy)/sizeof(double));
									saveData(buffer_ekf_vz,"ekf_vz",sizeof(buffer_ekf_vz)/sizeof(double));
									saveData(buffer_ekf_phi,"ekf_phi",sizeof(buffer_ekf_phi)/sizeof(double));
									saveData(buffer_ekf_theta,"ekf_theta",sizeof(buffer_ekf_theta)/sizeof(double));
									saveData(buffer_ekf_psi,"ekf_psi",sizeof(buffer_ekf_psi)/sizeof(double));
									
									saveData(buffer_ekf_omega_phi,"ekf_omega_phi",sizeof(buffer_ekf_omega_phi)/sizeof(double));
									saveData(buffer_ekf_omega_theta,"ekf_omega_theta",sizeof(buffer_ekf_omega_theta)/sizeof(double));
									saveData(buffer_ekf_omega_psi,"ekf_omega_psi",sizeof(buffer_ekf_omega_psi)/sizeof(double));
									saveData(buffer_ekf_dist_x,"ekf_dist_x",sizeof(buffer_ekf_dist_x)/sizeof(double));
									saveData(buffer_ekf_dist_y,"ekf_dist_y",sizeof(buffer_ekf_dist_y)/sizeof(double));
									saveData(buffer_ekf_dist_z,"ekf_dist_z",sizeof(buffer_ekf_dist_z)/sizeof(double));
									
									saveData(buffer_mpc_alt_G,"mpc_alt_G",sizeof(buffer_mpc_alt_G)/sizeof(double));
									saveData(buffer_mpc_alt_thrust,"mpc_alt_thrust",sizeof(buffer_mpc_alt_thrust)/sizeof(double));
									saveData(buffer_mpc_pos_theta,"mpc_pos_theta",sizeof(buffer_mpc_pos_theta)/sizeof(double));
									saveData(buffer_mpc_pos_phi,"mpc_pos_phi",sizeof(buffer_mpc_pos_phi)/sizeof(double));
									saveData(buffer_mpc_pos_theta_comp,"mpc_pos_theta_comp",sizeof(buffer_mpc_pos_theta_comp)/sizeof(double));
									saveData(buffer_mpc_pos_phi_comp,"mpc_pos_phi_comp",sizeof(buffer_mpc_pos_phi_comp)/sizeof(double));
									saveData(buffer_mpc_att_tau_x_int,"mpc_att_tau_x_int",sizeof(buffer_mpc_att_tau_x_int)/sizeof(double));
									saveData(buffer_mpc_att_tau_y_int,"mpc_att_tau_y_int",sizeof(buffer_mpc_att_tau_y_int)/sizeof(double));
									saveData(buffer_mpc_att_tau_x,"mpc_att_tau_x",sizeof(buffer_mpc_att_tau_x)/sizeof(double));
									saveData(buffer_mpc_att_tau_y,"mpc_att_tau_y",sizeof(buffer_mpc_att_tau_y)/sizeof(double));
									saveData(buffer_mpc_att_tau_z,"mpc_att_tau_z",sizeof(buffer_mpc_att_tau_z)/sizeof(double));
									saveData(buffer_ts_sensor,"ts_sensor",sizeof(buffer_ts_sensor)/sizeof(double));
									saveData(buffer_ts_controller,"ts_controller",sizeof(buffer_ts_controller)/sizeof(double));
									saveData(buffer_ref_x,"ref_x",sizeof(buffer_ref_x)/sizeof(double));
									saveData(buffer_ref_y,"ref_y",sizeof(buffer_ref_y)/sizeof(double));
									
									buffer_counter=0;
							}
								else{ // else keep saving data to buffer
									buffer_acc_x[buffer_counter]=accRaw[0];
									buffer_acc_y[buffer_counter]=accRaw[1];
									buffer_acc_z[buffer_counter]=accRaw[2];
									buffer_gyr_x[buffer_counter]=gyrRaw[0];
									buffer_gyr_y[buffer_counter]=gyrRaw[1];
									buffer_gyr_z[buffer_counter]=gyrRaw[2];
									buffer_mag_x[buffer_counter]=magRawRot[0];
									buffer_mag_y[buffer_counter]=magRawRot[1];
									buffer_mag_z[buffer_counter]=magRawRot[2];
									
									buffer_angle_x[buffer_counter]=euler_comp[0];
									buffer_angle_y[buffer_counter]=euler_comp[1];
									buffer_angle_z[buffer_counter]=euler_comp[2];
									buffer_pos_x[buffer_counter]=posRaw[0];
									buffer_pos_y[buffer_counter]=posRaw[1];
									buffer_pos_z[buffer_counter]=posRaw[2];
									//buffer_omega_x[buffer_counter],"omega_x",sizeof(buffer_omega_x)/sizeof(double));
									//buffer_omega_y[buffer_counter],"omega_y",sizeof(buffer_omega_y)/sizeof(double));
									//buffer_omega_z[buffer_counter],"omega_z",sizeof(buffer_omega_z)/sizeof(double));
									
									buffer_ekf_x[buffer_counter]=stateDataBuffer[0];
									buffer_ekf_y[buffer_counter]=stateDataBuffer[1];
									buffer_ekf_z[buffer_counter]=stateDataBuffer[2];
									buffer_ekf_vx[buffer_counter]=stateDataBuffer[3];
									buffer_ekf_vy[buffer_counter]=stateDataBuffer[4];
									buffer_ekf_vz[buffer_counter]=stateDataBuffer[5];
									buffer_ekf_phi[buffer_counter]=stateDataBuffer[6];
									buffer_ekf_theta[buffer_counter]=stateDataBuffer[7];
									buffer_ekf_psi[buffer_counter]=stateDataBuffer[8];
									
									buffer_ekf_omega_phi[buffer_counter]=stateDataBuffer[9];
									buffer_ekf_omega_theta[buffer_counter]=stateDataBuffer[10];
									buffer_ekf_omega_psi[buffer_counter]=stateDataBuffer[11];
									//buffer_ekf_dist_x[buffer_counter]=stateDataBuffer[12];
									//buffer_ekf_dist_y[buffer_counter]=stateDataBuffer[13];
									buffer_ekf_dist_z[buffer_counter]=stateDataBuffer[14];
										
									buffer_mpc_alt_G[buffer_counter]=uControlThrustTorques[4];
									buffer_mpc_alt_thrust[buffer_counter]=uControlThrustTorques[0];
									buffer_mpc_pos_theta[buffer_counter]=uControlThrustTorques[5];
									buffer_mpc_pos_phi[buffer_counter]=uControlThrustTorques[6];
									buffer_mpc_pos_theta_comp[buffer_counter]=uControlThrustTorques[7];
									buffer_mpc_pos_phi_comp[buffer_counter]=uControlThrustTorques[8];
									buffer_mpc_att_tau_x_int[buffer_counter]=uControlThrustTorques[1];
									buffer_mpc_att_tau_y_int[buffer_counter]=uControlThrustTorques[2];
									buffer_mpc_att_tau_x[buffer_counter]=uControlThrustTorques[9];
									buffer_mpc_att_tau_y[buffer_counter]=uControlThrustTorques[10];
									buffer_mpc_att_tau_z[buffer_counter]=uControlThrustTorques[3];
									buffer_ts_sensor[buffer_counter]=ts_sensor_t;
									buffer_ts_controller[buffer_counter]=ts_controller_t;
									buffer_ref_x[buffer_counter]=ref_x_keyboard;
									buffer_ref_y[buffer_counter]=ref_y_keyboard;
									buffer_counter++;
									ts_sensor_t=0;
									ts_controller_t=0;
							}
							}
							else{
								data_tick++;
								ts_sensor_t+=tsTrue;
								ts_controller_t+=uControlThrustTorques[14];
							}
						}
					}
				}
				/// Calculate next shot
				if ( calibrationCounterP == MAG_CALIBRATION ) t.tv_sec += 5;
				//else if ( calibrationCounterM0 == (CALIBRATION-1) ) t.tv_sec += 10;
				else t.tv_nsec += (int)tsSensorsFusion;
				
				//t.tv_nsec += (int)tsSensorsFusion;
				
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
	double pwmValueBuffer[19], tsTrue;

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
	int pwmRangeSetting=0; // switch for setting the PWM for tuning range of speed controllers
	
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
				pwmRangeSetting=(int)keyboardData[17];
			pthread_mutex_unlock(&mutexKeyboardData);
			
			// Read data from controller process
			if(read(ptrPipe->parent[0], pwmValueBuffer, sizeof(pwmValueBuffer)) == -1) printf("read error in sensor from controller\n");
			//printf("Data received: %f\n", pwmValueBuffer[0]);
			
			//printf("(pwm)  % 3.4f % 3.4f % 3.4f % 3.4f (thrust) % 1.4f (torque) % 1.4f % 1.4f % 1.4f\n", pwmValueBuffer[0], pwmValueBuffer[1], pwmValueBuffer[2], pwmValueBuffer[3], pwmValueBuffer[4], pwmValueBuffer[5], pwmValueBuffer[6], pwmValueBuffer[7]);
			
			// killPWM is linked to keyboard start flying switch. Forces PWM to zero if stop signal is given
			if(!killPWM){
				pwmValueBuffer[0]=0.0f;
				pwmValueBuffer[1]=0.0f;
				pwmValueBuffer[2]=0.0f;
				pwmValueBuffer[3]=0.0f;
			}
			
			if(!killPWM && pwmRangeSetting){
				pwmValueBuffer[0]=100.0f;
				pwmValueBuffer[1]=100.0f;
				pwmValueBuffer[2]=100.0f;
				pwmValueBuffer[3]=100.0f;
			}
				
			//else{
				//printf("(pwm)  % 3.4f % 3.4f % 3.4f % 3.4f\n", pwmValueBuffer[0], pwmValueBuffer[1], pwmValueBuffer[2], pwmValueBuffer[3]);
			//}
			

			
			// Saturation pwm 0-100%
			for(int i=0;i<4;i++){
				if(pwmValueBuffer[i]>100){
					pwmValueBuffer[i]=100;
				}
				else if(pwmValueBuffer[i]<0){
					pwmValueBuffer[i]=0;
				}
			}
			
			//printf("(pwm)  % 3.4f % 3.4f % 3.4f % 3.4f (thrust) % 1.4f (torque) % 1.4f % 1.4f % 1.4f\n", pwmValueBuffer[0], pwmValueBuffer[1], pwmValueBuffer[2], pwmValueBuffer[3], pwmValueBuffer[4], pwmValueBuffer[5], pwmValueBuffer[6], pwmValueBuffer[7]);
			
			
			// Copy control signal over to global memory for EKF to use during next state estimation
			pthread_mutex_lock(&mutexControlData);	
				memcpy(controlData, pwmValueBuffer, sizeof(controlData));		
			pthread_mutex_unlock(&mutexControlData);
			
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
					//printf("PWM received: %3.4f %3.4f %3.4f %3.4f\n", pwmValueBuffer[0], pwmValueBuffer[1], pwmValueBuffer[2], pwmValueBuffer[3]);
					//printf("(pwm)  % 3.4f % 3.4f % 3.4f % 3.4f (thrust) % 1.4f (torque) % 1.4f % 1.4f % 1.4f\n", pwmValueBuffer[0], pwmValueBuffer[1], pwmValueBuffer[2], pwmValueBuffer[3], pwmValueBuffer[4], pwmValueBuffer[5], pwmValueBuffer[6], pwmValueBuffer[7]);
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

// EKF calibration bias
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
		 printf("Mean (bias) EKF 9x9 bias\n");
		 printmat(ekf0,6,1);
		 printf("Covariance matrix (sigma) EKF 9x9 bias\n");
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
		//Rekf[0]=1;
		//Rekf[4]=1;
		//Rekf[8]=1000;
	
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

// EKF calibration 7x7 observer
void ekfCalibration7x7(double *Rekf, double *ekf0, double *ekfCal, double *ymeas, int counterCal){
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
		//Rekf[0]=1;
		//Rekf[4]=1;
		//Rekf[8]=1000;
	
		// Print results
		printf("Mean (bias) EKF 7x7\n");
		printmat(ekf0,3,1);
		printf("Covariance matrix (sigma) EKF 7x7\n");
		printmat(Rekf,3,3);
	}
	// Default i save calibrartion data
	else{
		ekfCal[counterCal*3]=ymeas[0];
		ekfCal[counterCal*3+1]=ymeas[1];
		ekfCal[counterCal*3+2]=ymeas[2];
	}		
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

// Quaternions to Eulers according to ZYX rotation
void q2euler_zyx(double *result, double *q){
	double R[5];
	R[0] = 2.*pow(q[0],2)-1+2.*pow(q[1],2);
    R[1] = 2.*(q[1]*q[2]-q[0]*q[3]);
    R[2] = 2.*(q[1]*q[3]+q[0]*q[2]);
    R[3] = 2.*(q[2]*q[3]-q[0]*q[1]);
    R[4] = 2.*pow(q[0],2)-1+2.*pow(q[3],2);

	//R(1,1,:) = 2.*q(:,1).^2-1+2.*q(:,2).^2;
    //R(2,1,:) = 2.*(q(:,2).*q(:,3)-q(:,1).*q(:,4));
    //R(3,1,:) = 2.*(q(:,2).*q(:,4)+q(:,1).*q(:,3));
    //R(3,2,:) = 2.*(q(:,3).*q(:,4)-q(:,1).*q(:,2));
    //R(3,3,:) = 2.*q(:,1).^2-1+2.*q(:,4).^2;

    result[2] = atan2(R[3],R[4]); // phi
    result[1] = -atan(R[2]/(sqrt(1-pow(R[2],2)))); // theta
    result[0] = atan2(R[1],R[0]); // psi
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
	 
	 //V[0]=0;
	 //V[1]=0;
	 //V[2]=0;
	 //V[3]=0;
	 //V[4]=0;
	 //V[5]=0;
	
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

// State Observer - Extended Kalman Filter for 6 position states + yaw and disturbance z estimation
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

// State Observer - Extended Kalman Filter for 6 position states + disturbance z estimation
void EKF_7x7(double *Phat, double *xhat, double *u, double *ymeas, double *Q, double *R, double Ts, int flag, double *par_att){
	// Local variables
	double xhat_pred[7]={0,0,0,0,0,0,0};
	//double C[27]={1,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double C[21]={1,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0};
	//double eye9[81]={1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1};
	double eye7[49]={1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1};
	double S_inv[9];
	double A[49], S[9], C_temp[21], Jfx_temp[49], Phat_pred[49], K_temp[21], K[21], V[3], xhat_temp[3], x_temp[7], fone=1, fzero=0;
	int n=7, k=7, m=7, ione=1;
	
	// Prediction step
	fx_7x1(xhat_pred, xhat, u, Ts, par_att); // state 
	Jfx_7x7(xhat, A, u, Ts, par_att); // update Jacobian A matrix
	
	// A*Phat_prev*A' + Q
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,A,&m,Phat,&k,&fzero,Jfx_temp,&m);
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Jfx_temp,&m,A,&n,&fzero,Phat_pred,&m);
	Phat_pred[0]+=Q[0];
	Phat_pred[8]+=Q[1];
	Phat_pred[16]+=Q[2];
	Phat_pred[24]+=Q[3];
	Phat_pred[32]+=Q[4];
	Phat_pred[40]+=Q[5];
	Phat_pred[48]+=Q[6];

	// Update step
	// S=C*P*C'+R; Innovation covariance
	n=7, k=7, m=3;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,C,&m,Phat_pred,&k,&fzero,C_temp,&m);
	n=3, k=7, m=3;
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,C_temp,&m,C,&n,&fzero,S,&m);
	S[0]+=R[0];
	S[4]+=R[4];
	S[8]+=R[8];

	// K=P*C'*S^-1; Kalman gain
	n=3, k=7, m=7; // 7x7 * 7x3 = 7x3
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Phat_pred,&m,C,&n,&fzero,K_temp,&m);
	mInverse(S,S_inv);
	n=3, k=3, m=7; // 7x3 * 3*3 = 7x3
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K_temp,&m,S_inv,&k,&fzero,K,&m);

	// V=y_meas-C*x_hat; Innovation
	n=7, m=3; 
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
	n=3, m=7;
	F77_CALL(dgemv)("n",&m,&n,&fone,K,&m,V,&ione,&fzero,x_temp,&ione);
	xhat[0]=xhat_pred[0]+x_temp[0];
	xhat[1]=xhat_pred[1]+x_temp[1];
	xhat[2]=xhat_pred[2]+x_temp[2];
	xhat[3]=xhat_pred[3]+x_temp[3];
	xhat[4]=xhat_pred[4]+x_temp[4];
	xhat[5]=xhat_pred[5]+x_temp[5];
	xhat[6]=xhat_pred[6]+x_temp[6];
	
	//printf("\nxhat\n");
	//printmat(xhat,15,1);
	
	// P=P-K*S*K'; Covariance update
	n=3, k=3, m=7; // 7x3 * 3x3 = 7x3
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K,&m,S,&k,&fzero,K_temp,&m); // K*S
	n=7, k=3, m=7; // 7x3 * 3x7 = 7x7
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,K_temp,&m,K,&n,&fzero,Phat,&m); // K_temp*K'
	n=7, k=7, m=7; // 7x7 * 7x7 = 7x7
	fzero=-1;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,eye7,&m,Phat_pred,&k,&fzero,Phat,&m); // P=P-K*S*K'
	
	//printf("\nPhat\n");
	//printmat(Phat,15,15);
}

// State Observer - Extended Kalman Filter for 9 position states
void EKF_8x8(double *Phat, double *xhat, double *u, double *ymeas, double *Q, double *R, double Ts, int flag, double *par_att){
	// Local variables
	double xhat_pred[8]={0};
	double C[24]={1,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	double eye8[64]={1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1};
	double S_inv[9];
	double A[64], S[9], C_temp[24], Jfx_temp[64], Phat_pred[64], K_temp[24], K[24], V[3], xhat_temp[3], x_temp[8], fone=1, fzero=0;
	int n=8, k=8, m=8, ione=1;
	
	// Prediction step
	fx_8x1(xhat_pred, xhat, u, Ts, par_att); // state - par_att[0]=phi, par_att[1]=theta, par_att[2]=omega_y, par_att[3]=omega_z
	Jfx_8x8(xhat, A, u, Ts, par_att); // update Jacobian A matrix - par_att[0]=phi, par_att[1]=theta, par_att[2]=omega_y, par_att[3]=omega_z
	
	// A*Phat_prev*A' + Q
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,A,&m,Phat,&k,&fzero,Jfx_temp,&m);
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Jfx_temp,&m,A,&n,&fzero,Phat_pred,&m);
	Phat_pred[0]+=Q[0];
	Phat_pred[9]+=Q[1];
	Phat_pred[18]+=Q[2];
	Phat_pred[27]+=Q[3];
	Phat_pred[36]+=Q[4];
	Phat_pred[45]+=Q[5];
	Phat_pred[54]+=Q[6];
	Phat_pred[63]+=Q[7];

	// Update step
	// S=C*P*C'+R; Innovation covariance
	// 3x8 * 8x8 = 3x8
	n=8, k=8, m=3;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,C,&m,Phat_pred,&k,&fzero,C_temp,&m);
	n=3, k=8, m=3;
	// 3x8 * 8x3 = 3x3
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,C_temp,&m,C,&n,&fzero,S,&m);
	S[0]+=R[0];
	S[4]+=R[4];
	S[8]+=R[8];

	// K=P*C'*S^-1; Kalman gain
	n=3, k=8, m=8; // 8x8 * 8x3 = 8x3
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,Phat_pred,&m,C,&n,&fzero,K_temp,&m);
	mInverse(S,S_inv);
	n=3, k=3, m=8; // 8x3 * 3*3 = 8x3
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K_temp,&m,S_inv,&k,&fzero,K,&m);

	// V=y_meas-C*x_hat; Innovation
	n=8, m=3; 
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
	n=3, m=8;
	F77_CALL(dgemv)("n",&m,&n,&fone,K,&m,V,&ione,&fzero,x_temp,&ione);
	xhat[0]=xhat_pred[0]+x_temp[0];
	xhat[1]=xhat_pred[1]+x_temp[1];
	xhat[2]=xhat_pred[2]+x_temp[2];
	xhat[3]=xhat_pred[3]+x_temp[3];
	xhat[4]=xhat_pred[4]+x_temp[4];
	xhat[5]=xhat_pred[5]+x_temp[5];
	xhat[6]=xhat_pred[6]+x_temp[6];
	xhat[7]=xhat_pred[7]+x_temp[7];
	
	//printf("\nxhat\n");
	//printmat(xhat,15,1);
	
	// P=P-K*S*K'; Covariance update
	n=3, k=3, m=8; // 8x3 * 3x3 = 8x3
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,K,&m,S,&k,&fzero,K_temp,&m); // K*S
	n=8, k=3, m=8; // 8x3 * 3x8 = 8x8
	F77_CALL(dgemm)("n","t",&m,&n,&k,&fone,K_temp,&m,K,&n,&fzero,Phat,&m); // K_temp*K'
	n=8, k=8, m=8; // 8x8 * 8x8 = 8x8
	fzero=-1;
	F77_CALL(dgemm)("n","n",&m,&n,&k,&fone,eye8,&m,Phat_pred,&k,&fzero,Phat,&m); // P=P-K*S*K'
	
	//printf("\nPhat\n");
	//printmat(Phat,15,15);
}

// Nonlinear Model for attitude states (6x1)
void fx_6x1(double *xhat, double *xhat_prev, double *u, double Ts){
	/* 
	xhat[0]=xhat_prev[0] + Ts*(xhat_prev[3] + xhat_prev[5]*cos(xhat_prev[0])*tan(xhat_prev[1]) + xhat_prev[4]*sin(xhat_prev[0])*tan(xhat_prev[1]));
	 xhat[1]=xhat_prev[1] + Ts*(xhat_prev[4]*cos(xhat_prev[0]) - xhat_prev[5]*sin(xhat_prev[0]));
	 xhat[2]=xhat_prev[2] + Ts*((xhat_prev[5]*cos(xhat_prev[0]))/cos(xhat_prev[1]) + (xhat_prev[4]*sin(xhat_prev[0]))/cos(xhat_prev[1]));
	 xhat[3]=xhat_prev[3] - Ts*((xhat_prev[4]*xhat_prev[5]*(par_i_yy - par_i_zz))/par_i_xx - (par_L*par_c_m*par_k*(pow(u[0],2) - pow(u[2],2)))/par_i_xx);
	 xhat[4]=xhat_prev[4] + Ts*((xhat_prev[3]*xhat_prev[5]*(par_i_xx - par_i_zz))/par_i_yy + (par_L*par_c_m*par_k*(pow(u[1],2) - pow(u[3],2)))/par_i_yy);
	 xhat[5]=xhat_prev[5] + Ts*((par_b*par_c_m*(pow(u[0],2) - pow(u[1],2) + pow(u[2],2) - pow(u[3],2)))/par_i_zz - (xhat_prev[3]*xhat_prev[4]*(par_i_xx - par_i_yy))/par_i_zz);
 	*/
	xhat[0]=xhat_prev[0] + Ts*(xhat_prev[3] + xhat_prev[5]*cos(xhat_prev[0])*tan(xhat_prev[1]) + xhat_prev[4]*sin(xhat_prev[0])*tan(xhat_prev[1]));
	xhat[1]=xhat_prev[1] + Ts*(xhat_prev[4]*cos(xhat_prev[0]) - xhat_prev[5]*sin(xhat_prev[0]));
	xhat[2]=xhat_prev[2] + Ts*((xhat_prev[5]*cos(xhat_prev[0]))/cos(xhat_prev[1]) + (xhat_prev[4]*sin(xhat_prev[0]))/cos(xhat_prev[1]));
	xhat[3]=xhat_prev[3] - Ts*((xhat_prev[4]*xhat_prev[5]*(par_i_yy - par_i_zz))/par_i_xx + (par_L*par_c_m*par_k*(pow(u[0],2) - pow(u[2],2)))/par_i_xx);
	xhat[4]=xhat_prev[4] + Ts*((xhat_prev[3]*xhat_prev[5]*(par_i_xx - par_i_zz))/par_i_yy - (par_L*par_c_m*par_k*(pow(u[1],2) - pow(u[3],2)))/par_i_yy);
	xhat[5]=xhat_prev[5] + Ts*((par_b*par_c_m*(pow(u[0],2) - pow(u[1],2) + pow(u[2],2) - pow(u[3],2)))/par_i_zz - (xhat_prev[3]*xhat_prev[4]*(par_i_xx - par_i_yy))/par_i_zz);
}

// Jacobian of model for attitude states (6x6)
void Jfx_6x6(double *xhat, double *A, double *u, double Ts){
	 //A[0]=Ts*(xhat[4]*cos(xhat[0])*tan(xhat[1]) - xhat[5]*sin(xhat[0])*tan(xhat[1])) + 1;A[1]=-Ts*(xhat[5]*cos(xhat[0]) + xhat[4]*sin(xhat[0]));A[2]=Ts*((xhat[4]*cos(xhat[0]))/cos(xhat[1]) - (xhat[5]*sin(xhat[0]))/cos(xhat[1]));A[3]=0;A[4]=0;A[5]=0;A[6]=Ts*(xhat[5]*cos(xhat[0])*(pow(tan(xhat[1]),2) + 1) + xhat[4]*sin(xhat[0])*(pow(tan(xhat[1]),2) + 1));A[7]=1;A[8]=Ts*((xhat[5]*cos(xhat[0])*sin(xhat[1]))/pow(cos(xhat[1]),2) + (xhat[4]*sin(xhat[0])*sin(xhat[1]))/pow(cos(xhat[1]),2));A[9]=0;A[10]=0;A[11]=0;A[12]=0;A[13]=0;A[14]=1;A[15]=0;A[16]=0;A[17]=0;A[18]=Ts;A[19]=0;A[20]=0;A[21]=1;A[22]=(Ts*xhat[5]*(par_i_xx - par_i_zz))/par_i_yy;A[23]=-(Ts*xhat[4]*(par_i_xx - par_i_yy))/par_i_zz;A[24]=Ts*sin(xhat[0])*tan(xhat[1]);A[25]=Ts*cos(xhat[0]);A[26]=(Ts*sin(xhat[0]))/cos(xhat[1]);A[27]=-(Ts*xhat[5]*(par_i_yy - par_i_zz))/par_i_xx;A[28]=1;A[29]=-(Ts*xhat[3]*(par_i_xx - par_i_yy))/par_i_zz;A[30]=Ts*cos(xhat[0])*tan(xhat[1]);A[31]=-Ts*sin(xhat[0]);A[32]=(Ts*cos(xhat[0]))/cos(xhat[1]);A[33]=-(Ts*xhat[4]*(par_i_yy - par_i_zz))/par_i_xx;A[34]=(Ts*xhat[3]*(par_i_xx - par_i_zz))/par_i_yy;A[35]=1;
	A[0]=Ts*(xhat[4]*cos(xhat[0])*tan(xhat[1]) - xhat[5]*sin(xhat[0])*tan(xhat[1])) + 1;A[1]=-Ts*(xhat[5]*cos(xhat[0]) + xhat[4]*sin(xhat[0]));A[2]=Ts*((xhat[4]*cos(xhat[0]))/cos(xhat[1]) - (xhat[5]*sin(xhat[0]))/cos(xhat[1]));A[3]=0;A[4]=0;A[5]=0;A[6]=Ts*(xhat[5]*cos(xhat[0])*(pow(tan(xhat[1]),2) + 1) + xhat[4]*sin(xhat[0])*(pow(tan(xhat[1]),2) + 1));A[7]=1;A[8]=Ts*((xhat[5]*cos(xhat[0])*sin(xhat[1]))/pow(cos(xhat[1]),2) + (xhat[4]*sin(xhat[0])*sin(xhat[1]))/pow(cos(xhat[1]),2));A[9]=0;A[10]=0;A[11]=0;A[12]=0;A[13]=0;A[14]=1;A[15]=0;A[16]=0;A[17]=0;A[18]=Ts;A[19]=0;A[20]=0;A[21]=1;A[22]=(Ts*xhat[5]*(par_i_xx - par_i_zz))/par_i_yy;A[23]=-(Ts*xhat[4]*(par_i_xx - par_i_yy))/par_i_zz;A[24]=Ts*sin(xhat[0])*tan(xhat[1]);A[25]=Ts*cos(xhat[0]);A[26]=(Ts*sin(xhat[0]))/cos(xhat[1]);A[27]=-(Ts*xhat[5]*(par_i_yy - par_i_zz))/par_i_xx;A[28]=1;A[29]=-(Ts*xhat[3]*(par_i_xx - par_i_yy))/par_i_zz;A[30]=Ts*cos(xhat[0])*tan(xhat[1]);A[31]=-Ts*sin(xhat[0]);A[32]=(Ts*cos(xhat[0]))/cos(xhat[1]);A[33]=-(Ts*xhat[4]*(par_i_yy - par_i_zz))/par_i_xx;A[34]=(Ts*xhat[3]*(par_i_xx - par_i_zz))/par_i_yy;A[35]=1;
 }

// Nonlinear Model for position states (9x1)
void fx_9x1(double *xhat, double *xhat_prev, double *u, double Ts, double *par_att){
	xhat[0]=xhat_prev[0] + Ts*xhat_prev[3];
	xhat[1]=xhat_prev[1] + Ts*xhat_prev[4];
	xhat[2]=xhat_prev[2] + Ts*xhat_prev[5];
	xhat[3]=xhat_prev[3] + Ts*(xhat_prev[6]/par_mass - (par_k_d*xhat_prev[3])/par_mass + (par_c_m*par_k*(sin(par_att[0])*sin(par_att[2]) + cos(par_att[0])*cos(par_att[2])*sin(par_att[1]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[4]=xhat_prev[4] - Ts*((par_k_d*xhat_prev[4])/par_mass - xhat_prev[7]/par_mass + (par_c_m*par_k*(cos(par_att[2])*sin(par_att[0]) - cos(par_att[0])*sin(par_att[1])*sin(par_att[2]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[5]=xhat_prev[5] + Ts*(xhat_prev[8] - (par_k_d*xhat_prev[5])/par_mass + (par_c_m*par_k*cos(par_att[0])*cos(par_att[1])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[6]=xhat_prev[6];
	xhat[7]=xhat_prev[7];
	xhat[8]=xhat_prev[8];
}

// Jacobian of model for position states (9x9)
void Jfx_9x9(double *xhat, double *A, double *u, double Ts, double *par_att){
	//A[0]=1;A[1]=0;A[2]=0;A[3]=0;A[4]=0;A[5]=0;A[6]=0;A[7]=0;A[8]=0;A[9]=0;A[10]=1;A[11]=0;A[12]=0;A[13]=0;A[14]=0;A[15]=0;A[16]=0;A[17]=0;A[18]=0;A[19]=0;A[20]=1;A[21]=0;A[22]=0;A[23]=0;A[24]=0;A[25]=0;A[26]=0;A[27]=Ts;A[28]=0;A[29]=0;A[30]=1 - (Ts*par_k_d)/par_mass;A[31]=0;A[32]=0;A[33]=0;A[34]=0;A[35]=0;A[36]=0;A[37]=Ts;A[38]=0;A[39]=0;A[40]=1 - (Ts*par_k_d)/par_mass;A[41]=0;A[42]=0;A[43]=0;A[44]=0;A[45]=0;A[46]=0;A[47]=Ts;A[48]=0;A[49]=0;A[50]=1 - (Ts*par_k_d)/par_mass;A[51]=0;A[52]=0;A[53]=0;A[54]=0;A[55]=0;A[56]=0;A[57]=Ts;A[58]=0;A[59]=0;A[60]=1;A[61]=0;A[62]=0;A[63]=0;A[64]=0;A[65]=0;A[66]=0;A[67]=Ts;A[68]=0;A[69]=0;A[70]=1;A[71]=0;A[72]=0;A[73]=0;A[74]=0;A[75]=0;A[76]=0;A[77]=Ts;A[78]=0;A[79]=0;A[80]=1;
	A[0]=1;A[1]=0;A[2]=0;A[3]=0;A[4]=0;A[5]=0;A[6]=0;A[7]=0;A[8]=0;A[9]=0;A[10]=1;A[11]=0;A[12]=0;A[13]=0;A[14]=0;A[15]=0;A[16]=0;A[17]=0;A[18]=0;A[19]=0;A[20]=1;A[21]=0;A[22]=0;A[23]=0;A[24]=0;A[25]=0;A[26]=0;A[27]=Ts;A[28]=0;A[29]=0;A[30]=1 - (Ts*par_k_d)/par_mass;A[31]=0;A[32]=0;A[33]=0;A[34]=0;A[35]=0;A[36]=0;A[37]=Ts;A[38]=0;A[39]=0;A[40]=1 - (Ts*par_k_d)/par_mass;A[41]=0;A[42]=0;A[43]=0;A[44]=0;A[45]=0;A[46]=0;A[47]=Ts;A[48]=0;A[49]=0;A[50]=1 - (Ts*par_k_d)/par_mass;A[51]=0;A[52]=0;A[53]=0;A[54]=0;A[55]=0;A[56]=0;A[57]=Ts/par_mass;A[58]=0;A[59]=0;A[60]=1;A[61]=0;A[62]=0;A[63]=0;A[64]=0;A[65]=0;A[66]=0;A[67]=Ts/par_mass;A[68]=0;A[69]=0;A[70]=1;A[71]=0;A[72]=0;A[73]=0;A[74]=0;A[75]=0;A[76]=0;A[77]=Ts;A[78]=0;A[79]=0;A[80]=1;
}

// Nonlinear Model for position states (7x1), [x,y,z,vx,vy,vz,dz]
void fx_7x1(double *xhat, double *xhat_prev, double *u, double Ts, double *par_att){
	xhat[0]=xhat_prev[0] + Ts*xhat_prev[3];
	xhat[1]=xhat_prev[1] + Ts*xhat_prev[4];
	xhat[2]=xhat_prev[2] + Ts*xhat_prev[5];
	xhat[3]=xhat_prev[3] + Ts*( - (par_k_d*xhat_prev[3])/par_mass + (par_c_m*par_k*(sin(par_att[0])*sin(par_att[2]) + cos(par_att[0])*cos(par_att[2])*sin(par_att[1]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[4]=xhat_prev[4] - Ts*((par_k_d*xhat_prev[4])/par_mass + (par_c_m*par_k*(cos(par_att[2])*sin(par_att[0]) - cos(par_att[0])*sin(par_att[1])*sin(par_att[2]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[5]=xhat_prev[5] + Ts*(xhat_prev[6] - (par_k_d*xhat_prev[5])/par_mass + (par_c_m*par_k*cos(par_att[0])*cos(par_att[1])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[6]=xhat_prev[6];
}

// Jacobian of model for position states (7x7)
void Jfx_7x7(double *xhat, double *A, double *u, double Ts, double *par_att){
	//A[0]=1;A[1]=0;A[2]=0;A[3]=0;A[4]=0;A[5]=0;A[6]=0;A[7]=0;A[8]=0;A[9]=0;A[10]=1;A[11]=0;A[12]=0;A[13]=0;A[14]=0;A[15]=0;A[16]=0;A[17]=0;A[18]=0;A[19]=0;A[20]=1;A[21]=0;A[22]=0;A[23]=0;A[24]=0;A[25]=0;A[26]=0;A[27]=Ts;A[28]=0;A[29]=0;A[30]=1 - (Ts*par_k_d)/par_mass;A[31]=0;A[32]=0;A[33]=0;A[34]=0;A[35]=0;A[36]=0;A[37]=Ts;A[38]=0;A[39]=0;A[40]=1 - (Ts*par_k_d)/par_mass;A[41]=0;A[42]=0;A[43]=0;A[44]=0;A[45]=0;A[46]=0;A[47]=Ts;A[48]=0;A[49]=0;A[50]=1 - (Ts*par_k_d)/par_mass;A[51]=0;A[52]=0;A[53]=0;A[54]=0;A[55]=0;A[56]=0;A[57]=Ts;A[58]=0;A[59]=0;A[60]=1;A[61]=0;A[62]=0;A[63]=0;A[64]=0;A[65]=0;A[66]=0;A[67]=Ts;A[68]=0;A[69]=0;A[70]=1;A[71]=0;A[72]=0;A[73]=0;A[74]=0;A[75]=0;A[76]=0;A[77]=Ts;A[78]=0;A[79]=0;A[80]=1;
	//A[0]=1;A[1]=0;A[2]=0;A[3]=0;A[4]=0;A[5]=0;A[6]=0;A[7]=0;A[8]=0;A[9]=0;A[10]=1;A[11]=0;A[12]=0;A[13]=0;A[14]=0;A[15]=0;A[16]=0;A[17]=0;A[18]=0;A[19]=0;A[20]=1;A[21]=0;A[22]=0;A[23]=0;A[24]=0;A[25]=0;A[26]=0;A[27]=Ts;A[28]=0;A[29]=0;A[30]=1 - (Ts*par_k_d)/par_mass;A[31]=0;A[32]=0;A[33]=0;A[34]=0;A[35]=0;A[36]=0;A[37]=Ts;A[38]=0;A[39]=0;A[40]=1 - (Ts*par_k_d)/par_mass;A[41]=0;A[42]=0;A[43]=0;A[44]=0;A[45]=0;A[46]=0;A[47]=Ts;A[48]=0;A[49]=0;A[50]=1 - (Ts*par_k_d)/par_mass;A[51]=0;A[52]=0;A[53]=0;A[54]=0;A[55]=0;A[56]=0;A[57]=Ts/par_mass;A[58]=0;A[59]=0;A[60]=1;A[61]=0;A[62]=0;A[63]=0;A[64]=0;A[65]=0;A[66]=0;A[67]=Ts/par_mass;A[68]=0;A[69]=0;A[70]=1;A[71]=0;A[72]=0;A[73]=0;A[74]=0;A[75]=0;A[76]=0;A[77]=Ts;A[78]=0;A[79]=0;A[80]=1;
	A[0]=1;A[1]=0;A[2]=0;A[3]=0;A[4]=0;A[5]=0;A[6]=0;A[7]=0;A[8]=1;A[9]=0;A[10]=0;A[11]=0;A[12]=0;A[13]=0;A[14]=0;A[15]=0;A[16]=1;A[17]=0;A[18]=0;A[19]=0;A[20]=0;A[21]=Ts;A[22]=0;A[23]=0;A[24]=1 - (Ts*par_k_d)/par_mass;A[25]=0;A[26]=0;A[27]=0;A[28]=0;A[29]=Ts;A[30]=0;A[31]=0;A[32]=1 - (Ts*par_k_d)/par_mass;A[33]=0;A[34]=0;A[35]=0;A[36]=0;A[37]=Ts;A[38]=0;A[39]=0;A[40]=1 - (Ts*par_k_d)/par_mass;A[41]=0;A[42]=0;A[43]=0;A[44]=0;A[45]=0;A[46]=0;A[47]=Ts;A[48]=1;
}

// Nonlinear Model for position states (8x1) including yaw and disturbance z estimation
// Yaw estimation is based on the nonlinear model including gyro scope measurements
/*void fx_8x1(double *xhat, double *xhat_prev, double *u, double Ts, double *par_att){
	// par_att[0]=phi, par_att[1]=theta, par_att[2]=omega_y, par_att[3]=omega_z
	xhat[0]=xhat_prev[0] + Ts*xhat_prev[3];
	xhat[1]=xhat_prev[1] + Ts*xhat_prev[4];
	xhat[2]=xhat_prev[2] + Ts*xhat_prev[5];
	xhat[3]=xhat_prev[3] - Ts*((par_k_d*xhat_prev[3])/par_mass - (par_c_m*par_k*(sin(par_att[0])*sin(xhat_prev[6]) + cos(par_att[0])*cos(xhat_prev[6])*sin(par_att[1]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[4]=xhat_prev[4] - Ts*((par_k_d*xhat_prev[4])/par_mass + (par_c_m*par_k*(cos(xhat_prev[6])*sin(par_att[0]) - cos(par_att[0])*sin(par_att[1])*sin(xhat_prev[6]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[5]=xhat_prev[5] + Ts*(xhat_prev[7] - (par_k_d*xhat_prev[5])/par_mass + (par_c_m*par_k*cos(par_att[0])*cos(par_att[1])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[6]=xhat_prev[6] + Ts*((par_att[3]*cos(par_att[0]))/cos(par_att[1]) + (par_att[2]*sin(par_att[0]))/cos(par_att[1]));
	xhat[7]=xhat_prev[7];
}*/

// Nonlinear Model for position states (8x1) including yaw and disturbance z estimation
// Yaw estimation is based on pure position estimation and measurement update using the random walk model
void fx_8x1(double *xhat, double *xhat_prev, double *u, double Ts, double *par_att){
	// par_att[0]=phi, par_att[1]=theta
	xhat[0]=xhat_prev[0] + Ts*xhat_prev[3];
	xhat[1]=xhat_prev[1] + Ts*xhat_prev[4];
	xhat[2]=xhat_prev[2] + Ts*xhat_prev[5];
	xhat[3]=xhat_prev[3] - Ts*((par_k_d*xhat_prev[3])/par_mass - (par_c_m*par_k*(sin(par_att[0])*sin(xhat_prev[6]) + cos(par_att[0])*cos(xhat_prev[6])*sin(par_att[1]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[4]=xhat_prev[4] - Ts*((par_k_d*xhat_prev[4])/par_mass + (par_c_m*par_k*(cos(xhat_prev[6])*sin(par_att[0]) - cos(par_att[0])*sin(par_att[1])*sin(xhat_prev[6]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[5]=xhat_prev[5] - Ts*( - xhat_prev[7] + (par_k_d*xhat_prev[5])/par_mass - (par_c_m*par_k*cos(par_att[0])*cos(par_att[1])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[6]=xhat_prev[6];
	xhat[7]=xhat_prev[7];
}

// Jacobian of model for position states (8x8) including yaw bias and disturbance z estimation.
// Yaw bias estimattionis using the random walk model
/*void fx_8x1(double *xhat, double *xhat_prev, double *u, double Ts, double *par_att){
	// par_att[0]=phi, par_att[1]=theta, par_att[2]=psi
	xhat[0]=xhat_prev[0] + Ts*xhat_prev[3];
	xhat[1]=xhat_prev[1] + Ts*xhat_prev[4];
	xhat[2]=xhat_prev[2] + Ts*xhat_prev[5];
	xhat[3]=xhat_prev[3] - Ts*((par_k_d*xhat_prev[3])/par_mass - (par_c_m*par_k*(sin(par_att[2] + xhat_prev[6])*sin(par_att[0]) + cos(par_att[2] + xhat_prev[6])*cos(par_att[0])*sin(par_att[1]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[4]=xhat_prev[4] - Ts*((par_k_d*xhat_prev[4])/par_mass + (par_c_m*par_k*(cos(par_att[2] + xhat_prev[6])*sin(par_att[0]) - sin(par_att[2] + xhat_prev[6])*cos(par_att[0])*sin(par_att[1]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[5]=xhat_prev[5] + Ts*(xhat_prev[7] - (par_k_d*xhat_prev[5])/par_mass + (par_c_m*par_k*cos(par_att[0])*cos(par_att[1])*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass);
	xhat[6]=xhat_prev[6];
	xhat[7]=xhat_prev[7];
}*/

// Jacobian of model for position states (8x8) including yaw and disturbance z estimation.
// Yaw estimation is based on the nonlinear model including gyro scope measurements
/*void Jfx_8x8(double *xhat, double *A, double *u, double Ts, double *par_att){
	// par_att[0]=phi, par_att[1]=theta, par_att[2]=omega_y, par_att[3]=omega_z
	A[0]=1;A[1]=0;A[2]=0;A[3]=0;A[4]=0;A[5]=0;A[6]=0;A[7]=0;A[8]=0;A[9]=1;A[10]=0;A[11]=0;A[12]=0;A[13]=0;A[14]=0;A[15]=0;A[16]=0;A[17]=0;A[18]=1;A[19]=0;A[20]=0;A[21]=0;A[22]=0;A[23]=0;A[24]=Ts;A[25]=0;A[26]=0;A[27]=1 - (Ts*par_k_d)/par_mass;A[28]=0;A[29]=0;A[30]=0;A[31]=0;A[32]=0;A[33]=Ts;A[34]=0;A[35]=0;A[36]=1 - (Ts*par_k_d)/par_mass;A[37]=0;A[38]=0;A[39]=0;A[40]=0;A[41]=0;A[42]=Ts;A[43]=0;A[44]=0;A[45]=1 - (Ts*par_k_d)/par_mass;A[46]=0;A[47]=0;A[48]=0;A[49]=0;A[50]=0;A[51]=(Ts*par_c_m*par_k*(cos(xhat[6])*sin(par_att[0]) - cos(par_att[0])*sin(par_att[1])*sin(xhat[6]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[52]=(Ts*par_c_m*par_k*(sin(par_att[0])*sin(xhat[6]) + cos(par_att[0])*cos(xhat[6])*sin(par_att[1]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[53]=0;A[54]=1;A[55]=0;A[56]=0;A[57]=0;A[58]=0;A[59]=0;A[60]=0;A[61]=Ts;A[62]=0;A[63]=1;
}*/

// Jacobian of model for position states (8x8) including yaw and disturbance z estimation.
// Yaw estimation is based on pure position estimation and measurement update using the random walk model
void Jfx_8x8(double *xhat, double *A, double *u, double Ts, double *par_att){
	// par_att[0]=phi, par_att[1]=theta
	A[0]=1;A[1]=0;A[2]=0;A[3]=0;A[4]=0;A[5]=0;A[6]=0;A[7]=0;A[8]=0;A[9]=1;A[10]=0;A[11]=0;A[12]=0;A[13]=0;A[14]=0;A[15]=0;A[16]=0;A[17]=0;A[18]=1;A[19]=0;A[20]=0;A[21]=0;A[22]=0;A[23]=0;A[24]=Ts;A[25]=0;A[26]=0;A[27]=1 - (Ts*par_k_d)/par_mass;A[28]=0;A[29]=0;A[30]=0;A[31]=0;A[32]=0;A[33]=Ts;A[34]=0;A[35]=0;A[36]=1 - (Ts*par_k_d)/par_mass;A[37]=0;A[38]=0;A[39]=0;A[40]=0;A[41]=0;A[42]=Ts;A[43]=0;A[44]=0;A[45]=1 - (Ts*par_k_d)/par_mass;A[46]=0;A[47]=0;A[48]=0;A[49]=0;A[50]=0;A[51]=(Ts*par_c_m*par_k*(cos(xhat[6])*sin(par_att[0]) - cos(par_att[0])*sin(par_att[1])*sin(xhat[6]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[52]=(Ts*par_c_m*par_k*(sin(par_att[0])*sin(xhat[6]) + cos(par_att[0])*cos(xhat[6])*sin(par_att[1]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[53]=0;A[54]=1;A[55]=0;A[56]=0;A[57]=0;A[58]=0;A[59]=0;A[60]=0;A[61]=Ts;A[62]=0;A[63]=1;
}

// Jacobian of model for position states (8x8) including yaw bias and disturbance z estimation.
// Yaw bias estimattionis using the random walk model
/*void Jfx_8x8(double *xhat, double *A, double *u, double Ts, double *par_att){
	// par_att[0]=phi, par_att[1]=theta, par_att[2]=psi
	A[0]=1;A[1]=0;A[2]=0;A[3]=0;A[4]=0;A[5]=0;A[6]=0;A[7]=0;A[8]=0;A[9]=1;A[10]=0;A[11]=0;A[12]=0;A[13]=0;A[14]=0;A[15]=0;A[16]=0;A[17]=0;A[18]=1;A[19]=0;A[20]=0;A[21]=0;A[22]=0;A[23]=0;A[24]=Ts;A[25]=0;A[26]=0;A[27]=1 - (Ts*par_k_d)/par_mass;A[28]=0;A[29]=0;A[30]=0;A[31]=0;A[32]=0;A[33]=Ts;A[34]=0;A[35]=0;A[36]=1 - (Ts*par_k_d)/par_mass;A[37]=0;A[38]=0;A[39]=0;A[40]=0;A[41]=0;A[42]=Ts;A[43]=0;A[44]=0;A[45]=1 - (Ts*par_k_d)/par_mass;A[46]=0;A[47]=0;A[48]=0;A[49]=0;A[50]=0;A[51]=(Ts*par_c_m*par_k*(cos(par_att[2] + xhat[6])*sin(par_att[0]) - sin(par_att[2] + xhat[6])*cos(par_att[0])*sin(par_att[1]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[52]=(Ts*par_c_m*par_k*(sin(par_att[2] + xhat[6])*sin(par_att[0]) + cos(par_att[2] + xhat[6])*cos(par_att[0])*sin(par_att[1]))*(pow(u[0],2) + pow(u[1],2) + pow(u[2],2) + pow(u[3],2)))/par_mass;A[53]=0;A[54]=1;A[55]=0;A[56]=0;A[57]=0;A[58]=0;A[59]=0;A[60]=0;A[61]=Ts;A[62]=0;A[63]=1;
}*/

// Nonlinear Model for attitude states including bias estimation (9x1)
 void fx_9x1_bias(double *xhat, double *xhat_prev, double *u, double Ts){
	 xhat[0]=xhat_prev[0] + Ts*(xhat_prev[3] + xhat_prev[5]*cos(xhat_prev[0])*tan(xhat_prev[1]) + xhat_prev[4]*sin(xhat_prev[0])*tan(xhat_prev[1]));
	 xhat[1]=xhat_prev[1] + Ts*(xhat_prev[4]*cos(xhat_prev[0]) - xhat_prev[5]*sin(xhat_prev[0]));
	 xhat[2]=xhat_prev[2] + Ts*((xhat_prev[5]*cos(xhat_prev[0]))/cos(xhat_prev[1]) + (xhat_prev[4]*sin(xhat_prev[0]))/cos(xhat_prev[1]));
	 xhat[3]=xhat_prev[3] + Ts*(xhat_prev[6]/par_i_xx - (xhat_prev[4]*xhat_prev[5]*(par_i_yy - par_i_zz))/par_i_xx + (par_L*par_c_m*par_k*(pow(u[0],2) - pow(u[2],2)))/par_i_xx);
	 xhat[4]=xhat_prev[4] + Ts*(xhat_prev[7]/par_i_yy + (xhat_prev[3]*xhat_prev[5]*(par_i_xx - par_i_zz))/par_i_yy + (par_L*par_c_m*par_k*(pow(u[1],2) - pow(u[3],2)))/par_i_yy);
	 xhat[5]=xhat_prev[5] + Ts*(xhat_prev[8]/par_i_zz + (par_b*par_c_m*(pow(u[0],2) - pow(u[1],2) + pow(u[2],2) - pow(u[3],2)))/par_i_zz - (xhat_prev[3]*xhat_prev[4]*(par_i_xx - par_i_yy))/par_i_zz);
	 xhat[6]=xhat_prev[6];
	 xhat[7]=xhat_prev[7];
	 xhat[8]=xhat_prev[8];
 }

// Jacobian of model for attitude states including bias estimation (9x9)
 void Jfx_9x9_bias(double *xhat, double *A, double *u, double Ts){
	 A[0]=Ts*(xhat[4]*cos(xhat[0])*tan(xhat[1]) - xhat[5]*sin(xhat[0])*tan(xhat[1])) + 1;A[1]=-Ts*(xhat[5]*cos(xhat[0]) + xhat[4]*sin(xhat[0]));A[2]=Ts*((xhat[4]*cos(xhat[0]))/cos(xhat[1]) - (xhat[5]*sin(xhat[0]))/cos(xhat[1]));A[3]=0;A[4]=0;A[5]=0;A[6]=0;A[7]=0;A[8]=0;A[9]=Ts*(xhat[5]*cos(xhat[0])*(pow(tan(xhat[1]),2) + 1) + xhat[4]*sin(xhat[0])*(pow(tan(xhat[1]),2) + 1));A[10]=1;A[11]=Ts*((xhat[5]*cos(xhat[0])*sin(xhat[1]))/pow(cos(xhat[1]),2) + (xhat[4]*sin(xhat[0])*sin(xhat[1]))/pow(cos(xhat[1]),2));A[12]=0;A[13]=0;A[14]=0;A[15]=0;A[16]=0;A[17]=0;A[18]=0;A[19]=0;A[20]=1;A[21]=0;A[22]=0;A[23]=0;A[24]=0;A[25]=0;A[26]=0;A[27]=Ts;A[28]=0;A[29]=0;A[30]=1;A[31]=(Ts*xhat[5]*(par_i_xx - par_i_zz))/par_i_yy;A[32]=-(Ts*xhat[4]*(par_i_xx - par_i_yy))/par_i_zz;A[33]=0;A[34]=0;A[35]=0;A[36]=Ts*sin(xhat[0])*tan(xhat[1]);A[37]=Ts*cos(xhat[0]);A[38]=(Ts*sin(xhat[0]))/cos(xhat[1]);A[39]=-(Ts*xhat[5]*(par_i_yy - par_i_zz))/par_i_xx;A[40]=1;A[41]=-(Ts*xhat[3]*(par_i_xx - par_i_yy))/par_i_zz;A[42]=0;A[43]=0;A[44]=0;A[45]=Ts*cos(xhat[0])*tan(xhat[1]);A[46]=-Ts*sin(xhat[0]);A[47]=(Ts*cos(xhat[0]))/cos(xhat[1]);A[48]=-(Ts*xhat[4]*(par_i_yy - par_i_zz))/par_i_xx;A[49]=(Ts*xhat[3]*(par_i_xx - par_i_zz))/par_i_yy;A[50]=1;A[51]=0;A[52]=0;A[53]=0;A[54]=0;A[55]=0;A[56]=0;A[57]=Ts/par_i_xx;A[58]=0;A[59]=0;A[60]=1;A[61]=0;A[62]=0;A[63]=0;A[64]=0;A[65]=0;A[66]=0;A[67]=Ts/par_i_yy;A[68]=0;A[69]=0;A[70]=1;A[71]=0;A[72]=0;A[73]=0;A[74]=0;A[75]=0;A[76]=0;A[77]=Ts/par_i_zz;A[78]=0;A[79]=0;A[80]=1;
 }

// Saturation function
void saturation(double *var, int index, double limMin, double limMax){
	if(var[index]<limMin){
		var[index]=limMin;
	}
	else if(var[index]>limMax){
		var[index]=limMax;
	}
}

// Magnetometer part: mu_m
void magnetometerUpdate(double *quat, double *P, double *ymag, double *m0, double *Rm, double *L, double a, int *outlierFlag){
	// local variables
	int n=3, k=3, m=3;
	double normMag, L_temp, ykm[3], ykm2[9], Q[9], h1[9], h2[9], h3[9], h4[9], hd[12], Smag[9], P_temp[16], S_temp[12], K_temp[12], Smag_inv[9], ymag_diff[3], state_temp[4];
	double fkm[3]={0,0,0}, K[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
	double ymag_local[3] = {0.0};
	
	normMag=sqrt(pow(ymag[0],2) + pow(ymag[1],2) + pow(ymag[2],2));
	L_temp=(1-a)*L[0]+a*normMag; // recursive magnetometer compensator
	L[0]=L_temp;
	
	ymag_local[0] = ymag[0]/normMag;
	ymag_local[1] = ymag[1]/normMag;
	ymag_local[2] = ymag[2]/normMag;
	
	//L[0]=(1-a)*L[0]+a*sqrt(pow(ymag[0],2) + pow(ymag[1],2) + pow(ymag[2],2)); // recursive magnetometer compensator
	
	if ( normMag > 1.15*L_temp || normMag < 0.85*L_temp ){
		// dont use measurement
		//printf("Magnetometer Outlier\n");
		outlierFlag[0]=1;
	}
	else{
		// continue measurement
		// mu_m
		// ykm=Qq(x)'*ykm2=Qq(x)'*(m0+fkm); magnetometer and quaternion model relation
		Qq(Q, quat);
		ykm2[0]=m0[0]+fkm[0];
		ykm2[1]=m0[1]+fkm[1];
		ykm2[2]=m0[2]+fkm[2];
		F77_CALL(dgemv)("t",&m,&n,&fone,Q,&m,ykm2,&ione,&fzero,ykm,&ione);
		
		// [h1 h2 h3 h4]=dQqdq(x); jacobian
		// hd=[h1'*m0 h2'*m0 h3'*m0 h4'*m0];
		dQqdq(h1, h2, h3, h4, hd, quat, m0);	
		
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
		quat[0]+=state_temp[0];
		quat[1]+=state_temp[1];
		quat[2]+=state_temp[2];
		quat[3]+=state_temp[3];
		
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
		
		outlierFlag[0]=0;
	}
}

// Sensor calibration
void sensorCalibration(double *Rmag, double *mag0, double *magCal, double *ymag, int counterCal){
	// Calibration routine to get mean, variance and std_deviation
	if(counterCal==CALIBRATION){
		// Mean (bias) accelerometer, gyroscope and magnetometer
		for (int i=0;i<CALIBRATION;i++){
			mag0[0]+=magCal[i*3];
			mag0[1]+=magCal[i*3+1];
			mag0[2]+=magCal[i*3+2];
		}
		mag0[0]/=CALIBRATION;
		mag0[1]/=CALIBRATION;
		mag0[2]/=CALIBRATION;
		
		// Sum up for variance calculation
		for (int i=0;i<CALIBRATION;i++){
			Rmag[0]+=pow((magCal[i*3] - mag0[0]), 2);
			Rmag[4]+=pow((magCal[i*3+1] - mag0[1]), 2);
			Rmag[8]+=pow((magCal[i*3+2] - mag0[2]), 2);
		}
		// Variance (sigma)
		Rmag[0]/=CALIBRATION;
		Rmag[4]/=CALIBRATION;
		Rmag[8]/=CALIBRATION;
		
		// Print results
		printf("Mean (bias) magnetometer\n");
		printmat(mag0,3,1);
		printf("Covariance (sigma) magnetometer\n");
		printmat(Rmag,3,3);
	}
	// Default i save calibrartion data
	else{
		magCal[counterCal*3]=ymag[0];
		magCal[counterCal*3+1]=ymag[1];
		magCal[counterCal*3+2]=ymag[2];
	}		
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

// Low Pass Filter 24 order
void lowPassFilter(double *accRaw, double* gyrRaw, double *accRawMem, double* gyrRawMem, double* b_acc, double* b_gyr){
	// Shift all old data in measurement memory by one element: new -> [,,,,] -> old
	for (int k = 24; k > 0; k--){        
		accRawMem[k]=accRawMem[k-1];		// x-axis
		accRawMem[k+25]=accRawMem[k-1+25];	// y-axis
		accRawMem[k+50]=accRawMem[k-1+50];	// z-axis
		gyrRawMem[k]=gyrRawMem[k-1];		// x-axis
		gyrRawMem[k+25]=gyrRawMem[k-1+25];	// y-axis
		gyrRawMem[k+50]=gyrRawMem[k-1+50];	// z-axis
	}
	
	// Assign fresh non filtered measurement to memory
	accRawMem[0]=accRaw[0];		// x-axis
	accRawMem[0+25]=accRaw[1];	// y-axis
	accRawMem[0+50]=accRaw[2];	// z-axis
	gyrRawMem[0]=gyrRaw[0];		// x-axis
	gyrRawMem[0+25]=gyrRaw[1];	// y-axis
	gyrRawMem[0+50]=gyrRaw[2];	// z-axis
	
	// Zero out measurement before adding filtered data to
	accRaw[0]=0;
	accRaw[1]=0;
	accRaw[2]=0;
	gyrRaw[0]=0;
	gyrRaw[1]=0;
	gyrRaw[2]=0;
	
	// Filter the data
	for(int i=0;i<25;i++){
		accRaw[0]+=b_acc[i]*accRawMem[i];		// x-axis
		accRaw[1]+=b_acc[i]*accRawMem[i+25];	// y-axis
		accRaw[2]+=b_acc[i]*accRawMem[i+50];	// z-axis
		gyrRaw[0]+=b_gyr[i]*gyrRawMem[i];		// x-axis
		gyrRaw[1]+=b_gyr[i]*gyrRawMem[i+25];	// y-axis
		gyrRaw[2]+=b_gyr[i]*gyrRawMem[i+50];	// z-axis

		// ORIGINAL y(k) = y(k) + b(i)*acc_x(k-i);
	}
}

// Low Pass Filter 24 order for Gyro z
void lowPassFilterGyroZ(double* gyrRaw, double* gyrRawMem, double* b_gyr){
	// Shift all old data in measurement memory by one element: new -> [,,,,] -> old
	for (int k = 24; k > 0; k--){        
		//gyrRawMem[k]=gyrRawMem[k-1];		// x-axis
		//gyrRawMem[k+25]=gyrRawMem[k-1+25];	// y-axis
		gyrRawMem[k+50]=gyrRawMem[k-1+50];	// z-axis
	}
	
	// Assign fresh non filtered measurement to memory
	//gyrRawMem[0]=gyrRaw[0];		// x-axis
	//gyrRawMem[0+25]=gyrRaw[1];	// y-axis
	gyrRawMem[0+50]=gyrRaw[2];	// z-axis
	
	// Zero out measurement before adding filtered data to
	//gyrRaw[0]=0;
	//gyrRaw[1]=0;
	gyrRaw[2]=0;
	
	// Filter the data
	for(int i=0;i<25;i++){
		//gyrRaw[0]+=b_gyr[i]*gyrRawMem[i];		// x-axis
		//gyrRaw[1]+=b_gyr[i]*gyrRawMem[i+25];	// y-axis
		gyrRaw[2]+=b_gyr[i]*gyrRawMem[i+50];	// z-axis

		// ORIGINAL y(k) = y(k) + b(i)*acc_x(k-i);
	}
}

// Madgwick filter implementation
// Implementation of Sebastian Madgwick's "...efficient orientation filter for... inertial/magnetic sensor arrays"
// (see http://www.x-io.co.uk/category/open-source/ for examples and more details)
// which fuses acceleration, rotation rate, and magnetic moments to produce a quaternion-based estimate of absolute
// device orientation -- which can be converted to yaw, pitch, and roll. Useful for stabilizing quadcopters, etc.
// The performance of the orientation filter is at least as good as conventional Kalman-based filtering algorithms
// but is much less computationally intensive---it can be performed on a 3.3 V Pro Mini operating at 8 MHz!
void MadgwickQuaternionUpdate(float ax, float ay, float az, float gx, float gy, float gz, float mx, float my, float mz, float *q, float deltat){
	// float q[0] = q[0], q[1] = q[1], q[2] = q[2], q[3] = q[3];   // short name local variable for readability
	float norm;
	float hx, hy, _2bx, _2bz;
	float s1, s2, s3, s4;
	float qDot1, qDot2, qDot3, qDot4;

	// Auxiliary variables to avoid repeated arithmetic
	float _2q1mx;
	float _2q1my;
	float _2q1mz;
	float _2q2mx;
	float _4bx;
	float _4bz;
	float _2q1 = 2.0f * q[0];
	float _2q2 = 2.0f * q[1];
	float _2q3 = 2.0f * q[2];
	float _2q4 = 2.0f * q[3];
	float _2q1q3 = 2.0f * q[0] * q[2];
	float _2q3q4 = 2.0f * q[2] * q[3];
	float q1q1 = q[0] * q[0];
	float q1q2 = q[0] * q[1];
	float q1q3 = q[0] * q[2];
	float q1q4 = q[0] * q[3];
	float q2q2 = q[1] * q[1];
	float q2q3 = q[1] * q[2];
	float q2q4 = q[1] * q[3];
	float q3q3 = q[2] * q[2];
	float q3q4 = q[2] * q[3];
	float q4q4 = q[3] * q[3];

	// Normalise accelerometer measurement
	norm = sqrt(ax * ax + ay * ay + az * az);
	if (norm == 0.0f) return; // handle NaN
	norm = 1.0f/norm;
	ax *= norm;
	ay *= norm;
	az *= norm;

	// Normalise magnetometer measurement
	norm = sqrt(mx * mx + my * my + mz * mz);
	if (norm == 0.0f) return; // handle NaN
	norm = 1.0f/norm;
	mx *= norm;
	my *= norm;
	mz *= norm;

	// Reference direction of Earth's magnetic field
	_2q1mx = 2.0f * q[0] * mx;
	_2q1my = 2.0f * q[0] * my;
	_2q1mz = 2.0f * q[0] * mz;
	_2q2mx = 2.0f * q[1] * mx;
	hx = mx * q1q1 - _2q1my * q[3] + _2q1mz * q[2] + mx * q2q2 + _2q2 * my * q[2] + _2q2 * mz * q[3] - mx * q3q3 - mx * q4q4;
	hy = _2q1mx * q[3] + my * q1q1 - _2q1mz * q[1] + _2q2mx * q[2] - my * q2q2 + my * q3q3 + _2q3 * mz * q[3] - my * q4q4;
	_2bx = sqrt(hx * hx + hy * hy);
	_2bz = -_2q1mx * q[2] + _2q1my * q[1] + mz * q1q1 + _2q2mx * q[3] - mz * q2q2 + _2q3 * my * q[3] - mz * q3q3 + mz * q4q4;
	_4bx = 2.0f * _2bx;
	_4bz = 2.0f * _2bz;

	// Gradient decent algorithm corrective step
	s1 = -_2q3 * (2.0f * q2q4 - _2q1q3 - ax) + _2q2 * (2.0f * q1q2 + _2q3q4 - ay) - _2bz * q[2] * (_2bx * (0.5f - q3q3 - q4q4) + _2bz * (q2q4 - q1q3) - mx) + (-_2bx * q[3] + _2bz * q[1]) * (_2bx * (q2q3 - q1q4) + _2bz * (q1q2 + q3q4) - my) + _2bx * q[2] * (_2bx * (q1q3 + q2q4) + _2bz * (0.5f - q2q2 - q3q3) - mz);
	s2 = _2q4 * (2.0f * q2q4 - _2q1q3 - ax) + _2q1 * (2.0f * q1q2 + _2q3q4 - ay) - 4.0f * q[1] * (1.0f - 2.0f * q2q2 - 2.0f * q3q3 - az) + _2bz * q[3] * (_2bx * (0.5f - q3q3 - q4q4) + _2bz * (q2q4 - q1q3) - mx) + (_2bx * q[2] + _2bz * q[0]) * (_2bx * (q2q3 - q1q4) + _2bz * (q1q2 + q3q4) - my) + (_2bx * q[3] - _4bz * q[1]) * (_2bx * (q1q3 + q2q4) + _2bz * (0.5f - q2q2 - q3q3) - mz);
	s3 = -_2q1 * (2.0f * q2q4 - _2q1q3 - ax) + _2q4 * (2.0f * q1q2 + _2q3q4 - ay) - 4.0f * q[2] * (1.0f - 2.0f * q2q2 - 2.0f * q3q3 - az) + (-_4bx * q[2] - _2bz * q[0]) * (_2bx * (0.5f - q3q3 - q4q4) + _2bz * (q2q4 - q1q3) - mx) + (_2bx * q[1] + _2bz * q[3]) * (_2bx * (q2q3 - q1q4) + _2bz * (q1q2 + q3q4) - my) + (_2bx * q[0] - _4bz * q[2]) * (_2bx * (q1q3 + q2q4) + _2bz * (0.5f - q2q2 - q3q3) - mz);
	s4 = _2q2 * (2.0f * q2q4 - _2q1q3 - ax) + _2q3 * (2.0f * q1q2 + _2q3q4 - ay) + (-_4bx * q[3] + _2bz * q[1]) * (_2bx * (0.5f - q3q3 - q4q4) + _2bz * (q2q4 - q1q3) - mx) + (-_2bx * q[0] + _2bz * q[2]) * (_2bx * (q2q3 - q1q4) + _2bz * (q1q2 + q3q4) - my) + _2bx * q[1] * (_2bx * (q1q3 + q2q4) + _2bz * (0.5f - q2q2 - q3q3) - mz);
	norm = sqrt(s1 * s1 + s2 * s2 + s3 * s3 + s4 * s4);    // normalise step magnitude
	norm = 1.0f/norm;
	s1 *= norm;
	s2 *= norm;
	s3 *= norm;
	s4 *= norm;

	// Compute rate of change of quaternion
	qDot1 = 0.5f * (-q[1] * gx - q[2] * gy - q[3] * gz) - beta * s1;
	qDot2 = 0.5f * (q[0] * gx + q[2] * gz - q[3] * gy) - beta * s2;
	qDot3 = 0.5f * (q[0] * gy - q[1] * gz + q[3] * gx) - beta * s3;
	qDot4 = 0.5f * (q[0] * gz + q[1] * gy - q[2] * gx) - beta * s4;

	// Integrate to yield quaternion
	q[0] += qDot1 * deltat;
	q[1] += qDot2 * deltat;
	q[2] += qDot3 * deltat;
	q[3] += qDot4 * deltat;
	norm = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);    // normalise quaternion
	norm = 1.0f/norm;
	q[0] = q[0] * norm;
	q[1] = q[1] * norm;
	q[2] = q[2] * norm;
	q[3] = q[3] * norm;

}


// Mahony filter implementation
// Similar to Madgwick scheme but uses proportional and integral filtering on the error between estimated reference vectors and
 // measured ones.            
 void MahonyQuaternionUpdate(float ax, float ay, float az, float gx, float gy, float gz, float mx, float my, float mz, float *q, float deltat, float Kp, float Ki, float *eInt){
	//float q[0] = q[0], q[1] = q[1], q[2] = q[2], q[3] = q[3];   // short name local variable for readability
	float norm;
	float hx, hy, bx, bz;
	float vx, vy, vz, wx, wy, wz;
	float ex, ey, ez;
	float pa, pb, pc;

	// Auxiliary variables to avoid repeated arithmetic
	float q1q1 = q[0] * q[0];
	float q1q2 = q[0] * q[1];
	float q1q3 = q[0] * q[2];
	float q1q4 = q[0] * q[3];
	float q2q2 = q[1] * q[1];
	float q2q3 = q[1] * q[2];
	float q2q4 = q[1] * q[3];
	float q3q3 = q[2] * q[2];
	float q3q4 = q[2] * q[3];
	float q4q4 = q[3] * q[3];   

	// Normalise accelerometer measurement
	norm = sqrt(ax * ax + ay * ay + az * az);
	if (norm == 0.0f) return; // handle NaN
	norm = 1.0f / norm;        // use reciprocal for division
	ax *= norm;
	ay *= norm;
	az *= norm;

	// Normalise magnetometer measurement
	norm = sqrt(mx * mx + my * my + mz * mz);
	if (norm == 0.0f) return; // handle NaN
	norm = 1.0f / norm;        // use reciprocal for division
	mx *= norm;
	my *= norm;
	mz *= norm;

	// Reference direction of Earth's magnetic field
	hx = 2.0f * mx * (0.5f - q3q3 - q4q4) + 2.0f * my * (q2q3 - q1q4) + 2.0f * mz * (q2q4 + q1q3);
	hy = 2.0f * mx * (q2q3 + q1q4) + 2.0f * my * (0.5f - q2q2 - q4q4) + 2.0f * mz * (q3q4 - q1q2);
	bx = sqrt((hx * hx) + (hy * hy));
	bz = 2.0f * mx * (q2q4 - q1q3) + 2.0f * my * (q3q4 + q1q2) + 2.0f * mz * (0.5f - q2q2 - q3q3);

	// Estimated direction of gravity and magnetic field
	vx = 2.0f * (q2q4 - q1q3);
	vy = 2.0f * (q1q2 + q3q4);
	vz = q1q1 - q2q2 - q3q3 + q4q4;
	wx = 2.0f * bx * (0.5f - q3q3 - q4q4) + 2.0f * bz * (q2q4 - q1q3);
	wy = 2.0f * bx * (q2q3 - q1q4) + 2.0f * bz * (q1q2 + q3q4);
	wz = 2.0f * bx * (q1q3 + q2q4) + 2.0f * bz * (0.5f - q2q2 - q3q3);  

	// Error is cross product between estimated direction and measured direction of gravity
	ex = (ay * vz - az * vy) + (my * wz - mz * wy);
	ey = (az * vx - ax * vz) + (mz * wx - mx * wz);
	ez = (ax * vy - ay * vx) + (mx * wy - my * wx);
	if (Ki > 0.0f)
	{
		eInt[0] += ex;      // accumulate integral error
		eInt[1] += ey;
		eInt[2] += ez;
	}
	else
	{
		eInt[0] = 0.0f;     // prevent integral wind up
		eInt[1] = 0.0f;
		eInt[2] = 0.0f;
	}

	// Apply feedback terms
	gx = gx + Kp * ex + Ki * eInt[0];
	gy = gy + Kp * ey + Ki * eInt[1];
	gz = gz + Kp * ez + Ki * eInt[2];

	// Integrate rate of change of quaternion
	pa = q[1];
	pb = q[2];
	pc = q[3];
	q[0] = q[0] + (-q[1] * gx - q[2] * gy - q[3] * gz) * (0.5f * deltat);
	q[1] = pa + (q[0] * gx + pb * gz - pc * gy) * (0.5f * deltat);
	q[2] = pb + (q[0] * gy - pa * gz + pc * gx) * (0.5f * deltat);
	q[3] = pc + (q[0] * gz + pa * gy - pb * gx) * (0.5f * deltat);

	// Normalise quaternion
	norm = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	norm = 1.0f / norm;
	q[0] = q[0] * norm;
	q[1] = q[1] * norm;
	q[2] = q[2] * norm;
	q[3] = q[3] * norm;

}
