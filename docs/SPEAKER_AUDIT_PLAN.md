# Repo5 Speaker Audit — Working Concept

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

## Ownership

- WebSocket: transport saja. Tidak mengolah I2S dan tidak memiliki keputusan audio.
- Gemini audio parser: decode/protokol audio lalu menyerahkan PCM ke AudioEngine.
- AudioEngine: pemilik playback queue, playback task, dan lifecycle audio playback.
- Audio HAL: pemilik hardware I2S TX/RX dan operasi read/write hardware.
- AEC: belum dikerjakan sampai jalur dasar stabil.

## Masalah yang sedang diaudit

Runtime Repo5 menunjukkan `i2s_channel_write()` menghasilkan partial write dan `ESP_ERR_TIMEOUT` saat Gemini audio sedang diputar. Eksperimen pemecahan write menjadi 240 sample terbukti memperburuk audio karena partial timeout langsung mengakhiri satu operasi HAL.

## Hipotesis yang diuji

1. Kontrak `audio_hal_write_pcm()` terhadap partial write/timeout.
2. Interaksi I2S TX writer dengan DMA descriptor dan DMA event/ISR.
3. Scheduling/interrupt pressure saat WiFi/TLS/Gemini RX/decode berjalan bersamaan.
4. Lifecycle TX channel: init -> enable -> write -> disable.
5. Clock/slot configuration TX 24 kHz, 32-bit, mono LEFT.
6. CPU runtime dan kemungkinan scheduling pressure.

## Yang tidak boleh dilakukan pada tahap ini

- Jangan menyalin implementasi Repo4.
- Jangan mengubah WakeWord/MIC yang sudah stabil.
- Jangan mengubah WebSocket TX queue.
- Jangan memperbesar RX pool tanpa bukti.
- Jangan menambah task/queue baru tanpa kebutuhan runtime.
- Jangan mengerjakan AEC.
- Jangan menyimpulkan chunk 240 sebagai solusi.

## Kontrak playback Repo5

AudioEngine playback task boleh blocking menunggu hardware karena WebSocket sudah dipisahkan oleh playback queue.

`audio_hal_write_pcm()` harus menjaga seluruh PCM block tetap utuh. Jika driver menerima sebagian data, offset harus dilanjutkan dari jumlah aktual yang diterima. Sisa PCM tidak boleh dibuang hanya karena satu `ESP_ERR_TIMEOUT` terjadi setelah partial progress.

## Urutan kerja

1. Kembalikan eksperimen 240-sample yang terbukti buruk.
2. Pertahankan konfigurasi I2S/DMA Repo5 yang ada.
3. Koreksi kontrak write agar partial progress tidak kehilangan PCM.
4. Build/CI.
5. Flash hanya setelah build bersih.
6. Ambil runtime log speaker.
7. Jika timeout masih terjadi, audit DMA/ISR/scheduling berdasarkan bukti baru.
8. Baru ubah satu variabel pada satu waktu.

## Target audit

Kita mencari penyebab asli timeout TX, bukan membuat speaker terlihat bekerja dengan menambah buffer atau meniru Repo4.
