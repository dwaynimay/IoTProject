# Pengujian ESP-NOW Mesh dengan Manipulasi RSSI Otomatis

Dokumen ini dipakai untuk membuktikan mekanisme routing `direct` dan `relay`
tanpa harus memindahkan node secara fisik. Ide utamanya adalah setiap node
menjalankan skenario RSSI terjadwal, sehingga perubahan jarak disimulasikan
oleh firmware.

## Tujuan

1. Membuktikan `DynamicRouter` dapat memilih jalur `direct` saat RSSI node
   pengirim ke gateway masih baik.
2. Membuktikan `DynamicRouter` beralih ke `relay` saat RSSI pengirim dibuat
   lebih buruk daripada RSSI tetangga ke gateway.
3. Membuktikan rute kembali ke `direct` saat kondisi RSSI dipulihkan.
4. Menghasilkan log serial yang bisa langsung dikonversi menjadi tabel hasil
   dan narasi pengujian di buku TA.

## Firmware yang Diunggah

- Gateway: env `test_mesh_auto_gateway`
- Node pengirim: env `test_mesh_auto_sensor_n1`
- Node relay: env `test_mesh_auto_sensor_n2`

Semua env memakai sketch yang sama:

- [auto_mesh_rssi_test.cpp](D:/Github/perbaikan/IoTProject/firmware/test_sketches/auto_mesh_rssi_test.cpp)

## Skema Fase Otomatis

Setiap fase berjalan `20 detik` dan berulang otomatis:

1. `baseline_direct`
   `self RSSI = -48 dBm`, `neighbor RSSI = -56 dBm`, ekspektasi `DIRECT`
2. `forced_relay`
   `self RSSI = -82 dBm`, `neighbor RSSI = -43 dBm`, ekspektasi `RELAY`
3. `relay_hold`
   `self RSSI = -86 dBm`, `neighbor RSSI = -41 dBm`, ekspektasi `RELAY`
4. `direct_recovery`
   `self RSSI = -50 dBm`, `neighbor RSSI = -62 dBm`, ekspektasi `DIRECT`

Interpretasi narasi:

- Fase 1 mewakili node masih dekat dengan gateway.
- Fase 2 dan 3 mewakili node menjauh dari gateway tetapi masih dekat dengan
  node relay.
- Fase 4 mewakili node kembali mendekat ke gateway.

## Arti Log Serial

### Log Node Pengirim

- `[PHASE]` menandakan fase uji aktif.
- `[PLAN]` menandakan RSSI report yang dikirim.
- `[TX]` adalah log paling penting karena memuat:
  - `phase`
  - `seq`
  - `expect`
  - `actual`
  - `self`
  - `neighbor`
  - `next_hop`
  - `ok`

Contoh interpretasi:

```text
[TX] node=1 scenario=1 phase=forced_relay seq=7 expect=RELAY actual=RELAY self=-82 neighbor=-43 next_hop=2 ok=1 t=43012
```

Artinya pada paket ke-7, routing yang diharapkan adalah `relay`, dan keputusan
aktual `DynamicRouter` juga `relay` melalui node 2.

### Log Node Relay

- `[PLAN]` menunjukkan node relay mengirim RSSI scripted ke node pengirim.
- `[RELAY]` menunjukkan paket dari node 1 diteruskan ke gateway.

### Log Gateway

- `[GW] route=DIRECT` berarti paket sampai langsung dari node pengirim.
- `[GW] route=RELAYED` berarti gateway menerima paket hasil forward dari node
  relay.

## Variabel yang Bisa Dicatat ke Tabel TA

1. Nomor fase
2. Nama fase
3. RSSI pengirim ke gateway
4. RSSI relay ke gateway
5. Rute yang diharapkan
6. Rute aktual di node pengirim
7. Status penerimaan di gateway
8. Kesesuaian hasil (`sesuai` atau `tidak sesuai`)

## Kesimpulan yang Ingin Dibuktikan

Jika log menunjukkan:

- fase `baseline_direct` dan `direct_recovery` menghasilkan `actual=DIRECT`
- fase `forced_relay` dan `relay_hold` menghasilkan `actual=RELAY`
- gateway menerima paket `DIRECT` pada fase direct dan `RELAYED` pada fase relay

maka sistem mesh dapat dinyatakan berhasil melakukan pemilihan rute adaptif
berdasarkan kualitas link RSSI, walaupun pengujian dilakukan tanpa perpindahan
fisik node.
