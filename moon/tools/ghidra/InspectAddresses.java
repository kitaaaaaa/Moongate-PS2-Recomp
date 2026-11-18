// Prints what the listing holds at the addresses given as arguments (debug aid).
// @category PS2Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;

public class InspectAddresses extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String arg : getScriptArgs()) {
            Address a = toAddr(Long.parseLong(arg, 16));
            CodeUnit cu = currentProgram.getListing().getCodeUnitContaining(a);
            Function f = getFunctionContaining(a);
            Instruction prev = currentProgram.getListing().getInstructionContaining(a.subtract(4));
            println(String.format("Inspect %s: unit=%s @%s len=%d | func=%s body=%s | prev=%s flow=%s",
                a, cu == null ? "none" : cu.getClass().getSimpleName() + ":" + cu, cu == null ? "-" : cu.getMinAddress(),
                cu == null ? 0 : cu.getLength(), f == null ? "-" : f.getName(), f == null ? "-" : f.getBody(),
                prev == null ? "none" : prev.toString(), prev == null ? "-" : prev.getFlowType() + " fallthrough=" + prev.getFallThrough()));
        }
    }
}
