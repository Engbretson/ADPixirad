/* pixirad.cpp
 *
 * This is a driver for the PiXirad pixel array detector.
 * It communicates directly with the PiXirad box, it does not use the LabView TCP/IP server.
 *
 * Author: Mark Rivers
 *         University of Chicago
 *
 * Created:  January 11, 2014
 *
 */
 
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#endif

#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsEvent.h>
#include <epicsString.h>
#include <epicsStdio.h>
#include <epicsMutex.h>
#include <osiSock.h>
#include <iocsh.h>
#include <epicsExport.h>

#include <asynOctetSyncIO.h>

#include "ADDriver.h"
#include "PIXIEII_data_utilities.h"

/** Messages to/from server */
#define MAX_MESSAGE_SIZE 256 
#define SERVER_DEFAULT_TIMEOUT 1.0
/** Additional time to wait for a server response after the acquire should be complete */ 
#define SERVER_ACQUIRE_TIMEOUT 10.

#define MAX_UDP_DATA_BUFFER     256217728
#define MAX_UDP_PACKET_LEN           1448
#define DEFAULT_UDP_NUM_PACKETS       360
#define DAQ_PACKET_FRAGMENT            45
#define AUTOCAL_DATA                 0x40
#define AUTOCAL_NUM_PACKETS           135
#define PACKET_ID_OFFSET                2
#define PACKET_ID_BYTES                 2
#define PACKET_CRC_BYTES                4
#define PACKET_TAG_BYTES                2
#define PACKET_TAG_OFFSET               0
#define FRAME_HAS_ALIGN_ERRORS       0x20
#define PACKET_SENSOR_DATA_OFFSET    (PACKET_TAG_BYTES + PACKET_ID_BYTES)
#define PACKET_EXTRA_BYTES           (PACKET_ID_BYTES + PACKET_TAG_BYTES + PACKET_CRC_BYTES)
#define PACKET_SENSOR_DATA_BYTES     (MAX_UDP_PACKET_LEN - PACKET_EXTRA_BYTES)
#define PIXIEII_MODULES                 1
#define PIXIE_THDAC_OFFSET              0
#define DUMMY_0_OFFSET                  0
#define PIXIE_THDAC_MASK             0x1f
#define PIXIE_THDAC_OFFSET              0
#define DUMMY_1_OFFSET                  8

/** Save data */
typedef enum {
    SaveDataOff,
    SaveDataOn
} PixiradSaveDataState_t;

/** Trigger modes */
typedef enum {
    TMInternal,
    TMExternal,
    TMBulb
} PixiradTriggerMode_t;
static const char *PixiradTriggerModeStrings[] = {"INT", "EXT1", "EXT2"};

/** Cooling state */
typedef enum {
    CoolingOff,
    CoolingOn
} PixiradCoolingState_t;

/** High voltage state */
typedef enum {
    HVOff,
    HVOn
} PixiradHVState_t;

/** High voltage mode */
typedef enum {
    HVManual,
    HVAuto
} PixiradHVMode_t;
static const char *PixiradHVModeStrings[] = {"STDHV", "AUTOHV"};

/** Sync in/out polarity */
typedef enum {
    SyncPos,
    SyncNeg
} PixiradSyncPolarity_t;
static const char *PixiradSyncPolarityStrings[] = {"POS", "NEG"};

/** Sync out function */
typedef enum {
    SyncOutShutter,
    SyncOutReadoutDone,
    SyncOutRead
} PixiradSyncOutFunction_t;
static const char *PixiradSyncOutFunctionStrings[] = {"SHUTTER", "RODONE", "READ"};

/** Download speed */
typedef enum {
    SpeedHigh,
    SpeedLow
} PixiradDownloadSpeed_t;
static const char *PixiradDownloadSpeedStrings[] = {"UNMOD", "MOD"};

/** Collection mode */
typedef enum {
    CMOneColorLow,
    CMOneColorHigh,
    CMTwoColors,
    CMFourColors,
    CMOneColorDTF,
    CMTwoColorsDTF
} PixiradCollectionMode_t;
static const char *PixiradCollectionModeStrings[] = {"1COL0", "1COL1", "2COL", "4COL", "DTF", "2COLDTF"};

static const char *driverName = "pixirad";

#define PixiradCollectionModeString  "COLLECTION_MODE"
#define PixiradSaveDataString        "SAVE_DATA"
#define PixiradThreshold1String      "THRESHOLD1"
#define PixiradThreshold2String      "THRESHOLD2"
#define PixiradThreshold3String      "THRESHOLD3"
#define PixiradThreshold4String      "THRESHOLD4"
#define PixiradAutoCalibrateString   "AUTO_CALIBRATE"
#define PixiradHVValueString         "HV_VALUE"
#define PixiradHVStateString         "HV_STATE"
#define PixiradHVModeString          "HV_MODE"
#define PixiradHVActualString        "HV_ACTUAL"
#define PixiradHVCurrentString       "HV_CURRENT"
#define PixiradSyncInPolarityString  "SYNC_IN_POLARITY"
#define PixiradSyncOutPolarityString "SYNC_OUT_POLARITY"
#define PixiradSyncOutFunctionString "SYNC_OUT_FUNCTION"
#define PixiradDownloadSpeedString   "DOWNLOAD_SPEED"
#define PixiradImageFileTmotString   "IMAGE_FILE_TMOT"
#define PixiradCoolingStateString    "COOLING_STATE"
#define PixiradHotTemperatureString  "HOT_TEMPERATURE"
#define PixiradBoxTemperatureString  "BOX_TEMPERATURE"
#define PixiradBoxHumidityString     "BOX_HUMIDITY"
#define PixiradDewPointString        "DEW_POINT"
#define PixiradPeltierPowerString    "PELTIER_POWER"
#define PixiradAutoCalibrateString   "AUTO_CALIBRATE"

