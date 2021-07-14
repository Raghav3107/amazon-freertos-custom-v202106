
/* The config header is always included first. */
#include "iot_config.h"

#include "FreeRTOS.h"

/* Standard includes. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "aws_clientcredential.h"
#include "aws_clientcredential_keys.h"
#include "iot_demo_logging.h"
#include "iot_init.h"

/* Include common demo header. */
#include "aws_demo.h"



/* Constants that select which demos to build into the project:
 * Set the following to 1 to include the demo in the build, or 0 to exclude the
 * demo. 
 * */

#define democonfigCREATE_MQTT_PUB_SUB_TASK                 1 

#define democonfigCREATE_CODE_SIGNING_OTA_DEMO             1

#define democonfigCREATE_SHADOW_DEMO                       1


/**
 * @brief Create the task that demonstrates the sharing of an MQTT connection
 * using the coreMQTT Agent library.
 */
int RunCoreMqttAgentDemo( bool awsIotMqttMode,
                          const char * pIdentifier,
                          void * pNetworkServerInfo,
                          void * pNetworkCredentialInfo,
                          const void * pNetworkInterface );



void MQTT_Pub_Sub_Task(void *pvParameters);

void vShadowTask(void *pvParameters);