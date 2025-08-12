#ifndef YX5200_H
#define YX5200_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stddef.h> // size_t

// Library initialization: pass the UART the module is connected to
void yx5200_configure(UART_HandleTypeDef *huart, uint8_t feedback);

// ---- RX support (optional) ----

// YX5200 parsed frame (common 10-byte frame: 0x7E ... 0xEF)
typedef struct {
    uint8_t version;   // usually 0xFF
    uint8_t length;    // usually 0x06
    uint8_t cmd;
    uint8_t feedback;  // 0 or 1
    uint16_t param;     // DH:DL
    uint16_t checksum;  // as in protocol
} YX5200_Frame;

typedef enum {
    YX5200_RX_ERR_BAD_START = 1,  // not 0x7E
    YX5200_RX_ERR_BAD_END = 2,  // not 0xEF
    YX5200_RX_ERR_BAD_LENGTH = 3,  // length field not 0x06
    YX5200_RX_ERR_BAD_CSUM = 4   // checksum mismatch
} YX5200_RxError;

// Feed multiple bytes (e.g., DMA+IDLE chunk)
void yx5200_rx_process_bytes(const uint8_t *data, size_t len);

// Helper to start 1-byte IT reception on stored UART
// Call once after yx5200_configure if you want interrupt-driven RX
void yx5200_rx_start_it(void);

// Call from your HAL_UART_RxCpltCallback (or equivalent) to process a received byte and restart IT reception
void yx5200_rx_on_cplt(UART_HandleTypeDef *huart);

// Callbacks to handle frames and errors
void yx5200_on_frame(const YX5200_Frame *frame);

void yx5200_on_frame_error(YX5200_RxError error);

// Equalizer presets
typedef enum {
    YX5200_EQ_NORMAL = 0,
    YX5200_EQ_POP = 1,
    YX5200_EQ_ROCK = 2,
    YX5200_EQ_JAZZ = 3,
    YX5200_EQ_CLASSIC = 4,
    YX5200_EQ_BASS = 5
} YX5200_Equalizer;

// Play mode (repeat) options
typedef enum {
    YX5200_PM_ALL = 0, // Repeat all
    YX5200_PM_FOLDER = 1, // Repeat folder
    YX5200_PM_SINGLE = 2, // Repeat current track
    YX5200_PM_RANDOM = 3  // Random
} YX5200_PlayMode;

// Input source
typedef enum {
    YX5200_SRC_USB = 0,
    YX5200_SRC_SD = 1,
    YX5200_SRC_AUX = 2,
    YX5200_SRC_SLEEP = 3,
    YX5200_SRC_FLASH = 4
} YX5200_Source;

//Initialization
void yx5200_initialize(void);

// Playback control
void yx5200_next(void);

void yx5200_previous(void);

void yx5200_play_track(uint16_t index);             // Plays track by global index (0..2999)
void yx5200_volume_up(void);

void yx5200_volume_down(void);

void yx5200_set_volume(uint8_t volume);             // 0..30
void yx5200_set_equalizer(YX5200_Equalizer eq);

void yx5200_set_play_mode(YX5200_PlayMode mode);

void yx5200_set_source(YX5200_Source src);

void yx5200_mode_on(void);

void yx5200_mode_normal(void);

void yx5200_reset(void);

void yx5200_play(void);

void yx5200_pause(void);

void yx5200_play_folder_file(uint8_t folder, uint8_t file); // Plays selected folder and file (folder 01..99)
void yx5200_set_volume_gain(uint8_t enabled, uint8_t gain); // High=1 to enable, Low=gain 0..31
void yx5200_repeat_current(uint8_t enable);                  // 1 = repeat current, 0 = stop

// Queries (a device will reply if feedback is enabled in firmware/config)
void yx5200_query_is_usb(void);

void yx5200_query_is_sd(void);

void yx5200_query_is_flash(void);

void yx5200_response(void);              // Purpose depends on module firmware

void yx5200_query_status(void);

void yx5200_query_volume(void);

void yx5200_query_equalizer(void);

void yx5200_query_play_mode(void);

void yx5200_query_software(void);

void yx5200_query_sd_files(void);

void yx5200_query_usb_files(void);

void yx5200_query_flash_files(void);

void yx5200_query_on(void);              // Purpose depends on module firmware
void yx5200_query_sd_current(void);

void yx5200_query_usb_current(void);

void yx5200_query_flash_current(void);

#ifdef __cplusplus
}
#endif

#endif // YX5200_H