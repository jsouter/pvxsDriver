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
#include <cstring>
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

void subscriptionThread(void *argPtr) {
    // TODO, we need to make a struct that has all the info we need for the thread...
    // then how do we pass this info back up to the main thread...
    // std::string const pvName = * (std::string*) args;
    // maybe we need a pointer to a m_value or something to update...
    auto *driver = (pvxsDriver*) argPtr;
    // it would be nicer to have a smart pointer than a dumb one..

    // is there any reason we need the context to exist outside of this thread??
    pvxs::MPMCFIFO<std::shared_ptr<pvxs::client::Subscription>> workqueue(42u);
    auto sub = driver->m_ctxt.monitor(driver->m_pvName)
                .event([&workqueue](pvxs::client::Subscription& sub) {
                    // Subscription queue becomes not empty.
                    // Avoid I/O on PVXS worker thread,
                    // delegate to application thread
                    workqueue.push(sub.shared_from_this());
                })
                .exec();

    while(auto sub = workqueue.pop()) { // could workqueue.push(nullptr) to break
        try {
            // TODO, add in some mutexes
            pvxs::Value update = sub->pop();
            if(!update)
                continue; // Subscription queue empty, wait for another event callback
            if (!driver->m_value.valid()) {
                driver->m_value = update;
                driver->m_converter = std::make_shared<NTNDArrayConverterPvxs>(driver->m_value);
            } driver->m_value.assign(update);
            driver->updatePVsFromConverter();
        } catch(std::exception& e) {
            // may be Connected(), Disconnect(), Finished(), or RemoteError()
            std::cerr<<"Error "<<e.what()<<"\n";
        }
        // queue not empty, reschedule
        workqueue.push(sub);
    }
}

void pvxsDriver::updatePVsFromConverter(void) {
    // TODO, check if anything from monitorEvent is missing
    // also, be more careful about when we call lock and unlock...
    int acquire;
    getIntegerParam(ADAcquire, &acquire);
    if (acquire == 0) {
        // monitor->release(update);
        return;
    }
    const char *functionName = "updatePVsFromConverter";
    NTNDArrayInfo_t info;

    try
    {
        info = m_converter->getInfo();
    }
    catch(exception& e)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s failed to get info from NTNDArray: %s\n",
                driverName, functionName, e.what());
        // monitor->release(update);
        // continue;
    }

    NDArray *pImage = pNDArrayPool->alloc(info.ndims, (size_t*) &info.dims,
        info.dataType, info.totalBytes, NULL);

    if(!pImage)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s failed to alloc new NDArray"
                " - memory pool exhausted? (free: %d)\n",
                driverName, functionName, pNDArrayPool->getNumFree());
        // monitor->release(update);
        // continue;
    }

    // unlock();
    try
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
                    "%s::%s Converting to NDArray\n",
                    driverName, functionName);
        m_converter->toArray(pImage);
    }
    catch(exception& e)
    {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                "%s::%s failed to convert NTNDArray into NDArray: %s\n",
                driverName, functionName, e.what());
        pImage->release();
        // monitor->release(update);
        // lock();
        // continue;
    }
    // lock();

    int xSize     = pImage->dims[info.x.dim].size;
    int ySize     = pImage->dims[info.y.dim].size;
    setIntegerParam(ADMaxSizeX,   xSize);
    setIntegerParam(ADMaxSizeY,   ySize);
    setIntegerParam(ADSizeX,      xSize);
    setIntegerParam(ADSizeY,      ySize);
    setIntegerParam(NDArraySizeX, xSize);
    setIntegerParam(NDArraySizeY, ySize);
    setIntegerParam(NDArraySizeZ, pImage->dims[info.color.dim].size);
    setIntegerParam(ADMinX,       pImage->dims[info.x.dim].offset);
    setIntegerParam(ADMinY,       pImage->dims[info.y.dim].offset);
    setIntegerParam(ADBinX,       pImage->dims[info.x.dim].binning);
    setIntegerParam(ADBinY,       pImage->dims[info.y.dim].binning);
    setIntegerParam(ADReverseX,   pImage->dims[info.x.dim].reverse);
    setIntegerParam(ADReverseY,   pImage->dims[info.y.dim].reverse);
    setIntegerParam(NDArraySize,  (int) info.totalBytes);
    setIntegerParam(NDDataType,   (int) info.dataType);
    setIntegerParam(NDColorMode,  (int) info.colorMode);
    callParamCallbacks();

    int arrayCallbacks;
    getIntegerParam(NDArrayCallbacks, &arrayCallbacks);
    if(arrayCallbacks)
    {
    asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
                "%s::%s Callback with NDArray (%p)\n",
                driverName, functionName, pImage);
    doCallbacksGenericPointer(pImage, NDArrayData, 0);
    }

    // Update the counters after doCallbacksGenericPointer()
    int imageCounter;
    int arrayCounter;
    int imageMode;
    int numImages;
    getIntegerParam(NDArrayCounter, &arrayCounter);
    setIntegerParam(NDArrayCounter, arrayCounter+1);
    getIntegerParam(ADNumImagesCounter, &imageCounter);
    imageCounter++;
    setIntegerParam(ADNumImagesCounter, imageCounter);

    // See if acquisition should stop
    getIntegerParam(ADImageMode, &imageMode);
    if (imageMode == ADImageMultiple) {
    getIntegerParam(ADNumImages, &numImages);
    if (imageCounter >= numImages) setIntegerParam(ADAcquire, 0);
    }    
    if (imageMode == ADImageSingle) setIntegerParam(ADAcquire, 0);
    callParamCallbacks();

    pImage->release();
    // monitor->release(update);
}

asynStatus pvxsDriver::connectPv(std::string const & pvName)
{
    m_subscriptionThreadId = epicsThreadCreate(
        "subscriptionPopThread",
        0, //priority i have no idea
        0,
        // 1000, // stack size idk either lol.. invalid argument
        subscriptionThread, // fn ptr,
        (void*) this
    );

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
        if (value == 1) setIntegerParam(ADNumImagesCounter, 0);
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
        // if((status = connectPv(value)))
        //     status = (asynStatus)setStringParam(function, m_pvName);
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
