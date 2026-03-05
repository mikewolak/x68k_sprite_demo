typedef unsigned short uint16_t;
typedef unsigned long uint32_t;

static void print_hex(volatile uint16_t *tvram, int pos, uint32_t val, int digits, uint16_t attr) {
    int i;
    for(i = 0; i < digits; i++) {
        uint32_t nibble = (val >> (4 * (digits - 1 - i))) & 0xF;
        tvram[pos + i] = attr | (nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
    }
}

static void print_str(volatile uint16_t *tvram, int pos, const char *str, uint16_t attr) {
    int i;
    for(i = 0; str[i]; i++) {
        tvram[pos + i] = attr | str[i];
    }
}

void uhe(uint32_t *frame) {
    // Guru Meditation style exception handler
    volatile uint16_t *tvram = (uint16_t*)0xE00000;
    volatile uint16_t *crtc = (uint16_t*)0xE80028;
    uint32_t vector = frame[34];
    uint32_t pc = frame[36];
    uint32_t sr = frame[35];
    int i, row;
    uint16_t red_bg = 0x00E0;   // Red background
    uint16_t black_bg = 0x0000;  // Black background

    // Switch to text mode
    *crtc = 0x0012;  // 512×512 text mode

    // Clear screen to black
    for(i = 0; i < 96 * 128; i++) {
        tvram[i] = black_bg | ' ';
    }

    // Centered title (row 10)
    row = 10 * 96 + 30;
    print_str(tvram, row, "  SOFTWARE FAILURE  ", red_bg);

    // Exception info (centered, starting row 15)
    row = 15 * 96 + 25;
    print_str(tvram, row, "EXCEPTION: ", black_bg);
    print_hex(tvram, row + 11, vector, 2, black_bg);

    row = 17 * 96 + 25;
    print_str(tvram, row, "PC: ", black_bg);
    print_hex(tvram, row + 4, pc, 8, black_bg);

    row = 19 * 96 + 25;
    print_str(tvram, row, "SR: ", black_bg);
    print_hex(tvram, row + 4, sr, 4, black_bg);

    // Display registers D0-D7, A0-A6
    row = 22 * 96 + 20;
    for(i = 0; i < 8; i++) {
        print_str(tvram, row, "D", black_bg);
        print_hex(tvram, row + 1, i, 1, black_bg);
        print_str(tvram, row + 2, ":", black_bg);
        print_hex(tvram, row + 3, frame[2 + i], 8, black_bg);
        row += 96;
    }

    row = 22 * 96 + 50;
    for(i = 0; i < 7; i++) {
        print_str(tvram, row, "A", black_bg);
        print_hex(tvram, row + 1, i, 1, black_bg);
        print_str(tvram, row + 2, ":", black_bg);
        print_hex(tvram, row + 3, frame[10 + i], 8, black_bg);
        row += 96;
    }

    // Halt message
    row = 35 * 96 + 28;
    print_str(tvram, row, "SYSTEM HALTED", red_bg);

    // Infinite loop
    for(;;);
}
