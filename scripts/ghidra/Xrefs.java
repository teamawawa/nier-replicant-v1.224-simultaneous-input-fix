import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;

public class Xrefs extends GhidraScript {
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            Address addr = currentProgram.getAddressFactory().getAddress(a);
            println("// ==== xrefs to " + a + " ====");
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(addr);
            int n = 0;
            while (it.hasNext() && n < 200) {
                Reference r = it.next();
                Address from = r.getFromAddress();
                Function f = getFunctionContaining(from);
                Instruction ins = getInstructionAt(from);
                println("  " + from + "  " + r.getReferenceType() + "  in " +
                        (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()) +
                        (ins == null ? "" : "   | " + ins.toString()));
                n++;
            }
            if (n == 0) println("  (none)");
        }
    }
}
