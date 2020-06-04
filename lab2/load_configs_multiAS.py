"""
Automate the loading of the OSPF and BGP configs for all the routers in Internet2
"""

import os 
import sys
import subprocess

def mx_run_vty_command(node, cmds):
    """
    Run a cmd on a Quagga shell on a Mininext node and return the output
    """
    cmd = 'vtysh'
    for line in cmds:
        cmd += ' -c \"%s\"' % line
    # print("*** Running %s on node %s\n" % (cmd, node))
    res = subprocess.check_output(['./go_to.sh', node, '-c', cmd])
    return res

nodes = ['SEAT', 'LOSA', 'SALT', 'HOUS', 'KANS', 'ATLA', 'WASH', 'CHIC', 'NEWY', 'east', 'west']
# nodes = ['SEAT', 'west']
files = ['zebra.conf.sav', 'ospfd.conf.sav', 'bgpd.conf.sav']

root = '%s/%s' % (os.getcwd(), sys.argv[1])

for node in nodes:
    for f in files:
        # print("Open %s/%s/%s" % (root, node, f))
        file_open = open('%s/%s/%s' % (root, node, f), 'r')
        cmds = ['conf t']
        for line in file_open.readlines():
            cmds.append(line.strip())
        mx_run_vty_command(node, cmds)
