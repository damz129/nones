#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "arena.h"

#ifndef DISABLE_APU
#include "apu.h"
#endif

#include "cart.h"
#include "system.h"
#include "mapper.h"
#include "ppu.h"
#include "utils.h"

static System *system_ptr = NULL;

System *SystemCreate(Arena *arena)
{
    System *system = ArenaPush(arena, sizeof(System));
    system->cart = ArenaPush(arena, sizeof(Cart));
    system->cpu = ArenaPush(arena, sizeof(Cpu));
    system->apu = ArenaPush(arena, sizeof(Apu));
    system->ppu = ArenaPush(arena, sizeof(Ppu));
    system->joy_pad1 = ArenaPush(arena, sizeof(JoyPad));
    system->joy_pad2 = ArenaPush(arena, sizeof(JoyPad));

    system_ptr = system;
    return system;
}

int SystemLoadCart(Arena *arena, System *system, const char *path)
{
    return CartLoad(arena, system->cart, path);
}

void SystemInit(System *system, Arena *arena, bool ppu_warmup, bool swap_duty_cycles,
                int sample_rate, uint32_t **buffers, const uint32_t buffer_size)
{
    PPU_Init(system->ppu, system->cart->arrangement, ppu_warmup, buffers, buffer_size);
#ifndef DISABLE_APU
    APU_Init(system->apu, arena, swap_duty_cycles, sample_rate);
#endif
    CPU_Init(system->cpu);
}

uint8_t SystemReadOpenBus(void)
{
    return system_ptr->bus_data;
}

static bool ApuRegsActivated(System *system)
{
    return system->cpu_addr >= 0x4000 && system->cpu_addr < 0x4020;
}

static bool ExplicitAbortDmcDma(System *system)
{
#ifdef DISABLE_APU
    (void)system;
    return true;
#else
    return !system->apu->status.dmc;
#endif
}

static void SystemStartOamDma(System *system, const uint8_t page_num)
{
    system->oam_dma_triggered = false;
    uint16_t base_addr = (page_num * 0x100);
    // Add cpu halt cycle
    SystemTick();
    BusRead(system->cpu_addr);

    bool single_dma_cycle = false;
    system->oam_dma_bytes_remaining = 256;
    while (system->oam_dma_bytes_remaining > 0)
    {
        // OAM Alignment cycle if needed
        if (system->cpu->cycles & 1)
        {
            SystemTick();
            BusRead(system->cpu_addr);
        }

        SystemTick();
        // OAM DMA uses Ppu reg $2004 (OAM_DATA) internally
        const uint8_t data = BusRead(base_addr++);
        SystemTick();
        // Put
        BusWrite(OAM_DATA_REG, data);
        
        if (system->dmc_dma_triggered && system->oam_dma_bytes_remaining > 2)
        {
            SystemTick();
#ifndef DISABLE_APU
            ApuDmcDmaUpdate(system->apu);
#endif
            system->dmc_dma_triggered = false;
        }
        else if (system->dmc_dma_triggered && system->oam_dma_bytes_remaining == 2)
        {
            single_dma_cycle = true;
        }
        --system->oam_dma_bytes_remaining;
    }

    if (system->dmc_dma_triggered && !single_dma_cycle)
    {
        // DMC DMA dummy cycle
        SystemTick();
        BusRead(system->cpu_addr);

        // DMC Dma Alignment cycle if needed
        if (system->cpu->cycles & 1)
        {
            SystemTick();
            BusRead(system->cpu_addr);
        }

        SystemTick();
#ifndef DISABLE_APU
        ApuDmcDmaUpdate(system->apu);
#endif
        system->dmc_dma_triggered = false;
    }
    else if (system->dmc_dma_triggered && single_dma_cycle)
    {
        SystemTick();
#ifndef DISABLE_APU
        ApuDmcDmaUpdate(system->apu);
#endif
        system->dmc_dma_triggered = false;
    }
}

static void SystemStartDmcDma(System *system)
{
    // Add cpu halt cycle
    SystemTick();
    BusRead(system->cpu_addr);

    if (ExplicitAbortDmcDma(system))
    {
        system->dmc_dma_triggered = false;
        return;
    }

    // Add cpu dummy cycle
    SystemTick();
    BusRead(system->cpu_addr);

    // Alignment cycle if needed
    if (system->cpu->cycles & 1)
    {
        SystemTick();
        BusRead(system->cpu_addr);
    }

    SystemTick();
#ifndef DISABLE_APU
    ApuDmcDmaUpdate(system->apu);
#endif
    system->dmc_dma_triggered = false;
}

