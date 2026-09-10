#!/usr/bin/env perl
# Region classifier over a `diff our ref` of two `dumpobj -effid` dumps.
# Prints each hunk with a STRUCTURAL count = diff lines that survive
# digit-normalisation (a pure operand/offset change counts 0).
# Usage: perl str.pl <diff> [lo hi]      ('<' = OURS, '>' = REF)
use strict; use warnings;
my ($f,$lo,$hi) = @ARGV; open my $h,'<',$f or die;
my (@hunks,$cur);
while (my $l = <$h>) {
  if ($l =~ /^(\d+)(?:,(\d+))?([acd])(\d+)(?:,(\d+))?$/) {
    push @hunks,$cur if $cur;
    $cur = { hdr=>$l, start=>$1, ours=>[], ref=>[] };
  } elsif ($cur) {
    push @{$cur->{ours}}, $l if $l =~ s/^< //;
    push @{$cur->{ref}},  $l if $l =~ s/^> //;
  }
}
push @hunks,$cur if $cur;
my $tot = 0;
for my $x (@hunks) {
  next if defined $lo && ($x->{start} < $lo || $x->{start} > $hi);
  my @a = map { my $s=$_; $s =~ s/\d+/N/g; $s } @{$x->{ours}};
  my @b = map { my $s=$_; $s =~ s/\d+/N/g; $s } @{$x->{ref}};
  my %c; $c{$_}++ for @a; $c{$_}-- for @b;
  my $st = 0; $st += abs($_) for values %c;
  $tot += $st;
  chomp(my $hdr = $x->{hdr});
  print "=== $hdr (structural $st)\n";
  if (defined $lo) {
    print "< $_" for @{$x->{ours}};
    print "---\n" if @{$x->{ours}} && @{$x->{ref}};
    print "> $_" for @{$x->{ref}};
  }
}
print "TOTAL structural=$tot\n";
