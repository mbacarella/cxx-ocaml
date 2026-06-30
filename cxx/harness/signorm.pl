# Keep only `val` lines; canonicalise each line's type variables to 'a,'b,...
# (per-line: each binding is its own scheme) so structurally-equal sigs compare.
my @N = ('a'..'z', map { $_ . "1" } ('a'..'z'));
while (<STDIN>) {
  next unless /^val /;
  my %m; my $n = 0;
  s/'(_?[A-Za-z0-9]+)/ "'" . (defined $m{$1} ? $m{$1} : ($m{$1} = $N[$n++])) /ge;
  print;
}
