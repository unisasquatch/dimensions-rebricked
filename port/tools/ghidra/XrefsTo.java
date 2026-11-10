// Lists every reference to the given addresses (Ghidra addresses, hex) with the referencing function
// and instruction. Usage: -postScript XrefsTo.java 4130b68 ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class XrefsTo extends GhidraScript {
    @Override
    protected void run() throws Exception {
        for (String arg : getScriptArgs()) {
            Address target = toAddr(Long.parseLong(arg, 16));
            println("==== references to " + target);
            for (Reference ref : getReferencesTo(target)) {
                Address from = ref.getFromAddress();
                Function f = getFunctionContaining(from);
                Instruction ins = getInstructionAt(from);
                println("  " + from + " in " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()) + ": " +
                        (ins == null ? "" : ins.toString()) + " [" + ref.getReferenceType() + "]");
            }
        }
    }
}
