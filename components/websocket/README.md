# WebSocket

## Tujuan

Komponen `websocket` adalah **kurir/transport layer** antara firmware ESP32-S3 dan server Gemini.

WebSocket bertugas membawa data dan pesan dari satu sisi ke sisi lain. WebSocket **bukan pengolah audio** dan bukan pemilik hardware audio.

Prinsip utama:

> **WebSocket adalah kurir, bukan pengolah audio.**

---

## Struktur Komponen

```text
components/
└── websocket/
    │
    ├── CMakeLists.txt
    ├── README.md
    │
    ├── include/
    │   ├── websocket.h
    │   ├── websocket_event.h
    │   └── websocket_transport.h
    │
    ├── websocket.cpp
    ├── websocket_transport.cpp
    ├── websocket_event.cpp
    │
    ├── gemini/
    │   ├── gemini_protocol.h
    │   ├── gemini_protocol.cpp
    │   ├── gemini_message.h
    │   └── gemini_message.cpp
    │
    └── internal/
        ├── websocket_state.h
        └── websocket_config.h
```

Struktur ini dipisahkan supaya **transport WebSocket, event, dan protokol Gemini tidak bercampur menjadi satu file besar**.

---

# Tanggung Jawab File

## `websocket.h`

API utama yang digunakan komponen lain.

Tanggung jawab:

- inisialisasi WebSocket
- connect
- disconnect
- reconnect
- membaca status koneksi
- mengirim data melalui WebSocket

File ini menjadi **pintu masuk resmi** komponen WebSocket.

---

## `websocket.cpp`

Implementasi WebSocket Manager.

Mengatur alur umum:

```text
init
  ↓
connect
  ↓
connected
  ↓
communication
  ↓
disconnect / error
  ↓
reconnect
```

Manager tidak boleh mengakses I2S atau mengambil alih AudioEngine.

---

## `websocket_transport.h/.cpp`

Lapisan transport yang berhubungan langsung dengan library:

```text
esp_websocket_client
```

Tanggung jawab:

- membuat client
- membuka koneksi
- menutup koneksi
- mengirim payload
- menerima payload
- meneruskan event transport

Transport **tidak memahami keputusan audio atau state aplikasi**.

---

## `websocket_event.h/.cpp`

Menangani event dari WebSocket.

Contoh event:

```text
CONNECTED
DISCONNECTED
ERROR
DATA
CLOSED
```

Event handler hanya menerjemahkan event menjadi informasi yang dapat diproses oleh layer WebSocket.

---

# Gemini

Folder `gemini/` berisi logika **protokol Gemini**, bukan driver WebSocket.

## `gemini_protocol.h/.cpp`

Tanggung jawab:

- session setup
- membuat pesan Gemini
- memproses format pesan Gemini
- menentukan jenis payload berdasarkan protokol

---

## `gemini_message.h/.cpp`

Mendefinisikan struktur pesan yang keluar dan masuk.

Contoh konsep:

```text
Gemini Message
├── setup
├── audio
├── text
├── response
└── session
```

JSON dan encoding/decoding yang memang diperlukan protokol Gemini ditempatkan di layer ini, bukan di Audio HAL.

---

# Internal

## `internal/websocket_state.h`

Menyimpan state internal WebSocket.

Contoh:

```text
DISCONNECTED
CONNECTING
CONNECTED
ERROR
```

State ini tidak boleh menjadi pengganti state AudioEngine.

---

## `internal/websocket_config.h`

Konfigurasi internal WebSocket/Gemini.

Contoh:

- endpoint
- konfigurasi koneksi
- timeout
- reconnect policy
- parameter transport

Rahasia seperti API key tidak boleh ditulis hard-code ke source code.

---

# Jalur Data

## Upload Audio

```text
MIC
 ↓
Audio HAL
 ↓ PCM16
AudioEngine
 ↓
WebSocket
 ↓
Gemini
```

WebSocket hanya menerima data yang sudah disiapkan oleh `AudioEngine`.

WebSocket **tidak membaca microphone**.

---

## Download Audio

```text
Gemini
 ↓
WebSocket
 ↓ PCM16/data audio
AudioEngine
 ↓
Audio HAL
 ↓
Speaker
```

WebSocket **tidak menulis speaker**.

---

# Yang DILARANG di WebSocket

WebSocket tidak boleh:

```text
❌ Mengakses I2S RX/TX
❌ Mengakses DMA audio
❌ Membaca microphone langsung
❌ Menulis speaker langsung
❌ Mengatur sample rate hardware audio
❌ Mengubah I2S ↔ PCM
❌ Menjalankan WakeNet/WakeWord
❌ Mengelola AEC
❌ Mengelola NSNet2
❌ Mengambil alih state AudioEngine
❌ Mengurus Wi-Fi manager
❌ Mengurus display
❌ Mengandung logika UI
```

---

# Pemisahan Tanggung Jawab

| Komponen | Tanggung jawab |
|---|---|
| `audio_hal` | I2S, DMA, clock, RX/TX, hardware ↔ PCM16 |
| `audio_engine` | Aliran audio, buffer, ownership, audio state |
| `wakeword` | Deteksi wake word dari PCM mic |
| `websocket` | Transport komunikasi |
| `gemini` | Protokol dan format pesan Gemini |
| `wifi_manager` | Koneksi Wi-Fi |
| `display` | Tampilan perangkat |
| `uart_control` | Kontrol UART |
| `web_config` | Konfigurasi melalui web |

---

# Prinsip Dependency

Arah dependency yang diinginkan:

```text
Audio HAL
    ↑
AudioEngine
    ↑
WebSocket
    ↑
Gemini
```

Namun secara praktis WebSocket hanya menjadi penghubung transport. **WebSocket tidak boleh menarik dependency hardware audio.**

Khususnya jangan membuat:

```text
WebSocket → I2S
WebSocket → Audio HAL → hardware decision
WebSocket → WakeNet
```

---

# Task dan Buffer

Tahap awal tidak boleh membuat banyak task hanya karena pemisahan file.

Prioritas:

1. API sederhana.
2. Event handling sederhana.
3. Transport stabil.
4. Buffer ownership jelas.
5. Baru menambah worker/task jika memang diperlukan.

Jangan mengulangi pola lama yang membuat `websocket_task` terlalu berat dan berpotensi menyebabkan stack overflow.

---

# Tahap Implementasi

Implementasi dilakukan bertahap:

1. Struktur folder.
2. API WebSocket.
3. Transport client.
4. Event handling.
5. Connect/disconnect.
6. Reconnect.
7. Gemini protocol.
8. Gemini message handling.
9. Kirim data dari AudioEngine.
10. Terima audio dari Gemini.
11. Integrasi penuh dengan AudioEngine.

Setiap tahap harus dapat diaudit dan di-build sebelum melanjutkan ke tahap berikutnya.

---

# Aturan Utama

Jika suatu kode membuat WebSocket mulai melakukan pekerjaan seperti:

```text
membaca mic
mengolah PCM
mengatur speaker
menjalankan WakeNet
mengatur audio state
```

maka kode tersebut **berada di layer yang salah**.

Arsitektur yang harus dipertahankan:

```text
MIC
 ↓
Audio HAL
 ↓
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
SPEAKER
```

**WebSocket hanya kurir.**