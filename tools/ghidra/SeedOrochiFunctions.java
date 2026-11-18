// Create functions for entry points proven by the native trace/callback tables.
// @category WarriorsOrochi
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.address.Address;

public class SeedOrochiFunctions extends GhidraScript {
    @Override public void run() throws Exception {
        for (String value : getScriptArgs()) {
            Address address = toAddr(Long.decode(value));
            Function function = currentProgram.getFunctionManager().getFunctionAt(address);
            if (function == null) {
                disassemble(address);
                function = createFunction(address, "orochi_indirect_" + value.replace("0x", ""));
            }
            if (function == null) throw new IllegalStateException("Could not create traced function at " + address);
            println("Verified traced function " + function.getEntryPoint() + " through " + function.getBody().getMaxAddress());
        }
    }
}
