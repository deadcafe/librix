set datafile separator '\t'

if (!exists("ROOT")) ROOT = GPVAL_PWD
input_tsv = ROOT . '/tests/hashtbl/out/slot_query_sweep.tsv'
output_svg = ROOT . '/tests/hashtbl/out/slot_query_sweep.gnuplot.svg'
output_png = ROOT . '/tests/hashtbl/out/slot_query_sweep.gnuplot.png'

set terminal svg size 1400,800 dynamic
set output output_svg
set title 'slot query sweep (median, pinned core, fixed total keys)'
set xlabel 'query'
set ylabel 'cycles/key'
set xrange [1:256]
set grid xtics ytics
set key top right

plot \
  input_tsv using 1:2 with lines linewidth 2 title 'single', \
  input_tsv using 1:3 with lines linewidth 2 title 'x1', \
  input_tsv using 1:4 with lines linewidth 2 title 'x8pipe'

set terminal pngcairo size 1400,800
set output output_png
replot
