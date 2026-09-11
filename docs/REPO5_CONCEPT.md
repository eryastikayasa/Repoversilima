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

WebSocket adalah **transport/kurir**.

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

## 10. Prinsip Kelengkapan Audio dan Toleransi Delay

Repo5 **memprioritaskan kelengkapan data audio di atas latency**.

Prinsip ini berlaku untuk dua arah.

### Uplink

```text
MIC
 ↓
AudioEngine
 ↓
WebSocket
 ↓
Gemini
```

Audio microphone yang sudah menjadi bagian dari percakapan tidak boleh sengaja dipotong atau dibuang hanya untuk mengejar latency rendah.

### Downlink

```text
Gemini
 ↓
WebSocket
 ↓
AudioEngine
 ↓
Speaker
```

Audio PCM dari Gemini tidak boleh dipotong atau dibuang hanya karena sistem ingin segera kembali ke state berikutnya.

### Prioritas

Urutan prioritas Repo5:

1. Audio lengkap dan tidak korup.
2. Tidak ada deadlock, overflow, atau kehilangan data karena desain yang salah.
3. Lifecycle tetap benar.
4. Baru kemudian optimasi latency.

Jika terjadi konflik:

```text
AUDIO COMPLETENESS > LOW LATENCY
```

Delay jaringan beberapa detik **dapat diterima** jika diperlukan untuk menjaga audio tetap utuh dan sistem tetap sehat.

Prinsip ini **bukan** berarti retry tanpa batas atau buffering tanpa batas. Buffer tetap harus memiliki batas yang masuk akal, aturan timeout yang jelas, dan aturan kegagalan yang terukur. Yang dilarang adalah membuang audio valid hanya untuk mengejar realtime.

### Konsekuensi audit TX

Queue uplink harus diaudit terhadap:
- bitrate audio;
- kapasitas queue;
- lama backlog yang dapat ditahan;
- perilaku ketika network lebih lambat daripada producer;
- apakah frame lama dibuang;
- apakah frame baru dibuang;
- apakah kegagalan send menyebabkan kehilangan audio;
- timeout dan retry;
- ownership data antara capture dan sender.

Jika queue penuh lalu audio valid dibuang, itu adalah **FAIL** terhadap prinsip kelengkapan audio, walaupun sistem terasa lebih realtime.

### Konsekuensi audit RX

Jalur downlink harus diaudit terhadap:
- kehabisan buffer payload;
- queue RX penuh;
- kegagalan enqueue;
- data dibebaskan sebelum selesai diproses;
- kegagalan Base64 decode;
- playback queue penuh;
- PCM yang tidak seluruhnya diteruskan ke AudioEngine;
- reconnect yang memutus audio yang masih valid.

### `turnComplete` bukan otomatis playback selesai

```text
Gemini turnComplete
        ↓
masih mungkin ada PCM di AudioEngine
        ↓
Playback Queue
        ↓
Audio HAL / I2S / DMA
        ↓
Speaker selesai
```

Karena itu event protokol `turnComplete` harus dibedakan dari event audio `playback drained`.

Sistem tidak boleh memotong audio hanya karena `turnComplete` sudah diterima.

## 11. Prinsip Lifecycle Percakapan

Lifecycle harus membedakan:

```text
TURN END
```

dengan:

```text
CONVERSATION / SESSION END
```

`turnComplete` pada Gemini pada dasarnya adalah event akhir turn. Jangan otomatis menganggap event tersebut berarti:

- WebSocket harus disconnect;
- AudioEngine conversation harus stop;
- WakeWord harus restart.

Jika produk memang menggunakan pola:

```text
WakeWord
 ↓
1 pertanyaan
 ↓
1 jawaban
 ↓
Idle
```

maka `turnComplete + playback drained` dapat menjadi kandidat kondisi untuk mengakhiri conversation. Tetapi keputusan tersebut harus berasal dari kontrak produk, bukan asumsi dari event `turnComplete` saja.

Jika produk mendukung percakapan berkelanjutan:

```text
WakeWord
 ↓
Conversation
 ↓
Turn 1
 ↓
Turn 2
 ↓
Turn 3
 ↓
...
```

maka conversation tetap aktif setelah `turnComplete`.

### Audit lifecycle wajib memeriksa

- siapa yang memulai conversation;
- siapa yang mengakhiri conversation;
- apa arti `turnComplete`;
- kapan playback benar-benar selesai;
- kapan `wsaudio` boleh stop;
- kapan AudioEngine boleh stop conversation;
- kapan WebSocket boleh disconnect;
- kapan WakeWord boleh restart;
- apakah ada tombol/command explicit stop;
- apakah reconnect dapat terjadi saat audio masih pending.

