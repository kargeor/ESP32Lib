/*
	Author: Martin-Laclaustra 2021
	License: 
	Creative Commons Attribution ShareAlike 4.0
	https://creativecommons.org/licenses/by-sa/4.0/
	
	For further details check out: 
		https://github.com/bitluni
*/

/*
	CONNECTION
	
	A) voltageDivider = false; B) voltageDivider = true
	
	   55 shades                  179 shades
	
	ESP32        TV           ESP32                       TV     
	-----+                     -----+    ____ 100 ohm
	    G|-                        G|---|____|+          
	pin25|--------- Comp       pin25|---|____|+--------- Comp    
	pin26|-                    pin26|-        220 ohm
	     |                          |
	     |                          |
	-----+                     -----+
	
	Connect pin 25 or 26
*/
#pragma once
#include "CompositeI2SEngine.h"
//#include "../Graphics/Graphics.h"
#include "../Graphics/GraphicsW8.h"


class CompositeGrayDACI : public CompositeI2SEngine<BLpx1sz16sw1sh8>, public GraphicsW8 // (=) Graphics<ColorW8, BLpx1sz8sw0sh0, CTBIdentity>
{
  public:
	CompositeGrayDACI() //DAC based modes only work with I2S0
		: CompositeI2SEngine<BLpx1sz16sw1sh8>(0)
	{
		colorMinValue = 23;
		syncLevel = 0;
		colorMaxValue = 77;
		interruptStaticChild = &CompositeGrayDACI::interrupt;
	}

	int outputPin = 25;
	bool voltageDivider = false;

	int colorDepthConversionFactor = 1;
	int colorMaxValue = 255;
	int colorMinValue = 77;

	bool init(const ModeComposite &mode, const int outputPin = 25, const bool voltageDivider = false)
	{
		const int bitCount = 16;
		int pinMap[bitCount] = {
			-1, -1, 
			-1, -1, -1, -1, -1,
			-1, -1, -1, -1, -1,
			-1, -1, -1, -1
		};
		int clockPin = -1;
		this->outputPin = outputPin;
		this->voltageDivider = voltageDivider;
		if(voltageDivider)
		{
			colorMinValue = 77;
			syncLevel = 0;
			colorMaxValue = 255;
		}

		return initdynamicwritetorenderbuffer(mode, pinMap, bitCount, clockPin);
	}

	bool init(const ModeComposite &mode, const PinConfigComposite &pinConfig)
	{
		const int bitCount = 16;
		int pinMap[bitCount] = {
			-1, -1, 
			-1, -1, -1, -1, -1,
			-1, -1, -1, -1, -1,
			-1, -1, -1, -1
		};
		int clockPin = -1;

		return initdynamicwritetorenderbuffer(mode, pinMap, bitCount, clockPin);
	}

	bool initDAC(const ModeComposite &mode, const int *pinMap, const int bitCount, const int clockPin = -1, int descriptorsPerLine = 2)
	{
		this->mode = mode;
		int xres = mode.hRes;
		int yres = mode.vRes / mode.vDiv;
		totalLines = mode.linesPerFrame;
		if(descriptorsPerLine < 1 || descriptorsPerLine > 2) ERROR("Wrong number of descriptors per line");
		if(descriptorsPerLine == 1) allocateRendererBuffers1DescriptorsPerLine();
		if(descriptorsPerLine == 2) allocateRendererBuffers2DescriptorsPerLine();
		propagateResolution(xres, yres);
		//allocateLineBuffers();
		currentLine = 0;
		vSyncPassed = false;
		initParallelOutputMode(pinMap, mode.pixelClock, bitCount, clockPin);
		enableDAC(outputPin==25?1:2);
		startTX();
		return true;
	}

	bool initdynamicwritetorenderbuffer(const ModeComposite &mode, const int *pinMap, const int bitCount, const int clockPin = -1)
	{
		//values must be shifted to the MSByte to be output
		//which is equivalent to multiplying by 256
		//instead of shifting, do not divide here:
		//colorDepthConversionFactor = (colorMaxValue - colorMinValue + 1)/256;
		colorDepthConversionFactor = colorMaxValue - colorMinValue + 1;

		baseBufferValue = colorMinValue;
		syncBufferValue = syncLevel;

		lineBufferCount = 3;
		rendererBufferCount = 1;
		return initDAC(mode, pinMap, bitCount, clockPin, 1); // 1 buffer per line
	}

	//THE REST OF THE FILE IS SHARED CODE BETWEEN ...

	virtual void propagateResolution(const int xres, const int yres)
	{
		setResolution(xres, yres);
	}

	virtual void show(bool vSync = false)
	{
		if (!frameBufferCount)
			return;
		if (vSync)
		{
			vSyncPassed = false;
			while (!vSyncPassed)
				delay(0);
		}
		Graphics::show(vSync);
	}

