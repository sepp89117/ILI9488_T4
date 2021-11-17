/******************************************************************************
*  ILI9488_T4 library for driving an ILI9488 screen via SPI with a Teensy 4/4.1
*  Implements vsync and differential updates from a memory framebuffer.
*
*  Copyright (c) 2020 Arvind Singh.  All right reserved.
*
* This library is free software; you can redistribute it and/or
*  modify it under the terms of the GNU Lesser General Public
*  License as published by the Free Software Foundation; either
*  version 2.1 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*  Lesser General Public License for more details.
*
*  You should have received a copy of the GNU Lesser General Public
*  License along with this library; if not, write to the Free Software
*  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*******************************************************************************/

#include "ILI9488Driver.h"
#include <SPI.h>

namespace ILI9488_T4
{

    /**********************************************************************************************************
    * Initialization and general settings
    ***********************************************************************************************************/

    FLASHMEM ILI9488Driver::ILI9488Driver(uint8_t cs, uint8_t dc, uint8_t sclk, uint8_t mosi, uint8_t miso, uint8_t rst, uint8_t touch_cs, uint8_t touch_irq)
    {
        // general
        _width = ILI9488_T4_TFTWIDTH;
        _height = ILI9488_T4_TFTHEIGHT;
        _rotation = 0;
        _refreshmode = 0;
        _outputStream = nullptr;

        // buffering
        _late_start_ratio = ILI9488_T4_DEFAULT_LATE_START_RATIO;
        _late_start_ratio_override = true;
        _diff_gap = ILI9488_T4_DEFAULT_DIFF_GAP;
        _vsync_spacing = ILI9488_T4_DEFAULT_VSYNC_SPACING;
        _diff1 = nullptr;
        _diff2 = nullptr;
        _fb1 = nullptr;
        _fb2 = nullptr;
        _dummydiff1 = &_dd1;
        _dummydiff2 = &_dd2;
        _mirrorfb = nullptr;
        _ongoingDiff = nullptr;

        _fb2full = false;
        _compare_mask = 0;

        // vsync
        _period = 0;
        _synced_em = 0;
        _synced_scanline = 0;

        // dma
        _pcb = nullptr;
        _fb = nullptr;
        _diff = nullptr;
        _dma_state = ILI9488_T4_DMA_IDLE;
        _last_delta = 0;
        _timeframestart = 0;
        _last_y = 0;

        // spi
        _cs = cs;
        _dc = dc;
        _sclk = sclk;
        _mosi = mosi;
        _miso = miso;
        _rst = rst;
        _touch_cs = touch_cs;
        _touch_irq = touch_irq;
        _cspinmask = 0;
        _csport = NULL;

        _setTouchInterrupt();
        _timerinit();

        statsReset();
    }

    FLASHMEM bool ILI9488Driver::begin(uint32_t spi_clock, uint32_t spi_clock_read)
    {
        static const uint8_t init_commands[] = {
            16, 0xE0, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,
            16, 0XE1, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,
            3, 0XC0, 0x17, 0x15,
            2, 0xC1, 0x41,                   //Power Control 2
            4, 0xC5, 0x00, 0x12, 0x80,       //Power Control 3
            2, 0x36, 0x48,                   //Memory Access
            2, 0x3A, 0x66,                   // Interface Pixel Format, 18bit
            2, 0xB0, 0x80,                   // Interface Mode Control
            2, 0xB1, 0xA0,                   //Frame rate, 60hz
            2, 0xB4, 0x02,                   //Display Inversion Control
            1, 0XB6,                         //Display Function Control  RGB/MCU Interface Control
            2, 0x02, 0x02,                   //MCU
            2, 0xE9, 0x00,                   // Set Image Functio,Disable 24 bit data
            5, 0xF7, 0xA9, 0x51, 0x2C, 0x82, // Adjust Control
            0};

        _print("\n\n----------------- ILI9488_T4 begin() ------------------\n\n");
        statsReset();
        resync();            // resync at first upload
        _mirrorfb = nullptr; // force full redraw.
        _ongoingDiff = nullptr;

        if (_touch_cs != 255)
        { // set touch CS high to prevent interference.
            digitalWrite(_touch_cs, HIGH);
            pinMode(_touch_cs, OUTPUT);
            digitalWrite(_touch_cs, HIGH);
        }
        /*
        if (_cs != 255)
            { // set screen CS high also. 
            digitalWrite(_cs, HIGH);
            pinMode(_cs, OUTPUT);
            digitalWrite(_cs, HIGH);
            }
        */

        // verify SPI pins are valid
        int spinum_MOSI = -1;
        if (SPI.pinIsMOSI(_mosi))
            spinum_MOSI = 0;
        else if (SPI1.pinIsMOSI(_mosi))
            spinum_MOSI = 1;
        else if (SPI2.pinIsMOSI(_mosi))
            spinum_MOSI = 2;
        if (spinum_MOSI < 0)
        {
            _printf("\n*** ERROR: MOSI on pin %d is not a valid SPI pin ! ***\n\n", _mosi);
            return false;
        }
        else
            _printf("- MOSI on pin %d [SPI%d]\n", _mosi, spinum_MOSI);

        int spinum_MISO = -1;
        if (SPI.pinIsMISO(_miso))
            spinum_MISO = 0;
        else if (SPI1.pinIsMISO(_miso))
            spinum_MISO = 1;
        else if (SPI2.pinIsMISO(_miso))
            spinum_MISO = 2;
        if (spinum_MISO < 0)
        {
            _printf("\n*** ERROR: MISO on pin %d is not a valid SPI pin ! ***\n\n", _miso);
            return false;
        }
        else
            _printf("- MISO on pin %d [SPI%d]\n", _miso, spinum_MISO);

        int spinum_SCK = -1;
        if (SPI.pinIsSCK(_sclk))
            spinum_SCK = 0;
        else if (SPI1.pinIsSCK(_sclk))
            spinum_SCK = 1;
        else if (SPI2.pinIsSCK(_sclk))
            spinum_SCK = 2;
        if (spinum_SCK < 0)
        {
            _printf("\n*** ERROR: SCK on pin %d is not a valid SPI pin ! ***\n\n", _sclk);
            return false;
        }
        else
            _printf("- SCK on pin %d [SPI%d]\n", _sclk, spinum_SCK);

        if ((spinum_SCK != spinum_MISO) || (spinum_SCK != spinum_MOSI))
        {
            _printf("\n*** ERROR: SCK, MISO and MOSI must be on the same SPI bus ! ***\n\n", _sclk);
            return false;
        }

        if (spinum_SCK == 0)
        {
            _pspi = &SPI;
            _spi_num = 0;                  // Which buss is this spi on?
            _pimxrt_spi = &IMXRT_LPSPI4_S; // Could hack our way to grab this from SPI object, but...
        }
        else if (spinum_SCK == 1)
        {
            _pspi = &SPI1;
            _spi_num = 1;                  // Which buss is this spi on?
            _pimxrt_spi = &IMXRT_LPSPI3_S; // Could hack our way to grab this from SPI object, but...
        }
        else
        {
            _pspi = &SPI2;
            _spi_num = 2;                  // Which buss is this spi on?
            _pimxrt_spi = &IMXRT_LPSPI1_S; // Could hack our way to grab this from SPI object, but...
        }

        // Make sure we have all of the proper SPI pins selected.
        _pspi->setMOSI(_mosi);
        _pspi->setSCK(_sclk);
        _pspi->setMISO(_miso);

        // Hack to get hold of the SPI Hardware information...
        uint32_t *pa = (uint32_t *)((void *)_pspi);
        _spi_hardware = (SPIClass::SPI_Hardware_t *)(void *)pa[1];
        _pspi->begin();

        _pending_rx_count = 0; // Make sure it is zero if we we do a second begin...

        // CS pin direct access via port.
        _csport = portOutputRegister(_cs);
        _cspinmask = digitalPinToBitMask(_cs);
        pinMode(_cs, OUTPUT);
        _directWriteHigh(_csport, _cspinmask);

        _spi_tcr_current = _pimxrt_spi->TCR; // get the current TCR value

        if (!_pspi->pinIsChipSelect(_dc))
        {
            _printf("\n*** ERROR: DC (here on pin %d) is not a valid cs pin for SPI%d ***\n\n", _dc, _spi_num);
            return false; // ERROR, DC is not a hardware CS pin for the SPI bus.
        }
        _printf("- DC on pin %d [SPI%d]\n", _dc, _spi_num);
        _printf("- CS on pin %d\n", _cs);

        // Ok, DC is on a hardware CS pin
        uint8_t dc_cs_index = _pspi->setCS(_dc);
        dc_cs_index--; // convert to 0 based
        _tcr_dc_assert = LPSPI_TCR_PCS(dc_cs_index);
        _tcr_dc_not_assert = LPSPI_TCR_PCS(3);
        _maybeUpdateTCR(_tcr_dc_not_assert | LPSPI_TCR_FRAMESZ(7)); // drive DC high now.

        if (_rst < 255)
            _printf("- RST on pin %d\n", _rst);
        else
            _print("- RST pin not connected (set it to +3.3V).\n");
        if (_touch_cs < 255)
        {
            _printf("\n[Touchscreen is CONNECTED]\n- TOUCH_CS on pin %d\n", _touch_cs);
            if (_touch_irq < 255)
                _printf("- TOUCH_IRQ on pin %d\n", _touch_irq);
            else
                _print("- TOUCH_IRQ not connected\n");
        }
        else
        {
            _print("\n[Touchscreen NOT connected]\n");
        }

        _spi_clock = spi_clock;
        if (_spi_clock < 0)
            _spi_clock = ILI9488_T4_DEFAULT_SPICLOCK;
        _spi_clock_read = spi_clock_read;
        if (_spi_clock_read < 0)
            _spi_clock_read = ILI9488_T4_DEFAULT_SPICLOCK_READ;
        _printf("\n- SPI write speed : %.2fMhz\n", spi_clock / 1000000.0f);
        _printf("- SPI read speed : %.2fMhz\n\n", spi_clock_read / 1000000.0f);

        _rotation = 0; // default rotation

        int r = ILI9488_T4_RETRY_INIT;
        while (1)
        { // sometimes, init may fail because of instable power supply. Retry in this case.
            if (_rst < 255)
            { // Reset the screen
                pinMode(_rst, OUTPUT);
                digitalWrite(_rst, HIGH);
                delay(10);
                digitalWrite(_rst, LOW);
                delay(20);
                digitalWrite(_rst, HIGH);
            }
            else
            {
                _beginSPITransaction(_spi_clock / 4); // quarter speed for setup !
                for (int i = 0; i < 5; i++)
                    _writecommand_cont(ILI9488_T4_NOP); // send NOPs
                _writecommand_last(ILI9488_T4_SWRESET); // issue a software reset
                _endSPITransaction();
            }
            delay(150); // mandatory !

            _beginSPITransaction(_spi_clock / 4); // quarter speed for setup !
            const uint8_t *addr = init_commands;
            while (1)
            {
                uint8_t count = *addr++;
                if (count-- == 0)
                    break;
                _writecommand_cont(*addr++);
                while (count-- > 0)
                {
                    _writedata8_cont(*addr++);
                }
            }
            _writecommand_last(ILI9488_T4_SLPOUT); // Exit Sleep
            _endSPITransaction();

            delay(150);                            // must wait for the screen to exit sleep mode.
            _beginSPITransaction(_spi_clock / 4);  // quarter speed for setup !
            _writecommand_last(ILI9488_T4_DISPON); // Display on
            _endSPITransaction();

            // if everything is ok, we should have:
            // - Display Power Mode = 0x9C
            // - Pixel Format = 0x5
            // - Image Format = 0x0
            // - Self Diagnostic = 0xC0
            int res_RDMODE = _readcommand8(ILI9488_T4_RDMODE);
            int res_RDPIXFMT = _readcommand8(ILI9488_T4_RDPIXFMT);
            int res_RDIMGFMT = _readcommand8(ILI9488_T4_RDIMGFMT);
            int res_RDSELFDIAG = _readcommand8(ILI9488_T4_RDSELFDIAG);
            _print("\nReading status registers...\n");
            _print("  - Display Power Mode : 0x");
            _println(res_RDMODE, HEX);
            _print("  - Pixel Format       : 0x");
            _println(res_RDPIXFMT, HEX);
            _print("  - Image Format       : 0x");
            _println(res_RDIMGFMT, HEX);
            _print("  - Self Diagnostic    : 0x");
            _println(res_RDSELFDIAG, HEX);

            bool ok = true;
            if ((res_RDMODE == 0) && (res_RDPIXFMT == 0) && (res_RDIMGFMT == 0) && (res_RDSELFDIAG == 0))
            {
                _print("\n*** ERROR: Cannot read screen registers. Check the MISO line or decrease SPI read speed ***\n\n");
                ok = false;
            }
            else
            {
                if (res_RDMODE != 0x9C)
                { // wrong power display mode
                    _print("\n*** ERROR: incorrect power mode ! ***\n\n");
                    ok = false;
                }
                if (res_RDPIXFMT != 0x5)
                { // wrong pixel format
                    _print("\n*** ERROR: incorrect pixel format ! ***\n\n");
                    ok = false;
                }
                if (res_RDIMGFMT != 0x0)
                { // wrong image format
                    _print("\n*** ERROR: incorrect image format ! ***\n\n");
                    ok = false;
                }
                if (res_RDSELFDIAG != ILI9488_T4_SELFDIAG_OK)
                { // wrong self diagnotic value
                    _print("\n*** ERROR: incorrect self-diagnotic value ! ***\n\n");
                    ok = false;
                }
            }
            if (ok)
            {
                // all good, ready to warp pixels :-)
                // ok, we can talk to the display so we set the (max) refresh rate to read its exact values
                setRefreshMode(0);
                _period_mode0 = _period; // save the period for fastest mode.
                _print("\nOK. Screen initialization successful !\n\n");
                return true;
            }
            // error
            if (--r <= 0)
            {
                _print("\n*** CANNOT CONNECT TO ILI9488 SCREEN. ABORTING... ***\n\n");
            }
            _spi_clock_read /= 2;
            _printf("Retrying connexion with slower SPI read speed : %.2fMhz", _spi_clock_read / 1000000.0f);
        }
    }

