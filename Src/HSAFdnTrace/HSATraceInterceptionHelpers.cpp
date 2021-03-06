//==============================================================================
// Copyright (c) 2015 Advanced Micro Devices, Inc. All rights reserved.
/// \author AMD Developer Tools Team
/// \file
/// \brief  This file contains functions called by various intercepted APIs
//==============================================================================

#include <Defs.h>
#include <Logger.h>
#include <GlobalSettings.h>

#include "HSARTModuleLoader.h"
#include "ROCProfilerModule.h"
#include "AutoGenerated/HSATraceInterception.h"

#include "AutoGenerated/HSATraceStringOutput.h"

#include "HSAFdnAPIInfoManager.h"
#include "FinalizerInfoManager.h"
#include "HSATraceInterceptionHelpers.h"
#include "HSASignalPool.h"
#include "HSAAqlPacketTimeCollector.h"

// TODO: disable completion callback will result in profiling hang,
// also, we are collecting timestamps in the OnUnloadTool() function rather than this callback
// because there is a missing completion callback after the dispatch and thus there will be a missing timestamp.
// This is a workaround that need to be revisited.
bool CompletionCallback(rocprofiler_group_t group, void* pData)
{
    SP_UNREFERENCED_PARAMETER(group);
    SP_UNREFERENCED_PARAMETER(pData);

    return true;
}

// This is here to work around SWDEV-170322 -- queue destruction fails if a queue destroy callback is not registered
hsa_status_t DestroyCallback(hsa_queue_t*, void*)
{
    return HSA_STATUS_SUCCESS;
}

hsa_status_t DispatchCallback(const rocprofiler_callback_data_t* pCallbackData, void* pUserData, rocprofiler_group_t* pGroup)
{
    SP_UNREFERENCED_PARAMETER(pUserData);

    hsa_status_t status = HSA_STATUS_ERROR;

    rocprofiler_t* pContext = nullptr;
    ContextEntry* pEntry = new ContextEntry();

    rocprofiler_properties_t properties{};
    properties.handler = CompletionCallback;
    properties.handler_arg = reinterpret_cast<void*>(pEntry);

    ROCProfilerModule* pROCProfilerModule = HSARTModuleLoader<ROCProfilerModule>::Instance()->GetHSARTModule();
    if (nullptr != pROCProfilerModule && pROCProfilerModule->IsModuleLoaded())
    {
        status = pROCProfilerModule->rocprofiler_open(pCallbackData->agent, nullptr, 0, &pContext, 0, &properties);
        if (HSA_STATUS_SUCCESS != status)
        {
            GPULogger::Log(GPULogger::logERROR, "Error returned from rocprofiler_open() in ROCProfiler dispatch callback\n");
        }

        status = pROCProfilerModule->rocprofiler_get_group(pContext, 0, pGroup);
        if (HSA_STATUS_SUCCESS != status)
        {
            GPULogger::Log(GPULogger::logERROR, "Error returned from rocprofiler_get_group() in ROCProfiler dispatch callback\n");
        }

        pEntry->m_agent = pCallbackData->agent;
        pEntry->m_group = *pGroup;
        pEntry->m_data = *pCallbackData;
        pEntry->m_data.kernel_name = strdup(pCallbackData->kernel_name);
        pEntry->m_isValid = true;

        HSAAqlKernelDispatchPacket* pAqlPacket = new(std::nothrow) HSAAqlKernelDispatchPacket(*(pEntry->m_data.packet));

        if (nullptr != pAqlPacket)
        {
            pAqlPacket->m_agent = pCallbackData->agent;
            pAqlPacket->m_pQueue = const_cast<hsa_queue_t*>(pCallbackData->queue);
            pAqlPacket->m_pContextEntry = pEntry;
            pAqlPacket->m_isReady = false;
            pAqlPacket->m_isRocProfilerPacket = true;
            HSAAPIInfoManager::Instance()->AddAqlPacketEntry(reinterpret_cast<HSAAqlPacketBase*>(pAqlPacket));
        }
    }

    return status;
}

