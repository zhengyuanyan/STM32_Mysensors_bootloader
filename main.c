/* *****************************************************************************
 * The MIT License
 *
 * Copyright (c) 2010 LeafLabs LLC.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 * ****************************************************************************/

/**
 *  @file main.c
 *
 *  @brief main loop and calling any hardware init stuff. timing hacks for EEPROM
 *  writes not to block usb interrupts. logic to handle 2 second timeout then
 *  jump to user code.
 *
 */

#include "common.h"
#include <stdio.h>

#if 0
int main()
{
    SPIflash_CRC = 0;
    flash_CRC = 0;

    systemReset(); // peripherals but not PC
    setupCLK();
    setupLEDAndButton();
    //TODO: setupSPI(PA0);
    setupSPI();

#ifdef USE_USART
    USART_init();
    send_string_USART(myMessages[0]);
#endif // USE_USART

    //    CRC_init();   // Проверка контрольной суммы

// Можно реализовать действие по нажатию кнопки
//    if (readButtonState())
//    {

        uint8_t checkFlash;
        checkFlash = CheckFlashImage();
        if (!checkFlash)
        { // Начинаем процесс обновления

            setupFLASH();  //
            flashUnlock(); // Разблокируем флэш
            uint32_t flashSize = (mybuff[7] << 24) | (mybuff[8] << 16) | (mybuff[9] << 8) | mybuff[10];

            // #ifdef USE_USART             // Выводим размер прошивки
            //         Hex2Ascii(mybuff[5]);
            //         Hex2Ascii(mybuff[6]);
            //         Hex2Ascii(mybuff[7]);
            //         Hex2Ascii(mybuff[8]);
            // #endif // USE_USART

            // Очищаем потребное место
            uint32_t index;
            for (index = USER_CODE_FLASH0X8001000; index < (USER_CODE_FLASH0X8001000 + flashSize); index += 0x0400)
                flashErasePage(index);

            // MYSController пишет с учётом отступа 0x1000 загрузчика, т.е. так как указано в HEX файле,
            // поэтому чтобы не изгаляться с переадресовкой оставляем вначале spi флэша пустое пространство
            // и сдвигаемся на размер загрузчика вперед
            uint32_t flashAddress = USER_CODE_FLASH0X8001000; // адрес флэш памяти МК, куда мы пишем данные
            uint32_t i;

            //TODO: Доделать проверку на размер < 256 ??
            for (i = 0; i < (flashSize - FIRMWARE_START_ADDRESS); i = i + 4)
            {
                fast_read(FIRMWARE_START_ADDRESS + i + FIRMWARE_START_OFFSET, &mybuff[0], 0x04);
                if (!flashWriteWord(flashAddress + i, *(uint32_t *)&mybuff[0]))
                {
                    flashLock();

#ifdef USE_USART
                    send_string_USART(myMessages[9]); // Firmaware flashing error!
#endif // USE_USART
        //TODO: Доделать корректную дальнейшую работу при ошибке записи

                    strobePin(LED_BANK, LED_PIN, checkFlash, BLINK_SLOW, LED_ON_STATE);    
#ifdef USE_USART
        send_string_USART(myMessages[7]); // Reboot
#endif  
                    systemHardReset();
                }
            }
            flashLock();

#ifdef USE_USART
            send_string_USART(myMessages[8]);
#endif                                             // USE_USART

            // Очищаем spi флэш
	        while(busy());
    	    setWriteEnable(true);
            //TODO: Сделать очистку второго блока, если прошивка больше 64к
            erase64kBlock(FIRMWARE_START_ADDRESS);

            strobePin(LED_BANK, LED_PIN, 10, BLINK_FAST, LED_ON_STATE);
        }
        else
        {
            strobePin(LED_BANK, LED_PIN, checkFlash, BLINK_SLOW, LED_ON_STATE);
        }
//    }

    if (checkUserCode(USER_CODE_FLASH0X8001000))
    {

#ifdef USE_USART
        send_string_USART(myMessages[5]); // jump to user prog
#endif // USE_USART

        jumpToUser(USER_CODE_FLASH0X8001000);
    }
    else
    {

#ifdef USE_USART
        send_string_USART(myMessages[7]); // Reboot
#endif                                    // USE_USART
        strobePin(LED_BANK, LED_PIN, 5, BLINK_SLOW, LED_ON_STATE);
        systemHardReset();
    }

    while (1)
        ;

    return 0; // Added to please the compiler
}

#endif

#define FLASH_SIZE      (512 * 1024)       // 外部 Flash 总容量 xxxKB
#define RECOVERY_SIZE   (256 * 1024)       // Recovery 固件分区大小 256KB
#define OTA_BLOCK_SIZE  0x0400             // Flash 页大小
#define OTA_FLASH       0x00000000         // 动态 OTA 分区
#define RECOVERY_FLASH  (FLASH_SIZE - RECOVERY_SIZE)  // Recovery 固定分区地址