    int ILI9488Driver::selfDiagStatus()
    {
        waitUpdateAsyncComplete();
        resync();
        return _readcommand8(ILI9488_T4_RDSELFDIAG);
    }

    FLASHMEM void ILI9488Driver::printStatus()
    {
        waitUpdateAsyncComplete();
        _print("---------------- ILI9488Driver Status-----------------\n");
        uint8_t x = _readcommand8(ILI9488_T4_RDMODE);
        _print("- Display Power Mode  : 0x");
        _println(x, HEX);
        x = _readcommand8(ILI9488_T4_RDMADCTL);
        _print("- MADCTL Mode         : 0x");
        _println(x, HEX);
        x = _readcommand8(ILI9488_T4_RDPIXFMT);
        _print("- Pixel Format        : 0x");
        _println(x, HEX);
        x = _readcommand8(ILI9488_T4_RDIMGFMT);
        _print("- Image Format        : 0x");
        _println(x, HEX);
        x = _readcommand8(ILI9488_T4_RDSGNMODE);
        _print("- Display Signal Mode : 0x");
        _println(x, HEX);
        x = _readcommand8(ILI9488_T4_RDSELFDIAG);
        _print("- Self Diagnostic     : 0x");
        _print(x, HEX);
        if (x == ILI9488_T4_SELFDIAG_OK)
            _println(" [OK].\n");
        else
            _println(" [ERROR].\n");
        resync();
    }

    /**********************************************************************************************************
    * Misc. commands.
    ***********************************************************************************************************/

    FLASHMEM void ILI9488Driver::sleep(bool enable)
    {
        waitUpdateAsyncComplete();

        _mirrorfb = nullptr; // force full redraw.
        _ongoingDiff = nullptr;

        _beginSPITransaction(_spi_clock / 4); // quarter speed
        if (enable)
        {
            _writecommand_cont(ILI9488_T4_DISPOFF);
            _writecommand_last(ILI9488_T4_SLPIN);
            _endSPITransaction();
            delay(200);
        }
        else
        {
            _writecommand_cont(ILI9488_T4_DISPON);
            _writecommand_last(ILI9488_T4_SLPOUT);
            _endSPITransaction();
            delay(20);
        }
        resync();
    }

    void ILI9488Driver::invertDisplay(bool i)
    {
        waitUpdateAsyncComplete();
        _beginSPITransaction(_spi_clock / 4); // quarter speed
        _writecommand_last(i ? ILI9488_T4_INVON : ILI9488_T4_INVOFF);
        _endSPITransaction();
        resync();
    }

    void ILI9488Driver::setScroll(int offset)
    {
        if (offset < 0)
        {
            offset += (((-offset) / ILI9488_T4_TFTHEIGHT) + 1) * ILI9488_T4_TFTHEIGHT;
        }
        offset = offset % 320;
        waitUpdateAsyncComplete();
        _beginSPITransaction(_spi_clock);
        _writecommand_cont(ILI9488_T4_VSCRSADD);
        _writedata16_cont(offset);
        _writecommand_cont(ILI9488_T4_RAMWR); // must send RAMWR because two consecutive VSCRSADD command may stall
        _writecommand_last(ILI9488_T4_NOP);
        _endSPITransaction();
    }

    /**********************************************************************************************************
    * Screen orientation
    ***********************************************************************************************************/

    void ILI9488Driver::setRotation(uint8_t m)
    {
        m = _clip(m, (uint8_t)0, (uint8_t)3);
        if (m == _rotation)
            return;
        waitUpdateAsyncComplete();
        _mirrorfb = nullptr; // force full redraw.
        _ongoingDiff = nullptr;

        statsReset();
        _rotation = m;
        switch (m)
        {
        case 0: // portrait 320x480
        case 2: // portrait 320x480
            _width = ILI9488_T4_TFTWIDTH;
            _height = ILI9488_T4_TFTHEIGHT;
            break;
        case 1: // landscape 480x320
        case 3: // landscape 480x320
            _width = ILI9488_T4_TFTHEIGHT;
            _height = ILI9488_T4_TFTWIDTH;
            break;
        }
        resync();
    }

    /**********************************************************************************************************
    * About timing and vsync.
    ***********************************************************************************************************/

    void ILI9488Driver::setRefreshMode(int mode)
    {
        if ((mode < 0) || (mode > 31))
            return; // invalid mode, do nothing.
        _refreshmode = mode;
        uint8_t diva = 0;
        if (mode >= 16)
        {
            mode -= 16;
            diva = 1;
        }
        waitUpdateAsyncComplete();
        _beginSPITransaction(_spi_clock / 4);   // quarter speed
        _writecommand_cont(ILI9488_T4_FRMCTR1); // Column addr set
        _writedata8_cont(diva);
        _writedata8_last(0x10 + mode);
        _endSPITransaction();
        delayMicroseconds(50);
        _sampleRefreshRate(); // estimate the real refreshrate
        statsReset();
        resync();
    }

    void ILI9488Driver::printRefreshMode()
    {
        const int om = getRefreshMode();
        _print("------------ ILI9488Driver Refresh Modes -------------\n");
        for (int m = 0; m <= 31; m++)
        {
            setRefreshMode(m);
            float r = getRefreshRate();
            _printf("- mode %u : %fHz (%u FPS with vsync_spacing = 2).\n", m, r, (uint32_t)round(r / 2));
        }
        _println("");
        setRefreshMode(om);
    }

    /** return the current scanline in [0, 319]. Sync with SPI only if required */
    int ILI9488Driver::_getScanLine(bool sync)
    {
        if (!sync)
        {
            return (_synced_scanline + ((((uint64_t)_synced_em) * ILI9488_T4_NB_SCANLINES) / _period)) % ILI9488_T4_NB_SCANLINES;
        }
        int res[3] = {255}; // invalid value.
        _beginSPITransaction(_spi_clock_read);
        _maybeUpdateTCR(_tcr_dc_assert | LPSPI_TCR_FRAMESZ(7) | LPSPI_TCR_CONT);
        _pimxrt_spi->TDR = 0x45; // send command
        delayMicroseconds(5);    // wait as requested by manual.
        _maybeUpdateTCR(_tcr_dc_not_assert | LPSPI_TCR_FRAMESZ(7));
        _pimxrt_spi->TDR = 0; // send nothing
        _maybeUpdateTCR(_tcr_dc_not_assert | LPSPI_TCR_FRAMESZ(7));
        _pimxrt_spi->TDR = 0; // send nothing
        uint8_t rx_count = 3;
        while (rx_count)
        { // receive answer.
            if ((_pimxrt_spi->RSR & LPSPI_RSR_RXEMPTY) == 0)
            {
                res[--rx_count] = _pimxrt_spi->RDR;
            }
        }
        _synced_em = 0;
        _endSPITransaction();
        int sc = (2 * res[0]) - 3; // map [0,161] to [0, 319]
        if (sc < 0)
            sc = 0;                      // (put the extra time at scanline 0)
        _synced_scanline = (uint32_t)sc; // save the scanline
        return sc;
    }

    void ILI9488Driver::_sampleRefreshRate()
    {
        const int NB_SAMPLE_FRAMES = 10;
        while (_getScanLine(true) != 0)
            ; // wait to reach scanline 0
        while (_getScanLine(true) == 0)
            ;                 // wait to begin scanline 1.
        elapsedMicros em = 0; // start counter
        for (int i = 0; i < NB_SAMPLE_FRAMES; i++)
        {
            delayMicroseconds(5000); // must be less than 200 FPS so wait at least 5ms
            while (_getScanLine(true) != 0)
                ; // wait to reach scanline 0
            while (_getScanLine(true) == 0)
                ; // wait to begin scanline 1.
        }
        _period = (uint32_t)round(((float)em) / NB_SAMPLE_FRAMES);
    }

