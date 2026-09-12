ember

---

a purpose over beauty fork of HorseyofCoursey's EMBER (FLAC/MP3/AAC music player for the M5Cardputer ADV). Faster directory navigation, add to queue, search and more

---

work in progress!

---

on first boot, firmware creates `/.ember/config` file which contains several keys and default values that is essentially the settings of the firmware. very important first is to change `music_dir` key which tells the firmware where you store your music. by default this is /Music

---

after editing, do a scan first 

```
Fn + s
```

this is a one time thing to catalog every music you have into a manifest file found in /.ember/, this makes everything snappy as the device no longer needs to keep reading your SD for contents. massively helpful especially when you have one directory that has a lot of music in it.

keybinds:

`Fn + s` do a scan on music_dir

`up (;)` select up

`down (.)` select down

`right (/)` enter directory or play selection

`left (,)` go to parent directory

`-` lower volume

`=` higher volume

`Fn + -` lower brightness

`Fn + =` higher brightness

`BtnG0` toggle display

`'` add to queue

`Fn + '` play next

`space` play / pause

any key will perform a search string from the manifest

