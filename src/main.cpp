#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <ESP32Servo.h>

// Configuración del LED de la placa
#ifndef LED_BUILTIN
  #define LED_PIN 2  // Cambia este pin si tu placa usa otro GPIO para el LED
#else
  #define LED_PIN LED_BUILTIN
#endif

// UART que utilizaremos para el R503
HardwareSerial FingerSerial(1);

// Pines ESP32-S3
#define FINGER_RX 16
#define FINGER_TX 17
#define SERVO_PIN 33

constexpr uint8_t SENSOR_LED_GREEN = 0x04;
constexpr uint8_t R503_UPLOAD_IMAGE = 0x0A;
constexpr int SERVO_STOP_US = 1500;
constexpr int SERVO_OPEN_US = 2000;
constexpr int SERVO_CLOSE_US = 1000;
constexpr uint16_t SERVO_MOVE_TIME_MS = 1000;
constexpr uint16_t SERVO_OPEN_TIME_MS = 1000;
constexpr uint16_t FINGER_IMAGE_WIDTH = 192;
constexpr uint16_t FINGER_IMAGE_HEIGHT = 192;
constexpr size_t FINGER_IMAGE_BYTES =
    static_cast<size_t>(FINGER_IMAGE_WIDTH) * FINGER_IMAGE_HEIGHT / 2;
constexpr size_t IMAGE_CHUNK_SIZE = 384;
uint8_t fingerprintImage[FINGER_IMAGE_BYTES];
bool sensorLedWarningShown = false;
bool suppressRecognitionUntilFingerRemoved = false;
bool servoAvailable = false;

Adafruit_Fingerprint finger = Adafruit_Fingerprint(&FingerSerial);
Servo accessServo;

// Prototipos de funciones
void configurarLedSensor(uint8_t modo, uint8_t velocidad, uint8_t color,
                        uint8_t repeticiones);
void accionarServo();
void verificarHuella();
void registrarHuella();
void listarHuellas();
void enviarImagenHuella();
uint8_t getFingerprintEnroll(uint8_t id);
int getFingerprintID();
bool leerByteSensor(uint8_t &value, uint32_t timeoutMs);
uint8_t leerPaqueteImagen(uint8_t *payload, size_t payloadCapacity,
                          size_t &payloadLength, uint8_t &packetType);
void enviarBloqueImagen(const uint8_t *data, size_t length);

void setup()
{
    Serial.begin(115200);

    // Configurar el pin del LED como salida
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); // Apagado por defecto

    accessServo.setPeriodHertz(50);
    accessServo.attach(SERVO_PIN, 500, 2400);
    servoAvailable = accessServo.attached();
    if (servoAvailable)
    {
        accessServo.writeMicroseconds(SERVO_STOP_US);
    }
    else
    {
        Serial.println("Aviso: no se pudo inicializar el servomotor.");
    }

    delay(1000);

    Serial.println();
    Serial.println("=== R503 - CONTROL DE HUELLA, LED Y SERVO ===");

    // Iniciar UART del sensor
    FingerSerial.begin(
        57600,
        SERIAL_8N1,
        FINGER_RX,
        FINGER_TX
    );

    delay(500);

    Serial.println("Comprobando sensor...");

    if (finger.verifyPassword())
    {
        Serial.println("R503 detectado correctamente.");
    }
    else
    {
        Serial.println("No se encuentra el R503");
        Serial.println("Comprueba alimentacion, TX/RX y pines.");
        while (1) { delay(1); } // Detener ejecución si no hay sensor
    }

    if (finger.getParameters() != FINGERPRINT_OK)
    {
        Serial.println("No se pudieron leer los parametros del sensor.");
        while (1) { delay(1); }
    }

    Serial.print("Capacidad de busqueda: ");
    Serial.println(finger.capacity);

    configurarLedSensor(FINGERPRINT_LED_BREATHING, 80,
                        FINGERPRINT_LED_BLUE, 0);

    // Mostrar huellas guardadas actualmente
    finger.getTemplateCount();
    Serial.print("Huellas guardadas en memoria: ");
    Serial.println(finger.templateCount);

    Serial.println("\n-----------------------------------------------------");
    Serial.println(" -> Pon tu dedo para VERIFICAR e identificar la huella.");
    Serial.println(" -> Escribe 'i' para CAPTURAR y enviar la imagen de la huella.");
    Serial.println(" -> Escribe 'e' en el Monitor Serie para REGISTRAR una nueva.");
    Serial.println(" -> Escribe 'l' para LISTAR los IDs de las huellas guardadas.");
    Serial.println("-----------------------------------------------------\n");
}