    float ILI9488Driver::_refreshRateForMode(int mode) const
    {
        float freq = 1000000.0f / _period_mode0;
        if (mode >= 16)
        {
            freq /= 2.0f;
            mode -= 16;
        }
        return (freq * 16.0f) / (16.0f + mode);
    }

    int ILI9488Driver::_modeForRefreshRate(float hz) const
    {
        if (hz <= _refreshRateForMode(31))
            return 31;
        if (hz >= _refreshRateForMode(0))
            return 0;
        int a = 0;
        int b = 31;
        while (b - a > 1)
        { // dichotomy.
            int c = (a + b) / 2;
            ((hz < _refreshRateForMode(c)) ? a : b) = c;
        }
        float da = _refreshRateForMode(a) - hz;
        float db = hz - _refreshRateForMode(b);
        return (da < db ? a : b);
    }

    /**********************************************************************************************************
    * Buffering mode
    ***********************************************************************************************************/

    void ILI9488Driver::setFramebuffers(uint16_t *fb1, uint16_t *fb2)
    {
        waitUpdateAsyncComplete();
        _mirrorfb = nullptr; // complete redraw needed.
        _ongoingDiff = nullptr;

        _fb2full = false;
        if (fb1)
        {
            _fb1 = fb1;
            _fb2 = fb2;
        }
        else
        {
            _fb1 = fb2;
            _fb2 = fb1;
        }

        // zero the framebuffers
        if (_fb1)
            memset(_fb1, 0, ILI9488_T4_NB_PIXELS * 2);
        if (_fb2)
            memset(_fb2, 0, ILI9488_T4_NB_PIXELS * 2);

        resync();
    }

    /**********************************************************************************************************
    * Differential updates
    ***********************************************************************************************************/

    void ILI9488Driver::setDiffBuffers(DiffBuffBase *diff1, DiffBuffBase *diff2)
    {
        waitUpdateAsyncComplete();
        if (diff1)
        {
            _diff1 = diff1;
            _diff2 = diff2;
        }
        else
        {
            _diff1 = diff2;
            _diff2 = diff1;
        }
    }

    /**********************************************************************************************************
    * Update
    ***********************************************************************************************************/

    void ILI9488Driver::clear(uint16_t color)
    {
        waitUpdateAsyncComplete();

        _beginSPITransaction(_spi_clock);

        //setAddr
        _writecommand_cont(ILI9488_T4_PASET);           // Row addr set
        _writedata16_cont(0);                           //y0
        _writedata16_cont(ILI9488_T4_TFTHEIGHT - 1);    //y1
        _writecommand_cont(ILI9488_T4_CASET);           // Column addr set
        _writedata16_cont(0);                           //x0
        _writedata16_cont(ILI9488_T4_TFTWIDTH - 1);     //x1

        //Write data
        _writecommand_cont(ILI9488_T4_RAMWR);
        for (int i = 0; i < ILI9488_T4_NB_PIXELS; i++)
            _write16BitColor(color);
        _writecommand_last(ILI9488_T4_NOP);
        _endSPITransaction();
        if (_fb1)
        {
            for (int i = 0; i < ILI9488_T4_NB_PIXELS; i++)
                _fb1[i] = color;
            _mirrorfb = _fb1;
            _ongoingDiff = nullptr;
        }
        resync();
    }

    void ILI9488Driver::updateRegion(bool redrawNow, const uint16_t *fb, int xmin, int xmax, int ymin, int ymax, int stride)
    {
        if (stride < 0)
            stride = xmax - xmin + 1;
        switch (bufferingMode())
        {
        case NO_BUFFERING:
            // the only thing we can do is to push the sub-frame right away.
            // without DMA and without DIFF so we just upload the rectangle.
            // TODO : add vsync ?
            _mirrorfb = nullptr;
            _ongoingDiff = nullptr;
            _updateRectNow(fb, xmin, xmax, ymin, ymax, stride);
            return;

        case TRIPLE_BUFFERING:
            // triple buffering is useless with updateRegion (the second internal framebuffer is ignored).
            while (_fb2full)
                ; // we wait until the _fb2 is free (hence diff 2 is also free).

        default:
            // Treat DOUBLE_BUFFERING and TRIPLE_BUFFERING THE SAME WAY.
            if (_diff2 == nullptr)                                                                                                      // only for differential updates with 2 diffs buffers.
            {                                                                                                                           // NO DIFFERENTIAL UPDATES: copy into the framebuffer and update the screen if required
                _ongoingDiff = nullptr;                                                                                                 // no diff
                waitUpdateAsyncComplete();                                                                                              // wait if there is still an update in progress
                _dummydiff1->computeDiff(_fb1, nullptr, fb, xmin, xmax, ymin, ymax, stride, _rotation, _diff_gap, true, _compare_mask); // create a diff and copy to fb1.
                if (redrawNow)
                {
                    if (_mirrorfb)
                    {                                                       // _fb1 mirrors the screen so we just need to draw the region
                        _updateRectNow(fb, xmin, xmax, ymin, ymax, stride); // note that we can the method with fb and not _fb1. ***** TODO: replace by an async draw
                    }
                    else
                    { // redraw everything, via DMA
                        _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                        _updateAsync(_fb1, _dummydiff1); // launch update
                    }
                    _mirrorfb = _fb1;
                }
                else
                {
                    _mirrorfb = nullptr;
                }
                return;
            }

            // we have 2 diff buffers and a framebuffer.
            if (_mirrorfb)
            { // the framebuffer mirrors the screen
                if (asyncUpdateActive())
                {
                    _diff2->computeDiff(_fb1, nullptr, fb, xmin, xmax, ymin, ymax, stride, _rotation, _diff_gap, false, _compare_mask); // create diff while async update
                    waitUpdateAsyncComplete();
                    DiffBuffBase::copyfb(_fb1, fb, xmin, xmax, ymin, ymax, stride, _rotation); // copy to fb1
                }
                else
                {
                    _diff2->computeDiff(_fb1, nullptr, fb, xmin, xmax, ymin, ymax, stride, _rotation, _diff_gap, true, _compare_mask); // create a diff and copy to fb1.
                }
                _swapdiff();
                if (redrawNow)
                {
                    _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                    _updateAsync(_fb1, _diff1);
                    _mirrorfb = _fb1;
                    _ongoingDiff = nullptr;
                }
                else
                {
                    _mirrorfb = nullptr;
                    _ongoingDiff = _diff1;
                }
                return;
            }

            if (_ongoingDiff != nullptr)
            { // we are "in advance" w.r.t the screen
                if (asyncUpdateActive())
                {
                    _diff2->computeDiff(_fb1, _diff1, fb, xmin, xmax, ymin, ymax, stride, _rotation, _diff_gap, false, _compare_mask); // create diff while asyn update
                    waitUpdateAsyncComplete();
                    DiffBuffBase::copyfb(_fb1, fb, xmin, xmax, ymin, ymax, stride, _rotation); // copy to fb1
                }
                else
                {
                    _diff2->computeDiff(_fb1, _diff1, fb, xmin, xmax, ymin, ymax, stride, _rotation, _diff_gap, true, _compare_mask); // create a diff and copy to fb1.
                }
                _swapdiff();
                if (redrawNow)
                {
                    _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                    _updateAsync(_fb1, _diff1);
                    _mirrorfb = _fb1;
                    _ongoingDiff = nullptr;
                }
                else
                {
                    _mirrorfb = nullptr;
                    _ongoingDiff = _diff1;
                }
                return;
            }

            // here, the framebuffer does not mirror the screen
            waitUpdateAsyncComplete();
            DiffBuffBase::copyfb(_fb1, fb, xmin, xmax, ymin, ymax, stride, _rotation); // copy the region into to fb1
            if (redrawNow)
            {                                                                                   // redraw everything
                _dummydiff1->computeDiff(_fb1, fb, _rotation, _diff_gap, false, _compare_mask); // create a dummy diff
                _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                _updateAsync(_fb1, _dummydiff1);
                _mirrorfb = _fb1; // now we mirror the screen !
            }
            return;
        }
    }

