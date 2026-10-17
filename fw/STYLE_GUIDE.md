# Embedded C Style Guide & Architectural Standards

This document outlines the mandatory coding and architectural standards for the ESP32-S3 firmware. All code submitted via pull requests must adhere to these guidelines to ensure reliability, prevent memory corruption, and eliminate Git formatting noise.



---

## 1. Constants & Macros

* **General Constants:** Use `static const <type> NAME = value;` or `enum` for logic, math constants, and state tables.
* **The `#define` Exception:** Preprocessor `#define` is strictly restricted to:
  1. **Buffer and array sizing:** `#define AUDIO_BUFFER_SIZE 640`
  2. **Hardware silicon pin mappings:** `#define PIN_I2S_BCK GPIO_NUM_14`
  3. **Build flags & compile switches:** `#define IS_ROOT`

```c
// PREFERRED:
#define AUDIO_FRAME_BYTES 640
#define PIN_I2S_DATA_IN   GPIO_NUM_32 // ESP32-S3 example; use a valid pin for the active board

static const uint16_t PACKET_TIMEOUT_MS = 250;

enum
{
    AUDIO_SAMPLE_RATE_HZ = 16000,
    AUDIO_CHANNELS       = 1
};
```

---

## Section 2: Types & Integers

* **Integer Types:** Use fixed-width types from `<stdint.h>` (`uint8_t`, `int16_t`, `uint32_t`, etc.) when a value's range or representation matters, especially for PCM samples, packet fields, and sequence numbers. Use `size_t` from `<stddef.h>` for memory sizes and buffer lengths. Preserve ESP-IDF and FreeRTOS types (such as `esp_err_t`, `gpio_num_t`, and `TickType_t`) when interacting with their APIs.
* **Booleans:** Use `bool` from `<stdbool.h>` (`true` / `false`). Never use raw `int` for flags.
* **Loop Counter Exception:** Plain `int` is permitted **strictly** for local loop counters (e.g., `for (int i = 0; i < 10; i++)`).
* **Memory & Buffer Type Distinctions:**
  1. **Raw Microphone Input:** I2S capture receives 32-bit slots. Use the type required by the I2S API for its raw buffer; use `int32_t` when interpreting an individual signed slot as a value.
  2. **PCM Sample Values:** After conversion, individual signed 16-bit mono samples and arrays used for sample processing use `int16_t`.
     ```c
     int16_t pcm_samples[320]; // 320 samples @ 16-bit mono
     ```
  3. **Audio Packets & Raw Byte Streams:** The shared audio packet stores PCM as `uint8_t` bytes in an agreed byte order. Capture encodes each 16-bit sample into those bytes; playback interprets the same order. Network code may treat the PCM payload as opaque bytes. Serialized headers and other raw byte streams also use `uint8_t`.
     ```c
     uint8_t udp_packet[PACKET_MAX_SIZE];
     ```
  4. **Strings & Logging:** `char` is restricted **strictly** to printable human-readable text and logging tags. Never use `char` for raw binary data.




---

## 3. Function Signatures & Error Handling

* **Module Prefixing (No Namespaces):** All public API functions must follow the `<module>_<action>()` convention.
  * *Example:* `audio_capture_init()`, `network_socket_send()`.
* **Private File Helpers:** Any helper function not exposed in the `.h` file **must** be marked `static` to prevent global symbol collisions.
* **Error Handling:** Operations that can fail must return **`esp_err_t`** (`ESP_OK`, `ESP_FAIL`, `ESP_ERR_TIMEOUT`).
* **Query Functions:** Pure state-check queries must return `bool` and begin with `is_` or `has_` (e.g., `network_is_ready()`).
* **Pointer `const` Correctness:** Any function parameter that only *reads* from a buffer pointer must mark it `const`.

```c
// Public API example:
esp_err_t audio_capture_read(uint8_t *dest_buffer, size_t *bytes_read);
esp_err_t network_send_packet(const uint8_t *src_payload, size_t payload_len);

// State query example:
bool network_is_connected(void);

// Private file helper:
static esp_err_t configure_i2s_dma(void)
{
    // ...
}
```

---

## 4. Memory Management (Heap, Stack, and SRAM vs. PSRAM)

* **Reusable Allocations:** Allocate long-lived queues, message buffers, and task-owned working memory during startup or task initialization, then reuse them. Do not call `malloc()`, `calloc()`, or `realloc()` inside steady-state capture, network, or playback loops. Module-level `static` buffers are appropriate when a single module instance owns them.
* **Task Stack Protection:** Size each task's stack for its actual call path and check stack high-water marks under load. Prefer reusable buffers for large packet and audio frames rather than imposing an arbitrary local-buffer size limit.
* **Internal SRAM vs. External PSRAM:**
  * **Internal SRAM:** Use memory with the capabilities required by each driver; in particular, I2S DMA buffers need DMA-capable memory. Internal SRAM also holds ordinary application state when appropriate.
  * **External PSRAM:** The temporary ESP32-C3 has no PSRAM. On the ESP32-S3 N8R8, once PSRAM is configured and available, use it for suitable bulk storage (telemetry logs, audio record-and-replay buffers, large routing/history tables). Do not assume a PSRAM buffer is suitable for I2S DMA.

---

## 5. Logging Standards

* **Ban Raw `printf()`:** All console output must use ESP-IDF’s logging macros (`ESP_LOGE`, `ESP_LOGW`, `ESP_LOGI`, `ESP_LOGD`).
* **Mandatory Tags:** Every `.c` file must define a private static tag at the top:
  ```c
  static const char *TAG = "AUDIO_CAPTURE";
  ```
* **Audio Loop Rule:** Do not log on every iteration of a 20 ms loop. Log state transitions (e.g., PTT down/up) and the first occurrence or change of an error condition (e.g., DMA read timeout); count repeated errors instead of logging every failed frame.
* **Metrics:** Accumulate frame and repeated-error counts and log a rate-limited rolling summary once every 1 to 2 seconds.

---

## 6. Header Files & Include Guards

* All header files (`.h`) must use **`#pragma once`** at the top. Do not use legacy macro guards (`#ifndef`).

---

## 7. Formatting & Indentation (Enforced via `.clang-format`)

* **Style:** Allman indentation (braces always on their own lines).
* **Indentation Width:** 4 Spaces (Never use hard tab characters `\t`).
* **Line Width Limit:** 120 characters.
* **Control Flow:** Maximum **4 levels of indentation**. Use guard clauses and early returns to keep logic flat.

```c
// Standard Allman formatting with guard clause:
esp_err_t audio_capture_start(void)
{
    if (!s_is_initialized)
    {
        ESP_LOGE(TAG, "Driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_is_running)
    {
        return ESP_OK; // Early exit
    }

    return i2s_channel_enable(s_rx_handle);
}
```