#define INITIAL_HV_VALUE 350
#define INITIAL_HV_STATE HVOn
#define INITIAL_HV_MODE HVAuto
#define INITIAL_COOLING_VALUE 15
#define INITIAL_COOLING_STATE CoolingOn

/** Driver for PiXirad pixel array detectors using their server server over TCP/IP socket */
class pixirad : public ADDriver {
public:
    pixirad(const char *portName, const char *commandPortName,
            int dataPortNumber, int statusPortnumber, int maxDataPortBuffers,
            int maxSizeX, int maxSizeY,
            int maxBuffers, size_t maxMemory,
            int priority, int stackSize);
                 
    /* These are the methods that we override from ADDriver */
    virtual asynStatus writeInt32(asynUser *pasynUser, epicsInt32 value);
    virtual asynStatus writeFloat64(asynUser *pasynUser, epicsFloat64 value);
    void report(FILE *fp, int details);
    /* These should be private but are called from C so must be public */
    void statusTask();
    void dataTask();
    void udpDataListenerTask();
    
protected:
    int PixiradCollectionMode;
    #define FIRST_PIXIRAD_PARAM PixiradCollectionMode
    int PixiradSaveData;
    int PixiradThreshold1;
    int PixiradThreshold2;
    int PixiradThreshold3;
    int PixiradThreshold4;
    int PixiradAutoCalibrate;
    int PixiradHVValue;
    int PixiradHVState;
    int PixiradHVMode;
    int PixiradHVActual;
    int PixiradHVCurrent;
    int PixiradSyncInPolarity;
    int PixiradSyncOutPolarity;
    int PixiradSyncOutFunction;
    int PixiradDownloadSpeed;
    int PixiradImageFileTmot;
    int PixiradCoolingState;
    int PixiradHotTemperature;
    int PixiradBoxTemperature;
    int PixiradBoxHumidity;
    int PixiradDewPoint;
    int PixiradPeltierPower;
    #define LAST_PIXIRAD_PARAM PixiradPeltierPower

private:                                       
    /* These are the methods that are new to this class */
    asynStatus writeReadServer(double timeout);
    asynStatus setCoolingAndHV();
    asynStatus setThresholds();
    asynStatus doAutoCalibrate();
    asynStatus setSync();
    asynStatus startAcquire();
    asynStatus stopAcquire();
   
    /* Our data */
    epicsMessageQueueId dataMessageQueueId_;
    int dataPortNumber_;
    int statusPortNumber_;
    int maxDataPortBuffers_;
    char toServer_[MAX_MESSAGE_SIZE];
    char fromServer_[MAX_MESSAGE_SIZE];
    asynUser *pasynUserCommand_;
};

#define NUM_PIXIRAD_PARAMS ((int)(&LAST_PIXIRAD_PARAM - &FIRST_PIXIRAD_PARAM + 1))

static void statusTaskC(void *drvPvt)
{
    pixirad *pPvt = (pixirad *)drvPvt;
    
    pPvt->statusTask();
}

static void dataTaskC(void *drvPvt)
{
    pixirad *pPvt = (pixirad *)drvPvt;
    
    pPvt->dataTask();
}

static void udpDataListenerTaskC(void *drvPvt)
{
    pixirad *pPvt = (pixirad *)drvPvt;
    
    pPvt->udpDataListenerTask();
}

static int convert_bit_stream_to_counts(int code_depth, unsigned short* source_memory_offset,
                                        unsigned short* destination_memory_offset, int reusulting_readings)
{
    int i,j;
    unsigned short dout_masks[PIXIE_DOUTS], mask_seed=1;
    for(i=0;i<reusulting_readings;i++) dout_masks[i]=(mask_seed<<i);
    for(j=0;j<reusulting_readings;j++){
        destination_memory_offset[j]=0;
        for(i=code_depth-1;i>=0;i--){
            if(source_memory_offset[i] & dout_masks[j])
                destination_memory_offset[j]|= dout_masks[code_depth-i-1];
            else
                destination_memory_offset[j]&= ~dout_masks[code_depth-i-1];
        }
    }
    return(j);
}

static void my_bytes_swap(unsigned short* us_ptr)
{
    char a, b, *temp_char_ptr;
    temp_char_ptr = (char*)us_ptr;
    a = *temp_char_ptr;
    b = *(temp_char_ptr+1);
    *(temp_char_ptr+1) = a;
    *(temp_char_ptr) = b;
}



/** Constructor for Pixirad driver; most parameters are simply passed to ADDriver::ADDriver.
  * After calling the base class constructor this method creates a thread to collect the detector data, 
  * and sets reasonable default values for the parameters defined in this class, asynNDArrayDriver, and ADDriver.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] serverPort The name of the asyn port previously created with drvAsynIPPortConfigure to
  *            communicate with the PiXirad server.
  * \param[in] maxSizeX The size of the Pixirad detector in the X direction.
  * \param[in] maxSizeY The size of the Pixirad detector in the Y direction.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] maxBuffers The maximum number of NDArray buffers that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited number of buffers.
  * \param[in] maxMemory The maximum amount of memory that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited amount of memory.
  * \param[in] priority The thread priority for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  * \param[in] stackSize The stack size for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  */
pixirad::pixirad(const char *portName, const char *commandPortName,
                 int dataPortNumber, int statusPortNumber, int maxDataPortBuffers,
                 int maxSizeX, int maxSizeY,
                 int maxBuffers, size_t maxMemory,
                 int priority, int stackSize)

    : ADDriver(portName, 1, NUM_PIXIRAD_PARAMS, maxBuffers, maxMemory,
               0, 0,             /* No interfaces beyond those set in ADDriver.cpp */
               ASYN_CANBLOCK, 1, /* ASYN_CANBLOCK=1, ASYN_MULTIDEVICE=0, autoConnect=1 */
               priority, stackSize)

