"""Checks every code cave in a harness dump decodes cleanly and ends in a control transfer.

usage: python check_caves.py <dump.jsonl>   (needs `pip install capstone`)
A cave whose tail was overwritten (or that falls through into unrelated bytes)
shows up as an undecodable byte or a last instruction that is not jmp/ret."""
import json
import sys

import capstone

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
bad = 0
total = 0
for line in open(sys.argv[1]):
    p = json.loads(line)
    code = bytes.fromhex(p['caveBytes'])
    if not code:
        continue
    total += 1
    if code[:6] == bytes.fromhex('ff2500000000') and len(code) == 14:
        continue  # jmp [rip+0] thunk followed by its 8-byte target
    ins = list(md.disasm(code, 0))
    decoded = sum(i.size for i in ins)
    last = ins[-1] if ins else None
    ends = last is not None and (last.mnemonic in ('jmp', 'ret') or last.bytes[:2] == b'\xff\x25')
    if decoded != len(code) or not ends:
        bad += 1
        print('BAD %-40s decoded %d/%d bytes, last: %s %s' % (
            p['name'], decoded, len(code), last.mnemonic if last else '-', last.op_str if last else ''))
print('%d caves checked, %d bad' % (total, bad))
sys.exit(1 if bad else 0)
