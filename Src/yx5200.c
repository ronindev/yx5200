#include "yx5200.h"

// Local pointer to UART provided by the user during initialization
static UART_HandleTypeDef *s_uart = NULL;

// Feedback flag to request device replies (0 = no feedback, 1 = feedback)
static uint8_t s_feedback = 0;

typedef enum {
    RX_WAIT_START = 0,
    RX_COLLECT,
} RxState;
static volatile RxState s_rx_state = RX_WAIT_START;
static uint8_t s_rx_buf[10];
typedef enum {
    QS_WAITING = 0,
    QS_ACK,
    QS_ERROR,
    QS_TIMEOUT,
} QueryState;
static volatile uint8_t s_rx_pos = 0;
static volatile uint8_t query_state = QS_WAITING;
static volatile uint8_t s_error = 0;
// 1-byte buffer for HAL IT reception
static uint8_t s_rx_byte_it = 0;

//Media online status
typedef enum {
    USB_ONLINE = 1,
    SD_CARD_ONLINE = 2,
    USB_AND_SD_CARD_ONLINE = 3,
    PC_ONLINE = 4,
    NO_MEDIA = 5
} MediaState;
static volatile MediaState s_media_state = 0;

static DeviceStatus s_device_status;

static void processDevicePlugIn(const YX5200_Frame *frame);

static void processDevicePullOff(const YX5200_Frame *frame);

// Frame constants for YX5200 serial protocol
static const uint8_t YX5200_FRAME_START = 0x7E;
static const uint8_t YX5200_FRAME_END = 0xEF;
static const uint8_t YX5200_PROTOCOL_VER = 0xFF;
static const uint8_t YX5200_FRAME_LENGTH = 0x06;

// Command codes
typedef enum {
    YX5200_CMD_NEXT = 0x01,
    YX5200_CMD_PREVIOUS = 0x02,
    YX5200_CMD_PLAY_INDEX = 0x03, // 0..2999
    YX5200_CMD_VOLUME_UP = 0x04,
    YX5200_CMD_VOLUME_DOWN = 0x05,
    YX5200_CMD_SET_VOLUME = 0x06, // 0..30
    YX5200_CMD_SET_EQUALIZER = 0x07,
    YX5200_CMD_SET_PLAY_MODE = 0x08,
    YX5200_CMD_SET_SOURCE = 0x09,
    YX5200_CMD_SLEEP = 0x0A,
    YX5200_CMD_WAKEUP = 0x0B,
    YX5200_CMD_RESET = 0x0C,
    YX5200_CMD_PLAY = 0x0D,
    YX5200_CMD_PAUSE = 0x0E,
    YX5200_CMD_PLAY_FOLDER_FILE = 0x0F, // DH=folder (01~99), DL=track
    YX5200_CMD_SET_VOLUME_GAIN = 0x10, // High byte = 1 to enable preset volume, Low byte = gain 0..31
    YX5200_CMD_PLAY_ALL = 0x11  // 1 = repeat current track, 0 = stop
} YX5200_Command;

// Query codes
typedef enum {
    YX5200_Q_DEVICE_PLUGGED_IN = 0x3A,
    YX5200_Q_DEVICE_PULLED_OUT = 0x3B,
    YX5200_Q_MEDIA_STATE = 0x3F,
    YX5200_Q_ERROR = 0x40,
    YX5200_Q_FEEDBACK = 0x41,
    YX5200_Q_STATUS = 0x42,
    YX5200_Q_VOLUME = 0x43,
    YX5200_Q_EQUALIZER = 0x44,
    YX5200_Q_PLAY_MODE = 0x45,
    YX5200_Q_SOFTWARE = 0x46,
    YX5200_Q_SD_FILES = 0x47,
    YX5200_Q_USB_FILES = 0x48,
    YX5200_Q_FLASH_FILES = 0x49,
    YX5200_Q_ON = 0x4A,
    YX5200_Q_SD_CURRENT = 0x4B,
    YX5200_Q_USB_CURRENT = 0x4C,
    YX5200_Q_FLASH_CURRENT = 0x4D
} YX5200_Query;

// Byte helpers
static inline uint8_t lo8(uint16_t v) { return (uint8_t) (v & 0xFF); }

static inline uint8_t hi8(uint16_t v) { return (uint8_t) ((v >> 8) & 0xFF); }

// ---- Public: init ----

void yx5200_configure(UART_HandleTypeDef *huart, uint8_t feedback) {
  s_uart = huart;
  s_feedback = feedback ? 1u : 0u;
}

