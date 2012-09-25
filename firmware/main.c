/* Name: main.c
 * Project: USBaspLoader
 * Author: Christian Starkjohann
 * Creation Date: 2007-12-08
 * Tabsize: 4
 * Copyright: (c) 2007 by OBJECTIVE DEVELOPMENT Software GmbH
 * Portions Copyright: (c) 2012 Louis Beaudoin
 * License: GNU GPL v2 (see License.txt)
 * This Revision: $Id: main.c 786 2010-05-30 20:41:40Z cs $
 */
 
#define UBOOT_VERSION 2
// how many milliseconds should host wait till it sends another erase or write?
// needs to be above 4.5 (and a whole integer) as avr freezes for 4.5ms
#define UBOOT_WRITE_SLEEP 8


#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <avr/boot.h>
//#include <avr/eeprom.h>
#include <util/delay.h>
//#include <string.h>

static void leaveBootloader() __attribute__((__noreturn__));

#include "bootloaderconfig.h"
#include "usbdrv/usbdrv.c"

/* ------------------------------------------------------------------------ */

#ifndef ulong
#   define ulong    unsigned long
#endif
#ifndef uint
#   define uint     unsigned int
#endif

#ifndef BOOTLOADER_CAN_EXIT
#   define  BOOTLOADER_CAN_EXIT     0
#endif

/* allow compatibility with avrusbboot's bootloaderconfig.h: */
#ifdef BOOTLOADER_INIT
#   define bootLoaderInit()         BOOTLOADER_INIT
#   define bootLoaderExit()
#endif
#ifdef BOOTLOADER_CONDITION
#   define bootLoaderCondition()    BOOTLOADER_CONDITION
#endif

/* device compatibility: */
#ifndef GICR    /* ATMega*8 don't have GICR, use MCUCR instead */
#   define GICR     MCUCR
#endif

/* ------------------------------------------------------------------------ */

#define addr_t uint

// typedef union longConverter{
//     addr_t  l;
//     uint    w[sizeof(addr_t)/2];
//     uchar   b[sizeof(addr_t)];
// } longConverter_t;

//////// Stuff Bluebie Added
// postscript are the few bytes at the end of programmable memory which store tinyVectors
// and used to in USBaspLoader-tiny85 store the checksum iirc
#define POSTSCRIPT_SIZE 6 /* maybe it could be 4 now we do not have checksums? */
#define PROGMEM_SIZE (BOOTLOADER_ADDRESS - POSTSCRIPT_SIZE) /* max size of user program */

// verify the bootloader address aligns with page size
#if BOOTLOADER_ADDRESS % SPM_PAGESIZE != 0
#  error "BOOTLOADER_ADDRESS in makefile must be a multiple of chip's pagesize"
#endif

// events system schedules functions to run in the main loop
static uchar events = 0; // bitmap of events to run
#define EVENT_ERASE_APPLICATION 1
#define EVENT_WRITE_PAGE 2
#define EVENT_FINISH 4

// controls state of events
#define fireEvent(event) events |= (event)
#define isEvent(event)   (events & (event))
#define clearEvents()    events = 0

// length of bytes to write in to flash memory in upcomming usbFunctionWrite calls
static uchar writeLength;

// becomes 1 when some programming happened
// lets leaveBootloader know if needs to finish up the programming
static uchar didWriteSomething = 0;






static uint16_t vectorTemp[2]; // remember data to create tinyVector table before BOOTLOADER_ADDRESS
static addr_t currentAddress; // current progmem address, used for erasing and writing


/* ------------------------------------------------------------------------ */
static inline void eraseApplication(void);
static void writeFlashPage(void);
static void writeWordToPageBuffer(uint16_t data);
static void fillFlashWithVectors(void);
static uchar usbFunctionSetup(uchar data[8]);
static uchar usbFunctionWrite(uchar *data, uchar length);
static inline void initForUsbConnectivity(void);
static inline void tiny85FlashInit(void);
static inline void tiny85FlashWrites(void);
static inline void tiny85FinishWriting(void);
static inline __attribute__((noreturn)) void leaveBootloader(void);

// erase any existing application and write in jumps for usb interrupt and reset to bootloader
//  - Because flash can be erased once and programmed several times, we can write the bootloader
//  - vectors in now, and write in the application stuff around them later.
//  - if vectors weren't written back in immidately, usb would fail.
static inline void eraseApplication(void) {
    // erase all pages until bootloader, in reverse order (so our vectors stay in place for as long as possible)
    // while the vectors don't matter for usb comms as interrupts are disabled during erase, it's important
    // to minimise the chance of leaving the device in a state where the bootloader wont run, if there's power failure
    // during upload
    currentAddress = BOOTLOADER_ADDRESS;
    cli();
    while (currentAddress) {
        currentAddress -= SPM_PAGESIZE;
        
        boot_page_erase(currentAddress);
        boot_spm_busy_wait();
    }
    
    fillFlashWithVectors();
    sei();
}

