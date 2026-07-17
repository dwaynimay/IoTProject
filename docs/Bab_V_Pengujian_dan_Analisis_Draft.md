# BAB 5 PENGUJIAN DAN ANALISIS

## 5.1 Skema Pengujian Sistem

Bab ini menyajikan pengujian pada empat aspek utama, yaitu pengujian compressive sensing untuk sinyal IMU, pengujian compressive sensing untuk sinyal PPG, pengujian ESP-NOW mesh berbasis manipulasi RSSI, dan pengujian end-to-end dari node sensor hingga broker MQTT. Seluruh pengujian dilakukan menggunakan skenario otomatis agar setiap fase dapat diulang secara konsisten, sehingga hasil yang diperoleh dapat dibandingkan secara objektif antarpercobaan.

Pada pengujian compressive sensing, data yang dianalisis berasal dari keluaran firmware uji pada node IMU dan node PPG yang direkam melalui serial, kemudian direkonstruksi di sisi komputer untuk menghitung metrik RMSE, MAE, SNR, dan korelasi. Pada pengujian mesh, nilai RSSI dimanipulasi secara terkontrol untuk mensimulasikan perubahan kualitas link tanpa memindahkan node secara fisik. Selanjutnya, pengujian end-to-end digunakan untuk memverifikasi bahwa data hasil kompresi dapat melewati jalur mesh, diterima gateway, dikonversi menjadi payload MQTT, dan berhasil diteruskan ke broker.

## 5.2 Proses Pengujian dan Analisis Hasil

### 5.2.1 Pengujian Compressive Sensing pada Sinyal IMU

Pengujian compressive sensing pada sinyal IMU dilakukan dengan membandingkan sinyal asli dan sinyal hasil rekonstruksi pada enam sumbu, yaitu akselerometer sumbu x, y, z serta giroskop sumbu x, y, z. Data uji yang dianalisis terdiri atas 10 window, dengan panjang window 64 sampel dan jumlah pengukuran compressed sensing sebanyak 32 koefisien pada setiap window. Dengan konfigurasi tersebut, rasio kompresi yang diterapkan adalah 50% dari panjang sinyal awal.

Berdasarkan hasil perhitungan, sumbu AZ memberikan kualitas rekonstruksi terbaik dengan nilai SNR sebesar 31,76 dB dan korelasi 0,6370. Sementara itu, sumbu AY menghasilkan performa terendah dengan nilai SNR 3,42 dB dan korelasi 0,5450. Pada domain giroskop, ketiga sumbu masih menunjukkan pola rekonstruksi yang mengikuti bentuk sinyal asli, walaupun nilai error absolutnya lebih besar daripada domain akselerometer. Secara umum, hasil ini menunjukkan bahwa metode compressive sensing yang digunakan sudah mampu mempertahankan informasi utama sinyal IMU, tetapi tingkat kesetiaannya masih berbeda untuk tiap sumbu.

### 5.2.2 Pengujian Compressive Sensing pada Sinyal PPG

Pengujian compressive sensing pada sinyal PPG dilakukan pada 50 window data, dengan panjang window 64 sampel dan jumlah pengukuran compressed sensing sebanyak 32 koefisien. Evaluasi dilakukan menggunakan metrik RMSE, MAE, SNR, dan korelasi, serta divisualisasikan melalui grafik overlay antara sinyal asli dan hasil rekonstruksi.

Hasil pengujian menunjukkan bahwa rekonstruksi sinyal PPG memiliki nilai RMSE rata-rata 8,5797, MAE rata-rata 7,3734, SNR rata-rata 79,98 dB, dan korelasi rata-rata 0,7585. Selain itu, seluruh window yang dianalisis memiliki status ppg_valid bernilai true, sehingga data yang dipakai pada pengujian ini berada pada kondisi pembacaan yang valid. Dengan demikian, pendekatan compressive sensing pada sinyal PPG dapat dinilai cukup stabil untuk mempertahankan bentuk utama sinyal, terutama jika dilihat dari nilai SNR yang tinggi dan korelasi yang berada di atas 0,75.

