# Panduan Sumber Data Bab 5

Dokumen ini menjadi acuan bagi agent lain agar penyusunan Bab 5 dilakukan memakai sumber data yang benar, urut, dan tidak tertukar antarjenis pengujian. Fokus utamanya adalah membantu penulisan dari skema pengujian sampai analisis umum hasil pengujian.

## Tujuan Dokumen

Bab 5 pada proyek ini disusun dari beberapa kelompok pengujian yang berbeda fungsi. Karena nama folder pengujian mesh cukup mirip, agent lain berisiko mencampur antara data pembuktian relay mesh dan data QoS mesh. Dokumen ini menjelaskan sumber data utama, fungsi masing-masing sumber, file penting yang harus dibaca, dan urutan pemakaiannya dalam penulisan Bab 5.

## Lima Sumber Data Utama

### 1. Compressive Sensing IMU

Lokasi:
[server/data/cs_capture_20260717_025353](D:\Github\perbaikan\IoTProject\server\data\cs_capture_20260717_025353)

Fungsi:
Dipakai untuk subbab pengujian compressive sensing pada sinyal IMU. Dataset ini membandingkan sinyal asli dan sinyal hasil rekonstruksi pada enam sumbu, yaitu akselerometer `AX`, `AY`, `AZ` serta giroskop `GX`, `GY`, `GZ`.

File penting:
- `README_HASIL.txt`
- `summary_axis_mean.csv`
- `plot_overlay_combined.png`

Cara pakai:
- Ambil penjelasan umum jumlah window, ukuran window, dan konfigurasi pengujian dari `README_HASIL.txt`.
- Ambil nilai `RMSE`, `MAE`, `SNR`, dan `korelasi` tiap sumbu dari `summary_axis_mean.csv`.
- Gunakan `plot_overlay_combined.png` sebagai gambar overlay sinyal asli dan hasil rekonstruksi.

Catatan:
- Dataset ini dipakai untuk menunjukkan kualitas rekonstruksi CS pada domain IMU.
- Analisis yang tepat adalah membandingkan kualitas rekonstruksi antar sumbu, bukan membahas performa jaringan.

### 2. Compressive Sensing PPG

Lokasi:
[server/data/cs_ppg_capture_20260717_041643](D:\Github\perbaikan\IoTProject\server\data\cs_ppg_capture_20260717_041643)

Fungsi:
Dipakai untuk subbab pengujian compressive sensing pada sinyal PPG.

File penting:
- `README_HASIL_PPG.txt`
- `summary_metrics_ppg.csv`
- `plot_overlay_ppg.png`

Cara pakai:
- Ambil jumlah window, validitas data, dan gambaran umum hasil dari `README_HASIL_PPG.txt`.
- Ambil nilai rata-rata `RMSE`, `MAE`, `SNR`, dan `korelasi` dari `summary_metrics_ppg.csv`.
- Gunakan `plot_overlay_ppg.png` untuk memperlihatkan overlay sinyal asli dan hasil rekonstruksi.

Catatan:
- Dataset ini dipakai untuk menilai kestabilan rekonstruksi sinyal PPG.
- Jika menulis analisis, tekankan apakah bentuk utama sinyal masih terjaga setelah kompresi.

### 3. Pengujian Relay Mesh Berbasis Manipulasi RSSI

Lokasi:
[server/data/Backup_Pengujian_Mesh_Tes_Relay](D:\Github\perbaikan\IoTProject\server\data\Backup_Pengujian_Mesh_Tes_Relay)

Fungsi:
Dipakai untuk membuktikan mekanisme relay mesh ESP-NOW. Ini adalah sumber data yang benar untuk subbab pengujian perpindahan rute `direct -> relay -> direct`.

File penting:
- `README_HASIL_MESH.txt`
- `report.json`
- `summary_tx.csv`
- `summary_phase.csv`
- `summary_gateway.csv`

Cara pakai:
- Ambil angka ringkas seperti durasi, total transmisi, jumlah kecocokan route, success rate, dan distribusi route dari `report.json` atau `README_HASIL_MESH.txt`.
- Gunakan `summary_tx.csv` untuk membuktikan bahwa route aktual memang berubah antara `DIRECT` dan `RELAY`.
- Gunakan `summary_phase.csv` untuk menjelaskan fase pengujian `baseline_direct`, `forced_relay`, `relay_hold`, dan `direct_recovery`.
- Gunakan `summary_gateway.csv` bila ingin menjelaskan pengamatan dari sisi gateway.

