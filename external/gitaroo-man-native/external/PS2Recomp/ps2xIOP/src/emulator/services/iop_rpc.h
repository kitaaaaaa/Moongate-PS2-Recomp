#pragma once

#include "ps2x/iop/iop_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopGuestExecutor
    {
    public:
        virtual ~IopGuestExecutor() = default;

        [[nodiscard]] virtual uint32_t executeGuestFunction(uint32_t address,
                                                            uint32_t a0,
                                                            uint32_t a1,
                                                            uint32_t a2,
                                                            uint32_t a3,
                                                            uint32_t gp) = 0;
        [[nodiscard]] virtual uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                                      uint32_t a0,
                                                                      uint32_t a1,
                                                                      uint32_t a2,
                                                                      uint32_t a3,
                                                                      uint32_t gp,
                                                                      uint32_t instructionBudget)
        {
            return executeGuestFunction(address, a0, a1, a2, a3, gp);
        }
    };

    class IopRpcBridge
    {
    public:
        IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept;

        void reset();
        [[nodiscard]] bool dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request, IopGuestExecutor &executor);
        void onSifTransfer(const SifTransfer &transfer);
        // A SIF command the EE sent to the IOP (sceSifSendCmd on the EE). False when nothing handled it.
        bool receiveSifCommand(uint32_t commandId, const uint8_t *packet, size_t packetSize, IopGuestExecutor &executor);
        void removeServersInRange(uint32_t base, uint32_t size);

        [[nodiscard]] bool hasServer(uint32_t sid) const noexcept;
        // The IOP thread that serves `sid` (0 if unknown).
        [[nodiscard]] int serverThread(uint32_t sid) const noexcept;
        [[nodiscard]] size_t serverCount() const noexcept { return m_servers.size(); }

    private:
        struct RpcServer
        {
            uint32_t sid = 0;
            uint32_t serverData = 0;
            uint32_t function = 0;
            uint32_t gp = 0;
            uint32_t buffer = 0;
            uint32_t callback = 0;
            uint32_t callbackBuffer = 0;
            uint32_t queue = 0;
            int thread = 0; // the IOP thread that registered it, which serves it in sceSifRpcLoop
        };

        IopHost &m_host;
        IopMemory &m_memory;
        IopKernel &m_kernel;
        std::unordered_map<uint32_t, RpcServer> m_servers;
        uint32_t m_nextDmaId = 1u;
        bool m_sifInitialized = false;
        // sifcmd state as the IOP's SIFCMD module keeps it: software registers (sceSifGetSreg/SetSreg,
        // written by the EE's SET_SREG command) and the handlers registered with sceSifAddCmdHandler.
        std::array<uint32_t, 32> m_sregs{};
        struct CmdHandler
        {
            uint32_t function = 0;
            uint32_t data = 0;
            uint32_t gp = 0;
        };
        std::unordered_map<uint32_t, CmdHandler> m_cmdHandlers;
        uint32_t m_cmdPacketBuffer = 0; // IOP copy of the packet a handler receives
    };
}
