#include <ArduinoJson.h>
#include <ESP_FlexyStepper.h>
#include <HTTPClient.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <Wire.h>
#include <Arduino.h>
#include "FastLED.h"


///////////////////////////////////////////
////
////
////  To Debug or not to Debug
////
////
///////////////////////////////////////////


#define DEBUG // Comment out if you don't want to see debug info

void Debug_Text(String debug_info){
#ifdef DEBUG
  Serial.println(debug_info);
#endif
}



///////////////////////////////////////////
////
////
////  Things specific to the OSSM Refernce Board & Remote
////
////
///////////////////////////////////////////



// OSSM Reference Board
// INCLUDES


// SETTINGS



// CONSTRUCT THINGS

bool HAS_NOT_HOMED = true;



// OSSM Reference Remote
// INCLUDES
#include "SSD1306Wire.h"   // legacy include: `#include "SSD1306.h"`
#include "OLEDDisplayUi.h" // Include the UI lib
#include "images.h"        // Include custom images (for ui - this is the dots)
#include <RotaryEncoder.h>

// SETTINGS
#define ENCODER_SWITCH 35
#define ENCODER_A 18
#define ENCODER_B 5
#define REMOTE_SDA 21
#define REMOTE_CLK 19
#define REMOTE_ADDRESS 0x3c

// CONSTRUCT THINGS
// Encoder
RotaryEncoder encoder(ENCODER_A, ENCODER_B, RotaryEncoder::LatchMode::TWO03);



///////////////////////////////////////////
////
////
////  OLED UI Make Work Area
////
////
///////////////////////////////////////////


// Display
SSD1306Wire display(REMOTE_ADDRESS, REMOTE_SDA, REMOTE_CLK); // ADDRESS, SDA, SCL  -  SDA and SCL usually populate automatically based on your board's pins_arduino.h e.g. https://github.com/esp8266/Arduino/blob/master/variants/nodemcu/pins_arduino.h
OLEDDisplayUi ui(&display);                                  // Constructs the UI for the display

#include "ossm_ui.h"  // Moved UI stuff out to so that this code isn't too cluttered




///////////////////////////////////////////
////
////
////  Encoder functions & scaling
////
////
///////////////////////////////////////////


void encoder_update()
{
  encoder.tick();
}
void encoder_reset()
{
  encoder.setPosition(0);
  ui.nextFrame();
}

float getEncoderPercentage()
{
  const int encoderFullScale = 36;
  int position = encoder.getPosition();
  float positionPercentage;
  if (position < 0)
  {
    encoder.setPosition(0);
    position = 0;
  }
  else if (position > encoderFullScale)
  {
    encoder.setPosition(encoderFullScale);
    position = encoderFullScale;
  }

  positionPercentage = 100.0 * position / encoderFullScale;

  return positionPercentage;
}





///////////////////////////////////////////
////
////
////  WIFI Management
////
////
///////////////////////////////////////////


// Wifi Manager
WiFiManager wm;

// create the stepper motor object
ESP_FlexyStepper stepper;

// Current command state
volatile float strokePercentage = 0;
volatile float speedPercentage = 0;
volatile float deceleration = 0;

// Create tasks for checking pot input or web server control, and task to handle
// planning the motion profile (this task is high level only and does not pulse
// the stepper!)
TaskHandle_t wifiTask = nullptr;
TaskHandle_t getInputTask = nullptr;
TaskHandle_t motionTask = nullptr;
TaskHandle_t estopTask = nullptr;
TaskHandle_t oledTask = nullptr;

#define BRIGHTNESS 170
#define LED_TYPE WS2811
#define COLOR_ORDER GRB
#define LED_PIN 25
#define NUM_LEDS 1
CRGB leds[NUM_LEDS];





///////////////////////////////////////////
////
////
////  SETUP Parameters
////  These should be moved to an external file only for settings, or much higher up
////  it should be immediately obvious where these are and easy for novices to modify
////
////
///////////////////////////////////////////

