#ifdef NDS3_EPICS

#include <epicsStdlib.h>
#include <iocshRegisterCommon.h>
#include <registryCommon.h>
#include <dbStaticPvt.h>

#include "epicsFactoryImpl.h"
#include "epicsInterfaceImpl.h"
#include "../include/nds3/base.h"

#include <iostream>
#include <fstream>
#include "/usr/include/link.h"
#include <elf.h>
#include <dlfcn.h>

#include <set>
#include <string>
#include "scansymbols.h"

// Include embedded dbd file
#include "../dbd/dbdfile.h"

typedef void (*reg_func)(void);
epicsShareExtern reg_func pvar_func_arrInitialize, pvar_func_asSub,
    pvar_func_asynInterposeEosRegister,
    pvar_func_asynInterposeFlushRegister, pvar_func_asynRegister,
    pvar_func_dbndInitialize, pvar_func_ndsRegister,
    pvar_func_syncInitialize, pvar_func_tsInitialize;

namespace nds
{

extern "C" {

void EpicsFactoryImpl::createNdsDevice(const iocshArgBuf * arguments)
{
    if(arguments[0].sval == 0)
    {
        throw;
    }
    std::string parameter;
    if(arguments[1].sval != 0)
    {
        parameter = arguments[1].sval;
    }
    EpicsFactoryImpl::getInstance().createDriver(arguments[0].sval, parameter);
}

}

EpicsFactoryImpl& EpicsFactoryImpl::getInstance()
{
    static EpicsFactoryImpl factory;

    return factory;
}

EpicsFactoryImpl::EpicsFactoryImpl()
{
    static const iocshArg nameArgument = {"driver", iocshArgString};
    static const iocshArg parameterArgument = {"parameter", iocshArgString};
    static const iocshArg* commandArguments[] = {&nameArgument, &parameterArgument};

    static const std::string commandName("createNdsDevice");
    m_commandDefinition.arg = commandArguments;
    m_commandDefinition.nargs = sizeof(commandArguments) / sizeof(commandArguments[0]);
    m_commandDefinition.name = commandName.c_str();
    iocshRegister(&m_commandDefinition, createNdsDevice);
}

InterfaceBaseImpl* EpicsFactoryImpl::getNewInterface(const std::string& fullName)
{
    return new EpicsInterfaceImpl(fullName);
}


void EpicsFactoryImpl::run(int argc,char * argv[])
{
    iocshRegisterCommon();

    // Save and load the dbd file
    /////////////////////////////
    char tmpBuffer[L_tmpnam];

    std::string tmpFileName(tmpnam_r(tmpBuffer));

    std::string fileName(tmpFileName);
    std::ofstream outputStream(fileName.c_str());
    outputStream.write(dbdfile, sizeof(dbdfile));

    std::string command("dbLoadDatabase ");
    command += tmpFileName;
    iocshCmd(command.c_str());

    registerRecordTypes(*iocshPpdbbase);

    iocsh(0);
}


void EpicsFactoryImpl::registerRecordTypes(DBBASE* pDatabase)
{
    DBEntry dbEntry(pDatabase);

    DynamicModule thisModule;

    symbolsList_t symbols = getSymbols();

    // Find all the recordTypes, devices, variables
    std::set<std::string> recordTypes;
    std::set<std::string> devices;
    std::set<std::string> drivers;
    std::set<std::string> intVariables;
    std::set<std::string> doubleVariables;
    std::set<std::string> stringVariables;

    static const std::string epicsFuncPrefix("pvar_func_");
    static const std::string epicsDsetPrefix("pvar_dset_");
    static const std::string epicsDrvetPrefix("pvar_drvet_");
    static const std::string epicsRsetPostfix("RSET");
    static const std::string epicsSizeOffsetPostfix("RecordSizeOffset");

    static const std::string epicsIntPrefix("pvar_int_");
    static const std::string epicsDoublePrefix("pvar_double_");
    static const std::string epicsStringPrefix("pvar_string_");

    struct variablesHelpStruct
    {
        iocshArgType type;
        std::string prefix;
        std::set<std::string>* list;
    };

    variablesHelpStruct variablesHelp[] =
    {
        {iocshArgInt, epicsIntPrefix, &intVariables},
        {iocshArgDouble, epicsDoublePrefix, &doubleVariables},
        {iocshArgString, epicsStringPrefix, &stringVariables}
    };

    for(symbolsList_t::const_iterator scanSymbols(symbols.begin()), endSymbols(symbols.end()); scanSymbols != endSymbols; ++scanSymbols)
    {
        addToSet(scanSymbols->first, &recordTypes, epicsFuncPrefix, epicsSizeOffsetPostfix);
        addToSet(scanSymbols->first, &devices, epicsDsetPrefix, "");
        addToSet(scanSymbols->first, &drivers, epicsDrvetPrefix, "");
        addToSet(scanSymbols->first, &intVariables, epicsIntPrefix, "");
        addToSet(scanSymbols->first, &doubleVariables, epicsDoublePrefix, "");
        addToSet(scanSymbols->first, &stringVariables, epicsStringPrefix, "");
    }

    // Register all the record types
    ////////////////////////////////
    for(std::set<std::string>::const_iterator scanTypes(recordTypes.begin()), endTypes(recordTypes.end()); scanTypes != endTypes; ++scanTypes)
    {
        symbolsList_t::iterator findRset = symbols.find(*scanTypes + epicsRsetPostfix);
        if(findRset == symbols.end())
        {
            continue;
        }

        std::cout << "Registering type " << *scanTypes << "\n";

        // Collect both the rset and the sizeoffset functions
        const void* sizeOffset = symbols[epicsFuncPrefix + *scanTypes + epicsSizeOffsetPostfix].m_pAddress;
        const void* resetFunction = findRset->second.m_pAddress;

        m_recordTypeFunctions.emplace_back(recordTypeLocation());
        m_recordTypeFunctions.back().prset = (rset*)resetFunction;
        m_recordTypeFunctions.back().sizeOffset = *((computeSizeOffset*)sizeOffset);

        m_recordTypeNames.push_back(*scanTypes);
        m_recordTypeNamesCstr.emplace_back(m_recordTypeNames.back().c_str());
    }

    // Register all the devices
    ///////////////////////////
    for(std::set<std::string>::const_iterator scanDevices(devices.begin()), endDevices(devices.end()); scanDevices != endDevices; ++scanDevices)
    {
        std::cout << "Registering device " << *scanDevices << "\n";

        const void* deviceFunction = symbols[epicsDsetPrefix + *scanDevices].m_pAddress;
        m_deviceFunctions.push_back((dset*)*(dset**)deviceFunction);
        m_deviceNames.push_back(*scanDevices);
        m_deviceNamesCstr.emplace_back(m_deviceNames.back().c_str());
    }

    // Register all the drivers
    ///////////////////////////
    for(std::set<std::string>::const_iterator scanDrivers(drivers.begin()), endDrivers(drivers.end()); scanDrivers != endDrivers; ++scanDrivers)
    {
        std::cout << "Registering driver " << *scanDrivers << "\n";

        const void* driverFunction = symbols[epicsDrvetPrefix + *scanDrivers].m_pAddress;
        m_driverFunctions.push_back((drvet*)driverFunction);
        m_driverNames.push_back(*scanDrivers);
        m_driverNamesCstr.emplace_back(m_driverNames.back().c_str());
    }

    // Register all the variables
    /////////////////////////////
    for(int scanVariablesHelp(0); scanVariablesHelp != sizeof(variablesHelp) / sizeof(variablesHelp[0]); ++scanVariablesHelp)
    {
        std::set<std::string>* pList = variablesHelp[scanVariablesHelp].list;
        for(std::set<std::string>::const_iterator scanVariables(pList->begin()), endVariables(pList->end()); scanVariables != endVariables; ++scanVariables)
        {
            const void* variableFunction = symbols[variablesHelp[scanVariablesHelp].prefix + *scanVariables].m_pAddress;
            m_variableNames.push_back(*scanVariables);
            m_variableFunctions.emplace_back(iocshVarDef());
            m_variableFunctions.back().name = m_variableNames.back().c_str();
            m_variableFunctions.back().type = variablesHelp[scanVariablesHelp].type;
            m_variableFunctions.back().pval = (void*)variableFunction;
        }
    }

    m_variableFunctions.emplace_back(iocshVarDef());
    m_variableFunctions.back().name = 0;
    m_variableFunctions.back().type = iocshArgInt;
    m_variableFunctions.back().pval = 0;

    ::registerRecordTypes(pDatabase, m_recordTypeNames.size(), m_recordTypeNamesCstr.data(), m_recordTypeFunctions.data());

    ::registerDevices(pDatabase, m_deviceNames.size(), m_deviceNamesCstr.data(), m_deviceFunctions.data());

    ::registerDrivers(pDatabase, m_driverNames.size(), m_driverNamesCstr.data(), m_driverFunctions.data());

    pvar_func_arrInitialize();
    pvar_func_asSub();
    pvar_func_asynInterposeEosRegister();
    pvar_func_asynInterposeFlushRegister();
    pvar_func_asynRegister();
    pvar_func_dbndInitialize();
    pvar_func_syncInitialize();
    pvar_func_tsInitialize();

    ::iocshRegisterVariable(m_variableFunctions.data());

}

void EpicsFactoryImpl::addToSet(const std::string symbolName, std::set<std::string>* pSet, const std::string& prefix, const std::string& postfix)
{
    std::string name = compareString(symbolName, prefix, postfix);
    if(!name.empty())
    {
        pSet->insert(name);
    }
}

std::string EpicsFactoryImpl::compareString(const std::string& string, const std::string& prefix, const std::string& postfix)
{
    if(string.size() < prefix.size() || string.size() < postfix.size())
    {
        return "";
    }

    if(string.substr(0, prefix.size()) == prefix && string.substr(string.size() - postfix.size()) == postfix)
    {
        return string.substr(prefix.size(), string.size() - prefix.size() - postfix.size());
    }

    return "";
}

DBEntry::DBEntry(DBBASE* pDatabase)
{
    m_pDBEntry = dbAllocEntry(pDatabase);
}

DBEntry::~DBEntry()
{
    dbFreeEntry(m_pDBEntry);
}

DynamicModule::DynamicModule(): m_moduleHandle(dlopen(0, RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE))
{
}

DynamicModule::~DynamicModule()
{
    if(m_moduleHandle != 0)
    {
        dlclose(m_moduleHandle);
    }
}

void* DynamicModule::getAddress(const std::string& name)
{
    return dlsym(m_moduleHandle, name.c_str());
}


}

#endif // NDS3_EPICS