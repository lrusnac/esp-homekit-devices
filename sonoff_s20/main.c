#include <stdio.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
#include <espressif/esp_common.h>
#include <esp/uart.h>
#include <esp8266.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <string.h>
#include <sysparam.h>

#include "button.h"

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include "wifi_config.h"

#include <paho_mqtt_c/MQTTESP8266.h>
#include <paho_mqtt_c/MQTTClient.h>

#define MQTT_HOST ("192.168.1.42") // TODO change to your brocker ip/address
#define MQTT_PORT 1883
#define MQTT_USER "InsertYoutMqttUserHere" // TODO change to your username
#define MQTT_PASS "InsertYoutMqttPassHere" // TODO change to your password

#define MQTT_CMND_TOPIC "leo/outlets/command/kitchen/kettle" // TODO configure to your mqtt topic
#define MQTT_STAT_TOPIC "leo/outlets/status/kitchen/kettle" // TODO configure to your mqtt topic

QueueHandle_t publish_queue;
#define MQTT_MSG_LEN 16

const int led_gpio = 13;
const int relay_gpio = 12;
const int button_gpio = 0;

bool device_status = false;

void user_init();
void on_wifi_ready();
static char *get_accessory_name();
void device_write(bool value);

void button_callback(uint8_t gpio, button_event_t event);

homekit_value_t device_status_get();
void device_status_set(homekit_value_t value);

void send_device_status_to_mqtt();
static void mqtt_on_message_received(mqtt_message_data_t *md);
static void mqtt_task(void *pvParameters);

homekit_characteristic_t outlet_characteristic = HOMEKIT_CHARACTERISTIC_(ON, false, .getter=device_status_get, .setter=device_status_set);
homekit_characteristic_t accessory_serial_number = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, NULL);

homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_outlet, .services=(homekit_service_t*[]){
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Sonoff Outlet"),
            HOMEKIT_CHARACTERISTIC(MANUFACTURER, "Leo's lab"),
            HOMEKIT_CHARACTERISTIC(MODEL, "s20"),
            HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "0.42"),
            HOMEKIT_CHARACTERISTIC(IDENTIFY, NULL),
            &accessory_serial_number,
            NULL
        }),
        HOMEKIT_SERVICE(OUTLET, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Kettle"),
            HOMEKIT_CHARACTERISTIC(OUTLET_IN_USE, true),
            &outlet_characteristic,
            NULL
        }),
        NULL
    }),
    NULL
};

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-42-111"
};

void device_write(bool value) {
    device_status = value;
    gpio_write(led_gpio, value ? 1 : 0);
    gpio_write(relay_gpio, value ? 1 : 0);

    send_device_status_to_mqtt();
    
    homekit_characteristic_notify(&outlet_characteristic, HOMEKIT_BOOL(value));
    sysparam_set_bool("device_status", device_status); // save device_status to flash
}

homekit_value_t device_status_get() {
    return HOMEKIT_BOOL(device_status);
}

void device_status_set(homekit_value_t value) {
    if (value.format != homekit_format_bool) {
        printf("Invalid value format: %d\n", value.format);
        return;
    }

    device_write(value.bool_value);
}

void send_device_status_to_mqtt() {
    char msg[MQTT_MSG_LEN];

    if (device_status) {
        snprintf(msg, MQTT_MSG_LEN, "on");
    } else {
        snprintf(msg, MQTT_MSG_LEN, "off");
    }

    if (xQueueSend(publish_queue, (void *)msg, 0) == pdFALSE) {
        printf("Publish queue overflow.\r\n");
    }
}

static void mqtt_on_message_received(mqtt_message_data_t *md) {
    mqtt_message_t *message = md->message;
    bool value = false;

    if (!strncmp(message->payload, "on", 2)) {
        value = true;
    } else if (!strncmp(message->payload, "off", 3)) {
        value = false;
    } else if (!strncmp(message->payload, "toggle", 6)) {
        value = !device_status;
    } else {
        printf("unsupported message: %s\r\n", message->payload);
        return;
    }

    device_write(value);
}