// Parameters you may need to change for your specific implementation
#define MOTOR_STEP_PIN 14
#define MOTOR_DIRECTION_PIN 27
#define MOTOR_ENABLE_PIN 26
// controller knobs
#define STROKE_POT_PIN 32
#define SPEED_POT_PIN 34
// this pin resets WiFi credentials if needed
#define WIFI_RESET_PIN 0
// this pin toggles between manual knob control and Web-based control
#define WIFI_CONTROL_TOGGLE_PIN 22

#define WIFI_CONTROL_DEFAULT INPUT_PULLDOWN // uncomment for analog pots as default
//#define WIFI_CONTROL_DEFAULT INPUT_PULLUP // uncomment for WiFi control as default
//Pull pin 26 low if you want to switch to analog pot control

// define the IO pin the emergency stop switch is connected to
#define STOP_PIN 19
// define the IO pin where the limit switches are connected to (switches in
// series in normally closed setup)
#define LIMIT_SWITCH_PIN 12 //If commented out, limit switch will not be used and toy must be extended at least as far as the "maxStrokeLengthMm"



// limits and physical parameters
const float maxSpeedMmPerSecond = 1000;
const float motorStepPerRevolution = 800;
const float pulleyToothCount = 20;
const float maxStrokeLengthMm = 150;          // This is in millimeters, and is what's used to define how much of your rail is usable.
//                                            //  150mm on a 400mm rail is comfortable and will generally avoid smashing endstops
//                                            //  This can be lowered if you want to reduce the maximum stroke
const float minStrokeOffLimit = 6;            // Machine needs some room away from the limit switch to not tick every stroke @ 100% stroke
const float minimumCommandPercentage = 1.0f;
// GT2 belt has 2mm tooth pitch
const float beltPitchMm = 2;

// Tuning parameters
// affects acceleration in stepper trajectory
const float accelerationScaling = 80.0f;

const char *ossmId = "OSSM1"; // this should be unique to your device. You will use this on the
                              // web portal to interact with your OSSM.
// there is NO security other than knowing this name, make this unique to avoid
// collisions with other users

// Declarations
// TODO: Document functions
void getUserInputTask(void *pvParameters);
void motionCommandTask(void *pvParameters);
float getAnalogAverage(int pinNumber, int samples);
bool setInternetControl(bool wifiControlEnable);
bool getInternetSettings();

bool stopSwitchTriggered = 0;

/**
 * the iterrupt service routine (ISR) for the emergency swtich
 * this gets called on a rising edge on the IO Pin the emergency switch is connected
 * it only sets the stopSwitchTriggered flag and then returns. 
 * The actual emergency stop will than be handled in the loop function
 */
void ICACHE_RAM_ATTR stopSwitchHandler()
{
  stopSwitchTriggered = 1;
  vTaskSuspend(motionTask);
  vTaskSuspend(getInputTask);
  stepper.emergencyStop();
}





///////////////////////////////////////////
////
////
////  VOID SETUP -- Here's where it's hiding
////
////
///////////////////////////////////////////