// ---- Protocol helpers ----

// Compute protocol checksum: negative sum of version + length + command + feedback + paramH + paramL
static inline uint16_t yx5200_compute_checksum(uint8_t cmd, uint16_t param, uint8_t feedback_byte) {
  uint16_t sum = (uint16_t) (YX5200_PROTOCOL_VER + YX5200_FRAME_LENGTH + cmd + feedback_byte + hi8(param) + lo8(param));
  return (uint16_t) (0 - sum);
}

// Send one command frame (blocking). Adds a small delay after transmitting for safety.
static YX5200_Response yx5200_send_command(uint8_t cmd, uint16_t param) {
  if (s_uart == NULL) {
    return YX5200_ERR_BAD_CONFIGURATION; // UART is not initialized
  }

  const uint8_t fb = s_feedback;
  const uint16_t checksum = yx5200_compute_checksum(cmd, param, fb);

  uint8_t frame[10];
  frame[0] = YX5200_FRAME_START;
  frame[1] = YX5200_PROTOCOL_VER;
  frame[2] = YX5200_FRAME_LENGTH;
  frame[3] = cmd;
  frame[4] = fb;
  frame[5] = hi8(param);
  frame[6] = lo8(param);
  frame[7] = hi8(checksum);
  frame[8] = lo8(checksum);
  frame[9] = YX5200_FRAME_END;

  HAL_UART_Transmit(s_uart, frame, sizeof(frame), HAL_MAX_DELAY);
  query_state = QS_WAITING;
  if (s_feedback) {
    uint32_t startTime = HAL_GetTick();
    while (query_state != QS_ACK && query_state != QS_ERROR) {
      if (HAL_GetTick() - startTime > 100) {
        query_state = QS_TIMEOUT;
        return YX5200_ERR_TIMEOUT;
      }
    }
    if (query_state == QS_ERROR) {
      return s_error;
    }
    HAL_Delay(20); // If no delay is added, the device may not respond to the next command
  } else {
    HAL_Delay(100);
  }
  return YX5200_OK;
}

// ---- RX parser (state machine) ----

// Reset internal RX parser state
static void yx5200_rx_reset_parser(void) {
  s_rx_state = RX_WAIT_START;
  s_rx_pos = 0;
}

static void yx5200_parser_push(uint8_t b) {
  switch (s_rx_state) {
    case RX_WAIT_START:
      if (b == YX5200_FRAME_START) {
        s_rx_buf[0] = b;
        s_rx_pos = 1;
        s_rx_state = RX_COLLECT;
      } else {
        yx5200_on_frame_error(YX5200_RX_ERR_BAD_START);
      }
      break;

    case RX_COLLECT:
      s_rx_buf[s_rx_pos++] = b;

      if (s_rx_pos >= sizeof(s_rx_buf)) {
        // We have 10 bytes: verify end, fields, checksum
        const uint8_t *f = s_rx_buf;

        if (f[9] != YX5200_FRAME_END) {
          yx5200_on_frame_error(YX5200_RX_ERR_BAD_END);
          yx5200_rx_reset_parser();
          return;
        }
        if (f[1] != YX5200_PROTOCOL_VER || f[2] != YX5200_FRAME_LENGTH) {
          yx5200_on_frame_error((f[2] != YX5200_FRAME_LENGTH) ? YX5200_RX_ERR_BAD_LENGTH : YX5200_RX_ERR_BAD_CSUM);
          yx5200_rx_reset_parser();
          return;
        }

        const uint8_t cmd = f[3];
        const uint8_t fb = f[4];
        const uint16_t param = (uint16_t) ((f[5] << 8) | f[6]);
        const uint16_t csum = (uint16_t) ((f[7] << 8) | f[8]);
        const uint16_t calc = yx5200_compute_checksum(cmd, param, fb);

        if (csum != calc) {
          yx5200_on_frame_error(YX5200_RX_ERR_BAD_CSUM);
          yx5200_rx_reset_parser();
          return;
        }

        YX5200_Frame out = {
            .version  = f[1],
            .length   = f[2],
            .cmd      = cmd,
            .feedback = fb,
            .param    = param,
            .checksum = csum
        };
        yx5200_on_frame(&out);
        yx5200_rx_reset_parser();
      }
      break;
  }
}

