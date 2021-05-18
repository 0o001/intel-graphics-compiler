/*========================== begin_copyright_notice ============================

Copyright (C) 2019-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "wa_def.h"

#define GLV_REV_ID_A0   SI_REV_ID(0,0)

//******************* Main Wa Initializer for Device Id ********************
// Initialize COMMON/DESKTOP/MOBILE WA using PLATFORM_STEP_APPLICABLE() macro.

void InitGlvWaTable(PWA_TABLE pWaTable, PSKU_FEATURE_TABLE pSkuTable, PWA_INIT_PARAM pWaParam)
{
    //GLV workarounds
    int StepId_GLV = (int)pWaParam->usRevId;

    //=================================================================================================================
    //
    //              GLV WA for all platforms
    //
    //=================================================================================================================

    //---------------------------------------------
    //              IGC
    //---------------------------------------------

    SI_WA_ENABLE(
        WaConservativeRasterization,
        "No Link provided",
        "No Link provided",
        PLATFORM_ALL,
        SI_WA_UNTIL(StepId_GLV, GLV_REV_ID_A0));

    SI_WA_ENABLE(

        WaClearArfDependenciesBeforeEot,
        "No Link provided",
        "No HWSightingLink provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

    SI_WA_ENABLE(

        WaDoNotPushConstantsForAllPulledGSTopologies,
        "No Link provided",
        "No HWSightingLink provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

    SI_WA_ENABLE(

        WaThreadSwitchAfterCall,
        "No Link provided",
        "No HWSightingLink provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

    SI_WA_ENABLE(

        WaClearFlowControlGpgpuContextSave,
        "No Link provided",
        "No HWSightingLink provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

    SI_WA_ENABLE(

        WaBreakF32MixedModeIntoSimd8,
        "No Link provided",
        "No HWSightingLink provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

    SI_WA_ENABLE(

        WaForceMinMaxGSThreadCount,
        "No Link provided",
        "No HWSightingLink provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

    SI_WA_ENABLE(

        WaFloatMixedModeSelNotAllowedWithPackedDestination,
        "No Link provided",
        "No HWSightingLink provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

    SI_WA_ENABLE(

        WaStructuredBufferAsRawBufferOverride,
        "No Link provided",
        "No HWSightingLink provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

    SI_WA_ENABLE(

        WaMixModeSelInstDstNotPacked,
        "No Link provided",
        "No Link provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

    SI_WA_ENABLE(

        WaSamplerResponseLengthMustBeGreaterThan1,
        "No Link provided",
        "No HWSightingLink provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

    SI_WA_ENABLE(

        WaLodRequiredOnTypedMsaaUav,
        "No Link provided",
        "No HWSightingLink provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

    SI_WA_ENABLE(
        WaReturnZeroforRTReadOutsidePrimitive,
        "No Link provided",
        "No HWSightingLink provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

    SI_WA_ENABLE(
        WaResetPSDoesNotWriteToRT,
        "No Link provided",
        "No HWSightingLink provided",
        PLATFORM_ALL,
        SI_WA_FOR_EVER);

}

#ifdef __KCH
void InitGlvHASWaTable(PHW_DEVICE_EXTENSION pKchContext, PWA_TABLE pWaTable, PSKU_FEATURE_TABLE pSkuTable, PWA_INIT_PARAM pWaParam)
{
    //GLV work arounds will be added here
}
#endif // __KCH
