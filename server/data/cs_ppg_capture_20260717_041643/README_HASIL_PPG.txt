HASIL UJI COMPRESSIVE SENSING PPG
=================================
Port serial     : COM3
Baudrate        : 115200
Jumlah window   : 50
Panjang window  : 64 sampel
Jumlah y CS     : 32 pengukuran

Ringkasan rata-rata:
- RMSE rata-rata      : 8.5797
- MAE rata-rata       : 7.3734
- SNR rata-rata       : 79.98 dB
- Korelasi rata-rata  : 0.7585

File output utama:
- summary_metrics_ppg.csv
- sample_comparison_ppg.csv
- compressed_measurements_ppg.csv
- plot_overlay_ppg.png
- plot_metric_trends_ppg.png

Saran pemakaian di TA:
1. summary_metrics_ppg.csv untuk tabel metrik utama.
2. plot_overlay_ppg.png untuk gambar raw vs rekonstruksi.
3. plot_metric_trends_ppg.png untuk menunjukkan kestabilan performa.
4. Tambahkan keterangan apakah ppg_valid dominan true atau false.