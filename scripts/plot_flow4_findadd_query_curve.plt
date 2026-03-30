set datafile separator '\t'

if (!exists("ROOT")) ROOT = GPVAL_PWD
input_tsv = ROOT . '/samples/fcache/test/out/flow4_findadd_query_curve_median.tsv'
output_svg = ROOT . '/samples/fcache/test/out/flow4_findadd_query_curve_median.gnuplot.svg'
output_png = ROOT . '/samples/fcache/test/out/flow4_findadd_query_curve_median.gnuplot.png'

set terminal svg size 1400,800 dynamic
set output output_svg
set title 'flow4 findadd query sweep (median, pinned core, fixed total keys)'
set xlabel 'query'
set ylabel 'cycles/key'
set xrange [1:256]
set grid xtics ytics
set key top right

plot \
  input_tsv using 1:2 with lines linewidth 2 title 'bulk', \
  input_tsv using 1:3 with lines linewidth 2 title 'burst32'

set terminal pngcairo size 1400,800
set output output_png
replot