void HSA_APITrace_hsa_queue_create_PostCallHelper(hsa_status_t retVal, hsa_agent_t agent, uint32_t size, hsa_queue_type32_t type, void(*callback)(hsa_status_t status, hsa_queue_t* source,
                                                  void* data), void* data, uint32_t private_segment_size, uint32_t group_segment_size, hsa_queue_t** queue)
{
    SP_UNREFERENCED_PARAMETER(agent);
    SP_UNREFERENCED_PARAMETER(size);
    SP_UNREFERENCED_PARAMETER(type);
    SP_UNREFERENCED_PARAMETER(callback);
    SP_UNREFERENCED_PARAMETER(data);
    SP_UNREFERENCED_PARAMETER(private_segment_size);
    SP_UNREFERENCED_PARAMETER(group_segment_size);

    if (HSA_STATUS_SUCCESS == retVal && nullptr != queue)
    {
        g_pRealAmdExtFunctions->hsa_amd_profiling_async_copy_enable_fn(true);
        HSAAPIInfoManager::Instance()->AddQueue(*queue);

        ROCProfilerModule* pROCProfilerModule = HSARTModuleLoader<ROCProfilerModule>::Instance()->GetHSARTModule();

        if (nullptr != pROCProfilerModule && pROCProfilerModule->IsModuleLoaded())
        {
            rocprofiler_queue_callbacks_t queueCallbacks{};
            queueCallbacks.dispatch = DispatchCallback;
            queueCallbacks.destroy = DestroyCallback;

            hsa_status_t status = pROCProfilerModule->rocprofiler_set_queue_callbacks(queueCallbacks, nullptr);
            if (HSA_STATUS_SUCCESS != status)
            {
                GPULogger::Log(GPULogger::logERROR, "Error returned from rocprofiler_set_queue_callbacks()\n");
            }
        }
    }
}

void HSA_APITrace_hsa_executable_get_symbol_PostCallHelper(hsa_status_t retVal, hsa_executable_t executable, const char* module_name, const char* symbol_name, hsa_agent_t agent, int32_t call_convention, hsa_executable_symbol_t* symbol)
{
    SP_UNREFERENCED_PARAMETER(executable);
    SP_UNREFERENCED_PARAMETER(module_name);
    SP_UNREFERENCED_PARAMETER(agent);
    SP_UNREFERENCED_PARAMETER(call_convention);

    if (HSA_STATUS_SUCCESS == retVal && nullptr != symbol)
    {
        if (nullptr != symbol_name)
        {
            Log(logMESSAGE, "HSA_API_Trace_hsa_executable_get_symbol: Adding symbol handle/symbol name pair to FinalizerInfoManager\n");
            Log(logMESSAGE, "  SymHandle: %llu, SymName: %s \n", symbol->handle, symbol_name);
            FinalizerInfoManager::Instance()->m_symbolHandleToNameMap[symbol->handle] = std::string(symbol_name);

            uint64_t kernelObject;

            if (g_pRealCoreFunctions->hsa_executable_symbol_get_info_fn(*symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &kernelObject) == HSA_STATUS_SUCCESS)
            {
                Log(logMESSAGE, "HSA_API_Trace_hsa_executable_get_symbol: Adding code handle/symbol handle pair to FinalizerInfoManager\n");
                Log(logMESSAGE, "  CodeHandle: %llu, SymHandle: %llu\n", kernelObject, symbol->handle);
                FinalizerInfoManager::Instance()->m_codeHandleToSymbolHandleMap[kernelObject] = symbol->handle;
            }
        }
    }
}


