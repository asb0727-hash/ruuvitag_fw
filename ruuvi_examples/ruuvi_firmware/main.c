/** RuuviTag Environment-station  */
// Version 2.2.3 August 01, 2018; 
//  Rewrite initalization continue even if there are failures and announce failure status by may means

/* Copyright (c) 2015 Nordic Semiconductor. All Rights Reserved. 
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in 
 * NORDIC SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 * Licensees are granted free, non-transferable use of the information. 
 NO WARRANTY of ANY KIND is provided. This heading must NOT be removed from the file. */

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

// Nordic SDK
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "nordic_common.h"
#include "softdevice_handler.h"
#include "app_scheduler.h"
#include "app_timer_appsh.h"
#include "nrf_drv_clock.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"

//#define NRF_LOG_ENABLED 1   // log code only compiled if ENABLED 
#define NRF_LOG_MODULE_NAME "MAIN"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"

// BSP  Board Support Package : 
#include "bsp.h"

// Drivers
#include "lis2dh12.h"
#include "lis2dh12_acceleration_handler.h"
#include "bme280.h"
#include "battery.h"
#include "bluetooth_core.h"
#include "eddystone.h"
#include "pin_interrupt.h"
#include "nfc.h"
#include "nfc_t2t_lib.h"
#include "rtc.h"
#include "application_config.h"

// Libraries
#include "base64.h"
#include "sensortag.h"

// Init
#include "init.h"

// Configuration
#include "bluetooth_config.h"      // including  REVision, intervals, APP_TX_POWER

// Constants
#define DEAD_BEEF               0xDEADBEEF    //!< Value used as error code on stack dump, can be used to identify stack location on stack unwind.

// ID for main loop timer.
APP_TIMER_DEF(main_timer_id);                 // Creates timer id for our program.

static uint16_t init_status = 0;   // combined status of all initalizations.  Zero when all are complete if no errors occured.
static uint8_t NFC_message[100];   // NFC message buffer has 4 records, up to 128 bytes each minus some overhead for NFC NDEF data keeping. 
static size_t NFC_message_length = sizeof(NFC_message);
#define LOG_FAILED_INIT             0x0002
#define ACCELEROMETER_FAILED_INIT   0x0004
#define TEMP_HUM_PRESS_FAILED_INIT  0x0008
#define NFC_FAILED_INIT             0x0010
#define BLE_FAILED_INIT             0x0020
#define TIMER_FAILED_INIT           0x0040
#define RTC_FAILED_INIT             0x0080
#define PIN_ENA_FAILED_INIT         0x0200
#define ACCEL_INT_FAILED_INIT       0x0400
#define ACC_INT_FAILED_INIT         0x0800
#define BATTERY_MIN_V                 2600
#define BATTERY_FAILED_INIT         0x1000
#define BUTTON_FAILED_INIT          0x2000
#define BME_FAILED_INIT             0x4000

// define unconfusing macros for LEDs
#define RED_LED_ON    nrf_gpio_pin_clear(LED_RED)
#define RED_LED_OFF   nrf_gpio_pin_set(LED_RED)
#define GREEN_LED_ON  nrf_gpio_pin_clear(LED_GREEN)
#define GREEN_LED_OFF nrf_gpio_pin_set(LED_GREEN)

// Payload requires 9 characters
static char url_buffer[URL_BASE_LENGTH + URL_DATA_LENGTH] = URL_BASE;
static uint8_t data_buffer[RAW_DATA_LENGTH] = { 0 };
static bool model_plus = false;          // Flag for sensors available
static bool highres = true;              // Flag for used mode
static bool fast_advertising = true;     // Connectable mode
static uint64_t fast_advertising_start = 0;  // Timestamp of when tag became connectable
static uint64_t debounce = false;        // Flag for avoiding double presses
static uint16_t acceleration_events = 0; // Number of times accelerometer has triggered

static ruuvi_sensor_t data;

static void main_timer_handler(void * p_context);


/**@brief Handler for button press.
 * Called in scheduler, out of interrupt context.
 */
