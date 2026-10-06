#!/usr/bin/env bash
# fly.board mail stack installer (Debian/Ubuntu). Run as root.
#
#   curl ... | bash   OR   bash deploy/mail/install.sh
#
# Does:
#   1. verifies the mail-verify / mail-import / mail-alias-build binaries;
#   2. installs Postfix main.cf + master.cf pipe transport (fly-mail);
#   3. rebuilds /etc/postfix/fly_aliases(.db) from the live user table;
#   4. installs the Dovecot config (IMAP only, checkpassword auth);
#   5. (optionally) generates an OpenDKIM key and prints the DNS TXT record;
#   6. restarts services.
#
# Leaves relayhost EMPTY on purpose — see docs/mail.md for relay setup.
set -euo pipefail

SITE_ROOT="${FLY_SITE_ROOT:-/home/yjlee/fly.board}"
DOMAIN="oborona.zip"
MAIL_USER="${FLY_MAIL_USER:-flymail}"

if [ "$(id -u)" -ne 0 ]; then
    echo "install.sh must run as root" >&2
    exit 1
fi

for bin in mail-verify mail-import mail-alias-build; do
    if [ ! -x "$SITE_ROOT/$bin" ]; then
        echo "missing $SITE_ROOT/$bin — build it first: make mail-tools" >&2
        exit 1
    fi
done

echo "==> Installing packages (postfix, dovecot-core, opendkim)..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq || true
apt-get install -y -qq postfix dovecot-core dovecot-imapd opendkim opendkim-tools || true

echo "==> Creating Maildir root /var/mail/fly"
mkdir -p /var/mail/fly/spool
# Postfix refuses to run pipe transports as root, so delivery runs as an
# unprivileged user that can only reach the Maildir and the spool — the
# fly_board web server (root) sweeps the spool into the webmail DB.
id "$MAIL_USER" >/dev/null 2>&1 || useradd -r -m -s /usr/sbin/nologin "$MAIL_USER"
chown -R "$MAIL_USER:$MAIL_USER" /var/mail/fly
chmod 755 /var/mail/fly
chmod 775 /var/mail/fly/spool

echo "==> Installing Postfix configuration"
cat "$SITE_ROOT/deploy/mail/postfix-main.cf" >> /etc/postfix/main.cf

# Pipe transport: Postfix hands each inbound message to mail-import, which
# stores it in the recipient's Maildir and in the spool (the fly_board web
# server sweeps the spool into the webmail DB). The binary lives OUTSIDE
# /root because /root is not traversable by the unprivileged delivery user.
install -d -m 755 /usr/local/lib/fly-mail
install -m 755 "$SITE_ROOT/mail-import" /usr/local/lib/fly-mail/mail-import
grep -q '^fly-mail' /etc/postfix/master.cf || cat >> /etc/postfix/master.cf <<EOF
fly-mail   unix  -       n       n       -       -       pipe
  flags=R user=$MAIL_USER argv=/usr/local/lib/fly-mail/mail-import \${recipient}
EOF

echo "==> Rebuilding /etc/postfix/fly_aliases and fly_mailboxes"
( cd "$SITE_ROOT" && ./mail-alias-build /etc/postfix/fly_mailboxes ) > /etc/postfix/fly_aliases
postmap /etc/postfix/fly_aliases
postmap /etc/postfix/fly_mailboxes

echo "==> Installing Dovecot configuration"
cp "$SITE_ROOT/deploy/mail/dovecot.conf" /etc/dovecot/dovecot.conf

# Dedicated self-signed cert with the mail hostname in the SAN (clients only
# complain about the unknown CA, not a hostname mismatch).
if [ ! -f /etc/dovecot/mail.crt ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -subj "/CN=mail.$DOMAIN" \
        -addext "subjectAltName=DNS:mail.$DOMAIN,DNS:$DOMAIN" \
        -keyout /etc/dovecot/mail.key -out /etc/dovecot/mail.crt 2>/dev/null
    chmod 600 /etc/dovecot/mail.key
fi

# PAM bridge: Dovecot 2.4 removed the checkpassword driver, so passdb pam
# calls mail-verify through pam_exec (password on stdin via expose_authtok).
cat > /etc/pam.d/fly-mail <<EOF
auth     required pam_exec.so expose_authtok $SITE_ROOT/mail-verify pam
account  required pam_permit.so
password required pam_deny.so
session  required pam_permit.so
EOF

echo "==> OpenDKIM"
if [ -f "$SITE_ROOT/deploy/mail/opendkim.conf" ]; then
    cp "$SITE_ROOT/deploy/mail/opendkim.conf" /etc/opendkim.conf
    mkdir -p /etc/opendkim/keys/$DOMAIN
    if [ ! -f /etc/opendkim/keys/$DOMAIN/s1.private ]; then
        opendkim-genkey -b 2048 -d $DOMAIN -D /etc/opendkim/keys/$DOMAIN -s s1 -v
        chown -R opendkim:opendkim /etc/opendkim
        echo
        echo "==> ADD THIS DNS TXT RECORD (selector s1):"
        cat /etc/opendkim/keys/$DOMAIN/s1.txt
        echo
        echo "Then uncomment the milter_* lines in /etc/postfix/main.cf and restart postfix."
    fi
fi

echo "==> Restarting services"
systemctl restart postfix || service postfix restart
systemctl restart dovecot || service dovecot restart
systemctl restart opendkim 2>/dev/null || true

echo
echo "Done. Postfix listens on :25, Dovecot IMAP on 143/993."
echo "NOTE: relayhost is EMPTY — outbound goes by direct MX lookup."
echo "      If port 25 outbound is blocked, set a relay in /etc/postfix/main.cf:"
echo "        relayhost = [your.relay.example]:587"
echo "Next: add the DNS records listed in docs/mail.md (MX, SPF, DKIM, DMARC)."
