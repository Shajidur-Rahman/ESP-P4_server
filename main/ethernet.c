#include "ethernet.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "ethernet_init.h"

static const char *TAG = "ethernet";

static bool s_has_ip = false;
static char s_ip_str[32] = "0.0.0.0";
static char s_mac_str[32] = "00:00:00:00:00:00";

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac);
        snprintf(s_mac_str, sizeof(s_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Ethernet Link Up, MAC: %s", s_mac_str);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_has_ip = false;
        strcpy(s_ip_str, "0.0.0.0");
        ESP_LOGI(TAG, "Ethernet Link Down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Interface Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Interface Stopped");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ip_info->ip));
    s_has_ip = true;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Ethernet Connected!");
    ESP_LOGI(TAG, " IP Address : %s", s_ip_str);
    ESP_LOGI(TAG, " Netmask    : " IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, " Gateway    : " IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, " Web Server : http://%s", s_ip_str);
    ESP_LOGI(TAG, "========================================");
}

esp_err_t ethernet_start(void)
{
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles = NULL;

    ESP_LOGI(TAG, "Initializing Waveshare IP101 Ethernet driver (MDC=31, MDIO=52, RST=51)...");
    esp_err_t err = example_eth_init(&eth_handles, &eth_port_cnt);
    if (err != ESP_OK || eth_port_cnt == 0 || eth_handles == NULL) {
        ESP_LOGE(TAG, "Ethernet PHY initialization failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    assert(eth_netif);

    esp_eth_netif_glue_handle_t eth_glue = esp_eth_new_netif_glue(eth_handles[0]);
    assert(eth_glue);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, eth_glue));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    ESP_LOGI(TAG, "Starting Ethernet state machine...");
    ESP_ERROR_CHECK(esp_eth_start(eth_handles[0]));

    return ESP_OK;
}

bool ethernet_has_ip(void)
{
    return s_has_ip;
}

const char *ethernet_get_ip_str(void)
{
    return s_ip_str;
}

const char *ethernet_get_mac_str(void)
{
    return s_mac_str;
}
