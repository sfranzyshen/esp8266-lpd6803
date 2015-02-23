/*
 * lpd6803.c
 *
 *  Created on: 13 Dec 2014 
 *      Author: Aleksey Kudakov <popsodav@gmail.com>
 */
//#include "espmissingincludes.h"
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "lpd6803.h"

uint16_t numLEDs = maxLEDs; // max rgb leds

static uint16_t lpd6803_pixels[maxLEDs];
static uint16_t lpd6803_pixels_r[maxLEDs];
static uint16_t lpd6803_pixels_g[maxLEDs];
static uint16_t lpd6803_pixels_b[maxLEDs];

static int lpd6803_SendMode; // Used in interrupt 0=start,1=header,2=data,3=data done
static uint32_t lpd6803_BitCount;   // Used in interrupt
static uint16_t lpd6803_LedIndex; // Used in interrupt - Which LED we are sending.
static uint32_t lpd6803_BlankCounter;  //Used in interrupt.

static uint32_t lpd6803_lastdata = 0;
static uint16_t lpd6803_swapAsap = 0; //flag to indicate that the colors need an update asap
static uint8_t lpd6803_mode = 0;

//tpm2net mode variables
uint16_t lastframe_counter = 0; //counter for tpm2net no-frame recieved timeout
uint16_t tpm2net_timeout = 3000; // how long to wait until we consider the tpm2net has timedout

//Running Pixel and Running Line modes variables
static uint16_t lpd6803_mode_rp_CurrentPixel = 0;
static uint16_t lpd6803_mode_rp_PixelColor = 0;
static bool lpd6803_mode_rl_initialPixel = true;

//Rainbow modes variables
static uint16_t lpd6803_mode_rainbow_j = 0;

static os_timer_t modeTimer;
static os_timer_t lpd6803_timer;

void ICACHE_FLASH_ATTR lpd6803_LedOut() {
	switch (lpd6803_SendMode) {
	case LPD6803_DONE:            //Done..just send clocks with zero data
		if (lpd6803_swapAsap > 0) {
			if (!lpd6803_BlankCounter) //AS SOON AS CURRENT pwm IS DONE. BlankCounter
			{
				lpd6803_BitCount = 0;
				lpd6803_LedIndex = lpd6803_swapAsap;  //set current led
				lpd6803_SendMode = LPD6803_HEADER;
				lpd6803_swapAsap = 0;
				lpd6803_BlankCounter = 0;
			}
		}
		break;

	case LPD6803_DATA:               //Sending Data
		if ((1 << (15 - lpd6803_BitCount)) & lpd6803_pixels[lpd6803_LedIndex]) {
			if (!lpd6803_lastdata) { // digitalwrites take a long time, avoid if possible
				// If not the first bit then output the next bits
				// (Starting with MSB bit 15 down.)
				GPIO_OUTPUT_SET(0, 1);
				lpd6803_lastdata = 1;
			}
		} else {
			if (lpd6803_lastdata) { // digitalwrites take a long time, avoid if possible
				GPIO_OUTPUT_SET(0, 0);
				lpd6803_lastdata = 0;
			}
		}
		lpd6803_BitCount++;

		if (lpd6803_BitCount == 16)    //Last bit?
				{
			lpd6803_LedIndex++;        //Move to next LED
			if (lpd6803_LedIndex < numLEDs) //Still more leds to go or are we done?
			{
				lpd6803_BitCount = 0;  //Start from the fist bit of the next LED
			} else {
				// no longer sending data, set the data pin low
				GPIO_OUTPUT_SET(0, 0);
				lpd6803_lastdata = 0;      // this is a lite optimization
				lpd6803_SendMode = LPD6803_DONE; //No more LEDs to go, we are done!
			}
		}
		break;
	case LPD6803_HEADER:            //Header
		if (lpd6803_BitCount < 32) {
			GPIO_OUTPUT_SET(0, 0);
			lpd6803_lastdata = 0;
			lpd6803_BitCount++;
			if (lpd6803_BitCount == 32) {
				lpd6803_SendMode = LPD6803_DATA; //If this was the last bit of header then move on to data.
				lpd6803_LedIndex = 0;
				lpd6803_BitCount = 0;
			}
		}
		break;
	case LPD6803_START:            //Start
		if (!lpd6803_BlankCounter) //AS SOON AS CURRENT pwm IS DONE. BlankCounter
		{
			lpd6803_BitCount = 0;
			lpd6803_LedIndex = 0;
			lpd6803_SendMode = LPD6803_HEADER;
		}
		break;
	}

	// Clock out data (or clock LEDs)
	GPIO_OUTPUT_SET(2, 1);            //High
	GPIO_OUTPUT_SET(2, 0);            //Low

	//Keep track of where the LEDs are at in their pwm cycle.
	lpd6803_BlankCounter++;
}

