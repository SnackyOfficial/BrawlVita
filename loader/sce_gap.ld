/* vita-elf-create writes the module's import tables (a few KB) right after the last section of the code segment, and the default VitaSDK linker
 * script starts the data segment at the next 64 KB boundary. When the code happened to end less than that before the boundary, the build failed with
 * "Cannot allocate N bytes for SCE data at end of segment 0; segment 1 overlaps" (update 24, 3628 bytes needed, 3208 free).
 * This moves the location counter (not a section: segment 0 keeps its size) past the boundary when fewer than 16 KB are left, so the data segment starts
 * one 64 KB block later and the import tables always fit. */
SECTIONS
{
  . = (0x10000 - (. & 0xffff)) < 0x4000 ? . + 0x4000 : .;
}
INSERT AFTER .eh_frame;
