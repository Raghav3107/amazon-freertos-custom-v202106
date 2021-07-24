
/**
 * @file shadow_task.c
 * @brief 
 * Demo for showing how to use the Device Shadow library with the MQTT Agent. The Device
 * Shadow library provides macros and helper functions for assembling MQTT topics
 * strings, and for determining whether an incoming MQTT message is related to the
 * device shadow.
 *
 * This demo contains two tasks. The first demonstrates typical use of the Device Shadow library
 * by keeping the shadow up to date and reacting to changes made to the shadow.
 * If enabled, the second task uses the Device Shadow library to request change to the device
 * shadow. This serves to create events for the first task to react to for demonstration purposes.
 * Demo task for requesting updates to a device shadow using the Device Shadow library's API.
 * This task flips the powerOn state in the device shadow on a fixed interval.
 * STEPS:
 * 1. Assemble strings for the MQTT topics of device shadow, by using macros defined by the Device Shadow library.
 * 2. Subscribe to those MQTT topics using the MQTT Agent.
 * 3. Register callbacks for incoming shadow topic publishes with the subsciption_manager.
 * 4. Wait until it is time to publish a requested change.
 * 5. Publish a desired state of powerOn. That will cause a delta message to be sent to device.
 * 6. Wait until either prvIncomingPublishUpdateAcceptedCallback or prvIncomingPublishUpdateRejectedCallback handle
 *    the response.
 * 7. Repeat from step 4.
 */


/* Standard includes. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Demo Specific configs. */
#include "shadow_demo_config.h"

#include "aws_demo.h"

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
/* SHADOW API header. */
#include "shadow.h"

/* JSON library includes. */
#include "core_json.h"

/* shadow demo helpers header. */
#include "mqtt_demo_helpers.h"

/* Transport interface implementation include header for TLS. */
#include "transport_secure_sockets.h"


/* Demo Specific configs. */
#include "mqtt_agent_demo_config.h"

/* MQTT library includes. */
#include "core_mqtt.h"

/* MQTT agent include. */
#include "core_mqtt_agent.h"

/* Subscription manager header include. */
#include "subscription_manager.h"

/**
 * democonfigCLIENT_IDENTIFIER is required. Throw compilation error if it is not defined.
 */
#ifndef democonfigCLIENT_IDENTIFIER
#error "Please define democonfigCLIENT_IDENTIFIER in demo_config.h to the thing name registered with AWS IoT Core."
#endif

/**
 * @brief Format string representing a Shadow document with a "desired" state.
 *
 * The real json document will look like this:
 * {
 *   "state": {
 *     "desired": {
 *       "powerOn": 1
 *     }
 *   },
 *   "clientToken": "021909"
 * }
 *
 * Note the client token, which is optional for all Shadow updates. The client
 * token must be unique at any given time, but may be reused once the update is
 * completed. For this demo, a timestamp is used for a client token.
 */
#define SHADOW_DESIRED_JSON     \
    "{"                         \
    "\"state\":{"               \
    "\"desired\":{"             \
    "\"powerOn\":%01d"          \
    "}"                         \
    "},"                        \
    "\"clientToken\":\"%06lu\"" \
    "}"

/**
 * @brief The expected size of #SHADOW_DESIRED_JSON.
 *
 * Because all the format specifiers in #SHADOW_DESIRED_JSON include a length,
 * its full actual size is known by pre-calculation, here's the formula why
 * the length need to minus 3:
 * 1. The length of "%01d" is 4.
 * 2. The length of %06lu is 5.
 * 3. The actual length we will use in case 1. is 1 ( for the state of powerOn ).
 * 4. The actual length we will use in case 2. is 6 ( for the clientToken length ).
 * 5. Thus the additional size 3 = 4 + 5 - 1 - 6 + 1 (termination character).
 *
 * In your own application, you could calculate the size of the json doc in this way.
 */
#define SHADOW_DESIRED_JSON_LENGTH (sizeof(SHADOW_DESIRED_JSON) + 3)

/**
 * @brief Format string representing a Shadow document with a "reported" state.
 *
 * The real json document will look like this:
 * {
 *   "state": {
 *     "reported": {
 *       "powerOn": 1
 *     }
 *   },
 *   "clientToken": "021909"
 * }
 *
 * Note the client token, which is required for all Shadow updates. The client
 * token must be unique at any given time, but may be reused once the update is
 * completed. For this demo, a timestamp is used for a client token.
 */
#define SHADOW_REPORTED_JSON    \
    "{"                         \
    "\"state\":{"               \
    "\"reported\":{"            \
    "\"powerOn\":%01d"          \
    "}"                         \
    "},"                        \
    "\"clientToken\":\"%06lu\"" \
    "}"

/**
 * @brief The expected size of #SHADOW_REPORTED_JSON.
 *
 * Since all of the format specifiers in #SHADOW_REPORTED_JSON include a length,
 * its actual size can be precalculated at compile time from the difference between
 * the lengths of the format strings and their formatted output. We must subtract 2
 * from the length as according the following formula:
 * 1. The length of the format string "%1u" is 3.
 * 2. The length of the format string "%06lu" is 5.
 * 3. The formatted length in case 1. is 1 ( for the state of powerOn ).
 * 4. The formatted length in case 2. is 6 ( for the clientToken length ).
 * 5. Thus the additional size of our format is 2 = 3 + 5 - 1 - 6 + 1 (termination character).
 *
 * Custom applications may calculate the length of the JSON document with the same method.
 */
#define SHADOW_REPORTED_JSON_LENGTH (sizeof(SHADOW_REPORTED_JSON) + 3)

#ifndef THING_NAME

/**
 * @brief Predefined thing name.
 *
 * This is the example predefine thing name and could be compiled in ROM code.
 */
#define THING_NAME democonfigCLIENT_IDENTIFIER
#endif

/**
 * @brief The length of #THING_NAME.
 */
#define THING_NAME_LENGTH ((uint16_t)(sizeof(THING_NAME) - 1))

/**
 * @brief Time in ticks to wait between each cycle of the demo implemented
 * by RunDeviceShadowDemo().
 */
#define DELAY_BETWEEN_DEMO_ITERATIONS_TICKS (pdMS_TO_TICKS(1000U))

/**
 * @brief The maximum number of times to call MQTT_ProcessLoop() when waiting
 * for a response for Shadow delete operation.
 */
#define MQTT_PROCESS_LOOP_DELETE_RESPONSE_COUNT_MAX (30U)

/**
 * @brief This demo uses task notifications to signal tasks from MQTT callback
 * functions. shadowexampleMS_TO_WAIT_FOR_NOTIFICATION defines the time, in ticks,
 * to wait for such a callback.
 */
#define SHADOW_MS_TO_WAIT_FOR_NOTIFICATION (10000)

/**
 * @brief The maximum amount of time in milliseconds to wait for the commands
 * to be posted to the MQTT agent should the MQTT agent's command queue be full.
 * Tasks wait in the Blocked state, so don't use any CPU time.
 */
