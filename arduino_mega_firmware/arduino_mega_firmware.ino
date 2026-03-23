/*
═══════════════════════════════════════════════════════════════════════════════
    ПРОШИВКА ARDUINO MEGA 2560 ДЛЯ ДИАГНОСТИКИ ЭЛЕКТРОЛИЗЕРА
    Arduino Firmware for Hydrogen Electrolyzer Diagnostics
    
    Плата: Arduino MEGA 2560
    Язык: Arduino C/C++
    
    Автор: Dmitrii Plotnikov
    Дата: 2025-11-24
═══════════════════════════════════════════════════════════════════════════════
*/

#include <OneWire.h>
#include <DallasTemperature.h>

// ═══════════════════════════════════════════════════════════════════════════════
// КОНФИГУРАЦИЯ ПОРТОВ
// ═══════════════════════════════════════════════════════════════════════════════

// Цифровые порты
const int DS18B20_PIN = 10;              // OneWire для температурного датчика
const int PNEUMATIC_VALVE_PIN = 22;      // Управление пневмоклапаном
const int RELAY_PIN = 24;                 // Управление реле системы
const int STATUS_LED_PIN = 13;            // LED статуса (встроенный на MEGA)

// Аналоговые порты (ADC)
const int PRESSURE_PWM_PIN = A0;         // Датчик давления (0-500 кПа)
const int DIFFERENTIAL_P_START = A1;     // Дифманометр HT-1890 канал A
const int DIFFERENTIAL_P_END = A2;       // Дифманометр HT-1890 канал B

// ═══════════════════════════════════════════════════════════════════════════════
// ИНИЦИАЛИЗАЦИЯ ДАТЧИКОВ
// ═══════════════════════════════════════════════════════════════════════════════

// DS18B20 температурный датчик
OneWire oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);

// ═══════════════════════════════════════════════════════════════════════════════
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ═══════════════════════════════════════════════════════════════════════════════

// Состояние системы
boolean systemRunning = false;
boolean valveOpen = false;
unsigned long lastCommandTime = 0;
const unsigned long COMMAND_TIMEOUT = 5000;  // 5 секунд

// Буферы для сглаживания (экспоненциальное усреднение)
float smoothedPressure = 0;
float smoothedDiffStart = 0;
float smoothedDiffEnd = 0;
const float ALPHA = 0.7;  // Коэффициент сглаживания

// ═══════════════════════════════════════════════════════════════════════════════
// ФУНКЦИИ ИНИЦИАЛИЗАЦИИ
// ═══════════════════════════════════════════════════════════════════════════════

void setup() {
  // Инициализация Serial
  Serial.begin(9600);
  delay(1000);
  
  // Вывод приветствия
  Serial.println("╔═══════════════════════════════════════════════════════════════╗");
  Serial.println("║  Arduino MEGA 2560 - Electrolyzer Diagnostics System         ║");
  Serial.println("║  Firmware version 1.0 | 2025-11-24                           ║");
  Serial.println("╚═══════════════════════════════════════════════════════════════╝");
  Serial.println("");
  
  // Инициализация портов
  pinMode(DS18B20_PIN, INPUT);
  pinMode(PNEUMATIC_VALVE_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  
  // Установка начальных значений
  digitalWrite(PNEUMATIC_VALVE_PIN, LOW);  // Клапан закрыт
  digitalWrite(RELAY_PIN, LOW);             // Реле выключено
  digitalWrite(STATUS_LED_PIN, LOW);        // LED выключен
  
  // Инициализация датчика температуры
  sensors.begin();
  sensors.requestTemperatures();  // Первый запрос
  
  Serial.println("[INIT] System initialized");
  Serial.println("[INIT] Ports configured");
  Serial.println("[INIT] Temperature sensor ready");
  Serial.println("[INIT] Waiting for commands...");
  Serial.println("");
}

// ═══════════════════════════════════════════════════════════════════════════════
// ФУНКЦИИ ЧТЕНИЯ ДАТЧИКОВ
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * Чтение значения АДЦ с фильтрацией
 */
int readADCFiltered(int pin, int samples = 10) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delayMicroseconds(100);
  }
  return sum / samples;
}

