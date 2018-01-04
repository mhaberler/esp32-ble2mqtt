#include "config.h"
#include "ble.h"
#include "ble_utils.h"
#include "mqtt.h"
#include "wifi.h"
#include <esp_err.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

#define MAX_TOPIC_LEN 256
static const char *TAG = "BLE2MQTT";

/* Wi-Fi callback functions */
static void wifi_on_connected(void)
{
    ESP_LOGI(TAG, "Connected to WiFi, connecting to MQTT");
    mqtt_connect(config_mqtt_host_get(), config_mqtt_port_get(),
        config_mqtt_client_id_get(), config_mqtt_username_get(),
        config_mqtt_password_get());
}

static void wifi_on_disconnected(void)
{
    ESP_LOGI(TAG, "Disonnected from WiFi, stopping MQTT");
    mqtt_disconnect();
}

/* MQTT callback functions */
static void mqtt_on_connected(void)
{
    ESP_LOGI(TAG, "Connected to MQTT, scanning for BLE devices");
    ble_scan_start();
}

static void mqtt_on_disconnected(void)
{
    ESP_LOGI(TAG, "Disonnected from MQTT, stopping BLE");
    ble_disconnect_all();
    mqtt_connect(config_mqtt_host_get(), config_mqtt_port_get(),
        config_mqtt_client_id_get(), config_mqtt_username_get(),
        config_mqtt_password_get());
}

/* BLE functions */
static void ble_publish_connected(mac_addr_t mac, uint8_t is_connected)
{
    char topic[28];

    sprintf(topic, "%s/Connected", mactoa(mac));
    mqtt_publish(topic, (uint8_t *)(is_connected ? "true" : "false"),
        is_connected ? 4 : 5, config_mqtt_qos_get(),
        config_mqtt_retained_get());
}

/* BLE callback functions */
static void ble_on_device_discovered(mac_addr_t mac)
{
    uint8_t connect = config_ble_should_connect(mactoa(mac));

    ESP_LOGI(TAG, "Discovered BLE device: %s, %sconnecting", mactoa(mac),
        connect ? "" : "not ");

    if (!connect)
        return;

    ble_connect(mac);
}

static void ble_on_device_connected(mac_addr_t mac)
{
    ESP_LOGI(TAG, "Connected to device: %s, scanning", mactoa(mac));
    ble_publish_connected(mac, 1);
    ble_services_scan(mac);
}

static char *ble_topic_suffix(char *base, uint8_t is_get)
{
    static char topic[MAX_TOPIC_LEN];

    sprintf(topic, "%s%s", base, is_get ? config_mqtt_get_suffix_get() :
        config_mqtt_set_suffix_get());

    return topic;
}

static char *ble_topic(mac_addr_t mac, ble_uuid_t service_uuid,
    ble_uuid_t characteristic_uuid)
{
    static char topic[MAX_TOPIC_LEN];
    int i;

    i = sprintf(topic, "%s/%s", mactoa(mac), uuidtoa(service_uuid));
    sprintf(topic + i, "/%s", uuidtoa(characteristic_uuid));

    return topic;
}

static void ble_on_characteristic_removed(mac_addr_t mac, ble_uuid_t service_uuid,
    ble_uuid_t characteristic_uuid, uint8_t properties)
{
    char *topic = ble_topic(mac, service_uuid, characteristic_uuid);

    if (properties & CHAR_PROP_READ)
        mqtt_unsubscribe(ble_topic_suffix(topic, 1));

    if (properties & CHAR_PROP_WRITE)
        mqtt_unsubscribe(ble_topic_suffix(topic, 0));

    if (properties & CHAR_PROP_NOTIFY)
    {
        ble_characteristic_notify_unregister(mac, service_uuid,
            characteristic_uuid);
    }
}

static void ble_on_device_disconnected(mac_addr_t mac)
{
    ESP_LOGI(TAG, "Disconnected from device: %s", mactoa(mac));
    ble_publish_connected(mac, 0);
    ble_foreach_characteristic(mac, ble_on_characteristic_removed);
}