    void ILI9488Driver::update(const uint16_t *fb, bool force_full_redraw)
    {
        _ongoingDiff = nullptr; // here we just ignore possible ongoing diff and just redraw everything if _mirrorfb == nullptr.
                                // We could do better but don't care since its an edge case relevant only when swapping between
                                // methods updateRegion() and  update() which are not usually mixed.

        switch (bufferingMode())
        {
        case NO_BUFFERING:
        {
            waitUpdateAsyncComplete(); // wait until update is done (normally useless but still).
            _mirrorfb = nullptr;
            _dummydiff1->computeDummyDiff();
            _updateNow(fb, _dummydiff1);
            return;
        }

        case DOUBLE_BUFFERING:
        {
            if ((_vsync_spacing == -1) && (asyncUpdateActive()))
            {
                return;
            } // just drop the frame.

            if ((_diff1 == nullptr) || (_mirrorfb == nullptr) || (force_full_redraw))
            {                                                                                      // do not use differential update
                waitUpdateAsyncComplete();                                                         // wait until update is done.
                _dummydiff1->computeDiff(_fb1, fb, getRotation(), _diff_gap, true, _compare_mask); // create a dummy diff and copy to fb1.
                _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                _updateAsync(_fb1, _dummydiff1); // launch update
                _mirrorfb = _fb1;                // set as mirror
                return;
            }

            if (_diff2 == nullptr)
            {                              // double buffering with a single diff
                waitUpdateAsyncComplete(); // wait until update is done.
                if ((_mirrorfb == nullptr) || (force_full_redraw))
                {                                                                                      // complete redraw needed.
                    _dummydiff1->computeDiff(_fb1, fb, getRotation(), _diff_gap, true, _compare_mask); // create a dummy diff and copy to fb1.
                    _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                    _updateAsync(_fb1, _dummydiff1); // launch update
                }
                else
                {                                                                                 // diff redraw
                    _diff1->computeDiff(_fb1, fb, getRotation(), _diff_gap, true, _compare_mask); // create a diff and copy to fb1.
                    _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                    _updateAsync(_fb1, _diff1); // launch update
                }
                _mirrorfb = _fb1; // set as mirror
                return;
            }

            // double buffering with two diffs
            if (asyncUpdateActive())
            {                                                                                  // _diff2 is available so we use it to create the diff while update is in progress.
                _diff2->computeDiff(_fb1, fb, getRotation(), _diff_gap, false, _compare_mask); // create a diff without copying
                waitUpdateAsyncComplete();                                                     // wait until update is done.
                DiffBuff::copyfb(_fb1, fb, getRotation());                                     // save the framebuffer in fb1
                _swapdiff();                                                                   // swap the diffs so that diff1 contain the new diff.
                _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                _updateAsync(_fb1, _diff1); // launch update
            }
            else
            {
                _diff1->computeDiff(_fb1, fb, getRotation(), _diff_gap, true, _compare_mask); // create a diff and copy
                _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                _updateAsync(_fb1, _diff1); // launch update
            }
            _mirrorfb = _fb1; // set as mirror
            return;
        }

        case TRIPLE_BUFFERING:
        {
            if (!asyncUpdateActive())
            { // we can launch immediately
                if ((_diff2 == nullptr) || (_mirrorfb == nullptr) || (force_full_redraw))
                {                                                                                      // complete redraw needed.
                    _dummydiff1->computeDiff(_fb1, fb, getRotation(), _diff_gap, true, _compare_mask); // create a dummy diff and copy to fb1.
                    _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                    _updateAsync(_fb1, _dummydiff1); // launch update
                }
                else
                {
                    _diff1->computeDiff(_fb1, fb, getRotation(), _diff_gap, true, _compare_mask); // create a diff and copy
                    _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                    _updateAsync(_fb1, _diff1); // launch update
                }
                _mirrorfb = _fb1; // set as mirror
                return;
            }

            // there is an update in progress
            if (_vsync_spacing != -1)
            {
                while (_fb2full)
                    ; // we wait until the _fb2 is free.
            }

            // try again
            noInterrupts();
            if (asyncUpdateActive())
            {             // update still in progress so we replace_fb2.
                _setCB(); // remove callback to prevent upload of fb2
                interrupts();
                if ((_mirrorfb) && (!force_full_redraw) && (_diff2 != nullptr))
                {
                    _diff2->computeDiff(_fb1, fb, getRotation(), _diff_gap, false, _compare_mask); // create a diff without copying
                    DiffBuff::copyfb(_fb2, fb, getRotation());                                     // save in fb2
                    _flush_cache(_fb2, ILI9488_T4_NB_PIXELS * 2);
                    noInterrupts();
                    if (asyncUpdateActive())
                    {                                           // update still in progress...
                        _setCB(&ILI9488Driver::_buffer2fullCB); // set a callback
                        _fb2full = true;                        // and mark buffer as full.
                        _mirrorfb = _fb2;                       // this inform that we have a real diff in diff2.
                        interrupts();
                        return; // done
                    }
                    else
                    { // update done
                        interrupts();
                        _swapdiff();
                        _swapfb();
                        _mirrorfb = _fb1;
                        _updateAsync(_fb1, _diff1); // launch update
                        return;
                    }
                }
                else
                {
                    _dummydiff2->computeDiff(_fb1, fb, getRotation(), _diff_gap, false, _compare_mask); // create a dummy diff without copy
                    DiffBuff::copyfb(_fb2, fb, getRotation());                                          // save in fb2
                    _flush_cache(_fb2, ILI9488_T4_NB_PIXELS * 2);
                    noInterrupts();
                    if (asyncUpdateActive())
                    {                                           // update still in progress...
                        _setCB(&ILI9488Driver::_buffer2fullCB); // set a callback
                        _fb2full = true;                        // and mark buffer as full.
                        _mirrorfb = nullptr;                    // this inform that we have a dummy diff in _dummdiff2
                        interrupts();
                        return; // done
                    }
                    else
                    {
                        interrupts();
                        _swapdummydiff();
                        _swapfb();
                        _mirrorfb = _fb1;
                        _updateAsync(_fb1, _dummydiff1); // launch update
                        return;
                    }
                }
            }
            else
            { // we can launch immediately
                if ((_mirrorfb == nullptr) || (force_full_redraw) || (_diff2 == nullptr))
                {                                                                                      // complete redraw needed.
                    _dummydiff1->computeDiff(_fb1, fb, getRotation(), _diff_gap, true, _compare_mask); // create a dummy diff and copy to fb1.
                    _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                    _updateAsync(_fb1, _dummydiff1); // launch update
                }
                else
                {
                    _diff1->computeDiff(_fb1, fb, getRotation(), _diff_gap, true, _compare_mask); // create a diff and copy
                    _flush_cache(_fb1, ILI9488_T4_NB_PIXELS * 2);
                    _updateAsync(_fb1, _diff1); // launch update
                }
                _mirrorfb = _fb1; // set as mirror
                return;
            }
        }
        }
    }

    void ILI9488Driver::_buffer2fullCB()
    {
        if (_mirrorfb)
        {
            _swapdiff();
            _swapfb();
            _mirrorfb = _fb1;
            _fb2full = false;
            _updateAsync(_fb1, _diff1); // launch update
        }
        else
        {
            _swapdummydiff();
            _swapfb();
            _mirrorfb = _fb1;
            _fb2full = false;
            _updateAsync(_fb1, _dummydiff1); // launch update
        }
        _setCB();               // disable itself, just in case.
        _ongoingDiff = nullptr; // just in case, but should already be nullptr.
    }

    void ILI9488Driver::_pushpixels_mode0(const uint16_t *fb, int x, int y, int len)
    {
        const uint16_t *p = fb + x + (y * ILI9488_T4_TFTWIDTH);
        while (len-- > 0)
        {
            _write16BitColor(*p++);
        }
    }

    void ILI9488Driver::_pushpixels_mode1(const uint16_t *fb, int xx, int yy, int len)
    {
        int x = yy;
        int y = ILI9488_T4_TFTWIDTH - 1 - xx;
        while (len-- > 0)
        {
            _write16BitColor(fb[x + ILI9488_T4_TFTHEIGHT * y]);
            y--;
            if (y < 0)
            {
                y = ILI9488_T4_TFTWIDTH - 1;
                x++;
            }
        }
    }

    void ILI9488Driver::_pushpixels_mode2(const uint16_t *fb, int xx, int yy, int len)
    {
        int x = ILI9488_T4_TFTWIDTH - 1 - xx;
        int y = ILI9488_T4_TFTHEIGHT - 1 - yy;
        const uint16_t *p = fb + x + (y * ILI9488_T4_TFTWIDTH);
        while (len-- > 0)
        {
            _write16BitColor(*p--);
        }
    }

    void ILI9488Driver::_pushpixels_mode3(const uint16_t *fb, int xx, int yy, int len)
    {
        int x = ILI9488_T4_TFTHEIGHT - 1 - yy;
        int y = xx;
        while (len-- > 0)
        {
            _write16BitColor(fb[x + ILI9488_T4_TFTHEIGHT * y]);
            y++;
            if (y >= ILI9488_T4_TFTWIDTH)
            {
                y = 0;
                x--;
            }
        }
    }

    void ILI9488Driver::_updateNow(const uint16_t *fb, DiffBuffBase *diff)
    {
        if ((fb == nullptr) || (diff == nullptr))
            return;
        waitUpdateAsyncComplete();
        _startframe(_vsync_spacing > 0);
        _margin = ILI9488_T4_NB_SCANLINES;
        _stats_nb_uploaded_pixels = 0;
        diff->initRead();
        int x = 0, y = 0, len = 0;
        int sc1 = diff->readDiff(x, y, len, 0); // scanline at 0 so sc1 will contain the scanline start position.
        if (sc1 < 0)
        { // Diff is empty
            if (_vsync_spacing > 0)
            { // note the next time.
                uint32_t t1 = micros() + _microToReachScanLine(0, true);
                uint32_t t2 = _timeframestart + (_vsync_spacing * _period);
                if ((t1 - t2 < _period / 3) && (t2 - t1 < _period / 3))
                {
                    t1 = t2;
                } // same frame.
                uint32_t tfs = ((_late_start_ratio_override) || (t1 > t2) || (t2 - t1 > (ILI9488_T4_MAX_VSYNC_SPACING + 1) * _period)) ? t1 : t2;
                if (tfs < _timeframestart)
                    tfs = t2;
                _late_start_ratio_override = false;
                _last_delta = (int)round(((double)(tfs - _timeframestart)) / (_period)); // number of refresh between this frame and the previous one.
                _timeframestart = tfs;
            }
            _endframe();
            return;
        }
        // ok we have at least one instruction
        if (_vsync_spacing > 0)
        {
            const uint32_t dd = (_timeframestart + ((_vsync_spacing - 1) * _period)) - micros();
            _pauseUploadTime();
            _delayMicro(dd); // wait until the previous frame is displayed the correct number of times.
            _restartUploadTime();
            // we should now be around scanline 0 (or possibly late).
            int sc2 = sc1 + ((ILI9488_T4_NB_SCANLINES - 1 - sc1) * _late_start_ratio); // maximum 'late' scanline
            uint32_t t2 = _microToReachScanLine(sc2, true);                            // with resync
            uint32_t t = _microToReachScanLine(sc1, false);                            // without resync
            if (_late_start_ratio_override)
            {                                       // force resync which is the same as asking _late_start_ratio=0;
                _late_start_ratio_override = false; // oneshot.
            }
            else
            {
                if (t2 < t)
                    t = 0; // late, start right away.
            }
            _pauseUploadTime();
            if (t > 0)
                delayMicroseconds(t); // wait if needed
            while ((t = _microToExitRange(0, sc1)))
            {
                delayMicroseconds(t);
            } // make sure we are good (in case delayMicroseconds() in not precise enough).
            _restartUploadTime();
            // ok, scanline is just after sc1 (if not late).
            _slinitpos = _getScanLine(false);                                // save initial scanline position
            _em_async = 0;                                                   // start the counter
            const uint32_t tfs = micros() + _microToReachScanLine(0, false); // time when this frame will start being displayed on the screen.
            _last_delta = (int)round(((double)(tfs - _timeframestart)) / (_period));
            _timeframestart = tfs;
        }
        _beginSPITransaction(_spi_clock);
        // write full PASET/CASET now and we shall only update the start position from now on.
        _writecommand_cont(ILI9488_T4_CASET);
        _writedata16_cont(x);
        _writedata16_cont(ILI9488_T4_TFTWIDTH);
        _writecommand_cont(ILI9488_T4_PASET);
        _writedata16_cont(y);
        _writedata16_last(ILI9488_T4_TFTHEIGHT);
        int prev_x = x;
        int prev_y = y;
        while (1)
        {
            int asl = (_vsync_spacing > 0) ? (_slinitpos + _nbScanlineDuring(_em_async)) : (2 * ILI9488_T4_TFTHEIGHT);
            int r = diff->readDiff(x, y, len, asl);
            if (r > 0)
            { // we must wait
                int t = _timeForScanlines(r - asl + 1);
                if (t < ILI9488_T4_MIN_WAIT_TIME)
                    t = ILI9488_T4_MIN_WAIT_TIME;
                _pauseUploadTime();
                _delayMicro(t);
                _restartUploadTime();
                continue;
            }
            if (r < 0)
            { // finished
                _writecommand_last(ILI9488_T4_NOP);
                _endSPITransaction();
                _endframe();
                return;
            }
            _stats_nb_uploaded_pixels += len;
            _stats_nb_transactions++;
            if (x != prev_x)
            {
                _writecommand_cont(ILI9488_T4_CASET);
                _writedata16_cont(x);
                prev_x = x;
            }
            if (y != prev_y)
            {
                _writecommand_cont(ILI9488_T4_PASET);
                _writedata16_cont(y);
                prev_y = y;
            }
            _writecommand_cont(ILI9488_T4_RAMWR);
            _pushpixels(fb, x, y, len);
            if (_vsync_spacing > 0)
            {
                int m = (ILI9488_T4_TFTWIDTH * y + x + len) / ILI9488_T4_TFTWIDTH + ILI9488_T4_TFTHEIGHT - _slinitpos - _nbScanlineDuring(_em_async);
                if (m < _margin)
                    _margin = m;
            }
        }
    }