/**
 * Экспоненциальное сглаживание
 */
float exponentialSmoothing(float newValue, float smoothedValue) {
  return (ALPHA * newValue) + ((1.0 - ALPHA) * smoothedValue);
}

/**
 * Чтение давления датчика (0-500 кПа)
 * Формула: P = (ADC / 1023) * 500 кПа
 */
float readPressure() {
  int adcValue = readADCFiltered(PRESSURE_PWM_PIN, 15);
  float voltage = (adcValue / 1023.0) * 5.0;  // Преобразование в напряжение (V)
  float pressure = (voltage / 5.0) * 500.0;   // Преобразование в давление (кПа)
  
  smoothedPressure = exponentialSmoothing(pressure, smoothedPressure);
  return smoothedPressure;
}

/**
 * Чтение дифференциального давления в начале трубки
 * Диапазон: ±100 мбар = ±10 кПа
 */
float readDifferentialPressureStart() {
  int adcValue = readADCFiltered(DIFFERENTIAL_P_START, 15);
  float voltage = (adcValue / 1023.0) * 5.0;
  float pressure_mbar = -100.0 + (voltage / 5.0) * 200.0;  // ±100 мбар
  float pressure_kpa = pressure_mbar / 100.0;               // Преобразование в кПа
  
  smoothedDiffStart = exponentialSmoothing(pressure_kpa, smoothedDiffStart);
  return smoothedDiffStart;
}

/**
 * Чтение дифференциального давления в конце трубки
 */
float readDifferentialPressureEnd() {
  int adcValue = readADCFiltered(DIFFERENTIAL_P_END, 15);
  float voltage = (adcValue / 1023.0) * 5.0;
  float pressure_mbar = -100.0 + (voltage / 5.0) * 200.0;
  float pressure_kpa = pressure_mbar / 100.0;
  
  smoothedDiffEnd = exponentialSmoothing(pressure_kpa, smoothedDiffEnd);
  return smoothedDiffEnd;
}

/**
 * Чтение температуры с DS18B20
 */
float readTemperature() {
  static unsigned long lastReadTime = 0;
  
  // Запрос новых данных каждые 750 мс (предел DS18B20)
  if (millis() - lastReadTime > 750) {
    sensors.requestTemperatures();
    lastReadTime = millis();
  }
  
  float temp = sensors.getTempCByIndex(0);
  
  // Проверка ошибок
  if (temp == DEVICE_DISCONNECTED_C) {
    Serial.println("[TEMP] Error: Sensor disconnected!");
    return 25.0;  // Возврат номинального значения
  }
  
  return temp;
}

/**
 * Опрос всех датчиков и вывод JSON
 */
void pollAllSensors() {
  float pressure = readPressure();
  float diffStart = readDifferentialPressureStart();
  float diffEnd = readDifferentialPressureEnd();
  float temperature = readTemperature();
  
  // JSON формат для Python скрипта
  Serial.print("{");
  Serial.print("\"pressure\":");
  Serial.print(pressure, 3);
  Serial.print(",\"diff_start\":");
  Serial.print(diffStart, 3);
  Serial.print(",\"diff_end\":");
  Serial.print(diffEnd, 3);
  Serial.print(",\"temperature\":");
  Serial.print(temperature, 2);
  Serial.println("}");
}

// ═══════════════════════════════════════════════════════════════════════════════
// ФУНКЦИИ УПРАВЛЕНИЯ
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * Управление пневмоклапаном
 */
void controlPneumaticValve(boolean state) {
  digitalWrite(PNEUMATIC_VALVE_PIN, state ? HIGH : LOW);
  valveOpen = state;
  
  if (state) {
    Serial.println("[VALVE] Opened");
  } else {
    Serial.println("[VALVE] Closed");
  }
}

/**
 * Управление реле системы
 */