void loop()
{
    // 1. Comprobar si el usuario escribió 'e' para registrar una huella
    if (Serial.available())
    {
        char c = Serial.read();
        if (c == 'e' || c == 'E')
        {
            registrarHuella();
        }
        else if (c == 'l' || c == 'L')
        {
            listarHuellas();
        }
        else if (c == 'i' || c == 'I')
        {
            enviarImagenHuella();
        }
    }

    // No identificar la misma huella capturada hasta que se retire el dedo.
    if (suppressRecognitionUntilFingerRemoved)
    {
        if (finger.getImage() == FINGERPRINT_NOFINGER)
        {
            suppressRecognitionUntilFingerRemoved = false;
        }
    }
    else
    {
        verificarHuella();
    }

    delay(100); // Pausa breve entre escaneos
}

// -------------------------------------------------------------------
// FUNCIONES DE VERIFICACIÓN E IDENTIFICACIÓN
// -------------------------------------------------------------------

void configurarLedSensor(uint8_t modo, uint8_t velocidad, uint8_t color,
                         uint8_t repeticiones)
{
    uint8_t resultado = finger.LEDcontrol(modo, velocidad, color, repeticiones);
    if (resultado != FINGERPRINT_OK && !sensorLedWarningShown)
    {
        Serial.print("Aviso: el sensor rechazo el control LED. Codigo: ");
        Serial.println(resultado);
        sensorLedWarningShown = true;
    }
}

void accionarServo()
{
    if (!servoAvailable)
    {
        return;
    }

    accessServo.writeMicroseconds(SERVO_OPEN_US);
    delay(SERVO_MOVE_TIME_MS);
    accessServo.writeMicroseconds(SERVO_STOP_US);
    delay(SERVO_OPEN_TIME_MS);
    accessServo.writeMicroseconds(SERVO_CLOSE_US);
    delay(SERVO_MOVE_TIME_MS);
    accessServo.writeMicroseconds(SERVO_STOP_US);
}

void verificarHuella()
{
    int id = getFingerprintID();
    
    if (id > 0)
    {
        suppressRecognitionUntilFingerRemoved = true;
        Serial.print("¡HUELLA RECONOCIDA! ID #: ");
        Serial.print(id);
        Serial.print(" | Confianza: ");
        Serial.println(finger.confidence);

        configurarLedSensor(FINGERPRINT_LED_ON, 0, SENSOR_LED_GREEN, 0);
        digitalWrite(LED_PIN, HIGH);
        accionarServo();
        digitalWrite(LED_PIN, LOW);
        configurarLedSensor(FINGERPRINT_LED_BREATHING, 80,
                    FINGERPRINT_LED_BLUE, 0);
        Serial.println("Esperando siguiente huella...\n");
    }
    else if (id == 0)
    {
        Serial.println("Huella no reconocida.");
        configurarLedSensor(FINGERPRINT_LED_FLASHING, 25,
                    FINGERPRINT_LED_RED, 2);
        delay(1000);
        configurarLedSensor(FINGERPRINT_LED_BREATHING, 80,
                    FINGERPRINT_LED_BLUE, 0);
    }
}

int getFingerprintID()
{
    uint8_t p = finger.getImage();
    if (p != FINGERPRINT_OK) return -1; // No hay dedo presente

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) return -1;

    Serial.println("Analizando huella...");
    p = finger.fingerSearch();
    if (p == FINGERPRINT_NOTFOUND) return 0;
    if (p != FINGERPRINT_OK)
    {
        Serial.print("Error al buscar la huella. Codigo: ");
        Serial.println(p);
        return -1;
    }

    return finger.fingerID; // Devuelve el ID de la huella que coincidió
}

