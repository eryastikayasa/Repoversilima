# WebSocket

## Tujuan

Komponen `websocket` adalah lapisan transport komunikasi antara firmware ESP32-S3 dan server Gemini.

WebSocket **bukan** pemilik audio hardware. Komponen ini tidak boleh mengakses I2S, DMA, microphone, atau speaker secara langsung.

## Arsitektur

```text
MIC
 │
 ▼
Audio HAL
 │ PCM16
 ▼
AudioEngine
 │
 ▼
WebSocket
 │
 ▼
Gemini
 │
 ▼
WebSocket
 │ PCM16
 ▼
AudioEngine
 │
 ▼
Audio HAL
 │
 ▼
Speaker
```

## Tanggung Jawab

WebSocket hanya menangani:

- Membuka dan menutup koneksi WebSocket.
- Menjaga status koneksi.
- Reconnect ketika koneksi terputus.
- Mengirim data/protokol ke server Gemini.
- Menerima event dan data dari server Gemini.
- Menyerahkan data audio hasil Gemini ke `AudioEngine`.
- Menerima data audio yang sudah disiapkan `AudioEngine` untuk dikirim ke Gemini.

## Yang Tidak Boleh Dilakukan

WebSocket tidak boleh:

- Mengakses I2S RX/TX.
- Mengakses DMA audio.
- Membaca microphone secara langsung.
- Menulis speaker secara langsung.
- Mengatur sample rate audio hardware.
- Melakukan konversi I2S ke PCM atau PCM ke I2S.
- Menjalankan WakeNet/WakeWord.
- Mengambil keputusan audio.
- Mengelola AEC atau NSNet2.
- Mengelola state AudioEngine.
- Mengurus Wi-Fi manager.
- Mengurus display.
- Menaruh logika UI di dalam WebSocket.

## Jalur Audio

### Upload

Audio microphone harus melalui:

```text
I2S RX → Audio HAL → PCM16 → AudioEngine → WebSocket → Gemini
```

WebSocket hanya menerima buffer PCM/data dari `AudioEngine` dan mengirimkannya sesuai protokol Gemini.

### Download

Audio dari Gemini harus melalui:

```text
Gemini → WebSocket → AudioEngine → PCM16 → Audio HAL → I2S TX → Speaker
```

WebSocket tidak boleh meneruskan audio langsung ke driver I2S.

## Pemisahan Tanggung Jawab

| Komponen | Tanggung jawab |
|---|---|
| `audio_hal` | I2S, DMA, clock, RX/TX, konversi hardware ↔ PCM16 |
| `audio_engine` | Mengatur aliran audio dan ownership buffer/audio state |
| `wakeword` | Deteksi wake word dari PCM mic |
| `websocket` | Transport komunikasi dengan Gemini |
| `wifi_manager` | Koneksi dan konfigurasi Wi-Fi |
| `display` | Tampilan status perangkat |
| `uart_control` | Komunikasi/perintah UART |
| `web_config` | Konfigurasi perangkat melalui web |

## Prinsip Utama

> **WebSocket adalah kurir, bukan pengolah audio.**

Dengan pemisahan ini, masalah WebSocket seperti reconnect, timeout, atau server error tidak boleh merusak atau mengambil alih driver audio.

## Tahap Implementasi

Implementasi WebSocket di Repo5 dilakukan bertahap:

1. Struktur komponen dan API.
2. Inisialisasi client WebSocket.
3. Koneksi ke server.
4. Event handling.
5. Reconnect.
6. Pengiriman pesan/protokol Gemini.
7. Pengiriman audio dari `AudioEngine`.
8. Penerimaan audio dari Gemini.
9. Penyerahan audio ke `AudioEngine`.
10. Integrasi penuh setelah jalur Audio HAL dan AudioEngine stabil.

## Batasan Integrasi

Pada tahap awal, WebSocket **tidak dihubungkan langsung ke `main.cpp`** dan tidak boleh membuat task audio baru yang tidak diperlukan.

Prioritas utama adalah memastikan jalur berikut tetap sederhana dan terpisah:

```text
Audio HAL ↔ AudioEngine ↔ WebSocket ↔ Gemini
```

Setiap perubahan pada WebSocket harus menjaga agar komponen audio tetap independen dari transport jaringan.