Catatan penting:
- Folder ini bukan untuk QoS.
- Narasi yang benar pada dataset ini adalah pembuktian switching rute dan pemulihan rute saat RSSI dimanipulasi.
- Jangan mengambil latency rata-rata dari folder QoS lalu menuliskannya sebagai hasil uji relay.

### 4. Pengujian QoS Mesh

Lokasi:
[server/data/mesh_rssi_capture_20260717_054009](D:\Github\perbaikan\IoTProject\server\data\mesh_rssi_capture_20260717_054009)

Fungsi:
Dipakai untuk subbab performa jaringan mesh, seperti latency, packet loss, bandwidth, dan efisiensi data.

File penting:
- `README_HASIL_MESH.txt`
- `report.json`
- `summary_latency.csv`
- `summary_packet_loss.csv`
- `summary_bandwidth.csv`
- `summary_efficiency.csv`
- `summary_tx.csv`
- `summary_phase.csv`

Cara pakai:
- Ambil angka QoS utama dari `README_HASIL_MESH.txt` dan `report.json`.
- Gunakan file CSV khusus untuk tabel detail latency, packet loss, bandwidth, dan efisiensi.
- Bila perlu, pakai `summary_tx.csv` dan `summary_phase.csv` untuk memperlihatkan hubungan antara kondisi route dan hasil QoS.

Catatan penting:
- Folder ini dipakai untuk performa mesh, bukan untuk validasi mekanisme relay.
- Jika Bab 5 memisahkan “uji relay mesh” dan “uji QoS mesh”, maka folder ini harus diletakkan pada subbab berbeda dari `Backup_Pengujian_Mesh_Tes_Relay`.

### 5. Pengujian End-to-End

Lokasi:
[server/data/end_to_end_capture_20260717_060236](D:\Github\perbaikan\IoTProject\server\data\end_to_end_capture_20260717_060236)

Fungsi:
Dipakai untuk membuktikan bahwa data dari node sensor benar-benar melewati jalur mesh, diterima gateway, dipublish ke MQTT, dan diterima broker.

File penting:
- `README_HASIL_END_TO_END.txt`
- `report.json`

Cara pakai:
- Ambil jumlah transmisi dari node IMU dan node PPG.
- Ambil jumlah publish di gateway.
- Ambil jumlah payload yang diterima broker per topik.
- Gunakan data ini untuk menulis analisis keberhasilan aliran data dari sensor sampai backend.

Catatan:
- Dataset ini tidak dipakai untuk menilai kualitas rekonstruksi sinyal.
- Dataset ini juga tidak dipakai untuk membuktikan switching route secara detail.

## Urutan Penyusunan Bab 5

Urutan berikut direkomendasikan agar narasi Bab 5 runtut dan tidak meloncat.

### 5.1 Skema Pengujian Sistem

Gunakan seluruh sumber data sebagai dasar menjelaskan rancangan pengujian.

Urutan narasi yang disarankan:
- Pengujian CS IMU untuk melihat kualitas rekonstruksi data gerak.
- Pengujian CS PPG untuk melihat kualitas rekonstruksi data fisiologis.
- Pengujian relay mesh untuk membuktikan jalur komunikasi dapat berpindah sesuai kualitas link.
- Pengujian QoS mesh untuk menilai performa jaringan secara kuantitatif.
- Pengujian end-to-end untuk memastikan data benar-benar sampai ke broker MQTT.

Pada bagian ini, agent tidak perlu terlalu banyak angka. Fokusnya adalah menjelaskan tujuan pengujian, perangkat yang terlibat, alur data, dan hubungan antarpengujian.

### 5.2 Pengujian Compressive Sensing pada Sinyal IMU

Gunakan hanya sumber data IMU CS.

Hal yang harus diambil:
- Jumlah window
- Ukuran window
- Rasio kompresi
- Nilai `RMSE`, `MAE`, `SNR`, dan `korelasi` per sumbu
- Gambar overlay gabungan

Narasi analisis yang benar:
- Bahas sumbu terbaik dan terburuk.
- Jelaskan bahwa tiap sumbu dapat memiliki kualitas rekonstruksi berbeda.
- Hindari pembahasan tentang MQTT, RSSI, latency, atau route pada subbab ini.

### 5.3 Pengujian Compressive Sensing pada Sinyal PPG

Gunakan hanya sumber data PPG CS.

Hal yang harus diambil:
- Jumlah window
- Nilai rata-rata `RMSE`, `MAE`, `SNR`, dan `korelasi`
- Status validitas pembacaan `ppg_valid`
- Gambar overlay PPG

Narasi analisis yang benar:
- Jelaskan apakah bentuk utama sinyal masih terjaga.
- Kaitkan dengan kestabilan pembacaan PPG.
- Jangan campur dengan analisis routing mesh.

