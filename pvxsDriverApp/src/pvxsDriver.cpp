/* pvxsDriver.cpp
 *
 * This is a driver for EPICSv4 pvAccess NTNDArrays.
 *
 * Author: Bruno Martins
 *         Brookhaven National Laboratory
 *
 * Created:  April 10, 2015
 *
 */
#include <epicsThread.h>
#include <iocsh.h>

#include <ntndArrayConverterPvxs.h>

#include <ADDriver.h>

#include <epicsExport.h>
#include "pvxsDriver.h"

#include <iostream>

//#define DEFAULT_REQUEST "record[queueSize=100]field()"
#define DEFAULT_REQUEST "field()"

using namespace std;

static const char *driverName = "pvxsDriver";

/* Constructor for pvxsDriver; most parameters are simply passed to
 * ADDriver::ADDriver. Sets reasonable default values for parameters defined in
 * asynNDArrayDriver and ADDriver.
 *
 * The method init must be called after creating an instance.
 *
 * \param[in] portName The name of the asyn port driver to be created.
 * \param[in] pvName The v4 NTNDArray PV to be monitored
 * \param[in] maxBuffers The maximum number of NDArray buffers that the
 *            NDArrayPool for this driver is allowed to allocate. Set this to -1
 *            to allow an unlimited number of buffers.
 * \param[in] maxMemory The maximum amount of memory that the NDArrayPool for
 *            this driver is allowed to allocate. Set this to -1 to allow an
 *            unlimited amount of memory.
 * \param[in] priority The thread priority for the asyn port driver thread if
 *            ASYN_CANBLOCK is set in asynFlags.
 * \param[in] stackSize The stack size for the asyn port driver thread if
 *            ASYN_CANBLOCK is set in asynFlags.
 */
pvxsDriver::pvxsDriver (const char *portName, const char *pvName,
        int maxBuffers, size_t maxMemory, int priority, int stackSize)

    : ADDriver(portName, 1, NUM_PVA_DRIVER_PARAMS, maxBuffers, maxMemory, 0, 0, ASYN_CANBLOCK, 1,
            priority, stackSize),
      m_pvName(pvName),
      m_thisPtr(shared_ptr<pvxsDriver>(this)),
      m_ctxt(pvxs::client::Config::from_env().build())
{
    int status = asynSuccess;
    char versionString[20];
    const char *functionName = "pvxsDriver";

    lock();
    createParam(PVAOverrunCounterString,     asynParamInt32, &PVAOverrunCounter);
    createParam(PVAPvNameString,             asynParamOctet, &PVAPvName);
    createParam(PVAPvConnectionStatusString, asynParamInt32, &PVAPvConnectionStatus);

    /* Set some default values for parameters */
    status =  setStringParam (ADManufacturer, "PVAccess driver");
    status |= setStringParam (ADModel, "Basic PVAccess driver");
    epicsSnprintf(versionString, sizeof(versionString), "%d.%d.%d", 
                  DRIVER_VERSION, DRIVER_REVISION, DRIVER_MODIFICATION);
    status |= setStringParam(NDDriverVersion, versionString);
    // We use the PvAccess version as the SDK version
    epicsSnprintf(versionString, sizeof(versionString), "%d.%d.%d", 
                  PVXS_MAJOR_VERSION, PVXS_MINOR_VERSION, PVXS_MAINTENANCE_VERSION);
    status |= setStringParam(ADSDKVersion, versionString);
    status |= setStringParam(ADSerialNumber, "No serial number");
    status |= setStringParam(ADFirmwareVersion, "No firmware");
    status |= setIntegerParam(ADMaxSizeX, 0);
    status |= setIntegerParam(ADMaxSizeY, 0);
    status |= setIntegerParam(ADMinX, 0);
    status |= setIntegerParam(ADMinY, 0);
    status |= setIntegerParam(ADBinX, 1);
    status |= setIntegerParam(ADBinY, 1);
    status |= setIntegerParam(ADReverseX, 0);
    status |= setIntegerParam(ADReverseY, 0);
    status |= setIntegerParam(ADSizeX, 0);
    status |= setIntegerParam(ADSizeY, 0);
    status |= setIntegerParam(NDArraySizeX, 0);
    status |= setIntegerParam(NDArraySizeY, 0);
    status |= setIntegerParam(NDArraySize, 0);
    status |= setIntegerParam(NDDataType, 0);
    status |= setIntegerParam(PVAOverrunCounter, 0);
    status |= setStringParam (PVAPvName, pvName);
    status |= setIntegerParam(PVAPvConnectionStatus, 0);

    if(status)
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s unable to set driver parameters\n",
                driverName, functionName);

    connectPv(pvName);

    unlock();
}

asynStatus pvxsDriver::connectPv(std::string const & pvName)
{
    return asynError;
}

