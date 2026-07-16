/* Declarations for usblink.c */

#ifndef _H_USBLINK
#define _H_USBLINK

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool usblink_sending, usblink_connected;
extern int usblink_state;

#define CONSTANT  BSWAP16(0x54FD)
#define SRC_ADDR  BSWAP16(0x6400)
#define DST_ADDR  BSWAP16(0x6401)

struct packet {
    uint16_t constant;
    struct { uint16_t addr, service; } src;
    struct { uint16_t addr, service; } dst;
    uint16_t data_check;
    uint8_t data_size; // If 0xFF, bigdata* counts
    uint8_t ack;
    uint8_t seqno;
    uint8_t hdr_check;
    union {
        uint8_t      data[254];
        struct {
            uint32_t bigdatasize;
            uint8_t  bigdata[1440];
        };
        uint8_t      fulldata[1444];
    };
};

struct usblink_file {
    const char *filename;
    uint32_t size;
    bool is_dir;
};

/* f = NULL marks end of enumeration or error */
typedef void (*usblink_dirlist_cb)(struct usblink_file *f, bool is_error, void *user_data);
/* progress is 0-100 if successful or negative if an error occured.
   A value of 100 means complete and successful. */
typedef void (*usblink_progress_cb)(int progress, void *user_data);

void usblink_delete(const char *path, bool is_dir, usblink_progress_cb callback, void *user_data);
void usblink_dirlist(const char *path, usblink_dirlist_cb callback, void *user_data);
bool usblink_get_file(const char *path, const char *dest, usblink_progress_cb callback, void *user_data);
bool usblink_put_file(const char *filepath, const char *folder, usblink_progress_cb callback, void *user_data);
void usblink_new_dir(const char *path, usblink_progress_cb callback, void *user_data);
void usblink_move(const char *old_path, const char *new_path, usblink_progress_cb callback, void *user_data);
bool usblink_send_os(const char *filepath, usblink_progress_cb callback, void *user_data);

void usblink_received_packet(const uint8_t *data, uint32_t size);

void usblink_reset();
void usblink_connect();

#ifdef __cplusplus
}
#endif

#endif
