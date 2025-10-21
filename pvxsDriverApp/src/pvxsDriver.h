#include <memory>
#include <pvxs/client.h>
#include <pvxs/data.h>
#include <ntndArrayConverterPvxs.h>

#define PVAOverrunCounterString     "OVERRUN_COUNTER"
#define PVAPvNameString             "PV_NAME"
#define PVAPvConnectionStatusString "PV_CONNECTION"

#define DRIVER_VERSION      1
#define DRIVER_REVISION     6
#define DRIVER_MODIFICATION 0

class pvxsDriver;

class epicsShareClass pvxsDriver : public ADDriver, std::enable_shared_from_this<pvxsDriver>
{

public:
    pvxsDriver (const char *portName, const char *pvName, int maxBuffers,
            size_t maxMemory, int priority, int stackSize);

    // Overriden from ADDriver:
    asynStatus writeInt32(asynUser *pasynUser, epicsInt32 value);
    asynStatus writeOctet(asynUser *pasynUser, const char *value, size_t nChars, size_t *nActual);
    virtual void report (FILE *fp, int details);
    std::shared_ptr<pvxsDriver> getPtr() {
        return shared_from_this();
    };

protected:
    int PVAOverrunCounter;
    #define FIRST_PVA_DRIVER_PARAM PVAOverrunCounter   
    int PVAPvName;
    int PVAPvConnectionStatus;
    #define LAST_PVA_DRIVER_PARAM PVAPvConnectionStatus   

private:
    std::string m_pvName;
    asynStatus connectPv(std::string const & pvName);
    pvxs::client::Context m_ctxt;
    pvxs::Value m_value;
    NTNDArrayConverterPvxsPtr m_converter;
};

#define NUM_PVA_DRIVER_PARAMS ((int)(&LAST_PVA_DRIVER_PARAM - &FIRST_PVA_DRIVER_PARAM + 1))