void change_mode(void* data, uint16_t length)
{
  // Avoid double presses
  if ((millis() - debounce) < DEBOUNCE_THRESHOLD) { return; }
  debounce = millis();
  highres = !highres;
  if (model_plus)
  {
    if (highres)
    {
      //TODO: #define sample rate for application
      lis2dh12_set_sample_rate(LIS2DH12_SAMPLERATE_RAW);
      // Reconfigure application sample rate for RAW mode
      app_timer_stop(main_timer_id);
      app_timer_start(main_timer_id, APP_TIMER_TICKS(MAIN_LOOP_INTERVAL_RAW, RUUVITAG_APP_TIMER_PRESCALER), NULL); // 1 event / 1000 ms
      bluetooth_configure_advertising_interval(ADVERTISING_INTERVAL_RAW); // Broadcast only updated data, assuming there is an active receiver nearby.
      bluetooth_apply_configuration();
    }
    else
    {
      // Stop accelerometer as it's not useful on URL mode.
      lis2dh12_set_sample_rate(LIS2DH12_SAMPLERATE_URL);
      // Reconfigure application sample rate for URL mode.
      app_timer_stop(main_timer_id);
      app_timer_start(main_timer_id, APP_TIMER_TICKS(MAIN_LOOP_INTERVAL_URL, RUUVITAG_APP_TIMER_PRESCALER), NULL); // 1 event / 5000 ms
      bluetooth_configure_advertising_interval(ADVERTISING_INTERVAL_URL); // Broadcast often to "hit" occasional background scans.
      bluetooth_apply_configuration();
    }
  }
  NRF_LOG_INFO("Updating in %d mode\r\n", (uint32_t) highres);
  main_timer_handler(NULL);
}

/**
 * Tag enters connectable mode. Main loop timer will close the connectable mode after 20 seconds.
 *
 * Parameters are unused.
 */
static void become_connectable(void* data, uint16_t length)
{
  fast_advertising_start = millis();
  fast_advertising = true;
  bluetooth_configure_advertising_interval(ADVERTISING_INTERVAL_STARTUP);
  bluetooth_configure_advertisement_type(STARTUP_ADVERTISEMENT_TYPE);
  bluetooth_apply_configuration();
}


/**@brief Function for handling button events.
 * Schedulers call to handler.
 */
ret_code_t button_press_handler(const ruuvi_standard_message_t message)
{
  NRF_LOG_INFO("Button\r\n");
  //Change mode on button press
  //Use scheduler, do not use peripherals in interrupt conext (SPI write halts)
  GREEN_LED_ON;
  RED_LED_ON;  
  app_sched_event_put (NULL, 0, change_mode);
  app_sched_event_put (NULL, 0, become_connectable);

  return ENDPOINT_SUCCESS;
}

/**
 * Work around NFC data corruption bug by reinitializing NFC data after field has been lost.
 * Call this function outside of interrupt context.
 */
static void reinit_nfc(void* data, uint16_t length)
{
  init_nfc();
}

/**@brief Function for handling NFC events.
 * Schedulers call to handler.
 */
void app_nfc_callback(void* p_context, nfc_t2t_event_t event, const uint8_t* p_data, size_t data_length)
{
  NRF_LOG_INFO("NFC\r\n");
  switch (event)
  {
    case NFC_T2T_EVENT_FIELD_ON:
    NRF_LOG_INFO("NFC Field detected \r\n");
    break;
    case NFC_T2T_EVENT_FIELD_OFF:
    NRF_LOG_INFO("NFC Field lost \r\n");
    app_sched_event_put (NULL, 0, reinit_nfc);
    app_sched_event_put (NULL, 0, become_connectable);
    break;
    case NFC_T2T_EVENT_DATA_READ:
    NRF_LOG_INFO("Data read\r\n");
    default:
    break;
  }
}


/**@brief Function for doing power management.
 */
static void power_manage(void)
{
  // Clear both leds before sleep.
  GREEN_LED_OFF;
  RED_LED_OFF;

  uint32_t err_code = sd_app_evt_wait();
  APP_ERROR_CHECK(err_code);

  // Signal mode by led color.
  if (highres) { RED_LED_ON; }
  else { GREEN_LED_ON; }
}


static void updateAdvertisement(void)
{
  ret_code_t err_code = NRF_SUCCESS;
  if (highres) { err_code |= bluetooth_set_manufacturer_data(data_buffer, sizeof(data_buffer)); }
  else { err_code |= bluetooth_set_eddystone_url(url_buffer, sizeof(url_buffer)); }
}


/**@brief Timeout handler for the repeated timer
 */
