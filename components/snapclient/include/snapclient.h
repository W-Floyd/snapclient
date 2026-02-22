#ifndef __SNAPCLIENT_H__
#define __SNAPCLIENT_H__

#ifdef __cplusplus
extern "C" {
#endif

void init_snapcast(void (*set_volume)(int), void (*set_mute)(bool));
void start_snapcast();

#ifdef __cplusplus
}
#endif

#endif // __SNAPCLIENT_H__
