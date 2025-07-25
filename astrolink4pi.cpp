/*******************************************************************************
 Copyright(c) 2023 astrojolo.com
 .
 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License version 2 as published by the Free Software Foundation.
 .
 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.
 .
 You should have received a copy of the GNU Library General Public License
 along with this library; see the file COPYING.LIB.  If not, write to
 the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 Boston, MA 02110-1301, USA.
*******************************************************************************/

#include "astrolink4pi.h"

std::unique_ptr<AstroLink4Pi> astroLink4Pi(new AstroLink4Pi());

#define ACS_TYPE 0		// 0 - 20A, 1 - 5A

#define MAX_RESOLUTION 32							 // the highest resolution supported is 1/32 step
#define TEMPERATURE_UPDATE_TIMEOUT (5 * 1000)		 // 5 sec
#define TEMPERATURE_COMPENSATION_TIMEOUT (30 * 1000) // 30 sec
#define SYSTEM_UPDATE_PERIOD 1000
#define POLL_PERIOD 200
#define FAN_PERIOD (20 * 1000)

#define TSL2591_ADC_TIME 750  // integration time in ms for a single increment
#define TSL2591_ADDR (0x29)
#define TSL2591_COMMAND_BIT (0xA0)  // bits 7 and 5 for 'command normal'
#define TSL2591_ENABLE_POWERON (0x01)
#define TSL2591_ENABLE_POWEROFF (0x00)
#define TSL2591_ENABLE_AEN (0x02)
#define TSL2591_ENABLE_AIEN (0x10)
#define TSL2591_REGISTER_ENABLE 0x00
#define TSL2591_REGISTER_CONTROL 0x01
#define TSL2591_REGISTER_CHAN0_LOW 0x14
#define TSL2591_REGISTER_CHAN1_LOW 0x16
#define FILTER_COEFF -1.2

#define RP4_GPIO 0
#define RP5_GPIO 4
#define DECAY_PIN 14
#define EN_PIN 15
#define M0_PIN 17
#define M1_PIN 18
#define M2_PIN 27
#define RST_PIN 22
#define STP_PIN 24
#define DIR_PIN 23
#define OUT1_PIN 5
#define OUT2_PIN 6
#define PWM1_PIN 26
#define PWM2_PIN 19
#define MOTOR_PWM 20
#define CHK_IN_PIN 16
#define FAN_PIN 13

void ISPoll(void *p);

void ISInit()
{
	static int isInit = 0;

	if (isInit == 1)
		return;
	if (astroLink4Pi.get() == 0)
	{
		isInit = 1;
		astroLink4Pi.reset(new AstroLink4Pi());
	}
}

void ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int num)
{
	ISInit();
	astroLink4Pi->ISNewSwitch(dev, name, states, names, num);
}

void ISNewText(const char *dev, const char *name, char *texts[], char *names[], int num)
{
	ISInit();
	astroLink4Pi->ISNewText(dev, name, texts, names, num);
}

void ISNewNumber(const char *dev, const char *name, double values[], char *names[], int num)
{
	astroLink4Pi->ISNewNumber(dev, name, values, names, num);
}

AstroLink4Pi::AstroLink4Pi() : FI(this), WI(this)
{
	setVersion(VERSION_MAJOR, VERSION_MINOR);
}

AstroLink4Pi::~AstroLink4Pi()
{
	if (_motionThread.joinable())
	{
		_abort = true;
		_motionThread.join();
	}
}

const char *AstroLink4Pi::getDefaultName()
{
	return (char *)"AstroLink 4 Pi";
}

bool AstroLink4Pi::Connect()
{
	revision = checkRevision();
	if (revision < 3)
	{
		DEBUGF(INDI::Logger::DBG_ERROR, "This INDI driver version works only with AstroLink 4 Pi revision 3 and higer. Revision detected %d", revision);
		return false;
	}

	pigpioHandle = lgGpiochipOpen(gpioType);
	if (pigpioHandle < 0)
	{
		DEBUGF(INDI::Logger::DBG_ERROR, "Could not access GPIO. Error code %d , GPIO number %d", pigpioHandle, gpioType);
		return false;
	}

	lgGpioClaimOutput(pigpioHandle, 0, DECAY_PIN, 0);
	lgGpioClaimOutput(pigpioHandle, 0, EN_PIN, 1); // EN_PIN start as disabled
	lgGpioClaimOutput(pigpioHandle, 0, M0_PIN, 0);
	lgGpioClaimOutput(pigpioHandle, 0, M1_PIN, 0);
	lgGpioClaimOutput(pigpioHandle, 0, M2_PIN, 0);
	lgGpioClaimOutput(pigpioHandle, 0, RST_PIN, 1); // RST_PIN start as wake up
	lgGpioClaimOutput(pigpioHandle, 0, STP_PIN, 0);
	lgGpioClaimOutput(pigpioHandle, 0, DIR_PIN, 0);
	lgGpioClaimOutput(pigpioHandle, 0, OUT1_PIN, relayState[0]);
	lgGpioClaimOutput(pigpioHandle, 0, OUT2_PIN, relayState[1]);
	lgGpioClaimOutput(pigpioHandle, 0, PWM1_PIN, 0);
	lgGpioClaimOutput(pigpioHandle, 0, PWM2_PIN, 0);
	lgGpioClaimOutput(pigpioHandle, 0, MOTOR_PWM, 0);
	lgGpioClaimOutput(pigpioHandle, 0, FAN_PIN, 0);

	// Lock Relay Labels setting
	RelayLabelsTP.s = IPS_BUSY;
	IDSetText(&RelayLabelsTP, nullptr);

	// Get basic system info
	FILE *pipe;
	char buffer[128];

	// update Hardware
	// https://www.raspberrypi.org/documentation/hardware/raspberrypi/revision-codes/README.md
	pipe = popen("cat /sys/firmware/devicetree/base/model", "r");
	if (fgets(buffer, 128, pipe) != NULL)
		IUSaveText(&SysInfoT[SYSI_HARDWARE], buffer);
	pclose(pipe);

	// update Hostname
	pipe = popen("hostname", "r");
	if (fgets(buffer, 128, pipe) != NULL)
		IUSaveText(&SysInfoT[SYSI_HOST], buffer);
	pclose(pipe);

	// update Local IP
	pipe = popen("hostname -I|awk -F' '  '{print $1}'|xargs", "r");
	if (fgets(buffer, 128, pipe) != NULL)
		IUSaveText(&SysInfoT[SYSI_LOCALIP], buffer);
	pclose(pipe);

	// update Public IP
	pipe = popen("wget -qO- http://ipecho.net/plain|xargs", "r");
	if (fgets(buffer, 128, pipe) != NULL)
		IUSaveText(&SysInfoT[SYSI_PUBIP], buffer);
	pclose(pipe);

	// Update client
	IDSetText(&SysInfoTP, NULL);

	// read last position from file & convert from MAX_RESOLUTION to current resolution
	FocusAbsPosNP[0].setValue(savePosition(-1) != -1 ? (int)savePosition(-1) * resolution / MAX_RESOLUTION : 0);

	// preset resolution
	SetResolution(resolution);

	getFocuserInfo();
	long int currentTime = millis();
	nextTemperatureRead = currentTime + TEMPERATURE_UPDATE_TIMEOUT;
	nextTemperatureCompensation = currentTime + TEMPERATURE_COMPENSATION_TIMEOUT;
	nextSystemRead = currentTime + SYSTEM_UPDATE_PERIOD;
	nextFanUpdate = currentTime + 3000;

	SetTimer(POLL_PERIOD);
	setCurrent(true);

	DEBUG(INDI::Logger::DBG_SESSION, "AstroLink 4 Pi connected successfully.");

	return true;
}

bool AstroLink4Pi::Disconnect()
{
	lgGpioWrite(pigpioHandle, RST_PIN, 0);					 // sleep
	int enabledState = lgGpioWrite(pigpioHandle, EN_PIN, 1); // make disabled

	if (enabledState != 0)
	{
		DEBUGF(INDI::Logger::DBG_ERROR, "Cannot set GPIO line %i to disable stepper motor driver. Focusing motor may still be powered.", EN_PIN);
	}
	else
	{
		DEBUG(INDI::Logger::DBG_SESSION, "Focusing motor power disabled.");
	}

	lgGpioFree(pigpioHandle, DECAY_PIN);
	lgGpioFree(pigpioHandle, EN_PIN);
	lgGpioFree(pigpioHandle, M0_PIN);
	lgGpioFree(pigpioHandle, M1_PIN);
	lgGpioFree(pigpioHandle, M2_PIN);
	lgGpioFree(pigpioHandle, M2_PIN);
	lgGpioFree(pigpioHandle, RST_PIN);
	lgGpioFree(pigpioHandle, STP_PIN);
	lgGpioFree(pigpioHandle, DIR_PIN);
	lgGpioFree(pigpioHandle, OUT1_PIN);
	lgGpioFree(pigpioHandle, OUT2_PIN);
	lgGpioFree(pigpioHandle, PWM1_PIN);
	lgGpioFree(pigpioHandle, PWM2_PIN);
	lgGpioFree(pigpioHandle, MOTOR_PWM);
	lgGpioFree(pigpioHandle, FAN_PIN);

	lgGpiochipClose(pigpioHandle);

	// Unlock Relay Labels setting
	RelayLabelsTP.s = IPS_IDLE;
	IDSetText(&RelayLabelsTP, nullptr);

	DEBUG(INDI::Logger::DBG_SESSION, "AstroLink 4 Pi disconnected successfully.");

	return true;
}