#define INTERNAL_FLASH_ADDR  USER_CODE_FLASH0X8001000        // 内部 Flash 用户程序地址

#define FLASH_SECTOR_SIZE   0x1000   // 4KB sector — 常见的 SPI flash 擦除单位
#define ERASE_TIMEOUT_LOOPS 200000 // 超时计数（根据 MCU 速度可调）
#define COPY_PAGE_SIZE 256   // 每次写入 64B
// 等待 SPI flash not busy，带超时。返回 0=OK, 1=timeout
static uint8_t flash_wait_ready(uint32_t timeout_loops)
{
    // 如果你的 SPI 驱动提供 busy()，使用它；否则请替换为读取状态寄存器的函数
    while (timeout_loops--) {
        if (!busy()) { // busy() 应返回 1=忙, 0=空闲
            return 0;
        }
    }
    return 1; // timeout
}

// -------------------- OTA → Recovery 备份 --------------------


uint8_t copyOtaToRecovery(uint32_t otaAddr, uint32_t recoveryAddr, uint32_t flashSize){
    uint8_t tmp[COPY_PAGE_SIZE];
    uint32_t i, lastPct = 0;

#ifdef USE_USART
    send_string_USART("开始擦除 Recovery...\n");
#endif

    for(i = 0; i < RECOVERY_SIZE; i += FLASH_SECTOR_SIZE){
        setWriteEnable(true);
        eraseSector(recoveryAddr + i);

        // 输出擦除进度（以百分比显示）
        // uint32_t erasePct = (i + FLASH_SECTOR_SIZE) * 100U / RECOVERY_SIZE;
        // if(erasePct > 100) erasePct = 100;  // 避免超过100%
        // char buf[6];
        // buf[0] = '0' + erasePct / 100 % 10;
        // buf[1] = '0' + erasePct / 10 % 10;
        // buf[2] = '0' + erasePct % 10;
        // buf[3] = '%';
        // buf[4] = '\n';
        // buf[5] = 0;
        // send_string_USART(buf);

 
         uint32_t w = ERASE_TIMEOUT_LOOPS;
        while(busy() && w--) ;
        if(w == 0) {
#ifdef USE_USART
            send_string_USART("擦除超时!\n");
#endif
            return 0;
        }
        // 输出扇区地址，不用 sprintf
        char buf[12];
        uint32_t addr = recoveryAddr + i;
        uint8_t j;
        for(j=7;j>=0;j--){
            uint8_t nibble = (addr >> (j*4)) & 0xF;
            buf[7-j] = (nibble < 10) ? ('0'+nibble) : ('A'+nibble-10);
        }
        buf[8]='\n'; buf[9]=0;
        send_string_USART(buf);
    }

#ifdef USE_USART
    send_string_USART("开始拷贝 OTA → Recovery...\n");

#endif

    for(i = 0; i < flashSize; i += COPY_PAGE_SIZE){
        uint16_t len = (flashSize - i > COPY_PAGE_SIZE) ? COPY_PAGE_SIZE : (flashSize - i);

        fast_read(otaAddr + i, (char*)tmp, len);  // 直接读取
        
        setWriteEnable(true);

        writePage(recoveryAddr + i, tmp, len);    // 内部已等待写入完成

        uint32_t w = ERASE_TIMEOUT_LOOPS;
        while(busy() && w--) ;
        if(w == 0) {
#ifdef USE_USART
            send_string_USART("写入超时!\n");
#endif
            return 0;
        }
        // while(busy());

        strobePin(LED_BANK, LED_PIN, 1, BLINK_FAST, LED_ON_STATE);

        uint32_t pct = (i + len) * 100U / flashSize;
        if(pct != lastPct){
            char buf[6];
            buf[0] = '0' + pct / 100 % 10;
            buf[1] = '0' + pct / 10 % 10;
            buf[2] = '0' + pct % 10;
            buf[3] = '%';
            buf[4] = '\n';
            buf[5] = 0;
            send_string_USART(buf);
            lastPct = pct;
        }
    }
#ifdef USE_USART
    send_string_USART("Recovery 拷贝完成\n");
#endif
    setWriteEnable(false);
    return 1;
}




// -------------------- 清除 OTA 分区 --------------------

