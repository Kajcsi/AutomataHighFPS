"""Checks every code cave in a harness dump decodes cleanly and ends in a control transfer.

usage: python check_caves.py <dump.jsonl>   (needs `pip install capstone`)
A cave whose tail was overwritten (or that falls through into unrelated bytes)
shows up as an undecodable byte or a last instruction that is not jmp/ret."""
import json
import sys

import capstone

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
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
        continue
    # Branches inside a cave: conditional and short jumps must land on an instruction
    # boundary of the same cave (only rel32 jmp/call may leave it, back to game code).
    starts = {i.address for i in ins} | {len(code)}
    for i in ins:
        if not (i.group(capstone.CS_GRP_JUMP) and i.operands and i.operands[0].type == capstone.x86.X86_OP_IMM):
            continue
        target = i.operands[0].imm
        leaves = i.mnemonic == 'jmp' and i.size == 5
        if not leaves and target not in starts:
            bad += 1
            print('BAD %-40s %s %s at +%d lands outside the cave or mid-instruction' % (
                p['name'], i.mnemonic, i.op_str, i.address))
            break
print('%d caves checked, %d bad' % (total, bad))
sys.exit(1 if bad else 0)
