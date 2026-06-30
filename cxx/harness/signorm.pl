# Normalise an `ocamlc -i` / `c++type --infer` dump for comparison:
#  - join wrapped continuation lines (ocamlc breaks long types across indented
#    lines) back into their item line, and collapse runs of whitespace;
#  - keep only `val` lines;
#  - canonicalise each line's type variables to 'a,'b,.. (per-line: each binding
#    is its own scheme) so structurally-equal signatures compare equal.
my @N = ('a'..'z', map { $_ . "1" } ('a'..'z'));
my @lines;
while (<STDIN>) {
  chomp;
  if (/^\s/ && @lines) { (my $c = $_) =~ s/^\s+//; $lines[-1] .= " " . $c; }
  else { push @lines, $_; }
}
for (@lines) {
  next unless /^val /;
  s/\s+/ /g;
  my %m; my $n = 0;
  s/'(_?[A-Za-z0-9]+)/ "'" . (defined $m{$1} ? $m{$1} : ($m{$1} = $N[$n++])) /ge;
  print "$_\n";
}
