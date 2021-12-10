#!/bin/bash

set -ue

# Performs a delegated release step in a CircleCI Linux container. This mechanism is described
# in scripts/circleci/README.md. All of the necessary environment variables should already be
# in the generated CircleCI configuration.

STEP="$1"
SCRIPT="$2"
echo
echo "[${STEP}] executing ${SCRIPT}"
"./${SCRIPT}"