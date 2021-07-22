/**
 * @file mqtt_pub_sub.c
 * @brief 
 * This file demonstrates using the MQTT agent API to send MQTT packets that
 * contain a payload nearly equal to the buffer size used to serialize and
 * deserialize MQTT packets.  It can be used to test behavior when the MQTT
 * packet is larger than the TCP/IP buffers.  The task can run simultaneously
 * to other demo tasks that also use the MQTT agent API to interact over the
 * same MQTT connection to the same MQTT broker.
 *
 * prvLargeMessageSubscribePublishTask() implements the demo task, which
 * subscribes to a topic then periodically publishes large payloads to the
 * same topic to which it has subscribed.  Each time it publishes to the topic
 * it waits for the published data to be published back to it from the MQTT
 * broker - checking that the received data matches the transmitted data
 * exactly.
 */

/* Standard includes. */
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* MQTT library includes. */
#include "core_mqtt.h"

/* MQTT agent include. */
#include "core_mqtt_agent.h"

/* Subscription manager header include. */
#include "subscription_manager.h"

/* custom Demo Runner Configuration. */
#include "demoRunnerConfig.h"

/**
 * @brief This demo uses task notifications to signal tasks from MQTT callback
 * functions.  MQTT_LARGE_MSG_MS_TO_WAIT_FOR_NOTIFICATION defines the time, in ticks,
 * to wait for such a callback.
 */
#define MQTT_LARGE_MSG_MS_TO_WAIT_FOR_NOTIFICATION (5000)

/**
 * @brief Time, in milliseconds, to wait between cycles of the demo task.
 */
#define MQTT_LARGE_MSG_DELAY_BETWEEN_PUBLISH_OPERATIONS_MS (1000UL)

/**
 * @brief The maximum amount of time in milliseconds to wait for the commands
 * to be posted to the MQTT agent should the MQTT agent's command queue be full.
 * Tasks wait in the Blocked state, so don't use any CPU time.
 */
#define MQTT_LARGE_MSG_MAX_COMMAND_SEND_BLOCK_TIME_MS (200)

/**
 * @brief Create an MQTT payload that almost fills the buffer allocated for
 * MQTT message serialization, leaving a little room for the MQTT protocol
 * headers themselves.
 */
#define MQTT_LARGE_MSG_PROTOCOL_OVERHEAD (50)
#define MQTT_LARGE_MSG_MAX_PAYLOAD_LENGTH (MQTT_AGENT_NETWORK_BUFFER_SIZE - MQTT_LARGE_MSG_PROTOCOL_OVERHEAD)

/*-----------------------------------------------------------*/

/**
 * @brief Defines the structure to use as the command callback context in this
 * demo.
 */
struct MQTTAgentCommandContext
{
    MQTTStatus_t xReturnStatus; /* Pass out the result of the operation. */
    TaskHandle_t xTaskToNotify; /* Handle of the task to send a notification to. */
    void *pvTag;                /* Use for callback specific data. */
};

typedef struct
{
    char pcPublishMsgBuffer[300];
    uint8_t uBufferLength;
} PublishDataInfo_t;

PublishDataInfo_t xPublishDataInfo;

/**< It allow to publish the payload as soon the payload is created*/
bool bTriggerPublish = false;
/**
 * @brief Passed into MQTTAgent_Subscribe() as the callback to execute when the
 * broker ACKs the SUBSCRIBE message.  Its implementation sends a notification
 * to the task that called MQTTAgent_Subscribe() to let the task know the
 * SUBSCRIBE operation completed.  It also sets the xReturnStatus of the
 * structure passed in as the command's context to the value of the
 * xReturnStatus parameter - which enables the task to check the status of the
 * operation.
 *
 * See https://freertos.org/mqtt/mqtt-agent-demo.html#example_mqtt_api_call
 *
 * @param[in] pxCommandContext Context of the initial command.
 * @param[in].xReturnStatus The result of the command.
 */
static void prvSubscribeCommandCallback(void *pxCommandContext,
                                        MQTTAgentReturnInfo_t *pxReturnInfo);

