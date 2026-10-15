#include <Arduino.h>
#include <WiFi.h>
#include <functional>
#include <vector>
// LCD over I2C communication
#include <Wire.h>


// OTA
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>


// telnet serial
#define DEBUG_ON 1         // debug macros
#define DEBUG_USE_SERIAL 1 // output channel serial
#define DEBUG_USE_TELNET 1 // output channel telnet
#include "ESPTelnet.h"
ESPTelnet telnet;
// Telnet wird aus loop() und aus dem MDB-Task benutzt -> mit Mutex schützen
SemaphoreHandle_t telnetMutex = nullptr;
#define printCLI(x)                                                    \
  do                                                                   \
  {                                                                    \
    if (telnetMutex)                                                   \
      xSemaphoreTakeRecursive(telnetMutex, portMAX_DELAY);             \
    telnet.println(x);                                                 \
    if (telnetMutex)                                                   \
      xSemaphoreGiveRecursive(telnetMutex);                            \
    Serial.println(x);                                                 \
  } while (0)

#include "mdbMaster.h"
#include "mbedtls/md.h"

#define ST(A) #A
#define STR(A) ST(A)

// Include Library
#include <SimpleCLI.h>
// Create CLI Object
SimpleCLI cli;

// Commands
Command wifi;
Command reboot;



#include <PubSubClient.h>
#include <esp_task_wdt.h>
#include "secrets.h"

// Hardware-Watchdog: Reset wenn loop() oder MDB-Task so lange hängen
#define TASK_WDT_TIMEOUT_S 30

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// PubSubClient ist nicht threadsicher: Nachrichten aus dem MDB-Task werden
// hier gepuffert und ausschließlich in loop() gesendet
struct MqttMsg
{
  char topic[32];
  char payload[64];
};
QueueHandle_t mqttQueue = nullptr;
TaskHandle_t mdbTaskHandle = nullptr;
bool resetReasonSent = false;

void mqttPublishAsync(const char *topic, const char *payload)
{
  if (!mqttQueue)
    return;
  MqttMsg msg;
  strlcpy(msg.topic, topic, sizeof(msg.topic));
  strlcpy(msg.payload, payload, sizeof(msg.payload));
  xQueueSend(mqttQueue, &msg, 0); // bei voller Queue verwerfen statt blockieren
}

const char *ntpServer = "de.pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 0;
bool freeVend = 0;
// werden aus loop() und dem MDB-Task benutzt
volatile bool aktivmqtt = false;
volatile uint8_t requ_dispenser = 0;
uint16_t prod = 0;
uint16_t produktmqtt =0;
volatile float pricemqtt = 0.0;
volatile unsigned long wd_time_l = 300;
unsigned long wd_ten_l =0;
unsigned long Sek_timer=0, ten_Sek_timer =0;
uint32_t funds = 0;
String input = "";
uint8_t newByte = '\0';
volatile bool manualProduct = false;
// MDB functions and callback
// variable containing the product number to be dispensed
volatile uint8_t dispenser = 0;
// function to look up the price for the product in question

mdbProduct getProduct(uint16_t productID)
{
  float price = 0;
  String prodPrice = "80";
  price = prodPrice.toFloat();
  Serial.printf("Default Price is %.2f\r\n", price);

  // Auswahl Preis nach Produkt
  String prodName = "unbek.";
  uint16_t numPrices = 0; // cfg_doc["price_list"].size();
  Serial.printf("%d List entries\r\n", numPrices);
  for (size_t i = 0; i < numPrices; i++)
  {
    if (0 /*cfg_doc["price_list"][i]["Id"]*/ == productID)
    {
      printCLI("Price for Product");
      prodName = "Produkt X"; // cfg_doc["price_list"][i]["Name"].as<String>();
      printCLI(prodName);
      printCLI("is");
      String prodPrice = "81"; // cfg_doc["price_list"][i]["Price"];
      price = prodPrice.toFloat();
      printCLI(price);
      break;
    }
  }

  Serial.printf("Final Price is %.2f\r\n", price);
  return mdbProduct(productID, prodName.c_str(), mdbCurrency(price, 1, 2));
}

