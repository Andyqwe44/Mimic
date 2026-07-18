/**
 * export.h — Shared DLL export/import macro for MimicClient modules.
 *
 * Usage:
 *   In the DLL being built:   define GAM_BUILD_DLL before including this header.
 *   In the consumer (EXE):    do NOT define GAM_BUILD_DLL — functions are imported.
 *
 *   #include "export.h"
 *   GAM_API int my_function(int x);
 */
#pragma once

#ifdef _WIN32
  #ifdef GAM_BUILD_DLL
    #define GAM_API __declspec(dllexport)
  #else
    #define GAM_API __declspec(dllimport)
  #endif
#else
  #define GAM_API
#endif
