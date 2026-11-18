# Model 2 state map: SLPM-62767 (Virtual-On, Sega Ages 2500 Vol.31)

This page records where the PS2 port keeps each piece of emulated Sega Model 2 state in EE RAM, which functions fill it (and from which BIN.PAK files), and which functions turn it into GS/VU1 output.

Rules for this page:
- Only entries that a checker confirmed are in the main sections. Where a checker corrected a detail, the corrected value is shown and marked "(corrected)".
- Refuted and unverifiable entries are listed in a separate section, with the checker's note.
- "Not verified" means nobody measured it.

Sources: four mapping areas (loading, tilemap, polygons, texture) and one independent checker per area. Dumps used: `moon/build/runs/*/ram_<tick>.bin` and `gsvram_<tick>.bin` from runs m2load1, m2load2, m2tileA, m2polys1, m2tex_a, m2tex_w1, m2tex_w2, tr4000, tr343, draws, dbram. Ticks are 1500, 2500, 3000, 3300, 4002 and 5000. Scratch tools are under `J:/VOOperationMoongatePS2/scratch/m2map/`.

Model 2 hardware reference (used only to recognise structures): MAME `src/mame/sega/model2.cpp`, `model2_v.cpp`, `segaic24.cpp`.

## 0. Global pointer

- **gp = 0x0056B5F0.** crt0 sets it: `0x1002c4 lui a0,0x57` / `0x1002d8 addiu a0,a0,-0x4a10` / `0x1002ec move gp,a0`. The `.reginfo` gp word gives the same value.
- It is cross-checked by the init_rom slots (gp-0x4444 = 0x5671AC) and by the char-RAM base (gp-0x44E4 = 0x56710C, which holds 0x63A210).

---

## 1. Loading (BIN.PAK file -> EE RAM)

### 1.1 PAK and file API

| Item | Address / function | Details |
|---|---|---|
| PAK registration | FUN_0014d870 | Calls FUN_00202fd0 first (0x14d878), then FUN_00202da0('bin' from 0x564CE8) (0x14d884). (corrected order) |
| Pack registration | FUN_00202fd0 -> FUN_00202e18('BIN.PAK' @0x567588, max 0x1F4) | Allocates 16 sectors (0x8000 B) via 0x202488 and reads the PAK head with ADXF_ReadNw(h,16,buf). Errors with 'bl_file_pack_entry (%s) file_num %d > %d' (0x54CBA8) if max < the BE count at +8. |
| Pack table | 0x7AE060, stride 0x104 (`char name[0x100]; u32 dirPtr` at +0x100); count at 0x568500 (gp-0x30F0) | Dumps: name 'BIN.PAK', dirPtr 0x844C00, count 1 |
| Current dir string | 0x7AEA88 ('bin') | Written by FUN_00202da0 |
| PAK directory copy | 0x844C00..0x84CC00 | Equals BIN.PAK[0..0x8000) at ticks 3300 and 5000. Header 'PAKFILE', 0x1DA = 474 entries. |
| Name lookup | FUN_00202798 (0x202798-0x202AF4) | If the dir is '\\' or '', the plain name is used. Otherwise it builds '%s.PAK', strips '\\' and uppercases (FUN_00202740). The requested name is lowercased first (FUN_002026e8 at 0x202900). It scans dir+0x10+i*64 and returns BE sector (dir+0x48+i*64) and BE size (dir+0x4C+i*64). |
| File API | open FUN_00202af8, close FUN_00202bd8, seek FUN_00202c18, read FUN_00202c60, poll FUN_00202d00, size FUN_00202dd8, size rounded to 2048 FUN_00202de0, tell FUN_00202e00 | The handle is 0x10C bytes (FUN_002024e0): +0 ADXF handle, +4 byte pos = sector<<11, +8 size, +0xC path. A read is ADXF_Seek(pos>>11) then ADXF_ReadNw(ceil(size/2048)). |
| CRI ADXF entry points | ReadNw 0x251220, Seek 0x251630, GetFsizeSct 0x251828, GetStat 0x251B60 (identified by name strings); Open 0x2504F0, Close 0x250C50 (identified by call pattern only) | The meaning of stat values 3 and 4 is not verified from strings. |
| Disc path | sceCdSearchFile at pc 0x26232c: '\\BIN.PAK;1' lsn 0x10013c size 0xEB00000 | sceCdRead (0x284770) is called from 0x225788, 0x260de0 and 0x2633b0. No game-specific file RPC was seen. Whether sceCdRead itself runs was not traced. |

Other callers of FUN_00202af8 (not Model 2 ROMs; what they load is not checked): 0x1036d4 FUN_00103640, 0x18dbb0 FUN_0018db88, 0x1f5e48 FUN_001f5db8, 0x20b784, 0x20ba7c, 0x20bdf8, 0x20c468, 0x20c97c.

### 1.2 ROM images resident in RAM

| File | EE range | Pointer slot | Loader | Notes |
|---|---|---|---|---|
| bank2.bin | 0x1ED3500..0x1F440EC (0x70BEC) | 0x5671AC (gp-0x4444) g_rom_bank2 | FUN_001fa748 (init_rom, called at 0x14d778) | Byte-identical to the file |
| tblrom.bin | 0x1EB4480..0x1ED32CC (0x1EE4C) | 0x5671B4 g_rom_tbl | FUN_001fa748 | Byte-identical. Translator FUN_001fda30/FUN_001fda48: EE = arcade - 0x02BE0000 + base. |
| datarom.bin | 0x1D5D400..0x1EB4130 (0x156D30) | 0x5671B8 g_rom_data | FUN_001fa748 | Identical except a relocated pointer block of 384 dwords (every 8 bytes) at file 0x618..0x1214 (0xBFC bytes), EE 0x1D5DA18..0x1D5E614 (corrected). Each word = file - 0x02EA0BB8 + 0x1D5D400, the same formula as FUN_001fda60. The relocating code is not identified. |
| (none) | — | 0x5671B0 g_rom_mot, 0x5671BC g_rom_cop | — | Both 0 in all dumps |
| coprom0..a.bin | 0x372CC0..0x3B2CC0 (0x40000, one bank at a time) | fixed buffer | FUN_001e6e90(n) -> FUN_001fdaa8 ('coprom%01x.bin' @0x54C378, from offset 0) | n = *(0x31F1B8 + idx*0x18 + 0x14), callers FUN_001b0660/FUN_001b06b0. Entry +0/+4/+0xC are copied to gp-0x3218/-0x3214/-0x321C. Resident bank seen: coprom2 (3300, m2load), coprom3 (5000, m2load), coprom7 (4002), coprom0 (5000, m2polys1). coprom.bin = concat(coprom0..a). |

