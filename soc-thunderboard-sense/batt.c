/***************************************************************************//**
 * @file
 * @brief Battery Service
 *******************************************************************************
 * # License
 * <b>Copyright 2018 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc. Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement. This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/

/* standard library headers */
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

/* emlib */
#include "em_adc.h"

/* BG stack headers */
#include "bg_types.h"
#include "gatt_db.h"
#include "native_gecko.h"
#include "infrastructure.h"

/* plugin headers */
#include "app_timer.h"
#include "connection.h"

/* Own header*/
#include "batt.h"

/***********************************************************************************************//**
 * @addtogroup Features
 * @{
 **************************************************************************************************/

/***********************************************************************************************//**
 * @addtogroup batt
 * @{
 **************************************************************************************************/

/***************************************************************************************************
 * Local Macros and Definitions
 **************************************************************************************************/

/** Temperature measurement period in ms. */
#define BATT_IND_TIMEOUT                10000

/** Indicates currently there is no active connection using this service. */
#define BATT_NO_CONNECTION                   0xFF

#define ADC_INIT_THUNDER_BOARD_SENSE                                         \
  {                                                                          \
    adcOvsRateSel2,              /* 2x oversampling (if enabled). */         \
    adcWarmupNormal,             /* ADC shutdown after each conversion. */   \
    _ADC_CTRL_TIMEBASE_DEFAULT,  /* Use HW default value. */                 \
    2,                           /* Use a presc=2 => 38.4 / 3 = < 16 MHz. */ \
    false,                       /* Do not use tailgate. */                  \
    adcEm2Disabled               /* ADC disabled in EM2 */                   \
  }

/***************************************************************************************************
 * Local Type Definitions
 **************************************************************************************************/

typedef struct {
  uint16_t      voltage;
  uint8_t       capacity;
} VoltageCapacityPair;

/*******************************************************************************
 * Local Variables
 ******************************************************************************/

static VoltageCapacityPair battCR2032Model[] =
{ { 3000, 100 }, { 2900, 80 }, { 2800, 60 }, { 2700, 40 }, { 2600, 30 },
  { 2500, 20 }, { 2400, 10 }, { 2000, 0 } };

static uint8 battBatteryLevel; /* Battery Level */

/***************************************************************************************************
 * Static Function Declarations
 **************************************************************************************************/
static uint8_t appHwReadBatteryLevel(void);

static void adcInit(void)
{
  ADC_InitSingle_TypeDef initSingle = ADC_INITSINGLE_DEFAULT;

  // Setup single conversions
  initSingle.acqTime = adcAcqTime16;
  initSingle.reference = adcRef5VDIFF;
  initSingle.posSel = adcPosSelAVDD;
  initSingle.negSel = adcNegSelVSS;
  ADC_InitSingle(ADC0, &initSingle);
}

static uint32_t measureOneAdcSample(void)
{
  uint32_t adcData;

  ADC_Start(ADC0, adcStartSingle);
  while ((ADC_IntGet(ADC0) & ADC_IF_SINGLE) != ADC_IF_SINGLE) {
  }
  adcData = ADC_DataSingleGet(ADC0);

  return adcData;
}

static uint32_t measureBatteryVoltage(void)
{
  uint32_t adcData;
  uint32_t batteryVoltage;

  adcInit();
  adcData = measureOneAdcSample();
  batteryVoltage = (adcData * 5000 / 4096);

  return batteryVoltage;
}

static uint8_t calculateLevel(uint16_t voltage, VoltageCapacityPair *model, uint8_t modelEntryCount)
{
  uint8_t res = 0;
  int i;
  int32_t tmp;

  for (i = 0; i < modelEntryCount; i++) {
    if (i == modelEntryCount) {
      return model[i].capacity;
    } else if (voltage >= model[i].voltage) {
      res = model[i].capacity;
      break;
    } else if ((voltage < model[i].voltage) && (voltage >= model[i + 1].voltage)) {
      //
      tmp = (voltage - model[i].voltage)
            * (model[i + 1].capacity - model[i].capacity)
            / (model[i + 1].voltage - model[i].voltage);
      tmp += model[i].capacity;
      res = tmp;
      break;
    }
  }

  return res;
}

/***************************************************************************************************
 * Public Variable Definitions
 **************************************************************************************************/

/***************************************************************************************************
 * Public Function Definitions
 **************************************************************************************************/
void battInit(void)
{
  struct gecko_msg_hardware_set_soft_timer_rsp_t* ghsstrsp = NULL;
  ghsstrsp = gecko_cmd_hardware_set_soft_timer(TIMER_STOP, BATT_SERVICE_TIMER, false);
  APP_ASSERT_DBG(!ghsstrsp->result, ghsstrsp->result);
}

void battCharStatusChange(uint8_t connection, uint16_t clientConfig)
{
  struct gecko_msg_hardware_set_soft_timer_rsp_t* ghsstrsp = NULL;
  /* if the new value of CCC is not 0 (either indication or notification enabled)
   *  start battery level measurement */
  if (clientConfig) {
    battMeasure(); /* make an initial measurement */
    ghsstrsp = gecko_cmd_hardware_set_soft_timer(TIMER_MS_2_TIMERTICK(BATT_IND_TIMEOUT), BATT_SERVICE_TIMER, false);
    APP_ASSERT_DBG(!ghsstrsp->result, ghsstrsp->result);
  } else {
    ghsstrsp = gecko_cmd_hardware_set_soft_timer(TIMER_STOP, BATT_SERVICE_TIMER, false);
    APP_ASSERT_DBG(!ghsstrsp->result, ghsstrsp->result);
  }
}

static uint8_t appHwReadBatteryLevel(void)
{
  uint32_t voltage = measureBatteryVoltage();
  uint8_t level = calculateLevel(voltage, battCR2032Model, sizeof(battCR2032Model) / sizeof(VoltageCapacityPair));

  return level;
}

void battMeasure(void)
{
  struct gecko_msg_gatt_server_send_characteristic_notification_rsp_t* gsscnrsp = NULL;
  /* Update battery level based on battery level sensor */
  battBatteryLevel = appHwReadBatteryLevel();

  /* Send notification */
  gsscnrsp = gecko_cmd_gatt_server_send_characteristic_notification(
    conGetConnectionId(), gattdb_batt_measurement, sizeof(battBatteryLevel), &battBatteryLevel);
  APP_ASSERT_DBG(!gsscnrsp->result, gsscnrsp->result);
}

void battRead(void)
{
  struct gecko_msg_gatt_server_send_user_read_response_rsp_t* gssurrrsp = NULL;
  /* Update battery level based on battery level sensor */
  battBatteryLevel = appHwReadBatteryLevel();
  printf("Battery level = %d\r\n", battBatteryLevel);
  /* Send response to read request */
  gssurrrsp = gecko_cmd_gatt_server_send_user_read_response(conGetConnectionId(), gattdb_batt_measurement, 0,
                                                            sizeof(battBatteryLevel), &battBatteryLevel);
  APP_ASSERT_DBG(!gssurrrsp->result, gssurrrsp->result);
}

/** @} (end addtogroup batt) */
/** @} (end addtogroup Features) */