void HSA_APITrace_hsa_executable_get_symbol_by_name_PostCallHelper(hsa_status_t retVal, hsa_executable_t executable, const char* symbol_name, const hsa_agent_t* agent, hsa_executable_symbol_t* symbol)
{
    SP_UNREFERENCED_PARAMETER(executable);
    SP_UNREFERENCED_PARAMETER(agent);

    if (HSA_STATUS_SUCCESS == retVal && nullptr != symbol)
    {
        if (nullptr != symbol_name)
        {
            Log(logMESSAGE, "HSA_API_Trace_hsa_executable_get_symbol_by_name: Adding symbol handle/symbol name pair to FinalizerInfoManager\n");
            Log(logMESSAGE, "  SymHandle: %llu, SymName: %s \n", symbol->handle, symbol_name);
            FinalizerInfoManager::Instance()->m_symbolHandleToNameMap[symbol->handle] = std::string(symbol_name);

            uint64_t kernelObject;

            if (g_pRealCoreFunctions->hsa_executable_symbol_get_info_fn(*symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &kernelObject) == HSA_STATUS_SUCCESS)
            {
                Log(logMESSAGE, "HSA_API_Trace_hsa_executable_get_symbol_by_name: Adding code handle/symbol handle pair to FinalizerInfoManager\n");
                Log(logMESSAGE, "  CodeHandle: %llu, SymHandle: %llu\n", kernelObject, symbol->handle);
                FinalizerInfoManager::Instance()->m_codeHandleToSymbolHandleMap[kernelObject] = symbol->handle;
            }
        }
    }
}


void HSA_APITrace_hsa_executable_symbol_get_info_PostCallHelper(hsa_status_t retVal, hsa_executable_symbol_t executable_symbol, hsa_executable_symbol_info_t attribute, void* value)
{
    SP_UNREFERENCED_PARAMETER(attribute);

    if (HSA_STATUS_SUCCESS == retVal && nullptr != value)
    {
        uint32_t symbolNameLength = 0;

        if (g_pRealCoreFunctions->hsa_executable_symbol_get_info_fn(executable_symbol, HSA_EXECUTABLE_SYMBOL_INFO_NAME_LENGTH, &symbolNameLength) == HSA_STATUS_SUCCESS)
        {
            if (symbolNameLength > 0)
            {
                char* tempSymbolName = new(std::nothrow) char[symbolNameLength + 1];

                if (nullptr != tempSymbolName)
                {
                    memcpy(tempSymbolName, value, symbolNameLength);
                    tempSymbolName[symbolNameLength] = '\0';
                    Log(logMESSAGE, "HSA_API_Trace_hsa_executable_get_symbol: Adding symbol handle/symbol name pair to FinalizerInfoManager\n");
                    Log(logMESSAGE, "  SymHandle: %llu, SymName: %s \n", executable_symbol.handle, tempSymbolName);
                    FinalizerInfoManager::Instance()->m_symbolHandleToNameMap[executable_symbol.handle] = std::string(tempSymbolName);
                    delete[] tempSymbolName;

                    uint64_t kernelObject;

                    if (g_pRealCoreFunctions->hsa_executable_symbol_get_info_fn(executable_symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &kernelObject) == HSA_STATUS_SUCCESS)
                    {
                        Log(logMESSAGE, "HSA_API_Trace_hsa_executable_get_symbol: Adding code handle/symbol handle pair to FinalizerInfoManager\n");
                        Log(logMESSAGE, "  CodeHandle: %llu, SymHandle: %llu\n", kernelObject, executable_symbol.handle);
                        FinalizerInfoManager::Instance()->m_codeHandleToSymbolHandleMap[kernelObject] = executable_symbol.handle;
                    }
                }
            }
        }
    }
}

