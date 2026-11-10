// Decompiles every function that references a string containing the script argument.
// Usage (headless): -postScript DecompileStringUsers.java "<text>"
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.util.LinkedHashSet;
import java.util.Set;

public class DecompileStringUsers extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String needle = getScriptArgs()[0];
        Set<Function> functions = new LinkedHashSet<>();
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext()) {
            Data d = it.next();
            Object v = d.getValue();
            if (!(v instanceof String) || !((String) v).contains(needle)) continue;
            for (Reference r : getReferencesTo(d.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) functions.add(f);
            }
        }
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        for (Function f : functions) {
            println("==== " + f.getName() + " @ " + f.getEntryPoint());
            println(dec.decompileFunction(f, 120, monitor).getDecompiledFunction().getC());
        }
        if (functions.isEmpty()) println("no references to a string containing: " + needle);
    }
}
