// compile with Turbo C's TCC Compiler, Made by Rajeev Tiwari with the help of Google Gemini

#include <stdio.h>
#include <dos.h>
#include <conio.h>
#include <string.h>
#include <bios.h> // NEW: For direct hardware keyboard access

#define VIDEO_ADDRESS 0xB800

// --- Global Settings State (Inherited from Notepad + New Serial Vars) ---
int show_settings = 0;
int current_settings_tab = 0; // 0 = Serial, 1 = Display, 2 = Graphics, 3 = Other

int setting_com_port = 1;     // 1 to 4
long setting_baud_rate = 115200;
int setting_line_ending = 3;  // 0=None, 1=NL, 2=CR, 3=NL/CR

int setting_hover_effects = 1;
int setting_ansi_mode = 1;
int setting_hex_mode = 0; // NEW: 0 = Normal Text, 1 = Raw Hex Bytes
int setting_color = 1;
int setting_bg_color = 0;
int setting_fg_color = 7;
int setting_sel_color = 1;
int setting_crit_color = 4;

int running = 1; 

// --- Mouse Definitions & State ---
#define MOUSE_INT 0x33
#define INIT_MOUSE 0x00
#define SHOW_MOUSE 0x01
#define HIDE_MOUSE 0x02
#define GET_MOUSE_STATUS 0x03
#define CURSOR_CHAR 0x0D

int mouse_x = 0, mouse_y = 0;
int mouse_left = 0, mouse_right = 0;

//config saving and loading
void save_settings() {
    FILE *f = fopen("configSe.txt", "w");
    if (f) {
        // Dump all 11 setting variables separated by spaces
        fprintf(f, "%d %ld %d %d %d %d %d %d %d %d %d\n",
                setting_com_port, setting_baud_rate, setting_line_ending,
                setting_hover_effects, setting_ansi_mode, setting_hex_mode,
                setting_color, setting_bg_color, setting_fg_color,
                setting_sel_color, setting_crit_color);
        fclose(f);
    }
}

void load_settings() {
    FILE *f = fopen("configSe.txt", "r");
    if (f) {
        // Read them back in the exact same order
        fscanf(f, "%d %ld %d %d %d %d %d %d %d %d %d",
                &setting_com_port, &setting_baud_rate, &setting_line_ending,
                &setting_hover_effects, &setting_ansi_mode, &setting_hex_mode,
                &setting_color, &setting_bg_color, &setting_fg_color,
                &setting_sel_color, &setting_crit_color);
        fclose(f);
    }
}

// --- Theme Engine Forward Declarations ---
unsigned char make_color(unsigned char bg, unsigned char fg);
unsigned char get_bg_color();
unsigned char get_hl_color();
unsigned char get_cr_color();
unsigned char decode_char(char ch);

unsigned char mouse_arrow[16] = {
    0x00, 0x00,0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF,
    0xFF, 0xF8, 0xEC, 0xCC, 0x86, 0x02
};

// --- Mouse Functions ---
void load_cursor_glyph() {
    struct REGPACK reg;
    reg.r_ax = 0x1100; reg.r_bx = 0x1000; reg.r_cx = 1;
    reg.r_dx = CURSOR_CHAR; reg.r_es = FP_SEG(mouse_arrow); reg.r_bp = FP_OFF(mouse_arrow);
    intr(0x10, &reg);
}

void show_mouse() { 
    union REGS regs; 
    regs.x.ax = SHOW_MOUSE; 
    int86(MOUSE_INT, &regs, &regs); 
}

void hide_mouse() { 
    union REGS regs; 
    regs.x.ax = HIDE_MOUSE; 
    int86(MOUSE_INT, &regs, &regs); 
}

int init_mouse() {
    union REGS regs; regs.x.ax = INIT_MOUSE; int86(MOUSE_INT, &regs, &regs);
    if (regs.x.ax == 0xFFFF) {
        regs.x.ax = 0x000A; regs.x.bx = 0x0000; regs.x.cx = 0xF000; 
        regs.x.dx = 0x0F00 | CURSOR_CHAR; 
        int86(MOUSE_INT, &regs, &regs); 
        show_mouse(); 
        return 1;
    }
    return 0;
}

void update_mouse() {
    union REGS regs; regs.x.ax = GET_MOUSE_STATUS; int86(MOUSE_INT, &regs, &regs);
    mouse_x = regs.x.cx / 8; mouse_y = regs.x.dx / 8; 
    mouse_left = (regs.x.bx & 1); mouse_right = (regs.x.bx & 2);
}

// --- Input & Pseudo-Cursor State ---
char input_buffer[71] = ""; 
int input_cursor = 0;
int pseudo_cursor_state = 1;
int select_start = -1; // NEW: -1 means nothing is selected

void enable_bright_backgrounds() {
    union REGS regs;
    // INT 10h, AX=1003h: Toggle intensity/blinking
    regs.x.ax = 0x1003; 
    // BL=00h: Enable bright backgrounds (disable blinking)
    // BL=01h: Enable blinking (disable bright backgrounds)
    regs.h.bl = 0x00;   
    int86(0x10, &regs, &regs);
}

void hide_hardware_cursor() {
    union REGS regs;

    regs.h.ah = 0x01;
    regs.h.ch = 0x20; // bit 5 = 1 → hide
    regs.h.cl = 0x00;

    int86(0x10, &regs, &regs);
}

void show_hardware_cursor() {
    union REGS regs;

    regs.h.ah = 0x01;
    regs.h.ch = 0x06; // start scanline
    regs.h.cl = 0x07; // end scanline (must be >= CH)

    int86(0x10, &regs, &regs);
}


// --- Serial Data Buffer & Scrolling ---
#define MAX_SERIAL_LINES 100
unsigned char serial_chars[MAX_SERIAL_LINES][74];
unsigned char serial_colors[MAX_SERIAL_LINES][74];
int serial_lengths[MAX_SERIAL_LINES];

int total_serial_lines = 0;
int scroll_y = 0;
int max_vis_lines = 19; 

// --- Hardware & ANSI State ---
char rx_char_buffer[74];
unsigned char rx_color_buffer[74];
int rx_cursor = 0;

int ansi_state = 0;
int ansi_param = 0;
unsigned char current_rx_color = 0x07; // Will be initialized by theme

void add_serial_line(const char* text) {
    if (total_serial_lines < MAX_SERIAL_LINES) {
        unsigned char theme_color = get_bg_color(); // Grab the default theme color
        int len = strlen(text);
        if (len > 72) len = 72; // Cap the length to fit the screen
        
        // Loop through the text and save BOTH the character and the color
        for(int i = 0; i < len; i++) {
            serial_chars[total_serial_lines][i] = text[i];
            serial_colors[total_serial_lines][i] = theme_color;
        }
        
        // Save the length of this specific line for the renderer
        serial_lengths[total_serial_lines] = len;
        
        total_serial_lines++;
        
        // Auto-scroll to the bottom if we fill the screen
        if (total_serial_lines > max_vis_lines) {
            scroll_y = total_serial_lines - max_vis_lines;
        }
    }
}

