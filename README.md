# amazon-freertos-custom-v202107
## Introduction
* Shadow, MQTT and OTA tested together.
* Used coreMQTT-Agent for mqtt connection sharing.

It contains:
* Shadow task which publish desire and reported shadow.
* Publishing and Suscibing the MQTT topics.
* OTA over same mqtt connnection.

## Hardware

- ESP32


### Toolchain

Already given in repository `xtensa-esp32-elf-gcc8_4_0-esp-2020r3-linux-amd64.tar.gz`

Add the following environment variable to `~/.profile` file.

```shell script
* export IDF_PATH=cd/path/to/amazon-freertos-custom-v202107/amazon-freertos/vendors/espressif/esp-idf

* export PATH=$PATH:$IDF_PATH/tools

```

### Install dependency libraries 
```shell script
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-setuptools cmake ninja-build ccache libffi-dev libssl-dev dfu-util

cd path/to/amazon-freertos-custom-v202107

pip install --user -r amazon-freertos/vendors/espressif/esp-idf/requirements.txt
```

### Install python dependency libraries 
```shell script
* cd path/to/amazon-freertos-custom-v202107/amazon-freertos/vendors/espressif/esp-idf
* ./install.sh
* . ./export.sh
```

### Configure
* [Follow the instruction](https://docs.aws.amazon.com/freertos/latest/userguide/freertos-prereqs.html) and retrieve the following information 

   * Your AWS IoT endpoint
   * IoT thing name
   * aws_clientcredential_keys.h
* Open the freertos-configs/aws_clientcredential.h file. And change/set the following values:

```
...
#define clientcredentialMQTT_BROKER_ENDPOINT "[YOUR AWS IOT ENDPOINT]"
...
#define clientcredentialIOT_THING_NAME       "[YOUR IOT THING NAME]"
...
#define clientcredentialWIFI_SSID            "[WILL BE PROVIDED]"
...
#define clientcredentialWIFI_PASSWORD        "[WILL BE PROVIDED]"
...
#define clientcredentialWIFI_SECURITY        eWiFiSecurityWPA2
...
```

*  Copy aws_clientcredential_keys.h to freertos-configs directory.
*  Add OTA certificate in `ota_demo_config.h` and drag it at freertos-config directory. 
*  You can also check the App verison there.


### Compile 

```shell script
cmake -S . -B build -DIDF_SDKCONFIG_DEFAULTS=./sdkconfig -DCMAKE_TOOLCHAIN_FILE=amazon-freertos/tools/cmake/toolchains/xtensa-esp32.cmake -GNinja

cmake --build build --j4
```

### Flash code

```shell script
cmake --build build --target flash -j4
```
 
### Monitoring

```shell script
idf.py monitor
```
