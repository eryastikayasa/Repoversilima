# Audio HAL

## Tujuan

`audio_hal` adalah lapisan paling bawah untuk perangkat audio pada ESP32-S3.

Tugasnya sederhana: menangani hardware audio dan menyediakan PCM kepada layer di atasnya.

```text
MIC → AUDIO HAL → PCM16

PCM16 → AUDIO HAL → SPEAKER
```

Audio HAL **tidak mengetahui** Gemini, WebSocket, WiFi, session AI, JSON, Base64, atau keputusan alur percakapan.

---

## Prinsip Arsitektur

### 1. Audio HAL adalah satu-satunya pemilik I2S

Semua akses langsung ke hardware audio berada di Audio HAL:

- I2S RX untuk microphone
- I2S TX untuk speaker
- DMA audio
- sample rate
- slot/channel configuration
- konversi sample
- start/stop capture
- start/stop playback

Layer lain tidak boleh mengakses I2S secara langsung.

### 2. Format internal sederhana

Microphone:

```text
I2S RX
  ↓
32-bit input
  ↓
PCM16 signed
  ↓
16 kHz mono
```

Speaker:

```text
PCM16 signed
  ↓
24 kHz mono
  ↓
I2S TX
```

Format PCM menjadi batas yang jelas antara hardware audio dan AudioEngine.

### 3. Audio HAL tidak mengambil keputusan

Audio HAL hanya melakukan operasi yang diminta.

Contoh:

```text
AudioEngine: "beri saya PCM mic"
Audio HAL:   "ini PCM-nya"

AudioEngine: "putar PCM ini"
Audio HAL:   "PCM dikirim ke speaker"
```

Audio HAL tidak menentukan:

- kapan wake word aktif
- kapan recording dimulai karena AI
- kapan WebSocket dikirim
- kapan Gemini menjawab
- kapan session dimulai/selesai
- kapan audio harus di-drop karena state AI

---

## Jalur Audio Repo5

Arsitektur target Repo5:

```text
                         ┌──────────────┐
MIC ── I2S RX ─────────► │  AUDIO HAL   │
                         │              │
                         │  I2S + DMA   │
                         │  32 → PCM16  │
                         └──────┬───────┘
                                │
                         PCM16 / 16 kHz
                                │
                                ▼
                         ┌──────────────┐
                         │  AudioEngine │
                         └──────┬───────┘
                                │
                    ┌───────────┴───────────┐
                    ▼                       ▼
                 WakeWord              WebSocket
                                            │
                                          Gemini
                                            │
                                      PCM audio
                                            │
                                            ▼
                                      AudioEngine
                                            │
                                      PCM16 / 24 kHz
                                            │
                                            ▼
                                      ┌──────────┐
                                      │ AUDIO HAL│
                                      │ I2S TX   │
                                      └────┬─────┘
                                           │
                                        Speaker
```

### Jalur utama

```text
MIC
 ↓
AUDIO HAL
 ↓
AudioEngine
 ↓
Gemini
 ↓
AudioEngine
 ↓
AUDIO HAL
 ↓
SPEAKER
```

WebSocket hanya menjadi transport data antara AudioEngine dan Gemini.

---

## API Konsep

API Audio HAL dibuat kecil agar tanggung jawabnya jelas.

```cpp
audio_hal_init();

audio_hal_start_capture();
audio_hal_stop_capture();

audio_hal_read_pcm(int16_t *buffer, size_t samples);

audio_hal_start_playback();
audio_hal_stop_playback();

audio_hal_write_pcm(const int16_t *buffer, size_t samples);
```

Implementasi final dapat menyesuaikan kebutuhan hardware, tetapi prinsipnya tetap: **API Audio HAL tidak membawa logika AI atau jaringan.**

---

## Tanggung Jawab Audio HAL

### Input / Microphone

Audio HAL bertanggung jawab atas:

- konfigurasi I2S RX
- konfigurasi DMA RX
- pembacaan sample microphone
- konversi sample hardware menjadi PCM16
- penyediaan PCM16 mono 16 kHz kepada AudioEngine
- start/stop capture