void listarHuellas()
{
    Serial.println("\n--- HUELLAS GUARDADAS ---");

    if (finger.getParameters() != FINGERPRINT_OK)
    {
        Serial.println("Error al consultar la capacidad del sensor.");
        Serial.println("Fin de lista.");
        return;
    }

    if (finger.getTemplateCount() != FINGERPRINT_OK)
    {
        Serial.println("Error al consultar la cantidad de huellas.");
        Serial.println("Fin de lista.");
        return;
    }

    Serial.print("Capacidad: ");
    Serial.println(finger.capacity);
    Serial.print("Cantidad reportada por el sensor: ");
    Serial.println(finger.templateCount);
    Serial.println("IDs ocupados:");

    uint16_t huellasEncontradas = 0;
    for (uint16_t id = 0; id < finger.capacity; id++)
    {
        uint8_t resultado = finger.loadModel(id);
        if (resultado == FINGERPRINT_OK)
        {
            Serial.print("ID ");
            Serial.println(id);
            huellasEncontradas++;
        }
        else if (resultado != FINGERPRINT_BADLOCATION &&
             resultado != FINGERPRINT_DBRANGEFAIL)
        {
            Serial.print("Error al revisar el ID ");
            Serial.print(id);
            Serial.print(". Codigo: ");
            Serial.println(resultado);
            Serial.println("Fin de lista.");
            return;
        }
    }

    if (huellasEncontradas == 0)
    {
        Serial.println("No hay huellas guardadas.");
    }

    if (huellasEncontradas != finger.templateCount)
    {
        Serial.print("Aviso: se encontraron ");
        Serial.print(huellasEncontradas);
        Serial.println(" IDs, distinto de la cantidad reportada por el sensor.");
    }

    Serial.println("Fin de lista.");
}

void enviarImagenHuella()
{
    Serial.println("Coloca el dedo para capturar la imagen.");
    const unsigned long tiempoInicio = millis();
    uint8_t resultado = FINGERPRINT_NOFINGER;

    while (millis() - tiempoInicio < 10000)
    {
        resultado = finger.getImage();
        if (resultado == FINGERPRINT_OK)
        {
            break;
        }
        if (resultado != FINGERPRINT_NOFINGER)
        {
            Serial.print("FINGER_IMAGE_ERROR: No se pudo capturar la imagen. Codigo: ");
            Serial.println(resultado);
            return;
        }
        delay(100);
    }

    if (resultado != FINGERPRINT_OK)
    {
        Serial.println("FINGER_IMAGE_ERROR: Tiempo agotado esperando el dedo.");
        return;
    }
    suppressRecognitionUntilFingerRemoved = true;

    uint8_t command[] = {R503_UPLOAD_IMAGE};
    Adafruit_Fingerprint_Packet packet(
        FINGERPRINT_COMMANDPACKET, sizeof(command), command);
    finger.writeStructuredPacket(packet);

    if (finger.getStructuredPacket(&packet) != FINGERPRINT_OK ||
        packet.type != FINGERPRINT_ACKPACKET || packet.length < 3)
    {
        Serial.println("FINGER_IMAGE_ERROR: No se recibio respuesta al solicitar la imagen.");
        return;
    }
    if (packet.data[0] != FINGERPRINT_OK)
    {
        Serial.print("FINGER_IMAGE_ERROR: El sensor no pudo enviar la imagen. Codigo: ");
        Serial.println(packet.data[0]);
        return;
    }

    Serial.print("FINGER_IMAGE_BEGIN:");
    Serial.print(FINGER_IMAGE_WIDTH);
    Serial.print(":");
    Serial.print(FINGER_IMAGE_HEIGHT);
    Serial.print(":");
    Serial.println(FINGER_IMAGE_BYTES);

    uint8_t packetData[512];
    size_t totalBytes = 0;

    while (true)
    {
        size_t packetBytes = 0;
        uint8_t packetType = 0;
        resultado = leerPaqueteImagen(
            packetData, sizeof(packetData), packetBytes, packetType);
        if (resultado != FINGERPRINT_OK)
        {
            Serial.print("FINGER_IMAGE_ERROR: Error al recibir datos de imagen. Codigo: ");
            Serial.println(resultado);
            return;
        }
        if (totalBytes + packetBytes > FINGER_IMAGE_BYTES)
        {
            Serial.println("FINGER_IMAGE_ERROR: La imagen recibida supera el tamano esperado.");
            return;
        }

        for (size_t i = 0; i < packetBytes; i++)
        {
            fingerprintImage[totalBytes] = packetData[i];
            totalBytes++;
        }

        if (packetType == FINGERPRINT_ENDDATAPACKET)
        {
            break;
        }
    }

    if (totalBytes != FINGER_IMAGE_BYTES)
    {
        Serial.print("FINGER_IMAGE_ERROR: Imagen incompleta. Recibidos ");
        Serial.print(totalBytes);
        Serial.print(" de ");
        Serial.print(FINGER_IMAGE_BYTES);
        Serial.println(" bytes.");
        return;
    }

    for (size_t offset = 0; offset < totalBytes; offset += IMAGE_CHUNK_SIZE)
    {
        const size_t remaining = totalBytes - offset;
        const size_t chunkLength =
            remaining < IMAGE_CHUNK_SIZE ? remaining : IMAGE_CHUNK_SIZE;
        enviarBloqueImagen(fingerprintImage + offset, chunkLength);
    }

    Serial.println("FINGER_IMAGE_END");
    suppressRecognitionUntilFingerRemoved = true;
}

