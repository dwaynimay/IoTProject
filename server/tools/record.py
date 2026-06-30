import os
import sys
import time
import csv
from pathlib import Path

def main():
    print("=========================================")
    print(" DATASET RECORDER (Health Monitor)       ")
    print("=========================================")
    
    label = input("Masukkan nama aktivitas/label (contoh: jatuh, duduk, jalan): ").strip()
    if not label:
        label = "dataset"
        
    print(f"\nBersiap merekam dataset untuk label: {label.upper()}")
    print("Tekan ENTER untuk MULAI merekam...")
    input()
    
    # Kurangi 1 detik untuk buffer delay jaringan MQTT
    start_ms = int(time.time() * 1000) - 1000 
    print(f"\n[RECORDING] Merekam dimulai...")
    print(">> Lakukan aktivitas sekarang! <<")
    print("Tekan ENTER lagi untuk BERHENTI merekam...")
    input()
    
    # Tambah 1 detik untuk memastikan data terakhir masuk ke DB
    stop_ms = int(time.time() * 1000) + 1000
    print(f"\n[STOPPED] Merekam selesai.")
    
    project_root = Path(__file__).resolve().parent.parent.parent
    data_dir = project_root / "server" / "data" / "datasets"
    data_dir.mkdir(parents=True, exist_ok=True)
    
    filename = f"{label}_{start_ms}.csv"
    out_path = data_dir / filename
    
    print("\nMengekstrak data dari database...")
    # Default ke format stacked yang lebih umum untuk series
    export_script = project_root / "server" / "core" / "export_csv.py"
    db_path = project_root / "server" / "data" / "health_monitor.db"
    # Hanya sinyal IMU — abaikan 'ir' (PPG dummy yang dikirim record_imu firmware)
    cmd = (
        f'python "{export_script}" --db "{db_path}" --out "{out_path}" '
        f'--from-ms {start_ms} --to-ms {stop_ms} --format stacked '
        f'--signals ax ay az gx gy gz'
    )
    os.system(cmd)
    
    # Tambahkan kolom label jika file berhasil dibuat
    if out_path.exists():
        with open(out_path, 'r', newline='') as f:
            reader = list(csv.reader(f))
            
        if len(reader) > 1: # Punya data (bukan hanya header)
            print("Menambahkan kolom label ke CSV...")
            reader[0].append('label')
            for row in reader[1:]:
                row.append(label)
                
            with open(out_path, 'w', newline='') as f:
                writer = csv.writer(f)
                writer.writerows(reader)
                
            print(f"\n✅ Berhasil! Dataset tersimpan di: {out_path}")
            print(f"Total Baris Data: {len(reader)-1}")
        else:
            print(f"\n⚠️ File dibuat, tetapi KOSONG. Pastikan perangkat keras Anda sedang mengirimkan data ke database saat direkam.")
            out_path.unlink() # hapus file kosong
    else:
        print("\n❌ Gagal menyimpan CSV. Pastikan server/core/export_csv.py tersedia.")

if __name__ == "__main__":
    main()
