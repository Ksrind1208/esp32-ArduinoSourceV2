#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <SPI.h>
#include <Ethernet.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#define ESP_DRD_USE_SPIFFS true

#define JSON_CONFIG_FILE "/test_config.json"
#define ETHERNET_MAC "BA:E5:E3:B1:44:DD"
#define ETHERNET_IP "192.168.1.15"
#define ETHERNET_RESET_PIN -1
#define ETHERNET_CS_PIN 5

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

bool shouldSaveConfig = false;
WiFiManager wm;
WebServer server(80);

const char* mqtt_server = "mqtt.eclipseprojects.io";
const char* mqtt_username = "";
const char* mqtt_password = "";
const int mqtt_port = 1883;
const int max_retry = 10;
static int curRetry = 0;
const int max_retry_mqtt = 10;
static int curRetryMqtt = 0;
unsigned long lastMsg = 0;
static int ethConnectRetry=0;

WiFiClient wifiClient;
EthernetClient ethClient;
PubSubClient client;

const char* led_Topic = "/topic/qos0";
const char* temp_humid_Topic = "home/sensor/TemperatureandHumid";

#define MSG_BUFFER_SIZE (50)
char msg[MSG_BUFFER_SIZE];

static const char* root_ca PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
...
-----END CERTIFICATE-----
)EOF";

DHT dht(4, DHT11);
const int led_pin = 2;

bool isEthernetConnected = false;

void macCharArrayToBytes(const char* str, byte* bytes) {
    for (int i = 0; i < 6; i++) {
        bytes[i] = strtoul(str, NULL, 16);
        str = strchr(str, ':');
        if (str == NULL || *str == '\0') {
            break;
        }
        str++;
    }
}

void connectEthernet() {
    byte* mac = new byte[6];
    macCharArrayToBytes(ETHERNET_MAC, mac);
    Ethernet.init(ETHERNET_CS_PIN);
    Ethernet.begin(mac);

    // Tentativa de estabelecer conexão
    // while(Ethernet.linkStatus() != LinkON)
    // {
    //   Serial.print(".");
    //   curRetry++;
    //   if(curRetry>max_retry){
    //     Serial.println("Connect to ethernet fail");
    //     curRetry=0;   
    //     break;
    //     delay(20);
    //   }
    //   delay(1000);
    // }
    delay(1000);
    Serial.print("Ethernet IP is: ");
    Serial.println(Ethernet.localIP());
    client.setClient(ethClient);
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);
    connectMQTT();
    isEthernetConnected = true;

}

void setup() {
    Serial.begin(115200);
    dht.begin();
    pinMode(led_pin, OUTPUT);
    delay(2000);
    setupWiFi();
    delay(300);
    if(WiFi.status() == WL_CONNECTED){
      client.setClient(wifiClient);
      client.setServer(mqtt_server, mqtt_port);
      client.setCallback(callback);
      connectMQTT();
    }

    delay(200);
}

void loop() {
    // Check WiFi connection status
    if (WiFi.status() != WL_CONNECTED && ethConnectRetry<1 ) {
        Serial.println("WiFi disconnected, connecting to Ethernet...");
        connectEthernet();
        ethConnectRetry++;
    }
    delay(3000);
    if (WiFi.status() == WL_CONNECTED || Ethernet.linkStatus() == LinkON) {
    client.loop();
    }
    delay(200);
    unsigned long now = millis();
    if (now - lastMsg > 2000) {
        lastMsg = now;
        float temp = dht.readTemperature();
        float humid = dht.readHumidity();

        String data = "";
        data += (String(temp, 2) + "-" + String(humid, 2));
        client.publish(temp_humid_Topic, data.c_str());
        client.publish("dhtTemp", String(dht.readTemperature()).c_str());
        client.publish("dhtHum", String(dht.readHumidity()).c_str());
    }


}

void connectMQTT() {
    while (!client.connected()) {
        Serial.print("Attempting MQTT connection…");
        String clientId = "ESP32Client-";
        clientId += String(random(0xffff), HEX);
        curRetryMqtt++;
        if (curRetryMqtt > max_retry_mqtt) {
            Serial.print("MQTT connection failed");
            break;
        }
        if (client.connect(clientId.c_str(), mqtt_username, mqtt_password,"lastWillTopic",2,true,"Offline")) {
            Serial.println("connected");
            client.subscribe(led_Topic);
            client.subscribe(temp_humid_Topic);
            client.publish("lastWillTopic","Online");
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" try again in 3 seconds");
            delay(3000);
        }
    }
}

void callback(char* topic, byte* payload, unsigned int length) {
    String incommingMessage = "";
    for (int i = 0; i < length; i++) incommingMessage += (char)payload[i];
    Serial.println("Message arrived [" + String(topic) + "] " + incommingMessage);

    if (strcmp(topic, led_Topic) == 0) {
        if (incommingMessage.equals("ON")) {
            digitalWrite(led_pin, 1);
        }
        if (incommingMessage.equals("OFF")) {
            digitalWrite(led_pin, 0);
        }
    }
    if (strcmp(topic, temp_humid_Topic) == 0) {
        if (incommingMessage.equals("Closethedoor")) {
            Serial.println("Closethedoor");
        }
    }
}

void saveConfigFile() {
    Serial.println(F("Saving configuration..."));
    StaticJsonDocument<512> json;
    File configFile = SPIFFS.open(JSON_CONFIG_FILE, "w");
    if (!configFile) {
        Serial.println("Failed to open config file for writing");
        return;
    }
    serializeJsonPretty(json, Serial);
    if (serializeJson(json, configFile) == 0) {
        Serial.println(F("Failed to write to file"));
    }
    configFile.close();
}

bool loadConfigFile() {
    Serial.println("Mounting File System...");
    if (SPIFFS.begin(false) || SPIFFS.begin(true)) {
        Serial.println("Mounted file system");
        if (SPIFFS.exists(JSON_CONFIG_FILE)) {
            Serial.println("Reading config file");
            File configFile = SPIFFS.open(JSON_CONFIG_FILE, "r");
            if (configFile) {
                Serial.println("Opened configuration file");
                StaticJsonDocument<512> json;
                DeserializationError error = deserializeJson(json, configFile);
                serializeJsonPretty(json, Serial);
                if (!error) {
                    Serial.println("Parsing JSON");
                    return true;
                } else {
                    Serial.println("Failed to load JSON config");
                }
            }
        }
    } else {
        Serial.println("Failed to mount FS");
    }
    return false;
}

void saveConfigCallback() {
    Serial.println("Should save config");
    shouldSaveConfig = true;
}

void configModeCallback(WiFiManager* myWiFiManager) {
    Serial.println("Entered Configuration Mode");
    Serial.print("Config SSID: ");
    Serial.println(myWiFiManager->getConfigPortalSSID());
    Serial.print("Config IP Address: ");
    Serial.println(WiFi.softAPIP());
    connectEthernet();
}

void setupWiFi() {
    if (!loadConfigFile()) {
        Serial.println(F("Forcing config mode as there is no saved config"));
    }
    WiFi.mode(WIFI_AP_STA);
    wm.resetSettings();
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.setAPCallback(configModeCallback);
    if (!wm.autoConnect("ESP32_Manager_AP", "1234567890")) {
        Serial.println("Failed to connect and hit timeout");
        delay(3000);
        ESP.restart();
        delay(5000);
    }
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    if (shouldSaveConfig) {
        saveConfigFile();
    }
}