void ICACHE_FLASH_ATTR lpd6803_setPixelColor(uint16_t n, uint8_t r, uint8_t g, uint8_t b) {
	uint16_t data;

	if (n > numLEDs)
		return;

	lpd6803_pixels_r[n] = r;
	lpd6803_pixels_g[n] = g;
	lpd6803_pixels_b[n] = b;

	data = r & 0x1F;
	data <<= 5;
	data |= g & 0x1F;
	data <<= 5;
	data |= b & 0x1F;
	data |= 0x8000;

	lpd6803_pixels[n] = data;
}

void ICACHE_FLASH_ATTR lpd6803_setPixelColorByColor(uint16_t n, uint16_t c) {
	if (n > numLEDs)
		return;

	lpd6803_pixels[n] = 0x8000 | c;
}

void ICACHE_FLASH_ATTR lpd6803_setAllPixelColor(uint8_t r, uint8_t g, uint8_t b) {
	uint16_t i;
	for (i = 0; i < numLEDs; i++) {
		lpd6803_setPixelColor(i, r, g, b);
	}

}

uint8_t ICACHE_FLASH_ATTR lpd6803_getMode() {
	return lpd6803_mode;
}

uint16_t ICACHE_FLASH_ATTR lpd6803_getPixelColorR(uint16_t n) {
	if (n > numLEDs)
		return 0;
	return lpd6803_pixels_r[n];
}

uint16_t ICACHE_FLASH_ATTR lpd6803_getPixelColorG(uint16_t n) {
	if (n > numLEDs)
		return 0;
	return lpd6803_pixels_g[n];
}

uint16_t ICACHE_FLASH_ATTR lpd6803_getPixelColorB(uint16_t n) {
	if (n > numLEDs)
		return 0;
	return lpd6803_pixels_b[n];
}

void ICACHE_FLASH_ATTR lpd6803_init() {
	gpio_init();
	//Set GPIO2 to output mode for CLCK
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
	GPIO_OUTPUT_SET(2, 0);

	//Set GPIO0 to output mode for DATA
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0);
	GPIO_OUTPUT_SET(0, 0);

	uint16_t i;
	for (i = 0; i < numLEDs; i++) {
		lpd6803_setPixelColor(i, 0, 0, 0);
	}

	os_timer_disarm(&lpd6803_timer);
	os_timer_setfn(&lpd6803_timer, (os_timer_func_t *) lpd6803_LedOut, NULL);
	os_timer_arm_us(&lpd6803_timer, 40, 1);
}

void ICACHE_FLASH_ATTR lpd6803_show(void) {
	lpd6803_BitCount = lpd6803_LedIndex = lpd6803_BlankCounter = 0;
	lpd6803_SendMode = LPD6803_START;
}

// Create a 15 bit color value from R,G,B
unsigned int ICACHE_FLASH_ATTR lpd6803_Color(uint8_t r, uint8_t g, uint8_t b) {
	//Take the lowest 5 bits of each value and append them end to end
	return (((unsigned int) r & 0x1F) << 10 | ((unsigned int) g & 0x1F) << 5
			| ((unsigned int) b & 0x1F));
}

unsigned int ICACHE_FLASH_ATTR lpd6803_Wheel(uint8_t WheelPos) {
	uint8_t r = 0, g = 0, b = 0;
	switch (WheelPos >> 5) {
	case 0:
		r = 31 - WheelPos % 32;   //Red down
		g = WheelPos % 32;      // Green up
		b = 0;                  //blue off
		break;
	case 1:
		g = 31 - WheelPos % 32;  //green down
		b = WheelPos % 32;      //blue up
		r = 0;                  //red off
		break;
	case 2:
		b = 31 - WheelPos % 32;  //blue down
		r = WheelPos % 32;      //red up
		g = 0;                  //green off
		break;
	}
	return (lpd6803_Color(r, g, b));
}

void ICACHE_FLASH_ATTR lpd6803_loop() {
	switch (lpd6803_mode) {
	case (LPD6803_MODE_RUNNING_PIXEL):
		lpd6803_RunningPixel_loop();
		break;
	case (LPD6803_MODE_RUNNING_LINE):
		lpd6803_RunningLine_loop();
		break;
	case (LPD6803_MODE_RAINBOW):
		lpd6803_Rainbow_loop();
		break;
	case (LPD6803_MODE_RAINBOW2):
		lpd6803_Rainbow2_loop();
		break;
	case (LPD6803_MODE_SNOW):
		lpd6803_Snow_loop();
		break;
	case (LPD6803_MODE_RGB):
		lpd6803_RGB_loop();
		break;
	case (LPD6803_MODE_TPM2NET):
		lpd6803_tpm2net_loop();
		break;
	}
}

