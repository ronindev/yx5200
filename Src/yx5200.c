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
static volatile QueryState s_query_state = QS_WAITING;
static volatile yx5200_error_t s_error = 0;
// 1-byte buffer for HAL IT reception
static uint8_t s_rx_byte_it = 0;

// YX5200 parsed frame (common 10-byte frame: 0x7E ... 0xEF)
typedef struct {
    uint8_t version;   // usually 0xFF
    uint8_t length;    // usually 0x06
    uint8_t cmd;
    uint8_t feedback;  // 0 or 1
    uint16_t param;     // DH:DL
    uint16_t checksum;  // as in protocol
} yx5200_frame_t;

static volatile uint16_t s_last_response = 0;

static void processDevicePlugIn(const yx5200_frame_t *frame);

static void processDevicePullOff(const yx5200_frame_t *frame);

static void yx5200_on_frame(const yx5200_frame_t *frame);

// Frame constants for YX5200 serial protocol
static const uint8_t YX5200_FRAME_START = 0x7E;
static const uint8_t YX5200_FRAME_END = 0xEF;
static const uint8_t YX5200_PROTOCOL_VER = 0xFF;
static const uint8_t YX5200_FRAME_LENGTH = 0x06;

// Command codes
typedef enum {
    YX5200_CMD_NEXT = 0x01,
    YX5200_CMD_PREVIOUS = 0x02,
    YX5200_CMD_PLAY_TRACK = 0x03, // 1..2999
    YX5200_CMD_VOLUME_UP = 0x04,
    YX5200_CMD_VOLUME_DOWN = 0x05,
    YX5200_CMD_SET_VOLUME = 0x06, // 0..30
    YX5200_CMD_SET_EQUALIZER = 0x07,
    YX5200_CMD_LOOP_TRACK = 0x08,
    YX5200_CMD_SET_SOURCE = 0x09,
    YX5200_CMD_SLEEP = 0x0A,
    YX5200_CMD_WAKEUP = 0x0B,
    YX5200_CMD_RESET = 0x0C,
    YX5200_CMD_PLAY = 0x0D,
    YX5200_CMD_PAUSE = 0x0E,
    YX5200_CMD_PLAY_FOLDER_FILE = 0x0F, // DH=folder (01~99), DL=track
    YX5200_CMD_SET_AMPLIFIER_GAIN = 0x10, // High byte = 1 to enable preset volume, Low byte = gain 0..31
    YX5200_CMD_SET_LOOP_PLAY_MODE = 0x11,  // 1 = repeat tracks in the root directory of a storage device, 0 = stop
    YX5200_CMD_PLAY_MP3_FOLDER = 0x12,
    YX5200_CMD_INSERT_AD = 0x13, //Insert an advertisement (from folder "ADVERT") while playing a track
    YX5200_CMD_PLAY_BIG_FOLDER = 0x14,
    YX5200_CMD_STOP_AD = 0x15, //Stop playing inserted ad
    YX5200_CMD_STOP = 0x16, //Stop playing track
    YX5200_CMD_PLAY_FOLDER_LOOP = 0x17, //Specify repeat playback of a folder
    YX5200_CMD_PLAY_RANDOM = 0x18,
    YX5200_CMD_LOOP = 0x19,
    YX5200_CMD_DAC = 0x1A,
    YX5200_CMD_COMBINATION_PLAYBACK = 0x21, // TODO: Combination playback
    YX5200_CMD_PLAY_WITH_VOLUME = 0x22, // TODO: Play with volume
    YX5200_CMD_INSERT_AD_FROM_FOLDER = 0x25,
} YX5200_Command;

// Query codes
typedef enum {
    YX5200_Q_MEDIA_STATE = 0x3F,
    YX5200_Q_STATUS = 0x42,
    YX5200_Q_VOLUME = 0x43,
    YX5200_Q_EQUALIZER = 0x44,
    YX5200_Q_PLAYBACK_MODE = 0x45,
    YX5200_Q_FIRMWARE_VER = 0x46,
    YX5200_Q_USB_FILES = 0x47,
    YX5200_Q_SD_FILES = 0x48,
    YX5200_Q_FLASH_FILES = 0x49,
    YX5200_Q_SD_CURRENT = 0x4B,
    YX5200_Q_USB_CURRENT = 0x4C,
    YX5200_Q_FLASH_CURRENT = 0x4D,
    YX5200_Q_FOLDER_FILES_COUNT = 0x4E,
    YX5200_Q_TOTAL_FILES_COUNT = 0x4F,
} YX5200_Query;

