module X = struct type t = int let compare = compare end
module type T = sig type t = Set.Make(X).t end
