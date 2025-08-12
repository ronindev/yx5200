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
// 1-byte buffer for HAL IT reception
static uint8_t s_rx_byte_it = 0;

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
    YX5200_CMD_MODE_ON = 0x0A,
    YX5200_CMD_MODE_NORMAL = 0x0B,
    YX5200_CMD_RESET = 0x0C,
    YX5200_CMD_PLAY = 0x0D,
    YX5200_CMD_PAUSE = 0x0E,
    YX5200_CMD_PLAY_FOLDER_FILE = 0x0F, // DH=folder (01~99), DL=track
    YX5200_CMD_SET_VOLUME_GAIN = 0x10, // High byte = 1 to enable preset volume, Low byte = gain 0..31
    YX5200_CMD_REPEAT_CURRENT = 0x11  // 1 = repeat current track, 0 = stop
} YX5200_Command;

// Query codes
typedef enum {
    YX5200_Q_DEVICE_PLUGGED_IN = 0x3A,
    YX5200_Q_DEVICE_PULLED_OUT = 0x3B,
    YX5200_Q_IS_USB = 0x3C,
    YX5200_Q_IS_SD = 0x3D,
    YX5200_Q_IS_FLASH = 0x3E,
    YX5200_Q_INITIALIZE = 0x3F,
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
static void yx5200_send_command(uint8_t cmd, uint16_t param) {
  if (s_uart == NULL) {
    return; // UART is not initialized
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
        break;
      }
    }
  } else {
    HAL_Delay(100);
  }
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
    switch (frame->param) {
      case 0x0:
        //ACK
        query_state = QS_ACK;
        return;
    }
  } else if (frame->cmd == YX5200_Q_ERROR) {
    query_state = QS_ERROR;
    //TODO: add error handle
    switch (frame->param) {
      case 0x1:
        //Module busy (this info is returned when the initialization is not done)
        break;
      case 0x2:
        //Currently sleep mode(supports only specified device in sleep mode)
        break;
      case 0x3:
        //Serial receiving error(a frame has not been received completely yet)
        break;
      case 0x4:
        //Checksum incorrect
        break;
      case 0x5:
        //Specified track is out of current track scope
        break;
      case 0x6:
        //Specified track is not found
        break;
      case 0x7:
        //Insertion error(an inserting operation only can be done when a track is being played)
        break;
      case 0x8:
        //SD card reading failed(SD card pulled out or damaged)
        break;
      case 0xA:
        //Entered into sleep mode
        break;
    }
    return;
  } else if (frame->cmd == YX5200_Q_DEVICE_PLUGGED_IN) {
    return;
  } else if (frame->cmd == YX5200_Q_DEVICE_PULLED_OUT) {
    return;
  } else if (frame->cmd == YX5200_Q_INITIALIZE) {
    query_state = QS_ACK;
    return;
  }
}

void yx5200_on_frame_error(YX5200_RxError error) {
  //TODO: add weak callback to handle errors
  query_state = QS_ERROR;
}

// ---- High-level API ----

void yx5200_initialize(void) {
  yx5200_send_command(YX5200_Q_INITIALIZE, 0);
}

void yx5200_next(void) {
  yx5200_send_command(YX5200_CMD_NEXT, 0);
}

void yx5200_previous(void) {
  yx5200_send_command(YX5200_CMD_PREVIOUS, 0);
}

void yx5200_play_track(uint16_t index) {
  // Plays track by global index (0..2999)
  yx5200_send_command(YX5200_CMD_PLAY_INDEX, index);
}

void yx5200_volume_up(void) {
  yx5200_send_command(YX5200_CMD_VOLUME_UP, 0);
}

void yx5200_volume_down(void) {
  yx5200_send_command(YX5200_CMD_VOLUME_DOWN, 0);
}

void yx5200_set_volume(uint8_t volume) {
  // volume: 0..30
  yx5200_send_command(YX5200_CMD_SET_VOLUME, volume);
}

