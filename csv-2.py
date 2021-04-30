

import matplotlib.pyplot as plt

x = []
y1 = []
y2 = []

with open("CWND.csv","r") as fp:
	fp.readline()
	for line in fp:
		data = line.strip().split(",")
		y1.append(float(data[0]))
		x.append(float(data[1]))
		y2.append(float(data[2]))

fig, ax1 = plt.subplots()
ax2 = ax1.twinx()

ax1.plot(x,y1,"g--")
ax1.set_ylim([0,70])

ax2.step(x,y2,"r--")
ax2.set_ylim([0,70])

ax1.set_ylabel("Window size")
ax1.set_xlabel("Time (Sec)")
ax2.set_ylabel("Ssthresh")

ax1.yaxis.label.set_color("g")
ax2.yaxis.label.set_color("r")
plt.show()

plt.savefig("cwnd.pdf")
