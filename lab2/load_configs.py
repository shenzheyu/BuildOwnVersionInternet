"""
Automate the loading of the configs for all the routers in Internet2
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

nodes = ['SEAT', 'LOSA', 'SALT', 'HOUS', 'KANS', 'ATLA', 'WASH', 'CHIC', 'NEWY']

root = '%s/%s' % (os.getcwd(), sys.argv[1])

for node in nodes:
    zebra = open('%s/%s/zebra.conf.sav' % (root, node), 'r')
    cmds = ['conf t']
    flag = False
    for line in zebra.readlines():
        if line.startswith('interface'):
            cmds.append(line.strip())
            flag = True
            continue
        if flag:
            if line.startswith('!'):
                mx_run_vty_command(node, cmds)
                cmds = ['conf t']
                flag = False
                continue
            cmds.append(line.strip())
            mx_run_vty_command(node, cmds)
            cmds = ['conf t']
            flag = False

for node in nodes:
    ospfd = open('%s/%s/ospfd.conf.sav' % (root, node), 'r')
    cmds = ['conf t', 'router ospf', 'network 4.0.0.0/8 area 0']
    mx_run_vty_command(node, cmds)
    cmds = ['conf t']
    flag = False
    for line in ospfd.readlines():
        if line.startswith('interface'):
            cmds.append(line.strip())
            flag = True
            continue
        if flag:
            if line.startswith('!'):
                mx_run_vty_command(node, cmds)
                cmds = ['conf t']
                flag = False
                continue
            cmds.append(line.strip())
            mx_run_vty_command(node, cmds)
            cmds = ['conf t']
            flag = False