init_rom (FUN_001fa748): the table at 0x54BC40 has 3 entries {name, slot}. For each one it opens the file, gets the size, rounds it, allocates with FUN_001fe8b0 (heap 2, tag 0x20504D54), stores the pointer at 0x1fa7d8, then reads, polls and closes. Afterwards it prints 'g_rom_bank2/mot/tbl/data/cop = %x'.

coprom.bin, motrom, pdtrom, tdt0rom and tdt1rom are not resident as whole files. No ELF string names them. Only partial slices are found in RAM, and those coincide with per-robot files. pdtrom[0x706B00..0x74DB60) == pd50.bin[0xE6E40..] is confirmed.

### 1.3 Per-robot model files

| Kind | Loader (format string) | Slot array (13 words, ids at +0x34) |
|---|---|---|
| pm | FUN_001fb220 ('pm%02x.bin'), heap 1 FUN_001fe938 tag 0x204C444D | 0x5671CC.. |
| th / th_hi | FUN_001fb768 | 0x567234.. |
| tp | FUN_001fbc20 | 0x56729C.. |
| pd | FUN_001fc0b0 | 0x567304.. |
| mot | FUN_001fc4b8 | 0x56736C / 0x567370 |
| mdl | FUN_001fcb80 | 0x5673D0.. |

- **Model 0x50 (stage), resident in all dumps, byte-identical to the files:**
  - th_hi50 at 0x1D4BB80 (slot 0x567248)
  - tp50 at 0x1C82300 (0x5672B0)
  - pd50 at 0x19D5A80 (0x567318)
  - mdl50 at 0x19C8200 (0x5673E4)
- **mot:**
  - Tick 5000: mot32 at 0x168C900 (0x567370), mot34 at 0x1838180 (0x56736C). The same pointers appear at 0x4EA07C/0x4EA08C and 0x4EA0FC/0x4EA10C.
  - Tick 3300: mot37 at 0x1741100, mot36 at 0x1867180.
- **pm unpack (FUN_001fb4c0):**
  - pm header: 16 x {u8 id, u8 kind (1 th_hi, 2 tp, 3 pd, 4 mdl), u16 0, u32 off, u32 stored, u32 unpacked}.
  - Each component is lzs2-decoded (FUN_00200d50) or memcpy'd (0x29BE2C) into heap-2 buffers in the th/tp/pd/mdl slots.
  - The slots are cleared afterwards (0x1fb4a0, 0x1fbaa0, 0x1fbf30, 0x1fc3c0, 0x1fced0).
  - All 160 components of the 30 pm files equal the standalone files.
  - At ticks 3300/5000 every robot pm/th/tp/pd/mdl slot is 0.
- **Arcade-address translators (rom_*_conv):**
  - pd FUN_001fd448 (jump table 0x54C2CC), tp FUN_001fd1c8, th FUN_001fcf48, mot FUN_001fd6d0.
  - Each scans a per-type list (table 0x4889A8) of {key, off} pairs ending in -1 and returns slotbase + off - 0x10.
  - The slot is chosen per type: type 5 -> slot 5, PD 0x567318, no 0x56734C. The keys passed in are PA<<2, TP<<1 and TH<<1.

### 1.4 lzs2 decompressor

FUN_00200d50(dest, src):
- The output size is LE at src+4. The magic is not checked by the code.
- Flags are read MSB first.
- Flag 1: a BE u16 v, then copy (v>>12)+3 bytes from ring[v&0xFFF].
- Flag 0: a literal byte = src ^ (ringpos & 0xFF).
- Ring buffer 0x77B060 (4 KB). Output staging 0x77C060..0x7AE060, flushed with memcpy 0x29BE2C.

### 1.5 Texture files (LoadHiTextureMap)

- **Entry point:** FUN_001fea88(mode, no) -> FUN_001feb40 plus the state machine FUN_001fec18/FUN_001feca0/FUN_001fecd0/FUN_001fed20/FUN_001fed78.
- **Name table 0x490CE8[mode*16+no]:**
  - mode 0: maptexture0-9 and maptextured (no 13)
  - mode 1: statictexture
  - mode 2: extratexture0-4
- **Bank table 0x490DA8 = {1,0,1}:** modes 0 and 2 use banks 1 and 3; mode 1 uses banks 0 and 2.
- **Buffers:**
  - Handle at 0x5674C8.
  - Compressed buffer pointer at 0x5674CC, allocated with the fixed size 0x40000 from gp-0x30F8 (corrected). The read size is round2048(file size).
  - Decompressed buffer pointer at 0x5674D0 (0x80000 bytes).
- **Flow:**
  1. The decoded first half is memcpy'd to staging 0x6D2300 (FUN_001f9aa8 always returns 0x6D2300).
  2. FUN_001f9ab8(bank) stores the bank at 0x712744 and the byte 1 at 0x712748. It loops forever on bank >= 4.
  3. The second half follows for the other bank.
- **Consumer FUN_001f87c8 (per frame):** if byte 0x712748 is set and bank < 4, it nibble-swaps 0x6D2300 in place (FUN_001f9b40) and calls FUN_002091b0(bank, 0x6D2300). See section 4.4.
- **Measured** (all 17 files decode to 0x80000 bytes):

  | Tick | Bank 3 / staging 0x6D2300 | Bank 2 |
  |---|---|---|
  | 5000 (m2load) | nswap(maptexture1 second half) | nswap(statictexture second half) |
  | 3300 | maptexture5 second half | statictexture second half |
  | 5000 (m2tex_a) | nswap(maptexture3 second half) | nswap(statictexture second half) |

- **texrom0-5** (table 0x490CD0, FUN_001fe9c0) can never be loaded:
  - The only caller of FUN_001fe9c0 is FUN_001f81a8, which has no callers and no pointer to it in the ELF.
  - FUN_001f81a8 would then call FUN_001f9ab8(-1), which prints 'GeoTexUpdate bank err' and spins.
  - texrom data is not resident in RAM.

---

## 2. Tilemap (System 24 tile generator as used by Model 2)

### 2.1 Tile-chip block at 0x62FA00

FUN_001f7bc8 passes a1 = 0x62FA00 (0x1f7c78 lui 0x63 / 0x1f7c84 addiu -0x600).

