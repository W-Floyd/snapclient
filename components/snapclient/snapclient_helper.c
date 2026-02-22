#include "snapclient_helper.h"

#include <string.h>


// Settings manager is an optional component that can be used to persist snapclient settings like server host and port,
// mdns enabled etc. If it's not included, default values from sdkconfig will be used (if set) or hardcoded defaults.
#ifndef HAS_SETTINGS_MANAGER
esp_err_t settings_get_mdns_enabled(bool* enabled) {
#ifdef CONFIG_SNAPSERVER_USE_MDNS
  *enabled = CONFIG_SNAPSERVER_USE_MDNS;
#else
  *enabled = false;
#endif
  return ESP_OK;
}

esp_err_t settings_get_server_host(char *host, size_t max_len) {
#ifdef CONFIG_SNAPSERVER_HOST
  strncpy(host, CONFIG_SNAPSERVER_HOST, max_len - 1);
  host[max_len - 1] = '\0';
#else
  host[0] = '\0';
#endif
  return ESP_OK;
}

esp_err_t settings_get_server_port(int32_t* port) {
#ifdef CONFIG_SNAPSERVER_PORT
  *port = CONFIG_SNAPSERVER_PORT;
#else
  *port = 0;
#endif
  return ESP_OK;
}

esp_err_t settings_get_hostname(char *hostname, size_t max_len) {
#ifdef CONFIG_SNAPCLIENT_NAME
  strncpy(hostname, CONFIG_SNAPCLIENT_NAME, max_len - 1);
  hostname[max_len - 1] = '\0';
#else
  // Ultimate fallback
  strncpy(hostname, "esp32-snapclient", max_len - 1);
  hostname[max_len - 1] = '\0';
#endif
  return ESP_OK;
}
#endif

// provide few functions to avoid including the whole network_interface component
#ifndef HAS_NET_IF

static bool netif_desc_matches_with(esp_netif_t *netif, void *ctx) {
  return strcmp(ctx, esp_netif_get_desc(netif)) == 0;
}

/**
 */
esp_netif_t *network_get_netif_from_desc(const char *desc) {
  return esp_netif_find_if(netif_desc_matches_with, (void *)desc);
}
#endif