void button_callback(uint8_t gpio, button_event_t event) {
    printf("Toggling outlet\n");
    device_write(!device_status);
}

static char *get_accessory_name() {
    uint8_t macaddr[6];
    sdk_wifi_get_macaddr(STATION_IF, macaddr);
    
    int name_len = snprintf(NULL, 0, "ESP-%02X%02X%02X", macaddr[3], macaddr[4], macaddr[5]);
    char *name_value = malloc(name_len+1);
    snprintf(name_value, name_len+1, "ESP-%02X%02X%02X", macaddr[3], macaddr[4], macaddr[5]);
    
    return name_value;
}

void on_wifi_ready() {
    homekit_server_init(&config);
    xTaskCreate(&mqtt_task, "mqtt_task", 1024, NULL, 4, NULL);
}

void user_init() {
    uart_set_baud(0, 115200);

    publish_queue = xQueueCreate(3, MQTT_MSG_LEN);
    wifi_config_init("ESP", NULL, on_wifi_ready); 

    gpio_enable(led_gpio, GPIO_OUTPUT);
    gpio_enable(relay_gpio, GPIO_OUTPUT);
    
    sysparam_get_bool("device_status", &device_status); // read device_status from flash
    
    device_write(device_status);
    accessory_serial_number.value = HOMEKIT_STRING(get_accessory_name());

    if (button_create(button_gpio, 0, 10000, button_callback)) {
        printf("Failed to initialize button\n");
    }
}

static void mqtt_task(void *pvParameters) {
    int ret = 0;
    struct mqtt_network network;
    mqtt_client_t client = mqtt_client_default;
    char mqtt_client_id[20];
    uint8_t mqtt_buf[100];
    uint8_t mqtt_readbuf[100];
    mqtt_packet_connect_data_t data = mqtt_packet_connect_data_initializer;

    mqtt_network_new(&network);
    memset(mqtt_client_id, 0, sizeof(mqtt_client_id));
    strcat(mqtt_client_id, get_accessory_name());

    printf("mqtt client id: %s\r\n", mqtt_client_id);

    data.willFlag = 0;
    data.MQTTVersion = 4;
    data.clientID.cstring = mqtt_client_id;
    data.username.cstring = MQTT_USER;
    data.password.cstring = MQTT_PASS;
    data.keepAliveInterval = 10;
    data.cleansession = 0;

    while (1) {
        printf("%s: (Re)connecting to MQTT server %s ... ",__func__, MQTT_HOST);
        ret = mqtt_network_connect(&network, MQTT_HOST, MQTT_PORT);
        if (ret) {
            printf("error: %d\n\r", ret);
            taskYIELD();
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        printf("done\n\r");
        mqtt_client_new(&client, &network, 5000, mqtt_buf, 100, mqtt_readbuf, 100);

        printf("Send MQTT connect ... ");
        ret = mqtt_connect(&client, &data);
        if (ret) {
            printf("error: %d\n\r", ret);
            mqtt_network_disconnect(&network);
            taskYIELD();
            continue;
        }
        printf("done\r\n");
        
        mqtt_subscribe(&client, MQTT_CMND_TOPIC, MQTT_QOS1, mqtt_on_message_received);
        xQueueReset(publish_queue);

        while (1) {
            char msg[MQTT_MSG_LEN - 1] = "\0";
            while (xQueueReceive(publish_queue, (void *)msg, 0) == pdTRUE) {
                printf("got message to publish\r\n");
                mqtt_message_t message;
                message.payload = msg;
                message.payloadlen = strlen(msg);
                message.dup = 0;
                message.qos = MQTT_QOS1;
                message.retained = 0;

                ret = mqtt_publish(&client, MQTT_STAT_TOPIC, &message);
                if (ret != MQTT_SUCCESS){
                    printf("error while publishing message: %d\n", ret);
                    break;
                }
            }

            ret = mqtt_yield(&client, 1000);
            if (ret == MQTT_DISCONNECTED) break;
        }

        printf("Connection dropped, request restart\n\r");
        mqtt_network_disconnect(&network);
        taskYIELD();
    }
}