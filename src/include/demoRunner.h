
/* The config header is always included first. */
#include "iot_config.h"

/* Includes for library initialization. */
#include "iot_demo_runner.h"
#include "platform/iot_threads.h"
#include "types/iot_network_types.h"

#include "aws_demo.h"
#include "aws_demo_config.h"

/**
 * @brief All C SDK demo functions have this signature.
 */
// typedef int (*demoFunction_t)(bool awsIotMqttMode,
//                               const char *pIdentifier,
//                               void *pNetworkServerInfo,
//                               void *pNetworkCredentialInfo,
//                               const IotNetworkInterface_t *pNetworkInterface);

// typedef void (*networkConnectedCallback_t)(bool awsIotMqttMode,
//                                            const char *pIdentifier,
//                                            void *pNetworkServerInfo,
//                                            void *pNetworkCredentialInfo,
//                                            const IotNetworkInterface_t *pNetworkInterface);

// typedef void (*networkDisconnectedCallback_t)(const IotNetworkInterface_t *pNetworkInteface);

/* Forward declaration of demo entry function to be renamed from #define in
 * aws_demo_config.h */
int DEMO_entryFUNCTION( bool awsIotMqttMode,
                        const char * pIdentifier,
                        void * pNetworkServerInfo,
                        void * pNetworkCredentialInfo,
                        const IotNetworkInterface_t * pNetworkInterface );


/* Forward declaration of network connected DEMO callback to be renamed from
 * #define in aws_demo_config.h */
#ifdef DEMO_networkConnectedCallback
    void DEMO_networkConnectedCallback( bool awsIotMqttMode,
                                        const char * pIdentifier,
                                        void * pNetworkServerInfo,
                                        void * pNetworkCredentialInfo,
                                        const IotNetworkInterface_t * pNetworkInterface );
#else
    #define DEMO_networkConnectedCallback    ( NULL )
#endif


/* Forward declaration of network disconnected DEMO callback to be renamed from #define in aws_demo_config.h */
#ifdef DEMO_networkDisconnectedCallback
    void DEMO_networkDisconnectedCallback( const IotNetworkInterface_t * pNetworkInterface );
#else
    #define DEMO_networkDisconnectedCallback    ( NULL )
#endif


void runDemoTaskCustom(void);