{
    int status = asynSuccess;
    const char *functionName = "pixirad";

    dataPortNumber_ = dataPortNumber;
    statusPortNumber_ = statusPortNumber;
    maxDataPortBuffers_ = maxDataPortBuffers;

    dataMessageQueueId_ = epicsMessageQueueCreate(maxDataPortBuffers_, sizeof(unsigned char *));
    if (dataMessageQueueId_ == 0) {
        printf("%s:%s: epicsMessageQueueCreate failed\n",
            driverName, functionName);
        return;
    }

    if (osiSockAttach() == 0) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s:%s: osiSockAttach failed\n",
            driverName, functionName);
        return;
    }

    /* Connect to Pixirad server */
    status = pasynOctetSyncIO->connect(commandPortName, 0, &pasynUserCommand_, NULL);

    createParam(PixiradCollectionModeString,  asynParamInt32,   &PixiradCollectionMode);
    createParam(PixiradSaveDataString,        asynParamInt32,   &PixiradSaveData);
    createParam(PixiradThreshold1String,      asynParamFloat64, &PixiradThreshold1);
    createParam(PixiradThreshold2String,      asynParamFloat64, &PixiradThreshold2);
    createParam(PixiradThreshold3String,      asynParamFloat64, &PixiradThreshold3);
    createParam(PixiradThreshold4String,      asynParamFloat64, &PixiradThreshold4);
    createParam(PixiradAutoCalibrateString,   asynParamInt32,   &PixiradAutoCalibrate);
    createParam(PixiradHVValueString,         asynParamFloat64, &PixiradHVValue);
    createParam(PixiradHVStateString,         asynParamInt32,   &PixiradHVState);
    createParam(PixiradHVModeString,          asynParamInt32,   &PixiradHVMode);
    createParam(PixiradHVActualString,        asynParamFloat64, &PixiradHVActual);
    createParam(PixiradHVCurrentString,       asynParamFloat64, &PixiradHVCurrent);
    createParam(PixiradSyncInPolarityString,  asynParamInt32,   &PixiradSyncInPolarity);
    createParam(PixiradSyncOutPolarityString, asynParamInt32,   &PixiradSyncOutPolarity);
    createParam(PixiradSyncOutFunctionString, asynParamInt32,   &PixiradSyncOutFunction);
    createParam(PixiradDownloadSpeedString,   asynParamInt32,   &PixiradDownloadSpeed);
    createParam(PixiradImageFileTmotString,   asynParamFloat64, &PixiradImageFileTmot);
    createParam(PixiradCoolingStateString,    asynParamInt32,   &PixiradCoolingState);
    createParam(PixiradHotTemperatureString,  asynParamFloat64, &PixiradHotTemperature);
    createParam(PixiradBoxTemperatureString,  asynParamFloat64, &PixiradBoxTemperature);
    createParam(PixiradBoxHumidityString,     asynParamFloat64, &PixiradBoxHumidity);
    createParam(PixiradDewPointString,        asynParamFloat64, &PixiradDewPoint);
    createParam(PixiradPeltierPowerString,    asynParamFloat64, &PixiradPeltierPower);

    /* Set some default values for parameters */
    status =  setStringParam (ADManufacturer, "Pixirad");
    status |= setStringParam (ADModel, "Pixirad 1");
    status |= setIntegerParam(ADMaxSizeX, maxSizeX);
    status |= setIntegerParam(ADMaxSizeY, maxSizeY);
    status |= setIntegerParam(ADSizeX, maxSizeX);
    status |= setIntegerParam(ADSizeX, maxSizeX);
    status |= setIntegerParam(ADSizeY, maxSizeY);
    status |= setIntegerParam(NDArraySizeX, maxSizeX);
    status |= setIntegerParam(NDArraySizeY, maxSizeY);
    status |= setIntegerParam(NDArraySize, 0);
    status |= setIntegerParam(NDDataType,  NDUInt16);
    status |= setIntegerParam(ADNumImages, 1);
    status |= setIntegerParam(ADImageMode, ADImageContinuous);
    status |= setIntegerParam(ADTriggerMode, TMInternal);
    status |= setDoubleParam(ADAcquireTime, 1.0);
    status |= setDoubleParam(ADAcquirePeriod, 1.0);
    status |= setDoubleParam(ADTemperature, INITIAL_COOLING_VALUE);
    status |= setIntegerParam(PixiradCoolingState, INITIAL_COOLING_STATE);
    status |= setDoubleParam(PixiradHVValue, INITIAL_HV_VALUE);
    status |= setIntegerParam(PixiradHVState, INITIAL_HV_STATE);
    status |= setIntegerParam(PixiradHVMode, INITIAL_HV_MODE);
    status |= setIntegerParam(PixiradDownloadSpeed, SpeedHigh);
    status |= setDoubleParam(PixiradThreshold1, 10.0);
    status |= setDoubleParam(PixiradThreshold2, 15.0);
    status |= setDoubleParam(PixiradThreshold3, 20.0);
    status |= setDoubleParam(PixiradThreshold4, 25.0);
    status |= setIntegerParam(PixiradCollectionMode, CMOneColorLow);
    status |= setIntegerParam(PixiradSyncInPolarity, SyncPos);
    status |= setIntegerParam(PixiradSyncOutPolarity, SyncPos);
    status |= setIntegerParam(PixiradSyncOutFunction, SyncOutShutter);
    
    // There is a bug in the current firmware.  
    // Two different HV values must be sent or it won't accept it.
    setDoubleParam(PixiradHVValue, INITIAL_HV_VALUE - 1.0);
    setCoolingAndHV();
    setDoubleParam(PixiradHVValue, INITIAL_HV_VALUE);
    setCoolingAndHV();
    
    if (status) {
        printf("%s:%s: unable to set camera parameters\n", driverName, functionName);
        return;
    }
    
    /* Create the thread that receives status broadcasts for temperature, etc. */
    epicsThreadCreate("PixiradStatusTask",
                       epicsThreadPriorityMedium,
                       epicsThreadGetStackSize(epicsThreadStackMedium),
                       (EPICSTHREADFUNC)statusTaskC, this);

    /* Create the thread that gets messages from the UDP data task and does NDArray callbacks */
    epicsThreadCreate("PixiradDataTask",
                       epicsThreadPriorityMedium,
                       epicsThreadGetStackSize(epicsThreadStackMedium),
                       (EPICSTHREADFUNC)dataTaskC, this);

    /* Create the thread that reads UDP data from the detector */
    epicsThreadCreate("PixiradUDPDataTask",
                       epicsThreadPriorityMedium,
                       epicsThreadGetStackSize(epicsThreadStackMedium),
                       (EPICSTHREADFUNC)udpDataListenerTaskC, this);

}