| Offset | EE address | Content |
|---|---|---|
| +0x0000 | 0x62FA00 | Layer 0 names, 64x64 u16 |
| +0x2000 | 0x631A00 | Layer 1 names |
| +0x4000 | 0x633A00 | Layer 2 names |
| +0x6000 | 0x635A00 | Layer 3 names |
| +0x8000 / +0x8400 / +0x8800 / +0x8C00 | 0x637A00 / 0x637E00 / 0x638200 / 0x638600 | Line-scroll tables, 0x200 u16 each |
| +0x9000..+0x9006 | 0x638A00..06 | hscr for layers 0..3 (MAME hscr; bit 0x8000 = line-scroll mode) |
| +0x9008..+0x900E | 0x638A08..0E | vscr for layers 0..3 (MAME vscr; 0x8000 = layer off). The low bits are the vertical scroll used by FUN_00205b60. MAME's register block is at +0xA000; the game's order (4 hscr, then 4 vscr) matches MAME. |
| +0x9010 | 0x638A10 / 0x639610 | Per-line byte tables, 0xC00 each (see 2.4) |
| +0xA810 | 0x63A210..0x6BA210 | Char RAM, 512 KB, pre-swizzled PSMT4 sheet |
| +0x8A810 | 0x6BA210..0x6BC210 | Palette RAM, 2 x 0x800 RGB555 |
| +0x8C810 | 0x6BC210 | 96-byte 5->8-bit LUT (3 x 32) |

Pointer table 0x5670A0..0x56710C (same in all 7 dumps):
- A0/A4 = 0x6BA210
- A8 = 0x6C8300 (g_geo)
- AC/B0/B4 = 0x6BC270 / 0x6C0270 / 0x6C4270 (colour tables)
- B8/BC = 0x62FA00, C0 = 0x633A00, C4 = 0x631A00, C8 = 0x635A00
- CC..D8 = the line-scroll tables
- DC..F8 = 0x638A00..0x638A0E
- FC = 0x638A10, 100 = 0x639610, 104 = 0x5684B8, 108 = 0x5684BA, 10C = 0x63A210

Further pointer tables: layers 0x6C8270 = {62FA00, 631A00, 633A00, 635A00}; line scroll 0x6C8280 = {637A00, 637E00, 638200, 638600}.

**Name entry** (FUN_00205b60):
- Bits 0..13 = character.
- U = ((e>>7)&0x7F)*8 and V = (e&0x7F)*8 texels in the 1024x1024 sheet, so character n sits at column n>>7, row n&127.
- Palette = ((e&0x7F80)>>7) + (bit 15 ? 0x100 : 0). CBP = 0x35FC + palette.
- The colour field overlaps character bits 7..13, as in MAME segaic24.

**Name writers (confirmed):**
- Rectangle blit FUN_001f7ca0 and priority XOR FUN_001f7d30, both via 0x6C8270. Line scroll via FUN_001f7db0 (0x6C8280).
- Direct writers through the pointer table:
  - FUN_001ec850 (layer 2 via *0x5670C0, OR 0x8000)
  - FUN_001ec618 (layer 0 via *0x5670B8)
  - FUN_001ec730 and FUN_001ec9c0 (layer 1 via *0x5670C4)
- Cursor for these helpers: FUN_001eb128 -> 0x568478 (x), 0x56847C (y).
- FUN_001eef18 writes datarom+0x14F330 (64x30, 409 distinct values from 0x3480) to layer 0 and datarom+0x150B30 (64x39, 951 distinct values) to layer 2 (corrected).
- FUN_001f4b30 writes name 0x9570 to layer 1 rows 0..5 and 42..47. FUN_001f4c38 clears layer 1.

**Reset:** FUN_001eaea8 (about 90 call sites) clears the registers, the four name tables, 0xC00 halfwords at 0x638A10 (0x1eaf1c..0x1eaf38), the line scroll, and struct +0x4..+0x18.

### 2.2 Register image struct at 0x3B2FF0

- **Layout:** +0x00 handler pointer (0x1ea960 in all dumps), +0x04..+0x12 eight u16 registers, +0x14 optional line-scroll source pointer, +0x18 a word.
- **FUN_001ea960** (only caller 0x14dc6c in FUN_0014dbd8) copies the struct into tile RAM:
  - If +0x14 == 0: +0x04 goes to *0x5670DC (0x638A00).
  - If +0x14 != 0: it writes 0x8000 to 0x638A00 and calls FUN_001ea930(*0x5670CC, +0x14, (s16)+0x04), which adds +0x04 to 0x200 u16 into the line-scroll table 0x637A00.
  - Then +0x06..+0x12 go to *0x5670E0..*0x5670F8 (0x638A02..0x638A0E), and it calls FUN_001f7f38(1).
- **Writers of the struct:** FUN_001f4408 writes +0x6 = 0. FUN_001f44f8 writes +0x6. FUN_001f4b30 ORs 0x8000 into +0x10/+0x12 (layers 2 and 3 off).
- **Dump checks:**
  - Tick 4002: 0x3B2FF8 = 0x01CB = 0x638A04.
  - m2tileA 5000: +0x14 = 0x623F98, 0x638A00 = 0x8000, 0x637A00 holds 512 non-zero words, 0x3B2FF8 = 0x1D1 but 0x638A04 = 0x12E. So the copy is not always current at dump time.

### 2.3 Tile renderer (per frame)

- **Top level:** FUN_001f7bc8 calls FUN_00207538(mode, 0x62FA00) at 0x1F7C80.
  - FUN_00207538 loops over pairs: k=1 is layers 2/3, k=0 is layers 0/1.
  - The vscr bit 0x8000 skips a layer. When bit 0x4000 is set, the second layer reuses the first layer's scroll, vscr and line-scroll pointer. When bit 0x2000 is clear, the second layer reads its own vscr.
- **Per-layer calls:** FUN_00205b60 (sprite patch: 64 columns x 48 rows; TEX0 = 0x5675E0 | CBP<<37; UV = (e&0x7F)<<23 | (e&0x3F80); second UV +0x7F007F) and FUN_00205c58 (line scroll / XYOFFSET), called at 0x2076a8/0x207714.
- **W1 work area:**
  - Pointer at 0x568524 (gp-0x30CC) = 0x9DE080, size at 0x568528 = 0x2A6440.
  - Allocated and zeroed by FUN_002074a0 (called from 0x100530). It needs 16-byte alignment and size > 0x2A643F. It calls FUN_00206af0 (static templates) and FUN_00205320 (W1+0x296400, 0x10000 bytes).
  - Two halves h at W1 + h*0xB8000 (0x9DE080 / 0xA96080):

    | Half offset | Size | Content |
    |---|---|---|
    | +0x0000 | 0x8000 | CLUTs: 512 x 16 CT32 |
    | +0x8000 | 0x80000 | Char sheet copy |
    | +0x88000 | 0x30000 | Mask layers k=0 and k=1, 0x18000 each. 0xA66080 and 0xB1E080 are layer 0 of half 0 and of half 1. |

  - Packets at W1 + 0x170000 + h*0x93200, with sprite templates at +0xDB00/+0x2EBD0/+0x4FCA0/+0x70D70.
  - The frame toggle word is at W1+0x2A6400.
