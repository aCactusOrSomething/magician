savedcmd_magician.mod := printf '%s\n'   magician.o | awk '!x[$$0]++ { print("./"$$0) }' > magician.mod
