# SPDX-License-Identifier: MIT
"""Index-buffer register stream check for t_vk_draw --only share_idx (the frame-build fast path's key).

DUMP.jsonl is written by the fake backend (MALI_FAKE_DUMP=path). The share_idx frame is four indexed draws whose per-draw
parameters are identical and whose index buffer alternates A B A B, with an identical pipeline rebind before the third. The
driver skips the per-draw register packing when nothing differs (mali_cs_emit.c same_draw_words), and vkCmdBindIndexBuffer
sets no dirty bit, so the index-buffer VA is only protected by being part of that key. This script counts the writes of the
index-buffer register in the last kick and requires A B A B: four writes, first == third, second == fourth, first != second.

  check_share_idx.py DUMP.jsonl ARCH(v11|v13)      exit 0 on pass, 1 on fail
"""
import json, struct, sys

IDX_REG = {11: 54, 13: 48}      # KB_IDVS_INDEX_BUFFER / KB13_IDVS_INDEX_BUFFER
MASK48 = (1 << 48) - 1
RUN = {11: 6, 13: 12}           # RUN_IDVS / RUN_IDVS2


def kicks(path):
    rings = []
    for line in open(path):
        e = json.loads(line)
        if e.get('ev') == 'ring' and 'hex' in e:
            rings.append(struct.unpack('<%dQ' % (len(e['hex']) // 16), bytes.fromhex(e['hex'])))
    return rings


def main():
    path, arch = sys.argv[1], int(sys.argv[2].lstrip('v'))
    rs = kicks(path)
    if not rs:
        print('no ring in %s (MALI_FAKE_DUMP set?)' % path)
        return 1
    q = rs[-1]
    reg = IDX_REG[arch]
    writes, runs = [], 0
    for w in q:
        op, a = w >> 56, (w >> 48) & 0xff
        if op == RUN[arch]:
            runs += 1
        elif arch == 11 and op == 1 and a == reg:        # MOVE48 dst(reg)
            writes.append((w & 0xffffffff) | (((w >> 32) & 0xffff) << 32))
        elif arch == 13 and op == 2 and a == reg:        # MOVE32 dst(reg): the VA's low word (M64 = two MOVE32)
            writes.append(w & 0xffffffff)
    bad = []
    if runs != 4:
        bad.append('%d draw runs, want 4' % runs)
    if len(writes) != 4:
        bad.append('%d index-buffer writes, want 4 (a skipped identical-word draw loses the new index buffer)' % len(writes))
    elif not (writes[0] == writes[2] and writes[1] == writes[3] and writes[0] != writes[1]):
        bad.append('write sequence %s, want A B A B with A != B' % ' '.join('%#x' % v for v in writes))
    if bad:
        print('; '.join(bad))
        return 1
    print('4 draws, 4 index writes A B A B (%#x / %#x)' % (writes[0], writes[1]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