### 5.2.3 Pengujian ESP-NOW Mesh dengan Manipulasi RSSI

Pengujian mesh relay dilakukan dengan memanipulasi nilai RSSI secara terkontrol untuk memaksa perpindahan rute dari direct ke relay, kemudian mengamati apakah sistem dapat bertahan pada jalur relay dan kembali lagi ke jalur direct saat kualitas link membaik. Skenario otomatis dijalankan selama 430 detik dengan empat fase utama, yaitu baseline_direct, forced_relay, relay_hold, dan direct_recovery.

Berdasarkan hasil pengujian, dari 29 transmisi yang diuji terdapat 28 paket yang route aktualnya sesuai dengan route yang diharapkan, sehingga tingkat keberhasilan perpindahan rute mencapai 96,55%. Distribusi keputusan routing juga seimbang, yaitu 15 kali direct dan 14 kali relay. Data fase menunjukkan bahwa saat nilai RSSI node pengirim diturunkan hingga kisaran -80 dBm sampai -89 dBm, paket beralih ke relay melalui node perantara. Setelah RSSI direct diperbaiki kembali ke kisaran sekitar -48 dBm sampai -53 dBm, rute kembali menggunakan jalur direct. Hasil ini menunjukkan bahwa mekanisme mesh yang dibangun telah mampu melakukan switching rute dan pemulihan rute secara adaptif sesuai perubahan kualitas link.

### 5.2.4 Pengujian End-to-End Sistem

Pengujian end-to-end digunakan untuk memastikan bahwa data hasil kompresi tidak hanya berhasil dikirim antarnode, tetapi juga benar-benar dipublikasikan oleh gateway ke broker MQTT. Pada skenario ini, node IMU menghasilkan 132 paket cs_imu, node PPG menghasilkan 132 paket cs_ppg, gateway mencatat 260 publish MQTT yang berhasil, dan broker menerima total 260 payload pada dua topik utama sistem.

Hasil tersebut memperlihatkan bahwa sistem sudah mampu mengalirkan data hasil kompresi dari kedua node menuju broker MQTT melalui gateway. Broker menerima 130 payload cs_imu dan 130 payload cs_ppg, sehingga total paket yang tercatat di broker adalah 260 payload. Jika dibandingkan dengan total transmisi awal dari kedua node, tingkat keberhasilan end-to-end berada pada kisaran 98,48%. Selisih empat paket antara sisi pengirim dan broker dapat diinterpretasikan sebagai kehilangan yang terjadi pada fase awal atau akhir pengujian otomatis, namun secara umum aliran data utama tetap berjalan stabil dan konsisten.

### 5.2.5 Analisis Umum Hasil Pengujian

Secara keseluruhan, hasil pengujian menunjukkan bahwa sistem telah memenuhi fungsi utama yang dirancang. Pengujian compressive sensing membuktikan bahwa sinyal IMU dan PPG masih dapat direkonstruksi dengan karakteristik utama yang tetap terjaga setelah kompresi 50%. Pengujian mesh berbasis RSSI membuktikan bahwa mekanisme routing dapat berpindah dari jalur direct ke relay dan kembali lagi sesuai kualitas link. Selanjutnya, pengujian end-to-end memperlihatkan bahwa data hasil kompresi dapat diterima gateway dan diteruskan ke broker MQTT dengan tingkat keberhasilan yang tinggi.

Berdasarkan hasil tersebut, sistem dapat dinyatakan berhasil mengintegrasikan mekanisme akuisisi data, compressive sensing, routing mesh, dan distribusi data ke backend dalam satu alur kerja yang utuh. Walaupun demikian, hasil pengujian IMU menunjukkan bahwa kualitas rekonstruksi antar sumbu belum seragam, sehingga masih terdapat ruang perbaikan pada konfigurasi kompresi atau metode rekonstruksi agar fidelitas sinyal dapat ditingkatkan pada seluruh kanal pengukuran.
