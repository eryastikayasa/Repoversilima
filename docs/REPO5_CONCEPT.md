# Repo5 — Konsep Arsitektur

## 1. Prinsip Utama

Repo5 adalah rebuild baru untuk ESP32-S3. Struktur, boundary, lifecycle, dan jalur data Repo5 **tidak boleh dipaksa mengikuti Repo4**.

Repo4 hanya digunakan sebagai referensi historis atau pembanding ketika diperlukan. Repo4 bukan blueprint implementasi Repo5.

Tujuan Repo5 adalah membangun jalur yang lebih kecil, jelas, terpisah, dan mudah diaudit.

## 2. Jalur Audio Utama

```text
MIC
 ↓
Audio HAL
 ↓
Audio Engine
 ↓
WebSocket
 ↓
Gemini
 ↓
WebSocket
 ↓
Audio Engine
 ↓
Audio HAL
 ↓
SPEAKER
```

Secara konseptual:

```text
MIC → AUDIO ENGINE → WEBSOCKET → GEMINI
                         ↓
GEMINI → WEBSOCKET → AUDIO ENGINE → SPEAKER
```

## 3. Tanggung Jawab Komponen

### Audio HAL

Audio HAL adalah lapisan hardware audio.

Tugas:
- menginisialisasi hardware I2S;
- mengelola channel RX/TX I2S;
- membaca data microphone;
- menulis data speaker;
- menangani lifecycle hardware audio.

Audio HAL **tidak** mengetahui Gemini, WebSocket, WakeWord, atau keputusan percakapan.

### Audio Engine

Audio Engine adalah pemilik jalur audio aplikasi.

Tugas:
- mengatur ownership microphone dan speaker;
- mengatur capture audio;
- menghubungkan capture dengan WakeWord atau conversation;
- menyediakan frame PCM untuk uplink;
- menerima PCM playback dari jalur Gemini;
- mengatur queue/task audio bila memang diperlukan;
- memastikan data audio tidak hilang karena kontrak antar-layer yang salah.

Audio Engine adalah tempat logika audio berada.

### WebSocket

WebSocket adalah **transport/kuryur**.

Tugas:
- membuka dan menutup koneksi;
- mengirim data ke Gemini;
- menerima data dari Gemini;
- menangani fragmentasi payload jaringan;
- meneruskan audio masuk ke Audio Engine;
- mengambil frame audio yang sudah disiapkan Audio Engine untuk dikirim.

WebSocket **tidak boleh**:
- membaca I2S secara langsung;
- menulis I2S secara langsung;
- menjalankan WakeWord;
- melakukan AEC/NS;
- mengambil keputusan audio;
- mengelola state AudioEngine;
- mengontrol speaker secara langsung.

### Gemini

Lapisan Gemini menangani protokol dan format pesan Gemini Live.

Tugas:
- setup session;
- mengirim realtime audio input;
- membaca serverContent;
- mendeteksi audio PCM dari response;
- meneruskan PCM hasil decode ke Audio Engine.

Gemini tidak boleh mengambil alih ownership hardware audio.

## 4. Boundary yang Harus Dipertahankan

```text
┌──────────────┐
│   Audio HAL  │  Hardware I2S
└──────┬───────┘
       │ PCM
┌──────▼───────┐
│ Audio Engine │  Audio ownership + audio flow
└──────┬───────┘
       │ PCM/frame
┌──────▼───────┐
│  WebSocket   │  Transport only
└──────┬───────┘
       │ network
┌──────▼───────┐
│    Gemini    │  Protocol/service
└──────────────┘
```

Setiap layer hanya berbicara melalui interface yang dimilikinya.

## 5. Prinsip Debugging Repo5

Jika terjadi masalah, jangan langsung menyalin implementasi Repo4.

Urutan audit:

