NASM := nasm
BUILD := build
ZERO0 := $(BUILD)/zero0
BOOT_SRCS := bootstrap/seed0.asm bootstrap/tokens.inc bootstrap/lexer.inc \
             bootstrap/codegen.inc bootstrap/efi_boot.inc bootstrap/efi_writer.inc \
             bootstrap/types.inc bootstrap/intrinsics.inc bootstrap/views.inc \
             bootstrap/parser.inc

.PHONY: all test qemu clean

all: $(ZERO0)

$(ZERO0): $(BOOT_SRCS)
	@mkdir -p $(BUILD)
	$(NASM) -f bin -I . bootstrap/seed0.asm -o $(ZERO0)
	@chmod +x $(ZERO0)

test: $(ZERO0)
	@bash tests/run_tests.sh $(ZERO0)

qemu: $(ZERO0)
	@mkdir -p $(BUILD)/esp/EFI/BOOT
	./$(ZERO0) --target=efi-os examples/gate1_os.zr $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI
	qemu-system-x86_64 -enable-kvm -cpu host -m 512M \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
		-drive format=raw,file=fat:rw:$(BUILD)/esp \
		-debugcon stdio -global isa-debugcon.iobase=0xe9 \
		-display sdl -no-reboot

clean:
	rm -rf $(BUILD)