class PInterface : public IPayment
{
private:
  // the device staring the session
  IPaymentDevice *sessionDev = nullptr;
  // selected product with price ans name
  mdbProduct myProduct;
  // time of vend start
  uint32_t vendStart = 0;
  // array of all mdb devices with funds
  std::vector<IPaymentDevice *> devicesWithFunds;

  
  // product select made by customer
  //aktivmqtt=false; //set true when selection was made
   //which number of product has been selected


public:
  // helper function giving the total available credit
  float getTotalFunds()
  {
    float total = 0;
    if (sessionDev)
    {
      total += sessionDev->getCurrentFunds().getValue();
    }

    for (auto const &rdev : devicesWithFunds)
    {
      total += rdev->getCurrentFunds().getValue();
    }
    return total;
  }

  float getMaxDevFunds()
  {
    float max = 0;
    if (sessionDev)
    {
      if (sessionDev->getCurrentFunds().getValue() > max)
        max = sessionDev->getCurrentFunds().getValue();
    }

    for (auto const &rdev : devicesWithFunds)
    {
      if (rdev->getCurrentFunds().getValue() > max)
        max = rdev->getCurrentFunds().getValue();
    }
    return max;
  }
  // event fired when cashless device wants to print something on the screen
  void requestText(IPaymentDevice *dev, char *msg, uint32_t duration)
  {
    printCLI(msg);
    // TODO: clear the screen after duration
     Serial.printf("for %d ms\n\r",  duration);
  }

  // event fired when a revalue is requested by a card reader
  void requestRevalue(IPaymentDevice *dev)
  {
  }

  // event fired when a payment session is about to be started by a cashless device
  bool startSession(IPaymentDevice *dev)
  {
    if (sessionDev)
      return false;
    Serial.print("Start Session Requested on ");
    printCLI(dev->getDeviceName());
    Serial.printf("\tFunds Available:  %.2f\r\n", getTotalFunds());
    sessionDev = dev;
    char line2[48];
    snprintf(line2, sizeof(line2), "Guthaben:  %.2f", dev->getCurrentFunds().getValue());
    mqttPublishAsync("/Automat/Status", line2);
    return true;
  }

  // event fired when session is ended
  void endSession(IPaymentDevice *dev)
  {
    if (sessionDev)
    {
      aktivmqtt=false;
      printCLI("End of Session");
      sessionDev = nullptr;
      manualProduct = false;
      // TODO: tell the user its all wrapped up


    mqttPublishAsync("/Automat/Status", "Guthaben:  0.00");
    }
  }

  // event fired when fund changed on the system
  void fundsChanged(IPaymentDevice *dev)
  {
    // if the device has funds, we need to add to list
    if (dev->getCurrentFunds().getValue() > 0)
    {
      if (std::find(devicesWithFunds.begin(), devicesWithFunds.end(), dev) == devicesWithFunds.end())
      {
        devicesWithFunds.push_back(dev);
      }
    }
    else
    {
      // if the device has no funds, we need to remove from list
      if (std::find(devicesWithFunds.begin(), devicesWithFunds.end(), dev) != devicesWithFunds.end())
      {
        devicesWithFunds.erase(std::find(devicesWithFunds.begin(), devicesWithFunds.end(), dev));
      }
    }
    // For specific device
    Serial.print("Funds changed on ");
    printCLI(dev->getDeviceName());
    Serial.printf("\tFunds Available:  %.2f\r\n", dev->getCurrentFunds().getValue());
    // end specific device

    // TODO: so something with all the new funds value
    char line2[48];
    snprintf(line2, sizeof(line2), "Guthaben:  %.2f", getTotalFunds());
    mqttPublishAsync("/Automat/Status", line2);


  }

  // event fired when the payment devices authorised the payment
  void vendAccepted(IPaymentDevice *dev, mdbCurrency c)
  {

    char line1[48];
    snprintf(line1, sizeof(line1), "Produkt %d Preis %.2f", myProduct.getNumber(), c.getValue());

    mqttPublishAsync("/Automat/Status", line1);
    char line2[48];
    snprintf(line2, sizeof(line2), "Produkt: %s", myProduct.getName());


    Serial.printf("Vend Accepted on %s for %.2f\r\n", dev->getDeviceName(), c.getValue());
    vendStart = millis();
    
    //set the dispenser var to start output
    dispenser = myProduct.getNumber();
  }

  // event fired when the payment device denies the vend
  void vendDenied(IPaymentDevice *dev)
  {
  
    char line1[48];
    snprintf(line1, sizeof(line1), "Produkt %d Preis %.2f", myProduct.getNumber(), myProduct.getPrice()->getValue());
    mqttPublishAsync("/Automat/Status", line1);

    char line2[48];
    snprintf(line2, sizeof(line2), "Produkt: %s", myProduct.getName());
  
    Serial.printf("Vend Denied on %s\r\n", dev->getDeviceName());
  }

