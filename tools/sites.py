#!/usr/bin/env python3
"""The scratchpad's access sites, from RT_SITES runs.

    make -C recomp BUILD=../build/recomp-sites RECOMP_FLAGS=--sites \
         OPT="-O2 -DRT_SITES" DEBUG=0
    RT_SITES_OUT=run1.txt build/recomp-sites/tpm-host ...   (several runs)
    tools/sites.py run1.txt run2.txt > config/scratch_sites.us.txt

A site goes in the list when every access it made in those runs was to the
scratchpad (at 0x1F800000). The recompiler (--scratch-sites) then checks
the scratchpad first there; any address still works anywhere, so a site
the runs did not reach, or got wrong, only costs time.
"""
import sys

mask = {}
for path in sys.argv[1:]:
    for line in open(path):
        pc, m = line.split()
        mask[int(pc, 16)] = mask.get(int(pc, 16), 0) | int(m)
scr = sorted(pc for pc, m in mask.items() if m == 2)
print("# scratchpad access sites (tools/sites.py): %d of %d sites seen"
      % (len(scr), len(mask)))
for pc in scr:
    print("%08X" % pc)
