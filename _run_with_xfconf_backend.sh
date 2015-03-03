#!/bin/sh

PROGDIR=$(dirname $0)
GSETTINGS_BACKEND=xfconf GIO_EXTRA_MODULES="${PROGDIR}" $*
