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
f = open('../buffer_overflow/stats_time.out')
lines = f.readlines()

#GETSET
s0 = int(lines[0])
a0 = int(lines[1])

s = [int(lines[2+x*2]) for x in range(8)]
a = [int(lines[2+x*2+1]) for x in range(8)]


fig = plt.figure(figsize=[4.5,3])
ax = fig.add_subplot(1, 1, 1)

ax.set_yscale('log')
ax.set_xlim([0,9])
ax.set_ylim([0.1,100])

freq = 2270000

objects = ('', '1', '2', '3', '4', '5', '6', '7', '8')
y_pos = np.arange(len(objects))

server_dyn = [x / freq for x in ([0]+ list(map(lambda e: e-s0, s)))]
attacker_dyn = [x / freq for x in ([0]+ list(map(lambda e: e-a0, a)))]

# ax.bar(y_pos-0.2, server_results, width=0.4, color=c1, hatch='//', align='center', label="victim")
# ax.bar(y_pos+0.2, attacker_results, width=0.4, color=c2, align='center', label="attacker")
ax.bar(y_pos-0.2, server_dyn, width=0.4, color=c3, hatch='//', align='center', label="victim")
ax.bar(y_pos+0.2, attacker_dyn, width=0.4, color=c4, align='center', label="attacker")

plt.xlabel("Size of the secret to recover (Bytes)")
plt.ylabel("Execution time (ms)")
plt.xticks(y_pos, objects)
# red = mpatches.Patch(facecolor=c1, hatch='//', label='victim')
# blue = mpatches.Patch(facecolor=c2, label='attacker')
green = mpatches.Patch(facecolor=c3, hatch='//', label='victim dynamic')
orange = mpatches.Patch(facecolor=c4, label='attacker dynamic')
plt.legend([# red, blue,
                    green, orange])

def savepdfviasvg(fig, name, **kwargs):
    import subprocess
    fig.savefig(name+".svg", format="svg", **kwargs)
    incmd = ["inkscape", name+".svg", "--export-pdf={}.pdf".format(name),
             "--export-pdf-version=1.5"] #"--export-ignore-filters",
    subprocess.check_output(incmd)

plt.tight_layout()
    
# savepdfviasvg(fig, "timing_ov")

fig.savefig("timing_ov.pdf")

# plt.show()
