// Copies the ESP log output into a RAM ring buffer that the web server
// can serve, so the device can be diagnosed without a serial cable.

#ifndef _WEBLOG_H_
#define _WEBLOG_H_

#include <stddef.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once, early at startup. Keeps the UART output as it is. */
void weblog_init(size_t ring_size);

/* Writes a numbered marker line into the log. */
void weblog_mark(void);

/* Sends the buffered log as text/plain. */
esp_err_t weblog_send(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif /* _WEBLOG_H_ */
