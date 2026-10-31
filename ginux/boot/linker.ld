ENTRY(_start)

SECTIONS
{
    . = 1M;
    kernel_start = .;

    .multiboot_hdr BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
    }

    text_start = .;
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.text)
    }
    text_end = .;

    rodata_start = .;
    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }
    rodata_end = .;

    data_start = .;
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }
    data_end = .;

    bss_start = .;
    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }
    bss_end = .;

    kernel_end = .;
}