/**
 * @brief Passed into MQTTAgent_Subscribe() as the callback to execute when
 * there is an incoming publish on the topic being subscribed to.  Its
 * implementation copies the incoming MQTT message payload into a buffer so the
 * task can validate the received data matches the outgoing data (the task
 * subscribes to the same topic that it publishes to, so any outgoing data on
 * that topic is also received back from the MQTT broker).
 *
 * See https://freertos.org/mqtt/mqtt-agent-demo.html#example_mqtt_api_call
 *
 * @param[in] pxSubscriptionContext Context of the initial command.
 * @param[in] pxCommandContext Context of the initial command.
 */
static void prvLargePSIncomingPublishCallback(void *pxSubscriptionContext,
                                              MQTTPublishInfo_t *pxPublishInfo);

/**
 * @brief Called by the task to wait for a notification from a callback function
 * after the task first executes either MQTTAgent_Publish()* or
 * MQTTAgent_Subscribe().
 *
 * See https://freertos.org/mqtt/mqtt-agent-demo.html#example_mqtt_api_call
 *
 * @param[in] pxCommandContext Context of the initial command.
 *
 * @return The value returned from the containing ulTaskNotifyTake() call.  This
 * is the task's notification value before it was decremented by
 * ulTaskNotificationTake().
 */
static uint32_t prvWaitForCommandAcknowledgment(void);

/**
 * @brief Create a buffer of data that is easily recognizable in WireShark.
 * Completely fill the MQTT network buffer, other than leaving a space for the
 * MQTT protocol headers themselves.
 *
 * @param[in] pcBuffer The buffer into which the MQTT payload is written.
 * @param[in] xBufferSize The length of buffer pointed to by pcBuffer.
//  */
// static void prvCreateMQTTPayload(char *pcBuffer,
//                                  size_t xBufferSize);

/**
 * @brief Subscribe to the topic the demo task will also publish to - that
 * results in all outgoing publishes being published back to the task
 * (effectively echoed back).
 *
 * @param[in] pcReceivedPublishPayload The buffer into which the callback that
 * executes on reception of incoming publish messages will write the payload
 * from the incoming message.  This is stored in the callback context so it can
 * be accessed from within the callback function.
 */
static void prvSubscribeToTopic();

/**
 * @brief The function that implements the task demonstrated by this file.
 */
void prvLargeMessageSubscribePublishTask(void *pvParameters);

/*-----------------------------------------------------------*/

/* The MQTT topic used by this demo. */
static const char *pcTopicFilter = "iot_demo/new/aws/test";

extern MQTTAgentContext_t xGlobalMqttAgentContext;

/*-----------------------------------------------------------*/

/*-----------------------------------------------------------*/

static void prvSubscribeCommandCallback(void *pxCommandContext,
                                        MQTTAgentReturnInfo_t *pxReturnInfo)
{
    bool xSubscriptionAdded = false;
    MQTTAgentCommandContext_t *pxApplicationDefinedContext = (MQTTAgentCommandContext_t *)pxCommandContext;

    /* Store the result in the application defined context so the calling task
     * can check it. */
    pxApplicationDefinedContext->xReturnStatus = pxReturnInfo->returnCode;

    /* Check if the subscribe operation is a success. Only one topic is
     * subscribed by this demo. */
    if (pxReturnInfo->returnCode == MQTTSuccess)
    {
        /* Add subscription so that incoming publishes are routed to the application
         * callback. */
        xSubscriptionAdded = addSubscription((SubscriptionElement_t *)xGlobalMqttAgentContext.pIncomingCallbackContext,
                                             pcTopicFilter,
                                             (uint16_t)strlen(pcTopicFilter),
                                             prvLargePSIncomingPublishCallback,
                                             pxApplicationDefinedContext);

        if (xSubscriptionAdded == false)
        {
            LogError(("Failed to register an incoming publish callback for topic %.*s.",
                      (unsigned int)strlen(pcTopicFilter),
                      pcTopicFilter));
        }
    }

    xTaskNotifyGive(pxApplicationDefinedContext->xTaskToNotify);
}

/*-----------------------------------------------------------*/