#define SHADOW_MAX_COMMAND_SEND_BLOCK_TIME_MS (200)

/**
 * @brief JSON key for response code that indicates the type of error in
 * the error document received on topic `delete/rejected`.
 */
#define SHADOW_DELETE_REJECTED_ERROR_CODE_KEY "code"

/**
 * @brief Length of #SHADOW_DELETE_REJECTED_ERROR_CODE_KEY
 */
#define SHADOW_DELETE_REJECTED_ERROR_CODE_KEY_LENGTH ((uint16_t)(sizeof(SHADOW_DELETE_REJECTED_ERROR_CODE_KEY) - 1))

/**
 * @brief Defines the structure to use as the command callback context in this
 * demo.
 */

struct MQTTAgentCommandContext
{
    MQTTStatus_t xReturnStatus;
};

// extern MQTTAgentContext_t xGlobalMqttAgentContext;

/**
 * @brief The MQTT agent manages the MQTT contexts.  This set the handle to the
 * context used by this demo.
 */
extern MQTTAgentContext_t xGlobalMqttAgentContext;

/**
 * @brief The simulated device current power on state.
 */
static uint32_t ulReportPowerOnState = 0;

/**
 * @brief The simulated device desired power on state.
 */
static uint32_t ulDesiredPowerOnState = 5;

/**
 * @brief The simulated device previous power on state.
 */
static uint32_t ulPreviousPowerOnState = 1;

/**
 * @brief Match the received clientToken with the one sent in a device shadow
 * update. Set to 0 when not waiting on a response.
 */
static uint32_t ulClientToken = 0U;

/**
 * @brief The handle of this task. It is used by callbacks to notify this task.
 */
static TaskHandle_t xShadowDeviceTaskHandle;

/**
 * @brief Status of the response of Shadow delete operation from AWS IoT
 * message broker.
 */
static BaseType_t xDeleteResponseReceived = pdFALSE;

/**
 * @brief Status of the Shadow delete operation.
 *
 * The Shadow delete status will be updated by the incoming publishes on the
 * MQTT topics for delete acknowledgement from AWS IoT message broker
 * (accepted/rejected). Shadow document is considered to be deleted if an
 * incoming publish is received on `delete/accepted` topic or an incoming
 * publish is received on `delete/rejected` topic with error code 404. Code 404
 * indicates that the Shadow document does not exist for the Thing yet.
 */
static BaseType_t xShadowDeleted = pdFALSE;

/* A buffer containing the desired document. It has static duration to prevent
* it from being placed on the call stack. 
*/
static char pcUpdateDesiredDocument[SHADOW_DESIRED_JSON_LENGTH + 1] = {0};

/* A buffer containing the update document. It has static duration to prevent
* it from being placed on the call stack. 
*/
static char pcUpdateReportDocument[SHADOW_REPORTED_JSON_LENGTH + 1] = {0};

/**
 * @brief semaphore use to wait for PUBACK acknowledge from cloud
 * 
 */
static SemaphoreHandle_t xPubAckWaitLock = NULL;

/**
 * @brief The flag to indicate the device current power on state changed.
 */
static bool stateChanged = false;
/**
 * @brief Subscribe to the used device shadow topics.
 *
 * @return true if the subscribe is successful;
 * false otherwise.
 */
static bool prvSubscribeToShadowUpdateTopics(void);

/**
 * @brief Passed into MQTTAgent_Subscribe() as the callback to execute when the
 * broker ACKs the SUBSCRIBE message. Its implementation sends a notification
 * to the task that called MQTTAgent_Subscribe() to let the task know the
 * SUBSCRIBE operation completed. It also sets the xReturnStatus of the
 * structure passed in as the command's context to the value of the
 * xReturnStatus parameter - which enables the task to check the status of the
 * operation.
 *
 * See https://freertos.org/mqtt/mqtt-agent-demo.html#example_mqtt_api_call
 *
 * @param[in] pxCommandContext Context of the initial command.
 * @param[in] pxReturnInfo The result of the command.
 */
static void prvSubscribeCommandCallback(void *pxCommandContext,
                                        MQTTAgentReturnInfo_t *pxReturnInfo);

/**
 * @brief The callback to execute when there is an incoming publish on the
 * topic for delta updates. It verifies the document and sets the
 * powerOn state accordingly.
 *
 * @param[in] pvIncomingPublishCallbackContext Context of the initial command.
 * @param[in] pxPublishInfo Deserialized publish.
 */
static void prvIncomingPublishUpdateDeltaCallback(void *pxSubscriptionContext,
                                                  MQTTPublishInfo_t *pxPublishInfo);

/**
 * @brief The callback to execute when there is an incoming publish on the
 * topic for accepted requests. It verifies the document is valid and is being waited on.
 * If so it updates the last reported state and notifies the task to inform completion
 * of the update request.
 *
 * @param[in] pvIncomingPublishCallbackContext Context of the initial command.
 * @param[in] pxPublishInfo Deserialized publish.
 */
static void prvIncomingPublishUpdateAcceptedCallback(void *pxSubscriptionContext,
                                                     MQTTPublishInfo_t *pxPublishInfo);

/**
 * @brief The callback to execute when there is an incoming publish on the
 * topic for rejected requests. It verifies the document is valid and is being waited on.
 * If so it notifies the task to inform completion of the update request.
 *
 * @param[in] pvIncomingPublishCallbackContext Context of the initial command.
 * @param[in] pxPublishInfo Deserialized publish.
 */
static void prvIncomingPublishUpdateRejectedCallback(void *pxSubscriptionContext,
                                                     MQTTPublishInfo_t *pxPublishInfo);

/**
 * @brief Entry point of shadow demo.
 *
 * This main function demonstrates how to use the macros provided by the
 * Device Shadow library to assemble strings for the MQTT topics defined
 * by AWS IoT Device Shadow. It uses these macros for topics to subscribe
 * to:
 * - SHADOW_TOPIC_STRING_UPDATE_DELTA for "$aws/things/thingName/shadow/update/delta"
 * - SHADOW_TOPIC_STRING_UPDATE_ACCEPTED for "$aws/things/thingName/shadow/update/accepted"
 * - SHADOW_TOPIC_STRING_UPDATE_REJECTED for "$aws/things/thingName/shadow/update/rejected"
 *
 * It also uses these macros for topics to publish to:
 * - SHADOW_TOPIC_STIRNG_DELETE for "$aws/things/thingName/shadow/delete"
 * - SHADOW_TOPIC_STRING_UPDATE for "$aws/things/thingName/shadow/update"
 */