void SystemSignalDmcDma(void)
{
    system_ptr->dma_pending = true;
    system_ptr->dmc_dma_triggered = true;
}

static void SystemHandleDMA(System *system)
{
    if (!system->dma_pending)
        return;

    if (system->dmc_dma_triggered && !system->oam_dma_triggered)
    {
        SystemStartDmcDma(system);
    }
    else if (system->oam_dma_triggered)
    {
        SystemStartOamDma(system, system->bus_data);
    }

    system->dma_pending = false;
}

uint8_t SystemRead(const uint16_t addr)
{
    system_ptr->cpu_addr = addr;
    SystemHandleDMA(system_ptr);
    SystemTick();
    return BusRead(addr);
}

uint8_t BusRead(const uint16_t addr)
{
    System *system = system_ptr;
    ++system->cpu->cycles;

    if (ApuRegsActivated(system))
    {
        uint8_t val = addr & 0x1F;
        if (val == 0x15)
        {
#ifdef DISABLE_APU
            return 0x00;
#else
            return ApuReadStatus(system->apu, system->bus_data);
#endif
        }
        else if (val == 0x16)
        {
            // Clear bits 0–4
            system->bus_data &= 0xE0;
            // Update bits 0–4 
            system->bus_data |= (ReadJoyPadReg(system->joy_pad1) & 0x1F);
            return system->bus_data;
        }
        else if (val == 0x17)
        {
            // Clear bits 0–4
            system->bus_data &= 0xE0;
#ifdef DISABLE_APU
            return system->bus_data;
#else
            // Update bits 0–4
            system->bus_data |= (ReadJoyPadReg(system->joy_pad2) & 0x1F);
            return system->bus_data;
#endif
        }
    }

    system->bus_data = system->cart->MemMapReadFn[addr](system->cart, addr);
    // Finally read the data from the bus
    return system->bus_data;
}

void SystemWrite(const uint16_t addr, const uint8_t data)
{
    system_ptr->cpu_addr = addr;
    SystemTick();
    BusWrite(addr, data);
}

void BusWrite(const uint16_t addr, const uint8_t data)
{
    System *system = system_ptr;
    ++system->cpu->cycles;

    system->cart->MemMapWriteFn[addr](system->cart, addr, data);
    system->bus_data = data;
}

System *SystemGetPtr(void) { return system_ptr; }
Cart *SystemGetCart(void) { return system_ptr->cart; }
Apu *SystemGetApu(void) { return system_ptr->apu; }
Ppu *SystemGetPpu(void) { return system_ptr->ppu; }
Cpu *SystemGetCpu(void) { return system_ptr->cpu; }
uint8_t SystemGetPpuA9(void) { return system_ptr->ppu->v.raw_bits.bit9; }

uint8_t PpuBusReadChrRom(const uint16_t addr)
{
    return MapperReadChrRom(system_ptr->cart, addr);
}

void PpuBusWriteChrRam(const uint16_t addr, const uint8_t data)
{
    Cart *cart = system_ptr->cart;
    if (!cart->chr_rom.ram)
        return;

    MapperWriteChrRam(cart, addr, data);
}

void MapperClockAudioTimers(void)
{
#ifndef DISABLE_APU
    if (system_ptr->cart->mapper_num == MAPPER_MMC5)
    {
        Mmc5ClockAudioTimers();
    }
#endif
}

void MapperClockAudio(void)
{
#ifndef DISABLE_APU
    if (system_ptr->cart->mapper_num == MAPPER_MMC5)
    {
        Mmc5ClockAudio();
    }
#endif
}

float MapperGetMixedAudio(void)
{
#ifdef DISABLE_APU
    return 0.0f;
#else
    if (system_ptr->cart->mapper_num != MAPPER_MMC5)
        return 0;

    return Mmc5GetMixedAudio();
#endif
}

void PpuClockMMC3(void)
{
    if (system_ptr->cart->mapper_num != MAPPER_MMC3)
        return;

    Mmc3ClockIrqCounter(system_ptr->cart);
}

