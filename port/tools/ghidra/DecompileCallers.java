// Decompiles every function that calls a symbol whose name contains one of the script arguments
// (imports are named by GhidraOrbis, e.g. sceAppContentAddcontMount, or by NID).
// Usage (headless): -postScript DecompileCallers.java sceAppContentAddcontMount VANhIWcqYak
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

import java.util.LinkedHashSet;
import java.util.Set;

public class DecompileCallers extends GhidraScript {
    @Override
    protected void run() throws Exception {
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Set<Function> done = new LinkedHashSet<>();
        for (String arg : getScriptArgs()) {
            SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
            while (it.hasNext()) {
                Symbol s = it.next();
                if (!s.getName().contains(arg)) continue;
                println("==== symbol " + s.getName() + " @ " + s.getAddress());
                Set<Function> callers = new LinkedHashSet<>();
                for (Reference ref : getReferencesTo(s.getAddress())) {
                    Function f = getFunctionContaining(ref.getFromAddress());
                    if (f != null) callers.add(f);
                    // Calls usually go through a thunk: follow references to the thunk too.
                    Function thunk = getFunctionAt(ref.getFromAddress());
                    if (thunk != null)
                        for (Reference r2 : getReferencesTo(thunk.getEntryPoint())) {
                            Function g = getFunctionContaining(r2.getFromAddress());
                            if (g != null) callers.add(g);
                        }
                }
                for (Function f : callers) {
                    if (!done.add(f)) continue;
                    println("---- " + f.getName() + " @ " + f.getEntryPoint());
                    println(dec.decompileFunction(f, 120, monitor).getDecompiledFunction().getC());
                }
            }
        }
    }
}
