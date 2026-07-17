# 4.3 Pengujian Skenario Routing ESP-NOW Mesh

Pengujian pada mekanisme routing ESP-NOW Mesh bertujuan untuk memvalidasi algoritma `DynamicRouter` dalam melakukan perpindahan rute adaptif antara mode *direct* dan *relay*. Pengujian ini difokuskan pada respons sistem ketika terjadi perubahan kualitas sinyal (RSSI) antar *node*. 

## 4.3.1 Skenario Pengujian

Untuk memastikan pengujian berjalan dengan konsisten dan terisolasi dari variabel lingkungan eksternal (seperti pantulan sinyal ruangan atau interferensi acak), pengujian tidak dilakukan dengan memindahkan *node* secara fisik. Pengujian dilakukan dengan memanipulasi nilai RSSI (*Received Signal Strength Indicator*) secara *software* (emulasi) di dalam *firmware* pengujian, yang merepresentasikan skenario seolah-olah sensor sedang dibawa berjalan menjauh atau mendekat ke *gateway*.

Terdapat tiga *node* yang dilibatkan dalam pengujian ini:
1. **Gateway** (Penerima akhir data)
2. **Node 1 (IMU)** sebagai pengirim data utama (*Sensor Node*).
3. **Node 2 (PPG)** sebagai perantara (*Relay Node*).

Sistem diatur untuk menjalankan empat fase perpindahan jarak secara siklikal berurutan, dengan setiap fase berdurasi 20 detik:
1. **Fase 1: Baseline Direct**
   Skenario ini mensimulasikan *Node 1* berada di dekat *Gateway*. Nilai RSSI mandiri (*self RSSI*) diatur kuat (-48 dBm), sementara RSSI tetangga ke *Gateway* diatur lebih lemah (-56 dBm). Ekspektasi: Paket dikirim langsung (*Direct*).
2. **Fase 2: Forced Relay**
   Skenario ini mensimulasikan *Node 1* dibawa berjalan menjauh dari *Gateway*, mendekati area *Node 2*. Nilai RSSI mandiri dijatuhkan menjadi lemah (-82 dBm), sedangkan RSSI *Node 2* ke *Gateway* kuat (-43 dBm). Ekspektasi: *DynamicRouter* mendeteksi penurunan link dan mengalihkan rute menjadi *Relay* melalui *Node 2*.
3. **Fase 3: Relay Hold**
   Skenario ini mensimulasikan *Node 1* bertahan di jarak yang jauh dari *Gateway*. RSSI mandiri sangat lemah (-86 dBm) dan RSSI tetangga tetap kuat (-41 dBm). Ekspektasi: Sistem menahan rute pada jalur *Relay*.
4. **Fase 4: Direct Recovery**
   Skenario ini mensimulasikan *Node 1* dibawa kembali berjalan mendekati *Gateway*. RSSI mandiri dipulihkan menjadi kuat (-50 dBm) dan RSSI tetangga ke *Gateway* melemah (-62 dBm). Ekspektasi: *DynamicRouter* mengembalikan jalur pengiriman menjadi *Direct*.

Pengulangan siklus fase ini dilakukan sebanyak 5 kali untuk membuktikan stabilitas algoritma *routing* saat *node* berpindah-pindah. Pada setiap iterasi, nilai RSSI disuntikkan secara dinamis dengan penambahan *jitter* acak (±3 dBm) agar fluktuasi sinyal alami di dunia nyata tetap dapat direpresentasikan.

## 4.3.2 Hasil Pengujian dan Analisis

Berdasarkan pengujian otomatis yang dijalankan secara siklikal selama lebih dari 7 menit (430 detik) yang merepresentasikan **5 kali perulangan skenario (percobaan)** secara utuh, sistem berhasil mengirimkan dan memproses total **132 paket data** saat terjadi fluktuasi sinyal.

**1. Analisis Keberhasilan Transisi Rute (*Routing Success Rate*)**
- **Total Pengujian Keputusan Rute:** 132 kejadian
- **Transisi Sesuai Ekspektasi (Sukses):** 130 kejadian
- **Tingkat Keberhasilan (*Success Rate*):** **98.48%**
- **Distribusi Rute:** 64 kali via *DIRECT*, 68 kali via *RELAY*

Tingkat keberhasilan **98.48%** membuktikan bahwa algoritma pemilihan rute berdasar nilai *threshold* selisih RSSI (Δ5 dBm) pada *DynamicRouter* berfungsi dengan sangat stabil dan berhasil menekan *route flapping* (loncatan rute yang berulang-ulang).

**2. Analisis Keandalan Pengiriman (*Packet Loss*)**
- **Paket Dikirim (TX):** 132 paket
- **Paket Diterima (RX Gateway):** 132 paket
- **Packet Loss:** **0.0%** (0 paket hilang)

Hasil *packet loss* 0% ini menunjukkan bahwa sistem *Mesh* berbasis ESP-NOW yang diimplementasikan (beserta penambahan mekanisme keandalan pengiriman) sangat tahan terhadap gangguan transmisi meskipun paket berpindah rute di tengah jalan.

**3. Analisis Latensi Jaringan (*Latency End-to-End*)**
- **Rata-Rata Latensi Keseluruhan:** **73.49 ms**
- **Rata-Rata Latensi Rute *Direct*:** **70.58 ms**
- **Rata-Rata Latensi Rute *Relay*:** **76.24 ms**

Penambahan waktu tempuh sebesar ~6 ms pada saat rute melewati Node 2 (*Relay*) dinilai sangat wajar dan masih sangat jauh di bawah batas toleransi transmisi data medis real-time, sehingga kinerja waktu nyatanya terbukti sangat baik.

**4. Analisis Efisiensi dan Bandwidth (*Compressive Sensing*)**
- **Ukuran *Raw Window*:** 256 bytes
- **Ukuran *CS Measurement*:** 128 bytes
- **Rasio Kompresi (*Data Saved*):** **50.0%**
- **Throughput Aktual:** **46.08 Bytes/second**

Penggunaan *Compressive Sensing* (CS) terbukti mampu mereduksi muatan paket hingga 50%, menekan beban *bandwidth*, dan secara langsung berkontribusi pada latensi rendah serta kemampuan mempertahankan *0% packet loss* pada jaringan *mesh* yang padat.