void controlSystemRelay(boolean state) {
  digitalWrite(RELAY_PIN, state ? HIGH : LOW);
  systemRunning = state;
  
  if (state) {
    Serial.println("[RELAY] System ON");
    digitalWrite(STATUS_LED_PIN, HIGH);
  } else {
    Serial.println("[RELAY] System OFF");
    digitalWrite(STATUS_LED_PIN, LOW);
  }
}

/**
 * Индикация LED
 */
void indicateLED() {
  if (systemRunning) {
    // Мигание LED если система работает
    static unsigned long lastBlinkTime = 0;
    if (millis() - lastBlinkTime > 500) {
      digitalWrite(STATUS_LED_PIN, digitalRead(STATUS_LED_PIN) == LOW ? HIGH : LOW);
      lastBlinkTime = millis();
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ФУНКЦИИ ОБРАБОТКИ КОМАНД
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * Обработка команд из Serial порта
 * Формат: CMD:ACTION:PIN:VALUE
 * Примеры:
 *   - READ_ADC:A0 → вывести значение ADC с порта A0
 *   - CMD:VALVE:22:HIGH → открыть клапан (HIGH)
 *   - CMD:VALVE:22:LOW → закрыть клапан (LOW)
 *   - CMD:RELAY:24:HIGH → включить реле
 *   - CMD:RELAY:24:LOW → выключить реле
 *   - CMD:STOP_SYSTEM → остановить систему
 *   - CMD:START_SYSTEM → запустить систему
 *   - READ_TEMP:10 → прочитать температуру с DS18B20
 *   - POLL_ALL → опрос всех датчиков
 */
void processCommand(String command) {
  command.trim();
  
  if (command.length() == 0) return;
  
  Serial.print("[CMD] Processing: ");
  Serial.println(command);
  
  // Разбор команды
  int colonPos = command.indexOf(':');
  if (colonPos == -1) {
    Serial.println("[ERROR] Invalid command format");
    return;
  }
  
  String action = command.substring(0, colonPos);
  String params = command.substring(colonPos + 1);
  
  // ─────────────────────────────────────────────────────────────────────────────
  // Чтение ADC
  // ─────────────────────────────────────────────────────────────────────────────
  if (action == "READ_ADC") {
    int pin = A0;
    
    if (params == "A0") pin = A0;
    else if (params == "A1") pin = A1;
    else if (params == "A2") pin = A2;
    else if (params == "A3") pin = A3;
    
    int adcValue = readADCFiltered(pin, 20);
    Serial.println(adcValue);
  }
  
  // ─────────────────────────────────────────────────────────────────────────────
  // Чтение температуры
  // ─────────────────────────────────────────────────────────────────────────────
  else if (action == "READ_TEMP") {
    float temp = readTemperature();
    Serial.println(temp, 2);
  }
  
  // ─────────────────────────────────────────────────────────────────────────────
  // Опрос всех датчиков
  // ─────────────────────────────────────────────────────────────────────────────
  else if (action == "POLL_ALL") {
    pollAllSensors();
  }
  
  // ─────────────────────────────────────────────────────────────────────────────
  // Управление пневмоклапаном
  // ─────────────────────────────────────────────────────────────────────────────
  else if (action == "CMD") {
    int colonPos2 = params.indexOf(':');
    if (colonPos2 == -1) {
      Serial.println("[ERROR] Invalid command format");
      return;
    }
    
    String subAction = params.substring(0, colonPos2);
    String remaining = params.substring(colonPos2 + 1);
    
    colonPos2 = remaining.indexOf(':');
    if (colonPos2 == -1) {
      Serial.println("[ERROR] Invalid command format");
      return;
    }
    
    int pin = remaining.substring(0, colonPos2).toInt();
    String state = remaining.substring(colonPos2 + 1);
    
    if (subAction == "VALVE") {
      controlPneumaticValve(state == "HIGH");
    }
    else if (subAction == "RELAY") {
      controlSystemRelay(state == "HIGH");
    }
  }
  
  // ─────────────────────────────────────────────────────────────────────────────
  // Команды остановки/запуска системы
  // ─────────────────────────────────────────────────────────────────────────────
  else if (action == "CMD") {
    // Эта часть обрабатывает команды вида "CMD:STOP_SYSTEM" или "CMD:START_SYSTEM"
    if (params == "STOP_SYSTEM") {
      controlSystemRelay(false);
      controlPneumaticValve(false);
      Serial.println("[SYSTEM] Stopped");
    }
    else if (params == "START_SYSTEM") {
      controlSystemRelay(true);
      controlPneumaticValve(true);
      Serial.println("[SYSTEM] Started");
    }
  }
  
  else {
    Serial.print("[ERROR] Unknown action: ");
    Serial.println(action);
  }
  
  lastCommandTime = millis();
}

// ═══════════════════════════════════════════════════════════════════════════════
// ГЛАВНЫЙ ЦИКЛ
// ═══════════════════════════════════════════════════════════════════════════════

void loop() {
  // Обработка входящих команд
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    processCommand(command);
  }
  
  // Индикация LED
  indicateLED();
  
  // Малая задержка для стабильности
  delayMicroseconds(100);
}

/*
═══════════════════════════════════════════════════════════════════════════════
РАСШИРЕННАЯ ДОКУМЕНТАЦИЯ
═══════════════════════════════════════════════════════════════════════════════

ПОДКЛЮЧЕНИЕ ДАТЧИКОВ И УСТРОЙСТВ К ARDUINO MEGA 2560:

1. ДАТЧИК ДАВЛЕНИЯ (PWM, 0-500 кПа):
   - Сигнал → Pin A0 (Analog Input)
   - Питание 5V → 5V
   - Земля → GND
   
2. ДИФФЕРЕЦНИАЛЬНЫЙ МАНОМЕТР HT-1890 (Канал A - начало трубки):
   - Сигнал → Pin A1 (Analog Input)
   - Питание 5V → 5V
   - Земля → GND
   
3. ДИФФЕРЕНЦИАЛЬНЫЙ МАНОМЕТР HT-1890 (Канал B - конец трубки):
   - Сигнал → Pin A2 (Analog Input)
   - Питание 5V → 5V
   - Земля → GND
   
4. ДАТЧИК ТЕМПЕРАТУРЫ DS18B20 (OneWire):
   - Signal → Pin 10 (Digital Input with 4.7kΩ pull-up)
   - VDD → 5V
   - GND → GND
   - DQ → Pin 10 (через 4.7kΩ резистор к 5V)
   
5. ПНЕВМОКЛАПАН (Управление через реле):
   - Signal → Pin 22 (Digital Output)
   - Реле подключено к Pin 22
   - Реле управляет питанием клапана 24V
   
6. РЕЛЕ СИСТЕМЫ (Управление главным реле):
   - Signal → Pin 24 (Digital Output)
   - Реле подключено к Pin 24
   - Реле включает/отключает систему
   
ПРИМЕРЫ КОМАНД ИЗ PYTHON СКРИПТА:

  # Чтение давления
  arduino.send_command("READ_ADC:A0")
  
  # Чтение дифф. давления
  arduino.send_command("READ_ADC:A1")
  arduino.send_command("READ_ADC:A2")
  
  # Чтение температуры
  arduino.send_command("READ_TEMP:10")
  
  # Опрос всех датчиков
  arduino.send_command("POLL_ALL")
  
  # Управление клапаном
  arduino.send_command("CMD:VALVE:22:HIGH")  # Открыть
  arduino.send_command("CMD:VALVE:22:LOW")   # Закрыть
  
  # Управление реле
  arduino.send_command("CMD:RELAY:24:HIGH")  # Включить
  arduino.send_command("CMD:RELAY:24:LOW")   # Выключить
  
  # Управление системой
  arduino.send_command("CMD:START_SYSTEM")   # Запустить
  arduino.send_command("CMD:STOP_SYSTEM")    # Остановить

═══════════════════════════════════════════════════════════════════════════════
*/
