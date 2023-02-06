#include "bitmaps.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <TFT_eSPI.h> 
#include <SPI.h>
#include <Bme280.h>
#include "SparkFun_AS3935.h"

const int SW2             = 18;
const int SW1             = 6;
//const int INDOOR        = 0x12;  // defined in SparkFun_AS3935.h
//const int OUTDOOR       = 0xE;   // defined in SparkFun_AS3935.h
const int LIGHTNING_INT   = 0x08;
const int DISTURBER_INT   = 0x04;
const int NOISE_INT       = 0x01;

SparkFun_AS3935 as3935;

// Wifi Information
const char* ssid        = "xxxxxxxx";
const char* password    = "xxxxxxxx";

// needed for Open Weather APIs
String my_Api_Key       = "00000000000000000000000000000000";  //specify your open weather api key
String my_city          = "New Bern";       //specify your city
String my_country_code  = "US";             //specify your country code

String json_array;            // returned from OpenWeather

// needed for AS3539 lightning detector
const int AS3935_INT     = 15; // Interrupt pin for lightning detection 
const int SPI_CS         = 13; //SPI chip select pin
const int SPI_RX         = 12; //SPI MISO
const int SPI_TX         = 11; //SPI MOSI
const int SPI_SCK        = 10; //SPI SCK

// variable for deatling with the lightning detector
int noiseCount            = 0; // How many noise interrupts
int distCount             = 0; // How many disturber interrupts
int lightingCount         = 0; // How many lightning strikes
int lastLightingDistance  = 0; // Distance measure from last strike
const int noiseAdjStart   = 9; // Self adjust the noise level
int noiseAdj              = noiseAdjStart;
const int disturbAdjStart = 9; // Self adjust the disturber level
int disturbAdj            = 0;


const long DEBOUNCE      = 200;  // in milliseconds
volatile unsigned long last_micros = 0; // used to measure debounce time.

// This variable holds the number representing the lightning or non-lightning
// event issued by the lightning detector. 
int intVal                = 0;
int noiseSetting          = 2; // Value between 1-7 
int disturberSetting      = 2; // Value between 1-10

// The defined screens to show
typedef enum SCREENS {

  screenIndoor     = 1,
  screenOutdoor    = 2,
  screenLightning  = 3

} screens; 

volatile int displayedScreen = 0;  // The present screen

// Flags set by interrupt routines
volatile bool shouldAdvanceScreen  = false;
volatile bool shouldCheckLightning = false;

// The Temperature/Humity sensor
Bme280TwoWire sensor;

// The Screen
TFT_eSPI tft = TFT_eSPI();  //pins defined in User_Setup.h

void setup() {

  // first setup the input pins used by the switched and the int pin on the AS3539
  pinMode(SW2, INPUT_PULLDOWN);
  pinMode(SW1, INPUT_PULLDOWN);   // Not presently used
  pinMode(AS3935_INT, INPUT_PULLDOWN); 

  // Start the serial port
  Serial.begin(9600);

  // Initialize i2c for the temperature/humidity sensor
  Wire.begin();

  // Set up the temperature/humidity sensor
  sensor.begin(Bme280TwoWireAddress::Primary);
  sensor.setSettings(Bme280Settings::indoor());

  // Set up the Screen
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);

  // Not needed but give a person 5 seconds to start the Serial Monitor since the Pico doesn't
  // like it to be kept open
  delay(5000);
  Serial.println ("Starting..."); 

  // Set up the SPI 1 hardware
  SPI1.setRX(SPI_RX);
  SPI1.setTX(SPI_TX);
  SPI1.setSCK(SPI_SCK);
  SPI1.setCS(SPI_CS);
  // Start the SPI 1 channel and tell it we are using a hardware chip select
  SPI1.begin(true); 

  // Setup the AS3539 Lightning Detector
  if( !as3935.beginSPI(SPI_CS,1000000,SPI1 ) ){ 
    String errorString = "Lightning Detector did not start up, freezing!";
    Serial.println (errorString);
    tft.drawString(errorString, 20, 40, 4);
         
    while(1)
    {
        Serial.println ("Lightning Detector hung...");
        delay(1000); 
    }
  }
  else {
    Serial.println("Lightning Detector Ready!");

    // Start the detector in a known state
    as3935.resetSettings();
  }
  // if you know you have a lot of disturber noise, you can set this to true.
  // It will get set to true if we detect too much
  as3935.maskDisturber(false);

  // Get the WiFi up and working.  
  show_wifi_setup();

  // put up the initial screen
  show_BME280_measurements();

  // configure the Switch and detector INT pin to trigger interrupts
  attachInterrupt(digitalPinToInterrupt(SW2), screenIRQ, RISING);
  attachInterrupt(digitalPinToInterrupt(AS3935_INT), checkLightningIRQ, RISING);
  
  // The as3935 seems to start with the Int high, so we clear it by readding the Interrupt Register
  as3935.readInterruptReg();  //clear it to start
}

