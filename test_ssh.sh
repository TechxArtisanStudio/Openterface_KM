#!/usr/bin/expect -f

set timeout 10
set password "bbot"

spawn ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o PreferredAuthentications=keyboard-interactive,password -o PubkeyAuthentication=no bot@127.0.0.1 -p 18048 "echo 'SSH connection successful!'; whoami; hostname"

expect {
    "assword:" {
        send "$password\r"
        exp_continue
    }
    "SSH connection successful!" {
        puts "\n[OK] SSH connection test passed!"
        exit 0
    }
    timeout {
        puts "\n[X] SSH connection timed out"
        exit 1
    }
    eof
}