  //loop function running continuously
  PaymentActions::Action update(IPaymentDevice *dev)
  {
    //someone wants their money back - allow?
    if (dev->pendingEscrowRequest())
    {
      return PaymentActions::REQUEST_REVALUE;
    }

    if (dev->getCurrentFunds().getValue() > 0)
    {
      // handle product selection
      if (aktivmqtt)
      {
        switch (requ_dispenser)
        {
        case 1: // Peanuts
          myProduct = mdbProduct(requ_dispenser, "Peanuts", mdbCurrency(pricemqtt, 100, 2));
          
          break;

        case 2: // Ritter Sport
          myProduct = mdbProduct(requ_dispenser, "Ritter Sport", mdbCurrency(0.05f, 1, 2));
          break;

        case 3: // Smarties
          myProduct = mdbProduct(requ_dispenser, "Smarties", mdbCurrency(0.10f, 1, 2));
          break;

        default:
          break;
        }

        if ((myProduct.getPrice()->getValue() <= dev->getCurrentFunds().getValue()))
        {
          Serial.printf("Requesting Product: %s on %s for %.2f\r\n", myProduct.getName(), dev->getDeviceName(), myProduct.getPrice()->getValue());
          sessionDev = dev;
          dispenser = requ_dispenser;
          manualProduct = true;
          return PaymentActions::REQUEST_PRODUCT;
        }
        else
        {
          if (myProduct.getPrice()->getValue() >= dev->getCurrentFunds().getValue())
          {
            printCLI("Guthaben nicht ausreichend!");
          }
          dispenser = 0;

          aktivmqtt=false;
          printCLI("End of Session");
          sessionDev = nullptr;
          manualProduct = false;
        }
      }
    }
    return PaymentActions::NO_ACTION;
  }

  PaymentActions::Action updateSession(IPaymentDevice *dev)
  {
    if (dev == sessionDev)
    {
      // handle product selection
      if (aktivmqtt)
      {
        //determine which selection means which product
        switch (requ_dispenser)
        {
        case 1: // Peanuts
          myProduct = mdbProduct(requ_dispenser, "Peanuts", mdbCurrency(pricemqtt, 100, 2));
          break;

        case 2: // Ritter Sport
          myProduct = mdbProduct(requ_dispenser, "Ritter Sport", mdbCurrency(0.05f, 1, 2));
          break;

        case 3: // Smarties
          myProduct = mdbProduct(requ_dispenser, "Smarties", mdbCurrency(0.10f, 1, 2));
          break;

        default:
          break;
        }
       // Serial.printf("Requesting Product: %s on %s for %.2f\r\n", myProduct.getName(), dev->getDeviceName(), myProduct.getPrice()->getValue());
       manualProduct = true;
        return PaymentActions::REQUEST_PRODUCT;
      }
    }
    else
    {
      // Check if Other payment devices have money to revalue
      uint8_t foundDevices = 0;
      for (auto const &rdev : devicesWithFunds)
      {
        Serial.printf("Funds: %.2f on %s\r\n", rdev->getCurrentFunds().getValue(), rdev->getDeviceName());
        foundDevices++;
      }

      // revalue if we can, else vend
      if (foundDevices && dev->canRevalue())
      {
        // revalue if we can
        printCLI("Requesting Revalue");
        return PaymentActions::REQUEST_REVALUE;
      }
    }
    return PaymentActions::NO_ACTION;
  }

  //event fired after vend accept till dispense is dones
  PaymentActions::Action updateDispense(IPaymentDevice *dev)
  {
    if (dev == sessionDev)
    {
      if (manualProduct)
      {
        // dispenser = 1;
        if (millis() - vendStart > 1000)
        {
          dispenser = 0;
          return PaymentActions::PRODUCT_DISPENSE_SUCCESS;
        }else{
          // if vending failed, do this
          // return PaymentActions::PRODUCT_DISPENSE_FAILURE;
        }

      }
    }
    return PaymentActions::NO_ACTION;
  }

  mdbProduct *getCurrentProduct(IPaymentDevice *dev)
  {
    return &myProduct;
  }

  mdbCurrency getPendingRevalueAmount(IPaymentDevice *dev)
  {
    if (dev == sessionDev)
    {
      float total = 0;
      for (auto const &rdev : devicesWithFunds)
      {
        total += rdev->getCurrentFunds().getValue();
      }
      return mdbCurrency(total, 1, 2);
    }
    else
    {
      return dev->getCurrentFunds();
    }
  }