- **Sprite packet** (FUN_00205a18): PRIM 0x316 (sprite, TME, FST, context 2), FRAME_2 0x80000/0x80070, TEST_2 0x31001 (ATE, ATST NEVER, AFAIL FB_ONLY, so pen 0 is written). Then 3072 x REGLIST {TEX0_2, UV, XYZF2, UV, XYZF2}.
- **Kick:** jal 0x246860 at 0x207774.

### 2.4 Per-line tables 0x638A10 / 0x639610 (window/priority masks, not names)

- **Consumer:** FUN_00206ff8(src, half).
  - It runs only when 0x5675D0 (gp-0x4020) is set, which FUN_00207528 does (called from 0x1F7C60). Pair k must be enabled by bit k of 0x567650 (gp-0x3FA0; value 1 at tick 5000).
  - The control word is src+0x9008+4k.
- **Source selection:**
  - Bit 0x4000: per-line path at 0x2071A0 (not analysed). This path checks hscr bit 0x8000.
  - Else bit 0x2000: the static table 0x54D1D0 + (ctrl&0x3FF)*8.
  - Else: src+0x9010+k*0xC00.
- **Expansion:** 3072 bytes go to SPR 0x400. The LUT at 0x54FDD0 (loaded to SPR 0) turns bit i into PSMT4 nibble i. fromSPR 0x300 qw then writes the result to half+0x88000+k*0x18000.
- **Known writers of 0x638A10:**
  - FUN_001efb18: 3 u16 per line, stride 8.
  - FUN_001f4b30: 48 lines 0xFFFF, 288 lines 0, 48 lines 0xFFFF.
  - FUN_001f4408: 128 halfwords 0xFFFF from +0x940.
  - FUN_001f4620: clears 0x1000 u16 from 0x638A10, which runs 0x800 bytes into char RAM.
  - FUN_001f4c38: clears 0x800 u16.
- **Measured:** the tables are all zero in all 7 dumps. The expanded W1 half-1 layer 0 had 480 non-zero bytes at tick 5000.

---

## 3. Polygons / geometry

Section 3 follows the data from the game logic down to the VIF1 DMA kick.

### 3.1 Geometrizer input ("bufferRAM", "dmdata")

- **Buffer:** 0x332C40..0x362C40 (0xC000 words). The command list starts at word 0x4000 = 0x342C40. The word index is at 0x566D84 (gp-0x486C).
- **Format:** raw Model 2 geo command words, opcode = (w>>23)&0x1F, as in MAME geo_process_command.
- **Index handling:**
  - Write pointer FUN_001e8b78 (0x332C40 + idx*4).
  - Commit FUN_001e8ba0 ('BufferRAM Overrun' when idx > 0xBFFF).
  - List start FUN_001e8b68 = 0x342C40.
  - FUN_001e8b90 stores index = 0x4000; it is reached by a tail jump from 0x1f9c30 in FUN_001f9bd8.
- **InitCopro FUN_001e8938:** copies constants from 0x566EF4 into 0x332C40+0xC00..0xC58, prints 'dmdata/bufferRAM', and ends with index = 0.
- **Geo* writer API:** FUN_001f8958, FUN_001f8980, FUN_001f89b8 (end 0x07800f0f), FUN_001f8a40, FUN_001f9108 (object), FUN_001f92a0 (direct), FUN_001f9548, FUN_001f96b8, FUN_001f9748, FUN_001f97a8, FUN_001f9870, FUN_001f98c8, FUN_001f9918, FUN_001f9968, FUN_001f99b8, FUN_001f99e8.
- **Object command FUN_001f9108:**
  - Writes 0x00800101, TP, TH, PA, PN and a type word (bits 8-11 from 0x5674DC, bits 4-7 type from 0x5674D4, bits 0-3 no from 0x5674D8).
  - Rejects TH > 0x9FFFFF and PA outside 0x800000..0xBFFFFF.
  - Does nothing unless *0x564910 != 0.
- **Dumps:**
  - Tick 4002: index 0x486D, words 0x0b001616, 0x47800000, 0x03800707, ...
  - Tick 5000 (m2polys1): index 0x4315.

### 3.2 Geo parser FUN_001f9c58 (once per frame)

- **Call:** called at 0x1f8928 in FUN_001f87c8 when *0x5684CC (gp-0x3124) == 0; otherwise FUN_001f8118 runs. 0x5684CC was 0 in the dumps.
- **Dispatch:** jump table 0x54BA90. Opcode 0xF is caught before the table, at 0x1f9cd8. Undefined opcodes print 'Undefined Command' and loop forever. The output word counts per opcode were checked.
- **Object handler (op1):**
  - Arguments: w1 = TP, w2 = TH, w3 = PA, w4 = PN, w5 = type word.
  - Lookup: FUN_00200ca0(&{w2,w1,w3,w4}, (w5>>4)&0xF, (w5>>8)&0xF).
  - Hit: it emits {0x00800000, 0, 0, 0x00800000, chainptr}.
  - Miss, type 5: it searches the conversion cache or fills it (FUN_001f8f08).
  - Miss, other types: it prints 'NO PRECONV MODEL ...' and continues.
- **Direct data (op2):** copied to 0x73AF80..0x75AF80 (write pointer 0x75AF80; overflow above 0x8000 words gives 'GeoBufferDirect Overflow!'). Then FUN_001f8c00, FUN_001f8c58 and FUN_001f8df0 convert it.

### 3.3 Geo output list ("GeoBuffer")

- **Buffer:** 0x720F80..0x730F80.
  - Write pointer 0x712740, list pointer 0x712304 (= 0x720F80), start index 0x712300. InitTGP FUN_001f8690 sets them at 0x1f86e8/0x1f8704.
  - Overflow check at 0x1fa660.
  - FUN_001f9ba8 resets the list each frame by writing 0x07800000 at word 0.
- **Records:**
  - Object: 5 words {0x00800000 (0x08800000 from FUN_001f8d10 when (a1&0xFF) != 0), 0, 0, 0x00800000, chain}.
  - Matrix: 0x05800000 + 12 floats.
- **Measured contents:**

  | Tick | Objects | Matrices | Other | End |
  |---|---|---|---|---|
  | 4002 | 105 | 102 | 5 windows, 4 lights, 2 tex-param, 1 LOD | 0x722F8C |
  | 5000 | 90 | 88 | 10 op1B, 6 windows | 0x722C1C |