//*****************************************************
// Now its time to party...
//*****************************************************
void loop() {

  // Was the button pushed?
  if (shouldAdvanceScreen)
  {
    delay(DEBOUNCE);   // poor man's debounce
    advanceScreen();
    shouldAdvanceScreen = false;  // clear interrupt flag
  }
  // Was there a Lightning Strike?
  if (shouldCheckLightning)
  {
    checkLightning();
    shouldCheckLightning = false;  // clear interrupt flag

    // force the screen to the lightning one.
    displayedScreen = screenLightning;
    show_Lightning();  
  }
  // The problem with the lightning detector INT line, is that it stays high until
  // the register is read.  Normally the IRQ sets the shouldCheckLightning which causes the 
  // checkLightning function to read it.  However, there is a slight timing change that it might happen otherwise
  // reading the reg is harmless but will insure we get the next IRQ
  as3935.readInterruptReg();  //clear it

  delay(100);  // Small delay
}

// Interrupt functions should do very little.  In this case, advance the screen to be display and 
// set the flag to move advance it in the main loop
void screenIRQ()
{
  if (displayedScreen++ > screenLightning) displayedScreen = screenIndoor;
  shouldAdvanceScreen = true;
}

// This function displays the screen that as set either in the screenIRQ interrupt of if we got
// a lightning strike
void advanceScreen()
{
    switch (displayedScreen)
    {
       case screenIndoor:
          show_BME280_measurements();
       break;
       case screenOutdoor:
          show_OpenWeather();
       break;
       case screenLightning:
          show_Lightning();
       break;
    }
}

// Interrupt functions should do very little.  This just sets a flag that tells the main
// loop it should get information from the lightning detector
void checkLightningIRQ()
{
  shouldCheckLightning = true;
}

// This function reads the interrupt register from the AS3935 and sets various values used in the 
// lightning screen.   It also attempts to reduce the noise if it see it is getting it.  That last 
// part could be made better as there is no measuring the gap between event, instead it is strickly 
// cumlative

void checkLightning()
{

    intVal = as3935.readInterruptReg();
    if(intVal == NOISE_INT){
      Serial.print("Noise.  Level: "); 
      // if we are getting too much noise, adjust it
      noiseCount++;
      if ((!noiseAdj--)&& (noiseSetting < 7))
      {
        as3935.setNoiseLevel(++noiseSetting); 
        noiseAdj = noiseAdjStart;
      }
      Serial.println(noiseSetting);
    }
    else if(intVal == DISTURBER_INT){
      Serial.print("Disturber. Count: "); 
      // if we are getting too much noise, adjust it
      if (distCount++ > disturbAdjStart * 11)
      {
          // the noise isn't going away so turn the irq off
          as3935.maskDisturber(true);
        
      }
      if ((!disturbAdj--) && (disturberSetting < 10))
      {
        as3935.watchdogThreshold(++disturberSetting);
        disturbAdj = disturbAdjStart;
      }
     
      Serial.println(distCount);  
      
    }
    else if(intVal == LIGHTNING_INT){
      Serial.println("Lightning Strike Detected!"); 
      // Lightning! Now how far away is it? Distance estimation takes into
      // account any previously seen events in the last 15 seconds. 
      byte distance = as3935.distanceToStorm(); 
      Serial.print("Approximately: "); 
      Serial.print(distance); 
      Serial.println("km away!"); 
      lastLightingDistance = distance;
      lightingCount++;
    }

}

// display the information we found about lightning strikes
void show_Lightning()
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("Lightning Detection", 20, 10, 4);
    tft.drawXBitmap(27, 45, lightning, iconWidth, iconHeight, TFT_RED);
    tft.setTextColor(TFT_RED);
    String lightningStr = "Strikes: " + String(lightingCount);
    tft.drawString(lightningStr, 95, 55, 4);
    lightningStr = "Distance: " + String(lastLightingDistance) + " km";
    tft.drawString(lightningStr, 95, 75, 4);
    tft.setTextColor(TFT_BLUE);
    lightningStr = "Noise Count: " + String(noiseCount)+ ", level: " + String(noiseSetting);
    tft.drawString(lightningStr, 10, 120, 4);
    tft.setTextColor(TFT_YELLOW);
    lightningStr = "Disturb Count: " + String(distCount);
    if (distCount < disturbAdjStart * 11)
    {
        lightningStr += ", level: " + String(disturberSetting);
    }
    else
    {
      lightningStr += ", masked";
    }
    tft.drawString(lightningStr, 10, 175, 4);
}