void main_timer_handler(void * p_context)
{
  int32_t  raw_t  = 0;
  uint32_t raw_p = 0;
  uint32_t raw_h = 0;
  lis2dh12_sensor_buffer_t buffer;
  int32_t acc[3] = {0};

  if (fast_advertising && ((millis() - fast_advertising_start) > ADVERTISING_STARTUP_PERIOD))
  {
    fast_advertising = false;
    bluetooth_configure_advertisement_type(APPLICATION_ADVERTISEMENT_TYPE);
    if (highres) { bluetooth_configure_advertising_interval(ADVERTISING_INTERVAL_RAW); }
    else { bluetooth_configure_advertising_interval(ADVERTISING_INTERVAL_URL); }
    bluetooth_apply_configuration();
  }

  // If we have all the sensors.
  if (model_plus)
  {
    // Get raw environmental data.
    bme280_read_measurements();
    raw_t = bme280_get_temperature();
    raw_p = bme280_get_pressure();
    raw_h = bme280_get_humidity();

    // Get accelerometer data.
    lis2dh12_read_samples(&buffer, 1);
    acc[0] = buffer.sensor.x;
    acc[1] = buffer.sensor.y;
    acc[2] = buffer.sensor.z;
  }
  // If only temperature sensor is present.
  else
  {
    int32_t temp;                                        // variable to hold temp reading
    (void)sd_temp_get(&temp);                            // get new temperature
    temp *= 25;                                          // SD returns temp * 4. Ruuvi format expects temp * 100. 4*25 = 100.
    raw_t = (int32_t) temp;
  }

  // Get battery voltage
  //static uint32_t vbat_update_counter;
  static uint16_t vbat = 0;
  vbat = getBattery();

  // Embed data into structure for parsing.
  parseSensorData(&data, raw_t, raw_p, raw_h, vbat, acc);
  NRF_LOG_DEBUG("temperature: %d, pressure: %d, humidity: %d x: %d y: %d z: %d\r\n", raw_t, raw_p, raw_h, acc[0], acc[1], acc[2]);
  NRF_LOG_DEBUG("VBAT: %d send %d \r\n", vbat, data.vbat);
  if (highres)
  {
    // Prepare bytearray to broadcast.
    bme280_data_t environmental;
    environmental.temperature = raw_t;
    environmental.humidity = raw_h;
    environmental.pressure = raw_p;
    encodeToRawFormat5(data_buffer, &environmental, &buffer.sensor, acceleration_events, vbat, BLE_TX_POWER);
  }
  else
  {
    encodeToUrlDataFromat(url_buffer, URL_BASE_LENGTH, &data);
  }

  updateAdvertisement();
  watchdog_feed();
}


/**
 * @brief Handle interrupt from lis2dh12.
 * Never do long actions, such as sensor reads in interrupt context.
 * Using peripherals in interrupt is also risky,
 * as peripherals might require interrupts for their function.
 *
 *  @param message Ruuvi message, with source, destination, type and 8 byte payload. Ignore for now.
 **/
ret_code_t lis2dh12_int2_handler(const ruuvi_standard_message_t message)
{
  NRF_LOG_DEBUG("Accelerometer interrupt to pin 2\r\n");
  acceleration_events++;
  /*
  app_sched_event_put ((void*)(&message),
                       sizeof(message),
                       lis2dh12_scheduler_event_handler);
  */
  return NRF_SUCCESS;
}


