savedcmd_snd-ns6-audio.mod := printf '%s\n'   snd-ns6-audio.o | awk '!x[$$0]++ { print("./"$$0) }' > snd-ns6-audio.mod
