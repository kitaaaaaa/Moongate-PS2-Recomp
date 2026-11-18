// Removes data the analysis defined inside .text and gives the code back to its functions.
// SLPM_627.67's .text holds only code (GCC puts jump tables in .rodata), but Ghidra's analysis defines a few
// "pointers" there, some 2 bytes off instruction alignment. Disassembly stops at them, so the function before
// is cut short where it falls through, and the stray byte left over became a 1-byte "function"
// (the recompiled game then resumed at an odd address).
// @category PS2Recomp

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.app.script.GhidraScript;

import java.util.ArrayList;
import java.util.List;

public class CleanTextData extends GhidraScript {
    @Override
    public void run() throws Exception {
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        Listing listing = currentProgram.getListing();

        List<Address> cleared = new ArrayList<>();
        DataIterator it = listing.getDefinedData(text.getStart(), true);
        List<Data> data = new ArrayList<>();
        while (it.hasNext()) {
            Data d = it.next();
            if (!text.contains(d.getMinAddress()))
                break;
            data.add(d);
        }
        for (Data d : data) {
            // Back to the instruction word holding its first byte.
            Address start = toAddr(d.getMinAddress().getOffset() & ~3L);
            Address end = d.getMaxAddress();
            listing.clearCodeUnits(start, end, false);
            cleared.add(start);
            println(String.format("CleanTextData: cleared %s..%s (%s)", start, end, d.getDataType().getName()));
        }

        // Functions whose body is not whole instructions are the stray bytes wrapped as functions.
        List<Function> bogus = new ArrayList<>();
        FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(text.getStart(), true);
        while (fi.hasNext()) {
            Function f = fi.next();
            if (!text.contains(f.getEntryPoint()))
                break;
            if (f.getBody().getNumAddresses() % 4 != 0 || (f.getEntryPoint().getOffset() & 3L) != 0)
                bogus.add(f);
        }
        for (Function f : bogus) {
            listing.clearCodeUnits(f.getBody().getMinAddress(), f.getBody().getMaxAddress(), false);
            currentProgram.getFunctionManager().removeFunction(f.getEntryPoint());
            cleared.add(toAddr(f.getEntryPoint().getOffset() & ~3L));
            println("CleanTextData: removed " + f.getName());
        }

        // Disassemble the freed code and let the function falling into it take it back.
        for (Address a : cleared) {
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            Function owner = getFunctionContaining(a.subtract(4));
            if (owner != null)
                CreateFunctionCmd.fixupFunctionBody(currentProgram, owner.getEntryPoint().equals(a) ? owner : owner, monitor);
        }
        println("CleanTextData: " + data.size() + " data items, " + bogus.size() + " functions");
    }
}
