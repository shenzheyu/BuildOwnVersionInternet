"""
Automate configuring each Internet2 host's interface and configuring the default route
"""

import os
import sys
import subprocess

def mx_run_command(node, cmd):
    """
    Run a cmd on a Mininext node's shell and return the output
    """

    # print("*** Running %s on node %s\n" % (cmd, node))
    res = subprocess.check_output(['./go_to.sh', node, '-c', cmd])

nodes = {
    'seat': '109',
    'losa': '108',
    'salt': '107', 
    'hous': '106', 
    'kans': '105', 
    'atla': '104',
    'wash': '103',
    'chic': '102',
    'newy': '101'
}

for node in nodes.keys():
    host = '%s-host' % (node.upper())
    ifconfig = 'sudo ifconfig %s 4.%s.0.1/24 up' % (node, nodes[node])
    gateway = 'sudo route add default gw 4.%s.0.2 %s' % (nodes[node], node)
    mx_run_command(host, ifconfig)
    mx_run_command(host, gateway)