- **Consumer:** FUN_002093c0(0x712300, 0x6C8300) at 0x1f88ec -> FUN_00207908(*(0x712304), (*(0x712300)&0x7FFFC)>>2).

### 3.4 g_geo state block (base 0x6C8300, pointer at 0x5670A8)

| Field | Address | Content |
|---|---|---|
| +0x800 / +0x1000 / +0x1800 | 0x6C8B00.. | Colour-table byte reductions (see 4.5) |
| +0x2000 | 0x6CA300 | 0x800-byte table copied to the luma area (see 4.6) |
| +0xA000 | 0x6D2300 | Texture staging (256 KB) |
| +0x4A000 | 0x712300 | Render list header |
| +0x4A440 / 44 / 48..4B | 0x712740 / 744 / 748.. | Output write pointer / texture bank (-1 at init) / flags |
| +0x4A460 | 0x712760 | u32[0x800] converted chain per cache slot |
| +0x4C460 | 0x714760 | u8[0x800] per slot (0 in dumps) |
| +0x4CC60 | 0x714F60 | Cache 0x800 x 0x18: {u8 used, w1, w2, w3, w4, ttl=3} |
| +0x92C80 | 0x75AF80 | Direct buffer write pointer |

- FUN_001f9040 decrements the ttl each frame. At 0 the slot is freed (FUN_001f8eb8 -> FUN_001fe9a0).
- InitTGP clears only 0x8000 of the 0xC000-byte cache.
- Tick 5000: 4 slots in use (chains 0x108F580/0x108FB80/0x1090180/0x1090780). Tick 4002: 12 slots in use.

### 3.5 Preconverted model lists

- **Descriptors:** 0x4C19B8 + 0x14*k, chosen by FUN_00200060 (table 0x54CA30).
  - Type 2 -> 0x4C19B8. Type 3 -> 0x4C19CC/0x4C19E0 (by the bits 8-11 field). Type 4 -> 0x4C19F4/0x4C1A08. Type 5 -> 0x4C1A1C. Type 6 -> 0x4C1A30.
  - Descriptor layout: {entries, ptr, ptr, count, no}.
- **Entries:** 0x14 bytes each, {TH, TP, PA, PN, VIF1 chain}, in arrays at 0x490DB8 + n*0x3C00.
- **Counts:** 0 in the ELF, filled at run time. The type-5 count stays 0, so stage objects use the run-time cache. Types 2, 3, 4 and 6 have non-zero counts.
- **Lookup:** FUN_00200ca0 (linear search, returns entry+0x10).
- **Filler:** FUN_002008a8 calls the converter FUN_0020aec8 at 0x200a18. The rest of its logic is not traced.

### 3.6 Converter FUN_0020aec8 (Model 2 polygon data -> VIF1 chain)

- **Entry:** called from FUN_001f8df0. Output goes to scratch 0x730F80 (at most 0xA000 bytes), is copied to a heap-1 allocation, and the pointer is stored at 0x712760[slot].
- **Input:** polygon records of 0x28 bytes from PA+0x28. TH advances 0x10 when attr&3 == 1, otherwise 0xC.
- **Emitters:** FUN_0020ad30 when ((attr>>4)&0xF) > 0 and sp+0x68 == 0, otherwise FUN_00209c80. The mode global is gp-0x308C = 0x568564 (corrected).
- **Chain layout:**
  1. ref -> 0x550BB0 (NOP NOP FLUSH BASE 0x13C OFFSET 0x144 MSCALF 0).
  2. When TP+2 changes: ref qwc 8 -> 0x992440 + k*0x80 (luma rows) with STCYCL 4,4 + UNPACK S-8 128 to VU 0x30/0xB0.
  3. Per polygon: a cnt record, one of 4 shapes. Counts at tick 5000: cnt9 1786, cnt7 291, cnt8 284, cnt6 60. cnt9 = UNPACK S-16 ->0, V2-32x4 ->5, V3-32x5 ->9, V2-32x4 ->0xE, then MSCNT.
  4. ret.

### 3.7 Render-state work area W2 and VIF1 packet builder

- **W2:** pointer 0x56852C (gp-0x30C4) = 0x84CC80, size 0x568530 = 0x191350.
  - FUN_00209108 (called from 0x100554) zeroes it, then calls FUN_00208F60 and FUN_00205170 at W2+0x18DD80.
  - Sub-areas:
    - +h*0x1800: colour-table copies
    - +0x3000+h*0x21380: packets
    - +0x457C0: texture pages 0/1
    - +0xC57C0 / +0x1057C0: pages 2 / 3
    - +0x1457C0 = 0x992440: luma rows
    - +0x14D7C0: u32 (op5, &0x7FFF)
    - +0x16D7C0: VU1 staging, 32 qw
    - +0x16D9C0: u16 (op4)
    - +0x18D9C0: matrix -> VU1 0x3C4
    - +0x18D9F0: -> 0x3C7
    - +0x18DA00: light/mode
    - +0x18DA10: window table, stride 0x18
- **FUN_002093c0 frame setup:** 0x23D8E8, 0x23E330, 0x2488F8(0,0,0x1EF,0x17F), static chain 0x5502A0, FUN_0020BC10, then FUN_00207908.
- **FUN_00207908 (jump table 0x550B0C):**
  - op1/0x11 -> FUN_00207810, which adds a DMA CALL to w4 when w3&0xFFFF0000 is set and w4 is not 0 or -1.
  - op3 window table + chain 0x550490. op4/op5 the arrays above. op6 VU1 (w1>>2)&0x1F via FUN_002078c8 (scalar gp-0x3F98).
  - op7/opA light -> VU1 0x020. opB matrix -> 0x3C4. opC -> 0x3C7. opF -> FUN_00205200. op1B -> 0x136. op1C -> FUN_0020C1E8. op1D -> static chain.
  - Fixed uploads at entry: 0x550AA0 -> 0x3E0, 0x550350 -> 0x021, 0x550440 -> 0x137, staging -> 0x000.
- **VIF1 double buffers:**
  - Control struct 0x1F96840: 2 x 0x30-byte descriptors, current index at +0x60.
  - Buffers 0x568B40 and 0x5A9B40, 0x41000 each.
  - CALL nodes from FUN_00244340 (0x20 bytes: CALL then NEXT).
  - FUN_00244db0 kicks D1_TADR = *(desc+0x2C) & 0x7FFFFFFF, D1_CHCR 0x145.
  - Tick 5000, descriptor 1: 255 CALLs (46 into 0x1400000-0x1600000, 209 lower, for example cached chains and 0x5502A0).