// simply write currently stored page in to already erased flash memory
static void writeFlashPage(void) {
    didWriteSomething = 1;
    cli();
    boot_page_write(currentAddress - 2);
    boot_spm_busy_wait(); // Wait until the memory is written.
    sei();
}

// clear memory which stores data to be written by next writeFlashPage call
#define __boot_page_fill_clear()   \
(__extension__({                                 \
    __asm__ __volatile__                         \
    (                                            \
        "sts %0, %1\n\t"                         \
        "spm\n\t"                                \
        :                                        \
        : "i" (_SFR_MEM_ADDR(__SPM_REG)),        \
          "r" ((uint8_t)(__BOOT_PAGE_FILL | (1 << CTPB)))     \
    );                                           \
}))

// write a word in to the page buffer, doing interrupt table modifications where they're required
static void writeWordToPageBuffer(uint16_t data) {
    // first two interrupt vectors get replaced with a jump to the bootloader vector table
    if (currentAddress == (RESET_VECTOR_OFFSET * 2) || currentAddress == (USBPLUS_VECTOR_OFFSET * 2)) {
        data = 0xC000 + (BOOTLOADER_ADDRESS/2) - 1;
    }

    if (currentAddress == BOOTLOADER_ADDRESS - TINYVECTOR_RESET_OFFSET) {
        data = vectorTemp[0] + ((FLASHEND + 1) - BOOTLOADER_ADDRESS)/2 + 2 + RESET_VECTOR_OFFSET;
    }
    
    if (currentAddress == BOOTLOADER_ADDRESS - TINYVECTOR_USBPLUS_OFFSET) {
        data = vectorTemp[1] + ((FLASHEND + 1) - BOOTLOADER_ADDRESS)/2 + 1 + USBPLUS_VECTOR_OFFSET;
    }
    
    
    // clear page buffer as a precaution before filling the buffer on the first page
    // in case the bootloader somehow ran after user program and there was something
    // in the page buffer already
    if (currentAddress == 0x0000) __boot_page_fill_clear();
    
    cli();
    boot_page_fill(currentAddress, data);
    sei();
    
	// only need to erase if there is data already in the page that doesn't match what we're programming
	// TODO: what about this: if (pgm_read_word(currentAddress) & data != data) { ??? should work right?
	//if (pgm_read_word(currentAddress) != data && pgm_read_word(currentAddress) != 0xFFFF) {
    //if ((pgm_read_word(currentAddress) & data) != data) {
    //    fireEvent(EVENT_PAGE_NEEDS_ERASE);
    //}
    
    // increment progmem address by one word
    currentAddress += 2;
}

// fills the rest of this page with vectors - interrupt vector or tinyvector tables where needed
static void fillFlashWithVectors(void) {
    int16_t i;

    // fill all or remainder of page with 0xFFFF (as if unprogrammed)
    for (i = currentAddress % SPM_PAGESIZE; i < SPM_PAGESIZE; i += 2) {
        writeWordToPageBuffer(0xFFFF); // is where vector tables are sorted out
    }

    writeFlashPage();
}

/* ------------------------------------------------------------------------ */

static uchar usbFunctionSetup(uchar data[8]) {
    usbRequest_t *rq = (void *)data;
    static uchar replyBuffer[4] = {
        (((uint)PROGMEM_SIZE) >> 8) & 0xff,
        ((uint)PROGMEM_SIZE) & 0xff,
        SPM_PAGESIZE,
        UBOOT_WRITE_SLEEP
    };
    
    if (rq->bRequest == 0) { // get device info
        usbMsgPtr = replyBuffer;
        return 4;
        
    } else if (rq->bRequest == 1) { // write page
        writeLength = rq->wValue.word;
        currentAddress = rq->wIndex.word;
        
        return USB_NO_MSG; // hands off work to usbFunctionWrite
        
    } else if (rq->bRequest == 2) { // erase application
        fireEvent(EVENT_ERASE_APPLICATION);
        
    } else { // exit bootloader
#       if BOOTLOADER_CAN_EXIT
            fireEvent(EVENT_FINISH);
#       endif
    }
    
    return 0;
}


// read in a page over usb, and write it in to the flash write buffer
static uchar usbFunctionWrite(uchar *data, uchar length) {
    writeLength -= length;
    
    do {
        // remember vectors or the tinyvector table 
        if (currentAddress == RESET_VECTOR_OFFSET * 2) {
            vectorTemp[0] = *(short *)data;
        }
        
        if (currentAddress == USBPLUS_VECTOR_OFFSET * 2) {
            vectorTemp[1] = *(short *)data;
        }
        
        // make sure we don't write over the bootloader!
        if (currentAddress >= PROGMEM_SIZE) {
            __boot_page_fill_clear();
            break;
        }
        
        writeWordToPageBuffer(*(uint16_t *) data);
        data += 2; // advance data pointer
        length -= 2;
    } while(length);
    
    // TODO: Isn't this always last?
    // if we have now reached another page boundary, we're done
    uchar isLast = (writeLength == 0);
    if (isLast) fireEvent(EVENT_WRITE_PAGE); // ask runloop to write our page
    
    return isLast; // let vusb know we're done with this request
}

