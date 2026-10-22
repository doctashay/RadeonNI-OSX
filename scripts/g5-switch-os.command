#!/bin/sh
# Copyright (c) kouta-kun and Claude. MIT.
# Boot the other Mac OS X volume (Tiger <-> Leopard): bless it, then restart.
case "$(sw_vers -productVersion)" in
10.4*) other=Leopard ;;
*)     other=Tiger ;;
esac
echo "This will restart the Mac into $other."
printf "Press Return to continue, Ctrl-C to cancel. "
read dummy
sudo bless --mount "/Volumes/$other" --setBoot || exit 1
nvram boot-device
sudo /sbin/shutdown -r now
