// Ghidra post-script: export function bounds, register-jump targets and uncovered .text bytes.
// Args: <output dir>
//   ghidra_funcs.csv     entry,name,min,max,ranges,instructions
//   ghidra_jumps.csv     site,func,flow,targets(;-separated)   (every jr that is not jr $ra)
//   ghidra_uncovered.csv start,end   (non-zero .text bytes in no function body)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

public class WG3DExport extends GhidraScript {
    static final long TEXT_START = 0x80001C00L, TEXT_END = 0x8009BC50L;

    @Override
    public void run() throws Exception {
        Path out = Path.of(getScriptArgs()[0]);
        Listing listing = currentProgram.getListing();
        Memory mem = currentProgram.getMemory();
        AddressSet text = new AddressSet(toAddr(TEXT_START), toAddr(TEXT_END - 1));
        AddressSet covered = new AddressSet();

        try (PrintWriter w = new PrintWriter(out.resolve("ghidra_funcs.csv").toFile())) {
            w.println("entry,name,min,max,ranges,instructions");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                AddressSetView body = f.getBody();
                if (!text.contains(f.getEntryPoint())) continue;
                covered.add(body);
                int ninsn = 0;
                for (Instruction i : listing.getInstructions(body, true)) ninsn++;
                w.printf("0x%s,%s,0x%s,0x%s,%d,%d%n", f.getEntryPoint(), f.getName(),
                        body.getMinAddress(), body.getMaxAddress(), body.getNumAddressRanges(), ninsn);
            }
        }

        try (PrintWriter w = new PrintWriter(out.resolve("ghidra_jumps.csv").toFile())) {
            w.println("site,func,flow,targets");
            for (Instruction i : listing.getInstructions(text, true)) {
                if (!i.getMnemonicString().endsWith("jr")) continue;
                String reg = i.getDefaultOperandRepresentation(0);
                if (reg.equals("ra")) continue;
                List<String> t = new ArrayList<>();
                for (Reference r : i.getReferencesFrom()) {
                    if (r.getReferenceType().isJump()) t.add(r.getToAddress().toString());
                }
                Function f = getFunctionContaining(i.getAddress());
                w.printf("0x%s,%s,%s,%s%n", i.getAddress(), f == null ? "" : "0x" + f.getEntryPoint(),
                        i.getFlowType(), String.join(";", t));
            }
        }

        try (PrintWriter w = new PrintWriter(out.resolve("ghidra_uncovered.csv").toFile())) {
            w.println("start,end");
            for (AddressRange r : text.subtract(covered)) {
                boolean nonzero = false;
                for (Address a = r.getMinAddress(); a.compareTo(r.getMaxAddress()) <= 0; a = a.add(1)) {
                    if (mem.getByte(a) != 0) { nonzero = true; break; }
                }
                if (nonzero) w.printf("0x%s,0x%s%n", r.getMinAddress(), r.getMaxAddress().add(1));
            }
        }
        println("WG3DExport: done");
    }
}
