module X = struct type t = int let compare = compare end
module type S = sig open Set.Make(X) val e : t val l : elt list end
