#!/bin/sh

# Networking: create tap devices
# Note: this has to be done before the lldp service is started (S60)
# mod by DL5OP
TAP_IP_ADDR=`fw_printenv -n hnap_tap_ip`
TAP_GW_ADDR=`fw_printenv -n hnap_gw_ip 2> /dev/null || echo 192.168.123.1`
TAP_FW_OUT=`fw_printenv -n hnap_fw_out 2> /dev/null || echo tap0`
TAP_FW_IN=`fw_printenv -n hnap_fw_in 2> /dev/null || echo usb0`

echo 1 > /proc/sys/net/ipv4/ip_forward
tunctl -t tap0
ifconfig tap0 $TAP_IP_ADDR
ifconfig tap0 up

route add default gw $TAP_GW_ADDR $TAP_FW_OUT
iptables -A FORWARD --in-interface $TAP_FW_IN -j ACCEPT
iptables --table nat -A POSTROUTING --out-interface $TAP_FW_OUT -j MASQUERADE
