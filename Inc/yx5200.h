#ifndef YX5200_H
#define YX5200_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stddef.h> // size_t


//Media online status
typedef enum {
    YX5200_MEDIA_UNKNOWN = 0,
    YX5200_MEDIA_USB = 1,
    YX5200_MEDIA_SD = 2,
    YX5200_MEDIA_PC = 4,
} yx5200_media_t;

// Equalizer presets
typedef enum {
    YX5200_EQ_NORMAL = 0,
    YX5200_EQ_POP = 1,
    YX5200_EQ_ROCK = 2,
    YX5200_EQ_JAZZ = 3,
    YX5200_EQ_CLASSIC = 4,
    YX5200_EQ_BASS = 5,
} yx5200_eq_t;

// Error codes
typedef enum {
    YX5200_OK = 0x00, // Ok!
    YX5200_ERR_MODULE_BUSY = 0x01, // Module busy (initialization not finished)
    YX5200_ERR_SLEEP_MODE = 0x02, // Currently in sleep mode
    YX5200_ERR_SERIAL_RX = 0x03, // Serial receiving error (frame not fully received)
    YX5200_ERR_CHECKSUM = 0x04, // Checksum incorrect
    YX5200_ERR_TRACK_OUT_OF_RANGE = 0x05, // Specified track out of range
    YX5200_ERR_TRACK_NOT_FOUND = 0x06, // Specified track not found
    YX5200_ERR_INSERTION = 0x07, // Insertion error (allowed only while playing)
    YX5200_ERR_SD_READ_FAIL = 0x08, // SD card read failed (removed or damaged)
    YX5200_ERR_ENTER_SLEEP = 0x0A,  // Entered sleep mode
    YX5200_ERR_TIMEOUT = 0x0B, // Timeout
    YX5200_ERR_BAD_CONFIGURATION = 0x0C, // UART is not configured. Call yx5200_setup() first
    YX5200_ERR_RX_BAD_START,  // not 0x7E
    YX5200_ERR_RX_BAD_END,  // not 0xEF
    YX5200_ERR_RX_BAD_LENGTH,  // length field not 0x06
    YX5200_ERR_RX_BAD_CSUM,   // checksum mismatch
    YX5200_ERR_TX,   // Transmission error
    YX5200_ERR_BAD_PARAM,   // Bad param
} yx5200_error_t;

typedef struct {
    yx5200_error_t error;
    uint16_t value;
} yx5200_response_t;

typedef enum {
    STATUS_STOPPED = 0x0,
    STATUS_PLAYING = 0x1,
    STATUS_PAUSED = 0x2
} yx5200_player_status;

typedef struct {
    yx5200_error_t error;
    yx5200_media_t data;
} yx5200_media_response_t;

typedef struct {
    yx5200_error_t error;
    yx5200_media_t media;
    yx5200_player_status player;
} yx5200_status_response_t;

typedef struct {
    yx5200_error_t error;
    yx5200_eq_t equalizer;
} yx5200_eq_response_t;


// Library initialization: pass the UART the module is connected to
void yx5200_setup(UART_HandleTypeDef *huart, uint8_t feedback);

// Feed multiple bytes (e.g., DMA+IDLE chunk)
void yx5200_rx_process_bytes(const uint8_t *data, size_t len);

// Helper to start 1-byte IT reception on stored UART
// Call once after yx5200_setup if you want interrupt-driven RX
void yx5200_rx_start_it(void);

// Call from your HAL_UART_RxCpltCallback (or equivalent) to process a received byte and restart IT reception
void yx5200_rx_on_cplt(UART_HandleTypeDef *huart);

// Called when the media is attached. Beware: this is called from an interrupt context.
void __attribute__((weak)) yx5200_on_media_attached_callback(yx5200_media_t media);

// Called when the media is detached. Beware: this is called from an interrupt context.
void __attribute__((weak)) yx5200_on_media_detached_callback(yx5200_media_t media);

// Called when the track finishes playing. Beware: this is called from an interrupt context.
void __attribute__((weak)) yx5200_on_track_finished_callback(yx5200_media_t media, uint16_t index);

// Core send helpers
yx5200_error_t yx5200_set_source(yx5200_media_t src);

yx5200_error_t yx5200_set_volume(uint8_t vol);                     // 0..30

yx5200_error_t yx5200_volume_up(void);

yx5200_error_t yx5200_volume_down(void);

yx5200_error_t yx5200_set_equalizer(yx5200_eq_t eq);

yx5200_error_t yx5200_play(void);

yx5200_error_t yx5200_pause(void);

yx5200_error_t yx5200_stop(void);

yx5200_error_t yx5200_next(void);

yx5200_error_t yx5200_previous(void);

yx5200_error_t yx5200_reset(void);

yx5200_error_t yx5200_play_track(uint16_t globalIndex);            // 1..2999
yx5200_error_t yx5200_play_folder_file(uint8_t folder, uint8_t file); // folder 1..99, file 1..255
yx5200_error_t yx5200_play_big_folder_file(uint8_t folder, uint16_t file); // folder 1..16, file 1..3000
yx5200_error_t yx5200_play_mp3_folder(uint16_t index);             // 1..3000 ("MP3" folder)

//Loop All Tracks in Root Directory
yx5200_error_t yx5200_set_all_loop(uint8_t on);                    // 0x11: 1=on, 0=off
//Set Current Track for Loop Playback
yx5200_error_t yx5200_set_single_loop(uint8_t on);                 // 0x19: loop current track
//Start Loop Playback of Specified Folder
yx5200_error_t yx5200_play_folder_loop(uint8_t folder);            // 0x17: loop folder 1..99
//Single Track Loop Play Command
yx5200_error_t yx5200_loop_track(uint16_t globalIndex);            // 0x08: loop specific track
//Random Playback of Device Files
yx5200_error_t yx5200_play_random(void);                           // 0x18

// Advertisement
// Insert Advertisement from ADVERT folder
yx5200_error_t yx5200_advert_play(uint8_t index);                  // 0x13: 1..255
// Insert Advertisement from ADVERT{folder} folder
yx5200_error_t yx5200_advert_play_folder(uint8_t, uint8_t); // 0x25: 1..255
yx5200_error_t yx5200_advert_stop(void);                           // 0x15

// DAC
yx5200_error_t yx5200_set_dac_config(uint8_t on);

// Queries (commands with feedback)

//Query available media
yx5200_media_response_t yx5200_query_media_online(void);

yx5200_status_response_t yx5200_query_status(void);                          // 0x42
//Query current volume
yx5200_response_t yx5200_query_volume(void);                          // 0x43
//Query current equalizer setting
yx5200_eq_response_t yx5200_query_eq(void);                     // 0x44
//Query the total number of files on the U disk
yx5200_response_t yx5200_query_total_usb(void);                       // 0x47
//Query the total number of files on the SD card
yx5200_response_t yx5200_query_total_sd(void);                        // 0x48
//Currently playing or finished playing is a physical sequence
yx5200_response_t yx5200_query_current_usb(void);                     // 0x4B
//Currently playing or finished playing is a physical sequence
yx5200_response_t yx5200_query_current_sd(void);                      // 0x4C

#ifdef __cplusplus
}
#endif

#endif // YX5200_H