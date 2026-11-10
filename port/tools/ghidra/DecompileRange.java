// Decompiles every function whose entry is in [start, end) (hex, offsets without the 0x1000000 base)
// into a file. Usage (headless): -postScript DecompileRange.java <start> <end> <out file>
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.PrintWriter;

public class DecompileRange extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        long base = 0x1000000L;
        Address start = toAddr(base + Long.parseLong(a[0], 16));
        Address end = toAddr(base + Long.parseLong(a[1], 16));
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(a[2])) {
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(start, true);
            while (it.hasNext()) {
                Function f = it.next();
                if (f.getEntryPoint().compareTo(end) >= 0) break;
                out.println("==== " + f.getName() + " @ " + f.getEntryPoint());
                var r = dec.decompileFunction(f, 120, monitor);
                if (r.getDecompiledFunction() != null) out.println(r.getDecompiledFunction().getC());
            }
        }
    }
}