  void payoutEnd(IPaymentDevice *dev, bool success)
  {
    if (success)
    {
      printCLI("Auszahlung erfolgt!");
      printCLI(getPendingRevalueAmount(dev).getValue());
    }
    else
    {
      printCLI("Kein Wechselgeld!");
      printCLI(getPendingRevalueAmount(dev).getValue());
    }
  }

  void revalueAccepted(IPaymentDevice *dev)
  {
    Serial.printf("%5.2f EUR aufgeladen!", getPendingRevalueAmount(dev).getValue());
    printCLI("Revalue was accepted");
    for (auto const &rdev : devicesWithFunds)
    {
      rdev->clearCurrentFunds();
    }
    devicesWithFunds.clear();
  }

  void revalueDenied(IPaymentDevice *dev)
  {
    printCLI("Revalue was denied");
  }
};

PInterface myInterface;

void MDBRUN(void *args)
{
  // char tmp[30];
  mdbMaster::init();
  MDBDevice_Cashless_1.registerPaymentInterface(&myInterface);
  // MDBDevice_Cashless_2.registerPaymentInterface(&myInterface);
  // MDBDevice_Changer.registerPaymentInterface(&myInterface);
  esp_task_wdt_add(NULL);
  while (1)
  {
    mdbMaster::handleDevices();
    esp_task_wdt_reset();
    delay(10);
  }
}

const char *resetReasonStr()
{
  switch (esp_reset_reason())
  {
  case ESP_RST_POWERON:   return "PowerOn";
  case ESP_RST_EXT:       return "Extern";
  case ESP_RST_SW:        return "Software";
  case ESP_RST_PANIC:     return "Panic/Absturz";
  case ESP_RST_INT_WDT:   return "Interrupt-Watchdog";
  case ESP_RST_TASK_WDT:  return "Task-Watchdog";
  case ESP_RST_WDT:       return "Watchdog";
  case ESP_RST_DEEPSLEEP: return "DeepSleep";
  case ESP_RST_BROWNOUT:  return "Brownout/Unterspannung";
  case ESP_RST_SDIO:      return "SDIO";
  default:                return "Unbekannt";
  }
}

// läuft innerhalb von mqttClient.loop(), also im loop()-Task
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  char value[32];
  size_t n = min((size_t)length, sizeof(value) - 1);
  memcpy(value, payload, n);
  value[n] = '\0';

  if (!strcmp(topic, "/Automat/Aktiv"))
  {
    pricemqtt = (n > 0) ? String(value).toFloat() : 0.0;
    requ_dispenser = 1;
    aktivmqtt = true;
  }

  if (!strcmp(topic, "/Automat/beenden"))
  {
    aktivmqtt = false;
    printCLI("End of Session");
    manualProduct = false;
  }

  if (!strcmp(topic, "/Automat/Watchdog"))
  {
    wd_time_l = String(value).toInt();
  }

  Serial.printf("(Publish) Topic: %s Value= %s\r\n", topic, value);
}

// nicht blockierender Reconnect, max. ein Versuch alle 5 s
void mqttReconnect()
{
  static unsigned long lastAttempt = 0;
  if (millis() - lastAttempt < 5000)
    return;
  lastAttempt = millis();

  Serial.println("[MQTT] Verbinde Broker...");
  if (mqttClient.connect("esp8266-123456", MQTT_USER, MQTT_PASS))
  {
    Serial.println("[MQTT] verbunden");
    mqttClient.subscribe("/Automat/Preis");
    mqttClient.subscribe("/Automat/Aktiv");
    mqttClient.subscribe("/Automat/beenden");
    mqttClient.subscribe("/Automat/Watchdog");
    if (!resetReasonSent)
    {
      mqttClient.publish("/Automat/ResetReason", resetReasonStr(), true);
      resetReasonSent = true;
    }
  }
  else
  {
    Serial.printf("[MQTT] fehlgeschlagen, rc=%d\r\n", mqttClient.state());
  }
}

// WLAN neu anstoßen, wenn es länger als 60 s weg ist
void wifiCheck()
{
  static unsigned long lostSince = 0;
  if (WiFi.status() == WL_CONNECTED)
  {
    lostSince = 0;
    return;
  }
  if (lostSince == 0)
  {
    lostSince = millis();
  }
  else if (millis() - lostSince > 60000)
  {
    printCLI("[WiFi] Verbindung weg, neuer Versuch");
    WiFi.disconnect();
    WiFi.begin(SSID, WIFI_PASS);
    lostSince = millis();
  }
}