void ICACHE_FLASH_ATTR lpd6803_tpm2net_loop() {
	lastframe_counter += 1; 
	if (lastframe_counter > tpm2net_timeout) { // tpm2net source timedout
		lpd6803_startRainbow();
	} 
}

void ICACHE_FLASH_ATTR lpd6803_starttpm2net() {
	lpd6803_mode = LPD6803_MODE_TPM2NET;
	os_timer_disarm(&modeTimer);
	os_timer_setfn(&modeTimer, (os_timer_func_t *) lpd6803_loop, NULL);
	os_timer_arm(&modeTimer, 150, 1);
}

void ICACHE_FLASH_ATTR lpd6803_RunningLine_loop() {
	uint16_t i;
	if (lpd6803_mode_rp_CurrentPixel == 0) {
		for (i = 0; i < numLEDs; i++) {
			lpd6803_setPixelColor(i, 0, 0, 0);
		}
	}

	if (!lpd6803_mode_rl_initialPixel) {
		lpd6803_setPixelColorByColor(lpd6803_mode_rp_CurrentPixel,
				lpd6803_mode_rp_PixelColor);
		lpd6803_mode_rp_CurrentPixel++;
	} else {
		lpd6803_mode_rl_initialPixel = false;
	}

	if (lpd6803_mode_rp_CurrentPixel >= numLEDs) {
		lpd6803_mode_rp_CurrentPixel = 0;
		lpd6803_mode_rl_initialPixel = true;
	}

	lpd6803_show();
}

void ICACHE_FLASH_ATTR lpd6803_startRunningLine(uint16_t color) {
	lpd6803_mode_rp_PixelColor = color;
	lpd6803_mode_rp_CurrentPixel = 0;
	lpd6803_mode_rl_initialPixel = true;
	lpd6803_mode = LPD6803_MODE_RUNNING_LINE;
	os_timer_disarm(&modeTimer);
	os_timer_setfn(&modeTimer, (os_timer_func_t *) lpd6803_loop, NULL);
	os_timer_arm(&modeTimer, 150, 1);
}

void ICACHE_FLASH_ATTR lpd6803_RunningPixel_loop() {
	uint16_t i;
	lpd6803_setPixelColorByColor(lpd6803_mode_rp_CurrentPixel,
			lpd6803_mode_rp_PixelColor);

	if (lpd6803_mode_rp_CurrentPixel <= 0) {
		for (i = 1; i < numLEDs; i++) {
			lpd6803_setPixelColor(i, 0, 0, 0);
		}
	} else {
		for (i = 0; i < numLEDs; i++) {
			if (i != lpd6803_mode_rp_CurrentPixel) {
				lpd6803_setPixelColor(i, 0, 0, 0);
			}
		}
	}

	lpd6803_mode_rp_CurrentPixel++;

	if (lpd6803_mode_rp_CurrentPixel >= numLEDs) {
		lpd6803_mode_rp_CurrentPixel = 0;
	}

	lpd6803_show();
}

void ICACHE_FLASH_ATTR lpd6803_startRunningPixel(uint16_t color) {
	lpd6803_mode_rp_PixelColor = color;
	lpd6803_mode_rp_CurrentPixel = 0;
	lpd6803_mode = LPD6803_MODE_RUNNING_PIXEL;
	os_timer_disarm(&modeTimer);
	os_timer_setfn(&modeTimer, (os_timer_func_t *) lpd6803_loop, NULL);
	os_timer_arm(&modeTimer, 150, 1);
}

void ICACHE_FLASH_ATTR lpd6803_Rainbow_loop() {
	int i;
	if (lpd6803_mode_rainbow_j < 96 * 5) {

		for (i = 0; i < numLEDs; i++) {
			lpd6803_setPixelColorByColor(i,
					lpd6803_Wheel(
							(i * 96 / numLEDs + lpd6803_mode_rainbow_j) % 96));
		}
		lpd6803_mode_rainbow_j++;
		lpd6803_show();
	} else {
		lpd6803_mode_rainbow_j = 0;
	}
}

void ICACHE_FLASH_ATTR lpd6803_startRainbow() {
	lpd6803_mode_rainbow_j = 0;
	lpd6803_mode = LPD6803_MODE_RAINBOW;
	os_timer_disarm(&modeTimer);
	os_timer_setfn(&modeTimer, (os_timer_func_t *) lpd6803_loop, NULL);
	os_timer_arm(&modeTimer, 50, 1);
}