void yx5200_set_equalizer(YX5200_Equalizer eq) {
  yx5200_send_command(YX5200_CMD_SET_EQUALIZER, (uint16_t) eq);
}

void yx5200_set_play_mode(YX5200_PlayMode mode) {
  yx5200_send_command(YX5200_CMD_SET_PLAY_MODE, (uint16_t) mode);
}

void yx5200_set_source(YX5200_Source src) {
  yx5200_send_command(YX5200_CMD_SET_SOURCE, (uint16_t) src);
}

void yx5200_mode_on(void) {
  yx5200_send_command(YX5200_CMD_MODE_ON, 0);
}

void yx5200_mode_normal(void) {
  yx5200_send_command(YX5200_CMD_MODE_NORMAL, 0);
}

void yx5200_reset(void) {
  yx5200_send_command(YX5200_CMD_RESET, 0);
}

void yx5200_play(void) {
  yx5200_send_command(YX5200_CMD_PLAY, 0);
}

void yx5200_pause(void) {
  yx5200_send_command(YX5200_CMD_PAUSE, 0);
}

void yx5200_play_folder_file(uint8_t folder, uint8_t file) {
  // Plays a file within a folder: param = (folder << 8) | file, folder 01..99
  uint16_t param = ((uint16_t) folder << 8) | file;
  yx5200_send_command(YX5200_CMD_PLAY_FOLDER_FILE, param);
}

void yx5200_set_volume_gain(uint8_t enabled, uint8_t gain) {
  // High byte = 1 to apply preset volume, Low byte = gain (0..31)
  uint16_t param = ((uint16_t) enabled << 8) | gain;
  yx5200_send_command(YX5200_CMD_SET_VOLUME_GAIN, param);
}

void yx5200_repeat_current(uint8_t enable) {
  // 1 = repeat the current track, 0 = stop repeating
  yx5200_send_command(YX5200_CMD_REPEAT_CURRENT, enable);
}

// ---- Queries ----

void yx5200_query_is_usb(void) {
  yx5200_send_command(YX5200_Q_IS_USB, 0);
}

void yx5200_query_is_sd(void) {
  yx5200_send_command(YX5200_Q_IS_SD, 0);
}

void yx5200_query_is_flash(void) {
  yx5200_send_command(YX5200_Q_IS_FLASH, 0);
}

void yx5200_response(void) {
  yx5200_send_command(YX5200_Q_FEEDBACK, 0);
}

void yx5200_query_status(void) {
  yx5200_send_command(YX5200_Q_STATUS, 0);
}

void yx5200_query_volume(void) {
  yx5200_send_command(YX5200_Q_VOLUME, 0);
}

void yx5200_query_equalizer(void) {
  yx5200_send_command(YX5200_Q_EQUALIZER, 0);
}

void yx5200_query_play_mode(void) {
  yx5200_send_command(YX5200_Q_PLAY_MODE, 0);
}

void yx5200_query_software(void) {
  yx5200_send_command(YX5200_Q_SOFTWARE, 0);
}

void yx5200_query_sd_files(void) {
  yx5200_send_command(YX5200_Q_SD_FILES, 0);
}

void yx5200_query_usb_files(void) {
  yx5200_send_command(YX5200_Q_USB_FILES, 0);
}

void yx5200_query_flash_files(void) {
  yx5200_send_command(YX5200_Q_FLASH_FILES, 0);
}

void yx5200_query_on(void) {
  yx5200_send_command(YX5200_Q_ON, 0);
}

void yx5200_query_sd_current(void) {
  yx5200_send_command(YX5200_Q_SD_CURRENT, 0);
}

void yx5200_query_usb_current(void) {
  yx5200_send_command(YX5200_Q_USB_CURRENT, 0);
}

void yx5200_query_flash_current(void) {
  yx5200_send_command(YX5200_Q_FLASH_CURRENT, 0);
}
