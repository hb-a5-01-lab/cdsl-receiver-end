#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "component_config.hpp"

// OLED CONFIG
constexpr uint8_t OLED_ADDR = 0x3C;
constexpr int16_t OLED_WIDTH = 128;
constexpr int16_t OLED_HEIGHT = 64;

Adafruit_SSD1306 display(
    OLED_WIDTH,
    OLED_HEIGHT,
    &Wire,
    -1
);

bool oled_ready = false;

// PACKET CONFIG
constexpr uint8_t START_BYTE = 0xAA;
constexpr uint16_t CRC16_INITIAL_VALUE = 0xFFFF;
constexpr uint16_t CRC16_POLYNOMIAL = 0x8408;
constexpr uint8_t PACKET_SIZE = 10;
constexpr uint8_t CRC_SIZE = 2;

// PCA9685 CONFIG
constexpr uint8_t PCA_ADDR = 0x40;
Adafruit_PWMServoDriver pca(PCA_ADDR);

// SERVO CHANNELS
constexpr uint8_t BASE_MOTOR = 0;
constexpr uint8_t LEFT_MOTOR = 1;
constexpr uint8_t RIGHT_MOTOR = 2;
constexpr uint8_t ELBOW_MOTOR = 3;
constexpr uint8_t WRIST1_MOTOR = 4;
constexpr uint8_t WRIST2_MOTOR = 5;
constexpr uint8_t GRIPPER_MOTOR = 6;

// SERVO LIMITS
constexpr uint16_t SERVO_MIN = 130;
constexpr uint16_t SERVO_MAX = 520;

// OLED STATUS DISPLAY
void showStatus(
    const char* line1,
    const char* line2 = "",
    const char* line3 = "",
    const char* line4 = "",
    const char* line5 = ""
) {
    if (!oled_ready) {
        return;
    }

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    display.setCursor(0, 0);
    display.println(line1);
    display.println("---------------------");
    display.println(line2);
    display.println(line3);
    display.println(line4);
    display.println(line5);

    display.display();
}

// SERVO CONTROL
uint16_t angleToPulse(float angle) {
    return static_cast<uint16_t>(
        SERVO_MIN +
        (angle / 180.0f) * (SERVO_MAX - SERVO_MIN)
    );
}

void setServo(uint8_t channel, float angle) {
    const uint16_t pulse = angleToPulse(angle);
    pca.setPWM(channel, 0, pulse);
}

void moveShoulder(float angle) {
    // Shoulder motors are mechanically mirrored.
    const float rightAngle = 180.0f - angle;

    setServo(LEFT_MOTOR, angle);
    setServo(RIGHT_MOTOR, rightAngle);
}

// CRC
uint16_t crc16(const uint8_t* data, uint8_t size) {
    uint16_t crc = CRC16_INITIAL_VALUE;

    for (uint8_t i = 0; i < size; ++i) {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8; ++bit) {
            if ((crc & 1) != 0) {
                crc = (crc >> 1) ^ CRC16_POLYNOMIAL;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}

// PACKET DECODING
uint16_t read_uint16_le(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

float read_float(const uint8_t* data) {
    float value;
    memcpy(&value, data, sizeof(value));
    return value;
}

bool receive_packet(uint8_t* packet) {
    static uint8_t index = 0;

    while (Serial.available() > 0) {
        const uint8_t byte = Serial.read();

        // Wait for the start byte.
        if (index == 0) {
            if (byte != START_BYTE) {
                continue;
            }

            packet[index++] = byte;
            continue;
        }

        packet[index++] = byte;

        if (index == PACKET_SIZE) {
            index = 0;
            return true;
        }
    }

    return false;
}

bool verify_packet(const uint8_t* packet) {
    const uint16_t calculated_crc =
        crc16(packet, PACKET_SIZE - CRC_SIZE);

    const uint16_t received_crc =
        static_cast<uint16_t>(packet[8]) |
        (static_cast<uint16_t>(packet[9]) << 8);

    return calculated_crc == received_crc;
}

// PACKET EXECUTION
void execute_command(ComponentId component, float value) {
    switch (component) {
        case ComponentId::Base:
            setServo(BASE_MOTOR, value);
            break;

        case ComponentId::Shoulder:
            moveShoulder(value);
            break;

        case ComponentId::Elbow:
            setServo(ELBOW_MOTOR, value);
            break;

        case ComponentId::Wrist:
            setServo(WRIST1_MOTOR, value);
            break;

        case ComponentId::Palm:
            setServo(WRIST2_MOTOR, value);
            break;

        case ComponentId::Claw:
            setServo(GRIPPER_MOTOR, value);
            break;
    }
}

// SETUP
void setup() {
    // Serial is retained exclusively for binary packet communication.
    Serial.begin(BAUD_RATE);

    Wire.begin();

    // Initialize OLED.
    oled_ready = display.begin(
        SSD1306_SWITCHCAPVCC,
        OLED_ADDR
    );

    if (oled_ready) {
        showStatus(
            "HB-A5-01 CONTROLLER",
            "Initializing..."
        );
    }

    // Initialize PCA9685 servo controller.
    pca.begin();
    pca.setPWMFreq(50);

    showStatus(
        "HB-A5-01 CONTROLLER",
        "Status: READY",
        "Awaiting commands..."
    );
}

// LOOP
void loop() {
    uint8_t packet[PACKET_SIZE];

    if (!receive_packet(packet)) {
        return;
    }

    // Ignore corrupted packets.
    if (!verify_packet(packet)) {
        showStatus(
            "PACKET ERROR",
            "Invalid packet CRC",
            "Command rejected"
        );

        return;
    }

    // Decode packet.
    //
    // Byte 0   : Start byte
    // Byte 1-2 : Packet ID
    // Byte 3   : Component ID
    // Byte 4-7 : Float value
    // Byte 8-9 : CRC16
    const uint16_t packet_id = read_uint16_le(&packet[1]);
    const uint8_t raw_component_id = packet[3];
    const float value = read_float(&packet[4]);

    // Validate component.
    if (!is_valid_component_id(raw_component_id)) {
        char component_buffer[22];

        snprintf(
            component_buffer,
            sizeof(component_buffer),
            "Component ID: %u",
            raw_component_id
        );

        showStatus(
            "PACKET ERROR",
            "Invalid component",
            component_buffer
        );

        return;
    }

    const ComponentId component =
        static_cast<ComponentId>(raw_component_id);

    // Execute command.
    execute_command(component, value);

    // Display command details on the OLED.
    char packet_buffer[22];
    char value_buffer[22];

    snprintf(
        packet_buffer,
        sizeof(packet_buffer),
        "Packet ID: %u",
        packet_id
    );

    snprintf(
        value_buffer,
        sizeof(value_buffer),
        "Value: %.2f",
        static_cast<double>(value)
    );

    showStatus(
        "ROBOT ARM - ACTIVE",
        packet_buffer,
        component_name(component),
        value_buffer,
        "Command executed"
    );
}
