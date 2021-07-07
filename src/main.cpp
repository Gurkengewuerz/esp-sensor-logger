#include <Arduino.h>
#include <BME280I2C.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
#include <NTPClient.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUdp.h>
#include <Wire.h>

#if !defined WIFI_NAME || !defined WIFI_PASS || !defined INFLUXDB_URL || !defined INFLUXDB_TOKEN || !defined INFLUXDB_ORG || !defined INFLUXDB_BUCKET
#error some variable is missing in your system enviroment
#endif

WiFiMulti wifiMulti;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

BME280I2C::Settings settings(
    BME280::OSR_X1,
    BME280::OSR_X1,
    BME280::OSR_X1,
    BME280::Mode_Forced,
    BME280::StandbyTime_250ms,
    BME280::Filter_Off,
    BME280::SpiEnable_False,
    BME280I2C::I2CAddr_0x76  // I2C address. I2C specific.
);
BME280I2C bme(settings);

uint32_t chipId = 0;

InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN);

Point reading("readings");

bool writePoint(int sensor, float value) {
    const unsigned long nowUnixTime = timeClient.getEpochTime();
    const unsigned long prev = nowUnixTime - (nowUnixTime % 900 /* Sekunden = 15 Minuten */);

    reading.clearFields();
    reading.clearTags();

    reading.addTag("station", String(chipId));
    reading.addTag("sensor", String(sensor));
    reading.addField("value", value);
    reading.addField("calculated", value);
    reading.setTime(prev);

    if (!client.writePoint(reading)) {
        Serial.print("InfluxDB write failed: ");
        Serial.println(client.getLastErrorMessage());
        return false;
    } else {
        Serial.println("written influx point");
    }
    return true;
}

void setup() {
    Serial.begin(MON_SPEED);
    Serial.println();

    for (int i = 0; i < 17; i = i + 8) {
        chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
    }

    pinMode(GPIO_NUM_19, OUTPUT);
    pinMode(GPIO_NUM_2, OUTPUT);
    digitalWrite(GPIO_NUM_19, HIGH);
    digitalWrite(GPIO_NUM_2, HIGH);
    delay(100);

    Wire.begin();
    delay(100);

    if (!bme.begin()) {
        Serial.println("Could not find a valid BME280 sensor, check wiring!");
        delay(5 * 1000);
        ESP.restart();
    }

    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.persistent(false);
    WiFi.disconnect(true);
    wifiMulti.addAP(WIFI_NAME, WIFI_PASS);

    Serial.print("Connecting to ");
    Serial.print(WIFI_NAME);
    if (wifiMulti.run() != WL_CONNECTED) {
        digitalWrite(GPIO_NUM_2, LOW);
        delay(1 * 1000);
        ESP.restart();
    }
    digitalWrite(GPIO_NUM_2, HIGH);
    Serial.println();

    Serial.printf("Connected to %s, IP address %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

    Serial.println("Syncing time");
    timeClient.begin();

    if (!timeClient.update()) {
        Serial.println("Syncing time failed! Restarting again");
        delay(30 * 1000);
        ESP.restart();
    }

    Serial.println("Current time is:");
    Serial.println(timeClient.getFormattedTime());

    if (client.validateConnection()) {
        Serial.print("Connected to InfluxDB: ");
        Serial.println(client.getServerUrl());
    } else {
        Serial.print("InfluxDB connection failed: ");
        Serial.println(client.getLastErrorMessage());
    }

    client.setWriteOptions(WriteOptions().writePrecision(WritePrecision::S).batchSize(10).bufferSize(30));

    float temperature = 0;
    float pressure = 0;
    float humidity = 0;

    BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);
    BME280::PresUnit presUnit(BME280::PresUnit_hPa);

    bme.read(pressure, temperature, humidity, tempUnit, presUnit);

    Serial.print("Temperature = ");
    Serial.print(temperature);
    Serial.println(" *C");

    Serial.print("Pressure = ");
    Serial.print(pressure);
    Serial.println(" hPa");

    Serial.print("Humidity = ");
    Serial.print(humidity);
    Serial.println(" %");

    Serial.println();

    writePoint(5001, temperature);
    writePoint(5002, pressure);
    //writePoint(5003, altitude);
    writePoint(5004, humidity);

    if (wifiMulti.run() != WL_CONNECTED) {
        Serial.println("Wifi connection lost");
    }

    Serial.println("Flushing data into InfluxDB");
    if (!client.flushBuffer()) {
        Serial.print("InfluxDB flush failed: ");
        Serial.println(client.getLastErrorMessage());
        Serial.print("Full buffer: ");
        Serial.println(client.isBufferFull() ? "Yes" : "No");
    }

    const unsigned long nowUnixTime = timeClient.getEpochTime();
    const unsigned long prev = nowUnixTime - (nowUnixTime % 900 /* Sekunden = 15 Minuten */);
    unsigned long timeToNext = (prev + 900) - nowUnixTime + (3 * 60);

    Serial.print("Waking up in ");
    Serial.print(timeToNext);
    Serial.println("s");

    digitalWrite(GPIO_NUM_19, LOW);
    digitalWrite(GPIO_NUM_2, LOW);
    delay(100);

    if (timeToNext > 25 * 60) timeToNext = 15 * 60;

    uint64_t time_us = timeToNext * 1000000ull;
    esp_sleep_enable_timer_wakeup(time_us);
    esp_deep_sleep_start();
}

void loop() {
    ESP.restart();
}