// Creates a function at the first non-zero word of every stretch of .text no function covers, until none is left.
// SLPM_627.67 is stripped: functions reached only through pointers (callbacks, tables) are not found by Ghidra's
// analysis, and a call to one stops the recompiled game. GCC emits only code in .text (jump tables are in .rodata),
// so every non-padding word there is an instruction.
// @category PS2Recomp

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.MemoryBlock;

public class CreateGapFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        FunctionManager fm = currentProgram.getFunctionManager();
        long start = text.getStart().getOffset();
        long end = text.getEnd().getOffset() + 1;
        int created = 0;
        for (int pass = 0; pass < 50; pass++) {
            int thisPass = 0;
            for (long a = start; a < end; a += 4) {
                Address addr = toAddr(a);
                if (fm.getFunctionContaining(addr) != null) {
                    continue;
                }
                if (currentProgram.getMemory().getInt(addr) == 0) {
                    continue;
                }
                new DisassembleCommand(addr, null, true).applyTo(currentProgram, monitor);
                if (new CreateFunctionCmd(addr).applyTo(currentProgram, monitor)) {
                    thisPass++;
                    if (fm.getFunctionAt(addr) != null) {
                        a = fm.getFunctionAt(addr).getBody().getMaxAddress().getOffset() & ~3L;
                    }
                } else {
                    println(String.format("CreateGapFunctions: could not create a function at 0x%08X", a));
                }
            }
            created += thisPass;
            if (thisPass == 0) {
                break;
            }
        }
        println("CreateGapFunctions: created " + created);
    }
}