void HSA_APITrace_hsa_amd_memory_async_copy_PreCallHelper(void* dst, hsa_agent_t dst_agent, const void* src, hsa_agent_t src_agent, size_t size, uint32_t num_dep_signals, const hsa_signal_t* dep_signals, hsa_signal_t& completion_signal, unsigned long long asyncCopyIdentifier)
{
    SP_UNREFERENCED_PARAMETER(dst);
    SP_UNREFERENCED_PARAMETER(dst_agent);
    SP_UNREFERENCED_PARAMETER(src);
    SP_UNREFERENCED_PARAMETER(src_agent);
    SP_UNREFERENCED_PARAMETER(size);
    SP_UNREFERENCED_PARAMETER(num_dep_signals);
    SP_UNREFERENCED_PARAMETER(dep_signals);

    if (HSAAPIInfoManager::Instance()->IsHsaTransferTimeDisabled())
    {
        return;
    }

    if (0 != completion_signal.handle)
    {
        hsa_signal_value_t origValue = g_pRealCoreFunctions->hsa_signal_load_scacquire_fn(completion_signal);

        hsa_signal_t replacementSignal;

        if (HSAAPIInfoManager::Instance()->IsCapReached() || !HSASignalPool::Instance()->AcquireSignal(origValue, replacementSignal))
        {
            replacementSignal = completion_signal;
        }
        else
        {
            HSAAPIInfoManager::Instance()->AddReplacementAsyncCopySignal(completion_signal, replacementSignal);
            HSAAPIInfoManager::Instance()->AddAsyncCopyCompletionSignal(replacementSignal, asyncCopyIdentifier);
            completion_signal = replacementSignal;
        }
    }
}

void HSA_APITrace_hsa_amd_memory_async_copy_rect_PreCallHelper(const hsa_pitched_ptr_t* dst, const hsa_dim3_t* dst_offset, const hsa_pitched_ptr_t* src, const hsa_dim3_t* src_offset, const hsa_dim3_t* range, hsa_agent_t copy_agent, hsa_amd_copy_direction_t dir, uint32_t num_dep_signals, const hsa_signal_t* dep_signals, hsa_signal_t completion_signal, unsigned long long asyncCopyIdentifier)
{
    SP_UNREFERENCED_PARAMETER(dst);
    SP_UNREFERENCED_PARAMETER(dst_offset);
    SP_UNREFERENCED_PARAMETER(src);
    SP_UNREFERENCED_PARAMETER(src_offset);
    SP_UNREFERENCED_PARAMETER(range);
    SP_UNREFERENCED_PARAMETER(copy_agent);
    SP_UNREFERENCED_PARAMETER(dir);
    SP_UNREFERENCED_PARAMETER(num_dep_signals);
    SP_UNREFERENCED_PARAMETER(dep_signals);

    if (HSAAPIInfoManager::Instance()->IsHsaTransferTimeDisabled())
    {
        return;
    }

    if (0 != completion_signal.handle)
    {
        hsa_signal_value_t origValue = g_pRealCoreFunctions->hsa_signal_load_scacquire_fn(completion_signal);

        hsa_signal_t replacementSignal;

        if (HSAAPIInfoManager::Instance()->IsCapReached() || !HSASignalPool::Instance()->AcquireSignal(origValue, replacementSignal))
        {
            replacementSignal = completion_signal;
        }
        else
        {
            HSAAPIInfoManager::Instance()->AddReplacementAsyncCopySignal(completion_signal, replacementSignal);
            HSAAPIInfoManager::Instance()->AddAsyncCopyCompletionSignal(replacementSignal, asyncCopyIdentifier);
            completion_signal = replacementSignal;
        }
    }
}

