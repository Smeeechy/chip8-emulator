#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <SDL.h>

// SDL container
typedef struct {
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_AudioSpec want, have;
  SDL_AudioDeviceID device;
} sdl_t;

// Emulator config container
typedef struct {
  uint32_t window_width;
  uint32_t window_height;
  uint32_t foreground_color;  // RGBA8888
  uint32_t background_color;  // RGBA8888
  uint32_t scale_factor;      // amount by which to scale up chip8 pixels
  uint32_t clock_speed;       // number of instructions to run per second
  uint32_t square_wave_freq;  // for audio playback
  uint32_t audio_sample_rate; // for audio playback
  int16_t volume;             // loudness of audio
} config_t;

// Program state
typedef enum {
  RUNNING, PAUSED, STOPPED
} emulator_state_t;

// Chip8 instruction format
typedef struct {
  uint16_t opcode;
  uint16_t NNN;     // 12 bit address or constant - _NNN
  uint8_t NN;       // 8 bit constant - __NN
  uint8_t N;        // 4 bit constant - ___N
  uint8_t X;        // 4 bit register identifier - _X__
  uint8_t Y;        // 4 bit register identifier - __Y_
} instruction_t;

// Chip8 machine object
typedef struct {
  emulator_state_t state;
  uint8_t ram[4096];
  bool display[64 * 32];  // keeps track of if each pixel should be on or off
  uint16_t stack[16];      // subroutine stack which typically handles 12 levels of nesting
  uint16_t *stack_ptr;     // pointer to next open stack location
  uint8_t V[16];          // data registers V0 - VF
  uint16_t I;             // index register
  uint16_t PC;            // program counter, abstraction for memory address that is being executed
  uint8_t delay_timer;    // decrements at 60hz if above zero
  uint8_t sound_timer;    // decrements at 60hz and plays tone while nonzero
  bool keypad[16];        // original keypad was 4x4 with keys 0 - F
  const char *rom_name;   // name of currently loaded ROM
  instruction_t inst;     // currently executing instruction
} chip8_t;

// Audio callback function
void audio_callback(void *userdata, uint8_t *stream, int len) {
  config_t *config = (config_t *) userdata;
  int16_t *data = (int16_t *) stream;
  static uint32_t running_sample_index = 0;
  const int32_t square_wave_period = config->audio_sample_rate / config->square_wave_freq;
  const int32_t half_square_wave_period = square_wave_period / 2;
  
  for (int i = 0; i < len / 2; i++) {
    data[i] = ((running_sample_index++ / half_square_wave_period) % 2) ? config->volume : -config->volume;
  }
}

bool set_config_from_args(config_t *config, int argc, char **argv) {
  // set defaults
  *config = (config_t) {
    .window_width = 64,
    .window_height = 32,
    .foreground_color = 0xFFFFFFFF, // white pixels
    .background_color = 0x000000FF, // black background
    .scale_factor = 20,             // window will have resolution of 1280x640 by default
    .clock_speed = 700,             // 700hz is a standard for running old 80s ROMs
    .square_wave_freq = 440,        // 440hz is middle A
    .audio_sample_rate = 44100,     // 44100 is CD quality audio
    .volume = 3000
  };
  
  // TODO: override from program arguments
  (void) argc;
  (void) argv;
  
  return true;
}

bool sdl_init(sdl_t *sdl, config_t *config) {
  uint32_t init_flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
  if (SDL_Init(init_flags) != 0) {
    SDL_Log("Unable to initialize SDL: %s\n", SDL_GetError());
    return false;
  }
  
  // creating window
  sdl->window = SDL_CreateWindow("Chip8 Emulator by Smeechy",  // title
                                 SDL_WINDOWPOS_CENTERED, // xpos
                                 SDL_WINDOWPOS_CENTERED, // ypos
                                 config->window_width * config->scale_factor,
                                 config->window_height * config->scale_factor,
                                 0);
  if (sdl->window == NULL) {
    SDL_Log("Unable to create window: %s\n", SDL_GetError());
    return false;
  }
  
  // creating renderer
  sdl->renderer = SDL_CreateRenderer(sdl->window, -1, 0);
  if (sdl->renderer == NULL) {
    SDL_Log("Unable to create renderer: %s\n", SDL_GetError());
    return false;
  }
  
  // creating audio spec
  sdl->want = (SDL_AudioSpec) {
    .freq = config->audio_sample_rate,
    .format = AUDIO_S16LSB, // signed 16bit little-endian
    .channels = 1,          // mono
    .samples = 512,
    .callback = audio_callback,
    .userdata = config
  };
  
  sdl->device = SDL_OpenAudioDevice(NULL, 0, &sdl->want, &sdl->have, 0);
  
  if (sdl->device == 0) {
    SDL_Log("Error creating audio device: %s\n", SDL_GetError());
    return false;
  }
  
  if ((sdl->want.format != sdl->have.format) || (sdl->want.channels != sdl->have.channels)) {
    SDL_Log("Unable to create desired audio spec\n");
    return false;
  }
  
  return true;
}