1. Tentukan layer tempat masalah terjadi.
2. Periksa kontrak interface antar-layer.
3. Periksa lifecycle komponen tersebut.
4. Periksa ownership resource.
5. Periksa blocking/non-blocking behavior.
6. Periksa task scheduling bila relevan.
7. Periksa queue hanya jika log/runtime membuktikan diperlukan.
8. Buat perubahan sekecil mungkin.
9. Build dan runtime-test.
10. Jika hasil memburuk, rollback perubahan tersebut.

## 6. Kasus Speaker Saat Ini

Masalah speaker tidak boleh diselesaikan hanya dengan meniru pola chunking Repo4.

Jalur yang harus diaudit adalah:

```text
Gemini RX
   ↓
Gemini Audio Parser
   ↓
AudioEngine Playback Queue
   ↓
AudioEngine Playback Task
   ↓
Audio HAL
   ↓
I2S TX
   ↓
DMA
   ↓
Speaker
```

Fokus audit:
- lifecycle TX channel;
- enable/disable TX;
- konfigurasi DMA;
- konfigurasi slot dan clock;
- perilaku `i2s_channel_write()` pada ESP-IDF 6.0.1;
- kontrak `samples_written`;
- handling partial write;
- timeout dan zero-progress;
- scheduling playback task;
- ownership TX antara Audio HAL dan Audio Engine.

**Jangan mengubah chunk size hanya karena Repo4 menggunakan nilai tertentu.**

## 7. Kontrak Partial Write

Jika `i2s_channel_write()` mengembalikan jumlah data yang hanya sebagian diterima, layer Repo5 harus mempertahankan data yang belum terkirim.

Contoh:

```text
requested = 1024 samples
written   = 704 samples

704 samples sudah diterima I2S.
320 samples masih harus diproses.
```

Kesalahan yang harus dihindari:

```text
write 1024
↓
I2S menerima 704
↓
fungsi langsung membuang 320
```

Tetapi juga jangan membuat retry tanpa batas. Retry harus memiliki aturan timeout/batas progress yang jelas sehingga tidak dapat menggantung task playback.

## 8. Queue dan Task

Repo5 menggunakan task/queue hanya ketika ada alasan arsitektural atau bukti runtime.

Tidak ada prinsip bahwa semakin banyak queue/task semakin baik.

Setiap queue/task baru harus menjawab:

- masalah apa yang diselesaikan;
- siapa producer;
- siapa consumer;
- siapa owner data;
- bagaimana lifecycle start/stop;
- apa yang terjadi ketika penuh;
- bagaimana mencegah stale data;
- bagaimana mencegah race condition.

## 9. Network dan Audio Tidak Dicampur

Network yang lambat tidak boleh membuat capture microphone ikut blocking.

Karena itu jalur uplink menggunakan pemisahan:

```text
AudioEngine
    ↓
Capture
    ↓
TX Queue
    ↓
Network Sender
    ↓
WebSocket
```

Jika jaringan lambat, masalah network ditangani pada boundary TX, bukan dengan membuat AudioEngine menunggu network.

Sebaliknya, network RX tidak boleh langsung menjalankan operasi hardware speaker yang berat di callback WebSocket.

```text
WebSocket RX
    ↓
RX handling
    ↓
Gemini parser
    ↓
AudioEngine
    ↓
Playback task
    ↓
Audio HAL
```

## 10. Hal yang Tidak Dilakukan Sekarang

Sebelum jalur dasar stabil, jangan:

- menambahkan AEC;
- menambahkan pemrosesan audio baru;
- menambah queue tanpa bukti;
- menambah worker hanya untuk mengikuti Repo4;
- memindahkan ownership audio ke WebSocket;
- mengubah baseline WakeWord/MIC yang sudah terbukti;
- mengubah banyak layer sekaligus.

## 11. Definisi Sederhana Repo5

Repo5 harus tetap dapat dijelaskan dengan kalimat berikut:

> **Audio Engine memiliki audio. WebSocket hanya mengantar audio. Audio HAL hanya berbicara dengan hardware. Gemini hanya menangani protokol/service.**

Jika suatu perubahan membuat boundary tersebut kabur, perubahan tersebut harus diaudit ulang sebelum diterapkan.
