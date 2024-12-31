#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <cstdint>
#include <fcntl.h>
#include "mman.h"
#include <unistd.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "minigb_new2.c"

#define OPCREL(_) opcrel = (opcode - _) / 8

#define OP4_NX8(_,X) case _: case _ + 8*X: case _ + 16*X: case _ + 24*X:

#define OP4_NX16_REL(_) OP4_NX8(_, 2) opcrel = (opcode - _) / 16;

#define OP5_FLAG(_, always)                                                    \
  OP4_NX8(_, 1)                                                                \
  case always:                                                                 \
    OPCREL(_), carry = opcode == always || !(F & F_mask[opcrel]) ^ opcrel & 1;

#define OP8_REL(_)                                                             \
  case _ ... _ + 7:                                                            \
    tmp8 = reg8_access(0, 0, opcrel = opcode);

#define OP8_NX8_REL(_)                                                         \
  OP4_NX8(_, 1) OP4_NX8(_ + 32, 1) tmp8 = reg8_access(0, 0, OPCREL(_));

#define OP64_REL(_)                                                            \
  case _ ... _ + 55: OP8_REL(_ + 56) OPCREL(_);

#define OP9_IMM_PTR(_)                                                         \
  OP8_REL(_) case _ + 70 : operand = opcode & 64 ? mem8(PC++) : tmp8;

// cart mbc / memory info
uint8_t CART_MBC[] =
{
    0, 1, 1, 1,0, 2, 2,0, 0, 0,0, 0, 0, 0,0, 3,
    3, 3, 3, 3,0,0,0,0,0, 5, 5, 5, 5, 5, 5, 0
};

uint8_t CART_RAM[] =
{
    0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0,
    1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0
};