/**
 * @brief Passed into MQTTAgent_Unsubscribe() as the callback to execute when the
 * broker ACKs the SUBSCRIBE message. Its implementation sends a notification
 * to the task that called MQTTAgent_Subscribe() to let the task know the
 * SUBSCRIBE operation completed. It also sets the xReturnStatus of the
 * structure passed in as the command's context to the value of the
 * xReturnStatus parameter - which enables the task to check the status of the
 * operation.
 *
 * See https://freertos.org/mqtt/mqtt-agent-demo.html#example_mqtt_api_call
 *
 * @param[in] pxCommandContext Context of the initial command.
 * @param[in] pxReturnInfo The result of the command.
 */
static void prvUnsubscribeCommandCallback(void *pxCommandContext,
                                          MQTTAgentReturnInfo_t *pxReturnInfo);

/**
 * @brief The callback to execute when there is an incoming publish on the
 * topic for accepted requests. It verifies the document is valid and is being waited on.
 * 
 *
 * @param[in] pvIncomingPublishCallbackContext Context of the initial command.
 * @param[in] pxPublishInfo Deserialized publish.
 */
static void prvIncomingPublishDeleteAcceptedCallback(void *pxSubscriptionContext,
                                                     MQTTPublishInfo_t *pxPublishInfo);

/**
 * @brief The callback to execute when there is an incoming publish on the
 * topic for accepted requests. It verifies the document is valid and is being waited on.
 *
 * @param[in] pvIncomingPublishCallbackContext Context of the initial command.
 * @param[in] pxPublishInfo Deserialized publish.
 */
static void prvIncomingPublishDeleteRejectedCallback(void *pxSubscriptionContext,
                                                     MQTTPublishInfo_t *pxPublishInfo);

/*-----------------------------------------------------------*/

static bool prvSubscribeToShadowUpdateTopics(void)
{
    printf("prvSubscribeToShadowUpdateTopics...........\n");
    bool xReturnStatus = false;
    MQTTStatus_t xStatus;
    uint32_t ulNotificationValue;
    MQTTAgentCommandInfo_t xCommandParams = {0};

    /* These must persist until the command is processed. */
    MQTTAgentSubscribeArgs_t xSubscribeArgs = {0};
    MQTTSubscribeInfo_t xSubscribeInfo[5];
    MQTTAgentCommandContext_t xApplicationDefinedContext = {0};

    /* Subscribe to shadow topic for responses for incoming delta updates. */
    xSubscribeInfo[0].pTopicFilter = SHADOW_TOPIC_STRING_UPDATE_DELTA(THING_NAME);
    xSubscribeInfo[0].topicFilterLength = SHADOW_TOPIC_LENGTH_UPDATE_DELTA(THING_NAME_LENGTH);
    xSubscribeInfo[0].qos = MQTTQoS1;
    /* Subscribe to shadow topic for accepted responses for submitted updates. */
    xSubscribeInfo[1].pTopicFilter = SHADOW_TOPIC_STRING_UPDATE_ACCEPTED(THING_NAME);
    xSubscribeInfo[1].topicFilterLength = SHADOW_TOPIC_LENGTH_UPDATE_ACCEPTED(THING_NAME_LENGTH);
    xSubscribeInfo[1].qos = MQTTQoS1;
    /* Subscribe to shadow topic for rejected responses for submitted updates. */
    xSubscribeInfo[2].pTopicFilter = SHADOW_TOPIC_STRING_UPDATE_REJECTED(THING_NAME);
    xSubscribeInfo[2].topicFilterLength = SHADOW_TOPIC_LENGTH_UPDATE_REJECTED(THING_NAME_LENGTH);
    xSubscribeInfo[2].qos = MQTTQoS1;

    xSubscribeInfo[3].pTopicFilter = SHADOW_TOPIC_STRING_DELETE_ACCEPTED(THING_NAME);
    xSubscribeInfo[3].topicFilterLength = SHADOW_TOPIC_LENGTH_DELETE_ACCEPTED(THING_NAME_LENGTH);
    xSubscribeInfo[3].qos = MQTTQoS1;

    xSubscribeInfo[4].pTopicFilter = SHADOW_TOPIC_STRING_DELETE_REJECTED(THING_NAME);
    xSubscribeInfo[4].topicFilterLength = SHADOW_TOPIC_LENGTH_DELETE_REJECTED(THING_NAME_LENGTH);
    xSubscribeInfo[4].qos = MQTTQoS1;

    /* Complete the subscribe information. The topic string must persist for
     * duration of subscription - although in this case it is a static const so
     * will persist for the lifetime of the application. */
    xSubscribeArgs.pSubscribeInfo = xSubscribeInfo;
    xSubscribeArgs.numSubscriptions = 5;

    /* Loop in case the queue used to communicate with the MQTT agent is full and
     * attempts to post to it time out.  The queue will not become full if the
     * priority of the MQTT agent task is higher than the priority of the task
     * calling this function. */
    xTaskNotifyStateClear(NULL);
    xCommandParams.blockTimeMs = SHADOW_MAX_COMMAND_SEND_BLOCK_TIME_MS;
    xCommandParams.cmdCompleteCallback = prvSubscribeCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = &xApplicationDefinedContext;
    LogInfo(("Sending subscribe request to agent for shadow topics."));

    do
    {
        /* If this fails, the agent's queue is full, so we retry until the agent
         * has more space in the queue. */
        xStatus = MQTTAgent_Subscribe(&xGlobalMqttAgentContext,
                                      &(xSubscribeArgs),
                                      &xCommandParams);
    } while (xStatus != MQTTSuccess);

    /* Wait for acks from subscribe messages - this is optional.  If the
     * returned value is zero then the wait timed out. */
    ulNotificationValue = ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(SHADOW_MS_TO_WAIT_FOR_NOTIFICATION));
    //configASSERT(ulNotificationValue != 0UL);

    LogInfo(("MQTTAgent_Subscribe...........MQTTSuccess "));

    /* The callback sets the xReturnStatus member of the context. */
    if (xApplicationDefinedContext.xReturnStatus != MQTTSuccess)
    {
        LogError(("Failed to subscribe to shadow update topics."));
    }
    else
    {
        LogInfo(("Successfully subscribed to shadow update topics."));
        xReturnStatus = true;
    }

    return xReturnStatus;
}

/*-----------------------------------------------------------*/

