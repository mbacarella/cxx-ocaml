module M = struct type t = int let compare = compare end
module A = Map.Make (M)
module B = Map.Make (M)
