// Recover indirect entry points referenced as addresses rather than direct JALs.
// @category WarriorsOrochi
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;
import java.io.File;
import java.util.ArrayList;
import java.util.HashSet;

public class DiscoverOrochiFunctions extends GhidraScript {
    @Override public void run() throws Exception {
        ArrayList<Address[]> candidates = new ArrayList<>();
        HashSet<Long> seen = new HashSet<>();
        SymbolIterator symbols = currentProgram.getSymbolTable().getAllSymbols(true);
        while (symbols.hasNext() && !monitor.isCancelled()) {
            Symbol symbol = symbols.next();
            Address address = symbol.getAddress();
            long target = address.getOffset();
            if (target < 0x100000L || target >= 0x3068e8L || (target & 3) != 0 ||
                !seen.add(target) || getInstructionAt(address) == null ||
                currentProgram.getFunctionManager().getFunctionContaining(address) != null) continue;
            ReferenceIterator references = currentProgram.getReferenceManager().getReferencesTo(address);
            while (references.hasNext()) {
                Reference reference = references.next();
                Address source = reference.getFromAddress();
                // Require an actual data/address reference to existing decoded
                // instructions, not a guessed function prologue in raw bytes.
                if (!reference.getReferenceType().isData() ||
                    source.getOffset() < 0x100000L || source.getOffset() >= 0x358500L) continue;
                if (getInstructionAt(source) == null && source.getOffset() < 0x3068e8L) continue;
                candidates.add(new Address[]{source,address});
                break;
            }
        }
        println("Indirect code candidates with address-reference evidence: " + candidates.size());
        if (candidates.size() > 512) throw new IllegalStateException("Too many candidates; review before applying");
        try (PrintWriter out = new PrintWriter(new File(getScriptArgs()[0]))) {
            out.println("ReferenceFrom,Target,FunctionCreated");
            for (Address[] candidate : candidates) {
                Function existing = currentProgram.getFunctionManager().getFunctionContaining(candidate[1]);
                Function function = existing == null ? createFunction(candidate[1],
                    "orochi_referenced_"+candidate[1].toString()) : existing;
                out.printf("%s,%s,%s%n",candidate[0],candidate[1],function != null);
                println("Address reference " + candidate[0]+" -> "+candidate[1]+" function="+(function != null));
            }
        }
    }
}