static void prvSubscribeCommandCallback(void *pxCommandContext,
                                        MQTTAgentReturnInfo_t *pxReturnInfo)
{
    printf("prvSubscribeCommandCallback.......... \n");
    bool xSuccess = false;
    MQTTAgentCommandContext_t *pxApplicationDefinedContext = (MQTTAgentCommandContext_t *)pxCommandContext;

    /* Check if the subscribe operation is a success. */
    if (pxReturnInfo->returnCode == MQTTSuccess)
    {
        LogInfo(("prvSubscribeCommandCallback...pxReturnInfo->returnCode....... MQTTSuccess"));
        /* Add subscriptions so that incoming publishes are routed to the application
         * callback. */
        xSuccess = addSubscription((SubscriptionElement_t *)xGlobalMqttAgentContext.pIncomingCallbackContext,
                                   SHADOW_TOPIC_STRING_UPDATE_DELTA(THING_NAME),
                                   SHADOW_TOPIC_LENGTH_UPDATE_DELTA(THING_NAME_LENGTH),
                                   prvIncomingPublishUpdateDeltaCallback,
                                   NULL);

        if (xSuccess == false)
        {
            LogError(("Failed to register an incoming publish callback for topic %.*s.",
                      SHADOW_TOPIC_LENGTH_UPDATE_DELTA(THING_NAME_LENGTH),
                      SHADOW_TOPIC_STRING_UPDATE_DELTA(THING_NAME)));
        }
        else
        {
            LogInfo(("Successfully register an incoming publish callback for topic %.*s.",
                     SHADOW_TOPIC_LENGTH_UPDATE_DELTA(THING_NAME_LENGTH),
                     SHADOW_TOPIC_STRING_UPDATE_DELTA(THING_NAME)));
        }
    }

    if (xSuccess == true)
    {

        xSuccess = addSubscription((SubscriptionElement_t *)xGlobalMqttAgentContext.pIncomingCallbackContext,
                                   SHADOW_TOPIC_STRING_UPDATE_ACCEPTED(THING_NAME),
                                   SHADOW_TOPIC_LENGTH_UPDATE_ACCEPTED(THING_NAME_LENGTH),
                                   prvIncomingPublishUpdateAcceptedCallback,
                                   NULL);

        if (xSuccess == false)
        {
            LogError(("Failed to register an incoming publish callback for topic %.*s.",
                      SHADOW_TOPIC_LENGTH_UPDATE_ACCEPTED(THING_NAME_LENGTH),
                      SHADOW_TOPIC_STRING_UPDATE_ACCEPTED(THING_NAME)));
        }
        else
        {
            LogInfo(("Successfully register an incoming publish callback for topic %.*s.",
                     SHADOW_TOPIC_LENGTH_UPDATE_ACCEPTED(THING_NAME_LENGTH),
                     SHADOW_TOPIC_STRING_UPDATE_ACCEPTED(THING_NAME)));
        }
    }

    if (xSuccess == true)
    {
        xSuccess = addSubscription((SubscriptionElement_t *)xGlobalMqttAgentContext.pIncomingCallbackContext,
                                   SHADOW_TOPIC_STRING_UPDATE_REJECTED(THING_NAME),
                                   SHADOW_TOPIC_LENGTH_UPDATE_REJECTED(THING_NAME_LENGTH),
                                   prvIncomingPublishUpdateRejectedCallback,
                                   NULL);

        if (xSuccess == false)
        {
            LogError(("Failed to register an incoming publish callback for topic %.*s.",
                      SHADOW_TOPIC_LENGTH_UPDATE_REJECTED(THING_NAME_LENGTH),
                      SHADOW_TOPIC_STRING_UPDATE_REJECTED(THING_NAME)));
        }
        else
        {
            LogInfo(("Successfully register an incoming publish callback for topic %.*s.",
                     SHADOW_TOPIC_LENGTH_UPDATE_REJECTED(THING_NAME_LENGTH),
                     SHADOW_TOPIC_STRING_UPDATE_REJECTED(THING_NAME)));
        }

        if (xSuccess == true)
        {
            xSuccess = addSubscription((SubscriptionElement_t *)xGlobalMqttAgentContext.pIncomingCallbackContext,
                                       SHADOW_TOPIC_STRING_DELETE_ACCEPTED(THING_NAME),
                                       SHADOW_TOPIC_LENGTH_DELETE_ACCEPTED(THING_NAME_LENGTH),
                                       prvIncomingPublishDeleteAcceptedCallback,
                                       NULL);

            if (xSuccess == false)
            {
                LogError(("Failed to register an incoming publish callback for topic %.*s.",
                          SHADOW_TOPIC_LENGTH_DELETE_ACCEPTED(THING_NAME_LENGTH),
                          SHADOW_TOPIC_STRING_DELETE_ACCEPTED(THING_NAME)));
            }
            else
            {
                LogInfo(("Successfully register an incoming publish callback for topic %.*s.",
                         SHADOW_TOPIC_LENGTH_DELETE_ACCEPTED(THING_NAME_LENGTH),
                         SHADOW_TOPIC_STRING_DELETE_ACCEPTED(THING_NAME)));
            }
        }

        if (xSuccess == true)
        {
            xSuccess = addSubscription((SubscriptionElement_t *)xGlobalMqttAgentContext.pIncomingCallbackContext,
                                       SHADOW_TOPIC_STRING_DELETE_REJECTED(THING_NAME),
                                       SHADOW_TOPIC_LENGTH_DELETE_REJECTED(THING_NAME_LENGTH),
                                       prvIncomingPublishDeleteRejectedCallback,
                                       NULL);

            if (xSuccess == false)
            {
                LogError(("Failed to register an incoming publish callback for topic %.*s.",
                          SHADOW_TOPIC_LENGTH_DELETE_REJECTED(THING_NAME_LENGTH),
                          SHADOW_TOPIC_STRING_DELETE_REJECTED(THING_NAME)));
            }
            else
            {
                LogInfo(("Successfully register an incoming publish callback for topic %.*s.",
                         SHADOW_TOPIC_LENGTH_DELETE_REJECTED(THING_NAME_LENGTH),
                         SHADOW_TOPIC_STRING_DELETE_REJECTED(THING_NAME)));
            }
        }
    }

    /* Store the result in the application defined context so the calling task
     * can check it. */
    pxApplicationDefinedContext->xReturnStatus = MQTTSuccess;

    xTaskNotifyGive(xShadowDeviceTaskHandle);
}

