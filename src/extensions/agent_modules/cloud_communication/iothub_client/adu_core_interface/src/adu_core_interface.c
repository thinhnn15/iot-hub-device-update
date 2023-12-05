/**
 * @file adu_core_interface.c
 * @brief Methods to communicate with "urn:azureiot:AzureDeviceUpdateCore:1" interface.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */

#include "aduc/adu_core_interface.h"
#include "aduc/adu_core_export_helpers.h" // ADUC_Workflow_SetUpdateStateWithResult
#include "aduc/agent_orchestration.h"
#include "aduc/agent_workflow.h"
#include "aduc/c_utils.h"
#include "aduc/client_handle_helper.h"
#include "aduc/config_utils.h"
#include "aduc/d2c_messaging.h"
#include "aduc/hash_utils.h"
#include "aduc/logging.h"
#include "aduc/string_c_utils.h"
#include "aduc/types/update_content.h"
#include "aduc/workflow_data_utils.h"
#include "aduc/workflow_utils.h"

#include "startup_msg_helper.h"

#include <azure_c_shared_utility/strings.h> // STRING_*
#include <iothub_client_version.h>
#include <parson.h>
#include <pnp_protocol.h>

// Name of an Device Update Agent component that this device implements.
static const char g_aduPnPComponentName[] = "deviceUpdate";

// Name of properties that Device Update Agent component supports.

// This is the device-to-cloud property.
// An agent communicates its state and other data to ADU Management service by reporting this property to IoTHub.
static const char g_aduPnPComponentAgentPropertyName[] = "agent";

// This is the cloud-to-device property.
// ADU Management send an 'Update Action' to this device by setting this property on IoTHub.
static const char g_aduPnPComponentServicePropertyName[] = "service";

/**
 * @brief Handle for Device Update Agent component to communication to service.
 */
ADUC_ClientHandle g_iotHubClientHandleForADUComponent;

/**
 * @brief This function is called when the message is no longer being process.
 *
 * @param context The ADUC_D2C_Message object
 * @param status The message status.
 */
static void OnUpdateResultD2CMessageCompleted(void* context, ADUC_D2C_Message_Status status)
{
    UNREFERENCED_PARAMETER(context);
    Log_Debug("Send message completed (status:%d)", status);
}

/**
 * @brief Initialize a ADUC_WorkflowData object.
 *
 * @param[out] workflowData Workflow metadata.
 * @param argc Count of arguments in @p argv
 * @param argv Command line parameters.
 * @return bool True on success.
 */
bool ADUC_WorkflowData_Init(ADUC_WorkflowData* workflowData, int argc, char** argv)
{
    bool succeeded = false;

    memset(workflowData, 0, sizeof(*workflowData));

    ADUC_Result result = ADUC_MethodCall_Register(&(workflowData->UpdateActionCallbacks), argc, (const char**)argv);
    if (IsAducResultCodeFailure(result.ResultCode))
    {
        Log_Error("ADUC_RegisterPlatformLayer failed %d, %d", result.ResultCode, result.ExtendedResultCode);
        goto done;
    }

    // Only call Unregister if register succeeded.
    workflowData->IsRegistered = true;

    workflowData->DownloadProgressCallback = ADUC_Workflow_DefaultDownloadProgressCallback;

    workflowData->ReportStateAndResultAsyncCallback = AzureDeviceUpdateCoreInterface_ReportStateAndResultAsync;

    workflowData->LastCompletedWorkflowId = NULL;

    workflow_set_cancellation_type(workflowData->WorkflowHandle, ADUC_WorkflowCancellationType_None);

    succeeded = true;

done:
    return succeeded;
}

/**
 * @brief Free members of ADUC_WorkflowData object.
 *
 * @param workflowData Object whose members should be freed.
 */
void ADUC_WorkflowData_Uninit(ADUC_WorkflowData* workflowData)
{
    if (workflowData == NULL)
    {
        return;
    }

    if (workflowData->IsRegistered)
    {
        ADUC_MethodCall_Unregister(&(workflowData->UpdateActionCallbacks));
    }

    workflow_free_string(workflowData->LastCompletedWorkflowId);
    memset(workflowData, 0, sizeof(*workflowData));
}

