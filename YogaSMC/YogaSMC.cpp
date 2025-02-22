//
//  YogaSMC.cpp
//  YogaSMC
//
//  Created by Zhen on 7/29/20.
//  Copyright © 2020 Zhen. All rights reserved.
//

#include "YogaSMC.hpp"

OSDefineMetaClassAndStructors(YogaSMC, YogaBaseService);

bool ADDPR(debugEnabled) = false;
uint32_t ADDPR(debugPrintDelay) = 0;

static const struct sensorPair presetTemperatureDeciKelvin[] = {
    {KeyTB0T(0), "Battery"},
    {KeyTB0T(1), "Battery Sensor 1"},
    {KeyTB0T(2), "Battery Sensor 2"}
};

static const struct sensorPair presetTemperature[] = {
    {KeyTCSA,    "CPU System Agent Core"},
    {KeyTCXC,    "CPU Core PECI"},
    // Laptops only have 1 key for both channel
    {KeyTM0P,    "Memory Proximity"},
    // Desktops
    {KeyTM0p(0), "SO-DIMM 1 Proximity"},
    {KeyTM0p(1), "SO-DIMM 2 Proximity"},
    {KeyTM0p(2), "SO-DIMM 3 Proximity"},
    {KeyTM0p(3), "SO-DIMM 4 Proximity"},
    {KeyTPCD,    "Platform Controller Hub Die"},
    {KeyTW0P,    "Airport Proximity"},
    {KeyTaLC,    "Airflow Left"},
    {KeyTaRC,    "Airflow Right"},
    {KeyTh0H(1), "Fin Stack Proximity Right"},
    {KeyTh0H(2), "Fin Stack Proximity Left"},
    {KeyTs0P(0), "Palm Rest"},
    {KeyTs0P(1), "Trackpad Actuator"}
};

void YogaSMC::addVSMCKey() {
    // ACPI-based
    if (!conf || !ec)
        return;

    ECSensorBase = sensorCount;

    OSDictionary *status = OSDictionary::withCapacity(1);
    OSString *method;

    for (auto &pair : presetTemperatureDeciKelvin)
        addECKeySp(pair.key, pair.name, atomicSpDeciKelvinKey);

    for (auto &pair : presetTemperature)
        addECKeySp(pair.key, pair.name, atomicSpKey);

    setProperty("DirectECKey", status);
    status->release();
}

bool YogaSMC::start(IOService *provider) {
    if (!super::start(provider))
        return false;

    DebugLog("Starting");

    validateEC();

    awake = true;

    if (!(poller = initPoller()) ||
        (workLoop->addEventSource(poller) != kIOReturnSuccess)) {
        AlwaysLog("Failed to add poller");
        return false;
    }

    // WARNING: watch out, key addition is sorted here!
    addVSMCKey();
    qsort(const_cast<VirtualSMCKeyValue *>(vsmcPlugin.data.data()), vsmcPlugin.data.size(), sizeof(VirtualSMCKeyValue), VirtualSMCKeyValue::compare);
    setProperty("Status", vsmcPlugin.data.size(), 32);

    vsmcNotifier = VirtualSMCAPI::registerHandler(vsmcNotificationHandler, this);

    poller->setTimeoutMS(POLLING_INTERVAL);
    poller->enable();
    registerService();
    return true;
}

void YogaSMC::stop(IOService *provider)
{
    DebugLog("Stopping");

    poller->disable();
    workLoop->removeEventSource(poller);
    OSSafeReleaseNULL(poller);

    terminate();

    super::stop(provider);
}

bool YogaSMC::vsmcNotificationHandler(void *sensors, void *refCon, IOService *vsmc, IONotifier *notifier) {
    auto self = OSDynamicCast(YogaSMC, reinterpret_cast<OSMetaClassBase*>(sensors));
    if (sensors && vsmc) {
        DBGLOG("yogasmc", "got vsmc notification");
        auto &plugin = self->vsmcPlugin;
        auto ret = vsmc->callPlatformFunction(VirtualSMCAPI::SubmitPlugin, true, sensors, &plugin, nullptr, nullptr);
        if (ret == kIOReturnSuccess) {
            DBGLOG("yogasmc", "submitted plugin");
            return true;
        } else if (ret != kIOReturnUnsupported) {
            SYSLOG("yogasmc", "plugin submission failure %X", ret);
        } else {
            DBGLOG("yogasmc", "plugin submission to non vsmc");
        }
    } else {
        SYSLOG("yogasmc", "got null vsmc notification");
    }
    return false;
}

YogaSMC* YogaSMC::withDevice(IOService *provider, IOACPIPlatformDevice *device) {
    YogaSMC* drv = OSTypeAlloc(YogaSMC);

    drv->conf = OSDictionary::withDictionary(OSDynamicCast(OSDictionary, provider->getProperty("Sensors")));

    OSDictionary *dictionary = OSDictionary::withCapacity(1);
    dictionary->setObject("Sensors", drv->conf);

    drv->ec = device;
    drv->name = device->getName();

    if (!drv->init(dictionary) ||
        !drv->attach(provider)) {
        OSSafeReleaseNULL(drv);
    }

    dictionary->release();
    return drv;
}

void YogaSMC::updateEC() {
    UInt32 result = 0;
    for (UInt8 i = ECSensorBase; i < sensorCount; i++)
        if (ec->evaluateInteger(sensorMethods[i-1], &result) == kIOReturnSuccess && result != 0)
            atomic_store_explicit(&currentSensor[i-1], result, memory_order_release);
    poller->setTimeoutMS(POLLING_INTERVAL);
}

IOReturn YogaSMC::setPowerState(unsigned long powerStateOrdinal, IOService * whatDevice) {
    if (super::setPowerState(powerStateOrdinal, whatDevice) != kIOPMAckImplied)
        return kIOReturnInvalid;

    if (powerStateOrdinal == 0) {
        if (awake) {
            poller->disable();
            workLoop->removeEventSource(poller);
            awake = false;
            DebugLog("Going to sleep");
        }
    } else {
        if (!awake) {
            awake = true;
            workLoop->addEventSource(poller);
            poller->setTimeoutMS(POLLING_INTERVAL);
            poller->enable();
            DebugLog("Woke up");
        }
    }
    return kIOPMAckImplied;
}

EXPORT extern "C" kern_return_t ADDPR(kern_start)(kmod_info_t *, void *) {
    // Report success but actually do not start and let I/O Kit unload us.
    // This works better and increases boot speed in some cases.
    PE_parse_boot_argn("liludelay", &ADDPR(debugPrintDelay), sizeof(ADDPR(debugPrintDelay)));
    ADDPR(debugEnabled) = checkKernelArgument("-vsmcdbg");
    return KERN_SUCCESS;
}

EXPORT extern "C" kern_return_t ADDPR(kern_stop)(kmod_info_t *, void *) {
    // It is not safe to unload VirtualSMC plugins!
    return KERN_FAILURE;
}