static void prvLargePSIncomingPublishCallback(void *pxSubscriptionContext,
                                              MQTTPublishInfo_t *pxPublishInfo)
{
    MQTTAgentCommandContext_t *pxApplicationDefinedContext = (MQTTAgentCommandContext_t *)pxSubscriptionContext;

    /* Check the incoming message will fit in the buffer before writing it to
     * buffer so the demo task can check the data received from the topic
     * matches the data previously written to the same topic. */
    configASSERT(pxPublishInfo->payloadLength <= MQTT_LARGE_MSG_MAX_PAYLOAD_LENGTH);
    static char cTerminatedString[MQTT_LARGE_MSG_MAX_PAYLOAD_LENGTH];

    /* Create a message that contains the incoming MQTT payload to the logger,
     * terminating the string first. */
    if (pxPublishInfo->payloadLength < MQTT_LARGE_MSG_MAX_PAYLOAD_LENGTH)
    {
        memcpy((void *)cTerminatedString, pxPublishInfo->pPayload, pxPublishInfo->payloadLength);
        cTerminatedString[pxPublishInfo->payloadLength] = 0x00;
    }
    else
    {
        memcpy((void *)cTerminatedString, pxPublishInfo->pPayload, MQTT_LARGE_MSG_MAX_PAYLOAD_LENGTH);
        cTerminatedString[MQTT_LARGE_MSG_MAX_PAYLOAD_LENGTH - 1] = 0x00;
    }

    printf("Payload Received [ %s ] \n", (char *)cTerminatedString);

    /* Send a notification to the task in case it is waiting for this incoming
     * message. */
    xTaskNotifyGive(pxApplicationDefinedContext->xTaskToNotify);
}

/*-----------------------------------------------------------*/

static uint32_t prvWaitForCommandAcknowledgment(void)
{
    uint32_t ulReturn;

    /* Wait for this task to get notified. */
    ulReturn = ulTaskNotifyTake(pdFALSE, MQTT_LARGE_MSG_MS_TO_WAIT_FOR_NOTIFICATION);

    return ulReturn;
}

/*-----------------------------------------------------------*/

void prvCreateMQTTPayload(char *pcBuffer,
                          size_t xBufferSize)
{
    strcpy(xPublishDataInfo.pcPublishMsgBuffer, pcBuffer);
    xPublishDataInfo.uBufferLength = xBufferSize;
    printf("Publish Data: %s Length:%u \n", xPublishDataInfo.pcPublishMsgBuffer, xPublishDataInfo.uBufferLength);
    bTriggerPublish = true;
}

/*-----------------------------------------------------------*/

static void prvSubscribeToTopic()
{
    MQTTAgentSubscribeArgs_t xSubscribeArgs;
    MQTTSubscribeInfo_t xSubscribeInfo;
    MQTTStatus_t xStatus;
    uint32_t ulNotificationValue;
    MQTTAgentCommandInfo_t xCommandParams = {0};

    /* Context must persist as long as subscription persists. */
    static MQTTAgentCommandContext_t xApplicationDefinedContext = {0};

    /* Record the handle of this task in the context that will be used within
    * the callbacks so the callbacks can send a notification to this task. */
    xApplicationDefinedContext.xTaskToNotify = xTaskGetCurrentTaskHandle();
    //xApplicationDefinedContext.pvTag = NULL; //( void * ) pcReceivedPublishPayload;

    /* Ensure the return status is not accidentally MQTTSuccess already. */
    xApplicationDefinedContext.xReturnStatus = MQTTBadParameter;

    /* Complete the subscribe information.  The topic string must persist for
     * duration of subscription - although in this case is it a static const so
     * will persist for the lifetime of the application. */
    xSubscribeInfo.pTopicFilter = pcTopicFilter;
    xSubscribeInfo.topicFilterLength = (uint16_t)strlen(pcTopicFilter);
    xSubscribeInfo.qos = MQTTQoS1;
    xSubscribeArgs.pSubscribeInfo = &xSubscribeInfo;
    xSubscribeArgs.numSubscriptions = 1;

    /* Loop in case the queue used to communicate with the MQTT agent is full and
     * attempts to post to it time out.  The queue will not become full if the
     * priority of the MQTT agent task is higher than the priority of the task
     * calling this function. */
    xTaskNotifyStateClear(NULL);
    xCommandParams.blockTimeMs = MQTT_LARGE_MSG_MAX_COMMAND_SEND_BLOCK_TIME_MS;
    xCommandParams.cmdCompleteCallback = prvSubscribeCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = &xApplicationDefinedContext;
    LogInfo(("Sending subscribe request to agent for topic filter: %s", pcTopicFilter));

    do
    {
        xStatus = MQTTAgent_Subscribe(&xGlobalMqttAgentContext,
                                      &(xSubscribeArgs),
                                      &xCommandParams);
    } while (xStatus != MQTTSuccess);

    /* Wait for acks from subscribe messages - this is optional.  If the
     * returned value is zero then the wait timed out. */
    ulNotificationValue = prvWaitForCommandAcknowledgment();
    configASSERT(ulNotificationValue != 0UL);

    /* The callback sets the xReturnStatus member of the context. */
    if (xApplicationDefinedContext.xReturnStatus == MQTTSuccess)
    {
        LogInfo(("Received subscribe ack for topic %s", pcTopicFilter));
    }
    else
    {
        LogError(("Failed to subscribe to topic %s", pcTopicFilter));
    }
}