---

## 4. Textures, luma, palettes

### 4.1 Char RAM (Model 2 tile char RAM)

- **Writer:** FUN_001F7DE8. Base at 0x56710C = 0x63A210.
  - For arcade word w, the EE address is base + 4*(((((w>>4)&0x38)+(w>>14))*128 + (w&0x7F))*16 + ((w>>10)&0xF)).
  - The stored value is nswap(b2)<<24 | nswap(b3)<<16 | nswap(b0)<<8 | nswap(b1).
- **Caller:** FUN_001EA0E8 (from 0x1EA5E0/0x1EA60C/0x1EA634). It byte-swaps each source halfword first, so the net effect is that every source byte keeps its order and is only nibble-swapped.
- **Layout:** the result is 64 PSMT4 128x128 tiles, with tile (x,y) at y*0x10000 + x*0x2000.
- **Upstream source file:** not verified.
- **Measured:**
  - Char RAM equals both W1 sheets (0x9E6080, 0xA9E080).
  - Ticks 4002/5000: 12929 of 16384 characters are non-zero, and character 0 is blank.

### 4.2 Char sheet upload -> GS TBP 0x3800 (14336), PSMT4 1024x1024, TBW 16

- **Copy:** FUN_00207460 copies 0x8000 qw from 0x62FA00+0xA810 to half+0x8000.
  - The counter is gp-0x3F9C = 0x567654 (corrected). mode == 1 resets it. Otherwise the copy runs while the incremented value is < 3.
- **Upload packet:** FUN_00205628 (via FUN_00205970) sends 64 x 128x128 transfers. BITBLTBUF is 0x14103800<<32. Each source is src + y*0x10000 + x*0x2000. Trace: about 151k such transfers.
- **Second writer of the same sheet:** W2 texture pages 0/1 (W2+0x457C0) are uploaded to the same DBP 0x3800 by FUN_00208A88, using the same BITBLTBUF constant. Their EE contents differ from char RAM in 411285 of 524288 bytes. Which upload the GS holds at a given moment was not measured.

### 4.3 Palettes

- **Conversion FUN_00206E30:**
  - It toSPRs the 32-entry curve from 0x6BC210 (00 25 37 45 ... FA FF; three copies at SPR 0x00/0x20/0x40).
  - Then 2 banks of 2048 RGB555 from 0x6BA210 + i*0x1000: bits 0-4 -> R, 5-9 -> G, 10-14 -> B.
  - Output to half+i*0x2000 (alpha 0xFF for pen 0) and to half+0x4000+i*0x2000 (alpha 0xFF for pen 0, 0x01 for the others).
  - Finally [0x4C1B00] = ([0x4C1B00]&0xFF000000) | (half[0]&0xFFFFFF) (corrected).
  - Tick 5000: palette RAM has 6543 of 8192 non-zero bytes, and palette 0 pen 0 = 0x2906 -> RGB 0x66,0x79,0x88.
- **Palette RAM writer:** not traced.
- **CLUT uploads FUN_002053F0:** 0x200 transfers, DBP 0x35FC+i <- half+i*0x40, plus DBP 0x37FC <- 0x4C1B00. Slots 0x37FD/0x37FE: see the refuted section (the checker corrected the sources to 0x54D180 / 0x54D140).

### 4.4 Model 2 texture RAM pages (polygon textures)

- **Staging:** 0x6D2300 (256 KB). It is filled by the LoadHiTextureMap path (1.5). FUN_001F87C8 then calls FUN_001F9B40 (nibble swap) and FUN_002091B0(page = [0x712744], 0x6D2300).
- **Pages 2/3:**
  - FUN_00204748 copies 0x4000 qw to W2 + 0x457C0 + (page<<18).
  - The flag *(W2+0x185D34) = 1 is set only on this path.
  - Upload FUN_00208DA0: 2 x 512x1024 PSMT4 to the DBP in 0x5676B8/0x5676BC, both 0x2A10 (10768).
  - Measured at tick 5000 (m2tex_a): page 3 = 0x6D2300 = nswap(maptexture3 second half), and page 2 = nswap(statictexture second half).
- **Pages 0/1:**
  - 8 rows x 4 chunks. FUN_002049d0 does an interleaved toSPR (D_SQWC TQWC=4, SQWC=12, CHCR 0x109), then FUN_00204690 does a fromSPR to W2 + 0x457C0 + (bank<<15) + row*0x10000 + chunk*0x2000.
  - Upload: FUN_00208A88 to DBP 0x3800.
  - Measured content is wrong. At tick 5000 the whole 0x892440..0x912440 is one 0x2000-byte block repeated 64 times, and no part of the expected first halves is present. That block also occurs at 0x6B6210, 0xA62080 and 0xB1A080.
  - Code read (not run-verified): the runtime's `PS2Memory::tryProcessScratchpadDma` (ps2_memory.cpp:1993-1995) returns false when the CHCR mode != 0. So the interleave transfer is skipped and fromSPR copies stale scratchpad contents.
- **Polygon TEX0 selection:** base 0x3800, or 0x2A10 when polygon flag bit 10 is set (0x209F38-0x209F4C).

### 4.5 Colour-translation tables (Model 2 colorxlat, 3 x 16 KB)

- **Source:** [0x5670AC/B0/B4] = 0x6BC270 / 0x6C0270 / 0x6C4270, 32 rows x 512 bytes each. They are filled from 0x6C0270-based data by FUN_001f7bc8 when gp-0x3134 bit 4 is set. The writer of these tables is not traced.
- **Reduction:** FUN_001F87C8 (0x1F8858-0x1F88C8) does S[0x800+v*64+t] = R[v*512+t*2], and the same for G (S+0x1000) and B (S+0x1800), with S = 0x6C8300.
- **Copy:** to W2+h*0x1800, measured equal. FUN_00208448 is the likely copier, via FUN_00204748(dst, g_geo+0x800, 0x180).
- **Upload FUN_00208910/FUN_00208698:** PSMT8 64x32 to DBP 0x3340 at (0,0), (0,32) and (64,0), which are TBP 13120, 13128 and 13136 (corrected). The CLUT is DBP 0x3358 from 0x550660 (identity ramp).
- **Draw FUN_00208768:** 3 sprites into FBP 409 with FBMSK 0xFFFF00 (13120), 0xFF00FF (13128) and 0xFFFF (13136).
- **Readers of FBP 409:** not identified.

### 4.6 Luma-candidate table