void eraseOtaPartition() {
    uint32_t otaSize = FLASH_SIZE - RECOVERY_SIZE;
    uint32_t start = OTA_FLASH;
    uint32_t end = OTA_FLASH + otaSize;
    uint32_t addr = start;

#ifdef USE_USART
    send_string_USART("开始擦除 OTA 分区...\n");
#endif

    // setupSPI(); // 确保 SPI 已就绪（如果需要）

    for (addr = start; addr < end; addr += FLASH_SECTOR_SIZE) {

        // 发起扇区擦除（你的驱动接口名可能不同，保持原有 flashErasePage 或替换）
        eraseSector(addr); // 若 flashErasePage 不是用于外部 flash 的扇区擦除，请替换为正确的擦除函数

        // 等待擦除完成并带超时保护
        if (flash_wait_ready(ERASE_TIMEOUT_LOOPS)) {
#ifdef USE_USART
            send_string_USART("擦除超时！\n");
#endif
            // 出现超时：尽量退出并不继续擦除，以免破坏 Recovery 区
            break;
        }
    }

    // 可选：上锁 / 清除写使能
    // setWriteEnable(false);

#ifdef USE_USART
    send_string_USART("OTA 分区擦除完成（已保留 Recovery 分区）\n");
#endif
}

// -------------------- 内部 Flash 写入函数 --------------------
uint8_t writeOtaToInternal(uint32_t internalAddr, uint32_t externalAddr, uint32_t flashSize) {
    

    // setupFLASH();
    flashUnlock();

    // 擦除内部 Flash
    uint32_t index;
    for(index = internalAddr; index < internalAddr + flashSize; index += OTA_BLOCK_SIZE)
        flashErasePage(index);

    // 写入外部 Flash 到内部 Flash
    uint32_t i;
    for(i = 0; i < flashSize - FIRMWARE_START_ADDRESS; i += 4) {
        // fast_read(FIRMWARE_START_ADDRESS + i + FIRMWARE_START_OFFSET + externalAddr, &mybuff[0], 4);
        fast_read(FIRMWARE_START_ADDRESS + FIRMWARE_START_OFFSET + externalAddr + i, &mybuff[0], 4);
        if(!flashWriteWord(internalAddr + i, *(uint32_t*)&mybuff[0])) {
            flashLock();
            return 0; // 写入失败
        }
    }

    flashLock();
    return 1; // 写入成功
}

// 尝试加载指定 Flash 区的固件，成功返回 true
bool tryLoadFirmware(uint32_t flashAddr) {
    if (CheckFlashImage(flashAddr) == 0) {
        uint32_t size = (mybuff[7]<<24)|(mybuff[8]<<16)|(mybuff[9]<<8)|mybuff[10];
#ifdef USE_USART
        send_string_USART("发现固件，写入内部 Flash...\n");
#endif
        if (!writeOtaToInternal(INTERNAL_FLASH_ADDR, flashAddr, size)) {
#ifdef USE_USART
            send_string_USART("写入失败，尝试下一步...\n");
#endif
            return false;
        }
#ifdef USE_USART
        send_string_USART("写入完成！\n");
#endif
        return true;
    }
    return false;
}

// 如果是第一次 OTA，需要拷贝 Recovery
void handleFirstOtaCopyToRecovery(void) {
    if (CheckFlashImage(RECOVERY_FLASH) != 0) {
        uint32_t size = (mybuff[7]<<24)|(mybuff[8]<<16)|(mybuff[9]<<8)|mybuff[10];
#ifdef USE_USART
        send_string_USART("第一次 OTA, 拷贝到 Recovery...\n");
#endif
        if (!copyOtaToRecovery(OTA_FLASH, RECOVERY_FLASH, size)) {
#ifdef USE_USART
            send_string_USART("拷贝 Recovery 失败，重启...\n");
#endif
            systemHardReset();
        }
    }
    eraseOtaPartition();
}


// -------------------- 主函数 --------------------
int main(void) {
    systemReset();
    setupCLK();
    setupLEDAndButton();
    setupSPI();
#ifdef USE_USART
    USART_init();
    send_string_USART("Bootloader 启动\n");
#endif

    // 1. 优先检查 OTA 分区
    if (tryLoadFirmware(OTA_FLASH)) {
        handleFirstOtaCopyToRecovery();   // 第一次 OTA 时备份到 Recovery
        jumpToUser(INTERNAL_FLASH_ADDR);
    }

    // 2. 如果没有 OTA，直接检查内部 Flash
    if (checkUserCode(INTERNAL_FLASH_ADDR)) {
#ifdef USE_USART
        send_string_USART("发现有效用户程序，跳转...\n");
#endif
        jumpToUser(INTERNAL_FLASH_ADDR);
    }

    // 3. 内部 Flash 无效，再尝试 Recovery
    if (tryLoadFirmware(RECOVERY_FLASH)) {
#ifdef USE_USART
        send_string_USART("使用 Recovery 固件恢复成功，跳转...\n");
#endif
        jumpToUser(INTERNAL_FLASH_ADDR);
    }

    // 4. 如果三者都失败，系统重启
#ifdef USE_USART
    send_string_USART("无效固件，系统重启...\n");
#endif
    systemHardReset();

    while (1);
}
