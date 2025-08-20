# YX5200 (DFPlayer-compatible) UART Library

A small C library to control audio modules based on the YX5200 chipset (e.g., DFPlayer Mini and compatibles) via UART.  
It formats and transmits protocol frames (0x7E ... 0xEF), provides a simple TX-only control API, and optionally supports
receiving and parsing device responses when feedback is enabled.

- MCU stack: STM32 HAL UART
- Language: C11

Note: If feedback is disabled, transmissions are blocking and include a 100 ms post-send delay for robustness.

## Features

- Basic playback control: play, pause, next, previous, reset.
- Play by global track index (0..2999).
- Play a file in a folder (folders 01..99).
- Volume 0..30, equalizer presets (Normal/Pop/Rock/Jazz/Classic/Bass).
- Play modes: all/folder/single/random.
- Source selection: USB/SD/FLASH.
- Optional feedback and RX parser:
    - When enabled, the device may send responses to commands and queries.
    - Lightweight state machine to parse 10-byte frames with checksum verification.
    - Weak callbacks to receive parsed frames and errors.

## Getting Started

### 1) Initialize the library

Pass your HAL UART handle and whether you want feedback replies.

```c 
// #include "yx5200.h"
extern UART_HandleTypeDef huartX; // your configured UART
int main(void) { 
    // ... HAL_Init(), clocks, GPIO, UART init ... 
    yx5200_configure(&huartX, 0); // feedback: 0 = no replies, 1 = request replies
    yx5200_initialize(); // initialize after configuration
    while (1) {
        // ...
    }
}
```

### 2) Send commands

```c 
// Select source: SD card 
yx5200_set_source(YX5200_SRC_SD);
// Volume 20 
yx5200_set_volume(20);
// Equalizer: Rock 
yx5200_set_equalizer(YX5200_EQ_ROCK);
// Play by global index 
yx5200_play_track(123);
// Play file 004 in folder 02 
yx5200_play_folder_file(2, 4);
// Pause / resume 
yx5200_pause(); 
yx5200_play();
// Next / previous 
yx5200_next(); 
yx5200_previous();
// Repeat current track: on/off 
yx5200_play_all(1); 
yx5200_play_all(0);
```

## Enabling Feedback and Receiving Replies (Optional)

Set feedback to 1 at initialization to request device replies.

```c
yx5200_configure(&huartX, 1); // request replies
```

You have two options to feed received bytes into the parser.

### Option A: Interrupt-driven RX (1-byte IT)

- Start IT reception once after init:

```c
yx5200_rx_start_it();
```

- In your HAL UART RX complete callback, forward the event to the library:

```c
void HAL_UART_RxCpltCallback(UART_HandleTypeDef* huart) { 
    yx5200_rx_on_cplt(huart); // library consumes the byte and restarts 1-byte IT
}
```

### Option B: DMA + IDLE chunks

If you already receive into a ring or linear DMA buffer and detect IDLE:

- Pass the new data block to the parser:

```c
// data,len = the new bytes captured since the last IDLE 
yx5200_rx_process_bytes(data, len);
```

## Parameter Ranges

- Volume: 0..30
- Global track index: 0..2999
- Folder: 1..99 (folders on SD should be named `01`..`99`)
- File in folder: 1..255 (depends on module firmware and naming)

The library does not clamp these values; ensure correctness on the caller side.

## Protocol Details (Reference)

Frame (10 bytes):

- 0: 0x7E (start)
- 1: 0xFF (version)
- 2: 0x06 (length)
- 3: command
- 4: feedback (0 or 1)
- 5: param high (DH)
- 6: param low  (DL)
- 7: checksum high
- 8: checksum low
- 9: 0xEF (end)

Checksum is the two’s complement of the sum of bytes 1..6:

```c
checksum = (uint16_t)(0 - (ver + len + cmd + feedback + DH + DL))
```

## Timing and Blocking Behavior

- Transmits are blocking (HAL_UART_Transmit with HAL_MAX_DELAY).
- A small delay (~100 ms) is inserted after each command to avoid overruns on slower modules if no feedback is requested.
    - You may tune or remove it if your setup is stable and responsive.

## Integration Tips

- Ensure UART TX and RX are configured at the module’s expected baud rate (commonly 9600 8N1).
- If you only need one-way control, initialize with feedback = 0 and omit RX integration.
- If you implement Interrupt-driven RX reception:
    - Make sure to call `yx5200_rx_on_cplt(huartX)` in your HAL UART RX complete callback. 
- If you implement DMA+IDLE reception:
    - Make sure to provide contiguous data slices to `yx5200_rx_process_bytes(...)`.
    - Handle buffer wrap-around in your DMA ring before feeding the parser.

## Troubleshooting

- No replies from the device:
    - Make sure feedback was enabled at init (feedback = 1).
    - Verify module firmware supports replies and the UART wiring is correct (TX/RX crossed).
- Parser errors (bad start/end/length/checksum):
    - Check baud rate and signal integrity.
    - Ensure you pass unmodified data slices and do not drop bytes.
- Commands sometimes ignored:
    - Reduce command rate or adjust the post-send delay.
    - Verify that SD/USB media is present and files/folders follow expected naming.

---

