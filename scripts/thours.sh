#!/bin/zsh

if test $# -gt 0; then
	echo "postscript('thours.ps', onefile=TRUE);"
	for arg; do
		echo "x <- fread('"
		thours < "${arg}"
		echo "', colClasses=c('factor', 'factor', 'integer'), col.names=c('day', 'hour', 'count'));"
		cat <<EOF
print(ggplot(x, aes(hour, count, fill=day)) +
geom_bar(stat='identity', width=0.8, position=position_dodge(width=0.8)) +
scale_fill_discrete(labels=function(x)c('M','T','W','R','F','A','S')[as.numeric(x)]));
EOF
	done
	echo "dev.off();"
fi \
	| R --no-save
