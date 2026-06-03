#!/bin/bash

echo "Hello World"

# Handle errors and exceptions
set -e

# Check if a command fails and exit with error code
command || { echo 'Error: Command failed'; exit 1; }

# Define a function to handle signals
signal_handler() {
  echo 'Signal received, exiting...'
  exit 1
}

# Trap signals and call the signal handler
trap signal_handler SIGINT SIGTERM