/** Called when asyn clients call pasynInt32->write().
  * This function performs actions for some parameters.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus pvxsDriver::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    int acquire;
    static const char *functionName = "writeInt32";

    getIntegerParam(ADAcquire, &acquire);
    
   /* Set the parameter in the parameter library. */
    status = (asynStatus) setIntegerParam(function, value);

    if (function == ADAcquire){
        if (value == 1){
            setIntegerParam(ADNumImagesCounter, 0);
            auto result = m_ctxt.get(m_pvName).exec()->wait();
            if (!m_value.valid()) {
                // TODO, maybe there are other situations where we would need to
                // make a new converter
                m_value = pvxs::Value(result);
                m_converter = std::make_shared<NTNDArrayConverterPvxs>(m_value);
            } else {
                m_value.assign(result);
            }
            auto info = m_converter->getInfo();
            NDArray *pImage = pNDArrayPool->alloc(info.ndims, (size_t*) &info.dims, info.dataType, info.totalBytes, NULL);
            m_converter->toArray(pImage);
            std::cout << pImage->uniqueId << std::endl;
            return asynError;
            // m_monitor->start();
        }
        else {
            return asynError;
            // m_monitor->stop();
        }
    } else {
        // If this parameter belongs to a base class call its method
        if (function < FIRST_PVA_DRIVER_PARAM)
            status = ADDriver::writeInt32(pasynUser, value);
    }
    
    // Do callbacks so higher layers see any changes
    status = (asynStatus) callParamCallbacks();
    
    if (status) 
        epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize, 
                  "%s:%s: status=%d, function=%d, value=%d", 
                  driverName, functionName, status, function, value);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s:%s: function=%d, value=%d\n", 
              driverName, functionName, function, value);
    return status;
}

/** Called when asyn clients call pasynOctet->write().
  * This function performs actions for some parameters, including AttributesFile.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Address of the string to write.
  * \param[in] nChars Number of characters to write.
  * \param[out] nActual Number of characters actually written. */
asynStatus pvxsDriver::writeOctet(asynUser *pasynUser, const char *value, size_t nChars, size_t *nActual)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    const char *functionName = "writeOctet";

    // Set the parameter in the parameter library.
    status = (asynStatus)setStringParam(function, (char *)value);

    if (function == PVAPvName){
        if((status = connectPv(value)))
            status = (asynStatus)setStringParam(function, m_pvName);
    }
    else if (function < FIRST_PVA_DRIVER_PARAM) {
        /* If this parameter belongs to a base class call its method */
        status = ADDriver::writeOctet(pasynUser, value, nChars, nActual);
    }

    if (status){
        epicsSnprintf(pasynUser->errorMessage, pasynUser->errorMessageSize,
                "%s:%s: status=%d, function=%d, value=%s",
                driverName, functionName, status, function, value);
    } else {
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
                "%s:%s: function=%d, value=%s\n",
                driverName, functionName, function, value);
    }

    // Do callbacks so higher layers see any changes
    callParamCallbacks();

    *nActual = nChars;
    return status;
}

void pvxsDriver::report (FILE *fp, int details)
{
    fprintf(fp, "pvxs detector %s\n", this->portName);
    if (details > 0)
        fprintf(fp, " PV Name: %s\n", m_pvName.c_str());

    ADDriver::report(fp, details);
}

/** Configuration command, called directly or from iocsh */
extern "C" int pvxsDriverConfig (const char *portName, char *pvName,
        int maxBuffers, int maxMemory, int priority, int stackSize)
{
    new pvxsDriver(portName, pvName, maxBuffers, maxMemory, priority, stackSize);
    return(asynSuccess);
}

/** Code for iocsh registration */
static const iocshArg pvxsDriverConfigArg0 = {"Port name", iocshArgString};
static const iocshArg pvxsDriverConfigArg1 = {"PV name", iocshArgString};
static const iocshArg pvxsDriverConfigArg2 = {"maxBuffers", iocshArgInt};
static const iocshArg pvxsDriverConfigArg3 = {"maxMemory", iocshArgInt};
static const iocshArg pvxsDriverConfigArg4 = {"priority", iocshArgInt};
static const iocshArg pvxsDriverConfigArg5 = {"stackSize", iocshArgInt};
static const iocshArg * const pvxsDriverConfigArgs[] = {
        &pvxsDriverConfigArg0, &pvxsDriverConfigArg1, &pvxsDriverConfigArg2,
        &pvxsDriverConfigArg3, &pvxsDriverConfigArg4, &pvxsDriverConfigArg5};

static const iocshFuncDef configpvxsDriver = {"pvxsDriverConfig", 6,
        pvxsDriverConfigArgs};

static void configpvxsDriverCallFunc (const iocshArgBuf *args)
{
    pvxsDriverConfig(args[0].sval, args[1].sval, args[2].ival, args[3].ival,
            args[4].ival, args[5].ival);
}

static void pvxsDriverRegister (void)
{
    iocshRegister(&configpvxsDriver, configpvxsDriverCallFunc);
}

extern "C" {
    epicsExportRegistrar(pvxsDriverRegister);
}
