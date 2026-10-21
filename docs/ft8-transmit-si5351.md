# Kế hoạch phát FT8 bằng Si5351 cho ESP32 + SI4732

Mục tiêu: radio tự trả lời FT8 (CQ → báo cáo → RR73) ở công suất QRP, phát sạch hài, có chuyển
thu/phát an toàn cho SI4732.

## Vì sao không dùng APLL của ESP32

Đã thử (2026-10-18) xuất APLL qua I2S0 MCLK ra GPIO0:

- Bước tần nhỏ nhất ở 7 MHz là ~8.7 Hz, ở 28 MHz là ~34 Hz, trong khi FT8 cần khoảng cách tone đúng
  6.25 Hz. Muốn đạt 6.25 Hz phải dithering giữa hai nấc kề nhau ở tần số cao.
- Mỗi lần ghi hệ số APLL đều làm pha giật một chút, nên dithering biến thành nhiễu "click" băng rộng.
  Cả SI4732 lẫn RTL-SDR đều không thấy tone FSK, chỉ thấy sóng mang.
- Sóng vuông lấy thẳng từ GPIO bị dòng WiFi điều chế, sinh lược vạch phụ cách 100 Hz, và không có
  lọc hài.

Si5351 giải quyết cả ba vấn đề: độ phân giải multisynth ~0.01 Hz, đổi tần không giật, có đầu ra
50 Ω riêng.

## 1. Phần cứng

| Khối | Đề xuất | Ghi chú |
|---|---|---|
| Bộ tạo tần | Module Si5351A (thạch anh 25/27 MHz, tốt nhất loại TCXO) | I2C địa chỉ 0x60 |
| Đệm / PA | Giai đoạn 1: 3× BS170 song song kiểu QRP Labs (5 V, ~0.5–1 W). Giai đoạn 2: IRF510 / 12 V (~5 W) | Nguồn PA tách riêng khỏi nguồn ESP32 |
| Lọc thông thấp | LPF 7 phần tử theo băng (ví dụ bộ kit LPF của QRP Labs cho 40 m / 20 m) | Bắt buộc, hài ≤ −43 dBc |
| Chuyển T/R | Relay tín hiệu 2 tiếp điểm (ví dụ Omron G5V-2) qua transistor + diode dập | Ngắt hẳn đầu vào SI4732 khi phát |
| Bảo vệ đầu vào RX | Cặp diode 1N4148 đấu ngược + điện trở 47 Ω nối tiếp trước chân AMI | Phòng relay chập chờn |
| Tải giả | 50 Ω ≥ 2 W (hai trở 100 Ω/1 W song song) | Dùng cho mọi bước thử đầu tiên |

### Nối dây

| ESP32 | Tới | Ghi chú |
|---|---|---|
| GPIO13 (SDA), GPIO14 (SCL) | Si5351 SDA/SCL | Dùng chung bus với SI4732 (SI4732 ở 0x11/0x63, không trùng 0x60) |
| 3V3, GND | Si5351 VIN, GND | Thêm tụ 10 µF + 100 nF sát module |
| GPIO26 | Transistor điều khiển relay T/R | HIGH = phát |
| GPIO19 | Bật PA (MOSFET cấp nguồn/bias) | HIGH = PA bật, mặc định LOW khi khởi động |
| — | Si5351 CLK0 → PA → LPF → relay → anten | Cáp đồng trục ngắn |
| — | Relay (NC) → SI4732 AMI | Lúc nghỉ relay nối anten với máy thu |
| (tuỳ chọn) | Si5351 CLK2 → RCLK của SI4732 | Thay thạch anh 32.768 kHz trôi nhiều (đo được ~35 ppm/giờ) |

Không dùng GPIO0, GPIO2, GPIO12 vì là chân strapping lúc boot. Các chân còn trống: 19, 21, 22, 26.

## 2. Firmware

1. **Driver Si5351**: dùng thư viện Etherkit Si5351 hoặc tự viết bản tối giản. Cố định PLL (ví dụ
   PLLA ~ 700–900 MHz), chỉ đổi phần phân số của MultiSynth0 khi đổi tone; mỗi tone ghi 8 thanh ghi
   qua I2C 400 kHz (~0.3 ms).
2. **Bộ phát FT8 (`Ft8Tx.cpp`)**:
   - Mã hoá bằng `ftx_message_encode` + `ft8_encode` của ft8_lib (đã có trong firmware).
   - Bắt đầu tại giây 0.5 của slot (DT = 0) theo đồng hồ SNTP hiện có; 79 symbol × 160 ms qua
     `esp_timer`.
   - Tần phát = dial + tần audio chọn được (mặc định 1500 Hz, hoặc chỗ trống trên waterfall).
   - Tránh tranh I2C với SI4732: mọi lệnh I2C chạy từ cùng một task, hoặc dùng mutex.
3. **Trình tự T/R**: tắt AGC/âm thanh RX → relay sang TX → chờ 10 ms → bật PA → bật CLK0 → phát →
   tắt CLK0 → tắt PA → chờ 10 ms → relay về RX. Bộ giải mã bỏ qua slot đang phát.
4. **Tự động QSO**: máy trạng thái trên danh sách gói đã giải: CQ → (gọi lại) → báo cáo → R-report →
   RR73 → 73. Có nút "Halt TX" trên LCD, web và serial; có giới hạn số lần gọi lại.
5. **Giao diện**: web/LCD thêm ô hô hiệu + grid, nút CQ, chọn tần audio TX, hiển thị trạng thái TX.
   Hô hiệu lưu trong NVS; không cho phát khi chưa nhập hô hiệu.
6. **Hiệu chỉnh**: ppm của Si5351 đo bằng RTL-SDR/OpenWebRX (đã có đường đo) hoặc từ độ lệch trung
   bình các trạm FT8 giải được.

## 3. Các bước thử

1. Si5351 phát sóng mang vào tải giả; dùng RTL-SDR (OpenWebRX trên Pi) đo tần và độ sạch: không được
   có lược vạch 100 Hz.
2. Phát FT8 vào tải giả ở mức thấp, để OpenWebRX/jt9 giải mã được; đồng thời kiểm tra radio ESP32
   bỏ qua slot đang phát.
3. Kiểm tra relay T/R: SI4732 vẫn thu bình thường ngay slot sau khi phát, không bị treo/hỏng đầu vào.
4. Kiểm hài sau LPF bằng RTL-SDR ở 2×/3× tần phát.
5. Nối anten, phát CQ ngắn có hô hiệu, xác nhận trên PSKReporter.
6. Bật tự động QSO.

## 4. Lưu ý pháp lý và an toàn

- Chỉ phát khi có chứng chỉ vô tuyến nghiệp dư, trong băng được phép; mọi bản tin phải có hô hiệu.
- Luôn có LPF trước anten; không phát khi chưa nối anten hoặc tải giả.
- Nguồn PA tách riêng, có cầu chì; ESP32 và SI4732 dùng nguồn sạch để tránh nhiễu ngược vào máy thu.
