#!/bin/sh
set -eu

cert=/run/tls/tls.crt
key=/run/tls/tls.key
prev=
for arg in "$@"; do
    case "$prev" in
        -cert) cert=$arg ;;
        -key) key=$arg ;;
    esac
    prev=$arg
done

if [ ! -s "$cert" ] || [ ! -s "$key" ]; then
    mkdir -p "$(dirname "$cert")" "$(dirname "$key")"
    umask 077
    openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
        -subj '/CN=nanokvm-rdp-gateway' -keyout "$key" -out "$cert"
fi

exec /usr/local/bin/nanokvm-rdp-gateway "$@"