// Feed single byte into the parser (call from your RX path)
static void yx5200_rx_process_byte(uint8_t byte) {
  // If new start appears mid-frame, resync
  if (s_rx_state == RX_COLLECT && byte == YX5200_FRAME_START) {
    yx5200_rx_reset_parser();
    s_rx_buf[0] = byte;
    s_rx_pos = 1;
    s_rx_state = RX_COLLECT;
    return;
  }
  if (s_rx_state == RX_WAIT_START && byte != YX5200_FRAME_START) {
    // ignore noise until start; optionally report BAD_START
    return;
  }
  yx5200_parser_push(byte);
}

void yx5200_rx_process_bytes(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    yx5200_rx_process_byte(data[i]);
  }
}

void yx5200_rx_start_it(void) {
  if (s_uart) {
    (void) HAL_UART_Receive_IT(s_uart, &s_rx_byte_it, 1);
    yx5200_rx_reset_parser();
  }
}

void yx5200_rx_on_cplt(UART_HandleTypeDef *huart) {
  if (huart != s_uart) {
    return;
  }
  yx5200_rx_process_byte(s_rx_byte_it);
  (void) HAL_UART_Receive_IT(s_uart, &s_rx_byte_it, 1);
}

// Weak callbacks (user can override them)
void yx5200_on_frame(const YX5200_Frame *frame) {
  if (frame->cmd == YX5200_Q_FEEDBACK) {
    //ACK
    query_state = QS_ACK;
  } else if (frame->cmd == YX5200_Q_ERROR) {
    query_state = QS_ERROR;
    s_error = frame->param;
    return;
  } else if (frame->cmd == YX5200_Q_DEVICE_PLUGGED_IN) {
    processDevicePlugIn(frame);
    return;
  } else if (frame->cmd == YX5200_Q_DEVICE_PULLED_OUT) {
    processDevicePullOff(frame);
    return;
  } else if (frame->cmd == YX5200_Q_MEDIA_STATE) {
    s_media_state = frame->param;
    query_state = QS_ACK;
    return;
  } else if (frame->cmd == YX5200_Q_STATUS) {
    s_device_status.mediaSource = hi8(frame->param);
    s_device_status.playerStatus = lo8(frame->param);
    query_state = QS_ACK;
    return;
  }
}

static void processDevicePlugIn(const YX5200_Frame *frame) {
  switch (frame->param) {
    case 0x1:
      //USB
      if (yx5200_is_sd_online()) {
        s_media_state = USB_AND_SD_CARD_ONLINE;
      } else {
        s_media_state = USB_ONLINE;
      }
      break;
    case 0x2:
      //SD card
      if (yx5200_is_usb_online()) {
        s_media_state = USB_AND_SD_CARD_ONLINE;
      } else {
        s_media_state = SD_CARD_ONLINE;
      }
      break;
    case 0x3:
      //PC
      s_media_state = PC_ONLINE;
      break;
  }
}

static void processDevicePullOff(const YX5200_Frame *frame) {
  switch (frame->param) {
    case 0x1:
      //USB
      if (yx5200_is_sd_online()) {
        s_media_state = SD_CARD_ONLINE;
      } else {
        s_media_state = NO_MEDIA;
      }
      break;
    case 0x2:
      //SD card
      if (yx5200_is_usb_online()) {
        s_media_state = USB_ONLINE;
      } else {
        s_media_state = NO_MEDIA;
      }
      break;
    case 0x3:
      //PC TODO: check if this is correct (maybe re-query state?)
      s_media_state = NO_MEDIA;
      break;
  }
}

void yx5200_on_frame_error(YX5200_RxError error) {
  //TODO: add weak callback to handle errors
  query_state = QS_ERROR;
}

// ---- High-level API ----

YX5200_Response yx5200_initialize(void) {
  YX5200_Response response = yx5200_send_command(YX5200_Q_MEDIA_STATE, 0);
  if (response == YX5200_ERR_TIMEOUT) {
    s_media_state = NO_MEDIA;
  }
  return response;
}

YX5200_Response yx5200_next(void) {
  return yx5200_send_command(YX5200_CMD_NEXT, 0);
}

YX5200_Response yx5200_previous(void) {
  return yx5200_send_command(YX5200_CMD_PREVIOUS, 0);
}

YX5200_Response yx5200_play_track(uint16_t index) {
  // Plays track by global index (0..2999)
  return yx5200_send_command(YX5200_CMD_PLAY_INDEX, index);
}

YX5200_Response yx5200_volume_up(void) {
  return yx5200_send_command(YX5200_CMD_VOLUME_UP, 0);
}

YX5200_Response yx5200_volume_down(void) {
  return yx5200_send_command(YX5200_CMD_VOLUME_DOWN, 0);
}