// Event codes
typedef enum {
    YX5200_EVENT_DEVICE_PLUGGED_IN = 0x3A,
    YX5200_EVENT_DEVICE_PULLED_OUT = 0x3B,
    YX5200_EVENT_USB_TRACK_FINISHED = 0x3C,
    YX5200_EVENT_SD_TRACK_FINISHED = 0x3D,
    YX5200_EVENT_FLASH_TRACK_FINISHED = 0x3E,
    YX5200_RESP_MEDIA_STATE = 0x3F,
    YX5200_RESP_ERROR = 0x40,
    YX5200_RESP_FEEDBACK = 0x41,
    YX5200_RESP_STATUS = 0x42,
    YX5200_RESP_VOLUME = 0x43,
    YX5200_RESP_EQUALIZER = 0x44,
    YX5200_RESP_SD_FILES = 0x47,
    YX5200_RESP_USB_FILES = 0x48,
    YX5200_RESP_FLASH_FILES = 0x49,
    YX5200_RESP_ON = 0x4A,
    YX5200_RESP_SD_CURRENT = 0x4B,
    YX5200_RESP_USB_CURRENT = 0x4C,
    YX5200_RESP_FLASH_CURRENT = 0x4D
} yx5200_response_code_t;

// Byte helpers
static inline uint8_t lo8(uint16_t v) { return (uint8_t) (v & 0xFF); }

static inline uint8_t hi8(uint16_t v) { return (uint8_t) ((v >> 8) & 0xFF); }

// ---- Public: init ----