- **Copy:** FUN_00209308 does FUN_00204748(W2+0x1457C0 = 0x992440, 0x6CA300, 0x800) when [0x712749] != 0.
- **Partial fill:** only the first 0x800 bytes (blocks 0..15) are copied this way. Ticks 4002/5000 show 66 of 256 non-zero 128-byte blocks, with a maximum byte value of 63. The source of blocks 17 and up is not traced.
- **Reader:** the only caller of FUN_002077F8 is FUN_0020A548 (0x20A56C). It emits a ref of 8 qw from W2+0x1457C0 + byte*0x80 (see 3.6).
- **Meaning:** that this table is Model 2 luma RAM is not verified.

### 4.7 Mask layers -> GS

- **Uploads:** FUN_00206068 (mode 0, 64x384 DBW 2) and FUN_002062A8 (mode 1, 512x384 DBW 8). Both use FUN_00205798.
  - k=0: half+0x88000 -> DBP 0x33FC (13308).
  - k=1: half+0xA0000 -> DBP 0x31FC (12796).
  - m2tex_a counts: 1029 x 0x33FC 64x384, 153 x 0x33FC 512x384, 1183 x 0x31FC 64x384.

---

## 5. How the GS output is built from each piece

### 5.1 Tilemap -> FBP 112 sprites

1. FUN_00207538 converts the palettes (FUN_00206E30) and copies the char sheet (FUN_00207460).
2. It patches TEX0.CBP and UV into the 3072-sprite templates per layer (FUN_00205b60).
3. It kicks the W1 packet (0x246860).
4. The packet uploads the CLUTs to 0x35FC.. and the sheet to 0x3800, then draws 8x8 PSMT4 sprites (TBP 14336, TBW 16) into FBP 112 with alpha test NEVER/FB_ONLY.

### 5.2 Mask/priority composite into FBP 224

Measured draw order in run tr343 (FBMSK = the frame-buffer bits that are kept):

| Step | Sprites | Source | CLUT / FBMSK |
|---|---|---|---|
| 1 | 8 | TBP 12796 PSMT4 | CBP 14332, FBMSK 0x7F000000 |
| 2 | 16 | FBP 112 copies (TBP 3584) | FBMSK 0x80000000 |
| 3 | 8 | TBP 13308 | FBMSK 0x7FFFFFFF |
| 4 | 16 | FBP 112 copies | — |
| 5 | 8 | TBP 7168 as PSMT4HL | CBP 14333, FBMSK 0x00FFFFFF |
| 6 | 8 | TBP 14335 32x16 (FUN_002058A8 from 0x54D040) | CBP 14334 |

Calling this a "mask/priority pass" is an interpretation and is not verified.

### 5.3 Polygons -> VU1

1. The game writes geo commands to bufferRAM.
2. FUN_001f9c58 normalises them into GeoBuffer and resolves each object to a VIF1 chain (preconverted, or cached via FUN_0020aec8).
3. FUN_00207908 turns the list into VU1 uploads plus DMA CALLs in the VIF1 double buffer.
4. FUN_00244db0 kicks DMA channel 1.
5. The chains reference luma rows at 0x992440 and texture sheets 0x3800/0x2A10. robot draws are prim 4/5 into FBP 224 with TBW 16, PSMT4, CBP 13152 (run draws: 269 draws).

### 5.4 Colour translation -> FBP 409

Three PSMT8 64x32 images are drawn into the R/G/B bytes of FBP 409 (4.5).

---

## 6. Why the tilemap is empty

### 6.1 Measured chain

1. The sky sprites (FBP 112) use TBP 14336, TBW 16, PSMT4, CBP 13820 and UV 0..8 texels, so every cell samples character 0 with palette 0 (tr343, m2tileA).
2. The decode is correct. In m2tileA tick 5000, the 746 non-zero layer-0 entries produced matching UV/CBP. The other 11542 of 12288 sprites have uv 0,0 and cbp 13820 because their name entries are 0.
3. The name tables hold this data:
   - Layers 1..3 (0x631A00, 0x633A00, 0x635A00) are entirely zero in all 7 dumps.
   - Layer 0 (0x62FA00) holds only text-like rows: 0 / 646 / 0 / 472 / 174 / 746 / 572 non-zero entries, in row pairs 12-13, 19-32 and 37-39.
4. Everything else in the path is populated: char RAM (12929 non-zero characters, character 0 blank), palette RAM, the GS sheet at 0x3800 (352413 of 524288 bytes non-zero at 5000), and the CLUTs.
5. The layers are enabled: the vscr bit 0x8000 is clear in the dumps. Empty cells are therefore drawn opaquely as character 0, pen 0 = RGB(0x66,0x79,0x88), alpha 0xFF. That colour is the uniform "sky".
6. The five routines named earlier (FUN_001efb18, FUN_001f4408, FUN_001f4620, FUN_001f4b30, FUN_001f4c38) mainly write the per-line table 0x638A10 and struct registers. Some also write or clear layer 1 names. None of them fills layers 2/3 with background.
   - FUN_001efb18 is called only from FUN_001f33b8/FUN_001f1ad8, at 0x1eadd8/0x1eae04, when *0x513104 == 4 and *0x513108 is 7..12.
   - FUN_0017d9c0 and FUN_00187828 are reached through the table at 0x53B258/0x53B2A0 (repeated at 0x53B530/578/630/678/730/778).
   - In the dumps *0x513104 = 1 and *0x513108 = 7 (0/0 at tick 3000), so these gates are closed.
7. FUN_001eaea8 resets all four name tables. In m2tileA, FUN_001eef18 wrote layer 0 and layer 2 from datarom (+0x14F330 and +0x150B30). The tilemap mapper observed layer 2 zero again by tick 5000, but its watch skipped the clear loop at pc 0x1eafc4. That observation was not separately confirmed by the checker.

### 6.2 Not established

- Whether the arcade fills layers 1..3 in this scene at all.
- Whether pen 0 should be transparent here, with a polygon sky behind it.
- Whether the attract demo should be in mode 4 (battle).
- Separately from the names: W2 texture pages 0/1, which share GS TBP 0x3800 with the char sheet, contain stale data because the mode-2 interleave toSPR is not emulated (code read, not run-verified). This affects polygon textures. It does not explain the zero name entries.

### 6.3 Exact next step

Run one game with a write watch on the three empty name tables and no skip, plus a RAM dump at the sky tick:

```
bash moon/tools/run.sh m2names 110 PS2X_GS_BACKEND=cpu PS2X_WATCH_WRITES=631A00,6000 PS2X_DUMP_RAM_TICK=5000
```

From the `[watch]` lines (pc/ra), list every writer of 0x631A00..0x637A00 and the last clear before tick 5000 (FUN_001eaea8 caller via ra, or FUN_001f4c38/FUN_001ec9c0).

