import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class Decomp extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String a : args) {
            Address addr = currentProgram.getAddressFactory().getAddress(a);
            Function f = getFunctionContaining(addr);
            if (f == null) { println("// no function at " + a); continue; }
            println("// ==== " + f.getName() + " @ " + f.getEntryPoint() + " (asked " + a + ") ====");
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            if (r.decompileCompleted()) println(r.getDecompiledFunction().getC());
            else println("// decompile failed: " + r.getErrorMessage());
        }
        di.dispose();
    }
}
