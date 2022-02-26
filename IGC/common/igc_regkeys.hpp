/*========================== begin_copyright_notice ============================

Copyright (C) 2017-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

/*****************************************************************************\
STRUCT: SRegKeyVariableMetaData
PURPOSE: Defines meta data for holding/defining regkey variables
\*****************************************************************************/
#pragma once
#include "IGC/common/igc_debug.h"
#include "IGC/common/igc_flags.hpp"
#include "common/SysUtils.hpp"
#include <string>

typedef char debugString[256];

#if defined(_DEBUG) || defined(_INTERNAL)
#define IGC_DEBUG_VARIABLES
#endif

#if defined(__linux__)
#define IGC_DEBUG_VARIABLES
#endif


#if defined(IGC_DEBUG_VARIABLES)
#include <vector>
struct HashRange
{
    unsigned long long start;
    unsigned long long end;
    union
    {
        unsigned    m_Value;
        debugString m_string;
    };
};

struct SRegKeyVariableMetaData
{
    union
    {
        unsigned    m_Value;
        debugString m_string;
    };
    std::vector<HashRange> hashes;
    bool m_isSetToNonDefaultValue;
    virtual const char* GetName() const = 0;
    virtual unsigned GetDefault() const = 0;
    virtual void SetToNonDefaultValue() = 0;
    virtual ~SRegKeyVariableMetaData()
    {
    }
};

#if defined (__linux__) && !defined(_DEBUG) && !defined(_INTERNAL)
#define LINUX_RELEASE_MODE
#endif

/*****************************************************************************\
MACRO: IGC_REGKEY
PURPOSE: Declares a new regkey variable.
\*****************************************************************************/
#define IGC_REGKEY(dataType, regkeyName, defaultValue, description, releaseMode)    \
struct SRegKeyVariableMetaData_##regkeyName : public SRegKeyVariableMetaData \
{                                                   \
    SRegKeyVariableMetaData_##regkeyName()          \
    {                                               \
        m_Value = (unsigned)defaultValue;           \
        m_isSetToNonDefaultValue = false;           \
    }                                               \
    const char* GetName() const                     \
    {                                               \
        return #regkeyName;                         \
    }                                               \
    unsigned GetDefault() const                     \
    {                                               \
        return (unsigned)defaultValue;              \
    }                                               \
    bool IsSetToNonDefaultValue() const             \
    {                                               \
        return m_isSetToNonDefaultValue;            \
    }                                               \
    void SetToNonDefaultValue()                     \
    {                                               \
        m_isSetToNonDefaultValue = true;            \
    }                                               \
    bool IsReleaseMode() const                      \
    {                                               \
        return releaseMode;                         \
    }                                               \
} regkeyName

// XMACRO defining the regkeys
#define DECLARE_IGC_REGKEY(dataType, regkeyName, defaultValue, description, releaseMode) \
    IGC_REGKEY(dataType, regkeyName, defaultValue, description, releaseMode);
struct SRegKeysList
{
#include "igc_regkeys.h"
};
#undef DECLARE_IGC_REGKEY
bool CheckHashRange(SRegKeyVariableMetaData& varname);
extern SRegKeysList g_RegKeyList;
#if defined(LINUX_RELEASE_MODE)
#define IGC_GET_FLAG_VALUE(name)                 \
  ((CheckHashRange(g_RegKeyList.name) && g_RegKeyList.name.IsReleaseMode()) ? g_RegKeyList.name.m_Value : g_RegKeyList.name.GetDefault())
#define IGC_GET_FLAG_DEFAULT_VALUE(name)         (g_RegKeyList.name.GetDefault())
#define IGC_IS_FLAG_ENABLED(name)                (IGC_GET_FLAG_VALUE(name) != 0)
#define IGC_IS_FLAG_DISABLED(name)               (!IGC_IS_FLAG_ENABLED(name))
#define IGC_SET_FLAG_VALUE(name, regkeyValue)    (g_RegKeyList.name.m_Value = regkeyValue)
#define IGC_GET_REGKEYSTRING(name)               \
  ((CheckHashRange(g_RegKeyList.name) && g_RegKeyList.name.IsReleaseMode()) ? g_RegKeyList.name.m_string : "")
#else
#define IGC_GET_FLAG_VALUE(name)                 \
  (CheckHashRange(g_RegKeyList.name) ? g_RegKeyList.name.m_Value : g_RegKeyList.name.GetDefault())
#define IGC_GET_FLAG_DEFAULT_VALUE(name)         (g_RegKeyList.name.GetDefault())
#define IGC_IS_FLAG_ENABLED(name)                (IGC_GET_FLAG_VALUE(name) != 0)
#define IGC_IS_FLAG_DISABLED(name)               (!IGC_IS_FLAG_ENABLED(name))
#define IGC_SET_FLAG_VALUE(name, regkeyValue)    (g_RegKeyList.name.m_Value = regkeyValue)
#define IGC_GET_REGKEYSTRING(name)               \
  (CheckHashRange(g_RegKeyList.name) ? g_RegKeyList.name.m_string : "")
#endif

#define IGC_REGKEY_OR_FLAG_ENABLED(name, flag) (IGC_IS_FLAG_ENABLED(name) || IGC::Debug::GetDebugFlag(IGC::Debug::DebugFlag::flag))

#if defined(_WIN64) || defined(_WIN32)
struct DEVICE_INFO
{
    std::string description;
    DWORD deviceID;
    DWORD revisionID;
    DWORD pciBus;
    DWORD pciDevice;
    DWORD pciFunction;
    std::string driverRegistryPath;

    DEVICE_INFO(DEVINST deviceInstance);

    void get_device_property(DEVINST deviceInstace, DWORD property);
};
#endif

void GetKeysSetExplicitly(std::string* KeyValuePairs, std::string* OptionKeys);
void DumpIGCRegistryKeyDefinitions();
void DumpIGCRegistryKeyDefinitions3(std::string driverRegistryPath, unsigned long pciBus, unsigned long pciDevice, unsigned long pciFunction);
void LoadRegistryKeys(const std::string& options = "", bool *RegFlagNameError = nullptr);
void SetCurrentDebugHash(unsigned long long hash);
#undef LINUX_RELEASE_MODE
#else
static inline void GetKeysSetExplicitly(std::string* KeyValuePairs, std::string* OptionKeys) {}
static inline void SetCurrentDebugHash(unsigned long long hash) {}
static inline void LoadRegistryKeys(const std::string& options = "", bool *RegFlagNameError=nullptr) {}
#define IGC_SET_FLAG_VALUE(name, regkeyValue)
#define DECLARE_IGC_REGKEY(dataType, regkeyName, defaultValue, description, releaseMode) \
    static const unsigned int regkeyName##default = (unsigned int)defaultValue;
namespace IGC
{
    class DebugVariable
    {
    public:
#include "igc_regkeys.h"
    };
};
#undef DECLARE_IGC_REGKEY

#define IGC_IS_FLAG_ENABLED(name)     (IGC::DebugVariable::name##default != 0)
#define IGC_IS_FLAG_DISABLED(name)    (IGC::DebugVariable::name##default == 0)
#define IGC_GET_FLAG_VALUE(name)      (IGC::DebugVariable::name##default)
#define IGC_GET_FLAG_DEFAULT_VALUE(name) IGC_GET_FLAG_VALUE(name)
#define IGC_GET_REGKEYSTRING(name)    ("")
#define IGC_REGKEY_OR_FLAG_ENABLED(name, flag) \
  (IGC_IS_FLAG_ENABLED(name) || IGC::Debug::GetDebugFlag(IGC::Debug::DebugFlag::flag))
#endif
