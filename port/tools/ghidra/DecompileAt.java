// Decompiles functions by image offset or by the strings they use.
// Arguments: hex image offsets (e.g. 5a5ce8, a return address: the call before it is printed too)
// or s:<text> (every function referencing a string containing <text>).
// Usage (headless): -postScript DecompileAt.java 5a5ce8 "s:HDR2"
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

import java.util.LinkedHashSet;
import java.util.Set;

public class DecompileAt extends GhidraScript {
    private DecompInterface dec;

    private void decompile(Function f) {
        println("==== " + f.getName() + " @ " + f.getEntryPoint());
        println(dec.decompileFunction(f, 120, monitor).getDecompiledFunction().getC());
    }

    @Override
    protected void run() throws Exception {
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Address base = currentProgram.getImageBase();
        Set<Function> done = new LinkedHashSet<>();
        for (String arg : getScriptArgs()) {
            if (arg.startsWith("s:")) {
                String needle = arg.substring(2);
                DataIterator it = currentProgram.getListing().getDefinedData(true);
                while (it.hasNext()) {
                    Data d = it.next();
                    Object v = d.getValue();
                    if (!(v instanceof String) || !((String) v).contains(needle)) continue;
                    for (Reference r : getReferencesTo(d.getAddress())) {
                        Function f = getFunctionContaining(r.getFromAddress());
                        if (f != null && done.add(f)) decompile(f);
                    }
                }
                continue;
            }
            Address ret = base.add(Long.parseLong(arg, 16));
            Instruction call = getInstructionBefore(ret);
            if (call != null) println("==== before " + ret + ": " + call);
            Function f = getFunctionContaining(ret);
            if (f != null && done.add(f)) decompile(f);
        }
    }
}