YX5200_Response yx5200_set_volume(uint8_t volume) {
  // volume: 0..30
  return yx5200_send_command(YX5200_CMD_SET_VOLUME, volume);
}

YX5200_Response yx5200_set_equalizer(YX5200_Equalizer eq) {
  return yx5200_send_command(YX5200_CMD_SET_EQUALIZER, (uint16_t) eq);
}

YX5200_Response yx5200_set_play_mode(YX5200_PlayMode mode) {
  return yx5200_send_command(YX5200_CMD_SET_PLAY_MODE, (uint16_t) mode);
}

YX5200_Response yx5200_set_source(YX5200_Source src) {
  return yx5200_send_command(YX5200_CMD_SET_SOURCE, (uint16_t) src);
}

YX5200_Response yx5200_sleep(void) {
  return yx5200_send_command(YX5200_CMD_SLEEP, 0);
}

YX5200_Response yx5200_wakeup(void) {
  return yx5200_send_command(YX5200_CMD_WAKEUP, 0);
}

YX5200_Response yx5200_reset(void) {
  return yx5200_send_command(YX5200_CMD_RESET, 0);
}

YX5200_Response yx5200_play(void) {
  return yx5200_send_command(YX5200_CMD_PLAY, 0);
}

YX5200_Response yx5200_pause(void) {
  return yx5200_send_command(YX5200_CMD_PAUSE, 0);
}

YX5200_Response yx5200_play_folder_file(uint8_t folder, uint8_t file) {
  // Plays a file within a folder: param = (folder << 8) | file, folder 01..99
  uint16_t param = ((uint16_t) folder << 8) | file;
  return yx5200_send_command(YX5200_CMD_PLAY_FOLDER_FILE, param);
}

YX5200_Response yx5200_set_volume_gain(uint8_t enabled, uint8_t gain) {
  // High byte = 1 to apply preset volume, Low byte = gain (0..31)
  uint16_t param = ((uint16_t) enabled << 8) | gain;
  return yx5200_send_command(YX5200_CMD_SET_VOLUME_GAIN, param);
}

YX5200_Response yx5200_play_all(uint8_t enable) {
  // 1 = play all tracks, 0 = stop playing all tracks (no playing after current track finished)
  return yx5200_send_command(YX5200_CMD_PLAY_ALL, enable);
}

uint8_t yx5200_is_usb_online(void) {
  return s_media_state == USB_AND_SD_CARD_ONLINE || s_media_state == USB_ONLINE;
}

uint8_t yx5200_is_sd_online(void) {
  return s_media_state == USB_AND_SD_CARD_ONLINE || s_media_state == SD_CARD_ONLINE;
}

uint8_t yx5200_is_pc_online(void) {
  return s_media_state == PC_ONLINE;
}

// ---- Queries ----

YX5200_Response yx5200_query_status(void) {
  return yx5200_send_command(YX5200_Q_STATUS, 0);
}

DeviceStatus yx5200_get_status(void) {
  return s_device_status;
}

YX5200_Response yx5200_query_volume(void) {
  return yx5200_send_command(YX5200_Q_VOLUME, 0);
}

YX5200_Response yx5200_query_equalizer(void) {
  return yx5200_send_command(YX5200_Q_EQUALIZER, 0);
}

YX5200_Response yx5200_query_play_mode(void) {
  return yx5200_send_command(YX5200_Q_PLAY_MODE, 0);
}

YX5200_Response yx5200_query_software(void) {
  return yx5200_send_command(YX5200_Q_SOFTWARE, 0);
}

YX5200_Response yx5200_query_sd_files(void) {
  return yx5200_send_command(YX5200_Q_SD_FILES, 0);
}

YX5200_Response yx5200_query_usb_files(void) {
  return yx5200_send_command(YX5200_Q_USB_FILES, 0);
}

YX5200_Response yx5200_query_flash_files(void) {
  return yx5200_send_command(YX5200_Q_FLASH_FILES, 0);
}

YX5200_Response yx5200_query_on(void) {
  return yx5200_send_command(YX5200_Q_ON, 0);
}

YX5200_Response yx5200_query_sd_current(void) {
  return yx5200_send_command(YX5200_Q_SD_CURRENT, 0);
}

YX5200_Response yx5200_query_usb_current(void) {
  return yx5200_send_command(YX5200_Q_USB_CURRENT, 0);
}

YX5200_Response yx5200_query_flash_current(void) {
  return yx5200_send_command(YX5200_Q_FLASH_CURRENT, 0);
}
