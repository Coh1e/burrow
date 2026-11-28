#ifndef BRW_DIGBY_DOWNLOAD_IO_H
#define BRW_DIGBY_DOWNLOAD_IO_H

#include <stdbool.h>
#include <stddef.h>

/* The CLI serializes foreground operations. A successful begin installs the
   operator interrupt handler until end, including staging and publication. */
struct digby_download_control;
const char *digby_download_control_begin(struct digby_download_control **out);
bool digby_download_cancelled(const struct digby_download_control *control);
void digby_download_control_end(struct digby_download_control *control);

/* Bytes are borrowed for this call. A non-NULL static error stops the transfer. */
typedef const char *(*digby_download_write)(void *context, const unsigned char *bytes, size_t length);
const char *digby_download_transfer(struct digby_download_control *control, const char *url,
                                    digby_download_write write, void *context);

#endif