    void ILI9488Driver::_updateRectNow(const uint16_t *sub_fb, int xmin, int xmax, int ymin, int ymax, int stride)
    {
        int x1, x2, y1, y2;
        DiffBuffBase::rotationBox(_rotation, xmin, xmax, ymin, ymax, x1, x2, y1, y2);
        const int w = x2 - x1 + 1;

        if ((sub_fb == nullptr) || (x2 < x1) || (y2 < y1))
            return;
        waitUpdateAsyncComplete();
        _startframe(false);
        _stats_nb_uploaded_pixels = 0;

        _beginSPITransaction(_spi_clock);
        _writecommand_cont(ILI9488_T4_CASET);
        _writedata16_cont(x1);
        _writedata16_cont(x2);
        _writecommand_cont(ILI9488_T4_PASET);
        _writedata16_cont(y1);
        _writedata16_cont(y2);
        _writecommand_cont(ILI9488_T4_RAMWR);

        int mdelta = 0;
        switch (_rotation)
        {
        case PORTRAIT_320x480:
            mdelta = 1;
            break;
        case LANDSCAPE_480x320:
            mdelta = -stride;
            break;
        case PORTRAIT_320x480_FLIPPED:
            mdelta = -1;
            break;
        case LANDSCAPE_480x320_FLIPPED:
            mdelta = stride;
            break;
        }
        for (int yc = y1; yc <= y2; yc++)
        {
            int m = 0;
            switch (_rotation)
            {
            case PORTRAIT_320x480:
                m = stride * (yc - y1);
                break;
            case LANDSCAPE_480x320:
                m = (yc - y1) + stride * (x2 - x1);
                break;
            case PORTRAIT_320x480_FLIPPED:
                m = stride * (y2 - yc) + (x2 - x1);
                break;
            case LANDSCAPE_480x320_FLIPPED:
                m = y2 - yc;
                break;
            }
            for (int n = 0; n < w; n++, m += mdelta)
                _write16BitColor(sub_fb[m]);
        }
        _writecommand_last(ILI9488_T4_NOP);
        _endSPITransaction();
        _endframe();
        return;
    }

    void ILI9488Driver::_updateAsync(const uint16_t *fb, DiffBuffBase *diff)
    {
        /*
        //override and use _updateNow instead
        int o = _rotation;
        _rotation = 0;
        _updateNow(fb, diff);
        _rotation = o;
        return;
        */
        if ((fb == nullptr) || (diff == nullptr))
            return; // do not call callback for invalid param.
        waitUpdateAsyncComplete();
        _startframe(_vsync_spacing > 0);
        _stats_nb_uploaded_pixels = 0;
        _margin = ILI9488_T4_NB_SCANLINES;
        _dma_state = ILI9488_T4_DMA_ON;
        _dmaObject[_spi_num] = this; // set up object callback.
        //_flush_cache(fb, 2 * ILI9488_T4_NB_PIXELS); // BEWARE THAT CACHE IF FLUSHED BEFORE CALLING THIS METHOD !
        _fb = fb;
        _diff = diff;
        diff->initRead();
        int x = 0, y = 0, len = 0;
        int sc1 = diff->readDiff(x, y, len, 0); // scanline at 0 so sc1 will contain the scanline start position.
        if (sc1 < 0)
        { // Diff is empty.
            _dmaObject[_spi_num] = nullptr;
            if (_vsync_spacing > 0)
            { // note the next time.
                uint32_t t1 = micros() + _microToReachScanLine(0, true);
                uint32_t t2 = _timeframestart + (_vsync_spacing * _period);
                if ((t1 - t2 < _period / 3) && (t2 - t1 < _period / 3))
                {
                    t2 = t1;
                } // same frame.
                uint32_t tfs = ((_late_start_ratio_override) || (t1 > t2) || (t2 - t1 > (ILI9488_T4_MAX_VSYNC_SPACING + 1) * _period)) ? t1 : t2;
                if (tfs < _timeframestart)
                    tfs = t2;
                _late_start_ratio_override = false;
                _last_delta = (int)round(((double)(tfs - _timeframestart)) / (_period)); // number of refresh between this frame and the previous one.
                _timeframestart = tfs;
            }
            _endframe();
            if (_touch_request_read)
            {
                _updateTouch2(); // touch update requested. do it now.
                _touch_request_read = false;
            }
            _dmaObject[_spi_num] = nullptr;
            _dma_state = ILI9488_T4_DMA_IDLE;
            if (_pcb)
            {
                (this->*_pcb)();
            }
            _pcb = nullptr; // remove it afterward.
            return;
        }

        // write full PASET/CASET now and we shall only update the start position from now on.
        _beginSPITransaction(_spi_clock);
        _writecommand_cont(ILI9488_T4_CASET);
        _writedata16_cont(x);
        _writedata16_cont(ILI9488_T4_TFTWIDTH);
        _writecommand_cont(ILI9488_T4_PASET);
        _writedata16_cont(y);
        _writedata16_last(ILI9488_T4_TFTHEIGHT);
        _endSPITransaction();
        _prev_caset_x = x;
        _prev_paset_y = y;
        _slinitpos = sc1; // save the requested scanline initial position

        if (_vsync_spacing <= 0)
        { // call next method asap
            _pauseUploadTime();
            _setTimerIn(1, &ILI9488Driver::_subFrameTimerStartcb); // call now
        }
        else
        { // time the call of the next method with vsync
            _pauseUploadTime();
            _setTimerAt(_timeframestart + (_vsync_spacing - 1) * _period, &ILI9488Driver::_subFrameTimerStartcb); // call at start of screen refresh.
        }

        _pauseCpuTime();
        return;
    }

    void ILI9488Driver::_subFrameTimerStartcb()
    {
        // we should be around scanline 0 (unless we are late).
        _restartCpuTime();
        _restartUploadTime();
        if (_vsync_spacing <= 0)
        {
            _pauseUploadTime();
            _setTimerIn(1, &ILI9488Driver::_subFrameTimerStartcb2);
        }
        else
        {
            int sc1 = _slinitpos;
            int sc2 = sc1 + ((ILI9488_T4_NB_SCANLINES - 1 - sc1) * _late_start_ratio); // maximum 'late' scanline
            uint32_t t2 = _microToReachScanLine(sc2, true);                            // with resync
            uint32_t t = _microToReachScanLine(sc1, false);                            // without resync
            if (_late_start_ratio_override)
            {                                       // force resync which is the same as asking _late_start_ratio=0;
                _late_start_ratio_override = false; // oneshot.
            }
            else
            {
                if (t2 < t)
                    t = 0; // late, start right away.
            }
            _pauseUploadTime();
            _setTimerIn(t, &ILI9488Driver::_subFrameTimerStartcb2); // call when ready to start transfer.
        }
        _pauseCpuTime();
        return;
    }

    void ILI9488Driver::_subFrameTimerStartcb2()
    {
        _restartUploadTime();
        _restartCpuTime();

        if (_vsync_spacing > 0)
        {
            int t;
            while ((t = _microToExitRange(0, _slinitpos)))
            {
                delayMicroseconds(t);
            } // make sure we are good
            // ok, scanline is just after sc1 (if not late).
            _slinitpos = _getScanLine(false);                                // save initial scanline position
            _em_async = 0;                                                   // start the counter
            const uint32_t tfs = micros() + _microToReachScanLine(0, false); // time when this frame will start being displayed on the screen.
            _last_delta = (int)round(((double)(tfs - _timeframestart)) / (_period));
            _timeframestart = tfs;
        }

        // read the first instruction
        int x = 0, y = 0, len = 0;
        int asl = (_vsync_spacing > 0) ? _slinitpos : (2 * ILI9488_T4_TFTHEIGHT);
        int r = _diff->readDiff(x, y, len, asl);
        if ((r != 0) || (len == 0) || (x != _prev_caset_x) || (y != _prev_paset_y))
        { // this should not happen, but try to fail gracefully.
            _endframe();
            if (_touch_request_read)
            {
                _updateTouch2(); // touch update requested. do it now.
                _touch_request_read = false;
            }
            _dmaObject[_spi_num] = nullptr;
            _dma_state = ILI9488_T4_DMA_IDLE;
            if (_pcb)
            {
                (this->*_pcb)();
            }
            _pcb = nullptr; // remove it afterward.
            return;
        }

        _dma_spi_tcr_assert = (_spi_tcr_current & ~ILI9488_T4_TCR_MASK) | (_tcr_dc_assert | LPSPI_TCR_FRAMESZ(7) | LPSPI_TCR_RXMSK);
        _dma_spi_tcr_deassert = (_spi_tcr_current & ~ILI9488_T4_TCR_MASK) | (_tcr_dc_not_assert | LPSPI_TCR_FRAMESZ(15) | LPSPI_TCR_RXMSK); // bug with | LPSPI_TCR_CONT

        _last_y = (ILI9488_T4_TFTWIDTH * y + x + len) / ILI9488_T4_TFTWIDTH;
        _stats_nb_uploaded_pixels = len;

        /* not used...
        _dmaRAMWR = ILI9488_T4_RAMWR;
        _dmasettingsDiff[0].sourceBuffer(&_dmaRAMWR, 1);
        _dmasettingsDiff[0].destination(_pimxrt_spi->TDR);
        _dmasettingsDiff[0].TCD->ATTR_DST = 0;
        _dmasettingsDiff[0].replaceSettingsOnCompletion(_dmasettingsDiff[1]);
        */

        _dmasettingsDiff[1].sourceBuffer(&_dma_spi_tcr_deassert, 4);
        _dmasettingsDiff[1].destination(_pimxrt_spi->TCR);
        _dmasettingsDiff[1].TCD->ATTR_DST = 2;
        _dmasettingsDiff[1].replaceSettingsOnCompletion(_dmasettingsDiff[2]);

        _dmasettingsDiff[2].sourceBuffer(_fb + x + (y * ILI9488_T4_TFTWIDTH), 2 * len);
        _dmasettingsDiff[2].destination(_pimxrt_spi->TDR);
        _dmasettingsDiff[2].TCD->ATTR_DST = 1;
        _dmasettingsDiff[2].replaceSettingsOnCompletion(_dmasettingsDiff[1]);
        _dmasettingsDiff[2].interruptAtCompletion();
        _dmasettingsDiff[2].disableOnCompletion();

        _dmatx = _dmasettingsDiff[1];

        _dmatx.triggerAtHardwareEvent(_spi_hardware->tx_dma_channel);
        if (_spi_num == 0)
            _dmatx.attachInterrupt(_dmaInterruptSPI0Diff);
        else if (_spi_num == 1)
            _dmatx.attachInterrupt(_dmaInterruptSPI1Diff);
        else
            _dmatx.attachInterrupt(_dmaInterruptSPI2Diff);

        // start spi transaction
        _beginSPITransaction(_spi_clock);

        _pimxrt_spi->FCR = 0;
        _maybeUpdateTCR(_tcr_dc_assert | LPSPI_TCR_FRAMESZ(7) | LPSPI_TCR_RXMSK); // bug with the continu flag | LPSPI_TCR_CONT , why ?
        _pimxrt_spi->DER = LPSPI_DER_TDDE;
        _pimxrt_spi->SR = 0x3f00;
        _pimxrt_spi->FCR = LPSPI_FCR_TXWATER(2); // CHOOSING LPSPI_FCR_TXWATER(0) = 0 MAY BE MUCH SAFER (BUT SLOWER) ????

        _pimxrt_spi->TCR = _dma_spi_tcr_assert;
        _pimxrt_spi->TDR = ILI9488_T4_RAMWR;

        NVIC_SET_PRIORITY(IRQ_DMA_CH0 + _dmatx.channel, ILI9488_T4_IRQ_PRIORITY);
        _dmatx.begin(false);
        _dmatx.enable(); // go !
        NVIC_SET_PRIORITY(IRQ_DMA_CH0 + _dmatx.channel, ILI9488_T4_IRQ_PRIORITY);
        _pauseCpuTime();
    }