  protected:
	bool useInterrupt()
	{ 
		return true; 
	};

	static void interrupt(void *arg);

	static void interruptPixelLine(int y, uint8_t *pixels, void *arg);
};


void IRAM_ATTR CompositeGrayDACI::interrupt(void *arg)
{
	CompositeGrayDACI * staticthis = (CompositeGrayDACI *)arg;

	//obtain currently rendered line from the buffer just read, based on the conventioned ordering and buffers per line
	staticthis->currentLine = staticthis->dmaBufferDescriptorActive >> ( (staticthis->descriptorsPerLine==2) ? 1 : 0 );

	//in the case of two buffers per line,
	//render only when the sync half of the line ended (longer period until next interrupt)
	//else exit early
	//This might need to be revised, because it might be better to overlap and miss the second interrupt
	if ( (staticthis->descriptorsPerLine==2) && (staticthis->dmaBufferDescriptorActive & 1 != 0) ) return;

	//render ahead (the lenght of buffered lines)
	int renderLine = (staticthis->currentLine + staticthis->lineBufferCount);
	if (renderLine >= staticthis->totalLines) renderLine -= staticthis->totalLines;

	if(!staticthis->mode.interlaced)
	{
		//TO DO: This should be precalculated outside the interrupt
		int vInactiveLinesCount = staticthis->mode.vFront + staticthis->mode.vOddFieldOffset + staticthis->mode.vBack;

		if (renderLine >= vInactiveLinesCount)
		{
			int renderActiveLine = renderLine - vInactiveLinesCount;
			uint8_t *activeRenderingBuffer = ((uint8_t *)
			staticthis->dmaBufferDescriptors[renderLine * staticthis->descriptorsPerLine + staticthis->descriptorsPerLine - 1].buffer() + staticthis->dataOffsetInLineInBytes
			);

			int y = renderActiveLine / staticthis->mode.vDiv;
			if (y >= 0 && y < staticthis->yres)
				staticthis->interruptPixelLine(y, activeRenderingBuffer, arg);
		}

		if (renderLine == 0)
			staticthis->vSyncPassed = true;
	} else {
		//TO DO: This should be precalculated outside the interrupt
		int oddFieldStart = staticthis->mode.vFront + staticthis->mode.vOddFieldOffset + staticthis->mode.vBack;
		int oddFieldEnd = oddFieldStart + staticthis->mode.vActive;
		int evenFieldStart = staticthis->mode.vFront + staticthis->mode.vEvenFieldOffset + staticthis->mode.vBack;
		int evenFieldEnd = evenFieldStart + staticthis->mode.vActive;
		
		if (renderLine >= oddFieldStart && renderLine < oddFieldEnd)
		{
			int renderActiveLine = renderLine - oddFieldStart;
			uint8_t *activeRenderingBuffer = ((uint8_t *)
			staticthis->dmaBufferDescriptors[renderLine * staticthis->descriptorsPerLine + staticthis->descriptorsPerLine - 1].buffer() + staticthis->dataOffsetInLineInBytes
			);

			int y = 2*renderActiveLine / staticthis->mode.vDiv;
			if (y >= 0 && y < staticthis->yres)
				staticthis->interruptPixelLine(y, activeRenderingBuffer, arg);
		} else if (renderLine >= evenFieldStart && renderLine < evenFieldEnd)
		{
			int renderActiveLine = renderLine - evenFieldStart;
			uint8_t *activeRenderingBuffer = ((uint8_t *)
			staticthis->dmaBufferDescriptors[renderLine * staticthis->descriptorsPerLine + staticthis->descriptorsPerLine - 1].buffer() + staticthis->dataOffsetInLineInBytes
			);

			int y = (2*renderActiveLine + 1) / staticthis->mode.vDiv;
			if (y >= 0 && y < staticthis->yres)
				staticthis->interruptPixelLine(y, activeRenderingBuffer, arg);
		}

		if (renderLine == 0)
			staticthis->vSyncPassed = true;
	}

}

void IRAM_ATTR CompositeGrayDACI::interruptPixelLine(int y, uint8_t *pixels, void *arg)
{
	CompositeGrayDACI * staticthis = (CompositeGrayDACI *)arg;
	uint8_t *line = staticthis->frontBuffer[y];
	for (int i = 0; i < staticthis->mode.hRes / 2; i++)
	{
		//writing two pixels improves speed drastically (avoids memory reads)
		//values must be shifted to the MSByte to be output
		//which is equivalent to multiplying by 256
		//instead of shifting, colorDepthConversionFactor was not divided by 256
		((uint32_t *)pixels)[i] = 
			((staticthis->baseBufferValue << 8) + (staticthis->colorDepthConversionFactor*(int)line[i * 2 + 1]))
			| (((staticthis->baseBufferValue << 8) + (staticthis->colorDepthConversionFactor*(int)line[i * 2])) << 16);
	}
}
