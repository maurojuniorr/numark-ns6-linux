savedcmd_snd-ns6.mod := printf '%s\n'   snd-ns6.o | awk '!x[$$0]++ { print("./"$$0) }' > snd-ns6.mod
