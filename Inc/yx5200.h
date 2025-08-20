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
    YX5200_MEDIA_USB = 1,
    YX5200_MEDIA_SD = 2,
    YX5200_MEDIA_FLASH = 3,
    YX5200_MEDIA_PC = 4,
} YX5200_Media;

// Feed multiple bytes (e.g., DMA+IDLE chunk)
void yx5200_rx_process_bytes(const uint8_t *data, size_t len);

// Helper to start 1-byte IT reception on stored UART
// Call once after yx5200_configure if you want interrupt-driven RX
void yx5200_rx_start_it(void);

// Call from your HAL_UART_RxCpltCallback (or equivalent) to process a received byte and restart IT reception
void yx5200_rx_on_cplt(UART_HandleTypeDef *huart);

// Called when the media is attached. Beware: this is called from an interrupt context.
void __attribute__((weak)) yx5200_on_media_attached_callback(YX5200_Media media);

// Called when the media is detached. Beware: this is called from an interrupt context.
void __attribute__((weak)) yx5200_on_media_detached_callback(YX5200_Media media);

// Called when the track finishes playing. Beware: this is called from an interrupt context.
void __attribute__((weak)) yx5200_on_track_finished_callback(YX5200_Media media, uint16_t index);

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
    YX5200_SRC_AUX = 2, //I have no idea what is this
    YX5200_SRC_SLEEP = 3, //Same here
    YX5200_SRC_FLASH = 4
} YX5200_Source;

// Response codes
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
    YX5200_ERR_BAD_CONFIGURATION = 0x0C, // UART is not configured. Call yx5200_configure() first
    YX5200_RX_ERR_BAD_START,  // not 0x7E
    YX5200_RX_ERR_BAD_END,  // not 0xEF
    YX5200_RX_ERR_BAD_LENGTH,  // length field not 0x06
    YX5200_RX_ERR_BAD_CSUM,   // checksum mismatch
    YX5200_ERR_TX,   // Transmission error
} YX5200_Response;

typedef enum {
    STATUS_USB = 0x01, // USB flash drive
    STATUS_SD = 0x02, //SD card
    STATUS_SLEEP = 0x10 //Module in sleep mode
} MSBStatus;

typedef enum {
    STATUS_STOPPED = 0x0,
    STATUS_PLAYING = 0x1,
    STATUS_PAUSED = 0x2
} LSBStatus;

typedef struct {
    volatile LSBStatus playerStatus;  // Second status byte
    volatile MSBStatus mediaSource;  // First status byte
} DeviceStatus;

//Initialization (query media status)
YX5200_Response yx5200_initialize(void);

// Playback control
YX5200_Response yx5200_next(void);
YX5200_Response yx5200_previous(void);
YX5200_Response yx5200_play_track(uint16_t index); // Plays track by global index (0..2999).
YX5200_Response yx5200_play_track_in_loop(uint16_t index); // Plays track by global index (0..2999). Track will be played again and again until it receives a command for stop or pause
YX5200_Response yx5200_play_folder_file(uint8_t folder, uint8_t file); // Plays selected folder and file (folder 01..99)
// Play/pause control.
YX5200_Response yx5200_pause(void);
YX5200_Response yx5200_play(void);
// Play all the tracks in the device ceaselessly again and again until it receives a command for stop or pause
// 0 argument may stop Behavior.
YX5200_Response yx5200_play_all(uint8_t enable);
YX5200_Response yx5200_volume_up(void);
YX5200_Response yx5200_volume_down(void);
YX5200_Response yx5200_set_volume(uint8_t volume);             // 0..30
YX5200_Response yx5200_set_equalizer(YX5200_Equalizer eq);
YX5200_Response yx5200_set_play_mode(YX5200_PlayMode mode); //TODO: ?
YX5200_Response yx5200_set_source(YX5200_Source src);
// Sleep/wakeup control. Maybe not working.
YX5200_Response yx5200_sleep(void); // Not working on my yx5200-24ss. After this command module responds with YX5200_ERR_MODULE_BUSY
YX5200_Response yx5200_wakeup(void); // After sleep not working either.
// Reset device.
YX5200_Response yx5200_reset(void);
// Maybe not working.
YX5200_Response yx5200_set_volume_gain(uint8_t enabled, uint8_t gain); // High=1 to enable, Low=gain 0..31

// Media status. To re-query status, call yx5200_initialize()
// Determine if USB online
uint8_t yx5200_is_usb_online(void);

// Determine if SD online
uint8_t yx5200_is_sd_online(void);

// Determine if PC online
uint8_t yx5200_is_pc_online(void);

// Queries (a device will reply if feedback is enabled in firmware/config)

YX5200_Response yx5200_query_status(void);

DeviceStatus yx5200_get_status(void);

YX5200_Response yx5200_query_volume(void);

YX5200_Response yx5200_query_equalizer(void);

YX5200_Response yx5200_query_play_mode(void);

YX5200_Response yx5200_query_software(void);

YX5200_Response yx5200_query_sd_files(void);

YX5200_Response yx5200_query_usb_files(void);

YX5200_Response yx5200_query_flash_files(void);
YX5200_Response yx5200_query_on(void);              // Purpose depends on module firmware
YX5200_Response yx5200_query_sd_current(void);
YX5200_Response yx5200_query_usb_current(void);
YX5200_Response yx5200_query_flash_current(void);
YX5200_Response yx5200_query_folders_count(void);
YX5200_Response yx5200_query_folder_files_count(uint16_t);

#ifdef __cplusplus
}
#endif

#endif // YX5200_H