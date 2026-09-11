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

## Temuan runtime terbaru — commit `9171a91`

Pengujian hardware pada commit `9171a9148ba4bc0cab6e501ad8101b23696fd77d` memberikan bukti penting tentang bottleneck full-duplex.

### 1. Wi-Fi link terlihat sehat saat bottleneck terjadi

Diagnostic saat TX mulai mengalami tekanan:

```text
Wi-Fi DIAG: rssi=-53 dBm channel=5 second=0 bandwidth=1 phy=4 got_ip=1 disconnects=0 last_reason=0 ps=1 txpwr=80
```

Kesimpulan:

- RSSI `-53 dBm` menunjukkan link yang pada saat pengujian tergolong kuat.
- `got_ip=1` menunjukkan koneksi IP aktif.
- `disconnects=0` menunjukkan tidak ada putus Wi-Fi yang tercatat sebelum bottleneck.
- Karena itu, log ini **tidak mendukung Wi-Fi RF sebagai penyebab langsung** dari stall TX pada pengujian tersebut.
- Ini tidak membuktikan jaringan internet selalu sehat; hanya menyatakan kondisi link Wi-Fi pada saat snapshot tersebut tidak menunjukkan masalah yang jelas.

### 2. Bottleneck utama terlihat pada WebSocket/TCP TX

Payload audio uplink sekitar `4341 byte` membutuhkan waktu yang sangat tidak normal untuk dikirim:

```text
Transport TX lambat: 4183 ms len=4341 sent=4341
Transport TX lambat: 5028 ms len=4341 sent=0
```

Sebelum kegagalan juga terlihat beberapa send sekitar `150–400 ms` dan kemudian meningkat menjadi beberapa detik.

Kesimpulan:

**Jalur `websocket_send_text()` / transport di bawahnya dapat stall selama beberapa detik walaupun Wi-Fi link lokal pada saat itu terlihat sehat.**

Ini sekarang menjadi tersangka utama dan harus diaudit lebih dalam sebelum mengubah Audio HAL atau I2S.

### 3. TX queue penuh adalah akibat backpressure, bukan akar masalah yang sudah terbukti

Saat transport tertahan, sender tidak mampu mengirim pesan audio pada kecepatan produksi MIC:

```text
TX queue penuh; audio message tertua drop, terbaru dipertahankan
```

Queue Repo5 hanya sekitar 600 ms audio. Karena satu operasi send dapat berlangsung lebih dari 4 detik, queue pasti habis dan mulai membuang pesan lama.

Kesimpulan:

- TX queue bekerja sesuai desain: mempertahankan data terbaru agar latency tidak semakin tua.
- Membesarkan TX queue sekarang hanya akan menunda gejala dan meningkatkan memory/buffer pressure.
- Queue penuh diperlakukan sebagai **gejala network/transport backpressure**, bukan solusi yang harus ditutup dengan queue lebih besar.

### 4. MIC akhirnya ikut mengalami backpressure

Setelah WebSocket gagal:

```text
Conversation MIC queue penuh; frame drop total=1
Conversation MIC queue penuh; frame drop total=65
Conversation MIC queue penuh; frame drop total=129
...
```

Ini menunjukkan efek berantai:

```text
Transport TX stall
    -> TX sender tertahan
    -> TX queue penuh
    -> audio message drop
    -> jalur conversation kehilangan kemampuan mengalirkan audio
```

Setelah transport disconnect, MIC queue terus penuh karena tidak ada lagi jalur uplink yang dapat mengonsumsi audio secara normal.

### 5. WebSocket akhirnya disconnect setelah write stall

Kegagalan final:

```text
transport_ws: Error transport_poll_write(0)
websocket_client: esp_transport_write() returned 0
WS_TRANSPORT: WebSocket ERROR
WS_TRANSPORT: WebSocket DISCONNECTED
```

Operasi terakhir tercatat:

```text
Transport TX lambat: 5028 ms len=4341 sent=0
Transport TX hasil tidak lengkap: elapsed=5028ms requested=4341 sent=0
```

Kesimpulan:

**Timeout 5 detik pada operasi TX tercapai tanpa progress dan akhirnya menyebabkan kegagalan transport/WebSocket.**

### 6. Speaker/I2S bukan fokus perubahan berikutnya

Pada pengujian ini tidak muncul lagi:

```text
I2S speaker write fail
Speaker PCM write tidak lengkap
```

Gemini juga sempat mengirim PCM audio dan playback dimulai.

Kesimpulan:

- Kontrak write speaker yang sudah dikoreksi tidak perlu disentuh lagi berdasarkan log ini.
- Konfigurasi I2S/DMA speaker jangan diubah hanya karena masalah TX WebSocket.
- Audio HAL/I2S tetap dianggap **known-good relatif terhadap gejala yang sedang diaudit**.

### 7. RX pressure tetap ada, tetapi belum terbukti sebagai akar masalah utama

RX worker sebelumnya sudah diperbesar menjadi 10 buffer. Pada pengujian ini tekanan RX dapat muncul bersamaan dengan TX overload, tetapi bukti terkuat tetap berada pada TX transport.

Karena itu:

- Jangan langsung membuat RX worker kedua.
- Jangan terus memperbesar RX pool tanpa bukti baru.
- Jika nanti RX `free=0` bertahan secara konsisten sementara TX transport normal, barulah RX processing layak diaudit sebagai bottleneck independen.

## Kesimpulan sementara yang harus dicatat

### Status diagnosis

```text
Wi-Fi RF/link       : TIDAK TERBUKTI sebagai akar masalah pada test ini
AudioEngine MIC     : bekerja sampai backpressure dari uplink terjadi
TX queue            : bekerja, tetapi penuh karena transport stall
WebSocket transport : TERSANGKA UTAMA bottleneck
RX worker           : mendapat pressure, kemungkinan efek gabungan
Speaker/I2S         : tidak menunjukkan timeout pada test ini
```

### Rantai kejadian yang paling didukung log

```text
MIC menghasilkan audio
        ↓
AudioEngine
        ↓
TX queue
        ↓
WebSocket send_text()
        ↓
STALL 0.1–5 detik
        ↓
TX queue penuh
        ↓
oldest message drop
        ↓
WebSocket transport gagal
        ↓
disconnect
        ↓
MIC queue penuh / frame drop
```

**Belum boleh disebut sebagai root cause final.** Root cause yang sudah terbukti baru sebatas: **transport TX mengalami stall ekstrem hingga sekitar 5 detik dan menyebabkan cascading backpressure.** Penyebab stall di bawah `esp_websocket_client` / TLS / TCP masih harus dibuktikan.

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
6. Aktifkan separate WebSocket TX lock dan pertahankan timeout TX lock 5000 ms. **Sudah diterapkan.**
7. Tambahkan diagnostic Wi-Fi link dan TX queue age. **Sudah diterapkan.**
8. Build/CI. **Commit `9171a91` PASS.**
9. Flash dan ambil runtime evidence. **Selesai; log menghasilkan temuan TX stall ekstrem.**
10. Audit lapisan di bawah `websocket_send_text()`: transport write, TLS/TCP, dan kemungkinan network-layer stall. **Langkah berikutnya.**
11. Ubah satu variabel saja setelah bukti transport cukup kuat.

## Target audit

Kita mencari penyebab asli bottleneck full-duplex dan timeout TX, bukan membuat speaker terlihat bekerja dengan menambah buffer atau meniru Repo4.