uint16_t NUM_ROM_BANKS[] =
{
    2, 4, 8,16,32,64,128, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,72,80,96, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

uint8_t  NUM_RAM_BANKS[] = { 0, 1, 1, 4, 16 };


uint8_t opcode, opcrel, tmp8, operand, carry, neg, *rom0, *rom1, io[512],
video_ram[8192], work_ram[16384], extram[32*1024], *extrambank,
reg8[] = { 19, 0, 216, 0, 77, 1, 176, 1 }, & F = reg8[6], & A = reg8[7],
* reg8_group[] = { reg8 + 1, reg8,     reg8 + 3, reg8 + 2,
                 reg8 + 5, reg8 + 4, &F,       &A },
    & IF = io[271], & LCDC = io[320], & LY = io[324], IME, halt,

    //add 1231
    CurROMBank=1, mbc1mode =0, ysize=8,
   &LCDS = io[321], &LYC=io[325], &TIMA=io[260],&TMA=io[261],&TMC=io[262],
    gb_mbc = 0,gb_cram=0, cram_mode, cram_bank, cram_banks,cram_enable=0, cgb_enable=0;
    ;
//타이머를 위한 변수
/*
#define TIMA 0xFF05
#define TMA 0xFF06
#define TMC 0xFF07 */

uint16_t m_TimerCounter;

uint16_t clock[]= {1024,16,64,256};

uint16_t SetClockFreq(){
  return clock[TMC & 3];
}

uint8_t const *key_state;

uint16_t PC = 256, *reg16 = (uint16_t *)reg8, &HL = reg16[2], SP = 65534,
         &DIV = (uint16_t &)io[259], ppu_dot = 32,
         *reg16_group1[] = {reg16, reg16 + 1, &HL, &SP},
         *reg16_group2[] = {reg16, reg16 + 1, &HL, &HL}, prev_cycles, cycles, cart_ram_bank_offset=0,u16temp,
      //add 1206
    rom_bank,rom_banks ;

int tmp, tmp2, F_mask[] = {128, 128, 16, 16}, frame_buffer[23040],
               palette[] = {-1, -23197,   -65536,    -1 << 24,
                            -1, -8092417, -12961132, -1 << 24};

void tick() { cycles += 4; }

//12.31 MBC1을 추가할 예정
//BasicBoy의 소스 코드를 참조함

uint8_t mem8(uint16_t addr = HL, uint8_t val = 0, int write = 0) {
  tick();
  switch (addr >> 13) {
  
    case 1: //0x2000-0x3FFF
          if(write ) {
	rom1 = rom0 + ( (val ? val & 63 : 1) << 14) ; //mbc3 or maybe mbc1 ok?? 2^14=16*1024=16,384bytes
	CurROMBank = val;
          }
   case 0: //0x0000-0x1FFF
     
     return rom0[addr];

    case 2: //0x4000-0x5FFF
           if(write && val<=3) {
	if(gb_mbc==3 || mbc1mode) extrambank=extram+(val<<13); //mbc3
                 else if(gb_mbc==1) {
		CurROMBank = 127 & (CurROMBank + val * 32);
		rom1 = rom0 + CurROMBank * 16384;
	 }
           }
        
    case 3: //0x6000-0x7FFF
            if(write) mbc1mode = val & 1;
           return rom1[addr&16383];
     
    case 4: //0x8000-0x9FFF
      addr &= 8191;
      if (write)
        video_ram[addr] = val;
      return video_ram[addr];

    case 5: //0xA000-0xBFFF
     addr &= 8191;
    if(write) extrambank[addr] = val;
    return extrambank[addr];

    
    case 7: //0xF000 -0xFFFF
      

    if ((addr >= 0xFF10) && (addr <= 0xFF3F)) {
      if(write) audio_write(addr, val);     
      else     return audio_read(addr);
     }

      if (addr >= 65024) {
        if (write) {
          if (addr == 65350)
            for (int y = 160; --y >= 0;)
              io[y] = mem8(val << 8 | y);
          io[addr & 511] = val;
        }

        if (addr == 65280) {
          if (~io[256] & 16)
            return ~(16 + key_state[SDL_SCANCODE_DOWN] * 8 +
                     key_state[SDL_SCANCODE_UP] * 4 +
                     key_state[SDL_SCANCODE_LEFT] * 2 +
                     key_state[SDL_SCANCODE_RIGHT]);
          if (~io[256] & 32)
            return ~(32 + key_state[SDL_SCANCODE_RETURN] * 8 +
                     key_state[SDL_SCANCODE_TAB] * 4 +
                     key_state[SDL_SCANCODE_Z] * 2 +
                     key_state[SDL_SCANCODE_X]);
          return 255;
        }
        return io[addr & 511];
      }

    case 6:
      addr &= 16383;
      if (write)
        work_ram[addr] = val;
      return work_ram[addr];
  }
}


void set_flags(uint8_t mask, int Z, int N, int H, int C) {
  F = F & mask | !Z << 7 | N << 6 | H << 5 | C << 4;
}

uint16_t read16(uint16_t &addr = PC) {
  tmp8 = mem8(addr++);
  return mem8(addr++) << 8 | tmp8;
}

void push(uint16_t val) {
  mem8(--SP, val >> 8, 1);
  mem8(--SP, val, 1);
  tick();
}

uint8_t reg8_access(uint8_t val, int write = 1, uint8_t o = opcrel) {
  return (o &= 7) == 6 ? mem8(HL, val, write)
         : write       ? *reg8_group[o] = val
                       : *reg8_group[o];
}

uint8_t get_color(int tile, int y_offset, int x_offset) {
  uint8_t *tile_data = &video_ram[tile * 16 + y_offset * 2];
  return (tile_data[1] >> x_offset) % 2 * 2 + (*tile_data >> x_offset) % 2;
}


int main() {
  //SDL_RWread(SDL_RWFromFile("PokemonRed.gb","rb"), rom0, 1024*1024,1);
 //   SDL_RWread(SDL_RWFromFile("zelda.gb", "rb"), rom0, 1024 * 1024, 1);

/* 
rom1 = (rom0 = (uint8_t *)mmap(0, 1<<20, PROT_READ, MAP_SHARED,
                                 open("PokemonRed.gb", O_RDONLY), 0)) +	  16384; //32768;
*/


 rom1 = (rom0 = (uint8_t *)mmap(0, 512*1024, PROT_READ, MAP_SHARED,
                                 open("zelda.gb", O_RDONLY), 0)) +	  32768;

/*
rom1 = (rom0 = (uint8_t *)mmap(0, 32*1024, PROT_READ, MAP_SHARED,
                                 open("tetris.gb", O_RDONLY), 0)) +	  16384; //32768;
*/

 extrambank = extram;
  gb_mbc = rom0[327]; //0x147=>327
  gb_mbc = CART_MBC[rom0[327]];
  gb_cram = CART_RAM[rom0[327]];
  rom_banks = NUM_ROM_BANKS[rom0[328]]; //0x148=>328
  cram_banks = NUM_RAM_BANKS[rom0[329]];//0x149=>329
  cgb_enable = (rom0[0x143] == 0xC0 || rom0[0x143] == 0x80);
 
  printf("MBC is %d \n", gb_mbc);

  LCDC = 145;
  DIV = 44032;

 SDL_setenv("SDL_AUDIODRIVER", "directsound", SDL_TRUE);

 /* Initialise frontend implementation, in this case, SDL2. */
	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) < 0)
	{
		char buf[128];
		SDL_snprintf(buf, sizeof(buf),
				"Unable to initialise SDL2: %s", SDL_GetError());
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", buf, NULL);
		exit(EXIT_FAILURE);
	}

    SDL_Renderer *renderer = SDL_CreateRenderer(
      SDL_CreateWindow("2024MinGameboy", 10, 30, 800, 600, SDL_WINDOW_SHOWN), -1,
      SDL_RENDERER_PRESENTVSYNC);

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"1");

  SDL_Texture *texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, 160, 144);
  key_state = SDL_GetKeyboardState(0);


       SDL_AudioDeviceID dev;
       SDL_AudioSpec want, have;
		want.freq = AUDIO_SAMPLE_RATE;
		want.format   = AUDIO_S16,
		want.channels = 2;
		want.samples = AUDIO_SAMPLES;
		want.callback = audio_callback;
		want.userdata = NULL;
        dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		audio_init();
		SDL_PauseAudioDevice(dev, 0);

  while (1) {
    prev_cycles = cycles;
    if (IME & IF & io[511]) {
      IF = halt = IME = 0;
      cycles += 8;
      push(PC);
      PC = 64;
    } else if (halt)
      tick();
    else
      switch (opcode = mem8(PC++)) {
      OP4_NX16_REL(1) // LD r16, u16
        *reg16_group1[opcrel] = read16();
      case 0: // NOP
        break;

      //1205 OPCODE Add 
      case 0x08: // 8 LD (imm), SP
          u16temp = mem8(PC++) | mem8(PC++) << 8;
          mem8(u16temp++, SP & 0xFF, 1);
          mem8(u16temp, SP >> 8, 1);
          cycles += 20;
          break;
      case 0x10: //16 STOP
          halt = 1;
          break;
      case 0xe8: //232 ADD SP, r8
          tmp = mem8(PC++);
          set_flags(128, 1, 0, SP % 4096 + tmp % 4096 > 4095, SP + tmp > 65535); //set_flags(mask, Z,N,H,C);
          SP += tmp;
          cycles += 16;
          break;
      case 0xc7: //199 rst $00
          push(PC);
          PC = 0x00;
          cycles += 12;
          break;
      case 0xd7: //215 rst $10
          push(PC);
          PC = 0x10;//16
          cycles += 12;
          break;
      case 0xe7: //231 rst $20
          push(PC);
          PC = 0x20;//32
          cycles += 12;
          break;
      case 0xf7: //247 rst $30
          push(PC);
          PC = 0x30;//48
          cycles += 12;
          break;
      case 0xcf: //207 rst $08
          push(PC);
          PC = 0x08;//8
          cycles += 12;
          break;
      case 0xdf: //223 rst $18
          push(PC);
          PC = 0x18;//24
          cycles += 12;
          break;
      case 0xef: //239 rst $28
          push(PC);
          PC = 0x28;//40
          cycles += 12;
          break;
      case 0xff: //255 rst $38
          push(PC);
          PC = 0x38;//56
          cycles += 12;
          break;
      //
      //  


      OP4_NX16_REL(10) // LD A, (r16)
      OP4_NX16_REL(2) // LD (r16), A
        tmp = opcode & 8;
        reg8_access(mem8(*reg16_group2[opcrel], A, !tmp), tmp, 7);
        HL += opcrel < 2 ? 0 : 5 - 2 * opcrel;
        break;

      OP4_NX16_REL(11) // DEC r16
      OP4_NX16_REL(3)  // INC r16
        *reg16_group1[opcrel] += opcode & 8 ? -1 : 1;
        tick();
        break;

      OP8_NX8_REL(5) // DEC r8 / DEC (HL)
      OP8_NX8_REL(4) // INC r8 / INC (HL)
        neg = opcode & 1;
        reg8_access(tmp8 += 1 - neg * 2);
        set_flags(16, tmp8, neg, !(tmp8 + neg & 15), 0);
        break;

      OP8_NX8_REL(6) // LD r8, u8 / LD (HL), u8
        reg8_access(mem8(PC++));
        break;

      OP4_NX16_REL(9) // ADD HL, r16
        tmp = *reg16_group1[opcrel];
        set_flags(128, 1, 0, HL % 4096 + tmp % 4096 > 4095, HL + tmp > 65535);
        HL += tmp;
        tick();
        break; 

      OP4_NX8(7,1)
        neg = 1;
        goto ROTATE;

      OP5_FLAG(32, 24) // JR i8 / JR <condition>, i8
        tmp8 = mem8(PC++);
        if (carry)
          PC += (int8_t)tmp8, tick();
        break;

      case 39: // DAA
        carry = tmp8 = 0;
        if (F & 32 || ~F & 64 && A % 16 > 9)
          tmp8 = 6;
        if (F & 16 || ~F & 64 && A > 153)
          tmp8 |= 96, carry = 1;
        set_flags(65, A += F & 64 ? -tmp8 : tmp8, 0, 0, carry);
        break;

      case 47: // CPL
        A = ~A;
        set_flags(144, 1, 1, 1, 0);
        break;

      case 55: case 63: // SCF / CCF
        set_flags(128, 1, 0, 0, opcode & 8 ? !(F & 16) : 1);
        break;

      OP64_REL(64) // LD r8, r8 / LD r8, (HL) / LD (HL), r8 / HALT
        opcode == 118 ? halt = 1 : reg8_access(tmp8);
        break;

      OP9_IMM_PTR(128) // ADD A, r8 / ADD A, (HL) / ADD A, u8
        neg = carry = 0;
        goto ALU;

      OP9_IMM_PTR(136) // ADC A, r8 / ADC A, (HL) / ADC A, u8
        neg = 0;
        carry = F / 16 % 2;
        goto ALU;

      OP9_IMM_PTR(184) // CP A, r8 / CP A, (HL) / CP A, u8
        goto SUB;
      OP9_IMM_PTR(144) // SUB A, r8 / SUB A, (HL) / SUB A, u8
      SUB:
        carry = 1;
        goto SUBTRACT;

      OP9_IMM_PTR(152) // SBC A, r8 / SBC A, (HL) / SBC A, u8
        carry = !(F / 16 % 2);
      SUBTRACT:
        neg = 1;
        operand = ~operand;
      ALU:
        set_flags(0, tmp8 = A + operand + carry, neg,
                  (A % 16 + operand % 16 + carry > 15) ^ neg,
                  (A + operand + carry > 255) ^ neg);
        if (~(opcode / 8) & 7)
          A = tmp8;
        break;

      OP9_IMM_PTR(160) // AND A, r8 / AND A, (HL) / AND A, u8
        set_flags(0, A &= operand, 0, 1, 0);
        break;

      OP9_IMM_PTR(168) // XOR A, r8 / XOR A, (HL) / XOR A, u8
        set_flags(0, A ^= operand, 0, 0, 0);
        break;

      OP9_IMM_PTR(176) // OR A, r8 / OR A, (HL) / OR A, u8
        set_flags(0, A |= operand, 0, 0, 0);
        break;

      case 217: // RETI
        carry = IME = 1;
        goto RET;

      OP5_FLAG(192, 201) // RET / RET <condition>
      RET:
        tick();
        if (carry)
          PC = read16(SP);
        break;

      OP4_NX16_REL(193) // POP r16
        reg16[opcrel] = read16(SP);
        break;

      OP5_FLAG(194, 195) // JP u16 / JP <condition>, u16
        goto CALL;
      OP5_FLAG(196, 205) // CALL u16 / CALL <condition>, u16
      CALL:
        tmp = read16();
        if (carry)
          opcode & 4 ? push(PC) : tick(), PC = tmp;
        break;

      OP4_NX16_REL(197) // PUSH r16
        push(reg16[opcrel]);
        break;

      case 203:
        neg = 0;
        opcode = mem8(PC++);
        ROTATE:
        switch (opcode) {
          OP8_REL(0) // RLC r8 / RLC (HL)
          OP8_REL(16) // RL r8 / RL (HL)
          OP8_REL(32) // SLA r8 / SLA (HL)
            carry = tmp8 >> 7;
            tmp8 += tmp8 + (opcode & 16 ? F / 16 % 2 : opcode & 32 ? 0 : carry);
            goto CARRY_ZERO_FLAGS_U;

          OP8_REL(48) // SWAP r8 / SWAP (HL)
            carry = 0;
            tmp8 = tmp8 * 16 + tmp8 / 16;
            goto CARRY_ZERO_FLAGS_U;

          OP8_REL(8) // RRC r8 / RRC (HL)
          OP8_REL(24) // RR r8 / RR (HL)
          OP8_REL(40) // SRA r8 / SRA (HL)
          OP8_REL(56) // SRL r8 / SRL (HL)
            carry = tmp8 & 1;
            tmp8 = (opcode & 48) == 32
                       ? (int8_t)tmp8 >> 1
                       : tmp8 / 2 + (opcode & 32   ? 0
                                     : opcode & 16 ? (F * 8 & 128)
                                                   : carry * 128);
          CARRY_ZERO_FLAGS_U:
            reg8_access(tmp8);
            set_flags(0, neg || tmp8, 0, 0, carry);
            break;

          OP64_REL(64) // BIT bit, r8 / BIT bit, (HL)
            set_flags(16, tmp8 & 1 << opcrel, 0, 1, 0);
            break;

          OP64_REL(128) // RES bit, r8 / RES bit, (HL)
            reg8_access(tmp8 & ~(1 << opcrel),1,opcode);
            break;

          OP64_REL(192) // SET bit, r8 / SET bit, (HL)
            reg8_access(tmp8 | 1 << opcrel,1,opcode);
        }
        break;

      case 224: case 226: case 234:
      case 240: case 242: case 250:
        // LD A, (FF00 + u8) / LD A, (FF00 + C) / LD A, (u16)
        // LD (FF00 + u8), A / LD (FF00 + C), A / LD (u16), A
        tmp = opcode & 16;
        reg8_access(mem8(opcode & 8
                             ? read16()
                             : 65280 + (opcode & 2 ? *reg8 : mem8(PC++)),
                         A, !tmp),
                    tmp, 7);
        break;

      case 233: // JP HL
        PC = HL;
        break;

      case 243: case 251: // DI / EI
        IME = opcode != 243;
        break;

      case 248: // LD HL, SP + i8
        HL = SP + (int8_t)(tmp8 = mem8(PC++));
        set_flags(0, 1, 0, SP % 16 + tmp8 % 16 > 15, (uint8_t)SP + tmp8 > 255);
        tick();
        break;

      case 249: // LD SP, HL
        SP = HL;
        tick();
      }

    for (DIV += cycles - prev_cycles; prev_cycles++ != cycles;)
      if (LCDC & 128) {
        if (++ppu_dot == 456) {
          if (LY < 144)
            for (tmp = 160; --tmp >= 0;) {
              uint8_t is_window =
                          LCDC & 32 && LY >= io[330] && tmp >= io[331] - 7,
                      x_offset = is_window ? tmp - io[331] + 7 : tmp + io[323],
                      y_offset = is_window ? LY - io[330] : LY + io[322];
              uint16_t
                  palette_index = 0,
                  tile = video_ram[(LCDC & (is_window ? 64 : 8) ? 7 : 6) << 10 |
                                   y_offset / 8 * 32 + x_offset / 8],
                  color = get_color(LCDC & 16 ? tile : 256 + (int8_t)tile,
                                    y_offset & 7, 7 - x_offset & 7);

             //8X16 크기가 가능하도록 수정함. 12.31

              if (LCDC & 2)
               
                for (uint8_t *sprite = io; sprite < io + 160; sprite += 4) {
	  ysize = 8;
                 	  if(LCDC&4) ysize=16;
                 
                  uint8_t sprite_x = tmp - sprite[1] + 8,
                          sprite_y = LY - *sprite + 16,
                          sprite_color = get_color(
                              sprite[2], sprite_y ^ (sprite[3] & 64 ? ysize-1 : 0),
                              sprite_x ^ (sprite[3] & 32 ? 0 : 7));
                  if (sprite_x < 8 && sprite_y < ysize &&
                      !(sprite[3] & 128 && color) && sprite_color) {
                    color = sprite_color;
                    palette_index = 1 + !!(sprite[3] & 16);
                    break;
                  }
                }
              


              frame_buffer[LY * 160 + tmp] =
                  palette[(io[327 + palette_index] >> 2 * color) % 4 +
                              palette_index * 4 &
                          7];
            }

          if (LY == 143) {
            IF |= 1;
            SDL_UpdateTexture(texture, 0, frame_buffer, 640);
            SDL_RenderCopy(renderer, texture, 0, 0);
            SDL_RenderPresent(renderer);
            SDL_Event event;
            while (SDL_PollEvent(&event))
              if (event.type == SDL_QUIT)
                return 0;
          }

          LY = (LY + 1) % 154;
          ppu_dot = 0;
        }
      } else
        LY = ppu_dot = 0;
  }
}