    void ILI9488Driver::_subFrameInterruptDiff()
    {
        if (_vsync_spacing > 0)
        { // check margin when using vsync
            int m = _last_y + ILI9488_T4_TFTHEIGHT - _slinitpos - _nbScanlineDuring(_em_async);
            if (m < _margin)
                _margin = m;
        }
        int x = 0, y = 0, len = 0;
        int asl = (_vsync_spacing > 0) ? (_slinitpos + _nbScanlineDuring(_em_async)) : (2 * ILI9488_T4_TFTHEIGHT);
        int r = _diff->readDiff(x, y, len, asl);
        if (r < 0)
        { // we are done !
            while (_pimxrt_spi->FSR & 0x1f)
                ; // wait for transmit fifo to be empty
            while (_pimxrt_spi->SR & LPSPI_SR_MBF)
                ;                                                         // wait while spi bus is busy.
            _pimxrt_spi->FCR = LPSPI_FCR_TXWATER(15);                     // Transmit Data Flag (TDF) should now be set when there if less or equal than 15 words in the transmit fifo
            _pimxrt_spi->DER = 0;                                         // DMA no longer doing TX (nor RX)
            _pimxrt_spi->CR = LPSPI_CR_MEN | LPSPI_CR_RRF | LPSPI_CR_RTF; // enable module (MEM), reset RX fifo (RRF), reset TX fifo (RTF)
            _pimxrt_spi->SR = 0x3f00;                                     // clear out all of the other status...
            /*
            _maybeUpdateTCR(_tcr_dc_assert | LPSPI_TCR_FRAMESZ(7)); // output Command with 8 bits            
            _writecommand_last(ILI9488_T4_NOP);
            */
            _endSPITransaction();
            _endframe();
            if (_touch_request_read)
            {
                _updateTouch2(); // touch update requested. do it now.
                _touch_request_read = false;
            }
            // _flush_cache(_fb, 2 * ILI9488_T4_NB_PIXELS);   /// NOT USEFUL AFTER NO ????
            _dmaObject[_spi_num] = nullptr;
            _dma_state = ILI9488_T4_DMA_IDLE;
            if (_pcb)
            {
                (this->*_pcb)();
            }
            _pcb = nullptr; // remove it afterward.
            return;
        }
        else if (r > 0)
        { // we must wait
            int t = _timeForScanlines(r - asl + 1);
            if (t < ILI9488_T4_MIN_WAIT_TIME)
                t = ILI9488_T4_MIN_WAIT_TIME;
            //while (_pimxrt_spi->FSR & 0x1f);        // wait for transmit fifo to be empty
            //while (_pimxrt_spi->SR & LPSPI_SR_MBF); // wait while spi bus is busy.
            _pauseUploadTime();
            _setTimerIn(t, &ILI9488Driver::_subFrameInterruptDiff2);
            _pauseCpuTime();
            return;
        }
        // new instruction
        _pimxrt_spi->TCR = _dma_spi_tcr_assert;
        if (x != _prev_caset_x)
        {
            _pimxrt_spi->TDR = ILI9488_T4_CASET;
            _pimxrt_spi->TCR = _dma_spi_tcr_deassert;
            _pimxrt_spi->TDR = x;
            _pimxrt_spi->TCR = _dma_spi_tcr_assert;
            _prev_caset_x = x;
        }
        if (y != _prev_paset_y)
        {
            _pimxrt_spi->TDR = ILI9488_T4_PASET;
            _pimxrt_spi->TCR = _dma_spi_tcr_deassert;
            _pimxrt_spi->TDR = y;
            _pimxrt_spi->TCR = _dma_spi_tcr_assert;
            _prev_paset_y = y;
        }
        _pimxrt_spi->TDR = ILI9488_T4_RAMWR;

        _last_y = (ILI9488_T4_TFTWIDTH * y + x + len) / ILI9488_T4_TFTWIDTH;
        _stats_nb_uploaded_pixels += len;

        _dmasettingsDiff[2].sourceBuffer(_fb + x + (y * ILI9488_T4_TFTWIDTH), len * 2);
        _dmasettingsDiff[2].destination(_pimxrt_spi->TDR);
        _dmasettingsDiff[2].TCD->ATTR_DST = 1;
        _dmasettingsDiff[2].replaceSettingsOnCompletion(_dmasettingsDiff[1]);

        _dmatx.enable();
        return;
    }

    void ILI9488Driver::_subFrameInterruptDiff2()
    {
        noInterrupts();
        _restartUploadTime();
        _restartCpuTime();
        _subFrameInterruptDiff();
        _pauseCpuTime();
        interrupts();
    }

    void ILI9488Driver::_write16BitColor(uint16_t color, bool last_pixel)
    {
        uint8_t r = (color & 0xF800) >> 11;
        uint8_t g = (color & 0x07E0) >> 5;
        uint8_t b = color & 0x001F;
        r = (r * 255) / 31;
        g = (g * 255) / 63;
        b = (b * 255) / 31;
        uint32_t color24 = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

        if (last_pixel)
        {
            _maybeUpdateTCR(_tcr_dc_not_assert | LPSPI_TCR_FRAMESZ(23));
            _pimxrt_spi->TDR = color24;
            _pending_rx_count++; //
            _waitTransmitComplete();
        }
        else
        {
            _maybeUpdateTCR(_tcr_dc_not_assert | LPSPI_TCR_FRAMESZ(23) | LPSPI_TCR_CONT);
            _pimxrt_spi->TDR = color24;
            _pending_rx_count++; //
            _waitFifoNotFull();
        }
    }

    void ILI9488Driver::_write16BitColor(uint16_t color, uint16_t count, bool last_pixel)
    {
        uint8_t r = (color & 0xF800) >> 11;
        uint8_t g = (color & 0x07E0) >> 5;
        uint8_t b = color & 0x001F;
        r = (r * 255) / 31;
        g = (g * 255) / 63;
        b = (b * 255) / 31;
        uint32_t color24 = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

        while (count > 1)
        {
            _maybeUpdateTCR(_tcr_dc_not_assert | LPSPI_TCR_FRAMESZ(23) | LPSPI_TCR_CONT);
            _pimxrt_spi->TDR = color24;
            _pending_rx_count++; //
            _waitFifoNotFull();
            count--;
        }

        if (last_pixel)
        {
            _maybeUpdateTCR(_tcr_dc_not_assert | LPSPI_TCR_FRAMESZ(23));
            _pimxrt_spi->TDR = color24;
            _pending_rx_count++;
            _waitTransmitComplete();
        }
        else
        {
            _maybeUpdateTCR(_tcr_dc_not_assert | LPSPI_TCR_FRAMESZ(23) | LPSPI_TCR_CONT);
            _pimxrt_spi->TDR = color24;
            _pending_rx_count++; //
            _waitFifoNotFull();
        }
    }

    /**********************************************************************************************************
    * DMA Interrupts
    ***********************************************************************************************************/

    ILI9488Driver *volatile ILI9488Driver::_dmaObject[3] = {nullptr, nullptr, nullptr};

    void ILI9488Driver::_dmaInterruptDiff()
    {
        noInterrupts();
        _dmatx.clearInterrupt();
        _dmatx.clearComplete();
        _restartCpuTime();
        _stats_nb_transactions++; // count a spi transaction
        _subFrameInterruptDiff();
        _pauseCpuTime();
        interrupts();
        return;
    }

    /**********************************************************************************************************
    * IntervalTimer
    ***********************************************************************************************************/

    ILI9488Driver *volatile ILI9488Driver::_pitObj[4] = {nullptr, nullptr, nullptr, nullptr}; // definition

    void ILI9488Driver::_timerinit()
    {
        _istimer = false;
        for (int i = 0; i < 4; i++)
        {
            if (_pitObj[i] == nullptr)
            {
                _pitObj[i] = this;
                _pitindex = i;
                return;
            }
        }
        // OUCH !Boom boom boom booom...
        _print("\n *** TOO MANY INSTANCES OF ILI9488Driver CREATED ***\n\n");
    }

    /**********************************************************************************************************
    * SPI
    ***********************************************************************************************************/

    void ILI9488Driver::_drawRect(int xmin, int xmax, int ymin, int ymax, uint16_t color)
    {
        waitUpdateAsyncComplete();
        _beginSPITransaction(_spi_clock);
        _writecommand_cont(ILI9488_T4_PASET);
        _writedata16_cont(ymin);
        _writedata16_cont(ymax);
        _writecommand_cont(ILI9488_T4_CASET);
        _writedata16_cont(xmin);
        _writedata16_cont(xmax);
        _writecommand_cont(ILI9488_T4_RAMWR);
        for (int i = 0; i < (xmax - xmin + 1) * (ymax - ymin + 1); i++)
            _write16BitColor(color);
        _writecommand_last(ILI9488_T4_NOP);
        _endSPITransaction();
        _mirrorfb = nullptr;
        _ongoingDiff = nullptr;
    }