void setup()
{
 
  Serial.begin(115200);
#ifdef DEBUG
  Serial.println("\n Starting");
  delay(200);
#endif
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(150);
  setLedRainbow(leds);
  FastLED.show();
  stepper.connectToPins(MOTOR_STEP_PIN, MOTOR_DIRECTION_PIN);

  float stepsPerMm =
      motorStepPerRevolution /
      (pulleyToothCount * beltPitchMm); // GT2 belt has 2mm tooth pitch
  stepper.setStepsPerMillimeter(stepsPerMm);
  // initialize the speed and acceleration rates for the stepper motor. These
  // will be overwritten by user controls. 100 values are placeholders
  stepper.setSpeedInStepsPerSecond(100);
  stepper.setAccelerationInMillimetersPerSecondPerSecond(100);
  stepper.setDecelerationInStepsPerSecondPerSecond(100);
  stepper.setLimitSwitchActive(LIMIT_SWITCH_PIN);


  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
  // put your setup code here, to run once:
  pinMode(MOTOR_ENABLE_PIN, OUTPUT);
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
  pinMode(WIFI_CONTROL_TOGGLE_PIN, WIFI_CONTROL_DEFAULT);
  //set the pin for the emegrenxy witch to input with inernal pullup
  //the emergency switch is connected in a Active Low configuraiton in this example, meaning the switch connects the input to ground when closed
  pinMode(STOP_PIN, INPUT_PULLUP);
  //attach an interrupt to the IO pin of the switch and specify the handler function
  attachInterrupt(digitalPinToInterrupt(STOP_PIN), stopSwitchHandler, RISING);
  // Set analog pots (control knobs)
  pinMode(STROKE_POT_PIN, INPUT);
  adcAttachPin(STROKE_POT_PIN);

  pinMode(SPEED_POT_PIN, INPUT);
  adcAttachPin(SPEED_POT_PIN);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db); // allows us to read almost full 3.3V range

  // This is here in case you want to change WiFi settings - pull IO low
  if (digitalRead(WIFI_RESET_PIN) == LOW)
  {
    // reset settings - for testing
    wm.resetSettings();
#ifdef DEBUG
    Serial.println("settings reset");
#endif
  }


///////////////////////////////////////////
////
////
//// Can all of this OLED stuff Move up way higher so we can have the screen on boot?
////
////
///////////////////////////////////////////




  //OLED SETUP
  // The ESP is capable of rendering 60fps in 80Mhz mode
  // but that won't give you much time for anything else
  // run it in 160Mhz mode or just set it to 30 fps
  ui.setTargetFPS(20);

  // Customize the active and inactive symbol
  ui.setActiveSymbol(activeSymbol);
  ui.setInactiveSymbol(inactiveSymbol);

  // You can change this to
  // TOP, LEFT, BOTTOM, RIGHT
  ui.setIndicatorPosition(LEFT);

  // Defines where the first frame is located in the bar.
  ui.setIndicatorDirection(LEFT_RIGHT);

  // You can change the transition that is used
  // SLIDE_LEFT, SLIDE_RIGHT, SLIDE_UP, SLIDE_DOWN
  // ui.setFrameAnimation(SLIDE_LEFT);

  // Add frames
  ui.setFrames(frames, frameCount);

  // Add overlays
  ui.setOverlays(overlays, overlaysCount);

  // Initialising the UI will init the display too.
  ui.init();
  ui.disableAutoTransition();

  display.flipScreenVertically();


  // Rotary Encoder Setup

  pinMode(ENCODER_A, INPUT_PULLDOWN);
  pinMode(ENCODER_B, INPUT_PULLDOWN);
  pinMode(ENCODER_SWITCH, INPUT_PULLDOWN);

  attachInterrupt(digitalPinToInterrupt(ENCODER_A), encoder_update, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_B), encoder_update, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_SWITCH), encoder_reset, RISING);




  if(HAS_NOT_HOMED == true){


        #ifdef DEBUG
          Serial.println("OSSM will now home");
        #endif

        stepper.moveToHomeInMillimeters(1,100,400,LIMIT_SWITCH_PIN);

        #ifdef DEBUG
          Serial.println("OSSM has homed, will now move out to max length");
        #endif

        stepper.moveToPositionInMillimeters((-1 * maxStrokeLengthMm) - minStrokeOffLimit);

        #ifdef DEBUG
          Serial.println("OSSM has moved out, will now set new home?");
        #endif

        stepper.setCurrentPositionAsHomeAndStop();

        #ifdef DEBUG
          Serial.println("OSSM should now be home and happy");
        #endif


        HAS_NOT_HOMED = false;
      }


  



  // Start the stepper instance as a service in the "background" as a separate
  // task and the OS of the ESP will take care of invoking the processMovement()
  // task regularly on core 1 so you can do whatever you want on core 0
  stepper.startAsService(); // Kinky Makers - we have modified this function
                            // from default library to run on core 1 and suggest
                            // you don't run anything else on that core.

  // Kick off the http and motion tasks - they begin executing as soon as they
  // are created here! Do not change the priority of the task, or do so with
  // caution. RTOS runs first in first out, so if there are no delays in your
  // tasks they will prevent all other code from running on that core!
  xTaskCreatePinnedToCore(
      wifiConnectionTask,   /* Task function. */
      "wifiConnectionTask", /* name of task. */
      10000,                /* Stack size of task */
      NULL,                 /* parameter of the task */
      1,                    /* priority of the task */
      &wifiTask,            /* Task handle to keep track of created task */
      0);                   /* pin task to core 0 */
  delay(5000);
  xTaskCreatePinnedToCore(
      getUserInputTask,   /* Task function. */
      "getUserInputTask", /* name of task. */
      10000,              /* Stack size of task */
      NULL,               /* parameter of the task */
      1,                  /* priority of the task */
      &getInputTask,      /* Task handle to keep track of created task */
      0);                 /* pin task to core 0 */
  delay(500);
  xTaskCreatePinnedToCore(
      motionCommandTask,   /* Task function. */
      "motionCommandTask", /* name of task. */
      10000,               /* Stack size of task */
      NULL,                /* parameter of the task */
      1,                   /* priority of the task */
      &motionTask,         /* Task handle to keep track of created task */
      0);                  /* pin task to core 0 */

  delay(500);
  xTaskCreatePinnedToCore(
      estopResetTask,   /* Task function. */
      "estopResetTask", /* name of task. */
      10000,            /* Stack size of task */
      NULL,             /* parameter of the task */
      1,                /* priority of the task */
      &estopTask,       /* Task handle to keep track of created task */
      0);               /* pin task to core 0 */

  delay(500);

  xTaskCreatePinnedToCore(
      oledUpdateTask,   /* Task function. */
      "oledUpdateTask", /* name of task. */
      10000,            /* Stack size of task */
      NULL,             /* parameter of the task */
      2,                /* priority of the task */
      &oledTask,        /* Task handle to keep track of created task */
      0);               /* pin task to core 0 */

  delay(500);
} //Void Setup()






