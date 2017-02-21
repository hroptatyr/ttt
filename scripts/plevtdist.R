#!/usr/bin/env Rscript

args <- commandArgs(trailingOnly=F);
## find ourselves
pivot <- Position(function(x) x == "--args", args)
self <- unlist(strsplit(args[pivot - 1], "=", fixed=T));
self <- self[length(self)];
sdir <- dirname(self)

library(data.table)
library(ggplot2)
library(scales)

theme_set(theme_light())

asinh_breaks <- function(x) {
        b <- 10^(0:10);
        b <- sort(c(-b, -10*b, 0, b, 10*b));

        rng <- range(x, na.rm = TRUE);

        nx1 <- max(Filter(function(x) x <= rng[1], b))
        nx2 <- min(Filter(function(x) x >= rng[2], b))
        c(nx1, Filter(function(x) x < rng[2], Filter(function(x) x > rng[1], b)), nx2);
}

asinh_mbreaks <- function(x) {
        b <- 10^(0:10);
        b <- sort(c(-b, -10*b, 0, b, 10*b) / 2);

        rng <- range(x, na.rm = TRUE);

        nx1 <- max(Filter(function(x) x <= rng[1], b))
        nx2 <- min(Filter(function(x) x >= rng[2], b))
        c(nx1, Filter(function(x) x < rng[2], Filter(function(x) x > rng[1], b)), nx2);
}


ssqrt_breaks <- asinh_breaks;
ssqrt_mbreaks <- asinh_mbreaks;

tlog_breaks <- function(x) {
        b <- c(1,5,10,30,60,120,300,600,1800,3600,7200,6*3600,12*3600,24*3600,48*3600,3*24*3600,7*24*3600);

        rng <- range(x, na.rm = TRUE);

        nx1 <- pmax(Filter(function(x) x <= rng[1], b))
        nx2 <- pmin(Filter(function(x) x >= rng[2], b))
        c(nx1, Filter(function(x) x < rng[2], Filter(function(x) x > rng[1], b)), nx2);
}

tlog_mbreaks <- function(x) {
        b <- c(0.1,0.25,0.5,2,20,45,180,1200,2700,3600+1800,7200+3600,4*3600,9*3600,18*3600);

        rng <- range(x, na.rm = TRUE);

        nx1 <- pmax(Filter(function(x) x <= rng[1], b))
        nx2 <- pmin(Filter(function(x) x >= rng[2], b))
        c(nx1, Filter(function(x) x < rng[2], Filter(function(x) x > rng[1], b)), nx2);
}


asinh_trans <-
        trans_new(name = 'asinh',
                transform = function(x) asinh(x),
                inverse = function(x) sinh(x));

ssqrt_trans <-
        trans_new(name = 'sqrt',
                transform = function(x) sign(x)*sqrt(abs(x)),
                inverse = function(x) (x*abs(x)));

tlog_trans <-
        trans_new(name = 'tlog',
                transform = function(x) asinh(x),
                inverse = function(x) sinh(x));

scale_y_asinh <- function(...) {
        scale_y_continuous(..., trans = asinh_trans, breaks = asinh_breaks, minor_breaks = asinh_mbreaks)
}

scale_x_asinh <- function(...) {
        scale_x_continuous(..., trans = asinh_trans, breaks = asinh_breaks, minor_breaks = asinh_mbreaks)
}

scale_y_ssqrt <- function(...) {
        scale_y_continuous(..., trans = ssqrt_trans, breaks = ssqrt_breaks, minor_breaks = ssqrt_mbreaks)
}

scale_x_ssqrt <- function(...) {
        scale_x_continuous(..., trans = ssqrt_trans, breaks = ssqrt_breaks, minor_breaks = ssqrt_mbreaks)
}

scale_y_tlog <- function(...) {
        scale_y_continuous(..., trans = tlog_trans, breaks = tlog_breaks, minor_breaks = tlog_mbreaks)
}

scale_x_tlog <- function(...) {
        scale_x_continuous(..., trans = tlog_trans, breaks = tlog_breaks, minor_breaks = tlog_mbreaks)
}


## file we're meant to process
file <- args[pivot + 1];
fread(file, col.names=c("cndl", "lo", "hi", "cnt", "Erlang", "Gamma", "Lomax")) -> X

## yearly partition
X[, year:=substr(cndl, 1, 4)]
## melt
Y <- melt(X, id.vars=c("cndl", "year", "lo", "hi"))

continue_on_error <- function() {}
options(error=continue_on_error);

postscript(file=paste0(sub('[.]surv$', '', file), '.ps'), onefile=T, horizontal=F)
print(ggplot(Y[variable=="cnt"], aes(lo, value)) + facet_grid(year~.) + geom_line(size=0.25) + geom_line(data=Y[variable!="cnt"], aes(lo, value, colour=variable), linetype=3, size=0.5) + scale_x_tlog() + scale_y_asinh() + labs(x=NULL, y=NULL, title=paste(file, "activity"), colour=NULL) + theme(axis.text.x = element_text(size=7, angle=60, hjust=1, vjust=1), axis.text.y = element_text(size=7)))
dev.off()