    uint8_t ILI9488Driver::_readcommand8(uint8_t c, uint8_t index, int timeout_ms)
    {
        // Bail if not valid miso
        if (_miso == 0xff)
            return 0;
        uint8_t r = 0;
        _beginSPITransaction(_spi_clock_read);
        // Lets assume that queues are empty as we just started transaction.
        _pimxrt_spi->CR = LPSPI_CR_MEN | LPSPI_CR_RRF /* | LPSPI_CR_RTF */; // actually clear both...
        _maybeUpdateTCR(_tcr_dc_assert | LPSPI_TCR_FRAMESZ(7) | LPSPI_TCR_CONT);
        _pimxrt_spi->TDR = 0xD9; // writecommand(0xD9); // sekret command
        _maybeUpdateTCR(_tcr_dc_not_assert | LPSPI_TCR_FRAMESZ(7) | LPSPI_TCR_CONT);
        _pimxrt_spi->TDR = 0x10 + index; // writedata(0x10 + index);
        _maybeUpdateTCR(_tcr_dc_assert | LPSPI_TCR_FRAMESZ(7) | LPSPI_TCR_CONT);
        _pimxrt_spi->TDR = c; // writecommand(c);
        _maybeUpdateTCR(_tcr_dc_not_assert | LPSPI_TCR_FRAMESZ(7));
        _pimxrt_spi->TDR = 0; // readdata
        // Now wait until completed.
        elapsedMillis ems;
        uint8_t rx_count = 4;
        while (rx_count && ((timeout_ms <= 0) || (ems < (uint32_t)timeout_ms)))
        {
            if ((_pimxrt_spi->RSR & LPSPI_RSR_RXEMPTY) == 0)
            {
                r = _pimxrt_spi->RDR; // Read any pending RX bytes in
                rx_count--;           // decrement count of bytes still levt
            }
        }
        _endSPITransaction();
        if (rx_count)
            return 0; // timeout
        return r;     // get the received byte... should check for it first...
    }

    void ILI9488Driver::_waitFifoNotFull()
    {
        uint32_t tmp __attribute__((unused));
        do
        {
            if ((_pimxrt_spi->RSR & LPSPI_RSR_RXEMPTY) == 0)
            {
                tmp = _pimxrt_spi->RDR; // Read any pending RX bytes in
                if (_pending_rx_count)
                    _pending_rx_count--; // decrement count of bytes still levt
            }
        } while ((_pimxrt_spi->SR & LPSPI_SR_TDF) == 0);
    }

    void ILI9488Driver::_waitTransmitComplete()
    {
        uint32_t tmp __attribute__((unused));
        while (_pending_rx_count)
        {
            if ((_pimxrt_spi->RSR & LPSPI_RSR_RXEMPTY) == 0)
            {
                tmp = _pimxrt_spi->RDR; // Read any pending RX bytes in
                _pending_rx_count--;    // decrement count of bytes still levt
            }
        }
        _pimxrt_spi->CR = LPSPI_CR_MEN | LPSPI_CR_RRF; // Clear RX FIFO
    }

    /**********************************************************************************************************
    * Statistics
    ***********************************************************************************************************/

    FLASHMEM void ILI9488Driver::statsReset()
    {
        _stats_nb_frame = 0;
        _stats_elapsed_total = 0;
        _statsvar_cputime.reset();
        _statsvar_uploadtime.reset();
        _statsvar_uploaded_pixels.reset();
        _statsvar_transactions.reset();
        _statsvar_margin.reset();
        _statsvar_vsyncspacing.reset();
        _nbteared = 0;
    }

    FLASHMEM void ILI9488Driver::printStats() const
    {
        //waitUpdateAsyncComplete();
        _print("----------------- ILI9488Driver Stats ----------------\n");
        _print("[Configuration]\n");
        _printf("- SPI speed          : write=%u  read=%u\n", _spi_clock, _spi_clock_read);
        _print("- screen orientation : ");
        switch (getRotation())
        {
        case 0:
            _print("0 (PORTRAIT_320x480)\n");
            break;
        case 1:
            _print("1 (LANDSCAPE_480x320)\n");
            break;
        case 2:
            _print("2 (PORTRAIT_320x480_FLIPPED)\n");
            break;
        case 3:
            _print("3 (LANDSCAPE_480x320_FLIPPED)\n");
            break;
        }

        _printf("- refresh rate       : %.1fHz  (mode %u)\n", getRefreshRate(), getRefreshMode());
        int m = bufferingMode();
        _printf("- buffering mode     : %u", m);
        switch (m)
        {
        case NO_BUFFERING:
            _print(" (NO BUFFERING)\n");
            break;
        case DOUBLE_BUFFERING:
            _print(" (DOUBLE BUFFERING)\n");
            break;
        case TRIPLE_BUFFERING:
            _print(" (TRIPLE BUFFERING)\n");
            break;
        }
        _printf("- vsync_spacing      : %i ", _vsync_spacing);
        if (_vsync_spacing <= 0)
            _print(" (VSYNC DISABLED).\n");
        else
            _print(" (VSYNC ENABLED).\n");

        _print("- requested FPS      : ");
        if (_vsync_spacing == -1)
            _print("max fps [drop frames when busy]\n");
        else if (_vsync_spacing == 0)
            _print("max fps [do not drop frames]\n");
        else
            _printf("%.1fHz [=refresh_rate/vsync_spacing]\n", getRefreshRate() / _vsync_spacing);

        if (diffUpdateActive())
        {
            if (_diff2)
            {
                _print("- diff. updates      : ENABLED - 2 diffs buffers.\n");
            }
            else
            {
                _print("- diff. updates      : ENABLED - 1 diff buffer.\n");
            }
            _printf("- diff [gap]         : %u\n", _diff_gap);
            if (_compare_mask == 0)
            {
                _print("- diff [compare_mask]: STRICT COMPARISON.");
            }
            else
            {
                _print("- diff [compare_mask]: R=");
                for (int i = 15; i >= 11; i--)
                {
                    _print(bitRead(_compare_mask, i) ? '1' : '0');
                }
                _print(" G=");
                for (int i = 10; i >= 5; i--)
                {
                    _print(bitRead(_compare_mask, i) ? '1' : '0');
                }
                _print(" B=");
                for (int i = 4; i >= 0; i--)
                {
                    _print(bitRead(_compare_mask, i) ? '1' : '0');
                }
            }
        }
        else
        {
            if (_diff1 == nullptr)
                _print("- diff. updates      : DISABLED.\n");
            else
                _print("- differential update: DISABLED [ONLY 1 DIFF BUFFER PROVIDED WHEN 2 ARE NEEDED WITH TRIPLE BUFFERING]\n");
        }

        _print("\n\n[Statistics]\n");
        _printf("- average framerate  : %.1f FPS  (%u frames in %ums)\n", statsFramerate(), statsNbFrames(), statsTotalTime());
        if (diffUpdateActive())
            _printf("- upload rate        : %.1f FPS  (%.2fx compared to full redraw)\n", 1000000.0f / _statsvar_uploadtime.avg(), statsDiffSpeedUp());
        else
            _printf("- upload rate        : %.1f FPS\n", 1000000.0f / _statsvar_uploadtime.avg());
        _print("- upload time / frame: ");
        _statsvar_uploadtime.print("us", "\n", _outputStream);
        _print("- CPU time / frame   : ");
        _statsvar_cputime.print("us", "\n", _outputStream);
        _print("- pixels / frame     : ");
        _statsvar_uploaded_pixels.print("", "\n", _outputStream);
        _print("- transact. / frame  : ");
        _statsvar_transactions.print("", "\n", _outputStream);
        if (_vsync_spacing > 0)
        {
            _printf("- teared frames      : %u (%.1f%%)\n", statsNbTeared(), 100 * statsRatioTeared());
            _print("- real vsync spacing : ");
            _statsvar_vsyncspacing.print("", "\n", _outputStream, true);
            _print("- margin / frame     : ");
            _statsvar_margin.print("", "\n", _outputStream);
        }
        _print("\n");
    }

    void ILI9488Driver::_endframe()
    {
        _stats_nb_frame++;

        _stats_cputime += _stats_elapsed_cputime;
        _statsvar_cputime.push(_stats_cputime);

        _stats_uploadtime += _stats_elapsed_uploadtime;
        _statsvar_uploadtime.push(_stats_uploadtime);

        _statsvar_uploaded_pixels.push(_stats_nb_uploaded_pixels);

        _statsvar_transactions.push(_stats_nb_transactions);

        if (_vsync_spacing > 0)
        {
            if (_statsvar_margin.count() > 0)
                _statsvar_vsyncspacing.push(_last_delta);

            if (_margin < 0)
                _nbteared++;

            _statsvar_margin.push(_margin);
        }
    }

    /**********************************************************************************************************
    * Touch
    ***********************************************************************************************************/

    ILI9488Driver *volatile ILI9488Driver::_touchObjects[4] = {nullptr, nullptr, nullptr, nullptr};

    /** set the touch interrupt routine */
    FLASHMEM void ILI9488Driver::_setTouchInterrupt()
    {
        _touch_z_threshold = ILI9488_T4_TOUCH_Z_THRESHOLD;
        _touch_has_calibration = false;

        _touch_request_read = false;
        _touched = true;
        ;
        _touched_read = true;
        _touch_x = _touch_y = _touch_z = 0;

        bool slotfound = false;
        if ((_touch_irq >= 0) && (_touch_irq < 42)) // valid digital pin
        {
            pinMode(_touch_irq, INPUT);
            if ((!slotfound) && (_touchObjects[0] == nullptr))
            {
                _touchObjects[0] = this;
                attachInterrupt(_touch_irq, _touch_int0, FALLING);
                slotfound = true;
            }
            if ((!slotfound) && (_touchObjects[1] == nullptr))
            {
                _touchObjects[1] = this;
                attachInterrupt(_touch_irq, _touch_int1, FALLING);
                slotfound = true;
            }
            if ((!slotfound) && (_touchObjects[2] == nullptr))
            {
                _touchObjects[2] = this;
                attachInterrupt(_touch_irq, _touch_int2, FALLING);
                slotfound = true;
            }
            if ((!slotfound) && (_touchObjects[3] == nullptr))
            {
                _touchObjects[3] = this;
                attachInterrupt(_touch_irq, _touch_int3, FALLING);
                slotfound = true;
            }
        }
        if (!slotfound)
        {
            _touch_irq = 255;
        } // disable touch irq
    }

    int32_t ILI9488Driver::lastTouched()
    {
        const bool b = _touched;
        _touched = false;
        return (b && (_touch_irq != 255)) ? ((int32_t)_em_touched_irq) : -1;
    }

