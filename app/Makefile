# =========================================================
# Application — production des deux binaires
#
# Un binaire compile est lie a une adresse fixe. L'architecture
# dual-slot impose donc de le compiler deux fois, une par
# emplacement : app_slotA.bin ne fonctionne qu'a 0x08008000,
# app_slotB.bin qu'a 0x08080000.
#
# L'outil PC interroge la carte pour connaitre le slot libre,
# puis envoie le binaire correspondant. Un seul fichier traverse
# la liaison serie ; les deux existent uniquement sur le disque.
# =========================================================

CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

SRCS    = main.c startup.s src/crc.c src/flash.c src/metadata_mgr.c

CPU     = -mcpu=cortex-m4 -mthumb
FPU     = -mfpu=fpv4-sp-d16 -mfloat-abi=hard

CFLAGS  = $(CPU) $(FPU)
CFLAGS += -Wall -Wextra
CFLAGS += -O0 -g3
CFLAGS += -ffreestanding
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -I../shared

LDBASE  = $(CPU) $(FPU)
LDBASE += -nostdlib
LDBASE += -Wl,--gc-sections

# ---------------------------------------------------------
all: app_slotA.bin app_slotB.bin
	@echo ""
	@echo "  Deux binaires produits :"
	@ls -l app_slotA.bin app_slotB.bin | awk '{printf "    %-16s %6s octets\n", $$9, $$5}'
	@echo ""

# --- Slot A ---
app_slotA.elf: $(SRCS) linker_slotA.ld
	$(CC) $(CFLAGS) $(LDBASE) -T linker_slotA.ld \
	      -Wl,-Map=app_slotA.map $(SRCS) -o $@
	@$(SIZE) $@

app_slotA.bin: app_slotA.elf
	$(OBJCOPY) -O binary $< $@

# --- Slot B ---
app_slotB.elf: $(SRCS) linker_slotB.ld
	$(CC) $(CFLAGS) $(LDBASE) -T linker_slotB.ld \
	      -Wl,-Map=app_slotB.map $(SRCS) -o $@
	@$(SIZE) $@

app_slotB.bin: app_slotB.elf
	$(OBJCOPY) -O binary $< $@

# ---------------------------------------------------------
# Verification : les deux binaires doivent etre lies a des
# adresses differentes. Une erreur ici passerait autrement
# inapercue jusqu'au plantage silencieux de l'application.
verify: app_slotA.elf app_slotB.elf
	@echo ""
	@echo "  Adresses de liaison :"
	@arm-none-eabi-objdump -h app_slotA.elf | \
	   awk '/\.isr_vector/{printf "    slot A : 0x%s  (attendu 08008000)\n", $$4}'
	@arm-none-eabi-objdump -h app_slotB.elf | \
	   awk '/\.isr_vector/{printf "    slot B : 0x%s  (attendu 08080000)\n", $$4}'
	@echo ""

# ---------------------------------------------------------
# Flash direct par SWD, utile pour amorcer un slot sans passer
# par le bootloader.
flashA: app_slotA.elf
	openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
	        -c "program app_slotA.elf verify reset exit"

flashB: app_slotB.elf
	openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
	        -c "program app_slotB.elf verify reset exit"

clean:
	rm -f app_slotA.elf app_slotA.bin app_slotA.map
	rm -f app_slotB.elf app_slotB.bin app_slotB.map

.PHONY: all verify flashA flashB clean
