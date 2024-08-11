#!/bin/bash
unionfs -o cow \
"/home/lorenzo-zurini/CHANGES/"=RW:\
"/home/lorenzo-zurini/DIR13/"=RO:\
"/home/lorenzo-zurini/DIR12/"=RO:\
"/home/lorenzo-zurini/DIR11/"=RO:\
"/home/lorenzo-zurini/DIR10/"=RO:\
"/home/lorenzo-zurini/DIR9/"=RO:\
"/home/lorenzo-zurini/DIR8/"=RO:\
"/home/lorenzo-zurini/DIR7/"=RO:\
"/home/lorenzo-zurini/DIR6/"=RO:\
"/home/lorenzo-zurini/DIR5/"=RO:\
"/home/lorenzo-zurini/DIR4/"=RO:\
"/home/lorenzo-zurini/DIR3/"=RO:\
"/home/lorenzo-zurini/DIR2/"=RO:\
"/home/lorenzo-zurini/DIR1/"=RO:\
"/home/lorenzo-zurini/DEFPREFIX_WINE/"=RO \
"/home/lorenzo-zurini/.wine/"

exit
