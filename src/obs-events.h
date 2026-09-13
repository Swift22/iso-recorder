#pragma once

#include <obs.h>

// libobs declares these in util/threading.h, but that header includes
// <pthread.h> on every platform, and a headers-only obs-studio checkout has no
// pthreads headers for Windows: they ship in OBS's own dependency bundle, not
// in the source tree. This plugin uses only the event half of that header, so
// the declarations are repeated here. The enum order and the signatures must
// match libobs exactly; the implementations come from obs.dll / libobs.

#ifdef __cplusplus
extern "C" {
#endif

enum os_event_type {
	OS_EVENT_TYPE_AUTO,
	OS_EVENT_TYPE_MANUAL,
};

struct os_event_data;
typedef struct os_event_data os_event_t;

EXPORT int os_event_init(os_event_t **event, enum os_event_type type);
EXPORT void os_event_destroy(os_event_t *event);
EXPORT int os_event_wait(os_event_t *event);
EXPORT int os_event_timedwait(os_event_t *event, unsigned long milliseconds);
EXPORT int os_event_try(os_event_t *event);
EXPORT int os_event_signal(os_event_t *event);
EXPORT void os_event_reset(os_event_t *event);

#ifdef __cplusplus
}
#endif