// display information about setting up and connect to WiFi
void show_wifi_setup()
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("Connecting to Wi-Fi...");
  tft.drawString("Connecting to Wi-Fi...", 20, 40, 4);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi Not Connected.");
    delay(1000);
  }
  Serial.println("");
  Serial.println("Wi-Fi Connected.");
  Serial.print("IP:");
  Serial.println(WiFi.localIP());
  tft.drawString("Wi-Fi Connected.", 20, 100, 4);
  tft.drawString("Press SW2 to", 20, 165, 4);
  tft.drawString("    Advance Display.", 20, 190, 4);
}

// display the temperatue, pressure and humidity
void show_BME280_measurements()
{
  tft.fillScreen(TFT_BLACK);
  tft.drawXBitmap(27, 50, temp, iconWidth, iconHeight, TFT_RED);
  tft.drawXBitmap(27, 105, humidity, iconWidth, iconHeight, TFT_BLUE);
  tft.drawXBitmap(27, 160, pressure, iconWidth, iconHeight, TFT_YELLOW);
  
  float tempC = sensor.getTemperature();
  auto temperatureC = String(tempC) + " C";
  float tempF = ((tempC * 9) / 5) + 32;
  auto temperatureF = String(tempF) + " F";
  auto pressure = String(sensor.getPressure() / 100.0) + " hPa";
  auto humidity = String(sensor.getHumidity()) + " %";

  tft.setTextColor(TFT_WHITE);
  tft.drawString("Indoor BME280 Readings", 20, 10, 4);
  tft.setTextColor(TFT_RED);
  String measurements = temperatureC + " / " + temperatureF;
  tft.drawString(measurements, 95, 65, 4);
  tft.setTextColor(TFT_BLUE);
  tft.drawString(humidity, 95, 120, 4);
  tft.setTextColor(TFT_YELLOW);
  tft.drawString(pressure, 95, 175, 4);
}

// display the infomation retrieved from OpenWeather
void show_OpenWeather(){
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Outdoor OpenWeather", 20, 7, 4);
  if (WiFi.status() == WL_CONNECTED) {
    String server = "http://api.openweathermap.org/data/2.5/weather?q=" + my_city + "," + my_country_code + "&APPID=" + my_Api_Key + "&units=imperial";
    json_array = GET_Request(server.c_str());
    Serial.println(json_array);
    JSONVar my_obj = JSON.parse(json_array);
    if (JSON.typeof(my_obj) == "undefined") {
      Serial.println("Parsing input failed!");
      return;
    }
    Serial.println("JSON object = ");
    Serial.println(my_obj);
    Serial.print("Temperature: ");
    Serial.println(my_obj["main"]["temp"]);
    Serial.print("Humidity: ");
    Serial.println(my_obj["main"]["humidity"]);
    Serial.print("Pressure: ");
    Serial.println(my_obj["main"]["pressure"]);
    Serial.print("Wind Speed: ");
    Serial.println(my_obj["wind"]["speed"]);

    tft.drawXBitmap(27, 42, temp, iconWidth, iconHeight, TFT_RED);
    tft.drawXBitmap(27, 95, humidity, iconWidth, iconHeight, TFT_BLUE);
    tft.drawXBitmap(27, 147, pressure, iconWidth, iconHeight, TFT_YELLOW);
    tft.drawXBitmap(27, 199, wind, iconWidth, iconHeight, TFT_GREEN);
        
    tft.setTextColor(TFT_RED);
    String measurement = JSON.stringify(my_obj["main"]["temp"]) + " F";
    tft.drawString(measurement, 95, 57, 4);
    tft.setTextColor(TFT_BLUE);
    measurement = JSON.stringify(my_obj["main"]["humidity"]) + " %";
    tft.drawString(measurement, 95, 110, 4);
    tft.setTextColor(TFT_YELLOW);
    measurement = JSON.stringify(my_obj["main"]["pressure"]) + " hPa";
    tft.drawString(measurement, 95, 162, 4);
    tft.setTextColor(TFT_GREEN);
    measurement = JSON.stringify(my_obj["wind"]["speed"]) + " mph";
    tft.drawString(measurement, 95, 214, 4);
  }
  else {
    Serial.println("WiFi Disconnected");
    tft.drawString("WiFi Disconnected", 20, 110, 4);
  }
}

// function to get the json data from OpenWeather
String GET_Request(const char* server) {
  HTTPClient http;
  http.begin(server);
  int httpResponseCode = http.GET();
  String payload = "{}";
  if (httpResponseCode > 0) {
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    payload = http.getString();
  }
  else {
    Serial.print("Error code: ");
    Serial.println(httpResponseCode);
  }
  http.end();
  return payload;
}