asynStatus pixirad::setCoolingAndHV()
{
    double coolingValue;
    int coolingState;
    double HVValue;
    int HVState;
    asynStatus status;
    
    getDoubleParam(ADTemperature, &coolingValue);
    getIntegerParam(PixiradCoolingState, &coolingState);
    getDoubleParam(PixiradHVValue, &HVValue);
    getIntegerParam(PixiradHVState, &HVState);
    
    epicsSnprintf(toServer_, sizeof(toServer_), 
                  "DAQ:! INIT %.1f %d %.1f %d", 
                  coolingValue, coolingState, HVValue, HVState);
    status = writeReadServer(1.0);
    return status;
}
  
asynStatus pixirad::setSync()
{
    int syncInPolarity;
    int syncOutPolarity;
    int syncOutFunction;
    asynStatus status;
    
    getIntegerParam(PixiradSyncInPolarity, &syncInPolarity);
    getIntegerParam(PixiradSyncOutPolarity, &syncOutPolarity);
    getIntegerParam(PixiradSyncOutFunction, &syncOutFunction);
    
    epicsSnprintf(toServer_, sizeof(toServer_), 
                  "DAQ:! SET_SYNC %s %s %s", 
                  PixiradSyncPolarityStrings[syncInPolarity],
                  PixiradSyncPolarityStrings[syncOutPolarity],
                  PixiradSyncOutFunctionStrings[syncOutFunction]);
    status = writeReadServer(1.0);
    return status;
}
    
asynStatus pixirad::setThresholds()
{
    double thresholdEnergy[4];
    double actualThresholdEnergy[4];
    int thresholdReg[4];
    int i, vthMax, ref=2, auFS=7;
    int collectionMode;
    asynStatus status;
    const char *dtf, *nbi;
    
    getIntegerParam(PixiradCollectionMode, &collectionMode);
    
    if ((collectionMode == CMOneColorDTF) ||
        (collectionMode == CMTwoColorsDTF))
        dtf = "DTF";
    else
        dtf = "NODTF";

    nbi = "NBI"; 

    getDoubleParam(PixiradThreshold1, &thresholdEnergy[0]);
    getDoubleParam(PixiradThreshold2, &thresholdEnergy[1]);
    getDoubleParam(PixiradThreshold3, &thresholdEnergy[2]);
    getDoubleParam(PixiradThreshold4, &thresholdEnergy[3]);
    
    // Calculate the optimum value of vthMax for the first threshold
    vthMax = 1500;
    
    for (i=0; i<4; i++) {
        thresholdReg[i] = (int) thresholdEnergy[i];
        actualThresholdEnergy[i] = thresholdReg[i];
    }
    
    setDoubleParam(PixiradThreshold1, actualThresholdEnergy[0]);
    setDoubleParam(PixiradThreshold2, actualThresholdEnergy[1]);
    setDoubleParam(PixiradThreshold3, actualThresholdEnergy[2]);
    setDoubleParam(PixiradThreshold4, actualThresholdEnergy[3]);
    
    epicsSnprintf(toServer_, sizeof(toServer_), 
                  "DAQ:! SET_SENSOR_OPERATINGS %d %d %d %d %d %d %d %s %s", 
                  thresholdReg[3], thresholdReg[2], thresholdReg[1], thresholdReg[0],
                  vthMax, ref, auFS, dtf, nbi);
    status = writeReadServer(1.0);
    return status;
}
    
asynStatus pixirad::doAutoCalibrate()
{
    asynStatus status;
    
    epicsSnprintf(toServer_, sizeof(toServer_), 
                  "DAQ:! AUTOCAL");
    status = writeReadServer(1.0);
    return status;
}
    
asynStatus pixirad::startAcquire()
{
    int numImages;
    double acquireTime;
    double acquirePeriod;
    double shutterPause;
    int collectionMode;
    int triggerMode;
    int downloadSpeed;
    int HVMode;
    asynStatus status;
    
    getIntegerParam(ADNumImages, &numImages);
    getDoubleParam(ADAcquireTime, &acquireTime);
    getDoubleParam(ADAcquirePeriod, &acquirePeriod);
    shutterPause = acquirePeriod - acquireTime;
    if (shutterPause < 0.) shutterPause = 0;
    getIntegerParam(PixiradCollectionMode, &collectionMode);
    getIntegerParam(ADTriggerMode, &triggerMode);
    getIntegerParam(PixiradDownloadSpeed, &downloadSpeed);
    getIntegerParam(PixiradHVMode, &HVMode);
    setIntegerParam(ADNumImagesCounter, 0);
   
    epicsSnprintf(toServer_, sizeof(toServer_), 
                  "DAQ:! LOOP %d %d %d %s %s %s %s",
                  numImages, 
                  (int)(acquireTime*1000),
                  int(shutterPause*1000),
                  PixiradCollectionModeStrings[collectionMode],
                  PixiradTriggerModeStrings[triggerMode],
                  PixiradDownloadSpeedStrings[downloadSpeed],
                  PixiradHVModeStrings[HVMode]);
    status = writeReadServer(1.0);
    return status;
}
    