///////////////////////////////////////////
////
////
////   VOID LOOP - Hides here
////
////
///////////////////////////////////////////

void loop()
{
  ui.update();

  //vTaskDelete(NULL); // we don't want this loop to run (because it runs on core
  // 0 where we have the critical FlexyStepper code)
}



///////////////////////////////////////////
////
////
////  freeRTOS multitasking
////
////
///////////////////////////////////////////




void oledUpdateTask(void *pvParameters)
{

  for (;;)
  {
    //ui.update();
    //test
    vTaskDelay(10);
  }
}

void estopResetTask(void *pvParameters)
{
  for (;;)
  {
    if (stopSwitchTriggered == 1)
    {
      while ((getAnalogAverage(SPEED_POT_PIN, 50) + getAnalogAverage(STROKE_POT_PIN, 50)) > 2)
      {
        vTaskDelay(1);
      }
      stopSwitchTriggered = 0;
      vTaskResume(motionTask);
      vTaskResume(getInputTask);
    }
    vTaskDelay(100);
  }
}

void wifiConnectionTask(void *pvParameters)
{
  wm.setConfigPortalTimeout(1);
  wm.setConfigPortalBlocking(false);
  // here we try to connect to WiFi or launch settings hotspot for you to enter
  // WiFi credentials
  if (!wm.autoConnect("OSSM-setup"))
  {
    // TODO: Set Status LED to indicate failure
#ifdef DEBUG
    Serial.println("failed to connect and hit timeout");
#endif
  }
  else
  {
    // TODO: Set Status LED to indicate everything is ok!
#ifdef DEBUG
    Serial.println("Connected!");
#endif
  }
  for (;;)
  {
    wm.process();
    vTaskDelay(1);

    //delete this task once connected!
    if (WiFi.status() == WL_CONNECTED)
    {
      vTaskDelete(NULL);
    }
  }
}