static bool prvUnsubscribeToShadowUpdateTopics(void)
{
    printf("prvUnsubscribeToShadowUpdateTopics...........\n");
    bool xReturnStatus = false;
    MQTTStatus_t xStatus;
    uint32_t ulNotificationValue;
    MQTTAgentCommandInfo_t xCommandParams = {0};

    /* These must persist until the command is processed. */
    MQTTAgentSubscribeArgs_t xUnubscribeArgs = {0};
    MQTTSubscribeInfo_t xUnsubscribeInfo[2];
    MQTTAgentCommandContext_t xApplicationDefinedContext = {0};

    xUnsubscribeInfo[0].pTopicFilter = SHADOW_TOPIC_STRING_DELETE_ACCEPTED(THING_NAME);
    xUnsubscribeInfo[0].topicFilterLength = SHADOW_TOPIC_LENGTH_DELETE_ACCEPTED(THING_NAME_LENGTH);
    xUnsubscribeInfo[0].qos = MQTTQoS1;

    xUnsubscribeInfo[1].pTopicFilter = SHADOW_TOPIC_STRING_DELETE_REJECTED(THING_NAME);
    xUnsubscribeInfo[1].topicFilterLength = SHADOW_TOPIC_LENGTH_DELETE_REJECTED(THING_NAME_LENGTH);
    xUnsubscribeInfo[1].qos = MQTTQoS1;

    /* Complete the subscribe information. The topic string must persist for
     * duration of subscription - although in this case it is a static const so
     * will persist for the lifetime of the application. */
    xUnubscribeArgs.pSubscribeInfo = xUnsubscribeInfo;
    xUnubscribeArgs.numSubscriptions = 2;
    // xApplicationDefinedContext.xReturnStatus;
    /* Loop in case the queue used to communicate with the MQTT agent is full and
     * attempts to post to it time out.  The queue will not become full if the
     * priority of the MQTT agent task is higher than the priority of the task
     * calling this function. */
    xTaskNotifyStateClear(NULL);
    xCommandParams.blockTimeMs = SHADOW_MAX_COMMAND_SEND_BLOCK_TIME_MS;
    xCommandParams.cmdCompleteCallback = prvUnsubscribeCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = &xApplicationDefinedContext;
    LogInfo(("Sending Unsubscribe request to agent for shadow topics."));

    do
    {
        /* If this fails, the agent's queue is full, so we retry until the agent
         * has more space in the queue. */
        xStatus = MQTTAgent_Unsubscribe(&xGlobalMqttAgentContext,
                                        &(xUnubscribeArgs),
                                        &xCommandParams);
    } while (xStatus != MQTTSuccess);

    /* Wait for acks from subscribe messages - this is optional.  If the
     * returned value is zero then the wait timed out. */
    ulNotificationValue = ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(SHADOW_MS_TO_WAIT_FOR_NOTIFICATION));
    //configASSERT(ulNotificationValue != 0UL);

    LogInfo(("MQTTAgent_Unsubscribe...........MQTTSuccess "));

    /* The callback sets the xReturnStatus member of the context. */
    if (xApplicationDefinedContext.xReturnStatus != MQTTSuccess)
    {
        LogError(("Failed to Unsubscribe to shadow update topics."));
        printf("Failed to Unsubscribe to shadow update topics \n");
    }
    else
    {
        LogInfo(("Successfully subscribed to shadow update topics."));
        printf("Successfully subscribed to shadow update topics.\n");
    }
    xReturnStatus = true;
    return xReturnStatus;
}

/*-------------------------------------------------------*/

static void prvUnsubscribeCommandCallback(void *pxCommandContext,
                                          MQTTAgentReturnInfo_t *pxReturnInfo)
{
    printf("prvUnsubscribeCommandCallback.......... \n");
    bool xSuccess = false;
    MQTTAgentCommandContext_t *pxApplicationDefinedContext = (MQTTAgentCommandContext_t *)pxCommandContext;

    /* Check if the subscribe operation is a success. */
    if (pxReturnInfo->returnCode == MQTTSuccess)
    {
        LogInfo(("prvUnsubscribeCommandCallback...pxReturnInfo->returnCode....... MQTTSuccess"));

        removeSubscription((SubscriptionElement_t *)xGlobalMqttAgentContext.pIncomingCallbackContext,
                           SHADOW_TOPIC_STRING_DELETE_ACCEPTED(THING_NAME),
                           SHADOW_TOPIC_LENGTH_DELETE_ACCEPTED(THING_NAME_LENGTH));

        removeSubscription((SubscriptionElement_t *)xGlobalMqttAgentContext.pIncomingCallbackContext,
                           SHADOW_TOPIC_STRING_DELETE_REJECTED(THING_NAME),
                           SHADOW_TOPIC_LENGTH_DELETE_REJECTED(THING_NAME_LENGTH));
    }
    /* Store the result in the application defined context so the calling task
     * can check it. */
    pxApplicationDefinedContext->xReturnStatus = MQTTSuccess;

    xTaskNotifyGive(xShadowDeviceTaskHandle);
}

/*-----------------------------------------------------------*/

static void prvIncomingPublishUpdateDeltaCallback(void *pxSubscriptionContext,
                                                  MQTTPublishInfo_t *pxPublishInfo)
{
    printf("prvIncomingPublishUpdateDeltaCallback................\n");
    static uint32_t ulCurrentVersion = 0; /* Remember the latest version number we've received */
    uint32_t ulVersion = 0UL;
    uint32_t ulNewState = 0UL;
    char *pcOutValue = NULL;
    uint32_t ulOutValueLength = 0UL;
    JSONStatus_t result = JSONSuccess;

    /* Remove compiler warnings about unused parameters. */
    (void)pxSubscriptionContext;

    configASSERT(pxPublishInfo != NULL);
    configASSERT(pxPublishInfo->pPayload != NULL);

    LogDebug(("/update/delta json payload:%.*s.",
              pxPublishInfo->payloadLength,
              (const char *)pxPublishInfo->pPayload));

    /* The payload will look similar to this:
     * {
     *      "state": {
     *          "powerOn": 1
     *      },
     *      "metadata": {
     *          "powerOn": {
     *              "timestamp": 1595437367
     *          }
     *      },
     *      "timestamp": 1595437367,
     *      "clientToken": "388062",
     *      "version": 12
     *  }
     */

    /* Make sure the payload is a valid json document. */
    result = JSON_Validate(pxPublishInfo->pPayload,
                           pxPublishInfo->payloadLength);

    if (result != JSONSuccess)
    {
        LogError(("Invalid JSON document recieved!"));
    }
    else
    {
        /* Obtain the version value. */
        result = JSON_Search((char *)pxPublishInfo->pPayload,
                             pxPublishInfo->payloadLength,
                             "version",
                             sizeof("version") - 1,
                             &pcOutValue,
                             (size_t *)&ulOutValueLength);

        if (result != JSONSuccess)
        {
            LogError(("Version field not found in JSON document!"));
        }
        else
        {
            /* Convert the extracted value to an unsigned integer value. */
            ulVersion = (uint32_t)strtoul(pcOutValue, NULL, 10);

            /* Make sure the version is newer than the last one we received. */
            if (ulVersion <= ulCurrentVersion)
            {
                /* In this demo, we discard the incoming message
                 * if the version number is not newer than the latest
                 * that we've received before. Your application may use a
                 * different approach.
                 */
                LogWarn(("Recieved unexpected delta update with version %u. Current version is %u",
                         (unsigned int)ulVersion,
                         (unsigned int)ulCurrentVersion));
            }
            else
            {
                LogInfo(("Recieved delta update with version %.*s.",
                         ulOutValueLength,
                         pcOutValue));

                /* Set received version as the current version. */
                ulCurrentVersion = ulVersion;

                /* Get powerOn state from json documents. */
                result = JSON_Search((char *)pxPublishInfo->pPayload,
                                     pxPublishInfo->payloadLength,
                                     "state.powerOn",
                                     sizeof("state.powerOn") - 1,
                                     &pcOutValue,
                                     (size_t *)&ulOutValueLength);

                if (result != JSONSuccess)
                {
                    LogError(("powerOn field not found in JSON document!"));
                }
                else
                {
                    /* Convert the powerOn state value to an unsigned integer value. */
                    ulNewState = (uint32_t)strtoul(pcOutValue, NULL, 10);

                    LogInfo(("Setting powerOn state to %u.",
                             (unsigned int)ulNewState));
                    /* Set the new powerOn state. */
                    ulReportPowerOnState = ulNewState;
                    ulDesiredPowerOnState = ulNewState;

                    stateChanged = true;
                }
            }
        }
    }
}