bool chip8_init(chip8_t *chip8, const char *rom_name) {
  const uint16_t entry_point = 0x200;  // chip8 ROMs load to 0x200
  const uint8_t font[80] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
    0x20, 0x60, 0x20, 0x20, 0x70, // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
    0x90, 0x90, 0xF0, 0x10, 0x10, // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
    0xF0, 0x10, 0x20, 0x40, 0x40, // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90, // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
    0xF0, 0x80, 0x80, 0x80, 0xF0, // C
    0xE0, 0x90, 0x90, 0x90, 0xE0, // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
    0xF0, 0x80, 0xF0, 0x80, 0x80  // F
  };
  
  // initialize empty chip8 machine
  memset(chip8, 0, sizeof(chip8_t));
  
  // load font
  memcpy(chip8->ram, font, sizeof(font)); // font is loaded into beginning of memory
  
  // open ROM file
  FILE *rom_file = fopen(rom_name, "rb");  // open ROM file in binary read mode
  if (rom_file == NULL) {
    SDL_Log("Unable to open ROM file: %s\n", rom_name);
    return false;
  }
  
  // check ROM size
  fseek(rom_file, 0, SEEK_END);
  const size_t rom_size = ftell(rom_file);
  const size_t max_size = sizeof chip8->ram - entry_point;
  if (rom_size > max_size) {
    SDL_Log("Rom file %s is too large. Size: %zu, Maximum: %zu\n", rom_name, rom_size, max_size);
    return false;
  }
  
  // load ROM into memory
  rewind(rom_file); // to reset file position indicator from fseek above
  if (fread(&chip8->ram[entry_point], rom_size, 1, rom_file) != 1) {
    SDL_Log("Failed to load ROM into memory.\n");
    return false;
  }
  
  // close ROM file
  fclose(rom_file);
  
  // default machine state
  chip8->state = RUNNING;
  chip8->stack_ptr = chip8->stack;
  chip8->PC = entry_point;
  chip8->rom_name = rom_name;
  
  return true;
}

