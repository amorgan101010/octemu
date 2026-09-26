/* See ot-dsp56k.h. SPDX-License-Identifier: MIT */
#include "ot-dsp56k.h"

#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace dsp56k;

namespace ot {

Icc g_icc;
void (*g_execHook)(Core &, TWord) = nullptr;
void (*g_iccHook)(unsigned, bool, TWord) = nullptr;

void Icc::send(unsigned from, TWord w)
{
    word[from].store(w, std::memory_order_release);
    full[from].store(true, std::memory_order_release);
    if (from == 0) {
        if (w < 2) renders++;
        else if (w == 2) phase2++;
    }
}

bool Icc::txPending(unsigned from)
{
    if (full[from].load(std::memory_order_acquire) && !alive[from ^ 1].load())
        full[from].store(false);
    return full[from].load(std::memory_order_acquire);
}

TWord Icc::receive(unsigned to)
{
    const unsigned from = to ^ 1;
    const TWord w = word[from].load(std::memory_order_acquire);
    full[from].store(false, std::memory_order_release);
    return w;
}

TWord PeriphY::read(TWord addr, Instruction)
{
    switch (addr) {
    case 0xffffd3: return g_icc.rxFull(m_index) ? kFlag : 0;
    case 0xffffd4: {
        const TWord w = g_icc.receive(m_index);
        if (g_iccHook)
            g_iccHook(m_index, false, w);
        return w;
    }
    case 0xffffd6: return g_icc.txPending(m_index) ? kFlag : 0;
    default:       return m_mem[addr & 0x7F];
    }
}

void PeriphY::write(TWord addr, TWord v)
{
    if (addr == 0xffffd7) {
        g_icc.send(m_index, v & 0xFFFFFF);
        if (g_iccHook)
            g_iccHook(m_index, true, v & 0xFFFFFF);
        return;
    }
    m_mem[addr & 0x7F] = v & 0xFFFFFF;
}

void PeriphY::reset() { memset(m_mem, 0, sizeof m_mem); }

namespace {

class Validator final : public IMemoryValidator {
public:
    bool memValidateAccess(EMemArea, TWord, bool) const override { return true; }
};
Validator g_validator;

int g_iccFd = -1;

/* The DSP56721's eight shared 8Kx24 blocks, mapped into all six views. */
bool iccMap(Memory &m)
{
    for (EMemArea a : {MemArea_P, MemArea_X, MemArea_Y}) {
        TWord *base = m.getMemAreaPtr(a);
        if (!base)
            return false;
        void *t = base + kIccBase;
        if (mmap(t, (size_t)kIccWords * sizeof(TWord), PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FIXED, g_iccFd, 0) != t)
            return false;
    }
    return true;
}

} // namespace

void Core::init(unsigned i, RxFn rx, TxFn tx)
{
    index = i;
    onRx = std::move(rx);
    onTx = std::move(tx);
    periphY.attach(i);
    mem.reset(new Memory(g_validator, kMemSizeP, kMemSizeXY, kIccBase));
    if (!iccMap(*mem)) {
        fprintf(stderr, "ot-dsp56k: shared window mmap failed\n");
        abort();
    }
    /* ☠ Power-on RAM is NOT zero, and the payload relies on that: the shared
     * FX mixer (P:$1847) multiplies every wet output by zero while the state
     * word at slot+$82 is zero, and only DARK REV ever writes that word —
     * PLATE's wet is silenced forever on zero-filled memory and unmuted by
     * whatever junk (or DARK leftovers) the word holds on silicon. Measured:
     * 15 emulator dumps with PLATE warmed all hold X:$6282 = 0; a DARK-then-
     * PLATE walk (DARK leaves $800000 there) renders PLATE's first wet tail.
     * A small fixed nonzero fill reproduces silicon's "junk is nonzero"
     * deterministically. P stays zero: jitOk() reads unmapped P as absence. */
    for (EMemArea a : {MemArea_X, MemArea_Y}) {
        TWord *base = mem->getMemAreaPtr(a);
        for (TWord w = 0; w < kIccBase; w++)
            base[w] = 0x010101;
    }
    dsp.reset(new DSP(*mem, &periphX, &periphY));
    /* Size the JIT entry table to all of P before anything runs: on macOS it
     * is a plain vector and execJit indexes it without a bounds check. */
    dsp->getJit().notifyProgramMemWrite(kMemSizeP - 1);

    auto &h = hdi();
    h.setTransmitDataAlwaysEmpty(false);       /* HTDE reflects the latch */
    h.setRXRateLimit(2);

    auto &esai = periphX.getEsai();
    esai.setReadRxCallback([this](uint64_t &, Audio::RxFrame &f) {
        f.resize(kSlots);
        for (uint32_t s = 0; s < f.size(); s++)
            f[s].fill(0);
        int32_t in[kIns] = {0};
        if (onRx)
            onRx(in);
        for (unsigned s = 0; s < kIns; s++)
            f[s][0] = TWord(in[s] & 0xFFFFFF);
        rxFrames++;
    });
    esai.setWriteTxCallback([this](uint64_t &, const Audio::TxFrame &f) {
        int32_t slots[kSlots] = {0};
        for (uint32_t s = 0; s < f.size() && s < kSlots; s++)
            slots[s] = int32_t(f[s][0] << 8) >> 8;
        txFrames++;
        if (onTx)
            onTx(slots);
    });
    periphX.getEsaiClock().setCyclesPerSample(kCyclesPerSlot);
    boot.reset(new DspBoot(*dsp));
}

bool Core::jitOk() const
{
    const TJitFunc *e = dsp->getJitEntries();
    const TWord p = dsp->getPC().toWord();
    return e && p < kMemSizeP && e[p] != nullptr;
}

bool Core::step(unsigned n)
{
    if (!runnable())
        return false;
    execs += n;
    for (unsigned i = 0; i < n; i++) {
        if (!jitOk()) {
            dead = true;
            g_icc.alive[index] = false;
            fprintf(stderr, "ot-dsp56k: core %u retired at P:%#07x\n",
                    index, dsp->getPC().toWord());
            return false;
        }
        if (g_execHook) {
            const TWord p = dsp->getPC().toWord();
            dsp->exec();
            g_execHook(*this, p);
        } else {
            dsp->exec();
        }
    }
    return true;
}

void Core::fastForwardSlot()
{
    dsp->fastForward(kCyclesPerSlot, kCyclesPerSlot);
    step(8);                    /* let the poll and peripherals observe it */
}

bool Core::hostTxArmed()
{
    auto &d = periphX.getDMA();
    for (unsigned ch = 0; ch < 6; ch++) {
        const TWord dcr = d.getDCR(ch);
        if ((dcr & 0x800000) && ((dcr >> 11) & 0x1f) == 17)
            return true;
    }
    return false;
}

void Core::disarmHostRx()
{
    auto &d = periphX.getDMA();
    for (unsigned ch = 0; ch < 2; ch++) {
        const TWord dcr = d.getDCR(ch);
        if ((dcr & 0x800000) && ((dcr >> 11) & 0x1f) == 16)
            d.setDCR(ch, dcr & ~TWord(0x800000));
    }
}

bool portIcrReset(Core &c, const StepFn &stepper)
{
    for (unsigned g = 0; g < 100000 && c.hdi().hasRXData() && !c.dead; g++)
        stepper();
    const bool ok = !c.hdi().hasRXData();
    c.hdi().clearRX();
    return ok;
}

bool portCommand(Core &c, TWord cvr, size_t argOcc, const StepFn &stepper)
{
    if (!c.runnable())
        return false;
    bool ok = true;
    if (bittest<TWord, HDI08::HCR_HCIE>(c.hdi().readControlRegister())) {
        c.dsp->injectExternalInterrupt((cvr & 0x3F) * 2);
        /* Barrier 1: the vector must be internalised before any further word
         * enters the port, or later data overtakes the command. */
        for (unsigned g = 0;
             g < 100000 && c.dsp->hasPendingExternalInterrupts() && !c.dead; g++)
            stepper();
        ok = !c.dsp->hasPendingExternalInterrupts();
    }
    /* Barrier 2: the handler has consumed its arguments. */
    for (unsigned g = 0;
         g < 4096 && c.hdi().rxData().size() > argOcc && !c.dead; g++)
        stepper();
    return ok && c.hdi().rxData().size() <= argOcc;
}

bool Chip::open()
{
    char name[64];
    snprintf(name, sizeof name, "/octemu-icc-%d", (int)getpid());
    shm_unlink(name);
    g_iccFd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (g_iccFd < 0)
        return false;
    shm_unlink(name);
    return ftruncate(g_iccFd, (off_t)kIccWords * sizeof(TWord)) == 0;
}

void Chip::step(unsigned n)
{
    for (auto &c : core)
        c.step(n);
}

} // namespace ot
