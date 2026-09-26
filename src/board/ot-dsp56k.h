/*
 * The Octatrack's DSP56721: two DSP56300 cores on one chip, and the host-port
 * semantics both of this repo's DSP consumers need.
 *
 * ONE source of truth. octdsp (src/dsp-main.cc) drives the block
 * protocol itself with no QEMU; the QEMU shim (ot-dsp-shim.cc) lets the guest
 * drive it. Everything below is identical for both and every line of it was
 * paid for once:
 *
 *   - The two cores are ONE chip: 0x30000-0x3FFFF aliases across P/X/Y of both
 *     cores (one shm object, six mmaps). Core B is booted by core A through it.
 *   - y:$ffffd3-d7 is the inter-core mailbox. Core 0 publishes the bank index
 *     (0/1) once per block — that IS the block clock — then a 2. Core 1's
 *     transfers must be delivered in the SECOND phase or core 1 dies at
 *     P:0xfff000, deterministically.
 *   - ICR 0x81 is a per-transfer FIFO reset: drain THEN clear, or the previous
 *     transfer's tail is destroyed.
 *   - A command needs two barriers: the vector internalised before any further
 *     word enters the port, and the handler's arguments consumed (occupancy
 *     back to its pre-argument level, NOT drain-to-empty).
 *   - macOS/JIT: size the JIT entry table to all of P up front, and PeriphY
 *     reads must return nullptr from readAsPtr or the polls JIT-cache forever.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OT_DSP56K_H
#define OT_DSP56K_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/dspBootCode.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

namespace ot {

using dsp56k::TWord;

constexpr TWord kMemSizeP = 0x40000, kMemSizeXY = 0x40000;
constexpr TWord kIccBase = 0x30000, kIccWords = kMemSizeP - kIccBase;
constexpr unsigned kCyclesPerSlot = 566;   /* 200 MHz / (44100 * 8 TDM slots) */
constexpr unsigned kSlots = 8, kIns = 4, kFrames = 16;
constexpr unsigned kFifoDepth = 1024;      /* the HDI24's ~1024-word RX FIFO  */
constexpr unsigned kRate = 44100;

/* The parked wait states. Neither is idle: instruction retirement IS the
 * passage of peripheral time here, so the codec's spin at the block-top DSR2
 * poll is what clocks the ESAI. Skipping it advances the audio timeline. */
constexpr TWord kPollLo = 0x4a, kPollHi = 0x54;   /* codec block-top DSR2 poll */
inline bool atBlockPoll(TWord pc) { return pc >= kPollLo && pc < kPollHi; }
inline bool atMailboxWait(TWord pc) { return pc == 0x57 || pc == 0x8d; }

/* ---- the inter-core mailbox at y:$ffffd3-d7 ------------------------------ */
struct Icc {
    std::atomic<TWord> word[2] {{0}, {0}};
    std::atomic<bool> full[2] {{false}, {false}};
    std::atomic<bool> alive[2] {{true}, {true}};
    std::atomic<uint64_t> renders {0};    /* core 0 published a bank index */
    std::atomic<uint64_t> phase2 {0};     /* ...and its trailing 2         */

    void send(unsigned from, TWord w);
    bool rxFull(unsigned to) const
    {
        return full[to ^ 1].load(std::memory_order_acquire);
    }
    /* A retired peer never takes the word; do not wedge the live core. */
    bool txPending(unsigned from);
    TWord receive(unsigned to);
};
extern Icc g_icc;

struct Core;
/* DIAG hooks, null unless a tracer installs them (the shim's
 * OCTA_HANDOFF_TRACE). g_execHook runs after every exec with the PC the exec
 * started at; g_iccHook on every mailbox post (send) and take (!send). */
extern void (*g_execHook)(Core &c, TWord pcBefore);
extern void (*g_iccHook)(unsigned core, bool send, TWord w);

class PeriphY final : public dsp56k::IPeripherals {
public:
    PeriphY() : IPeripherals(dsp56k::PeripheralType::PeripheralsNop) {}
    void attach(unsigned i) { m_index = i; }
    uint32_t exec() noexcept { return MaxDelayCycles; }

private:
    static constexpr TWord kFlag = 1u << 1;

    TWord read(TWord addr, dsp56k::Instruction) override;
    /* nullptr: both payloads poll these in branch-to-self loops; a pointer
     * would let the JIT cache the read and never call read() again. */
    const TWord *readAsPtr(TWord, dsp56k::Instruction) override
    {
        return nullptr;
    }
    void write(TWord addr, TWord v) override;
    void reset() override;
    void setSymbols(dsp56k::Disassembler &) const override {}
    void terminate() override {}

    unsigned m_index = 0;
    TWord m_mem[0x80] = {0};
};

/* ---- one core ------------------------------------------------------------ */
struct Core {
    /* Fill kIns input slots for one ESAI frame / take kSlots output slots. */
    using RxFn = std::function<void(int32_t *)>;
    using TxFn = std::function<void(const int32_t *)>;

    unsigned index = 0;
    dsp56k::Peripherals56362 periphX;
    PeriphY periphY;
    std::unique_ptr<dsp56k::Memory> mem;
    std::unique_ptr<dsp56k::DSP> dsp;
    std::unique_ptr<dsp56k::DspBoot> boot;
    bool booted = false, dead = false;
    uint64_t rxFrames = 0, txFrames = 0, execs = 0;
    RxFn onRx;
    TxFn onTx;

    void init(unsigned i, RxFn rx, TxFn tx);
    dsp56k::HDI08 &hdi() { return periphX.getHDI08(); }
    TWord pc() const { return dsp->getPC().toWord(); }
    bool runnable() const { return booted && !dead; }

    /* True while the JIT has an entry for the current PC; false means the core
     * has walked into unmapped P and must be retired rather than run. */
    bool jitOk() const;
    /* Step n JIT blocks. Returns false once the core is dead. */
    bool step(unsigned n);
    /* Advance one ESAI slot without executing the poll through the JIT. */
    void fastForwardSlot();

    /* Is a DSP-side DMA channel armed to feed the host port (DRS 17)? When
     * nothing is armed a host read returns the stale latch, as silicon does. */
    bool hostTxArmed();
    /* Disarm a hungry host-RECEIVE channel (DRS 16): it would eat command
     * argument words ahead of the handler. */
    void disarmHostRx();
};

/* ---- host-port primitives ------------------------------------------------
 * The two callers step the DSP very differently (the standalone tool runs it
 * inline and unlocked; the QEMU shim runs it on the vCPU under the execution
 * lock), so they hand in their own quantum. The SEMANTICS are shared, and it
 * is the semantics that were expensive to find. */
using StepFn = std::function<void()>;

/* ICR 0x81: drain THEN clear. Clearing first destroys the previous
 * transfer's tail. Returns false if the FIFO never emptied. */
bool portIcrReset(Core &c, const StepFn &stepper);

/* One host command. argOcc is the FIFO occupancy recorded before the
 * argument words went in — barrier 2 waits for the handler to consume back
 * down to it, NOT for the FIFO to drain empty. Returns false if either
 * barrier timed out. */
bool portCommand(Core &c, TWord cvr, size_t argOcc, const StepFn &stepper);

/* Both cores plus the shared window. */
struct Chip {
    Core core[2];
    bool open();                                   /* the 0x30000 shm object */
    void step(unsigned n);                         /* every runnable core    */
    Core &operator[](unsigned i) { return core[i & 1]; }
};

} // namespace ot

#endif
