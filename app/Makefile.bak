# ---------- Outils ----------
CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

# ---------- Nom du projet ----------
TARGET  = app

# ---------- Fichiers sources ----------
SRCS    = main.c startup.s

# ---------- Options de compilation ----------
CPU     = -mcpu=cortex-m4 -mthumb
FPU     = -mfpu=fpv4-sp-d16 -mfloat-abi=hard

CFLAGS  = $(CPU) $(FPU)
CFLAGS += -Wall -Wextra
CFLAGS += -O0 -g3
CFLAGS += -ffreestanding
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -I../shared

# ---------- Options d'édition de liens ----------
LDFLAGS  = $(CPU) $(FPU)
LDFLAGS += -T linker.ld
LDFLAGS += -nostdlib
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-Map=$(TARGET).map

# ---------- Règles ----------
all: $(TARGET).bin

$(TARGET).elf: $(SRCS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

flash: $(TARGET).elf
	openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
	        -c "program $(TARGET).elf verify reset exit"

clean:
	rm -f $(TARGET).elf $(TARGET).bin $(TARGET).map

.PHONY: all flash clean