/*-------------------------------------------------------------*/

static void prvIncomingPublishDeleteAcceptedCallback(void *pxSubscriptionContext,
                                                     MQTTPublishInfo_t *pxPublishInfo)

{
    printf("prvIncomingPublishDeleteAcceptedCallback............\n");
    /* Remove compiler warnings about unused parameters. */
    (void)pxSubscriptionContext;

    xShadowDeleted = pdTRUE;
    xTaskNotifyGive(xShadowDeviceTaskHandle);
}
/*------------------------------------------------------------*/

static void prvIncomingPublishDeleteRejectedCallback(void *pxSubscriptionContext,
                                                     MQTTPublishInfo_t *pxPublishInfo)
{
    printf("prvIncomingPublishDeleteRejectedCallback............\n");
    JSONStatus_t result = JSONSuccess;
    char *pcOutValue = NULL;
    uint32_t ulOutValueLength = 0UL;
    uint32_t ulErrorCode = 0UL;

    /* Remove compiler warnings about unused parameters. */
    (void)pxSubscriptionContext;

    configASSERT(pxPublishInfo != NULL);
    configASSERT(pxPublishInfo->pPayload != NULL);
    assert(pxPublishInfo != NULL);
    assert(pxPublishInfo->pPayload != NULL);

    LogInfo(("/delete/rejected json payload:%s.", (const char *)pxPublishInfo->pPayload));

    /* The payload will look similar to this:
     * {
     *    "code": error-code,
     *    "message": "error-message",
     *    "timestamp": timestamp,
     *    "clientToken": "token"
     * }
     */

    /* Make sure the payload is a valid json document. */
    result = JSON_Validate(pxPublishInfo->pPayload,
                           pxPublishInfo->payloadLength);

    if (result == JSONSuccess)
    {
        /* Then we start to get the version value by JSON keyword "version". */
        result = JSON_Search((char *)pxPublishInfo->pPayload,
                             pxPublishInfo->payloadLength,
                             SHADOW_DELETE_REJECTED_ERROR_CODE_KEY,
                             SHADOW_DELETE_REJECTED_ERROR_CODE_KEY_LENGTH,
                             &pcOutValue,
                             (size_t *)&ulOutValueLength);
    }
    else
    {
        LogError(("The json document is invalid!!"));
    }

    if (result == JSONSuccess)
    {
        LogInfo(("Error code is: %.*s.",
                 ulOutValueLength,
                 pcOutValue));

        /* Convert the extracted value to an unsigned integer value. */
        ulErrorCode = (uint32_t)strtoul(pcOutValue, NULL, 10);
    }
    else
    {
        LogError(("No error code in json document!!"));
    }

    LogInfo(("Error code:%lu.", ulErrorCode));

    /* Mark Shadow delete operation as a success if error code is 404. */
    if (ulErrorCode == 404)
    {
        xShadowDeleted = pdTRUE;
    }
    xTaskNotifyGive(xShadowDeviceTaskHandle);
}

/*-----------------------------------------------------------*/

static void prvIncomingPublishUpdateAcceptedCallback(void *pxSubscriptionContext,
                                                     MQTTPublishInfo_t *pxPublishInfo)
{
    printf("prvIncomingPublishUpdateAcceptedCallback................\n");
    char *pcOutValue = NULL;
    uint32_t ulOutValueLength = 0UL;
    uint32_t ulReceivedToken = 0UL;
    JSONStatus_t result = JSONSuccess;

    /* Remove compiler warnings about unused parameters. */
    (void)pxSubscriptionContext;

    configASSERT(pxPublishInfo != NULL);
    configASSERT(pxPublishInfo->pPayload != NULL);

    LogDebug(("/update/accepted JSON payload: %.*s.",
              pxPublishInfo->payloadLength,
              (const char *)pxPublishInfo->pPayload));

    /* Handle the reported state with state change in /update/accepted topic.
     * Thus we will retrieve the client token from the JSON document to see if
     * it's the same one we sent with reported state on the /update topic.
     * The payload will look similar to this:
     *  {
     *      "state": {
     *          "reported": {
     *             "powerOn": 1
     *          }
     *      },
     *      "metadata": {
     *          "reported": {
     *              "powerOn": {
     *                  "timestamp": 1596573647
     *              }
     *          }
     *      },
     *      "version": 14698,
     *      "timestamp": 1596573647,
     *      "clientToken": "022485"
     *  }
     */

    /* Make sure the payload is a valid json document. */
    result = JSON_Validate(pxPublishInfo->pPayload,
                           pxPublishInfo->payloadLength);

    if (result != JSONSuccess)
    {
        LogError(("Invalid JSON document recieved!"));
    }
    else
    {
        /* Get clientToken from json documents. */
        result = JSON_Search((char *)pxPublishInfo->pPayload,
                             pxPublishInfo->payloadLength,
                             "clientToken",
                             sizeof("clientToken") - 1,
                             &pcOutValue,
                             (size_t *)&ulOutValueLength);
    }

    if (result != JSONSuccess)
    {
        LogDebug(("Ignoring publish on /update/accepted with no clientToken field."));
    }
    else
    {
        /* Convert the code to an unsigned integer value. */
        ulReceivedToken = (uint32_t)strtoul(pcOutValue, NULL, 10);

        /* If we are waiting for a response, ulClientToken will be the token for the response
         * we are waiting for, else it will be 0. ulRecievedToken may not match if the response is
         * not for us or if it is is a response that arrived after we timed out
         * waiting for it.
         */
        if (ulReceivedToken != ulClientToken)
        {
            LogDebug(("Ignoring publish on /update/accepted with clientToken %lu.", (unsigned long)ulReceivedToken));
        }
        else
        {
            LogInfo(("Received accepted response for update with token %lu. ", (unsigned long)ulClientToken));

            /*  Obtain the accepted state from the response and update our last sent state. */
            result = JSON_Search((char *)pxPublishInfo->pPayload,
                                 pxPublishInfo->payloadLength,
                                 "state.reported.powerOn",
                                 sizeof("state.reported.powerOn") - 1,
                                 &pcOutValue,
                                 (size_t *)&ulOutValueLength);

            if (result != JSONSuccess)
            {
                LogError(("powerOn field not found in JSON document!"));
            }
            else
            {
                /* Convert the powerOn state value to an unsigned integer value and
                 * save the new last reported value*/
                // ulDesiredPowerOnState = (uint32_t)strtoul(pcOutValue, NULL, 10);
            }

            /* Wake up the shadow task which is waiting for this response. */
            xTaskNotifyGive(xShadowDeviceTaskHandle);
        }
    }
}

