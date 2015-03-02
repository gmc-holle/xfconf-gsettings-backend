#!/bin/sh

PROGDIR=$(dirname $0)
shift 1

GSETTINGS_BACKEND=xfconf GIO_EXTRA_MODULES="${PROGDIR}" $*