void handle_input(chip8_t *chip8) {
  SDL_Event event = {0};
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_KEYUP:
        switch (event.key.keysym.sym) {
          case SDLK_1:
            chip8->keypad[0x1] = false;
            break;
  
          case SDLK_2:
            chip8->keypad[0x2] = false;
            break;
  
          case SDLK_3:
            chip8->keypad[0x3] = false;
            break;
  
          case SDLK_4:
            chip8->keypad[0xC] = false;
            break;
  
          case SDLK_q:
            chip8->keypad[0x4] = false;
            break;
  
          case SDLK_w:
            chip8->keypad[0x5] = false;
            break;
  
          case SDLK_e:
            chip8->keypad[0x6] = false;
            break;
  
          case SDLK_r:
            chip8->keypad[0xD] = false;
            break;
  
          case SDLK_a:
            chip8->keypad[0x7] = false;
            break;
  
          case SDLK_s:
            chip8->keypad[0x8] = false;
            break;
  
          case SDLK_d:
            chip8->keypad[0x9] = false;
            break;
  
          case SDLK_f:
            chip8->keypad[0xE] = false;
            break;
  
          case SDLK_z:
            chip8->keypad[0xA] = false;
            break;
  
          case SDLK_x:
            chip8->keypad[0x0] = false;
            break;
  
          case SDLK_c:
            chip8->keypad[0xB] = false;
            break;
  
          case SDLK_v:
            chip8->keypad[0xF] = false;
            break;
  
          default:
            break;
        }
        break;
      
      case SDL_KEYDOWN:
        switch(event.key.keysym.sym) {
          case SDLK_ESCAPE:
            chip8->state = STOPPED;
            return;
            
          case SDLK_SPACE:
            if (chip8->state == RUNNING) {
              chip8->state = PAUSED;
              SDL_Log("=== EMULATION PAUSED ===\n");
            } else {
              chip8->state = RUNNING;
              SDL_Log("=== EMULATION RESUMED ===\n");
            }
            return;
  
          case SDLK_1:
            chip8->keypad[0x1] = true;
            break;
  
          case SDLK_2:
            chip8->keypad[0x2] = true;
            break;
  
          case SDLK_3:
            chip8->keypad[0x3] = true;
            break;
  
          case SDLK_4:
            chip8->keypad[0xC] = true;
            break;
  
          case SDLK_q:
            chip8->keypad[0x4] = true;
            break;
  
          case SDLK_w:
            chip8->keypad[0x5] = true;
            break;
  
          case SDLK_e:
            chip8->keypad[0x6] = true;
            break;
  
          case SDLK_r:
            chip8->keypad[0xD] = true;
            break;
  
          case SDLK_a:
            chip8->keypad[0x7] = true;
            break;
  
          case SDLK_s:
            chip8->keypad[0x8] = true;
            break;
  
          case SDLK_d:
            chip8->keypad[0x9] = true;
            break;
  
          case SDLK_f:
            chip8->keypad[0xE] = true;
            break;
  
          case SDLK_z:
            chip8->keypad[0xA] = true;
            break;
  
          case SDLK_x:
            chip8->keypad[0x0] = true;
            break;
  
          case SDLK_c:
            chip8->keypad[0xB] = true;
            break;
  
          case SDLK_v:
            chip8->keypad[0xF] = true;
            break;
            
          default:
            break;
        }
        
      default:
        break;
    }
  }
}