uint8_t ExtNameTableRead(Ppu *ppu, const uint16_t addr)
{
    if (system_ptr->cart->mapper_num != MAPPER_MMC5)
        return PpuNametableRead(ppu, addr);

    return Mmc5ReadNameTable(ppu, addr);
}

void SystemUpdateState(System *system, SystemState state)
{
    if (system->state == PAUSED && state == PAUSED)
        system->state ^= PAUSED;
    else
    {
        system->state = state;
    }
}

void SystemRun(System *system, bool debug_info)
{
    if (system->state == PAUSED)
        return;

    system->ppu->frame_finished = false;

    do {
        CPU_ExecuteInstr(system->cpu, debug_info);
    } while (!system->ppu->frame_finished && system->state != STEP_INSTR);

    if ((system->state == STEP_FRAME && system->ppu->frame_finished) || system->state == STEP_INSTR)
    {
        system->state = PAUSED;
    }
}

bool SystemPollAllIrqs(void)
{
#ifdef DISABLE_APU
    return PollMapperIrq();
#else
    return PollApuIrqs(system_ptr->apu) || PollMapperIrq();
#endif
}

static inline uint8_t SystemReadNmiPin(System *system)
{
    return (system->ppu->ctrl.vblank_nmi & system->ppu->status.vblank);
}

static void SystemPollNmi(System *system)
{
    const uint8_t current_nmi_pin = SystemReadNmiPin(system);
    system->cpu->nmi_pending |= (current_nmi_pin & ~system->cpu->nmi_pin);
    system->cpu->nmi_pin = current_nmi_pin;
}

void SystemTick(void)
{
#ifndef DISABLE_APU
    APU_Tick(system_ptr->apu, system_ptr->cpu->cycles & 1);
#endif
    PPU_Tick(system_ptr->ppu);
    SystemPollNmi(system_ptr);
    PPU_Tick(system_ptr->ppu);
    PPU_Tick(system_ptr->ppu);
    PpuScheduleRendererUpdate(system_ptr->ppu);
}

void SystemAddCpuCycles(uint32_t cycles)
{
    system_ptr->cpu->cycles += cycles;
}

void SystemUpdateJPButtons(System *system, const bool *buttons)
{
    JoyPadSetButton(system->joy_pad1, JOYPAD_A, buttons[0]);
    JoyPadSetButton(system->joy_pad1, JOYPAD_B, buttons[1]);
    JoyPadSetButton(system->joy_pad1, JOYPAD_UP, buttons[2]);
    JoyPadSetButton(system->joy_pad1, JOYPAD_DOWN, buttons[3]);
    JoyPadSetButton(system->joy_pad1, JOYPAD_LEFT, buttons[4]);
    JoyPadSetButton(system->joy_pad1, JOYPAD_RIGHT, buttons[5]);
    JoyPadSetButton(system->joy_pad1, JOYPAD_START, buttons[6]);
    JoyPadSetButton(system->joy_pad1, JOYPAD_SELECT, buttons[7]);

    JoyPadSetButton(system->joy_pad2, JOYPAD_A, buttons[8]);
    JoyPadSetButton(system->joy_pad2, JOYPAD_B, buttons[9]);
    JoyPadSetButton(system->joy_pad2, JOYPAD_UP, buttons[10]);
    JoyPadSetButton(system->joy_pad2, JOYPAD_DOWN, buttons[11]);
    JoyPadSetButton(system->joy_pad2, JOYPAD_LEFT, buttons[12]);
    JoyPadSetButton(system->joy_pad2, JOYPAD_RIGHT, buttons[13]);
    JoyPadSetButton(system->joy_pad2, JOYPAD_START, buttons[14]);
    JoyPadSetButton(system->joy_pad2, JOYPAD_SELECT, buttons[15]);
}

void SystemReset(System *system)
{
    MapperReset(system->cart);
#ifndef DISABLE_APU
    APU_Reset(system->apu);
#endif
    PPU_Reset(system->ppu);
    CPU_Reset(system->cpu);
}

void SystemShutdown(System *system)
{
#ifndef DISABLE_APU
    APU_Shutdown(system->apu);
#endif
    CartSaveSram(system->cart);
}