asynStatus pixirad::stopAcquire()
{
    asynStatus status;
    
    epicsSnprintf(toServer_, sizeof(toServer_), 
                  "DAQ:!!ACQUISITIONBREAK");
    status = writeReadServer(1.0);
    return status;
}
    

asynStatus pixirad::writeReadServer(double timeout)
{
    size_t nwrite, nread;
    asynStatus status=asynSuccess;
    asynUser *pasynUser = pasynUserCommand_;
    int eomReason;
    const char *functionName="writeReadServer";

    status = pasynOctetSyncIO->writeRead(pasynUser, toServer_, strlen(toServer_), 
                                         fromServer_, sizeof(fromServer_), 
                                         timeout, &nwrite, &nread, &eomReason);
    if (status != asynSuccess)
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
                    "%s:%s, timeout=%f, status=%d wrote %lu bytes, received %lu bytes\n%s\n",
                    driverName, functionName, timeout, status, 
                    (unsigned long)nwrite, (unsigned long)nread, fromServer_);
    else {
        /* Look for the string "GOT:" in the response */
        if (!strstr(fromServer_, "GOT:")) {
            asynPrint(pasynUser, ASYN_TRACE_ERROR,
                      "%s:%s unexpected response from server = %s\n",
                      driverName, functionName, fromServer_);
            setStringParam(ADStatusMessage, "Error from server");
            status = asynError;
        } else
            setStringParam(ADStatusMessage, "Server returned OK");
    }

    /* Set input and output strings so they can be displayed by EPICS */
    setStringParam(ADStringToServer, toServer_);
    setStringParam(ADStringFromServer, fromServer_);

    return(status);
}


void pixirad::dataTask()
{
    int numFrames;
    int collectionMode;
    int sizeX, sizeY;
    size_t dims[2];
    NDArray *pImage;
    int imageCounter;
    int numImagesCounter;
    int arrayCallbacks;
    epicsTimeStamp startTime;
    int is_autocal_data, i, j ,k, code_depth;
    unsigned short *local_buffer_ptr, *temp_us_ptr, *conv, *process_buf_ptr;
    unsigned short packet_tag;
    int status;
    static const char *functionName = "dataTask";

    local_buffer_ptr = (unsigned short *) calloc(PIXIEII_MODULES*MATRIX_DIM_WORDS*15, sizeof(unsigned short));
    if(local_buffer_ptr == NULL){
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s:%s: Error allocating local buffer\n",
            driverName, functionName);
        return;
    }
    conv = conversion_table_allocation();

    lock();
    getIntegerParam(ADMaxSizeX, &sizeX);
    getIntegerParam(ADMaxSizeY, &sizeY);
    dims[0] = sizeX;
    dims[1] = sizeY;
    while(1) {
        unlock();
        status = epicsMessageQueueReceive(dataMessageQueueId_, &process_buf_ptr, sizeof(&process_buf_ptr));
        lock();
        if (status <= 0) {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s:%s: Error calling epicsMessageQueueReceive, status=%d\n",
                driverName, functionName, status);
             continue;
        }
        if (process_buf_ptr == NULL) {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s:%s: Error process_buf_ptr=NULL\n",
                driverName, functionName);
            continue;
        }
        unlock();
        packet_tag = *(process_buf_ptr + PACKET_TAG_OFFSET*2);
        if (packet_tag & AUTOCAL_DATA) {
            code_depth = 5;
            is_autocal_data = 1;
        } else {
            code_depth = 15;
            is_autocal_data = 0;
        }
        if (packet_tag & FRAME_HAS_ALIGN_ERRORS) {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
                "%s:%s: frame has alignment errors\n",
                driverName, functionName);
        }

        temp_us_ptr = process_buf_ptr + (PACKET_TAG_BYTES/2);
        for (i=0; i<PIXIEII_MODULES; i++) {
            for(j=0; j<COLS_PER_DOUT*PIXIE_ROWS; j++) {
                for (k=0; k<code_depth; k++){
                    my_bytes_swap(temp_us_ptr + i +( j*PIXIEII_MODULES*code_depth) + (k*PIXIEII_MODULES));
                    local_buffer_ptr[(i*COLS_PER_DOUT*PIXIE_ROWS*code_depth) + (j*code_depth)+k] =
                        temp_us_ptr[i + (j*PIXIEII_MODULES*code_depth) +( k*PIXIEII_MODULES)];
                }
            }
        }

        for(i=0; i<PIXIEII_MODULES; i++){
            for(j=0; j<COLS_PER_DOUT*PIXIE_ROWS; j++) {
                convert_bit_stream_to_counts(code_depth,
                    local_buffer_ptr + (i*COLS_PER_DOUT*PIXIE_ROWS*code_depth) + (j*code_depth),
                    process_buf_ptr + (i*MATRIX_DIM_WORDS) + (j*PIXIE_DOUTS) + (PACKET_TAG_BYTES/2), PIXIE_DOUTS);

            }
        }

        for(i=0; i<PIXIEII_MODULES; i++){
            if (is_autocal_data == 0)
                decode_pixie_data_buffer(conv, process_buf_ptr + (PACKET_TAG_BYTES/2) + i*MATRIX_DIM_WORDS);
            databuffer_sorting(process_buf_ptr + (PACKET_TAG_BYTES/2) + i*MATRIX_DIM_WORDS);
            map_data_buffer_on_pixie(process_buf_ptr + (PACKET_TAG_BYTES/2) + i*MATRIX_DIM_WORDS);
        }

        lock();
        getIntegerParam(ADNumImages, &numFrames);
        getIntegerParam(PixiradCollectionMode, &collectionMode);
        getIntegerParam(NDArrayCallbacks, &arrayCallbacks);
        getIntegerParam(NDArrayCounter, &imageCounter);
        imageCounter++;
        setIntegerParam(NDArrayCounter, imageCounter);
        getIntegerParam(ADNumImagesCounter, &numImagesCounter);
        numImagesCounter++;
        setIntegerParam(ADNumImagesCounter, numImagesCounter);
        /* Call the callbacks to update any changes */
        callParamCallbacks();

        /* Get the current time */
        epicsTimeGetCurrent(&startTime);
        /* Get an image buffer from the pool */
        pImage = this->pNDArrayPool->alloc(2, dims, NDUInt16, 0, NULL);

        /* Copy the data from the process_buf to the NDArray */
        memcpy(pImage->pData, process_buf_ptr+PACKET_TAG_BYTES/2, MATRIX_DIM_WORDS * sizeof(unsigned short));

        // Free the process_buf_ptr, no longer needed
        free(process_buf_ptr);

        /* Put the frame number and time stamp into the buffer */
        pImage->uniqueId = imageCounter;
        pImage->timeStamp = startTime.secPastEpoch + startTime.nsec / 1.e9;
        updateTimeStamp(&pImage->epicsTS);

        /* Get any attributes that have been defined for this driver */        
        this->getAttributes(pImage->pAttributeList);

        if (arrayCallbacks) {
            /* Call the NDArray callback */
            /* Must release the lock here, or we can get into a deadlock, because we can
             * block on the plugin lock, and the plugin can be calling us */
            unlock();
            asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
                 "%s:%s: calling NDArray callback\n", driverName, functionName);
            doCallbacksGenericPointer(pImage, NDArrayData, 0);
            lock();
        }
        /* Free the image buffer */
        pImage->release();
        if (numImagesCounter >= numFrames) {
            setIntegerParam(ADAcquire, 0);
            callParamCallbacks();
        }
    }
}

