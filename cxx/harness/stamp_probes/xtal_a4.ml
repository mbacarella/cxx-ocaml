module X = struct type t = int let compare = compare end
module type T = sig val v : Stdlib__Set.Make(X).t end