bool AstroLink4Pi::initProperties()
{
	INDI::DefaultDevice::initProperties();

	setDriverInterface(AUX_INTERFACE | FOCUSER_INTERFACE | WEATHER_INTERFACE);

	FI::SetCapability(FOCUSER_CAN_ABS_MOVE |
					  FOCUSER_CAN_REL_MOVE |
					  FOCUSER_CAN_REVERSE |
					  FOCUSER_CAN_SYNC |
					  FOCUSER_CAN_ABORT |
					  FOCUSER_HAS_BACKLASH);

	FI::initProperties(FOCUS_TAB);
	WI::initProperties(SYSTEM_TAB, ENVIRONMENT_TAB);

	// addDebugControl();
	// addSimulationControl();
	addConfigurationControl();

	// Focuser Resolution
	IUFillSwitch(&FocusResolutionS[RES_1], "RES_1", "Full Step", ISS_ON);
	IUFillSwitch(&FocusResolutionS[RES_2], "RES_2", "Half Step", ISS_OFF);
	IUFillSwitch(&FocusResolutionS[RES_4], "RES_4", "1/4 STEP", ISS_OFF);
	IUFillSwitch(&FocusResolutionS[RES_8], "RES_8", "1/8 STEP", ISS_OFF);
	IUFillSwitch(&FocusResolutionS[RES_16], "RES_16", "1/16 STEP", ISS_OFF);
	IUFillSwitch(&FocusResolutionS[RES_32], "RES_32", "1/32 STEP", ISS_OFF);
	IUFillSwitchVector(&FocusResolutionSP, FocusResolutionS, 6, getDeviceName(), "FOCUS_RESOLUTION", "Resolution", OPTIONS_TAB, IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

	// Focuser motor hold
	IUFillSwitch(&FocusHoldS[HOLD_0], "HOLD_0", "0%", ISS_ON);
	IUFillSwitch(&FocusHoldS[HOLD_20], "HOLD_20", "20%", ISS_OFF);
	IUFillSwitch(&FocusHoldS[HOLD_40], "HOLD_40", "40%", ISS_OFF);
	IUFillSwitch(&FocusHoldS[HOLD_60], "HOLD_60", "60%", ISS_OFF);
	IUFillSwitch(&FocusHoldS[HOLD_80], "HOLD_80", "80%", ISS_OFF);
	IUFillSwitch(&FocusHoldS[HOLD_100], "HOLD_100", "100%", ISS_OFF);
	IUFillSwitchVector(&FocusHoldSP, FocusHoldS, 6, getDeviceName(), "FOCUS_HOLD", "Hold power", OPTIONS_TAB, IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

	// Step delay setting
	IUFillNumber(&FocusStepDelayN[0], "FOCUS_STEPDELAY_VALUE", "microseconds", "%0.0f", 200, 20000, 1, 2000);
	IUFillNumberVector(&FocusStepDelayNP, FocusStepDelayN, 1, getDeviceName(), "FOCUS_STEPDELAY", "Step Delay", OPTIONS_TAB, IP_RW, 0, IPS_IDLE);

	IUFillNumber(&PWMcycleN[0], "PWMcycle", "PWM freq. [Hz]", "%0.0f", 10, 1000, 10, 20);
	IUFillNumberVector(&PWMcycleNP, PWMcycleN, 1, getDeviceName(), "PWMCYCLE", "PWM frequency", OPTIONS_TAB, IP_RW, 0, IPS_IDLE);

	// Focuser temperature
	IUFillNumber(&FocusTemperatureN[0], "FOCUS_TEMPERATURE_VALUE", "°C", "%0.2f", -50, 50, 1, 0);
	IUFillNumberVector(&FocusTemperatureNP, FocusTemperatureN, 1, getDeviceName(), "FOCUS_TEMPERATURE", "Temperature", MAIN_CONTROL_TAB, IP_RO, 0, IPS_IDLE);

	// Temperature Coefficient
	IUFillNumber(&TemperatureCoefN[0], "steps/C", "", "%.1f", -1000, 1000, 1, 0);
	IUFillNumberVector(&TemperatureCoefNP, TemperatureCoefN, 1, getDeviceName(), "Temperature Coefficient", "", OPTIONS_TAB, IP_RW, 0, IPS_IDLE);

	// Compensate for temperature
	IUFillSwitch(&TemperatureCompensateS[0], "Enable", "", ISS_OFF);
	IUFillSwitch(&TemperatureCompensateS[1], "Disable", "", ISS_ON);
	IUFillSwitchVector(&TemperatureCompensateSP, TemperatureCompensateS, 2, getDeviceName(), "Temperature Compensate", "", OPTIONS_TAB, IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

	// Focuser Info
	IUFillNumber(&FocuserInfoN[FOC_STEP_SIZE], "FOC_STEP_SIZE", "Step Size (um)", "%0.2f", 0, 1000, 1, 0);
	IUFillNumber(&FocuserInfoN[FOC_CFZ], "FOC_CFZ", "Critical Focus Zone (um)", "%0.2f", 0, 1000, 1, 0);
	IUFillNumber(&FocuserInfoN[FOC_STEPS_CFZ], "FOC_STEPS_CFZ", "Steps / Critical Focus Zone", "%0.0f", 0, 1000, 1, 0);
	IUFillNumberVector(&FocuserInfoNP, FocuserInfoN, 3, getDeviceName(), "FOCUSER_PARAMETERS", "Focuser Info", MAIN_CONTROL_TAB, IP_RO, 0, IPS_IDLE);

	// Maximum focuser travel
	IUFillNumber(&FocuserTravelN[0], "FOCUSER_TRAVEL_VALUE", "mm", "%0.0f", 10, 200, 10, 10);
	IUFillNumberVector(&FocuserTravelNP, FocuserTravelN, 1, getDeviceName(), "FOCUSER_TRAVEL", "Max Travel", OPTIONS_TAB, IP_RW, 0, IPS_IDLE);

	// Scope params
	IUFillNumber(&ScopeParametersN[SCOPE_DIAM], "SCOPE_DIAM", "Aperture (mm)", "%0.0f", 10, 5000, 0, 0.0);
	IUFillNumber(&ScopeParametersN[SCOPE_FL], "SCOPE_FL", "Focal Length (mm)", "%0.0f", 10, 10000, 0, 0.0);
	IUFillNumberVector(&ScopeParametersNP, ScopeParametersN, 2, getDeviceName(), "TELESCOPE_INFO", "Scope Properties", OPTIONS_TAB, IP_RW, 60, IPS_OK);

	IUFillText(&SysTimeT[SYST_TIME], "SYST_TIME", "Local Time", NULL);
	IUFillText(&SysTimeT[SYST_OFFSET], "SYST_OFFSET", "UTC Offset", NULL);
	IUFillTextVector(&SysTimeTP, SysTimeT, 2, getDeviceName(), "SYSTEM_TIME", "System Time", SYSTEM_TAB, IP_RO, 60, IPS_IDLE);

	IUFillText(&SysInfoT[SYSI_HARDWARE], "SYSI_HARDWARE", "Hardware", NULL);
	IUFillText(&SysInfoT[SYSI_CPUTEMP], "SYSI_CPUTEMP", "CPU Temp (°C)", NULL);
	IUFillText(&SysInfoT[SYSI_UPTIME], "SYSI_UPTIME", "Uptime (hh:mm)", NULL);
	IUFillText(&SysInfoT[SYSI_LOAD], "SYSI_LOAD", "Load (1 / 5 / 15 min.)", NULL);
	IUFillText(&SysInfoT[SYSI_HOST], "SYSI_HOST", "Hostname", NULL);
	IUFillText(&SysInfoT[SYSI_LOCALIP], "SYSI_LOCALIP", "Local IP", NULL);
	IUFillText(&SysInfoT[SYSI_PUBIP], "SYSI_PUBIP", "Public IP", NULL);
	IUFillTextVector(&SysInfoTP, SysInfoT, 7, getDeviceName(), "SYSTEM_INFO", "System Info", SYSTEM_TAB, IP_RO, 60, IPS_IDLE);

	IUFillNumber(&FanPowerN[0], "FAN_PWR", "Speed [%]", "%0.0f", 0, 100, 1, 33);
	IUFillNumberVector(&FanPowerNP, FanPowerN, 1, getDeviceName(), "FAN_POWER", "Internal fan", SYSTEM_TAB, IP_RO, 60, IPS_IDLE);

	IUFillText(&RelayLabelsT[LAB_OUT1], "LAB_OUT1", "OUT 1", "OUT 1");
	IUFillText(&RelayLabelsT[LAB_OUT2], "LAB_OUT2", "OUT 2", "OUT 2");
	IUFillText(&RelayLabelsT[LAB_PWM1], "LAB_PWM1", "PWM 1", "PWM 1");
	IUFillText(&RelayLabelsT[LAB_PWM2], "LAB_PWM2", "PWM 2", "PWM 2");
	IUFillTextVector(&RelayLabelsTP, RelayLabelsT, 4, getDeviceName(), "RELAYLABELS", "Relay Labels", OPTIONS_TAB, IP_RW, 60, IPS_IDLE);
	
	IUFillNumber(&SQMOffsetN[0], "SQMOffset", "mag/arcsec2", "%0.2f", -1, 1, 0.01, 0);
	IUFillNumberVector(&SQMOffsetNP, SQMOffsetN, 1, getDeviceName(), "SQMOFFSET", "SQM calibration", OPTIONS_TAB, IP_RW, 60, IPS_IDLE);    
	

	// Load options before connecting
	// load config before defining switches
	defineProperty(&RelayLabelsTP);
	loadConfig();

	IUFillNumber(&StepperCurrentN[0], "STEPPER_CURRENT", "mA", "%0.0f", 200, 2000, 50, 400);
	IUFillNumberVector(&StepperCurrentNP, StepperCurrentN, 1, getDeviceName(), "STEPPER_CURRENT", "Stepper current", OPTIONS_TAB, IP_RW, 0, IPS_IDLE);

	IUFillSwitch(&Switch1S[S1_ON], "S1_ON", "ON", ISS_OFF);
	IUFillSwitch(&Switch1S[S1_OFF], "S1_OFF", "OFF", ISS_ON);
	IUFillSwitchVector(&Switch1SP, Switch1S, 2, getDeviceName(), "SWITCH_1", RelayLabelsT[0].text, OUTPUTS_TAB, IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

	IUFillSwitch(&Switch2S[S2_ON], "S2_ON", "ON", ISS_OFF);
	IUFillSwitch(&Switch2S[S2_OFF], "S2_OFF", "OFF", ISS_ON);
	IUFillSwitchVector(&Switch2SP, Switch2S, 2, getDeviceName(), "SWITCH_2", RelayLabelsT[1].text, OUTPUTS_TAB, IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

	IUFillNumber(&PWM1N[0], "PWMout1", "%", "%0.0f", 0, 100, 10, 0);
	IUFillNumberVector(&PWM1NP, PWM1N, 1, getDeviceName(), "PWMOUT1", RelayLabelsT[2].text, OUTPUTS_TAB, IP_RW, 60, IPS_IDLE);

	IUFillNumber(&PWM2N[0], "PWMout2", "%", "%0.0f", 0, 100, 10, 0);
	IUFillNumberVector(&PWM2NP, PWM2N, 1, getDeviceName(), "PWMOUT2", RelayLabelsT[3].text, OUTPUTS_TAB, IP_RW, 60, IPS_IDLE);

	// Power readings
	IUFillNumber(&PowerReadingsN[POW_VIN], "POW_VIN", "Input voltage [V]", "%0.2f", 0, 15, 10, 0);
	IUFillNumber(&PowerReadingsN[POW_VREG], "POW_VREG", "Regulated voltage [V]", "%0.2f", 0, 15, 10, 0);
	IUFillNumber(&PowerReadingsN[POW_ITOT], "POW_ITOT", "Total current [A]", "%0.2f", 0, 20, 1, 0);
	IUFillNumber(&PowerReadingsN[POW_PTOT], "POW_PTOT", "Total power [W]", "%0.1f", 0, 200, 1, 0);
	IUFillNumber(&PowerReadingsN[POW_AH], "POW_AH", "Energy consumed [Ah]", "%0.2f", 0, 10000, 1, 0);
	IUFillNumber(&PowerReadingsN[POW_WH], "POW_WH", "Energy consumed [Wh]", "%0.2f", 0, 100000, 1, 0);
	IUFillNumberVector(&PowerReadingsNP, PowerReadingsN, 6, getDeviceName(), "POWER_READINGS", "Power readings", OUTPUTS_TAB, IP_RO, 60, IPS_IDLE);

	// Environment Group
	addParameter("WEATHER_TEMPERATURE", "Temperature [C]", -15, 35, 15);
	addParameter("WEATHER_HUMIDITY", "Humidity %", 0, 100, 15);
	addParameter("WEATHER_DEWPOINT", "Dew Point [C]", -25, 20, 15);
	addParameter("WEATHER_SKY_TEMP", "Sky temperature [C]", -50, 20, 20);
	addParameter("WEATHER_SKY_DIFF", "Temperature difference [C]", -5, 40, 10);
	addParameter("SQM_READING", "Sky brightness [mag/arcsec2]", 10, 25, 15);

	// initial values at resolution 1/1
	FocusMaxPosNP[0].setMin(1000);
	FocusMaxPosNP[0].setMax(100000);
	FocusMaxPosNP[0].setStep(1000);
	FocusMaxPosNP[0].setValue(10000);

	FocusRelPosNP[0].setMin(0);
	FocusRelPosNP[0].setMax(10000);
	FocusRelPosNP[0].setStep(100);
	FocusRelPosNP[0].setValue(100);

	FocusAbsPosNP[0].setMin(0);
	FocusAbsPosNP[0].setMax(FocusMaxPosNP[0].getValue());
	FocusAbsPosNP[0].setStep((int)FocusAbsPosNP[0].getMax() / 100);

	FocusMotionSP[FOCUS_OUTWARD].setState(ISS_ON);
	FocusMotionSP[FOCUS_INWARD].setState(ISS_OFF);
	FocusMotionSP.apply();

	return true;
}

bool AstroLink4Pi::updateProperties()
{
	INDI::DefaultDevice::updateProperties();

	if (isConnected())
	{
		FI::updateProperties();
		WI::updateProperties();

		defineProperty(&ScopeParametersNP);
		defineProperty(&FocuserTravelNP);
		defineProperty(&FocusResolutionSP);
		defineProperty(&FocusHoldSP);
		defineProperty(&FocuserInfoNP);
		defineProperty(&FocusStepDelayNP);
		defineProperty(&SysTimeTP);
		defineProperty(&SysInfoTP);
		defineProperty(&Switch1SP);
		defineProperty(&Switch2SP);
		defineProperty(&PWM1NP);
		defineProperty(&PWM2NP);
		defineProperty(&PWMcycleNP);
		defineProperty(&StepperCurrentNP);
		defineProperty(&FocusTemperatureNP);
		defineProperty(&TemperatureCoefNP);
		defineProperty(&TemperatureCompensateSP);
		defineProperty(&PowerReadingsNP);
		defineProperty(&FanPowerNP);
		defineProperty(&SQMOffsetNP);  
	}
	else
	{
		deleteProperty(SQMOffsetNP.name);
		deleteProperty(ScopeParametersNP.name);
		deleteProperty(FocuserTravelNP.name);
		deleteProperty(FocusResolutionSP.name);
		deleteProperty(FocusHoldSP.name);
		deleteProperty(FocuserInfoNP.name);
		deleteProperty(FocusStepDelayNP.name);
		deleteProperty(FocusTemperatureNP.name);
		deleteProperty(TemperatureCoefNP.name);
		deleteProperty(TemperatureCompensateSP.name);
		deleteProperty(SysTimeTP.name);
		deleteProperty(SysInfoTP.name);
		deleteProperty(Switch1SP.name);
		deleteProperty(Switch2SP.name);
		deleteProperty(PWM1NP.name);
		deleteProperty(PWM2NP.name);
		deleteProperty(PWMcycleNP.name);
		deleteProperty(StepperCurrentNP.name);
		deleteProperty(PowerReadingsNP.name);
		deleteProperty(FanPowerNP.name);
		FI::updateProperties();
		WI::updateProperties();
	}

	return true;
}

bool AstroLink4Pi::ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n)
{
	// first we check if it's for our device
	if (!strcmp(dev, getDeviceName()))
	{
		// handle scope params
		if (!strcmp(name, ScopeParametersNP.name))
		{
			ScopeParametersNP.s = IPS_BUSY;
			IUUpdateNumber(&ScopeParametersNP, values, names, n);
			IDSetNumber(&FocusStepDelayNP, nullptr);
			ScopeParametersNP.s = IPS_OK;
			IDSetNumber(&ScopeParametersNP, nullptr);
			getFocuserInfo();
			DEBUGF(INDI::Logger::DBG_SESSION, "Scope parameters set to %0.0f / %0.0f.", ScopeParametersN[SCOPE_DIAM].value, ScopeParametersN[SCOPE_FL].value);
			return true;
		}

		// handle focus step delay
		if (!strcmp(name, FocusStepDelayNP.name))
		{
			FocusStepDelayNP.s = IPS_BUSY;
			IUUpdateNumber(&FocusStepDelayNP, values, names, n);
			IDSetNumber(&FocusStepDelayNP, nullptr);
			FocusStepDelayNP.s = IPS_OK;
			IDSetNumber(&FocusStepDelayNP, nullptr);
			DEBUGF(INDI::Logger::DBG_SESSION, "Step delay set to %0.0f us.", FocusStepDelayN[0].value);
			return true;
		}

		// handle focus maximum position
		if (!strcmp(name, FocusMaxPosNP.getName()))
		{
			FocusMaxPosNP.update(values, names, n);

			FocusAbsPosNP[0].setMax(FocusMaxPosNP[0].getValue());
			FocusAbsPosNP.updateMinMax(); // This call is not INDI protocol compliant

			FocusAbsPosNP.setState(IPS_OK);
			FocusMaxPosNP.apply();
			getFocuserInfo();
			return true;
		}

		// handle temperature coefficient
		if (!strcmp(name, TemperatureCoefNP.name))
		{
			IUUpdateNumber(&TemperatureCoefNP, values, names, n);
			TemperatureCoefNP.s = IPS_OK;
			IDSetNumber(&TemperatureCoefNP, nullptr);
			DEBUGF(INDI::Logger::DBG_SESSION, "Temperature coefficient set to %0.1f steps/Â°C", TemperatureCoefN[0].value);
			return true;
		}

		// handle focuser travel
		if (!strcmp(name, FocuserTravelNP.name))
		{
			IUUpdateNumber(&FocuserTravelNP, values, names, n);
			getFocuserInfo();
			FocuserTravelNP.s = IPS_OK;
			IDSetNumber(&FocuserTravelNP, nullptr);
			DEBUGF(INDI::Logger::DBG_SESSION, "Maximum focuser travel set to %0.0f mm", FocuserTravelN[0].value);
			return true;
		}

		// handle PWMouts
		if (!strcmp(name, PWM1NP.name))
		{
			IUUpdateNumber(&PWM1NP, values, names, n);
			PWM1NP.s = IPS_OK;
			IDSetNumber(&PWM1NP, nullptr);
			lgTxPwm(pigpioHandle, PWM1_PIN, PWMcycleN[0].value, PWM1N[0].value, 0, 0);
			pwmState[0] = PWM1N[0].value;
			DEBUGF(INDI::Logger::DBG_SESSION, "PWM 1 set to %0.0f", PWM1N[0].value);
			return true;
		}

		if (!strcmp(name, PWM2NP.name))
		{
			IUUpdateNumber(&PWM2NP, values, names, n);
			PWM2NP.s = IPS_OK;
			IDSetNumber(&PWM2NP, nullptr);
			lgTxPwm(pigpioHandle, PWM2_PIN, PWMcycleN[0].value, PWM2N[0].value, 0, 0);
			pwmState[1] = PWM2N[0].value;
			DEBUGF(INDI::Logger::DBG_SESSION, "PWM 2 set to %0.0f", PWM2N[0].value);
			return true;
		}
		
        // SQM calibration
        if (!strcmp(name, SQMOffsetNP.name))
        {
            SQMOffsetNP.s = IPS_BUSY;
            IUUpdateNumber(&SQMOffsetNP, values, names, n);
            SQMOffsetNP.s = IPS_OK;
            IDSetNumber(&SQMOffsetNP, nullptr);
            return true;
        }    		

		// handle PWMcycle
		if (!strcmp(name, PWMcycleNP.name))
		{
			IUUpdateNumber(&PWMcycleNP, values, names, n);
			PWMcycleNP.s = IPS_OK;
			IDSetNumber(&PWMcycleNP, nullptr);
			lgTxPwm(pigpioHandle, PWM1_PIN, PWMcycleN[0].value, PWM1N[0].value, 0, 0);
			lgTxPwm(pigpioHandle, PWM2_PIN, PWMcycleN[0].value, PWM1N[0].value, 0, 0);
			DEBUGF(INDI::Logger::DBG_SESSION, "PWM frequency set to %0.0f Hz", PWMcycleN[0].value);
			return true;
		}

		// handle stepper current
		if (!strcmp(name, StepperCurrentNP.name))
		{
			IUUpdateNumber(&StepperCurrentNP, values, names, n);
			StepperCurrentNP.s = IPS_OK;
			IDSetNumber(&StepperCurrentNP, nullptr);
			DEBUGF(INDI::Logger::DBG_SESSION, "Stepper current set to %0.0f mA", StepperCurrentN[0].value);
			setCurrent(true);
			return true;
		}

		if (strstr(name, "FOCUS_"))
			return FI::processNumber(dev, name, values, names, n);
		if (strstr(name, "WEATHER_"))
			return WI::processNumber(dev, name, values, names, n);
	}

	return INDI::DefaultDevice::ISNewNumber(dev, name, values, names, n);
}

bool AstroLink4Pi::ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n)
{
	int rv = 0;

	// first we check if it's for our device
	if (!strcmp(dev, getDeviceName()))
	{
		// handle temperature compensation
		if (!strcmp(name, TemperatureCompensateSP.name))
		{
			IUUpdateSwitch(&TemperatureCompensateSP, states, names, n);

			if (TemperatureCompensateS[0].s == ISS_ON)
			{
				TemperatureCompensateSP.s = IPS_OK;
				DEBUG(INDI::Logger::DBG_SESSION, "Temperature compensation ENABLED.");
			}

			if (TemperatureCompensateS[1].s == ISS_ON)
			{
				TemperatureCompensateSP.s = IPS_IDLE;
				DEBUG(INDI::Logger::DBG_SESSION, "Temperature compensation DISABLED.");
			}

			IDSetSwitch(&TemperatureCompensateSP, nullptr);
			return true;
		}

		// handle relay 1
		if (!strcmp(name, Switch1SP.name))
		{
			IUUpdateSwitch(&Switch1SP, states, names, n);

			if (Switch1S[S1_ON].s == ISS_ON)
			{
				rv = lgGpioWrite(pigpioHandle, OUT1_PIN, 1);
				if (rv != 0)
				{
					DEBUG(INDI::Logger::DBG_ERROR, "Error setting AstroLink Relay #1");
					Switch1SP.s = IPS_ALERT;
					Switch1S[S1_ON].s = ISS_OFF;
					IDSetSwitch(&Switch1SP, NULL);
					return false;
				}
				relayState[0] = 1;
				DEBUG(INDI::Logger::DBG_SESSION, "AstroLink Relays #1 set to ON");
				Switch1SP.s = IPS_OK;
				Switch1S[S1_OFF].s = ISS_OFF;
				IDSetSwitch(&Switch1SP, NULL);
				return true;
			}
			if (Switch1S[S1_OFF].s == ISS_ON)
			{
				rv = lgGpioWrite(pigpioHandle, OUT1_PIN, 0);
				if (rv != 0)
				{
					DEBUG(INDI::Logger::DBG_ERROR, "Error setting AstroLink Relay #1");
					Switch1SP.s = IPS_ALERT;
					Switch1S[S1_OFF].s = ISS_OFF;
					IDSetSwitch(&Switch1SP, NULL);
					return false;
				}
				relayState[0] = 0;
				DEBUG(INDI::Logger::DBG_SESSION, "AstroLink Relays #1 set to OFF");
				Switch1SP.s = IPS_IDLE;
				Switch1S[S1_ON].s = ISS_OFF;
				IDSetSwitch(&Switch1SP, NULL);
				return true;
			}
		}

		// handle relay 2
		if (!strcmp(name, Switch2SP.name))
		{
			IUUpdateSwitch(&Switch2SP, states, names, n);

			if (Switch2S[S2_ON].s == ISS_ON)
			{
				rv = lgGpioWrite(pigpioHandle, OUT2_PIN, 1);
				if (rv != 0)
				{
					DEBUG(INDI::Logger::DBG_ERROR, "Error setting AstroLink Relay #2");
					Switch2SP.s = IPS_ALERT;
					Switch2S[S2_ON].s = ISS_OFF;
					IDSetSwitch(&Switch2SP, NULL);
					return false;
				}
				relayState[1] = 1;
				DEBUG(INDI::Logger::DBG_SESSION, "AstroLink Relays #2 set to ON");
				Switch2SP.s = IPS_OK;
				Switch2S[S2_OFF].s = ISS_OFF;
				IDSetSwitch(&Switch2SP, NULL);
				return true;
			}
			if (Switch2S[S2_OFF].s == ISS_ON)
			{
				rv = lgGpioWrite(pigpioHandle, OUT2_PIN, 0);
				if (rv != 0)
				{
					DEBUG(INDI::Logger::DBG_ERROR, "Error setting AstroLink Relay #2");
					Switch2SP.s = IPS_ALERT;
					Switch2S[S2_OFF].s = ISS_OFF;
					IDSetSwitch(&Switch2SP, NULL);
					return false;
				}
				relayState[1] = 0;
				DEBUG(INDI::Logger::DBG_SESSION, "AstroLink Relays #2 set to OFF");
				Switch2SP.s = IPS_IDLE;
				Switch2S[S2_ON].s = ISS_OFF;
				IDSetSwitch(&Switch2SP, NULL);
				return true;
			}
		}

		// handle focus motor hold
		if (!strcmp(name, FocusHoldSP.name))
		{
			IUUpdateSwitch(&FocusHoldSP, states, names, n);
			FocusHoldSP.s = IPS_OK;
			IDSetSwitch(&FocusHoldSP, nullptr);
			setCurrent(true);
			return true;
		}

		// handle focus resolution
		if (!strcmp(name, FocusResolutionSP.name))
		{
			int last_resolution = resolution;

			IUUpdateSwitch(&FocusResolutionSP, states, names, n);

			// Resolution 1/1
			if (FocusResolutionS[RES_1].s == ISS_ON)
				resolution = 1;

			// Resolution 1/2
			if (FocusResolutionS[RES_2].s == ISS_ON)
				resolution = 2;

			// Resolution 1/4
			if (FocusResolutionS[RES_4].s == ISS_ON)
				resolution = 4;

			// Resolution 1/8
			if (FocusResolutionS[RES_8].s == ISS_ON)
				resolution = 8;

			// Resolution 1/16
			if (FocusResolutionS[RES_16].s == ISS_ON)
				resolution = 16;

			// Resolution 1/32
			if (FocusResolutionS[RES_32].s == ISS_ON)
				resolution = 32;

			// Adjust position to a step in lower resolution
			int position_adjustment = last_resolution * (FocusAbsPosNP[0].getValue() / last_resolution - (int)FocusAbsPosNP[0].getValue() / last_resolution);
			if (resolution < last_resolution && position_adjustment > 0)
			{
				if ((float)position_adjustment / last_resolution < 0.5)
				{
					position_adjustment *= -1;
				}
				else
				{
					position_adjustment = last_resolution - position_adjustment;
				}
				DEBUGF(INDI::Logger::DBG_SESSION, "Focuser position adjusted by %d steps at 1/%d resolution to sync with 1/%d resolution.", position_adjustment, last_resolution, resolution);
				MoveAbsFocuser(FocusAbsPosNP[0].getValue() + position_adjustment);
			}

			SetResolution(resolution);

			// update values based on resolution
			FocusRelPosNP[0].setMin((int)FocusRelPosNP[0].getMin() * resolution / last_resolution);
			FocusRelPosNP[0].setMax((int)FocusRelPosNP[0].getMax() * resolution / last_resolution);
			FocusRelPosNP[0].setStep((int)FocusRelPosNP[0].getStep() * resolution / last_resolution);
			FocusRelPosNP[0].setValue((int)FocusRelPosNP[0].getValue() * resolution / last_resolution);
			FocusRelPosNP.apply();
			FocusRelPosNP.updateMinMax();

			FocusAbsPosNP[0].setMax((int)FocusAbsPosNP[0].getMax() * resolution / last_resolution);
			FocusAbsPosNP[0].setStep((int)FocusAbsPosNP[0].getStep() * resolution / last_resolution);
			FocusAbsPosNP[0].setValue((int)FocusAbsPosNP[0].getValue() * resolution / last_resolution);
			FocusAbsPosNP.apply();
			FocusAbsPosNP.updateMinMax();

			FocusMaxPosNP[0].setMin((int)FocusMaxPosNP[0].getMin() * resolution / last_resolution);
			FocusMaxPosNP[0].setMax((int)FocusMaxPosNP[0].getMax() * resolution / last_resolution);
			FocusMaxPosNP[0].setStep((int)FocusMaxPosNP[0].getStep() * resolution / last_resolution);
			FocusMaxPosNP[0].setValue((int)FocusMaxPosNP[0].getValue() * resolution / last_resolution);
			FocusMaxPosNP.apply();
			FocusMaxPosNP.updateMinMax();

			getFocuserInfo();

			FocusResolutionSP.s = IPS_OK;
			IDSetSwitch(&FocusResolutionSP, nullptr);
			return true;
		}

		if (strstr(name, "FOCUS"))
			return FI::processSwitch(dev, name, states, names, n);
        if (strstr(name, "WEATHER_")) 
            return WI::processSwitch(dev, name, states, names, n);
	}

	return INDI::DefaultDevice::ISNewSwitch(dev, name, states, names, n);
}

bool AstroLink4Pi::ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n)
{
	// first we check if it's for our device
	if (!strcmp(dev, getDeviceName()))
	{
		// handle relay labels
		if (!strcmp(name, RelayLabelsTP.name))
		{
			if (isConnected())
			{
				DEBUG(INDI::Logger::DBG_WARNING, "Cannot set labels while device is connected.");
				return false;
			}

			IUUpdateText(&RelayLabelsTP, texts, names, n);
			RelayLabelsTP.s = IPS_OK;
			IDSetText(&RelayLabelsTP, nullptr);
			DEBUG(INDI::Logger::DBG_SESSION, "AstroLink 4 Pi labels set . You need to save configuration and restart driver to activate the changes.");
			DEBUGF(INDI::Logger::DBG_DEBUG, "AstroLink 4 Pi labels set to OUT1: %s, OUT2: %s, PWM1: %s, PWM2: %s", RelayLabelsT[0].text, RelayLabelsT[1].text, RelayLabelsT[2].text, RelayLabelsT[3].text);

			return true;
		}
	}

	return INDI::DefaultDevice::ISNewText(dev, name, texts, names, n);
}

bool AstroLink4Pi::saveConfigItems(FILE *fp)
{
	FI::saveConfigItems(fp);
	WI::saveConfigItems(fp);
	IUSaveConfigSwitch(fp, &FocusResolutionSP);
	IUSaveConfigSwitch(fp, &FocusHoldSP);
	IUSaveConfigSwitch(fp, &TemperatureCompensateSP);
	IUSaveConfigNumber(fp, &FocusStepDelayNP);
	IUSaveConfigNumber(fp, &FocuserTravelNP);
	IUSaveConfigNumber(fp, &ScopeParametersNP);
	IUSaveConfigNumber(fp, &TemperatureCoefNP);
	IUSaveConfigNumber(fp, &PWMcycleNP);
	IUSaveConfigText(fp, &RelayLabelsTP);
	IUSaveConfigSwitch(fp, &Switch1SP);
	IUSaveConfigSwitch(fp, &Switch2SP);
	IUSaveConfigNumber(fp, &StepperCurrentNP);
	IUSaveConfigNumber(fp, &PWM1NP);
	IUSaveConfigNumber(fp, &PWM2NP);
	IUSaveConfigNumber(fp, &SQMOffsetNP);

	return true;
}

void AstroLink4Pi::TimerHit()
{
	if (!isConnected())
		return;

	long int timeMillis = millis();
	SQMavailable = readSQM(nextTemperatureRead < timeMillis);

	if (nextTemperatureRead < timeMillis)
	{
		SHTavailable = readSHT();
		MLXavailable = readMLX();

		nextTemperatureRead = timeMillis + TEMPERATURE_UPDATE_TIMEOUT;

		if (SHTavailable || MLXavailable)
		{
			FocusTemperatureN[0].value = focuserTemperature;
			FocusTemperatureNP.s = IPS_OK;
		}
		else
		{
			FocusTemperatureN[0].value = 0.0;
			FocusTemperatureNP.s = IPS_ALERT;
			IDSetNumber(&FocusTemperatureNP, nullptr);
		}
		IDSetNumber(&FocusTemperatureNP, nullptr);
	}
	if (nextTemperatureCompensation < timeMillis)
	{
		temperatureCompensation();
		nextTemperatureCompensation = timeMillis + TEMPERATURE_COMPENSATION_TIMEOUT;
	}
	if (nextSystemRead < timeMillis)
	{
		systemUpdate();
		nextSystemRead = timeMillis + SYSTEM_UPDATE_PERIOD;
	}
	if (nextFanUpdate < timeMillis)
	{
		fanUpdate();
		nextFanUpdate = timeMillis + FAN_PERIOD;
	}
	readPower();

	SetTimer(POLL_PERIOD);
}

bool AstroLink4Pi::AbortFocuser()
{
	if (_motionThread.joinable())
	{
		_abort = true;
		_motionThread.join();
	}
	DEBUG(INDI::Logger::DBG_SESSION, "Focuser motion aborted.");
	return true;
}

IPState AstroLink4Pi::MoveRelFocuser(FocusDirection dir, uint32_t ticks)
{
	uint32_t targetTicks = FocusAbsPosNP[0].getValue() + ((int32_t)ticks * (dir == FOCUS_INWARD ? -1 : 1));
	return MoveAbsFocuser(targetTicks);
}

IPState AstroLink4Pi::MoveAbsFocuser(uint32_t targetTicks)
{
	if (targetTicks < FocusAbsPosNP[0].getMin() || targetTicks > FocusAbsPosNP[0].getMax())
	{
		DEBUG(INDI::Logger::DBG_WARNING, "Requested position is out of range.");
		return IPS_ALERT;
	}

	if (targetTicks == FocusAbsPosNP[0].getValue())
	{
		DEBUG(INDI::Logger::DBG_SESSION, "Already at the requested position.");
		return IPS_OK;
	}

	// set focuser busy
	FocusAbsPosNP.setState(IPS_BUSY);
	FocusAbsPosNP.apply();
	setCurrent(false);

	// set direction
	const char *direction;
	int newDirection;
	if (targetTicks > FocusAbsPosNP[0].getValue())
	{
		// OUTWARD
		direction = "OUTWARD";
		newDirection = 1;
	}
	else
	{
		// INWARD
		direction = "INWARD";
		newDirection = -1;
	}

	// if direction changed do backlash adjustment
	if (lastDirection != 0 && newDirection != lastDirection && FocusBacklashNP[0].getValue() != 0)
	{
		DEBUGF(INDI::Logger::DBG_SESSION, "Backlash compensation by %0.0f steps.", FocusBacklashNP[0].getValue());
		backlashTicksRemaining = FocusBacklashNP[0].getValue();
	}
	else
	{
		backlashTicksRemaining = 0;
	}

	lastDirection = newDirection;

	DEBUGF(INDI::Logger::DBG_SESSION, "Focuser is moving %s to position %d.", direction, targetTicks);

	if (_motionThread.joinable())
	{
		_abort = true;
		_motionThread.join();
	}

	_abort = false;
	_motionThread = getMotorThread(targetTicks, lastDirection, pigpioHandle, backlashTicksRemaining);
	return IPS_BUSY;
}

std::thread AstroLink4Pi::getMotorThread(uint32_t targetTicks, int lastDirection, int pigpioHandle, int backlashTicksRemaining)
{
	return std::thread([this](uint32_t targetPos, int direction, int pigpioHandle, int backlashTicksRemaining)
					   {
		int motorDirection = direction;

		uint32_t currentPos = FocusAbsPosNP[0].getValue();
		while (currentPos != targetPos && !_abort)
		{
			if (currentPos % 100 == 0)
			{
				FocusAbsPosNP[0].setValue(currentPos);
				FocusAbsPosNP.setState(IPS_BUSY);
				FocusAbsPosNP.apply();
			}
			if (FocusReverseSP[INDI_ENABLED].getState() == ISS_ON)
			{
				lgGpioWrite(pigpioHandle, DIR_PIN, (motorDirection < 0) ? 1 : 0);
			}
			else
			{
				lgGpioWrite(pigpioHandle, DIR_PIN, (motorDirection < 0) ? 0 : 1);
			}
			lgGpioWrite(pigpioHandle, STP_PIN, 1);
			usleep(10);
			lgGpioWrite(pigpioHandle, STP_PIN, 0);

			if (backlashTicksRemaining <= 0)
			{ // Only Count the position change if it is not due to backlash
				currentPos += motorDirection;
			}
			else
			{ // Don't count the backlash position change, just decrement the counter
				backlashTicksRemaining -= 1;
			}
			usleep(FocusStepDelayN[0].value);
		}

		// update abspos value and status
		DEBUGF(INDI::Logger::DBG_SESSION, "Focuser moved to position %i", (int)currentPos);
		FocusAbsPosNP[0].setValue(currentPos);
		FocusAbsPosNP.setState(IPS_OK);
		FocusAbsPosNP.apply();
		FocusRelPosNP.setState(IPS_OK);
		FocusRelPosNP.apply();

		savePosition((int)FocusAbsPosNP[0].getValue() * MAX_RESOLUTION / resolution); // always save at MAX_RESOLUTION
		lastTemperature = FocusTemperatureN[0].value;							// register last temperature
		setCurrent(true); },
					   targetTicks, lastDirection, pigpioHandle, backlashTicksRemaining);
}

void AstroLink4Pi::SetResolution(int res)
{
	// Release lines
	lgGpioWrite(pigpioHandle, M0_PIN, 1);
	lgGpioWrite(pigpioHandle, M1_PIN, 1);
	lgGpioWrite(pigpioHandle, M2_PIN, 1);

	switch (res)
	{
	case 1: // 1:1

		lgGpioWrite(pigpioHandle, M0_PIN, 0);
		lgGpioWrite(pigpioHandle, M1_PIN, 0);
		lgGpioWrite(pigpioHandle, M2_PIN, 0);
		break;
	case 2: // 1:2
		lgGpioWrite(pigpioHandle, M0_PIN, 1);
		lgGpioWrite(pigpioHandle, M1_PIN, 0);
		lgGpioWrite(pigpioHandle, M2_PIN, 0);
		break;
	case 4: // 1:4
		lgGpioWrite(pigpioHandle, M0_PIN, 0);
		lgGpioWrite(pigpioHandle, M1_PIN, 1);
		lgGpioWrite(pigpioHandle, M2_PIN, 0);
		break;
	case 8: // 1:8
		lgGpioWrite(pigpioHandle, M0_PIN, 1);
		lgGpioWrite(pigpioHandle, M1_PIN, 1);
		lgGpioWrite(pigpioHandle, M2_PIN, 0);
		break;
	case 16: // 1:16
		lgGpioWrite(pigpioHandle, M0_PIN, 0);
		lgGpioWrite(pigpioHandle, M1_PIN, 0);
		lgGpioWrite(pigpioHandle, M2_PIN, 1);
		break;
	case 32: // 1:32
		lgGpioWrite(pigpioHandle, M0_PIN, 1);
		lgGpioWrite(pigpioHandle, M1_PIN, 1);
		lgGpioWrite(pigpioHandle, M2_PIN, 1);
		break;
	default: // 1:1
		lgGpioWrite(pigpioHandle, M0_PIN, 0);
		lgGpioWrite(pigpioHandle, M1_PIN, 0);
		lgGpioWrite(pigpioHandle, M2_PIN, 0);

		break;
	}

	DEBUGF(INDI::Logger::DBG_SESSION, "Resolution set to 1 / %d.", res);
}

bool AstroLink4Pi::ReverseFocuser(bool enabled)
{
	if (enabled)
	{
		DEBUG(INDI::Logger::DBG_SESSION, "Reverse direction ENABLED.");
	}
	else
	{
		DEBUG(INDI::Logger::DBG_SESSION, "Reverse direction DISABLED.");
	}
	return true;
}

int AstroLink4Pi::savePosition(int pos)
{
	FILE *pFile;
	char posFileName[MAXRBUF];
	char buf[100];

	if (getenv("INDICONFIG"))
	{
		snprintf(posFileName, MAXRBUF, "%s.position", getenv("INDICONFIG"));
	}
	else
	{
		snprintf(posFileName, MAXRBUF, "%s/.indi/%s.position", getenv("HOME"), getDeviceName());
	}

	if (pos == -1)
	{
		pFile = fopen(posFileName, "r");
		if (pFile == NULL)
		{
			DEBUGF(INDI::Logger::DBG_ERROR, "Failed to open file %s.", posFileName);
			return -1;
		}

		if (fgets(buf, 100, pFile) == NULL)
		{
			DEBUGF(INDI::Logger::DBG_ERROR, "Failed to read file %s.", posFileName);
			return -1;
		}
		else
		{
			pos = atoi(buf);
			DEBUGF(INDI::Logger::DBG_DEBUG, "Reading position %d from %s.", pos, posFileName);
		}
	}
	else
	{
		pFile = fopen(posFileName, "w");
		if (pFile == NULL)
		{
			DEBUGF(INDI::Logger::DBG_ERROR, "Failed to open file %s.", posFileName);
			return -1;
		}

		sprintf(buf, "%d", pos);
		fputs(buf, pFile);
		DEBUGF(INDI::Logger::DBG_DEBUG, "Writing position %s to %s.", buf, posFileName);
	}

	fclose(pFile);

	return pos;
}

bool AstroLink4Pi::SyncFocuser(uint32_t ticks)
{
	FocusAbsPosNP[0].setValue(ticks);
	FocusAbsPosNP.apply();
	savePosition(ticks);

	DEBUGF(INDI::Logger::DBG_SESSION, "Absolute Position reset to %0.0f", FocusAbsPosNP[0].getValue());

	return true;
}

bool AstroLink4Pi::SetFocuserBacklash(int32_t steps)
{
	DEBUGF(INDI::Logger::DBG_SESSION, "Backlash set to %i steps", steps);
	return true;
}

bool AstroLink4Pi::SetFocuserMaxPosition(uint32_t ticks)
{
	DEBUGF(INDI::Logger::DBG_SESSION, "Max position set to %i steps", ticks);
	return true;
}

void AstroLink4Pi::temperatureCompensation()
{
	if (!isConnected())
		return;

	if (TemperatureCompensateS[0].s == ISS_ON && FocusTemperatureN[0].value != lastTemperature)
	{
		float deltaTemperature = FocusTemperatureN[0].value - lastTemperature; // change of temperature from last focuser movement
		float deltaPos = TemperatureCoefN[0].value * deltaTemperature;

		// Move focuser once the compensation is larger than 1/2 CFZ
		if (abs(deltaPos) > (FocuserInfoN[2].value / 2))
		{
			int thermalAdjustment = round(deltaPos);				   // adjust focuser by half number of steps to keep it in the center of cfz
			MoveAbsFocuser(FocusAbsPosNP[0].getValue() + thermalAdjustment); // adjust focuser position
			lastTemperature = FocusTemperatureN[0].value;			   // register last temperature
			DEBUGF(INDI::Logger::DBG_SESSION, "Focuser adjusted by %d steps due to temperature change by %0.2fÂ°C", thermalAdjustment, deltaTemperature);
		}
	}
}

int AstroLink4Pi::getHoldPower()
{
	if (FocusHoldS[HOLD_20].s == ISS_ON)
		return 1;
	if (FocusHoldS[HOLD_40].s == ISS_ON)
		return 2;
	if (FocusHoldS[HOLD_60].s == ISS_ON)
		return 3;
	if (FocusHoldS[HOLD_80].s == ISS_ON)
		return 4;
	if (FocusHoldS[HOLD_100].s == ISS_ON)
		return 5;
	return 0;
}

void AstroLink4Pi::setCurrent(bool standby)
{
	if (!isConnected())
		return;

	if (standby)
	{
		lgGpioWrite(pigpioHandle, EN_PIN, (getHoldPower() > 0) ? 0 : 1);
		lgGpioWrite(pigpioHandle, DECAY_PIN, 0);

		if (revision < 4)
		{
			// for 0.1 ohm resistor Vref = iref / 2
			setDac(0, 255 * (getHoldPower() * StepperCurrentN[0].value / 5) / 4096);
		}
		if (revision >= 4)
		{
			lgTxPwm(pigpioHandle, MOTOR_PWM, 5000, getMotorPWM(getHoldPower() * StepperCurrentN[0].value / 5), 0, 0);
		}

		if (getHoldPower() > 0)
		{
			DEBUGF(INDI::Logger::DBG_SESSION, "Stepper motor enabled %d %%.", getHoldPower() * 20);
		}
		else
		{
			DEBUG(INDI::Logger::DBG_SESSION, "Stepper motor disabled.");
		}
	}
	else
	{
		lgGpioWrite(pigpioHandle, EN_PIN, 0);
		lgGpioWrite(pigpioHandle, DECAY_PIN, 1);
		if (revision < 4)
		{
			DEBUGF(INDI::Logger::DBG_SESSION, "Stepper current %0.2f", StepperCurrentN[0].value);
			// for 0.1 ohm resistor Vref = iref / 2
			setDac(0, 255 * StepperCurrentN[0].value / 4096);
		}
		if (revision >= 4)
		{
			lgTxPwm(pigpioHandle, MOTOR_PWM, 5000, getMotorPWM(StepperCurrentN[0].value), 0, 0);
		}
	}
}

void AstroLink4Pi::systemUpdate()
{
	// update time
	struct tm *local_timeinfo;
	static char ts[32];
	time_t rawtime;
	time(&rawtime);
	local_timeinfo = localtime(&rawtime);
	strftime(ts, 20, "%Y-%m-%dT%H:%M:%S", local_timeinfo);
	IUSaveText(&SysTimeT[SYST_TIME], ts);
	snprintf(ts, sizeof(ts), "%4.2f", (local_timeinfo->tm_gmtoff / 3600.0));
	IUSaveText(&SysTimeT[SYST_OFFSET], ts);
	SysTimeTP.s = IPS_OK;
	IDSetText(&SysTimeTP, NULL);

	SysInfoTP.s = IPS_BUSY;
	IDSetText(&SysInfoTP, NULL);

	FILE *pipe;
	char buffer[128];

	// update CPU temp
	pipe = popen("echo $(($(cat /sys/class/thermal/thermal_zone0/temp)/1000))", "r");
	if (fgets(buffer, 128, pipe) != NULL)
		IUSaveText(&SysInfoT[SYSI_CPUTEMP], buffer);
	pclose(pipe);

	// update uptime
	pipe = popen("uptime|awk -F, '{print $1}'|awk -Fup '{print $2}'|xargs", "r");
	if (fgets(buffer, 128, pipe) != NULL)
		IUSaveText(&SysInfoT[SYSI_UPTIME], buffer);
	pclose(pipe);

	// update load
	pipe = popen("uptime|awk -F, '{print $3\" /\"$4\" /\"$5}'|awk -F: '{print $2}'|xargs", "r");
	if (fgets(buffer, 128, pipe) != NULL)
		IUSaveText(&SysInfoT[SYSI_LOAD], buffer);
	pclose(pipe);

	SysInfoTP.s = IPS_OK;
	IDSetText(&SysInfoTP, NULL);
}

void AstroLink4Pi::getFocuserInfo()
{
	// https://www.innovationsforesight.com/education/how-much-focus-error-is-too-much/
	float travel_mm = (float)FocuserTravelN[0].value;
	float aperture = (float)ScopeParametersN[SCOPE_DIAM].value;
	float focal = (float)ScopeParametersN[SCOPE_FL].value;
	float f_ratio;

	// handle no snooping data from telescope
	if (aperture * focal != 0)
	{
		f_ratio = focal / aperture;
	}
	else
	{
		f_ratio = 0;
		DEBUG(INDI::Logger::DBG_DEBUG, "No telescope focal length and/or aperture info available.");
	}

	float cfz = 4.88 * 0.520 * pow(f_ratio, 2); // CFZ = 4.88 Â· Î» Â· f^2
	float step_size = 1000.0 * travel_mm / FocusMaxPosNP[0].getValue();
	float steps_per_cfz = (int)cfz / step_size;

	if (steps_per_cfz >= 4)
	{
		FocuserInfoNP.s = IPS_OK;
	}
	else if (steps_per_cfz > 2 && steps_per_cfz < 4)
	{
		FocuserInfoNP.s = IPS_BUSY;
	}
	else
	{
		FocuserInfoNP.s = IPS_ALERT;
	}

	FocuserInfoN[FOC_STEP_SIZE].value = step_size;
	FocuserInfoN[FOC_CFZ].value = cfz;
	FocuserInfoN[FOC_STEPS_CFZ].value = steps_per_cfz;
	IDSetNumber(&FocuserInfoNP, nullptr);

	DEBUGF(INDI::Logger::DBG_DEBUG, "Focuser Info: %0.2f %0.2f %0.2f.", FocuserInfoN[0].value, FocuserInfoN[1].value, FocuserInfoN[2].value);
}

long int AstroLink4Pi::millis()
{
	static uint64_t nsec_zero = lguTimestamp();
	int millis = (int)((lguTimestamp() - nsec_zero) / 1000000);
	return millis;
}

int AstroLink4Pi::getMotorPWM(int current)
{
	// 100 = 1.03V = 2.06A, 1 = 20mA
	return current / 20;
}

int AstroLink4Pi::setDac(int chan, int value)
{
	char spiData[2];
	uint8_t chanBits, dataBits;

	if (chan == 0)
		chanBits = 0x30;
	else
		chanBits = 0xB0;

	chanBits |= ((value >> 4) & 0x0F);
	dataBits = ((value << 4) & 0xF0);

	spiData[0] = chanBits;
	spiData[1] = dataBits;

	int spiHandle = lgSpiOpen(pigpioHandle, 1, 100000, 0);
	int written = lgSpiWrite(spiHandle, spiData, 2);
	lgSpiClose(spiHandle);

	return written;
}

void AstroLink4Pi::fanUpdate()
{
	FanPowerNP.s = IPS_BUSY;
	int fanPinAvailable = lgGpioClaimOutput(pigpioHandle, 0, FAN_PIN, 0);
	if (fanPinAvailable == 0)
	{
		int temp = std::stoi(SysInfoT[SYSI_CPUTEMP].text);
		int cycle = 0;
		double fanPwr = 33.0;
		if (temp > 65)
		{
			cycle = 50;
			fanPwr = 66.0;
		}
		if (temp > 70)
		{
			cycle = 100;
			fanPwr = 100.0;
		}
		lgTxPwm(pigpioHandle, FAN_PIN, 100, cycle, 0, 0);
		FanPowerN[0].value = fanPwr;
		FanPowerNP.s = IPS_OK;
	}
	else
	{
		FanPowerNP.s = IPS_ALERT;
		DEBUGF(INDI::Logger::DBG_SESSION, "GPIO fan pin not available %d\n", fanPinAvailable);
	}
	IDSetNumber(&FanPowerNP, nullptr);
}

bool AstroLink4Pi::readSQM(bool triggerOldSensor)
{
	SQMavailable = readTSL() || (triggerOldSensor && readOLD());
	return SQMavailable;
}

bool AstroLink4Pi::readTSL()
{
	bool available = false;
	int i2cHandle = lgI2cOpen(1, TSL2591_ADDR, 0);
	
	if(i2cHandle < 0)
	{
		TSLmode = TSL_NOTAVAILABLE;
		return false;
	}
	
	if(TSLmode == TSL_NOTAVAILABLE) 
	{
		int write = lgI2cWriteByte(i2cHandle, 0x80 | 0x20 | 0x12);
		if(write == 0) {
			TSLmode = TSL_AVAILABLE;
			available = true;
		}
	}
	else if(TSLmode == TSL_AVAILABLE)
	{
		int write = lgI2cWriteByte(i2cHandle, TSL2591_COMMAND_BIT | TSL2591_REGISTER_ENABLE);
        write += lgI2cWriteByte(i2cHandle, TSL2591_ENABLE_POWERON | TSL2591_ENABLE_AEN | TSL2591_ENABLE_AIEN);

		// Enable device - power down mode on boot
        write += lgI2cWriteByte(i2cHandle, TSL2591_COMMAND_BIT | TSL2591_REGISTER_CONTROL); 
        write += lgI2cWriteByte(i2cHandle, 0x05 | 0x30); 
        
        write += lgI2cWriteByte(i2cHandle, TSL2591_COMMAND_BIT | TSL2591_REGISTER_ENABLE); 
        write += lgI2cWriteByte(i2cHandle, TSL2591_ENABLE_POWEROFF); 
        
		TSLmode = (write == 0) ? TSL_INITIALIZED : TSL_NOTAVAILABLE;
		available = (write == 0);
	}
	else if(TSLmode == TSL_INITIALIZED)
	{
		if(adcStartTime == 0)
		{
			int write = lgI2cWriteByte(i2cHandle, TSL2591_COMMAND_BIT | TSL2591_REGISTER_ENABLE);
			write += lgI2cWriteByte(i2cHandle, TSL2591_ENABLE_POWERON | TSL2591_ENABLE_AEN | TSL2591_ENABLE_AIEN);	
			adcStartTime = millis();
			TSLmode = (write == 0) ? TSL_INITIALIZED : TSL_NOTAVAILABLE;
			available = (write == 0);
		}
		else if(millis() > (adcStartTime + TSL2591_ADC_TIME))
		{
			int ir = lgI2cReadWordData(i2cHandle, TSL2591_COMMAND_BIT | TSL2591_REGISTER_CHAN1_LOW);
			int full = lgI2cReadWordData(i2cHandle, TSL2591_COMMAND_BIT | TSL2591_REGISTER_CHAN0_LOW);

	        int write = lgI2cWriteByte(i2cHandle, TSL2591_COMMAND_BIT | TSL2591_REGISTER_ENABLE); 
			write += lgI2cWriteByte(i2cHandle, TSL2591_ENABLE_POWEROFF); 	
			adcStartTime = 0;	

			int visCumulative = fullCumulative - irCumulative;
			if(full < ir) return true;
			if(niter < 5 || (visCumulative < 500 && niter < 150))
			{
				niter++;
				fullCumulative += full;
				irCumulative += ir;
			}
			else
			{
				double VIS = (double) visCumulative / (29628.0 * niter);
				double mpsas = 12.6 - 1.086 * log(VIS) + SQMOffsetN[0].value + FILTER_COEFF;
				setParameterValue("SQM_READING", mpsas);
				
				niter = 0;
				irCumulative = fullCumulative = 0;
			}	

			TSLmode = (write == 0) ? TSL_INITIALIZED : TSL_NOTAVAILABLE;
			available = (write == 0);
		}
	}	
	lgI2cClose(i2cHandle);
	return available;
}

bool AstroLink4Pi::readOLD()
{
	char i2cData[7];
	int i2cHandle = lgI2cOpen(1, 0x33, 0);
	if (i2cHandle >= 0)
	{
		int read = lgI2cReadDevice(i2cHandle, i2cData, 7);
		lgI2cClose(i2cHandle);
		if (read > 6)
		{
			int sqm = i2cData[5] * 256 + i2cData[6];
			setParameterValue("SQM_READING", 0.01 * sqm);
			// DEBUGF(INDI::Logger::DBG_SESSION, "SQM read %i %i", i2cData[5], i2cData[6]);
			return true;
		}
	}	
	return false;
}

bool AstroLink4Pi::readMLX()
{
	int i2cHandle = lgI2cOpen(1, 0x5A, 0);
	if (i2cHandle >= 0)
	{
		int Tamb = lgI2cReadWordData(i2cHandle, 0x06);
		int Tobj = lgI2cReadWordData(i2cHandle, 0x07);
		lgI2cClose(i2cHandle);
		if (Tamb >= 0 && Tobj >= 0)
		{
			setParameterValue("WEATHER_SKY_TEMP", 0.02 * Tobj - 273.15);
			setParameterValue("WEATHER_SKY_DIFF", 0.02 * (Tobj - Tamb));
			if (!SHTavailable)
				focuserTemperature = 0.02 * Tamb - 273.15;
			MLXavailable = true;
		}
		else
		{
			DEBUG(INDI::Logger::DBG_DEBUG, "Cannot read data from MLX sensor.");
			MLXavailable = false;
		}
	}
	else
	{
		DEBUG(INDI::Logger::DBG_DEBUG, "No MLX sensor found.");
		MLXavailable = false;
	}

	if (!MLXavailable)
	{
		setParameterValue("WEATHER_SKY_TEMP", 0.0);
		setParameterValue("WEATHER_SKY_DIFF", 0.0);
	}

	return MLXavailable;
}

bool AstroLink4Pi::readSHT()
{
	char i2cData[6];
	char i2cWrite[2];

	int i2cHandle = lgI2cOpen(1, 0x44, 0);
	if (i2cHandle >= 0)
	{
		i2cWrite[0] = 0x24;
		i2cWrite[1] = 0x00;
		int written = lgI2cWriteDevice(i2cHandle, i2cWrite, 2);
		if (written == 0)
		{
			usleep(30000);
			int read = lgI2cReadDevice(i2cHandle, i2cData, 6);

			if (read > 4)
			{
				int temp = i2cData[0] * 256 + i2cData[1];
				double cTemp = -45.0 + (175.0 * temp / 65535.0);
				double humidity = 100.0 * (i2cData[3] * 256.0 + i2cData[4]) / 65535.0;

				double a = 17.271;
				double b = 237.7;
				double tempAux = (a * cTemp) / (b + cTemp) + log(humidity * 0.01);
				double Td = (b * tempAux) / (a - tempAux);

				setParameterValue("WEATHER_TEMPERATURE", cTemp);
				setParameterValue("WEATHER_HUMIDITY", humidity);
				setParameterValue("WEATHER_DEWPOINT", Td);
				focuserTemperature = cTemp;
				SHTavailable = true;
			}
		}
		else
		{
			DEBUG(INDI::Logger::DBG_DEBUG, "Cannot write data to SHT sensor");
			SHTavailable = false;
		}
		lgI2cClose(i2cHandle);
	}
	else
	{
		DEBUG(INDI::Logger::DBG_DEBUG, "No SHT sensor found.");
		SHTavailable = false;
	}

	if (!SHTavailable)
	{
		setParameterValue("WEATHER_TEMPERATURE", 0.0);
		setParameterValue("WEATHER_HUMIDITY", 0.0);
		setParameterValue("WEATHER_DEWPOINT", 0.0);
	}
	return SHTavailable;
}

bool AstroLink4Pi::readPower()
{
	if (revision < 4)
		return false;

	char writeBuf[3];
	char readBuf[2];

	int i2cHandle = lgI2cOpen(1, 0x48, 0);
	if (i2cHandle >= 0)
	{
		/*
		powerIndex 0-1 Vin WR, 2-3 Vreg WR, 4-5 Itot WR

		15 		- 1 	start single conv
		14:12	- 100 	Vin, 101 Vreg, 110 Itot, 111 Iref, 011 Ireal
		11:9  	- 001	+-4.096V
		8		- 1 single

		7:5		- 010 32SPS, 011 64SPS, 001 16SPS
		4:2		- 000 comparator
		1:0		- 11 comparator disable
		*/

		writeBuf[0] = 0x01;
		writeBuf[1] = 0b11000011;
		writeBuf[2] = 0b00100011;
		if ((powerIndex % 2) == 0) // Trigger conversion
		{
			switch (powerIndex)
			{
			case 0:
				writeBuf[1] = 0b11000011;
				break;
			case 2:
				writeBuf[1] = 0b11010011;
				break;
			case 4:
				writeBuf[1] = 0b10110011;
				break;
			}
			int written = lgI2cWriteDevice(i2cHandle, writeBuf, 3);
			if (written != 0)
			{
				DEBUG(INDI::Logger::DBG_DEBUG, "Cannot write data to power sensor");
				PowerReadingsNP.s = IPS_ALERT;
			}
		}
		else // Trigger read
		{
			PowerReadingsNP.s = IPS_BUSY;

			writeBuf[0] = 0x00;
			int written = lgI2cWriteDevice(i2cHandle, writeBuf, 1);
			if (written == 0)
			{
				int read = lgI2cReadDevice(i2cHandle, readBuf, 2);
				if (read > 0)
				{
					int16_t val = readBuf[0] * 255 + readBuf[1];

					switch (powerIndex)
					{
					case 1:
						PowerReadingsN[POW_VIN].value = (float)val / 32768.0 * 4.096 * 6.6;
						break;
					case 3:
						PowerReadingsN[POW_VREG].value = (float)val / 32768.0 * 4.096 * 6.6;
						break;
					case 5:
						PowerReadingsN[POW_ITOT].value = (float)val / 32768.0 * 4.096 * 1 * ((ACS_TYPE == 0) ? 20 : 10.8);
						break;
					}
					PowerReadingsN[POW_PTOT].value = PowerReadingsN[POW_VIN].value * PowerReadingsN[POW_ITOT].value;
					energyAs += PowerReadingsN[POW_ITOT].value * 0.4;
					energyWs += PowerReadingsN[POW_VIN].value * PowerReadingsN[POW_ITOT].value * 0.4;
					PowerReadingsN[POW_AH].value = energyAs / 3600;
					PowerReadingsN[POW_WH].value = energyWs / 3600;

					PowerReadingsNP.s = IPS_OK;
				}
				else
				{
					DEBUG(INDI::Logger::DBG_DEBUG, "Cannot read data from power sensor");
					PowerReadingsNP.s = IPS_ALERT;
				}
			}
			else
			{
				DEBUG(INDI::Logger::DBG_DEBUG, "Cannot write data to power sensor");
				PowerReadingsNP.s = IPS_ALERT;
			}
		}
		powerIndex++;
		if (powerIndex > 5)
			powerIndex = 0;

		lgI2cClose(i2cHandle);
		IDSetNumber(&PowerReadingsNP, nullptr);
		return true;
	}
	else
	{
		DEBUG(INDI::Logger::DBG_DEBUG, "No power sensor found.");
		return false;
	}
}

int AstroLink4Pi::checkRevision()
{
	int handle = lgGpiochipOpen(RP5_GPIO);

	if (handle < 0)
	{
		handle = lgGpiochipOpen(RP4_GPIO);
		if (handle < 0)
			DEBUG(INDI::Logger::DBG_SESSION, "Neither RPi4 nor RPi5 GPIO was detected.\n");
		else
			gpioType = RP4_GPIO;
	}
	else
	{
		gpioType = RP5_GPIO;
	}

	lgChipInfo_t cInfo;
	int status = lgGpioGetChipInfo(handle, &cInfo);

	if (status == LG_OKAY)
	{
		DEBUGF(INDI::Logger::DBG_SESSION, "GPIO chip lines=%d name=%s label=%s\n", cInfo.lines, cInfo.name, cInfo.label);
		pigpioHandle = handle;
	}

	int spiHandle = lgSpiOpen(pigpioHandle, 1, 100000, 0);
	if (spiHandle >= 0)
	{
		DEBUG(INDI::Logger::DBG_SESSION, "SPI bus active.\n");
		lgSpiClose(spiHandle);
	}
	int i2cHandle = lgI2cOpen(1, 0x68, 0);
	if (i2cHandle >= 0)
	{
		DEBUG(INDI::Logger::DBG_SESSION, "I2C bus active.\n");
		lgI2cClose(i2cHandle);
	}

	int rev = 1;
	lgGpioClaimInput(handle, 0, MOTOR_PWM);	 // OLD CHK_PIN
	lgGpioClaimInput(handle, 0, CHK_IN_PIN); // OLD CHK2_PIN

	setDac(1, 0);
	if (lgGpioRead(handle, MOTOR_PWM) == 0)
	{
		setDac(1, 255);
		if (lgGpioRead(handle, MOTOR_PWM) == 1)
			rev = 2;
	}

	setDac(1, 0);
	if (lgGpioRead(handle, CHK_IN_PIN) == 0)
	{
		setDac(1, 255);
		if (lgGpioRead(handle, CHK_IN_PIN) == 1)
			rev = 3;
	}

	lgGpioClaimOutput(handle, 0, MOTOR_PWM, 0);
	if (rev == 1)
	{
		if (lgGpioRead(handle, CHK_IN_PIN) == 0)
		{
			lgGpioWrite(handle, MOTOR_PWM, 1);			// pin20
			if (lgGpioRead(handle, CHK_IN_PIN) == 1)	// pin16
			{
				rev = 4;
			}
		}
	}
	lgGpioFree(handle, MOTOR_PWM);
	lgGpioFree(handle, CHK_IN_PIN);

	if (handle >= 0)
		lgGpiochipClose(handle);

	DEBUGF(INDI::Logger::DBG_SESSION, "AstroLink 4 Pi revision %d detected", rev);
	return rev;
}
