| JagSkyroads - the C blob's entry table.
|
| RMAC calls the blob through FIXED offsets (C_ENTRY_INIT, C_ENTRY_TICK in
| include/map.inc), six bytes each: it does not read the linker's symbols.
| New entries go at the END, never in the middle.
|
| Nothing else to do here: ColdBoot copies the whole image (code, rodata,
| data) to DBLOB, and .bss lives in CBSS, which the boot's DRAM clear has
| already zeroed.

	.section .entry,"ax"
	.global entry_table
entry_table:
	jmp	c_init
	jmp	c_tick