void pixirad::udpDataListenerTask()
{
    struct sockaddr_in si_me;
    unsigned short packet_tag;
    unsigned int i,j,temp_NPACK;
    SOCKET data_udp_sock_fd;
    int this_frame_has_aligment_errors=0;
    unsigned int packet_id, local_packet_id;
    long packet_id_gap;
    epicsTimeStamp timer_a, timer_b;
    double  time_interval, MBytesPerSecond;
    unsigned int received_packets=0;
    unsigned int id_error_packets=0;
    static const char *functionName = "udpDataListenerTask";
    unsigned char *process_buf;
    unsigned char *buf = (unsigned char*) calloc(MAX_UDP_PACKET_LEN * DEFAULT_UDP_NUM_PACKETS, sizeof(unsigned short));

    if ((data_udp_sock_fd = epicsSocketCreate(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s:%s: Error creating socket error = %s\n", 
            driverName, functionName, strerror(errno));
        return;
    }

    memset((char *) &si_me, 0, sizeof(si_me));
    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(dataPortNumber_);
    si_me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(data_udp_sock_fd, (struct sockaddr *) &si_me, sizeof(si_me)) == -1) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s:%s: Error binding socket error = %s\n", 
            driverName, functionName, strerror(errno));
        return;
    }

    int buffsize;
    osiSocklen_t czm = sizeof(int);
    int received_bytes;
    buffsize = MAX_UDP_DATA_BUFFER;
    if (setsockopt(data_udp_sock_fd, SOL_SOCKET,
                   SO_RCVBUF,
                   (char*)&buffsize,
                   czm) == -1) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s:%s: Error calling setsockopt for SO_RCVBUF = %s\n", 
            driverName, functionName, strerror(errno));
        return;
    }

    if (getsockopt(data_udp_sock_fd, SOL_SOCKET,
                   SO_RCVBUF,
                   (char*)&buffsize,
                   &czm) == -1) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s:%s: Error calling getsockopt for SO_RCVBUF = %s\n", 
            driverName, functionName, strerror(errno));
        return;
    }

    if (buffsize != MAX_UDP_DATA_BUFFER) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s:%s: Unable to set requested buffer size for SO_RCVBUF, actual=%d\n", 
            driverName, functionName, buffsize);
    }

    while (1) {
        this_frame_has_aligment_errors=0;
        temp_NPACK = DEFAULT_UDP_NUM_PACKETS;
        i=0;
        while(i < temp_NPACK ) {
            j = 0;
            while(j < DAQ_PACKET_FRAGMENT) {
                received_bytes=recvfrom(data_udp_sock_fd, (char*) buf+(i*MAX_UDP_PACKET_LEN), MAX_UDP_PACKET_LEN, 0, NULL, 0);
                if (received_bytes == -1) {
                    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                        "%s:%s: Error calling recvfrom = %s\n", 
                        driverName, functionName, strerror(errno));
                } else if (received_bytes == MAX_UDP_PACKET_LEN) {
                    if (i == 0)  epicsTimeGetCurrent(&timer_a);
                    received_packets++;
                    packet_tag = *buf;
                    if (packet_tag & AUTOCAL_DATA)
                        temp_NPACK = AUTOCAL_NUM_PACKETS;
                    else
                        temp_NPACK = DEFAULT_UDP_NUM_PACKETS;

                    packet_id = buf[MAX_UDP_PACKET_LEN*(i)+PACKET_ID_OFFSET]<<8;
                    packet_id += buf[MAX_UDP_PACKET_LEN*(i)+1+PACKET_ID_OFFSET];
                    packet_id = packet_id%DAQ_PACKET_FRAGMENT;
                    packet_id_gap = (packet_id-j);

                    if (packet_id_gap != 0){
                        if (id_error_packets<10)
                            printf("ID: %d j= %d\n",packet_id,j);
                        id_error_packets++;
                        this_frame_has_aligment_errors=1;
                    }
                    if (packet_id_gap >= 0){
                        j+=(packet_id_gap+1);
                        i+=(packet_id_gap+1);
                    }
                    else {
                        j=DAQ_PACKET_FRAGMENT;
                        i+=DAQ_PACKET_FRAGMENT;
                    }
                }
            }
        }

        epicsTimeGetCurrent(&timer_b);
        time_interval = epicsTimeDiffInSeconds(&timer_b, &timer_a);
        MBytesPerSecond = (i * MAX_UDP_PACKET_LEN) / (time_interval*1024*1024);
        asynPrint(pasynUserSelf, ASYN_TRACEIO_DRIVER,
            "%s:%s: received %d packets (%d bytes each)  in %.3f s, = %.3f MB/s\n",
            driverName, functionName, i, MAX_UDP_PACKET_LEN, time_interval, MBytesPerSecond);

        i=0;
        local_packet_id=0;
        process_buf = (unsigned char *) calloc((MAX_UDP_PACKET_LEN - PACKET_EXTRA_BYTES + PACKET_TAG_BYTES) * DEFAULT_UDP_NUM_PACKETS, 
                                              sizeof(unsigned short));
        if (process_buf == NULL) {
            asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s:%s: error allocated process_buf\n", 
                driverName, functionName);
            return;
        }
        while(i < temp_NPACK) {
            memcpy(process_buf + PACKET_TAG_BYTES + (PACKET_SENSOR_DATA_BYTES * local_packet_id),
                   buf + PACKET_SENSOR_DATA_OFFSET + (MAX_UDP_PACKET_LEN*i), PACKET_SENSOR_DATA_BYTES);
            for(j=0; j<PACKET_TAG_BYTES; j++)
                process_buf[PACKET_TAG_OFFSET+j] = buf[j];\
            if (this_frame_has_aligment_errors)
                process_buf[PACKET_TAG_OFFSET] |= FRAME_HAS_ALIGN_ERRORS;
            else
                process_buf[PACKET_TAG_OFFSET] &= (~FRAME_HAS_ALIGN_ERRORS);
            local_packet_id++;
            i++;
        }
        epicsMessageQueueSend(dataMessageQueueId_, &process_buf, sizeof(&process_buf));
    }
}