void HSA_APITrace_hsa_system_get_extension_table_PostCallHelper(hsa_status_t retVal, uint16_t extension, uint16_t version_major, uint16_t version_minor, void* table)
{
    SP_UNREFERENCED_PARAMETER(version_major);
    SP_UNREFERENCED_PARAMETER(version_minor);

    if (HSA_STATUS_SUCCESS == retVal && nullptr != table)
    {
        if (HSA_EXTENSION_AMD_LOADER == extension)
        {
            hsa_ven_amd_loader_1_01_pfn_t* tableReplacer = reinterpret_cast<hsa_ven_amd_loader_1_01_pfn_t*>(table);
            size_t tableSize = sizeof(hsa_ven_amd_loader_1_01_pfn_t);
            g_pRealLoaderExtFunctions = reinterpret_cast<hsa_ven_amd_loader_1_01_pfn_t*>(malloc(tableSize));
            memcpy(g_pRealLoaderExtFunctions, tableReplacer, tableSize);

            tableReplacer->hsa_ven_amd_loader_query_host_address = HSA_API_Trace_hsa_ven_amd_loader_query_host_address;
            tableReplacer->hsa_ven_amd_loader_query_segment_descriptors = HSA_API_Trace_hsa_ven_amd_loader_query_segment_descriptors;
            tableReplacer->hsa_ven_amd_loader_query_executable = HSA_API_Trace_hsa_ven_amd_loader_query_executable;

            if (version_minor > 0)
            {
                //these methods added in minor version 1
                tableReplacer->hsa_ven_amd_loader_executable_iterate_loaded_code_objects = HSA_API_Trace_hsa_ven_amd_loader_executable_iterate_loaded_code_objects;
                tableReplacer->hsa_ven_amd_loader_loaded_code_object_get_info = HSA_API_Trace_hsa_ven_amd_loader_loaded_code_object_get_info;
            }
        }
        else if (HSA_EXTENSION_AMD_AQLPROFILE == extension)
        {
            hsa_ven_amd_aqlprofile_pfn_t* tableReplacer = reinterpret_cast<hsa_ven_amd_aqlprofile_pfn_t*>(table);
            size_t tableSize = sizeof(hsa_ven_amd_aqlprofile_pfn_t);
            g_pRealAqlProfileExtFunctions = reinterpret_cast<hsa_ven_amd_aqlprofile_pfn_t*>(malloc(tableSize));
            memcpy(g_pRealAqlProfileExtFunctions, tableReplacer, tableSize);

            tableReplacer->hsa_ven_amd_aqlprofile_version_major = HSA_API_Trace_hsa_ven_amd_aqlprofile_version_major;
            tableReplacer->hsa_ven_amd_aqlprofile_version_minor = HSA_API_Trace_hsa_ven_amd_aqlprofile_version_minor;
            tableReplacer->hsa_ven_amd_aqlprofile_validate_event = HSA_API_Trace_hsa_ven_amd_aqlprofile_validate_event;
            tableReplacer->hsa_ven_amd_aqlprofile_start = HSA_API_Trace_hsa_ven_amd_aqlprofile_start;
            tableReplacer->hsa_ven_amd_aqlprofile_stop = HSA_API_Trace_hsa_ven_amd_aqlprofile_stop;
            tableReplacer->hsa_ven_amd_aqlprofile_read = HSA_API_Trace_hsa_ven_amd_aqlprofile_read;
            tableReplacer->hsa_ven_amd_aqlprofile_legacy_get_pm4 = HSA_API_Trace_hsa_ven_amd_aqlprofile_legacy_get_pm4;
            tableReplacer->hsa_ven_amd_aqlprofile_get_info = HSA_API_Trace_hsa_ven_amd_aqlprofile_get_info;
            tableReplacer->hsa_ven_amd_aqlprofile_iterate_data = HSA_API_Trace_hsa_ven_amd_aqlprofile_iterate_data;
            tableReplacer->hsa_ven_amd_aqlprofile_error_string = HSA_API_Trace_hsa_ven_amd_aqlprofile_error_string;
        }
    }
}

