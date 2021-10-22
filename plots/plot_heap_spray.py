import matplotlib
matplotlib.use('Agg')

import pylab
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

# color code
c1 = (114/255., 158/255., 206/255.)
c2 = (255/255., 158/255.,  74/255.)
c3 = (103/255., 191/255.,  92/255.)
c4 = (237/255., 102/255.,  93/255.)

index = 2
f = open('../heap_spray/stats_time.out')
lines = f.readlines()

#GETSET
s0 = int(lines[0])
a0 = int(lines[1])

s = [int(lines[2+x*2]) for x in range(6)]
a = [int(lines[2+x*2+1]) for x in range(6)]

fig = plt.figure(figsize=[4.5,3])
ax = fig.add_subplot(1, 1, 1)

ax.set_yscale('log')
ax.set_xlim([0,9])
ax.set_ylim([0.01,1000])

freq = 2270000

objects = ('', '1', '2', '3', '4', '5', '6', ' ', 'Find set')#, '7', '8')
y_pos = np.arange(len(objects))
server_results = [x / freq for x in [0] + list(map(lambda el: el-s0, s)) + [0, s0]]
attacker_results = [x / freq for x in [0] + list(map(lambda el: el-a0, a)) + [0, a0]]

ax.bar(y_pos-0.2, server_results, width=0.4, color=c1, hatch='\\', align='center', label="victim")
ax.bar(y_pos+0.2, attacker_results, width=0.4, color=c2, align='center', label="attacker")

plt.xlabel("Size of the secret to recover (Bytes)")
plt.ylabel("Execution time (ms)")
plt.xticks(y_pos, objects)
red = mpatches.Patch(facecolor=c1, hatch='\\', label='victim')
blue = mpatches.Patch(facecolor=c2, label='attacker')
plt.legend([red, blue])


def savepdfviasvg(fig, name, **kwargs):
    import subprocess
    fig.savefig(name+".svg", format="svg", **kwargs)
    incmd = ["inkscape", name+".svg", "--export-pdf={}.pdf".format(name),
             ""] #"--export-ignore-filters",
    subprocess.check_output(incmd)

plt.tight_layout()    
# savepdfviasvg(fig, "timing")

fig.savefig("timing.pdf")

# plt.show()