/* ------------------------------------------------------------------------ */

void PushMagicWord (void) __attribute__ ((naked)) __attribute__ ((section (".init3")));

// put the word "B007" at the bottom of the stack (RAMEND - RAMEND-1)
void PushMagicWord (void) {
    asm volatile("ldi r16, 0xB0"::);
    asm volatile("push r16"::);
    asm volatile("ldi r16, 0x07"::);
    asm volatile("push r16"::);
}

/* ------------------------------------------------------------------------ */

static inline void initForUsbConnectivity(void) {
    usbInit();
    /* enforce USB re-enumerate: */
    usbDeviceDisconnect();  /* do this while interrupts are disabled */
    _delay_ms(500);
    usbDeviceConnect();
    sei();
}

static inline void tiny85FlashInit(void) {
    // check for erased first page (no bootloader interrupt vectors), add vectors if missing
    // this needs to happen for usb communication to work later - essential to first run after bootloader
    // being installed
    if(pgm_read_word(RESET_VECTOR_OFFSET * 2) != 0xC000 + (BOOTLOADER_ADDRESS/2) - 1 ||
            pgm_read_word(USBPLUS_VECTOR_OFFSET * 2) != 0xC000 + (BOOTLOADER_ADDRESS/2) - 1) {

        fillFlashWithVectors();
    }

    // TODO: necessary to reset currentAddress?
    currentAddress = 0;
}

static inline void tiny85FlashWrites(void) {
    _delay_us(2000); // TODO: why is this here? - it just adds pointless two level deep loops seems like?
    // write page to flash, interrupts will be disabled for > 4.5ms including erase
    
    if (currentAddress % SPM_PAGESIZE) {
        fillFlashWithVectors();
    } else {
        writeFlashPage();
    }
}

// finishes up writing to the flash, including adding the tinyVector tables at the end of memory
// TODO: can this be simplified? EG: currentAddress = PROGMEM_SIZE; fillFlashWithVectors();
static inline void tiny85FinishWriting(void) {
    // make sure remainder of flash is erased and write checksum and application reset vectors
    if (didWriteSomething) {
        while (currentAddress < BOOTLOADER_ADDRESS) {
            fillFlashWithVectors();
        }
    }
}

// reset system to a normal state and launch user program
static inline __attribute__((noreturn)) void leaveBootloader(void) {
    //DBG1(0x01, 0, 0);
    bootLoaderExit();
    cli();
    USB_INTR_ENABLE = 0;
    USB_INTR_CFG = 0;       /* also reset config bits */

    // clear magic word from bottom of stack before jumping to the app
    *(uint8_t*)(RAMEND) = 0x00;
    *(uint8_t*)(RAMEND-1) = 0x00;

    // jump to application reset vector at end of flash
    asm volatile ("rjmp __vectors - 4");
}

int __attribute__((noreturn)) main(void) {
    uint16_t idlePolls = 0;

    /* initialize  */
    wdt_disable();      /* main app may have enabled watchdog */
    tiny85FlashInit();
    bootLoaderInit();
    
    
    if (bootLoaderCondition()){
        initForUsbConnectivity();
        do {
            usbPoll();
            _delay_us(100);
            idlePolls++;
            
            if (events) idlePolls = 0;
            
            // these next two freeze the chip for ~ 4.5ms, breaking usb protocol
            // and usually both of these will activate in the same loop, so host
            // needs to wait > 9ms before next usb request
            if (isEvent(EVENT_ERASE_APPLICATION)) eraseApplication();
            if (isEvent(EVENT_WRITE_PAGE)) tiny85FlashWrites();
            
            if (isEvent(EVENT_FINISH)) { // || AUTO_EXIT_CONDITION()) {
                tiny85FinishWriting();
                
#               if BOOTLOADER_CAN_EXIT
                    _delay_ms(10); // removing delay causes USB errors
                    break;
#               endif
            }
// #           if BOOTLOADER_CAN_EXIT
//                 // exit if requested by the programming app, or if we timeout waiting for the pc with a valid app
//                 if (isEvent(EVENT_EXIT_BOOTLOADER) || AUTO_EXIT_CONDITION()) {
//                     //_delay_ms(10);
//                     break;
//                 }
// #           endif
            
            clearEvents();
            
        } while(bootLoaderCondition());  /* main event loop */
    }
    
    leaveBootloader();
}

/* ------------------------------------------------------------------------ */
