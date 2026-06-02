#ifndef __SNAPCLIENT_H__
#define __SNAPCLIENT_H__

#ifdef __cplusplus
extern "C" {
#endif

int init_snapcast(void (*set_volume)(int), void (*set_mute)(bool));
void start_snapcast();

// Send volume/mute to the snapserver targeting only THIS client (by MAC)
// Uses JSON-RPC method Client.SetVolume on the server
// Returns 0 on success, -1 on failure
int snapcast_send_client_volume(int volume, bool muted);

#ifdef __cplusplus
}
#endif

#endif // __SNAPCLIENT_H__
