# WakeWord — LOCKED / PASS

## Status

**WAKEWORD = PASS ✅**

Repo5 (`Repoversilima`) telah berhasil menjalankan jalur:

```text
MIC → I2S RX → Audio HAL → PCM16 → WakeNet9 → AudioEngine → MAIN
```

WakeNet9 berhasil mendeteksi:

```text
HI, ESP
```

Deteksi berhasil terjadi lebih dari satu kali pada pengujian hardware.

## Proven configuration

- SoC: ESP32-S3
- ESP-IDF: 6.0.1
- WakeNet: WakeNet9
- Model: `wn9_hiesp`
- Sample rate: 16 kHz
- Channel: mono
- I2S RX: I2S_NUM_1
- Mic format: 32-bit I2S → PCM16
- Mic pins:
  - BCLK/SCK: GPIO 5
  - WS/LRCK: GPIO 4
  - DATA: GPIO 6
- WakeNet chunk: 512 samples
- Wake phrase: `HI, ESP`

## Critical fix

Commit:

```text
e22a9e3bd88daa2eda44ca39f47823c52c8513a8
```

Message:

```text
audio: match Repo4 MIC RX blocking read
```

Perubahan penting pada `audio_hal_read_pcm()`:

```cpp
i2s_channel_read(
    s_rx,
    s_rx_raw,
    input_bytes,
    &bytes_read,
    portMAX_DELAY);
```

Sebelumnya menggunakan timeout 100 ms. Perubahan ini mengikuti perilaku capture proven Repo4 dan menghindari `ESP_ERR_TIMEOUT` pada jalur capture normal.

## Stack overflow fix

Dua perbaikan sebelumnya juga merupakan bagian dari baseline WakeWord yang sudah terbukti:

```text
5709322a4a0b074ddcce85b83d0f1fd4c1a5fb44
8df27aa7dcbaab962642bf5c41e77eb99f6e7508
```

Fungsinya:

1. Buffer raw MIC dipindahkan dari stack task ke storage static.
2. Stack `wakeword_task` dinaikkan menjadi 8192 byte.

## Hardware proof log

Log penting dari pengujian:

```text
WAKEWORD: WakeNet ready: model=wn9_hiesp rate=16000Hz chunk=512 channels=1
WAKEWORD: Wake word aktif: HI, ESP
AUDIO_ENGINE: AudioEngine ready: MIC -> Audio HAL -> WakeNet
AUDIO_HAL: MIC capture START
AUDIO_ENGINE: WakeWord capture START
MAIN: WAKEWORD READY - menunggu HI, ESP
WAKEWORD: WAKE WORD TERDETEKSI: HI, ESP (id=1)
AUDIO_ENGINE: WakeWord event diterima AudioEngine
MAIN: MAIN: WakeWord event
```

Deteksi kedua juga berhasil:

```text
WAKEWORD: WAKE WORD TERDETEKSI: HI, ESP (id=1)
AUDIO_ENGINE: WakeWord event diterima AudioEngine
MAIN: MAIN: WakeWord event
```

## LOCK RULE

**JANGAN mengubah WakeWord lagi tanpa alasan teknis yang kuat.**

Jangan mengubah:

- pin I2S MIC
- konfigurasi I2S RX
- sample rate 16 kHz
- mono/LEFT slot
- WakeNet9 model `wn9_hiesp`
- chunk 512
- buffer proven
- stack `wakeword_task` 8192
- `portMAX_DELAY` pada RX read

Jika tahap berikutnya mengalami masalah, anggap WakeWord sebagai komponen **known-good** dan cari masalah di layer setelahnya.

## Next stage

Setelah WakeWord dikunci, pengembangan dilanjutkan ke:

```text
WakeWord PASS
    ↓
AudioEngine — conversation audio
    ↓
WebSocket transport
    ↓
Gemini protocol
```

WebSocket tetap hanya sebagai kurir/transport dan tidak boleh mengambil alih tanggung jawab AudioEngine.

---

**Baseline terakhir yang terbukti:** `e22a9e3`

**Status: WAKEWORD LOCKED — PASS ✅**