/**
 * @brief Reports the client json via PnP so it ends up in the reported section of the twin.
 *
 * @param messageType The message type.
 * @param json_value The json value to be reported.
 * @param workflowData The workflow data.
 * @return bool true if call succeeded.
 */
static bool
ReportClientJsonProperty(ADUC_D2C_Message_Type messageType, const char* json_value, ADUC_WorkflowData* workflowData)
{
    UNREFERENCED_PARAMETER(workflowData);

    bool success = false;

    if (g_iotHubClientHandleForADUComponent == NULL)
    {
        Log_Error("ReportClientJsonProperty called with invalid IoTHub Device Client handle! Can't report!");
        return false;
    }

    STRING_HANDLE jsonToSend = NULL;

    if (workflowData->CommunicationChannel == ADUC_CommunicationChannelType_IoTHubPnP)
    {
        jsonToSend = PnP_CreateReportedProperty(g_aduPnPComponentName, g_aduPnPComponentAgentPropertyName, json_value);
    }
    else
    {
        jsonToSend = STRING_construct_sprintf("{\"%s\":%s}", g_aduPnPComponentAgentPropertyName, json_value);
    }

    if (jsonToSend == NULL)
    {
        Log_Error("Unable to create Reported property for ADU client.");
        goto done;
    }

    if (!ADUC_D2C_Message_SendAsync(
            messageType,
            &g_iotHubClientHandleForADUComponent,
            STRING_c_str(jsonToSend),
            NULL /* responseCallback */,
            OnUpdateResultD2CMessageCompleted,
            NULL /* statusChangedCallback */,
            NULL /* userData */))
    {
        Log_Error("Unable to send update result.");
        goto done;
    }

    success = true;

done:
    STRING_delete(jsonToSend);

    return success;
}

/**
 * @brief Reports values to the cloud which do not change throughout ADUs execution
 * @details the current expectation is to report these values after the successful
 * connection of the AzureDeviceUpdateCoreInterface
 * @param workflowData the workflow data.
 * @returns true when the report is sent and false when reporting fails.
 */
bool ReportStartupMsg(ADUC_WorkflowData* workflowData)
{
    if (g_iotHubClientHandleForADUComponent == NULL)
    {
        Log_Error("ReportStartupMsg called before registration! Can't report!");
        return false;
    }

    bool success = false;
    const ADUC_ConfigInfo* config = NULL;
    char* jsonString = NULL;

    JSON_Value* startupMsgValue = json_value_init_object();

    if (startupMsgValue == NULL)
    {
        goto done;
    }

    JSON_Object* startupMsgObj = json_value_get_object(startupMsgValue);

    if (startupMsgObj == NULL)
    {
        goto done;
    }

    config = ADUC_ConfigInfo_GetInstance();

    if (config == NULL)
    {
        goto done;
    }

    const ADUC_AgentInfo* agent = ADUC_ConfigInfo_GetAgent(config, 0);

    if (!StartupMsg_AddDeviceProperties(startupMsgObj, agent))
    {
        Log_Error("Could not add Device Properties to the startup message");
        goto done;
    }

    if (!StartupMsg_AddCompatPropertyNames(startupMsgObj))
    {
        Log_Error("Could not add compatPropertyNames to the startup message");
        goto done;
    }

    jsonString = json_serialize_to_string(startupMsgValue);

    if (jsonString == NULL)
    {
        Log_Error("Serializing JSON to string failed!");
        goto done;
    }

    ReportClientJsonProperty(ADUC_D2C_Message_Type_Device_Properties, jsonString, workflowData);

    success = true;
done:
    json_value_free(startupMsgValue);
    json_free_serialized_string(jsonString);
    ADUC_ConfigInfo_ReleaseInstance(config);
    return success;
}

//
// AzureDeviceUpdateCoreInterface methods
//