/** This function reads the environmental parameters (temperature, humidity, etc.)
    which are periodically UDP broadcast by the Pixirad box on port 2224 */
void pixirad::statusTask()
{
  struct sockaddr_in serverAddr;
  struct sockaddr_in remoteAddr;
  osiSocklen_t remoteAddrLen;
  SOCKET fd;
  int numItems=0;
  int nRead;
  float value;
  char buffer[256];
  double h, temperature, humidity, dewPoint;
  char *pString;
  typedef struct {
    const char* str;
    int param;
  } lookupEntry;
  lookupEntry entry;
  int i;
  char format[80];
  #define MAX_STATUS_PARAMS 7
  lookupEntry lookupTable[MAX_STATUS_PARAMS] = {
    {"READ_TCOLD",         ADTemperatureActual},
    {"READ_THOT",          PixiradHotTemperature},
    {"READ_BOX_TEMP",      PixiradBoxTemperature},
    {"READ_BOX_HUM",       PixiradBoxHumidity},
    {"READ_PELTIER_PWR",   PixiradPeltierPower},
    {"READ_HV",            PixiradHVActual},
    {"READ_HV_CURRENT",    PixiradHVCurrent}
  };
  static const char *functionName = "statusTask";

  lock();

  // Create the socket
  if ((fd = epicsSocketCreate(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
      asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
          "%s:%s: Can't create socket: %s\n", 
          driverName, functionName, strerror(SOCKERRNO));
      return;
  }

  serverAddr.sin_family = AF_INET;
  serverAddr.sin_addr.s_addr = INADDR_ANY;
  serverAddr.sin_port = htons(statusPortNumber_);
  if (bind(fd, (struct sockaddr *) &serverAddr, sizeof(serverAddr)) < 0)
  {
      asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
          "%s:%s: Error in binding port %d: %s\n", 
          driverName, functionName, statusPortNumber_, strerror(errno));
      epicsSocketDestroy(fd);
      return;
  }

  while (1) {
    unlock();
    epicsThreadSleep(1.0);
    remoteAddrLen = sizeof(remoteAddr);
    nRead = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&remoteAddr, &remoteAddrLen);
    lock();

    if (nRead < 0) {
      asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
        "%s:%s: error reading status broadcast message: %s\n",
        driverName, functionName, strerror(errno));
      continue;

    } else {
      asynPrint(pasynUserSelf, ASYN_TRACEIO_DRIVER,
        "%s:%s: received status nRead=%d, message=%s\n",
        driverName, functionName, nRead, buffer);
    }

    for (i=0; i<MAX_STATUS_PARAMS; i++) {
      entry = lookupTable[i];
      pString = strstr(buffer, entry.str);
      if (!pString) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
          "%s:%s: cannot find string %s in status broadcast message=%s\n",
          driverName, functionName, entry.str, buffer);
        continue;
      }
      sprintf(format, "%s %%f", entry.str);
      numItems = sscanf(pString, format, &value);
      if (numItems == 1) {
        setDoubleParam(entry.param, value);
      } else {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
          "%s:%s: error parsing %s in status broadcast message=%s\n",
          driverName, functionName, entry.str, buffer);
      }
    }
    getDoubleParam(PixiradBoxHumidity, &humidity);
    getDoubleParam(PixiradBoxTemperature, &temperature);
    // Calculate the dew point (Magnus formula, e-mail from Pixirad 1/14/2014)
    h = (log10(humidity) - 2.) / 0.4343 + (17.62 * temperature) / (243.12 + temperature);
    dewPoint = 243.12 * h / (17.62 - h); 
    setDoubleParam(PixiradDewPoint, dewPoint);
    callParamCallbacks();
  }
}