static int ble_split_topic(const char *topic, mac_addr_t mac, ble_uuid_t service,
    ble_uuid_t characteristic)
{
    char *s = strdup(topic);
    const char *mac_str, *service_str, *characteristic_str;
    int ret;

    mac_str = strtok(s, "/");
    service_str = strtok(NULL, "/");
    characteristic_str = strtok(NULL, "/");

    ret = atomac(mac_str, mac) || atouuid(service_str, service) ||
        atouuid(characteristic_str, characteristic);

    free(s);
    return ret;
}

static void ble_on_mqtt_get(const char *topic, const uint8_t *payload,
    size_t len)
{
    mac_addr_t mac;
    ble_uuid_t service, characteristic;

    ESP_LOGD(TAG, "Got read request: %s", topic);
    if (ble_split_topic(topic, mac, service, characteristic))
        return;

    ble_characteristic_read(mac, service, characteristic);
}

static void ble_on_mqtt_set(const char *topic, const uint8_t *payload,
    size_t len)
{
    mac_addr_t mac;
    ble_uuid_t service, characteristic;

    ESP_LOGD(TAG, "Got write request: %s, len: %u", topic, len);
    if (ble_split_topic(topic, mac, service, characteristic))
        return;

    ble_characteristic_write(mac, service, characteristic, payload, len);
}

static void ble_on_characteristic_found(mac_addr_t mac, ble_uuid_t service_uuid,
    ble_uuid_t characteristic_uuid, uint8_t properties)
{
    ESP_LOGD(TAG, "Found new characteristic!");
    ESP_LOGD(TAG, "  Service: %s", uuidtoa(service_uuid));
    ESP_LOGD(TAG, "  Characteristic: %s", uuidtoa(characteristic_uuid));
    char *topic = ble_topic(mac, service_uuid, characteristic_uuid);

    /* Characteristic is readable */
    if (properties & CHAR_PROP_READ)
    {
        mqtt_subscribe(ble_topic_suffix(topic, 1), config_mqtt_qos_get(),
            ble_on_mqtt_get);
        ble_characteristic_read(mac, service_uuid, characteristic_uuid);
    }

    /* Characteristic is writable */
    if (properties & CHAR_PROP_WRITE)
    {
        mqtt_subscribe(ble_topic_suffix(topic, 0), config_mqtt_qos_get(),
            ble_on_mqtt_set);
    }

    /* Characteristic can notify on changes */
    if (properties & CHAR_PROP_NOTIFY)
    {
        ble_characteristic_notify_register(mac, service_uuid,
            characteristic_uuid);
    }
}

static void ble_on_device_services_discovered(mac_addr_t mac)
{
    ESP_LOGD(TAG, "Services discovered on device: %s", mactoa(mac));
    ble_foreach_characteristic(mac, ble_on_characteristic_found);
}

static void ble_on_device_characteristic_value(mac_addr_t mac,
    ble_uuid_t service, ble_uuid_t characteristic, uint8_t *value,
    size_t value_len)
{
    char *topic = ble_topic(mac, service, characteristic);

    ESP_LOGI(TAG, "Publishing: %s", topic);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, value, value_len, ESP_LOG_DEBUG);
    mqtt_publish(topic, value, value_len, config_mqtt_qos_get(),
        config_mqtt_retained_get());
}

void app_main()
{
    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Init configuration */
    ESP_ERROR_CHECK(config_initialize());

    /* Init Wi-Fi */
    ESP_ERROR_CHECK(wifi_initialize());
    wifi_set_on_connected_cb(wifi_on_connected);
    wifi_set_on_disconnected_cb(wifi_on_disconnected);

    /* Init MQTT */
    ESP_ERROR_CHECK(mqtt_initialize());
    mqtt_set_on_connected_cb(mqtt_on_connected);
    mqtt_set_on_disconnected_cb(mqtt_on_disconnected);

    /* Init BLE */
    ESP_ERROR_CHECK(ble_initialize());
    ble_set_on_device_discovered_cb(ble_on_device_discovered);
    ble_set_on_device_connected_cb(ble_on_device_connected);
    ble_set_on_device_disconnected_cb(ble_on_device_disconnected);
    ble_set_on_device_services_discovered_cb(ble_on_device_services_discovered);
    ble_set_on_device_characteristic_value_cb(
        ble_on_device_characteristic_value);

    /* Start by connecting to WiFi */
    wifi_connect(config_wifi_ssid_get(), config_wifi_password_get());
}
