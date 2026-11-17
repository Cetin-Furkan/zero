#!/usr/bin/env python3
"""
ZeroOS Physical Laptop Bootable GPT + FAT32 USB Image Builder (`scripts/build_usb_image.py`)

Builds a deterministic, zero-root, UEFI-2.10 & Microsoft-FAT32 compliant 64 MiB
raw disk image (`build/zeroos_usb.img`) containing `/EFI/BOOT/BOOTX64.EFI`
compiled by the self-hosted `build/zero2` compiler with `qemu_debug :: 0`
(verified 0 port `0xE9` instructions and strict W^X PE32+ sections).
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import uuid
import zlib

SECTOR_SIZE = 512
IMAGE_BYTES = 64 * 1024 * 1024          # 64 MiB
TOTAL_LBAS  = IMAGE_BYTES // SECTOR_SIZE # 131,072 sectors

ESP_START_LBA = 2048
ESP_END_LBA   = 131007                  # 128,960 sectors (~62.97 MiB)
ESP_SECTORS   = ESP_END_LBA - ESP_START_LBA + 1

ESP_TYPE_GUID = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b").bytes_le
DISK_GUID     = uuid.UUID("5a45524f-4f53-4750-9433-000000000001").bytes_le
PART_GUID     = uuid.UUID("5a45524f-4f53-4553-9032-000000000002").bytes_le


def compile_laptop_efi(compiler: str, src_path: str, build_dir: str) -> bytes:
    if not os.path.exists(compiler):
        raise RuntimeError(f"Self-hosted compiler missing at {compiler}; run `make selfhost` first.")

    os.makedirs(build_dir, exist_ok=True)
    os_link = os.path.join(build_dir, "os")
    target_os_dir = os.path.relpath(os.path.join(os.path.dirname(src_path), "os"), build_dir)
    if os.path.lexists(os_link):
        os.unlink(os_link)
    os.symlink(target_os_dir, os_link)

    src_text = open(src_path, "r", encoding="utf-8").read()
    laptop_src, n_subs = re.subn(
        r"^qemu_debug\s*::\s*1\b",
        "qemu_debug :: 0",
        src_text,
        count=1,
        flags=re.MULTILINE,
    )
    if n_subs != 1:
        raise RuntimeError(f"Could not toggle `qemu_debug :: 0` in {src_path}")

    laptop_zr  = os.path.join(build_dir, "usb_gate1_laptop.zr")
    laptop_efi = os.path.join(build_dir, "usb_BOOTX64.EFI")
    with open(laptop_zr, "w", encoding="utf-8") as f:
        f.write(laptop_src)

    subprocess.run([compiler, "--target=efi-os", laptop_zr, laptop_efi], check=True)

    # Verify PE32+ EFI Subsystem (0000000a), strict W^X sections, and 0 port 0xE9 instructions
    hdr_out = subprocess.check_output(["objdump", "-p", laptop_efi], text=True)
    if not re.search(r"Subsystem\s+0000000a\s+\(EFI application\)", hdr_out):
        raise RuntimeError(f"{laptop_efi} is missing UEFI application subsystem 0000000a")

    sec_out = subprocess.check_output(["objdump", "-x", laptop_efi], text=True)
    for m in re.finditer(r"^\s*\d+\s+\.\S+.*\n\s+(.*)$", sec_out, flags=re.MULTILINE):
        flags_line = m.group(1)
        if "EXECUTE" in flags_line and "WRITE" in flags_line:
            raise RuntimeError(f"{laptop_efi} violates W^X section policy: {flags_line}")

    dis_out = subprocess.check_output(["objdump", "-d", laptop_efi], text=True)
    if re.search(r"\b(in|out)\b.*0xe9", dis_out):
        raise RuntimeError(f"{laptop_efi} still contains port 0xE9 debug instructions!")

    with open(laptop_efi, "rb") as f:
        return f.read()


def make_dir_entry(name83: bytes, attr: int, cluster: int, size: int) -> bytes:
    assert len(name83) == 11
    # Fixed deterministic timestamp: 2026-11-17 12:00:00
    dos_time = (12 << 11) | (0 << 5) | 0
    dos_date = ((2026 - 1980) << 9) | (11 << 5) | 17
    clus_hi = (cluster >> 16) & 0xFFFF
    clus_lo = cluster & 0xFFFF
    return struct.pack(
        "<11sBBBHHHHHHHI",
        name83,
        attr,
        0,
        0,
        dos_time,
        dos_date,
        dos_date,
        clus_hi,
        dos_time,
        dos_date,
        clus_lo,
        size,
    )


def format_fat32_esp(efi_bytes: bytes) -> bytearray:
    """
    Formats a 128,960-sector FAT32 EFI System Partition (`SecPerClus = 1` ->
    126,912 data clusters >= 65,525 Microsoft FAT32 threshold).
    """
    vol = bytearray(ESP_SECTORS * SECTOR_SIZE)
    rsvd_sec = 32
    num_fats = 2
    sec_per_clus = 1
    fat_sz32 = 1008  # 1008 * 512 / 4 = 129,024 FAT entries

    data_sec = ESP_SECTORS - rsvd_sec - (num_fats * fat_sz32)
    cluster_count = data_sec // sec_per_clus
    assert cluster_count >= 65525, f"FAT32 requires >= 65525 clusters, got {cluster_count}"

    # 1. Boot Sector (VBR) at sector 0 + Backup at sector 6
    vbr = bytearray(SECTOR_SIZE)
    vbr[0:3] = b"\xEB\x58\x90"
    vbr[3:11] = b"ZEROOS  "
    struct.pack_into(
        "<HBHBHHBHHHII",
        vbr,
        11,
        SECTOR_SIZE,   # BytsPerSec
        sec_per_clus,  # SecPerClus
        rsvd_sec,      # RsvdSecCnt
        num_fats,      # NumFATs
        0,             # RootEntCnt (0 for FAT32)
        0,             # TotSec16   (0 for FAT32)
        0xF8,          # Media
        0,             # FATSz16    (0 for FAT32)
        63,            # SecPerTrk
        255,           # NumHeads
        ESP_START_LBA, # HiddSec
        ESP_SECTORS,   # TotSec32
    )
    struct.pack_into(
        "<IHHIHH12sBBBI11s8s",
        vbr,
        36,
        fat_sz32,      # FATSz32
        0,             # ExtFlags
        0,             # FSVer
        2,             # RootClus
        1,             # FSInfo
        6,             # BkBootSec
        b"\x00" * 12,  # Reserved
        0x80,          # DrvNum
        0,             # Reserved1
        0x29,          # BootSig
        0x5A45524F,    # VolID
        b"ZEROOS_ESP ",# VolLab
        b"FAT32   ",   # FilSysType
    )
    vbr[510:512] = b"\x55\xAA"

    # Cluster layout:
    #   Cluster 2: `/`
    #   Cluster 3: `/EFI`
    #   Cluster 4: `/EFI/BOOT`
    #   Cluster 5..5+N-1: `/EFI/BOOT/BOOTX64.EFI`
    efi_clusters = max(1, (len(efi_bytes) + SECTOR_SIZE - 1) // SECTOR_SIZE)
    first_efi_clus = 5
    used_clusters = 3 + efi_clusters  # clusters 2, 3, 4, and BOOTX64.EFI

    # 2. FSInfo at sector 1 + Backup at sector 7
    fsinfo = bytearray(SECTOR_SIZE)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    struct.pack_into("<I", fsinfo, 488, cluster_count - used_clusters)
    struct.pack_into("<I", fsinfo, 492, 2 + used_clusters)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)

    vol[0 * SECTOR_SIZE : 1 * SECTOR_SIZE] = vbr
    vol[1 * SECTOR_SIZE : 2 * SECTOR_SIZE] = fsinfo
    vol[6 * SECTOR_SIZE : 7 * SECTOR_SIZE] = vbr
    vol[7 * SECTOR_SIZE : 8 * SECTOR_SIZE] = fsinfo

    # 3. FAT1 & FAT2
    fat = bytearray(fat_sz32 * SECTOR_SIZE)
    eoc = 0x0FFFFFFF
    struct.pack_into("<I", fat, 0 * 4, 0x0FFFFFF8)
    struct.pack_into("<I", fat, 1 * 4, eoc)
    struct.pack_into("<I", fat, 2 * 4, eoc)  # `/`
    struct.pack_into("<I", fat, 3 * 4, eoc)  # `/EFI`
    struct.pack_into("<I", fat, 4 * 4, eoc)  # `/EFI/BOOT`

    for i in range(efi_clusters):
        c = first_efi_clus + i
        nxt = (c + 1) if (i + 1 < efi_clusters) else eoc
        struct.pack_into("<I", fat, c * 4, nxt)

    fat1_off = rsvd_sec * SECTOR_SIZE
    fat2_off = (rsvd_sec + fat_sz32) * SECTOR_SIZE
    vol[fat1_off : fat1_off + len(fat)] = fat
    vol[fat2_off : fat2_off + len(fat)] = fat

    # 4. Data Region (starts at cluster 2)
    data_off = (rsvd_sec + num_fats * fat_sz32) * SECTOR_SIZE

    def clus_off(c: int) -> int:
        return data_off + (c - 2) * SECTOR_SIZE

    # Cluster 2: Root directory `/`
    root_dir = (
        make_dir_entry(b"ZEROOS_ESP ", 0x08, 0, 0)
        + make_dir_entry(b"EFI        ", 0x10, 3, 0)
    )
    vol[clus_off(2) : clus_off(2) + len(root_dir)] = root_dir

    # Cluster 3: `/EFI` (`..` pointing to root has cluster 0 per FAT32 spec)
    efi_dir = (
        make_dir_entry(b".          ", 0x10, 3, 0)
        + make_dir_entry(b"..         ", 0x10, 0, 0)
        + make_dir_entry(b"BOOT       ", 0x10, 4, 0)
    )
    vol[clus_off(3) : clus_off(3) + len(efi_dir)] = efi_dir

    # Cluster 4: `/EFI/BOOT`
    boot_dir = (
        make_dir_entry(b".          ", 0x10, 4, 0)
        + make_dir_entry(b"..         ", 0x10, 3, 0)
        + make_dir_entry(b"BOOTX64 EFI", 0x20, first_efi_clus, len(efi_bytes))
    )
    vol[clus_off(4) : clus_off(4) + len(boot_dir)] = boot_dir

    # Clusters 5..: `BOOTX64.EFI` payload
    vol[clus_off(first_efi_clus) : clus_off(first_efi_clus) + len(efi_bytes)] = efi_bytes
    return vol


def make_gpt_header(my_lba: int, alt_lba: int, part_entry_lba: int, part_array_crc: int) -> bytes:
    hdr = bytearray(SECTOR_SIZE)
    first_usable = 34
    last_usable  = TOTAL_LBAS - 34
    struct.pack_into(
        "<8sIIIIQQQQ16sQIII",
        hdr,
        0,
        b"EFI PART",
        0x00010000,
        92,
        0,  # HeaderCRC32 filled below
        0,
        my_lba,
        alt_lba,
        first_usable,
        last_usable,
        DISK_GUID,
        part_entry_lba,
        128,
        128,
        part_array_crc,
    )
    crc = zlib.crc32(hdr[:92]) & 0xFFFFFFFF
    struct.pack_into("<I", hdr, 16, crc)
    return bytes(hdr)


def build_gpt_usb_image(efi_bytes: bytes, out_path: str) -> None:
    img = bytearray(IMAGE_BYTES)

    # 1. Protective MBR at LBA 0 (type 0xEE spanning LBA 1..TOTAL_LBAS-1)
    mbr = bytearray(SECTOR_SIZE)
    struct.pack_into(
        "<BBBBBBBBII",
        mbr,
        446,
        0x00,
        0x00, 0x02, 0x00,
        0xEE,
        0xFF, 0xFF, 0xFF,
        1,
        TOTAL_LBAS - 1,
    )
    mbr[510:512] = b"\x55\xAA"
    img[0:SECTOR_SIZE] = mbr

    # 2. GPT Partition Entry Array (128 entries * 128 bytes = 32 sectors)
    part_array = bytearray(128 * 128)
    name_utf16 = "ZeroOS EFI System".encode("utf-16le").ljust(72, b"\x00")
    struct.pack_into(
        "<16s16sQQQ72s",
        part_array,
        0,
        ESP_TYPE_GUID,
        PART_GUID,
        ESP_START_LBA,
        ESP_END_LBA,
        0,
        name_utf16,
    )
    part_crc = zlib.crc32(part_array) & 0xFFFFFFFF

    # Primary GPT Header (LBA 1) + Entries (LBA 2..33)
    img[1 * SECTOR_SIZE : 2 * SECTOR_SIZE] = make_gpt_header(1, TOTAL_LBAS - 1, 2, part_crc)
    img[2 * SECTOR_SIZE : 34 * SECTOR_SIZE] = part_array

    # Backup GPT Entries (TOTAL_LBAS - 33 .. TOTAL_LBAS - 2) + Header (TOTAL_LBAS - 1)
    bkp_entries_lba = TOTAL_LBAS - 33
    img[bkp_entries_lba * SECTOR_SIZE : (TOTAL_LBAS - 1) * SECTOR_SIZE] = part_array
    img[(TOTAL_LBAS - 1) * SECTOR_SIZE : TOTAL_LBAS * SECTOR_SIZE] = make_gpt_header(
        TOTAL_LBAS - 1, 1, bkp_entries_lba, part_crc
    )

    # 3. FAT32 EFI System Partition at LBA 2048..131007
    esp_vol = format_fat32_esp(efi_bytes)
    img[ESP_START_LBA * SECTOR_SIZE : (ESP_END_LBA + 1) * SECTOR_SIZE] = esp_vol

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(img)


def main() -> int:
    ap = argparse.ArgumentParser(description="Build bootable 64 MiB GPT+FAT32 ZeroOS USB image.")
    ap.add_argument("--compiler", default="build/zero2", help="Path to self-hosted zero2 compiler")
    ap.add_argument("--src", default="examples/gate1_os.zr", help="Path to ZeroOS root .zr source")
    ap.add_argument("--build-dir", default="build", help="Scratch build directory")
    ap.add_argument("--out", default="build/zeroos_usb.img", help="Output 64 MiB GPT+FAT32 image")
    args = ap.parse_args()

    efi_bytes = compile_laptop_efi(args.compiler, args.src, args.build_dir)
    build_gpt_usb_image(efi_bytes, args.out)
    print(
        f"[usb-img] built {args.out} ({IMAGE_BYTES} bytes, GPT + FAT32 ESP, "
        f"/EFI/BOOT/BOOTX64.EFI = {len(efi_bytes)} bytes, qemu_debug=0, 0xE9=0)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
