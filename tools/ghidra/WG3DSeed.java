// Ghidra pre-script (runs after import, before auto-analysis) for the W.G. 3D Hockey main segment.
// Splits memory into executable .text and non-executable RSP/data, adds .bss, and seeds
// function starts: the entry stub, the boot function, and the library names from tools/gen_libsyms.py.
// Args: <path to syms/libultra_symbols.txt>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class WG3DSeed extends GhidraScript {
    static final long TEXT_END = 0x8009BC50L;
    static final long BSS_START = 0x800B8390L, BSS_SIZE = 0xAA570L;

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        MemoryBlock all = mem.getBlock(toAddr(0x80001C00L));
        mem.split(all, toAddr(TEXT_END));
        MemoryBlock text = mem.getBlock(toAddr(0x80001C00L));
        MemoryBlock rest = mem.getBlock(toAddr(TEXT_END));
        text.setName(".text");
        text.setRead(true); text.setWrite(false); text.setExecute(true);
        rest.setName("rsp_and_data");
        rest.setRead(true); rest.setWrite(true); rest.setExecute(false);
        if (mem.getBlock(toAddr(BSS_START)) == null) {
            MemoryBlock bss = mem.createUninitializedBlock(".bss", toAddr(BSS_START), BSS_SIZE, false);
            bss.setRead(true); bss.setWrite(true);
        }

        seed(0x80001C00L, "entrypoint");
        seed(0x80003480L, "boot_main");
        Pattern p = Pattern.compile("^([^ ]+) = 0x([0-9A-Fa-f]{8});");
        int n = 0;
        for (String line : Files.readAllLines(Path.of(getScriptArgs()[0]))) {
            Matcher m = p.matcher(line);
            if (m.find()) {
                seed(Long.parseLong(m.group(2), 16), m.group(1));
                n++;
            }
        }
        println("WG3DSeed: seeded " + n + " library functions");
    }

    private void seed(long addr, String name) throws Exception {
        Address a = toAddr(addr);
        disassemble(a);
        if (getFunctionAt(a) == null) {
            createFunction(a, name);
        } else {
            getFunctionAt(a).setName(name, SourceType.USER_DEFINED);
        }
    }
}
