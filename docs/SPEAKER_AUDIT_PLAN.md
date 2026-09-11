# Repo5 Speaker / Full-Duplex Audit — Working Concept

## Prinsip utama

Repo5 adalah arsitektur baru. Repo4 hanya referensi historis, bukan blueprint implementasi.

Speaker harus mengikuti boundary Repo5:

```text
Gemini RX
  -> WebSocket
  -> Gemini Audio Parser
  -> AudioEngine playback queue
  -> AudioEngine playback task
  -> Audio HAL
  -> I2S TX DMA
  -> Speaker
```

Uplink tetap:

```text
MIC
  -> AudioEngine
  -> WebSocket TX queue
  -> WebSocket transport
  -> Gemini
```

## Ownership

- WebSocket: transport saja. Tidak mengolah I2S dan tidak memiliki keputusan audio.
- Gemini audio parser: decode/protokol audio lalu menyerahkan PCM ke AudioEngine.
- AudioEngine: pemilik playback queue, playback task, dan lifecycle audio playback.
- Audio HAL: pemilik hardware I2S TX/RX dan operasi read/write hardware.
- AEC: belum dikerjakan sampai jalur dasar stabil.

## Temuan runtime terbaru

Setelah kontrak speaker dikembalikan dari eksperimen 240-sample, runtime terbaru tidak lagi menunjukkan `I2S speaker write fail` atau `Speaker PCM write tidak lengkap`. Jalur Gemini RX -> AudioEngine -> speaker karena itu tidak perlu diubah pada tahap ini.

Masalah yang dominan sekarang adalah full-duplex WebSocket uplink. Satu pesan audio uplink berisi 1600 sample / 3200 byte PCM dan sekitar 4341 byte JSON. Runtime menunjukkan `send_text()` dapat tertahan dari ratusan milidetik sampai sekitar 4,1 detik. Selama sender tertahan, TX queue 6 pesan penuh dan melakukan drop oldest untuk menjaga latency.

Runtime juga sempat menunjukkan `RX buffer penuh`, sehingga kita tidak akan langsung memperbesar RX pool. Bukti saat ini lebih cocok dengan contention antara jalur WebSocket TX dan RX daripada kekurangan queue semata.

## Hipotesis WebSocket yang diuji

`esp_websocket_client` menyediakan opsi separate TX lock agar operasi send tidak menggunakan lock yang sama dengan jalur receive/control. Dokumentasi konfigurasi komponen menjelaskan bahwa separate TX lock ditujukan untuk menghindari lock contention ketika send dan receive berjalan bersamaan. Repo5 sekarang mengaktifkan opsi tersebut dengan timeout TX lock 5000 ms.

Perubahan ini hanya menyentuh konfigurasi internal WebSocket client; arsitektur Repo5, TX queue, AudioEngine, Gemini protocol, WakeWord, dan Audio HAL tidak diubah.

## Masalah speaker yang sebelumnya diaudit

Runtime sebelumnya menunjukkan `i2s_channel_write()` menghasilkan partial write dan `ESP_ERR_TIMEOUT` saat Gemini audio sedang diputar. Eksperimen pemecahan write menjadi 240 sample terbukti memperburuk audio karena partial timeout langsung mengakhiri satu operasi HAL.

## Hipotesis speaker yang tetap dicatat

1. Kontrak `audio_hal_write_pcm()` terhadap partial write/timeout.
2. Interaksi I2S TX writer dengan DMA descriptor dan DMA event/ISR.
3. Scheduling/interrupt pressure saat WiFi/TLS/Gemini RX/decode berjalan bersamaan.
4. Lifecycle TX channel: init -> enable -> write -> disable.
5. Clock/slot configuration TX 24 kHz, 32-bit, mono LEFT.
6. CPU runtime dan kemungkinan scheduling pressure.

## Yang tidak boleh dilakukan pada tahap ini

- Jangan menyalin implementasi Repo4.
- Jangan mengubah WakeWord/MIC yang sudah stabil.
- Jangan memperbesar TX queue hanya untuk menutupi network backpressure.
- Jangan memperbesar RX pool tanpa bukti.
- Jangan menambah task/queue baru tanpa kebutuhan runtime.
- Jangan mengerjakan AEC.
- Jangan menyimpulkan chunk 240 sebagai solusi.
- Jangan mengubah konfigurasi I2S speaker lagi sebelum ada bukti baru.

## Kontrak playback Repo5

AudioEngine playback task boleh blocking menunggu hardware karena WebSocket sudah dipisahkan oleh playback queue.

`audio_hal_write_pcm()` harus menjaga seluruh PCM block tetap utuh. Jika driver menerima sebagian data, offset harus dilanjutkan dari jumlah aktual yang diterima. Sisa PCM tidak boleh dibuang hanya karena satu `ESP_ERR_TIMEOUT` terjadi setelah partial progress.

## Urutan kerja saat ini

1. Kembalikan eksperimen 240-sample yang terbukti buruk. **Selesai.**
2. Pertahankan konfigurasi I2S/DMA Repo5 yang ada. **Dipertahankan.**
3. Koreksi kontrak write agar partial progress tidak kehilangan PCM. **Selesai.**
4. Validasi runtime speaker. **Log terbaru: timeout speaker tidak muncul.**
5. Audit full-duplex WebSocket berdasarkan bukti runtime. **Sedang dikerjakan.**
6. Aktifkan separate WebSocket TX lock dan pertahankan timeout TX lock 5000 ms. **Baru diterapkan; perlu CI + runtime.**
7. Build/CI.
8. Flash hanya setelah build bersih.
9. Bandingkan runtime: TX send latency, TX queue drops, RX buffer pressure, dan playback.
10. Jika contention tetap ada, baru audit scheduling/task priority/network transport satu variabel pada satu waktu.

## Target audit

Kita mencari penyebab asli bottleneck full-duplex dan timeout TX, bukan membuat speaker terlihat bekerja dengan menambah buffer atau meniru Repo4.