/*-----------------------------------------------------------*/

static void prvIncomingPublishUpdateRejectedCallback(void *pxSubscriptionContext,
                                                     MQTTPublishInfo_t *pxPublishInfo)
{
    printf("prvIncomingPublishUpdateRejectedCallback................\n");
    JSONStatus_t result = JSONSuccess;
    char *pcOutValue = NULL;
    uint32_t ulOutValueLength = 0UL;
    uint32_t ulReceivedToken = 0UL;

    /* Remove compiler warnings about unused parameters. */
    (void)pxSubscriptionContext;

    configASSERT(pxPublishInfo != NULL);
    configASSERT(pxPublishInfo->pPayload != NULL);

    LogDebug(("/update/rejected json payload: %.*s.",
              pxPublishInfo->payloadLength,
              (const char *)pxPublishInfo->pPayload));

    /* The payload will look similar to this:
     * {
     *    "code": error-code,
     *    "message": "error-message",
     *    "timestamp": timestamp,
     *    "clientToken": "token"
     * }
     */

    /* Make sure the payload is a valid json document. */
    result = JSON_Validate(pxPublishInfo->pPayload,
                           pxPublishInfo->payloadLength);

    if (result != JSONSuccess)
    {
        LogError(("Invalid JSON document recieved!"));
    }
    else
    {
        /* Get clientToken from json documents. */
        result = JSON_Search((char *)pxPublishInfo->pPayload,
                             pxPublishInfo->payloadLength,
                             "clientToken",
                             sizeof("clientToken") - 1,
                             &pcOutValue,
                             (size_t *)&ulOutValueLength);
    }

    if (result != JSONSuccess)
    {
        LogDebug(("Ignoring publish on /update/rejected with clientToken %lu.", (unsigned long)ulReceivedToken));
    }
    else
    {
        /* Convert the code to an unsigned integer value. */
        ulReceivedToken = (uint32_t)strtoul(pcOutValue, NULL, 10);

        /* If we are waiting for a response, ulClientToken will be the token for the response
         * we are waiting for, else it will be 0. ulRecievedToken may not match if the response is
         * not for us or if it is is a response that arrived after we timed out
         * waiting for it.
         */
        if (ulReceivedToken != ulClientToken)
        {
            LogDebug(("Ignoring publish on /update/rejected with clientToken %lu.", (unsigned long)ulReceivedToken));
        }
        else
        {
            /*  Obtain the error code. */
            result = JSON_Search((char *)pxPublishInfo->pPayload,
                                 pxPublishInfo->payloadLength,
                                 "code",
                                 sizeof("code") - 1,
                                 &pcOutValue,
                                 (size_t *)&ulOutValueLength);

            if (result != JSONSuccess)
            {
                LogWarn(("Received rejected response for update with token %lu and no error code.", (unsigned long)ulClientToken));
            }
            else
            {
                LogWarn(("Received rejected response for update with token %lu and error code %.*s.", (unsigned long)ulClientToken,
                         ulOutValueLength,
                         pcOutValue));
            }

            /* Wake up the shadow task which is waiting for this response. */
            xTaskNotifyGive(xShadowDeviceTaskHandle);
            //xSemaphoreGive(xPubAckWaitLock);
        }
    }
}

/*-----------------------------------------------------------*/