// Task to read settings from server - only need to check this when in WiFi
// control mode
void getUserInputTask(void *pvParameters)
{
  bool wifiControlEnable = false;
  for (;;) // tasks should loop forever and not return - or will throw error in
           // OS
  {


  Debug_Text("Speed: " + String(speedPercentage) + "\% Stroke: " + String(strokePercentage) + "\% Distance to target: " + String(stepper.getDistanceToTargetSigned()) + " steps?");

    if (speedPercentage > 1){
      stepper.releaseEmergencyStop();
    }
    else {
      stepper.emergencyStop();
        #ifdef DEBUG
          Serial.println("FULL STOP CAPTAIN");
        #endif
    }


    if (digitalRead(WIFI_CONTROL_TOGGLE_PIN) == HIGH) // TODO: check if wifi available and handle gracefully
    {
      if (wifiControlEnable == false)
      {
        // this is a transition to WiFi, we should tell the server it has
        // control
        wifiControlEnable = true;
        setInternetControl(wifiControlEnable);
      }
      getInternetSettings(); // we load speedPercentage and strokePercentage in
                             // this routine.
    }
    else
    {
      if (wifiControlEnable == true)
      {
        // this is a transition to local control, we should tell the server it
        // cannot control
        wifiControlEnable = false;
        setInternetControl(wifiControlEnable);
      }
      speedPercentage = getAnalogAverage(SPEED_POT_PIN, 50); // get average analog reading, function takes pin and # samples
      // strokePercentage = getAnalogAverage(STROKE_POT_PIN, 50);
      strokePercentage = getEncoderPercentage();
    }

    // We should scale these values with initialized settings not hard coded
    // values!
    if (speedPercentage > minimumCommandPercentage)
    {
      stepper.setSpeedInMillimetersPerSecond(maxSpeedMmPerSecond *
                                             speedPercentage / 100.0);
      stepper.setAccelerationInMillimetersPerSecondPerSecond(
          maxSpeedMmPerSecond * speedPercentage * speedPercentage / accelerationScaling);
      // We do not set deceleration value here because setting a low decel when
      // going from high to low speed causes the motor to travel a long distance
      // before slowing. We should only change decel at rest
    }
    vTaskDelay(100); // let other code run!
  }
}

void motionCommandTask(void *pvParameters)
{

  for (;;) // tasks should loop forever and not return - or will throw error in
           // OS
  {
    // poll at 200Hz for when motion is complete
    while ((stepper.getDistanceToTargetSigned() != 0) ||
           (strokePercentage < minimumCommandPercentage) || (speedPercentage < minimumCommandPercentage))
    {
      vTaskDelay(5); // wait for motion to complete and requested stroke more than zero
    }

    float targetPosition = (strokePercentage / 100.0) * maxStrokeLengthMm;
#ifdef DEBUG
    Serial.printf("Moving stepper to position %ld \n", targetPosition);
    vTaskDelay(1);
#endif
    stepper.setDecelerationInMillimetersPerSecondPerSecond(
        maxSpeedMmPerSecond * speedPercentage * speedPercentage / accelerationScaling);
    stepper.setTargetPositionInMillimeters(targetPosition);
    vTaskDelay(1);

    while ((stepper.getDistanceToTargetSigned() != 0) ||
           (strokePercentage < minimumCommandPercentage) || (speedPercentage < minimumCommandPercentage))
    {
      vTaskDelay(5); // wait for motion to complete, since we are going back to
                     // zero, don't care about stroke value
    }
    targetPosition = 0;
    // Serial.printf("Moving stepper to position %ld \n", targetPosition);
    vTaskDelay(1);
    stepper.setDecelerationInMillimetersPerSecondPerSecond(
        maxSpeedMmPerSecond * speedPercentage * speedPercentage / accelerationScaling);
    stepper.setTargetPositionInMillimeters(targetPosition);
    vTaskDelay(1);
  }
}