void emulate_instruction(chip8_t *chip8, config_t config) {
  // fetch opcode and pre-increment program counter. opcodes are 16 bits so we need to combine ram[PC] and ram[PC + 1]
  chip8->inst.opcode = (chip8->ram[chip8->PC] << 8) | chip8->ram[chip8->PC + 1]; // they're also big endian
#ifdef DEBUG
  SDL_Log("Address: 0x%04X   Opcode: 0x%04X\n", chip8->PC, chip8->inst.opcode);
//  if (chip8->state == RUNNING) chip8->state = PAUSED; // enables single cycle step-through
#endif
  chip8->PC += 2;
  
  // decode opcode
  chip8->inst.NNN = chip8->inst.opcode & 0xFFF;
  chip8->inst.NN = chip8->inst.opcode & 0xFF;
  chip8->inst.N = chip8->inst.opcode & 0xF;
  chip8->inst.X = chip8->inst.opcode >> 8 & 0xF;
  chip8->inst.Y = chip8->inst.opcode >> 4 & 0xF;
  
  // execute opcode
  switch (chip8->inst.opcode >> 12) {
    case 0x0:
      switch (chip8->inst.NN) {
        case 0xE0: // 0x00E0 - clear the screen
          memset(chip8->display, false, sizeof(chip8->display));  // reset display array to all false
          break;
          
        case 0xEE: // 0x00EE - return from a subroutine
          chip8->PC = *--chip8->stack_ptr;  // change program counter to last address on stack and decrement stack pointer
          break;
  
        default: // 0x0NNN - jump to NNN
          chip8->PC = chip8->inst.NNN;
          break;
      }
      break;
    
    case 0x1: // 0x1NNN - jump to address NNN
      chip8->PC = chip8->inst.NNN;
      break;
    
    case 0x2: // 0x2NNN - call subroutine at NNN
      *chip8->stack_ptr++ = chip8->PC;  // push current program counter to stack and increment stack pointer
      chip8->PC = chip8->inst.NNN;  // change program counter to NNN
      break;
    
    case 0x3: // 0x3XNN - skip next instruction if VX == NN
      if (chip8->V[chip8->inst.X] == chip8->inst.NN)
        chip8->PC += 2;
      break;
    
    case 0x4: // 0x4XNN - skip next instruction if VX != NN
      if (chip8->V[chip8->inst.X] != chip8->inst.NN)
        chip8->PC += 2;
      break;
    
    case 0x5: // 0x5XY0 - skip next instruction if VX == VY
      if (chip8->V[chip8->inst.X] == chip8->V[chip8->inst.Y])
        chip8->PC += 2;
      break;
    
    case 0x6: // 0x6XNN - set VX to NN
      chip8->V[chip8->inst.X] = chip8->inst.NN;
      break;
    
    case 0x7: // 0x7XNN - add NN to VX
      chip8->V[chip8->inst.X] += chip8->inst.NN;
      break;
    
    case 0x8:
      switch (chip8->inst.N) {
        case 0x0: // 0x8XY0 - set VX to VY
          chip8->V[chip8->inst.X] = chip8->V[chip8->inst.Y];
          break;
          
        case 0x1: // 0x8XY1 - VX |= VY
          chip8->V[chip8->inst.X] |= chip8->V[chip8->inst.Y];
          break;
  
        case 0x2: // 0x8XY2 - VX &= VY
          chip8->V[chip8->inst.X] &= chip8->V[chip8->inst.Y];
          break;
  
        case 0x3: // 0x8XY3 - VX ^= VY
          chip8->V[chip8->inst.X] ^= chip8->V[chip8->inst.Y];
          break;
          
        case 0x4: // 0x8XY4 - add VY to VX. VF is set to 1 when there's a carry, and 0 when there isn't
          if ((uint16_t) (chip8->V[chip8->inst.X] + chip8->V[chip8->inst.Y]) > 0xFF)
            chip8->V[0xF] = 1;
          else
            chip8->V[0xF] = 0;
          chip8->V[chip8->inst.X] += chip8->V[chip8->inst.Y];
          break;
        
        case 0x5: // 0x8XY5 - subtract VY from VX. VF is set to 0 when there's a borrow, and 1 when there isn't
          if (chip8->V[chip8->inst.Y] > chip8->V[chip8->inst.X])
            chip8->V[0xF] = 0;
          else
            chip8->V[0xF] = 1;
          chip8->V[chip8->inst.X] = chip8->V[chip8->inst.X] - chip8->V[chip8->inst.Y];
          break;
          
        case 0x6: // 0x8XY6 - stores the least significant bit of VX in VF and then shifts VX to the right by 1
          chip8->V[0xF] = chip8->V[chip8->inst.X] & 1;
          chip8->V[chip8->inst.X] >>= 1;
          break;
        
        case 0x7: // 0x8XY7 - VX = VY - VX. VF is set to 0 when there's a borrow, and 1 when there is not
          if (chip8->V[chip8->inst.X] > chip8->V[chip8->inst.Y])
            chip8->V[0xF] = 0;
          else
            chip8->V[0xF] = 1;
          chip8->V[chip8->inst.X] = chip8->V[chip8->inst.Y] - chip8->V[chip8->inst.X];
          break;
        
        case 0xE: // 0x8XYE - stores the most significant bit of VX in VF and then shifts VX to the left by 1
          chip8->V[0xF] = chip8->V[chip8->inst.X] >> 7;
          chip8->V[chip8->inst.X] <<= 1;
          break;
          
        default:
          break;
      }
      break;
    
    case 0x9: // 0x9XY0 - skip next instruction if VX != VY
      if (chip8->V[chip8->inst.X] != chip8->V[chip8->inst.Y])
        chip8->PC += 2;
      break;
    
    case 0xA: // 0xANNN - set I register to NNN
      chip8->I = chip8->inst.NNN;
      break;
    
    case 0xB: // 0xBNNN - jump to address NNN + V0
      chip8->PC = chip8->inst.NNN + chip8->V[0x0];
      break;
    
    case 0xC: // 0xCXNN - set VX to NN & <a random int 0-255>
      chip8->V[chip8->inst.X] = (uint8_t) (chip8->inst.NN & SDL_GetTicks());
      break;
    
    case 0xD: { // 0xDXYN - draw N rows starting at (VX, VY) from memory location I
      uint8_t x = chip8->V[chip8->inst.X] % config.window_width;
      uint8_t y = chip8->V[chip8->inst.Y] % config.window_height;
      const uint8_t x0 = x; // for resetting x value before inner loop
      
      chip8->V[0xF] = 0; // carry flag initialized to 0
      for (uint16_t i = 0; i < chip8->inst.N; i++) {
        const uint8_t sprite_data = chip8->ram[chip8->I + i];
        x = x0; // reset x to beginning of row
        
        for (int8_t j = 7; j >= 0; j--) {
          bool *display_pixel = &chip8->display[y * config.window_width + x]; // current pixel state
          const bool sprite_pixel = sprite_data & (1 << j); // sprite pixel state
          if (*display_pixel && sprite_pixel)
            chip8->V[0xF] = 1; // pixel "collision" means set carry flag
          *display_pixel ^= sprite_pixel; // "collisions" unset the pixels
          if (++x >= config.window_width) break; // stop drawing row if reached the right edge of window
        }
        if (++y >= config.window_height) break; // stop drawing sprite if reached the bottom edge of window
      }
      break;
    }
    
    case 0xE:
      switch (chip8->inst.NN) {
        case 0x9E: // 0xEX9E - skip next instruction if key in VX is pressed
          if (chip8->keypad[chip8->V[chip8->inst.X]])
            chip8->PC += 2;
          break;
        
        case 0xA1: // 0xEXA1 - skip next instruction if key in VX is not pressed
          if (!chip8->keypad[chip8->V[chip8->inst.X]])
            chip8->PC += 2;
          break;
        
        default:
          break;
      }
      break;
    
    case 0xF:
      switch (chip8->inst.NN) {
        case 0x07: // 0xFX07 - set VX to the value of the delay timer
          chip8->V[chip8->inst.X] = chip8->delay_timer;
          break;
        
        case 0x0A: { // 0xFX0A - await a key press, then store it in VX
          bool waiting = true;
          for (int i = 0; i <= 0xF; i++) {
            if (chip8->keypad[i]) {
              chip8->V[chip8->inst.X] = i;
              waiting = false;
              break;
            }
          }
          
          // this is a way to continuously run this instruction while still allowing the timers to decrement properly
          if (waiting)
            chip8->PC -= 2;
          
          break;
        }
        
        case 0x15: // 0xFX15 - set delay timer to VX
          chip8->delay_timer = chip8->V[chip8->inst.X];
          break;
        
        case 0x18: // 0xFX18 - set sound timer to VX
          chip8->sound_timer = chip8->V[chip8->inst.X];
          break;
        
        case 0x1E: // 0xFX1E - add VX to I
          chip8->I += chip8->V[chip8->inst.X];
          break;
          
        case 0x29: // 0xFX29 - set I to the memory location of the sprite in VX
          chip8->I = 5 * (chip8->V[chip8->inst.X] & 0xF);
          break;
        
        case 0x33: { // 0xFX33 - store hundreds/tens/ones place of binary coded decimal value of VX in memory at I/I+1/I+2
          uint8_t value = chip8->V[chip8->inst.X];
          uint8_t ones, tens, hundreds;
          ones = value % 10;
          value /= 10;
          tens = value % 10;
          hundreds = value / 10;
          chip8->ram[chip8->I] = hundreds;
          chip8->ram[chip8->I + 1] = tens;
          chip8->ram[chip8->I + 2] = ones;
          break;
        }
        
        case 0x55: // 0xFX55 - store from V0 to VX inclusive in memory starting at I
          for (int offset = 0; offset <= chip8->inst.X; offset++)
            chip8->ram[chip8->I + offset] = chip8->V[offset];
          break;
        
        case 0x65: // 0xFX65 - load from V0 to VX inclusive from memory starting at I
          for (int offset = 0; offset <= chip8->inst.X; offset++)
            chip8->V[offset] = chip8->ram[chip8->I + offset];
          break;
        
        default:
          break;
      }
      break;
    
    default:
      break;
  }
}