bool leerByteSensor(uint8_t &value, uint32_t timeoutMs)
{
    const unsigned long inicio = millis();
    while (FingerSerial.available() == 0)
    {
        if (millis() - inicio >= timeoutMs)
        {
            return false;
        }
        delay(1);
    }
    value = static_cast<uint8_t>(FingerSerial.read());
    return true;
}

uint8_t leerPaqueteImagen(uint8_t *payload, size_t payloadCapacity,
                          size_t &payloadLength, uint8_t &packetType)
{
    uint8_t value = 0;
    bool startCodeStarted = false;
    const unsigned long inicio = millis();

    while (millis() - inicio < 2000)
    {
        if (!leerByteSensor(value, 2000))
        {
            return FINGERPRINT_TIMEOUT;
        }
        if (!startCodeStarted)
        {
            startCodeStarted = value == 0xEF;
        }
        else if (value == 0x01)
        {
            break;
        }
        else
        {
            startCodeStarted = value == 0xEF;
        }
    }

    if (!startCodeStarted || value != 0x01)
    {
        return FINGERPRINT_TIMEOUT;
    }

    uint8_t header[7];
    for (uint8_t &byte : header)
    {
        if (!leerByteSensor(byte, 2000))
        {
            return FINGERPRINT_TIMEOUT;
        }
    }

    packetType = header[4];
    const uint16_t packetLength =
        (static_cast<uint16_t>(header[5]) << 8) | header[6];
    if ((packetType != FINGERPRINT_DATAPACKET &&
         packetType != FINGERPRINT_ENDDATAPACKET) ||
        packetLength < 2 || packetLength > payloadCapacity)
    {
        return FINGERPRINT_BADPACKET;
    }

    uint16_t checksum = packetType + header[5] + header[6];
    for (uint16_t i = 0; i < packetLength; i++)
    {
        if (!leerByteSensor(payload[i], 2000))
        {
            return FINGERPRINT_TIMEOUT;
        }
        if (i < packetLength - 2)
        {
            checksum += payload[i];
        }
    }

    const uint16_t receivedChecksum =
        (static_cast<uint16_t>(payload[packetLength - 2]) << 8) |
        payload[packetLength - 1];
    if (checksum != receivedChecksum)
    {
        return FINGERPRINT_BADPACKET;
    }

    payloadLength = packetLength - 2;
    return FINGERPRINT_OK;
}