void vShadowTask(void *pvParameters)
{
    LogInfo(("============================== \n"));
    LogInfo(("Starting Shadow Demo Task\n"));
    LogInfo(("============================== \n"));

    /* Remove compiler warnings about unused parameters. */
    (void)pvParameters;

    bool xStatus = true;
    uint32_t ulNotificationValue;
    static MQTTPublishInfo_t xPublishInfo = {0};
    MQTTAgentCommandInfo_t xCommandParams = {0};
    MQTTStatus_t xCommandAdded;
    MQTTAgentCommandContext_t xApplicationDefinedContext;
    BaseType_t xReturn;

    /* Record the handle of this task so that the callbacks so the callbacks can
     * send a notification to this task. */
    xShadowDeviceTaskHandle = xTaskGetCurrentTaskHandle();

    /* Subscribe to Shadow topics. */
    xStatus = prvSubscribeToShadowUpdateTopics();
    if (xStatus == true)
    {
        printf("prvSubscribeToShadowUpdateTopics ........Passed \n");
    }

    //--------------For Deleting Shadow-------//

    /* Set up the MQTTAgentCommandInfo_t for the demo loop.
    * We do not need a completion callback here since for publishes, we expect to get a
    * response on the appropriate topics for accepted or rejected reports, and for pings
    * we do not care about the completion. 
    * */
    xCommandParams.blockTimeMs = SHADOW_MAX_COMMAND_SEND_BLOCK_TIME_MS;
    xCommandParams.cmdCompleteCallback = NULL; //prvIncomingPublishUpdateCallback;
    //xCommandParams.pCmdCompleteCallbackContext = &xApplicationDefinedContext;

    /* Set up MQTTPublishInfo_t for the update reports. */
    xPublishInfo.qos = MQTTQoS1;
    xPublishInfo.pTopicName = SHADOW_TOPIC_STRING_DELETE(THING_NAME),
    xPublishInfo.topicNameLength = SHADOW_TOPIC_LENGTH_DELETE(THING_NAME_LENGTH);
    xPublishInfo.pPayload = pcUpdateDesiredDocument;
    xPublishInfo.payloadLength = (SHADOW_DESIRED_JSON_LENGTH + 1);

    ulClientToken = (xTaskGetTickCount() % 1000000);

    /* Generate desired report. */
    (void)memset(pcUpdateDesiredDocument,
                 0x00,
                 sizeof(pcUpdateDesiredDocument));

    snprintf(pcUpdateDesiredDocument,
             SHADOW_DESIRED_JSON_LENGTH + 1,
             SHADOW_DESIRED_JSON,
             (int)ulDesiredPowerOnState,
             (long unsigned)ulClientToken);

    xCommandAdded = MQTTAgent_Publish(&xGlobalMqttAgentContext,
                                      &xPublishInfo,
                                      &xCommandParams);

    if (xCommandAdded == MQTTSuccess)
    {
        LogInfo((" Desired Document Published Successfully "));
    }
    else
    {
        LogError(("  Desired Document Published Failed "));
    }
    ulNotificationValue = ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(SHADOW_MS_TO_WAIT_FOR_NOTIFICATION));

    if (ulNotificationValue == 0)
    {
        LogError(("Did not get response from Delete."));
    }

    if (ulNotificationValue != 0)
    {
        LogInfo((" Shadow Deleted Successfully\n"));

        xStatus = prvUnsubscribeToShadowUpdateTopics();

        if (xStatus == true)
        {
            printf("prvUnsubscribeToShadowUpdateTopics ........Passed \n");
        }
        else
        {
            printf("prvUnsubscribeToShadowUpdateTopics ........Failed \n");
        }
    }

    ulClientToken = 0;

    // TaskHandle_t bme680 = NULL;
    // xTaskCreate(initialize_sensor,      //Calliing BME680 Initilize
    //             "BME680-Init",          //Task Name
    //             4096,                   //Stack size
    //             NULL,                   //Parameter
    //             2,                      //Priority
    //             &bme680);               //Task Handle

    //initialize_sensor();
    if (xStatus == true)
    {
        for (;;)
        {
            if (ulDesiredPowerOnState != ulPreviousPowerOnState)
            {

                /* Set up the MQTTAgentCommandInfo_t for the demo loop.
                * We do not need a completion callback here since for publishes, we expect to get a
                * response on the appropriate topics for accepted or rejected reports, and for pings
                * we do not care about the completion.
                * */
                xCommandParams.blockTimeMs = SHADOW_MAX_COMMAND_SEND_BLOCK_TIME_MS;
                xCommandParams.cmdCompleteCallback = NULL; //prvIncomingPublishUpdateCallback;
                //xCommandParams.pCmdCompleteCallbackContext = &xApplicationDefinedContext;

                /* Set up MQTTPublishInfo_t for the update reports. */
                xPublishInfo.qos = MQTTQoS1;
                xPublishInfo.pTopicName = SHADOW_TOPIC_STRING_UPDATE(THING_NAME);
                xPublishInfo.topicNameLength = SHADOW_TOPIC_LENGTH_UPDATE(THING_NAME_LENGTH);
                xPublishInfo.pPayload = pcUpdateDesiredDocument;
                xPublishInfo.payloadLength = (SHADOW_DESIRED_JSON_LENGTH + 1);

                /* Create a new client token and save it for use in the update accepted and rejected callbacks. */
                ulClientToken = (xTaskGetTickCount() % 1000000);

                /* Generate desired report. */
                (void)memset(pcUpdateDesiredDocument,
                             0x00,
                             sizeof(pcUpdateDesiredDocument));

                snprintf(pcUpdateDesiredDocument,
                         SHADOW_DESIRED_JSON_LENGTH + 1,
                         SHADOW_DESIRED_JSON,
                         (int)ulDesiredPowerOnState,
                         (long unsigned)ulClientToken);

                /* Send update. */
                LogInfo(("Publishing to %s with following client token %lu.", xPublishInfo.pTopicName, (long unsigned)ulClientToken));
                printf("Desired Publish content: %.*s \n", SHADOW_DESIRED_JSON_LENGTH, pcUpdateDesiredDocument);

                xCommandAdded = MQTTAgent_Publish(&xGlobalMqttAgentContext,
                                                  &xPublishInfo,
                                                  &xCommandParams);

                if (xCommandAdded == MQTTSuccess)
                {
                    LogInfo((" Desired Document Published Successfully "));
                }
                else
                {
                    LogError(("  Desired Document Published Failed "));
                }

                ulNotificationValue = ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(SHADOW_MS_TO_WAIT_FOR_NOTIFICATION));

                if (ulNotificationValue == 0)
                {
                    LogError(("Timed out waiting for response to desired."));
                }
                else
                {
                    LogInfo((" Get Desired Response \n"));

                    ulPreviousPowerOnState = ulDesiredPowerOnState;
                }
                ulClientToken = 0;
            }

            if (stateChanged == true)
            {
                LogInfo(("PowerOn state is now %u. Sending new report.", (unsigned int)ulReportPowerOnState));

                /* Set up the MQTTAgentCommandInfo_t for the demo loop.
                * We do not need a completion callback here since for publishes, we expect to get a
                * response on the appropriate topics for accepted or rejected reports, and for pings
                * we do not care about the completion.
                * */
                xCommandParams.blockTimeMs = SHADOW_MAX_COMMAND_SEND_BLOCK_TIME_MS;
                xCommandParams.cmdCompleteCallback = NULL; //prvIncomingPublishUpdateCallback;
                //xCommandParams.pCmdCompleteCallbackContext = &xApplicationDefinedContext;

                /* Set up MQTTPublishInfo_t for the update reports. */
                xPublishInfo.qos = MQTTQoS1;
                xPublishInfo.pTopicName = SHADOW_TOPIC_STRING_UPDATE(THING_NAME);
                xPublishInfo.topicNameLength = SHADOW_TOPIC_LENGTH_UPDATE(THING_NAME_LENGTH);
                xPublishInfo.pPayload = pcUpdateReportDocument;
                xPublishInfo.payloadLength = (SHADOW_REPORTED_JSON_LENGTH + 1);
                /* Create a new client token and save it for use in the update accepted and rejected callbacks. */
                ulClientToken = (xTaskGetTickCount() % 1000000);

                /* Generate update report. */
                (void)memset(pcUpdateReportDocument,
                             0x00,
                             sizeof(pcUpdateReportDocument));

                snprintf(pcUpdateReportDocument,
                         SHADOW_REPORTED_JSON_LENGTH + 1,
                         SHADOW_REPORTED_JSON,
                         (int)ulReportPowerOnState,
                         (long unsigned)ulClientToken);

                /* Send update. */
                LogInfo(("Publishing to %s with following client token %lu.", xPublishInfo.pTopicName, (long unsigned)ulClientToken));
                printf("Report Publish content: %.*s \n", SHADOW_REPORTED_JSON_LENGTH, pcUpdateReportDocument);

                xCommandAdded = MQTTAgent_Publish(&xGlobalMqttAgentContext,
                                                  &xPublishInfo,
                                                  &xCommandParams);

                if (xCommandAdded == MQTTSuccess)
                {
                    LogInfo((" Mqtt Agent Publish .... Success "));
                }
                else
                {
                    LogError((" Mqtt Agent Publish .... Failed "));
                }
                /* Wait for the response to our report. When the Device shadow service receives the request it will
                 * publish a response to  the /update/accepted or update/rejected */
                ulNotificationValue = ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(SHADOW_MS_TO_WAIT_FOR_NOTIFICATION));

                if (ulNotificationValue == 0)
                {
                    LogError(("Timed out waiting for response to report."));
                }
                else
                {
                    stateChanged = false;
                }

                /* Clear the client token */
                ulClientToken = 0;
            }

            //LogInfo(("No change in powerOn state since last report. Current state is %u.", ulDesiredPowerOnState));
            /* The following line is only needed for winsim. Due to an inaccurate tick rate, the connection
            * times out as the keepalive packets are not sent at the expected interval.
            */
            // MQTTAgent_Ping(&xGlobalMqttAgentContext,
            //                &xCommandParams);

            LogDebug(("Sleeping until next update check."));
            vTaskDelay(pdMS_TO_TICKS(DELAY_BETWEEN_DEMO_ITERATIONS_TICKS));
        }
    }
}

/*-----------------------------------------------------------*/
