// Lists the instructions (address, bytes, text) in [start, end) (hex offsets without the 0x1000000 base).
// Usage (headless): -postScript ListInstructions.java <start> <end>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class ListInstructions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        long base = 0x1000000L;
        Address start = toAddr(base + Long.parseLong(a[0], 16));
        Address end = toAddr(base + Long.parseLong(a[1], 16));
        InstructionIterator it = currentProgram.getListing().getInstructions(start, true);
        while (it.hasNext()) {
            Instruction in = it.next();
            if (in.getAddress().compareTo(end) >= 0) break;
            StringBuilder hex = new StringBuilder();
            for (byte b : in.getBytes()) hex.append(String.format("%02x", b));
            println(String.format("%s  %-24s %s", in.getAddress(), hex, in));
        }
    }
}