// --- Hardware Serial Logic ---
char rx_buffer[74] = "";

// --- The ANSI Color Translator ---
// --- The ANSI Color Translator ---
void apply_ansi_code(int code) {
    unsigned char bg = current_rx_color & 0xF0;
    unsigned char fg = current_rx_color & 0x0F;
    
    // Reset explicitly ignores the failsafe so normal text blends perfectly
    if (code == 0) { current_rx_color = get_bg_color(); return; } 
    if (code == 1) { fg |= 0x08; current_rx_color = bg | fg; return; } // Bold/Bright
    
    // The master map translating ANSI order to DOS VGA order
    const unsigned char dos_map[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    
    // Standard Foreground (30-37) - Normal Intensity
    if (code >= 30 && code <= 37) {
        fg = dos_map[code - 30]; 
    }
    // Standard Background (40-47)
    else if (code >= 40 && code <= 47) {
        bg = dos_map[code - 40] << 4; 
    }
    // Bright Foreground (90-97)
    else if (code >= 90 && code <= 97) {
        fg = 0x08 | dos_map[code - 90]; 
    }
    // Bright Background (100-107) 
    else if (code >= 100 && code <= 107) {
        bg = (0x08 | dos_map[code - 100]) << 4; 
    }

    // --- NEW SAFETY CONDITION ---
    // Grab just the background bits (top 4 bits) of your current theme
    unsigned char theme_bg = get_bg_color() & 0xF0; 
    
    // If the incoming ANSI background matches your theme's background, force it to Black (0x00)
    if (bg == theme_bg) {
        bg = 0x00; 
    }

    // Combine them and save!
    current_rx_color = bg | fg;
}

// --- Buffer Management ---
void commit_rx_buffer() {
    if (total_serial_lines < MAX_SERIAL_LINES) {
        for(int i = 0; i < rx_cursor; i++) {
            serial_chars[total_serial_lines][i] = rx_char_buffer[i];
            serial_colors[total_serial_lines][i] = rx_color_buffer[i];
        }
        serial_lengths[total_serial_lines] = rx_cursor;
        total_serial_lines++;
        if (total_serial_lines > max_vis_lines) scroll_y = total_serial_lines - max_vis_lines;
        rx_cursor = 0;
    }
}

void add_local_serial_line(const char* text, unsigned char color) {
    if (total_serial_lines < MAX_SERIAL_LINES) {
        int len = strlen(text);
        if (len > 72) len = 72;
        for(int i = 0; i < len; i++) {
            serial_chars[total_serial_lines][i] = text[i];
            serial_colors[total_serial_lines][i] = color;
        }
        serial_lengths[total_serial_lines] = len;
        total_serial_lines++;
        if (total_serial_lines > max_vis_lines) scroll_y = total_serial_lines - max_vis_lines;
    }
}

int get_com_base() {
    if (setting_com_port == 2) return 0x2F8; // COM2
    if (setting_com_port == 3) return 0x3E8; // COM3
    if (setting_com_port == 4) return 0x2E8; // COM4
    return 0x3F8; // Default COM1
}

void init_serial() {
    int base = get_com_base();
    int divisor = 115200 / setting_baud_rate;

    outportb(base + 1, 0x00);    // Disable all interrupts
    outportb(base + 3, 0x80);    // Enable DLAB (Divisor Latch Access Bit)
    outportb(base + 0, divisor & 0xFF);         // Divisor Low Byte
    outportb(base + 1, (divisor >> 8) & 0xFF);  // Divisor High Byte
    outportb(base + 3, 0x03);    // 8 Bits, No Parity, 1 Stop Bit (8N1)
    outportb(base + 2, 0xC7);    // Enable FIFO, clear them, 14-byte threshold
    outportb(base + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

// Returns 1 if a full line was received and added to the UI
int read_serial() {
    int base = get_com_base();
    int line_added = 0;
    
    // Polling: While Bit 0 of the Line Status Register is 1 (Data is ready)
    while (inp(base + 5) & 0x01) {
        char c = inp(base + 0); 
        
        // --- NEW: HEX MODE INTERCEPTOR ---
        if (setting_hex_mode) {
            char hex_str[5];
            sprintf(hex_str, "%02X ", (unsigned char)c); // Format as "FF "
            
            for (int i = 0; i < 3; i++) { // Push the 3 characters into memory
                if (rx_cursor < 72) {
                    rx_char_buffer[rx_cursor] = hex_str[i];
                    rx_color_buffer[rx_cursor] = get_bg_color();
                    rx_cursor++;
                }
            }
            
            // FIXED: Commit to screen if line is full, OR if we receive a Line Ending (\r or \n)
            if (rx_cursor >= 69 || c == '\r' || c == '\n') { 
                commit_rx_buffer(); 
                line_added = 1; 
            }
            
            continue; // Skip the rest of the text/ANSI parser for this byte!
        }

        // --- 1. ANSI State Machine ---
        if (ansi_state == 1) { // Got ESC (\x1B)
            if (c == '[') { ansi_state = 2; ansi_param = 0; }
            else ansi_state = 0; // Abort, not an ANSI code
        }
        else if (ansi_state == 2) { // Parsing parameters (like "31" for Red)
            if (c >= '0' && c <= '9') ansi_param = (ansi_param * 10) + (c - '0');
            else if (c == ';') { apply_ansi_code(ansi_param); ansi_param = 0; } // Multi-param
            else if (c == 'm') { apply_ansi_code(ansi_param); ansi_state = 0; } // End of ANSI code
            else ansi_state = 0; // Abort
        }
        else if (c == 27) { // 27 is ASCII for ESC (\x1B). Start sequence!
            ansi_state = 1;
        }
        // --- 2. Normal Text & Line Ending Handling ---
        else if (c == '\r' || c == '\n') {
            if (rx_cursor > 0) { 
                commit_rx_buffer(); // Push both text AND color arrays to memory
                line_added = 1; 
            }
        } 
        else if (c >= 32 && c <= 126 && rx_cursor < 72) {
            rx_char_buffer[rx_cursor] = c;
            rx_color_buffer[rx_cursor] = current_rx_color; // Save the active ANSI color!
            rx_cursor++;
        }
    }
    return line_added;
}

void send_serial_char(char c) {
    int base = get_com_base();
    // Wait until the Transmit Holding Register is empty (Bit 5 of LSR is 1)
    while ((inportb(base + 5) & 0x20) == 0);
    outportb(base + 0, c);
}

// --- NEW: Helper to convert a Hex Character to a Value ---
unsigned char hex_char_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

void send_serial_string(const char* str) {
    if (setting_hex_mode) {
        // --- HEX SENDER ---
        int i = 0;
        while (str[i] != '\0') {
            if (str[i] == ' ') { i++; continue; } // Skip spaces
            
            char c1 = str[i++];
            if (c1 == '\0') break; 
            
            char c2 = str[i];
            if (c2 != '\0' && c2 != ' ') {
                i++; // Consume second char
                unsigned char byte_val = (hex_char_to_val(c1) << 4) | hex_char_to_val(c2);
                send_serial_char(byte_val);
            } else {
                // Single trailing char (e.g. typing "F" instead of "0F")
                send_serial_char(hex_char_to_val(c1)); 
            }
        }
    } else {
        // --- STANDARD TEXT SENDER ---
        for (int i = 0; str[i] != '\0'; i++) {
            if (str[i] == '\\' && str[i+1] == 'e') {
                send_serial_char(27); // Send actual ESC byte (0x1B)
                i++; // Skip the 'e'
            } else {
                send_serial_char(str[i]);
            }
        }
        if (setting_line_ending == 1) send_serial_char('\n');
        else if (setting_line_ending == 2) send_serial_char('\r');
        else if (setting_line_ending == 3) { send_serial_char('\r'); send_serial_char('\n'); }
    }
}

// --- Theme Engine ---
unsigned char make_color(unsigned char bg, unsigned char fg) { 
    return (bg << 4) | fg; 
}

unsigned char get_bg_color() {
    if (!setting_color) {
        unsigned char bg = (setting_bg_color == 7) ? 7 : 0;
        return make_color(bg, (bg == 7) ? 0 : 7);
    }
    return make_color((unsigned char)setting_bg_color, (unsigned char)setting_fg_color);
}

unsigned char get_hl_color() {
    if (!setting_color) return (setting_bg_color == 7) ? make_color(0, 7) : make_color(7, 0);
    return make_color((unsigned char)setting_sel_color, (setting_sel_color == 7) ? 0 : 15);
}

unsigned char get_cr_color() {
    if (!setting_color) return (setting_bg_color == 7) ? make_color(0, 7) : make_color(7, 0);
    return make_color((unsigned char)setting_crit_color, (setting_crit_color == 7) ? 0 : 15);
}

unsigned char decode_char(char ch) {
    if (setting_ansi_mode == 0) {
        switch(ch) {
            case '|': case '!': case '@': case '$': case '?': return '|';
            case '*': case '=': case '~': case '^': case '_': return '-';
            case '[': case ']': case '{': case '}': case '+': return '+';
            default: return (unsigned char)ch;
        }
    } 
    else {
        switch(ch) {
            case '|': return 179; case '*': return 196; case '[': return 218; 
            case ']': return 191; case '{': return 192; case '}': return 217; 
            case '^': return 194; case '_': return 193; case '@': return 195; 
            case '$': return 180; case '+': return 197; case '?': return 219;
            default: return (unsigned char)ch;
        }
    }
}

// --- VRAM Rendering ---
void set_char(int r, int c, char ch, unsigned char color) {
    unsigned char far* vram = (unsigned char far*)MK_FP(VIDEO_ADDRESS, 0x0000);
    int offset = (r * 80 + c) * 2;
    vram[offset] = ch; vram[offset + 1] = color;
}

void print_str(int r, int c, const char* str, unsigned char color) {
    int i = 0; while (str[i] != '\0' && (c + i) < 80) { 
        set_char(r, c + i, str[i], color); i++; 
    }
}

void print_mapped_str(int r, int c, const char* str, unsigned char color) {
    int i = 0; while (str[i] != '\0' && (c + i) < 80) { 
        set_char(r, c + i, decode_char(str[i]), color); i++; 
    }
}

void print_theme_line(int row, const char* label, int val, int color_enabled, unsigned char bg_color, unsigned char hl_color, int hover_fx) {
    char buf[80]; 
    char cb = setting_ansi_mode == 1 ? '\xFB' : 'V'; 
    char rb = setting_ansi_mode == 1 ? '\x07' : '*';
    sprintf(buf, "  %s - ", label); print_str(row, 2, buf, bg_color);
    
    unsigned char c_blk = (hover_fx && mouse_y == row && mouse_x >= 27 && mouse_x <= 35) ? hl_color : bg_color;
    sprintf(buf, "Black (%c)", val == 0 ? rb : ' '); print_str(row, 27, buf, c_blk); print_str(row, 36, ", ", bg_color);
    
    unsigned char c_wht = (hover_fx && mouse_y == row && mouse_x >= 38 && mouse_x <= 46) ? hl_color : bg_color;
    sprintf(buf, "White (%c)", val == 7 ? rb : ' '); print_str(row, 38, buf, c_wht);
    
    if (color_enabled) { 
        print_str(row, 47, ", ", bg_color);
        unsigned char c_blu = (hover_fx && mouse_y == row && mouse_x >= 49 && mouse_x <= 56) ? hl_color : bg_color;
        sprintf(buf, "Blue [%c]", (val != 0 && val != 7 && (val & 1)) ? cb : ' '); print_str(row, 49, buf, c_blu);
        
        print_str(row, 57, ", ", bg_color);
        unsigned char c_grn = (hover_fx && mouse_y == row && mouse_x >= 59 && mouse_x <= 67) ? hl_color : bg_color;
        sprintf(buf, "Green [%c]", (val != 0 && val != 7 && (val & 2)) ? cb : ' '); print_str(row, 59, buf, c_grn);
        
        print_str(row, 68, ", ", bg_color);
        unsigned char c_red = (hover_fx && mouse_y == row && mouse_x >= 70 && mouse_x <= 76) ? hl_color : bg_color;
        sprintf(buf, "Red [%c]", (val != 0 && val != 7 && (val & 4)) ? cb : ' '); print_str(row, 70, buf, c_red);
    }
}

void draw_scrollbar(int x, int y, int h, int visible_lines, int total_lines, int scroll_pos) {
    // 1. Calculate color attribute.
    unsigned char attr_track = (0 << 4) | 7;
    unsigned char attr_thumb = (0 << 4) | 7;
    unsigned char border_color = get_bg_color(); // Match the UI frame color!
    
   

    // 3. If everything fits on screen, draw empty track and return.
    if (total_lines <= visible_lines) {
        return; // Background is already cleared by Step 2, just exit!
    }
    else if (total_lines > visible_lines) {
         // 2. Clear background area first AND draw the left border
        for (int i = 0; i < h; i++) {
            set_char(y + i, x, ' ', attr_track);
            // Draw the vertical separator line at x - 1
            set_char(y + i, x - 1, decode_char('|'), border_color);  
        }
        
        // Connect the left border to the main UI frame T-junctions
        set_char(y - 1, x - 1, decode_char('^'), border_color); 
        set_char(y + h, x - 1, decode_char('_'), border_color); 
    }

    // 4. Calculate usable track height (total height minus arrows and horizontal lines).
    int usable_h = h - 4; 
    
    // 5. Calculate thumb height based on the ratio of visible space.
    long total = total_lines;
    long vis = visible_lines;
    int thumb_h = (int)(vis * usable_h / total);
    
    // Clamp thumb height (Min 1 character, max total usable height)
    if (thumb_h < 1) thumb_h = 1;
    if (thumb_h >= usable_h) thumb_h = usable_h - 1; 

    // 6. Calculate thumb position based on scroll_pos and available space.
    int max_scrolled_pos = total_lines - visible_lines;
    int max_thumb_pos = usable_h - thumb_h;
    int thumb_y = (int)((long)scroll_pos * max_thumb_pos / max_scrolled_pos);

    // 7. DRAW THE COMPONENTS
    // Draw Top Arrow & Line, and left and right t junctions

    if (setting_ansi_mode){ //left right t junction, not applicable on ansi mode
        set_char(y + 1, x-1, 195, attr_track);
        set_char(y + 1, x+1, 180, attr_track);
        set_char(y, x, 30, attr_track); //arrow
        set_char(y + 1, x, 196, attr_track);
    }
    else if(!setting_ansi_mode){
        set_char(y, x, '^', attr_track); //arrow
        set_char(y + 1, x, '-', attr_track);
    }
    
    // Draw Bottom Line & Arrow
    if (setting_ansi_mode){ //left right t junction, not applicable on ansi mode
        set_char(y + h - 2, x-1, 195, attr_track);
        set_char(y + h - 2, x+1, 180, attr_track);
        set_char(y + h - 2, x, 196, attr_track);
        set_char(y + h - 1, x, 31, attr_track); //arrow
    }
    else if(!setting_ansi_mode){
        set_char(y + h - 1, x, 'V', attr_track); //arrow
        set_char(y + h - 2, x, '-', attr_track);
    }

    // Draw Solid Thumb (219 character: █)
    for (int k = 0; k < thumb_h; k++) {
        // Position starts at y+2 (below top arrow AND line) plus calculated thumb_y.
        set_char(y + 2 + thumb_y + k, x, 219, attr_thumb);
    }
}

void draw_serial_output() {
    if (show_settings) return; // Don't draw over the settings menu!
    unsigned char bg = get_bg_color();

    for (int r = 3; r <= 21; r++) {
        int line_idx = scroll_y + (r - 3);
        
        if (line_idx < total_serial_lines) {
            // 1. Draw Line Number in the gutter (Cols 1-4)
            char lineno[6];
            sprintf(lineno, "%4d", line_idx + 1);
            print_str(r, 1, lineno, bg);
            
            // 2. Loop through and paint each character with its saved ANSI color!
            int len = serial_lengths[line_idx];
            for (int i = 0; i < len; i++) {
                set_char(r, 6 + i, serial_chars[line_idx][i], serial_colors[line_idx][i]);
            }
            
            // 3. ERASER LOOP: Blank out the rest of the line (Stop at 77 to protect the scrollbar)
            for (int c = 6 + len; c <= 77; c++) {
                set_char(r, c, ' ', bg);
            }
        } else {
            // ERASER LOOP: If the row is totally empty, blank out the gutter and text area
            for (int c = 1; c <= 4; c++) set_char(r, c, ' ', bg); 
            for (int c1 = 6; c1 <= 77; c1++) set_char(r, c1, ' ', bg);
        }
        
        // Draw the standard right-side border
        set_char(r, 79, decode_char('|'), bg);
    }
    
    // --- NEW: Draw the modern scrollbar at Column 78, Row 3, Height 19 ---
    draw_scrollbar(78, 3, 19, max_vis_lines, total_serial_lines, scroll_y);
}

void clear_area(int r1, int c1, int r2, int c2, unsigned char color) {
    for (int r = r1; r <= r2; r++) {
        for (int c = c1; c <= c2; c++) {
            set_char(r, c, ' ', color);
        }
    }
}

// --- Main UI Rendering ---
void updateStatus() {
    char buffer[80];
    const char* le_str = (setting_line_ending==0)?"None":(setting_line_ending==1)?"NL":(setting_line_ending==2)?"CR":"NL/CR";
    
    if (setting_hex_mode) {
        sprintf(buffer, "Dos Serial Monitor - Com %d, %ld Bps, %s [HEX MODE]", setting_com_port, setting_baud_rate, le_str);
    } else {
        sprintf(buffer, "Dos Serial Monitor - Com %d, %ld Bps, %s", setting_com_port, setting_baud_rate, le_str);
    }
    
    for (int i = 12; i <= 74; i++) set_char(1, i, ' ', get_bg_color());
    print_str(1, 13, buffer, get_bg_color());
}

// --- NEW: Helper to erase highlighted text ---
int delete_selection() {
    if (select_start == -1 || select_start == input_cursor) {
        select_start = -1;
        return 0; // Nothing was selected
    }
    int s_min = (input_cursor < select_start) ? input_cursor : select_start;
    int s_max = (input_cursor > select_start) ? input_cursor : select_start;
    int len = strlen(input_buffer);
    
    // Slide the memory left to crush the selected text!
    memmove(&input_buffer[s_min], &input_buffer[s_max], len - s_max + 1);
    
    input_cursor = s_min;
    select_start = -1;
    return 1; // Successfully deleted
}

void draw_input_area() {
    if (show_settings) return; 
    
    unsigned char bg = get_bg_color();
    unsigned char dim_color = (bg & 0xF0) | 0x08; 
    unsigned char hl_color = get_hl_color(); // The highlight color!
    
    // Classic DOS Color Inversion for the Cursor (Swap Top/Bottom 4 bits)
    unsigned char invert_color = ((bg & 0x0F) << 4) | ((bg & 0xF0) >> 4);
    
    // 1. Clear the old input text area with spaces
    for(int i = 10; i <= 71; i++) set_char(23, i, ' ', bg);
    
    int len = strlen(input_buffer);
    
    // 2. Print Placeholder OR Character-by-Character Highlighted Text
    if (len == 0 && pseudo_cursor_state == 0) {
        print_str(23, 10, "Type Here", dim_color);
    } else {
        for (int i = 0; i < len; i++) {
            unsigned char c_col = bg;
            
            // If this character falls inside the selection range, apply Highlight!
            if (select_start != -1) {
                int s_min = (input_cursor < select_start) ? input_cursor : select_start;
                int s_max = (input_cursor > select_start) ? input_cursor : select_start;
                if (i >= s_min && i < s_max) c_col = hl_color;
            }
            set_char(23, 10 + i, input_buffer[i], c_col);
        }
    }
    
    // 3. The Inverting Block Cursor
    if (pseudo_cursor_state) {
        if (input_cursor < len) set_char(23, 10 + input_cursor, input_buffer[input_cursor], invert_color);
        else set_char(23, 10 + input_cursor, ' ', invert_color);
    }
}

void drawUI() {
    unsigned char bg = get_bg_color();
    
    print_mapped_str(0, 0, "[**********^***************************************************************^***]", bg);
    print_mapped_str(1, 0, "| Settings |                                                               | X |", bg);
    print_mapped_str(2, 0, "@**********_***************************************************************_***$", bg);
    
    // Notepad's Secret: Explicitly overwrite the "empty" space with literal spaces
    for (int r = 3; r <= 21; r++) { 
        set_char(r, 0, decode_char('|'), bg); 
        for (int c = 1; c <= 78; c++) {
            set_char(r, c, ' ', bg); // This wipes any old settings text!
        }
        set_char(r, 79, decode_char('|'), bg); 
    }
    
    print_mapped_str(22, 0, "@*******^***************************************************************^******$", bg);
    print_mapped_str(23, 0, "| Clear | Type Here                                                     | Send |", bg);
    print_mapped_str(24, 0, "{*******_***************************************************************_******}", bg);
    updateStatus();
    draw_serial_output();
    draw_input_area();
}

void draw_settings() {
    int y; 
    unsigned char bg = get_bg_color(), hl = get_hl_color(), cr = get_cr_color(); 
    char buf[80]; char cb = setting_ansi_mode == 1 ? '\xFB' : 'V'; char rb = setting_ansi_mode == 1 ? '\x07' : '*';
    
    clear_area(0, 0, 24, 79, bg);
    
    print_mapped_str(0, 0, "[**********^***************************************************************^***]", bg); 
    print_mapped_str(1, 0, "| Settings |                                                               | X |", bg); 
    print_mapped_str(2, 0, "@********^*_*******^**********^*******^********************************^***_***$", bg); 
    if(!setting_ansi_mode){
        print_mapped_str(3, 0, "| Serial | Display | Graphics | Other |                                |< Back |", bg);
    }
    else {
        print_mapped_str(3, 0, "| Serial | Display | Graphics | Other |                                |\x1B Back |", bg);
    }
    
    if (current_settings_tab == 0) { // Serial Tab
        print_mapped_str(4, 0, "|        {*********_**********_*******_********************************_*******$", bg); 
        
        // --- 1. COM Port Column ---
        print_str(6, 2, "COM Port:", bg);
        for(int i = 1; i <= 2; i++) {
            int row = 6 + i;
            unsigned char c_col = (setting_hover_effects && mouse_y == row && mouse_x >= 4 && mouse_x <= 11) ? hl : bg;
            sprintf(buf, "(%c) COM%d", setting_com_port == i ? rb : ' ', i);
            print_str(row, 4, buf, c_col);
        }
        for(int i11 = 1; i11 <= 2; i11++) {
            int row = 6 + i11;
            unsigned char c_col = (setting_hover_effects && mouse_y == row && mouse_x >= 15 && mouse_x <= 22) ? hl : bg;
            sprintf(buf, "(%c) COM%d", setting_com_port == i11+2 ? rb : ' ', i11+2);
            print_str(row, 15, buf, c_col);
        }

        // --- 2. Baud Rate Column ---
        print_str(10, 2, "Baud Rate:", bg);
        long bauds[] = {9600, 19200, 38400, 57600, 115200};
        for(int i1 = 0; i1 < 5; i1++) {
            int row = 11 + i1;
            unsigned char c_col = (setting_hover_effects && mouse_y == row && mouse_x >= 4 && mouse_x <= 13) ? hl : bg;
            sprintf(buf, "(%c) %ld", setting_baud_rate == bauds[i1] ? rb : ' ', bauds[i1]);
            print_str(row, 4, buf, c_col);
        }

        // --- 3. Line Endings Column ---
        print_str(17, 2, "Line Endings:", bg);
        const char* les[] = {"None", "NL", "CR", "NL/CR"};
        for(int i2 = 0; i2 < 4; i2++) {
            int row = 18 + i2;
            unsigned char c_col = (setting_hover_effects && mouse_y == row && mouse_x >= 4 && mouse_x <= 12) ? hl : bg;
            sprintf(buf, "(%c) %s", setting_line_ending == i2 ? rb : ' ', les[i2]);
            print_str(row, 4, buf, c_col);
        }
        
    } else if (current_settings_tab == 1) { // Display Tab
        print_mapped_str(4, 0, "@********}         {**********_*******_********************************_*******$", bg);
        print_str(6, 2, "Reserved for future display settings.", bg);
        
    } else if (current_settings_tab == 2) { // Graphics Tab
        int disp_bg = setting_color ? setting_bg_color : ((setting_bg_color == 7) ? 7 : 0);
        int disp_fg = setting_color ? setting_fg_color : ((disp_bg == 7) ? 0 : 7);
        print_mapped_str(4, 0, "@********_*********}          {*******_********************************_*******$", bg);
        
        unsigned char ec_c = (setting_hover_effects && mouse_y == 10 && mouse_x >= 2 && mouse_x <= 20) ? hl : bg;
        sprintf(buf, "Enable Color [%c]", setting_color ? cb : ' '); 
        print_str(10, 2, buf, ec_c);
        
        print_str(6, 2, "Hover Effects:", bg); 
        unsigned char he_off_c = (setting_hover_effects && mouse_y == 7 && mouse_x >= 4 && mouse_x <= 20) ? hl : bg;
        unsigned char he_on_c  = (setting_hover_effects && mouse_y == 8 && mouse_x >= 4 && mouse_x <= 20) ? hl : bg;
        sprintf(buf, "(%c) Off", setting_hover_effects == 0 ? rb : ' '); print_str(7, 4, buf, he_off_c); 
        sprintf(buf, "(%c) On ", setting_hover_effects == 1 ? rb : ' '); print_str(8, 4, buf, he_on_c); 
                
        print_str(12, 2, "Ansi vs Unicode compatibility Mode:", bg); 
        unsigned char ansi0_c = (setting_hover_effects && mouse_y == 13 && mouse_x >= 4 && mouse_x <= 30) ? hl : bg;
        unsigned char ansi1_c = (setting_hover_effects && mouse_y == 14 && mouse_x >= 4 && mouse_x <= 30) ? hl : bg;
        sprintf(buf, "(%c) Standard Ansi Mode", setting_ansi_mode == 0 ? rb : ' '); 
        print_str(13, 4, buf, ansi0_c); 
        sprintf(buf, "(%c) Extended Ascii Mode", setting_ansi_mode == 1 ? rb : ' '); 
        print_str(14, 4, buf, ansi1_c);

        print_str(16, 2, "Theme Settings:", bg); 
        print_theme_line(17, "Background Color(BG)", disp_bg, setting_color, bg, hl, setting_hover_effects); 
        print_theme_line(18, "Foreground Color(FG)", disp_fg, setting_color, bg, hl, setting_hover_effects);
        if (setting_color) { 
            print_theme_line(19, "Selection  Color(BG)", setting_sel_color, 1, bg, hl, setting_hover_effects); 
            print_theme_line(20, "Critical   Color(BG)", setting_crit_color, 1, bg, hl, setting_hover_effects); 
        }
        
    } else { // Other Tab
        print_mapped_str(4, 0, "@********_*********_**********}       {********************************_*******$", bg);
        print_str(6, 2, "Hardware Debugging:", bg);
        
        unsigned char hex_c = (setting_hover_effects && mouse_y == 8 && mouse_x >= 4 && mouse_x <= 25) ? hl : bg;
        sprintf(buf, "[%c] Enable Hex Mode", setting_hex_mode ? cb : ' ', setting_hex_mode ? cb : ' '); 
        print_str(8, 4, buf, hex_c);
    }
    
    for (y = 5; y <= 23; y++) { set_char(y, 0, decode_char('|'), bg); set_char(y, 79, decode_char('|'), bg); } 
    print_mapped_str(24, 0, "{******************************************************************************}", bg);
    
    if (setting_hover_effects) {
        if (mouse_y == 1 && mouse_x >= 76 && mouse_x <= 78) print_str(1, 76, " X ", cr);
        if (mouse_y == 3 && mouse_x >= 1 && mouse_x <= 8) print_str(3, 1, " Serial ", hl); 
        if (mouse_y == 3 && mouse_x >= 10 && mouse_x <= 18) print_str(3, 10, " Display ", hl); 
        if (mouse_y == 3 && mouse_x >= 20 && mouse_x <= 29) print_str(3, 20, " Graphics ", hl); 
        if (mouse_y == 3 && mouse_x >= 31 && mouse_x <= 37) print_str(3, 31, " Other ", hl); 
        if (mouse_y == 3 && mouse_x >= 71 && mouse_x <= 77) print_str(3, 72, setting_ansi_mode ? "\x1B Back " : "< Back ", cr); 
    }
}

// --- Visual Updates ---
void draw_hovers() {
    if (show_settings) { 
        draw_settings(); 
        return; 
    }

    unsigned char bg = get_bg_color();
    unsigned char hl = get_hl_color();
    unsigned char cr = get_cr_color(); 

    // Reset Defaults
    print_str(1, 1, " Settings ", bg);
    print_str(1, 76, " X ", bg);
    print_str(23, 1, " Clear ", bg);
    print_str(23, 73, " Send ", bg);

    // --- NEW: Reset Scrollbar Arrows ---
    if (total_serial_lines > max_vis_lines) {
        unsigned char attr_track = (0 << 4) | 7;
        char top_arr = setting_ansi_mode ? 30 : '^';
        char bot_arr = setting_ansi_mode ? 31 : 'V';
        set_char(3, 78, top_arr, attr_track);
        set_char(21, 78, bot_arr, attr_track);
    }

    if (setting_hover_effects) {
        if (mouse_y == 1 && mouse_x >= 1 && mouse_x <= 10) print_str(1, 1, " Settings ", hl);
        else if (mouse_y == 1 && mouse_x >= 76 && mouse_x <= 78) print_str(1, 76, " X ", cr);
        else if (mouse_y == 23 && mouse_x >= 1 && mouse_x <= 7) print_str(23, 1, " Clear ", cr); // Red Highlight
        else if (mouse_y == 23 && mouse_x >= 73 && mouse_x <= 78) print_str(23, 73, " Send ", hl);
        
        // --- NEW: Highlight Scrollbar Arrows ---
        if (total_serial_lines > max_vis_lines) {
            char top_arr = setting_ansi_mode ? 30 : '^';
            char bot_arr = setting_ansi_mode ? 31 : 'V';
            if (mouse_x == 78 && mouse_y == 3) set_char(3, 78, top_arr, hl);
            else if (mouse_x == 78 && mouse_y == 21) set_char(21, 78, bot_arr, hl);
        }
    }
}

// --- Interaction Logic ---
void handle_mouse_click() {
    int state_changed = 0; // Track if we need to redraw

    if (show_settings) {
        if (mouse_y == 3) { // Tab row clicks
            if (mouse_x >= 1 && mouse_x <= 8) { 
                current_settings_tab = 0; 
                state_changed = 1; 
            }
            else if (mouse_x >= 10 && mouse_x <= 18) { 
                current_settings_tab = 1; 
                state_changed = 1; 
            }
            else if (mouse_x >= 20 && mouse_x <= 29) { 
                current_settings_tab = 2; 
                state_changed = 1; 
            }
            else if (mouse_x >= 31 && mouse_x <= 37) { 
                current_settings_tab = 3; 
                state_changed = 1; 
            }
            else if (mouse_x >= 71 && mouse_x <= 77) {
                show_settings = 0; // Go Back
                save_settings();   // NEW: Save to disk
                state_changed = 1;
            }
        } 
        else if (current_settings_tab == 0) { // Serial Tab
            // COM Port
            if (mouse_y == 7) {
                if (mouse_x >= 4 && mouse_x <= 11) setting_com_port = 1;
                else if (mouse_x >= 15 && mouse_x <= 22) setting_com_port = 3;
                state_changed = 1;
            }
            else if (mouse_y == 8) {
                if (mouse_x >= 4 && mouse_x <= 11) setting_com_port = 2;
                else if (mouse_x >= 15 && mouse_x <= 22) setting_com_port = 4;
                state_changed = 1;
            } 
            //baud rate
            else if (mouse_y == 11) {
                if (mouse_x >= 4 && mouse_x <= 13) setting_baud_rate = 9600;
                state_changed = 1;
            }
            else if (mouse_y == 12) {
                if (mouse_x >= 4 && mouse_x <= 13) setting_baud_rate = 19200;
                state_changed = 1;
            }
            else if (mouse_y == 13) {
                if (mouse_x >= 4 && mouse_x <= 13) setting_baud_rate = 38400;
                state_changed = 1;
            }
            else if (mouse_y == 14) {
                if (mouse_x >= 4 && mouse_x <= 13) setting_baud_rate = 57600;
                state_changed = 1;
            }
            else if (mouse_y == 15) {
                if (mouse_x >= 4 && mouse_x <= 13) setting_baud_rate = 115200;
                state_changed = 1;
            }
            // Line Endings
            else if (mouse_y == 18) {
                if (mouse_x >= 4 && mouse_x <= 12) setting_line_ending = 0;
                state_changed = 1;
            }
            else if (mouse_y == 19) {
                if (mouse_x >= 4 && mouse_x <= 12) setting_line_ending = 1;
                state_changed = 2;
            }
            else if (mouse_y == 20) {
                if (mouse_x >= 4 && mouse_x <= 12) setting_line_ending = 2;
                state_changed = 3;
            }
            else if (mouse_y == 21) {
                if (mouse_x >= 4 && mouse_x <= 12) setting_line_ending = 3;
                state_changed = 4;
            }
        } 
        else if (current_settings_tab == 1) { // Display Tab
            // Reserved for future
        } 
        else if (current_settings_tab == 2) { // Graphics Tab
            if (mouse_y == 10 && mouse_x >= 2 && mouse_x <= 20) {
                setting_color = !setting_color;
                if (!setting_color) { 
                    setting_bg_color = (setting_bg_color == 7) ? 7 : 0; 
                    setting_fg_color = (setting_bg_color == 7) ? 0 : 7; 
                }
                state_changed = 1;
            } 
            else if (mouse_y == 7 && mouse_x >= 4 && mouse_x <= 20) { setting_hover_effects = 0; state_changed = 1; }
            else if (mouse_y == 8 && mouse_x >= 4 && mouse_x <= 20) { setting_hover_effects = 1; state_changed = 1; }
            
            else if (mouse_y == 13 && mouse_x >= 4 && mouse_x <= 30) { setting_ansi_mode = 0; state_changed = 1; }
            else if (mouse_y == 14 && mouse_x >= 4 && mouse_x <= 30) { setting_ansi_mode = 1; state_changed = 1; }

            else if (mouse_y >= 17 && mouse_y <= 20) {//theme settings
                int* target = (mouse_y == 17) ? &setting_bg_color : ((mouse_y == 18) ? &setting_fg_color : ((mouse_y == 19) ? &setting_sel_color : &setting_crit_color));
                if (!(!setting_color && mouse_y >= 15)) { // Don't allow changing Selection/Critical if color is off
                    if (mouse_x >= 27 && mouse_x <= 35) { *target = 0; state_changed = 1; }
                    else if (mouse_x >= 38 && mouse_x <= 46) { *target = 7; state_changed = 1; }
                    else if (setting_color) {
                        if (mouse_x >= 49 && mouse_x <= 56) { if (*target == 0 || *target == 7) *target = 1; else *target ^= 1; state_changed = 1; }
                        else if (mouse_x >= 59 && mouse_x <= 67) { if (*target == 0 || *target == 7) *target = 2; else *target ^= 2; state_changed = 1; }
                        else if (mouse_x >= 70 && mouse_x <= 76) { if (*target == 0 || *target == 7) *target = 4; else *target ^= 4; state_changed = 1; }
                    }
                    if (setting_bg_color == setting_fg_color) {
                        if (mouse_y == 17) setting_fg_color = (setting_bg_color == 7) ? 0 : 7;
                        else if (mouse_y == 18) setting_bg_color = (setting_fg_color == 7) ? 0 : 7;
                    }
                }
            }
        }  
        else if (current_settings_tab == 3) { // Other Tab
            if (mouse_y == 8 && mouse_x >= 4 && mouse_x <= 25) { 
                setting_hex_mode = !setting_hex_mode; 
                state_changed = 1; 
            }
        }
        
        // Close App 'X' button inside Settings
        if (mouse_y == 1 && mouse_x >= 76 && mouse_x <= 78) { 
            show_settings = 0; 
            save_settings(); // NEW: Save to disk
            state_changed = 1; 
        }

    } 
    else {
        // Main UI Clicks
        if (mouse_y == 1 && mouse_x >= 76 && mouse_x <= 78) running = 0; 
        else if (mouse_y == 1 && mouse_x >= 1 && mouse_x <= 10) { 
            show_settings = 1; 
            state_changed = 1; 
            pseudo_cursor_state = 0; // Lose focus when opening settings
        }
        else if (mouse_y == 23) { 
            // 1. Clear Button Hitbox
            if (mouse_x >= 1 && mouse_x <= 7) {
                total_serial_lines = 0; scroll_y = 0; state_changed = 1; pseudo_cursor_state = 0; 
                current_rx_color = get_bg_color(); // Reset color parser on clear!
            }
            // 2. Text Box Hitbox
            else if (mouse_x >= 10 && mouse_x <= 71) { 
                int click_pos = mouse_x - 10; 
                if (click_pos < 0) click_pos = 0;
                if (click_pos > strlen(input_buffer)) click_pos = strlen(input_buffer);
                
                // Shift-Click selection support!
                int shift_pressed = bioskey(2) & 0x03;
                if (shift_pressed) {
                    if (select_start == -1) select_start = input_cursor; 
                } else {
                    select_start = -1; // Standard click drops the selection
                }
                
                input_cursor = click_pos;
                pseudo_cursor_state = 1; 
                draw_input_area(); 
            }
            // 3. Send Button Hitbox
            else if (mouse_x >= 73 && mouse_x <= 78) {
                if (input_buffer[0] != '\0') {
                    send_serial_string(input_buffer);
                    
                    char temp_line[74];
                    sprintf(temp_line, "-> %s", input_buffer);
                    
                    add_local_serial_line(temp_line, get_bg_color()); 
                    
                    input_buffer[0] = '\0';
                    input_cursor = 0;
                    state_changed = 1; 
                }
            }
        }
        // --- FIXED: Scrollbar Hitbox moved OUTSIDE of mouse_y == 23! ---
        else if (mouse_x == 78 && mouse_y >= 3 && mouse_y <= 21 && total_serial_lines > max_vis_lines) {
            int max_scroll = total_serial_lines - max_vis_lines;
            
            if (mouse_y == 3) { 
                // Top Arrow Click
                scroll_y -= 1;
                if (scroll_y < 0) scroll_y = 0;
                state_changed = 1;
            }
            else if (mouse_y == 21) { 
                // Bottom Arrow Click
                scroll_y += 1;
                if (scroll_y > max_scroll) scroll_y = max_scroll;
                state_changed = 1;
            }
            else if (mouse_y >= 5 && mouse_y <= 19) { 
                // 1. Calculate the exact size and position of the thumb
                int usable_h = 19 - 4; // 15 usable track rows
                int thumb_h = (int)(max_vis_lines * usable_h / total_serial_lines);
                
                if (thumb_h < 1) thumb_h = 1;
                if (thumb_h >= usable_h) thumb_h = usable_h - 1; 

                int max_thumb_pos = usable_h - thumb_h;
                int thumb_y = (int)((long)scroll_y * max_thumb_pos / max_scroll);
                
                // 2. Define the Thumb's hit-box on the screen
                int thumb_screen_top = 5 + thumb_y;
                int thumb_screen_bottom = 5 + thumb_y + thumb_h - 1;

                // 3. Check if the mouse is ON the thumb
                if (mouse_y >= thumb_screen_top && mouse_y <= thumb_screen_bottom) {
                    // User clicked the thumb. Do absolutely nothing!
                } 
                else {
                    // 4. User clicked the empty track. Execute the jump!
                    int relative_y = mouse_y - 5; 
                    int new_scroll = (relative_y * max_scroll) / (usable_h - 1); 
                    
                    if (new_scroll < 0) new_scroll = 0;
                    if (new_scroll > max_scroll) new_scroll = max_scroll;
                    
                    scroll_y = new_scroll;
                    state_changed = 1;
                }
            }
        }
        else if (mouse_y >= 3 && mouse_y <= 21) {
            // Clicked OUTSIDE the text box in the main output area (Lose Focus)
            if (pseudo_cursor_state == 1) {
                pseudo_cursor_state = 0; // Turn static cursor OFF
                draw_input_area();       // Instantly wipe the cursor
            }
        }
    }

    // We only redraw ONCE at the end, based on current state.
    if (state_changed) {
        if (show_settings) {
            draw_settings();
        } else {
            init_serial(); // Re-initialize hardware in case COM Port or Baud changed!
            drawUI();
        }
    }
}


void wait_vsync() {
    while ((inportb(0x3DA) & 0x08));
    while (!(inportb(0x3DA) & 0x08));
}

int main() {
    load_settings();
    textmode(C80);
    enable_bright_backgrounds();
    init_mouse();
    load_cursor_glyph();
    hide_mouse();
    clear_area(0, 0, 24, 79, 0x07);
    drawUI();
    hide_hardware_cursor();
    show_mouse();
    update_mouse();
    drawUI();

    int prev_mouse_left = 0, last_mouse_x = -1, last_mouse_y = -1;
    

    while (running) {
        update_mouse();

        // --- HARDWARE POLLING ---
        // Constantly check the COM port. If a line came in, force a UI redraw!
        if (read_serial()) {
            hide_mouse();
            draw_serial_output();
            show_mouse();
        }

        if (mouse_x != last_mouse_x || mouse_y != last_mouse_y) {
            hide_mouse();
            draw_hovers();
            show_mouse();
            last_mouse_x = mouse_x; last_mouse_y = mouse_y;
        }

        if (mouse_left && !prev_mouse_left) {
            hide_mouse();
            handle_mouse_click();
            show_mouse();
        }
        // --- NEW: MOUSE DRAG SELECTION ---
        else if (mouse_left && prev_mouse_left) {
            if (!show_settings && pseudo_cursor_state && mouse_y == 23) {
                int drag_pos = mouse_x - 10;
                int len = strlen(input_buffer);
                if (drag_pos < 0) drag_pos = 0;
                if (drag_pos > len) drag_pos = len;
                
                if (drag_pos != input_cursor) {
                    if (select_start == -1) select_start = input_cursor; // Anchor the selection
                    input_cursor = drag_pos;
                    hide_mouse(); draw_input_area(); show_mouse();
                }
            }
        }
        prev_mouse_left = mouse_left;

        // --- NEW HARDWARE KEYBOARD ENGINE ---
        if (bioskey(1)) { 
            int raw_key = bioskey(0); 
            unsigned char ascii = raw_key & 0xFF;       
            unsigned char scan_code = (raw_key >> 8);   
            int shift_pressed = bioskey(2) & 0x03; // NEW: Detect Left/Right Shift!
            
            if (ascii == 0) { 
                if (scan_code == 35 && !show_settings) { // Alt+H
                    setting_hex_mode = !setting_hex_mode;
                    save_settings(); hide_mouse(); drawUI(); show_mouse();
                }
                // --- NEW: SCROLLING KEYS ---
                else if (!show_settings && (scan_code == 72 || scan_code == 80 || scan_code == 73 || scan_code == 81)) {
                    int max_scroll = total_serial_lines - max_vis_lines;
                    if (max_scroll < 0) max_scroll = 0;
                    if (scan_code == 72) scroll_y -= 1;        // Up Arrow
                    else if (scan_code == 80) scroll_y += 1;   // Down Arrow
                    else if (scan_code == 73) scroll_y -= 15;  // Page Up
                    else if (scan_code == 81) scroll_y += 15;  // Page Down
                    if (scroll_y < 0) scroll_y = 0;
                    if (scroll_y > max_scroll) scroll_y = max_scroll;
                    hide_mouse(); draw_serial_output();
                    
                    show_mouse();
                }
                // ---------------------------
                else if (!show_settings && pseudo_cursor_state) {
                    int len = strlen(input_buffer);
                    
                    // --- NEW: Shift-Selection Anchor Logic ---
                    int is_nav_key = (scan_code == 75 || scan_code == 77 || scan_code == 71 || scan_code == 79);
                    if (is_nav_key) {
                        if (shift_pressed && select_start == -1) select_start = input_cursor;
                        else if (!shift_pressed) select_start = -1;
                    }

                    if (scan_code == 75 && input_cursor > 0) { input_cursor--; hide_mouse(); draw_input_area(); show_mouse(); } // Left
                    else if (scan_code == 77 && input_cursor < len) { input_cursor++; hide_mouse(); draw_input_area(); show_mouse(); } // Right
                    else if (scan_code == 71 && input_cursor > 0) { input_cursor = 0; hide_mouse(); draw_input_area(); show_mouse(); } // Home
                    else if (scan_code == 79 && input_cursor < len) { input_cursor = len; hide_mouse(); draw_input_area(); show_mouse(); } // End
                    else if (scan_code == 83 && input_cursor < len) { // Delete
                        if (!delete_selection()) { memmove(&input_buffer[input_cursor], &input_buffer[input_cursor + 1], len - input_cursor); }
                        hide_mouse(); draw_input_area(); show_mouse();
                    }
                }
            }
            else if (ascii == 27) { // Escape
                if (show_settings) { show_settings = 0; save_settings(); hide_mouse(); drawUI(); show_mouse(); }
                else running = 0; 
            }
            else if (!show_settings) {
                int len = strlen(input_buffer);
                
                if (ascii == 8) { // Backspace
                    if (!delete_selection() && input_cursor > 0) {
                        memmove(&input_buffer[input_cursor - 1], &input_buffer[input_cursor], len - input_cursor + 1);
                        input_cursor--;
                    }
                    pseudo_cursor_state = 1; hide_mouse(); draw_input_area(); show_mouse();
                }
                else if (ascii == 13 && len > 0) { // Enter
                    send_serial_string(input_buffer);
                    char temp_line[74]; sprintf(temp_line, "-> %s", input_buffer);
                    add_local_serial_line(temp_line, get_bg_color()); 
                    input_buffer[0] = '\0'; input_cursor = 0; select_start = -1;
                    pseudo_cursor_state = 1; hide_mouse(); draw_serial_output(); draw_input_area(); show_mouse();
                }
                else if (ascii >= 32 && ascii <= 126 && len < 61) { // Typing
                    delete_selection(); // Crush selected text before typing over it!
                    len = strlen(input_buffer); 
                    memmove(&input_buffer[input_cursor + 1], &input_buffer[input_cursor], len - input_cursor + 1);
                    input_buffer[input_cursor] = ascii; input_cursor++;
                    pseudo_cursor_state = 1; hide_mouse(); draw_input_area(); show_mouse();
                }
            }
        }
        wait_vsync(); 
    }
    hide_mouse();
    clear_area(0, 0, 24, 79, 0x07);
    show_hardware_cursor();
    return 0;
}