void printWiFiStatus()
{
  switch (WiFi.status())
  {
  case WL_NO_SSID_AVAIL:
    printCLI("[WiFi] SSID not found");
    break;
  case WL_CONNECT_FAILED:
    Serial.print("[WiFi] Failed - WiFi not connected! Reason: ");
    return;
    break;
  case WL_CONNECTION_LOST:
    printCLI("[WiFi] Connection was lost");
    break;
  case WL_SCAN_COMPLETED:
    printCLI("[WiFi] Scan is completed");
    break;
  case WL_DISCONNECTED:
    printCLI("[WiFi] WiFi is disconnected");
    break;
  case WL_CONNECTED:
    printCLI("[WiFi] WiFi is connected!");
    Serial.print("[WiFi] IP address: ");
    printCLI(WiFi.localIP());
    break;
  default:
    Serial.print("[WiFi] WiFi Status: ");
    printCLI(WiFi.status());
    break;
  }
}

// Callback function for wifi command
void wifiCallback(cmd *c)
{
  Command cmd(c); // Create wrapper object
  int numArgs = cmd.countArgs();
  if (!numArgs)
  {
    printCLI("zu wenige Argumente");
    return;
  }
  Argument argSubCmd = cmd.getArg(0);
  String subCmd = argSubCmd.getValue();
  if (!strcmp(subCmd.c_str(), "status"))
  {
    printWiFiStatus();
  }
  else if (!strcmp(subCmd.c_str(), "connect") && numArgs > 2)
  {
    WiFi.disconnect(false, true);
    Argument argSSID = cmd.getArg(1);
    String wiFiName = argSSID.getValue();

    Argument argPw = cmd.getArg(2);
    String wiFiPassword = argPw.getValue();

    WiFi.begin(wiFiName.c_str(), wiFiPassword.c_str());

    // Wait for the WiFi event
unsigned long start = millis();

while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    printWiFiStatus();
    delay(500);
}
if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Verbindung fehlgeschlagen, fährt neu hoch...");
    ESP.restart(); // oder alternative Behandlung
}


  }
  else if (!strcmp(subCmd.c_str(), "disconnect"))
  {
    WiFi.disconnect(false, true);
  }
}

// Callback function for reboot command
void rebootCallback(cmd *c)
{
  ESP.restart();
}

// Callback in case of an error
void errorCallback(cmd_error *e)
{
  CommandError cmdError(e); // Create wrapper object

  Serial.print("ERROR: ");
  printCLI(cmdError.toString());

  if (cmdError.hasCommand())
  {
    Serial.print("Did you mean \"");
    Serial.print(cmdError.getCommand().toString());
    printCLI("\"?");
  }
}

