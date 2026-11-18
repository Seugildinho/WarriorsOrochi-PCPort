// Export one existing function for local investigation without changing it.
// @category WarriorsOrochi
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import java.io.File;
import java.io.PrintWriter;

public class InspectOrochiFunction extends GhidraScript {
    @Override public void run() throws Exception {
        String[] args=getScriptArgs();
        Function function=getFunctionContaining(toAddr(Long.decode(args[0])));
        if(function==null) throw new IllegalStateException("No analysed function contains "+args[0]);
        DecompInterface decompiler=new DecompInterface();
        try {
            if(!decompiler.openProgram(currentProgram)) throw new IllegalStateException("Could not open decompiler");
            DecompileResults result=decompiler.decompileFunction(function,30,monitor);
            if(!result.decompileCompleted() || result.getDecompiledFunction()==null)
                throw new IllegalStateException(result.getErrorMessage());
            try(PrintWriter out=new PrintWriter(new File(args[1]))) {
                out.println("// Original USA program "+currentProgram.getName()+"; function "+function.getName()+" at "+function.getEntryPoint());
                out.println(result.getDecompiledFunction().getC());
            }
            println("Exported existing function "+function.getEntryPoint()+" to "+args[1]);
        } finally { decompiler.dispose(); }
    }
}
