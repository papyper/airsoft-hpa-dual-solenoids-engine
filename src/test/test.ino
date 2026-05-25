// Khai báo các chân kết nối cho ESP32-C3
const int SOLENOID_PIN = D3;  // Chân D4 (GPIO4) điều khiển Solenoid
const int SOLENOID_PIN2 = D4;
const int TRIGGER_PIN = D10;  // Chân D10 (GPIO10) nhận tín hiệu Trigger

// Biến lưu trạng thái trước đó của chân trigger để phát hiện sườn lên
bool lastTriggerState = LOW;

void setup() {
  Serial.begin(115200);

  // Cấu hình chân Solenoid là Output và đảm bảo nó đang tắt
  pinMode(SOLENOID_PIN, OUTPUT);
  pinMode(SOLENOID_PIN2, OUTPUT);
  digitalWrite(SOLENOID_PIN, LOW); 
  digitalWrite(SOLENOID_PIN2, LOW); 

  // Cấu hình chân Trigger là Input. 
  // Dùng INPUT_PULLDOWN nếu tín hiệu kích là mức CAO (HIGH).
  pinMode(TRIGGER_PIN, INPUT_PULLDOWN);
  
  Serial.println("Hệ thống đã sẵn sàng. Chờ tín hiệu trigger...");
}

void loop() {
  // Đọc trạng thái hiện tại của chân Trigger
  bool currentTriggerState = digitalRead(TRIGGER_PIN);

  // Kiểm tra nếu tín hiệu chuyển từ LOW sang HIGH (bắt đầu kích hoạt)
  if (currentTriggerState == HIGH && lastTriggerState == LOW) {
    Serial.println("Phát hiện Trigger! Bật solenoid trong 3000us...");
    
    // 1. Bật Solenoid
    digitalWrite(SOLENOID_PIN, HIGH);
    
    // 2. Chờ đúng 3000 microgiây (3 miligiây)
    delayMicroseconds(3000000);
    
    // 3. Tắt Solenoid ngay lập tức
    digitalWrite(SOLENOID_PIN, LOW);
    
    Serial.println("Solenoid đã tắt.");
  }

  // Cập nhật trạng thái trigger cho vòng lặp tiếp theo
  lastTriggerState = currentTriggerState;
  
  // Trễ một chút để chống dội (debounce) nếu bạn dùng nút nhấn cơ học
  delay(10); 
}