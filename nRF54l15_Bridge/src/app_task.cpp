/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "board/board.h"
#include "lib/core/CHIPError.h"
#include "lib/support/CodeUtils.h"
#include "lib/support/Span.h"

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Commands.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/CommandHandlerInterface.h>
#include <app/CommandHandlerInterfaceRegistry.h>
#include <app/reporting/reporting.h>
#include <app/server/Server.h>
#include <app/util/attribute-storage.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#if CONFIG_BRIDGE_XIAO_IMU
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#endif
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define ZCL_BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_REVISION (4u)
#define ZCL_BRIDGED_DEVICE_BASIC_INFORMATION_FEATURE_MAP (0u)
#define ZCL_TEMPERATURE_SENSOR_CLUSTER_REVISION (4u)
#define ZCL_TEMPERATURE_SENSOR_FEATURE_MAP (0u)
#define ZCL_RELATIVE_HUMIDITY_SENSOR_CLUSTER_REVISION (3u)
#define ZCL_RELATIVE_HUMIDITY_SENSOR_FEATURE_MAP (0u)
#define ZCL_ON_OFF_CLUSTER_REVISION (6u)
#define ZCL_ON_OFF_FEATURE_MAP (1u)
#define ZCL_BOOLEAN_STATE_CLUSTER_REVISION (1u)
#define ZCL_BOOLEAN_STATE_FEATURE_MAP (0u)
#define ZCL_OCCUPANCY_SENSING_CLUSTER_REVISION (5u)
#define ZCL_OCCUPANCY_SENSING_FEATURE_MAP (0x02u)
#define ZCL_PRESSURE_MEASUREMENT_CLUSTER_REVISION (3u)
#define ZCL_PRESSURE_MEASUREMENT_FEATURE_MAP (0u)
#define ZCL_FLOW_MEASUREMENT_CLUSTER_REVISION (3u)
#define ZCL_FLOW_MEASUREMENT_FEATURE_MAP (0u)
#define ZCL_ILLUMINANCE_MEASUREMENT_CLUSTER_REVISION (3u)
#define ZCL_ILLUMINANCE_MEASUREMENT_FEATURE_MAP (0u)
#define ZCL_LEVEL_CONTROL_CLUSTER_REVISION (6u)
/* OnOff | Lighting */
#define ZCL_LEVEL_CONTROL_FEATURE_MAP (0x03u)
#define ZCL_COLOR_CONTROL_CLUSTER_REVISION (7u)
/* ColorTemperature */
#define ZCL_COLOR_CONTROL_FEATURE_MAP (0x10u)
#define ZCL_WINDOW_COVERING_CLUSTER_REVISION (5u)
/* Lift | PositionAwareLift */
#define ZCL_WINDOW_COVERING_FEATURE_MAP (0x05u)
#define ZCL_DOOR_LOCK_CLUSTER_REVISION (7u)
#define ZCL_DOOR_LOCK_FEATURE_MAP (0u)
/* HueSaturation | ColorTemperature */
#define ZCL_COLOR_CONTROL_FULL_FEATURE_MAP (0x11u)
#define ZCL_THERMOSTAT_CLUSTER_REVISION (8u)
/* Heating | Cooling */
#define ZCL_THERMOSTAT_FEATURE_MAP (0x03u)
#define ZCL_FAN_CONTROL_CLUSTER_REVISION (4u)
/* MultiSpeed off, only percent control */
#define ZCL_FAN_CONTROL_FEATURE_MAP (0u)
#define ZCL_AIR_QUALITY_CLUSTER_REVISION (1u)
#define ZCL_AIR_QUALITY_FEATURE_MAP (0u)
#define ZCL_SMOKE_CO_ALARM_CLUSTER_REVISION (2u)
/* SmokeAlarm */
#define ZCL_SMOKE_CO_ALARM_FEATURE_MAP (0x01u)
#define ZCL_VALVE_CLUSTER_REVISION (1u)
#define ZCL_VALVE_FEATURE_MAP (0u)

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace {
constexpr uint16_t kAggregatorEndpointId = 1;
constexpr uint16_t kFirstDynamicEndpointId = 2;
constexpr size_t kMaxVirtualEndpoints = CONFIG_BRIDGE_MAX_ENDPOINTS;
constexpr size_t kMaxVirtualEndpointNameLength = 32;
constexpr size_t kUniqueIdSize = 33;

/* Longest identity string a bridged device reports (vendor, product, serial,
 * version strings). Matter allows 32 characters plus the length byte. */
constexpr size_t kDeviceInfoStringSize = 33;
constexpr size_t kNodeLabelSize = kMaxVirtualEndpointNameLength + 1;
constexpr size_t kDescriptorAttributeArraySize = 66;

constexpr DeviceTypeId kDeviceTypeBridgedNode = 0x0013;
constexpr DeviceTypeId kDeviceTypeContactSensor = 0x0015;
constexpr DeviceTypeId kDeviceTypeOnOffLight = 0x0100;
constexpr DeviceTypeId kDeviceTypeOccupancySensor = 0x0107;
constexpr DeviceTypeId kDeviceTypeTempSensor = 0x0302;
constexpr DeviceTypeId kDeviceTypeHumiditySensor = 0x0307;
constexpr DeviceTypeId kDeviceTypePressureSensor = 0x0305;
constexpr DeviceTypeId kDeviceTypeFlowSensor = 0x0306;
constexpr DeviceTypeId kDeviceTypeLightSensor = 0x0106;
constexpr DeviceTypeId kDeviceTypeWaterLeakDetector = 0x0043;
constexpr DeviceTypeId kDeviceTypeOnOffPlug = 0x010A;
constexpr DeviceTypeId kDeviceTypeDimmableLight = 0x0101;
constexpr DeviceTypeId kDeviceTypeColorTempLight = 0x010C;
constexpr DeviceTypeId kDeviceTypeWindowCovering = 0x0202;
constexpr DeviceTypeId kDeviceTypeDoorLock = 0x000A;
constexpr DeviceTypeId kDeviceTypeExtendedColorLight = 0x010D;
constexpr DeviceTypeId kDeviceTypeDimmablePlug = 0x010B;
constexpr DeviceTypeId kDeviceTypeThermostat = 0x0301;
constexpr DeviceTypeId kDeviceTypeFan = 0x002B;
constexpr DeviceTypeId kDeviceTypeAirPurifier = 0x002D;
constexpr DeviceTypeId kDeviceTypeAirQualitySensor = 0x002C;
constexpr DeviceTypeId kDeviceTypeSmokeCoAlarm = 0x0076;
constexpr DeviceTypeId kDeviceTypeWaterValve = 0x0042;
constexpr DeviceTypeId kDeviceTypeWaterFreezeDetector = 0x0041;
constexpr DeviceTypeId kDeviceTypeRainSensor = 0x0044;
constexpr DeviceTypeId kDeviceTypeOnOffSensor = 0x0850;
constexpr DeviceTypeId kDeviceTypeMountedOnOffControl = 0x010F;
constexpr DeviceTypeId kDeviceTypePump = 0x0303;
constexpr DeviceTypeId kDeviceTypeCooktop = 0x0078;
constexpr DeviceTypeId kDeviceTypeMountedDimmableLoadControl = 0x0110;
constexpr DeviceTypeId kDeviceTypeSpeaker = 0x0022;
constexpr DeviceTypeId kDeviceTypeExtractorHood = 0x007A;
constexpr DeviceTypeId kDeviceTypeRoomAirConditioner = 0x0072;
constexpr DeviceTypeId kDeviceTypeHeatingCoolingUnit = 0x0300;
constexpr DeviceTypeId kDeviceTypeHeatPump = 0x0309;
constexpr DeviceTypeId kDeviceTypeWaterHeater = 0x050F;
constexpr DeviceTypeId kDeviceTypeSoilSensor = 0x0045;
constexpr DeviceTypeId kDeviceTypeGenericSwitch = 0x000F;
constexpr DeviceTypeId kDeviceTypeElectricalSensor = 0x0510;
constexpr DeviceTypeId kDeviceTypeDeviceEnergyManagement = 0x050D;
constexpr DeviceTypeId kDeviceTypeEnergyEvse = 0x050C;
constexpr DeviceTypeId kDeviceTypeSolarPower = 0x0017;
constexpr DeviceTypeId kDeviceTypeBatteryStorage = 0x0018;
constexpr DeviceTypeId kDeviceTypeElectricalEnergyTariff = 0x0513;
constexpr DeviceTypeId kDeviceTypeRoboticVacuum = 0x0074;
constexpr DeviceTypeId kDeviceTypeRefrigerator = 0x0070;
constexpr DeviceTypeId kDeviceTypeTemperatureControlledCabinet = 0x0071;
constexpr DeviceTypeId kDeviceTypeLaundryWasher = 0x0073;
constexpr DeviceTypeId kDeviceTypeLaundryDryer = 0x007C;
constexpr DeviceTypeId kDeviceTypeDishwasher = 0x0075;
constexpr DeviceTypeId kDeviceTypeOven = 0x007B;
constexpr DeviceTypeId kDeviceTypeCookSurface = 0x0077;
constexpr DeviceTypeId kDeviceTypeMicrowaveOven = 0x0079;
constexpr DeviceTypeId kDeviceTypeBasicVideoPlayer = 0x0028;
constexpr DeviceTypeId kDeviceTypeCastingVideoPlayer = 0x0023;
constexpr DeviceTypeId kDeviceTypeContentApp = 0x0024;
constexpr DeviceTypeId kDeviceTypeClosure = 0x0230;
constexpr DeviceTypeId kDeviceTypeClosurePanel = 0x0231;
constexpr DeviceTypeId kDeviceTypeCamera = 0x0142;


constexpr uint16_t kBridgedNodeDeviceVersion = 3;
constexpr uint16_t kContactSensorDeviceVersion = 2;
constexpr uint16_t kOnOffLightDeviceVersion = 3;
constexpr uint16_t kOccupancySensorDeviceVersion = 4;
constexpr uint16_t kTempSensorDeviceVersion = 2;
constexpr uint16_t kHumiditySensorDeviceVersion = 2;
constexpr uint16_t kPressureSensorDeviceVersion = 1;
constexpr uint16_t kFlowSensorDeviceVersion = 1;
constexpr uint16_t kLightSensorDeviceVersion = 2;
constexpr uint16_t kWaterLeakDetectorDeviceVersion = 1;
constexpr uint16_t kOnOffPlugDeviceVersion = 3;
constexpr uint16_t kDimmableLightDeviceVersion = 3;
constexpr uint16_t kColorTempLightDeviceVersion = 4;
constexpr uint16_t kWindowCoveringDeviceVersion = 5;
constexpr uint16_t kDoorLockDeviceVersion = 3;
constexpr uint16_t kExtendedColorLightDeviceVersion = 4;
constexpr uint16_t kDimmablePlugDeviceVersion = 4;
constexpr uint16_t kThermostatDeviceVersion = 4;
constexpr uint16_t kFanDeviceVersion = 3;
constexpr uint16_t kAirPurifierDeviceVersion = 2;
constexpr uint16_t kAirQualitySensorDeviceVersion = 1;
constexpr uint16_t kSmokeCoAlarmDeviceVersion = 1;
constexpr uint16_t kWaterValveDeviceVersion = 1;
constexpr uint16_t kWaterFreezeDetectorDeviceVersion = 1;
constexpr uint16_t kRainSensorDeviceVersion = 1;
constexpr uint16_t kOnOffSensorDeviceVersion = 1;
constexpr uint16_t kMountedOnOffControlDeviceVersion = 1;
constexpr uint16_t kPumpDeviceVersion = 3;
constexpr uint16_t kCooktopDeviceVersion = 1;
constexpr uint16_t kMountedDimmableLoadControlDeviceVersion = 1;
constexpr uint16_t kSpeakerDeviceVersion = 2;
constexpr uint16_t kExtractorHoodDeviceVersion = 1;
constexpr uint16_t kRoomAirConditionerDeviceVersion = 2;
constexpr uint16_t kHeatingCoolingUnitDeviceVersion = 2;
constexpr uint16_t kHeatPumpDeviceVersion = 1;
constexpr uint16_t kWaterHeaterDeviceVersion = 1;
constexpr uint16_t kSoilSensorDeviceVersion = 1;
constexpr uint16_t kGenericSwitchDeviceVersion = 3;
constexpr uint16_t kElectricalSensorDeviceVersion = 1;
constexpr uint16_t kDeviceEnergyManagementDeviceVersion = 1;
constexpr uint16_t kEnergyEvseDeviceVersion = 1;
constexpr uint16_t kSolarPowerDeviceVersion = 1;
constexpr uint16_t kBatteryStorageDeviceVersion = 1;
constexpr uint16_t kElectricalEnergyTariffDeviceVersion = 1;
constexpr uint16_t kRoboticVacuumDeviceVersion = 3;
constexpr uint16_t kRefrigeratorDeviceVersion = 2;
constexpr uint16_t kTemperatureControlledCabinetDeviceVersion = 3;
constexpr uint16_t kLaundryWasherDeviceVersion = 2;
constexpr uint16_t kLaundryDryerDeviceVersion = 1;
constexpr uint16_t kDishwasherDeviceVersion = 2;
constexpr uint16_t kOvenDeviceVersion = 2;
constexpr uint16_t kCookSurfaceDeviceVersion = 1;
constexpr uint16_t kMicrowaveOvenDeviceVersion = 1;
constexpr uint16_t kBasicVideoPlayerDeviceVersion = 2;
constexpr uint16_t kCastingVideoPlayerDeviceVersion = 2;
constexpr uint16_t kContentAppDeviceVersion = 1;
constexpr uint16_t kClosureDeviceVersion = 1;
constexpr uint16_t kClosurePanelDeviceVersion = 1;
constexpr uint16_t kCameraDeviceVersion = 1;


constexpr int32_t kDefaultTemperatureValue = 2300;
constexpr int32_t kDefaultHumidityValue = 4500;
constexpr int32_t kDefaultBinaryValue = 0;
constexpr int32_t kMinColorTempMireds = 153; /* ~6500 K */
constexpr int32_t kMaxColorTempMireds = 500; /* ~2000 K */
constexpr int32_t kDefaultColorTempMireds = 370;
constexpr int32_t kDefaultLevel = 254;
constexpr int32_t kDefaultHeatSetpoint = 2100;  /* 21.00 C */
constexpr int32_t kDefaultCoolSetpoint = 2500;  /* 25.00 C */

constexpr char kBridgeSettingsMetaKey[] = "bridge/meta";
constexpr char kBridgeSettingsSlotPrefix[] = "bridge/ep";
constexpr uint32_t kBridgeStorageMagic = 0x42524731; // BRG1
constexpr uint16_t kBridgeStorageVersion = 3;

static_assert(
    CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT == kMaxVirtualEndpoints,
    "Keep Matter dynamic endpoint storage in sync with the bridge slot table.");

enum class VirtualEndpointType : uint8_t {
  TemperatureSensor = 1,
  HumiditySensor = 2,
  OnOffLight = 3,
  ContactSensor = 4,
  OccupancySensor = 5,
  PressureSensor = 6,
  FlowSensor = 7,
  IlluminanceSensor = 8,
  WaterLeakDetector = 9,
  OnOffPlug = 10,
  DimmableLight = 11,
  ColorTemperatureLight = 12,
  WindowCovering = 13,
  DoorLock = 14,
  ExtendedColorLight = 15,
  DimmablePlug = 16,
  Thermostat = 17,
  Fan = 18,
  AirPurifier = 19,
  AirQualitySensor = 20,
  SmokeCoAlarm = 21,
  WaterValve = 22,
  WaterFreezeDetector = 23,
  RainSensor = 24,
  OnOffSensor = 25,
  MountedOnOffControl = 26,
  Pump = 27,
  Cooktop = 28,
  MountedDimmableLoadControl = 29,
  Speaker = 30,
  ExtractorHood = 31,
  RoomAirConditioner = 32,
  HeatingCoolingUnit = 33,
  HeatPump = 34,
  WaterHeater = 35,
  SoilSensor = 36,
  GenericSwitch = 37,
  ElectricalSensor = 38,
  DeviceEnergyManagement = 39,
  EnergyEvse = 40,
  SolarPower = 41,
  BatteryStorage = 42,
  ElectricalEnergyTariff = 43,
  RoboticVacuum = 44,
  Refrigerator = 45,
  TemperatureControlledCabinet = 46,
  LaundryWasher = 47,
  LaundryDryer = 48,
  Dishwasher = 49,
  Oven = 50,
  CookSurface = 51,
  MicrowaveOven = 52,
  BasicVideoPlayer = 53,
  CastingVideoPlayer = 54,
  ContentApp = 55,
  Closure = 56,
  ClosurePanel = 57,
  Camera = 58,
};

struct VirtualEndpointDefinition {
  VirtualEndpointType type;
  const char *slug;
  const char *displayName;
  DeviceTypeId deviceTypeId;
  uint16_t deviceVersion;
  const char *defaultName;
  int32_t defaultValue;
  ClusterId valueClusterId;
  AttributeId valueAttributeId;
  const char *valueHint;
};

struct VirtualEndpoint {
  bool allocated = false;
  uint16_t endpointId = 0;
  uint8_t dynamicIndex = 0;
  VirtualEndpointType type = VirtualEndpointType::TemperatureSensor;
  bool hasValue = false;
  int32_t value = 0;
  /* Secondary values for multi-attribute devices, e.g. a dimmable light uses
   * value=on/off and value2=brightness, a colour light adds value3=colour
   * temperature, a thermostat uses value2=setpoint and value3=mode. */
  int32_t value2 = 0;
  int32_t value3 = 0;
  int32_t value4 = 0;
  int32_t value5 = 0;
  bool reachable = true;
  bool globalSceneControl = true;
  uint16_t onTime = 0;
  uint16_t offWaitTime = 0;
  uint8_t startUpOnOff = 0xff;
  char name[kMaxVirtualEndpointNameLength] = {};
  char uniqueId[kUniqueIdSize] = {};
};

struct StoredBridgeMeta {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
};

struct StoredVirtualEndpoint {
  uint32_t magic;
  uint16_t version;
  uint8_t type;
  uint8_t hasValue;
  uint8_t reachable;
  uint8_t reserved;
  int32_t value;
  int32_t value2;
  int32_t value3;
  int32_t value4;
  int32_t value5;
  char name[kMaxVirtualEndpointNameLength];
};

static_assert(sizeof(StoredVirtualEndpoint) <= SETTINGS_MAX_VAL_LEN,
              "Stored endpoint state must fit one Zephyr settings value.");

VirtualEndpoint gVirtualEndpoints[kMaxVirtualEndpoints];

const VirtualEndpointDefinition kVirtualEndpointDefinitions[] = {
#if CONFIG_BRIDGE_TYPE_TEMPERATURE
    {VirtualEndpointType::TemperatureSensor, "temp", "Temperature Sensor",
     kDeviceTypeTempSensor, kTempSensorDeviceVersion, "temperature",
     kDefaultTemperatureValue, Clusters::TemperatureMeasurement::Id,
     Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Id,
     "centi-C, example 2300 = 23.00 C"},
#endif
#if CONFIG_BRIDGE_TYPE_HUMIDITY
    {VirtualEndpointType::HumiditySensor, "humidity", "Humidity Sensor",
     kDeviceTypeHumiditySensor, kHumiditySensorDeviceVersion, "humidity",
     kDefaultHumidityValue, Clusters::RelativeHumidityMeasurement::Id,
     Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Id,
     "centi-percent, example 4500 = 45.00%"},
#endif
#if CONFIG_BRIDGE_TYPE_ONOFF_LIGHT
    {VirtualEndpointType::OnOffLight, "light", "On/Off Light",
     kDeviceTypeOnOffLight, kOnOffLightDeviceVersion, "light",
     kDefaultBinaryValue, Clusters::OnOff::Id,
     Clusters::OnOff::Attributes::OnOff::Id, "0 = off, 1 = on"},
#endif
#if CONFIG_BRIDGE_TYPE_CONTACT
    {VirtualEndpointType::ContactSensor, "contact", "Contact Sensor",
     kDeviceTypeContactSensor, kContactSensorDeviceVersion, "contact",
     kDefaultBinaryValue, Clusters::BooleanState::Id,
     Clusters::BooleanState::Attributes::StateValue::Id,
     "0 = closed/false, 1 = open/true"},
#endif
#if CONFIG_BRIDGE_TYPE_OCCUPANCY
    {VirtualEndpointType::OccupancySensor, "occupancy", "Occupancy Sensor",
     kDeviceTypeOccupancySensor, kOccupancySensorDeviceVersion, "occupancy",
     kDefaultBinaryValue, Clusters::OccupancySensing::Id,
     Clusters::OccupancySensing::Attributes::Occupancy::Id,
     "0 = clear, 1 = occupied"},
#endif
#if CONFIG_BRIDGE_TYPE_PRESSURE
    {VirtualEndpointType::PressureSensor, "pressure", "Pressure Sensor",
     kDeviceTypePressureSensor, kPressureSensorDeviceVersion, "pressure",
     1013, Clusters::PressureMeasurement::Id,
     Clusters::PressureMeasurement::Attributes::MeasuredValue::Id,
     "hPa as int16, example 1013 = 1013 hPa"},
#endif
#if CONFIG_BRIDGE_TYPE_FLOW
    {VirtualEndpointType::FlowSensor, "flow", "Flow Sensor",
     kDeviceTypeFlowSensor, kFlowSensorDeviceVersion, "flow",
     0, Clusters::FlowMeasurement::Id,
     Clusters::FlowMeasurement::Attributes::MeasuredValue::Id,
     "deci-m3/h as uint16, example 250 = 25.0 m3/h"},
#endif
#if CONFIG_BRIDGE_TYPE_ILLUMINANCE
    {VirtualEndpointType::IlluminanceSensor, "illuminance", "Light Sensor",
     kDeviceTypeLightSensor, kLightSensorDeviceVersion, "illuminance",
     0, Clusters::IlluminanceMeasurement::Id,
     Clusters::IlluminanceMeasurement::Attributes::MeasuredValue::Id,
     "lux mapped value as uint16 (0..65534)"},
#endif
#if CONFIG_BRIDGE_TYPE_WATER_LEAK
    {VirtualEndpointType::WaterLeakDetector, "leak", "Water Leak Detector",
     kDeviceTypeWaterLeakDetector, kWaterLeakDetectorDeviceVersion, "leak",
     kDefaultBinaryValue, Clusters::BooleanState::Id,
     Clusters::BooleanState::Attributes::StateValue::Id,
     "0 = dry, 1 = leak detected"},
#endif
#if CONFIG_BRIDGE_TYPE_ONOFF_PLUG
    {VirtualEndpointType::OnOffPlug, "plug", "On/Off Plug",
     kDeviceTypeOnOffPlug, kOnOffPlugDeviceVersion, "plug",
     kDefaultBinaryValue, Clusters::OnOff::Id,
     Clusters::OnOff::Attributes::OnOff::Id, "0 = off, 1 = on"},
#endif
#if CONFIG_BRIDGE_TYPE_DIMMABLE_LIGHT
    {VirtualEndpointType::DimmableLight, "dimmer", "Dimmable Light",
     kDeviceTypeDimmableLight, kDimmableLightDeviceVersion, "dimmer",
     kDefaultBinaryValue, Clusters::OnOff::Id,
     Clusters::OnOff::Attributes::OnOff::Id,
     "<on/off> [level 0-254], example: 1 128"},
#endif
#if CONFIG_BRIDGE_TYPE_COLOR_LIGHT
    {VirtualEndpointType::ColorTemperatureLight, "colorlight",
     "Color Temperature Light", kDeviceTypeColorTempLight,
     kColorTempLightDeviceVersion, "colorlight", kDefaultBinaryValue,
     Clusters::OnOff::Id, Clusters::OnOff::Attributes::OnOff::Id,
     "<on/off> [level 0-254] [mireds 153-500], example: 1 254 370"},
#endif
#if CONFIG_BRIDGE_TYPE_WINDOW_COVERING
    {VirtualEndpointType::WindowCovering, "blind", "Window Covering",
     kDeviceTypeWindowCovering, kWindowCoveringDeviceVersion, "blind", 0,
     Clusters::WindowCovering::Id,
     Clusters::WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id,
     "position 0-100 (0 = open, 100 = closed)"},
#endif
#if CONFIG_BRIDGE_TYPE_DOOR_LOCK
    {VirtualEndpointType::DoorLock, "lock", "Door Lock", kDeviceTypeDoorLock,
     kDoorLockDeviceVersion, "lock", 1, Clusters::DoorLock::Id,
     Clusters::DoorLock::Attributes::LockState::Id,
     "0 = unlocked, 1 = locked"},
#endif
#if CONFIG_BRIDGE_TYPE_RGB_LIGHT
    {VirtualEndpointType::ExtendedColorLight, "rgblight",
     "Extended Color Light", kDeviceTypeExtendedColorLight,
     kExtendedColorLightDeviceVersion, "rgblight", kDefaultBinaryValue,
     Clusters::OnOff::Id, Clusters::OnOff::Attributes::OnOff::Id,
     "<on/off> [level 0-254] [mireds 153-500]; hue/sat set from the app"},
#endif
#if CONFIG_BRIDGE_TYPE_DIMMABLE_PLUG
    {VirtualEndpointType::DimmablePlug, "dimplug", "Dimmable Plug",
     kDeviceTypeDimmablePlug, kDimmablePlugDeviceVersion, "dimplug",
     kDefaultBinaryValue, Clusters::OnOff::Id,
     Clusters::OnOff::Attributes::OnOff::Id,
     "<on/off> [level 0-254], example: 1 200"},
#endif
#if CONFIG_BRIDGE_TYPE_THERMOSTAT
    {VirtualEndpointType::Thermostat, "thermostat", "Thermostat",
     kDeviceTypeThermostat, kThermostatDeviceVersion, "thermostat", 2100,
     Clusters::Thermostat::Id,
     Clusters::Thermostat::Attributes::LocalTemperature::Id,
     "<temp centi-C> [heat setpoint] [mode 0=off,1=auto,3=cool,4=heat]"},
#endif
#if CONFIG_BRIDGE_TYPE_FAN
    {VirtualEndpointType::Fan, "fan", "Fan", kDeviceTypeFan, kFanDeviceVersion,
     "fan", 0, Clusters::FanControl::Id,
     Clusters::FanControl::Attributes::PercentSetting::Id,
     "speed 0-100 percent"},
#endif
#if CONFIG_BRIDGE_TYPE_AIR_PURIFIER
    {VirtualEndpointType::AirPurifier, "purifier", "Air Purifier",
     kDeviceTypeAirPurifier, kAirPurifierDeviceVersion, "purifier", 0,
     Clusters::FanControl::Id,
     Clusters::FanControl::Attributes::PercentSetting::Id,
     "speed 0-100 percent"},
#endif
#if CONFIG_BRIDGE_TYPE_AIR_QUALITY
    {VirtualEndpointType::AirQualitySensor, "airquality", "Air Quality Sensor",
     kDeviceTypeAirQualitySensor, kAirQualitySensorDeviceVersion, "airquality",
     1, Clusters::AirQuality::Id,
     Clusters::AirQuality::Attributes::AirQuality::Id,
     "0=unknown,1=good,2=fair,3=moderate,4=poor,5=very poor,6=extremely poor"},
#endif
#if CONFIG_BRIDGE_TYPE_SMOKE_CO
    {VirtualEndpointType::SmokeCoAlarm, "smoke", "Smoke / CO Alarm",
     kDeviceTypeSmokeCoAlarm, kSmokeCoAlarmDeviceVersion, "smoke", 0,
     Clusters::SmokeCoAlarm::Id,
     Clusters::SmokeCoAlarm::Attributes::SmokeState::Id,
     "0 = normal, 1 = warning, 2 = critical"},
#endif
#if CONFIG_BRIDGE_TYPE_WATER_VALVE
    {VirtualEndpointType::WaterValve, "valve", "Water Valve",
     kDeviceTypeWaterValve, kWaterValveDeviceVersion, "valve",
     kDefaultBinaryValue, Clusters::ValveConfigurationAndControl::Id,
     Clusters::ValveConfigurationAndControl::Attributes::CurrentState::Id,
     "0 = closed, 1 = open"},
#endif
#if CONFIG_BRIDGE_TYPE_WATER_FREEZE
    {VirtualEndpointType::WaterFreezeDetector, "freeze",
     "Water Freeze Detector", kDeviceTypeWaterFreezeDetector,
     kWaterFreezeDetectorDeviceVersion, "freeze", kDefaultBinaryValue,
     Clusters::BooleanState::Id,
     Clusters::BooleanState::Attributes::StateValue::Id,
     "0 = ok, 1 = freeze detected"},
#endif
#if CONFIG_BRIDGE_TYPE_RAIN
    {VirtualEndpointType::RainSensor, "rain", "Rain Sensor",
     kDeviceTypeRainSensor, kRainSensorDeviceVersion, "rain",
     kDefaultBinaryValue, Clusters::BooleanState::Id,
     Clusters::BooleanState::Attributes::StateValue::Id,
     "0 = dry, 1 = rain detected"},
#endif
#if CONFIG_BRIDGE_TYPE_ONOFF_SENSOR
    {VirtualEndpointType::OnOffSensor, "onoffsensor", "On/Off Sensor",
     kDeviceTypeOnOffSensor, kOnOffSensorDeviceVersion, "onoffsensor", kDefaultBinaryValue,
     Clusters::OnOff::Id,
     Clusters::OnOff::Attributes::OnOff::Id,
     "0 = off, 1 = on"},
#endif
#if CONFIG_BRIDGE_TYPE_MOUNTED_ONOFF
    {VirtualEndpointType::MountedOnOffControl, "mountedonoff", "Mounted On/Off Control",
     kDeviceTypeMountedOnOffControl, kMountedOnOffControlDeviceVersion, "mountedonoff", kDefaultBinaryValue,
     Clusters::OnOff::Id,
     Clusters::OnOff::Attributes::OnOff::Id,
     "0 = off, 1 = on"},
#endif
#if CONFIG_BRIDGE_TYPE_PUMP
    {VirtualEndpointType::Pump, "pump", "Pump",
     kDeviceTypePump, kPumpDeviceVersion, "pump", kDefaultBinaryValue,
     Clusters::OnOff::Id,
     Clusters::OnOff::Attributes::OnOff::Id,
     "0 = off, 1 = on"},
#endif
#if CONFIG_BRIDGE_TYPE_COOKTOP
    {VirtualEndpointType::Cooktop, "cooktop", "Cooktop",
     kDeviceTypeCooktop, kCooktopDeviceVersion, "cooktop", kDefaultBinaryValue,
     Clusters::OnOff::Id,
     Clusters::OnOff::Attributes::OnOff::Id,
     "0 = off, 1 = on"},
#endif
#if CONFIG_BRIDGE_TYPE_MOUNTED_DIMMER
    {VirtualEndpointType::MountedDimmableLoadControl, "mounteddimmer", "Mounted Dimmable Load Control",
     kDeviceTypeMountedDimmableLoadControl, kMountedDimmableLoadControlDeviceVersion, "mounteddimmer", kDefaultBinaryValue,
     Clusters::OnOff::Id,
     Clusters::OnOff::Attributes::OnOff::Id,
     "<on/off> [level 0-254]"},
#endif
#if CONFIG_BRIDGE_TYPE_SPEAKER
    {VirtualEndpointType::Speaker, "speaker", "Speaker (on/off + volume)",
     kDeviceTypeSpeaker, kSpeakerDeviceVersion, "speaker", kDefaultBinaryValue,
     Clusters::OnOff::Id,
     Clusters::OnOff::Attributes::OnOff::Id,
     "<on/off> [level 0-254]"},
#endif
#if CONFIG_BRIDGE_TYPE_EXTRACTOR_HOOD
    {VirtualEndpointType::ExtractorHood, "hood", "Extractor Hood",
     kDeviceTypeExtractorHood, kExtractorHoodDeviceVersion, "hood", 0,
     Clusters::FanControl::Id,
     Clusters::FanControl::Attributes::PercentSetting::Id,
     "speed 0-100 percent"},
#endif
#if CONFIG_BRIDGE_TYPE_ROOM_AC
    {VirtualEndpointType::RoomAirConditioner, "aircon", "Room Air Conditioner",
     kDeviceTypeRoomAirConditioner, kRoomAirConditionerDeviceVersion, "aircon", 2100,
     Clusters::Thermostat::Id,
     Clusters::Thermostat::Attributes::LocalTemperature::Id,
     "<temp centi-C> [setpoint] [mode]"},
#endif
#if CONFIG_BRIDGE_TYPE_HVAC_UNIT
    {VirtualEndpointType::HeatingCoolingUnit, "hvacunit", "Heating/Cooling Unit",
     kDeviceTypeHeatingCoolingUnit, kHeatingCoolingUnitDeviceVersion, "hvacunit", 2100,
     Clusters::Thermostat::Id,
     Clusters::Thermostat::Attributes::LocalTemperature::Id,
     "<temp centi-C> [setpoint] [mode]"},
#endif
#if CONFIG_BRIDGE_TYPE_HEAT_PUMP
    {VirtualEndpointType::HeatPump, "heatpump", "Heat Pump",
     kDeviceTypeHeatPump, kHeatPumpDeviceVersion, "heatpump", 2100,
     Clusters::Thermostat::Id,
     Clusters::Thermostat::Attributes::LocalTemperature::Id,
     "<temp centi-C> [setpoint] [mode]"},
#endif
#if CONFIG_BRIDGE_TYPE_WATER_HEATER
    {VirtualEndpointType::WaterHeater, "waterheater", "Water Heater",
     kDeviceTypeWaterHeater, kWaterHeaterDeviceVersion, "waterheater", 2100,
     Clusters::Thermostat::Id,
     Clusters::Thermostat::Attributes::LocalTemperature::Id,
     "<temp centi-C> [setpoint] [mode]"},
#endif
#if CONFIG_BRIDGE_TYPE_SOIL
    {VirtualEndpointType::SoilSensor, "soil", "Soil Sensor (moisture)",
     kDeviceTypeSoilSensor, kSoilSensorDeviceVersion, "soil", 4500,
     Clusters::RelativeHumidityMeasurement::Id,
     Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Id,
     "centi-percent moisture, 4500 = 45.00%"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_CONTROL
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::GenericSwitch, "switch", "Generic Switch [stub]",
     kDeviceTypeGenericSwitch, kGenericSwitchDeviceVersion, "switch", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_ENERGY
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::ElectricalSensor, "elecsensor", "Electrical Sensor [stub]",
     kDeviceTypeElectricalSensor, kElectricalSensorDeviceVersion, "elecsensor", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_ENERGY
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::DeviceEnergyManagement, "dem", "Device Energy Management [stub]",
     kDeviceTypeDeviceEnergyManagement, kDeviceEnergyManagementDeviceVersion, "dem", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_ENERGY
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::EnergyEvse, "evse", "EV Charger (EVSE) [stub]",
     kDeviceTypeEnergyEvse, kEnergyEvseDeviceVersion, "evse", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_ENERGY
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::SolarPower, "solar", "Solar Power [stub]",
     kDeviceTypeSolarPower, kSolarPowerDeviceVersion, "solar", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_ENERGY
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::BatteryStorage, "battery", "Battery Storage [stub]",
     kDeviceTypeBatteryStorage, kBatteryStorageDeviceVersion, "battery", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_ENERGY
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::ElectricalEnergyTariff, "tariff", "Electrical Energy Tariff [stub]",
     kDeviceTypeElectricalEnergyTariff, kElectricalEnergyTariffDeviceVersion, "tariff", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_APPLIANCE
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::RoboticVacuum, "vacuum", "Robotic Vacuum Cleaner [stub]",
     kDeviceTypeRoboticVacuum, kRoboticVacuumDeviceVersion, "vacuum", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_APPLIANCE
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::Refrigerator, "fridge", "Refrigerator [stub]",
     kDeviceTypeRefrigerator, kRefrigeratorDeviceVersion, "fridge", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_APPLIANCE
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::TemperatureControlledCabinet, "cabinet", "Temperature Controlled Cabinet [stub]",
     kDeviceTypeTemperatureControlledCabinet, kTemperatureControlledCabinetDeviceVersion, "cabinet", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_APPLIANCE
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::LaundryWasher, "washer", "Laundry Washer [stub]",
     kDeviceTypeLaundryWasher, kLaundryWasherDeviceVersion, "washer", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_APPLIANCE
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::LaundryDryer, "dryer", "Laundry Dryer [stub]",
     kDeviceTypeLaundryDryer, kLaundryDryerDeviceVersion, "dryer", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_APPLIANCE
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::Dishwasher, "dishwasher", "Dishwasher [stub]",
     kDeviceTypeDishwasher, kDishwasherDeviceVersion, "dishwasher", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_APPLIANCE
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::Oven, "oven", "Oven [stub]",
     kDeviceTypeOven, kOvenDeviceVersion, "oven", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_APPLIANCE
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::CookSurface, "cooksurface", "Cook Surface [stub]",
     kDeviceTypeCookSurface, kCookSurfaceDeviceVersion, "cooksurface", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_APPLIANCE
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::MicrowaveOven, "microwave", "Microwave Oven [stub]",
     kDeviceTypeMicrowaveOven, kMicrowaveOvenDeviceVersion, "microwave", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_MEDIA
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::BasicVideoPlayer, "videoplayer", "Basic Video Player [stub]",
     kDeviceTypeBasicVideoPlayer, kBasicVideoPlayerDeviceVersion, "videoplayer", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_MEDIA
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::CastingVideoPlayer, "castplayer", "Casting Video Player [stub]",
     kDeviceTypeCastingVideoPlayer, kCastingVideoPlayerDeviceVersion, "castplayer", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_MEDIA
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::ContentApp, "contentapp", "Content App [stub]",
     kDeviceTypeContentApp, kContentAppDeviceVersion, "contentapp", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_CLOSURE
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::Closure, "closure", "Closure [stub]",
     kDeviceTypeClosure, kClosureDeviceVersion, "closure", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_CLOSURE
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::ClosurePanel, "closurepanel", "Closure Panel [stub]",
     kDeviceTypeClosurePanel, kClosurePanelDeviceVersion, "closurepanel", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
#if CONFIG_BRIDGE_TYPE_STUB_CAMERA
    /* STUB: endpoint is announced with Descriptor + BridgedDeviceBasicInformation
     * only, no functional clusters yet. */
    {VirtualEndpointType::Camera, "camera", "Camera [stub]",
     kDeviceTypeCamera, kCameraDeviceVersion, "camera", kDefaultBinaryValue,
     Clusters::BridgedDeviceBasicInformation::Id,
     Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
     "stub: 0/1 placeholder, no real control yet"},
#endif
};

const EmberAfDeviceType gBridgedTempSensorDeviceTypes[] = {
    {kDeviceTypeTempSensor, kTempSensorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedHumiditySensorDeviceTypes[] = {
    {kDeviceTypeHumiditySensor, kHumiditySensorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedOnOffLightDeviceTypes[] = {
    {kDeviceTypeOnOffLight, kOnOffLightDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedContactSensorDeviceTypes[] = {
    {kDeviceTypeContactSensor, kContactSensorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedOccupancySensorDeviceTypes[] = {
    {kDeviceTypeOccupancySensor, kOccupancySensorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedPressureSensorDeviceTypes[] = {
    {kDeviceTypePressureSensor, kPressureSensorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedFlowSensorDeviceTypes[] = {
    {kDeviceTypeFlowSensor, kFlowSensorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedIlluminanceSensorDeviceTypes[] = {
    {kDeviceTypeLightSensor, kLightSensorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedWaterLeakDeviceTypes[] = {
    {kDeviceTypeWaterLeakDetector, kWaterLeakDetectorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedOnOffPlugDeviceTypes[] = {
    {kDeviceTypeOnOffPlug, kOnOffPlugDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedDimmableLightDeviceTypes[] = {
    {kDeviceTypeDimmableLight, kDimmableLightDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedColorTempLightDeviceTypes[] = {
    {kDeviceTypeColorTempLight, kColorTempLightDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedWindowCoveringDeviceTypes[] = {
    {kDeviceTypeWindowCovering, kWindowCoveringDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedDoorLockDeviceTypes[] = {
    {kDeviceTypeDoorLock, kDoorLockDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedExtendedColorLightDeviceTypes[] = {
    {kDeviceTypeExtendedColorLight, kExtendedColorLightDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedDimmablePlugDeviceTypes[] = {
    {kDeviceTypeDimmablePlug, kDimmablePlugDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedThermostatDeviceTypes[] = {
    {kDeviceTypeThermostat, kThermostatDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedFanDeviceTypes[] = {
    {kDeviceTypeFan, kFanDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedAirPurifierDeviceTypes[] = {
    {kDeviceTypeAirPurifier, kAirPurifierDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedAirQualityDeviceTypes[] = {
    {kDeviceTypeAirQualitySensor, kAirQualitySensorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedSmokeCoAlarmDeviceTypes[] = {
    {kDeviceTypeSmokeCoAlarm, kSmokeCoAlarmDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedWaterValveDeviceTypes[] = {
    {kDeviceTypeWaterValve, kWaterValveDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedWaterFreezeDeviceTypes[] = {
    {kDeviceTypeWaterFreezeDetector, kWaterFreezeDetectorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedRainSensorDeviceTypes[] = {
    {kDeviceTypeRainSensor, kRainSensorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

const EmberAfDeviceType gBridgedOnOffSensorDeviceTypes[] = {
    {kDeviceTypeOnOffSensor, kOnOffSensorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedMountedOnOffControlDeviceTypes[] = {
    {kDeviceTypeMountedOnOffControl, kMountedOnOffControlDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedPumpDeviceTypes[] = {
    {kDeviceTypePump, kPumpDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedCooktopDeviceTypes[] = {
    {kDeviceTypeCooktop, kCooktopDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedMountedDimmableLoadControlDeviceTypes[] = {
    {kDeviceTypeMountedDimmableLoadControl, kMountedDimmableLoadControlDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedSpeakerDeviceTypes[] = {
    {kDeviceTypeSpeaker, kSpeakerDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedExtractorHoodDeviceTypes[] = {
    {kDeviceTypeExtractorHood, kExtractorHoodDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedRoomAirConditionerDeviceTypes[] = {
    {kDeviceTypeRoomAirConditioner, kRoomAirConditionerDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedHeatingCoolingUnitDeviceTypes[] = {
    {kDeviceTypeHeatingCoolingUnit, kHeatingCoolingUnitDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedHeatPumpDeviceTypes[] = {
    {kDeviceTypeHeatPump, kHeatPumpDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedWaterHeaterDeviceTypes[] = {
    {kDeviceTypeWaterHeater, kWaterHeaterDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedSoilSensorDeviceTypes[] = {
    {kDeviceTypeSoilSensor, kSoilSensorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedGenericSwitchDeviceTypes[] = {
    {kDeviceTypeGenericSwitch, kGenericSwitchDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedElectricalSensorDeviceTypes[] = {
    {kDeviceTypeElectricalSensor, kElectricalSensorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedDeviceEnergyManagementDeviceTypes[] = {
    {kDeviceTypeDeviceEnergyManagement, kDeviceEnergyManagementDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedEnergyEvseDeviceTypes[] = {
    {kDeviceTypeEnergyEvse, kEnergyEvseDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedSolarPowerDeviceTypes[] = {
    {kDeviceTypeSolarPower, kSolarPowerDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedBatteryStorageDeviceTypes[] = {
    {kDeviceTypeBatteryStorage, kBatteryStorageDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedElectricalEnergyTariffDeviceTypes[] = {
    {kDeviceTypeElectricalEnergyTariff, kElectricalEnergyTariffDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedRoboticVacuumDeviceTypes[] = {
    {kDeviceTypeRoboticVacuum, kRoboticVacuumDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedRefrigeratorDeviceTypes[] = {
    {kDeviceTypeRefrigerator, kRefrigeratorDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedTemperatureControlledCabinetDeviceTypes[] = {
    {kDeviceTypeTemperatureControlledCabinet, kTemperatureControlledCabinetDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedLaundryWasherDeviceTypes[] = {
    {kDeviceTypeLaundryWasher, kLaundryWasherDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedLaundryDryerDeviceTypes[] = {
    {kDeviceTypeLaundryDryer, kLaundryDryerDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedDishwasherDeviceTypes[] = {
    {kDeviceTypeDishwasher, kDishwasherDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedOvenDeviceTypes[] = {
    {kDeviceTypeOven, kOvenDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedCookSurfaceDeviceTypes[] = {
    {kDeviceTypeCookSurface, kCookSurfaceDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedMicrowaveOvenDeviceTypes[] = {
    {kDeviceTypeMicrowaveOven, kMicrowaveOvenDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedBasicVideoPlayerDeviceTypes[] = {
    {kDeviceTypeBasicVideoPlayer, kBasicVideoPlayerDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedCastingVideoPlayerDeviceTypes[] = {
    {kDeviceTypeCastingVideoPlayer, kCastingVideoPlayerDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedContentAppDeviceTypes[] = {
    {kDeviceTypeContentApp, kContentAppDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedClosureDeviceTypes[] = {
    {kDeviceTypeClosure, kClosureDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedClosurePanelDeviceTypes[] = {
    {kDeviceTypeClosurePanel, kClosurePanelDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};
const EmberAfDeviceType gBridgedCameraDeviceTypes[] = {
    {kDeviceTypeCamera, kCameraDeviceVersion},
    {kDeviceTypeBridgedNode, kBridgedNodeDeviceVersion},
};

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(tempSensorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(
    Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Id, INT16S, 2,
    0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::TemperatureMeasurement::Attributes::MinMeasuredValue::Id,
        INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::TemperatureMeasurement::Attributes::MaxMeasuredValue::Id,
        INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::TemperatureMeasurement::Attributes::FeatureMap::Id, BITMAP32,
        4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::TemperatureMeasurement::Attributes::ClusterRevision::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(humiditySensorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(
    Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Id,
    INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::RelativeHumidityMeasurement::Attributes::MinMeasuredValue::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::RelativeHumidityMeasurement::Attributes::MaxMeasuredValue::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::RelativeHumidityMeasurement::Attributes::FeatureMap::Id,
        BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::RelativeHumidityMeasurement::Attributes::ClusterRevision::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(onOffAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OnOff::Attributes::OnOff::Id, BOOLEAN, 1,
                          0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::OnOff::Attributes::GlobalSceneControl::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OnOff::Attributes::OnTime::Id, INT16U,
                              2, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OnOff::Attributes::OffWaitTime::Id,
                              INT16U, 2, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OnOff::Attributes::StartUpOnOff::Id,
                              ENUM8, 1, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OnOff::Attributes::FeatureMap::Id,
                              BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OnOff::Attributes::ClusterRevision::Id,
                              INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId onOffIncomingCommands[] = {
    Clusters::OnOff::Commands::Off::Id,
    Clusters::OnOff::Commands::On::Id,
    Clusters::OnOff::Commands::Toggle::Id,
    Clusters::OnOff::Commands::OffWithEffect::Id,
    Clusters::OnOff::Commands::OnWithRecallGlobalScene::Id,
    Clusters::OnOff::Commands::OnWithTimedOff::Id,
    kInvalidCommandId,
};

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(booleanStateAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::BooleanState::Attributes::StateValue::Id,
                          BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::BooleanState::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::BooleanState::Attributes::ClusterRevision::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(occupancySensingAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OccupancySensing::Attributes::Occupancy::Id,
                          BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::OccupancySensing::Attributes::OccupancySensorType::Id, ENUM8,
        1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::OccupancySensing::Attributes::OccupancySensorTypeBitmap::Id,
        BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::OccupancySensing::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::OccupancySensing::Attributes::ClusterRevision::Id, INT16U, 2,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(pressureSensorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(
    Clusters::PressureMeasurement::Attributes::MeasuredValue::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::PressureMeasurement::Attributes::MinMeasuredValue::Id, INT16S,
        2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::PressureMeasurement::Attributes::MaxMeasuredValue::Id, INT16S,
        2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::PressureMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::PressureMeasurement::Attributes::ClusterRevision::Id, INT16U,
        2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(flowSensorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(
    Clusters::FlowMeasurement::Attributes::MeasuredValue::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::FlowMeasurement::Attributes::MinMeasuredValue::Id, INT16U, 2,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::FlowMeasurement::Attributes::MaxMeasuredValue::Id, INT16U, 2,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::FlowMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::FlowMeasurement::Attributes::ClusterRevision::Id, INT16U, 2,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(illuminanceSensorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(
    Clusters::IlluminanceMeasurement::Attributes::MeasuredValue::Id, INT16U, 2,
    0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::IlluminanceMeasurement::Attributes::MinMeasuredValue::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::IlluminanceMeasurement::Attributes::MaxMeasuredValue::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::IlluminanceMeasurement::Attributes::FeatureMap::Id, BITMAP32,
        4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::IlluminanceMeasurement::Attributes::ClusterRevision::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(levelControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::LevelControl::Attributes::CurrentLevel::Id,
                          INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::LevelControl::Attributes::RemainingTime::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::LevelControl::Attributes::MinLevel::Id,
                              INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::LevelControl::Attributes::MaxLevel::Id,
                              INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::LevelControl::Attributes::Options::Id,
                              BITMAP8, 1, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::LevelControl::Attributes::OnLevel::Id,
                              INT8U, 1, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::LevelControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::LevelControl::Attributes::ClusterRevision::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId levelControlIncomingCommands[] = {
    Clusters::LevelControl::Commands::MoveToLevel::Id,
    Clusters::LevelControl::Commands::Move::Id,
    Clusters::LevelControl::Commands::Step::Id,
    Clusters::LevelControl::Commands::Stop::Id,
    Clusters::LevelControl::Commands::MoveToLevelWithOnOff::Id,
    Clusters::LevelControl::Commands::MoveWithOnOff::Id,
    Clusters::LevelControl::Commands::StepWithOnOff::Id,
    Clusters::LevelControl::Commands::StopWithOnOff::Id,
    kInvalidCommandId,
};

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(colorControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(
    Clusters::ColorControl::Attributes::ColorTemperatureMireds::Id, INT16U, 2,
    0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::ColorMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::ColorControl::Attributes::Options::Id,
                              BITMAP8, 1, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::EnhancedColorMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::ColorCapabilities::Id, BITMAP16, 2,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::ColorTempPhysicalMinMireds::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::ColorControl::Attributes::
                                  CoupleColorTempToLevelMinMireds::Id,
                              INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::ClusterRevision::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId colorControlIncomingCommands[] = {
    Clusters::ColorControl::Commands::MoveToColorTemperature::Id,
    Clusters::ColorControl::Commands::MoveColorTemperature::Id,
    Clusters::ColorControl::Commands::StepColorTemperature::Id,
    kInvalidCommandId,
};

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(windowCoveringAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::WindowCovering::Attributes::Type::Id, ENUM8,
                          1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::WindowCovering::Attributes::ConfigStatus::Id, BITMAP8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::WindowCovering::Attributes::OperationalStatus::Id, BITMAP8, 1,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::WindowCovering::Attributes::EndProductType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::WindowCovering::Attributes::Mode::Id,
                              BITMAP8, 1, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::WindowCovering::Attributes::
            TargetPositionLiftPercent100ths::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::WindowCovering::Attributes::
            CurrentPositionLiftPercent100ths::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::WindowCovering::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::WindowCovering::Attributes::ClusterRevision::Id, INT16U, 2,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId windowCoveringIncomingCommands[] = {
    Clusters::WindowCovering::Commands::UpOrOpen::Id,
    Clusters::WindowCovering::Commands::DownOrClose::Id,
    Clusters::WindowCovering::Commands::StopMotion::Id,
    Clusters::WindowCovering::Commands::GoToLiftPercentage::Id,
    kInvalidCommandId,
};

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(doorLockAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::DoorLock::Attributes::LockState::Id, ENUM8,
                          1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::DoorLock::Attributes::LockType::Id,
                              ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::DoorLock::Attributes::ActuatorEnabled::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::DoorLock::Attributes::OperatingMode::Id, ENUM8, 1,
        ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::DoorLock::Attributes::SupportedOperatingModes::Id, BITMAP16,
        2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::DoorLock::Attributes::FeatureMap::Id,
                              BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::DoorLock::Attributes::ClusterRevision::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId doorLockIncomingCommands[] = {
    Clusters::DoorLock::Commands::LockDoor::Id,
    Clusters::DoorLock::Commands::UnlockDoor::Id,
    kInvalidCommandId,
};

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(colorControlFullAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::ColorControl::Attributes::CurrentHue::Id,
                          INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::CurrentSaturation::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::ColorTemperatureMireds::Id, INT16U,
        2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::ColorControl::Attributes::ColorMode::Id,
                              ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::ColorControl::Attributes::Options::Id,
                              BITMAP8, 1, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::EnhancedColorMode::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::ColorCapabilities::Id, BITMAP16, 2,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::ColorTempPhysicalMinMireds::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::ColorControl::Attributes::
                                  CoupleColorTempToLevelMinMireds::Id,
                              INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ColorControl::Attributes::ClusterRevision::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId colorControlFullIncomingCommands[] = {
    Clusters::ColorControl::Commands::MoveToHue::Id,
    Clusters::ColorControl::Commands::MoveToSaturation::Id,
    Clusters::ColorControl::Commands::MoveToHueAndSaturation::Id,
    Clusters::ColorControl::Commands::MoveToColorTemperature::Id,
    Clusters::ColorControl::Commands::MoveColorTemperature::Id,
    Clusters::ColorControl::Commands::StepColorTemperature::Id,
    kInvalidCommandId,
};

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(thermostatAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(
    Clusters::Thermostat::Attributes::LocalTemperature::Id, INT16S, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::OccupiedHeatingSetpoint::Id, INT16S,
        2, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::OccupiedCoolingSetpoint::Id, INT16S,
        2, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::AbsMinHeatSetpointLimit::Id, INT16S,
        2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::AbsMaxHeatSetpointLimit::Id, INT16S,
        2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::AbsMinCoolSetpointLimit::Id, INT16S,
        2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::AbsMaxCoolSetpointLimit::Id, INT16S,
        2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::MinHeatSetpointLimit::Id, INT16S, 2,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::MaxHeatSetpointLimit::Id, INT16S, 2,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::MinCoolSetpointLimit::Id, INT16S, 2,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::MaxCoolSetpointLimit::Id, INT16S, 2,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::MinSetpointDeadBand::Id, INT8S, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::ControlSequenceOfOperation::Id, ENUM8,
        1, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::Thermostat::Attributes::SystemMode::Id,
                              ENUM8, 1, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::Thermostat::Attributes::FeatureMap::Id,
                              BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::Thermostat::Attributes::ClusterRevision::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId thermostatIncomingCommands[] = {
    Clusters::Thermostat::Commands::SetpointRaiseLower::Id,
    kInvalidCommandId,
};

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(fanControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::FanControl::Attributes::FanMode::Id, ENUM8,
                          1, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::FanControl::Attributes::FanModeSequence::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::FanControl::Attributes::PercentSetting::Id, INT8U, 1,
        ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::FanControl::Attributes::PercentCurrent::Id, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::FanControl::Attributes::FeatureMap::Id,
                              BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::FanControl::Attributes::ClusterRevision::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(airQualityAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::AirQuality::Attributes::AirQuality::Id,
                          ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::AirQuality::Attributes::FeatureMap::Id,
                              BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::AirQuality::Attributes::ClusterRevision::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(smokeCoAlarmAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(
    Clusters::SmokeCoAlarm::Attributes::ExpressedState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::SmokeCoAlarm::Attributes::SmokeState::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::SmokeCoAlarm::Attributes::BatteryAlert::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::SmokeCoAlarm::Attributes::TestInProgress::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::SmokeCoAlarm::Attributes::HardwareFaultAlert::Id, BOOLEAN, 1,
        0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::SmokeCoAlarm::Attributes::EndOfServiceAlert::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::SmokeCoAlarm::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::SmokeCoAlarm::Attributes::ClusterRevision::Id, INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(valveAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(
    Clusters::ValveConfigurationAndControl::Attributes::OpenDuration::Id,
    INT32U, 4, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::ValveConfigurationAndControl::
                                  Attributes::DefaultOpenDuration::Id,
                              INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(WRITABLE) |
                                  ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::ValveConfigurationAndControl::
                                  Attributes::RemainingDuration::Id,
                              INT32U, 4, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ValveConfigurationAndControl::Attributes::CurrentState::Id,
        ENUM8, 1, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ValveConfigurationAndControl::Attributes::TargetState::Id,
        ENUM8, 1, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ValveConfigurationAndControl::Attributes::FeatureMap::Id,
        BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::ValveConfigurationAndControl::Attributes::ClusterRevision::Id,
        INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

constexpr CommandId valveIncomingCommands[] = {
    Clusters::ValveConfigurationAndControl::Commands::Open::Id,
    Clusters::ValveConfigurationAndControl::Commands::Close::Id,
    kInvalidCommandId,
};

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(bridgedDeviceBasicAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(
    Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id,
    CHAR_STRING, kNodeLabelSize, ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
        BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::BridgedDeviceBasicInformation::Attributes::UniqueID::Id,
        CHAR_STRING, kUniqueIdSize, 0),
    /* Identity. Without these a controller has nothing to show and every
     * bridged device appears as "Unknown" - manufacturer, model, serial, the
     * lot. They are answered from the bridge's own identity: the devices are
     * virtual, so the thing that vouches for them is this board. */
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::BridgedDeviceBasicInformation::Attributes::VendorName::Id,
        CHAR_STRING, kDeviceInfoStringSize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::BridgedDeviceBasicInformation::Attributes::ProductName::Id,
        CHAR_STRING, kDeviceInfoStringSize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::BridgedDeviceBasicInformation::
                                  Attributes::SerialNumber::Id,
                              CHAR_STRING, kDeviceInfoStringSize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::BridgedDeviceBasicInformation::
                                  Attributes::HardwareVersion::Id,
                              INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::BridgedDeviceBasicInformation::
                                  Attributes::HardwareVersionString::Id,
                              CHAR_STRING, kDeviceInfoStringSize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::BridgedDeviceBasicInformation::
                                  Attributes::SoftwareVersion::Id,
                              INT32U, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::BridgedDeviceBasicInformation::
                                  Attributes::SoftwareVersionString::Id,
                              CHAR_STRING, kDeviceInfoStringSize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::BridgedDeviceBasicInformation::
                                  Attributes::ConfigurationVersion::Id,
                              INT32U, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(
        Clusters::BridgedDeviceBasicInformation::Attributes::FeatureMap::Id,
        BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::BridgedDeviceBasicInformation::
                                  Attributes::ClusterRevision::Id,
                              INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::Descriptor::Attributes::DeviceTypeList::Id,
                          ARRAY, kDescriptorAttributeArraySize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::Descriptor::Attributes::ServerList::Id,
                              ARRAY, kDescriptorAttributeArraySize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::Descriptor::Attributes::ClientList::Id,
                              ARRAY, kDescriptorAttributeArraySize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::Descriptor::Attributes::PartsList::Id,
                              ARRAY, kDescriptorAttributeArraySize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Clusters::Descriptor::Attributes::FeatureMap::Id,
                              BITMAP32, 4, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedTempSensorClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::TemperatureMeasurement::Id, tempSensorAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedHumiditySensorClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::RelativeHumidityMeasurement::Id,
                        humiditySensorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedOnOffLightClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::OnOff::Id, onOffAttrs,
                        ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedContactSensorClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::BooleanState::Id, booleanStateAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedOccupancySensorClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::OccupancySensing::Id, occupancySensingAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedPressureSensorClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::PressureMeasurement::Id, pressureSensorAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedFlowSensorClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::FlowMeasurement::Id, flowSensorAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedIlluminanceSensorClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::IlluminanceMeasurement::Id,
                        illuminanceSensorAttrs, ZAP_CLUSTER_MASK(SERVER),
                        nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedWaterLeakClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::BooleanState::Id, booleanStateAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedOnOffPlugClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::OnOff::Id, onOffAttrs,
                        ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedDimmableLightClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::OnOff::Id, onOffAttrs,
                        ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::LevelControl::Id, levelControlAttrs,
                            ZAP_CLUSTER_MASK(SERVER),
                            levelControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedColorTempLightClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::OnOff::Id, onOffAttrs,
                        ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::LevelControl::Id, levelControlAttrs,
                            ZAP_CLUSTER_MASK(SERVER),
                            levelControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::ColorControl::Id, colorControlAttrs,
                            ZAP_CLUSTER_MASK(SERVER),
                            colorControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedWindowCoveringClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::WindowCovering::Id, windowCoveringAttrs,
                        ZAP_CLUSTER_MASK(SERVER),
                        windowCoveringIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedDoorLockClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::DoorLock::Id, doorLockAttrs,
                        ZAP_CLUSTER_MASK(SERVER), doorLockIncomingCommands,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedExtendedColorLightClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::OnOff::Id, onOffAttrs,
                        ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::LevelControl::Id, levelControlAttrs,
                            ZAP_CLUSTER_MASK(SERVER),
                            levelControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::ColorControl::Id, colorControlFullAttrs,
                            ZAP_CLUSTER_MASK(SERVER),
                            colorControlFullIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedDimmablePlugClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::OnOff::Id, onOffAttrs,
                        ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::LevelControl::Id, levelControlAttrs,
                            ZAP_CLUSTER_MASK(SERVER),
                            levelControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedThermostatClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::Thermostat::Id, thermostatAttrs,
                        ZAP_CLUSTER_MASK(SERVER), thermostatIncomingCommands,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedFanClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::FanControl::Id, fanControlAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedAirPurifierClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::FanControl::Id, fanControlAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedAirQualityClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::AirQuality::Id, airQualityAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedSmokeCoAlarmClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::SmokeCoAlarm::Id, smokeCoAlarmAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedWaterValveClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::ValveConfigurationAndControl::Id, valveAttrs,
                        ZAP_CLUSTER_MASK(SERVER), valveIncomingCommands,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedWaterFreezeClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::BooleanState::Id, booleanStateAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedRainSensorClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::BooleanState::Id, booleanStateAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

/* STUB endpoints: only the mandatory bridge plumbing, no functional clusters.
 * Used by device types that are announced but not implemented yet. */
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedStubClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs,
                        ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id,
                            bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER),
                            nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(bridgedStubEndpoint, bridgedStubClusters);

DECLARE_DYNAMIC_ENDPOINT(bridgedTempSensorEndpoint, bridgedTempSensorClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedHumiditySensorEndpoint,
                         bridgedHumiditySensorClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedOnOffLightEndpoint, bridgedOnOffLightClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedContactSensorEndpoint,
                         bridgedContactSensorClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedOccupancySensorEndpoint,
                         bridgedOccupancySensorClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedPressureSensorEndpoint,
                         bridgedPressureSensorClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedFlowSensorEndpoint, bridgedFlowSensorClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedIlluminanceSensorEndpoint,
                         bridgedIlluminanceSensorClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedWaterLeakEndpoint, bridgedWaterLeakClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedOnOffPlugEndpoint, bridgedOnOffPlugClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedDimmableLightEndpoint,
                         bridgedDimmableLightClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedColorTempLightEndpoint,
                         bridgedColorTempLightClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedWindowCoveringEndpoint,
                         bridgedWindowCoveringClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedDoorLockEndpoint, bridgedDoorLockClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedExtendedColorLightEndpoint,
                         bridgedExtendedColorLightClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedDimmablePlugEndpoint,
                         bridgedDimmablePlugClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedThermostatEndpoint, bridgedThermostatClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedFanEndpoint, bridgedFanClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedAirPurifierEndpoint,
                         bridgedAirPurifierClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedAirQualityEndpoint, bridgedAirQualityClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedSmokeCoAlarmEndpoint,
                         bridgedSmokeCoAlarmClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedWaterValveEndpoint, bridgedWaterValveClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedWaterFreezeEndpoint,
                         bridgedWaterFreezeClusters);
DECLARE_DYNAMIC_ENDPOINT(bridgedRainSensorEndpoint, bridgedRainSensorClusters);

/* Shared by every stub device type: only one endpoint can occupy a given
 * dynamic index at a time, so a single table is enough. */

/* One shared data-version table for every endpoint type: a dynamic index is
 * owned by exactly one endpoint at a time, and each row is sized for the
 * largest cluster list (extended colour light: OnOff + LevelControl +
 * ColorControl + Descriptor + BridgedDeviceBasicInformation). */
constexpr size_t kMaxClustersPerEndpoint = 6;
DataVersion gEndpointDataVersions[kMaxVirtualEndpoints][kMaxClustersPerEndpoint];

static_assert(MATTER_ARRAY_SIZE(bridgedExtendedColorLightClusters) <=
                  kMaxClustersPerEndpoint,
              "Shared data-version rows must fit the largest cluster list.");
static_assert(MATTER_ARRAY_SIZE(bridgedThermostatClusters) <=
                  kMaxClustersPerEndpoint,
              "Shared data-version rows must fit the largest cluster list.");

class ChipStackLock {
public:
  ChipStackLock() { PlatformMgr().LockChipStack(); }
  ~ChipStackLock() { PlatformMgr().UnlockChipStack(); }

  ChipStackLock(const ChipStackLock &) = delete;
  ChipStackLock &operator=(const ChipStackLock &) = delete;
};

const VirtualEndpointDefinition *
FindEndpointDefinition(VirtualEndpointType type) {
  for (const auto &definition : kVirtualEndpointDefinitions) {
    if (definition.type == type) {
      return &definition;
    }
  }

  return nullptr;
}

bool IsKnownEndpointType(uint8_t type) {
  return FindEndpointDefinition(static_cast<VirtualEndpointType>(type)) !=
         nullptr;
}

bool IsOnOffControllableType(VirtualEndpointType type) {
  switch (type) {
  case VirtualEndpointType::OnOffSensor:
  case VirtualEndpointType::MountedOnOffControl:
  case VirtualEndpointType::Pump:
  case VirtualEndpointType::Cooktop:
  case VirtualEndpointType::MountedDimmableLoadControl:
  case VirtualEndpointType::Speaker:
    return true;
  default:
    return false;
  }
}

bool IsLevelControllableType(VirtualEndpointType type) {
  switch (type) {
  case VirtualEndpointType::MountedDimmableLoadControl:
  case VirtualEndpointType::Speaker:
    return true;
  default:
    return false;
  }
}

bool IsThermostatLikeType(VirtualEndpointType type) {
  switch (type) {
  case VirtualEndpointType::Thermostat:
  case VirtualEndpointType::RoomAirConditioner:
  case VirtualEndpointType::HeatingCoolingUnit:
  case VirtualEndpointType::HeatPump:
  case VirtualEndpointType::WaterHeater:
    return true;
  default:
    return false;
  }
}

bool TypeNameMatchesDefinition(const VirtualEndpointDefinition &definition,
                               const char *name) {
  if (strcmp(name, definition.slug) == 0) {
    return true;
  }

  switch (definition.type) {
  case VirtualEndpointType::TemperatureSensor:
    return strcmp(name, "temperature") == 0;
  case VirtualEndpointType::HumiditySensor:
    return strcmp(name, "humi") == 0 || strcmp(name, "rh") == 0;
  case VirtualEndpointType::OnOffLight:
    return strcmp(name, "onoff") == 0 || strcmp(name, "light") == 0;
  case VirtualEndpointType::ContactSensor:
    return strcmp(name, "contact_sensor") == 0;
  case VirtualEndpointType::OccupancySensor:
    return strcmp(name, "presence") == 0;
  case VirtualEndpointType::PressureSensor:
    return strcmp(name, "baro") == 0 || strcmp(name, "barometer") == 0;
  case VirtualEndpointType::FlowSensor:
    return false;
  case VirtualEndpointType::IlluminanceSensor:
    return strcmp(name, "lux") == 0 || strcmp(name, "light_sensor") == 0;
  case VirtualEndpointType::WaterLeakDetector:
    return strcmp(name, "water") == 0 || strcmp(name, "leak_sensor") == 0;
  case VirtualEndpointType::OnOffPlug:
    return strcmp(name, "outlet") == 0 || strcmp(name, "socket") == 0;
  case VirtualEndpointType::DimmableLight:
    return strcmp(name, "dim") == 0 || strcmp(name, "dimmable") == 0;
  case VirtualEndpointType::ColorTemperatureLight:
    return strcmp(name, "color") == 0 || strcmp(name, "cct") == 0;
  case VirtualEndpointType::WindowCovering:
    return strcmp(name, "cover") == 0 || strcmp(name, "curtain") == 0 ||
           strcmp(name, "shade") == 0;
  case VirtualEndpointType::DoorLock:
    return strcmp(name, "doorlock") == 0;
  case VirtualEndpointType::ExtendedColorLight:
    return strcmp(name, "rgb") == 0 || strcmp(name, "colorlight_rgb") == 0;
  case VirtualEndpointType::DimmablePlug:
    return strcmp(name, "dimmableplug") == 0;
  case VirtualEndpointType::Thermostat:
    return strcmp(name, "thermo") == 0 || strcmp(name, "hvac") == 0;
  case VirtualEndpointType::Fan:
    return strcmp(name, "ventilator") == 0;
  case VirtualEndpointType::AirPurifier:
    return strcmp(name, "airpurifier") == 0;
  case VirtualEndpointType::AirQualitySensor:
    return strcmp(name, "aqi") == 0 || strcmp(name, "air") == 0;
  case VirtualEndpointType::SmokeCoAlarm:
    return strcmp(name, "smokeco") == 0 || strcmp(name, "co") == 0;
  case VirtualEndpointType::WaterValve:
    return strcmp(name, "watervalve") == 0;
  case VirtualEndpointType::WaterFreezeDetector:
    return strcmp(name, "frost") == 0;
  case VirtualEndpointType::RainSensor:
    return strcmp(name, "rainsensor") == 0;
  case VirtualEndpointType::OnOffSensor:
  case VirtualEndpointType::MountedOnOffControl:
  case VirtualEndpointType::Pump:
  case VirtualEndpointType::Cooktop:
  case VirtualEndpointType::MountedDimmableLoadControl:
  case VirtualEndpointType::Speaker:
  case VirtualEndpointType::ExtractorHood:
  case VirtualEndpointType::RoomAirConditioner:
  case VirtualEndpointType::HeatingCoolingUnit:
  case VirtualEndpointType::HeatPump:
  case VirtualEndpointType::WaterHeater:
  case VirtualEndpointType::SoilSensor:
  case VirtualEndpointType::GenericSwitch:
  case VirtualEndpointType::ElectricalSensor:
  case VirtualEndpointType::DeviceEnergyManagement:
  case VirtualEndpointType::EnergyEvse:
  case VirtualEndpointType::SolarPower:
  case VirtualEndpointType::BatteryStorage:
  case VirtualEndpointType::ElectricalEnergyTariff:
  case VirtualEndpointType::RoboticVacuum:
  case VirtualEndpointType::Refrigerator:
  case VirtualEndpointType::TemperatureControlledCabinet:
  case VirtualEndpointType::LaundryWasher:
  case VirtualEndpointType::LaundryDryer:
  case VirtualEndpointType::Dishwasher:
  case VirtualEndpointType::Oven:
  case VirtualEndpointType::CookSurface:
  case VirtualEndpointType::MicrowaveOven:
  case VirtualEndpointType::BasicVideoPlayer:
  case VirtualEndpointType::CastingVideoPlayer:
  case VirtualEndpointType::ContentApp:
  case VirtualEndpointType::Closure:
  case VirtualEndpointType::ClosurePanel:
  case VirtualEndpointType::Camera:
    return false;
  }

  return false;
}

const VirtualEndpointDefinition *
FindEndpointDefinitionByName(const char *name) {
  for (const auto &definition : kVirtualEndpointDefinitions) {
    if (TypeNameMatchesDefinition(definition, name)) {
      return &definition;
    }
  }

  return nullptr;
}

const char *VirtualEndpointTypeName(VirtualEndpointType type) {
  const auto *definition = FindEndpointDefinition(type);
  return definition != nullptr ? definition->slug : "unknown";
}

VirtualEndpoint *FindVirtualEndpoint(uint16_t endpointId) {
  for (auto &endpoint : gVirtualEndpoints) {
    if (endpoint.allocated && endpoint.endpointId == endpointId) {
      return &endpoint;
    }
  }

  return nullptr;
}

bool HasEndpointOfType(VirtualEndpointType type) {
  for (const auto &endpoint : gVirtualEndpoints) {
    if (endpoint.allocated && endpoint.type == type) {
      return true;
    }
  }

  return false;
}

VirtualEndpoint *AllocateVirtualEndpointAt(size_t index) {
  if (index >= kMaxVirtualEndpoints || gVirtualEndpoints[index].allocated) {
    return nullptr;
  }

  auto &endpoint = gVirtualEndpoints[index];
  endpoint = {};
  endpoint.allocated = true;
  endpoint.dynamicIndex = static_cast<uint8_t>(index);
  endpoint.endpointId = static_cast<uint16_t>(kFirstDynamicEndpointId + index);
  endpoint.reachable = true;
  endpoint.globalSceneControl = true;
  endpoint.startUpOnOff = 0xff;
  /* Sensible defaults for the multi-value device types. */
  endpoint.value2 = kDefaultLevel;
  endpoint.value3 = kDefaultColorTempMireds;
  snprintf(endpoint.uniqueId, sizeof(endpoint.uniqueId), "vd-%u",
           endpoint.endpointId);
  return &endpoint;
}

size_t AllocatedVirtualEndpointCount() {
  size_t count = 0;

  for (const auto &endpoint : gVirtualEndpoints) {
    if (endpoint.allocated) {
      ++count;
    }
  }

  return count;
}

size_t AvailableVirtualEndpointCount() {
  return kMaxVirtualEndpoints - AllocatedVirtualEndpointCount();
}

void NotifyEndpointListChanged() {
  MatterReportingAttributeChangeCallback(
      kAggregatorEndpointId, Clusters::Descriptor::Id,
      Clusters::Descriptor::Attributes::PartsList::Id);
}

/* The board's user LED, exposed as an ordinary bridged On/Off device.
 *
 * It is the one endpoint whose state is visible without any other equipment:
 * toggle it from a controller and the light on the board follows, which makes
 * it a standing end-to-end check of the whole path. */
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
#define BRIDGE_HAS_ONBOARD_LED 1
constexpr char kOnboardLedName[] = "Onboard LED";
const struct gpio_dt_spec gOnboardLed = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
uint16_t gOnboardLedEndpointId = 0;
#endif

/* Drives the physical LED when its endpoint changes. Called for every value
 * change and cheap enough to leave unconditional - the id comparison fails
 * immediately for every other endpoint. */
void ApplyOnboardLedState(const VirtualEndpoint &endpoint) {
#ifdef BRIDGE_HAS_ONBOARD_LED
  if (gOnboardLedEndpointId == 0 || endpoint.endpointId != gOnboardLedEndpointId) {
    return;
  }
  (void)gpio_pin_set_dt(&gOnboardLed, endpoint.value != 0 ? 1 : 0);
#else
  (void)endpoint;
#endif
}

void NotifyEndpointValueChanged(const VirtualEndpoint &endpoint) {
  ApplyOnboardLedState(endpoint);

  const auto *definition = FindEndpointDefinition(endpoint.type);
  if (definition == nullptr) {
    return;
  }

  MatterReportingAttributeChangeCallback(endpoint.endpointId,
                                         definition->valueClusterId,
                                         definition->valueAttributeId);
}

void CopyString(char *destination, size_t destinationSize, const char *source) {
  if (destinationSize == 0) {
    return;
  }

  destination[0] = '\0';
  if (source == nullptr) {
    return;
  }

  strncpy(destination, source, destinationSize - 1);
  destination[destinationSize - 1] = '\0';
}

/* Percent-decoding, so a name can contain anything Matter allows.
 *
 * The Zephyr shell line editor only inserts characters that pass isprint(),
 * and that check is not optional - CONFIG_SHELL_ASCII_FILTER does not reach
 * it. Every byte of a UTF-8 name is therefore dropped on the way in, which is
 * why "Кухня" arrived as an empty string and the endpoint fell back to its
 * default name. Companions send such names as %D0%9A%D1%83..., which survives
 * the filter because it is plain ASCII; a person typing an ASCII name at the
 * console is unaffected, since there is nothing to decode.
 *
 * Truncation stops on a whole character: cutting a multi-byte sequence in half
 * would put invalid UTF-8 into the node label, and a controller is entitled to
 * reject that.
 */
size_t DecodePercentEscapes(char *text) {
  const char *read = text;
  char *write = text;

  auto hexDigit = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };

  while (*read != '\0') {
    if (read[0] == '%' && read[1] != '\0' && read[2] != '\0') {
      const int high = hexDigit(read[1]);
      const int low = hexDigit(read[2]);
      if (high >= 0 && low >= 0) {
        *write++ = static_cast<char>((high << 4) | low);
        read += 3;
        continue;
      }
    }
    *write++ = *read++;
  }

  *write = '\0';
  return static_cast<size_t>(write - text);
}

/* Drops a trailing partial UTF-8 sequence left behind by truncation. */
void TrimToCompleteUtf8(char *text) {
  size_t length = strlen(text);
  while (length > 0) {
    const auto last = static_cast<unsigned char>(text[length - 1]);
    if ((last & 0x80) == 0) {
      return; /* plain ASCII: always complete */
    }

    /* Walk back to the lead byte and see whether the sequence is whole. */
    size_t start = length - 1;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
      --start;
    }
    const auto lead = static_cast<unsigned char>(text[start]);
    size_t expected = 1;
    if ((lead & 0xE0) == 0xC0) {
      expected = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      expected = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      expected = 4;
    }

    if (length - start == expected) {
      return; /* the last character is complete */
    }
    text[start] = '\0';
    length = start;
  }
}

void CopyEndpointName(char *destination, size_t destinationSize, size_t argc,
                      char **argv, size_t firstNameArgument,
                      const char *fallbackName) {
  if (destinationSize == 0) {
    return;
  }

  destination[0] = '\0';

  /* Assemble and decode in a roomy scratch buffer, then truncate.
   *
   * A percent-escape is three characters for every byte, so "Спальня" arrives
   * as 42 characters for 14 bytes of text. Cutting that to the 32-byte field
   * first left five letters and half an escape - the endpoint ended up called
   * "Спаль%". Decoding before the limit applies means the limit counts the
   * bytes that will actually be stored. */
  char scratch[kMaxVirtualEndpointNameLength * 3 + 4] = {};

  for (size_t i = firstNameArgument; i < argc; ++i) {
    if (strlen(scratch) >= sizeof(scratch) - 1) {
      break;
    }

    if (i > firstNameArgument) {
      strncat(scratch, " ", sizeof(scratch) - strlen(scratch) - 1);
    }

    if (strlen(scratch) >= sizeof(scratch) - 1) {
      break;
    }

    strncat(scratch, argv[i], sizeof(scratch) - strlen(scratch) - 1);
  }

  DecodePercentEscapes(scratch);
  CopyString(destination, destinationSize, scratch);
  TrimToCompleteUtf8(destination);

  if (destination[0] == '\0') {
    CopyString(destination, destinationSize, fallbackName);
  }
}

void BuildEndpointName(char *destination, size_t destinationSize,
                       const char *prefix,
                       const VirtualEndpointDefinition &definition) {
  if (prefix != nullptr && prefix[0] != '\0') {
    CopyString(destination, destinationSize, prefix);
    strncat(destination, "_", destinationSize - strlen(destination) - 1);
    strncat(destination, definition.slug,
            destinationSize - strlen(destination) - 1);
  } else {
    CopyString(destination, destinationSize, definition.defaultName);
  }
}

bool ParseUInt16(const char *string, uint16_t &value) {
  char *end = nullptr;

  errno = 0;
  unsigned long parsed = strtoul(string, &end, 0);
  if (errno != 0 || end == string || *end != '\0' || parsed > UINT16_MAX) {
    return false;
  }

  value = static_cast<uint16_t>(parsed);
  return true;
}

bool ParseInt32(const char *string, int32_t &value) {
  char *end = nullptr;

  errno = 0;
  long parsed = strtol(string, &end, 0);
  if (errno != 0 || end == string || *end != '\0' || parsed < INT32_MIN ||
      parsed > INT32_MAX) {
    return false;
  }

  value = static_cast<int32_t>(parsed);
  return true;
}

bool ValidateEndpointValue(VirtualEndpointType type, int32_t value,
                           const char **errorMessage) {
  switch (type) {
  case VirtualEndpointType::TemperatureSensor:
    if (value < INT16_MIN || value > INT16_MAX) {
      *errorMessage = "temperature must be centi-C in int16 range";
      return false;
    }
    return true;
  case VirtualEndpointType::HumiditySensor:
    if (value < 0 || value > 10000) {
      *errorMessage = "humidity must be centi-percent in 0..10000 range";
      return false;
    }
    return true;
  case VirtualEndpointType::OnOffLight:
  case VirtualEndpointType::ContactSensor:
  case VirtualEndpointType::OccupancySensor:
  case VirtualEndpointType::WaterLeakDetector:
  case VirtualEndpointType::OnOffPlug:
    if (value != 0 && value != 1) {
      *errorMessage = "value must be 0 or 1";
      return false;
    }
    return true;
  case VirtualEndpointType::PressureSensor:
    if (value < INT16_MIN || value > INT16_MAX) {
      *errorMessage = "pressure must be int16 (hPa)";
      return false;
    }
    return true;
  case VirtualEndpointType::FlowSensor:
  case VirtualEndpointType::IlluminanceSensor:
    if (value < 0 || value > UINT16_MAX) {
      *errorMessage = "value must be uint16 (0..65535)";
      return false;
    }
    return true;
  case VirtualEndpointType::DimmableLight:
  case VirtualEndpointType::ColorTemperatureLight:
  case VirtualEndpointType::DoorLock:
    if (value != 0 && value != 1) {
      *errorMessage = "value must be 0 or 1";
      return false;
    }
    return true;
  case VirtualEndpointType::WindowCovering:
    if (value < 0 || value > 100) {
      *errorMessage = "position must be 0..100 percent";
      return false;
    }
    return true;
  case VirtualEndpointType::ExtendedColorLight:
  case VirtualEndpointType::DimmablePlug:
  case VirtualEndpointType::WaterValve:
  case VirtualEndpointType::WaterFreezeDetector:
  case VirtualEndpointType::RainSensor:
    if (value != 0 && value != 1) {
      *errorMessage = "value must be 0 or 1";
      return false;
    }
    return true;
  case VirtualEndpointType::Fan:
  case VirtualEndpointType::AirPurifier:
    if (value < 0 || value > 100) {
      *errorMessage = "speed must be 0..100 percent";
      return false;
    }
    return true;
  case VirtualEndpointType::Thermostat:
    if (value < -5000 || value > 5000) {
      *errorMessage = "temperature must be centi-C in -5000..5000";
      return false;
    }
    return true;
  case VirtualEndpointType::AirQualitySensor:
    if (value < 0 || value > 6) {
      *errorMessage = "air quality must be 0..6";
      return false;
    }
    return true;
  case VirtualEndpointType::SmokeCoAlarm:
    if (value < 0 || value > 2) {
      *errorMessage = "alarm state must be 0..2";
      return false;
    }
    return true;
  case VirtualEndpointType::OnOffSensor:
  case VirtualEndpointType::MountedOnOffControl:
  case VirtualEndpointType::Pump:
  case VirtualEndpointType::Cooktop:
  case VirtualEndpointType::MountedDimmableLoadControl:
  case VirtualEndpointType::Speaker:
  case VirtualEndpointType::GenericSwitch:
  case VirtualEndpointType::ElectricalSensor:
  case VirtualEndpointType::DeviceEnergyManagement:
  case VirtualEndpointType::EnergyEvse:
  case VirtualEndpointType::SolarPower:
  case VirtualEndpointType::BatteryStorage:
  case VirtualEndpointType::ElectricalEnergyTariff:
  case VirtualEndpointType::RoboticVacuum:
  case VirtualEndpointType::Refrigerator:
  case VirtualEndpointType::TemperatureControlledCabinet:
  case VirtualEndpointType::LaundryWasher:
  case VirtualEndpointType::LaundryDryer:
  case VirtualEndpointType::Dishwasher:
  case VirtualEndpointType::Oven:
  case VirtualEndpointType::CookSurface:
  case VirtualEndpointType::MicrowaveOven:
  case VirtualEndpointType::BasicVideoPlayer:
  case VirtualEndpointType::CastingVideoPlayer:
  case VirtualEndpointType::ContentApp:
  case VirtualEndpointType::Closure:
  case VirtualEndpointType::ClosurePanel:
  case VirtualEndpointType::Camera:
    if (value != 0 && value != 1) {
      *errorMessage = "value must be 0 or 1";
      return false;
    }
    return true;
  case VirtualEndpointType::ExtractorHood:
    if (value < 0 || value > 100) {
      *errorMessage = "speed must be 0..100 percent";
      return false;
    }
    return true;
  case VirtualEndpointType::RoomAirConditioner:
  case VirtualEndpointType::HeatingCoolingUnit:
  case VirtualEndpointType::HeatPump:
  case VirtualEndpointType::WaterHeater:
    if (value < -5000 || value > 5000) {
      *errorMessage = "temperature must be centi-C in -5000..5000";
      return false;
    }
    return true;
  case VirtualEndpointType::SoilSensor:
    if (value < 0 || value > 10000) {
      *errorMessage = "moisture must be centi-percent in 0..10000";
      return false;
    }
    return true;
  }

  *errorMessage = "unsupported endpoint type";
  return false;
}

Protocols::InteractionModel::Status CopyScalarToBuffer(const void *value,
                                                       size_t valueSize,
                                                       uint8_t *buffer,
                                                       uint16_t maxReadLength) {
  if (buffer == nullptr || value == nullptr || maxReadLength < valueSize) {
    return Protocols::InteractionModel::Status::Failure;
  }

  memcpy(buffer, value, valueSize);
  return Protocols::InteractionModel::Status::Success;
}

Protocols::InteractionModel::Status EncodeZclCharString(uint8_t *buffer,
                                                        uint16_t maxReadLength,
                                                        const char *string) {
  if (buffer == nullptr || maxReadLength == 0 || string == nullptr) {
    return Protocols::InteractionModel::Status::Failure;
  }

  size_t strLen = strlen(string);
  if (strLen > UINT8_MAX || maxReadLength < static_cast<uint16_t>(strLen + 1)) {
    return Protocols::InteractionModel::Status::Failure;
  }

  buffer[0] = static_cast<uint8_t>(strLen);
  memcpy(buffer + 1, string, strLen);
  return Protocols::InteractionModel::Status::Success;
}

Protocols::InteractionModel::Status
DecodeZclCharStringToEndpointName(const uint8_t *buffer, uint16_t maxLength,
                                  char *destination, size_t destinationSize) {
  if (buffer == nullptr || destination == nullptr || destinationSize == 0 ||
      maxLength == 0) {
    return Protocols::InteractionModel::Status::Failure;
  }

  size_t strLen = buffer[0];
  if (strLen + 1 > maxLength) {
    return Protocols::InteractionModel::Status::Failure;
  }

  size_t copyLen = strLen;
  if (copyLen >= destinationSize) {
    copyLen = destinationSize - 1;
  }

  memcpy(destination, buffer + 1, copyLen);
  destination[copyLen] = '\0';
  return Protocols::InteractionModel::Status::Success;
}

Protocols::InteractionModel::Status
HandleReadBridgedDeviceBasicAttribute(const VirtualEndpoint &endpoint,
                                      chip::AttributeId attributeId,
                                      uint8_t *buffer, uint16_t maxReadLength) {
  using namespace Clusters::BridgedDeviceBasicInformation::Attributes;

  if (attributeId == Reachable::Id) {
    bool reachable = endpoint.reachable;
    return CopyScalarToBuffer(&reachable, sizeof(reachable), buffer,
                              maxReadLength);
  }
  if (attributeId == NodeLabel::Id) {
    return EncodeZclCharString(buffer, maxReadLength, endpoint.name);
  }
  if (attributeId == UniqueID::Id) {
    return EncodeZclCharString(buffer, maxReadLength, endpoint.uniqueId);
  }
  /* Identity, answered from the bridge's own. The bridged devices are virtual;
   * what a controller is really asking is who vouches for them. */
  if (attributeId == VendorName::Id) {
    return EncodeZclCharString(buffer, maxReadLength,
                               CONFIG_CHIP_DEVICE_VENDOR_NAME);
  }
  if (attributeId == ProductName::Id) {
    return EncodeZclCharString(buffer, maxReadLength,
                               CONFIG_CHIP_DEVICE_PRODUCT_NAME);
  }
  if (attributeId == SerialNumber::Id) {
    return EncodeZclCharString(buffer, maxReadLength,
                               CONFIG_CHIP_DEVICE_SERIAL_NUMBER);
  }
  if (attributeId == HardwareVersionString::Id) {
    return EncodeZclCharString(buffer, maxReadLength,
                               CONFIG_CHIP_DEVICE_HARDWARE_VERSION_STRING);
  }
  if (attributeId == HardwareVersion::Id) {
    uint16_t hardwareVersion = CONFIG_CHIP_DEVICE_HARDWARE_VERSION;
    return CopyScalarToBuffer(&hardwareVersion, sizeof(hardwareVersion), buffer,
                              maxReadLength);
  }
  /* Software version comes from the project's VERSION file, the same place the
   * root node reports it from - keeping one answer for "which firmware is
   * this". CONFIG_CHIP_DEVICE_SOFTWARE_VERSION_STRING does not exist in NCS
   * 3.4 and setting it has no effect. */
  if (attributeId == SoftwareVersionString::Id) {
    return EncodeZclCharString(buffer, maxReadLength, APP_VERSION_STRING);
  }
  if (attributeId == SoftwareVersion::Id) {
    uint32_t softwareVersion = APPVERSION;
    return CopyScalarToBuffer(&softwareVersion, sizeof(softwareVersion), buffer,
                              maxReadLength);
  }
  if (attributeId == ConfigurationVersion::Id) {
    uint32_t configVersion = 1;
    return CopyScalarToBuffer(&configVersion, sizeof(configVersion), buffer,
                              maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_BRIDGED_DEVICE_BASIC_INFORMATION_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision =
        ZCL_BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status
HandleReadTempMeasurementAttribute(const VirtualEndpoint &endpoint,
                                   chip::AttributeId attributeId,
                                   uint8_t *buffer, uint16_t maxReadLength) {
  using namespace Clusters::TemperatureMeasurement::Attributes;

  if (attributeId == MeasuredValue::Id) {
    int32_t rawValue = endpoint.hasValue ? endpoint.value : 0;
    if (rawValue < INT16_MIN) {
      rawValue = INT16_MIN;
    } else if (rawValue > INT16_MAX) {
      rawValue = INT16_MAX;
    }
    int16_t measuredValue = static_cast<int16_t>(rawValue);
    return CopyScalarToBuffer(&measuredValue, sizeof(measuredValue), buffer,
                              maxReadLength);
  }
  if (attributeId == MinMeasuredValue::Id) {
    int16_t minValue = INT16_MIN;
    return CopyScalarToBuffer(&minValue, sizeof(minValue), buffer,
                              maxReadLength);
  }
  if (attributeId == MaxMeasuredValue::Id) {
    int16_t maxValue = INT16_MAX;
    return CopyScalarToBuffer(&maxValue, sizeof(maxValue), buffer,
                              maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_TEMPERATURE_SENSOR_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_TEMPERATURE_SENSOR_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status HandleReadHumidityMeasurementAttribute(
    const VirtualEndpoint &endpoint, chip::AttributeId attributeId,
    uint8_t *buffer, uint16_t maxReadLength) {
  using namespace Clusters::RelativeHumidityMeasurement::Attributes;

  if (attributeId == MeasuredValue::Id) {
    int32_t rawValue = endpoint.hasValue ? endpoint.value : 0;
    if (rawValue < 0) {
      rawValue = 0;
    } else if (rawValue > 10000) {
      rawValue = 10000;
    }
    uint16_t measuredValue = static_cast<uint16_t>(rawValue);
    return CopyScalarToBuffer(&measuredValue, sizeof(measuredValue), buffer,
                              maxReadLength);
  }
  if (attributeId == MinMeasuredValue::Id) {
    uint16_t minValue = 0;
    return CopyScalarToBuffer(&minValue, sizeof(minValue), buffer,
                              maxReadLength);
  }
  if (attributeId == MaxMeasuredValue::Id) {
    uint16_t maxValue = 10000;
    return CopyScalarToBuffer(&maxValue, sizeof(maxValue), buffer,
                              maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_RELATIVE_HUMIDITY_SENSOR_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_RELATIVE_HUMIDITY_SENSOR_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status HandleReadPressureMeasurementAttribute(
    const VirtualEndpoint &endpoint, chip::AttributeId attributeId,
    uint8_t *buffer, uint16_t maxReadLength) {
  using namespace Clusters::PressureMeasurement::Attributes;

  if (attributeId == MeasuredValue::Id) {
    int32_t rawValue = endpoint.hasValue ? endpoint.value : 0;
    if (rawValue < INT16_MIN) {
      rawValue = INT16_MIN;
    } else if (rawValue > INT16_MAX) {
      rawValue = INT16_MAX;
    }
    int16_t measuredValue = static_cast<int16_t>(rawValue);
    return CopyScalarToBuffer(&measuredValue, sizeof(measuredValue), buffer,
                              maxReadLength);
  }
  if (attributeId == MinMeasuredValue::Id) {
    int16_t minValue = INT16_MIN;
    return CopyScalarToBuffer(&minValue, sizeof(minValue), buffer,
                              maxReadLength);
  }
  if (attributeId == MaxMeasuredValue::Id) {
    int16_t maxValue = INT16_MAX;
    return CopyScalarToBuffer(&maxValue, sizeof(maxValue), buffer,
                              maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_PRESSURE_MEASUREMENT_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_PRESSURE_MEASUREMENT_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status HandleReadUnsignedMeasurementAttribute(
    const VirtualEndpoint &endpoint, chip::AttributeId attributeId,
    uint8_t *buffer, uint16_t maxReadLength, uint32_t featureMapValue,
    uint16_t clusterRevisionValue) {
  // Shared layout for FlowMeasurement / IlluminanceMeasurement: a single
  // uint16 MeasuredValue plus uint16 Min/Max bounds, FeatureMap, ClusterRev.
  // Their attribute IDs are identical across these measurement clusters.
  using namespace Clusters::FlowMeasurement::Attributes;

  if (attributeId == MeasuredValue::Id) {
    int32_t rawValue = endpoint.hasValue ? endpoint.value : 0;
    if (rawValue < 0) {
      rawValue = 0;
    } else if (rawValue > 0xFFFE) {
      rawValue = 0xFFFE;
    }
    uint16_t measuredValue = static_cast<uint16_t>(rawValue);
    return CopyScalarToBuffer(&measuredValue, sizeof(measuredValue), buffer,
                              maxReadLength);
  }
  if (attributeId == MinMeasuredValue::Id) {
    uint16_t minValue = 0;
    return CopyScalarToBuffer(&minValue, sizeof(minValue), buffer,
                              maxReadLength);
  }
  if (attributeId == MaxMeasuredValue::Id) {
    uint16_t maxValue = 0xFFFE;
    return CopyScalarToBuffer(&maxValue, sizeof(maxValue), buffer,
                              maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    return CopyScalarToBuffer(&featureMapValue, sizeof(featureMapValue), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    return CopyScalarToBuffer(&clusterRevisionValue,
                              sizeof(clusterRevisionValue), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status
HandleReadLevelControlAttribute(const VirtualEndpoint &endpoint,
                                chip::AttributeId attributeId, uint8_t *buffer,
                                uint16_t maxReadLength) {
  using namespace Clusters::LevelControl::Attributes;

  if (attributeId == CurrentLevel::Id) {
    int32_t raw = endpoint.value2;
    if (raw < 1) {
      raw = 1;
    } else if (raw > 254) {
      raw = 254;
    }
    uint8_t level = static_cast<uint8_t>(raw);
    return CopyScalarToBuffer(&level, sizeof(level), buffer, maxReadLength);
  }
  if (attributeId == RemainingTime::Id) {
    uint16_t remaining = 0;
    return CopyScalarToBuffer(&remaining, sizeof(remaining), buffer,
                              maxReadLength);
  }
  if (attributeId == MinLevel::Id) {
    uint8_t minLevel = 1;
    return CopyScalarToBuffer(&minLevel, sizeof(minLevel), buffer,
                              maxReadLength);
  }
  if (attributeId == MaxLevel::Id) {
    uint8_t maxLevel = 254;
    return CopyScalarToBuffer(&maxLevel, sizeof(maxLevel), buffer,
                              maxReadLength);
  }
  if (attributeId == Options::Id) {
    uint8_t options = 0;
    return CopyScalarToBuffer(&options, sizeof(options), buffer, maxReadLength);
  }
  if (attributeId == OnLevel::Id) {
    uint8_t onLevel = 254;
    return CopyScalarToBuffer(&onLevel, sizeof(onLevel), buffer, maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_LEVEL_CONTROL_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_LEVEL_CONTROL_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status
HandleReadColorControlAttribute(const VirtualEndpoint &endpoint,
                                chip::AttributeId attributeId, uint8_t *buffer,
                                uint16_t maxReadLength) {
  using namespace Clusters::ColorControl::Attributes;

  if (attributeId == ColorTemperatureMireds::Id) {
    int32_t raw = endpoint.value3;
    if (raw < kMinColorTempMireds) {
      raw = kMinColorTempMireds;
    } else if (raw > kMaxColorTempMireds) {
      raw = kMaxColorTempMireds;
    }
    uint16_t mireds = static_cast<uint16_t>(raw);
    return CopyScalarToBuffer(&mireds, sizeof(mireds), buffer, maxReadLength);
  }
  if (attributeId == ColorMode::Id || attributeId == EnhancedColorMode::Id) {
    /* 2 = color temperature mireds */
    uint8_t mode = 2;
    return CopyScalarToBuffer(&mode, sizeof(mode), buffer, maxReadLength);
  }
  if (attributeId == Options::Id) {
    uint8_t options = 0;
    return CopyScalarToBuffer(&options, sizeof(options), buffer, maxReadLength);
  }
  if (attributeId == ColorCapabilities::Id) {
    /* bit 4 = color temperature supported */
    uint16_t capabilities = 0x0010;
    return CopyScalarToBuffer(&capabilities, sizeof(capabilities), buffer,
                              maxReadLength);
  }
  if (attributeId == ColorTempPhysicalMinMireds::Id) {
    uint16_t value = kMinColorTempMireds;
    return CopyScalarToBuffer(&value, sizeof(value), buffer, maxReadLength);
  }
  if (attributeId == ColorTempPhysicalMaxMireds::Id) {
    uint16_t value = kMaxColorTempMireds;
    return CopyScalarToBuffer(&value, sizeof(value), buffer, maxReadLength);
  }
  if (attributeId == CoupleColorTempToLevelMinMireds::Id) {
    uint16_t value = kMinColorTempMireds;
    return CopyScalarToBuffer(&value, sizeof(value), buffer, maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_COLOR_CONTROL_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_COLOR_CONTROL_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status HandleReadWindowCoveringAttribute(
    const VirtualEndpoint &endpoint, chip::AttributeId attributeId,
    uint8_t *buffer, uint16_t maxReadLength) {
  using namespace Clusters::WindowCovering::Attributes;

  if (attributeId == CurrentPositionLiftPercent100ths::Id ||
      attributeId == TargetPositionLiftPercent100ths::Id) {
    int32_t percent = endpoint.hasValue ? endpoint.value : 0;
    if (percent < 0) {
      percent = 0;
    } else if (percent > 100) {
      percent = 100;
    }
    uint16_t hundredths = static_cast<uint16_t>(percent * 100);
    return CopyScalarToBuffer(&hundredths, sizeof(hundredths), buffer,
                              maxReadLength);
  }
  if (attributeId == Type::Id) {
    /* 0 = rollershade */
    uint8_t type = 0;
    return CopyScalarToBuffer(&type, sizeof(type), buffer, maxReadLength);
  }
  if (attributeId == EndProductType::Id) {
    uint8_t endProductType = 0;
    return CopyScalarToBuffer(&endProductType, sizeof(endProductType), buffer,
                              maxReadLength);
  }
  if (attributeId == ConfigStatus::Id) {
    /* Operational | LiftPositionAware */
    uint8_t configStatus = 0x0b;
    return CopyScalarToBuffer(&configStatus, sizeof(configStatus), buffer,
                              maxReadLength);
  }
  if (attributeId == OperationalStatus::Id) {
    /* Not moving */
    uint8_t operationalStatus = 0;
    return CopyScalarToBuffer(&operationalStatus, sizeof(operationalStatus),
                              buffer, maxReadLength);
  }
  if (attributeId == Mode::Id) {
    uint8_t mode = 0;
    return CopyScalarToBuffer(&mode, sizeof(mode), buffer, maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_WINDOW_COVERING_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_WINDOW_COVERING_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status
HandleReadDoorLockAttribute(const VirtualEndpoint &endpoint,
                            chip::AttributeId attributeId, uint8_t *buffer,
                            uint16_t maxReadLength) {
  using namespace Clusters::DoorLock::Attributes;

  if (attributeId == LockState::Id) {
    /* 1 = locked, 2 = unlocked */
    uint8_t lockState = (endpoint.hasValue && endpoint.value != 0) ? 1 : 2;
    return CopyScalarToBuffer(&lockState, sizeof(lockState), buffer,
                              maxReadLength);
  }
  if (attributeId == LockType::Id) {
    /* 0 = deadbolt */
    uint8_t lockType = 0;
    return CopyScalarToBuffer(&lockType, sizeof(lockType), buffer,
                              maxReadLength);
  }
  if (attributeId == ActuatorEnabled::Id) {
    bool actuatorEnabled = true;
    return CopyScalarToBuffer(&actuatorEnabled, sizeof(actuatorEnabled), buffer,
                              maxReadLength);
  }
  if (attributeId == OperatingMode::Id) {
    /* 0 = normal */
    uint8_t operatingMode = 0;
    return CopyScalarToBuffer(&operatingMode, sizeof(operatingMode), buffer,
                              maxReadLength);
  }
  if (attributeId == SupportedOperatingModes::Id) {
    /* Only "normal" supported: all other bits set to 1 = unsupported */
    uint16_t supported = 0xfff6;
    return CopyScalarToBuffer(&supported, sizeof(supported), buffer,
                              maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_DOOR_LOCK_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_DOOR_LOCK_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status HandleReadColorControlFullAttribute(
    const VirtualEndpoint &endpoint, chip::AttributeId attributeId,
    uint8_t *buffer, uint16_t maxReadLength) {
  using namespace Clusters::ColorControl::Attributes;

  if (attributeId == CurrentHue::Id) {
    uint8_t hue = static_cast<uint8_t>(endpoint.value4 & 0xff);
    return CopyScalarToBuffer(&hue, sizeof(hue), buffer, maxReadLength);
  }
  if (attributeId == CurrentSaturation::Id) {
    uint8_t saturation = static_cast<uint8_t>(endpoint.value5 & 0xff);
    return CopyScalarToBuffer(&saturation, sizeof(saturation), buffer,
                              maxReadLength);
  }
  if (attributeId == ColorMode::Id || attributeId == EnhancedColorMode::Id) {
    /* 0 = hue/saturation, 2 = colour temperature */
    uint8_t mode = (endpoint.value4 != 0 || endpoint.value5 != 0) ? 0 : 2;
    return CopyScalarToBuffer(&mode, sizeof(mode), buffer, maxReadLength);
  }
  if (attributeId == ColorCapabilities::Id) {
    /* bit0 = hue/sat, bit4 = colour temperature */
    uint16_t capabilities = 0x0011;
    return CopyScalarToBuffer(&capabilities, sizeof(capabilities), buffer,
                              maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_COLOR_CONTROL_FULL_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }

  /* Colour-temperature attributes and the cluster revision behave exactly as
   * on the CT-only light. */
  return HandleReadColorControlAttribute(endpoint, attributeId, buffer,
                                         maxReadLength);
}

Protocols::InteractionModel::Status
HandleReadThermostatAttribute(const VirtualEndpoint &endpoint,
                              chip::AttributeId attributeId, uint8_t *buffer,
                              uint16_t maxReadLength) {
  using namespace Clusters::Thermostat::Attributes;

  auto clampToInt16 = [](int32_t raw) -> int16_t {
    if (raw < INT16_MIN) {
      raw = INT16_MIN;
    } else if (raw > INT16_MAX) {
      raw = INT16_MAX;
    }
    return static_cast<int16_t>(raw);
  };

  if (attributeId == LocalTemperature::Id) {
    int16_t temp = clampToInt16(endpoint.hasValue ? endpoint.value : 2000);
    return CopyScalarToBuffer(&temp, sizeof(temp), buffer, maxReadLength);
  }
  if (attributeId == OccupiedHeatingSetpoint::Id) {
    int16_t setpoint = clampToInt16(endpoint.value2);
    return CopyScalarToBuffer(&setpoint, sizeof(setpoint), buffer,
                              maxReadLength);
  }
  if (attributeId == OccupiedCoolingSetpoint::Id) {
    int16_t setpoint = clampToInt16(endpoint.value4);
    return CopyScalarToBuffer(&setpoint, sizeof(setpoint), buffer,
                              maxReadLength);
  }
  if (attributeId == AbsMinHeatSetpointLimit::Id ||
      attributeId == MinHeatSetpointLimit::Id) {
    int16_t value = 700; /* 7.00 C */
    return CopyScalarToBuffer(&value, sizeof(value), buffer, maxReadLength);
  }
  if (attributeId == AbsMaxHeatSetpointLimit::Id ||
      attributeId == MaxHeatSetpointLimit::Id) {
    int16_t value = 3000; /* 30.00 C */
    return CopyScalarToBuffer(&value, sizeof(value), buffer, maxReadLength);
  }
  if (attributeId == AbsMinCoolSetpointLimit::Id ||
      attributeId == MinCoolSetpointLimit::Id) {
    int16_t value = 1600; /* 16.00 C */
    return CopyScalarToBuffer(&value, sizeof(value), buffer, maxReadLength);
  }
  if (attributeId == AbsMaxCoolSetpointLimit::Id ||
      attributeId == MaxCoolSetpointLimit::Id) {
    int16_t value = 3200; /* 32.00 C */
    return CopyScalarToBuffer(&value, sizeof(value), buffer, maxReadLength);
  }
  if (attributeId == MinSetpointDeadBand::Id) {
    int8_t deadBand = 25; /* 2.5 C */
    return CopyScalarToBuffer(&deadBand, sizeof(deadBand), buffer,
                              maxReadLength);
  }
  if (attributeId == ControlSequenceOfOperation::Id) {
    /* 4 = cooling and heating */
    uint8_t sequence = 4;
    return CopyScalarToBuffer(&sequence, sizeof(sequence), buffer,
                              maxReadLength);
  }
  if (attributeId == SystemMode::Id) {
    uint8_t mode = static_cast<uint8_t>(endpoint.value3 & 0xff);
    return CopyScalarToBuffer(&mode, sizeof(mode), buffer, maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_THERMOSTAT_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_THERMOSTAT_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status
HandleReadFanControlAttribute(const VirtualEndpoint &endpoint,
                              chip::AttributeId attributeId, uint8_t *buffer,
                              uint16_t maxReadLength) {
  using namespace Clusters::FanControl::Attributes;

  int32_t percent = endpoint.hasValue ? endpoint.value : 0;
  if (percent < 0) {
    percent = 0;
  } else if (percent > 100) {
    percent = 100;
  }

  if (attributeId == PercentSetting::Id || attributeId == PercentCurrent::Id) {
    uint8_t value = static_cast<uint8_t>(percent);
    return CopyScalarToBuffer(&value, sizeof(value), buffer, maxReadLength);
  }
  if (attributeId == FanMode::Id) {
    /* 0=off 1=low 2=medium 3=high */
    uint8_t mode = 0;
    if (percent > 66) {
      mode = 3;
    } else if (percent > 33) {
      mode = 2;
    } else if (percent > 0) {
      mode = 1;
    }
    return CopyScalarToBuffer(&mode, sizeof(mode), buffer, maxReadLength);
  }
  if (attributeId == FanModeSequence::Id) {
    /* 2 = off/low/med/high */
    uint8_t sequence = 2;
    return CopyScalarToBuffer(&sequence, sizeof(sequence), buffer,
                              maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_FAN_CONTROL_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_FAN_CONTROL_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status
HandleReadAirQualityAttribute(const VirtualEndpoint &endpoint,
                              chip::AttributeId attributeId, uint8_t *buffer,
                              uint16_t maxReadLength) {
  using namespace Clusters::AirQuality::Attributes;

  if (attributeId == AirQuality::Id) {
    int32_t raw = endpoint.hasValue ? endpoint.value : 0;
    if (raw < 0) {
      raw = 0;
    } else if (raw > 6) {
      raw = 6;
    }
    uint8_t quality = static_cast<uint8_t>(raw);
    return CopyScalarToBuffer(&quality, sizeof(quality), buffer, maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_AIR_QUALITY_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_AIR_QUALITY_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status
HandleReadSmokeCoAlarmAttribute(const VirtualEndpoint &endpoint,
                                chip::AttributeId attributeId, uint8_t *buffer,
                                uint16_t maxReadLength) {
  using namespace Clusters::SmokeCoAlarm::Attributes;

  int32_t raw = endpoint.hasValue ? endpoint.value : 0;
  if (raw < 0) {
    raw = 0;
  } else if (raw > 2) {
    raw = 2;
  }

  if (attributeId == ExpressedState::Id) {
    /* 0 = normal, 1 = smoke alarm */
    uint8_t state = raw != 0 ? 1 : 0;
    return CopyScalarToBuffer(&state, sizeof(state), buffer, maxReadLength);
  }
  if (attributeId == SmokeState::Id) {
    uint8_t state = static_cast<uint8_t>(raw);
    return CopyScalarToBuffer(&state, sizeof(state), buffer, maxReadLength);
  }
  if (attributeId == BatteryAlert::Id ||
      attributeId == EndOfServiceAlert::Id) {
    uint8_t state = 0;
    return CopyScalarToBuffer(&state, sizeof(state), buffer, maxReadLength);
  }
  if (attributeId == TestInProgress::Id ||
      attributeId == HardwareFaultAlert::Id) {
    bool flag = false;
    return CopyScalarToBuffer(&flag, sizeof(flag), buffer, maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_SMOKE_CO_ALARM_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_SMOKE_CO_ALARM_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status
HandleReadValveAttribute(const VirtualEndpoint &endpoint,
                         chip::AttributeId attributeId, uint8_t *buffer,
                         uint16_t maxReadLength) {
  using namespace Clusters::ValveConfigurationAndControl::Attributes;

  if (attributeId == CurrentState::Id || attributeId == TargetState::Id) {
    /* 0 = closed, 1 = open */
    uint8_t state = (endpoint.hasValue && endpoint.value != 0) ? 1 : 0;
    return CopyScalarToBuffer(&state, sizeof(state), buffer, maxReadLength);
  }
  if (attributeId == OpenDuration::Id ||
      attributeId == DefaultOpenDuration::Id ||
      attributeId == RemainingDuration::Id) {
    uint32_t duration = 0;
    return CopyScalarToBuffer(&duration, sizeof(duration), buffer,
                              maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_VALVE_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_VALVE_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status
HandleReadOnOffAttribute(const VirtualEndpoint &endpoint,
                         chip::AttributeId attributeId, uint8_t *buffer,
                         uint16_t maxReadLength) {
  using namespace Clusters::OnOff::Attributes;

  if (attributeId == OnOff::Id) {
    bool onOff = endpoint.hasValue && endpoint.value != 0;
    return CopyScalarToBuffer(&onOff, sizeof(onOff), buffer, maxReadLength);
  }
  if (attributeId == GlobalSceneControl::Id) {
    bool globalSceneControl = endpoint.globalSceneControl;
    return CopyScalarToBuffer(&globalSceneControl, sizeof(globalSceneControl),
                              buffer, maxReadLength);
  }
  if (attributeId == OnTime::Id) {
    uint16_t onTime = endpoint.onTime;
    return CopyScalarToBuffer(&onTime, sizeof(onTime), buffer, maxReadLength);
  }
  if (attributeId == OffWaitTime::Id) {
    uint16_t offWaitTime = endpoint.offWaitTime;
    return CopyScalarToBuffer(&offWaitTime, sizeof(offWaitTime), buffer,
                              maxReadLength);
  }
  if (attributeId == StartUpOnOff::Id) {
    uint8_t startUpOnOff = endpoint.startUpOnOff;
    return CopyScalarToBuffer(&startUpOnOff, sizeof(startUpOnOff), buffer,
                              maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_ON_OFF_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_ON_OFF_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status
HandleReadBooleanStateAttribute(const VirtualEndpoint &endpoint,
                                chip::AttributeId attributeId, uint8_t *buffer,
                                uint16_t maxReadLength) {
  using namespace Clusters::BooleanState::Attributes;

  if (attributeId == StateValue::Id) {
    bool stateValue = endpoint.hasValue && endpoint.value != 0;
    return CopyScalarToBuffer(&stateValue, sizeof(stateValue), buffer,
                              maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_BOOLEAN_STATE_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_BOOLEAN_STATE_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status
HandleReadOccupancySensingAttribute(const VirtualEndpoint &endpoint,
                                    chip::AttributeId attributeId,
                                    uint8_t *buffer, uint16_t maxReadLength) {
  using namespace Clusters::OccupancySensing::Attributes;

  if (attributeId == Occupancy::Id) {
    uint8_t occupancy = (endpoint.hasValue && endpoint.value != 0) ? 1 : 0;
    return CopyScalarToBuffer(&occupancy, sizeof(occupancy), buffer,
                              maxReadLength);
  }
  if (attributeId == OccupancySensorType::Id) {
    uint8_t sensorType = 0; // PIR
    return CopyScalarToBuffer(&sensorType, sizeof(sensorType), buffer,
                              maxReadLength);
  }
  if (attributeId == OccupancySensorTypeBitmap::Id) {
    uint8_t sensorTypeBitmap = 1; // PIR
    return CopyScalarToBuffer(&sensorTypeBitmap, sizeof(sensorTypeBitmap),
                              buffer, maxReadLength);
  }
  if (attributeId == FeatureMap::Id) {
    uint32_t featureMap = ZCL_OCCUPANCY_SENSING_FEATURE_MAP;
    return CopyScalarToBuffer(&featureMap, sizeof(featureMap), buffer,
                              maxReadLength);
  }
  if (attributeId == ClusterRevision::Id) {
    uint16_t clusterRevision = ZCL_OCCUPANCY_SENSING_CLUSTER_REVISION;
    return CopyScalarToBuffer(&clusterRevision, sizeof(clusterRevision), buffer,
                              maxReadLength);
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status
HandleWriteOnOffAttribute(VirtualEndpoint &endpoint,
                          chip::AttributeId attributeId, const uint8_t *buffer,
                          uint16_t size) {
  using namespace Clusters::OnOff::Attributes;

  if (buffer == nullptr) {
    return Protocols::InteractionModel::Status::Failure;
  }

  if (attributeId == OnOff::Id) {
    if (size < sizeof(bool)) {
      return Protocols::InteractionModel::Status::Failure;
    }
    endpoint.value = buffer[0] ? 1 : 0;
    endpoint.hasValue = true;
    NotifyEndpointValueChanged(endpoint);
    return Protocols::InteractionModel::Status::Success;
  }
  if (attributeId == OnTime::Id) {
    if (size < sizeof(uint16_t)) {
      return Protocols::InteractionModel::Status::Failure;
    }
    memcpy(&endpoint.onTime, buffer, sizeof(endpoint.onTime));
    return Protocols::InteractionModel::Status::Success;
  }
  if (attributeId == OffWaitTime::Id) {
    if (size < sizeof(uint16_t)) {
      return Protocols::InteractionModel::Status::Failure;
    }
    memcpy(&endpoint.offWaitTime, buffer, sizeof(endpoint.offWaitTime));
    return Protocols::InteractionModel::Status::Success;
  }
  if (attributeId == StartUpOnOff::Id) {
    if (size < sizeof(uint8_t)) {
      return Protocols::InteractionModel::Status::Failure;
    }
    endpoint.startUpOnOff = buffer[0];
    return Protocols::InteractionModel::Status::Success;
  }

  return Protocols::InteractionModel::Status::Failure;
}

CHIP_ERROR
SetMatterDynamicEndpoint(VirtualEndpoint *endpoint,
                         const EmberAfEndpointType *ep,
                         chip::Span<DataVersion> dataVersionStorage,
                         chip::Span<const EmberAfDeviceType> deviceTypeList) {
#if !CHIP_CONFIG_USE_ENDPOINT_UNIQUE_ID
  return emberAfSetDynamicEndpoint(endpoint->dynamicIndex, endpoint->endpointId,
                                   ep, dataVersionStorage, deviceTypeList,
                                   kAggregatorEndpointId);
#else
  return emberAfSetDynamicEndpointWithEpUniqueId(
      endpoint->dynamicIndex, endpoint->endpointId, ep, dataVersionStorage,
      deviceTypeList,
      chip::CharSpan(endpoint->uniqueId, strlen(endpoint->uniqueId)),
      kAggregatorEndpointId);
#endif
}

CHIP_ERROR AddMatterDynamicEndpoint(VirtualEndpoint *endpoint) {
  switch (endpoint->type) {
  case VirtualEndpointType::TemperatureSensor:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedTempSensorEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedTempSensorClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedTempSensorDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedTempSensorDeviceTypes)));
  case VirtualEndpointType::HumiditySensor:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedHumiditySensorEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedHumiditySensorClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedHumiditySensorDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedHumiditySensorDeviceTypes)));
  case VirtualEndpointType::OnOffLight:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedOnOffLightEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedOnOffLightClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedOnOffLightDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedOnOffLightDeviceTypes)));
  case VirtualEndpointType::ContactSensor:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedContactSensorEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedContactSensorClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedContactSensorDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedContactSensorDeviceTypes)));
  case VirtualEndpointType::OccupancySensor:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedOccupancySensorEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedOccupancySensorClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedOccupancySensorDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedOccupancySensorDeviceTypes)));
  case VirtualEndpointType::PressureSensor:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedPressureSensorEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedPressureSensorClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedPressureSensorDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedPressureSensorDeviceTypes)));
  case VirtualEndpointType::FlowSensor:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedFlowSensorEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedFlowSensorClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedFlowSensorDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedFlowSensorDeviceTypes)));
  case VirtualEndpointType::IlluminanceSensor:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedIlluminanceSensorEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedIlluminanceSensorClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedIlluminanceSensorDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedIlluminanceSensorDeviceTypes)));
  case VirtualEndpointType::WaterLeakDetector:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedWaterLeakEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedWaterLeakClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedWaterLeakDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedWaterLeakDeviceTypes)));
  case VirtualEndpointType::OnOffPlug:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedOnOffPlugEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedOnOffPlugClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedOnOffPlugDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedOnOffPlugDeviceTypes)));
  case VirtualEndpointType::DimmableLight:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedDimmableLightEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedDimmableLightClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedDimmableLightDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedDimmableLightDeviceTypes)));
  case VirtualEndpointType::ColorTemperatureLight:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedColorTempLightEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedColorTempLightClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedColorTempLightDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedColorTempLightDeviceTypes)));
  case VirtualEndpointType::WindowCovering:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedWindowCoveringEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedWindowCoveringClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedWindowCoveringDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedWindowCoveringDeviceTypes)));
  case VirtualEndpointType::DoorLock:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedDoorLockEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedDoorLockClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedDoorLockDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedDoorLockDeviceTypes)));
  case VirtualEndpointType::ExtendedColorLight:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedExtendedColorLightEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedExtendedColorLightClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedExtendedColorLightDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedExtendedColorLightDeviceTypes)));
  case VirtualEndpointType::DimmablePlug:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedDimmablePlugEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedDimmablePlugClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedDimmablePlugDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedDimmablePlugDeviceTypes)));
  case VirtualEndpointType::Thermostat:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedThermostatEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedThermostatClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedThermostatDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedThermostatDeviceTypes)));
  case VirtualEndpointType::Fan:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedFanEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedFanClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedFanDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedFanDeviceTypes)));
  case VirtualEndpointType::AirPurifier:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedAirPurifierEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedAirPurifierClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedAirPurifierDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedAirPurifierDeviceTypes)));
  case VirtualEndpointType::AirQualitySensor:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedAirQualityEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedAirQualityClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedAirQualityDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedAirQualityDeviceTypes)));
  case VirtualEndpointType::SmokeCoAlarm:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedSmokeCoAlarmEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedSmokeCoAlarmClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedSmokeCoAlarmDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedSmokeCoAlarmDeviceTypes)));
  case VirtualEndpointType::WaterValve:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedWaterValveEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedWaterValveClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedWaterValveDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedWaterValveDeviceTypes)));
  case VirtualEndpointType::WaterFreezeDetector:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedWaterFreezeEndpoint,
        chip::Span<DataVersion>(
            gEndpointDataVersions[endpoint->dynamicIndex],
            MATTER_ARRAY_SIZE(bridgedWaterFreezeClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedWaterFreezeDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedWaterFreezeDeviceTypes)));
  case VirtualEndpointType::RainSensor:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedRainSensorEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedRainSensorClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedRainSensorDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedRainSensorDeviceTypes)));
  case VirtualEndpointType::OnOffSensor:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedOnOffPlugEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedOnOffPlugClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedOnOffSensorDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedOnOffSensorDeviceTypes)));
  case VirtualEndpointType::MountedOnOffControl:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedOnOffPlugEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedOnOffPlugClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedMountedOnOffControlDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedMountedOnOffControlDeviceTypes)));
  case VirtualEndpointType::Pump:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedOnOffPlugEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedOnOffPlugClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedPumpDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedPumpDeviceTypes)));
  case VirtualEndpointType::Cooktop:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedOnOffPlugEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedOnOffPlugClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedCooktopDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedCooktopDeviceTypes)));
  case VirtualEndpointType::MountedDimmableLoadControl:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedDimmablePlugEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedDimmablePlugClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedMountedDimmableLoadControlDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedMountedDimmableLoadControlDeviceTypes)));
  case VirtualEndpointType::Speaker:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedDimmablePlugEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedDimmablePlugClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedSpeakerDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedSpeakerDeviceTypes)));
  case VirtualEndpointType::ExtractorHood:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedFanEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedFanClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedExtractorHoodDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedExtractorHoodDeviceTypes)));
  case VirtualEndpointType::RoomAirConditioner:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedThermostatEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedThermostatClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedRoomAirConditionerDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedRoomAirConditionerDeviceTypes)));
  case VirtualEndpointType::HeatingCoolingUnit:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedThermostatEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedThermostatClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedHeatingCoolingUnitDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedHeatingCoolingUnitDeviceTypes)));
  case VirtualEndpointType::HeatPump:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedThermostatEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedThermostatClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedHeatPumpDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedHeatPumpDeviceTypes)));
  case VirtualEndpointType::WaterHeater:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedThermostatEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedThermostatClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedWaterHeaterDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedWaterHeaterDeviceTypes)));
  case VirtualEndpointType::SoilSensor:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedHumiditySensorEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedHumiditySensorClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedSoilSensorDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedSoilSensorDeviceTypes)));
  case VirtualEndpointType::GenericSwitch:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedGenericSwitchDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedGenericSwitchDeviceTypes)));
  case VirtualEndpointType::ElectricalSensor:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedElectricalSensorDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedElectricalSensorDeviceTypes)));
  case VirtualEndpointType::DeviceEnergyManagement:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedDeviceEnergyManagementDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedDeviceEnergyManagementDeviceTypes)));
  case VirtualEndpointType::EnergyEvse:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedEnergyEvseDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedEnergyEvseDeviceTypes)));
  case VirtualEndpointType::SolarPower:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedSolarPowerDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedSolarPowerDeviceTypes)));
  case VirtualEndpointType::BatteryStorage:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedBatteryStorageDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedBatteryStorageDeviceTypes)));
  case VirtualEndpointType::ElectricalEnergyTariff:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedElectricalEnergyTariffDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedElectricalEnergyTariffDeviceTypes)));
  case VirtualEndpointType::RoboticVacuum:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedRoboticVacuumDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedRoboticVacuumDeviceTypes)));
  case VirtualEndpointType::Refrigerator:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedRefrigeratorDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedRefrigeratorDeviceTypes)));
  case VirtualEndpointType::TemperatureControlledCabinet:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedTemperatureControlledCabinetDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedTemperatureControlledCabinetDeviceTypes)));
  case VirtualEndpointType::LaundryWasher:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedLaundryWasherDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedLaundryWasherDeviceTypes)));
  case VirtualEndpointType::LaundryDryer:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedLaundryDryerDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedLaundryDryerDeviceTypes)));
  case VirtualEndpointType::Dishwasher:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedDishwasherDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedDishwasherDeviceTypes)));
  case VirtualEndpointType::Oven:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedOvenDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedOvenDeviceTypes)));
  case VirtualEndpointType::CookSurface:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedCookSurfaceDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedCookSurfaceDeviceTypes)));
  case VirtualEndpointType::MicrowaveOven:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedMicrowaveOvenDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedMicrowaveOvenDeviceTypes)));
  case VirtualEndpointType::BasicVideoPlayer:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedBasicVideoPlayerDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedBasicVideoPlayerDeviceTypes)));
  case VirtualEndpointType::CastingVideoPlayer:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedCastingVideoPlayerDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedCastingVideoPlayerDeviceTypes)));
  case VirtualEndpointType::ContentApp:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedContentAppDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedContentAppDeviceTypes)));
  case VirtualEndpointType::Closure:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedClosureDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedClosureDeviceTypes)));
  case VirtualEndpointType::ClosurePanel:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedClosurePanelDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedClosurePanelDeviceTypes)));
  case VirtualEndpointType::Camera:
    return SetMatterDynamicEndpoint(
        endpoint, &bridgedStubEndpoint,
        chip::Span<DataVersion>(gEndpointDataVersions[endpoint->dynamicIndex],
                                MATTER_ARRAY_SIZE(bridgedStubClusters)),
        chip::Span<const EmberAfDeviceType>(
            gBridgedCameraDeviceTypes,
            MATTER_ARRAY_SIZE(gBridgedCameraDeviceTypes)));
  }

  return CHIP_ERROR_INVALID_ARGUMENT;
}

void ClearMatterDynamicEndpoint(VirtualEndpoint *endpoint) {
  emberAfClearDynamicEndpoint(endpoint->dynamicIndex);
}

CHIP_ERROR InitVirtualEndpointInSlot(size_t slotIndex, VirtualEndpointType type,
                                     const char *name, int32_t value,
                                     bool hasValue,
                                     VirtualEndpoint **createdEndpoint) {
  VirtualEndpoint *endpoint = AllocateVirtualEndpointAt(slotIndex);
  if (endpoint == nullptr) {
    return CHIP_ERROR_NO_MEMORY;
  }

  endpoint->type = type;
  if (IsThermostatLikeType(type)) {
    endpoint->value2 = kDefaultHeatSetpoint;
    endpoint->value4 = kDefaultCoolSetpoint;
    endpoint->value3 = 4; /* heat */
  }
  CopyString(endpoint->name, sizeof(endpoint->name), name);
  endpoint->value = value;
  endpoint->hasValue = hasValue;

  CHIP_ERROR err = AddMatterDynamicEndpoint(endpoint);
  if (err != CHIP_NO_ERROR) {
    *endpoint = {};
    return err;
  }

  if (createdEndpoint != nullptr) {
    *createdEndpoint = endpoint;
  }

  return CHIP_NO_ERROR;
}

CHIP_ERROR CreateVirtualEndpoint(VirtualEndpointType type, const char *name,
                                 int32_t value, bool hasValue,
                                 VirtualEndpoint **createdEndpoint) {
  for (size_t index = 0; index < kMaxVirtualEndpoints; ++index) {
    if (!gVirtualEndpoints[index].allocated) {
      return InitVirtualEndpointInSlot(index, type, name, value, hasValue,
                                       createdEndpoint);
    }
  }

  return CHIP_ERROR_NO_MEMORY;
}

void FormatEndpointSettingsKey(size_t index, char *buffer, size_t bufferSize) {
  snprintf(buffer, bufferSize, "%s%u", kBridgeSettingsSlotPrefix,
           static_cast<unsigned int>(index));
}

bool LoadBridgeMeta() {
  StoredBridgeMeta meta = {};
  ssize_t bytesRead =
      settings_load_one(kBridgeSettingsMetaKey, &meta, sizeof(meta));

  if (bytesRead == -ENOENT || bytesRead == 0) {
    return false;
  }
  if (bytesRead != static_cast<ssize_t>(sizeof(meta)) ||
      meta.magic != kBridgeStorageMagic ||
      meta.version != kBridgeStorageVersion) {
    if (bytesRead < 0) {
      LOG_WRN("Failed to load bridge storage meta: %d",
              static_cast<int>(bytesRead));
    } else {
      LOG_WRN("Ignoring invalid bridge storage meta.");
    }
    return false;
  }

  return true;
}

bool LoadStoredEndpoint(size_t index, StoredVirtualEndpoint &storedEndpoint) {
  char key[SETTINGS_MAX_NAME_LEN + 1];
  FormatEndpointSettingsKey(index, key, sizeof(key));

  storedEndpoint = {};
  ssize_t bytesRead =
      settings_load_one(key, &storedEndpoint, sizeof(storedEndpoint));

  if (bytesRead == -ENOENT || bytesRead == 0) {
    return false;
  }
  if (bytesRead != static_cast<ssize_t>(sizeof(storedEndpoint)) ||
      storedEndpoint.magic != kBridgeStorageMagic ||
      storedEndpoint.version != kBridgeStorageVersion ||
      !IsKnownEndpointType(storedEndpoint.type)) {
    if (bytesRead < 0) {
      LOG_WRN("Failed to load bridge endpoint slot %u: %d",
              static_cast<unsigned int>(index), static_cast<int>(bytesRead));
    } else {
      LOG_WRN("Ignoring invalid bridge endpoint slot %u",
              static_cast<unsigned int>(index));
    }
    return false;
  }

  storedEndpoint.name[sizeof(storedEndpoint.name) - 1] = '\0';
  return true;
}

int SaveBridgeState() {
  StoredBridgeMeta meta = {
      .magic = kBridgeStorageMagic,
      .version = kBridgeStorageVersion,
      .reserved = 0,
  };

  int err = settings_save_one(kBridgeSettingsMetaKey, &meta, sizeof(meta));
  if (err != 0) {
    LOG_ERR("Failed to save bridge storage meta: %d", err);
    return err;
  }

  for (size_t index = 0; index < kMaxVirtualEndpoints; ++index) {
    char key[SETTINGS_MAX_NAME_LEN + 1];
    FormatEndpointSettingsKey(index, key, sizeof(key));

    const auto &endpoint = gVirtualEndpoints[index];
    if (!endpoint.allocated) {
      int deleteErr = settings_delete(key);
      if (deleteErr != 0 && deleteErr != -ENOENT) {
        LOG_WRN("Failed to delete bridge endpoint slot %u: %d",
                static_cast<unsigned int>(index), deleteErr);
      }
      continue;
    }

    StoredVirtualEndpoint stored = {
        .magic = kBridgeStorageMagic,
        .version = kBridgeStorageVersion,
        .type = static_cast<uint8_t>(endpoint.type),
        .hasValue = static_cast<uint8_t>(endpoint.hasValue ? 1 : 0),
        .reachable = static_cast<uint8_t>(endpoint.reachable ? 1 : 0),
        .reserved = 0,
        .value = endpoint.value,
        .value2 = endpoint.value2,
        .value3 = endpoint.value3,
        .value4 = endpoint.value4,
        .value5 = endpoint.value5,
        .name = {},
    };
    CopyString(stored.name, sizeof(stored.name), endpoint.name);

    err = settings_save_one(key, &stored, sizeof(stored));
    if (err != 0) {
      LOG_ERR("Failed to save bridge endpoint slot %u: %d",
              static_cast<unsigned int>(index), err);
      return err;
    }
  }

  return 0;
}

CHIP_ERROR RestorePersistedEndpoints(bool &storageFound,
                                     size_t &restoredCount) {
  storageFound = LoadBridgeMeta();
  restoredCount = 0;

  if (!storageFound) {
    return CHIP_NO_ERROR;
  }

  for (size_t index = 0; index < kMaxVirtualEndpoints; ++index) {
    StoredVirtualEndpoint stored = {};
    if (!LoadStoredEndpoint(index, stored)) {
      continue;
    }

    auto type = static_cast<VirtualEndpointType>(stored.type);
    const auto *definition = FindEndpointDefinition(type);
    VerifyOrReturnError(definition != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    const char *name =
        stored.name[0] != '\0' ? stored.name : definition->defaultName;

    VirtualEndpoint *endpoint = nullptr;
    ReturnErrorOnFailure(InitVirtualEndpointInSlot(
        index, type, name, stored.value, stored.hasValue != 0, &endpoint));
    endpoint->reachable = stored.reachable != 0;
    endpoint->value2 = stored.value2;
    endpoint->value3 = stored.value3;
    endpoint->value4 = stored.value4;
    endpoint->value5 = stored.value5;
    ++restoredCount;

    LOG_INF("Restored bridged %s endpoint EP%u name=%s value=%d",
            VirtualEndpointTypeName(endpoint->type), endpoint->endpointId,
            endpoint->name, endpoint->value);
  }

  return CHIP_NO_ERROR;
}


#if CONFIG_BRIDGE_XIAO_IMU
/* ---------------------------------------------------------------------------
 * XIAO nRF54L15 Sense: vibration / drop detection from the on-board LSM6DSO.
 *
 * The accelerometer is sampled periodically and the total acceleration is
 * compared against 1 g. Two things trigger the sensor:
 *   - shaking:  |‖a‖ - 1 g| exceeds the vibration threshold;
 *   - a drop:   free fall (‖a‖ near zero) followed by an impact spike.
 * The result is published through an ordinary bridged occupancy endpoint, so
 * every ecosystem that understands a motion sensor shows it.
 * ------------------------------------------------------------------------ */
constexpr char kImuEndpointName[] = "vibration";
constexpr int64_t kMilliGPerG = 1000;
/* 1 g expressed in milli-m/s^2, used to convert sensor readings to milli-g. */
constexpr int64_t kGravityMilliMs2 = 9807;

const struct device *const gImuDevice = DEVICE_DT_GET(DT_ALIAS(imu0));
/* Sampling runs on its own thread: the handler takes the Matter stack lock and
 * talks to the sensor over I2C, which is far too much for the shared system
 * workqueue stack. Overflowing that stack corrupts neighbouring memory and
 * shows up as a bus fault in an unrelated thread (MPSL). */
K_THREAD_STACK_DEFINE(gImuThreadStack, CONFIG_BRIDGE_XIAO_IMU_STACK_SIZE);
struct k_thread gImuThreadData;
uint16_t gImuEndpointId = 0;
uint8_t gFreeFallSamples = 0;
int64_t gImuClearAtUptimeMs = 0;
int32_t gImuLastMilliG = 0;
/* What we last published. Sampling runs 20x a second, but the Matter stack
 * lock is shared with the shell and the CHIP task, so it must only be taken
 * when the reported state actually changes - holding it every tick starves
 * everything else and makes the bridge look offline. */
int8_t gImuReported = -1;

int64_t SensorValueToMilliMs2(const struct sensor_value &value) {
  return static_cast<int64_t>(value.val1) * 1000 + value.val2 / 1000;
}

/* Newton's method, stopped the way integer arithmetic requires.
 *
 * The obvious `while (guess != previous)` does not terminate: near the root the
 * integer sequence stops converging and oscillates between two adjacent values,
 * so the two are never equal and the loop spins forever. Since this runs in the
 * IMU sampling thread, that thread then never sleeps again - it just burns the
 * CPU in 64-bit division, and everything at a lower priority (the shell, hence
 * the whole ESP link) is starved. Matter and Thread sit above it and keep
 * working, which is exactly what "HomeKit is fine but the bridge stopped
 * answering" looks like from outside.
 *
 * The sequence is monotonically decreasing until it starts to oscillate, so
 * stopping as soon as it fails to decrease is both correct and guaranteed to
 * finish. */
int64_t IntegerSqrt(int64_t value) {
  if (value <= 0) {
    return 0;
  }

  int64_t guess = value;
  int64_t previous;
  do {
    previous = guess;
    guess = (guess + value / guess) / 2;
  } while (guess < previous);
  return previous;
}

bool ReadAccelerationMilliG(int32_t &milliG) {
  struct sensor_value accel[3];

  if (sensor_sample_fetch(gImuDevice) < 0 ||
      sensor_channel_get(gImuDevice, SENSOR_CHAN_ACCEL_XYZ, accel) < 0) {
    return false;
  }

  const int64_t x = SensorValueToMilliMs2(accel[0]);
  const int64_t y = SensorValueToMilliMs2(accel[1]);
  const int64_t z = SensorValueToMilliMs2(accel[2]);
  const int64_t magnitude = IntegerSqrt(x * x + y * y + z * z);

  milliG = static_cast<int32_t>(magnitude * kMilliGPerG / kGravityMilliMs2);
  return true;
}

void SetImuEndpointValue(int32_t value) {
  VirtualEndpoint *endpoint = FindVirtualEndpoint(gImuEndpointId);
  if (endpoint == nullptr || endpoint->value == value) {
    return;
  }

  endpoint->value = value;
  endpoint->hasValue = true;
  NotifyEndpointValueChanged(*endpoint);
  /* Deliberately not persisted: the value is transient and a flash write on
   * every shake would wear the storage out and stall this thread. */
}

void ImuSampleOnce() {
  int32_t milliG = 0;
  if (ReadAccelerationMilliG(milliG)) {
    gImuLastMilliG = milliG;

    const int32_t deviation =
        milliG > kMilliGPerG ? milliG - kMilliGPerG : kMilliGPerG - milliG;
    bool triggered = deviation > CONFIG_BRIDGE_XIAO_IMU_VIBRATION_MG;

    /* Free fall arms the drop detector; the following impact confirms it. */
    if (milliG < CONFIG_BRIDGE_XIAO_IMU_FREEFALL_MG) {
      if (gFreeFallSamples < UINT8_MAX) {
        ++gFreeFallSamples;
      }
    } else {
      if (gFreeFallSamples >= 2 && milliG > CONFIG_BRIDGE_XIAO_IMU_IMPACT_MG) {
        LOG_WRN("IMU: drop detected (impact %d mg)", milliG);
        triggered = true;
      }
      gFreeFallSamples = 0;
    }

    if (triggered) {
      gImuClearAtUptimeMs =
          k_uptime_get() + CONFIG_BRIDGE_XIAO_IMU_HOLD_S * MSEC_PER_SEC;
    } else if (gImuClearAtUptimeMs != 0 &&
               k_uptime_get() >= gImuClearAtUptimeMs) {
      gImuClearAtUptimeMs = 0;
    }

    const int8_t want = gImuClearAtUptimeMs != 0 ? 1 : 0;
    if (want == gImuReported) {
      return; /* nothing changed - do not touch the Matter stack at all */
    }

    ChipStackLock lock;
    gImuReported = want;
    SetImuEndpointValue(want);
  }
}

void ImuThreadEntry(void *unused1, void *unused2, void *unused3) {
  ARG_UNUSED(unused1);
  ARG_UNUSED(unused2);
  ARG_UNUSED(unused3);

  while (true) {
    k_sleep(K_MSEC(CONFIG_BRIDGE_XIAO_IMU_PERIOD_MS));
    ImuSampleOnce();
  }
}

/* Brings up the board LED and gives it a bridged On/Off endpoint. */
CHIP_ERROR StartOnboardLed() {
#ifdef BRIDGE_HAS_ONBOARD_LED
  if (!gpio_is_ready_dt(&gOnboardLed)) {
    LOG_ERR("Onboard LED %s is not ready", gOnboardLed.port->name);
    return CHIP_NO_ERROR; /* the bridge is still perfectly useful without it */
  }
  if (gpio_pin_configure_dt(&gOnboardLed, GPIO_OUTPUT_INACTIVE) < 0) {
    LOG_ERR("Failed to configure the onboard LED");
    return CHIP_NO_ERROR;
  }

  VirtualEndpoint *endpoint = nullptr;
  for (auto &candidate : gVirtualEndpoints) {
    if (candidate.allocated && strcmp(candidate.name, kOnboardLedName) == 0) {
      endpoint = &candidate;
      break;
    }
  }

  if (endpoint == nullptr) {
    CHIP_ERROR err = CreateVirtualEndpoint(VirtualEndpointType::OnOffLight,
                                           kOnboardLedName, 0, true, &endpoint);
    if (err != CHIP_NO_ERROR) {
      LOG_ERR("Failed to create the onboard LED endpoint: %s", ErrorStr(err));
      return CHIP_NO_ERROR;
    }
    int saveErr = SaveBridgeState();
    if (saveErr != 0) {
      LOG_WRN("Onboard LED endpoint created but not persisted: %d", saveErr);
    }
  }

  gOnboardLedEndpointId = endpoint->endpointId;
  /* Restored state may say "on", so match the hardware to it right away. */
  (void)gpio_pin_set_dt(&gOnboardLed, endpoint->value != 0 ? 1 : 0);
  LOG_INF("Onboard LED on EP%u", gOnboardLedEndpointId);
#endif
  return CHIP_NO_ERROR;
}

CHIP_ERROR StartImuVibrationSensor() {
  /* The node is marked `zephyr,deferred-init`, so nothing has touched the part
   * yet - bring it up here, where the power rail has had seconds to settle
   * rather than the few milliseconds the boot sequence allows. Letting the
   * automatic init run instead failed its software reset ("failed to reboot
   * device") and left the device permanently unready: device_is_ready() reports
   * the result of that single attempt and never re-probes. */
  if (!device_is_ready(gImuDevice)) {
    const int err = device_init(gImuDevice);
    if (err < 0 && err != -EALREADY) {
      LOG_ERR("IMU %s failed to initialise: %d", gImuDevice->name, err);
    }
  }

  if (!device_is_ready(gImuDevice)) {
    LOG_ERR("IMU %s is not ready, vibration sensor disabled",
            gImuDevice->name);
    return CHIP_NO_ERROR; /* keep the bridge running without the sensor */
  }

  /* The driver powers the accelerometer down until an output data rate is
   * selected, so ask for one explicitly instead of relying on Kconfig. */
  struct sensor_value odr = {.val1 = 104, .val2 = 0};
  if (sensor_attr_set(gImuDevice, SENSOR_CHAN_ACCEL_XYZ,
                      SENSOR_ATTR_SAMPLING_FREQUENCY, &odr) < 0) {
    /* Not all drivers accept a runtime ODR; CONFIG_LSM6DSL_ACCEL_ODR already
     * takes the accelerometer out of power-down, so this is not a problem. */
    LOG_DBG("IMU keeps its build-time sampling rate");
  }

  VirtualEndpoint *endpoint = nullptr;
  for (auto &candidate : gVirtualEndpoints) {
    if (candidate.allocated &&
        strcmp(candidate.name, kImuEndpointName) == 0) {
      endpoint = &candidate;
      break;
    }
  }

#if CONFIG_BRIDGE_XIAO_IMU_AUTO_ENDPOINT
  if (endpoint == nullptr) {
    CHIP_ERROR err =
        CreateVirtualEndpoint(VirtualEndpointType::OccupancySensor,
                              kImuEndpointName, 0, true, &endpoint);
    if (err != CHIP_NO_ERROR) {
      LOG_ERR("Failed to create the IMU vibration endpoint: %s",
              ErrorStr(err));
      return CHIP_NO_ERROR;
    }
    int saveErr = SaveBridgeState();
    if (saveErr != 0) {
      LOG_WRN("IMU endpoint created but not persisted: %d", saveErr);
    }
  }
#endif

  if (endpoint == nullptr) {
    LOG_INF("IMU sampling is on, but no '%s' endpoint exists yet. "
            "Create one with: bridge add occupancy %s",
            kImuEndpointName, kImuEndpointName);
    return CHIP_NO_ERROR;
  }

  gImuEndpointId = endpoint->endpointId;
  LOG_INF("IMU vibration sensor active on EP%u (threshold %d mg)",
          gImuEndpointId, CONFIG_BRIDGE_XIAO_IMU_VIBRATION_MG);

  k_thread_create(&gImuThreadData, gImuThreadStack,
                  K_THREAD_STACK_SIZEOF(gImuThreadStack), ImuThreadEntry,
                  nullptr, nullptr, nullptr, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
  k_thread_name_set(&gImuThreadData, "imu");
  return CHIP_NO_ERROR;
}
#endif /* CONFIG_BRIDGE_XIAO_IMU */

CHIP_ERROR RestoreBridgeStateOrAddDefault() {
  bool storageFound = false;
  size_t restoredCount = 0;
  ReturnErrorOnFailure(RestorePersistedEndpoints(storageFound, restoredCount));

  if (storageFound) {
    LOG_INF("Bridge restored %u persisted virtual endpoint(s).",
            static_cast<unsigned int>(restoredCount));
    return CHIP_NO_ERROR;
  }

  /* Start empty: devices are added explicitly with `bridge add`. */
  LOG_INF("Bridge started with no virtual endpoints. "
          "Use 'bridge types' and 'bridge add <type> <name>'.");

  return CHIP_NO_ERROR;
}

class BridgeOnOffCommandHandler : public CommandHandlerInterface {
public:
  BridgeOnOffCommandHandler()
      : CommandHandlerInterface(Optional<EndpointId>::Missing(),
                                Clusters::OnOff::Id) {}

  void InvokeCommand(HandlerContext &handlerContext) override {
    VirtualEndpoint *endpoint =
        FindVirtualEndpoint(handlerContext.mRequestPath.mEndpointId);
    if (endpoint == nullptr ||
        (endpoint->type != VirtualEndpointType::OnOffLight &&
         endpoint->type != VirtualEndpointType::OnOffPlug &&
         endpoint->type != VirtualEndpointType::DimmableLight &&
         endpoint->type != VirtualEndpointType::ColorTemperatureLight &&
         endpoint->type != VirtualEndpointType::ExtendedColorLight &&
         endpoint->type != VirtualEndpointType::DimmablePlug &&
         !IsOnOffControllableType(endpoint->type))) {
      return;
    }

    bool handled = true;
    switch (handlerContext.mRequestPath.mCommandId) {
    case Clusters::OnOff::Commands::Off::Id:
    case Clusters::OnOff::Commands::OffWithEffect::Id:
      endpoint->value = 0;
      break;
    case Clusters::OnOff::Commands::On::Id:
    case Clusters::OnOff::Commands::OnWithRecallGlobalScene::Id:
    case Clusters::OnOff::Commands::OnWithTimedOff::Id:
      endpoint->value = 1;
      break;
    case Clusters::OnOff::Commands::Toggle::Id:
      endpoint->value = (endpoint->hasValue && endpoint->value != 0) ? 0 : 1;
      break;
    default:
      handled = false;
      break;
    }

    if (!handled) {
      return;
    }

    endpoint->hasValue = true;
    NotifyEndpointValueChanged(*endpoint);
    int saveErr = SaveBridgeState();
    if (saveErr != 0) {
      LOG_WRN("On/Off command applied but bridge state save failed: %d",
              saveErr);
    }

    handlerContext.SetCommandHandled();
    handlerContext.mCommandHandler.AddStatus(
        handlerContext.mRequestPath,
        Protocols::InteractionModel::Status::Success);
  }
};

class BridgeLevelControlCommandHandler : public CommandHandlerInterface {
public:
  BridgeLevelControlCommandHandler()
      : CommandHandlerInterface(Optional<EndpointId>::Missing(),
                                Clusters::LevelControl::Id) {}

  void InvokeCommand(HandlerContext &handlerContext) override {
    VirtualEndpoint *endpoint =
        FindVirtualEndpoint(handlerContext.mRequestPath.mEndpointId);
    if (endpoint == nullptr ||
        (endpoint->type != VirtualEndpointType::DimmableLight &&
         endpoint->type != VirtualEndpointType::ColorTemperatureLight &&
         endpoint->type != VirtualEndpointType::ExtendedColorLight &&
         endpoint->type != VirtualEndpointType::DimmablePlug &&
         !IsLevelControllableType(endpoint->type))) {
      return;
    }

    bool turnOn = false;
    int32_t newLevel = endpoint->value2;

    switch (handlerContext.mRequestPath.mCommandId) {
    case Clusters::LevelControl::Commands::MoveToLevel::Id:
    case Clusters::LevelControl::Commands::MoveToLevelWithOnOff::Id: {
      Clusters::LevelControl::Commands::MoveToLevel::DecodableType data;
      if (DataModel::Decode(handlerContext.mPayload, data) != CHIP_NO_ERROR) {
        return;
      }
      newLevel = data.level;
      turnOn = handlerContext.mRequestPath.mCommandId ==
               Clusters::LevelControl::Commands::MoveToLevelWithOnOff::Id;
      break;
    }
    case Clusters::LevelControl::Commands::Move::Id:
    case Clusters::LevelControl::Commands::MoveWithOnOff::Id:
    case Clusters::LevelControl::Commands::Step::Id:
    case Clusters::LevelControl::Commands::StepWithOnOff::Id:
    case Clusters::LevelControl::Commands::Stop::Id:
    case Clusters::LevelControl::Commands::StopWithOnOff::Id:
      /* Virtual devices move instantly, so the ramping commands only need to
       * be acknowledged; the current level stays as it is. */
      break;
    default:
      return;
    }

    if (newLevel < 1) {
      newLevel = 1;
    } else if (newLevel > 254) {
      newLevel = 254;
    }
    endpoint->value2 = newLevel;

    if (turnOn) {
      endpoint->value = 1;
      endpoint->hasValue = true;
      NotifyEndpointValueChanged(*endpoint);
    }

    MatterReportingAttributeChangeCallback(
        endpoint->endpointId, Clusters::LevelControl::Id,
        Clusters::LevelControl::Attributes::CurrentLevel::Id);

    int saveErr = SaveBridgeState();
    if (saveErr != 0) {
      LOG_WRN("Level command applied but bridge state save failed: %d",
              saveErr);
    }

    handlerContext.SetCommandHandled();
    handlerContext.mCommandHandler.AddStatus(
        handlerContext.mRequestPath,
        Protocols::InteractionModel::Status::Success);
  }
};

void ApplyColorChange(const VirtualEndpoint &endpoint,
                      chip::AttributeId attributeId) {
  MatterReportingAttributeChangeCallback(endpoint.endpointId,
                                         Clusters::ColorControl::Id,
                                         attributeId);
  int saveErr = SaveBridgeState();
  if (saveErr != 0) {
    LOG_WRN("Color update applied but bridge state save failed: %d", saveErr);
  }
}

class BridgeColorControlCommandHandler : public CommandHandlerInterface {
public:
  BridgeColorControlCommandHandler()
      : CommandHandlerInterface(Optional<EndpointId>::Missing(),
                                Clusters::ColorControl::Id) {}

  void InvokeCommand(HandlerContext &handlerContext) override {
    VirtualEndpoint *endpoint =
        FindVirtualEndpoint(handlerContext.mRequestPath.mEndpointId);
    if (endpoint == nullptr ||
        (endpoint->type != VirtualEndpointType::ColorTemperatureLight &&
         endpoint->type != VirtualEndpointType::ExtendedColorLight)) {
      return;
    }

    int32_t mireds = endpoint->value3;

    switch (handlerContext.mRequestPath.mCommandId) {
    case Clusters::ColorControl::Commands::MoveToHue::Id: {
      Clusters::ColorControl::Commands::MoveToHue::DecodableType data;
      if (DataModel::Decode(handlerContext.mPayload, data) != CHIP_NO_ERROR) {
        return;
      }
      endpoint->value4 = data.hue;
      ApplyColorChange(*endpoint,
                       Clusters::ColorControl::Attributes::CurrentHue::Id);
      handlerContext.SetCommandHandled();
      handlerContext.mCommandHandler.AddStatus(
          handlerContext.mRequestPath,
          Protocols::InteractionModel::Status::Success);
      return;
    }
    case Clusters::ColorControl::Commands::MoveToSaturation::Id: {
      Clusters::ColorControl::Commands::MoveToSaturation::DecodableType data;
      if (DataModel::Decode(handlerContext.mPayload, data) != CHIP_NO_ERROR) {
        return;
      }
      endpoint->value5 = data.saturation;
      ApplyColorChange(
          *endpoint, Clusters::ColorControl::Attributes::CurrentSaturation::Id);
      handlerContext.SetCommandHandled();
      handlerContext.mCommandHandler.AddStatus(
          handlerContext.mRequestPath,
          Protocols::InteractionModel::Status::Success);
      return;
    }
    case Clusters::ColorControl::Commands::MoveToHueAndSaturation::Id: {
      Clusters::ColorControl::Commands::MoveToHueAndSaturation::DecodableType
          data;
      if (DataModel::Decode(handlerContext.mPayload, data) != CHIP_NO_ERROR) {
        return;
      }
      endpoint->value4 = data.hue;
      endpoint->value5 = data.saturation;
      ApplyColorChange(*endpoint,
                       Clusters::ColorControl::Attributes::CurrentHue::Id);
      ApplyColorChange(
          *endpoint, Clusters::ColorControl::Attributes::CurrentSaturation::Id);
      handlerContext.SetCommandHandled();
      handlerContext.mCommandHandler.AddStatus(
          handlerContext.mRequestPath,
          Protocols::InteractionModel::Status::Success);
      return;
    }
    case Clusters::ColorControl::Commands::MoveToColorTemperature::Id: {
      Clusters::ColorControl::Commands::MoveToColorTemperature::DecodableType
          data;
      if (DataModel::Decode(handlerContext.mPayload, data) != CHIP_NO_ERROR) {
        return;
      }
      mireds = data.colorTemperatureMireds;
      break;
    }
    case Clusters::ColorControl::Commands::MoveColorTemperature::Id:
    case Clusters::ColorControl::Commands::StepColorTemperature::Id:
      /* Instant devices: acknowledge without ramping. */
      break;
    default:
      return;
    }

    if (mireds < kMinColorTempMireds) {
      mireds = kMinColorTempMireds;
    } else if (mireds > kMaxColorTempMireds) {
      mireds = kMaxColorTempMireds;
    }
    endpoint->value3 = mireds;

    MatterReportingAttributeChangeCallback(
        endpoint->endpointId, Clusters::ColorControl::Id,
        Clusters::ColorControl::Attributes::ColorTemperatureMireds::Id);

    int saveErr = SaveBridgeState();
    if (saveErr != 0) {
      LOG_WRN("Color command applied but bridge state save failed: %d",
              saveErr);
    }

    handlerContext.SetCommandHandled();
    handlerContext.mCommandHandler.AddStatus(
        handlerContext.mRequestPath,
        Protocols::InteractionModel::Status::Success);
  }
};

class BridgeWindowCoveringCommandHandler : public CommandHandlerInterface {
public:
  BridgeWindowCoveringCommandHandler()
      : CommandHandlerInterface(Optional<EndpointId>::Missing(),
                                Clusters::WindowCovering::Id) {}

  void InvokeCommand(HandlerContext &handlerContext) override {
    VirtualEndpoint *endpoint =
        FindVirtualEndpoint(handlerContext.mRequestPath.mEndpointId);
    if (endpoint == nullptr ||
        endpoint->type != VirtualEndpointType::WindowCovering) {
      return;
    }

    int32_t position = endpoint->hasValue ? endpoint->value : 0;

    switch (handlerContext.mRequestPath.mCommandId) {
    case Clusters::WindowCovering::Commands::UpOrOpen::Id:
      position = 0;
      break;
    case Clusters::WindowCovering::Commands::DownOrClose::Id:
      position = 100;
      break;
    case Clusters::WindowCovering::Commands::StopMotion::Id:
      break;
    case Clusters::WindowCovering::Commands::GoToLiftPercentage::Id: {
      Clusters::WindowCovering::Commands::GoToLiftPercentage::DecodableType
          data;
      if (DataModel::Decode(handlerContext.mPayload, data) != CHIP_NO_ERROR) {
        return;
      }
      position = static_cast<int32_t>(data.liftPercent100thsValue / 100);
      break;
    }
    default:
      return;
    }

    if (position < 0) {
      position = 0;
    } else if (position > 100) {
      position = 100;
    }
    endpoint->value = position;
    endpoint->hasValue = true;

    NotifyEndpointValueChanged(*endpoint);
    MatterReportingAttributeChangeCallback(
        endpoint->endpointId, Clusters::WindowCovering::Id,
        Clusters::WindowCovering::Attributes::
            TargetPositionLiftPercent100ths::Id);

    int saveErr = SaveBridgeState();
    if (saveErr != 0) {
      LOG_WRN("Covering command applied but bridge state save failed: %d",
              saveErr);
    }

    handlerContext.SetCommandHandled();
    handlerContext.mCommandHandler.AddStatus(
        handlerContext.mRequestPath,
        Protocols::InteractionModel::Status::Success);
  }
};

class BridgeDoorLockCommandHandler : public CommandHandlerInterface {
public:
  BridgeDoorLockCommandHandler()
      : CommandHandlerInterface(Optional<EndpointId>::Missing(),
                                Clusters::DoorLock::Id) {}

  void InvokeCommand(HandlerContext &handlerContext) override {
    VirtualEndpoint *endpoint =
        FindVirtualEndpoint(handlerContext.mRequestPath.mEndpointId);
    if (endpoint == nullptr ||
        endpoint->type != VirtualEndpointType::DoorLock) {
      return;
    }

    switch (handlerContext.mRequestPath.mCommandId) {
    case Clusters::DoorLock::Commands::LockDoor::Id:
      endpoint->value = 1;
      break;
    case Clusters::DoorLock::Commands::UnlockDoor::Id:
      endpoint->value = 0;
      break;
    default:
      return;
    }

    endpoint->hasValue = true;
    NotifyEndpointValueChanged(*endpoint);

    int saveErr = SaveBridgeState();
    if (saveErr != 0) {
      LOG_WRN("Lock command applied but bridge state save failed: %d", saveErr);
    }

    handlerContext.SetCommandHandled();
    handlerContext.mCommandHandler.AddStatus(
        handlerContext.mRequestPath,
        Protocols::InteractionModel::Status::Success);
  }
};

class BridgeThermostatCommandHandler : public CommandHandlerInterface {
public:
  BridgeThermostatCommandHandler()
      : CommandHandlerInterface(Optional<EndpointId>::Missing(),
                                Clusters::Thermostat::Id) {}

  void InvokeCommand(HandlerContext &handlerContext) override {
    VirtualEndpoint *endpoint =
        FindVirtualEndpoint(handlerContext.mRequestPath.mEndpointId);
    if (endpoint == nullptr ||
        !IsThermostatLikeType(endpoint->type)) {
      return;
    }

    if (handlerContext.mRequestPath.mCommandId !=
        Clusters::Thermostat::Commands::SetpointRaiseLower::Id) {
      return;
    }

    Clusters::Thermostat::Commands::SetpointRaiseLower::DecodableType data;
    if (DataModel::Decode(handlerContext.mPayload, data) != CHIP_NO_ERROR) {
      return;
    }

    /* amount is in 0.1 C steps, our setpoints are centi-C */
    const int32_t delta = static_cast<int32_t>(data.amount) * 10;
    const auto mode = static_cast<uint8_t>(data.mode);

    if (mode == 0 || mode == 2) { /* heat or both */
      endpoint->value2 += delta;
      MatterReportingAttributeChangeCallback(
          endpoint->endpointId, Clusters::Thermostat::Id,
          Clusters::Thermostat::Attributes::OccupiedHeatingSetpoint::Id);
    }
    if (mode == 1 || mode == 2) { /* cool or both */
      endpoint->value4 += delta;
      MatterReportingAttributeChangeCallback(
          endpoint->endpointId, Clusters::Thermostat::Id,
          Clusters::Thermostat::Attributes::OccupiedCoolingSetpoint::Id);
    }

    int saveErr = SaveBridgeState();
    if (saveErr != 0) {
      LOG_WRN("Setpoint applied but bridge state save failed: %d", saveErr);
    }

    handlerContext.SetCommandHandled();
    handlerContext.mCommandHandler.AddStatus(
        handlerContext.mRequestPath,
        Protocols::InteractionModel::Status::Success);
  }
};

class BridgeValveCommandHandler : public CommandHandlerInterface {
public:
  BridgeValveCommandHandler()
      : CommandHandlerInterface(Optional<EndpointId>::Missing(),
                                Clusters::ValveConfigurationAndControl::Id) {}

  void InvokeCommand(HandlerContext &handlerContext) override {
    VirtualEndpoint *endpoint =
        FindVirtualEndpoint(handlerContext.mRequestPath.mEndpointId);
    if (endpoint == nullptr ||
        endpoint->type != VirtualEndpointType::WaterValve) {
      return;
    }

    switch (handlerContext.mRequestPath.mCommandId) {
    case Clusters::ValveConfigurationAndControl::Commands::Open::Id:
      endpoint->value = 1;
      break;
    case Clusters::ValveConfigurationAndControl::Commands::Close::Id:
      endpoint->value = 0;
      break;
    default:
      return;
    }

    endpoint->hasValue = true;
    NotifyEndpointValueChanged(*endpoint);
    MatterReportingAttributeChangeCallback(
        endpoint->endpointId, Clusters::ValveConfigurationAndControl::Id,
        Clusters::ValveConfigurationAndControl::Attributes::TargetState::Id);

    int saveErr = SaveBridgeState();
    if (saveErr != 0) {
      LOG_WRN("Valve command applied but bridge state save failed: %d",
              saveErr);
    }

    handlerContext.SetCommandHandled();
    handlerContext.mCommandHandler.AddStatus(
        handlerContext.mRequestPath,
        Protocols::InteractionModel::Status::Success);
  }
};

BridgeThermostatCommandHandler gThermostatCommandHandler;
BridgeValveCommandHandler gValveCommandHandler;
BridgeLevelControlCommandHandler gLevelControlCommandHandler;
BridgeColorControlCommandHandler gColorControlCommandHandler;
BridgeWindowCoveringCommandHandler gWindowCoveringCommandHandler;
BridgeDoorLockCommandHandler gDoorLockCommandHandler;
BridgeOnOffCommandHandler gOnOffCommandHandler;

CHIP_ERROR RegisterBridgeCommandHandlers() {
  static bool registered = false;

  if (registered) {
    return CHIP_NO_ERROR;
  }

  ReturnErrorOnFailure(
      CommandHandlerInterfaceRegistry::Instance().RegisterCommandHandler(
          &gOnOffCommandHandler));
  ReturnErrorOnFailure(
      CommandHandlerInterfaceRegistry::Instance().RegisterCommandHandler(
          &gLevelControlCommandHandler));
  ReturnErrorOnFailure(
      CommandHandlerInterfaceRegistry::Instance().RegisterCommandHandler(
          &gColorControlCommandHandler));
  ReturnErrorOnFailure(
      CommandHandlerInterfaceRegistry::Instance().RegisterCommandHandler(
          &gWindowCoveringCommandHandler));
  ReturnErrorOnFailure(
      CommandHandlerInterfaceRegistry::Instance().RegisterCommandHandler(
          &gDoorLockCommandHandler));
  ReturnErrorOnFailure(
      CommandHandlerInterfaceRegistry::Instance().RegisterCommandHandler(
          &gThermostatCommandHandler));
  ReturnErrorOnFailure(
      CommandHandlerInterfaceRegistry::Instance().RegisterCommandHandler(
          &gValveCommandHandler));
  registered = true;
  return CHIP_NO_ERROR;
}

} // namespace

Protocols::InteractionModel::Status emberAfExternalAttributeReadCallback(
    EndpointId endpoint, ClusterId clusterId,
    const EmberAfAttributeMetadata *attributeMetadata, uint8_t *buffer,
    uint16_t maxReadLength) {
  uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);
  if (endpointIndex >= kMaxVirtualEndpoints || attributeMetadata == nullptr) {
    return Protocols::InteractionModel::Status::Failure;
  }

  VirtualEndpoint *endpointObj = &gVirtualEndpoints[endpointIndex];
  if (!endpointObj->allocated) {
    return Protocols::InteractionModel::Status::Failure;
  }

  if (clusterId == Clusters::BridgedDeviceBasicInformation::Id) {
    return HandleReadBridgedDeviceBasicAttribute(
        *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
  }

  switch (endpointObj->type) {
  case VirtualEndpointType::TemperatureSensor:
    if (clusterId == Clusters::TemperatureMeasurement::Id) {
      return HandleReadTempMeasurementAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::HumiditySensor:
    if (clusterId == Clusters::RelativeHumidityMeasurement::Id) {
      return HandleReadHumidityMeasurementAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::OnOffLight:
    if (clusterId == Clusters::OnOff::Id) {
      return HandleReadOnOffAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::ContactSensor:
    if (clusterId == Clusters::BooleanState::Id) {
      return HandleReadBooleanStateAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::OccupancySensor:
    if (clusterId == Clusters::OccupancySensing::Id) {
      return HandleReadOccupancySensingAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::PressureSensor:
    if (clusterId == Clusters::PressureMeasurement::Id) {
      return HandleReadPressureMeasurementAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::FlowSensor:
    if (clusterId == Clusters::FlowMeasurement::Id) {
      return HandleReadUnsignedMeasurementAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength,
          ZCL_FLOW_MEASUREMENT_FEATURE_MAP,
          ZCL_FLOW_MEASUREMENT_CLUSTER_REVISION);
    }
    break;
  case VirtualEndpointType::IlluminanceSensor:
    if (clusterId == Clusters::IlluminanceMeasurement::Id) {
      return HandleReadUnsignedMeasurementAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength,
          ZCL_ILLUMINANCE_MEASUREMENT_FEATURE_MAP,
          ZCL_ILLUMINANCE_MEASUREMENT_CLUSTER_REVISION);
    }
    break;
  case VirtualEndpointType::WaterLeakDetector:
    if (clusterId == Clusters::BooleanState::Id) {
      return HandleReadBooleanStateAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::OnOffPlug:
    if (clusterId == Clusters::OnOff::Id) {
      return HandleReadOnOffAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::DimmableLight:
    if (clusterId == Clusters::OnOff::Id) {
      return HandleReadOnOffAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    if (clusterId == Clusters::LevelControl::Id) {
      return HandleReadLevelControlAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::ColorTemperatureLight:
    if (clusterId == Clusters::OnOff::Id) {
      return HandleReadOnOffAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    if (clusterId == Clusters::LevelControl::Id) {
      return HandleReadLevelControlAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    if (clusterId == Clusters::ColorControl::Id) {
      return HandleReadColorControlAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::WindowCovering:
    if (clusterId == Clusters::WindowCovering::Id) {
      return HandleReadWindowCoveringAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::DoorLock:
    if (clusterId == Clusters::DoorLock::Id) {
      return HandleReadDoorLockAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::ExtendedColorLight:
    if (clusterId == Clusters::OnOff::Id) {
      return HandleReadOnOffAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    if (clusterId == Clusters::LevelControl::Id) {
      return HandleReadLevelControlAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    if (clusterId == Clusters::ColorControl::Id) {
      return HandleReadColorControlFullAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::DimmablePlug:
    if (clusterId == Clusters::OnOff::Id) {
      return HandleReadOnOffAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    if (clusterId == Clusters::LevelControl::Id) {
      return HandleReadLevelControlAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::Thermostat:
    if (clusterId == Clusters::Thermostat::Id) {
      return HandleReadThermostatAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::Fan:
  case VirtualEndpointType::AirPurifier:
    if (clusterId == Clusters::FanControl::Id) {
      return HandleReadFanControlAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::AirQualitySensor:
    if (clusterId == Clusters::AirQuality::Id) {
      return HandleReadAirQualityAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::SmokeCoAlarm:
    if (clusterId == Clusters::SmokeCoAlarm::Id) {
      return HandleReadSmokeCoAlarmAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::WaterValve:
    if (clusterId == Clusters::ValveConfigurationAndControl::Id) {
      return HandleReadValveAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::WaterFreezeDetector:
  case VirtualEndpointType::RainSensor:
    if (clusterId == Clusters::BooleanState::Id) {
      return HandleReadBooleanStateAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::OnOffSensor:
  case VirtualEndpointType::MountedOnOffControl:
  case VirtualEndpointType::Pump:
  case VirtualEndpointType::Cooktop:
    if (clusterId == Clusters::OnOff::Id) {
      return HandleReadOnOffAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::MountedDimmableLoadControl:
  case VirtualEndpointType::Speaker:
    if (clusterId == Clusters::OnOff::Id) {
      return HandleReadOnOffAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    if (clusterId == Clusters::LevelControl::Id) {
      return HandleReadLevelControlAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::ExtractorHood:
    if (clusterId == Clusters::FanControl::Id) {
      return HandleReadFanControlAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::RoomAirConditioner:
  case VirtualEndpointType::HeatingCoolingUnit:
  case VirtualEndpointType::HeatPump:
  case VirtualEndpointType::WaterHeater:
    if (clusterId == Clusters::Thermostat::Id) {
      return HandleReadThermostatAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::SoilSensor:
    if (clusterId == Clusters::RelativeHumidityMeasurement::Id) {
      return HandleReadHumidityMeasurementAttribute(
          *endpointObj, attributeMetadata->attributeId, buffer, maxReadLength);
    }
    break;
  case VirtualEndpointType::GenericSwitch:
  case VirtualEndpointType::ElectricalSensor:
  case VirtualEndpointType::DeviceEnergyManagement:
  case VirtualEndpointType::EnergyEvse:
  case VirtualEndpointType::SolarPower:
  case VirtualEndpointType::BatteryStorage:
  case VirtualEndpointType::ElectricalEnergyTariff:
  case VirtualEndpointType::RoboticVacuum:
  case VirtualEndpointType::Refrigerator:
  case VirtualEndpointType::TemperatureControlledCabinet:
  case VirtualEndpointType::LaundryWasher:
  case VirtualEndpointType::LaundryDryer:
  case VirtualEndpointType::Dishwasher:
  case VirtualEndpointType::Oven:
  case VirtualEndpointType::CookSurface:
  case VirtualEndpointType::MicrowaveOven:
  case VirtualEndpointType::BasicVideoPlayer:
  case VirtualEndpointType::CastingVideoPlayer:
  case VirtualEndpointType::ContentApp:
  case VirtualEndpointType::Closure:
  case VirtualEndpointType::ClosurePanel:
  case VirtualEndpointType::Camera:
    /* STUB types expose no functional clusters. */
    break;
  }

  return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status emberAfExternalAttributeWriteCallback(
    EndpointId endpoint, ClusterId clusterId,
    const EmberAfAttributeMetadata *attributeMetadata, uint8_t *buffer) {
  uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);
  if (endpointIndex >= kMaxVirtualEndpoints || attributeMetadata == nullptr) {
    return Protocols::InteractionModel::Status::Failure;
  }

  VirtualEndpoint *endpointObj = &gVirtualEndpoints[endpointIndex];
  if (!endpointObj->allocated) {
    return Protocols::InteractionModel::Status::Failure;
  }

  if (clusterId == Clusters::BridgedDeviceBasicInformation::Id &&
      attributeMetadata->attributeId ==
          Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id) {
    auto status = DecodeZclCharStringToEndpointName(
        buffer, attributeMetadata->size, endpointObj->name,
        sizeof(endpointObj->name));
    if (status == Protocols::InteractionModel::Status::Success) {
      int saveErr = SaveBridgeState();
      if (saveErr != 0) {
        LOG_WRN("NodeLabel updated but bridge state save failed: %d", saveErr);
      }
    }
    return status;
  }

  if (clusterId == Clusters::OnOff::Id &&
      endpointObj->type == VirtualEndpointType::OnOffLight) {
    auto status =
        HandleWriteOnOffAttribute(*endpointObj, attributeMetadata->attributeId,
                                  buffer, attributeMetadata->size);
    if (status == Protocols::InteractionModel::Status::Success) {
      int saveErr = SaveBridgeState();
      if (saveErr != 0) {
        LOG_WRN("On/Off attribute updated but bridge state save failed: %d",
                saveErr);
      }
    }
    return status;
  }

  return Protocols::InteractionModel::Status::Failure;
}

namespace {

/* What kind of thing the primary value is, in one word.
 *
 * `valueHint` explains the format to a person; this says the same thing to a
 * program, so a UI can offer a switch for something that is on or off and a
 * slider for something that runs 0..100 instead of making everyone type
 * numbers. Derived from the cluster rather than stored per row: the cluster
 * already decides the units, and one more column across sixty entries is one
 * more column to get wrong.
 */
const char *ValueKindFor(const VirtualEndpointDefinition &definition) {
  /* Multi-value devices first: their primary attribute is only half the story.
   * A dimmable light reports OnOff as its main value, so a cluster-only answer
   * called it "bool" and the panel drew a switch for something that also has a
   * brightness. The parts are listed in the order `bridge set` takes them:
   * <on/off> [level] [mireds]. */
  switch (definition.type) {
  case VirtualEndpointType::DimmableLight:
  case VirtualEndpointType::DimmablePlug:
  case VirtualEndpointType::MountedDimmableLoadControl:
    return "bool+level";
  case VirtualEndpointType::ColorTemperatureLight:
  case VirtualEndpointType::ExtendedColorLight:
    return "bool+level+mireds";
  /* Everything driven through the Thermostat cluster takes the same three
   * parts, so they must all say so. Reporting a bare "centidegree" made a
   * panel draw a single temperature box for devices that also accept a
   * setpoint and a mode - the values were being handled, just never offered. */
  case VirtualEndpointType::Thermostat:
  case VirtualEndpointType::RoomAirConditioner:
  case VirtualEndpointType::HeatingCoolingUnit:
  case VirtualEndpointType::HeatPump:
  case VirtualEndpointType::WaterHeater:
    return "centidegree+setpoint+mode";
  default:
    break;
  }

  switch (definition.valueClusterId) {
  case Clusters::OnOff::Id:
  case Clusters::BooleanState::Id:
  case Clusters::OccupancySensing::Id:
  case Clusters::SmokeCoAlarm::Id:
  case Clusters::DoorLock::Id:
  case Clusters::ValveConfigurationAndControl::Id:
    return "bool";
  case Clusters::LevelControl::Id:
    return "level"; /* 1..254 */
  case Clusters::WindowCovering::Id:
  case Clusters::FanControl::Id:
    return "percent"; /* 0..100 */
  case Clusters::RelativeHumidityMeasurement::Id:
    return "centipercent"; /* 0..10000 */
  case Clusters::TemperatureMeasurement::Id:
  case Clusters::Thermostat::Id:
    return "centidegree"; /* -27315..32767 */
  case Clusters::AirQuality::Id:
    return "enum";
  default:
    return "number";
  }
}

void PrintSupportedTypes(const struct shell *sh) {
  shell_print(sh, "Supported virtual endpoint types:");
  for (const auto &definition : kVirtualEndpointDefinitions) {
    shell_print(sh, "  %-9s %s (0x%04x), kind=%s, value: %s", definition.slug,
                definition.displayName,
                static_cast<unsigned int>(definition.deviceTypeId),
                ValueKindFor(definition), definition.valueHint);
  }
  shell_print(sh, "Aliases: temperature=temp, humi/rh=humidity, onoff=light, "
                  "presence=occupancy");
  shell_print(sh, "Use: bridge add <type> <name>");
  shell_print(sh, "Use: bridge add all [prefix]");
}

int BridgeStatusCommand(const struct shell *sh, size_t argc, char **argv) {
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  size_t allocatedCount;
  bool paired;
  {
    ChipStackLock lock;
    allocatedCount = AllocatedVirtualEndpointCount();
    paired = chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
  }

  shell_print(sh, "Bridge base endpoints:");
  shell_print(sh, "  EP0: Root Node");
  shell_print(sh, "  EP%u: Aggregator", kAggregatorEndpointId);
  shell_print(sh, "Virtual endpoint slots: %u/%u used",
              static_cast<unsigned int>(allocatedCount),
              static_cast<unsigned int>(kMaxVirtualEndpoints));
  shell_print(sh, "Persistent storage: Zephyr settings (%s, %s*)",
              kBridgeSettingsMetaKey, kBridgeSettingsSlotPrefix);
  shell_print(sh, "Matter dynamic endpoint backend: initialized");
  /* `paired=` is a contract line, same spirit as `manual=`/`qr=` in
   * BridgePairingCommand() below - the ESP parses it to know whether
   * commissioning actually finished, not just whether the link answered. */
  shell_print(sh, "paired=%s", paired ? "yes" : "no");
  shell_print(sh, "Run 'bridge types' to see what can be added.");

  return 0;
}

/* The onboarding codes, on demand.
 *
 * The board already prints these once at boot, commissioned or not, but the
 * companion cannot count on having been listening at that moment: it is often
 * powered up hours later, or restarts on its own while the board keeps running.
 * So it asks instead, and the codes are regenerated from the same payload the
 * boot banner used - identical every time, because they are derived from the
 * discriminator and passcode fixed at build time.
 *
 * Being already paired changes nothing here. The codes remain the ones needed
 * for the next commissioning window, which is exactly when someone goes looking
 * for them.
 *
 * The `manual=` and `qr=` prefixes are what the ESP parses out of this answer;
 * they are part of the contract, not decoration. */
int BridgePairingCommand(const struct shell *sh, size_t argc, char **argv) {
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  /* 20 chars for the long manual code, 128 for the base-38 QR payload - the
   * upper bounds Matter defines for each, plus the terminator.
   *
   * Static, not automatic. As locals these 156 bytes overflowed the shell
   * thread's stack and the board died in MemManage on the first call - the
   * fault address sat 52 bytes below the thread's stack pointer, which is the
   * guard region doing its job. Shell commands all run on that one thread, so
   * there is no second caller to race with here. */
  static char manualBuf[24];
  static char qrBuf[132];
  memset(manualBuf, 0, sizeof(manualBuf));
  memset(qrBuf, 0, sizeof(qrBuf));
  chip::MutableCharSpan manual(manualBuf, sizeof(manualBuf));
  chip::MutableCharSpan qr(qrBuf, sizeof(qrBuf));

  /* kBLE, matching what matter_init prints at startup. Using anything else here
   * would hand out a payload that disagrees with the boot banner. */
  const chip::RendezvousInformationFlags flags(chip::RendezvousInformationFlag::kBLE);

  CHIP_ERROR manualErr;
  CHIP_ERROR qrErr;
  {
    ChipStackLock lock;
    manualErr = GetManualPairingCode(manual, flags);
    qrErr = GetQRCode(qr, flags);
  }

  if (manualErr == CHIP_NO_ERROR) {
    shell_print(sh, "manual=%s", manualBuf);
  } else {
    shell_print(sh, "manual=");
    shell_print(sh, "Manual code unavailable: %s", chip::ErrorStr(manualErr));
  }
  if (qrErr == CHIP_NO_ERROR) {
    shell_print(sh, "qr=%s", qrBuf);
  } else {
    shell_print(sh, "qr=");
    shell_print(sh, "QR payload unavailable: %s", chip::ErrorStr(qrErr));
  }

  return 0;
}

/* The other half of the deferred-BLE scheme (see AppTask::Init() below): BLE
 * advertising no longer starts on its own at boot, so a fresh board answers
 * `bridge pairing` on a clean UART line - no radio active yet to interfere
 * with it - instead of racing the advertising that used to start within the
 * first couple hundred milliseconds. Once the companion has the code, it
 * calls this to actually open the commissioning window and start
 * advertising. Safe to call more than once, and a no-op once paired. */
int BridgeCommissionCommand(const struct shell *sh, size_t argc, char **argv) {
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  CHIP_ERROR err = CHIP_NO_ERROR;
  {
    ChipStackLock lock;
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
      shell_print(sh, "Already paired - not opening a commissioning window.");
      return 0;
    }
    err = chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow();
  }
  if (err != CHIP_NO_ERROR) {
    shell_print(sh, "Failed to open commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
    return -1;
  }
  shell_print(sh, "Commissioning window open - BLE advertising starting.");
  return 0;
}

int BridgeTypesCommand(const struct shell *sh, size_t argc, char **argv) {
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  PrintSupportedTypes(sh);
  return 0;
}

int BridgeListCommand(const struct shell *sh, size_t argc, char **argv) {
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  ChipStackLock lock;

  if (AllocatedVirtualEndpointCount() == 0) {
    shell_print(sh, "No virtual endpoints allocated.");
    return 0;
  }

  for (const auto &endpoint : gVirtualEndpoints) {
    if (!endpoint.allocated) {
      continue;
    }

    /* Secondary values for the devices that have them - a dimmable light is
     * on/off *and* a brightness, and a panel that can only read the first of
     * those has to guess where its slider should sit. Printed only when the
     * type actually uses them, so a plain sensor line stays short.
     *
     * `name=` goes last on purpose: it is the only field that may contain
     * spaces or UTF-8, so a reader can take the rest of the line verbatim
     * instead of guessing where the name ends. */
    const auto *definition = FindEndpointDefinition(endpoint.type);
    const char *kind = definition != nullptr ? ValueKindFor(*definition) : "number";
    const char *firstPlus = strchr(kind, '+');
    const bool hasSecond = firstPlus != nullptr;
    const bool hasThird = hasSecond && strchr(firstPlus + 1, '+') != nullptr;

    char extra[32] = "";
    if (hasThird) {
      snprintf(extra, sizeof(extra), " v2=%d v3=%d", endpoint.value2, endpoint.value3);
    } else if (hasSecond) {
      snprintf(extra, sizeof(extra), " v2=%d", endpoint.value2);
    }

    if (endpoint.hasValue) {
      shell_print(sh, "EP%u type=%s value=%d%s name=%s", endpoint.endpointId,
                  VirtualEndpointTypeName(endpoint.type), endpoint.value, extra,
                  endpoint.name);
    } else {
      shell_print(sh, "EP%u type=%s value=<unset>%s name=%s", endpoint.endpointId,
                  VirtualEndpointTypeName(endpoint.type), extra, endpoint.name);
    }
  }

  return 0;
}

int BridgeAddAllCommand(const struct shell *sh, size_t argc, char **argv) {
  char prefix[kMaxVirtualEndpointNameLength] = {};
  if (argc > 2) {
    CopyEndpointName(prefix, sizeof(prefix), argc, argv, 2, "");
  }

  ChipStackLock lock;

  size_t missingCount = 0;
  for (const auto &definition : kVirtualEndpointDefinitions) {
    if (!HasEndpointOfType(definition.type)) {
      ++missingCount;
    }
  }

  if (missingCount == 0) {
    shell_print(sh, "All supported endpoint types are already present.");
    return 0;
  }

  if (AvailableVirtualEndpointCount() < missingCount) {
    shell_error(sh, "Not enough free endpoint slots: need %u, have %u",
                static_cast<unsigned int>(missingCount),
                static_cast<unsigned int>(AvailableVirtualEndpointCount()));
    return -ENOMEM;
  }

  size_t addedCount = 0;
  for (const auto &definition : kVirtualEndpointDefinitions) {
    if (HasEndpointOfType(definition.type)) {
      shell_print(sh, "Skipping %s: already present", definition.slug);
      continue;
    }

    char endpointName[kMaxVirtualEndpointNameLength] = {};
    BuildEndpointName(endpointName, sizeof(endpointName), prefix, definition);

    VirtualEndpoint *endpoint = nullptr;
    CHIP_ERROR err =
        CreateVirtualEndpoint(definition.type, endpointName,
                              definition.defaultValue, true, &endpoint);
    if (err != CHIP_NO_ERROR) {
      shell_error(sh, "Failed to register %s endpoint: %s", definition.slug,
                  ErrorStr(err));
      NotifyEndpointListChanged();
      SaveBridgeState();
      return -EIO;
    }

    ++addedCount;
    shell_print(sh, "Allocated EP%u type=%s name=%s", endpoint->endpointId,
                definition.slug, endpoint->name);
  }

  NotifyEndpointListChanged();

  int saveErr = SaveBridgeState();
  if (saveErr != 0) {
    shell_error(sh, "Added endpoints but failed to persist bridge state: %d",
                saveErr);
    return -EIO;
  }

  shell_print(sh, "Added %u endpoint(s). State persisted.",
              static_cast<unsigned int>(addedCount));
  return 0;
}

int BridgeAddCommand(const struct shell *sh, size_t argc, char **argv) {
  if (argc < 2) {
    shell_error(sh, "Usage: bridge add <type> <name>");
    shell_print(sh, "Try: bridge types");
    return -EINVAL;
  }

  if (strcmp(argv[1], "all") == 0) {
    return BridgeAddAllCommand(sh, argc, argv);
  }

  if (argc < 3) {
    shell_error(sh, "Usage: bridge add <type> <name>");
    shell_print(sh, "Example: bridge add temp living_room");
    return -EINVAL;
  }

  const auto *definition = FindEndpointDefinitionByName(argv[1]);
  if (definition == nullptr) {
    shell_error(sh, "Unsupported endpoint type: %s", argv[1]);
    PrintSupportedTypes(sh);
    return -EINVAL;
  }

  ChipStackLock lock;

  char endpointName[kMaxVirtualEndpointNameLength] = {};
  CopyEndpointName(endpointName, sizeof(endpointName), argc, argv, 2,
                   definition->defaultName);

  VirtualEndpoint *endpoint = nullptr;
  CHIP_ERROR err =
      CreateVirtualEndpoint(definition->type, endpointName,
                            definition->defaultValue, true, &endpoint);
  if (err != CHIP_NO_ERROR) {
    shell_error(sh, "Failed to register Matter dynamic endpoint: %s",
                ErrorStr(err));
    return -EIO;
  }

  NotifyEndpointListChanged();

  int saveErr = SaveBridgeState();
  if (saveErr != 0) {
    shell_error(sh, "Endpoint added but failed to persist bridge state: %d",
                saveErr);
    return -EIO;
  }

  shell_print(sh, "Allocated EP%u type=%s name=%s", endpoint->endpointId,
              definition->slug, endpoint->name);
  shell_print(sh, "Matter dynamic endpoint backend: active and persisted.");

  return 0;
}

int BridgeSetCommand(const struct shell *sh, size_t argc, char **argv) {
  if (argc < 3 || argc > 5) {
    shell_error(sh, "Usage: bridge set <endpoint_id> <value> [level] [mireds]");
    shell_print(sh, "Run 'bridge types' for value formats.");
    return -EINVAL;
  }

  uint16_t endpointId;
  if (!ParseUInt16(argv[1], endpointId)) {
    shell_error(sh, "Invalid endpoint id: %s", argv[1]);
    return -EINVAL;
  }

  int32_t value;
  if (!ParseInt32(argv[2], value)) {
    shell_error(sh, "Invalid value: %s", argv[2]);
    return -EINVAL;
  }

  ChipStackLock lock;

  VirtualEndpoint *endpoint = FindVirtualEndpoint(endpointId);
  if (endpoint == nullptr) {
    shell_error(sh, "Virtual endpoint EP%u not found.", endpointId);
    return -ENOENT;
  }

  const char *errorMessage = nullptr;
  if (!ValidateEndpointValue(endpoint->type, value, &errorMessage)) {
    shell_error(sh, "Invalid value for %s: %s",
                VirtualEndpointTypeName(endpoint->type), errorMessage);
    return -EINVAL;
  }

  endpoint->value = value;
  endpoint->hasValue = true;

  /* Optional extra arguments drive the secondary attributes, and what they mean
   * depends on the device.
   *
   * They used to be parsed as brightness and colour temperature for every type,
   * which meant a thermostat could never be given a setpoint: `set <ep> 2150
   * 2300 4` failed the 1..254 level check, bailed out, and left the setpoint
   * and mode at their defaults while the temperature had already been applied.
   * The whole climate family - thermostat, room AC, heating/cooling unit, heat
   * pump, water heater - shares the Thermostat cluster and therefore this
   * shape. */
  const bool climate = endpoint->type == VirtualEndpointType::Thermostat ||
                       endpoint->type == VirtualEndpointType::RoomAirConditioner ||
                       endpoint->type == VirtualEndpointType::HeatingCoolingUnit ||
                       endpoint->type == VirtualEndpointType::HeatPump ||
                       endpoint->type == VirtualEndpointType::WaterHeater;

  if (argc >= 4) {
    int32_t second = 0;
    if (climate) {
      if (!ParseInt32(argv[3], second) || second < INT16_MIN || second > INT16_MAX) {
        shell_error(sh, "Invalid setpoint '%s': expected centi-C in %d..%d",
                    argv[3], INT16_MIN, INT16_MAX);
        return -EINVAL;
      }
      endpoint->value2 = second;
      MatterReportingAttributeChangeCallback(
          endpoint->endpointId, Clusters::Thermostat::Id,
          Clusters::Thermostat::Attributes::OccupiedHeatingSetpoint::Id);
    } else {
      if (!ParseInt32(argv[3], second) || second < 1 || second > 254) {
        shell_error(sh, "Invalid level '%s': expected 1..254", argv[3]);
        return -EINVAL;
      }
      endpoint->value2 = second;
      MatterReportingAttributeChangeCallback(
          endpoint->endpointId, Clusters::LevelControl::Id,
          Clusters::LevelControl::Attributes::CurrentLevel::Id);
    }
  }
  if (argc >= 5) {
    int32_t third = 0;
    if (climate) {
      /* SystemMode: 0 off, 1 auto, 3 cool, 4 heat, 5 emergency heat, 7 fan,
       * 8 dry. Anything else is not a mode a controller will understand. */
      if (!ParseInt32(argv[4], third) || third < 0 || third > 8) {
        shell_error(sh, "Invalid mode '%s': expected 0..8 "
                        "(0=off, 1=auto, 3=cool, 4=heat)", argv[4]);
        return -EINVAL;
      }
      endpoint->value3 = third;
      MatterReportingAttributeChangeCallback(
          endpoint->endpointId, Clusters::Thermostat::Id,
          Clusters::Thermostat::Attributes::SystemMode::Id);
    } else {
      if (!ParseInt32(argv[4], third) || third < kMinColorTempMireds ||
          third > kMaxColorTempMireds) {
        shell_error(sh, "Invalid color temperature '%s': expected %d..%d mireds",
                    argv[4], static_cast<int>(kMinColorTempMireds),
                    static_cast<int>(kMaxColorTempMireds));
        return -EINVAL;
      }
      endpoint->value3 = third;
      MatterReportingAttributeChangeCallback(
          endpoint->endpointId, Clusters::ColorControl::Id,
          Clusters::ColorControl::Attributes::ColorTemperatureMireds::Id);
    }
  }

  NotifyEndpointValueChanged(*endpoint);

  int saveErr = SaveBridgeState();
  if (saveErr != 0) {
    shell_error(sh, "Value updated but failed to persist bridge state: %d",
                saveErr);
    return -EIO;
  }

  shell_print(sh, "Updated EP%u type=%s value=%d", endpoint->endpointId,
              VirtualEndpointTypeName(endpoint->type), endpoint->value);
  shell_print(sh, "Matter virtual endpoint value updated and persisted.");

  return 0;
}

int BridgeRemoveCommand(const struct shell *sh, size_t argc, char **argv) {
  if (argc != 2) {
    shell_error(sh, "Usage: bridge remove <endpoint_id>");
    return -EINVAL;
  }

  uint16_t endpointId;
  if (!ParseUInt16(argv[1], endpointId)) {
    shell_error(sh, "Invalid endpoint id: %s", argv[1]);
    return -EINVAL;
  }

  ChipStackLock lock;

  VirtualEndpoint *endpoint = FindVirtualEndpoint(endpointId);
  if (endpoint == nullptr) {
    shell_error(sh, "Virtual endpoint EP%u not found.", endpointId);
    return -ENOENT;
  }

  ClearMatterDynamicEndpoint(endpoint);
  *endpoint = {};
  NotifyEndpointListChanged();

  int saveErr = SaveBridgeState();
  if (saveErr != 0) {
    shell_error(sh, "Endpoint removed but failed to persist bridge state: %d",
                saveErr);
    return -EIO;
  }

  shell_print(sh, "Removed virtual endpoint EP%u", endpointId);
  shell_print(sh,
              "Matter dynamic endpoint backend: EP%u cleared and "
              "persisted.",
              endpointId);

  return 0;
}

int BridgeRebootCommand(const struct shell *sh, size_t argc, char **argv) {
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  shell_print(sh, "Rebooting the device...");
  k_msleep(50);
  sys_reboot(SYS_REBOOT_COLD);
  return 0;
}

int BridgeFactoryResetCommand(const struct shell *sh, size_t argc,
                              char **argv) {
  ARG_UNUSED(argv);

  if (argc < 2 || strcmp(argv[1], "confirm") != 0) {
    shell_warn(sh, "This erases Matter fabrics AND all bridged endpoints.");
    shell_print(sh, "Run 'bridge factoryreset confirm' to proceed.");
    return 0;
  }

  shell_print(sh, "Performing factory reset...");
  PlatformMgr().ScheduleWork(
      [](intptr_t) { ConfigurationMgr().InitiateFactoryReset(); }, 0);
  return 0;
}

int BridgeHelpCommand(const struct shell *sh, size_t argc, char **argv) {
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  shell_print(sh, "Matter bridge commands:");
  shell_print(sh, "  bridge list                    list active devices");
  shell_print(sh, "  bridge types                   list device types you can add");
  shell_print(sh, "  bridge add <type> <name>       add a device (e.g. add plug lamp)");
  shell_print(sh, "  bridge add all                 add one of every type (fills slots)");
  shell_print(sh, "  bridge set <id> <value>        set a device value (e.g. set 3 1)");
  shell_print(sh, "  bridge set <id> <on> <level>   dimmable light: on/off + 1-254");
  shell_print(sh, "  bridge set <id> <on> <lvl> <ct> color light: + 153-500 mireds");
  shell_print(sh, "  bridge remove <id>             remove a device by endpoint id");
  shell_print(sh, "  bridge status                  show slot usage / runtime status");
  shell_print(sh, "  bridge pairing                 show the manual and QR codes");
  shell_print(sh, "  bridge commission              open the commissioning window (starts BLE)");
#if CONFIG_BRIDGE_XIAO_IMU
  shell_print(sh, "  bridge imu                     live IMU reading (XIAO Sense)");
#endif
  shell_print(sh, "  bridge reboot                  reboot the board");
  shell_print(sh, "  bridge factoryreset confirm    erase all devices + Matter fabrics");
  shell_print(sh, "  bridge help                    show this list");
  return 0;
}

#if CONFIG_BRIDGE_XIAO_IMU
int BridgeImuCommand(const struct shell *sh, size_t argc, char **argv) {
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  if (!device_is_ready(gImuDevice)) {
    shell_error(sh, "IMU %s is not ready.", gImuDevice->name);
    return -ENODEV;
  }

  int32_t milliG = 0;
  if (!ReadAccelerationMilliG(milliG)) {
    shell_error(sh, "Failed to read the IMU.");
    return -EIO;
  }

  const int32_t deviation =
      milliG > 1000 ? milliG - 1000 : 1000 - milliG;

  shell_print(sh, "IMU %s", gImuDevice->name);
  shell_print(sh, "  acceleration : %d mg (1000 mg = 1 g at rest)", milliG);
  shell_print(sh, "  deviation    : %d mg", deviation);
  shell_print(sh, "  thresholds   : vibration %d mg, free fall %d mg, "
                  "impact %d mg",
              CONFIG_BRIDGE_XIAO_IMU_VIBRATION_MG,
              CONFIG_BRIDGE_XIAO_IMU_FREEFALL_MG,
              CONFIG_BRIDGE_XIAO_IMU_IMPACT_MG);
  if (gImuEndpointId != 0) {
    shell_print(sh, "  endpoint     : EP%u (holds %d s after a trigger)",
                gImuEndpointId, CONFIG_BRIDGE_XIAO_IMU_HOLD_S);
  } else {
    shell_print(sh, "  endpoint     : none, run 'bridge add occupancy %s'",
                kImuEndpointName);
  }
  return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
    bridge_commands,
    SHELL_CMD_ARG(help, NULL, "Show the command list.", BridgeHelpCommand, 1, 0),
    SHELL_CMD_ARG(status, NULL, "Show bridge runtime status.",
                  BridgeStatusCommand, 1, 0),
    SHELL_CMD_ARG(pairing, NULL, "Show the manual and QR onboarding codes.",
                  BridgePairingCommand, 1, 0),
    SHELL_CMD_ARG(commission, NULL,
                  "Open the commissioning window (starts BLE advertising).",
                  BridgeCommissionCommand, 1, 0),
    SHELL_CMD_ARG(types, NULL, "List endpoint types that can be added.",
                  BridgeTypesCommand, 1, 0),
    SHELL_CMD_ARG(list, NULL, "List virtual endpoints.", BridgeListCommand, 1,
                  0),
    SHELL_CMD_ARG(add, NULL,
                  "Add endpoint: bridge add <type> <name>, bridge add all.",
                  BridgeAddCommand, 2, kMaxVirtualEndpointNameLength),
    SHELL_CMD_ARG(set, NULL,
                  "Set value: bridge set <id> <value> [level] [mireds].",
                  BridgeSetCommand, 3, 2),
    SHELL_CMD_ARG(remove, NULL, "Remove endpoint: bridge remove <endpoint_id>.",
                  BridgeRemoveCommand, 2, 0),
#if CONFIG_BRIDGE_XIAO_IMU
    SHELL_CMD_ARG(imu, NULL, "Show live IMU reading and thresholds.",
                  BridgeImuCommand, 1, 0),
#endif
    SHELL_CMD_ARG(reboot, NULL, "Reboot the device.", BridgeRebootCommand, 1,
                  0),
    SHELL_CMD_ARG(factoryreset, NULL,
                  "Erase fabrics + endpoints: bridge factoryreset confirm.",
                  BridgeFactoryResetCommand, 1, 1),
    SHELL_SUBCMD_SET_END);

// Root handler prints the menu, so a bare "bridge" over a plain UART (no
// tab-completion) lists everything instead of erroring out.
SHELL_CMD_REGISTER(bridge, &bridge_commands, "Matter bridge commands (run for menu).",
                   BridgeHelpCommand);

/* Deferred commissioning window - the other half lives in `bridge commission`
 * (BridgeCommissionCommand, above) and in `paired=` on `bridge status`.
 *
 * Left to its own defaults, an uncommissioned board opens the commissioning
 * window - and starts CHIPoBLE advertising - within the first couple hundred
 * milliseconds of boot, before the shell has had any real chance to be asked
 * for anything. That is exactly the window `bridge pairing` is most needed
 * in, and exactly the window in which the companion's UART line turns out to
 * be unusable: the radio work BLE advertising pulls in leaves the shell
 * thread starved often enough, on this board, to look like the link itself
 * is down. Confirmed on hardware this session, not assumed - a byte-level
 * capture of the UART RX line showed continuous line noise tracking the
 * advertising, not anything the companion sent.
 *
 * `advertiseCommissionableIfNoFabrics = false` here stops that window from
 * opening on its own. The board comes up perfectly reachable over UART -
 * `bridge status`, `bridge pairing`, everything - and simply stays that way,
 * uncommissioned, until something calls `bridge commission`. Being already
 * paired is unaffected either way: Server::Init() disables BLE advertising
 * outright in that case regardless of this flag (see Server.cpp), so a
 * commissioned board's boot sequence does not change at all. */
CHIP_ERROR DeferCommissioningWindow() {
  Nrf::Matter::InitData::sServerInitParamsDefault.advertiseCommissionableIfNoFabrics = false;
  return CHIP_NO_ERROR;
}
} // namespace

CHIP_ERROR AppTask::Init() {
  /* Initialize Matter stack. BLE advertising deliberately does not start on
   * its own here - see DeferCommissioningWindow() above for why, and
   * `bridge commission` for what starts it. */
  Nrf::Matter::InitData initData;
  initData.mPreServerInitClbk = DeferCommissioningWindow;
  ReturnErrorOnFailure(Nrf::Matter::PrepareServer(initData));

  if (!Nrf::GetBoard().Init()) {
    LOG_ERR("User interface initialization failed.");
    return CHIP_ERROR_INCORRECT_STATE;
  }

  /* Register Matter event handler that controls the connectivity status LED
   * based on the captured Matter network state. */
  ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(
      Nrf::Board::DefaultMatterEventHandler, 0));

  LOG_INF("Bridge CLI is available over Zephyr shell. Try: bridge types");

  ReturnErrorOnFailure(Nrf::Matter::StartServer());

  /* Bridge set-up happens here, not from mPostServerInitClbk.
   *
   * That callback fires from inside PrepareServer(), while the stack is still
   * assembling itself. Registering dynamic endpoints there reaches into
   * Bluetooth before its buffer pools exist, and the allocator then works from
   * a pool whose __bufs pointer is still zero - the write lands on address
   * 0x0a and the board dies in the controller's thread, several subsystems
   * away from anything the bridge did. It survived for a long time because the
   * outcome depends on how the code happens to be laid out: adding three log
   * lines was enough to make the symptom disappear without fixing anything. */
  ReturnErrorOnFailure(RegisterBridgeCommandHandlers());
  ReturnErrorOnFailure(RestoreBridgeStateOrAddDefault());

#if CONFIG_BRIDGE_XIAO_IMU
  /* Started only once the server is actually running.
   *
   * It used to start from mPostServerInitClbk, which runs inside
   * PrepareServer(): the sampling thread then began taking the Matter stack
   * lock and reporting attribute changes while the server behind them did not
   * exist yet. The damage never showed up where it was done - it surfaced much
   * later as a bus fault in the Bluetooth controller's thread, dereferencing a
   * net_buf pool pointer that something had already overwritten.
   *
   * It survived for weeks because a commissioned board takes a different, and
   * slower, path through start-up; wiping the settings changed the timing and
   * the race began losing every single boot. */
  ReturnErrorOnFailure(StartOnboardLed());
  ReturnErrorOnFailure(StartImuVibrationSensor());
#endif

  return CHIP_NO_ERROR;
}

CHIP_ERROR AppTask::StartApp() {
  ReturnErrorOnFailure(Init());

  while (true) {
    Nrf::DispatchNextTask();
  }

  return CHIP_NO_ERROR;
}
