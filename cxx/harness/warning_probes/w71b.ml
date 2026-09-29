let[@tail_mod_cons] rec map f = function [] -> [] | x :: xs -> f x :: other f xs
and other f xs = map f xs