/** Called when asyn clients call pasynInt32->write().
  * This function performs actions for some parameters, including ADAcquire, ADTriggerMode, etc.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus pixirad::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
    int function = pasynUser->reason;
    int isAcquiring;
    asynStatus status = asynSuccess;
    const char *functionName = "writeInt32";

    /* Get the current value of ADAcquire */
    getIntegerParam(ADAcquire, &isAcquiring);

    status = setIntegerParam(function, value);

    if (function == ADAcquire) {
        if (value && !isAcquiring) {
            status = startAcquire();
        } 
        else if (!value && isAcquiring) {
            //  This seems to be screwing things up, comment out temporarily
            //status = stopAcquire();
        }

    } else if ((function == PixiradSyncInPolarity)  ||
               (function == PixiradSyncOutPolarity) ||
               (function == PixiradSyncOutFunction)) {
        status = setSync();

    } else if ((function == PixiradHVState) ||
               (function == PixiradCoolingState)) {
          status = setCoolingAndHV();
        
    } else if (function == PixiradAutoCalibrate) {
        status = doAutoCalibrate();

    } else { 
        /* If this parameter belongs to a base class call its method */
        if (function < FIRST_PIXIRAD_PARAM) status = ADDriver::writeInt32(pasynUser, value);
    }
            
    /* Do callbacks so higher layers see any changes */
    callParamCallbacks();
    
    if (status) 
        asynPrint(pasynUser, ASYN_TRACE_ERROR, 
              "%s:%s: error, status=%d function=%d, value=%d\n", 
              driverName, functionName, status, function, value);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s:%s: function=%d, value=%d\n", 
              driverName, functionName, function, value);
    return status;
}


/** Called when asyn clients call pasynFloat64->write().
  * This function performs actions for some parameters, including ADAcquireTime, ADGain, etc.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus pixirad::writeFloat64(asynUser *pasynUser, epicsFloat64 value)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    const char *functionName = "writeFloat64";

    /* Set the parameter and readback in the parameter library.  This may be overwritten when we read back the
     * status at the end, but that's OK */
    status = setDoubleParam(function, value);
    
    if ((function == ADTemperature) ||
        (function == PixiradHVValue)) {
        status = setCoolingAndHV();

    } else if ((function == PixiradThreshold1) ||
               (function == PixiradThreshold2) ||
               (function == PixiradThreshold3) ||
               (function == PixiradThreshold4)) {
        status = setThresholds();

    } else {
        /* If this parameter belongs to a base class call its method */
        if (function < FIRST_PIXIRAD_PARAM) status = ADDriver::writeFloat64(pasynUser, value);
    }

    if (status) {
        asynPrint(pasynUser, ASYN_TRACE_ERROR, 
              "%s:%s error, status=%d function=%d, value=%f\n", 
              driverName, functionName, status, function, value);
    }
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s:%s: function=%d, value=%f\n", 
              driverName, functionName, function, value);
    
    /* Do callbacks so higher layers see any changes */
    callParamCallbacks();
    return status;
}


/** Report status of the driver.
  * Prints details about the driver if details>0.
  * It then calls the ADDriver::report() method.
  * \param[in] fp File pointed passed by caller where the output is written to.
  * \param[in] details If >0 then driver details are printed.
  */
void pixirad::report(FILE *fp, int details)
{

    fprintf(fp, "Pixirad detector %s\n", this->portName);
    if (details > 0) {
        int nx, ny, dataType;
        getIntegerParam(ADSizeX, &nx);
        getIntegerParam(ADSizeY, &ny);
        getIntegerParam(NDDataType, &dataType);
        fprintf(fp, "  NX, NY:            %d  %d\n", nx, ny);
        fprintf(fp, "  Data type:         %d\n", dataType);
    }
    /* Invoke the base class method */
    ADDriver::report(fp, details);
}

extern "C" int pixiradConfig(const char *portName, const char *commandPort,
                                    int dataPortNumber, int statusPortNumber, int maxDataPortBuffers,
                                    int maxSizeX, int maxSizeY,
                                    int maxBuffers, size_t maxMemory,
                                    int priority, int stackSize)
{
    new pixirad(portName, commandPort, dataPortNumber, statusPortNumber, maxDataPortBuffers,
                maxSizeX, maxSizeY, maxBuffers, maxMemory,
                priority, stackSize);
    return(asynSuccess);
}

/* Code for iocsh registration */
static const iocshArg pixiradConfigArg0 = {"Port name", iocshArgString};
static const iocshArg pixiradConfigArg1 = {"command port name", iocshArgString};
static const iocshArg pixiradConfigArg2 = {"data port number", iocshArgInt};
static const iocshArg pixiradConfigArg3 = {"status port number", iocshArgInt};
static const iocshArg pixiradConfigArg4 = {"max data port buffers", iocshArgInt};
static const iocshArg pixiradConfigArg5 = {"maxSizeX", iocshArgInt};
static const iocshArg pixiradConfigArg6 = {"maxSizeY", iocshArgInt};
static const iocshArg pixiradConfigArg7 = {"maxBuffers", iocshArgInt};
static const iocshArg pixiradConfigArg8 = {"maxMemory", iocshArgInt};
static const iocshArg pixiradConfigArg9 = {"priority", iocshArgInt};
static const iocshArg pixiradConfigArg10 = {"stackSize", iocshArgInt};
static const iocshArg * const pixiradConfigArgs[] =  {&pixiradConfigArg0,
                                                      &pixiradConfigArg1,
                                                      &pixiradConfigArg2,
                                                      &pixiradConfigArg3,
                                                      &pixiradConfigArg4,
                                                      &pixiradConfigArg5,
                                                      &pixiradConfigArg6,
                                                      &pixiradConfigArg7,
                                                      &pixiradConfigArg8,
                                                      &pixiradConfigArg9,
                                                      &pixiradConfigArg10};
static const iocshFuncDef configpixirad = {"pixiradConfig", 11, pixiradConfigArgs};
static void configpixiradCallFunc(const iocshArgBuf *args)
{
    pixiradConfig(args[0].sval, args[1].sval, args[2].ival,  args[3].ival,  
                  args[4].ival, args[5].ival, args[6].ival,  args[7].ival,
                  args[8].ival, args[9].ival, args[10].ival);
}


static void pixiradRegister(void)
{

    iocshRegister(&configpixirad, configpixiradCallFunc);
}

extern "C" {
epicsExportRegistrar(pixiradRegister);
}