void yx5200_setup(UART_HandleTypeDef *huart, uint8_t feedback) {
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
static yx5200_error_t yx5200_send_command(uint8_t cmd, uint16_t param) {
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

  s_query_state = QS_WAITING;
  if (HAL_UART_Transmit(s_uart, frame, sizeof(frame), 100) != HAL_OK) {
    s_query_state = QS_ERROR;
    return YX5200_ERR_TX;
  }
  if (s_feedback) {
    uint32_t startTime = HAL_GetTick();
    while (s_query_state != QS_ACK && s_query_state != QS_ERROR) {
      if (HAL_GetTick() - startTime > 100) {
        s_query_state = QS_TIMEOUT;
        return YX5200_ERR_TIMEOUT;
      }
      HAL_Delay(1);
    }
    if (s_query_state == QS_ERROR) {
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
        s_error = YX5200_ERR_RX_BAD_START;
        s_query_state = QS_ERROR;
      }
      break;

    case RX_COLLECT:
      s_rx_buf[s_rx_pos++] = b;

      if (s_rx_pos >= sizeof(s_rx_buf)) {
        // We have 10 bytes: verify end, fields, checksum
        const uint8_t *f = s_rx_buf;

        if (f[9] != YX5200_FRAME_END) {
          s_error = YX5200_ERR_RX_BAD_END;
          s_query_state = QS_ERROR;
          yx5200_rx_reset_parser();
          return;
        }
        if (f[1] != YX5200_PROTOCOL_VER || f[2] != YX5200_FRAME_LENGTH) {
          s_error = ((f[2] != YX5200_FRAME_LENGTH) ? YX5200_ERR_RX_BAD_LENGTH : YX5200_ERR_RX_BAD_CSUM);
          s_query_state = QS_ERROR;
          yx5200_rx_reset_parser();
          return;
        }

        const uint8_t cmd = f[3];
        const uint8_t fb = f[4];
        const uint16_t param = (uint16_t) ((f[5] << 8) | f[6]);
        const uint16_t csum = (uint16_t) ((f[7] << 8) | f[8]);
        const uint16_t calc = yx5200_compute_checksum(cmd, param, fb);

        if (csum != calc) {
          s_error = YX5200_ERR_RX_BAD_CSUM;
          s_query_state = QS_ERROR;
          yx5200_rx_reset_parser();
          return;
        }

        yx5200_frame_t out = {
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

// Feed a single byte into the parser (call from your RX path)
static void yx5200_rx_process_byte(uint8_t byte) {
  // If a new start appears mid-frame, resync
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
void yx5200_on_frame(const yx5200_frame_t *frame) {
  s_last_response = frame->param;
  switch (frame->cmd) {
    //Success
    case YX5200_RESP_STATUS:
    case YX5200_RESP_VOLUME:
    case YX5200_RESP_EQUALIZER:
    case YX5200_RESP_SD_FILES:
    case YX5200_RESP_USB_FILES:
    case YX5200_RESP_FEEDBACK:
    case YX5200_RESP_SD_CURRENT:
    case YX5200_RESP_USB_CURRENT:
    case YX5200_RESP_MEDIA_STATE:
      s_query_state = QS_ACK;
      break;
      //Error
    case YX5200_RESP_ERROR:
      s_query_state = QS_ERROR;
      return;
      //Events
    case YX5200_EVENT_DEVICE_PLUGGED_IN:
      yx5200_on_media_attached_callback((yx5200_media_t) frame);
      break;
    case YX5200_EVENT_DEVICE_PULLED_OUT:
      yx5200_on_media_detached_callback((yx5200_media_t) frame);
      break;

    case YX5200_EVENT_SD_TRACK_FINISHED:
      yx5200_on_track_finished_callback(YX5200_MEDIA_SD, frame->param);
      break;
    case YX5200_EVENT_USB_TRACK_FINISHED:
      yx5200_on_track_finished_callback(YX5200_MEDIA_USB, frame->param);
      break;
  }
}

void __attribute__((weak)) yx5200_on_media_attached_callback(yx5200_media_t media) {
  (void) media;
}

void __attribute__((weak)) yx5200_on_media_detached_callback(yx5200_media_t media) {
  (void) media;
}

void __attribute__((weak)) yx5200_on_track_finished_callback(yx5200_media_t media, uint16_t index) {
  (void) media;
  (void) index;
}

// ---- High-level API ----

yx5200_error_t yx5200_set_source(yx5200_media_t src) {
  if (src != YX5200_MEDIA_SD && src != YX5200_MEDIA_USB) {
    return YX5200_ERR_BAD_PARAM;
  }
  return yx5200_send_command(YX5200_CMD_SET_SOURCE, (uint16_t) src);
}

yx5200_error_t yx5200_set_volume(uint8_t volume) {
  if (volume > 30) {
    return YX5200_ERR_BAD_PARAM;
  }
  return yx5200_send_command(YX5200_CMD_SET_VOLUME, volume);
}

yx5200_error_t yx5200_set_equalizer(yx5200_eq_t eq) {
  return yx5200_send_command(YX5200_CMD_SET_EQUALIZER, (uint16_t) eq);
}

yx5200_error_t yx5200_play(void) {
  return yx5200_send_command(YX5200_CMD_PLAY, 0);
}

yx5200_error_t yx5200_pause(void) {
  return yx5200_send_command(YX5200_CMD_PAUSE, 0);
}

yx5200_error_t yx5200_stop(void) {
  return yx5200_send_command(YX5200_CMD_STOP, 0);
}

yx5200_error_t yx5200_next(void) {
  return yx5200_send_command(YX5200_CMD_NEXT, 0);
}

yx5200_error_t yx5200_previous(void) {
  return yx5200_send_command(YX5200_CMD_PREVIOUS, 0);
}

yx5200_error_t yx5200_reset(void) {
  return yx5200_send_command(YX5200_CMD_RESET, 0);
}

yx5200_error_t yx5200_play_track(uint16_t globalIndex) {
  return yx5200_send_command(YX5200_CMD_PLAY_TRACK, globalIndex);
}

yx5200_error_t yx5200_play_folder_file(uint8_t folder, uint8_t file) {
  if (folder > 99 || file > 255 || folder == 0 || file == 0) {
    return YX5200_ERR_BAD_PARAM;
  }
  uint16_t param = ((uint16_t) folder << 8) | file;
  return yx5200_send_command(YX5200_CMD_PLAY_FOLDER_FILE, param);
}

yx5200_error_t yx5200_play_big_folder_file(uint8_t folder, uint16_t file) {
  if (folder > 16 || folder == 0 || file == 0 || file > 0xFFF) {
    return YX5200_ERR_BAD_PARAM;
  }
  uint16_t param = ((uint16_t) folder << 12) | file;
  return yx5200_send_command(YX5200_CMD_PLAY_FOLDER_FILE, param);
}

yx5200_error_t yx5200_play_mp3_folder(uint16_t index) {
  // 1..3000 ("MP3" folder)
  return yx5200_send_command(YX5200_CMD_PLAY_MP3_FOLDER, index);
}

yx5200_error_t yx5200_set_all_loop(uint8_t on) {
  // 0x11: 1=on, 0=off
  return yx5200_send_command(YX5200_CMD_SET_LOOP_PLAY_MODE, (uint16_t) (on ? 1 : 0));
}

yx5200_error_t yx5200_set_single_loop(uint8_t on) {
// 0x19: loop current track
  return yx5200_send_command(YX5200_CMD_LOOP, (uint16_t) (on ? 1 : 0));
}

yx5200_error_t yx5200_play_folder_loop(uint8_t folder) {
  // 0x17: loop folder 1..99
  if (folder > 99 || folder == 0) {
    return YX5200_ERR_BAD_PARAM;
  }
  return yx5200_send_command(YX5200_CMD_PLAY_FOLDER_LOOP, folder);
}

yx5200_error_t yx5200_loop_track(uint16_t globalIndex) {
  // 0x08: loop specific track
  return yx5200_send_command(YX5200_CMD_LOOP_TRACK, globalIndex);
}

yx5200_error_t yx5200_play_random(void) {
  // 0x18
  return yx5200_send_command(YX5200_CMD_PLAY_RANDOM, 0);
}

yx5200_error_t yx5200_advert_play(uint8_t index) {
  // 0x13: 1..255
  return yx5200_send_command(YX5200_CMD_INSERT_AD, index);
}

yx5200_error_t yx5200_advert_play_folder(uint8_t folder, uint8_t file) {
  // 0x25: 1..255
  if (folder > 0 && folder < 10 && file > 0) {
    uint16_t param = ((uint16_t) folder << 8) | file;
    return yx5200_send_command(YX5200_CMD_INSERT_AD_FROM_FOLDER, param);
  }
  return YX5200_ERR_BAD_PARAM;
}

yx5200_error_t yx5200_advert_stop(void) {
  // 0x15
  return yx5200_send_command(YX5200_CMD_STOP_AD, 0);
}

yx5200_error_t yx5200_set_dac_config(uint8_t on) {
  // 0x1A
  return yx5200_send_command(YX5200_CMD_DAC, (uint16_t) (on ? 1 : 0));
}


// ---- Queries ----
yx5200_media_response_t yx5200_query_media_online(void) {
  yx5200_error_t cmd = yx5200_send_command(YX5200_Q_MEDIA_STATE, 0);
  yx5200_media_response_t resp = {.error = cmd};
  if (cmd == YX5200_ERR_TIMEOUT) {
    resp.data = YX5200_MEDIA_UNKNOWN;
  }
  return resp;
}

yx5200_status_response_t yx5200_query_status(void) {
  // 0x42
  yx5200_error_t cmd = yx5200_send_command(YX5200_Q_STATUS, 0);
  yx5200_status_response_t resp = {.error = cmd};
  if (cmd == YX5200_ERR_TIMEOUT) {
    resp.player = hi8(s_last_response);
    resp.media = lo8(s_last_response);
  }
  return resp;
}

yx5200_response_t yx5200_query_volume(void) {
  // 0x43
  yx5200_error_t cmd = yx5200_send_command(YX5200_Q_VOLUME, 0);
  yx5200_response_t resp = {.error = cmd};
  if (cmd == YX5200_ERR_TIMEOUT) {
    resp.value = s_last_response;
  }
  return resp;
}

yx5200_response_t yx5200_query_total_usb(void) {
  // 0x47
  yx5200_error_t cmd = yx5200_send_command(YX5200_Q_USB_FILES, 0);
  yx5200_response_t resp = {.error = cmd};
  if (cmd == YX5200_ERR_TIMEOUT) {
    resp.value = s_last_response;
  }
  return resp;
}

yx5200_response_t yx5200_query_total_sd(void) {
  // 0x48
  yx5200_error_t cmd = yx5200_send_command(YX5200_Q_SD_FILES, 0);
  yx5200_response_t resp = {.error = cmd};
  if (cmd == YX5200_ERR_TIMEOUT) {
    resp.value = s_last_response;
  }
  return resp;
}

yx5200_response_t yx5200_query_current_usb(void) {
// 0x4B
  yx5200_error_t cmd = yx5200_send_command(YX5200_Q_USB_CURRENT, 0);
  yx5200_response_t resp = {.error = cmd};
  if (cmd == YX5200_ERR_TIMEOUT) {
    resp.value = s_last_response;
  }
  return resp;
}

yx5200_response_t yx5200_query_current_sd(void) {
  // 0x4C
  yx5200_error_t cmd = yx5200_send_command(YX5200_Q_SD_CURRENT, 0);
  yx5200_response_t resp = {.error = cmd};
  if (cmd == YX5200_ERR_TIMEOUT) {
    resp.value = s_last_response;
  }
  return resp;
}

yx5200_eq_response_t yx5200_query_eq(void) {
  // 0x4C
  yx5200_error_t cmd = yx5200_send_command(YX5200_Q_EQUALIZER, 0);
  yx5200_eq_response_t resp = {.error = cmd};
  if (cmd == YX5200_ERR_TIMEOUT) {
    resp.equalizer = s_last_response;
  }
  return resp;
}
