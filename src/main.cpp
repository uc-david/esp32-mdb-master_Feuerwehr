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
#define printCLI(x)  \
  telnet.println(x); \
  Serial.println(x)

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



#include <AsyncMQTT.h>
#include "secrets.h"


#ifdef ARDUINO_ARCH_ESP32
      #include <soc/soc.h>
      #include "soc/rtc_cntl_reg.h"
#endif



AsyncMQTT mqttClient(MQTT_USER,MQTT_PASS,MQTT_HOST,MQTT_PORT);




const char *ntpServer = "de.pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 0;
bool freeVend = 0; 
bool aktivmqtt = false;
uint8_t requ_dispenser = 0;
uint16_t prod = 0; 
uint16_t produktmqtt =0;
float pricemqtt = 0.0;
unsigned long wd_time_l = 300, wd_ten_l =0;
unsigned long Sek_timer=0, ten_Sek_timer =0;
String pricemqttstr = "";
String wd_time_str = "";
uint32_t funds = 0;
String input = "";
uint8_t newByte = '\0';
  bool manualProduct = false;
// MDB functions and callback
// variable containing the product number to be dispensed
uint8_t dispenser = 0;
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
    char line2[20];
    sprintf(line2, "Guthaben:  %.2f", dev->getCurrentFunds().getValue());
    mqttClient.publish("/Automat/Status", line2);
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


    mqttClient.publish("/Automat/Status", "Guthaben:  0.00");
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
    char line2[20];
    sprintf(line2, "Guthaben:  %.2f", getTotalFunds());
    mqttClient.publish("/Automat/Status", line2);


  }

  // event fired when the payment devices authorised the payment
  void vendAccepted(IPaymentDevice *dev, mdbCurrency c)
  {

    char line1[20];
    sprintf(line1, "Produkt %d Preis %.2f", myProduct.getNumber(), c.getValue());
  
    mqttClient.publish("/Automat/Status", line1);
    char line2[20];
    sprintf(line2, "Produkt: %s", myProduct.getName());
  

    Serial.printf("Vend Accepted on %s for %.2f\r\n", dev->getDeviceName(), c.getValue());
    vendStart = millis();
    
    //set the dispenser var to start output
    dispenser = myProduct.getNumber();
  }

  // event fired when the payment device denies the vend
  void vendDenied(IPaymentDevice *dev)
  {
  
    char line1[20];
    sprintf(line1, "Produkt %d Preis %.2f", myProduct.getNumber(), myProduct.getPrice()->getValue());
    mqttClient.publish("/Automat/Status", line1);
  
    char line2[20];
    sprintf(line2, "Produkt: %s", myProduct.getName());
  
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
  while (1)
  {
    mdbMaster::handleDevices();
    delay(10);
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
    int tryDelay = 1000;
    int numberOfTries = 10;

    // Wait for the WiFi event
    while (true)
    {
      printWiFiStatus();
      if (WiFi.status() == WL_CONNECTED)
      {
        return;
      }
      delay(tryDelay);

      if (numberOfTries <= 0)
      {
        Serial.print("[WiFi] Failed to connect to WiFi!");
        // Use disconnect function to force stop trying to connect
        WiFi.disconnect();
        return;
      }
      else
      {
        numberOfTries--;
      }
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

  xTaskCreatePinnedToCore(MDBRUN, "MDB Master Runner", 2048, NULL, 1, NULL, 1);


  Serial.println("Booting");
  Serial.print("ESP-MDB-Master Interface vers.:");

  Serial.println(STR(CODE_VERSION));

  // enable cli on serial port
  cli.setOnError(errorCallback); // Set error Callback
  wifi = cli.addBoundlessCommand("wifi", wifiCallback);
  reboot = cli.addCommand("reboot", rebootCallback);

  // wifi connection
  Serial.println("Verbinde WLAN");
  WiFi.setHostname("ESP32-MDB-Master");
  WiFi.begin();



#if defined(ESP32)
            WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif

 mqttClient.begin(SSID,WIFI_PASS);
      
      mqttClient.subscribe("/Automat/Preis");
      mqttClient.subscribe("/Automat/Aktiv");
      mqttClient.subscribe("/Automat/beenden");
      mqttClient.subscribe("/Automat/Watchdog");
    // Alle topics.
      for (const auto& [topic, value] : mqttClient.getTopics()){
            Serial.printf("Topic: %s state = %s\r\n",topic.c_str(),value.ultimoEstado.c_str());
      }




  ArduinoOTA
      .onStart([]()
               {
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
    else if (newByte != '\r')
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





if(mqttClient.isPublishReceived()){

       if (mqttClient.topicReceived == "/Automat/Aktiv") {
           pricemqttstr = mqttClient.valueReceived.c_str();
           pricemqtt= pricemqttstr.toFloat();
           aktivmqtt = true;
           requ_dispenser=1;
           Serial.println(aktivmqtt);
           Serial.println(pricemqttstr);
           Serial.println(pricemqtt);

      }


       if (mqttClient.topicReceived == "/Automat/beenden") {
      aktivmqtt=false;
      printCLI("End of Session");
      manualProduct = false;

      }




      if (mqttClient.topicReceived == "/Automat/Watchdog") {
           wd_time_str = mqttClient.valueReceived.c_str();
           wd_time_l= wd_time_str.toInt();
          // Serial.println(wd_time_str);
          // Serial.println(wd_time_l);

      }

            Serial.printf("(Publish) Topic: %s Value= %s\r\n",mqttClient.topicReceived.c_str(),mqttClient.valueReceived.c_str());
      }


if (millis() > ten_Sek_timer)
 {ten_Sek_timer=(millis()+10000);
 wd_ten_l = wd_ten_l+1;


    char tenchr[20];
    sprintf(tenchr, "%2d",wd_ten_l);

 mqttClient.publish("/Automat/WDESP", tenchr);
Serial.println(wd_ten_l);
}


 


if (millis() > Sek_timer)
 {Sek_timer=(millis()+1000);

  wd_time_l = wd_time_l-1;  //Comment, if watchdog is not necessary
Serial.println(wd_time_l);
}


if (wd_time_l <= 0){  //MQTT Watchdog in case of connection problems--> restart
 ESP.restart();
Serial.println(wd_time_l);
}



  // Handle OTA packets
  ArduinoOTA.handle();
  // handle telnet packets
  telnet.loop();
}