/*-----------------------------------------------------------*/

void MQTT_Pub_Sub_Task(void *pvParameters)
{

    uint32_t ulLargeMessageFailures = 0;
    MQTTPublishInfo_t xPublishInfo = {0};
    MQTTStatus_t xCommandAdded;
    uint32_t ulNotificationValue;
    MQTTAgentCommandInfo_t xCommandParams = {0};

    /* Context must persist as long as subscription persists. */
    static MQTTAgentCommandContext_t xApplicationDefinedContext = {0};

    (void)pvParameters;

    prvSubscribeToTopic();

    /* Subscribe to the topic that this task will also publish to so all
     * outgoing publishes to that topic are published back to this task
     * effectively echoed back to this task). */

    /* Prepare the publish message. */
    memset((void *)&xPublishInfo, 0x00, sizeof(xPublishInfo));
    xPublishInfo.qos = MQTTQoS1;
    xPublishInfo.pTopicName = pcTopicFilter;
    xPublishInfo.topicNameLength = (uint16_t)strlen(pcTopicFilter);
    xPublishInfo.pPayload = xPublishDataInfo.pcPublishMsgBuffer;
    xPublishInfo.payloadLength = xPublishDataInfo.uBufferLength;

    for (;;)
    {
        if (bTriggerPublish == true)
        {

            /* Publish to the topic to which this task is also subscribed to
            * receive an echo back.  Note the command callback is left NULL so this
            * task will not be notified of when the PUBLISH ack is received - instead
            * it just waits for a notification from prvSubscribeCommandCallback() that
            * the incoming publish (the message being echoed back) has been received. 
            * */
            LogInfo(("Sending large publish request to agent with message on topic \"%s\" \n",
                     pcTopicFilter));
            xCommandParams.blockTimeMs = MQTT_LARGE_MSG_MAX_COMMAND_SEND_BLOCK_TIME_MS;
            xCommandParams.cmdCompleteCallback = NULL; /* Note not used as going to wait for the echo anyway. */
            xCommandAdded = MQTTAgent_Publish(&xGlobalMqttAgentContext,
                                              &xPublishInfo,
                                              &xCommandParams);

            /* Ensure the messages was sent to the MQTT agent task. */
            configASSERT(xCommandAdded == MQTTSuccess);

            /* Wait for the publish back to this task.  prvSubscribeCommandCallback()
            * will notify this task. */
            ulNotificationValue = ulTaskNotifyTake(pdFALSE, MQTT_LARGE_MSG_MS_TO_WAIT_FOR_NOTIFICATION);

            /* Only expect a single notification from the callback that executes
            * when the incoming publish (the message being echoed back) is
            * received. */
            if (ulNotificationValue != 1)
            {
                ulLargeMessageFailures++;

                LogError(("Error -Payload Publish Fail %d times\n",
                          (int)ulLargeMessageFailures));
            }
            else
            {
                ulLargeMessageFailures = 0;
                LogInfo(("Payload Publish Successfully"));
                bTriggerPublish = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MQTT_LARGE_MSG_MS_TO_WAIT_FOR_NOTIFICATION));
    }
}
