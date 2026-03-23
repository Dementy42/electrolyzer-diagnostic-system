#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Гибкая система диагностики электролизера водорода
Поддерживает: 
 - Прямое подключение к Arduino
 - Анализ заранее сгенерированных JSON/CSV (baseline.json, synthetic_*.json, .csv)

Автор: Plotnikov D.A.
Дата: 2025-11-28
"""

import os
import sys
import json
import time
import traceback
import csv
from datetime import datetime

try:
    from openpyxl import Workbook
    from openpyxl.styles import Font, PatternFill, Alignment
    OPENPYXL_AVAILABLE = True
except ImportError:
    OPENPYXL_AVAILABLE = False
    print("⚠ openpyxl не установлена. Экспорт в Excel недоступен.")

# -- Импортировать классы из src/ или основной папки (универсально)
try:
    from hydrogen_analyzer_system import (SystemConfig, TubeStatusEnum,
        SystemStatusEnum, SeverityEnum, DiagnosticResult, TubeDiagnosticEngine)
    from hydrogen_analyzer_system import ArduinoInterface, PressureSensor, DifferentialPressureManometer, TemperatureSensor
except ImportError:
    # Если склонировано в единый файл
    # (сюда вставьте определения классов из исходной системы!)
    pass

class SystemControllerFlexible:
    """Гибридный контроллер диагностики (Arduino/файл)"""
    def __init__(self, config: 'SystemConfig' = None, mode: str = "arduino"):
        self.config = config or SystemConfig()
        self.mode = mode
        self.diagnostic_engine = TubeDiagnosticEngine(self.config)
        self.readings_history = []
        self.diagnostic_history = []

        if mode == "arduino":
            self.arduino = ArduinoInterface(baudrate=self.config.SERIAL_BAUDRATE)
            self.pressure_sensor = PressureSensor(self.config.PRESSURE_PWM_PIN, 0, 500)
            self.differential_start = DifferentialPressureManometer(self.config.DIFFERENTIAL_P_START, 100)
            self.differential_end = DifferentialPressureManometer(self.config.DIFFERENTIAL_P_END, 100)
            self.temperature_sensor = TemperatureSensor(self.config.DS18B20_PIN)
            self.system_status = SystemStatusEnum.OFFLINE
            self.is_running = False
            self.is_calibrated = False
            self._ensure_data_folder()

    def _ensure_data_folder(self):
        try:
            os.makedirs(self.config.DATA_FOLDER, exist_ok=True)
        except Exception as e:
            print(f"⚠ Ошибка создания папки: {e}")

    def connect(self):
        if self.mode == "arduino":
            if self.arduino.connect():
                self.system_status = SystemStatusEnum.OK
                return True
            else:
                self.system_status = SystemStatusEnum.OFFLINE
                return False
        return True  # В режиме файла соединения не требуется

    def disconnect(self):
        if self.mode == "arduino":
            self.arduino.disconnect()

    def calibrate_system(self):
        if self.mode != "arduino":
            print("Калибровка в режиме файла не нужна.")
            return True
        # ... (оставьте логику из оригинального SystemController)
        # ...

    def run_monitoring_loop(self, duration_seconds=60, sampling_rate=1):
        if self.mode == "arduino":
            # Вставьте логику run_monitoring_loop из оригинального SystemController
            pass  # ... (туда же вставка)
        else:
            print("⚠ Мониторинг по внешнему файлу — используйте run_file_analysis()")

    def run_file_analysis(self, file_path: str):
        print(f"\nЗагрузка данных из файла: {file_path}\n")
        try:
            if file_path.endswith(".json"):
                with open(file_path, "r", encoding="utf-8") as f:
                    dataset = json.load(f)
                samples = dataset["samples"] if "samples" in dataset else dataset
            elif file_path.endswith(".csv"):
                samples = []
                with open(file_path, newline="", encoding="utf-8") as f:
                    reader = csv.DictReader(f)
                    for row in reader:
                        s = {
                            "timestamp": float(row["timestamp"]) if "timestamp" in row else time.time(),
                            "sensors": {k: float(row[k]) for k in row if k not in ("timestamp", "errors", "warnings", "recovery")},
                        }
                        samples.append(s)
            else:
                print("Формат файла не поддерживается (только .json/.csv)")
                return

            # Запуск диагностики по всем сэмплам
            print(f"Всего сэмплов: {len(samples)}")
            for idx, sample in enumerate(samples, 1):
                sensors = sample["sensors"]
                pressure_elec = sensors.get("pressureanode") or sensors.get("pressure_electrolyzer") or 0
                pressure_start = sensors.get("pressurecathode") or sensors.get("pressure_start") or 0
                pressure_end = sensors.get("pressureend") or sensors.get("pressure_end") or 0
                temperature = sensors.get("temperature") or 25.0
                # Можно дополнить под ваши именования каналов

                res = self.diagnostic_engine.diagnose(pressure_elec, pressure_start, pressure_end, temperature)
                self.diagnostic_history.append(res)
                if idx % 20 == 0 or idx == len(samples):
                    print(f"Processed sample {idx}/{len(samples)}")

            print("\n✓ Анализ завершён.")
            self.save_all_data()
        except Exception as e:
            print(f"❌ Ошибка анализа файла: {e}\n{traceback.format_exc()}")

    def save_all_data(self):
        # ... (можно взять save_all_data/save_to_csv/save_to_excel из оригинального SystemController)
        pass

def main():
    print("\n" + "="*70)
    print("╔═════════ ГИБКАЯ ДИАГНОСТИКА (ARDUINO/ФАЙЛ) ═════════╗")
    print("="*70)
    choice = ""
    while choice not in ("1", "2"):
        print("\nВыберите источник данных:")
        print("1) Arduino (реальное оборудование)")
        print("2) Файл (JSON/CSV сгенерированные данные)")
        choice = input("Ваш выбор (1/2): ").strip()
    
    mode = "arduino" if choice == "1" else "file"
    system = SystemControllerFlexible(mode=mode)
    if not system.connect():
        print("❌ Не удалось подключиться к Arduino.")
        return

    if mode == "file":
        file_path = input("Введите путь к файлу (.json/.csv): ").strip()
        system.run_file_analysis(file_path)
    else:
        # Классическое меню из вашей диагностики
        pass # Можно вставить цикл меню

    system.disconnect()

if __name__ == "__main__":
    main()