bool AzureDeviceUpdateCoreInterface_Create(void** context, int argc, char** argv)
{
    bool succeeded = false;
    ADUC_WorkflowData* workflowData = NULL;

    workflowData = calloc(1, sizeof(*workflowData));
    if (workflowData == NULL)
    {
        goto done;
    }

    Log_Info("ADUC agent started. Using IoT Hub Client SDK %s", IoTHubClient_GetVersionString());

    if (!ADUC_WorkflowData_Init(workflowData, argc, argv))
    {
        Log_Error("Workflow data initialization failed");
        goto done;
    }

    succeeded = true;

done:

    if (!succeeded)
    {
        ADUC_WorkflowData_Uninit(workflowData);
        free(workflowData);
        workflowData = NULL;
    }

    // Set out parameter.
    *context = workflowData;

    return succeeded;
}

void AzureDeviceUpdateCoreInterface_Connected(void* componentContext)
{
    ADUC_WorkflowData* workflowData = (ADUC_WorkflowData*)componentContext;

    if (workflowData->WorkflowHandle == NULL)
    {
        // Only perform startup logic here, if no workflows has been created.
        ADUC_Workflow_HandleStartupWorkflowData(workflowData);
    }

    if (!ReportStartupMsg(workflowData))
    {
        Log_Warn("ReportStartupMsg failed");
    }
}

void AzureDeviceUpdateCoreInterface_DoWork(void* componentContext)
{
    ADUC_WorkflowData* workflowData = (ADUC_WorkflowData*)componentContext;

    // TODO (nox-msft) - process any queued deployment data here.

    ADUC_Workflow_DoWork(workflowData);
}

void AzureDeviceUpdateCoreInterface_Destroy(void** componentContext)
{
    ADUC_WorkflowData* workflowData = (ADUC_WorkflowData*)(*componentContext);

    Log_Info("ADUC agent stopping");

    ADUC_WorkflowData_Uninit(workflowData);
    free(workflowData);

    *componentContext = NULL;
}

/**
 * @brief Callback for the orchestrator that allows the new patches coming down from the cloud to be organized
 * @param clientHandle the client handle being used for the connection
 * @param propertyValue the value of the property being routed
 * @param propertyVersion the version of the property being routed
 * @param sourceContext the context of the origination point for the callback
 * @param context context for re-entering upon completion of the function
 */
void OrchestratorUpdateCallback(
    ADUC_ClientHandle clientHandle,
    JSON_Value* propertyValue,
    int propertyVersion,
    ADUC_PnPComponentClient_PropertyUpdate_Context* sourceContext,
    void* context)
{
    UNREFERENCED_PARAMETER(clientHandle);

    ADUC_WorkflowData* workflowData = (ADUC_WorkflowData*)context;
    STRING_HANDLE jsonToSend = NULL;

    // Reads out the json string so we can Log Out what we've got.
    // The value will be parsed and handled in ADUC_Workflow_HandlePropertyUpdate.
    char* jsonString = json_serialize_to_string(propertyValue);
    if (jsonString == NULL)
    {
        Log_Error(
            "OrchestratorUpdateCallback failed to convert property JSON value to string, property version (%d)",
            propertyVersion);
        goto done;
    }

    // To reduce TWIN size, remove UpdateManifestSignature and fileUrls before ACK.
    char* ackString = NULL;
    JSON_Object* signatureObj = json_value_get_object(propertyValue);
    if (signatureObj != NULL)
    {
        json_object_set_null(signatureObj, "updateManifestSignature");
        json_object_set_null(signatureObj, "fileUrls");
        ackString = json_serialize_to_string(propertyValue);
    }

    Log_Debug("Update Action info string (%s), property version (%d)", ackString, propertyVersion);

    ADUC_Workflow_HandlePropertyUpdate(workflowData, (const unsigned char*)jsonString, sourceContext->forceUpdate);
    free(jsonString);
    jsonString = ackString;

    // ACK the request.
    jsonToSend = PnP_CreateReportedPropertyWithStatus(
        g_aduPnPComponentName,
        g_aduPnPComponentServicePropertyName,
        jsonString,
        PNP_STATUS_SUCCESS,
        "", // Description for this acknowledgement.
        propertyVersion);

    if (jsonToSend == NULL)
    {
        Log_Error("Unable to build reported property ACK response.");
        goto done;
    }

    if (!ADUC_D2C_Message_SendAsync(
            ADUC_D2C_Message_Type_Device_Update_ACK,
            &g_iotHubClientHandleForADUComponent,
            STRING_c_str(jsonToSend),
            NULL /* responseCallback */,
            OnUpdateResultD2CMessageCompleted,
            NULL /* statusChangedCallback */,
            NULL /* userData */))
    {
        Log_Error("Unable to send update result.");
        goto done;
    }

done:
    STRING_delete(jsonToSend);

    free(jsonString);

    Log_Info("OrchestratorPropertyUpdateCallback ended");
}

