HASIL UJI COMPRESSIVE SENSING
==============================
Port serial     : COM15
Baudrate        : 115200
Jumlah window   : 10
Panjang window  : 64 sampel
Jumlah y CS     : 32 pengukuran

File output utama:
- summary_metrics.csv      : metrik per-window per-axis
- summary_axis_mean.csv    : rata-rata metrik per-axis
- sample_comparison.csv    : data raw vs rekonstruksi per sampel
- compressed_measurements.csv : data pengukuran kompresi (y)
- plot_overlay_all_windows.png : overlay multi-panel per axis
- plot_overlay_combined.png : overlay gabungan semua axis
- plot_overlay_ax.png ... plot_overlay_gz.png : overlay terpisah tiap axis
- plot_metric_trends.png   : grafik tren RMSE/MAE/SNR/korelasi

Ringkasan rata-rata per-axis:
- AX: RMSE=0.2924, MAE=0.2524, SNR=8.89 dB, Korelasi=0.5660
- AY: RMSE=0.4061, MAE=0.3557, SNR=3.42 dB, Korelasi=0.5450
- AZ: RMSE=0.3108, MAE=0.2644, SNR=31.76 dB, Korelasi=0.6370
- GX: RMSE=7.3808, MAE=6.1998, SNR=8.74 dB, Korelasi=0.6039
- GY: RMSE=6.2874, MAE=5.3671, SNR=6.01 dB, Korelasi=0.5489
- GZ: RMSE=6.0054, MAE=5.1793, SNR=4.66 dB, Korelasi=0.5824

Saran pemakaian di buku TA:
1. Pakai summary_axis_mean.csv untuk tabel ringkasan utama.
2. Pakai summary_metrics.csv jika ingin tabel detail per-window.
3. Pakai plot_overlay_combined.png untuk satu gambar ringkasan keseluruhan.
4. Pakai plot_overlay_ax.png sampai plot_overlay_gz.png untuk analisis per-axis.
5. Pakai plot_metric_trends.png untuk analisis kestabilan performa CS.