float getAnalogAverage(int pinNumber, int samples)
{
  float sum = 0;
  float average = 0;
  float percentage = 0;
  for (int i = 0; i < samples; i++)
  {
    // TODO: Possibly use fancier filters?
    sum += analogRead(pinNumber);
  }
  average = sum / samples;
  // TODO: Might want to add a deadband
  percentage = 100.0 * average / 4096.0; // 12 bit resolution
  return percentage;
}

bool setInternetControl(bool wifiControlEnable)
{
  // here we will SEND the WiFi control permission, and current speed and stroke
  // to the remote server. The cloudfront redirect allows http connection with
  // bubble backend hosted at app.researchanddesire.com

  String serverNameBubble = "http://d2g4f7zewm360.cloudfront.net/ossm-set-control"; //live server
  // String serverNameBubble = "http://d2oq8yqnezqh3r.cloudfront.net/ossm-set-control"; // this is version-test server

  // Add values in the document to send to server
  StaticJsonDocument<200> doc;
  doc["ossmId"] = ossmId;
  doc["wifiControlEnabled"] = wifiControlEnable;
  doc["stroke"] = strokePercentage;
  doc["speed"] = speedPercentage;
  String requestBody;
  serializeJson(doc, requestBody);

  // Http request
  HTTPClient http;
  http.begin(serverNameBubble);
  http.addHeader("Content-Type", "application/json");
  // post and wait for response
  int httpResponseCode = http.POST(requestBody);
  String payload = "{}";
  payload = http.getString();
  http.end();

  // deserialize JSON
  StaticJsonDocument<200> bubbleResponse;
  deserializeJson(bubbleResponse, payload);

  // TODO: handle status response
  // const char *status = bubbleResponse["status"]; // "success"

#ifdef DEBUG
  Serial.print("Setting Wifi Control: ");
  Serial.println(wifiControlEnable);
  Serial.println(requestBody);
  Serial.println(payload);
  Serial.print("HTTP Response code: ");
  Serial.println(httpResponseCode);
#endif



  return true;
}

bool getInternetSettings()
{
  // here we will request speed and stroke settings from the remote server. The
  // cloudfront redirect allows http connection with bubble backend hosted at
  // app.researchanddesire.com

  String serverNameBubble = "http://d2g4f7zewm360.cloudfront.net/ossm-get-settings"; //live server
  // String serverNameBubble = "http://d2oq8yqnezqh3r.cloudfront.net/ossm-get-settings"; // this is
  // version-test
  // server

  // Add values in the document
  StaticJsonDocument<200> doc;
  doc["ossmId"] = ossmId;
  String requestBody;
  serializeJson(doc, requestBody);

  // Http request
  HTTPClient http;
  http.begin(serverNameBubble);
  http.addHeader("Content-Type", "application/json");
  // post and wait for response
  int httpResponseCode = http.POST(requestBody);
  String payload = "{}";
  payload = http.getString();
  http.end();

  // deserialize JSON
  StaticJsonDocument<200> bubbleResponse;
  deserializeJson(bubbleResponse, payload);

  // TODO: handle status response
  // const char *status = bubbleResponse["status"]; // "success"
  strokePercentage = bubbleResponse["response"]["stroke"];
  speedPercentage = bubbleResponse["response"]["speed"];

#ifdef DEBUG
  // debug info on the http payload
  Serial.println(payload);
  Serial.print("HTTP Response code: ");
  Serial.println(httpResponseCode);
#endif

  return true;
}
void setLedRainbow(CRGB leds[])
{
  // int power = 250;

  for (int hueShift = 0; hueShift < 350; hueShift++)
  {
    int gHue = hueShift % 255;
    fill_rainbow(leds, NUM_LEDS, gHue, 25);
    FastLED.show();
    delay(4);
  }
}