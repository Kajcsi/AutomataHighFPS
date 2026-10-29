"""Disassembles the patch dump written by harness.exe (needs `pip install capstone`).

usage: python disasm_patches.py <dump.jsonl>
Shows each patched site before/after and its code cave, so caves can be reviewed
instruction by instruction. Addresses are image-relative (RVA)."""
import json
import sys

import capstone

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


def show(label, data, rva, limit=None):
    print('   ' + label)
    for n, i in enumerate(md.disasm(data, rva)):
        if limit and n >= limit:
            break
        print('     %8x: %-28s %s %s' % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))


for line in open(sys.argv[1]):
    p = json.loads(line)
    print('== %s  (rva %x, %d bytes)' % (p['name'], p['rva'], p['len']))
    show('original', bytes.fromhex(p['orig']), p['rva'], 6)
    show('patched', bytes.fromhex(p['new']), p['rva'], 4)
    if p['caveBytes']:
        show('cave', bytes.fromhex(p['caveBytes']), p['cave'])