- **If no non-clear writer appears:** the game never fills these layers in mode 1/sub 7. Then trace which mode-table entry (0x53B258..) runs for *0x513104 = 1, and compare it with the arcade attract sequence.
- **If a writer appears and is followed by a clear:** the clear's caller (ra) is the cause to examine.

The watch logs only the first 64 events and then power-of-two ordinals. If the result is ambiguous, repeat with `PS2X_WATCH_SKIP_PC=<dominant clear pc>`.

---

## 7. Refuted or unverifiable entries (checker notes)

| Area | Entry | Status | Checker note (summary) |
|---|---|---|---|
| tilemap | Char RAM layout "character n: column n&127, row n>>7" | refuted | Transposed. Character n is at column n>>7, row n&127, matching FUN_00205b60. FUN_00207460 also runs always when mode == 1. FUN_001f7de8 has 10 call sites (0x1874b8..0x187740, 0x1ea164, 0x1ea744, 0x1eb0d4, 0x1f4b04). FUN_001f7ea0 has no jal caller. |
| tilemap | Palette RAM / CLUT conversion (LUT from 0x54FDC0; alpha 0x01 on all copies) | refuted | The LUT is DMA'd from 0x6BC210 (a gamma ramp; filled from 0x6C0270+... by FUN_001f7bc8 when gp-0x3134 bit 4 is set). Alpha is 0x00 for non-zero pens in the first copy (palettes 0..255) and 0x01 only in the second copy (palettes 0x100..0x1FF). FUN_00206e30 also writes palette 0 pen 0 into 0x4C1B00. |
| tilemap | Per-scanline table writer list (fillers do not touch names) | refuted | FUN_001f4b30 writes name 0x9570 to layer 1. FUN_001f4c38 clears layer 1 and only 0x800 u16 of the table. FUN_001f4620 (via FUN_001ec9c0) clears layer 1 and clears 0x1000 u16 from 0x638A10, overrunning into char RAM. FUN_001f44f8 writes layer 1. |
| tilemap | FUN_00205b60 trace count "11742 of 12288 uv 0,0" | refuted | The count is 11542 (12288 - 746). Example cbp 14210 = palette 0x186 (bit 15 set). |
| tilemap | Zero-tilemap finding "all four control words 0" | refuted | At tick 2500, 0x638A08 = 0x4000 (per-line mode, 0x637A00 filled). At m2tileA 5000, 0x638A00 = 0x8000. The fillers do write or clear layer 1. Mode gating is confirmed. How 0x53B2xx is indexed by *0x513104 is not verified. |
| tilemap | Open question: per-line colour texture 0xA66080/0xB1E080 zero at every tick | refuted | 3840 non-zero bytes at m2tileA 5000, 480 at m2polys1 5000, and 1920 at 0xB1E080 at 3300. -0x3130(gp) = 6 at m2tileA 5000. The writer is not traced. |
| tilemap | Register order "scroll 0x638A00..06 / control 0x638A08..0E" | corrected (entry confirmed) | They are MAME hscr/vscr. The vscr low bits are the vertical scroll in FUN_00205b60. |
| texture | Tile CLUT upload sources for 0x37FD/0x37FE | refuted | 0x37FD <- 0x54D180 (0x000000FF, 0x400000FF, 0x00222222 ... 0x00FFFFFF), 0x37FE <- 0x54D140 (entry0 0, entry1 0x80FFFFFF). 0x54D100 is not used. Other 8x2 CT32 targets seen: 0x3DF9, 0x37FF, 0x3360 (FBP 411, 1180 uploads), 0x2A14. |
| texture | Robot CLUT LINE prims read TBP 13280 64x1024; mask uploads span to ~0x36FC | refuted | The block counts are confirmed: 81 blocks 0x33E2..0x3717 (65/16) and 41 blocks (27/14). The span is wrong: 512x384 PSMT4 = 0x180 blocks, ending near 0x357B (64x384 near 0x345B), so the masks do not reach 0x35FC. gsvram has non-zero data in 0x357C..0x35FB that no mask upload covers; the writer is not verified. |
| texture | Other GS uploads 0x3C00/0x3E00/0x3DFA/0x2A15 | unverifiable | Counts confirmed. 0x22DD24/0x1B0A58 are register+immediate adds, and that they are DBP values is not verified. Writer path not checked. |
| texture | "0x6D2300 staging arrives by SIF/IOP DMA; source file not found" | refuted (sub-claim) | 0x6D2300 == nswap(decoded maptexture3 second half) and page 2 == nswap(decoded statictexture second half). The raw PAK entries are lzs2-compressed. The watch is sampled, so the absence of EE stores is not proof. |
| polygons | VU1 microprogram / init chain at 0x5184D0 (FUN_0023d938) | unverifiable | Static bytes confirmed: DIRECTHL 6, STCYCL + UNPACK V4-32 to 0x3EC, MPG at VU1 0x000..0x700. 0x517D00 starts with a NEXT tag to 0x518060 and an MPG of 0x61 at 0. Kicked from 0x2478b4 (FUN_00247578). Whether it runs is not checked. |
| polygons | "pdtrom first bytes not found in RAM" | corrected | The first 16 bytes occur at 0x154E3A4 at tick 5000; the first 64 bytes are not found. Residency is not established. |
| polygons | "46 CALL tags into the preconverted range 0x1400000-0x1600000" | corrected | 255 CALLs in total: 46 into that range and 209 lower (cached chains, static 0x5502A0, preconverted chains below 0x1400000). |
| loading | coprom/motrom slice equalities pd20 == pdtrom start, tp20 == tdt0rom start, mot30 == motrom+8 | refuted | Only prefixes match: 2352, 240 and 12600 bytes respectively. |
| loading | EE texture bank image banks 0/1 "unverified" | resolved by checker | Stale repeated 0x2000 block; cause is the mode-2 interleave DMA not being emulated (code read, not run-verified; see 4.4). |
| loading | Disc path labels SRD/DVCI and the actual sceCdRead calls | unverifiable | The search result and call sites are confirmed; the labels and the runtime reads are not. |
| loading | Zero-tilemap finding of the loading area | unverifiable | The loading facts hold. The link between the wrong banks 0/1 and the character-0 sky is not verified. datarom[8..0x18) = 3c42424242423c00 0818280808083e00 (glyph-like); its link to the sheet is not verified. |
| texture | Luma copy destination "0x991440" | corrected | It is 0x992440. |
| texture | FBP 224 "mask/priority pass, not robot texels" | interpretation | The draw order is confirmed; the interpretation is not verified. |