### Output / Speaker

Audio HAL bertanggung jawab atas:

- konfigurasi I2S TX
- konfigurasi DMA TX
- menerima PCM16 dari AudioEngine
- konversi sesuai kebutuhan I2S speaker
- playback
- start/stop playback

---

## Yang Tidak Boleh Masuk ke Audio HAL

Jangan menambahkan kode berikut ke Audio HAL:

```text
WebSocket
Gemini
HTTP
WiFi
JSON
Base64
Session manager
Wake word decision
AI state
Conversation state
Network retry
Cloud API
```

Jika sebuah fungsi membutuhkan pengetahuan tentang Gemini atau WebSocket, fungsi tersebut bukan tanggung jawab Audio HAL.

---

## Pengujian Bertahap

Audio HAL Repo5 dibangun dan diuji secara bertahap.

### Stage A — Microphone

```text
MIC
 ↓
I2S RX
 ↓
DMA
 ↓
Audio HAL
 ↓
PCM16 16 kHz
 ↓
log / UART
```

Target:

- I2S RX stabil
- DMA stabil
- tidak terjadi overflow
- tidak terjadi stack overflow
- sample PCM valid
- sample rate 16 kHz

Tidak ada WebSocket dan Gemini pada tahap ini.

### Stage B — Speaker

```text
Test Tone / PCM16
 ↓
Audio HAL
 ↓
I2S TX
 ↓
DMA
 ↓
Speaker
```

Target:

- I2S TX stabil
- DMA stabil
- suara keluar dengan benar
- tidak ada ketergantungan WiFi/WebSocket

### Stage C — WakeWord

```text
MIC
 ↓
Audio HAL
 ↓
PCM16 16 kHz
 ↓
WakeNet
```

WakeWord hanya menggunakan PCM yang disediakan oleh jalur audio.

### Stage D — AudioEngine

```text
Audio HAL
 ↓
AudioEngine
 ├── WakeWord
 └── audio buffer / recorder
```

AudioEngine menjadi pengatur alur audio, bukan Audio HAL.

### Stage E — WebSocket

Baru setelah Audio HAL dan AudioEngine stabil:

```text
AudioEngine
 ↓
WebSocket
 ↓
Gemini
 ↓
WebSocket
 ↓
AudioEngine
 ↓
Audio HAL
 ↓
Speaker
```

---

## Aturan Penting

1. **Satu pemilik I2S:** Audio HAL.
2. **Satu format batas internal:** PCM16.
3. **Audio HAL tidak mengenal jaringan.**
4. **WebSocket tidak menyentuh I2S.**
5. **WakeWord tidak mengontrol I2S secara langsung.**
6. **AudioEngine menjadi pengatur alur audio.**
7. **Masalah WebSocket tidak boleh merusak Audio HAL.**
8. **Masalah audio tidak boleh membuat WebSocket mengambil keputusan audio.**
9. Setiap tahap harus dapat diuji secara independen sebelum tahap berikutnya ditambahkan.

---

## Hubungan dengan Repo4

Repo4 digunakan sebagai referensi hardware yang sudah terbukti berjalan, terutama konfigurasi I2S, DMA, format microphone, dan format speaker.

Namun, logika yang menggabungkan Audio HAL dengan AudioEngine, WebSocket, Gemini, atau session tidak dibawa menjadi tanggung jawab Audio HAL Repo5.

Repo5 mengambil **konfigurasi hardware yang terbukti**, kemudian memisahkan tanggung jawabnya agar jalur audio lebih sederhana dan mudah diuji.

---

## Status

`audio_hal` Repo5 saat ini mengikuti desain dasar berikut:

```text
HARDWARE
   │
   ▼
AUDIO HAL
   │
   │ PCM16
   ▼
AUDIO ENGINE
   │
   ├── WakeWord
   └── WebSocket → Gemini

Gemini
   │
   ▼
AUDIO ENGINE
   │
   ▼
AUDIO HAL
   │
   ▼
HARDWARE SPEAKER
```

Implementasi kode dilakukan bertahap setelah konsep dan batas tanggung jawab ini dinyatakan stabil.
