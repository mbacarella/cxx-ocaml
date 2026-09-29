#!/usr/bin/env perl
# Decode an OCaml marshalled value into an S-expression.
#
# A raw byte diff of two .cmi conflates three unrelated gaps -- physical
# sharing, type_expr ids and the import list -- so the value tree is what the
# .cmi work is actually scored on.
#
#   mdump.pl FILE [--skip N] [--count N] [--expand] [--norm-ids]
#
# --skip N    offset of the intext magic (12 for a .cmi, past "Caml1999I0NN")
# --count N   decode N consecutive values (a .cmi holds header, crcs, flags)
# --expand    follow CODE_SHARED back-references instead of printing #n
# --norm-ids  print every integer < -1 as `id` (Subst.newpersty counts down
#             from -1, so type_expr ids are exactly the negative ints)
use strict; use warnings; no warnings "recursion";

my ($path, $skip, $count, $expand, $normids) = (undef, 12, 1, 0, 0);
my @a = @ARGV;
while (@a) { my $x = shift @a;
  if    ($x eq '--skip')     { $skip = shift @a }
  elsif ($x eq '--count')    { $count = shift @a }
  elsif ($x eq '--expand')   { $expand = 1 }
  elsif ($x eq '--norm-ids') { $normids = 1 }
  else { $path = $x } }
die "usage: mdump.pl FILE\n" unless defined $path;

my $B = do { local $/; open my $fh, '<:raw', $path or die "$path: $!"; <$fh> };
my $P = $skip;
my @objs;

sub u8  { my $v = unpack('C', substr($B, $P, 1)); $P += 1; $v }
sub i8  { my $v = unpack('c', substr($B, $P, 1)); $P += 1; $v }
sub u16 { my $v = unpack('n', substr($B, $P, 2)); $P += 2; $v }
sub u32 { my $v = unpack('N', substr($B, $P, 4)); $P += 4; $v }
sub i32 { my $v = unpack('l>', substr($B, $P, 4)); $P += 4; $v }
sub i64 { my $v = unpack('q>', substr($B, $P, 8)); $P += 8; $v }
sub raw { my $n = shift; my $v = substr($B, $P, $n); $P += $n; $v }

sub mkstr { my $n = shift; my $i = scalar @objs; push @objs, undef;
            my $v = ['str', raw($n)]; $objs[$i] = $v; $v }
sub mkblk { my ($tag, $size) = @_; my $i = scalar @objs;
            my $v = ['blk', $tag, []]; push @objs, $v;
            push @{$v->[2]}, val() for 1 .. $size; $v }

sub val {
  my $c = u8();
  if ($c >= 0x80) { my $tag = $c & 0xF; my $size = ($c >> 4) & 0x7;
                    return mkblk($tag, $size) }
  return ['int', $c - 0x40] if $c >= 0x40;
  return mkstr($c & 0x1F)   if $c >= 0x20;
  if ($c == 0x00) { return ['int', i8()] }
  if ($c == 0x01) { my $v = unpack('s>', raw(2)); return ['int', $v] }
  if ($c == 0x02) { return ['int', i32()] }
  if ($c == 0x03) { return ['int', i64()] }
  if ($c == 0x04) { return ['shared', scalar(@objs) - u8()] }
  if ($c == 0x05) { return ['shared', scalar(@objs) - u16()] }
  if ($c == 0x06) { return ['shared', scalar(@objs) - u32()] }
  if ($c == 0x08) { my $h = u32(); return mkblk($h & 0xFF, $h >> 10) }
  if ($c == 0x13) { my $h = i64(); return mkblk($h & 0xFF, $h >> 10) }
  if ($c == 0x09) { return mkstr(u8()) }
  if ($c == 0x0A) { return mkstr(u32()) }
  if ($c == 0x0B) { my $i = scalar @objs;
                    my $v = ['dbl', unpack('d>', raw(8))]; push @objs, $v; $v }
  elsif ($c == 0x0C) { my $v = ['dbl', unpack('d<', raw(8))];
                       push @objs, $v; $v }
  elsif ($c == 0x12 || $c == 0x18) {          # CODE_CUSTOM / CODE_CUSTOM_LEN
    my $v = ['custom', '', '']; push @objs, $v;
    my $e = index($B, "\0", $P);
    my $name = substr($B, $P, $e - $P); $P = $e + 1;
    my $body = '';
    if ($c == 0x18) { my $s32 = u32(); i64(); $body = raw($s32) }
    $v->[1] = $name; $v->[2] = unpack('H*', $body); $v }
  else { die sprintf("unhandled code %02x at %d\n", $c, $P - 1) }
}

my %seen;
sub render {
  my ($v, $ind, $out) = @_;
  my $pad = ' ' x $ind;
  my $k = $v->[0];
  if ($k eq 'int') { my $n = $v->[1];
    push @$out, $pad . (($normids && $n < -1) ? 'id' : $n) }
  elsif ($k eq 'str')    { push @$out, $pad . '"' . $v->[1] . '"' }
  elsif ($k eq 'dbl')    { push @$out, $pad . $v->[1] }
  elsif ($k eq 'custom') { push @$out, $pad . "custom($v->[1],$v->[2])" }
  elsif ($k eq 'shared') {
    if (!$expand) { push @$out, $pad . '#' . $v->[1] }
    else { my $t = $objs[$v->[1]];
           if ($seen{$t}) { push @$out, $pad . '<cycle>' }
           else { render($t, $ind, $out) } } }
  elsif ($k eq 'blk') {
    if ($seen{$v}) { push @$out, $pad . '<cycle>'; return }
    $seen{$v} = 1;
    push @$out, $pad . '(t' . $v->[1];
    render($_, $ind + 1, $out) for @{$v->[2]};
    push @$out, $pad . ')';
    delete $seen{$v} }
  else { die "bad node $k\n" }
}

for my $k (1 .. $count) {
  my $magic = u32();
  my ($dlen, $nobj);
  if ($magic == 0x8495A6BE) { $dlen = u32(); $nobj = u32(); u32(); u32() }
  elsif ($magic == 0x8495A6BF) { u32(); $dlen = i64(); $nobj = i64(); i64() }
  else { die sprintf("bad magic %08x at %d\n", $magic, $P - 4) }
  @objs = (); %seen = ();
  my $v = val();
  my @out;
  render($v, 0, \@out);
  print "=== value $k  nobj=$nobj dlen=$dlen\n";
  print "$_\n" for @out;
}