void enviarBloqueImagen(const uint8_t *data, size_t length)
{
    constexpr char BASE64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Serial.print("FINGER_IMAGE_DATA:");
    for (size_t i = 0; i < length; i += 3)
    {
        const uint32_t value =
            (static_cast<uint32_t>(data[i]) << 16) |
            (i + 1 < length ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
            (i + 2 < length ? data[i + 2] : 0);
        Serial.print(BASE64[(value >> 18) & 0x3F]);
        Serial.print(BASE64[(value >> 12) & 0x3F]);
        Serial.print(i + 1 < length ? BASE64[(value >> 6) & 0x3F] : '=');
        Serial.print(i + 2 < length ? BASE64[value & 0x3F] : '=');
    }
    Serial.println();
}

// -------------------------------------------------------------------
// FUNCIONES PARA REGISTRAR / ENROLAR HUELLAS
// -------------------------------------------------------------------

void registrarHuella()
{
    Serial.println("\n--- REGISTRO DE NUEVA HUELLA ---");
    Serial.println("Introduce el ID (ejemplo: 1 a 127) en el Monitor Serie y presiona Enter:");

    while (!Serial.available());
    uint8_t id = Serial.parseInt();
    
    if (id == 0)
    {
        Serial.println("ID no válido o cancelación.");
        return;
    }

    Serial.print("Guardando en el ID #");
    Serial.println(id);

    while (!getFingerprintEnroll(id));
}

uint8_t getFingerprintEnroll(uint8_t id)
{
    int p = -1;
    configurarLedSensor(FINGERPRINT_LED_FLASHING, 35,
                        FINGERPRINT_LED_PURPLE, 0);
    Serial.print("Coloca el dedo para enrolar en la posición #");
    Serial.println(id);

    while (p != FINGERPRINT_OK)
    {
        p = finger.getImage();
        if (p == FINGERPRINT_OK) {
            Serial.println("Imagen 1 tomada.");
        }
        delay(100);
    }

    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK) {
        Serial.println("Error al procesar la primera imagen.");
        configurarLedSensor(FINGERPRINT_LED_FLASHING, 25,
                    FINGERPRINT_LED_RED, 3);
        configurarLedSensor(FINGERPRINT_LED_BREATHING, 80,
                    FINGERPRINT_LED_BLUE, 0);
        return true;
    }

    Serial.println("Retira el dedo...");
    delay(2000);
    p = 0;
    while (p != FINGERPRINT_NOFINGER) {
        p = finger.getImage();
    }

    Serial.println("Coloca el MISMO dedo otra vez...");
    p = -1;
    while (p != FINGERPRINT_OK) {
        p = finger.getImage();
        delay(100);
    }

    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK) {
        Serial.println("Error al procesar la segunda imagen.");
        configurarLedSensor(FINGERPRINT_LED_FLASHING, 25,
                    FINGERPRINT_LED_RED, 3);
        configurarLedSensor(FINGERPRINT_LED_BREATHING, 80,
                    FINGERPRINT_LED_BLUE, 0);
        return true;
    }

    p = finger.createModel();
    if (p == FINGERPRINT_OK) {
        Serial.println("¡Las dos muestras coinciden!");
    } else {
        Serial.println("Las muestras no coinciden. Inténtalo de nuevo.");
        configurarLedSensor(FINGERPRINT_LED_FLASHING, 25,
                    FINGERPRINT_LED_RED, 3);
        configurarLedSensor(FINGERPRINT_LED_BREATHING, 80,
                    FINGERPRINT_LED_BLUE, 0);
        return true;
    }

    p = finger.storeModel(id);
    if (p == FINGERPRINT_OK) {
        Serial.println("¡Huella guardada con éxito!");
        configurarLedSensor(FINGERPRINT_LED_FLASHING, 25,
                            SENSOR_LED_GREEN, 3);
    } else {
        Serial.println("Error al guardar en la memoria del sensor.");
        configurarLedSensor(FINGERPRINT_LED_FLASHING, 25,
                            FINGERPRINT_LED_RED, 3);
    }

    configurarLedSensor(FINGERPRINT_LED_BREATHING, 80,
                        FINGERPRINT_LED_BLUE, 0);
    return true;
}