/**  This is where it all starts ++++++++++++++++++++++++++++++++++++++++++ 
 main is entered as a result of one of SEVERAL events:
  - Normal startup from press of reset button.
  - Battery inserted.
  - After DFU (Device Firmware Upgrade) at manufacturing Quality Assurance or user DFU.
  - WatchDogTimer expiration and its interrupt handler didn't feed new value.
  - Some error occured and
  - Spontenous unknown reset.
 All subsystems are initalized and any failures are noted and available later in init_status
 Since some events occur after tag is deployed and no one can see the LEDs the system continues operating.

 After initalizition (including setting up interrupts)
    we loop here calling app_sched_execute and sd_app_evt_wait 
*/
int main(void)
{
   // LEDs first (they're easy and cannot fail)  drivers/init/init.c
  init_leds();
  RED_LED_ON;

  if( init_log() ) { init_status |=LOG_FAILED_INIT; }
  else { NRF_LOG_INFO("LOG initalized \r\n"); } // subsequent initalizations assume log is working

  // start watchdog now incase program hangs up.
  // watchdog_default_handler logs error and resets the tag.
  init_watchdog(NULL);

  // Battery voltage initialization cannot fail under any reasonable circumstance.
  battery_voltage_init(); 
  uint16_t vbat = getBattery();
  if( vbat < BATTERY_MIN_V ) { init_status |=BATTERY_FAILED_INIT; }
  else NRF_LOG_INFO("BATTERY initalized \r\n"); 

  if(init_sensors() == NRF_SUCCESS )
  {
   model_plus = true;
   NRF_LOG_INFO("Sensors initalized \r\n");  
 }

 // Init NFC ASAP in case we're waking from deep sleep via NFC (todo)
 // outputs ID:DEVICEID ,MAC:DEVICEADDR, SW:REVision
 set_nfc_callback(app_nfc_callback);
 if( init_nfc() ) { init_status |= NFC_FAILED_INIT; } 
 else { NRF_LOG_INFO("NFC init \r\n"); }

 pin_interrupt_init(); 

 if( pin_interrupt_enable(BSP_BUTTON_0, NRF_GPIOTE_POLARITY_HITOLO, button_press_handler) ) 
 {
  init_status |= BUTTON_FAILED_INIT;
}

  // Initialize BLE Stack. Starts LFCLK required for timer operation.
if( init_ble() ) { init_status |= BLE_FAILED_INIT; }
bluetooth_configure_advertisement_type(STARTUP_ADVERTISEMENT_TYPE);
bluetooth_tx_power_set(BLE_TX_POWER);
bluetooth_configure_advertising_interval(ADVERTISING_INTERVAL_STARTUP);

if( init_timer(main_timer_id, MAIN_LOOP_INTERVAL_RAW, main_timer_handler) )
{
  init_status |= TIMER_FAILED_INIT;
}

if( init_rtc() ) { init_status |= RTC_FAILED_INIT; }
else { NRF_LOG_INFO("RTC initalized \r\n"); }

  // Initialize lis2dh12 and BME280 - TODO: Differentiate LIS2DH12 and BME280 
if (model_plus)    
{
    lis2dh12_reset(); // Clear memory.
    
    // Enable Low-To-Hi rising edge trigger interrupt on nRF52 to detect acceleration events.
    if (pin_interrupt_enable(INT_ACC2_PIN, NRF_GPIOTE_POLARITY_LOTOHI, lis2dh12_int2_handler) )
    {
      init_status |= ACC_INT_FAILED_INIT;
    }
    
    nrf_delay_ms(10); // Wait for LIS reboot.
    // Enable XYZ axes.
    lis2dh12_enable();
    lis2dh12_set_scale(LIS2DH12_SCALE);
    lis2dh12_set_sample_rate(LIS2DH12_SAMPLERATE_RAW);
    lis2dh12_set_resolution(LIS2DH12_RESOLUTION);

    lis2dh12_set_activity_interrupt_pin_2(LIS2DH12_ACTIVITY_THRESHOLD);
    NRF_LOG_INFO("Accelerameter configuration done \r\n");

            // oversampling must be set for each used sensor.
    bme280_set_oversampling_hum  (BME280_HUMIDITY_OVERSAMPLING);
    bme280_set_oversampling_temp (BME280_TEMPERATURE_OVERSAMPLING);
    bme280_set_oversampling_press(BME280_PRESSURE_OVERSAMPLING);
    bme280_set_iir(BME280_IIR);
    bme280_set_interval(BME280_DELAY);
    bme280_set_mode(BME280_MODE_NORMAL);
    NRF_LOG_INFO("BME280 configuration done \r\n");
  }   

  // Log errors, add a note to NFC, blink RED to visually indicate the problem
  if (init_status )
  { 
    snprintf((char* )NFC_message, NFC_message_length, "Error: %X", init_status);
    NRF_LOG_WARNING (" -- Initalization error :  %X \r\n", init_status);
    for ( int16_t i=0; i<13; i++)
    { 
      RED_LED_ON;
      nrf_delay_ms(500u);
      RED_LED_OFF;
      nrf_delay_ms(500u); 
    }
  }
  
  // Turn green led on if model+ with no errors.
  // Power manage turns led off
  if (model_plus & !init_status)
  {
    GREEN_LED_ON;
  }

  // Delay before advertising so we get valid data on first packet
  // Turn off red led, leave green on to signal model+ without errors
  RED_LED_OFF;
  nrf_delay_ms(MAIN_LOOP_INTERVAL_RAW + 100); 

  bluetooth_advertising_start(); 
  NRF_LOG_INFO("Advertising started\r\n");

  // Enter main loop. spinning here interrupts will bring us out frequently
  // accelerometer invokes lis2dh12_int2_handler
  for (;;)
  {
    app_sched_execute();
    power_manage();         // calls sd_app_evt_wait
  }
}
