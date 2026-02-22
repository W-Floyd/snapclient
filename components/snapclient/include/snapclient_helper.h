#ifndef __SNAPCLIENT_HELPER_H__
#define __SNAPCLIENT_HELPER_H__

#ifdef __cplusplus
extern "C" {
#endif

// Settings manager is an optional component that can be used to persist snapclient settings like server host and port,
// mdns enabled etc. If it's not included, default values from sdkconfig will be used (if set) or hardcoded defaults.
#ifndef HAS_SETTINGS_MANAGER
esp_err_t settings_get_mdns_enabled(bool* enabled);
esp_err_t settings_get_server_host(char *host, size_t max_len);
esp_err_t settings_get_server_port(int32_t* port);
esp_err_t settings_get_hostname(char *hostname, size_t max_len);
#endif

// provide few functions to avoid including the whole network_interface component
#ifndef HAS_NET_IF
#define NETWORK_INTERFACE_DESC_STA "sta"
#define NETWORK_INTERFACE_DESC_ETH "eth"

esp_netif_t *network_get_netif_from_desc(const char *desc);
#endif

#ifdef __cplusplus
}
#endif

#endif // __SNAPCLIENT_HELPER_H__