void HSA_APITrace_hsa_system_get_major_extension_table_PostCallHelper(hsa_status_t retVal, uint16_t extension, uint16_t version_major, size_t table_length, void* table)
{
    SP_UNREFERENCED_PARAMETER(version_major);

    if (HSA_STATUS_SUCCESS == retVal && nullptr != table)
    {
        if (HSA_EXTENSION_AMD_LOADER == extension)
        {
            hsa_ven_amd_loader_1_01_pfn_t* tableReplacer = reinterpret_cast<hsa_ven_amd_loader_1_01_pfn_t*>(table);
            size_t tableSize = std::min(table_length, sizeof(hsa_ven_amd_loader_1_01_pfn_t));
            g_pRealLoaderExtFunctions = reinterpret_cast<hsa_ven_amd_loader_1_01_pfn_t*>(malloc(tableSize));
            memcpy(g_pRealLoaderExtFunctions, tableReplacer, tableSize);

            size_t requiredTableSize = sizeof(void*);

            if (table_length >= requiredTableSize)
            {
                requiredTableSize += sizeof(void*);
                tableReplacer->hsa_ven_amd_loader_query_host_address = HSA_API_Trace_hsa_ven_amd_loader_query_host_address;
            }

            if (table_length >= requiredTableSize)
            {
                requiredTableSize += sizeof(void*);
                tableReplacer->hsa_ven_amd_loader_query_segment_descriptors = HSA_API_Trace_hsa_ven_amd_loader_query_segment_descriptors;
            }

            if (table_length >= requiredTableSize)
            {
                requiredTableSize += sizeof(void*);
                tableReplacer->hsa_ven_amd_loader_query_executable = HSA_API_Trace_hsa_ven_amd_loader_query_executable;
            }

            if (table_length >= requiredTableSize)
            {
                requiredTableSize += sizeof(void*);

                tableReplacer->hsa_ven_amd_loader_executable_iterate_loaded_code_objects = HSA_API_Trace_hsa_ven_amd_loader_executable_iterate_loaded_code_objects;
            }

            if (table_length >= requiredTableSize)
            {
                requiredTableSize += sizeof(void*);
                tableReplacer->hsa_ven_amd_loader_loaded_code_object_get_info = HSA_API_Trace_hsa_ven_amd_loader_loaded_code_object_get_info;
            }
        }
        else if (HSA_EXTENSION_AMD_AQLPROFILE == extension)
        {
            hsa_ven_amd_aqlprofile_pfn_t* tableReplacer = reinterpret_cast<hsa_ven_amd_aqlprofile_pfn_t*>(table);
            size_t tableSize = std::min(table_length, sizeof(hsa_ven_amd_aqlprofile_pfn_t));
            g_pRealAqlProfileExtFunctions = reinterpret_cast<hsa_ven_amd_aqlprofile_pfn_t*>(malloc(tableSize));
            memcpy(g_pRealAqlProfileExtFunctions, tableReplacer, tableSize);

            size_t requiredTableSize = sizeof(void*);

            if (table_length >= requiredTableSize)
            {
                requiredTableSize += sizeof(void*);
                tableReplacer->hsa_ven_amd_aqlprofile_validate_event = HSA_API_Trace_hsa_ven_amd_aqlprofile_validate_event;
            }

            if (table_length >= requiredTableSize)
            {
                requiredTableSize += sizeof(void*);
                tableReplacer->hsa_ven_amd_aqlprofile_start = HSA_API_Trace_hsa_ven_amd_aqlprofile_start;
            }

            if (table_length >= requiredTableSize)
            {
                requiredTableSize += sizeof(void*);
                tableReplacer->hsa_ven_amd_aqlprofile_stop = HSA_API_Trace_hsa_ven_amd_aqlprofile_stop;
            }

            if (table_length >= requiredTableSize)
            {
                requiredTableSize += sizeof(void*);
                tableReplacer->hsa_ven_amd_aqlprofile_legacy_get_pm4 = HSA_API_Trace_hsa_ven_amd_aqlprofile_legacy_get_pm4;
            }

            if (table_length >= requiredTableSize)
            {
                requiredTableSize += sizeof(void*);
                tableReplacer->hsa_ven_amd_aqlprofile_get_info = HSA_API_Trace_hsa_ven_amd_aqlprofile_get_info;
            }

            if (table_length >= requiredTableSize)
            {
                requiredTableSize += sizeof(void*);
                tableReplacer->hsa_ven_amd_aqlprofile_iterate_data = HSA_API_Trace_hsa_ven_amd_aqlprofile_iterate_data;
            }

            if (table_length >= requiredTableSize)
            {
                requiredTableSize += sizeof(void*);
                tableReplacer->hsa_ven_amd_aqlprofile_error_string = HSA_API_Trace_hsa_ven_amd_aqlprofile_error_string;
            }
        }
    }
}