void sdl_finish(const sdl_t sdl) {
  SDL_DestroyRenderer(sdl.renderer);
  SDL_DestroyWindow(sdl.window);
  SDL_CloseAudioDevice(sdl.device);
  SDL_Quit();  // ends all running sdl subsystems
}

bool clear_screen(const sdl_t sdl, const config_t config) {
  // sets draw color to config background color
  // consider changing the struct to just use a bit field?
  const uint8_t r = config.background_color >> 24 & 0xFF;
  const uint8_t g = config.background_color >> 16 & 0xFF;
  const uint8_t b = config.background_color >> 8 & 0xFF;
  const uint8_t a = config.background_color & 0xFF;
  
  if (SDL_SetRenderDrawColor(sdl.renderer, r, g, b, a) != 0) {
    SDL_Log("Unable to set renderer draw color: %s\n", SDL_GetError());
    return false;
  }
  
  // draws a blank background
  if (SDL_RenderClear(sdl.renderer) != 0) {
    SDL_Log("Error clearing background: %s\n", SDL_GetError());
    return false;
  }
  
  return true;
}

void update_screen(const sdl_t sdl, config_t config, chip8_t chip8) {
  SDL_Rect _rect = {0, 0, config.scale_factor, config.scale_factor};
  SDL_Rect *rect = &_rect;
  
  // prepare foreground color values
  const uint8_t fg_r = config.foreground_color >> 24 & 0xFF;
  const uint8_t fg_g = config.foreground_color >> 16 & 0xFF;
  const uint8_t fg_b = config.foreground_color >> 8 & 0xFF;
  const uint8_t fg_a = config.foreground_color & 0xFF;
  
  // prepare background color values
  const uint8_t bg_r = config.background_color >> 24 & 0xFF;
  const uint8_t bg_g = config.background_color >> 16 & 0xFF;
  const uint8_t bg_b = config.background_color >> 8 & 0xFF;
  const uint8_t bg_a = config.background_color & 0xFF;
  
  // loop through display and render the above rectangle for each enabled pixel
  for (uint32_t index = 0; index < sizeof(chip8.display); index++) {
    // translate index to 2d coordinates
    rect->x = (index % config.window_width) * config.scale_factor;
    rect->y = (index / config.window_width) * config.scale_factor;
    
    if (chip8.display[index])
      SDL_SetRenderDrawColor(sdl.renderer, fg_r, fg_g, fg_b, fg_a); // draw with foreground color
    else
      SDL_SetRenderDrawColor(sdl.renderer, bg_r, bg_g, bg_b, bg_a); // draw with background color
      
    SDL_RenderFillRect(sdl.renderer, rect);
  }
  SDL_RenderPresent(sdl.renderer);
}

