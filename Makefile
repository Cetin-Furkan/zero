NASM := nasm
BUILD := build
ZERO0 := $(BUILD)/zero0
ZERO1 := $(BUILD)/zero1
ZERO2 := $(BUILD)/zero2
BOOT_SRCS := bootstrap/seed0.asm bootstrap/tokens.inc bootstrap/lexer.inc \
             bootstrap/imports.inc bootstrap/codegen.inc bootstrap/efi_boot.inc \
             bootstrap/ap_trampoline.inc bootstrap/efi_writer.inc bootstrap/capsule_writer.inc \
             bootstrap/types.inc bootstrap/intrinsics.inc bootstrap/views.inc bootstrap/pointers.inc \
             bootstrap/reactor.inc bootstrap/loops.inc bootstrap/parser.inc
SELF_SRCS := compiler/zero1.zr compiler/defs.zr compiler/lexer.zr \
             compiler/emit_x64.zr compiler/evex_zmm.zr compiler/containers.zr \
             compiler/efi_pe32.zr compiler/types_spaces.zr compiler/intrinsics_ptr.zr \
             compiler/parser_expr.zr compiler/loops_match.zr

.PHONY: all selfhost test usb-img qemu clean

all: $(ZERO0)

$(ZERO0): $(BOOT_SRCS)
	@mkdir -p $(BUILD)
	$(NASM) -f bin -I . bootstrap/seed0.asm -o $(ZERO0)
	@chmod +x $(ZERO0)

selfhost: $(ZERO2)

$(ZERO2): $(ZERO0) $(SELF_SRCS)
	./$(ZERO0) compiler/zero1.zr $(ZERO1)
	./$(ZERO1) compiler/zero1.zr $(ZERO2)

test: $(ZERO0)
	@bash tests/run_tests.sh $(ZERO0)

usb-img: $(ZERO2)
	@python3 scripts/build_usb_image.py --compiler $(ZERO2) --src examples/gate1_os.zr --build-dir $(BUILD) --out $(BUILD)/zeroos_usb.img

qemu: $(ZERO2)
	@mkdir -p $(BUILD)/esp/EFI/BOOT
	./$(ZERO2) --target=efi-os examples/gate1_os.zr $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI
	./$(ZERO2) --target=capsule examples/capsule_compute.zr $(BUILD)/capsule_z2.zcap
	./$(ZERO2) --target=capsule examples/capsules/cap_avx512_wave.zr $(BUILD)/cap_avx512_wave.zcap
	./$(ZERO2) --target=capsule examples/capsules/cap_matrix_crc.zr $(BUILD)/cap_matrix_crc.zcap
	@python3 -c 'import struct; caps=[("$(BUILD)/capsule_z2.zcap",0x5A32434150,4096),("$(BUILD)/cap_avx512_wave.zcap",0x5A32574156,8192),("$(BUILD)/cap_matrix_crc.zcap",0x5A324D5458,12288)]; d=bytearray(16*1024*1024); struct.pack_into("<QIIQ",d,0,0x31544E455458455A,1,len(caps),1); [ (c:=open(p,"rb").read(), crc:=struct.unpack_from("<I",c,20)[0], struct.pack_into("<QQQII",d,64+i*32,h,off,len(c),crc,1), d.__setitem__(slice(off,off+len(c)),c)) for i,(p,h,off) in enumerate(caps) ]; open("$(BUILD)/nvme_disk.img","wb").write(d)'
	qemu-system-x86_64 -machine q35 -enable-kvm -cpu host -overcommit cpu-pm=on -smp 4 -m 512M \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
		-drive format=raw,file=fat:rw:$(BUILD)/esp \
		-drive file=$(BUILD)/nvme_disk.img,if=none,id=nvm0,format=raw \
		-device nvme,serial=ZEROZCAP,drive=nvm0 \
		-debugcon stdio -global isa-debugcon.iobase=0xe9 \
		-display sdl -no-reboot

clean:
	rm -rf $(BUILD)
