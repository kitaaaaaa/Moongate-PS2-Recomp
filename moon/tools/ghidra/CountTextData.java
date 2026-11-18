// Counts defined data items in .text (debug aid).
// @category PS2Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;

public class CountTextData extends GhidraScript {
    @Override
    public void run() throws Exception {
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        DataIterator it = currentProgram.getListing().getDefinedData(text.getStart(), true);
        int n = 0;
        StringBuilder first = new StringBuilder();
        while (it.hasNext()) {
            Data d = it.next();
            if (!text.contains(d.getMinAddress())) break;
            if (n < 15) first.append(" ").append(d.getMinAddress()).append(":").append(d.getDataType().getName()).append("/").append(d.getLength());
            n++;
        }
        println("TextData count=" + n + first);
    }
}