    void ILI9488Driver::_updateTouch2()
    {
        int16_t data[6];
        int z;
        _pspi->beginTransaction(SPISettings(_spi_clock_read, MSBFIRST, SPI_MODE0));
        digitalWrite(_touch_cs, LOW);
        _pspi->transfer(0xB1);
        int16_t z1 = _pspi->transfer16(0xC1 /* Z2 */) >> 3;
        z = z1 + 4095;
        int16_t z2 = _pspi->transfer16(0x91 /* X */) >> 3;
        z -= z2;
        if (z >= _touch_z_threshold)
        {
            _pspi->transfer16(0x91 /* X */); // dummy X measure, 1st is always noisy
            data[0] = _pspi->transfer16(0xD1 /* Y */) >> 3;
            data[1] = _pspi->transfer16(0x91 /* X */) >> 3; // make 3 x-y measurements
            data[2] = _pspi->transfer16(0xD1 /* Y */) >> 3;
            data[3] = _pspi->transfer16(0x91 /* X */) >> 3;
        }
        else
        {
            data[0] = data[1] = data[2] = data[3] = 0; // Compiler warns these values may be used unset on early exit.
        }
        data[4] = _pspi->transfer16(0xD0 /* Y */) >> 3; // Last Y touch power down
        data[5] = _pspi->transfer16(0) >> 3;
        digitalWrite(_touch_cs, HIGH);
        _pspi->endTransaction();

        if (z < _touch_z_threshold)
        {
            _touch_z = 0;
            if (z < ILI9488_T4_TOUCH_Z_THRESHOLD_INT)
            {
                if (_touch_irq != 255)
                    _touched_read = false;
            }
            return;
        }

        int x = _besttwoavg(data[1], data[3], data[5]);
        int y = _besttwoavg(data[0], data[2], data[4]);

        _touch_x = x;
        _touch_y = y;
        _touch_z = z;
        _em_touched_read = 0; // good read completed, set wait
    }

    void ILI9488Driver::_updateTouch()
    {
        if (_em_touched_read < ILI9488_T4_TOUCH_MSEC_THRESHOLD)
            return; // read not so long ago
        if ((_touch_irq != 255) && (_touched_read == false))
            return; // nothing to do.
        if (asyncUpdateActive())
        {
            _touch_request_read = true; // request read at end of transfer
            while ((_touch_request_read) && (asyncUpdateActive()))
                ; // wait until transfer complete or reading done.
            if (!_touch_request_read)
                return;                  // reading was done, nothing to do.
            _touch_request_read = false; // remove request.
        }
        // we can do the reading now
        _updateTouch2();
        return;
    }

    bool ILI9488Driver::readTouch(int &x, int &y, int &z)
    {
        _updateTouch();
        if (_touch_z < _touch_z_threshold)
            return false;
        z = _touch_z;
        if (_touch_has_calibration)
        { // coord in orientation 0.
            int xx = _mapTouchX(_touch_x, _touch_calib[0], _touch_calib[1]);
            int yy = _mapTouchY(_touch_y, _touch_calib[2], _touch_calib[3]);
            switch (_rotation)
            {
            case 0:
                x = xx;
                y = yy;
                break;
            case 1:
                x = yy;
                y = ILI9488_T4_TFTWIDTH - 1 - xx;
                break;
            case 2:
                x = ILI9488_T4_TFTWIDTH - 1 - xx;
                y = ILI9488_T4_TFTHEIGHT - 1 - yy;
                break;
            case 3:
                x = ILI9488_T4_TFTHEIGHT - 1 - yy;
                y = xx;
                break;
            }
        }
        else
        { // raw values
            x = _touch_x;
            y = _touch_y;
        }
        return true;
    }

    int16_t ILI9488Driver::_besttwoavg(int16_t x, int16_t y, int16_t z)
    {
        int16_t da, db, dc;
        int16_t reta = 0;
        if (x > y)
            da = x - y;
        else
            da = y - x;
        if (x > z)
            db = x - z;
        else
            db = z - x;
        if (z > y)
            dc = z - y;
        else
            dc = y - z;
        if (da <= db && da <= dc)
            reta = (x + y) >> 1;
        else if (db <= da && db <= dc)
            reta = (x + z) >> 1;
        else
            reta = (y + z) >> 1; //    else if ( dc <= da && dc <= db ) reta = (x + y) >> 1;
        return (reta);
    }

    FLASHMEM void ILI9488Driver::setTouchCalibration(int touchCalibration[4])
    {
        if (touchCalibration)
        {
            _touch_has_calibration = true;
            for (int i = 0; i < 4; i++)
                _touch_calib[i] = touchCalibration[i];
        }
        else
        {
            _touch_has_calibration = false;
        }
    }

    FLASHMEM bool ILI9488Driver::getTouchCalibration(int touchCalibration[4])
    {
        if (_touch_has_calibration)
        {
            for (int i = 0; i < 4; i++)
                touchCalibration[i] = _touch_calib[i];
            return true;
        }
        else
        {
            return false;
        }
    }

    FLASHMEM void ILI9488Driver::_calibRect(int cx, int cy, int R)
    {
        const int R2 = R;
        const int R1 = R / 2;
        const uint16_t RED = 31 << 11;
        const uint16_t GREEN = 63 << 5;
        _beginSPITransaction(_spi_clock);
        _writecommand_cont(ILI9488_T4_PASET);
        _writedata16_cont(cy - R2);
        _writedata16_cont(cy + R2);
        _writecommand_cont(ILI9488_T4_CASET);
        _writedata16_cont(cx - R2);
        _writedata16_cont(cx + R2);
        _writecommand_cont(ILI9488_T4_RAMWR);
        for (int j = -R2; j <= R2; j++)
        {
            for (int i = -R2; i <= R2; i++)
            {
                uint16_t color = ((j >= -R1) && (j <= R1) && (i >= -R1) && (i <= R1)) ? RED : GREEN;
                _write16BitColor(color);
            }
        }
        _writecommand_last(ILI9488_T4_NOP);
        _endSPITransaction();
    }

    FLASHMEM void ILI9488Driver::_calibTouch(int &x, int &y, int &z, int prv_x, int prv_y)
    {
        const int NB_SAMPLE = 1;
        const int MIN_DIST = 500;
        do
        {
            _updateTouch();
            delay(10);
        } while (_touch_z > 0);

        while (1)
        {
            int nbs = 0;
            x = 0;
            y = 0;
            while (nbs < NB_SAMPLE)
            {
                _touch_z = 0;
                _updateTouch();
                if (_touch_z >= _touch_z_threshold)
                {
                    nbs++;
                    x += _touch_x;
                    y += _touch_y;
                }
                delay(10);
            }
            x /= NB_SAMPLE;
            y /= NB_SAMPLE;
            if (((abs(x - prv_x) > MIN_DIST) || (prv_x < 0)) || ((abs(y - prv_y) > MIN_DIST) || (prv_y < 0)))
                return;
        }
    }

    FLASHMEM void ILI9488Driver::calibrateTouch(int touchCalibration[4])
    {
        waitUpdateAsyncComplete();
        const int RADIUS = 6;
        _print("\n\n------------- Touch Calibration ---------------\n");
        int x[4];
        int y[4];
        int z[4];

        _print("\n- First corner: touch the center of the green/red rectangle... ");
        clear(0);
        _calibRect(RADIUS, RADIUS, RADIUS);
        _calibTouch(x[0], y[0], z[0]);
        _printf("\n%d  %d  %d\n", x[0], y[0], z[0]);

        _print("\n- Second corner: touch the center of the green/red rectangle... ");
        clear(0);
        _calibRect(ILI9488_T4_TFTWIDTH - 1 - RADIUS, RADIUS, RADIUS);
        _calibTouch(x[1], y[1], z[1], x[0], y[0]);
        _printf("\n%d  %d  %d\n", x[1], y[1], z[1]);

        _print("\n- Third corner: touch the center of the green/red rectangle... ");
        clear(0);
        _calibRect(ILI9488_T4_TFTWIDTH - 1 - RADIUS, ILI9488_T4_TFTHEIGHT - 1 - RADIUS, RADIUS);
        _calibTouch(x[2], y[2], z[2], x[1], y[1]);
        _printf("\n%d  %d  %d\n", x[2], y[2], z[2]);

        _print("\n- Fourth corner: touch the center of the green/red rectangle... ");
        clear(0);
        _calibRect(RADIUS, ILI9488_T4_TFTHEIGHT - 1 - RADIUS, RADIUS);
        _calibTouch(x[3], y[3], z[3], x[2], y[2]);
        _printf("\n%d  %d  %d\n", x[3], y[3], z[3]);

        float xa = (x[0] + x[3]) / 2.0f;
        float xb = (x[1] + x[2]) / 2.0f;
        float ya = (y[0] + y[1]) / 2.0f;
        float yb = (y[2] + y[3]) / 2.0f;

        const float xc = (xa + xb) / 2;
        const float ex = ((float)ILI9488_T4_TFTWIDTH) / (ILI9488_T4_TFTWIDTH - 2 * RADIUS);
        xa = xc + (xa - xc) * ex;
        xb = xc + (xb - xc) * ex;

        const float yc = (ya + yb) / 2;
        const float ey = ((float)ILI9488_T4_TFTHEIGHT) / (ILI9488_T4_TFTHEIGHT - 2 * RADIUS);
        ya = yc + (ya - yc) * ey;
        yb = yc + (yb - yc) * ey;

        int touch_calib[4];
        touch_calib[0] = (int)roundf(xa);
        touch_calib[1] = (int)roundf(xb);
        touch_calib[2] = (int)roundf(ya);
        touch_calib[3] = (int)roundf(yb);

        setTouchCalibration(touch_calib);
        if (touchCalibration)
        {
            for (int i = 0; i < 4; i++)
                touchCalibration[i] = touch_calib[i];
        }

        _printf("\n\nCalibration values = {%d, %d, %d, %d }\n\n", touch_calib[0], touch_calib[1], touch_calib[2], touch_calib[3]);
        _print("Test calibration by drawing on the white background.\nExit calibration by clicking on the green/red rectangle.\n\n");

        clear(0xFFFF);
        const int RADIUS2 = 20;
        _calibRect(RADIUS2, RADIUS2, RADIUS2);

        const int _oldrotation = _rotation; // save orientation
        _rotation = 0;
        while (1)
        {
            delay(1);
            int x, y, z;
            if (readTouch(x, y, z))
            {
                if ((x <= 2 * RADIUS2) && (y <= 2 * RADIUS2))
                { // exit
                    _print("------------- end of calibration --------------\n\n");
                    _rotation = _oldrotation; // restore orientation
                    _mirrorfb = nullptr;
                    _ongoingDiff = nullptr;
                    resync();
                    return;
                }
                // draw a white dot of size 3x3 at pos (xx,yy)
                _beginSPITransaction(_spi_clock);
                _writecommand_cont(ILI9488_T4_PASET);
                _writedata16_cont(y - 2);
                _writedata16_cont(y + 2);
                _writecommand_cont(ILI9488_T4_CASET);
                _writedata16_cont(x - 2);
                _writedata16_cont(x + 2);
                _writecommand_cont(ILI9488_T4_RAMWR);
                for (int i = 0; i < 25; i++)
                    _write16BitColor(0);
                _writecommand_last(ILI9488_T4_NOP);
                _endSPITransaction();
            }
        }
    }

}

/** end of file */
