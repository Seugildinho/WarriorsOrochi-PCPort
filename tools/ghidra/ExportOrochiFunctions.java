// Export decoded contiguous function-body ranges without treating gaps as code.
// @category WarriorsOrochi
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressRangeIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.TreeMap;

public class ExportOrochiFunctions extends GhidraScript {
    static class Range {
        long start, end; String name; boolean entry;
        Range(long start,long end,String name,boolean entry) {
            this.start=start;this.end=end;this.name=name;this.entry=entry;
        }
    }
    @Override public void run() throws Exception {
        String[] args=getScriptArgs();
        long minimum=args.length>1?Long.decode(args[1]):0x100000L;
        long maximum=args.length>2?Long.decode(args[2]):0x358500L;
        TreeMap<Long,Range> ranges=new TreeMap<>();
        int functions=0,discontiguous=0;
        FunctionIterator iterator=currentProgram.getFunctionManager().getFunctions(true);
        while(iterator.hasNext()) {
            Function function=iterator.next();
            if(function.isExternal()) continue;
            functions++;
            long entry=function.getEntryPoint().getOffset();
            AddressRangeIterator bodies=function.getBody().getAddressRanges(true);
            int count=0;
            while(bodies.hasNext()) {
                AddressRange body=bodies.next();
                long start=body.getMinAddress().getOffset(),end=body.getMaxAddress().getOffset()+1;
                if(start<minimum || end>maximum) continue;
                boolean ownsEntry=entry>=start && entry<end;
                String name=ownsEntry?function.getName():"orochi_block_"+Long.toHexString(start);
                Range previous=ranges.get(start);
                if(previous==null || ownsEntry && !previous.entry)
                    ranges.put(start,new Range(start,end,name,ownsEntry));
                else previous.end=Math.max(previous.end,end);
                count++;
            }
            if(count>1) discontiguous++;
        }
        ArrayList<Range> ordered=new ArrayList<>(ranges.values());
        try(PrintWriter out=new PrintWriter(new File(getScriptArgs()[0]))) {
            out.println("Name,Start,End,Size");
            int exported=0;
            for(int i=0;i<ordered.size();i++) {
                Range range=ordered.get(i);
                long end=i+1<ordered.size()?Math.min(range.end,ordered.get(i+1).start):range.end;
                if(end<=range.start || (range.start&3)!=0 || (end&3)!=0) continue;
                out.printf("%s,0x%08X,0x%08X,%d%n",range.name,range.start,end,end-range.start);
                exported++;
            }
            println("Exported "+exported+" non-overlapping instruction ranges from "+functions+
                    " functions; "+discontiguous+" functions had discontiguous bodies.");
        }
    }
}