void ICACHE_FLASH_ATTR lpd6803_disableModes() {
	if (lpd6803_mode != LPD6803_MODE_NONE) {
		uint16_t i;
		for (i = 0; i < numLEDs; i++) {
			lpd6803_setPixelColor(i, 0, 0, 0);
		}
		lpd6803_mode = LPD6803_MODE_NONE;
		lpd6803_show();
	}
}

void ICACHE_FLASH_ATTR lpd6803_Rainbow2_loop() {
	int i;
	if (lpd6803_mode_rainbow_j < 96 * 5) {

		for (i = 0; i < numLEDs; i++) {
			lpd6803_setPixelColorByColor(i,
					lpd6803_Wheel(
							(96 / numLEDs + lpd6803_mode_rainbow_j) % 96));
		}
		lpd6803_mode_rainbow_j++;
		lpd6803_show();
	} else {
		lpd6803_mode_rainbow_j = 0;
	}
}

void ICACHE_FLASH_ATTR lpd6803_startRainbow2() {
	lpd6803_mode_rainbow_j = 0;
	lpd6803_mode = LPD6803_MODE_RAINBOW2;
	os_timer_disarm(&modeTimer);
	os_timer_setfn(&modeTimer, (os_timer_func_t *) lpd6803_loop, NULL);
	os_timer_arm(&modeTimer, 50, 1);
}

void ICACHE_FLASH_ATTR lpd6803_Snow_loop() {
	int colors[] = { lpd6803_Color(255, 255, 255), lpd6803_Color(66, 170, 250),
			lpd6803_Color(0, 0, 255) };

	int j = 0;
	int i;

	for (i = lpd6803_mode_rainbow_j; i < numLEDs; i++) {
		lpd6803_setPixelColorByColor(i, colors[j]);
		j++;
		if (j > 2) {
			j = 0;
		}
	}

	for (i = 0; i < lpd6803_mode_rainbow_j; i++) {
		lpd6803_setPixelColorByColor(i, colors[j]);
		j++;
		if (j > 2) {
			j = 0;
		}
	}

	lpd6803_mode_rainbow_j++;
	if (lpd6803_mode_rainbow_j > numLEDs) {
		lpd6803_mode_rainbow_j = 0;
	}
	lpd6803_show();
}

void ICACHE_FLASH_ATTR lpd6803_startSnow() {
	lpd6803_mode_rainbow_j = 0;
	lpd6803_mode = LPD6803_MODE_SNOW;
	os_timer_disarm(&modeTimer);
	os_timer_setfn(&modeTimer, (os_timer_func_t *) lpd6803_loop, NULL);
	os_timer_arm(&modeTimer, 150, 1);
}

void ICACHE_FLASH_ATTR lpd6803_RGB_loop() {
	int colors[] = { lpd6803_Color(255, 0, 0), lpd6803_Color(0, 255, 0),
			lpd6803_Color(0, 0, 255) };

	int j = 0;
	int i;

	for (i = lpd6803_mode_rainbow_j; i < numLEDs; i++) {
		lpd6803_setPixelColorByColor(i, colors[j]);
		j++;
		if (j > 2) {
			j = 0;
		}
	}

	for (i = 0; i < lpd6803_mode_rainbow_j; i++) {
		lpd6803_setPixelColorByColor(i, colors[j]);
		j++;
		if (j > 2) {
			j = 0;
		}
	}

	lpd6803_mode_rainbow_j++;
	if (lpd6803_mode_rainbow_j > numLEDs) {
		lpd6803_mode_rainbow_j = 0;
	}
	lpd6803_show();
}

void ICACHE_FLASH_ATTR lpd6803_startRGB() {
	lpd6803_mode_rainbow_j = 0;
	lpd6803_mode = LPD6803_MODE_RGB;
	os_timer_disarm(&modeTimer);
	os_timer_setfn(&modeTimer, (os_timer_func_t *) lpd6803_loop, NULL);
	os_timer_arm(&modeTimer, 150, 1);
}

void ICACHE_FLASH_ATTR lpd6803_strip(uint8_t * buffer, uint16_t length) {
	uint16_t i;
	numLEDs = length/3; // set numLEDs to number recieved from network
	lastframe_counter = 0; // reset last frame received counter

	if (lpd6803_mode != LPD6803_MODE_TPM2NET) { // switch to tpm2net mode
		lpd6803_starttpm2net();
	}

	if (lpd6803_SendMode != LPD6803_DONE) { // wait until data is done?
		return; //drop frame
	}
	
	for( i = 0; i < numLEDs; i++ ) {
		uint16_t cled = i * 3; // current led array location
		lpd6803_setPixelColor(i, buffer[cled], buffer[cled+1], buffer[cled+2]); // load up lpd6803's data array
	}

	lpd6803_show();
}

