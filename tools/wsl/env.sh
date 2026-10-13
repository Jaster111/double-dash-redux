# Shared environment for running REA + Ghidra inside the "rea" WSL distro.
export GHIDRA_INSTALL_DIR=/opt/ghidra_12.1.4_PUBLIC
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
export REA_ANALYSIS_PROVIDER=ghidra
export GHIDRA_HEADLESS_MAXMEM=8G
export REA_GHIDRA_STARTUP_TIMEOUT_MS=3600000
export PROJ=/mnt/f/PROJEKTI/doubledashport
export WORK=/root/work
mkdir -p "$WORK"