void setup()
{
  delay(1000);
  // Pins and Ports
  Serial.begin(115200);
  Wire.setPins(13, 14);

  Serial.printf("Reset-Grund: %s\r\n", resetReasonStr());

  telnetMutex = xSemaphoreCreateRecursiveMutex();
  mqttQueue = xQueueCreate(20, sizeof(MqttMsg));

  // Task-Watchdog konfigurieren (Reset bei Hänger), loop() überwachen
  esp_task_wdt_init(TASK_WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  xTaskCreatePinnedToCore(MDBRUN, "MDB Master Runner", 8192, NULL, 1, &mdbTaskHandle, 1);


  Serial.println("Booting");
  Serial.print("ESP-MDB-Master Interface vers.:");

  Serial.println(STR(CODE_VERSION));

  // enable cli on serial port
  cli.setOnError(errorCallback); // Set error Callback
  wifi = cli.addBoundlessCommand("wifi", wifiCallback);
  reboot = cli.addCommand("reboot", rebootCallback);

  // wifi connection
  Serial.println("Verbinde WLAN");
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("ESP32-MDB-Master");
  WiFi.setAutoReconnect(true);
  WiFi.begin(SSID, WIFI_PASS);

  // Verbindung wird nicht blockierend in loop() aufgebaut
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setSocketTimeout(5);

  ArduinoOTA
      .onStart([]()
               {
      // OTA blockiert loop() länger als der Watchdog erlaubt
      esp_task_wdt_delete(NULL);
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type); })
      .onEnd([]()
             { Serial.println("\nEnd"); })
      .onProgress([](unsigned int progress, unsigned int total)
                  { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); })
      .onError([](ota_error_t error)
               {
      esp_task_wdt_add(NULL);
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed"); });

  ArduinoOTA.begin();

  // telnet setup
  telnet.onConnect([](String ip)
                   {
  Serial.print("- Telnet: ");
  Serial.print(ip);
  Serial.println(" connected");
  
  telnet.println("\nWelcome " + telnet.getIP());
  telnet.println("(Use ^] + q  to disconnect.)"); });
  telnet.onConnectionAttempt([](String ip)
                             {
  Serial.print("- Telnet: ");
  Serial.print(ip);
  Serial.println(" tried to connected"); });
  telnet.onReconnect([](String ip)
                     {
  Serial.print("- Telnet: ");
  Serial.print(ip);
  Serial.println(" reconnected"); });
  telnet.onDisconnect([](String ip)
                      {
  Serial.print("- Telnet: ");
  Serial.print(ip);
  Serial.println(" disconnected"); });
  telnet.onInputReceived([](String str)
                         {
    // checks for a certain command
    if (str == "quit")
    {
      telnet.println("> disconnecting you...");
      telnet.disconnectClient();
    }
    else
    {
      //let cli parse it
      printCLI(str);
      cli.parse(str);
      char tmp[300] = "";
      tmp[0] = '\0';
      strcpy(tmp, str.c_str());
      strcat(tmp, "\r\n");
    } });

  DEBUG_INFO;
  Serial.print("- Telnet: ");
  if (telnet.begin(23))
  {
    Serial.println("telent running");
  }

  printCLI(WiFi.localIP().toString());
  // DEBUG_WHERE;
  // DEBUG_MSG("Prints debug string");
  // DEBUG_VAR(a);
}







void loop()
{
  esp_task_wdt_reset();

  wifiCheck();
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected())
  {
    mqttReconnect();
  }
  mqttClient.loop();

  // gepufferte Nachrichten aus dem MDB-Task senden
  MqttMsg msg;
  while (xQueueReceive(mqttQueue, &msg, 0) == pdTRUE)
  {
    if (mqttClient.connected())
      mqttClient.publish(msg.topic, msg.payload);
  }

  // mimic Serial Console
  if (Serial.available())
  {
    newByte = Serial.read();

    if (newByte == '\n')
    {
      Serial.println();
      cli.parse(input);

      input = ""; // reset
      Serial.print("#");
    }
    else if (newByte != '\r' && input.length() < 128)
    {
      input += (char)newByte;
      Serial.print((char)newByte);
    }
  }
  if (cli.errored())
  {
    CommandError cmdError = cli.getError();

    printCLI("ERROR: ");
    printCLI(cmdError.toString());

    if (cmdError.hasCommand())
    {
      printCLI("Did you mean \"");
      printCLI(cmdError.getCommand().toString());
      printCLI("\"?");
    }
  }





 if (millis() - ten_Sek_timer >= 10000)
{
    ten_Sek_timer = millis();
    if (wd_ten_l > 0) wd_ten_l--;

    char tenchr[20];
    snprintf(tenchr, sizeof(tenchr), "%2lu", wd_ten_l);
    if (mqttClient.connected())
      mqttClient.publish("/Automat/WDESP", tenchr);
    Serial.println(wd_ten_l);

    // Diagnose: freier Heap und Rest-Stack des MDB-Tasks
    char diag[64];
    snprintf(diag, sizeof(diag), "heap=%u minheap=%u stackMDB=%u",
             ESP.getFreeHeap(), ESP.getMinFreeHeap(),
             mdbTaskHandle ? uxTaskGetStackHighWaterMark(mdbTaskHandle) : 0);
    if (mqttClient.connected())
      mqttClient.publish("/Automat/Diag", diag);
    Serial.println(diag);
}


 

if (millis() - Sek_timer >= 1000)
{
    Sek_timer = millis();
    if (wd_time_l > 0) wd_time_l--;
    Serial.println(wd_time_l);
}



if (wd_time_l <= 0){  //MQTT Watchdog in case of connection problems--> restart
 ESP.restart();
Serial.println(wd_time_l);
}

// immer aufrufen, sonst werden neue Telnet-Verbindungen nicht angenommen
xSemaphoreTakeRecursive(telnetMutex, portMAX_DELAY);
telnet.loop();
xSemaphoreGiveRecursive(telnetMutex);
ArduinoOTA.handle();

}