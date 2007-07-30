#!/bin/sh

#
# nothing need for root
#
if [ $UID -eq 0 ]; then
    exit 0
fi

if [ -z "$KRB5DIR" ]; then
    KRB5DIR=/usr/kerberos
fi

$KRB5DIR/bin/klist -5 -s
invalid=$?

if [ $invalid -eq 0 ]; then
    exit 0
fi

echo "***** refresh Kerberos V5 TGT for uid $UID *****"
if [ -z "$GSS_PASS" ]; then
    $KRB5DIR/bin/kinit
else
    expect <<EOF
set timeout 30 

log_user 1 

set spawnid [spawn /bin/bash]
send "export PS1=\"user@host $ \" \r"
expect {
    timeout {puts "timeout" ;exit 1}
    "user@host $ "
}

send "$KRB5DIR/bin/kinit\r"
expect {
    timeout {puts "timeout" ;exit 1}
    "Password for "
}

send "$GSS_PASS\r"
expect {
    timeout {puts "timeout" ;exit 1}
    "user@host $ "
}

exit 0
EOF
fi
ret=$?
exit $ret