### 5.4 Pengujian ESP-NOW Mesh dengan Manipulasi RSSI

Gunakan hanya sumber data relay mesh dari `Backup_Pengujian_Mesh_Tes_Relay`.

Hal yang harus diambil:
- Durasi pengujian
- Total transmisi
- Jumlah paket dengan route aktual sesuai route harapan
- Success rate
- Jumlah kejadian `DIRECT` dan `RELAY`
- Fase `baseline_direct`, `forced_relay`, `relay_hold`, `direct_recovery`

Narasi analisis yang benar:
- Jelaskan bahwa penurunan RSSI memaksa sistem berpindah ke relay.
- Jelaskan bahwa saat kualitas link membaik, route kembali menjadi direct.
- Tegaskan bahwa hasil ini membuktikan mekanisme relay dan recovery route.

Kesalahan yang harus dihindari:
- Jangan memakai angka latency QoS dari folder lain untuk subbab ini.
- Jangan menyebut ini sebagai pengujian bandwidth atau efisiensi data.

### 5.5 Pengujian QoS Mesh

Gunakan hanya sumber data mesh QoS dari `mesh_rssi_capture_20260717_054009`.

Hal yang harus diambil:
- Latency rata-rata
- Packet loss
- Throughput atau bandwidth
- Efisiensi data
- Distribusi route direct dan relay jika mendukung analisis

Narasi analisis yang benar:
- Bahas kualitas layanan jaringan.
- Bahas dampak adanya route relay terhadap delay atau efisiensi.
- Posisi subbab ini adalah lanjutan setelah mekanisme relay terbukti bekerja.

### 5.6 Pengujian End-to-End

Gunakan hanya sumber data end-to-end.

Hal yang harus diambil:
- Total TX dari node IMU
- Total TX dari node PPG
- Total publish oleh gateway
- Total payload diterima broker
- Nama topik yang relevan

Narasi analisis yang benar:
- Buktikan integrasi sistem dari node hingga backend.
- Jelaskan bahwa data hasil kompresi tidak berhenti di jaringan lokal, tetapi diteruskan sampai broker.

### 5.7 Analisis Umum Hasil Pengujian

Bagian ini menggabungkan semua subbab sebelumnya.

Urutan analisis yang disarankan:
- CS IMU menunjukkan kualitas rekonstruksi yang cukup, tetapi belum seragam antar sumbu.
- CS PPG menunjukkan bentuk sinyal utama masih dapat dipertahankan.
- Relay mesh menunjukkan sistem mampu beradaptasi terhadap perubahan kualitas link.
- QoS mesh menunjukkan performa komunikasi dapat diukur secara kuantitatif.
- End-to-end menunjukkan integrasi node, jaringan mesh, gateway, dan broker telah berjalan.

Bagian ini adalah tempat untuk menyatukan seluruh pembuktian sistem, bukan mengulang tabel satu per satu.

## Aturan Praktis untuk Agent Lain

- Selalu baca `README` lebih dulu untuk memahami konteks dataset.
- Gunakan file `CSV` sebagai sumber utama tabel angka.
- Gunakan file `PNG` sebagai sumber utama gambar hasil pengujian.
- Jangan menukar folder relay mesh dengan folder QoS mesh.
- Jika menemukan angka direct dan relay yang besar, cek kembali apakah itu berasal dari folder QoS atau relay sebelum menulis.
- Untuk subbab CS, fokus analisis harus pada kualitas rekonstruksi sinyal.
- Untuk subbab mesh relay, fokus analisis harus pada switching route.
- Untuk subbab QoS, fokus analisis harus pada latency, packet loss, throughput, dan efisiensi.
- Untuk subbab end-to-end, fokus analisis harus pada keberhasilan aliran data sampai broker.

## Ringkasan Singkat Pemilihan Dataset

- Jika subbab membahas rekonstruksi sinyal IMU, gunakan `cs_capture_20260717_025353`.
- Jika subbab membahas rekonstruksi sinyal PPG, gunakan `cs_ppg_capture_20260717_041643`.
- Jika subbab membahas pembuktian relay mesh, gunakan `Backup_Pengujian_Mesh_Tes_Relay`.
- Jika subbab membahas QoS jaringan mesh, gunakan `mesh_rssi_capture_20260717_054009`.
- Jika subbab membahas aliran data sensor sampai MQTT broker, gunakan `end_to_end_capture_20260717_060236`.

Dengan mengikuti panduan ini, agent lain seharusnya dapat menyusun Bab 5 secara runtut dari skema pengujian sampai analisis umum tanpa salah memilih sumber data.
