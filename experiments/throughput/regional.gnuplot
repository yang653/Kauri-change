set term postscript color eps enhanced 22
set output 'regional.eps'
load "../styles.inc"
set size 1,0.6

#set title "Throughput/Latency real vs. emulated" offset 0,-0.5

set xlabel "Processes"
set ylabel "Throughput (K tx/s) "
set grid y
#set ytics 100
set logscale y 2
#set mytics 5
set xrange [90:410]
#set yrange [:40000]
set key above horizontal width -1 font ",14" 



plot "data/regional.dat" using 1:($2/1000) with linespoints ls 20 title "Kauri", \
     ""                  using 1:($3/1000) with linespoints ls 21 title "Motor*" ,\
     ""                  using 1:($4/1000) with linespoints ls 22 title "HotStuff-bls" ,\
     ""                  using 1:($5/1000) with linespoints ls 23 title "HotStuff-secp"

!epstopdf "regional.eps"
!rm "regional.eps"