void update_timers(const sdl_t sdl, chip8_t *chip8) {
  if (chip8->delay_timer)
    chip8->delay_timer--;
  
  if (chip8->sound_timer) {
    chip8->sound_timer--;
    SDL_PauseAudioDevice(sdl.device, 0);  // play a sound
  } else {
    SDL_PauseAudioDevice(sdl.device, 1);  // stop playing a sound
  }
}

int main(int argc, char **argv) {
  // print usage if invalid args
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <rom_file>\n", argv[0]);
    exit(EXIT_FAILURE);
  }
  
  // setup configuration
  config_t config = {0};
  if (!set_config_from_args(&config, argc, argv))
    exit(EXIT_FAILURE);
  
  // sdl initialization
  sdl_t sdl = {0};
  if (!sdl_init(&sdl, &config))
    exit(EXIT_FAILURE);
  
  // chip8 machine initialization
  chip8_t chip8 = {0};
  const char *rom_name = argv[1];
  if (!chip8_init(&chip8, rom_name))
    exit(EXIT_FAILURE);
  
  // initial screen clear
  if (!clear_screen(sdl, config))
    exit(EXIT_FAILURE);
  
  // main emulation loop
  while (chip8.state != STOPPED) {
    // handle user input
    handle_input(&chip8);
    
    // needs to be after handling input otherwise we could never change the program state
    if (chip8.state == PAUSED) continue;
    
    // start timer
    uint64_t frame_start = SDL_GetPerformanceCounter();
    
    // emulate chip8 instructions according to clock speed but also allowing screen updates at ~60hz
    for (uint8_t i = 0; i < config.clock_speed / 60; i++)
      emulate_instruction(&chip8, config);
    
    // end timer and calculate duration
    uint64_t frame_end = SDL_GetPerformanceCounter();
    double frame_duration = (double) ((frame_end - frame_start) * 1000) / SDL_GetPerformanceFrequency();

    // should approximate 60hz (1 frame per ~16.667ms)
    SDL_Delay(frame_duration >= 16.667f ? 0 : 16.667f - frame_duration);
    
    // update screen every ~60hz
    update_screen(sdl, config, chip8);
    
    // update timers every ~60hz
    update_timers(sdl, &chip8);
  }
  
  // sdl cleanup
  sdl_finish(sdl);
  
  exit(EXIT_SUCCESS);
}