## 12. State yang Harus Dibedakan

Minimal secara konseptual:

```text
WebSocket Connected
Gemini Ready
Conversation Active
Turn Active
Playback Active
Playback Drained
Session Ending
```

Tidak semua state harus menjadi enum atau layer baru. Yang penting perilakunya tidak tercampur.

Contoh penting:

```text
Gemini Ready != Conversation Active
Conversation Active != Turn Active
Turn Complete != Playback Drained
Playback Drained != WebSocket Disconnected
```

## 13. Prinsip Error dan Network Lambat

Network lambat adalah kondisi yang harus dapat ditangani tanpa merusak audio.

Contoh:

```text
MIC menghasilkan audio
        ↓
network lambat
        ↓
backlog bertambah
        ↓
network mengejar backlog
        ↓
audio tetap dikirim lengkap
```

Namun sistem tidak boleh memiliki buffer tak terbatas. Jika kapasitas maksimum tercapai, failure mode harus eksplisit dan dapat diaudit. Jangan diam-diam membuang bagian percakapan.

Untuk downlink, prinsip yang sama berlaku:

```text
Gemini menghasilkan PCM
        ↓
network / parser menerima data
        ↓
AudioEngine menampung
        ↓
playback menguras buffer
        ↓
speaker selesai
```

Jika terjadi tekanan buffer, harus diketahui tepatnya di mana data gagal diterima dan mengapa.

## 14. Hal yang Tidak Dilakukan Sekarang

Sebelum jalur dasar stabil, jangan:

- menambahkan AEC;
- menambahkan pemrosesan audio baru;
- menambah queue tanpa bukti;
- menambah worker hanya untuk mengikuti Repo4;
- memindahkan ownership audio ke WebSocket;
- mengubah baseline WakeWord/MIC yang sudah terbukti;
- mengubah banyak layer sekaligus;
- mengorbankan kelengkapan audio hanya demi latency rendah.

## 15. Aturan Audit untuk Perubahan Berikutnya

Setiap perubahan pada jalur audio/network harus menjawab:

### Uplink
- Apakah seluruh audio MIC dapat sampai ke Gemini?
- Apakah ada frame yang sengaja dibuang?
- Apa yang terjadi saat network lambat?
- Apa yang terjadi saat send gagal?
- Apakah queue dapat penuh?
- Jika penuh, apakah audio hilang?

### Downlink
- Apakah seluruh PCM Gemini dapat sampai ke AudioEngine?
- Apakah ada payload yang dibuang?
- Apakah playback queue dapat penuh?
- Apakah partial write ditangani?
- Apakah `turnComplete` dapat menyebabkan audio terpotong?
- Apakah playback benar-benar drained sebelum lifecycle berikutnya?

### Lifecycle
- Siapa owner setiap state?
- Apakah turn end dicampur dengan session end?
- Apakah disconnect terjadi saat audio masih pending?
- Apakah WakeWord restart terlalu cepat?

### Resource
- Apakah queue/task memiliki kapasitas yang masuk akal?
- Apakah stack cukup?
- Apakah ada blocking call?
- Apakah ada retry tanpa batas?
- Apakah ada race start/stop?

Prinsip kerja tetap:

```text
AUDIT DULU
 ↓
BUKTIKAN MASALAH
 ↓
UBAH SESUAI KEBUTUHAN
 ↓
BUILD
 ↓
RUNTIME TEST
 ↓
LOCK JIKA PASS
```

Jangan menambahkan kode hanya karena secara teori terlihat lebih aman. Perubahan harus memiliki alasan teknis yang dapat diuji.

## 16. Definisi Sederhana Repo5

Repo5 harus tetap dapat dijelaskan dengan kalimat berikut:

> **Audio Engine memiliki audio. WebSocket hanya mengantar audio. Audio HAL hanya berbicara dengan hardware. Gemini hanya menangani protokol/service. Kelengkapan audio lebih penting daripada mengejar latency rendah. Delay jaringan beberapa detik dapat diterima jika diperlukan untuk menjaga audio tetap utuh dan sistem tetap sehat.**

Jika suatu perubahan membuat boundary tersebut kabur, atau menyebabkan audio valid dibuang demi latency, perubahan tersebut harus diaudit ulang sebelum diterapkan.