/**
 * @brief This function is invoked when Device Update PnP Interface property is updated.
 *
 * @param clientHandle A Device Update Client handle object.
 * @param propertyName The name of the property that changed.
 * @param propertyValue The new property value.
 * @param version Property version.
 * @param sourceContext An information about the source of the property update notificaion.
 * @param context An ADUC_WorkflowData object.
 */
void AzureDeviceUpdateCoreInterface_PropertyUpdateCallback(
    ADUC_ClientHandle clientHandle,
    const char* propertyName,
    JSON_Value* propertyValue,
    int version,
    ADUC_PnPComponentClient_PropertyUpdate_Context* sourceContext,
    void* context)
{
    if (strcmp(propertyName, g_aduPnPComponentServicePropertyName) == 0)
    {
        OrchestratorUpdateCallback(clientHandle, propertyValue, version, sourceContext, context);
    }
    else
    {
        Log_Info("Unsupported property. (%s)", propertyName);
    }
}

/**
 * @brief Report state, and optionally result to service.
 *
 * @param workflowDataToken A workflow data object.
 * @param updateState state to report.
 * @param result Result to report (optional, can be NULL).
 * @param installedUpdateId Installed update id (if update completed successfully).
 * @return true if succeeded.
 */
bool AzureDeviceUpdateCoreInterface_ReportStateAndResultAsync(
    ADUC_WorkflowDataToken workflowDataToken,
    ADUCITF_State updateState,
    const ADUC_Result* result,
    const char* installedUpdateId)
{
    bool success = false;
    ADUC_WorkflowData* workflowData = (ADUC_WorkflowData*)workflowDataToken;

    JSON_Value* rootValue = NULL;
    char* jsonString = NULL;

    if (g_iotHubClientHandleForADUComponent == NULL)
    {
        Log_Error("ReportStateAsync called before registration! Can't report!");
        return false;
    }

    if (AgentOrchestration_ShouldNotReportToCloud(updateState))
    {
        Log_Debug("Skipping report of state '%s'", ADUCITF_StateToString(updateState));
        return true;
    }

    if (result == NULL && updateState == ADUCITF_State_DeploymentInProgress)
    {
        ADUC_Result resultForSet = { ADUC_Result_DeploymentInProgress_Success };
        workflow_set_result(workflowData->WorkflowHandle, resultForSet);
    }

    rootValue = GetReportingJsonValue(workflowData, updateState, result, installedUpdateId);
    if (rootValue == NULL)
    {
        Log_Error("Failed to get reporting json value");
        goto done;
    }

    jsonString = json_serialize_to_string(rootValue);
    if (jsonString == NULL)
    {
        Log_Error("Serializing JSON to string failed");
        goto done;
    }

    if (!ReportClientJsonProperty(ADUC_D2C_Message_Type_Device_Update_Result, jsonString, workflowData))
    {
        goto done;
    }

    success = true;

done:
    json_value_free(rootValue);
    json_free_serialized_string(jsonString);
    // Don't free the persistenceData as that will be done by the startup logic that owns it.

    return success;
}